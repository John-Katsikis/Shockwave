# Level lifetime: runtime levels and travel

A game moves between levels: menu, then level A, then B, then back to the menu. Each move
has to end one level completely before the next one starts. Its GameMode, physics,
sounds and callbacks all go, and nothing of the old level may leak into the new one. In
the editor, a level change during Play must never touch the scene you're editing.

This is S03 of [the shipping plan](ShippingImplementation.md). It adds the lifetime rules
and a working move between levels. Loading in the background, loading screens, menus and
arriving at a spawn come with S04 and S05. The level catalog itself is
[Levels.md](Levels.md).

## Asking for another level

From any GameMode callback, or from a UI handler during Play. A circuit node for it comes
with S05.

```cpp
void fixedUpdate(GameContext& game, double) override {
    if (reachedTheDoor) game.requestLevel("b");             // B's default spawn
    if (tookTheLift) game.requestLevel("b", "rooftop");     // a LevelSpawn in B
}
```

The spawn travels with the request, and the started hook sees it. Placing the player
there is S05's job. The move doesn't happen inside the call. The level keeps running to the end of the frame,
then ends, and the next one starts. So it's safe from `fixedUpdate`, `frame`, a button's
handler, or even `beginPlay`.

`requestLevel` returns false when nothing can travel. That's a session without levels (a
tool, a test) or a request from a level that has already ended.

`game.startReason` says why a level's session started: `Fresh`, `RestoreSave`, or
`OnlineBaseline` for a client joining a host. `game.levelEpoch` says which level it is.
Each level that starts gets the next epoch.

## What happens, in order

`LevelHost` (`GameFramework/LevelHost.h`) owns the level that's playing. The player and
the editor call `process()` once a frame, after the frame's ticks, frame and UI work have
returned. When a request is waiting:

1. **Prepare.** The next level's scene loads into a separate world. If that fails, the
   request fails and the current level keeps playing, untouched. With asset leases on,
   its dependency closure is planned against what's resident, and a level over the
   budget fails here as `BudgetExceeded` ([AssetResidency.md](AssetResidency.md)).
2. **End.** The current level's session stops: the network closes, then `endPlay` runs,
   then circuits stop, then the subsystems go, newest first. If `endPlay` throws, the
   level still ends.
3. **Swap.** The new level's asset lease is taken and the old one given back, so assets
   both use stay resident. The new level moves into the same World object, so the address
   everything holds stays the same. It gets a new epoch.
4. **Start.** A new session and GameMode start. If the new level asks for another one in
   `beginPlay`, that request runs right after, in the same `process()`.

Every accepted request ends exactly once, through `onResult`:

| Result | When |
| --- | --- |
| `Succeeded` | the new level is playing |
| `Failed` | it didn't load (the current level plays on), it didn't start (nothing is playing; the player goes back to the entry level, the editor stops Play), or it's a redirect cycle |
| `Cancelled` | another request replaced it, or Play stopped first, even while it loaded |

## The rules it keeps

- **One level simulates.** Preparing never calls `beginPlay` or steps physics.
- **Nothing from an old level reaches the new one.**
  - An `EntityRef` made in level A stops resolving once B is in, even when B has an
    entity with the same handle. `World::levelEpoch()` tells them apart.
  - A `requestLevel` callback kept from A is refused.
  - A save game loaded in place keeps the same epoch, so its refs stay valid.
- **No loading forever.** Levels that ask for another before they've ticked are
  redirects. More than 8 in a row fail as a cycle, and the message lists the chain.
- **Epochs never wrap.** Running out of epochs (after 2^64 levels) fails explicitly.
- **Settings survive travel.** A level's session keeps the player's graphics settings
  (`PreserveUserSettings`). The application's own settings are put back once, when
  Play stops.
- **Hot reload waits.** The editor doesn't reload game code while a request is waiting.

## Session policies

`PlaySession` (`GameFramework/PlaySession.h`) now has two policies, set before
`start()`:

| Policy | Values | Default |
| --- | --- | --- |
| `SessionWorldPolicy` | `RestoreAuthoredSnapshot`: copy the world at start and put it back at Stop. `OwnedRuntimeWorld`: the world is the runtime's and is never copied or restored. | `RestoreAuthoredSnapshot` |
| `SessionSettingsPolicy` | `RestoreOnStop`: put graphics back at Stop. `PreserveUserSettings`: keep them. | `RestoreOnStop` |

The defaults are the old behaviour, so tools, tests and game code that make their own
`PlaySession` work as before. `LevelHost` always uses `OwnedRuntimeWorld` and
`PreserveUserSettings`. A level change therefore never copies a whole world.

## In the player

The player's world is the runtime world. Its level comes from the catalog: the entry
level, `--level`, or the level whose scene is the startup scene. A project without a
catalog plays its implicit `startup` level, and travel then has nowhere to go.

If a level fails to start, the player goes back to the entry level once. If that fails
too, the player exits with code 1. Online play (`--host`, joins, `--net-preview`) runs
through the same host. A client that joins a match starts its level with `OnlineBaseline`.

## In the editor

Play moves the scene you're editing aside and plays a copy, one copy per Play. Every panel
shows the running game: the viewport, Hierarchy, Inspector, picking and gizmos.

When the game travels:

- the copy is replaced by the next level;
- the selection is cleared, since nothing selected is in the new level;
- the editor's effect and animation previews restart.

Stop puts the authored scene back exactly, with its selection. The undo history never
sees Play, because edits during Play aren't recorded. The editor camera is the same.

`editor --level-cycles N` is a self-test. It presses Play, travels to the next level and
presses Stop, N times. Then it checks that the authored scene (as text), the undo
history, the selection and the camera are unchanged, and exits 1 if they aren't.

## Who owns what

The verification evidence asks for this table. "Lives" says how long each object lasts.
Objects are destroyed in reverse order of the table, newest first. Background producers
stop before the objects that read from them.

| Object | Owner | Lives | Notes |
| --- | --- | --- | --- |
| Game module (code) | Player / Editor `GameModuleLibrary` | the run (editor: until hot reload) | unloaded only after the GameInstance, sessions and every world holding its components are gone |
| GameInstance | Player / Editor | the run | makes each level's GameMode |
| Runtime `World` | Player `m_world`; editor `m_world` during Play | the run; contents change per level | never moves while a session refers to it; each level gets a new `levelEpoch` |
| Authored `World` (editor) | `Editor::m_authoredDuringPlay` | one Play | the scene being edited, put back by Stop |
| Undo / redo, selection, editor camera | Editor | the run | untouched by Play and travel; selection saved at Play, cleared on travel, restored at Stop |
| `AssetResidency`, `AssetGraph` | Player `m_residency`, `m_assetGraph`; editor the same | the run | declared before the host, so they outlive its lease ([AssetResidency.md](AssetResidency.md)) |
| `LevelHost` | Player `m_levels`; editor `m_runtimeLevels` | the run (editor: per game module) | owns the session; stops it before the module unloads |
| Level asset lease | `LevelHost` | one level | the next level's is taken before the old one goes |
| `PlaySession` + `GameContext` | `LevelHost` | one level | `unique_ptr`, never moved |
| `GameMode` | `PlaySession` | one level | destroyed only from `process()` or Stop, never on its own call stack |
| `PhysicsWorld` | session subsystems | one level | added first, destroyed last; rebuilt when a save loads |
| `AudioWorld` | session subsystems | one level | its raycast looks physics up per ray; voices stop with it |
| `NavigationWorld` | session subsystems | one level | the scene's saved navmesh is loaded by the load hook, or baked |
| `EffectsWorld` | session subsystems | one level | draws through the application's `Effects`, which outlives it |
| `UIWorld` | session subsystems | one level | documents through the application's `GameUI`, which outlives it |
| `AnimationWorld` | session subsystems | one level | |
| `SaveGames` | session subsystems | one level | its loader jobs are waited for when it's destroyed, so a stale load can't reach the next level |
| `NetworkWorld` | session subsystems | one level | closed first at Stop; the application's online services and match outlive it |
| `CircuitWorld` | session subsystems | one level | added last, destroyed first |
| Renderer, history buffers | Engine | the run | history is keyed by `World::lifetime()`, which is new for each level |
| Baked lighting, reflection probes (player) | Player | the run | read from the world's components each frame, so they follow the level |
| Nav overlay, `--camera` entity (player) | Player | the run | the tool camera is dropped once another level is in |
| Editor previews: effects, animation, navmesh | Editor | the run | restarted at Play and on travel |
| Online services, `OnlineMatch`, Steam | Player / Editor | the run | pumped every frame, during loading too; shut down after the last session |

## Code and tests

- `GameFramework/LevelLifetime.h`: the policies, `StartReason`, `LevelRequest` and
  `TravelResult`.
- `GameFramework/LevelHost.h`: the host.
- `GameFramework/PlaySession.h`: the policies on a session.
- `Engine/World.h` (`levelEpoch`) and `Engine/EntityRef.h`: refs from old levels.
- Tests:
  - `level_lifetime` (unit): lifecycles counted exactly, requests from each callback,
    failures, stale refs and callbacks, cycles, and 100 editor-style cycles;
  - `level_lifetime_editor` (gpu): the real editor's 100 cycles on
    `Tests/fixtures/levels`.
