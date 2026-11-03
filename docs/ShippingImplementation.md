# Shipping a complete game: implementation specification

Status: **implementation plan, not a claim that these features exist**.
Baseline inspected: `186ffc3`, 2026-10-27. Re-audit changed files before implementation.
Audience: an implementer who must be able to execute each batch without inventing a
second engine architecture. Normative words MUST, MUST NOT and SHOULD describe the
target implementation. Proposed names below are contracts to add, not existing APIs.

Open through **Help > Shipping implementation guide**. The shared documentation window
opens this Markdown in your associated application or copies its path. Command-line
access (implemented with this document):

```sh
build/bin/editor --shipping-guide
build/bin/editor --shipping-guide-path
```

The second command prints the path and exits without starting the engine. Rebuilding
`editor` refreshes its bundled documentation for relocated builds. All other new runtime
commands in this plan remain proposed until their owning batch is implemented.

## 0. Outcome and how to use this document

The outcome is a developer shipping a complete bounded-level game with Shockwave:

```text
launch installed game
  -> main menu -> new game -> loading -> level A
  -> objective complete -> results -> loading -> level B
  -> checkpoint -> quit -> relaunch -> continue in level B
  -> death -> retry checkpoint -> final objective -> ending/credits
  -> main menu -> new game or quit

multiplayer: menu -> lobby -> map A -> results -> map B -> leave -> menu
```

Multiple levels means genuinely unloading and loading different scenes, assets and
session services. Moving the camera between two rooms in one giant scene does not pass.
Restarting the executable between levels does not pass. Keeping every level permanently
resident does not pass. A responsive loading screen between bounded levels is acceptable;
seamless open-world streaming is outside this release's requirements.

The first release proves an offline complete game and, separately, the advertised online
mode. The target range is 1v1 through 32v32, but ship only player counts actually verified
in the integrated game. Engine completeness does not supply a finished game's art,
level design, balancing, narrative, localization text or store assets automatically.

Read in this order:

1. [Architecture](Architecture.md), [UI](UI.md), and the repository AGENTS instructions.
2. Sections 1–7 here: evidence, ownership, invariants and level-travel contracts.
3. Batches S00–S15 in order; respect the dependency table.
4. Section 25's packaged-game acceptance sequence. Use it to decide whether the work
   is complete, not the number of systems or lines of code added.

Keep [Graphics implementation](Graphics/README.md) and
[Steam multiplayer implementation](SteamMultiplayerImplementation.md) as the detailed
specialist specifications. Do not copy their algorithms into a competing implementation.
This document supplies the integration and release gates between those systems.

### 0.1 Execution discipline for the implementing model

For each batch:

1. Read its named source files and current tests. Record what already exists.
2. Implement the smallest complete vertical slice, including runtime, editor, text
   format, validation, CLI, migration and tests required by that batch.
3. Format changed C++ with the repository clang-format configuration. Build affected
   targets. Run the specified tests and related existing tests.
4. Regenerate schemas from registered metadata. Never hand-maintain a second field list.
5. Record commands, build revision, platform, results and limitations in the proposed
   `docs/ShippingVerification.md`. A missing test is **unverified**, never a pass.
6. Inspect UI on a real display. Capture screenshots for normal and narrow layouts.
7. Commit a coherent batch if the task authorizes commits. Do not silently push releases.

Do not mark a batch complete with stubbed success returns, hardcoded sample names,
disabled assertions, ignored warnings, sleeps pretending to be work, or a mock standing
in for a required installed-game/hardware test. No empty implementation placeholders
may remain on a release path. A feature intentionally deferred must be absent from the
advertised release contract and have a usable fallback.

## 1. What already exists, and what this audit actually established

This is a source inspection, not a fresh full-platform certification. Paths below are
relative to the repository root. Reuse these implementations.

| Area | Evidence in this checkout | Consequence for this plan |
|---|---|---|
| Project isolation | `Projects/Project.h`, `ProjectTemplate.cpp`, `Templates/Blank`, [Starting projects](StartingProjects.md) | Extend the project manifest/template; game logic stays outside engine modules. |
| Scene serialization | `Engine/SceneFile.h`, `SceneFile.cpp`; component metadata, prefabs | Keep `.swscene` as a readable scene description, referencing Content assets. It is not an asset archive. |
| Lifetime | `GameInstance.h`, `GameMode.h`, `PlaySession.h/.cpp` | GameInstance outlives scene sessions; PlaySession owns session subsystems and restores its snapshot on Stop. |
| Initial loading | `Programs/Player/Private/Player.cpp` | Startup loads a scene and constructs a PlaySession; no general game-facing level-travel service in the inspected public API. |
| Online scene replacement | `Programs/Player/Private/PlayerNetwork.cpp`, `Player::playScene` | Replacement exists for matches; it stops the old session before loading. Consolidate this into the new coordinator; do not add a third replacement path. |
| Saves | `GameFramework/SaveGame.h/.cpp`, `PlaySession::applyLoadedSave`, [Save games](SaveGames.md) | Background read/write and world snapshots exist. Cross-level loading, durable identity and release compatibility need stronger contracts. |
| Save paths | `Project::statePath`, `Player::saveFolder`, editor `saveGamesFolder` | Currently project-relative `Saved`; installed read-only games need per-user runtime paths. |
| Save restore lifetime | `PlaySession::applyLoadedSave` | Rebuilds physics; UI/audio keep running. Audit and reset every session subsystem for cross-level restores rather than reusing this as travel. |
| Asset loading | `Assets/AssetFolder.h`: `open` imports everything; `Assets.h` registry | Add dependency-driven residency and release ownership. Folder-wide import is insufficient for many large levels. |
| Menus/HUD | `GameUI/UIWorld.h`, `GameUI`, [Game UI](GameUI.md) | Reuse RmlUi documents, handlers, focus/navigation and bindings; do not build shipping menus from editor ImGui. |
| Gameplay services | Audio, Physics, Animation, Navigation, Effects, Circuits | Already exist; prove their reset, cancellation and lifetime across scene changes. |
| Packaging | `Packaging/Private/Packager.cpp`, `PackageSettings.cpp`, `ChildProcess.cpp` | Build/check/copy/verify/archive, dependencies, licenses and moved-package testing exist. Extend rather than replace. |
| Platform evidence | `.github/workflows/build.yml` | Linux, Windows and macOS jobs exist; some GPU/timing jobs are nonblocking. That is not proof of a playable release on each OS. |
| Windows packaging | `_WIN32` branches in Packager, direct `Binaries/<game>.exe` launcher | Some packaging docs lag implementation. Verify the port; do not call Windows packaging wholly absent or complete from docs alone. |
| Multiplayer | `NetworkingVerification.md` batches through 10 | 12-player simulated soaks and fault tests exist. Real Steam, cross-OS and desktop timing gates remain listed as outstanding. |
| Graphics | `Graphics.md`, `GraphicsImplementation.md`, `Graphics/` | Existing rendering plus a large future roadmap. Planned graphics chapters are not evidence of implemented effects. |

Public paths in the table abbreviate `Source/Runtime/` unless explicitly under Programs.
The baseline includes a modified Arena network config and untracked local test/Steam
files. Preserve local changes and do not package or commit developer AppID files.

### 1.1 Immediate architectural hazards to resolve

- A `GameContext` holds a `World&`. Replacing a World while a Schedule, callback,
  audio snapshot or UI handler still references it can invalidate live state.
- A `PlaySession` Stop restores its original World and graphics snapshot. Repeated
  runtime travel must not accidentally restore the editor document or undo a player's
  newly saved graphics preferences.
- EntityRef contains a World pointer and entity handle. Numeric entity IDs can repeat
  in a new level; ID equality is not cross-level identity.
- A loading UI owned only by the departing UIWorld disappears exactly when needed.
- Folder-wide imports and retained assets can make every successive level add memory.
- Renaming a temporary save file is useful, but power-loss durability and concurrent
  writers require explicit platform semantics; do not promise more than tested.
- A passed codec benchmark is not proof of a full 64-player game at its advertised
  frame rate, bandwidth, memory limit or Steam relay behavior.

## 2. Release scope and priority

S00 creates a versioned release contract. Proposed default: Windows x64 and Linux x64
desktop clients, Vulkan, keyboard/mouse and controller, bounded maps, an offline mode,
and an optional Steam online mode. macOS is supported only after its own release gates
pass. Jetson Orin Nano Super is an 8 GiB shared-memory stress target, not automatically
an advertised ARM deployment target. A working platform build must not be removed just
because it is outside a particular game's support matrix.

Release-critical:

- Complete game flow and multiple levels; deterministic ownership and cleanup.
- Saves/checkpoints/profiles/settings that survive reinstall/update outside the install.
- Responsive loading with measured memory limits, cooked dependencies and cancellation.
- Menus, rebinding, readable UI, audio/device/focus recovery, useful errors.
- Enough gameplay authoring and diagnostics to build and finish the chosen game.
- Relocatable packages, update compatibility, crash diagnosis and tested target machines.
- For an online product: actual Steam sessions, map rotation, disconnection recovery,
  authority, bounded resources and supported player-count evidence.

Not prerequisites by themselves: open worlds, fluid simulation, procedural worlds,
ray tracing, strand hair, cinematic virtualized geometry, seamless travel, host migration,
mod workshop integration, or every optional effect in the graphics roadmap. A game can
ship without them. Missing content or broken basic flow cannot be excused by adding them.

## 3. Ownership and module boundaries

Extend existing modules. Do not create an all-purpose Shipping singleton.

| Responsibility | Owner and proposed public surface |
|---|---|
| Stable application ID, level catalog, release settings parsing | Projects: `LevelCatalog.h`, `ReleaseSettings.h`; only value data and validation |
| OS writable paths and subprocess execution | Platform: `UserPaths.h`, `Process.h`; platform-specific internals |
| Asset dependency graph, prepared content and lifetime leases | Assets: `AssetDependencies.h`, `AssetResidency.h` |
| Scene parsing and world-local epoch/identity contracts | Engine: extend SceneFile, World, EntityRef; new scene staging helpers as needed |
| GPU upload completion and deferred destruction | Renderer public resource API; RHI/Vulkan implementation stays in its plugin |
| Runtime travel, persistent progress, game-flow ownership | GameFramework: `GameFlow.h`, `LevelTravel.h`, `CampaignState.h` |
| Loading/menu documents outside a level | GameUI: application UI layer separate from UIWorld bindings |
| Network map-transition protocol | Network message definitions; Multiplayer state; GameFramework orchestration |
| Cook/package report and installed-game verification | Packaging, with tooling orchestration in Player |
| Authoring | Editor panels; no runtime dependency on Editor or ImGui |
| Objectives, guns, inventory, scoring, story and ending | Independent game module and its Content/Config/Scenes |

GameFramework may depend on Projects explicitly if needed; declare that dependency in
its CMakeLists, respecting `Source/Runtime/CMakeLists.txt` order. Projects must not depend
on GameFramework or Engine. Circuits must not include GameFramework (it precedes it).
For circuit travel nodes, inject an Engine-owned narrow command sink implemented by
GameFramework, or register the nodes in GameFramework through the existing node registry.
Choose one and document it; do not break the module DAG. Inspect node registration
initialization before using the latter approach.

## 4. Lifetime model and invariants

```text
Application / Engine
  GameInstance + GameFlow + UserPaths + application UI + online services
    CampaignState / profile / selected settings (value data)
    one active RuntimeLevel { World, PlaySession, AssetLease, LevelId, epoch }
    at most one PreparedLevel (CPU metadata + budgeted staged resources)
    editor only: separate authored World, undo/selection/editor camera
```

RuntimeLevel storage must have a stable address while PlaySession or EntityRef refers
to its World. Store it through `unique_ptr`; never move its World after creating the
session. Existing Player/Editor references to `m_world` must be audited and routed to
the active runtime world during Play, or explicitly kept on the authored world while
editing. Merely adding a second World without redirecting render/picking/UI is wrong.

The coordinator processes commands after the current frame's callbacks and simulation
work have returned. It never destroys a GameMode on that GameMode's call stack.

Required invariants:

1. Exactly one level can simulate. Staging never calls beginPlay or advances physics.
2. Every accepted travel request has exactly one terminal result: Succeeded, Failed or
   Cancelled. No accepted request disappears on Stop, quit, timeout or replacement.
3. No event, worker result, entity reference or network packet from an old epoch can
   mutate the new level. Epochs are monotonic 64-bit values; overflow fails explicitly.
4. No per-level lease outlives its last CPU/audio/GPU consumer. Release uses actual GPU
   completion, not an assumed number of frames.
5. UI updates, world publication and session lifecycle hooks run on the main thread.
6. Failed preparation preserves the active level. Once its teardown begins, recovery
   goes to a known safe menu; do not claim arbitrary gameplay callbacks can be rolled back.
7. Runtime travel never changes authored scenes, editor undo history or editor selection.
8. A stale async save/load/import completion cannot publish into a different session.
9. Network sockets/callbacks continue being serviced during loading. Local pause never
   pauses an online authority. A disconnected client cannot become authority by fallback.
10. Loading progress and errors are observable from both UI and machine-readable reports.

### 4.1 Entity references and persistent state

Keep World-local entity handles for fast ECS access. Add a runtime world lifetime token
and epoch check for externally retained EntityRefs, or an equivalent checked handle
whose validation does not dereference freed World memory. A weak lifetime token can
guard a raw World pointer; first lock the token, then check epoch/alive state on the
main thread. This is not a general concurrent World access API.

Persistent gameplay data uses stable IDs, not pointers, entity slot numbers or names:

```cpp
// Proposed types, not existing headers.
struct LevelId { std::string value; };           // catalog key, e.g. "factory"
struct PersistentId { std::array<std::byte, 16> value; }; // authored stable UUID
struct TravelTicket { uint64_t value; };
struct LevelEpoch { uint64_t value; };
```

Entity persistent IDs are unique per level. Prefab instances combine a stable instance
ID with stable prefab-local IDs; cloning an instance assigns a new instance ID. An
import does not regenerate already stored IDs. A cross-level reference is
`(LevelId, PersistentId)` and is unresolved while that level is unloaded. Never load a
level merely to dereference it. Names remain user-facing labels, not identity.

CampaignState stores schema-versioned game values such as unlocked levels, inventory
item IDs, objective completion and selected loadout. It cannot contain EntityRef,
GPU handles, raw pointers, Jolt bodies or std::function callbacks. The game owns those
types/registrations; the engine owns versioning, persistence and lifecycle dispatch.

## 5. Level catalog and authoring contracts

Proposed optional `levels "Config/Levels.swlevels"` in `.swproject`. Existing projects
without it remain valid and expose their startup scene as a single implicit `startup`
level. Do not write a catalog to disk merely by opening an old project.

Proposed format version 1 (implement parser and schema before shipping examples):

```text
shockwave_levels 1
entry "menu"
level "menu"
  scene "Scenes/menu.swscene"
  kind menu
end
level "training"
  scene "Scenes/training.swscene"
  kind gameplay
  default_spawn "start"
  next "factory"
  preload "Content/UI/loading.rml"
end
level "factory"
  scene "Scenes/factory.swscene"
  kind gameplay
  default_spawn "entrance"
  next "ending"
end
level "ending"
  scene "Scenes/ending.swscene"
  kind ending
end
```

Rules: IDs `[a-z][a-z0-9_-]{0,63}`, unique; one entry; scene paths relative to project,
normalized and contained within it; only `menu|gameplay|ending` kinds in v1; `next`
optional and names another catalog entry; preload paths project-relative, unlike
component asset names, which stay Content-relative. Duplicate keys, unknown version,
duplicate IDs and escaping symlinks are errors. Quoted strings follow existing project
escaping conventions. Unknown keys are reported and fail strict release validation.

`next` is a default destination, **not automatic travel on load**. Explicit game logic
requests progression after victory. Cycles are valid for replay/map rotation; report
unreachable entries as warnings, with an explicit allowed alternate-entry annotation
in release settings. A menu need not have a gameplay camera when rendered using only UI.

Proposed registered components:

| Component | Fields and defaults | Validation |
|---|---|---|
| `LevelSpawn` | `id="start"`, `team=-1` | nonempty ID unique in scene; Transform present; team optional game filter |
| `LevelExit` | `destination=""`, `spawn=""`, `enabled=true`, `one_shot=true` | destination in catalog; spawn exists there; trigger behavior via physics or circuit, no always-on teleport |
| `Checkpoint` | `id=""`, `spawn=""`, `enabled=true` | unique ID; local spawn exists; offline only unless game explicitly handles authority |
| `PersistentIdentity` | `id=""` | valid UUID, unique effective prefab identity |

Use registered component metadata for inspector fields. Level/spawn pickers should
provide reference-aware choices but save stable text IDs. The Levels panel offers Add,
Rename ID, Set entry, Open scene, Play level, Validate, and graph/list of exits. Rename
is a previewable cross-file edit, updates catalog/references and undo, and never rewrites
save files silently. An old-to-new ID migration belongs in the game's migration table.

Schema: proposed `player --level-schema <path|->`; report:
`player --project <file> --levels --json <out>`; strict `--check` visits every catalog
scene and referenced prefab/circuit, not only the startup scene. `--level <id>` chooses
a startup override. Reject combining it with `--scene`, incompatible online options,
or a cross-level `--load`; show precise diagnostics instead of guessing precedence.
Existing `--scene` remains a developer tool and does not silently join release catalogs.

## 6. Travel API and state machine

Proposed `game.flow()` facade on GameContext, backed by GameInstance's coordinator.
Standalone tests can inject a fake coordinator; no global current game pointer.

```cpp
enum class TravelReason { NewGame, Exit, Restart, Checkpoint, LoadSave, Menu, OnlineMap };
enum class TravelPhase {
    Queued, Validating, Reading, Preparing, AwaitingBoundary, Retiring,
    Uploading, Starting, Succeeded, Failed, Cancelled
};
struct TravelRequest {
    LevelId destination;
    std::string spawn;              // empty: catalog default
    TravelReason reason;
    bool resetCampaign = false;     // legal only for NewGame after UI confirmation
};
struct TravelStatus {
    TravelTicket ticket;
    TravelPhase phase;
    float progress;                // finite [0,1], monotonic within one ticket
    std::string code, message;
    bool cancellable;
};
// Contract sketches: use the repository's result conventions for the actual signatures.
// request(request) -> Result<TravelTicket, TravelError>
// cancel(ticket) -> Result<void, TravelError>
// status(ticket) -> optional<TravelStatus>
// restartLevel(), returnToMenu(), continueSave(slot) -> Result<TravelTicket, TravelError>
```

Main-thread API only. Systems submit a small typed command to a bounded main-thread
queue, through the Schedule's command mechanism if available. Do not assert that all
game APIs are thread safe. Capacity 32 travel commands; repeated requests while a ticket
is active return `Busy`, without creating another ticket. Duplicate LevelExit contacts
must be latched. `cancel` for a completed/unknown ticket returns a typed result and does
not cancel a later ticket. Quit and editor Stop cancel pending work and join/drain it.

Bounded history: retain the last 64 terminal statuses for diagnostics; a query for an
expired ticket returns unavailable. Observers unsubscribe via lifetime-owned tokens.
Notifications are queued until after lifecycle operations so listeners cannot reenter
the coordinator during commit. Completion events carry ticket, old/new LevelId, epoch,
duration and result. Do not capture a retiring GameMode in a completion callback.

### 6.1 Preparation and commit algorithm

Implement a CPU-first, memory-bounded, blocking-at-publication v1. Async is an ownership
property, not permission to call existing renderer/world APIs from arbitrary workers.

1. Validate request on main thread; refuse client-authored online travel. Resolve all
   paths, scene/spawn IDs, installed content and transition permissions.
2. Retain the old level. Show application-owned loading overlay; suppress gameplay
   input and pause offline simulation. Continue UI/event/audio/network pumps.
3. Worker jobs read files and decode immutable scene/content descriptions. If current
   SceneFile parsing or registered component callbacks are not thread safe, split
   syntax/data parsing from World construction. Until split, construct in measured
   bounded main-thread slices; do not move `loadScene` to a worker blindly.
4. Build transitive dependency closure, validate formats and hashes, reserve CPU/GPU
   budgets. Resolve prefab dependencies and navigation sidecars. No beginPlay yet.
5. On failure/cancel before Retiring: release staged resources, hide overlay, restore
   prior pause/input state and keep old World, mode and progress. Notify once.
6. At a frame boundary: stop dispatching level input/events, finish Schedule jobs and
   freeze persistent transfer data. All active callbacks must have returned.
7. Mark Retiring, which is the no-rollback boundary. Call old endPlay once, detach UI
   handlers/circuits, stop level audio/effects, close or detach its network session as
   defined in S11, then destroy session services before destroying their World.
8. Release old asset lease after CPU consumers are gone; GPU resources enter deferred
   retirement. Wait/pump until fences complete if memory is needed; do not allocate
   the new GPU working set on the assumption that deferred frees already happened.
9. Materialize destination World, acquire its asset lease, upload within budget and
   wait for required resources. Optional high mips may continue later; required collision,
   UI/fonts and gameplay assets cannot be placeholders. Headless path skips GPU work.
10. Create its PlaySession, configure scene/circuit/save/network settings, hydrate
    campaign/player transfer into registered data, build services and start once.
11. For a save restore, apply restored data before gameplay begins; select the correct
    GameMode for the destination. Separate authored beginPlay from restore hooks to
    avoid respawning saved enemies/loot. See S06's ordering.
12. Set epoch, LevelId, input context, previous/current transforms equal, interpolation
    history, camera and renderer temporal-reset notification. Do not interpolate from
    the previous level. Clear latched fire/jump actions and elapsed loading time debt.
13. Publish new active level only when initialization succeeds. Hide loading overlay,
    enable input, unpause, and emit Succeeded. Never tick a partly initialized session.

An exception in endPlay must not skip subsystem cleanup. A start failure after Retiring
must destroy the failed candidate and show a built-in application error/menu document
with Retry / Main menu / Quit. The minimal fallback is independent of project assets;
it must remain available when the menu scene itself is corrupt. Do not reconstruct the
old live World from a stale snapshot and claim player progress was preserved.

### 6.2 Memory policy and honest progress

Default policy is unload-before-GPU-upload, not double-buffering whole maps. Let:

```text
peak CPU = application + old level CPU + bounded prepared CPU + decode scratch
peak GPU during retirement = application GPU + old live GPU + pending frees
peak GPU during activation = application GPU + new level GPU + upload scratch
```

For shared-memory devices, account for physical backing once where measurable, plus
driver/system reserve; do not simply add two accounting views of the same allocation.
Never treat all advertised 8 GiB as allocatable game memory. S00 defines the reserve and
measures peak process/system memory on the actual Orin. If required allocations exceed
the budget, report `BudgetExceeded` with resource totals before teardown when knowable.

Keep staged decoded data bounded by a configurable byte budget. Spill deterministic
prepared content to cache or decode it after retirement; do not retain every compressed
and uncompressed version. A later optional overlap/preload mode must reserve the combined
working set before allocation. It is not required for release.

Progress is phase-weighted: Validate .05, Read .15, Prepare .25, Retire .10, Upload .25,
Start .20. Within a phase use completed/total bytes or items; if unknown show a phase
label and indeterminate indicator. Clamp numeric progress and never show 100% before
Succeeded. Weights are a UI estimate, not a time guarantee. Record actual per-phase
milliseconds separately. Cancel is enabled only before Retiring; Quit remains available.

### 6.3 Why the boundaries are necessary

The safety argument relies on no worker retaining a published mutable World and on
waiting for all old consumers: if all old work has terminated before destruction and
every later completion checks the request epoch, no accepted completion can mutate a
retired World. Removing either precondition invalidates the argument. Tests and sanitizers
must verify the implementation; prose is not a proof that C++ code is race free.

The memory bound follows from admitting at most one prepared candidate with bounded
scratch and refusing new allocations until retired resources complete. Counting a
resource as freed when merely queued for deletion violates the bound.

## 7. Batch dependency and completion table

| Batch | Deliverable | Prerequisites |
|---|---|---|
| S00 | release contract and evidence baseline | none |
| S01 | per-user runtime paths and settings identity | S00 |
| S02 | level catalog and authoring | S00 |
| S03 | runtime/editor lifetime separation | S02 |
| S04 | dependency residency and bounded loading | S03 |
| S05 | playable level travel and full shell | S01–S04 |
| S06 | cross-level saves/checkpoints and migrations | S05 |
| S07 | complete reference game and reusable authoring | S06 |
| S08 | player settings, accessibility and localization | S05 |
| S09 | cooking, dependency closure and release packages | S04, S07, S08 |
| S10 | hardware performance and graphics release tier | S07, S09 |
| S11 | multiplayer map lifecycle and release qualification | S05, S09; existing networking batches |
| S12 | crash diagnostics, recovery and external process hardening | S01, S05, S09 |
| S13 | platform/install/update compatibility | S09–S12, S11 for online releases |
| S14 | CI and automated shipped-game acceptance | test infrastructure starts S00; final gates after S13 |
| S15 | documentation, release rehearsal and final sign-off | all required batches |

Do not delay writing tests until S14. Every preceding batch carries its own tests.
Offline first-release acceptance need not wait for optional online qualification, but
the release must then explicitly advertise offline-only play.

## 8. S00 — Record the release contract and baseline

Add proposed `Config/Release.swrelease` through Projects metadata, editable in
**Project > Release readiness**. It records application ID, release version, supported
platform/architecture pairs, required modes, supported player counts, minimum graphics
tier, memory budgets, frame-time budgets and the acceptance scenario. Prefer a compact
versioned text parser consistent with Package/Network settings, not a new DSL engine.

Required fields: `application_id` (reverse-domain-style ASCII stable ID),
`release_version`, repeated `platform`, repeated `mode offline|online`,
`max_players` (1 for offline-only, even 2..64 for this online shooter), `target_fps`, `simulation_hz`,
`cpu_budget_mib`, `gpu_budget_mib`, `shared_budget_mib` (0 means discrete accounting),
`loading_budget_mib`, `transition_timeout_seconds`, `acceptance_scenario`.
Finite positive numeric values only; 0 has no implicit unlimited meaning except where
explicitly defined. Device-tier overrides may be keyed records; do not infer hardware
minimums from one RTX 3080 run. The release ID is independent of project display name.

Add `--release-schema`, `--release-info --json`, and `--check` cross-validation with
level, package and network settings. Define machine-readable diagnostics with stable
code, severity, file, line, object ID and message. Preserve existing plain output.
Use exit 0 pass, 1 validation/runtime failure, 2 malformed CLI; do not print success
after a fatal problem. Unknown report format versions fail clearly.

Record current Release build test inventory and baseline package/benchmark commands.
Capture CPU/GPU/driver/OS/compiler/build flags, resolution, graphics settings, workload,
cache state and run duration. The existing build here has no explicit build type;
do not label timings Release without configuring and checking it.

Tests: parser roundtrip/defaults, invalid/NaN/overflow values, duplicate platform entries,
missing catalog, mismatched max_players, and legacy project without a release contract.
Development projects remain usable without it; release qualification requires it.

Completion: a machine can answer which game/platform/mode is being qualified and which
gates are measured, failed or still unverified. No manually checked green boxes.

## 9. S01 — Writable state for installed games

Extend Platform with `UserPaths`, accepting stable application ID and optional explicit
development override. Expose read-only install/content roots separately from writable
config, saves, logs, cache and crash roots. Platform implementation uses OS user-directory
facilities (SDL's preference path can be the initial implementation if its platform
behavior satisfies the contract); never assume the current directory is writable.

Route Player saveFolder, graphics user file, input overrides, audio settings and future
profile data through the service. Editor keeps project-local editor state. A runtime
launched from the editor uses a designated development profile, never the customer's
production profile. CI passes a temporary `--user-dir` explicitly. Reject an override
inside install/content for strict installed-release testing.

Create paths lazily with errors; no swallowed permission failures. Stable profile IDs
must not derive from Steam display names or current project names. Define default local
profile and explicit account-linked profile switching; do not merge two users' saves.
Never delete a profile automatically when an online identity changes.

Migrate legacy project `Saved/SaveGames` only through an explicit one-time import with
preview, copies and collision handling. Do not overwrite newer slots. Renaming a game's
display name must not move its data. Install update/uninstall must leave player data.

Editor Preferences/Release panel: resolved paths, Open folder, development override and
Import legacy saves. CLI: proposed `--user-paths --json`, `--user-dir`,
`--import-legacy-saves <folder>` with an explicit destination profile and no overwrite
by default. Log paths without personal account tokens.

Tests: read-only install, arbitrary CWD, Unicode/spaces, missing home directory,
permission denial, two profiles, concurrent runs, rename of display name, existing slot
collision, and moved-package save/relaunch. No test may write real user data.

## 10. S02 — Catalog, scene links and validation

Implement section 5's format in Projects. Add component metadata in an appropriate
runtime owner, then expose level-aware checks in Player's validation orchestration.
Avoid Projects depending on World just to validate component references.

Use existing scene/prefab loading in an isolated validation world or an immutable parsed
representation. Inspect every referenced scene exactly once; detect cyclic prefab
expansion separately from valid level-graph cycles. File diagnostics point to the
reference site and destination where useful. A missing optional level is still a
release error if the packaged campaign can reach it.

Add `LevelsPanel.cpp` to Editor and the proposed command paths from section 5. Save
catalog atomically. Changing entry/next/spawn supports undo and external-file conflict
warnings. Double-clicking a destination opens it through existing scene unsaved-change
handling. During Play, authoring controls are disabled or explicitly target authored
data; do not mutate the active catalog underneath travel.

Tests (`level_catalog`, `level_check_cli`): legacy one-scene project; valid three-level
graph; duplicate ID; unresolved exit/spawn; missing camera in gameplay vs UI menu;
case-only path collisions; paths with `..`/symlink escape; broken prefab; invalid kind;
unreachable warning; intentional cycle; schema regeneration exactness. Error output
must identify the file and line. Build and validate a minimal menu/A/B fixture.

## 11. S03 — Separate editor documents from runtime level lifetime

Modify PlaySession with explicit policies rather than abusing Stop:

```cpp
enum class SessionWorldPolicy { RestoreAuthoredSnapshot, OwnedRuntimeWorld };
enum class SessionSettingsPolicy { RestoreOnStop, PreserveUserSettings };
```

Existing tests/standalone construction retain RestoreAuthoredSnapshot by default.
GameFlow uses OwnedRuntimeWorld: Stop tears down subsystems but does not restore an
authored snapshot or make a second full World copy. Editor Play captures the initial
authored document once, creates a runtime clone and uses OwnedRuntimeWorld for all
travel. Stop returns rendering/picking to the authored document and original selection.
Tests must establish which graphics overrides are session effects and which are user
preferences; runtime map changes preserve the latter.

Audit Player/Editor rendering, gizmos, UIWorld, PhysicsWorld, NavigationWorld, audio,
effects, animation, circuits, saves and online hooks. Track pointers in a written owner
table in verification evidence. Destruction order is reverse dependency order, but
explicitly stop background producers before destroying their consumers. Game module
unload occurs after every object/callback whose code lives in that module is destroyed.

Introduce lifecycle context with start reason (Fresh, RestoreSave, OnlineBaseline) so
initialization is explicit. A scene-change request from beginPlay is deferred until
activation completes; cap redirect chains (8 successive pre-gameplay redirects) and
report a cycle rather than loading forever.

Tests (`level_lifetime`): count construction/start/end/destruction exactly once; request
from fixedUpdate/frame/UI callback; exception during start/end; queued job after Stop;
retained EntityRef across repeated IDs; loading while paused; Stop during preparation;
hot reload attempted during transition; 100 Play/A/B/Stop cycles preserve the authored
scene, undo, selection and cameras. Run ASan/UBSan; targeted TSan where dependencies
permit, with genuine races fixed rather than blanket suppressions.

## 12. S04 — Dependency-driven assets and bounded loading

Extend Assets instead of creating a separate registry for each level. Separate:

- **Catalog**: known asset IDs/paths, metadata and import dependencies, cheap to enumerate.
- **Prepared data**: immutable decoded CPU data; cancellable and bounded.
- **Resident resource**: published CPU/audio/GPU object with a lifetime lease.

`AssetLease` represents a set of references owned by a level or application UI. Shared
assets are loaded once and reference-counted; a departing level cannot free another
level's UI font or common mesh. GPU handles need generation/validity checks when slots
are reused. Gameplay must not retain unleased raw handles indefinitely.

Implement dependency discovery by registered providers for scenes/prefabs, imported
models and textures, material assets, RmlUi/RCSS/font/image imports, audio, Effekseer
textures/models, animation/skeletons, navigation sidecars and circuits (including named
prefab/level references). Do not rely on regex over arbitrary text to find dependencies.
Dynamic C++ asset names require explicit `runtime_roots` declarations in cook settings;
warn that unregistered dynamic strings cannot be discovered statically.

Cycle detection uses DFS colors or strongly connected components; report a readable
cycle. Allow only dependency kinds whose runtime resolver explicitly supports cycles.
Canonical path and asset-type conflicts are errors. Cache key includes input hash,
all dependency hashes, import options, importer version, target platform and format
version; timestamp-only cache validity is insufficient.

Workers may read/decode only thread-safe immutable inputs. Main thread publishes
registry updates and submits uploads through Renderer. Add upload queue byte/time
budgets, completion tokens and deferred destruction if missing. No Vulkan in Assets,
GameFramework or Editor. Stage required resources first; high-quality optional mips
later. Offline runtime does not periodically re-import all Content. Editor hot reload
still works and defers publication during a level commit/online match per policy.

Loading UI displays phase, resources/bytes, budget, elapsed time and cancellation; a
developer Resource residency panel shows owner, refs, CPU/GPU bytes, pending frees and
last error. Proposed CLI `--asset-dependencies --level <id> --json <out>` and `--load-report <out.json>`
share the runtime graph and counters. `--check` detects unresolved required edges.

Tests (`asset_residency`, `asset_dependencies`): shared asset survives A->B, unique asset
retires, cancelled decode never publishes, failed upload cleans up, duplicate request
coalesces, asset change invalidates transitive cache, 100 A/B cycles plateau, no GPU
publication from worker, and minimum budget produces an explicit error. Fake GPU tests
validate delayed completion; real GPU tests validate resource lifetime and validation
layer output. A plateau is measured after pending frees drain, not immediately on Stop.

## 13. S05 — Complete offline flow, travel and loading UI

Implement section 6 in `GameFlow.cpp` / `LevelTravel.cpp`; provide the same facade to
Player, Editor and tests. Replace Player::playScene's manual destroy/load sequence with
coordinator requests. Preserve existing online start/error semantics until S11 extends
them; reject unsupported online map travel instead of silently disconnecting peers.

Application UI layer owns persistent menu/loading/error documents and has explicit
priority over session HUD input. Application handlers reference GameFlow via lifetime
tokens, never a departing GameContext. If GameUI currently has one data model, namespace
application data (`flow.*`) and level data (`game.*`) or use independent contexts; verify
handlers and focus cannot leak between them. UIWorld stays the per-level facade.

Required shell controls: New game, Continue (disabled with reason if no compatible save),
Level select where the game permits, Settings, Credits and Quit. Pause offers Resume,
Restart checkpoint, Restart level, Settings and Main menu. Destructive progress reset
requires an in-game confirmation. Loading offers Cancel only when safe. Failure offers
Retry, Main menu and Quit, preserving an actionable reason. A zero-asset fallback error
view must not depend on loading a corrupt project font or scene.

Add circuit nodes with exec input/output and typed values: RequestLevelTravel,
RestartLevel, ReturnToMenu, TravelStatus and OnTravelFinished. Outputs distinguish
accepted request from completed travel. A retiring circuit must not receive the new
level's completion event; application/game circuit observes it through the new epoch.
Regenerate node schema and add invalid-level/spawn checks for constant node inputs.

Add proposed CLI `--flow-test <scenario.json> --json <report>` using the same command
handlers as menus. Scenario v1 contains bounded steps (travel, click, wait_phase,
assert_level, assert_value, save, load, quit), a per-step timeout and total timeout.
No arbitrary shell commands or embedded scripts. Exit nonzero at first failed assertion,
write diagnostics and current state. Headless mode tests flow logic; GPU mode exercises
rendered loading/UI. Headless success cannot prove visual correctness.

Tests (`level_travel`, `game_flow`, `game_flow_cli`): A->B->A, explicit spawn, missing
destination, invalid resources, cancel before retirement, rejected cancellation after,
two simultaneous exits, reset/new campaign, pause->load, failure starting destination,
menu scene broken, shutdown in every phase, UI action fired twice, and completion once.
Assert previous transforms reset and no held fire/jump enters the next level.

Completion: from an actual running build a player can use buttons to enter level A,
finish it, see a loading screen, enter a distinct B, restart B and return to the menu,
without code reload, executable restart, stale audio or runaway memory.

## 14. S06 — Campaign progress, checkpoints and save compatibility

Extend SaveGames and CampaignState. Do not add a parallel save serializer in the sample.
Existing `.swsave` text remains readable; introduce a versioned header upgrade, keeping
the old reader/migration path for saves whose meaning can actually be preserved.

Required save envelope fields:

| Field | Purpose |
|---|---|
| save format version | engine envelope/parser compatibility |
| application ID | refuse accidental loading of another game's saves |
| game data schema version | game-provided migration dispatch |
| build/content ID | identify content compatibility; not an unconditional exact-build lock |
| level ID and legacy scene path | cross-level destination; path only for old-format import |
| checkpoint ID and spawn ID | logical restart location |
| profile ID, slot ID, revision | prevent wrong-profile writes and stale overwrite |
| UTC time and accumulated simulation time | presentation and play-time accounting |
| campaign values | durable game-owned progression |
| level state | selected registered components/entities plus stable identities |
| checksum | detect corruption, not claim protection against intentional editing |

Offline saves remain user-editable. A changed checksum should be handled through an
explicit inspect/repack tool path; it is not anti-cheat. Never trust a client's local
save as an authoritative competitive multiplayer state.

### 14.1 Restore ordering

1. Read and validate envelope and bounded payload on a worker; reject oversized counts,
   malformed IDs, invalid floats and unsupported versions before allocating World data.
2. Resolve application/profile/level compatibility. Run game migration functions on
   detached value data, oldest-to-newest, with no side effects. Preserve original file.
3. Request travel with RestoreSave startup reason. Normal travel exclusion rules apply.
4. Load destination authored defaults and required content. Apply saved entity records
   and tombstones using stable IDs; remap entity-reference fields in a second pass.
   For v1's exact-world snapshots, use an explicit conversion to the new representation
   or a documented legacy exact restore mode. Never silently merge two interpretations.
5. Hydrate CampaignState and runtime components before creating physics/navigation,
   animation/audio/effects/circuits and before any gameplay tick.
6. Configure systems and bind handlers once, then call a dedicated restore lifecycle
   hook. Split beginPlay initialization into binding/setup and fresh-start spawning as
   necessary. Do not call a fresh-game spawn routine over restored actors.
7. Recreate bodies, navigation agents, animation state, looping sounds, effects and HUD
   from restored components. One-shot audio/effects are not replayed unless opted in.
8. Reset interpolation/history/input, publish level and report successful restore.

Saved animation transition internals need not be bit-identical if the game declares
them cosmetic; health, inventory, transforms, velocities, objectives and gameplay timers
must satisfy the game's persistence contract. The schema explicitly marks transient,
saved and network-replicated roles independently; do not infer one from another.

Entity tombstones express destroyed authored entities, so opening a completed checkpoint
does not resurrect enemies or collected items. Runtime-spawned persisted entities carry
their prefab/type and stable instance IDs. A missing required prefab rejects the load;
an optional cosmetic field can migrate with a warning if the migration declares it.
Clear cached EntityRefs and re-resolve stable IDs after restore.

### 14.2 Checkpoint and restart semantics

- New game resets campaign only after confirmation; settings and profile identity persist.
- Checkpoint captures a consistent between-ticks state and becomes the restart target
  only after its write succeeds. Display Saving / Saved / Failed honestly.
- Restart checkpoint loads the last committed checkpoint for the active campaign.
- Restart level loads its authored initial state and the level-entry campaign snapshot;
  it does not restore inventory consumed after entry or grant duplicated rewards.
- Continue finds the newest compatible committed slot for the selected profile; a corrupt
  newest slot offers a prior valid one, with an explanation.
- Level victory updates campaign progression once with an idempotent completion ID.
  Commit the completion and next destination in one save transaction. A crash between
  victory and next-level loading must not lose rewards or grant them twice on resume.
- Returning to menu does not implicitly delete progress. Quit during Saving shows the
  pending write and drains it within a bounded timeout, otherwise records failure.
- Online play continues to reject local save/restore. Offline fallback never inherits
  an active online world as trusted campaign progress.

### 14.3 Durable writes and concurrency

Take a consistent snapshot at a simulation boundary. Measure snapshot time; the current
main-thread World copy is not guaranteed cheap for all games. If over budget, use
immutable snapshot chunks/versioned records without copying mutable World memory on a
worker concurrently. Bound snapshots in flight to two and serialize writes per slot.

Write a unique temporary file beside its destination, flush data, atomically replace
using the platform's supported primitive and persist directory metadata where required.
Platform-specific tests establish the actual durability promise. Keep one last-known-good
backup and a small recovery marker if multi-file publication is necessary. Prefer one
save transaction file to eliminate partial campaign/world commits. Clean abandoned temp
files only after validating that they are not active writers' files.

Per-slot revision/CAS or an OS lock prevents two game processes silently overwriting the
same save. A stale writer reports Conflict and retains both candidates for resolution.
Cancellation after durable replace cannot report that the save was never committed.
On load failure the current level remains unchanged until the normal travel boundary.

UI: Save Games list with compatibility/error, checkpoint indicators, backup recovery,
profile selection and explicit legacy import. CLI: extend `--saves --json`, add proposed
`--save-info <file>`, `--save-migrate <file> --output <newfile>` and `--save-inspect` /
`--save-repack` if any binary representation is introduced. Never overwrite input by
default. `--check` reports required missing references and unsupported game schema.

Tests (`save_travel`, `save_migration`, `save_durability`): save in B while A is active
on next launch; correct GameMode; destroyed enemy stays dead; inventory rewards once;
missing content; wrong game/profile; newer unsupported version; migration chain;
truncated/checksum-bad file; disk-full/permission errors; two writers; delayed completion
from old epoch; interrupt every write boundary; restore while paused; baseline v1 save.
Fault injection verifies simulated interruption; separate real filesystem tests verify
atomic replacement. Do not label a killed-process test proof against physical power loss.

## 15. S07 — Build a genuinely finishable reference game

Create an independent `Games/ShippingDemo/` using the existing template/CMake project
registration route. Do not turn Arena's engine-specific tests into the only proof and
do not put weapon/objective rules in Engine. Reuse Arena gameplay facilities where
appropriate through ordinary game code, preserving Arena's existing behavior.

The demo is deliberately small but complete:

```text
Config/Levels.swlevels
Scenes/menu.swscene
Scenes/training.swscene      distinct asset set, teach movement/use/fire
Scenes/factory.swscene       different assets, checkpoint, combat/objective, exit
Scenes/ending.swscene        completion/credits UI
Content/Prefabs/             player, target/enemy, interactable, pickup
Content/UI/                 shell, pause, loading, HUD, results, ending
Config/Input.swinput
Config/Graphics.swgraphics   follow actual graphics file naming conventions
Config/Package.swpackage
Source/                     GameInstance, mode(s), registered game data and systems
Tests/                      game-specific objectives/combat/progression tests
```

Use generated or permissively licensed small assets with recorded provenance; do not
make test execution depend on a private asset library. At least one model, audio clip,
UI image and scene-specific dependency differ between gameplay levels to exercise
unloading. Include reusable prefabs and one circuit-authored exit to prove both workflows.

Minimum gameplay loop: move/look, shoot/reload, damage/death, pickup/use, objective
complete, results, next level, checkpoint, retry, final victory and credits. An AI enemy
can be simple navigation/line-of-sight logic; it must not require developing a generic
behavior-tree editor to finish this demo. Guns need gameplay feedback, not final art.
Controller and keyboard/mouse both complete the loop. UI must display interaction prompts
through bound actions, not hardcoded key letters that ignore rebinding.

Game-owned components: Health, Inventory/Loadout, ObjectiveProgress, CampaignProgress,
InteractionTarget and whatever the chosen shooter needs. Register persistent fields
with metadata and validate invalid prefab/item/objective references. Use Schedule systems
for bulk work. Main-thread interaction commands bridge UI/circuits and systems safely.
Define death/victory precedence for simultaneous events and make reward completion
idempotent. A frame-rate-dependent objective timer is a failure.

Editor authoring requirements: create/duplicate a level, place spawn/exit/checkpoint,
select destination, configure objective IDs, preview a prefab, play from entry or current
level and inspect failed checks. Existing inspector/circuit/physics/navigation tools
remain authoritative. Add missing interaction/trigger event plumbing only in its owner.
Tools must identify whether a failure is missing content, bad navigation, a circuit error
or gameplay code; do not hide it behind a generic "level won't run" message.

CLI scenarios play the same game logic: actions are injected through the input/action
boundary and objectives asserted through registered test-readable state. A diagnostic
test action may skip traversal time but must not directly set the final success flag.
Maintain one full real-input route through all levels to prove game completion.

Completion: a new tester can launch the packaged demo, finish both gameplay levels,
save/quit/resume in the second, reach credits and start again without editing files or
using a terminal. Supply a short human playtest script and a deterministic automated one.

## 16. S08 — Player controls, accessibility and presentation reliability

Audit Input, GameUI, Audio and Graphics against this checklist; extend existing APIs
rather than creating duplicate settings managers. Player preferences have three layers:
shipped defaults, profile overrides, temporary session overrides. Applying and saving
are separate operations with a single transaction across related fields.

Required menu groups and behavior:

| Group | Required behavior |
|---|---|
| Display | resolution/window mode/monitor where supported, VSync, frame cap, render scale, quality preset; device capability disables unsupported choices with reason |
| Graphics | existing effect controls; custom preset survives restart; true off paths per graphics spec; safe fallback after failed application |
| Input | rebind actions, collision warning, reset defaults, sensitivity, invert axes, controller dead zone; no inescapable binding state |
| Audio | master/music/effects/UI categories, mute on focus loss if selected, output-device error/recovery; volume persists |
| Accessibility | UI scale, readable contrast, non-color-only status, subtitles for speech if present, subtitle size, remappable controls, hold/toggle behavior |
| Camera comfort | motion blur/camera shake/flash reduction where effects exist; FOV range validated by the game |
| Language | locale selection and fallback; text keys rather than hardcoded gameplay sentences |

Dangerous display changes use Apply -> confirm within 15 seconds -> keep, otherwise
revert to last working settings, including after focus loss or a restart following a
failed attempt. Do not busy-wait. A failed resolution change must leave a usable menu.
Fullscreen requests use platform abstractions, not renderer-specific calls in game code.

Add a small localization catalog service if absent, with stable keys, UTF-8 values,
placeholder validation and deterministic fallback (requested locale -> project default
-> visible missing-key diagnostic). No need for a massive translation platform. UI
layout tests use long strings and non-Latin glyphs. Package font fallbacks with licenses.
Do not concatenate translated sentence fragments or render untrusted names as markup.

Input focus stack order: error/modal > loading > menu > HUD > gameplay. Release held
actions when context/device/focus changes; do not fire a weapon when closing a menu.
Reconnect controller without losing keyboard navigation. Offline pause suspends gameplay
time while the application clock drives UI; online menus suppress local actions without
pausing the simulation. Persist bindings by stable action IDs across updates.

Editor: edit shipped defaults and preview scaling/locales; shared gui controls for the
editor panels. Shipping UI uses the game's RmlUi styles. Text/schema: reuse Input and
Graphics formats; add versioned audio/accessibility/localization metadata as required,
with `--check` for ranges, missing keys/fonts/actions and invalid device defaults.
CLI info commands dump effective values plus origin layer; secrets are not settings.

Tests (`player_preferences`, extensions to `input`, `game_ui`, `audio`, `graphics`):
apply/cancel/revert, persistence, invalid ranges/NaN, unplug/replug, focus loss while
firing, controller-only menu traversal, font missing, long translations, 720p/high-DPI
layouts, muted category isolation and restore after a failed graphics operation.
Visual/audio/device checks on actual hardware are distinct from dummy-driver tests.

## 17. S09 — Cook all required content and package a standalone game

Packaging already exists. Add a reproducible **content preparation manifest** and a
strict release mode to that pipeline. Do not replace readable source content with an
opaque proprietary blob to appear more professional.

### 17.1 Cook inputs and outputs

Inputs are the level catalog closure, registered runtime roots, application UI/fallback
assets, project/game module schemas, import options, shader/pipeline variants, navigation
data and target platform capabilities. The cook creates:

```text
manifest version
application/build/content IDs
target platform + architecture + relevant renderer capability tier
entry levels and level -> asset dependency table
asset ID -> type, source hash, dependency hash, cooked format/version, output path/size/hash
runtime module/plugin names + ABI/build IDs
licenses and provenance inventory
```

Store the manifest as readable versioned text/JSON. Content IDs derive from canonical,
ordered content metadata and bytes; timestamps and absolute checkout paths are excluded.
The package's display creation time can vary without changing the content identity.
Cook determinism means equivalent inputs produce equivalent cooked outputs/manifest;
do not claim native compiled binaries are bit-reproducible until toolchain paths and
timestamps are controlled and tested.

Prepare models/LODs/collision, textures, required animation/skeletons, effects dependencies,
UI/fonts, circuits and nav data. Reuse existing optimizers and caches. Release startup
must not unexpectedly import `.blend`, compress textures, bake navigation, compile
circuits from scratch or scan a huge source tree. If a type has no cooked runtime path,
implement it or declare/budget the load cost honestly before allowing release mode.
Platform shader pipelines may still require driver work; S10 handles warm-up and cache.

For every introduced binary format supply `--inspect`, export to a documented readable
representation and re-encode/verify where meaningful. Be precise: a lossy cooked texture
cannot reconstruct the original image, and compiled machine code cannot be decompiled
back to original C++. Supply inspectable metadata, source availability and source-to-cook
rebuild tools instead of promising impossible reversibility.

### 17.2 Validation and publication

Proposed `player --cook --project <file> --target <platform> --json <report>` prepares
content but does not claim cross-compiling native executables. Reject targets unsupported
by installed toolchains. `--cook-info` inspects a manifest; `--check --release` verifies
the catalog/closure against release policy. `--package --release` requires a Release
configuration and compatible cooked manifest. Preserve existing development packaging.

Fail release cook/package for missing required dependency, stale generated data, absolute
developer paths, unresolved dynamic roots, excluded required assets, unsupported formats,
wrong module ABI, debug-only AppID, missing licenses or target-incompatible resources.
Distinguish optional content from unresolved content; do not demote critical errors to
warnings to get a green package. Include all levels, not only the current startup scene.

Use a new staging folder. Verify before atomic publication. Publish archive and folder
with a consistent build ID; a failed archive must not masquerade as a fully successful
release. Keep the old valid artifact available. Never package user saves, credentials,
Steam SDK headers, captures, editor state or source cache garbage. Developers can opt to
ship source separately; runtime correctness cannot depend on a checkout being present.

Editor Package window adds Release mode, target, cook status, dependency report, excluded
required-asset errors, package size and verification summary. Clicking a missing asset
opens its authoring reference. CLI and UI call the same packaging service, not separate
command construction logic. Keep existing headless checking and moved-package smoke.

Tests (`cook_dependencies`, `cook_reproducibility`, extensions to `package`/`package_cli`):
two identical cooks; change a shared dependency invalidates both levels; remove a required
effect/UI texture; exclude level B; runtime-root-only asset; cancellation; target mismatch;
hash corruption; missing runtime library; build absent; path spaces/Unicode; clean-cache
package loads without source; read-only package A->B->save->resume from arbitrary CWD.

## 18. S10 — Establish a measured graphics and performance release tier

Use the graphics roadmap for implementation details. Choose and record a release subset:
correct color/material import, stable shadows, acceptable aliasing treatment, readable
lighting, scalable effects and a predictable fallback tier. Bloom, AO, reflections, fog,
particles and higher tiers must have actual disabled paths when the feature permits it.
No effect is mandatory merely because another engine has it. A visually missing feature
may be mandatory for this game's art direction; record that decision explicitly.

Separate CPU simulation, CPU submission, GPU frame, asset/upload, audio, memory and
network budgets. For target rate F, the frame budget is `1000/F` ms: 16.667 at 60 Hz,
8.333 at 120 Hz. CPU and GPU mostly overlap; adding their reported totals is not a
reliable display-frame-time model. Use actual presented frame intervals and queue data.

Proposed minimum release evidence: 60 fps tier at 720p or declared internal resolution
on the actual chosen minimum PC; a 120 fps competitive tier on a separately named
machine. These are targets, not measurements. Define p95/p99 frame times, maximum
avoidable hitch, memory and loading budgets in S00. Warm-up and loading intervals are
reported separately, not hidden by deleting inconvenient samples. Record cold-cache and
warm-cache runs, both offline and worst supported online player count.

Avoid first-use stalls with cooked content, predicted pipeline variants and bounded
warm-up during loading. Driver pipeline caches are device/driver-specific, versioned
and disposable; they are not interchangeable cross-machine assets. Warm-up work must
appear in loading progress. Dynamic variants need a deliberate fallback, not a synchronous
compile in combat. Preloading everything forever is not an acceptable optimization.

Simulation debt after loading/focus suspend is capped or reset by explicit clock policy.
Never run thousands of catch-up physics ticks when a loading screen closes. Network
reconciliation uses authoritative tick policy; do not just discard online simulation
time in an unrelated render loop. Keep tick and render interpolation behavior tested at
30/60/144/uncapped rendering and the selected fixed tick rates.

Resource stress scenes include many materials, visible/skinned characters, particles,
dynamic lights, shadow casters, audio voices and repeated map rotations. Use real
representative geometry and shaders; 64 empty cubes do not prove 64-player rendering.
Optimization reports identify bottleneck/pass and percentile, not only average FPS.

Orin test: confirm an executable/driver path actually runs first; record unified memory,
OS reserve, thermal/power mode and clocks. Test sustained play and travel while monitoring
system pressure and swap. Report it separately from desktop minimum requirements.
Never promise "any PC": publish a tested capability/driver/hardware floor.

UI: existing Stats/Optimisation/Graphics plus residency/transition counters; CLI: existing
`--benchmark --json`, `--optimise --json`, extend reports with load phase and residency
watermarks. Tests: budget accounting, real effect-off resource removal, repeated level
memory plateau, shader cold start and minimum-tier screenshot/reference comparisons.
Do not gate hardware timing on a noisy generic CI runner; dedicated release hardware
records must still be required for a performance claim.

## 19. S11 — Multiplayer must survive the whole match lifecycle

Continue the existing network implementation and verification record. Preserve strict
SteamApi isolation and the SDK-supplied-by-developer rule. Current simulated 12-player
evidence does not complete the 64-player or real-service gates.

### 19.1 Choose the first map-rotation protocol explicitly

For v1 use **controlled session restart with lobby continuity**. Do not attempt seamless
migration of Jolt state or raw solver memory between maps. Keep Steam/online services and
lobby membership at application scope; per-map NetworkWorld/replication state is rebuilt.
Extend OnlineMatch so ordinary session close can be distinguished from an announced
map rotation; otherwise current close semantics may eject everyone to the main menu.

Proposed reliable authenticated control messages (extend the existing envelope/versioning):

| Message | Required fields |
|---|---|
| MapPrepare | transition ID, next epoch, level ID, content/schema digest, roster revision, load deadline |
| MapReady | transition ID, next epoch, validated content digest; sent only after local preparation |
| MapReject | transition ID, bounded reason code; no arbitrary executable paths |
| MapCommit | transition ID, next epoch, authoritative start policy/tick, session nonce |
| MapAbort | transition ID, reason and return destination |

Only the current authority sends prepare/commit/abort. A client request may ask for a
vote but cannot trigger travel directly. Validate level against catalog and installed
manifest. An untrusted peer never sends a filesystem path to load. Control messages use
bounded fields, counts, retries and timeouts; extend wire tests and protocol reference.

Host flow: finish round -> freeze score/rewards -> announce Prepare -> clients prepare
with application UI -> host also prepares -> collect Ready until deadline -> disconnect
unready peers with clear reason or abort according to game policy -> commit new epoch ->
recreate per-map sessions/baselines -> resume only players with acknowledged baselines.
Online services continue pumping throughout. If existing transport connections cannot
survive session recreation safely, explicitly reconnect through the retained lobby with
a new nonce; prove slots and auth tickets are reclaimed. Do not assume sockets survive
because GameInstance does.

Input/snapshot/event routing includes map epoch/session nonce. Old reliable retransmits
cannot spawn old-map entities or grant rewards in the new map. New epoch clears snapshot
baselines, interpolation, prediction/rewind history, event deduplication and spawn IDs.
Network IDs cannot accidentally alias pending old-epoch events.

No synchronized wall-clock countdown is required for v1: authority advances match ticks,
clients begin after valid baseline/start state and use existing tick synchronization.
Never equate unsynchronized local timestamps with a shared start instant. Late joiners
receive the current catalog level/epoch and active round state; joining during rotation
queues against the next epoch or fails with retry information, never a mixed baseline.

Host failure during preparation/commit goes to an explicit disconnected screen and menu
after cleanup. Host migration is a separate future feature. An authoritative dedicated
server can avoid listen-host departure but is not a replacement for failure handling.

### 19.2 Advertised player counts and operational behavior

Test 2, 12, 32 and 64 integrated players when advertising 1v1–32v32, with movement,
combat, respawns, projectiles/objects if used, score, late join and map rotation. Record
host/client CPU, physics, upload/download rates, queues, memory and render frame times.
Load generation must execute the real replication/gameplay path. Mock connections test
faults; multi-process and real-machine sessions test actual scheduling and service behavior.

Host upload approximately grows with peers and visible replicated data; group/shared
encoding reduces CPU, not necessarily sent bytes. Measure total host outbound payload
and transport overhead separately. Bound queues and degrade update frequency/relevance
explicitly before exceeding configured bandwidth. Do not silently change authoritative
tick rate. Content and protocol mismatch fail with user-facing version information.

Session controls: lobby capacity/team balance policy, ready state, leave, kick authority,
timeouts, late join, reconnect policy and clear unavailable reasons. Matchmaking/ranked
services and anti-cheat deployment are game/product decisions; do not claim Steam auth
alone prevents cheating. Keep authority checks, input limits, malformed-packet handling
and rate limits. Raw memory writes into Jolt are not an accepted network protocol.

Real-service gates: two licensed/test-authorized accounts on separate machines; host/find/
invite/join/play/rotate/leave/rejoin; Steam relayed path; Windows/Linux cross-play if
advertised; offline startup; auth revocation; host quit; client stall; mismatched build.
No automated tool should upload a build or message players as a side effect of testing.

Editor Network window: level/epoch, rotation phase, ready roster, timeout and error;
game menus show loading/disconnect/retry. CLI extends existing `--net-test` scenario
registry with `map_rotation`, with deterministic seeds and JSON evidence. Regenerate
`NetworkProtocol.md`, NetworkSchema and checks when their contracts change.

Tests (`network_map_rotation` plus existing network suites): duplicate prepare/commit,
old snapshot after commit, peer never ready, late join each phase, hash mismatch, host
loss, 12-second outage, reauth failure, repeated map rotation and 64-player resource caps.
Completion includes real-service evidence; mock-only implementation remains unqualified.

## 20. S12 — Diagnose failures and recover without losing the game

Extend Core logging with a bounded recent-log ring and structured session/build context.
Normal logs rotate under UserPaths, not beside the executable. Record application/build/
content/schema IDs, level/epoch, travel phase, renderer/device/driver, active graphics
tier, last successful save revision and error code. Do not record auth tickets, access
tokens, raw voice data or personally identifying machine paths unnecessarily.

Crash support has two layers:

1. In-process safe error handling for recoverable operations (parse, load, device error,
   failed IO), returning typed errors and displaying the application error view.
2. Minimal platform crash capture for unrecoverable faults. Do not run arbitrary C++
   allocation, logging locks, Vulkan cleanup or complex unwinding from an unsafe signal
   handler. Use supported platform facilities and, if required, a small justified crash
   helper dependency. Capture locally; uploading reports requires explicit product policy
   and user action/consent, not automatic network traffic hidden in the engine.

Keep matching debug symbols indexed by build ID outside the player package. Supply a
developer symbolization command/process and prove a controlled crash names the relevant
game/engine frame. Release builds must not strip away the only copy of usable symbols.
On next startup detect an unclean previous exit and offer Safe graphics / Open logs /
Restore last valid save. Do not delete saves or reset all settings automatically.

Safe graphics chooses a known supported minimal tier and windowed mode, skips optional
effects and ignores a suspect pipeline cache. Device lost cannot always recover in
process; offer a clear restart path with the last committed checkpoint intact. Never
continue submitting work through a known failed renderer as if nothing happened.

Add proposed `--diagnostics --json`, `--safe-mode`, and a developer-only fault-injection
test target. The shipping executable must not expose an accidentally reachable crash
button. Editor Console links stable diagnostic codes to documentation and source files.
Release readiness shows unresolved fatal errors, not a wall of raw logs alone.

### 20.1 Process and file handling

Current Packaging ChildProcess uses shell command strings and popen/_popen. Introduce
Platform Process execution with executable path + argument vector + explicit environment,
CWD, stdout/stderr capture, timeout, cancellation and child cleanup. Replace packaging/
build invocations incrementally. Windows argument encoding and POSIX exec semantics
need their own tests; quoting for one shell is not portable argument escaping.

Test paths containing spaces, quotes, percent, semicolon, ampersand and Unicode without
executing their contents. Do not construct a shell script from project-controlled names.
If a shell is intentionally required for a developer command, make that a distinct
explicit API and never use it for package verification or game startup.

Normalize package/asset paths, reject escaping paths and case collisions, and handle
symlinks deliberately. Do not recursively delete a path supplied by malformed project
metadata. Staging directories must be created by the packager and ownership-checked
before cleanup. Validate content/network/save sizes before allocation. These are concrete
shipped-file boundaries, not a requirement to sandbox trusted native game modules.

Tests (`diagnostics`, `process`, `failure_recovery`): controlled load exception, crash log
presence, symbolization, no secret in captured auth failure, full disk log rotation,
subprocess timeout/cancel including child processes, literal hostile-looking filenames,
device-failure simulation and safe-mode recovery. Actual device loss is a manual gate
when reproducible safely; an injected error is labeled as such.

## 21. S13 — Supported platforms, installs and updates

Qualify each advertised platform separately; a compile pass is insufficient. Reconcile
Packaging.md with actual Windows/macOS implementation and tested limits. Do not publish
OS/driver minimum versions by guessing from the current developer workstation.

For each supported platform:

1. Build Release on a controlled toolchain whose runtime baseline is documented.
2. Package all required shared libraries/plugins/fonts/shaders/cooked assets/licenses.
3. Inspect dynamic-library dependencies and wrong-architecture files; do not depend on
   the developer PATH, LD_LIBRARY_PATH, build tree, SDK installation or working directory.
4. Install/move to a fresh machine or representative clean VM with an actual supported
   GPU path for rendering tests. Use a standard nonadministrator user and read-only
   install root. Test spaces, Unicode and a different launch working directory.
5. Launch, play all levels, save and quit, restart and Continue; verify controls/audio/UI.
6. Install an update with changed content and a schema migration. Preserve user data,
   migrate a copied save, and verify old save fallback. Refuse incompatible downgrade
   with an explanation; never overwrite a newer-format save with defaults.
7. Remove/move the installation and reinstall; user data remains accessible by stable ID.

Windows: executable/launcher path, runtime library policy, DPI, controller reconnect,
fullscreen recovery and crash symbols. Linux: glibc/C++ baseline, display backends,
permissions and driver discovery. macOS if advertised: bundle/library paths, MoltenVK
capability gaps, high-DPI and supported architecture/OS tests. Signing/notarization or
store upload are separate distribution steps with credentials, not embedded into a
local verification run. Document exact applicable procedures when that release is made.

Steam release: use the game's real AppID and correct runtime library. Keep SDK headers,
development AppID files and captures out of artifacts. Verify launch via Steam and its
restart path, service unavailable handling, offline policy and overlay behavior on the
actual release package. Preserve existing development-package escape hatch explicitly;
it must not silently turn an AppID 480 build into a release-qualified artifact.

No full differential patcher is necessary for the first game. A tested full-package
replacement or store-managed update is sufficient. Versioned content/save compatibility
is mandatory even with store-managed distribution. Cloud save support is optional; if
added, local writes must still be transactional and cloud conflict resolution must
preserve both versions rather than choosing solely by unreliable wall-clock timestamp.

Editor Release readiness lists each platform and evidence state. Proposed CLI
`--verify-install <folder> --scenario <file> --json <report>` launches only known package
entrypoints via Process and validates build IDs/report exit codes. No arbitrary commands
from an untrusted report. Test running a moved package without the engine checkout.

Completion: release evidence names exact artifact hashes and tested machines. An
untested macOS/ARM/64-player combination is omitted from supported claims, not inferred
from a related successful configuration.

## 22. S14 — Automated acceptance, CI and regression prevention

Extend `.github/workflows/build.yml`; do not weaken its existing passing checks. Add
test labels `unit`, `integration`, `gpu`, `timing`, `installed`, `online_mock` and
`online_real` where useful. Use separate jobs for fast validation, package flow and
hardware qualification. An advisory GPU job is useful feedback but does not satisfy
a required release gate. A skipped gate is reported skipped/unverified.

### 22.1 Required test inventory

These are proposed CTest names to introduce in their owning batches; actual executable
names follow repository conventions. Every test has a timeout and isolated temporary
user/project/output directories.

| Test | Principal assertions |
|---|---|
| `level_catalog`, `level_check_cli` | format/schema/references/legacy defaults |
| `level_lifetime` | stop/start exactly once, old refs/jobs invalid, editor restoration |
| `asset_dependencies`, `asset_residency` | complete closure, budget/refcount/retirement/cancellation |
| `level_travel` | state machine, rollback before boundary, safe failure after boundary |
| `game_flow`, `game_flow_cli` | menu->A->B->ending, real shared commands, timeout reports |
| `save_travel`, `save_migration`, `save_durability` | cross-map restore, ID remapping, revision/atomicity |
| `player_preferences` | safe settings persistence, focus and invalid overrides |
| `shipping_demo_gameplay` | objectives, death/retry, one-time rewards, win/ending |
| `cook_dependencies`, `cook_reproducibility` | all levels/assets, cache keys, stable manifests |
| `network_map_rotation` | authority, epochs, readiness/deadlines, old packet rejection |
| `diagnostics`, `process`, `failure_recovery` | actionable failures, argument safety, safe restart |
| `installed_game_flow` | moved read-only package completes, saves and resumes |

Keep relevant existing tests: `ecs`, `prefab`, `project`, `project_create_cli`,
`save_games`, `save_games_cli`, `circuits`, `circuits_cli`, `physics`, `audio`, `input`,
`game_ui`, `navigation`, `effects`, `animation`, `graphics`, `graphics_cli`,
`optimisation`, `package`, `package_cli`, and current network/Steam tests. Check current
CTest inventory rather than assuming all platform-conditional tests are present.

Test reports must include expected test count; a regex selecting zero tests is an error.
Use `ctest --show-only=json-v1` to enumerate when constructing filtered gates. Do not
hide failures with unconditional success or `continue-on-error` on required tests.

### 22.2 CI layers

- Pull request: build, non-GPU tests, parser/CLI negative cases, deterministic flow,
  schema freshness, shared dummy audio, and package closure checks.
- Merge/nightly: ASan/UBSan, supported TSan subset, repeated travel/save/Stop, seeded
  network faults and long sessions, clean/warm cook, installed-game flow.
- Release candidate: clean platform packages, real display/GPU/UI/audio, all supported
  hardware tiers, real Steam if online, update/save migration and controlled crash
  symbolization. Require recorded manual evidence where automation is unavailable.

Linux display-dependent tests need an appropriate Xvfb/Wayland/Vulkan environment;
SDL dummy audio only substitutes sound devices. Setting a dummy video driver does not
make Vulkan tests meaningful. Check Windows multi-config binary paths explicitly.
Cache dependency sources/builds by OS, architecture, compiler/configuration and CMake
inputs as needed; never restore incompatible object files and call the result portable.

Schema regeneration includes Scene, Input, CircuitNodes, Graphics, Network and new
Level/Release/Save references as their registries change. Generated docs must be stable
across runs, independent of local profile and wall time. Strict check verifies the
shipping demo and Arena without altering their authored content.

### 22.3 Scenario report contract

Proposed report shape (JSON; fields shown are required):

```json
{
  "format": 1,
  "scenario": "complete_game",
  "build_id": "recorded-build-id",
  "content_id": "recorded-content-hash",
  "platform": "linux-x86_64",
  "renderer": "vulkan",
  "passed": false,
  "steps": [
    {"id": "enter_factory", "status": "failed", "code": "MissingAsset",
     "level": "factory", "epoch": 3, "duration_ms": 123.0}
  ],
  "peak_cpu_bytes": 0,
  "peak_gpu_bytes": 0,
  "unverified": ["real_steam"]
}
```

Zero memory in this illustrative example is not a measured result. Actual reports use
null/unavailable for unsupported counters, not fabricated zero. Add hardware/settings,
cache-state and artifact hash metadata for performance/installed tests. Limit report
sizes and keep full logs as separate artifacts. JSON never mixes ordinary log text into
the document; diagnostics go to stderr or a separate log file.

## 23. S15 — Documentation and release rehearsal

Update Projects, StartingProjects, SaveGames, Packaging, GameUI, Networking, Architecture
and Tutorial to show implemented behavior. Add `Levels.md` and generated `LevelSchema.md`
when S02 exists. Keep this document labeled as a plan until every required batch has
evidence; use a batch status table linking to verification records, not broad "done" prose.

Developer-facing routes:

- Help > Shipping implementation guide: this plan and verification links.
- Project > Levels: catalog and destinations.
- Window > Runtime flow: active level, epoch, travel phase, pause/input context, errors.
- Window > Resource residency: leases, memory, pending uploads/frees.
- Build > Package game: cook/build/verify/install evidence.
- Project > Release readiness: required vs measured gates and platform support.

These are proposed authoring surfaces except the Help entry added alongside this spec.
Implement the panels in their batches using shared gui controls/themes, not custom
colors or rounded panels. New metadata belongs in schemas, not only editor state.
Do not put shipping game-specific menus in the editor's menu bar.

Add a tutorial that starts from a fresh external project, creates two levels, links an
exit, sets an entry, adds a checkpoint, builds a menu and packages it. Follow the tutorial
literally on a clean checkout with no prior sample cache or hidden game module settings.
Document API thread/lifetime rules, how to cancel loads, persistence scopes and how to
inspect binary cooked data. Explain that a level ID survives file moves through explicit
catalog edits and that old save IDs require migrations.

Release rehearsal uses a new tester and a clean machine/profile. Give only the package
and short player instructions. Record every developer intervention. An intervention to
set a path, copy a missing library, edit a save or use a debug console is a failed
standalone-game gate until fixed and rerun.

## 24. Error catalog and exact negative behavior

Use stable codes in UI/CLI/reporting. The message may be localized; the code is stable.
At minimum implement these outcomes across the relevant owner APIs:

| Code | Required behavior |
|---|---|
| `Busy` | refuse second travel; active ticket unchanged |
| `UnknownLevel` / `UnknownSpawn` | fail before retirement; show reference and destination |
| `InvalidCatalog` | preserve previous valid editor catalog; release check fails |
| `MissingAsset` / `ContentMismatch` | identify dependency chain; no silent required placeholder |
| `BudgetExceeded` | identify budget/estimate; offer lower tier or return, no unbounded allocation |
| `Cancelled` | terminal once, release staging, old level continues before boundary |
| `TooLateToCancel` | operation continues safely; UI disables Cancel |
| `StartFailed` | cleanup candidate, application fallback UI remains usable |
| `SaveConflict` | preserve both revisions; no silent overwrite |
| `IncompatibleSave` | explain app/schema/content requirement; original file untouched |
| `WriteFailed` | previous committed save remains selected; no Saved toast |
| `AuthorityRequired` | client cannot choose authoritative map/state |
| `LoadTimeout` | abort/drop according to declared offline/online policy; never wait forever |
| `DeviceUnavailable` | safe graphics/device/restart options; don't lose committed progress |
| `UnsupportedTarget` | cook/package rejects the requested unavailable target |
| `IncompleteReleaseEvidence` | release qualification remains failed/unverified |

An error record includes ticket/epoch when relevant and a file/reference location when
known. Do not expose raw OS errors as the only player explanation. Keep the detailed
underlying error in developer logs.

## 25. Final installed-game acceptance sequence

This is the definition of a complete playable reference game. Run both automation and
human playthroughs. Automation does not replace evaluating whether a menu is readable.

1. Build Release from a clean checkout and create the package using only documented
   commands. Record artifact hash, commit and contract.
2. Move package to a different path; run from a third working directory. Remove access
   to source/build paths. Install is read-only; user directory is initially empty.
3. Launch through the player-facing executable/launcher. A usable main menu appears.
   No editor, terminal, SDK, local dev AppID or import tools are needed for offline play.
4. Change graphics/audio and one input binding. Apply, restart, verify persistence and
   demonstrate safe display revert on an unconfirmed change.
5. Start a new campaign. Observe loading and enter training. Verify HUD, controls,
   physics, audio, an animated actor/effect where used and a gameplay objective.
6. Complete training through its normal objective/exit. Observe a different scene and
   asset set in factory. Verify old audio, UI handlers, physics bodies and effects are gone.
7. Confirm campaign inventory/progress transfer. Reach checkpoint, kill/collect something,
   save, quit process, relaunch, Continue directly into factory with correct state.
8. Die, restart checkpoint, then restart the level. Verify their different persistence
   semantics. Repeated actions do not duplicate rewards or resurrect tombstoned actors.
9. Complete factory, reach ending/credits, return to menu and start again. No executable
   restart or developer console is needed to complete the campaign.
10. Repeat A/B/menu travel 100 times. After retirement drains, memory plateaus within the
    declared tolerance and no subsystems/handlers/threads accumulate. Capture watermarks.
11. Inject missing/corrupt destination content, disk-full save failure and cancelled
    preparation. Observe correct error/recovery, with the last committed save intact.
12. Upgrade to the next test build with a real schema/content change. Continue the old
    save through migration; keep the original; incompatible downgrade fails safely.
13. Run the same flow with keyboard/mouse and controller, at minimum resolution and
    high DPI, on each advertised platform and hardware tier.
14. For an online release, host/join through the real service, complete a match, rotate
    to another map, late join, disconnect/rejoin and return to menu. Repeat supported
    player-count/fault/performance gates from S11. Record any unsupported combinations.
15. Cause a controlled developer-test crash. Locate local report, symbolize against the
    matching build and recover on next boot without erasing player data.

Success requires **all required steps**, not a package that merely passes `--check`.
A platform/mode may be intentionally excluded by the release contract before qualification;
do not silently remove a failed required gate after testing to make the report green.

### 25.1 Flow-test scenario v1: exact harness contract

Use JSON with strict duplicate/unknown-key checking. Maximum input 1 MiB, 512 steps,
maximum per-step timeout 600 seconds and total timeout 3600 seconds. Reject negative,
zero or nonfinite timeouts, repeated step IDs and missing required parameters. These
are harness safety bounds, not in-game loading limits. Defaults: step timeout 30 seconds,
total 300 seconds. Scenario timeouts use monotonic real time even while simulation pauses.

```json
{
  "format": 1,
  "id": "two_level_smoke",
  "timeout_seconds": 180,
  "steps": [
    {"id": "menu", "op": "assert_level", "level": "menu"},
    {"id": "new", "op": "click", "document": "Shell", "element": "new_game"},
    {"id": "training_ready", "op": "wait_level", "level": "training", "timeout_seconds": 60},
    {"id": "next", "op": "travel", "level": "factory", "spawn": "entrance"},
    {"id": "factory_ready", "op": "wait_level", "level": "factory"},
    {"id": "save", "op": "save", "slot": "acceptance"},
    {"id": "save_done", "op": "wait_save", "slot": "acceptance"},
    {"id": "back", "op": "menu"},
    {"id": "back_ready", "op": "wait_level", "level": "menu"},
    {"id": "restore", "op": "load", "slot": "acceptance"},
    {"id": "restored", "op": "wait_level", "level": "factory"},
    {"id": "end", "op": "quit"}
  ]
}
```

This example is a plumbing smoke test. It intentionally requests travel directly and
therefore **does not prove normal objective completion**. The full acceptance scenario
must use the game's normal actions/objectives and test cross-process save resume too.

Operation definitions:

| `op` | Parameters beyond `id`, `op`, optional timeout | Completion condition |
|---|---|---|
| `click` | document, element | visible/enabled element exists, same UI handler as user input dispatched once |
| `action` | action, value, ticks | existing input action injected for N fixed ticks (1..36000), then released; action type/range checked |
| `travel` | level, optional spawn | coordinator accepts request; harness stores ticket; failure to accept fails step |
| `wait_phase` | phase | most recently accepted travel ticket reaches phase; terminal failure fails immediately |
| `wait_level` | level | active LevelId matches, associated travel succeeded and one simulation tick or ready menu frame completed |
| `assert_level` | level | immediate comparison of current active level, no travel triggered |
| `assert_value` | key, expected, optional absolute_tolerance | compare registered test-visible game state; missing key fails, numeric tolerance nonnegative finite |
| `save` | slot | request accepted; remember expected next slot revision |
| `wait_save` | slot | that revision durably committed, not merely present from an earlier run |
| `load` | slot | normal cross-level restore accepted; remember travel ticket |
| `menu` | none | returnToMenu accepted |
| `quit` | none | orderly shutdown drains/cancels work and reports success only if all previous assertions passed |

Game readback keys come from a registered diagnostic interface returning immutable values;
the harness cannot mutate arbitrary component fields. Inputs go through the action sampler,
not directly into physics. Menus may accept controller navigation actions; the click
operation alone is not evidence of controller support. Delayed steps advance/pump the
real application; do not substitute an independent simulator in the installed-game test.

For save/relaunch coverage the parent test runs scenario A (save + quit), then starts a
new packaged process using the same isolated user directory for scenario B (continue +
assert progress). The parent verifies both exit codes, committed revision and matching
artifact IDs. Do not implement a `shell` step to achieve process restart.

## 26. Concrete first implementation assignment

Give the next implementing model this bounded instruction:

> Read AGENTS.md, Architecture.md, UI.md and ShippingImplementation.md. Re-audit the
> listed baseline files. Implement S00–S02 first, preserving existing project behavior
> and user changes. Provide the release metadata, per-user runtime path service and
> level catalog with editor/CLI/schema/check coverage. Do not implement a competing
> save/network/asset system. Run relevant tests, regenerate affected schemas, record
> evidence and list exact remaining batches. Then implement S03–S05 as one coherent
> level-travel milestone, with safe lifetime separation before wiring menu buttons.

The first visible milestone is **menu -> level A -> level B -> menu** through shared
runtime APIs. The next is **save in B -> quit -> Continue in B -> finish -> credits**.
Only after these work should the release effort be called a complete-game pipeline.
