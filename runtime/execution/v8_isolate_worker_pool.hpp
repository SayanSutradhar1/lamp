#pragma once

// Lamp Framework - Phase 5
// File: runtime/execution/v8_isolate_worker_pool.hpp
// C++20: bounded isolate workers with backward-compatible synchronous execution
// and worker-local, cooperatively managed JavaScript Promise continuations.

#include "execution/async_execution_contract.hpp"
#include "execution/v8_worker_async_execution.hpp"
#include "execution/v8_promise_continuation_scheduler.hpp"

#include <v8.h>


#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lamp::execution {

struct V8IsolateWorkerPoolConfig final {
    std::size_t worker_count = 2;
    std::size_t max_pending_tasks = 2048;
    std::size_t priority_burst_limit = 32;
    std::size_t max_heap_size_mb = 128;
    bool drain_on_shutdown = true;
};

struct V8WorkerExecutionContext final {
    std::size_t worker_index = 0;
    ::v8::Isolate* isolate = nullptr;
    ::v8::Local<::v8::Context> context;
};

using V8WorkerOperationExecutor = std::function<bridge::ResultEnvelope(
    const bridge::RequestEnvelope&, const V8WorkerExecutionContext&)>;

class V8IsolateWorkerPool final : public IAsyncExecutionScheduler {
public:
    using Clock = std::chrono::steady_clock;
    using CompletionPtr = std::shared_ptr<AsyncTaskCompletion>;

    explicit V8IsolateWorkerPool(V8WorkerOperationExecutor executor,
                                 V8IsolateWorkerPoolConfig config = {})
        : sync_executor_(std::move(executor)), config_(config) {
        if (!sync_executor_) {
            throw std::invalid_argument("V8 worker pool requires an executor");
        }
        ValidateConfig();
        StartWorkers();
    }

    V8IsolateWorkerPool(V8WorkerAsyncOperationExecutor executor,
                        V8WorkerPromiseResultMapper mapper,
                        V8IsolateWorkerPoolConfig config = {},
                        V8WorkerAsyncExecutionConfig async_config = {})
        : async_executor_(std::move(executor)), mapper_(std::move(mapper)),
          config_(config), async_config_(async_config), async_mode_(true) {
        if (!async_executor_ || !mapper_) {
            throw std::invalid_argument(
                "V8 async worker pool requires executor and settlement mapper");
        }
        ValidateConfig();
        async_config_.Validate();
        StartWorkers();
    }

    V8IsolateWorkerPool(const V8IsolateWorkerPool&) = delete;
    V8IsolateWorkerPool& operator=(const V8IsolateWorkerPool&) = delete;

    ~V8IsolateWorkerPool() override {
        // Destruction from a worker thread is unsupported: ownership must outlive
        // all worker callbacks. Shutdown() explicitly rejects self-join.
        try { Shutdown(); } catch (...) { std::terminate(); }
    }

    AsyncSubmitResult Submit(bridge::RequestEnvelope request,
                             AsyncScheduleOptions options,
                             AsyncTaskHandle& output) override {
        output = AsyncTaskHandle{};
        if (options.target == TaskExecutionTarget::NativeWorker) {
            return AsyncSubmitResult::Failure(AsyncSubmitStatus::BackendUnavailable,
                "V8 pool cannot execute native-targeted tasks");
        }
        if (request.IsCancelled()) {
            return AsyncSubmitResult::Failure(AsyncSubmitStatus::Cancelled,
                "Request already cancelled");
        }
        if (options.reject_expired && request.IsExpired()) {
            return AsyncSubmitResult::Failure(AsyncSubmitStatus::DeadlineExceeded,
                "Request deadline already expired");
        }
        if (const auto error = request.Validate()) {
            return AsyncSubmitResult::Failure(AsyncSubmitStatus::InvalidRequest,
                error->message);
        }

        CompletionPtr completion;
        try {
            completion = std::make_shared<AsyncTaskCompletion>(std::move(request));
        } catch (const std::exception& error) {
            return AsyncSubmitResult::Failure(AsyncSubmitStatus::InternalError,
                error.what());
        } catch (...) {
            return AsyncSubmitResult::Failure(AsyncSubmitStatus::InternalError,
                "Unable to allocate task completion");
        }

        Task task;
        task.completion = completion;
        task.options = options;
        task.submitted_at = Clock::now();
        {
            std::lock_guard lock(mutex_);
            if (stopping_) {
                return AsyncSubmitResult::Failure(AsyncSubmitStatus::SchedulerStopped,
                    "V8 worker pool is stopping");
            }
            if (pending_tasks_ >= config_.max_pending_tasks) {
                return AsyncSubmitResult::Failure(AsyncSubmitStatus::QueueFull,
                    "V8 worker queue is full");
            }
            if (!options.allow_queueing &&
                (pending_tasks_ != 0 || active_tasks_ >= config_.worker_count)) {
                return AsyncSubmitResult::Failure(AsyncSubmitStatus::QueueFull,
                    "No V8 worker is immediately available");
            }
            if (!completion->MarkQueued()) {
                return AsyncSubmitResult::Failure(AsyncSubmitStatus::InternalError,
                    "Unable to queue V8 task");
            }
            task.sequence = next_sequence_++;
            try {
                queues_[PriorityIndex(options.priority)].push_back(std::move(task));
            } catch (...) {
                return AsyncSubmitResult::Failure(AsyncSubmitStatus::InternalError,
                    "Failed to enqueue V8 task");
            }
            ++pending_tasks_;
        }
        output = AsyncTaskHandle(std::move(completion));
        queue_cv_.notify_one();
        return AsyncSubmitResult::Success();
    }

    [[nodiscard]] bool IsRunning() const noexcept override {
        std::lock_guard lock(mutex_);
        return !stopping_;
    }
    [[nodiscard]] std::size_t PendingTasks() const noexcept override {
        std::lock_guard lock(mutex_);
        return pending_tasks_;
    }
    [[nodiscard]] std::size_t ActiveTasks() const noexcept override {
        std::lock_guard lock(mutex_);
        return active_tasks_;
    }
    [[nodiscard]] std::size_t WorkerCount() const noexcept {
        return config_.worker_count;
    }

    void Shutdown() override {
        std::lock_guard shutdown_lock(shutdown_mutex_);
        const auto caller = std::this_thread::get_id();
        for (const auto& worker : workers_) {
            if (worker.get_id() == caller) {
                throw std::logic_error(
                    "V8 worker pool shutdown cannot run on a worker thread");
            }
        }
        std::vector<CompletionPtr> discarded;
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
            if (!config_.drain_on_shutdown) {
                DiscardQueuedLocked(discarded);
            }
        }
        for (const auto& completion : discarded) {
            if (completion) {
                CompleteSafely(completion,
                    bridge::ResultEnvelope::Cancelled(completion->Request()));
            }
        }
        queue_cv_.notify_all();
        for (auto& worker : workers_) {
            if (worker.joinable()) worker.join();
        }
    }

private:
    struct Task final {
        CompletionPtr completion;
        AsyncScheduleOptions options;
        Clock::time_point submitted_at{};
        std::uint64_t sequence = 0;
    };
    static constexpr std::size_t kPriorityCount = 4;

    struct PendingPromise final {
        CompletionPtr completion;
    };

    void ValidateConfig() {
        if (config_.worker_count == 0 || config_.max_pending_tasks == 0) {
            throw std::invalid_argument("V8 worker and queue limits must be positive");
        }
        if (config_.priority_burst_limit == 0) config_.priority_burst_limit = 1;
        if (config_.max_heap_size_mb == 0 ||
            config_.max_heap_size_mb >
                static_cast<std::size_t>(std::numeric_limits<int>::max()) /
                    (1024u * 1024u)) {
            throw std::invalid_argument("Invalid V8 worker heap size");
        }
    }

    void StartWorkers() {
        workers_.reserve(config_.worker_count);
        std::vector<std::future<void>> startup;
        startup.reserve(config_.worker_count);
        try {
            for (std::size_t index = 0; index < config_.worker_count; ++index) {
                std::promise<void> ready;
                startup.push_back(ready.get_future());
                workers_.emplace_back([this, index, ready = std::move(ready)]() mutable {
                    WorkerLoop(index, std::move(ready));
                });
            }
            for (auto& future : startup) future.get();
        } catch (...) {
            {
                std::lock_guard lock(mutex_);
                stopping_ = true;
            }
            queue_cv_.notify_all();
            for (auto& worker : workers_) {
                if (worker.joinable()) worker.join();
            }
            throw;
        }
    }

    void DiscardQueuedLocked(std::vector<CompletionPtr>& discarded) {
        for (auto& queue : queues_) {
            while (!queue.empty()) {
                discarded.push_back(std::move(queue.front().completion));
                queue.pop_front();
            }
        }
        pending_tasks_ = 0;
    }

    [[nodiscard]] static constexpr std::size_t PriorityIndex(
        TaskPriority priority) noexcept {
        return static_cast<std::size_t>(priority);
    }

    [[nodiscard]] std::optional<Task> PopNextTaskLocked() {
        if (pending_tasks_ == 0) return std::nullopt;
        std::size_t selected = kPriorityCount;
        if (high_priority_burst_ >= config_.priority_burst_limit) {
            auto oldest = std::numeric_limits<std::uint64_t>::max();
            for (std::size_t i = 0; i + 1 < kPriorityCount; ++i) {
                if (!queues_[i].empty() && queues_[i].front().sequence < oldest) {
                    oldest = queues_[i].front().sequence;
                    selected = i;
                }
            }
        }
        if (selected == kPriorityCount) {
            for (std::size_t i = kPriorityCount; i > 0; --i) {
                if (!queues_[i - 1].empty()) {
                    selected = i - 1;
                    break;
                }
            }
        }
        if (selected == kPriorityCount) return std::nullopt;
        Task task = std::move(queues_[selected].front());
        queues_[selected].pop_front();
        --pending_tasks_;
        if (selected == kPriorityCount - 1) {
            if (high_priority_burst_ < config_.priority_burst_limit)
                ++high_priority_burst_;
        } else {
            high_priority_burst_ = 0;
        }
        return task;
    }

    static bridge::ResultEnvelope Failure(const bridge::RequestEnvelope& request,
                                           const char* message,
                                           const char* identifier) {
        return bridge::ResultEnvelope::Failure(request,
            {bridge::OperationErrorCode::ExecutionFailed, message, identifier, false},
            bridge::ExecutionBackend::JavaScript);
    }

    static void CompleteSafely(const CompletionPtr& completion,
                               bridge::ResultEnvelope result) noexcept {
        try {
            (void)completion->Complete(std::move(result));
        } catch (...) {
            // Completion publication is best effort. Never throw across worker loop.
        }
    }

    void DecrementActive() noexcept {
        std::lock_guard lock(mutex_);
        if (active_tasks_ > 0) --active_tasks_;
    }

    void WorkerLoop(std::size_t index, std::promise<void> ready) noexcept {
        ::v8::Isolate* isolate = nullptr;
        std::unique_ptr<::v8::ArrayBuffer::Allocator> allocator;
        try {
            allocator.reset(::v8::ArrayBuffer::Allocator::NewDefaultAllocator());
            ::v8::Isolate::CreateParams params;
            params.array_buffer_allocator = allocator.get();
            // Diagnostic only: use V8's default heap constraints.
            params.constraints.ConfigureDefaultsFromHeapSize(
                0, config_.max_heap_size_mb * 1024 * 1024);
            isolate = ::v8::Isolate::New(params);
            if (!isolate) throw std::runtime_error("Unable to create V8 isolate");
            {
                ::v8::Isolate::Scope isolate_scope(isolate);
                ::v8::HandleScope handle_scope(isolate);
                auto context = ::v8::Context::New(isolate);
                if (context.IsEmpty()) {
                    throw std::runtime_error("Unable to create V8 context");
                }
                ::v8::Context::Scope context_scope(context);
                ready.set_value();
                ExecuteWorkerTasks(index, isolate, context);
            }
        } catch (...) {
            bool startup_failure = false;
            try {
                ready.set_exception(std::current_exception());
                startup_failure = true;
            } catch (...) {
                // Startup was acknowledged already.
            }
            if (!startup_failure) {
                std::vector<CompletionPtr> abandoned;
                {
                    std::lock_guard lock(mutex_);
                    stopping_ = true;
                    DiscardQueuedLocked(abandoned);
                }
                for (const auto& completion : abandoned) {
                    if (completion) {
                        CompleteSafely(completion, Failure(completion->Request(),
                            "V8 worker exited unexpectedly", "V8_WORKER_EXIT"));
                    }
                }
                queue_cv_.notify_all();
            }
        }
        if (isolate) isolate->Dispose();
    }

    void ExecuteWorkerTasks(std::size_t index, ::v8::Isolate* isolate,
                            ::v8::Local<::v8::Context> context) {
        std::unique_ptr<V8PromiseContinuationScheduler> scheduler;
        std::unordered_map<std::uint64_t, PendingPromise> pending;
        if (async_mode_) {
            scheduler = std::make_unique<V8PromiseContinuationScheduler>(isolate,
                V8PromiseSchedulerConfig{
                    async_config_.max_pending_continuations_per_worker,
                    async_config_.microtask_checkpoints_per_pump});
        }
        // A continuation ID is worker-local and independent of RequestId:
        // request IDs are not guaranteed unique across submissions.
        std::uint64_t next_continuation = 1;
        try {
            for (;;) {
                std::optional<Task> task;
                bool stopping = false;
                {
                    std::unique_lock lock(mutex_);
                    if (!pending.empty()) {
                        queue_cv_.wait_for(lock, async_config_.idle_pump_interval,
                            [this] { return stopping_ || pending_tasks_ > 0; });
                    } else {
                        queue_cv_.wait(lock, [this] {
                            return stopping_ || pending_tasks_ > 0;
                        });
                    }
                    stopping = stopping_;
                    if (pending_tasks_ > 0) {
                        // Keep a bounded number of queued invocations moving,
                        // even while previous Promises remain unresolved.
                        task = PopNextTaskLocked();
                        if (task) ++active_tasks_;
                    } else if (stopping && pending.empty()) {
                        break;
                    }
                }
                if (task) {
                    ExecuteTask(*task, index, isolate, context, scheduler.get(),
                                pending, next_continuation);
                }
                if (scheduler && !pending.empty()) {
                    // On shutdown, unresolved Promises cannot be awaited forever:
                    // drain queued work, then abandon remaining continuations.
                    if (stopping) {
                        bool queues_empty;
                        {
                            std::lock_guard lock(mutex_);
                            queues_empty = pending_tasks_ == 0;
                        }
                        if (queues_empty) {
                            (void)scheduler->AbandonAll();
                        } else {
                            PumpPromises(*scheduler, pending, isolate);
                        }
                    } else {
                        PumpPromises(*scheduler, pending, isolate);
                    }
                }
                // If the final pending Promise settled in this iteration,
                // the next iteration observes shutdown and exits.
            }
        } catch (...) {
            // Worker-local cleanup must occur while its isolate is entered.
            if (scheduler) {
                try { (void)scheduler->AbandonAll(); } catch (...) {}
            }
            for (auto& [id, entry] : pending) {
                CompleteSafely(entry.completion, Failure(entry.completion->Request(),
                    "V8 continuation processing failed", "V8_CONTINUATION_FAILURE"));
                DecrementActive();
            }
            pending.clear();
            throw;
        }
    }

    void PumpPromises(V8PromiseContinuationScheduler& scheduler,
                      std::unordered_map<std::uint64_t, PendingPromise>& pending,
                      ::v8::Isolate* isolate) {
        ::v8::HandleScope scope(isolate);
        std::vector<std::uint64_t> cancelled;
        std::vector<std::uint64_t> expired;
        cancelled.reserve(pending.size());
        expired.reserve(pending.size());
        for (const auto& [id, entry] : pending) {
            if (entry.completion->CancellationRequested()) cancelled.push_back(id);
            else if (entry.completion->Request().IsExpired()) expired.push_back(id);
        }
        for (const auto id : cancelled) (void)scheduler.Cancel(id);
        for (const auto id : expired) {
            const auto it = pending.find(id);
            if (it != pending.end()) {
                CompleteSafely(it->second.completion,
                    bridge::ResultEnvelope::DeadlineExceeded(it->second.completion->Request()));
            }
            (void)scheduler.Cancel(id);
        }
        (void)scheduler.Pump();
    }

    void ExecuteTask(Task& task, std::size_t index, ::v8::Isolate* isolate,
                     ::v8::Local<::v8::Context> context,
                     V8PromiseContinuationScheduler* scheduler,
                     std::unordered_map<std::uint64_t, PendingPromise>& pending,
                     std::uint64_t& next_continuation) noexcept {
        const auto completion = task.completion;
        bool deferred = false;
        // All return paths decrement active accounting except successful
        // Promise registration, which transfers that responsibility to callback.
        const auto finish = [this, &deferred] {
            if (!deferred) DecrementActive();
        };
        if (!completion) { finish(); return; }
        const auto& request = completion->Request();
        if (completion->CancellationRequested()) {
            CompleteSafely(completion, bridge::ResultEnvelope::Cancelled(request));
            finish(); return;
        }
        if (request.IsExpired()) {
            CompleteSafely(completion, bridge::ResultEnvelope::DeadlineExceeded(request));
            finish(); return;
        }
        if (task.options.max_queue_wait.count() > 0 &&
            Clock::now() - task.submitted_at > task.options.max_queue_wait) {
            CompleteSafely(completion, bridge::ResultEnvelope::Failure(request,
                {bridge::OperationErrorCode::ResourceExhausted,
                 "V8 task exceeded queue wait limit", "V8_QUEUE_WAIT_EXCEEDED", true}));
            finish(); return;
        }
        if (!completion->MarkRunning()) { finish(); return; }
        try {
            ::v8::HandleScope scope(isolate);
            V8WorkerExecutionContext worker_context{index, isolate, context};
            if (!async_mode_) {
                CompleteSafely(completion, sync_executor_(request, worker_context));
            } else {
                auto result = async_executor_(request, worker_context);
                if (result.IsCompleted()) {
                    CompleteSafely(completion, std::move(result).TakeCompletedResult());
                } else if (!scheduler ||
                           pending.size() >= async_config_.max_pending_continuations_per_worker) {
                    CompleteSafely(completion, bridge::ResultEnvelope::Failure(request,
                        {bridge::OperationErrorCode::ResourceExhausted,
                         "V8 Promise continuation capacity exhausted",
                         "V8_PROMISE_CAPACITY", true},
                        bridge::ExecutionBackend::JavaScript));
                } else {
                    const auto id = next_continuation++;
                    // Insert bookkeeping first: a registration failure can then
                    // erase it without leaving a dangling continuation callback.
                    pending.emplace(id, PendingPromise{completion});
                    auto callback = [this, &pending, id, completion](
                        const V8PromiseSettlement& settlement,
                        ::v8::Local<::v8::Value> value) {
                        // Mapper and completion callbacks must not mutate the
                        // scheduler while it is pumping.
                        bridge::ResultEnvelope mapped;
                        const auto& req = completion->Request();
                        try {
                            switch (settlement.kind) {
                                case V8PromiseSettlementKind::Cancelled:
                                    mapped = bridge::ResultEnvelope::Cancelled(req); break;
                                case V8PromiseSettlementKind::DeadlineExceeded:
                                    mapped = bridge::ResultEnvelope::DeadlineExceeded(req); break;
                                case V8PromiseSettlementKind::Abandoned:
                                    mapped = Failure(req, "V8 Promise abandoned before settlement",
                                                     "V8_PROMISE_ABANDONED"); break;
                                case V8PromiseSettlementKind::Fulfilled:
                                case V8PromiseSettlementKind::Rejected:
                                    mapped = mapper_(req, settlement.kind, value); break;
                            }
                        } catch (...) {
                            mapped = Failure(req, "V8 Promise result mapping failed",
                                             "V8_PROMISE_MAPPING_EXCEPTION");
                        }
                        // Deadline results published before scheduler.Cancel()
                        // remain authoritative due to exactly-once completion.
                        CompleteSafely(completion, std::move(mapped));
                        pending.erase(id);
                        DecrementActive();
                    };
                    try {
                        if (!scheduler->Register(id, result.PromiseHandle(),
                                                 std::move(callback))) {
                            pending.erase(id);
                            CompleteSafely(completion, bridge::ResultEnvelope::Failure(request,
                                {bridge::OperationErrorCode::ResourceExhausted,
                                 "V8 Promise registration rejected",
                                 "V8_PROMISE_REGISTER", true},
                                bridge::ExecutionBackend::JavaScript));
                        } else {
                            deferred = true;
                        }
                    } catch (...) {
                        pending.erase(id);
                        throw;
                    }
                }
            }
        } catch (...) {
            CompleteSafely(completion, Failure(request,
                "V8 worker execution failed", "V8_WORKER_EXCEPTION"));
        }
        finish();
    }

    V8WorkerOperationExecutor sync_executor_;
    V8WorkerAsyncOperationExecutor async_executor_;
    V8WorkerPromiseResultMapper mapper_;
    V8IsolateWorkerPoolConfig config_;
    V8WorkerAsyncExecutionConfig async_config_;
    bool async_mode_ = false;

    mutable std::mutex mutex_;
    std::mutex shutdown_mutex_;
    std::condition_variable queue_cv_;
    std::array<std::deque<Task>, kPriorityCount> queues_;
    std::vector<std::thread> workers_;
    std::size_t pending_tasks_ = 0;
    std::size_t active_tasks_ = 0;
    std::uint64_t next_sequence_ = 1;
    std::size_t high_priority_burst_ = 0;
    bool stopping_ = false;
};

} // namespace lamp::execution
