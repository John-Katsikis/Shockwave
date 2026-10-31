# Temporal anti-aliasing and motion vectors

TAA smooths edges, thin lines and shimmering detail by moving the camera a fraction of a
pixel every frame and blending each new frame with what the last ones showed. It's
steadier than FXAA on thin geometry and fine detail, at the cost of some softness, and it
can ghost briefly behind fast motion.

It's the player's `anti_aliasing` graphics setting (docs/Graphics.md): `none`, `fxaa` or
`taa`. Presets keep FXAA; `taa` is opt-in. Off (`none` or `fxaa`) there's no jitter, no
motion vectors, no history and no extra passes or memory: the frame is exactly what it
was. TAA and FXAA are never stacked.

```
player --project Games/Arena/Arena.swproject --anti-aliasing taa
player --view motion      # each pixel's motion since last frame
player --view reactive    # where transparent drawing tells TAA not to trust history
```

The editor's *Shading* menu has the same two views (*Motion vectors*, *Reactive mask*).

## How it works

Batch G09 makes the motion vectors, G10 resolves with them (docs/GraphicsImplementation.md
13 and 14). The rules are in `Renderer/Temporal.h`, with CPU tests in `Tests/TemporalTest.cpp`.

1. **Jitter.** While TAA runs, the camera's projection is moved by a centred Halton(2, 3)
   offset of up to half a pixel, a different one each frame for eight frames. Culling
   uses the unmoved projection.
2. **Motion vectors.** The surface prepass also writes, for every opaque surface, how far
   it moved on screen since last frame (unjittered), how far from the camera it was then,
   and whether that's known. A draw's previous pose is found by its `RenderInstanceKey`
   (every mesh and model renderer has one): its model matrix last frame, and for skinned
   meshes its vertices skinned again with last frame's joints, so a bending arm has the
   right motion, not just its root's. Levels of detail share the mesh's vertices, so a LOD
   change keeps its motion.
3. **Reactive mask.** After particles, blended meshes write their coverage and particles
   how much they change what's behind them (alpha blending its alpha, additive its
   brightness, multiply how far from white; opaque particles count fully, as they have no
   motion vectors). Where it's high, history counts for less, so smoke and glass don't
   leave trails.
4. **Resolve** (`Shaders/taa.comp`), at the internal resolution, before bloom and tone
   mapping. For each pixel: the nearest surface in the 3x3 around it gives the motion
   (from the prepass, or where there's no previous pose, the sky included, reprojected
   from depth as if it stood still); history is read there through a Catmull-Rom filter;
   it's dropped off screen, after a reset, and where something nearer covered that spot
   last frame; it's clipped to the 3x3's colour range (mean and one standard deviation,
   in YCoCg); then blended in at 90% at rest, down to 70% at four pixels of motion a
   frame, and towards 0 under the reactive mask.

History is the resolved image on the unjittered pixel grid, so it's read at `uv - motion`
(`resolvedHistoryUv`); the spec's formula with the two jitters (`previousHistoryUv`) is
for reading a raw previous frame, and is tested as such. Game UI is drawn after all of
this and never enters history.

## When history starts again

History is thrown away, and the next frame is the current one alone, when:

- the view changes: another `RenderViewId` (a new scene, play and stop, another camera
  in the player, the editor switching between its camera and the scene's);
- the render size changes (window size, render scale);
- the projection changes between perspective and orthographic;
- anti-aliasing or the debug view changes, or a frame went by without it;
- the game says the camera jumped.

A teleport or respawn moves the camera without any of those, and TAA would blend the old
view into the new one for a few frames. Tell it:

```cpp
game.cutCamera();   // GameContext: this frame's camera jumped
```

(`Renderer::cutCamera()` underneath.) Arena does this when the local player respawns.

`player --graphics-report` shows the last frame under `temporal`: whether motion and TAA
ran, the last reset and why (`"view changed"`, `"camera cut"`, ...), how many resets so
far, how many draws had a previous pose and how many were new, and the jitter. The
`anti_aliasing` setting's detail line repeats the last reset and the memory it holds.

## The debug views

**Motion** (`--view motion`): red and green are `0.5 + pixels / 32` of motion along x
(right) and y (down), as stored in the screenshot's bytes (so still reads 127 or 128, and
±16 pixels a frame is the range shown); blue is 255 where the draw's previous pose was
known and 0 where TAA will reproject from depth instead. The background is black.

**Reactive** (`--view reactive`): white where blended drawing fully replaced what was
behind, black where nothing blended was drawn.

Neither jitters the camera.

## Limits

- Surfaces whose shading changes without moving (a flickering emissive, a moving shadow
  on a still floor, a texture animating in place) have zero motion; colour clipping keeps
  their ghosting short but it can show for a few frames. Particles and blended meshes are
  covered by the reactive mask; animated emissive materials aren't yet.
- Orthographic views don't reject history by depth (there's no distance to compare).
- A first-person weapon drawn with its own projection isn't supported: it needs its own
  view.
- Draws without a `RenderInstanceKey` (`Renderer::unkeyedDraws`) and draws seen for the
  first time are treated as still and reprojected from depth.
- Textures aren't sampled at a finer mip level under TAA, so far-away texture detail is
  as soft as without it.
- TAA isn't an upscaler: with render scale below 1 it resolves at the lower resolution
  and the spatial scaler stretches the result, as before.
