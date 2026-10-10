#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace shockwave {

// Tracks a group of jobs. wait() on it returns once every job in the group is done.
struct JobCounter {
    std::atomic<uint32_t> pending{0};
    bool done() const { return pending.load(std::memory_order_acquire) == 0; }
};

// Work-stealing job system. One queue per thread; idle workers steal from busy ones,
// and a thread that waits on a counter runs jobs instead of blocking.
//
// Thread 0 is the main thread, 1..workerCount() are workers. threadIndex() lets
// systems keep per-thread data (draw lists, command pools, allocators) without locks.
class JobSystem {
public:
    using Job = std::function<void()>;

    // workerCount 0 = one worker per hardware thread, minus the main thread.
    explicit JobSystem(uint32_t workerCount = 0);
    ~JobSystem();

    JobSystem(const JobSystem&) = delete;
    JobSystem& operator=(const JobSystem&) = delete;

    void run(Job job, JobCounter* counter = nullptr);

    // Runs fn(begin, end) over [0, count) in batches across all threads and returns when done.
    void parallelFor(size_t count, size_t batchSize, const std::function<void(size_t, size_t)>& fn);

    // Runs other jobs until the counter reaches zero.
    void wait(JobCounter& counter);

    uint32_t workerCount() const { return static_cast<uint32_t>(m_workers.size()); }
    uint32_t threadCount() const { return workerCount() + 1; }

    // Index of the calling thread: 0 for main, 1..workerCount() for workers,
    // kForeignThread for threads the job system doesn't own.
    static uint32_t threadIndex();
    static constexpr uint32_t kForeignThread = UINT32_MAX;

private:
    struct Task {
        Job job;
        JobCounter* counter = nullptr;
    };
    struct Queue;

    bool tryRunOne(uint32_t self);
    bool pop(uint32_t self, Task& out);
    void workerLoop(uint32_t index);

    std::vector<std::unique_ptr<Queue>> m_queues;   // one per thread, index = threadIndex()
    std::vector<std::thread> m_workers;
    std::atomic<uint32_t> m_queued{0};
    std::atomic<uint32_t> m_nextForeignQueue{0};
    std::atomic<bool> m_stop{false};
    std::mutex m_sleepMutex;
    std::condition_variable m_wake;
};

} // namespace shockwave
