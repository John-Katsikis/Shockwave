#pragma once

#include <Assets/Image.h>
#include <Assets/ImportSettings.h>
#include <Renderer/TextureData.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace shockwave {

class JobSystem;

// AssetFolder's background texture compression (docs/Optimisation.md). Encoding takes
// seconds, so it runs one texture at a time on a low-priority thread of its own: the job
// system's workers stay free for frames, and frame pacing stays steady while it works.
// Until a texture is done it's drawn uncompressed.
class TextureCompressor {
public:
    struct Task {
        // Which asset, and which version of it, the result belongs to.
        std::string asset;
        std::filesystem::file_time_type modified{};
        uintmax_t bytes = 0;
        int modelTexture = -1;   // a model's texture index, or -1 for a texture file
        std::shared_ptr<const void> owner;   // the model version it was made for

        Image image;
        TextureCompression mode = TextureCompression::High;
        bool srgb = true;
        float coverageCutoff = -1.0f;   // >= 0: mipmaps keep alpha-tested coverage (encodeKtx2)
        std::string variant;   // a texture file's variant (AssetFolder::TextureVariant::key); empty: the file's own
        bool bc7 = false;   // what to transcode to afterwards
        std::filesystem::path cacheFile;   // empty: no cache

        TextureData texture;   // the result
        size_t compressedBytes = 0;   // the .ktx2's size
        std::string error;
        std::atomic<bool> started{false}, done{false};
    };

    TextureCompressor();
    // Drops waiting tasks. One in progress finishes on its own, so quitting isn't held up.
    ~TextureCompressor();
    TextureCompressor(const TextureCompressor&) = delete;
    TextureCompressor& operator=(const TextureCompressor&) = delete;

    // Replaces any waiting task for the same texture of the same asset. Main thread.
    void add(std::shared_ptr<Task> task);
    std::vector<std::shared_ptr<Task>> takeFinished();
    size_t pending() const;
    // Runs every waiting task across the job system now, and waits for all of them.
    void finish(JobSystem& jobs);
    // Drops waiting tasks and waits for the one in progress.
    void clear();

private:
    // Shared with the thread, which may outlive the compressor by one task.
    struct State {
        std::mutex mutex;
        std::condition_variable wake, progress;
        std::deque<std::shared_ptr<Task>> tasks;   // waiting, running and finished, in order
        bool stop = false, busy = false;
    };
    static void run(Task& task);
    static void loop(State& state);
    void waitUntilIdle(std::unique_lock<std::mutex>& lock);

    std::shared_ptr<State> m_state;
    std::thread m_thread;
};

}   // namespace shockwave
