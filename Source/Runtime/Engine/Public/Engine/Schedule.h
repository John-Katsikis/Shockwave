#pragma once

#include <Engine/World.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace shockwave {

// What a system gets each tick.
struct SystemContext {
    World& world;
    JobSystem& jobs;
    float dt;   // seconds per tick

    // The calling thread's command buffer: queue entity creation, destruction and
    // component add/remove here; they're applied when the whole schedule has run.
    Commands& commands() const { return (*m_commands)[JobSystem::threadIndex()]; }

    std::vector<Commands>* m_commands = nullptr;
};

// Runs a game's systems every tick, as many at once as is safe.
//
// Every system says which component types it reads and which it writes. Systems that
// don't conflict (nobody writes what another reads or writes) run side by side on the
// job system, and each() systems also split their entities across cores. Systems
// that do conflict run in the order they were added, so behaviour never depends on
// thread timing.
//
//   schedule.each<Transform, const Spin>("Spin", [](const SystemContext& ctx, Transform& t, const Spin& s) {
//       t.rotation = ...;
//   });
//   schedule.run(world, jobs, dt);
class Schedule {
public:
    struct Access {
        std::vector<uint32_t> reads, writes;
        bool exclusive = false;   // runs alone; may change the world's structure directly

        template <class T>
        Access& read() {
            reads.push_back(componentId<T>());
            return *this;
        }
        template <class T>
        Access& write() {
            writes.push_back(componentId<T>());
            return *this;
        }
    };
    using SystemFn = std::function<void(const SystemContext&)>;

    // A general system: fn runs once per tick and does whatever it declared in access.
    void add(std::string name, Access access, SystemFn fn);

    // A per-entity system over every entity with all of Ts. const Ts are read, the rest
    // are written. fn is (const SystemContext&, Ts&...) or (const SystemContext&, Entity, Ts&...)
    // and runs on many threads at once.
    template <class... Ts, class Fn>
    void each(std::string name, Fn fn, size_t batchSize = 256) {
        Access access;
        (declare<Ts>(access), ...);
        add(std::move(name), std::move(access), [fn = std::move(fn), batchSize](const SystemContext& ctx) {
            if constexpr (std::is_invocable_v<const Fn&, const SystemContext&, Entity, Ts&...>) {
                ctx.world.parallelEach<Ts...>(ctx.jobs, [&](Entity e, Ts&... c) { fn(ctx, e, c...); }, batchSize);
            } else {
                ctx.world.parallelEach<Ts...>(ctx.jobs, [&](Ts&... c) { fn(ctx, c...); }, batchSize);
            }
        });
    }

    // Runs every system once, then applies the queued commands.
    void run(World& world, JobSystem& jobs, double dt);

    // Systems grouped into the stages they run in; systems in one stage run in parallel.
    std::vector<std::vector<std::string>> stages() const;
    size_t size() const { return m_systems.size(); }

private:
    struct System {
        std::string name;
        Access access;
        SystemFn fn;
    };

    template <class T>
    static void declare(Access& access) {
        if constexpr (std::is_const_v<T>) {
            access.read<std::remove_const_t<T>>();
        } else {
            access.write<T>();
        }
    }
    void build() const;

    std::vector<System> m_systems;
    mutable std::vector<std::vector<size_t>> m_stages;
    mutable bool m_built = false;
    std::vector<Commands> m_commands;   // one per job system thread
};

}   // namespace shockwave
