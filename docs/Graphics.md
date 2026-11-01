# Game graphics settings

Game code and menus use `game.graphics()` (include `<Shockwave/Game.h>`). The shared
service wraps the engine's graphics controls: render scale, shadow quality,
anti-aliasing (FXAA or TAA), bloom quality and strength, texture quality, anisotropic filtering,
mesh LOD error, VSync, frame limit and low latency. It doesn't change scene lighting,
materials, cameras, asset import quality, or the renderer backend.

```cpp
auto settings = game.graphics().requested();
settings.renderScale = 0.75f;
settings.shadows = shockwave::ShadowQuality::Low;
settings.frameLimit = 120;
std::string error;
if (!game.graphics().apply(settings, error)) {
    // Show error in your menu.
}
```

Call this on the main thread from GameMode hooks or UI callbacks, not ECS worker
systems. `apply` validates the entire struct and queues the request; the engine applies
it at the next frame boundary, before GPU command recording. `requested()` includes
queued values; `current()` reads the applied renderer/pacing values. An invalid request
changes neither. Unchanged settings aren't reapplied each frame. Changing shadow maps,
bloom or anti-aliasing never waits for the GPU to go idle: new images are made at the frame
boundary and the old ones are freed once the frames using them finish. Render resolution
changes still rebuild the scene targets. `status()` says whether the last request is still
pending, applied, or applied with a fallback (and why).

Every setting is described once, in `graphicsSettingInfo()`: key, label, tooltip, range,
how it applies, and whether presets set it. Files, the schema, `--graphics-set`, the editor
and Arena's menu all read that table, so a new setting appears everywhere at once. A setting
that's off costs nothing: no pass in the frame graph, no CPU work, no GPU memory.

- `anti_aliasing`: `none` (the default), `fxaa`, which smooths edges after tone mapping
  and before game UI, or `taa`, which jitters the camera and blends each frame with the
  last ones before bloom: steadier thin geometry and detail, a little softer, with brief
  ghosting on fast motion (docs/TemporalAntiAliasing.md). They're never stacked; off,
  neither costs anything. Games that teleport the camera call `game.cutCamera()`.
- `bloom_quality`: `off`, `low` (quarter resolution, 4 levels), `medium` (half, 5) or
  `high` (half, 6, the default).
- `bloom_scale` (0..1): the player's multiplier on the scene's authored bloom intensity.
- `texture_quality`: `ultra` (the default) keeps every material texture whole; `high`,
  `medium` and `low` drop its largest 1, 2 or 3 mip levels, so each step takes about a
  quarter of the memory of the one before. Textures never shrink below 32 pixels on
  their longer side, and UI textures (`category ui` in their `.swimport`, HDR environment
  maps, game UI images) never shrink. Changes rebuild textures on worker threads, a few
  per frame, and swap each in at a frame boundary: nothing waits for the GPU, and
  `status()` reads pending until they're all done. The engine keeps a CPU copy of each
  material texture so it can do this without reading files again; `--graphics-report`
  and **Window > Optimisation** show how much.
- `anisotropy`: `1`, `2`, `4`, `8` or `16` (the default). The most anisotropic filtering
  any texture gets; the GPU's limit caps it too. Textures that asked for nearest
  filtering (pixel art) never get it.
- `material_quality`: `high` (the default) or `low`. Low skips materials' detail layers,
  the close-up micro texture a `.swmaterial` can add (docs/Graphics/02-MaterialsAndDecals.md);
  base materials and second layers draw the same. It applies on the next frame.
- `ambient_occlusion`: `off` (the default), `low`, `medium` or `high` (8, 16 or 32
  samples a pixel). Screen-space ambient occlusion darkens the light from the
  surroundings in creases, corners and where objects meet, worked out at half resolution
  from a surface prepass (depth and normals) and blurred without crossing edges. Direct
  light, shadows, reflections and emission are never darkened. Each scene's Environment
  sets how far it looks (`ao_radius`, 0.3 m by default) and how strongly it darkens
  (`ao_intensity`, 1). Off, there's no prepass and no memory for it. `player --view ao`
  and the editor's *Ambient occlusion* shading view show it alone (at medium when the
  setting is off).
- `baked_lighting`: `true` (the default) or `false`. A scene's baked lightmaps and probe
  volumes (docs/Lighting.md, *Baked lighting*) replace the light from the surroundings
  with light that knows about walls and bounces. False lights everything from the
  surroundings alone, pixel for pixel as if the scene had never been baked, and applies
  on the next frame; the bake stays loaded. Scenes without a bake look the same either
  way. Presets leave it alone.
- `reflection_probes`: `true` (the default) or `false`. A scene's captured reflection
  probes (docs/Lighting.md, *Reflection probes*) replace the sky's reflection inside
  their boxes. False reflects the sky and environment everywhere, pixel for pixel as if
  the scene had no probes, and applies on the next frame; the captures stay loaded.
  Scenes without probes look the same either way. Presets leave it alone.
- `decals_quality`: `off`, `low`, `medium` or `high` (the default): how many projected
  decals (docs/Decals.md) a frame draws at most, 64, 256 or 1024, nearest the camera
  first, with decals marked `essential` kept before decoration. Off draws none: no
  decal pass, no decal buffers, and no surface prepass for them, pixel for pixel the
  scene without decals. `player --view decals` and the editor's *Decals* shading view
  show where they land.
- `light_culling`: `auto` (the default), `clustered` or `brute_force`. How each pixel
  finds the point and spot lights that reach it (docs/ClusteredLighting.md). Clustered
  sorts lights into screen tiles and distance slices first, so a pixel tests only those
  near it; brute force tests every light everywhere, with no cluster pass and no memory
  for it. Auto clusters when a frame has 8 or more point and spot lights. The picture is
  the same pixel for pixel every way: only the cost differs. Each scene's Environment
  sets how far out clusters reach (`cluster_far`, 250 m). `player --view clusters` and
  the editor's *Light clusters* shading view show how many lights each pixel tests.
  Presets leave it alone.
- `soft_particles`: `true` (the default) or `false`. Particles fade out where they meet
  walls and floors, and as they reach the camera, as each effect asks
  (docs/Effects.md, *Soft particles*). False draws the hard edges and doesn't read the
  scene's depth for it. Low turns it off.
- `particle_distortion`: `true` (the default) or `false`. Heat haze and other effects
  that bend the scene behind them. False skips those parts of an effect, with no copy of
  the scene and no memory for one. Low turns it off.
- `particle_lighting`: `scene` (the default) or `simple`. Lit particles are lit by every
  light through the light clusters, or by the first directional light and the ambient
  light alone. Low uses simple.
- `color_grading`: `true` (the default) or `false`. Postprocess profiles' white balance
  and LUTs (docs/PostProcess.md). False shows the colours ungraded, pixel for pixel as
  without them. Presets leave it alone.
- `auto_exposure`: `true` (the default) or `false`. Lets profiles that ask for it meter
  the scene and adapt exposure. False keeps each profile's manual exposure, pixel for
  pixel, with no metering pass and no memory for it. Presets leave it alone.
- `fog_quality`: `off`, `analytic`, `low`, `medium` (the default) or `high`. How fog from
  postprocess profiles is drawn (docs/Fog.md): the closed form alone, or a lit froxel
  volume near the camera with the sun's shadows and every lamp, at coarser to finer
  detail. Off draws no fog, with no passes or memory. `player --view fog` and the
  editor's *Fog transmittance* shading view show how much the fog lets through. Scenes
  without fog cost nothing either way.
- `clearcoat`: `true` (the default) or `false`. The varnish layer on clear-coated
  materials (docs/Shading.md). False draws the surface under it, exactly as without the
  coat. Presets leave it on.
- `transmission_quality`: `off`, `low`, `medium` (the default) or `high`. Glass: low
  looks straight through (blurred by rough glass), medium also bends the view through
  solid glass, high blurs rough glass more smoothly. Off draws simplified tinted glass,
  with no copy of the scene and no memory for it. `player --view transmission` shows
  where the light through each pixel came from. Low uses low.
- `skin_quality`: `off`, `low` or `high` (the default). Light spreading under skin. Off
  draws skin as an ordinary opaque surface, with no pass and no memory. Low turns it off.
- `hair_quality`: `low` or `high` (the default). High draws hair cards' highlight as a
  band across the strands; low draws an ordinary round one. Low uses low.
  Scenes without glass, skin or hair cost nothing for any of these.
- `instancing`: `true` (the default) or `false`. Neighbouring draws of the same mesh with
  the same pipeline go out as one instanced draw call (docs/Drawing.md). False issues one
  call per object. The picture is the same pixel for pixel either way. Presets leave it on.
- `occlusion_culling`: `on` (the default) or `off`. On frames that already draw the depth
  prepass (ambient occlusion, TAA or decals on), the GPU skips opaque objects the prepass
  proves hidden, and draws the rest indirectly (docs/Drawing.md). It never adds a
  prepass, never culls shadows, glass or skinned meshes, and draws anything it can't
  judge. The picture is the same pixel for pixel either way; off has no culling pass and
  no depth pyramid. The graphics report says why it didn't run on a frame it didn't.
  Presets leave it on.
- `graphics_memory_budget_mb`: `0` (the default, no budget) or 16..65536. The most GPU
  memory optional effects may take, in MiB. Over it, effects are turned down one at a time,
  TAA's history first, until they fit (docs/GraphicsBudget.md). Only effects that hold
  memory in the scene are turned down. Your settings stay as you chose them: the steps show
  as fallbacks with the reason. Presets leave it alone.

`graphicsPreset(GraphicsPreset::Low, settings)` selects a quality preset while retaining
VSync, the FPS cap, low-latency mode and bloom strength. Every preset turns on FXAA, and
sets bloom quality low, medium or high, texture quality to its own name, anisotropy to
4, 8, 16 and 16, material quality low for Low and high for the rest, ambient
occlusion off, low, medium and high, decals low, medium, high and high, and soft
particles, heat haze and scene-lit particles off (simple) for Low and on for the rest,
and fog analytic, low, medium and high, glass low, medium, medium and high, skin off,
low, high and high, and hair low for Low and high for the rest.
`matchingGraphicsPreset` names the preset the quality settings match, or none ("Custom").
Medium, High and Ultra are also available. These are starting points, not hardware detection or a performance guarantee. LOD quality only
helps meshes that have imported LODs. In headless tests, the service keeps settings in
memory and uses the same validation.

## Files and precedence

`.swgraphics` is a versioned, readable text format. [GraphicsSchema.md](GraphicsSchema.md)
lists every key and range. The player resolves settings in this order:

1. Project manifest (`vsync`, `frame_limit`, `low_latency`, `shadow_quality`, `lod_error_pixels`).
2. Optional `<project>/Config/Graphics.swgraphics`, shipped with the game.
3. The player's `Graphics.swgraphics` in SDL's OS preferences folder, keyed by game module
   ID. On Linux this is normally `~/.local/share/Shockwave/<game_module>/Graphics.swgraphics`.
4. Explicit command-line overrides (`--fps`, `--vsync`, `--low-latency`, `--shadows`, `--render-scale`,
   `--anti-aliasing`, and `--graphics-set key=value` for any key, repeatable).

`--graphics path.swgraphics` uses an explicit user file instead of the OS path. It must
exist; Apply & Save writes back to that file. `--no-user-graphics` skips automatic user
preferences and disables saving, unless an explicit `--graphics` file was given. CLI
quality overrides affect this run; they are saved only if the player chooses Apply & Save.

Corrupt automatic user files produce a warning and fall back to project defaults.
An invalid project-default file or explicit `--graphics` file fails with a file/line
error. Partial files inherit omitted values. A failed load leaves the destination
unchanged. Saves validate first and write through a temporary file before replacing
an existing file. A replacement failure is reported without deleting the old file.

Tools, screenshots and benchmarks ignore automatic user preferences for reproducibility;
use `--graphics` to explicitly test a configuration. Screenshots/tool modes still force
VSync off, as before. Benchmarks turn it off unless `--vsync on` was specified.

## Menus and editor

Arena demonstrates this in **P → Graphics Settings**. Click a setting to cycle it; choose
one of the four quality presets, then **Apply & Save**. **Defaults** edits the draft;
**Back / Cancel** discards unapplied changes. Keyboard/gamepad focus navigation is handled
by GameUI. The game-owned implementation lives in
[GraphicsMenu](../Games/Arena/Source/ArenaGraphicsMenu.cpp), with layout and colors in its
Content/UI/Arena files. Other games can supply entirely different menus using the same API.

For a menu with Apply/Cancel, keep a local `GraphicsSettings` draft. Validate it; if
`graphics.file()` isn't empty, call `graphics.save(draft, error)`, then `graphics.apply`.
Saving first means a write failure leaves the live settings unchanged. Show failures to
the player. `graphics.defaults()` supplies the resolved project defaults.

The editor's existing Preferences and viewport rendering controls use this API too.
During Play, Arena's menu applies temporary settings without saving player preferences.
Stop restores the graphics values from before Play. The player loads saved preferences
before gameplay starts; no game-specific renderer code is necessary.

## Verify from the command line

To see what the GPU supports, each setting as asked for and as actually running (with
the reason for any fallback), and GPU memory by category, use `--graphics-report`; the
editor's Stats window shows the same. Fixtures, baselines and image comparison are in
[GraphicsVerification.md](GraphicsVerification.md).

```sh
build/bin/player --project Games/Arena/Arena.swproject --graphics-info
build/bin/player --project Games/Arena/Arena.swproject --no-user-graphics --graphics-info
build/bin/player --project Games/Arena/Arena.swproject --graphics my.swgraphics --check
build/bin/player --project Games/Arena/Arena.swproject --render-scale 0.75 --shadows low --fps 120
build/bin/player --project Games/Arena/Arena.swproject --anti-aliasing fxaa --graphics-set bloom_quality=low
build/bin/player --project Games/Arena/Arena.swproject --graphics-info --json settings.json
build/bin/player --graphics-schema docs/GraphicsSchema.md
```

`--graphics-info` prints resolved settings, the matching preset and the user file path
without loading the renderer; `--json file` also writes them, with every setting's
descriptor. `--check` always validates Config/Graphics.swgraphics if present and any explicit
user file. Regenerate the schema when changing the format or its validation. Tests cover
file errors, inheritance, persistence, deferred renderer changes, CLI overrides and Arena's
real menu draft/apply/cancel behavior.

This API currently exposes the engine's implemented runtime quality controls. Window size
remains a startup setting (`--size` / `window_size`); fullscreen switching, TAA,
upscaling technologies and automatic hardware presets are not provided by this wrapper.
