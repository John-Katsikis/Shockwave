# 5. Your first game code

**In this chapter:** controls as named actions, your first component, a GameMode, and
how the editor loads your code. By the end, the Orb Hunt player can move.

## Controls are actions, not keys

Game code never asks "is W down?". It asks "is `move_forward` down?". A bindings file
says which keys and buttons mean `move_forward`. That way players can rebind controls,
and a gamepad works with no extra code.

Create `Games/OrbHunt/Config/Input.swinput`:

```text
shockwave_input 1
deadzone 0.15

bind move_left key A
bind move_right key D
bind move_forward key W
bind move_back key S
bind jump key Space

bind move_left pad_axis left_x -
bind move_right pad_axis left_x +
bind move_forward pad_axis left_y +
bind move_back pad_axis left_y -
bind jump pad south
```

`pad south` is the bottom face button: A on Xbox, Cross on PlayStation. The `deadzone`
ignores tiny stick movements so a resting stick doesn't drift. Every name you can use
is in [InputSchema.md](../InputSchema.md), and **Window > Input** in the editor shows
your controllers live and edits this file for you.

## Where game code lives

Your C++ goes in the project's `Source/` folder and is built into a **game module**: a
library (`build/bin/modules/game_orb_hunt.so`) that the editor and the player load when
they open your project.

Create `Games/OrbHunt/CMakeLists.txt` with one line:

```cmake
shockwave_game_module(orb_hunt)
```

That builds every `.cpp` in `Source/`. The id `orb_hunt` must match `game_module` in the
manifest.

## The code

Create `Games/OrbHunt/Source/OrbHunt.cpp`:

```cpp
#include <Shockwave/Game.h>   // everything game code normally needs

using namespace shockwave;

// How fast the player runs. A component: plain data, no code.
struct Runner {
    float speed = 5.0f;   // meters per second
};
SW_COMPONENT(Runner, SW_FIELD(speed, 0, 20))

// The rules of Orb Hunt.
class OrbHunt final : public GameMode {
public:
    // Play has started and the scene is loaded.
    void beginPlay(GameContext& game) override {
        m_player = game.find("Player");
        if (m_player && !m_player.has<Runner>()) m_player.add(Runner{});
        SW_LOG(LogGame, Info, "Orb Hunt started");
    }

    // Once per rendered frame: read the controls and move the player.
    void frame(GameContext& game, double frameDt) override {
        if (!m_player) return;
        Input& input = game.input();
        const float right = input.axis("move_left", "move_right");
        const float forward = input.axis("move_back", "move_forward");
        const float step = m_player.get<Runner>().speed * static_cast<float>(frameDt);
        Transform& transform = m_player.get<Transform>();
        transform.position.x += right * step;
        transform.position.z -= forward * step;   // forward is -Z
    }

private:
    EntityRef m_player;
};

SW_GAME("orb_hunt", OrbHunt)
```

Here's every piece.

### `#include <Shockwave/Game.h>`

One include that brings in everything game code usually needs: components, the World,
input, audio, physics, UI, logging and glm (the math library).

### The component and `SW_COMPONENT`

`Runner` is a component, exactly like the built-in ones. `SW_COMPONENT` registers it
with the engine when your module loads. `SW_FIELD(speed, 0, 20)` says "`speed` is saved
in scene files, and the Inspector should let you drag it between 0 and 20".

Once registered, your component works everywhere a built-in one does: you can add it in
the Inspector under **+ Component**, it's saved in scenes as `Runner` with `speed 5`,
it's listed in the schema, and undo works on it. Fields you *don't* list in
`SW_COMPONENT` are runtime-only: they exist while playing but aren't saved.

### The GameMode

A **GameMode** holds the rules for one Play of a scene. Each time you press Play, the
engine makes a fresh `OrbHunt`; when you press Stop, it's thrown away. You override the
hooks you need:

| Hook | When it runs | Use it for |
|---|---|---|
| `configureSystems` | as Play starts | adding systems (chapter 6) |
| `beginPlay` | once the scene is loaded | finding things, spawning, setup |
| `fixedUpdate` | every fixed tick, 60 times a second | game rules that must be steady |
| `frame` | once per rendered frame | input, sound, UI, one-off events |
| `endPlay` | as Play stops | cleaning up anything outside the World |

All of these run on the main thread, one at a time, so you can do anything in them.

### `GameContext`

Every hook gets a `GameContext`, called `game` here. It's your doorway into the running
game:

| Call | Gives you |
|---|---|
| `game.world` | all the entities and components |
| `game.find("Player")` | an entity by its name, as an `EntityRef` |
| `game.input()` | controls |
| `game.audio()`, `game.physics()`, `game.ui()`, `game.saves()` | the other systems, in later chapters |

### `EntityRef`

`game.find` returns an `EntityRef`: an entity plus the World it lives in, so you can
write `m_player.get<Transform>()` instead of `game.world.get<Transform>(entity)`. It
turns into `false` when nothing was found or the entity was destroyed, which is why the
code checks `if (!m_player) return;`. It also has `has<T>()`, `add(T{...})`,
`remove<T>()` and `destroy()`.

### `input.axis`

`axis("move_left", "move_right")` gives a number from -1 to 1. With keys it's -1, 0 or
1. With a stick it's anything in between, so the player walks slowly when you push the
stick a little.

### `SW_GAME`

The last line connects your GameMode to the id in the manifest. It must sit outside any
namespace.

## Build and play

Tell CMake about the new game (the sample stays in the list), then build:

```sh
cmake -B build "-DSHOCKWAVE_GAMES=$PWD/Games/Sample;$PWD/Games/OrbHunt"
cmake --build build -j
```

You only run the first line once; CMake remembers. Open the project and play:

```sh
./build/bin/editor Games/OrbHunt
```

Press **Ctrl+P**, click the viewport so it has keyboard focus, and walk with **WASD**
or the left stick. The Console shows "Orb Hunt started".

If Play stays greyed out, the module didn't build. The Console says why.

## Something's missing

Try walking off the edge of the floor. You don't fall: nothing in our code knows about
gravity or the ground. We're just changing numbers in a `Transform`. And moving in
`frame` means the speed depends on how smoothly frames arrive. Chapter 7 explains why
game rules belong in the fixed tick, and chapter 8 replaces this hand-made movement
with real physics.

> **Under the hood.** `SW_COMPONENT` and `SW_GAME` are macros in
> `Source/Runtime/GameFramework/Public/GameFramework/GameModule.h`. `SW_COMPONENT`
> records your struct's fields in the engine's component registry; that registry is
> what the Inspector, the scene reader, `player --schema` and `player --check` all read.
> `SW_GAME` defines a `GameModule` whose GameInstance makes your `GameMode` for every
> Play. If you want different rules per scene, write your own `GameInstance`
> ([Projects](../Projects.md#use-different-rules-per-scene)).

## Recap

- Controls are named actions, bound in `Config/Input.swinput`.
- Game code is a module: a `Source/` folder plus `shockwave_game_module(<id>)`.
- `SW_COMPONENT` turns a struct into a real component. `SW_GAME` connects your
  GameMode.
- Hooks: `beginPlay`, `fixedUpdate`, `frame`, `endPlay`. `GameContext` reaches
  everything else.

Next: [Systems: work on every core](06-systems.md).
