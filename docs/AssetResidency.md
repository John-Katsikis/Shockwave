# Asset residency: what each level needs, and what it holds

A level needs more than its scene. Its scene names meshes, materials, prefabs, sounds and
UI documents. A material names its parent and its textures, a prefab names other prefabs,
and a document names its stylesheets and images. Shockwave follows all of it, so it can
say exactly which files a level needs, find the broken links before anyone plays, and
keep track of what stays resident when the game moves from one level to the next.

This is the first half of S04 in [the shipping plan](ShippingImplementation.md). It adds
the dependency graph, the checks, per-level leases, a budget, the load report and the
editor's **Resource residency** window. Every asset is still imported when the game
starts, as before. Loading a level's assets only when it needs them, and freeing them
(with GPU frees that wait for their frame) when nothing holds them, comes in the second
half. Travel itself is in [LevelLifetime.md](LevelLifetime.md).

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
prepared data until the second half of S04 measures that.

### Turning it off

`--no-asset-residency` in the player, or **Track level leases** in the editor's window
(for the next Play). Levels then hold no lease and nothing is planned or refused.

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
- resident and peak CPU and GPU bytes afterwards.

It ends with what's still resident after Stop, which should be nothing. The loading
screen (S05) shows the same phases.

## In the editor

**Window > Resource residency**:

- **Level loads**: the last loads during Play, phase by phase, with shared, new and
  retired assets. A failed one says why.
- **Resident assets**: each asset, its kind, the leases holding it, refs, CPU and GPU bytes,
  pending frees and its last error. Filter by path or owner.
- **Open scene's dependencies**: **Check dependencies** follows the saved scene the way
  `--asset-dependencies` does.
- **Track level leases** and the CPU and GPU budgets, for the next Play.

## Code and tests

- `Assets/AssetGraph.h`: the graph, providers and diagnostics.
- `Assets/AssetResidency.h`: leases, the plan and the budget.
- `Engine/AssetReferences.h`: scene and prefab references, and `registerAssetPathField`.
- `GameFramework/LevelAssets.h`: a project's graph, a level's costs and the JSON.
- `GameFramework/LevelHost.h`: `Hooks::assets`, `setResidency`, `loadReports()`.
- Tests:
  - `asset_dependencies` (unit): every provider, each problem code, cycles, closure
    hashes changing transitively and only then, and unchanged files not read again;
  - `asset_residency` (unit): shared assets survive A to B, unique ones retire, duplicates
    coalesce, a level over budget is refused before teardown, a start over budget fails,
    leases off, and 100 A/B cycles that plateau and drain to nothing at Stop;
  - `asset_dependencies_cli` (integration): `--asset-dependencies`, `--check`,
    `--load-report`, `--no-asset-residency` and budgets on `Tests/fixtures/levels`.
