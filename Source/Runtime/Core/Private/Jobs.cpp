#include <Core/Jobs.h>

#include <Core/Profile.h>

#include <algorithm>
#include <deque>
#include <string>

namespace shockwave {

namespace {
thread_local uint32_t t_threadIndex = JobSystem::kForeignThread;
}

// A small locked deque per thread: the owner pushes and pops at the back (LIFO, cache-warm),
// thieves take from the front (FIFO, oldest and usually largest work first).
// Locks are uncontended almost all the time; a lock-free Chase-Lev deque can replace this
// without touching the interface once profiling says it matters.
struct alignas(64) JobSystem::Queue {
    std::mutex mutex;
    std::deque<Task> tasks;
};

JobSystem::JobSystem(uint32_t workerCount) {
    if (workerCount == 0) {
        const uint32_t hw = std::thread::hardware_concurrency();
        workerCount = hw > 1 ? hw - 1 : 1;
    }

    t_threadIndex = 0;   // the constructing thread is the main thread
    for (uint32_t i = 0; i < workerCount + 1; ++i) {
        m_queues.push_back(std::make_unique<Queue>());
    }
    m_workers.reserve(workerCount);
    for (uint32_t i = 1; i <= workerCount; ++i) {
        m_workers.emplace_back([this, i] { workerLoop(i); });
    }
}

JobSystem::~JobSystem() {
    {
        std::lock_guard lock(m_sleepMutex);
        m_stop.store(true);
    }
    m_wake.notify_all();
    for (std::thread& t : m_workers) t.join();
}

uint32_t JobSystem::threadIndex() { return t_threadIndex; }

void JobSystem::run(Job job, JobCounter* counter) {
    if (counter) counter->pending.fetch_add(1, std::memory_order_relaxed);

    uint32_t target = t_threadIndex;
    if (target == kForeignThread || target >= m_queues.size()) {
        target = m_nextForeignQueue.fetch_add(1, std::memory_order_relaxed) % m_queues.size();
    }
    {
        std::lock_guard lock(m_queues[target]->mutex);
        m_queues[target]->tasks.push_back({std::move(job), counter});
    }
    m_queued.fetch_add(1, std::memory_order_release);
    {
        // Taking the lock orders this wake against a worker that is about to sleep.
        std::lock_guard lock(m_sleepMutex);
    }
    m_wake.notify_one();
}

bool JobSystem::pop(uint32_t self, Task& out) {
    const uint32_t count = static_cast<uint32_t>(m_queues.size());
    if (self < count) {
        Queue& own = *m_queues[self];
        std::lock_guard lock(own.mutex);
        if (!own.tasks.empty()) {
            out = std::move(own.tasks.back());
            own.tasks.pop_back();
            return true;
        }
    }
    const uint32_t start = self < count ? self + 1 : 0;
    for (uint32_t i = 0; i < count; ++i) {
        Queue& victim = *m_queues[(start + i) % count];
        std::lock_guard lock(victim.mutex);
        if (!victim.tasks.empty()) {
            out = std::move(victim.tasks.front());
            victim.tasks.pop_front();
            return true;
        }
    }
    return false;
}

bool JobSystem::tryRunOne(uint32_t self) {
    Task task;
    if (!pop(self, task)) return false;
    m_queued.fetch_sub(1, std::memory_order_relaxed);
    task.job();
    if (task.counter) task.counter->pending.fetch_sub(1, std::memory_order_release);
    return true;
}

void JobSystem::workerLoop(uint32_t index) {
    t_threadIndex = index;
    SW_PROFILE_THREAD(("Worker " + std::to_string(index)).c_str());
    while (true) {
        if (tryRunOne(index)) continue;

        std::unique_lock lock(m_sleepMutex);
        m_wake.wait(lock, [this] { return m_stop.load() || m_queued.load(std::memory_order_acquire) > 0; });
        if (m_stop.load() && m_queued.load() == 0) return;
    }
}

void JobSystem::wait(JobCounter& counter) {
    const uint32_t self = t_threadIndex;
    while (!counter.done()) {
        if (!tryRunOne(self)) std::this_thread::yield();
    }
}

void JobSystem::parallelFor(size_t count, size_t batchSize, const std::function<void(size_t, size_t)>& fn) {
    if (count == 0) return;
    batchSize = std::max<size_t>(1, batchSize);

    JobCounter counter;
    for (size_t begin = batchSize; begin < count; begin += batchSize) {
        const size_t end = std::min(count, begin + batchSize);
        run([&fn, begin, end] { fn(begin, end); }, &counter);
    }
    fn(0, std::min(count, batchSize));   // the caller takes the first batch itself
    wait(counter);
}

}   // namespace shockwave
