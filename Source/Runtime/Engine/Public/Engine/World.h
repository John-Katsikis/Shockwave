#pragma once

#include <Core/Jobs.h>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <span>
#include <tuple>
#include <type_traits>
#include <vector>

namespace shockwave {

// A thing in the world. Just an id: what it is comes from the components attached to it.
// The generation changes every time an index is reused, so a handle to a destroyed
// entity never accidentally points at a new one.
struct Entity {
    uint32_t index = UINT32_MAX;
    uint32_t generation = 0;

    bool valid() const { return index != UINT32_MAX; }   // set at all; see World::alive
    bool operator==(const Entity&) const = default;
};

inline constexpr Entity kNullEntity{};

namespace detail {
uint32_t nextComponentId();
}

// A small dense id per component type, assigned on first use. Not stable between runs:
// scene files refer to components by their registered name instead.
template <class T>
uint32_t componentId() {
    static const uint32_t id = detail::nextComponentId();
    return id;
}

// Storage for one component type: a sparse set. Components sit packed in one array in
// no particular order, which is what makes iterating them fast and easy to split across
// threads; the sparse array maps an entity index to its slot.
class PoolBase {
public:
    virtual ~PoolBase() = default;
    virtual std::unique_ptr<PoolBase> clone() const = 0;
    virtual void remove(uint32_t index) = 0;
    virtual void copy(uint32_t fromIndex, Entity to) = 0;

    bool has(uint32_t index) const { return index < m_sparse.size() && m_sparse[index] != kNone; }
    size_t size() const { return m_entities.size(); }
    std::span<const Entity> entities() const { return m_entities; }

protected:
    static constexpr uint32_t kNone = UINT32_MAX;
    std::vector<uint32_t> m_sparse;   // entity index -> slot, or kNone
    std::vector<Entity> m_entities;   // slot -> entity
};

template <class T>
class Pool final : public PoolBase {
    static_assert(
        std::is_copy_constructible_v<T>, "components must be copyable (undo, play mode and duplicate copy them)");

public:
    T& set(Entity e, T value) {
        if (has(e.index)) {
            T& slot = m_data[m_sparse[e.index]];
            slot = std::move(value);
            return slot;
        }
        if (e.index >= m_sparse.size()) m_sparse.resize(size_t(e.index) + 1, kNone);
        m_sparse[e.index] = static_cast<uint32_t>(m_data.size());
        m_entities.push_back(e);
        return m_data.emplace_back(std::move(value));
    }

    T* find(uint32_t index) { return has(index) ? &m_data[m_sparse[index]] : nullptr; }
    const T* find(uint32_t index) const { return has(index) ? &m_data[m_sparse[index]] : nullptr; }

    std::span<T> components() { return m_data; }
    std::span<const T> components() const { return m_data; }

    void remove(uint32_t index) override {
        if (!has(index)) return;
        // Swap with the last slot and pop, so the array stays packed.
        const uint32_t slot = m_sparse[index];
        const uint32_t last = static_cast<uint32_t>(m_data.size() - 1);
        if (slot != last) {
            m_data[slot] = std::move(m_data[last]);
            m_entities[slot] = m_entities[last];
            m_sparse[m_entities[slot].index] = slot;
        }
        m_data.pop_back();
        m_entities.pop_back();
        m_sparse[index] = kNone;
    }

    void copy(uint32_t fromIndex, Entity to) override {
        if (const T* from = find(fromIndex)) {
            T value = *from;   // copy first: set() may grow the array under us
            set(to, std::move(value));
        }
    }

    std::unique_ptr<PoolBase> clone() const override { return std::make_unique<Pool<T>>(*this); }

private:
    std::vector<T> m_data;
};

// Holds every entity and component in a scene. Components are plain structs; any
// copyable type can be one, and no registration is needed to store it (registering adds
// it to scene files and the editor, see Components.h).
//
// Threading:
//   - Structural changes (create, destroy, add, remove, clear, assignment) belong to one
//     thread at a time, and nothing may iterate the world while they happen. During a
//     Schedule run, systems queue them in Commands instead.
//   - Reading and writing components is safe from many threads at once as long as no
//     two threads touch the same component of the same entity. each() / parallelEach()
//     never hand the same entity to two threads, and Schedule keeps systems that write
//     the same component type out of each other's way.
//
// Worlds copy deeply and cheaply enough for undo history and play mode snapshots.
class World {
public:
    World() = default;
    World(const World& other) { *this = other; }
    World& operator=(const World& other);
    World(World&&) noexcept = default;
    World& operator=(World&&) noexcept = default;

    Entity create();
    void destroy(Entity e);
    bool alive(Entity e) const {
        return e.index < m_generations.size() && m_alive[e.index] && m_generations[e.index] == e.generation;
    }
    // A new entity with copies of all of e's components.
    Entity clone(Entity e);
    void clear();
    size_t entityCount() const { return m_count; }

    // Visits live entities in index order, which is creation order: indices are only
    // recycled once many are free, so lists of entities stay put while you edit.
    template <class Fn>
    void forEachEntity(Fn&& fn) const {
        for (uint32_t i = 0; i < m_generations.size(); ++i) {
            if (m_alive[i]) fn(Entity{i, m_generations[i]});
        }
    }
    // The nth live entity in index order, or kNullEntity.
    Entity entityAt(size_t n) const;

    template <class T>
    T& add(Entity e, T value = {}) {
        assert(alive(e));
        return assurePool<T>().set(e, std::move(value));
    }
    template <class T>
    void remove(Entity e) {
        if (Pool<T>* p = pool<T>()) p->remove(e.index);
    }
    template <class T>
    bool has(Entity e) const {
        const Pool<T>* p = pool<T>();
        return p && alive(e) && p->has(e.index);
    }
    template <class T>
    T* get(Entity e) {
        Pool<T>* p = pool<T>();
        return p && alive(e) ? p->find(e.index) : nullptr;
    }
    template <class T>
    const T* get(Entity e) const {
        const Pool<T>* p = pool<T>();
        return p && alive(e) ? p->find(e.index) : nullptr;
    }
    template <class T>
    size_t count() const {
        const Pool<T>* p = pool<T>();
        return p ? p->size() : 0;
    }

    // Calls fn for every entity that has all of Ts, as fn(Ts&...) or fn(Entity, Ts&...).
    // Mark the components you only read as const (each<Transform, const Spin>): that's
    // what Schedule uses to work out which systems can run side by side.
    template <class... Ts, class Fn>
    void each(Fn&& fn) {
        forMatches<Ts...>([&](std::span<const Entity> entities, auto&... pools) {
            for (Entity e : entities) visit<Ts...>(fn, e, pools...);
        });
    }

    // Like each(), but split across the job system in batches of batchSize entities.
    // fn runs on many threads at once, so it may only touch the entity it was given
    // (and read things nobody is writing).
    template <class... Ts, class Fn>
    void parallelEach(JobSystem& jobs, Fn&& fn, size_t batchSize = 256) {
        forMatches<Ts...>([&](std::span<const Entity> entities, auto&... pools) {
            jobs.parallelFor(entities.size(), batchSize, [&](size_t begin, size_t end) {
                for (size_t i = begin; i < end; ++i) visit<Ts...>(fn, entities[i], pools...);
            });
        });
    }

    // Direct pool access, nullptr when no entity has ever had a T.
    template <class T>
    Pool<T>* pool() {
        const uint32_t id = componentId<T>();
        return id < m_pools.size() ? static_cast<Pool<T>*>(m_pools[id].get()) : nullptr;
    }
    template <class T>
    const Pool<T>* pool() const {
        const uint32_t id = componentId<T>();
        return id < m_pools.size() ? static_cast<const Pool<T>*>(m_pools[id].get()) : nullptr;
    }

private:
    template <class T>
    Pool<T>& assurePool() {
        const uint32_t id = componentId<T>();
        if (id >= m_pools.size()) m_pools.resize(size_t(id) + 1);
        if (!m_pools[id]) m_pools[id] = std::make_unique<Pool<T>>();
        return *static_cast<Pool<T>*>(m_pools[id].get());
    }

    // Finds the pools for Ts and hands fn the entities of the smallest one, which bounds
    // how many candidates need checking.
    template <class... Ts, class Fn>
    void forMatches(Fn&& fn) {
        static_assert(sizeof...(Ts) > 0, "each() needs at least one component type");
        auto pools = std::make_tuple(pool<std::remove_const_t<Ts>>()...);
        std::apply(
            [&](auto*... p) {
                if (((p == nullptr) || ...)) return;
                const PoolBase* candidates[] = {p...};
                const PoolBase* smallest = *std::min_element(
                    std::begin(candidates), std::end(candidates),
                    [](const PoolBase* a, const PoolBase* b) { return a->size() < b->size(); });
                fn(smallest->entities(), *p...);
            },
            pools);
    }

    template <class... Ts, class Fn, class... Pools>
    static void visit(Fn& fn, Entity e, Pools&... pools) {
        if (!(pools.has(e.index) && ...)) return;
        if constexpr (std::is_invocable_v<Fn&, Entity, Ts&...>) {
            fn(e, *pools.find(e.index)...);
        } else {
            static_assert(std::is_invocable_v<Fn&, Ts&...>, "each() callback must take (Ts&...) or (Entity, Ts&...)");
            fn(*pools.find(e.index)...);
        }
    }

    std::vector<uint32_t> m_generations;
    std::vector<uint8_t> m_alive;
    std::deque<uint32_t> m_free;
    size_t m_count = 0;
    std::vector<std::unique_ptr<PoolBase>> m_pools;   // indexed by componentId
};

// Structural changes recorded while a Schedule runs and applied once it finishes.
// Each thread records into its own buffer (SystemContext::commands), so no locks.
class Commands {
public:
    // Creates an entity later and calls init(world, entity) to set it up.
    void spawn(std::function<void(World&, Entity)> init) {
        m_commands.push_back([f = std::move(init)](World& w) { f(w, w.create()); });
    }
    void destroy(Entity e) {
        m_commands.push_back([e](World& w) { w.destroy(e); });
    }
    template <class T>
    void add(Entity e, T value) {
        m_commands.push_back([e, v = std::move(value)](World& w) {
            if (w.alive(e)) w.add<T>(e, v);
        });
    }
    template <class T>
    void remove(Entity e) {
        m_commands.push_back([e](World& w) { w.remove<T>(e); });
    }

    void apply(World& world) {
        for (auto& command : m_commands) command(world);
        m_commands.clear();
    }
    bool empty() const { return m_commands.empty(); }

private:
    std::vector<std::function<void(World&)>> m_commands;
};

}   // namespace shockwave
