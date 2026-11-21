
#include "bridge/async_operation_coordinator.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace bridge = lamp::bridge;
namespace execution = lamp::execution;

using Coordinator = bridge::AsyncOperationCoordinator;
using TaskHandle = execution::AsyncTaskHandle;
using TaskCompletion = execution::AsyncTaskCompletion;
using SubmitResult = execution::AsyncSubmitResult;
using SubmitStatus = execution::AsyncSubmitStatus;
using ScheduleOptions = execution::AsyncScheduleOptions;

void Require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

bridge::RequestEnvelope MakeRequest(
    std::uint64_t id
) {
    bridge::RequestEnvelope request{};
    request.request_id = id;
    return request;
}

// A deterministic scheduler. No background workers are
// started. Tests explicitly control task completion.

class MockScheduler final
    : public execution::IAsyncExecutionScheduler {
public:
    SubmitResult Submit(
        bridge::RequestEnvelope request,
        ScheduleOptions,
        TaskHandle& output
    ) override {
        if (!running_) {
            return SubmitResult::Failure(
                SubmitStatus::SchedulerStopped
            );
        }

        if (rejectNext_) {
            rejectNext_ = false;

            return SubmitResult::Failure(
                SubmitStatus::QueueFull
            );
        }

        auto completion =
            std::make_shared<TaskCompletion>(
                std::move(request)
            );

        Require(
            completion->MarkQueued(),
            "Mock task must enter queued state"
        );

        output = TaskHandle(completion);
        tasks_.push_back(std::move(completion));

        return SubmitResult::Success();
    }

    bool IsRunning() const noexcept override {
        return running_;
    }

    std::size_t PendingTasks() const noexcept override {
        return tasks_.size();
    }

    std::size_t ActiveTasks() const noexcept override {
        return 0;
    }

    void Shutdown() override {
        running_ = false;
    }

    void RejectNext() {
        rejectNext_ = true;
    }

    void Complete(
        std::size_t index,
        bridge::ResultEnvelope result = {}
    ) {
        Require(
            index < tasks_.size(),
            "Invalid mock task index"
        );

        auto& task = tasks_[index];

        Require(
            task->MarkRunning(),
            "Mock task must enter running state"
        );

        Require(
            task->Complete(std::move(result)),
            "Mock task completion must succeed"
        );
    }

    TaskHandle Handle(std::size_t index) const {
        return TaskHandle(tasks_.at(index));
    }

    std::size_t SubmittedCount() const noexcept {
        return tasks_.size();
    }

private:
    bool running_ = true;
    bool rejectNext_ = false;

    std::vector<std::shared_ptr<TaskCompletion>> tasks_;
};

void TestConstruction() {
    auto scheduler = std::make_shared<MockScheduler>();

    Coordinator coordinator(scheduler, 8);

    Require(
        coordinator.Capacity() == 8,
        "Incorrect coordinator capacity"
    );

    Require(
        coordinator.Outstanding() == 0,
        "New coordinator must be empty"
    );

    Require(
        !coordinator.Closed(),
        "New coordinator must be open"
    );
}

void TestNullSchedulerRejected() {
    bool threw = false;

    try {
        Coordinator coordinator(nullptr);
    } catch (
        const bridge::AsyncOperationCoordinatorError&
    ) {
        threw = true;
    }

    Require(threw, "Null scheduler must be rejected");
}

void TestZeroCapacityRejected() {
    auto scheduler = std::make_shared<MockScheduler>();

    bool threw = false;

    try {
        Coordinator coordinator(scheduler, 0);
    } catch (
        const bridge::AsyncOperationCoordinatorError&
    ) {
        threw = true;
    }

    Require(threw, "Zero capacity must be rejected");
}

void TestSuccessfulSubmission() {
    auto scheduler = std::make_shared<MockScheduler>();
    Coordinator coordinator(scheduler, 4);

    auto result = coordinator.Submit(
        MakeRequest(101),
        {},
        [](TaskHandle) {}
    );

    Require(result.Accepted(), "Submission failed");
    Require(
        scheduler->SubmittedCount() == 1,
        "Scheduler did not receive operation"
    );
    Require(
        coordinator.Outstanding() == 1,
        "Outstanding count is incorrect"
    );
}

void TestEmptyCallbackRejected() {
    auto scheduler = std::make_shared<MockScheduler>();
    Coordinator coordinator(scheduler);

    auto result = coordinator.Submit(
        MakeRequest(102),
        {},
        {}
    );

    Require(
        result.status == SubmitStatus::InvalidRequest,
        "Empty callback must be rejected"
    );

    Require(
        scheduler->SubmittedCount() == 0,
        "Rejected submission reached scheduler"
    );
}

void TestCapacityLimit() {
    auto scheduler = std::make_shared<MockScheduler>();
    Coordinator coordinator(scheduler, 1);

    auto first = coordinator.Submit(
        MakeRequest(201),
        {},
        [](TaskHandle) {}
    );

    auto second = coordinator.Submit(
        MakeRequest(202),
        {},
        [](TaskHandle) {}
    );

    Require(first.Accepted(), "First submission failed");

    Require(
        second.status == SubmitStatus::QueueFull,
        "Capacity limit was not enforced"
    );

    Require(
        scheduler->SubmittedCount() == 1,
        "Excess operation reached scheduler"
    );
}

void TestSchedulerRejection() {
    auto scheduler = std::make_shared<MockScheduler>();
    Coordinator coordinator(scheduler, 2);

    scheduler->RejectNext();

    auto result = coordinator.Submit(
        MakeRequest(301),
        {},
        [](TaskHandle) {}
    );

    Require(
        result.status == SubmitStatus::QueueFull,
        "Scheduler rejection was not propagated"
    );

    Require(
        coordinator.Outstanding() == 0,
        "Rejected operation consumed capacity"
    );
}

void TestIncompleteTaskNotDelivered() {
    auto scheduler = std::make_shared<MockScheduler>();
    Coordinator coordinator(scheduler);

    int callbacks = 0;

    auto result = coordinator.Submit(
        MakeRequest(401),
        {},
        [&](TaskHandle) {
            ++callbacks;
        }
    );

    Require(result.Accepted(), "Submission failed");

    Require(
        coordinator.Poll() == 0,
        "Incomplete task was delivered"
    );

    Require(
        callbacks == 0,
        "Incomplete task callback was invoked"
    );
}

void TestCompletedTaskDelivered() {
    auto scheduler = std::make_shared<MockScheduler>();
    Coordinator coordinator(scheduler);

    int callbacks = 0;
    std::uint64_t deliveredId = 0;

    auto result = coordinator.Submit(
        MakeRequest(501),
        {},
        [&](TaskHandle handle) {
            ++callbacks;
            deliveredId = handle.RequestId();

            Require(
                handle.IsCompleted(),
                "Delivered task is incomplete"
            );
        }
    );

    Require(result.Accepted(), "Submission failed");

    scheduler->Complete(0);

    Require(
        coordinator.Poll() == 1,
        "Completed task was not delivered"
    );

    Require(callbacks == 1, "Callback count incorrect");

    Require(
        deliveredId == 501,
        "Request identity was not preserved"
    );

    Require(
        coordinator.Outstanding() == 0,
        "Completed operation was not released"
    );
}

void TestExactlyOnceDelivery() {
    auto scheduler = std::make_shared<MockScheduler>();
    Coordinator coordinator(scheduler);

    int callbacks = 0;

    Require(
        coordinator.Submit(
            MakeRequest(601),
            {},
            [&](TaskHandle) {
                ++callbacks;
            }
        ).Accepted(),
        "Submission failed"
    );

    scheduler->Complete(0);

    Require(coordinator.Poll() == 1, "First poll failed");
    Require(coordinator.Poll() == 0, "Duplicate delivery");

    Require(
        callbacks == 1,
        "Completion callback executed more than once"
    );
}

void TestMultipleCompletions() {
    auto scheduler = std::make_shared<MockScheduler>();
    Coordinator coordinator(scheduler);

    std::vector<std::uint64_t> delivered;

    for (std::uint64_t id = 1; id <= 3; ++id) {
        Require(
            coordinator.Submit(
                MakeRequest(id),
                {},
                [&](TaskHandle handle) {
                    delivered.push_back(handle.RequestId());
                }
            ).Accepted(),
            "Submission failed"
        );
    }

    scheduler->Complete(2);
    scheduler->Complete(0);

    Require(
        coordinator.Poll() == 2,
        "Incorrect ready completion count"
    );

    Require(
        delivered.size() == 2 &&
        delivered[0] == 1 &&
        delivered[1] == 3,
        "Ready completions were delivered incorrectly"
    );

    Require(
        coordinator.Outstanding() == 1,
        "Pending task was incorrectly removed"
    );

    scheduler->Complete(1);

    Require(
        coordinator.Poll() == 1,
        "Remaining task was not delivered"
    );
}

void TestCallbackCanSubmit() {
    auto scheduler = std::make_shared<MockScheduler>();
    Coordinator coordinator(scheduler, 1);

    bool nestedAccepted = false;

    Require(
        coordinator.Submit(
            MakeRequest(701),
            {},
            [&](TaskHandle) {
                nestedAccepted = coordinator.Submit(
                    MakeRequest(702),
                    {},
                    [](TaskHandle) {}
                ).Accepted();
            }
        ).Accepted(),
        "Initial submission failed"
    );

    scheduler->Complete(0);

    Require(coordinator.Poll() == 1, "Poll failed");

    Require(
        nestedAccepted,
        "Callback could not submit after capacity release"
    );

    Require(
        coordinator.Outstanding() == 1,
        "Nested submission was not retained"
    );
}

void TestCallbackExceptionIsolation() {
    auto scheduler = std::make_shared<MockScheduler>();
    Coordinator coordinator(scheduler);

    int successfulCallbacks = 0;

    Require(
        coordinator.Submit(
            MakeRequest(801),
            {},
            [](TaskHandle) {
                throw std::runtime_error("Callback failure");
            }
        ).Accepted(),
        "First submission failed"
    );

    Require(
        coordinator.Submit(
            MakeRequest(802),
            {},
            [&](TaskHandle) {
                ++successfulCallbacks;
            }
        ).Accepted(),
        "Second submission failed"
    );

    scheduler->Complete(0);
    scheduler->Complete(1);

    Require(
        coordinator.Poll() == 2,
        "Both callbacks must be attempted"
    );

    Require(
        successfulCallbacks == 1,
        "Callback exception blocked another completion"
    );
}

void TestCancelOutstanding() {
    auto scheduler = std::make_shared<MockScheduler>();
    Coordinator coordinator(scheduler);

    Require(
        coordinator.Submit(
            MakeRequest(901),
            {},
            [](TaskHandle) {}
        ).Accepted(),
        "Submission failed"
    );

    coordinator.CancelOutstanding();

    Require(
        scheduler->Handle(0).CancellationRequested(),
        "Outstanding operation was not cancelled"
    );

    Require(
        coordinator.Outstanding() == 1,
        "Cancellation must not silently remove operation"
    );
}

void TestClose() {
    auto scheduler = std::make_shared<MockScheduler>();
    Coordinator coordinator(scheduler);

    int callbacks = 0;

    Require(
        coordinator.Submit(
            MakeRequest(1001),
            {},
            [&](TaskHandle) {
                ++callbacks;
            }
        ).Accepted(),
        "Submission failed"
    );

    coordinator.Close();

    Require(coordinator.Closed(), "Coordinator did not close");

    Require(
        coordinator.Outstanding() == 0,
        "Close did not release pending entries"
    );

    Require(
        scheduler->Handle(0).CancellationRequested(),
        "Close did not request cancellation"
    );

    Require(
        coordinator.Poll() == 0,
        "Closed coordinator delivered callbacks"
    );

    Require(
        callbacks == 0,
        "Close must discard pending callbacks"
    );

    coordinator.Close();
}

void TestSubmitAfterClose() {
    auto scheduler = std::make_shared<MockScheduler>();
    Coordinator coordinator(scheduler);

    coordinator.Close();

    auto result = coordinator.Submit(
        MakeRequest(1101),
        {},
        [](TaskHandle) {}
    );

    Require(
        result.status == SubmitStatus::SchedulerStopped,
        "Closed coordinator accepted submission"
    );
}

void TestCapacityReusedAfterCompletion() {
    auto scheduler = std::make_shared<MockScheduler>();
    Coordinator coordinator(scheduler, 1);

    Require(
        coordinator.Submit(
            MakeRequest(1201),
            {},
            [](TaskHandle) {}
        ).Accepted(),
        "First submission failed"
    );

    scheduler->Complete(0);

    Require(coordinator.Poll() == 1, "Poll failed");

    Require(
        coordinator.Submit(
            MakeRequest(1202),
            {},
            [](TaskHandle) {}
        ).Accepted(),
        "Capacity was not reused"
    );
}

} // namespace

int main() {
    struct TestCase {
        const char* name;
        std::function<void()> run;
    };

    const std::vector<TestCase> tests = {
        {"Construction", TestConstruction},
        {"NullSchedulerRejected", TestNullSchedulerRejected},
        {"ZeroCapacityRejected", TestZeroCapacityRejected},
        {"SuccessfulSubmission", TestSuccessfulSubmission},
        {"EmptyCallbackRejected", TestEmptyCallbackRejected},
        {"CapacityLimit", TestCapacityLimit},
        {"SchedulerRejection", TestSchedulerRejection},
        {"IncompleteTaskNotDelivered", TestIncompleteTaskNotDelivered},
        {"CompletedTaskDelivered", TestCompletedTaskDelivered},
        {"ExactlyOnceDelivery", TestExactlyOnceDelivery},
        {"MultipleCompletions", TestMultipleCompletions},
        {"CallbackCanSubmit", TestCallbackCanSubmit},
        {"CallbackExceptionIsolation", TestCallbackExceptionIsolation},
        {"CancelOutstanding", TestCancelOutstanding},
        {"Close", TestClose},
        {"SubmitAfterClose", TestSubmitAfterClose},
        {"CapacityReusedAfterCompletion",
         TestCapacityReusedAfterCompletion}
    };

    std::size_t passed = 0;

    for (const auto& test : tests) {
        try {
            test.run();
            ++passed;
            std::cout << "[PASS] " << test.name << '\n';
        } catch (const std::exception& error) {
            std::cerr
                << "[FAIL] " << test.name
                << ": " << error.what() << '\n';

            return 1;
        } catch (...) {
            std::cerr
                << "[FAIL] " << test.name
                << ": unknown exception\n";

            return 1;
        }
    }

    std::cout
        << "Lamp async operation coordinator: "
        << passed << "/" << tests.size()
        << " tests passed\n";

    return 0;
}
