# Level lifetime: runtime levels and travel

A game moves between levels: menu, then level A, then B, then back to the menu. Each move
has to end one level completely before the next one starts. Its GameMode, physics,
sounds and callbacks all go, and nothing of the old level may leak into the new one. In
the editor, a level change during Play must never touch the scene you're editing.

S03 of [the shipping plan](ShippingImplementation.md) added the lifetime rules and a
working move between levels. S04 added asset leases and loading on demand
([AssetResidency.md](AssetResidency.md)). S05's first part, described under
[Travel tickets](#travel-tickets-game-flow), turns travel into tickets with phases,
progress, cancel and stable error codes. It also adds arrival at a spawn, level exits,
circuit nodes and a clean start for input and the camera. The application's own menu,
pause, loading and error screens and `--flow-test` come in S05's second part. The level
catalog itself is [Levels.md](Levels.md).

## Asking for another level

From any GameMode callback, from a UI handler during Play, or from a circuit
(`Level.RequestTravel`).

```cpp
void fixedUpdate(GameContext& game, double) override {
    if (reachedTheDoor) game.requestLevel("b");             // B's default spawn
    if (tookTheLift) game.requestLevel("b", "rooftop");     // a LevelSpawn in B
}
```

The spawn travels with the request. An empty one is the catalog's `default_spawn`, or
the scene's `start` spawn. The new level sees where it was entered as `game.arrival`
(the spawn's id and world transform, `found` when there is one), and its GameMode puts the
player there in `beginPlay`. The move doesn't happen inside the call. The level keeps running to the end of the frame,
then ends, and the next one starts. So it's safe from `fixedUpdate`, `frame`, a button's
handler, or even `beginPlay`.

`requestLevel` returns false when the request is refused. `game.flow().request` says why
(below).

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

With `asset_loading levels`, the new level's assets aren't in memory until its lease
loads them, so the scene is read after the swap instead: plan, end, swap (the old level's
own assets unload), load the new assets, read the scene, start. A failure there leaves
nothing playing, like a level that didn't start ([AssetResidency.md](AssetResidency.md#loading-on-demand)).

Every accepted request ends exactly once, through `onResult`:

| Result | When |
| --- | --- |
| `Succeeded` | the new level is playing |
| `Failed` | it didn't load (the current level plays on), it didn't start (nothing is playing; the player goes back to the entry level, the editor stops Play), or it's a redirect cycle |
| `Cancelled` | it was cancelled before the current level ended, or Play stopped first, even while it loaded |

## Travel tickets (game flow)

`game.flow()` (`GameFramework/GameFlow.h`) is the whole interface. `LevelHost` implements
it in the player, the editor and tests. Each request gets a ticket:

```cpp
const shockwave::TravelTicket t = game.flow().request("factory", "entrance");
if (!t) showError(shockwave::travelErrorName(t.error), t.message);   // Busy, UnknownLevel, ...
...
if (auto s = game.flow().status(t.id)) loadingBar.set(s->progress);
game.flow().cancel(t.id);   // until the current level starts ending
game.flow().restartLevel();  // this level again, at the spawn it was entered at
game.flow().returnToMenu();  // the catalog's first menu level
game.flow().useExit(door);   // a LevelExit: its destination and spawn
```

**One travel at a time.** Another request while a ticket is active is refused as `Busy`,
and the active ticket carries on. So a button pressed twice, or two exits touched in the
same frame, travel once. A level asking for another from its `beginPlay` (a redirect) is
the exception, as before.

**Phases.** A ticket goes `queued`, `validating`, `reading`, `preparing`,
`awaiting_boundary`, `retiring`, `uploading`, `starting`, then ends `succeeded`, `failed`
or `cancelled`. Its progress uses the plan's weights: validate .05, read .15, prepare .25,
retire .10, upload .25, start .20. It never goes down and is 1 only once it succeeded.
The real milliseconds per phase are in the load report.

**Staged travel** (`level_travel staged`, the default). The change takes two frames. The
first `process()` validates, reads and prepares the next level, then pauses the current
one at `awaiting_boundary`. A frame renders, so a loading screen can show and the ticket
can still be cancelled. The next `process()` retires the old level and starts the new
one. `level_travel instant` in the project, or `player --instant-travel`, does it all in
one frame, as in S03.

**Cancel.** Until `retiring`, `cancel(ticket)` drops the prepared level. The current level
plays on, unpaused, untouched, with the same epoch. After that it returns
`TooLateToCancel` and the travel carries on. An ended ticket gives `AlreadyFinished`,
and an unknown one `UnknownTicket`. A cancel never touches another ticket.

**Codes.** Refused requests and ended tickets carry a stable code:

| Code | When |
| --- | --- |
| `Busy` | another travel is active |
| `NotPlaying` | nothing is playing, or a context without a flow (tools, tests) |
| `StaleLevel` | asked from a level that has ended (a kept `game.flow()` or callback) |
| `UnknownLevel` | not in the level catalog (refused before a ticket) |
| `UnknownSpawn` | the destination has no LevelSpawn with that id (fails before the current level ends) |
| `InvalidRequest` | no level named, `resetCampaign` outside `NewGame`, a disabled or used exit, no menu level |
| `AuthorityRequired` | during network play: map travel is the host's, and comes with S11 |
| `LoadFailed`, `BudgetExceeded` | the scene or its closure didn't load or fit (before the boundary: the level plays on) |
| `StartFailed` | the new level didn't start: nothing plays |
| `Cancelled`, `RedirectCycle` | cancelled or stopped; levels kept redirecting |

**Completions** are delivered once per ticket, after the change has finished, never in the
middle of one. They go to `LevelHost::onResult`, to `subscribe()` observers (the token
ends the subscription, and is safe to outlive the host), and to `Level.OnTravelFinished`
in the circuits of the level playing then. That's the new level after a success, or the
same level after a failure or cancel before the boundary. A level that has ended never
hears about the next one. The last 64 ended tickets stay queryable with `status()`.

**Exits.** `useExit(entity)` reads the entity's `LevelExit`. A disabled one is refused. A
one-shot exit is disabled once its request is accepted, so a second contact doesn't
travel again. What touches the exit (a trigger, a key) is the game's.

**Arriving clean.** Once the new level has started, the engine counts every input action
held through the change as released until it is let go (`Input::holdUntilReleased`), so
a held fire or jump doesn't fire in the new level. The camera's temporal history is cut,
and the time spent loading isn't turned into a burst of ticks
(`Engine::discardFrameTime`). Previous transforms are already the new level's own, since
it starts from its own world.

**Circuits.** `Level.RequestTravel` (level, spawn), `Level.RestartLevel`,
`Level.ReturnToMenu` and `Level.UseExit` have `accepted` and `refused` outputs, with the
ticket and code. `Level.TravelStatus` reads a ticket, and `Level.OnTravelFinished` says how
it ended ([CircuitNodes.md](CircuitNodes.md)). `player --check` fails a
`Level.RequestTravel` whose literal level isn't in the catalog (`UnknownLevel`) or whose
literal spawn isn't in that level (`UnknownSpawn`).

## The rules it keeps

- **One level simulates.** Preparing never calls `beginPlay` or steps physics.
- **Nothing from an old level reaches the new one.**
  - An `EntityRef` made in level A stops resolving once B is in, even when B has an
    entity with the same handle. `World::levelEpoch()` tells them apart.
  - A `game.flow()` kept from A is refused (`StaleLevel`).
  - A save game loaded in place keeps the same epoch, so its refs stay valid.
- **No loading forever.** Levels that ask for another before they've ticked are
  redirects. More than 8 in a row fail as a cycle, and the message lists the chain.
- **Epochs never wrap.** Running out of epochs (after 2^64 levels) fails explicitly.
- **Settings survive travel.** A level's session keeps the player's graphics settings
  (`PreserveUserSettings`). The application's own settings are put back once, when
  Play stops.
- **Hot reload waits.** The editor doesn't reload game code while a request is waiting
  or prepared at the boundary.
- **Online levels don't change.** During network play a request is refused as
  `AuthorityRequired` instead of silently disconnecting the peers. Map rotation is S11.

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
- `GameFramework/GameFlow.h`: tickets, phases, codes, `LevelFlow` (`game.flow()`).
- `GameFramework/LevelHost.h`: the host, which is the GameFlow.
- `Engine/Levels.h`: `levelArrival`, `findLevelSpawn`.
- `Input/Input.h`: `holdUntilReleased`.
- `GameFramework/PlaySession.h`: the policies on a session.
- `Engine/World.h` (`levelEpoch`) and `Engine/EntityRef.h`: refs from old levels.
- Tests:
  - `level_lifetime` (unit): lifecycles counted exactly, requests from each callback,
    failures, stale refs and callbacks, cycles, and 100 editor-style cycles;
  - `level_lifetime_editor` (gpu): the real editor's 100 cycles on
    `Tests/fixtures/levels`;
  - `level_travel` (unit): staged phases and progress, A to B to A, spawns, restart and
    menu, every refusal code, cancel before and after the boundary, a user's pause kept,
    two exits in one frame, failures before and after the boundary, Stop while queued,
    reading and at the boundary, completions exactly once, bounded history;
  - `game_flow` (unit): a game circuit using every `Level.*` node, completions reaching
    only the level playing afterwards, and stale and missing flows refusing;
  - `input` (unit): a key held through a level change reads as up until let go;
  - `level_check_cli` (integration): `--check` on circuit travel literals.
