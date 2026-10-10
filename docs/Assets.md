# Model and texture assets

Place source files under the project's `assets` directory. Names are paths relative
to that directory, with forward slashes. The editor and standalone player import
the same assets before loading the scene. Image formats remain PNG, JPEG, TGA, BMP,
PSD, GIF, HDR and PNM. Models use Assimp 6.0.5, built with its default importers,
including glTF/GLB, FBX, OBJ/MTL, Collada, STL, PLY and the legacy Blender reader.
`modelExtensions()` reports the formats actually enabled in the build; USD is not
enabled. A reader being available does not guarantee every variant of that format.

## Editor workflow

The Assets panel has a Models section. Drag a model into the viewport, double-click
it, or use its placement menu. Placement creates an entity with `Transform` and
`ModelRenderer`; its model reference, visibility and tint appear in the reflected
inspector and support undo. Drop a different model onto the inspector's Model
field to replace it. The imported material section shows source material settings.
Edit source materials in the modeling tool, then save to reload them.

Models preserve all triangle parts, per-part materials, node hierarchy matrices,
repeated mesh instances, vertex colors, UV0 and generated normals when missing.
The complete model is one scene entity; imported nodes aren't separate editable
ECS entities. A parent on that entity places the entire model. Selection outlines
use imported bounds; viewport selection tests the actual triangles. Negative scale
preserves front-face orientation. `assets/models/material-demo.glb` is a small,
self-authored three-material model for trying the workflow.

Scene files save model names rather than GPU IDs. `ModelRenderer` is registered
through the same reflection used by other components. Game code renders models
with `renderWorld(world, renderer, jobs, alpha, &engine.assets())`.

## Materials

The current renderer displays base color factors/textures, vertex colors, opaque
surfaces, alpha cutouts, basic alpha blending, and double-sided surfaces. Embedded
GLB textures and external image files are decoded. glTF's texture orientation is
retained. Transparency sorts parts by depth and does not solve intersecting
transparent surfaces. Only UV0 and the current shared repeat/linear sampler are
rendered; additional UV sets and sampler/texture-transform overrides aren't applied.

Metallic/roughness factors and normal, combined metallic/roughness, emissive and
occlusion maps are retained in `ModelData`. Full PBR shading, normal mapping,
emissive lighting, and occlusion rendering belong to the later lighting milestone;
imports report a warning when those material features aren't yet displayed.
Skinned meshes and animated scenes currently display their static pose and report
the missing playback support. This is an asset milestone, not skeletal animation.

## Blender files

Assimp's native `.blend` support is deprecated and unreliable for modern files.
Shockwave first launches Blender to export a temporary GLB, then imports that GLB
with Assimp. Install Blender on PATH, or launch with the environment variable:

```sh
SHOCKWAVE_BLENDER=/absolute/path/to/blender ./build/bin/editor
```

The executable path is passed as one argument, so spaces in it are supported.
Conversion uses background mode with automatic file script execution disabled,
a 120-second timeout, and a unique temporary directory removed after import.
The source `.blend` is never modified. External Blender image/library paths are
tracked for reload. Blender's glTF exporter determines which Blender features can
be represented; arbitrary shader nodes, simulations and animation playback aren't
supported by this renderer.

If Blender cannot run/export, the deprecated Assimp reader is attempted and its
limitations are reported. Older Blender files may work through that reader.
Exporting glTF/GLB yourself is the alternative when Blender isn't installed.
Modern `.blend` conversion was verified with a temporary Blender 4.5.3 build;
that temporary test executable is not an installed project dependency.

## Reload and failure behavior

Saving a model, an external image, a glTF `.bin`, or an OBJ `.mtl` reimports dependent
models. Dependency stamps include missing files, so repairing them triggers another
attempt. Imports decode on workers; the main thread uploads complete replacements
and publishes them between frames. Failed CPU/GPU imports keep the last working
model. Replacements keep the same model handle and recycle old GPU resource slots
after GPU readers finish. Files removed from disk keep their last working runtime
copy; reopening the project still requires the files to exist.

A failed model tile shows the error and has a **Retry imports** context action,
useful after installing/configuring Blender. Models currently import from source
at startup, so large projects will eventually need an on-disk import cache/cooker.
Shader-side features, complex DCC dependencies and animation are separate work.

## Verification

```sh
cmake --build build -j 4
ctest --test-dir build --output-on-failure
# Opt-in: requires a desktop and Vulkan GPU, not part of headless CTest.
./build/bin/model_gpu_smoke .
```

The CPU test covers glTF and GLB, embedded/external PNGs, OBJ triangulation and MTL,
nested transforms, materials, triangle picking, failed dependency repair, stable
asset references and scene serialization. The GPU smoke test covers actual uploads,
repeated reloads, missing buffers/textures, MTL edits, last-good retention, resource
recycling and rendering all three material parts. Fixtures are self-authored and
can be regenerated with `python3 tests/fixtures/models/generate.py`.
