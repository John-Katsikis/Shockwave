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

## Keep progress across levels

`game.saves()` values belong to the level you're in: they're gone after travel. Progress
that belongs to the whole game, such as unlocked levels, keys or the story so far, goes in
`game.campaign()` instead.

```cpp
game.campaign().set("keys", keys + 1);
if (game.campaign().getBool("met_the_captain")) ...
```

It has the same calls as the values (`set`, `get`, `getInt`, `getNumber`, `getBool`,
`has`), plus `erase`.

- It survives travel, and goes into every save.
- A load brings back the campaign from the save.
- **New game**, after the player confirms, clears it.
- **Restart level** puts it back as it was when you entered the level. A key picked up
  since then isn't kept, so the player can't take it twice.
- With the [application shell](AppShell.md), New game asks first when the campaign holds
  something.

## Saves from another level

A save remembers which level it was made in, and the spawn that level was entered at.
Loading it from another level travels there first, through the loading screen, as a
`LoadSave` travel. The new level starts with `game.startReason` set to `RestoreSave`.
Before its first tick, the save replaces the world, then `afterLoad` runs.

A save from the level you're in loads in place, as before.

The shell's **Continue** loads the newest save this game and profile can load. It skips
saves that are damaged or belong to someone else, and logs why.

## Checkpoints

Put a `Checkpoint` component in the level (an `id`, and the `LevelSpawn` it restarts
at). When the player reaches it:

```cpp
game.saves().checkpoint("bridge");
```

This saves to the `checkpoint` slot in the background.

- **Restart checkpoint** in the pause menu, or `game.saves().restartCheckpoint()`, loads
  it. A checkpoint in another level travels there.
- It only becomes the restart point once the file is written. If writing fails, the
  previous checkpoint stays.
- `checkpointState()` is `Saving`, then `Saved` or `Failed`, for a HUD. The shell has the
  same as `flow.checkpoint_state`.
- A checkpoint only restarts the campaign it was made in. After New game, there's none
  until the next one is reached.

## Rewards once

`game.campaign().complete("boss_1")` is true the first time, and false ever after in that
campaign, through saves and loads. Give a reward only when it's true:

```cpp
if (game.campaign().complete("boss_1")) game.campaign().set("gold", gold + 100);
```

Restart level undoes completions made since you entered the level, together with their
rewards, so they can be earned again but never twice.

## Finishing a level

When a level is done for good, finish it instead of just travelling:

```cpp
if (game.campaign().complete("factory_done")) game.campaign().set("medals", medals + 1);
game.finishLevel("ending");
```

It asks to travel to `ending`. Once that's accepted, it writes the `autosave` slot with
the campaign and the next level, and no world.

If the game crashes before `ending` starts, Continue picks the autosave. It starts
`ending` as authored, with the completion already recorded, so the medal isn't given
again.

## Things that stay gone

Authored entities with a `PersistentIdentity` stay destroyed when you come back to their
level: enemies you beat, pickups you took. When you leave a level, the identities that
are gone go into the campaign. When the level starts again, they're removed before
`beginPlay`.

- Restart level brings back what this visit destroyed.
- New game brings everything back.
- `remember_destroyed false` in the `.swproject` turns it off.

Entities spawned at runtime aren't tracked; a save holds them while the level plays.

## Circuits

| Node | What it does |
|---|---|
| `Level.ReachCheckpoint` (id) | `checkpoint(id)`: exits `saving` or `refused`, with the code |
| `Level.RestartCheckpoint` | `restartCheckpoint()` |
| `Level.FinishLevel` (level, spawn) | `finishLevel`: a ticket, like `Level.RequestTravel` |
| `Campaign.Complete` (id) | exits `first` the first time, `already` after |
| `Campaign.Set` (key, value), `Campaign.Get` (key) | campaign values, as text |

## Compatibility

Each save says which game it belongs to:

- the release contract's `application_id`;
- the player profile;
- the game's save schema;
- the release version that wrote it.

Loading refuses another game's save (`WrongGame`) and another profile's (`WrongProfile`).
A different release version still loads, since the version is only there for information.

When what your game keeps in saves changes meaning, raise `save_schema` in the
`.swproject`. It starts at 1. Then step older saves up in your `GameInstance`:

```cpp
bool migrateSave(int fromSchema, SaveInfo& save, std::string& error) const override {
    if (fromSchema == 1) {                       // 1 -> 2: coins became gold
        save.values["gold"] = save.values["coins"];
        save.values.erase("coins");
    }
    return true;                                 // false, with error, refuses the save
}
```

It runs once per step, oldest first: a schema 1 save in a schema 3 game calls it with 1,
then with 2. It runs on a job worker with the save on its own, so change only `save`.
The file on disk is never changed. A save from a newer schema than the game is refused
(`NewerSchema`).

Saves from before this (`save_game 1`) still load, in place, as schema 1. They have no
level, game or campaign, so the campaign you have stays as it is.

## When saving goes wrong

Every problem has a stable code at the start of its message (`lastError()`, the console,
`--save-info`):

| Code | What happened |
|---|---|
| `Corrupt` | The checksum doesn't match: the file is damaged or cut short. A damaged slot loads its backup instead, if it has one, and says so. |
| `NewerFormat`, `NewerSchema` | Written by a newer engine or a newer version of the game |
| `WrongGame`, `WrongProfile` | Another game's or another player's save |
| `MigrationFailed` | Your `migrateSave` refused it |
| `TooLarge` | Over the reader's limits: 256 MiB, 4096 values, 64 KiB per value |
| `Conflict` | Another copy of the game saved this slot since this one read it |
| `WriteFailed` | The disk is full, read-only or refused the file |
| `Busy` | Two saves are still being written |
| `NoCheckpoint` | Restart checkpoint with no checkpoint written in this campaign |
| `UnavailableDuringNetworkPlay` | Saving is off online |

How a save is written:

1. The world is copied between ticks.
2. The copy is written to a temporary file with a unique name, next to the slot.
3. The temporary file is flushed to the disk.
4. The previous save is copied to `<slot>.swsave.bak`.
5. The temporary file is renamed over the slot, which replaces it whole or not at all.
6. On Linux and macOS, the folder is flushed too.

A crash or a full disk at any step leaves the previous save as it was. Saves are written
in order, one at a time, and at most two can wait.

A slot remembers how many times it was written (`revision`). If another copy of the game
wrote it after this one read it, the save fails with `Conflict` and nothing is
overwritten. What it would have written is kept as `<slot>.swsave.conflict`. Load the slot
again to save over it.

The checksum catches damage, not cheating: offline saves are yours to edit. After editing
one by hand, delete its `checksum` line and it loads as an edited save. To get a new
checksum, run `player --save-migrate`.

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

Lists the saves, including damaged ones and why they can't load. With `--json` it also
writes them to a file: each save's envelope, values and campaign, `loadable`, and the
problem's `code`.

```sh
./build/bin/player Games/Sample/Sample.swproject --save-info Saved/SaveGames/slot1.swsave
```

Shows one save's envelope: format, schema, revision, checksum, game, profile, level and
values. It also says whether this game and profile can load it, exiting 1 when they
can't. `--json file` (or `-`) writes the same as `--saves` does for one save.

```sh
./build/bin/player Games/Sample/Sample.swproject --save-migrate old.swsave --output new.swsave
```

Writes a copy at this game's format and schema:

- it runs your migrations;
- it fills in the level from the save's scene;
- it gives the copy a new checksum;
- it keeps the entities exactly as they were.

It never writes over the file it reads, or over a file that already exists.

```sh
./build/bin/player Games/Sample/Sample.swproject --check
```

Checks the project as usual, and also every save in `Saved/SaveGames`. To check one
file, pass it: `player --check Games/Sample/Saved/SaveGames/slot1.swsave`.

## Where saves live

Run from source (player or editor Play), saves are `<project>/Saved/SaveGames/<slot>.swsave`.
The `Saved` folder is per user and isn't in version control. An installed game keeps them in
the player's own folder instead: `~/.local/share/<application id>/profiles/default/Saves/`
on Linux, with `%APPDATA%` on Windows and `~/Library/Application Support` on macOS. See
[UserPaths.md](UserPaths.md) for profiles, `--user-dir` and `--import-legacy-saves`.

Slot names use letters, digits, spaces, `-` and `_`, up to 64 characters.

## The file format

A save starts like any scene, then adds header lines before the first entity:

```text
# Shockwave scene
shockwave_scene 2
save_game 2
checksum 5f0c3e1a9b2d4c77
application "shockwave.sample"
schema 1
profile "default"
revision 3
level "startup"
title "Three pulses"
scene "Scenes/main.swscene"
saved_at "2026-10-11 10:37:21"
play_time 55.400
entities 17
campaign keys "2"
value pulses "4"
entity_ids 0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16

entity "Environment"
  Environment
    ...
end
```

| Line | Meaning |
|---|---|
| `save_game 2` | Marks the file as a save, and its format. Required. Format 1 files still load. |
| `checksum` | FNV-1a of the whole file without this line. Delete it after editing by hand. |
| `application`, `profile` | Which game and player it belongs to |
| `schema` | The game's `save_schema` when it was written |
| `content` | The release version that wrote it, for information |
| `revision` | How many times the slot has been written |
| `level`, `spawn` | The level it was saved in, and the spawn that level was entered at (a checkpoint's spawn) |
| `checkpoint` | The checkpoint it was made at |
| `title` | The title given to `save()`. Optional. |
| `scene` | The level's scene, relative to the project |
| `saved_at` | When it was saved, in UTC. |
| `play_time` | Seconds of play, carried over through loads. |
| `entities` | How many entities follow. Shown in lists. |
| `fresh` | An autosave between levels: no world, the level starts as authored |
| `campaign_id` | Which campaign it belongs to |
| `campaign <key> "<text>"` | One value from `game.campaign()` |
| `completed "<id>"` | One `complete()` already granted |
| `destroyed <level> <id>...` | Authored entities destroyed in that level |
| `value <key> "<text>"` | One value from `game.saves().set`. |
| `entity_ids` | The handle of each entity block, in order: `index`, or `index:generation`. Long lists wrap onto more `entity_ids` lines. |

The entity blocks follow [the scene format](SceneSchema.md).

You can write a save by hand or with a tool. Leave out `entity_ids` and the entities
get new handles when loaded. If you keep `entity_ids`, it must list exactly one id per
entity block; `player --check` tells you when it doesn't.

Copies of a [prefab](Prefabs.md) are saved in full: every part gets its own block,
marked `PrefabPart`, so a crate knocked off a prefab stack loads where it landed. A save
written by hand can name just the prefab copy, as a scene does, and the parts are made
from the prefab when it loads.

## How it works

- `save()` copies the world on the main thread. Copying is fast, and it's what makes
  the save consistent. Turning the copy into text and writing the file happen on a job
  worker, so saving doesn't hitch the frame.
- Files are written durably: see [When saving goes wrong](#when-saving-goes-wrong).
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
- A save is a snapshot of the whole level, with the same entity handles. Loading one into
  another level runs that level's `beginPlay` first (with `game.startReason` set to
  `RestoreSave`, so skip spawning then), then swaps the snapshot in and calls `afterLoad`.
