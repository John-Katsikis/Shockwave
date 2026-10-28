# Game graphics settings

Game code and menus use `game.graphics()` (include `<Shockwave/Game.h>`). The shared
service wraps the engine's existing graphics controls: render scale, shadow quality,
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
changes neither. Unchanged settings aren't reapplied each frame. Changing shadow maps
or render resolution can still cause a one-time GPU allocation stall.

`graphicsPreset(GraphicsPreset::Low, settings)` selects a quality preset while retaining
VSync, the FPS cap and low-latency mode. Medium, High and Ultra are also available. These
are starting points, not hardware detection or a performance guarantee. LOD quality only
helps meshes that have imported LODs. In headless tests, the service keeps settings in
memory and uses the same validation.

## Files and precedence

`.swgraphics` is a versioned, readable text format. [GraphicsSchema.md](GraphicsSchema.md)
lists every key and range. The player resolves settings in this order:

1. Project manifest (`vsync`, `frame_limit`, `low_latency`, `shadow_quality`, `lod_error_pixels`).
2. Optional `<project>/Config/Graphics.swgraphics`, shipped with the game.
3. The player's `Graphics.swgraphics` in SDL's OS preferences folder, keyed by game module
   ID. On Linux this is normally `~/.local/share/Shockwave/<game_module>/Graphics.swgraphics`.
4. Explicit command-line overrides (`--fps`, `--vsync`, `--low-latency`, `--shadows`, `--render-scale`).

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
build/bin/player --graphics-schema docs/GraphicsSchema.md
```

`--graphics-info` prints resolved settings and the user file path without loading the
renderer. `--check` always validates Config/Graphics.swgraphics if present and any explicit
user file. Regenerate the schema when changing the format or its validation. Tests cover
file errors, inheritance, persistence, deferred renderer changes, CLI overrides and Arena's
real menu draft/apply/cancel behavior.

This API currently exposes the engine's implemented runtime quality controls. Window size
remains a startup setting (`--size` / `window_size`); fullscreen switching, anti-aliasing,
upscaling technologies and automatic hardware presets are not provided by this wrapper.
