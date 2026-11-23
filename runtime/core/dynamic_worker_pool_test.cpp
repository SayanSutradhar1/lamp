#include "core/dynamic_worker_pool.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <set>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

using namespace lamp::core;
using namespace std::chrono_literals;

int g_failed = 0;

void Expect(bool cond, const char* expr, const char* file, int line) {
    if (!cond) {
        std::cerr << "[FAIL] " << file << ":" << line << " (" << expr << ")\n";
        ++g_failed;
    }
}

#define EXPECT(expr) Expect(static_cast<bool>(expr), #expr, __FILE__, __LINE__)

bool TestDynamicScaleUpAndIdleScaleDown() {
    DynamicWorkerPoolConfig cfg;
    cfg.min_threads = 2;
    cfg.max_threads = 6;
    cfg.idle_timeout = 80ms;

    DynamicWorkerPool pool(cfg);
    EXPECT(pool.CurrentThreadCount() == 2);

    // Burst 6 concurrent blocking tasks -> should scale up dynamically toward 6
    std::atomic<int> arrived{0};
    std::atomic<bool> release{false};
    std::mutex tids_mutex;
    std::set<std::thread::id> seen_tids;

    std::vector<std::future<void>> futs;
    for (int i = 0; i < 6; ++i) {
        futs.push_back(pool.Submit([&] {
            {
                std::lock_guard lock(tids_mutex);
                seen_tids.insert(std::this_thread::get_id());
            }
            arrived.fetch_add(1, std::memory_order_acq_rel);
            while (!release.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(2ms);
            }
        }));
    }

    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (arrived.load(std::memory_order_acquire) < 6 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(5ms);
    }

    EXPECT(arrived.load() == 6);
    EXPECT(pool.PeakThreadCount() >= 4);
    EXPECT(seen_tids.size() >= 4);

    release.store(true, std::memory_order_release);
    for (auto& f : futs) {
        f.get();
    }

    EXPECT(pool.TotalCompletedTasks() == 6);

    // Wait past idle_timeout -> transient extra workers exit back down to min_threads=2
    const auto scale_down_deadline = std::chrono::steady_clock::now() + 2s;
    while (pool.CurrentThreadCount() > 2 &&
           std::chrono::steady_clock::now() < scale_down_deadline) {
        std::this_thread::sleep_for(10ms);
    }
    EXPECT(pool.CurrentThreadCount() == 2);
    return true;
}

bool TestParallelSubtaskSplitting() {
    DynamicWorkerPoolConfig cfg;
    cfg.min_threads = 2;
    cfg.max_threads = 8;
    cfg.idle_timeout = 100ms;

    DynamicWorkerPool pool(cfg);

    std::vector<std::function<int()>> subtasks;
    for (int i = 1; i <= 16; ++i) {
        subtasks.push_back([i]() -> int {
            std::this_thread::sleep_for(5ms);
            return i * i;
        });
    }

    const std::vector<int> outputs = pool.RunParallel<int>(std::move(subtasks));
    EXPECT(outputs.size() == 16);
    for (int i = 1; i <= 16; ++i) {
        EXPECT(outputs[static_cast<std::size_t>(i - 1)] == i * i);
    }
    EXPECT(pool.PeakThreadCount() >= 3);

    // Exception propagation in parallel subtasks
    std::vector<std::function<int()>> failing;
    failing.push_back([]() { return 1; });
    failing.push_back([]() -> int { throw std::runtime_error("subtask boom"); });
    bool caught = false;
    try {
        (void)pool.RunParallel<int>(std::move(failing));
    } catch (const std::runtime_error&) {
        caught = true;
    }
    EXPECT(caught);

    return true;
}

} // namespace

int main() {
    const bool ok1 = TestDynamicScaleUpAndIdleScaleDown();
    const bool ok2 = TestParallelSubtaskSplitting();
    if (!ok1 || !ok2 || g_failed != 0) {
        std::cout << "Lamp dynamic worker pool: FAILED (" << g_failed << " assertions)\n";
        return 1;
    }
    std::cout << "Lamp dynamic worker pool: 2/2 tests passed\n";
    return 0;
}
