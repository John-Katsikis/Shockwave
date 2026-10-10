#include <Engine/World.h>

#include <atomic>

namespace shockwave {

namespace detail {
uint32_t nextComponentId() {
    static std::atomic<uint32_t> next{0};
    return next.fetch_add(1, std::memory_order_relaxed);
}
}   // namespace detail

namespace {
// Freed indices wait in line until this many are free, so a destroyed entity's slot
// isn't handed straight to the next new one: entity order stays creation order, and
// stale handles fail the generation check for longer.
constexpr size_t kMinFreeBeforeReuse = 1024;
}   // namespace

World& World::operator=(const World& other) {
    if (this == &other) return *this;
    m_generations = other.m_generations;
    m_alive = other.m_alive;
    m_free = other.m_free;
    m_count = other.m_count;
    m_pools.clear();
    m_pools.resize(other.m_pools.size());
    for (size_t i = 0; i < other.m_pools.size(); ++i) {
        if (other.m_pools[i]) m_pools[i] = other.m_pools[i]->clone();
    }
    return *this;
}

Entity World::create() {
    uint32_t index;
    if (m_free.size() > kMinFreeBeforeReuse) {
        index = m_free.front();
        m_free.pop_front();
    } else {
        index = static_cast<uint32_t>(m_generations.size());
        m_generations.push_back(0);
        m_alive.push_back(0);
    }
    m_alive[index] = 1;
    ++m_count;
    return Entity{index, m_generations[index]};
}

World World::withoutComponents() const {
    World result;
    result.m_generations = m_generations;
    result.m_alive = m_alive;
    result.m_free = m_free;
    result.m_count = m_count;
    return result;
}

void World::destroy(Entity e) {
    if (!alive(e)) return;
    for (auto& pool : m_pools) {
        if (pool) pool->remove(e.index);
    }
    m_alive[e.index] = 0;
    ++m_generations[e.index];
    m_free.push_back(e.index);
    --m_count;
}

Entity World::clone(Entity e) {
    if (!alive(e)) return kNullEntity;
    const Entity copy = create();
    for (auto& pool : m_pools) {
        if (pool) pool->copy(e.index, copy);
    }
    return copy;
}

void World::clear() {
    m_generations.clear();
    m_alive.clear();
    m_free.clear();
    m_count = 0;
    m_pools.clear();
}

Entity World::entityAt(size_t n) const {
    for (uint32_t i = 0; i < m_generations.size(); ++i) {
        if (m_alive[i] && n-- == 0) return Entity{i, m_generations[i]};
    }
    return kNullEntity;
}

}   // namespace shockwave
