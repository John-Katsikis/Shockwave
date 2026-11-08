#pragma once

// Flow tests (S05, docs/FlowTest.md): a scripted run through the game's screens and levels,
// for CI and agents. player --flow-test scenario.json --json report.json runs one.
//
//   {"format": 1, "name": "menu to b", "timeout": 60, "steps": [
//     {"click": "new_game"},                       a shell button, by id, as the mouse would
//     {"wait_phase": "succeeded"},                 the latest travel ticket reaches a phase
//     {"assert_level": "a"},
//     {"travel": "b", "spawn": "entrance"},        a request, as the application makes one
//     {"travel": "nowhere", "expect": "UnknownLevel"},   ... or its refusal
//     {"assert_value": "flow.screen", "equals": "pause"},   the shell's "flow" or the level's "game" model
//     {"click": "pulse", "document": "PauseMenu"}, a button in one of the level's documents
//     {"press": "pause"},                          the pause action, as its key does (the only one v1 knows)
//     {"save": "slot"}, {"load": "slot"},
//     {"hold": "key W", "until": "game.zone", "equals": "range"},   a key, button or stick held
//     {"hold": "pad_axis left_y +", "value": 0.8, "seconds": 1.5},   ... for game seconds
//     {"tap": "key E"},                            pressed for one frame, then let go
//     {"look": [120, -20], "frames": 4},           mouse movement, in pixels, over frames
//     {"wait": 0.5},                               game seconds
//     {"wait_value": "game.objective", "equals": "done"},   waits until a value is that
//     {"quit": true}]}
//
// hold, tap and look are real input (S07): they go in through Input::inject at the device
// boundary, with the text of a binding (docs/InputSchema.md), so the game reads them
// through its bindings exactly as it reads a keyboard, mouse or gamepad, rebinding and
// all. A "pad" binding plugs in a virtual gamepad. A hold lets go when its step ends,
// whether it passed or not.
//
// Clicks and presses go through the same handlers the buttons and keys do. There are no
// shell commands and no scripts: only these steps, each with a timeout (seconds, default
// 10) inside the scenario's total (default 60). A click waits for its element to show; an
// assertion doesn't wait. The run stops at the first step that fails, and the report says
// which, why, and the state at that moment.
//
// Without a GPU (--renderer null) it tests the flow's logic; it can't say anything was
// drawn right. Main thread only.

#include <GameFramework/GameFlow.h>
#include <Input/InputBinding.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace shockwave {

class AppShell;

struct FlowStep {
    enum class Kind {
        Travel,
        Click,
        Press,
        WaitPhase,
        AssertLevel,
        AssertValue,
        Save,
        Load,
        Quit,
        Hold,
        Tap,
        Look,
        Wait,
        WaitValue,
        Unplug
    };
    Kind kind = Kind::Quit;
    std::string target;   // the level, element, phase, value name or slot
    std::string spawn;   // travel
    std::string document;   // click: a level document's name; empty: the shell's screen
    std::string expect;   // travel: the refusal code it should get ("UnknownLevel"); empty: accepted
    std::string equals;   // assert_value, wait_value, hold until
    std::string until;   // hold: the value it holds until (with equals)
    InputBinding binding;   // hold, tap
    float value = 1;   // hold: how far (0..1)
    double seconds = 0;   // hold: game seconds (0: until); wait
    float lookX = 0, lookY = 0;   // look: pixels
    int frames = 1;   // look: spread over this many frames
    double timeout = 10;   // seconds
};
const char* flowStepKindName(FlowStep::Kind kind);   // "travel", "click", "wait_phase", ...

struct FlowScenario {
    std::string name;
    double timeout = 60;   // seconds, the whole run
    std::vector<FlowStep> steps;
};
// Strict: unknown keys, wrong types and out-of-range values are errors, with the step.
bool parseFlowScenario(std::string_view json, FlowScenario& out, std::string& error);
inline constexpr size_t kMaxFlowSteps = 1000;
inline constexpr double kMaxFlowTimeout = 3600;   // seconds

class FlowTest {
public:
    struct Hooks {
        GameFlow* flow = nullptr;   // required
        AppShell* shell = nullptr;   // the shell's buttons; clicks without a document need it
        std::function<bool()> playing;   // a level is playing
        // A click in one of the level's UI documents (by its name), as the mouse would.
        std::function<bool(std::string_view document, std::string_view element)> clickLevel;
        // "game.<name>": a value in the level's data model, as text; nullopt when unset.
        std::function<std::optional<std::string>(std::string_view name)> levelValue;
        // Saves and loads a slot, waiting for the file; false with why.
        std::function<bool(std::string_view slot, std::string& error)> save, load;
        // Real input at the device boundary (Input::inject, injectMouseMotion).
        std::function<bool(const InputBinding& binding, float value)> inject;
        std::function<void(float x, float y)> look;
        // {"unplug": "gamepad"}: the virtual gamepad goes, as pulling its cable would.
        std::function<void()> unplug;
    };
    enum class State { Running, Passed, Failed };

    FlowTest(FlowScenario scenario, Hooks hooks);
    ~FlowTest();
    FlowTest(const FlowTest&) = delete;
    FlowTest& operator=(const FlowTest&) = delete;

    // Once per frame, after travel has been processed: runs steps until one has to wait.
    // frameDt: the frame's game time, for hold and wait seconds.
    State frame(double frameDt = 0);
    State state() const { return m_state; }
    // Why it failed (empty while running or once passed).
    const std::string& message() const { return m_message; }
    size_t step() const { return m_step; }   // the step running, or the one that failed
    std::string reportJson() const;

private:
    struct Record {
        FlowStep::Kind kind;
        std::string target;
        bool ok = false;
        double ms = 0;
        int frames = 0;
        std::string message;
    };
    // Runs the current step: true when it finished (passed or failed), false to wait.
    bool run(const FlowStep& step, std::string& failure);
    void finishStep(bool ok, std::string message);
    void follow();   // notes a ticket started by a click, a level or the shell
    std::optional<std::string> value(std::string_view name) const;

    using Clock = std::chrono::steady_clock;
    FlowScenario m_scenario;
    Hooks m_hooks;
    State m_state = State::Running;
    std::string m_message;
    size_t m_step = 0;
    Clock::time_point m_started, m_stepStarted;
    bool m_stepBegun = false;
    int m_stepFrames = 0;
    double m_stepTime = 0;   // game seconds in this step
    bool m_holding = false;   // a hold or tap has pressed its binding
    int m_lookLeft = 0;   // look: frames still to move
    double m_frameDt = 0;
    void release();   // lets go of a hold or tap
    uint64_t m_ticket = 0;   // the latest travel ticket
    std::vector<Record> m_records;
    std::vector<TravelCompletion> m_completions;
    GameFlow::Subscription m_subscription;
};

}   // namespace shockwave
