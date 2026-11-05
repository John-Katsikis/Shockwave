#pragma once

// Asset residency: who holds which assets, and what a level change would cost (S04,
// docs/AssetResidency.md).
//
// A lease is a set of assets one owner holds: a level, or the application's own UI. Each
// asset is counted once however many leases hold it, so a level leaving can't take a font
// or a mesh the next level (or the menu) still uses. When the last lease on an asset goes,
// the asset retires.
//
//   AssetResidency residency;
//   auto menu = residency.acquire("level menu (epoch 1)", menuAssets);
//   const auto plan = residency.plan(aAssets, &menu);   // before anything is torn down
//   if (!residency.fits(plan, error)) ...                // BudgetExceeded, with totals
//   menu.release();
//   auto a = residency.acquire("level a (epoch 2)", aAssets);
//
// With a Loader (setLoader), it also decides what's in memory: load() imports a lease's
// assets that aren't yet, and an asset whose last lease goes is unloaded at once, its GPU
// memory freed once no frame in flight reads it (Renderer::pendingFrees). Without one, it
// only accounts, and the assets are whatever AssetFolder imported.
//
// Main thread only; the residency must outlive its leases.

#include <Assets/AssetGraph.h>

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace shockwave {

// One asset a lease needs and what it costs. CPU bytes are its file's size, standing in
// for its prepared data; GPU bytes are what its textures and meshes take there.
struct AssetCost {
    std::string id;   // AssetGraph id
    AssetKind kind = AssetKind::Other;
    uint64_t cpuBytes = 0;
    uint64_t gpuBytes = 0;
};

// The most a level change may leave resident. 0: no limit (the default).
struct AssetBudget {
    uint64_t cpuBytes = 0;
    uint64_t gpuBytes = 0;
    bool limited() const { return cpuBytes || gpuBytes; }
};

class AssetResidency {
public:
    class Lease {
    public:
        Lease() = default;
        ~Lease() { release(); }
        Lease(Lease&& other) noexcept;
        Lease& operator=(Lease&& other) noexcept;
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;

        bool valid() const { return m_residency != nullptr; }
        uint64_t id() const { return m_id; }
        const std::string& owner() const;
        size_t size() const;   // distinct assets
        // Gives the assets back; any only this lease held retire. Safe to repeat.
        void release();

    private:
        friend class AssetResidency;
        AssetResidency* m_residency = nullptr;
        uint64_t m_id = 0;
    };

    struct Entry {
        std::string id;
        AssetKind kind = AssetKind::Other;
        uint64_t cpuBytes = 0;
        uint64_t gpuBytes = 0;
        uint32_t refs = 0;   // leases holding it
        std::vector<std::string> owners;   // their owners, in the order they took it
        bool loaded = false;   // in memory: always, without a loader; after load(), with one
        std::string lastError;
    };

    // What imports and frees assets, for load() and retirement. AssetGraph ids.
    class Loader {
    public:
        virtual ~Loader() = default;
        // Imports ids. Fills loaded with those now in; false, with error, when any couldn't
        // be or cancelled() said stop (then nothing this call loaded stays loaded).
        virtual bool load(
            const std::vector<std::string>& ids, const std::function<bool()>& cancelled,
            std::vector<std::string>& loaded, std::string& error) = 0;
        // Unregisters ids and destroys their resources; GPU memory follows once no frame reads it.
        virtual void unload(const std::vector<std::string>& ids) = 0;
        // GPU objects destroyed but not freed yet, waiting for frames in flight.
        virtual size_t pendingFrees() const { return 0; }
    };

    // What travelling would do: the next level's assets against what's resident, with the
    // leaving lease given back. "Shared" assets stay (another lease, or the next level,
    // still holds them), "added" ones are new, "retired" ones go.
    struct Plan {
        size_t shared = 0, added = 0, retired = 0;
        uint64_t sharedCpu = 0, sharedGpu = 0;
        uint64_t addedCpu = 0, addedGpu = 0;
        uint64_t retiredCpu = 0, retiredGpu = 0;
        uint64_t afterCpu = 0, afterGpu = 0;   // resident once the next level is in
        // The most resident at once: the old level's set while it retires, or the new one's.
        uint64_t peakCpu = 0, peakGpu = 0;
    };

    AssetResidency() = default;
    ~AssetResidency();
    AssetResidency(const AssetResidency&) = delete;
    AssetResidency& operator=(const AssetResidency&) = delete;

    // An asset listed twice in one lease counts once; one another lease holds gains a ref.
    Lease acquire(std::string owner, const std::vector<AssetCost>& assets);

    std::vector<Entry> entries() const;   // sorted by id
    const Entry* find(std::string_view id) const;
    size_t size() const { return m_entries.size(); }
    uint64_t cpuBytes() const { return m_cpu; }
    uint64_t gpuBytes() const { return m_gpu; }
    size_t leases() const { return m_leases.size(); }
    // Assets that retired so far, in total, and the most recent ones (at most 64).
    uint64_t retiredCount() const { return m_retiredCount; }
    const std::vector<std::string>& recentlyRetired() const { return m_recent; }

    void setBudget(AssetBudget budget) { m_budget = budget; }
    const AssetBudget& budget() const { return m_budget; }

    Plan plan(const std::vector<AssetCost>& next, const Lease* leaving) const;
    // True when the plan stays inside the budget. Otherwise error says "BudgetExceeded: "
    // with the totals, so a level that can't fit is refused before the old one ends.
    bool fits(const Plan& plan, std::string& error) const;

    void setError(std::string_view id, std::string error);

    // ---- Loading (on demand) ----
    // Set before any lease is taken, and outlive the residency's leases. Null: accounting only.
    void setLoader(Loader* loader) { m_loader = loader; }
    Loader* loader() const { return m_loader; }
    // Imports what the lease holds that isn't in memory yet. False, with error, when any of
    // it can't be, or when cancelled() says stop first; release the lease then, and what it
    // alone holds is unloaded again. True at once without a loader.
    bool load(const Lease& lease, const std::function<bool()>& cancelled, std::string& error);
    bool load(const Lease& lease, std::string& error) { return load(lease, {}, error); }
    size_t pendingFrees() const { return m_loader ? m_loader->pendingFrees() : 0; }
    // In total so far: assets loaded by load(), and unloaded when they retired.
    uint64_t loadedCount() const { return m_loadedCount; }
    uint64_t unloadedCount() const { return m_unloadedCount; }

private:
    struct Held {
        std::string owner;
        std::vector<std::string> ids;
    };
    void release(uint64_t lease);

    std::map<std::string, Entry, std::less<>> m_entries;
    std::map<uint64_t, Held> m_leases;
    uint64_t m_nextLease = 1;
    uint64_t m_cpu = 0, m_gpu = 0;
    uint64_t m_retiredCount = 0;
    std::vector<std::string> m_recent;
    AssetBudget m_budget;
    Loader* m_loader = nullptr;
    uint64_t m_loadedCount = 0, m_unloadedCount = 0;
};

// "12.5 MiB", for messages.
std::string formatAssetBytes(uint64_t bytes);

}   // namespace shockwave
