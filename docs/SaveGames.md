# Save games

Save games let players stop and pick up where they left off. A save holds the whole
running world: every entity with its components, plus a few values your game adds. You
can save and load from game code, from the editor's Save Games window, and from the
player's command line.

A save file is an ordinary scene file with a short header on top, so you can open it,
read it, diff it and check it like any scene.

## Save and load from game code

`game.saves()` is there whenever the game is playing.

```cpp
#include <Shockwave/Game.h>
using namespace shockwave;

class MyGame final : public GameMode {
public:
    void frame(GameContext& game, double) override {
        if (game.input().wasActionPressed("quick_save")) game.saves().save("quicksave", "Quick save");
        if (game.input().wasActionPressed("quick_load")) game.saves().load("quicksave");
    }
};
```

- `save("slot", "title")` copies the world right away and writes the file in the
  background. The title is optional and shows in save lists.
- `load("slot")` reads the file in the background. The world is swapped in at the start
  of the next tick, or the next frame while the game is paused, so a pause menu can load.

The sample project does exactly this: F5 saves and F9 loads. The bindings are in
`Games/Sample/Config/Input.swinput`.

## Decide what gets saved

Every field you list in `SW_COMPONENT` is saved. Fields you leave out are not.

```cpp
struct Health {
    float current = 100;
    float max = 100;
    float flashTimer = 0;   // just for effects: not worth saving
};
SW_COMPONENT(Health, SW_FIELD(current), SW_FIELD(max))
```

So the easiest way to make game state survive a save is to keep it in a component. A
"GameState" entity with a component holding the score or the current level works well.

For state that lives in your GameMode instead, use values:

| Call | What it does |
|---|---|
| `game.saves().set("coins", 12)` | Stores a value. Takes text, `int`, `float`, `double` or `bool`. |
| `game.saves().getInt("coins")` | Reads it back. Also `get` (text), `getNumber`, `getBool`. Each takes a fallback for when it's missing. |
| `game.saves().has("coins")` | Whether the value is set. |

Values go into every save from then on, and a load replaces them with the saved ones.
Keys use letters, digits, `_`, `-` and `.`.

## React to saving and loading

Two GameMode hooks run on the main thread:

```cpp
// Just before a save copies the world: put GameMode state into values.
void beforeSave(GameContext& game) override { game.saves().set("pulses", m_pulses); }

// Right after a load swapped the world in: read values back, refresh the UI.
void afterLoad(GameContext& game) override {
    m_pulses = game.saves().getInt("pulses");
    game.ui().set("pulses", m_pulses);
}
```

Entities come back with the same handles they had when saved. An `EntityRef` you found
in `beginPlay`, such as `game.find("Player")`, still points at the player after a load,
as long as that entity existed when the game was saved.

## Use the editor

Open **Window > Save Games**. It lists the project's saves, newest first, with their
title, scene, time, play time and entity count.

| To | Do this |
|---|---|
| Save the running game | Press Play, type a slot name (and a title if you like), press **Save** |
| Load a save | During Play, select it and press **Load**, or double-click it |
| Start the game from a save | While editing, select it and press **Play from save** |
| Delete a save | Select it and press **Delete** |
| See its values | Select it: they're listed under the buttons |

Messages about saving and loading appear in the Console.

## Use the command line

These are handy for testing, and for tools and AI agents.

```sh
./build/bin/player Games/Sample/Sample.swproject --save slot1 --frames 90
```

Plays 90 frames in a hidden window, saves to `slot1`, and quits. Without `--frames` it
saves after 30 frames.

```sh
./build/bin/player Games/Sample/Sample.swproject --load slot1 --screenshot loaded.png
```

Starts the game from `slot1` and saves a screenshot. Without `--screenshot` you just
play from the save. `--load` also takes a path to any `.swsave` file. The game starts in
the scene the save came from, unless you pass `--scene`.

```sh
./build/bin/player Games/Sample/Sample.swproject --saves --json saves.json
```

Lists the saves. With `--json` it also writes them, values included, to a file.

```sh
./build/bin/player Games/Sample/Sample.swproject --check
```

Checks the project as usual, and also every save in `Saved/SaveGames`. To check one
file, pass it: `player --check Games/Sample/Saved/SaveGames/slot1.swsave`.

## Where saves live

Saves are `<project>/Saved/SaveGames/<slot>.swsave`. The `Saved` folder is per user and
isn't in version control.

Slot names use letters, digits, spaces, `-` and `_`, up to 64 characters.

## The file format

A save starts like any scene, then adds header lines before the first entity:

```text
# Shockwave scene
shockwave_scene 2
save_game 1
title "Three pulses"
scene "Scenes/main.swscene"
saved_at "2026-10-11 10:37:21"
play_time 55.400
entities 17
value pulses "4"
entity_ids 0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16

entity "Environment"
  Environment
    ...
end
```

| Line | Meaning |
|---|---|
| `save_game 1` | Marks the file as a save. Required. |
| `title` | The title given to `save()`. Optional. |
| `scene` | The scene Play started from, relative to the project. |
| `saved_at` | When it was saved, in UTC. |
| `play_time` | Seconds of play, carried over through loads. |
| `entities` | How many entities follow. Shown in lists. |
| `value <key> "<text>"` | One value from `game.saves().set`. |
| `entity_ids` | The handle of each entity block, in order: `index`, or `index:generation`. Long lists wrap onto more `entity_ids` lines. |

The entity blocks follow [the scene format](SceneSchema.md).

You can write a save by hand or with a tool. Leave out `entity_ids` and the entities
get new handles when loaded. If you keep `entity_ids`, it must list exactly one id per
entity block; `player --check` tells you when it doesn't.

## How it works

- `save()` copies the world on the main thread. Copying is fast, and it's what makes
  the save consistent. Turning the copy into text and writing the file happen on a job
  worker, so saving doesn't hitch the frame.
- Files are written to a temporary file and then renamed, so a crash mid-save never
  damages the previous save.
- `load()` reads and parses on a worker too. Between two ticks, the session swaps the
  new world in, rebuilds physics from the loaded components (velocities included), lets
  audio catch up, then calls `afterLoad`.
- If you call `load()` twice before the first finishes, only the newest one is used.
- `save`, `load` and the values belong to the main thread: GameMode hooks and UI
  handlers. Don't call them from systems.
- The code is in `Source/Runtime/GameFramework/Public/GameFramework/SaveGame.h`.

## Limits

- The GameMode keeps running through a load. Its member variables are not saved;
  restore them in `afterLoad` from values or components.
- Game UI keeps its handlers and data values through a load. Update what it shows in
  `afterLoad`.
- Sounds that are already playing keep playing.
- Saves are plain text and not compressed or encrypted.
- On a shipped game, saves go to the project's `Saved` folder. A per-user location will
  come with packaging.
