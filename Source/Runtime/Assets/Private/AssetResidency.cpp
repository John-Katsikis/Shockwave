// Asset residency: see Assets/AssetResidency.h and docs/AssetResidency.md.
#include <Assets/AssetResidency.h>

#include <algorithm>
#include <cstdio>
#include <set>

namespace shockwave {

std::string formatAssetBytes(uint64_t bytes) {
    char text[32];
    if (bytes < 1024)
        std::snprintf(text, sizeof(text), "%llu B", static_cast<unsigned long long>(bytes));
    else if (bytes < 1024ull * 1024)
        std::snprintf(text, sizeof(text), "%.1f KiB", double(bytes) / 1024.0);
    else if (bytes < 1024ull * 1024 * 1024)
        std::snprintf(text, sizeof(text), "%.1f MiB", double(bytes) / (1024.0 * 1024.0));
    else
        std::snprintf(text, sizeof(text), "%.2f GiB", double(bytes) / (1024.0 * 1024.0 * 1024.0));
    return text;
}

// ---- Lease ------------------------------------------------------------------------------

AssetResidency::Lease::Lease(Lease&& other) noexcept : m_residency(other.m_residency), m_id(other.m_id) {
    other.m_residency = nullptr;
    other.m_id = 0;
}

AssetResidency::Lease& AssetResidency::Lease::operator=(Lease&& other) noexcept {
    if (this != &other) {
        release();
        m_residency = other.m_residency;
        m_id = other.m_id;
        other.m_residency = nullptr;
        other.m_id = 0;
    }
    return *this;
}

const std::string& AssetResidency::Lease::owner() const {
    static const std::string none;
    if (!m_residency) return none;
    const auto it = m_residency->m_leases.find(m_id);
    return it == m_residency->m_leases.end() ? none : it->second.owner;
}

size_t AssetResidency::Lease::size() const {
    if (!m_residency) return 0;
    const auto it = m_residency->m_leases.find(m_id);
    return it == m_residency->m_leases.end() ? 0 : it->second.ids.size();
}

void AssetResidency::Lease::release() {
    if (!m_residency) return;
    m_residency->release(m_id);
    m_residency = nullptr;
    m_id = 0;
}

// ---- AssetResidency ---------------------------------------------------------------------

AssetResidency::~AssetResidency() = default;

AssetResidency::Lease AssetResidency::acquire(std::string owner, const std::vector<AssetCost>& assets) {
    Held held;
    held.owner = std::move(owner);
    std::set<std::string_view> seen;
    for (const AssetCost& cost : assets) {
        if (!seen.insert(cost.id).second) continue;   // listed twice: once
        auto [it, inserted] = m_entries.try_emplace(cost.id);
        Entry& entry = it->second;
        if (inserted) {
            entry.id = cost.id;
            entry.kind = cost.kind;
            entry.cpuBytes = cost.cpuBytes;
            entry.gpuBytes = cost.gpuBytes;
            m_cpu += cost.cpuBytes;
            m_gpu += cost.gpuBytes;
        }
        ++entry.refs;
        entry.owners.push_back(held.owner);
        held.ids.push_back(cost.id);
    }
    const uint64_t id = m_nextLease++;
    m_leases.emplace(id, std::move(held));
    Lease lease;
    lease.m_residency = this;
    lease.m_id = id;
    return lease;
}

void AssetResidency::release(uint64_t lease) {
    const auto it = m_leases.find(lease);
    if (it == m_leases.end()) return;
    for (const std::string& id : it->second.ids) {
        const auto entry = m_entries.find(id);
        if (entry == m_entries.end()) continue;
        Entry& e = entry->second;
        if (const auto owner = std::find(e.owners.begin(), e.owners.end(), it->second.owner); owner != e.owners.end())
            e.owners.erase(owner);
        if (--e.refs > 0) continue;
        m_cpu -= e.cpuBytes;
        m_gpu -= e.gpuBytes;
        ++m_retiredCount;
        m_recent.push_back(e.id);
        if (m_recent.size() > 64) m_recent.erase(m_recent.begin());
        m_entries.erase(entry);
    }
    m_leases.erase(it);
}

std::vector<AssetResidency::Entry> AssetResidency::entries() const {
    std::vector<Entry> out;
    out.reserve(m_entries.size());
    for (const auto& [id, entry] : m_entries) out.push_back(entry);
    return out;
}

const AssetResidency::Entry* AssetResidency::find(std::string_view id) const {
    const auto it = m_entries.find(id);
    return it == m_entries.end() ? nullptr : &it->second;
}

void AssetResidency::setError(std::string_view id, std::string error) {
    if (const auto it = m_entries.find(id); it != m_entries.end()) it->second.lastError = std::move(error);
}

AssetResidency::Plan AssetResidency::plan(const std::vector<AssetCost>& next, const Lease* leaving) const {
    Plan result;
    // How many refs each asset keeps once the leaving lease is given back.
    std::map<std::string_view, uint32_t> leavingRefs;
    if (leaving && leaving->m_residency == this)
        if (const auto it = m_leases.find(leaving->m_id); it != m_leases.end())
            for (const std::string& id : it->second.ids) ++leavingRefs[id];
    std::set<std::string_view> wanted;
    for (const AssetCost& cost : next) {
        if (!wanted.insert(cost.id).second) continue;
        if (const Entry* entry = find(cost.id)) {
            ++result.shared;
            result.sharedCpu += entry->cpuBytes;
            result.sharedGpu += entry->gpuBytes;
        } else {
            ++result.added;
            result.addedCpu += cost.cpuBytes;
            result.addedGpu += cost.gpuBytes;
        }
    }
    for (const auto& [id, refs] : leavingRefs) {
        const Entry* entry = find(id);
        if (!entry || entry->refs > refs || wanted.count(id)) continue;
        ++result.retired;
        result.retiredCpu += entry->cpuBytes;
        result.retiredGpu += entry->gpuBytes;
    }
    // Unload before upload (docs/ShippingImplementation.md section 6.2): the old level's
    // set while it retires, then the application's and the new level's.
    result.afterCpu = m_cpu - result.retiredCpu + result.addedCpu;
    result.afterGpu = m_gpu - result.retiredGpu + result.addedGpu;
    result.peakCpu = std::max(m_cpu, result.afterCpu);
    result.peakGpu = std::max(m_gpu, result.afterGpu);
    return result;
}

bool AssetResidency::fits(const Plan& p, std::string& error) const {
    std::string over;
    auto check = [&](const char* what, uint64_t after, uint64_t limit, uint64_t added, uint64_t retired) {
        if (!limit || after <= limit) return;
        if (!over.empty()) over += "; ";
        over += std::string(what) + " " + formatAssetBytes(after) + " over the budget of " + formatAssetBytes(limit) +
                " (" + formatAssetBytes(added) + " new, " + formatAssetBytes(retired) + " retiring)";
    };
    check("CPU", p.afterCpu, m_budget.cpuBytes, p.addedCpu, p.retiredCpu);
    check("GPU", p.afterGpu, m_budget.gpuBytes, p.addedGpu, p.retiredGpu);
    if (over.empty()) return true;
    error = "BudgetExceeded: " + over + "; " + std::to_string(p.shared) + " shared, " + std::to_string(p.added) +
            " new and " + std::to_string(p.retired) + " retiring assets";
    return false;
}

}   // namespace shockwave
