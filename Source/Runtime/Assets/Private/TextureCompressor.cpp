#include "TextureCompressor.h"

#include <Assets/TextureCompression.h>
#include <Core/Jobs.h>

#include <SDL3/SDL.h>

#include <algorithm>

namespace shockwave {

TextureCompressor::TextureCompressor() : m_state(std::make_shared<State>()) {}

TextureCompressor::~TextureCompressor() {
    bool busy = false;
    {
        std::lock_guard lock(m_state->mutex);
        m_state->stop = true;
        busy = m_state->busy;
    }
    m_state->wake.notify_all();
    if (!m_thread.joinable()) return;
    // An encode can take seconds and can't be interrupted. Rather than hold up quitting,
    // let it finish on its own: it only touches the shared state, and the cache file is
    // written through a temporary, so a process exiting under it leaves nothing broken.
    if (busy)
        m_thread.detach();
    else
        m_thread.join();
}

void TextureCompressor::add(std::shared_ptr<Task> task) {
    {
        std::lock_guard lock(m_state->mutex);
        // An older version still waiting is no longer wanted.
        std::erase_if(m_state->tasks, [&](const std::shared_ptr<Task>& t) {
            return t->asset == task->asset && t->modelTexture == task->modelTexture && !t->started.load();
        });
        m_state->tasks.push_back(std::move(task));
        if (!m_thread.joinable()) m_thread = std::thread([state = m_state] { loop(*state); });
    }
    m_state->wake.notify_one();
}

std::vector<std::shared_ptr<TextureCompressor::Task>> TextureCompressor::takeFinished() {
    std::vector<std::shared_ptr<Task>> finished;
    std::lock_guard lock(m_state->mutex);
    auto& tasks = m_state->tasks;
    for (auto it = tasks.begin(); it != tasks.end();) {
        if ((*it)->done.load(std::memory_order_acquire)) {
            finished.push_back(std::move(*it));
            it = tasks.erase(it);
        } else
            ++it;
    }
    return finished;
}

size_t TextureCompressor::pending() const {
    std::lock_guard lock(m_state->mutex);
    return static_cast<size_t>(std::count_if(m_state->tasks.begin(), m_state->tasks.end(), [](const auto& t) {
        return !t->done.load();
    }));
}

void TextureCompressor::finish(JobSystem& jobs) {
    std::vector<std::shared_ptr<Task>> waiting;
    {
        std::lock_guard lock(m_state->mutex);
        for (const auto& task : m_state->tasks)
            if (!task->started.load()) waiting.push_back(task);
    }
    jobs.parallelFor(waiting.size(), 1, [&](size_t begin, size_t end) {
        for (size_t i = begin; i < end; ++i) {
            if (waiting[i]->started.exchange(true)) continue;   // the background thread got there first
            run(*waiting[i]);
            { std::lock_guard lock(m_state->mutex); }   // so a waiter can't miss the notification
            m_state->progress.notify_all();
        }
    });
    std::unique_lock lock(m_state->mutex);
    waitUntilIdle(lock);
}

void TextureCompressor::clear() {
    std::unique_lock lock(m_state->mutex);
    std::erase_if(m_state->tasks, [](const std::shared_ptr<Task>& t) { return !t->started.load(); });
    waitUntilIdle(lock);
    m_state->tasks.clear();
}

void TextureCompressor::waitUntilIdle(std::unique_lock<std::mutex>& lock) {
    m_state->progress.wait(lock, [&] {
        return std::all_of(m_state->tasks.begin(), m_state->tasks.end(), [](const std::shared_ptr<Task>& t) {
            return !t->started.load() || t->done.load();
        });
    });
}

void TextureCompressor::run(Task& task) {
    std::vector<uint8_t> ktx2;
    if (encodeKtx2(task.image, task.mode, task.srgb, ktx2, task.error)) {
        task.compressedBytes = ktx2.size();
        std::string cacheError;
        if (!task.cacheFile.empty() && !storeCachedTexture(task.cacheFile, ktx2, cacheError))
            task.error = "compressed, but not cached: " + cacheError;
        std::string transcodeError;
        if (!transcodeKtx2(ktx2, task.bc7, task.texture, transcodeError)) task.error = transcodeError;
    }
    task.image = {};   // the pixels aren't needed any more
    task.done.store(true, std::memory_order_release);
}

void TextureCompressor::loop(State& state) {
    // Below the engine's threads: frames come first, compression fills idle time.
    SDL_SetCurrentThreadPriority(SDL_THREAD_PRIORITY_LOW);
    std::unique_lock lock(state.mutex);
    while (true) {
        std::shared_ptr<Task> next;
        state.wake.wait(lock, [&] {
            if (state.stop) return true;
            for (const auto& task : state.tasks) {
                if (!task->started.exchange(true)) {
                    next = task;
                    return true;
                }
            }
            return false;
        });
        if (state.stop) return;
        state.busy = true;
        lock.unlock();
        run(*next);
        lock.lock();
        state.busy = false;
        state.progress.notify_all();
    }
}

}   // namespace shockwave
