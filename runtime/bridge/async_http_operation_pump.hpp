
#pragma once

#include "bridge/async_operation_coordinator.hpp"

#include "http/http_async_connection.hpp"
#include "http/http_completion_queue.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lamp::bridge {

// ============================================================
// Async HTTP Operation Pump
//
// Bridges asynchronous Lamp operation execution with the
// owner-thread HTTP completion lifecycle.
//
// Threading contract:
//   - Submit(), Poll(), Drain(), Advance(), CancelConnection(),
//     CancelToken(), and Close() are called from the HTTP
//     owner thread.
//   - Scheduler workers never mutate HttpAsyncConnection.
//   - HttpCompletionQueue callbacks execute on the thread
//     calling Drain().
//   - The coordinator's Poll() invokes completion callbacks
//     on the thread calling Poll().
//
// Connection ownership:
//   - Connections are held through weak_ptr.
//   - Disconnects cannot leave a dangling connection pointer.
//   - Generation checks are performed by HttpAsyncConnection.
//   - Per-connection cancellation tokens propagate disconnect
//     and Close() to outstanding scheduler tasks.
//
// This component does not own the socket or event loop.
// ============================================================

class AsyncHttpOperationPumpError final
    : public std::runtime_error {
public:
    explicit AsyncHttpOperationPumpError(
        const std::string& message
    )
        : std::runtime_error(message) {}
};

struct AsyncHttpOperationPumpStats final {
    std::uint64_t submitted = 0;
    std::uint64_t submitRejected = 0;
    std::uint64_t mappedResponses = 0;
    std::uint64_t mapperFailures = 0;
    std::uint64_t queuedCompletions = 0;
    std::uint64_t directSaturationDeliveries = 0;
    std::uint64_t droppedClosedQueue = 0;
    std::uint64_t droppedExpiredConnection = 0;
    std::uint64_t cancelledConnections = 0;
    std::uint64_t cancelledTokens = 0;
    std::uint64_t drained = 0;
};

class AsyncHttpOperationPump final {
public:
    using Coordinator = AsyncOperationCoordinator;
    using TaskHandle = Coordinator::TaskHandle;
    using ScheduleOptions = Coordinator::ScheduleOptions;
    using SubmitResult = Coordinator::SubmitResult;

    using Connection = http::HttpAsyncConnection;
    using Token = http::HttpCompletionToken;
    using CompletionQueue = http::HttpCompletionQueue;

    using ResponseMapper = std::function<
        http::HttpResponse(TaskHandle)
    >;

    explicit AsyncHttpOperationPump(
        std::shared_ptr<Coordinator> coordinator,
        std::shared_ptr<CompletionQueue> completions,
        ResponseMapper mapper
    )
        : coordinator_(std::move(coordinator)),
          completions_(std::move(completions)),
          mapper_(std::move(mapper)),
          state_(std::make_shared<State>()) {
        if (!coordinator_) {
            throw AsyncHttpOperationPumpError(
                "Async HTTP pump requires a coordinator"
            );
        }

        if (!completions_) {
            throw AsyncHttpOperationPumpError(
                "Async HTTP pump requires a completion queue"
            );
        }

        if (!mapper_) {
            throw AsyncHttpOperationPumpError(
                "Async HTTP pump requires a response mapper"
            );
        }
    }

    ~AsyncHttpOperationPump() noexcept {
        Close();
    }

    AsyncHttpOperationPump(
        const AsyncHttpOperationPump&
    ) = delete;

    AsyncHttpOperationPump& operator=(
        const AsyncHttpOperationPump&
    ) = delete;

    // ========================================================
    // Map Scheduler Submission Failure -> HTTP Response
    // ========================================================

    [[nodiscard]]
    static http::HttpResponse MapSubmitFailure(
        const SubmitResult& result,
        http::HttpVersion version = {}
    ) {
        using Status = Coordinator::SubmitStatus;

        http::HttpStatus httpStatus =
            http::HttpStatus::InternalServerError;
        const char* body =
            R"({"error":"internal_server_error"})";

        switch (result.status) {
            case Status::InvalidRequest:
                httpStatus = http::HttpStatus::BadRequest;
                body = R"({"error":"bad_request"})";
                break;

            case Status::DeadlineExceeded:
                httpStatus = http::HttpStatus::GatewayTimeout;
                body = R"({"error":"deadline_exceeded"})";
                break;

            case Status::QueueFull:
            case Status::SchedulerStopped:
            case Status::BackendUnavailable:
            case Status::Cancelled:
                httpStatus =
                    http::HttpStatus::ServiceUnavailable;
                body = R"({"error":"service_unavailable"})";
                break;

            case Status::Accepted:
            case Status::InternalError:
            default:
                httpStatus =
                    http::HttpStatus::InternalServerError;
                body = R"({"error":"internal_server_error"})";
                break;
        }

        http::HttpResponse response(httpStatus, version);
        response.setHeader("Content-Type", "application/json");
        response.setHeader("Cache-Control", "no-store");
        response.setBody(body);
        return response;
    }

    // ========================================================
    // Submit HTTP Operation
    //
    // The caller builds a Lamp RequestEnvelope using its
    // operation routing and serialization rules.
    //
    // The token is supplied by HttpAsyncConnection.
    //
    // A successful submission does not mean the operation
    // has completed.
    // ========================================================

    [[nodiscard]]
    SubmitResult Submit(
        const Token& token,
        std::weak_ptr<Connection> connection,
        RequestEnvelope request,
        ScheduleOptions options = {}
    ) {
        if (state_->closed.load(std::memory_order_acquire)) {
            state_->RecordSubmitRejected();
            return SubmitResult::Failure(
                Coordinator::SubmitStatus::SchedulerStopped,
                "Async HTTP pump is closed"
            );
        }

        if (!token.valid()) {
            state_->RecordSubmitRejected();
            return SubmitResult::Failure(
                Coordinator::SubmitStatus::InvalidRequest,
                "Invalid HTTP completion token"
            );
        }

        auto target = connection.lock();

        if (!target ||
            target->closed() ||
            target->connectionId() != token.connectionId ||
            target->generation() != token.generation) {
            state_->RecordSubmitRejected();
            return SubmitResult::Failure(
                Coordinator::SubmitStatus::InvalidRequest,
                "HTTP connection is unavailable or stale"
            );
        }

        const RequestId envelopeRequestId = request.request_id;

        auto tokenRegistration = state_->RegisterToken(
            token,
            envelopeRequestId
        );

        if (!tokenRegistration.accepted) {
            state_->RecordSubmitRejected();
            return SubmitResult::Failure(
                Coordinator::SubmitStatus::InvalidRequest,
                "Duplicate active HTTP completion token"
            );
        }

        request.cancellation =
            RequestCancellationToken::Combine(
                std::move(request.cancellation),
                RequestCancellationToken::Combine(
                    tokenRegistration.connectionCancelToken,
                    tokenRegistration.tokenCancelToken
                )
            );

        // Retain no strong reference to the connection.
        // A completed operation may outlive its HTTP client.

        auto result = coordinator_->Submit(
            std::move(request),
            options,
            [
                token,
                connection = std::move(connection),
                queue = completions_,
                mapper = mapper_,
                state = state_
            ](TaskHandle handle) mutable {
                // Coordinator callbacks execute during Poll().
                // Release token tracking immediately.
                state->UnregisterToken(token);

                auto target = connection.lock();

                if (!target ||
                    target->closed() ||
                    target->connectionId() !=
                        token.connectionId ||
                    target->generation() !=
                        token.generation) {
                    state->RecordDroppedExpiredConnection();
                    return;
                }

                http::HttpResponse response;

                try {
                    response = mapper(std::move(handle));
                    state->RecordMappedResponse();
                } catch (...) {
                    state->RecordMapperFailure();

                    response = http::HttpResponse(
                        http::HttpStatus::InternalServerError
                    );

                    response.setBody(
                        "Internal Server Error"
                    );
                }

                auto responseState =
                    std::make_shared<http::HttpResponse>(
                        std::move(response)
                    );

                http::HttpCompletion completion{
                    token,
                    [
                        connection,
                        responseState
                    ](const Token& deliveredToken) mutable {
                        auto resolved = connection.lock();

                        if (!resolved) {
                            return;
                        }

                        (void)resolved->complete(
                            deliveredToken,
                            std::move(*responseState)
                        );
                    }
                };

                const auto posted =
                    queue->post(std::move(completion));

                if (posted ==
                    http::HttpCompletionPostResult::Accepted) {
                    state->RecordQueuedCompletion();
                    return;
                }

                if (posted ==
                    http::HttpCompletionPostResult::Closed) {
                    // Queue shutdown was explicitly requested.
                    // Do not bypass queue closure.
                    state->RecordDroppedClosedQueue();
                    return;
                }

                // Poll() is required to run on the owner
                // thread. If the queue is full (saturation),
                // deliver directly on that same thread
                // rather than silently losing a response
                // whose coordinator entry was already popped.
                //
                // This fallback must not be used if Poll()
                // is invoked by a scheduler worker.

                if (posted ==
                    http::HttpCompletionPostResult::Full) {
                    state->RecordDirectSaturationDelivery();

                    (void)target->complete(
                        token,
                        std::move(*responseState)
                    );
                } else {
                    state->RecordDroppedClosedQueue();
                }
            }
        );

        if (!result.Accepted()) {
            state_->UnregisterToken(token);
            state_->RecordSubmitRejected();
            return result;
        }

        state_->RecordSubmitted();
        return result;
    }

    // ========================================================
    // Submit or Emit Mapped Error Response on Rejection
    //
    // Convenience helper for transport adapters: ensures an
    // already-allocated HttpAsyncConnection slot receives an
    // HTTP error response when scheduler submission fails.
    // ========================================================

    [[nodiscard]]
    SubmitResult SubmitOrComplete(
        const Token& token,
        std::weak_ptr<Connection> connection,
        RequestEnvelope request,
        ScheduleOptions options = {},
        http::HttpVersion version = {}
    ) {
        auto result = Submit(
            token,
            connection,
            std::move(request),
            options
        );

        if (!result.Accepted()) {
            auto target = connection.lock();
            if (target &&
                !target->closed() &&
                target->connectionId() == token.connectionId &&
                target->generation() == token.generation) {
                (void)target->complete(
                    token,
                    MapSubmitFailure(result, version)
                );
            }
        }

        return result;
    }

    // ========================================================
    // Cancel Operations for Disconnected / Aborted Client
    // ========================================================

    std::size_t CancelConnection(
        std::uint64_t connectionId,
        std::uint64_t generation
    ) noexcept {
        auto cancelledIds =
            state_->CancelConnection(connectionId, generation);

        for (const auto requestId : cancelledIds.requestIds) {
            if (requestId != 0) {
                (void)coordinator_->CancelRequest(requestId);
            }
        }

        return cancelledIds.tokenCount;
    }

    std::size_t CancelConnection(
        const Connection& connection
    ) noexcept {
        return CancelConnection(
            connection.connectionId(),
            connection.generation()
        );
    }

    bool CancelToken(const Token& token) noexcept {
        const auto requestId = state_->CancelToken(token);
        if (!requestId.found) {
            return false;
        }

        if (requestId.requestId != 0) {
            (void)coordinator_->CancelRequest(
                requestId.requestId
            );
        }

        return true;
    }

    void SetWakeupCallback(std::function<void()> wakeup) {
        coordinator_->SetWakeupCallback(std::move(wakeup));
    }

    // ========================================================
    // Poll Completed Operations
    //
    // Must be invoked on the HTTP owner thread.
    // Completion callbacks publish into HttpCompletionQueue.
    // ========================================================

    [[nodiscard]]
    std::size_t Poll() {
        if (state_->closed.load(std::memory_order_acquire)) {
            return 0;
        }

        return coordinator_->Poll();
    }

    // ========================================================
    // Deliver Queued HTTP Completions
    //
    // Must be invoked on the same HTTP owner thread.
    // ========================================================

    [[nodiscard]]
    std::size_t Drain(
        std::size_t maxCount = 1024
    ) {
        if (state_->closed.load(std::memory_order_acquire)) {
            return 0;
        }

        const auto delivered = completions_->drain(maxCount);
        state_->RecordDrained(delivered);
        return delivered;
    }

    // ========================================================
    // Combined Owner-Thread Progress
    //
    // Poll first, then deliver queued responses.
    // ========================================================

    [[nodiscard]]
    std::size_t Advance(
        std::size_t maxDrain = 1024
    ) {
        const auto completed = Poll();
        const auto delivered = Drain(maxDrain);

        return completed + delivered;
    }

    // ========================================================
    // Shutdown
    //
    // Close() is terminal.
    //
    // Outstanding tasks are cancelled through both the
    // connection cancellation sources and the coordinator.
    // Queued callbacks are not drained here.
    //
    // The owner must coordinate queue shutdown with any
    // other producers/consumers sharing that queue.
    // ========================================================

    void Close() noexcept {
        bool expected = false;
        if (!state_->closed.compare_exchange_strong(
                expected,
                true,
                std::memory_order_acq_rel,
                std::memory_order_acquire
            )) {
            return;
        }

        state_->CancelAll();
        coordinator_->Close();
    }

    [[nodiscard]]
    bool Closed() const noexcept {
        return state_->closed.load(std::memory_order_acquire);
    }

    [[nodiscard]]
    std::size_t Outstanding() const noexcept {
        return coordinator_->Outstanding();
    }

    [[nodiscard]]
    std::size_t QueuedCompletions() const noexcept {
        return completions_->size();
    }

    [[nodiscard]]
    AsyncHttpOperationPumpStats Stats() const noexcept {
        return state_->SnapshotStats();
    }

private:
    struct ConnectionKey final {
        std::uint64_t connectionId = 0;
        std::uint64_t generation = 0;

        [[nodiscard]]
        friend bool operator==(
            const ConnectionKey&,
            const ConnectionKey&
        ) noexcept = default;
    };

    struct ConnectionKeyHash final {
        [[nodiscard]]
        std::size_t operator()(
            const ConnectionKey& key
        ) const noexcept {
            const auto h1 =
                std::hash<std::uint64_t>{}(key.connectionId);
            const auto h2 =
                std::hash<std::uint64_t>{}(key.generation);
            return h1 ^ (h2 + 0x9e3779b97f4a7c15ULL +
                         (h1 << 6) + (h1 >> 2));
        }
    };

    struct ActiveTokenEntry final {
        RequestId requestId = 0;
        RequestCancellationSource cancelSource;
    };

    struct ConnectionState final {
        RequestCancellationSource cancelSource;
        std::unordered_map<std::uint64_t, ActiveTokenEntry>
            tokens;
    };

    struct TokenRegistration final {
        bool accepted = false;
        RequestCancellationToken connectionCancelToken;
        RequestCancellationToken tokenCancelToken;
    };

    struct CancelledIds final {
        std::size_t tokenCount = 0;
        std::vector<RequestId> requestIds;
    };

    struct CancelledTokenInfo final {
        bool found = false;
        RequestId requestId = 0;
    };

    struct State final {
        std::atomic<bool> closed{false};
        mutable std::mutex mutex;
        std::unordered_map<
            ConnectionKey,
            ConnectionState,
            ConnectionKeyHash
        > connections;
        AsyncHttpOperationPumpStats stats{};

        TokenRegistration RegisterToken(
            const Token& token,
            RequestId requestId
        ) {
            std::lock_guard lock(mutex);

            if (closed.load(std::memory_order_acquire)) {
                return {};
            }

            const ConnectionKey key{
                token.connectionId,
                token.generation
            };

            auto& connState = connections[key];

            if (connState.tokens.find(token.requestId) !=
                connState.tokens.end()) {
                return {};
            }

            ActiveTokenEntry entry;
            entry.requestId = requestId;

            TokenRegistration reg;
            reg.accepted = true;
            reg.connectionCancelToken =
                connState.cancelSource.Token();
            reg.tokenCancelToken = entry.cancelSource.Token();

            connState.tokens.emplace(
                token.requestId,
                std::move(entry)
            );

            return reg;
        }

        void UnregisterToken(const Token& token) noexcept {
            std::lock_guard lock(mutex);

            const ConnectionKey key{
                token.connectionId,
                token.generation
            };

            const auto connIt = connections.find(key);
            if (connIt == connections.end()) {
                return;
            }

            connIt->second.tokens.erase(token.requestId);
            if (connIt->second.tokens.empty()) {
                connections.erase(connIt);
            }
        }

        CancelledIds CancelConnection(
            std::uint64_t connectionId,
            std::uint64_t generation
        ) noexcept {
            CancelledIds result;
            std::lock_guard lock(mutex);

            const ConnectionKey key{connectionId, generation};
            const auto connIt = connections.find(key);
            if (connIt == connections.end()) {
                return result;
            }

            connIt->second.cancelSource.Cancel();
            ++stats.cancelledConnections;

            result.tokenCount = connIt->second.tokens.size();
            result.requestIds.reserve(result.tokenCount);

            for (auto& [id, entry] : connIt->second.tokens) {
                (void)id;
                entry.cancelSource.Cancel();
                result.requestIds.push_back(entry.requestId);
            }

            connections.erase(connIt);
            return result;
        }

        CancelledTokenInfo CancelToken(
            const Token& token
        ) noexcept {
            CancelledTokenInfo info;
            std::lock_guard lock(mutex);

            const ConnectionKey key{
                token.connectionId,
                token.generation
            };

            const auto connIt = connections.find(key);
            if (connIt == connections.end()) {
                return info;
            }

            const auto tokIt =
                connIt->second.tokens.find(token.requestId);
            if (tokIt == connIt->second.tokens.end()) {
                return info;
            }

            tokIt->second.cancelSource.Cancel();
            ++stats.cancelledTokens;

            info.found = true;
            info.requestId = tokIt->second.requestId;

            connIt->second.tokens.erase(tokIt);
            if (connIt->second.tokens.empty()) {
                connections.erase(connIt);
            }

            return info;
        }

        void CancelAll() noexcept {
            std::lock_guard lock(mutex);

            for (auto& [key, connState] : connections) {
                (void)key;
                connState.cancelSource.Cancel();
                for (auto& [id, entry] : connState.tokens) {
                    (void)id;
                    entry.cancelSource.Cancel();
                }
            }

            connections.clear();
        }

        void RecordSubmitted() noexcept {
            std::lock_guard lock(mutex);
            ++stats.submitted;
        }

        void RecordSubmitRejected() noexcept {
            std::lock_guard lock(mutex);
            ++stats.submitRejected;
        }

        void RecordMappedResponse() noexcept {
            std::lock_guard lock(mutex);
            ++stats.mappedResponses;
        }

        void RecordMapperFailure() noexcept {
            std::lock_guard lock(mutex);
            ++stats.mapperFailures;
        }

        void RecordQueuedCompletion() noexcept {
            std::lock_guard lock(mutex);
            ++stats.queuedCompletions;
        }

        void RecordDirectSaturationDelivery() noexcept {
            std::lock_guard lock(mutex);
            ++stats.directSaturationDeliveries;
        }

        void RecordDroppedClosedQueue() noexcept {
            std::lock_guard lock(mutex);
            ++stats.droppedClosedQueue;
        }

        void RecordDroppedExpiredConnection() noexcept {
            std::lock_guard lock(mutex);
            ++stats.droppedExpiredConnection;
        }

        void RecordDrained(std::size_t count) noexcept {
            std::lock_guard lock(mutex);
            stats.drained += count;
        }

        AsyncHttpOperationPumpStats SnapshotStats()
            const noexcept {
            std::lock_guard lock(mutex);
            return stats;
        }
    };

    std::shared_ptr<Coordinator> coordinator_;
    std::shared_ptr<CompletionQueue> completions_;

    ResponseMapper mapper_;

    std::shared_ptr<State> state_;
};

} // namespace lamp::bridge
