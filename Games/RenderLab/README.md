# Shockwave Render Lab

A separate game project for testing the renderer with assets from your local library.
It uses the existing asset importer, PBR materials, prefab system, LODs, shadows, GameUI
and the shared graphics API. No renderer code changes are required.

## Run it

From the engine folder, after preparing the assets and building the module:

```sh
./build/bin/player --project Games/RenderLab/RenderLab.swproject
```

Hold **RMB** to look; **WASD** moves, **Q/E** moves vertically, **Shift** moves faster.
**O** toggles an orbit and **R** restores the original camera. **F1–F4** select Low,
Medium, High and Ultra. The on-screen buttons offer the same actions plus LOD/full detail.
These quality changes use `game.graphics()` and are temporary in Render Lab.

The default camera is static, so benchmark views are repeatable. There are no physics
bodies or gameplay bots; these workloads exercise rendering, asset import, scene traversal
and draw submission. The HUD shows recent FPS/GPU time/draw counts and rendered versus
full-detail triangle counts reported by the renderer, not a whole-session average.

| Scene | Model instances | Source mesh triangles before LOD |
|---|---:|---:|
| `materials` | 4 | 111,183 |
| `gallery` | 16 | 746,055 |
| `stress-64` | 64 | 2,984,220 |
| `stress-256` | 256 | 11,936,880 |
| `stress-1024` | 1,024 | 47,747,520 |

These source counts exclude plinths and floor. Drawn triangles vary with the view, culling
and LOD. Repeated instances share the same 16 assets: larger grids stress traversal,
submission, geometry and shadows, **not proportionally larger texture memory**.

```sh
./build/bin/player --project Games/RenderLab/RenderLab.swproject --scene Games/RenderLab/Scenes/stress-256.swscene
./build/bin/editor Games/RenderLab/RenderLab.swproject
```

In the editor, use File > Open for another scene and Play for the camera/HUD controls.
Models and reusable display prefabs appear in Content. All placements remain editable
text and ordinary scene entities. The game has no hard-coded spawn grid.

## Prepare it on a new checkout

The library assets and their derivatives are local, Git-ignored content. The originals
under `/mnt/STORAGE_/Asset Library/` are only read. The project tracks the importer,
preparation script, scene/prefab text, UI, and procedural studio environment. It does not
redistribute the asset packs or assume they have an open-source license.

Register this optional game alongside the games you want to build. `SHOCKWAVE_GAMES`
replaces the selected list; it does not append to it:

```sh
cmake -S . -B build '-DSHOCKWAVE_GAMES=Games/Sample;Games/Arena;Games/RenderLab'
cmake --build build --target renderlab_import -j
python Games/RenderLab/Tools/PrepareAssets.py --library '/mnt/STORAGE_/Asset Library/'
cmake --build build --target player editor -j
./build/bin/player --project Games/RenderLab/RenderLab.swproject --check
```

The Python preparation tool needs Pillow. The C++ importer uses the engine's existing
Assimp integration; no new engine library is added. Preparation regenerates the five
named scenes and 16 prefabs, replacing edits to those generated files; copy a scene before
customizing it if you want to regenerate later.

The selection is eight guns from **FBX - 30 Old Gun final** and eight engines from
**JRO_HardSurface_Kitbash_Vol.5**. The photo-reference pack has no mesh assets; the helicopter
contains Unreal `.uasset` files, which Shockwave cannot directly import. Neither is copied.

Guns get explicit UV-matched base-color and normal maps; separate roughness and metallic
maps are packed into the standard glTF green/blue channels. Height maps are not used
because this renderer does not expose displacement here. Normal maps are assumed to use
the OpenGL convention; rerun with `--flip-normal-green` if the source pack uses DirectX.
Untextured engine parts get a neutral metal material, rather than claiming to reproduce
an unavailable authored material. Model nodes/transforms, mesh parts and vertex data are
preserved; this converter explicitly rejects animated/skinned models.

Textures are capped at 2048 pixels by default. Use `--texture-size 4096` or `0` (original
size) to increase texture pressure, or `1024`/`512` for constrained devices. The same glTF
models are used for all workload sizes. The project enables BC7 compression and four
mesh LODs. The current prepared asset folder is about 101 MB on disk; that is not a RAM or
VRAM measurement. First import/optimization can take longer than warm startup.

`Saved/Import/manifest.json` records selected paths, SHA-256 hashes, model bounds, triangle
counts, texture sizes and workload counts. Individual import logs are beside it. The
scene generator derives model placement/scale from imported bounds so different source
units don't make models disappear or overlap.

## Inspect the binary geometry

The output is standard glTF JSON plus `geometry.bin` and PNGs. No proprietary container:

```sh
python Games/RenderLab/Tools/InspectGltf.py Games/RenderLab/Content/Library/gun_30/model.gltf
python Games/RenderLab/Tools/InspectGltf.py Games/RenderLab/Content/Library/gun_30/model.gltf --dump --output /tmp/gun-geometry.json
```

`--dump` exposes every vertex attribute and index as readable JSON. `ImportModel.cpp`
contains the conversion code. The original library files remain available for reauthoring.

## Benchmark and inspect rendering

```sh
python Games/RenderLab/Tools/Benchmark.py --profile high
python Games/RenderLab/Tools/Benchmark.py --profile low --scenes stress-256 stress-1024
python Games/RenderLab/Tools/Benchmark.py --profile full-detail --scenes stress-256 stress-1024
```

Reports and logs go to `Saved/Benchmarks/`. Each JSON records the command, build type,
quality settings, output resolution, GPU, frame-rate lows and GPU pass timings. The
`full-detail` profile uses High settings with LOD error zero. Default runs use a 1-second
warmup followed by a requested 5-second measurement. Engine statistics use a rolling
window of 2,048 frames, so high-frame-rate reports may cover less than the requested
interval. Process time includes startup; engine frame statistics don't.

Use a Release build, repeat runs on an otherwise idle machine, and compare the same
scene/resolution/profile. The initial [verification report](Docs/Verification.md) records
this checkout's results and limitations; these are not claims about minimum-spec support.

```sh
./build/bin/player --project Games/RenderLab/RenderLab.swproject --scene Games/RenderLab/Scenes/materials.swscene --screenshot /tmp/materials.png
./build/bin/player --project Games/RenderLab/RenderLab.swproject --scene Games/RenderLab/Scenes/materials.swscene --view normals --screenshot /tmp/normals.png
./build/bin/player --project Games/RenderLab/RenderLab.swproject --lod-colors --screenshot /tmp/lods.png
./build/bin/player --project Games/RenderLab/RenderLab.swproject --schema Games/RenderLab/Docs/SceneSchema.md
ctest --test-dir build -R 'renderlab_' --output-on-failure
```
