#pragma once

// CircuitWorld: runs circuits during Play. One per Play session, a subsystem PlaySession
// adds after the GameMode's beginPlay. Every event runs on the main thread, after the
// matching GameMode hook: the game circuit first, then entity circuits in entity order.
//
//   BeginPlay    Play starts (or the entity appears, for one spawned later)
//   EndPlay      Play stops
//   FixedTick    every simulation tick, after GameMode::fixedUpdate (outputs dt)
//   Frame        every rendered frame, after GameMode::frame (outputs dt)
//   ActionPressed / ActionReleased   an input action, checked once a frame
//   TriggerEnter / TriggerExit       entity circuits: a sensor collider touched (outputs other)
//   BeforeSave / AfterLoad           the game circuit: around save games
//   UICommand    the game circuit: a UI element's onclick="command" (outputs value, element, document)
//
// Entity circuits keep their variables in the entity's CircuitVars, so they show in the
// Inspector and go into save games. The game circuit's go into the save game's values as
// "circuit.<name>", written in beforeSave and read back in afterLoad.
//
// Saving a .swcircuit while playing compiles it again; running circuits move to the new
// version and keep variables whose name and type are unchanged.

#include <Circuits/CircuitProgram.h>
#include <Engine/Subsystem.h>
#include <Engine/World.h>

#include <functional>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace shockwave {

class CircuitWorld final : public Subsystem {
public:
    // gameCircuit: inside Content, or empty for none.
    CircuitWorld(World& world, CircuitHost host, std::string gameCircuit = {});
    ~CircuitWorld() override;
    CircuitWorld(const CircuitWorld&) = delete;
    CircuitWorld& operator=(const CircuitWorld&) = delete;

    // Main thread, from PlaySession.
    void beginPlay();
    void endPlay();
    void fixedTick(double dt);   // runs trigger events from the physics step too
    void frame(double dt);   // input actions, then Frame; picks up edited circuits
    // Game circuit values for save games: BeforeSave runs, then they're written with set.
    void beforeSave(const std::function<void(const std::string& key, const std::string& value)>& set);
    // After a save game replaced the world: variables come back from the entities'
    // CircuitVars and, for the game circuit, from get (nullopt-like: return false when
    // the save doesn't have the key). Then AfterLoad runs.
    void afterLoad(const std::function<bool(const std::string& key, std::string& value)>& get);

    // Problems loading or running circuits this session, "file:line: message", once each.
    const std::vector<std::string>& diagnostics() const { return m_diagnostics; }
    size_t instanceCount() const { return m_instances.size() + (m_game.program ? 1 : 0); }
    // A variable's value now: of the entity's circuit, or with kNullEntity, the game
    // circuit's. Null if there's no such circuit or variable. For tools and tests.
    const Value* variable(Entity entity, std::string_view name) const;
    // How many instructions one event may run before it's stopped (an endless loop).
    static constexpr uint64_t kInstructionBudget = 1'000'000;

private:
    struct Instance {
        std::shared_ptr<const CircuitProgram> program;
        std::string path;
        std::vector<Value> regs;
        uint64_t generation = 0;
        bool begun = false, dirty = false;
    };
    static uint64_t key(Entity e) { return (uint64_t(e.generation) << 32) | e.index; }

    // Starts, moves and drops instances to match the world's Circuit components and the
    // circuits' latest versions; then runs BeginPlay for new ones once Play has begun.
    void refresh();
    void beginNew();
    Instance* instanceOf(Entity e);
    std::string entityName(Entity e) const;
    void storeAll();
    void start(Instance& instance, std::shared_ptr<const CircuitProgram> program);
    void migrate(Instance& instance, std::shared_ptr<const CircuitProgram> program);
    void applyVars(Instance& instance, Entity entity);
    void storeVars(Instance& instance, Entity entity);
    // Runs every entry for this event in one instance; data goes into the event node's
    // outputs first (dt, other, value...).
    void dispatch(
        Instance& instance, Entity self, CircuitEvent event, std::string_view param = {},
        const std::function<void(const CircuitProgram::CallSite&, std::vector<Value>&)>& data = {});
    void dispatchAll(
        CircuitEvent event, std::string_view param = {},
        const std::function<void(const CircuitProgram::CallSite&, std::vector<Value>&)>& data = {});
    void run(Instance& instance, Entity self, uint32_t pc);
    void report(const std::string& problem);

    World& m_world;
    CircuitHost m_host;
    std::string m_gamePath;
    Instance m_game;
    std::unordered_map<uint64_t, Instance> m_instances;
    std::vector<Entity> m_order;   // entities with an instance, in entity order
    std::vector<std::string> m_diagnostics;
    std::set<std::string> m_reported;
    std::set<std::string> m_uiCommands;   // with a UIWorld handler already
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);   // UI handlers check it
    bool m_playing = false;
};

}   // namespace shockwave
