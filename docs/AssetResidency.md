# Asset residency: what each level needs, and what it holds

A level needs more than its scene. Its scene names meshes, materials, prefabs, sounds and
UI documents. A material names its parent and its textures, a prefab names other prefabs,
and a document names its stylesheets and images. Shockwave follows all of it, so it can
say exactly which files a level needs, find the broken links before anyone plays, and
keep track of what stays resident when the game moves from one level to the next.

This is S04 in [the shipping plan](ShippingImplementation.md). Its first half added the
dependency graph, the checks, per-level leases, a budget, the load report and the editor's
**Resource residency** window. Its second half loads a level's assets only when it needs
them and frees them when nothing holds them, with GPU frees that wait for the frames
still reading them ([Loading on demand](#loading-on-demand)). That's off by default:
every asset is imported when the game starts, as before. Travel itself is in
[LevelLifetime.md](LevelLifetime.md).

## The dependency graph

`Assets/AssetGraph.h` names each file by its path inside the project, with forward
slashes: `Scenes/a.swscene`, `Content/textures/bricks.png`. A mesh or texture made in code
is `builtin:cube`.

Each kind of file has a **provider** that reads it with its own parser:

| File | What it refers to |
| --- | --- |
| `.swscene`, `.swprefab` | every asset-typed field (mesh, texture, model, material, material overrides, audio clip, UI document), and String fields registered as asset paths: `Prefab.path`, `Circuit.circuit`, `ParticleEffect.effect`, `PostProcessVolume.profile`, `NetworkObject.prefab`, `BakedLighting.asset` and `.reflections` |
| `.swscene` (navigation) | its baked `.swnavmesh` beside it, if there is one |
| `.swmaterial` | its parent and each slot's texture |
| models | what the importer opened: external textures, `.bin` buffers, `.mtl` files |
| `.rml` | `<link>` stylesheets and templates, `<img src>`, and `<style>` blocks |
| `.rcss` | `@spritesheet` and `@font-face` `src`, and `image()` and `url()` |
| `.efkefc` | the textures and models Effekseer opens for it |
| `.swcircuit` | literal values on pins that take an asset: mesh, texture and model pins, `Entity.Spawn` prefabs, `Audio.PlaySound` clips, a `UI.Show` document file, and reflected component fields that are asset paths |
| `.swpostprocess` | its parent and its LUT |
| `.swlighting`, `.swreflection` | the generated files the manifest vouches for |

Nothing guesses paths out of text. Comments, data bindings (`{{icon}}`) and sprite names
aren't files, and are left alone. A file kind with no provider is a leaf.

Game code that keeps an asset path in a String field of its own says so once, where it
registers the component:

```cpp
registerAssetPathField("Spawner", "prefab", AssetKind::Prefab);
```

Names that only code knows (a mesh a game module registers at startup, a path built at
run time) can't be found by reading files. A name that's neither a file nor a built-in is
a warning, `AssetUnknownName`. Declaring such roots for cooking comes with S09.

### Problems

| Code | Severity | When |
| --- | --- | --- |
| `AssetMissing` | error | a required file isn't there |
| `AssetPathCase` | error | a path spelt differently from the disk (it works on Windows and macOS, and breaks on Linux) |
| `AssetPathOutside` | error | a path that leads outside the project |
| `AssetKindConflict` | error | a reference to the wrong kind of file (a model field naming a `.png`) |
| `AssetUnreadable` | error | a file its provider can't parse |
| `AssetCycle` | error | a file that reaches itself; the message lists the loop. No asset kind can depend on itself |
| `AssetUnknownName` | warning | a name only code can provide |

A navigation mesh or a lighting bake that isn't made yet is fine here: it's optional, and
`--check` warns about it on its own.

### Closure hashes

Each file has a hash of its bytes, and a **closure hash** of its bytes, its kind, its
providers' versions and every dependency's closure hash. Editing a texture changes the
closure hash of every material, prefab, scene and level that reaches it, and nothing else.
The S09 cook will key its cache on it, so a stale timestamp can't hide a change.

The graph keeps what it read. Asking again only reads the files whose size or time changed.

## Checking it

```sh
build/bin/player --project <file.swproject> --asset-dependencies [--level <id>] [--json <file|->]
build/bin/player --project <file.swproject> --check
```

`--asset-dependencies` lists each level's files with their sizes, GPU memory and closure
hash, then every problem, and exits 1 on an error. With `--level` it lists one level. With
`--json` it writes format 1: each level's closure, every asset with its edges, and the
diagnostics.

`--check` now also follows everything the checked scenes and the game circuit reach, and
fails on the errors above.

## Leases

`Assets/AssetResidency.h` keeps track of who holds which asset. A **lease** is the set one
owner holds: a level, or the application's own UI. An asset is counted once however many
leases hold it.

When the game travels from A to B (`LevelHost`, [LevelLifetime.md](LevelLifetime.md)):

1. B's scene loads into a separate world.
2. B's closure is planned against what's resident: which assets are **shared** (another
   lease, or both levels, hold them), which are **new**, and which will **retire**.
3. If B doesn't fit the budget, travel fails with `BudgetExceeded` and the totals, and A
   keeps playing. Nothing has ended yet.
4. A's session ends. B's lease is taken, then A's goes, so a font or mesh both levels use
   never leaves. Only A's own assets retire.
5. B starts.

Without a catalog, or for a scene from `--scene`, the level's scene is still followed.

### The budget

Off by default. Set it for a run:

```sh
build/bin/player --project <file.swproject> --asset-budget-gpu 2048 --asset-budget-cpu 4096
```

Sizes are MiB; 0 is no limit. A level that would leave more resident than that is refused
before the current one ends. The first level is refused too, and the player exits 1.
In the editor, the Resource residency window sets it for the next Play.

GPU bytes are what the AssetFolder measured for a texture's mipmaps and variants, plus a
model's vertex and index buffers. CPU bytes are each file's size, standing in for its
prepared data. A file that's never been loaded on demand costs 0 GPU bytes until its
first load measures it; after that the last measurement is kept, so plans stay honest
once it's unloaded again.

### Turning it off

`--no-asset-residency` in the player, or **Track level leases** in the editor's window
(for the next Play). Levels then hold no lease and nothing is planned or refused.

## Loading on demand

Set it in the project, or for one run:

```
asset_loading levels        # all (the default): everything is imported at startup
```

```sh
build/bin/player --project <file.swproject> --asset-loading levels
```

The editor's Resource residency window has the same switch (**Player asset loading**); it
saves the project. The editor itself stays eager, so everything stays editable.

With `levels`:

- **Opening the Content folder imports nothing.** It lists the files and watches them.
- **A lease loads its assets.** Before a level starts, `AssetResidency::load` asks its
  loader (`AssetFolderLoader`, in `GameFramework/LevelAssets.h`) for whatever the lease
  holds that isn't in memory yet. Files that need no import (scenes, RML, circuits) just
  count as loaded.
- **In bounded batches.** `AssetFolder::load` groups files while their estimated staging
  (four times the file size) stays under `LoadOptions::stagingBytes` (256 MiB by default;
  a file bigger than that goes alone). Workers decode; only the main thread creates GPU
  objects and registers names. Textures go first, then models and sounds, then
  materials, so a material resolves against textures already in.
- **Cancellable.** `cancelled()` is asked before each batch and after its decode, before
  anything is published. A cancelled load publishes nothing more and unloads what it
  already published.
- **Failures clean up.** A file whose upload fails is unregistered, its partial GPU
  objects destroyed, and its `error` says why. A material that can only resolve as the
  fallback counts as failed. The level fails with `AssetLoadFailed: <id>: <why>`.
- **The last lease going unloads.** An asset whose last lease is released is unregistered
  at once and its GPU objects destroyed. A material still reading an unloaded texture
  falls back (magenta) instead of reading freed memory.
- **GPU frees wait for their frames.** Destroying a texture or mesh hands it to the
  renderer, which frees it once no frame in flight reads it (`Renderer::pendingFrees()`;
  `flushFrees()` waits for the GPU and frees everything). On Vulkan that's the frame
  fences; the null renderer fakes two frames in flight so tests see the delay.
- **Hot reload only touches what's loaded.** A saved file that isn't loaded is picked up
  the next time it's asked for.

Travel on demand runs in this order:

1. plan against an empty world (graph errors are refused here, before anything ends);
2. end the current level's session;
3. take the new lease, give the old one back: the old level's own assets unload;
4. **upload**: load the new lease's assets;
5. **read**: the new level's scene, now that its assets are in;
6. start.

A failure in steps 4 to 6 happens after the old level ended, so nothing is playing; the
player goes back to its entry level, as for any level that didn't start
([LevelLifetime.md](LevelLifetime.md)).

The player also holds a lease on the game circuit's assets for the whole run, and one on
the startup scene's assets while it starts.

Assets named only in code (not reachable from a scene, prefab, material or document) aren't
loaded on demand. Leave such a game on `all` until it lists them; `runtime_roots` for the
cook is S09.

`asset_loading levels` needs leases: with `--no-asset-residency` the player refuses to
start (exit 2).

## The load report

```sh
build/bin/player --project <file.swproject> --load-report load.json
```

At exit it writes every level start and change (format 1):

- the level, where it came from, its epoch, and whether it succeeded;
- milliseconds per phase: `validate`, `read` (the scene, in a separate world), `prepare`
  (its closure and the budget), `retire` (the old level and its lease), `upload` (the new
  world and lease) and `start` (the session and `beginPlay`);
- assets: how many, shared, new and retired, with bytes;
- resident and peak CPU and GPU bytes afterwards;
- on demand, how many assets that load imported and unloaded, and the GPU frees pending.

It says which `asset_loading` the run used, and ends with what's still resident after
Stop, which should be nothing: no assets, no leases, no pending frees, and as many
unloaded as loaded in total. The loading
screen (S05) shows the same phases.

## In the editor

**Window > Resource residency**:

- **Level loads**: the last loads during Play, phase by phase, with shared, new and
  retired assets. A failed one says why.
- **Resident assets**: each asset, its kind, the leases holding it, refs, CPU and GPU bytes,
  whether it's loaded and its last error, with the GPU frees pending in the summary.
  Filter by path or owner.
- **Open scene's dependencies**: **Check dependencies** follows the saved scene the way
  `--asset-dependencies` does.
- **Track level leases** and the CPU and GPU budgets, for the next Play.
- **Player asset loading**: `all` or `levels`, saved to the project.

## Code and tests

- `Assets/AssetGraph.h`: the graph, providers and diagnostics.
- `Assets/AssetResidency.h`: leases, the plan, the budget and the `Loader`.
- `Assets/AssetFolder.h`: `setOnDemand`, `load` with `LoadOptions`, `unload`, `loaded`.
- `Renderer/Renderer.h`: `pendingFrees`, `flushFrees`.
- `Engine/AssetReferences.h`: scene and prefab references, and `registerAssetPathField`.
- `GameFramework/LevelAssets.h`: a project's graph, a level's costs, the JSON and
  `AssetFolderLoader`.
- `GameFramework/LevelHost.h`: `Hooks::assets`, `setResidency`, `loadReports()`.
- Tests:
  - `asset_dependencies` (unit): every provider, each problem code, cycles, closure
    hashes changing transitively and only then, and unchanged files not read again;
  - `asset_residency` (unit): shared assets survive A to B, unique ones retire, duplicates
    coalesce, a level over budget is refused before teardown, a start over budget fails,
    leases off, and 100 A/B cycles that plateau and drain to nothing at Stop; with a
    loader, loads and unloads per lease, a cancelled or failed load leaving nothing, the
    on-demand travel order, and 100 cycles on demand;
  - `asset_streaming` (unit, null renderer) and `asset_streaming_vulkan` (gpu): a real
    AssetFolder on demand: open imports nothing, batches under a staging limit, uploads on
    the main thread only, a cancelled decode never published, a failed upload freed, a
    material falling back when its texture goes, frees that wait for frames then drain,
    hot reload of loaded files only, a lease loading and unloading, and 100 cycles that
    plateau;
  - `asset_dependencies_cli` (integration): `--asset-dependencies`, `--check`,
    `--load-report`, `--no-asset-residency`, budgets and `--asset-loading` on
    `Tests/fixtures/levels`.
