# Levels: the level catalog

A game with more than one map, a menu or an ending lists them in a **level catalog**. It
says which scene each level is and what kind of level it is. It says which level the game
starts in, and where each level leads. The scenes hold the doors and arrival points
between levels. Shockwave checks that all of it lines up before anyone plays it.

This is S02 of [the shipping plan](ShippingImplementation.md). It adds the catalog and the
checks. Moving between levels while the game runs is [Level lifetime](LevelLifetime.md)
(S03 to S05): `game.flow()` travels to a level and spawn, `game.arrival` says where the
new level was entered, and `game.flow().useExit` leaves through a `LevelExit`. The file format, components and every diagnostic code are in
[LevelSchema.md](LevelSchema.md), which is generated.

## Turning it on, and off

The catalog is optional. A project without one works exactly as before: it has one
implicit level, `startup`, which is its `startup_scene`. Nothing is written to disk for it.

To use a catalog, add the `levels` key to the `.swproject` ([Projects.md](Projects.md)):

```
levels "Config/Levels.swlevels"
```

Removing the key turns it off again. The level components below do nothing on their own,
so scenes that have them still load and play in a project without a catalog.

## The file

```
shockwave_levels 1
entry "menu"

level "menu"
  scene "Scenes/menu.swscene"
  kind menu
  next "a"
end

level "a"
  scene "Scenes/a.swscene"
  kind gameplay
  default_spawn "start"
  next "b"
  preload "Content/UI/loading.rml"
end
```

- `entry` is the level the game starts in.
- `kind` is `menu`, `gameplay` or `ending`. A gameplay level needs a primary Camera. A menu
  or ending can be drawn by a UIDocument alone.
- `default_spawn` is the LevelSpawn players arrive at when nothing else says.
- `next` is where the level leads by default. It never travels on its own.
- `preload` lists files to load before the level starts. It can repeat.

Level IDs are lowercase: letters, digits, `-` and `_`, starting with a letter. They are
stable, so saves and exits can name them. Paths are relative to the project and must stay
inside it. Two levels can share a scene, and `next` can loop back (replays, rotations).

## In the scenes

Four components, in the editor's **Levels** category:

| Component | What it's for |
| --- | --- |
| `LevelSpawn` | where players arrive. `id` defaults to `start`; `team` -1 means anyone. |
| `LevelExit` | a way to another level: `destination`, and optionally the `spawn` to arrive at there. |
| `Checkpoint` | a point to restart from, with the `spawn` in this scene it puts players at. |
| `PersistentIdentity` | a stable ID (a UUID) for an entity whose state is saved. |

Persistent IDs are unique within a level. Inside a prefab copy, an entity's effective ID is
`<the copy's ID>/<its own>`, so the copy's root needs a PersistentIdentity of its own.
Adding a PersistentIdentity in the editor gives it a new ID.

## Checking it

```sh
build/bin/player --project <file.swproject> --check
build/bin/player --project <file.swproject> --levels [--json <file|->]
```

`--check` now checks the catalog too. It loads every level's scene in an isolated world
and follows the links between them:

- every exit leads to a real level, and to a spawn that exists there;
- every `default_spawn` and checkpoint spawn exists;
- no scene has two spawns, checkpoints or persistent IDs with one ID;
- gameplay levels have a camera;
- paths exist, stay in the project and match the disk's spelling exactly (a case mistake
  works on Windows and breaks on Linux).

A level nothing leads to from the entry is a warning, not an error, because a game may
reach it some other way.

### Checks

Every problem `--check` finds is listed under the kind of fix it needs, so a failure is
never just "the level won't run":

| Kind | Heading | What it covers |
|---|---|---|
| `content` | Missing or broken content | Files, scenes, prefabs, assets, UI, materials, a name that matches no entity, links between levels |
| `navigation` | Navigation | A navigation mesh that's unreadable or out of date, agents and areas that don't fit it |
| `circuit` | Circuit errors | A circuit that doesn't exist or doesn't compile, variables it doesn't declare, travel to a level that isn't there |
| `physics` | Physics setup | Bodies and characters that can't be built |
| `gameplay` | Gameplay code and rules | The game's own checks on its content, and a game that throws when it starts |
| `settings` | Project settings | The renderer, input bindings, packaging, release and network settings |

```text
Missing or broken content (1, content):
Scenes/training.swscene:276: no entity named 'Nowhere'; left empty
Circuit errors (1, circuit):
Scenes/training.swscene: entity 'Training exit': Circuit: Circuits/Missing.swcircuit doesn't exist in Content
Gameplay code and rules (1, gameplay):
Scenes/training.swscene: objective "training.line" (reach) needs a target with a Transform
3 problems in 4 scenes
```

The gameplay checks are the game's own. Override `GameInstance::checkScene` and return
one line per problem (without the file name). `--check` and the editor's Validate call it
on each scene, loaded but not playing:

```cpp
std::vector<std::string> checkScene(const shockwave::World& scene) const override {
    return MyRules::validate(scene);   // "objective \"core\" needs a target", ...
}
```

A game that refuses content its check already listed isn't reported a second time as
"gameplay failed to start".

`--levels` lists the levels with their spawns and exits, then the same diagnostics. With
`--json` it gives the same as JSON (format 1), with stable diagnostic codes. Both exit 1
when anything is wrong.

To start straight in one level:

```sh
build/bin/player --project <file.swproject> --level b
```

`--level` with an ID that isn't in the catalog exits 1 and lists the ones that are. It
can't be combined with `--scene`, `--load` or the online options, because they also choose
where to start (exit 2, `LevelOptionConflict`). `--scene` is still there for development and
skips the catalog.

## In the editor

**Project > Levels** edits the catalog. If the project has none, **Create level catalog**
writes one with a single level, `main`, and adds the `levels` key to the project.

- The table lists every level. Double-click one to open its scene.
- For the selected level you can set its ID (with **Rename**), whether it's the entry, its
  scene, kind, default spawn and next level, and its preloads.
- **New level** writes a new scene with a sky, a camera and ground in `Scenes/`, and adds
  it as a gameplay level.
- **Open scene**, **Play level**, **Duplicate level** and **Remove level** act on the
  selected level. Duplicate level copies its scene to a new file beside it and adds a level
  with the same kind, spawn, next level and preloads. Link to it with an exit or a `next`.
- **Play from entry** opens the entry level's scene and plays the game from its start.
- **Validate** runs the same checks as `--check` on every level's scene: the links between
  levels, the game's own rules, circuits, navigation and physics. It lists what it finds
  under the kind of fix each needs, with a filter for each kind (see [Checks](#checks)).
  Hover a finding for its code, and double-click one to open its scene. It also fills the
  spawn lists from the scenes.
- **Save** writes the file safely (through a temporary file). **Revert** throws edits
  away. Edits undo and redo.
- If the file changes on disk, the panel says so. Revert loads it; Save overwrites it.
- Everything that writes is off during Play.

### Authoring in the editor

The **Create** menu (and the Hierarchy's right-click menu) has a **Level** section:

- **Level spawn**: where players arrive. The scene's first one is `start`, the level's
  default; later ones get `spawn-2` and so on.
- **Level exit**: a way out, aimed at the catalog's next level for this scene. In the
  Inspector, the arrow beside `destination` lists the catalog's levels. What triggers the
  exit is the game's: a trigger and a circuit's `Level.UseExit`, or game code.
- **Checkpoint**: a checkpoint with its own spawn on the same entity. The arrow beside its
  `spawn` lists the scene's spawns.

Objective IDs and the rest of a game's own components are fields in the Inspector, like
any other component. Prefabs open in their own tab to preview and edit.

`editor --level-authoring` runs these actions without anyone at the keyboard, on whatever
project it opens (tests use a copy): New level, a spawn, checkpoint and exit in it,
Duplicate level, Save, then Validate. It prints each finding as `level authoring: <kind>
<severity>: <finding> [<code>]` and exits 1 on any error.

**Rename** changes the ID in the catalog only (its `entry` and every `next`). Exits in
scenes that still use the old ID show up in Validate, so you can fix them.

## Releases

The `level_catalog` release gate ([Release.md](Release.md)) is measured now:

- **failed** without a catalog, since a release lists its levels;
- **failed** with the first error when the catalog or a link is wrong;
- **unverified** when the files were checked but not the links between scenes (the
  editor's Release readiness window, until you run `--check`);
- **passed** when every scene loads and every exit and spawn resolves.

`player --release-info` loads the scenes, so it measures the links.
