# Tutorial: from an empty folder to a packaged game

This tutorial makes **Two Rooms**, a small game with everything a finished game needs
around its gameplay:

- a main menu, a pause screen and a loading screen;
- two levels joined by a door;
- a checkpoint that saves, so **Continue** works after you quit;
- a test that plays it from the menu to the end and back;
- a release package that installs and runs on its own.

It starts from an empty folder outside the engine, so nothing comes from the Sample or
another game. Each step shows the exact file or command. You can click instead of typing
where the editor offers it, and each step says how.

The engine's CI follows this page literally every night (`Tests/FollowTutorial.py`), so
if a step stops working, CI fails rather than the page quietly going stale.

You need a Shockwave checkout and its build tools ([README](../README.md#get-started)).
Run the commands from the engine folder, in a POSIX shell (on Windows, Git's bash).

## 0. Build the player

The player makes new projects, so build it first. No games yet: yours joins the build
in step 2. Skip this if you already have `build/bin/player`.

<!-- tutorial: run -->
```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release "-DSHOCKWAVE_GAMES="
cmake --build build --target player -j4
```

## 1. Make the project

Pick a folder for the game. It must be empty or not exist yet:

```sh
GAME="$HOME/Games/TwoRooms"
```

Create the project there. `two_rooms` is the name of its C++ game module:

<!-- tutorial: run -->
```sh
build/bin/player --new-project "$GAME" --name "Two Rooms" --module two_rooms
```

That makes `TwoRooms.swproject`, a `Source/Game.cpp`, a `CMakeLists.txt`, empty bindings
and a starter scene. The editor's project hub **Create project** does the same.

## 2. Build it

Tell the build about the game, then build the player, the editor and the game:

<!-- tutorial: run -->
```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release "-DSHOCKWAVE_GAMES=$GAME"
cmake --build build --target player editor game_two_rooms -j4
```

`SHOCKWAVE_GAMES` replaces the list of games in this build. To keep the bundled ones,
list them too, separated by semicolons ([Starting projects](StartingProjects.md#register-and-build)).

## 3. Controls

Games read **actions**, not keys. Replace `Config/Input.swinput` with:

<!-- tutorial: file Config/Input.swinput -->
```text
shockwave_input 1
deadzone 0.15
bind move_left key A
bind move_right key D
bind move_forward key W
bind move_back key S
bind pause key Escape
bind move_left pad_axis left_x -
bind move_right pad_axis left_x +
bind move_forward pad_axis left_y +
bind move_back pad_axis left_y -
bind pause pad start
```

`pause` is the action the pause screen listens for. In the editor, **Window > Input** edits
the same file ([Input](Input.md)).

## 4. Three scenes

The game has a menu and two rooms. Each is a scene. Delete the starter
`Scenes/main.swscene`:

<!-- tutorial: run -->
```sh
rm "$GAME/Scenes/main.swscene"
```

The **menu** scene only needs something to look at. The menu itself is drawn by the
application shell (step 6).

<!-- tutorial: file Scenes/menu.swscene -->
```text
shockwave_scene 2

entity "Environment"
  Environment
    sky_color 0.08 0.1 0.16 1
    ambient_intensity 0.6
end

entity "Camera"
  Transform
    position 0 3 7
    rotation -15 0 0
  Camera
    primary 1
end
```

**Room A** has the player, a spawn to start at, a checkpoint halfway and a door to room B:

<!-- tutorial: file Scenes/room_a.swscene -->
```text
shockwave_scene 2

entity "Environment"
  Environment
    sky_color 0.15 0.22 0.3 1
    ambient_intensity 0.8
end

entity "Sun"
  Transform
    rotation -50 -25 0
  Light
    type Directional
    intensity 2
end

entity "Camera"
  Transform
    position 4 6 10
    rotation -30 0 0
  Camera
    primary 1
end

entity "Floor"
  Transform
    position 4 -0.5 0
    scale 14 1 6
  MeshRenderer
    mesh cube
    texture white
    tint 0.3 0.34 0.38 1
end

entity "Player"
  Transform
    position 0 0.5 0
  MeshRenderer
    mesh cube
    texture white
    tint 0.9 0.4 0.12 1
end

entity "Start"
  Transform
    position 0 0.5 0
  LevelSpawn
    id start
end

entity "Halfway"
  Transform
    position 4 0.5 0
  LevelSpawn
    id halfway
  Checkpoint
    id halfway
    spawn halfway
end

entity "Door to room B"
  Transform
    position 8 0.5 0
  MeshRenderer
    mesh cube
    texture white
    tint 0.2 0.8 0.4 1
  LevelExit
    destination room_b
    spawn entrance
end
```

**Room B** has an entrance and a way back out to the menu:

<!-- tutorial: file Scenes/room_b.swscene -->
```text
shockwave_scene 2

entity "Environment"
  Environment
    sky_color 0.3 0.2 0.15 1
    ambient_intensity 0.8
end

entity "Sun"
  Transform
    rotation -50 25 0
  Light
    type Directional
    intensity 2
end

entity "Camera"
  Transform
    position 3 6 10
    rotation -30 0 0
  Camera
    primary 1
end

entity "Floor"
  Transform
    position 3 -0.5 0
    scale 10 1 6
  MeshRenderer
    mesh cube
    texture white
    tint 0.38 0.34 0.3 1
end

entity "Player"
  Transform
    position 0 0.5 0
  MeshRenderer
    mesh cube
    texture white
    tint 0.9 0.4 0.12 1
end

entity "Entrance"
  Transform
    position 0 0.5 0
  LevelSpawn
    id entrance
end

entity "Way out"
  Transform
    position 6 0.5 0
  MeshRenderer
    mesh cube
    texture white
    tint 0.2 0.8 0.4 1
  LevelExit
    destination menu
end
```

The four level components do this ([Levels](Levels.md#in-the-scenes)):

- **`LevelSpawn`** is where a player arrives. Room A's `start` is where a new game begins.
- **`Checkpoint`** is a place to restart from, at the spawn it names.
- **`LevelExit`** is a door: its `destination` level and the `spawn` to arrive at there.
  Without a spawn, the destination's default is used. It only describes the door: your
  game code decides when it's used (step 7).
- **`PersistentIdentity`** isn't needed here. Add it to anything whose state must survive
  a save, like a picked-up key ([Save games](SaveGames.md#decide-what-gets-saved)).

In the editor, **Create** adds the entities. The components are in the Inspector's
**Levels** category.

## 5. The level catalog

The catalog says which scenes are levels, what kind each is, where the game starts and
which level follows which. Create `Config/Levels.swlevels`:

<!-- tutorial: file Config/Levels.swlevels -->
```text
shockwave_levels 1
entry "menu"
level "menu"
  scene "Scenes/menu.swscene"
  kind menu
  next "room_a"
end
level "room_a"
  scene "Scenes/room_a.swscene"
  kind gameplay
  default_spawn "start"
  next "room_b"
end
level "room_b"
  scene "Scenes/room_b.swscene"
  kind gameplay
  default_spawn "entrance"
  next "menu"
end
```

- `entry` is where the game starts.
- `kind menu` puts the main menu over that level. `kind gameplay` is for playing.
- `next` is where **New game** goes from the menu, and where each room leads.
- `default_spawn` is where travel arrives when nobody asked for a spawn.

A level's ID (`room_a`) is what saves and exits refer to. It stays when you move or
rename the scene file: change `scene` here, and every door and save still finds it. Don't
rename an ID once players have saves: old saves name the old ID, and only a save migration
can move them ([Save games](SaveGames.md#compatibility)).

**Project > Levels** in the editor edits this file, checks every link and adds levels
from scenes ([Levels](Levels.md#in-the-editor)).

## 6. The project file

Point the project at the catalog, start at the menu, and turn on every screen of the
application shell. Replace `TwoRooms.swproject` with:

<!-- tutorial: file TwoRooms.swproject -->
```text
# Paths are relative to this file. Both editor and player load this manifest.
shockwave_project 1
name "Two Rooms"
game_module "two_rooms"
startup_scene "Scenes/menu.swscene"
assets "Content"
levels "Config/Levels.swlevels"
app_shell all
tick_rate 60
window_size 1280 720
vsync true
```

`app_shell all` gives the game a main menu (New game, Continue, Settings, Credits,
Quit), a pause screen, a loading screen and an error screen, all switchable one by one
([Application shell](AppShell.md)). **Project > Settings** edits this file.

## 7. The game code

The game code moves the player, saves at checkpoints and uses doors. Replace
`Source/Game.cpp` with:

<!-- tutorial: file Source/Game.cpp -->
```cpp
#include <Shockwave/Game.h>

using namespace shockwave;

namespace {

constexpr float kSpeed = 5.0f;   // units per second
constexpr float kReach = 1.5f;   // this close to a checkpoint or exit counts as touching it

class TwoRooms final : public GameMode {
public:
    void beginPlay(GameContext& game) override {
        m_player = game.find("Player").entity();
        if (!m_player.valid()) return;   // the menu has no player
        // Arrive where travel asked for: the LevelSpawn of the exit, or the level's default.
        if (game.startReason == StartReason::Fresh && game.arrival.found)
            game.world.get<Transform>(m_player)->position = game.arrival.transform.position;
    }

    void fixedUpdate(GameContext& game, double dt) override {
        if (!m_player.valid()) return;
        Input& input = game.input();
        Transform& body = *game.world.get<Transform>(m_player);
        body.position.x += input.axis("move_left", "move_right") * kSpeed * static_cast<float>(dt);
        body.position.z -= input.axis("move_back", "move_forward") * kSpeed * static_cast<float>(dt);

        auto touching = [&](Entity e) {
            return glm::distance(glm::vec3(worldMatrix(game.world, e)[3]), body.position) < kReach;
        };
        // A checkpoint saves once; Restart checkpoint on the pause screen comes back here.
        game.world.each<const Checkpoint>([&](Entity e, const Checkpoint& checkpoint) {
            if (checkpoint.enabled && touching(e) && game.saves().checkpointId() != checkpoint.id)
                game.saves().checkpoint(checkpoint.id);
        });
        // An exit travels to its destination. It's one-shot, so touching it again does nothing.
        game.world.each<const LevelExit>([&](Entity e, const LevelExit& exit) {
            if (exit.enabled && touching(e)) game.flow().useExit(e);
        });
        // For the flow test (and a HUD, if you make one): the checkpoint reached.
        game.ui().set("checkpoint", game.saves().checkpointId());
    }

private:
    Entity m_player{};
};

}   // namespace

SW_GAME("two_rooms", TwoRooms)
```

What it does:

- **`game.arrival`** is the spawn this level was entered at: the door's spawn, or the
  catalog's `default_spawn`. `beginPlay` puts the player there. `StartReason::Fresh`
  leaves a loaded save alone, since the save already put the player where they were.
- **`game.saves().checkpoint(id)`** writes the checkpoint save. Continue loads the newest
  save, and the pause screen's **Restart checkpoint** goes back to it.
- **`game.flow().useExit(e)`** travels through a door. The level doesn't end there and
  then: it ends after this frame, and the loading screen shows while the next one loads.
  The request carries this level's **epoch**, so a callback left over from an old level
  can't travel by mistake. A second request while one is under way is refused as `Busy`.
- **`game.ui().set("checkpoint", ...)`** publishes a value for UI and for tests.

Rebuild the game and check the project:

<!-- tutorial: run -->
```sh
cmake --build build --target game_two_rooms
build/bin/player --project "$GAME" --check
build/bin/player --project "$GAME" --levels
```

`--check` reads every scene, the catalog and the bindings, and exits with 1 on a problem.
`--levels` lists each level and where its doors lead:

```text
level menu menu Scenes/menu.swscene, next room_a, 0 spawns, exits: none
level room_a gameplay Scenes/room_a.swscene, next room_b, 2 spawns, exits: room_b:entrance
level room_b gameplay Scenes/room_b.swscene, next menu, 1 spawn, exits: menu
```

## 8. Play it

```sh
build/bin/player --project "$GAME"
```

Choose **New game**, walk right with **D** (or the left stick) past the checkpoint and
into the green door. Room B loads. Press **Escape** for the pause screen, or walk right
again to get back to the menu, where **Continue** now works.

Keys held while a level loads don't count until you let go of them and press again. The
loading screen had the input, so the new level doesn't inherit a key press it never saw
([Input](Input.md)).

In the editor (`build/bin/editor "$GAME"`), Play does the same. **Window > Runtime flow**
shows the level playing, its epoch, each travel phase by phase and its result.

## 9. A test that plays it

A flow test plays the game through its real menus and controls, and fails at the first
step that doesn't happen ([Flow tests](FlowTest.md)). Create `Tests/play.json`:

<!-- tutorial: file Tests/play.json -->
```json
{
  "format": 1,
  "name": "menu, room A, checkpoint, room B, back to the menu",
  "timeout": 120,
  "steps": [
    {"assert_level": "menu"},
    {"click": "new_game"},
    {"wait_phase": "succeeded", "timeout": 30},
    {"assert_level": "room_a"},
    {"wait": 0.5},
    {"hold": "key D", "until": "game.checkpoint", "equals": "halfway", "timeout": 10},
    {"hold": "key D", "seconds": 2},
    {"wait_phase": "succeeded", "timeout": 30},
    {"assert_level": "room_b"},
    {"wait": 0.5},
    {"hold": "key D", "seconds": 2},
    {"wait_phase": "succeeded", "timeout": 30},
    {"assert_level": "menu"},
    {"wait_value": "flow.can_continue", "equals": "true"},
    {"quit": true}
  ]
}
```

The short waits after each level loads give the loading screen time to hand the input
back. Run it without a window:

<!-- tutorial: run expect "flow test passed" -->
```sh
SDL_VIDEO_DRIVER=offscreen SDL_AUDIO_DRIVER=dummy \
    build/bin/player --project "$GAME" --flow-test "$GAME/Tests/play.json" --renderer null
```

## 10. The release contract

A release says what it promises: which game, which version, which platforms and modes,
its budgets, and the scenario that proves it plays. Create `Config/Release.swrelease`.
Change `platform` to yours (`windows-x86_64`, `macos-arm64`, ...) if it isn't Linux:

<!-- tutorial: file Config/Release.swrelease -->
```text
shockwave_release 2
application_id "com.example.tworooms"
release_version "1.0.0"
platform linux-x86_64
mode offline
max_players 1
target_fps 60
simulation_hz 60
graphics_tier medium
cpu_budget_mib 4096
gpu_budget_mib 2048
shared_budget_mib 0
loading_budget_mib 512
transition_timeout_seconds 30
acceptance_scenario "Tests/play.json"
frame_p95_ms 16.667
frame_p99_ms 20.833
max_hitch_ms 50
level_load_seconds 10
```

`application_id` names the game for good: players' saves and settings are found by it,
so never change it ([Release](Release.md)). Check the project against the release rules:

<!-- tutorial: run -->
```sh
build/bin/player --project "$GAME" --check --release
```

## 11. Package it

<!-- tutorial: run expect "package .*TwoRooms-1\.0\.0-" -->
```sh
build/bin/player --project "$GAME" --package --release
```

The package is in `Saved/Packages/`. It holds only what the levels need (the cook's
list, [Cooking](Cooking.md)), the game's program and its libraries. Players start it with
`TwoRooms`, with no engine, editor or terminal ([Packaging](Packaging.md)).

Last, install it the hard way: read-only, at a path with spaces and accented letters,
started from another folder with a minimal environment. `--verify-install` does that,
plays the contract's scenario, saves, quits, continues and reinstalls:

<!-- tutorial: run expect "verify-install passed" -->
```sh
SDL_VIDEO_DRIVER=offscreen SDL_AUDIO_DRIVER=dummy build/bin/player \
    --verify-install "$GAME/Saved/Packages/TwoRooms-1.0.0-linux-x86_64" --project "$GAME" --renderer null
```

On Windows or macOS the folder name ends in your platform. On the null renderer this
proves the install, not the graphics. A real release runs it on real hardware with
`--install-record` ([Release](Release.md#platforms)).

## What to know before you ship more

- **Main thread.** Game code runs on the main thread: `beginPlay`, `fixedUpdate`, `frame`,
  UI handlers and travel callbacks. Systems you schedule run on workers, with the access
  they declare ([Architecture](Architecture.md)).
- **What lives how long.** A level's world, entities, `EntityRef`s, UI documents, physics
  bodies and sounds end with the level. `game.campaign()` lives for the whole game, and
  goes into every save. Saves and settings live in the player's own folders, found by
  `application_id` ([Level lifetime](LevelLifetime.md#who-owns-what),
  [User paths](UserPaths.md)).
- **Persistence.** There are three scopes. The campaign holds progress across levels.
  A level's entities with a `PersistentIdentity` keep their state in saves. Things
  destroyed with one stay destroyed when you come back. Restart level resets the level,
  and New game resets everything ([Save games](SaveGames.md#keep-progress-across-levels)).
- **Cancelling a load.** `game.flow().cancel(ticket)` works until the old level starts
  retiring. After that it says `TooLateToCancel` and the travel finishes. The loading
  screen hides Cancel at that point ([Level lifetime](LevelLifetime.md#travel-tickets-game-flow)).
- **Errors have codes.** Travel, saves and releases fail with a stable code (`Busy`,
  `UnknownLevel`, `IncompatibleSave`, ...) and a message. Match the code, show the
  message ([Shipping plan, section 24](ShippingImplementation.md#24-error-catalog-and-exact-negative-behavior)).
- **Looking inside a release.** `player --cook-info <Cook.swcook>` lists what a cook
  holds, and `--verify <folder>` checks every file against it. `player --release-info`
  lists every release gate: passed, failed or not yet measured ([Cooking](Cooking.md),
  [Release](Release.md#gates)).
- **Before players see it**, run a release rehearsal: a new tester, a clean machine, only
  the package and a few lines of instructions ([Release rehearsal](ReleaseRehearsal.md)).
