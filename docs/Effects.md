# Particle effects

Particle effects are fire, smoke, sparks, magic, explosions and lasers. You make them in
the free [Effekseer](https://effekseer.github.io/) editor, save them as `.efkefc` files
in your project's `Content` folder, and place them in a scene. They reload when you save,
in the editor and in the running game.

Effekseer runs the simulation on a worker thread. Shockwave turns its particles into
plain triangles and draws them into the 3D scene with its own renderer, after solid
objects and before tonemapping. Because of that, effects work with every renderer
plugin, and survive a renderer swap or reload.

New to Shockwave? [Tutorial: your first game in Shockwave](Tutorial.md) walks through a
whole project first.

## Add an effect to a scene

1. In the editor, use Create > Particle effect. This adds an entity with a
   `ParticleEffect` component where the viewport is looking, playing the first `.efkefc`
   in `Content`.
2. The viewport plays it straight away, while you edit.
3. Pick another effect from the list in the Inspector, under the `ParticleEffect`
   component. Move, turn and scale the entity to place it.

A scene plays an effect through a `ParticleEffect` component on an entity with a
`Transform`. In a `.swscene` file it looks like this:

```text
entity "Laser"
  Transform
    position -5 1 -3
    rotation 0 90 0
  ParticleEffect
    effect "Effects/laser.efkefc"
    playing 1
    loop 1
    speed 1
    scale 0.12
    color 1 1 1 1
end
```

| Setting | Text name | What it does |
|---|---|---|
| Effect | `effect` | The `.efkefc` file, as a path inside `Content`. |
| Playing | `playing` | Runs the effect. Turning it off stops new particles; the live ones finish. |
| Loop | `loop` | Starts the effect again each time it ends. |
| Speed | `speed` | 1 is the speed it was made at, 2 is twice as fast, 0 freezes it. |
| Scale | `scale` | Makes the whole effect bigger or smaller, on top of the `Transform`'s scale. |
| Color | `color` | Multiplies every particle's color and alpha. White leaves it as made. |

The effect follows its entity as it moves. Particles already let go of stay where they
are, if the effect was made that way.

The sample project's `Scenes/effects.swscene` has two lasers, one tinted green and
slowed down. Its effect is `Games/Sample/Content/Effects/laser.efkefc`.

## Make an effect

1. Download the Effekseer editor from [effekseer.github.io](https://effekseer.github.io/).
   It runs on Linux, Windows and macOS, and is free.
2. Make your effect, or start from one of its samples.
3. Save it as an `.efkefc` file inside your project's `Content` folder, with its
   textures and models next to it or in a folder below it, such as `Content/Effects/`.

Effekseer works in its own units. If an effect looks far too big or too small, change its
`scale`. The sample's laser uses 0.12.

Older `.efkproj` files can't be played. Open them in the Effekseer editor and save them as
`.efkefc`.

## Play effects from game code

Game code reaches effects through `game.effects()`, during Play.

Play an effect once at a place in the world. It's gone when it ends:

```cpp
game.effects().play("Effects/explosion.efkefc", position);
```

`play` returns a handle, so you can move or stop the effect while it runs:

```cpp
auto beam = game.effects().play("Effects/laser.efkefc", muzzle, rotation);
game.effects().move(beam, muzzle, rotation);
game.effects().stop(beam);          // lets live particles finish
game.effects().stop(beam, true);    // removes them now
```

Play an effect at an entity's place with `playAt`. It doesn't follow the entity afterwards:

```cpp
game.effects().playAt("Effects/hit.efkefc", enemy);
```

Control an entity's `ParticleEffect` by the entity:

```cpp
auto ship = game.find("Ship");
game.effects().play(ship);          // starts its effect again from the first frame
game.effects().stop(ship);          // turns playing off; live particles finish
```

You can also change the component's settings directly, like any other component, for
example `ship.get<ParticleEffect>().speed = 2.0f`.

Effekseer effects can have triggers and dynamic inputs, which you set up in the
Effekseer editor. Game code sends them like this:

```cpp
game.effects().trigger(beam, 0);            // trigger 0 to 3
game.effects().setInput(beam, 0, 0.5f);     // input 0 to 3
game.effects().setSpeed(beam, 2.0f);
game.effects().setColor(beam, {1, 0.5f, 0.2f, 1});
```

To avoid a stall the first time an effect plays, load it in `beginPlay`:

```cpp
game.effects().preload("Effects/explosion.efkefc");
```

Calls with an unknown handle, or an entity without a running effect, do nothing and
return false. When Play stops, every effect game code started is removed.

Call `game.effects()` from the main thread: `beginPlay`, `frame`, `fixedUpdate` and
`endPlay`. Don't call it from systems, which run on worker threads.

## Check effects for mistakes

`player --check` loads every effect the way the game does and reports what's wrong:

```sh
build/bin/player --check
```

It checks each scene's `ParticleEffect`s, then every other `.efkefc` in `Content`,
because game code can play those too. You'll see lines like these:

```text
Scenes/main.swscene: entity 'Torch': ParticleEffect Effects/fire.efkefc: Missing texture Flame01.png (can't open file)
Scenes/main.swscene: entity 'Smoke': ParticleEffect has no effect file
warning Content/Effects/portal.efkefc: Uses distortion, which isn't drawn yet; those parts are skipped
```

It reports these problems, which fail the check:

- a `ParticleEffect` with no effect file, a missing file, or an `.efkproj`;
- a file Effekseer can't read;
- missing textures and models;
- a `ParticleEffect` without a `Transform`, a `scale` of 0 or less, or a negative `speed`.

Warnings name the parts of an effect that Shockwave doesn't draw yet (see Limits below).
They don't fail the check, because the rest of the effect still plays.

In the editor, the same problems appear in the `ParticleEffect`'s Inspector, errors in
red and warnings in yellow. The Console shows them as `LogEffects` messages.

## See effects from the command line

Screenshots include effects, drawn exactly as players see them:

```sh
build/bin/player Games/Sample/Sample.swproject --scene Games/Sample/Scenes/effects.swscene --screenshot fx.png --frames 30
```

`player --benchmark` reports what effects cost: their GPU pass, named `particles`, and
their CPU time.

```sh
build/bin/player --scene Games/Sample/Scenes/effects.swscene --benchmark 5 --json out.json
```

You'll see lines like these, and `effects_update_ms`, `effects_build_ms` and a
`particles` object in the JSON:

```text
  pass particles                6.835 ms
effects cpu:  0.153 ms simulation (worker) + 0.089 ms build (2 effects, 219 particles, 6 draws, 868 vertices)
particles:    152 average, 301 peak (cap 20000)
  simulate    0.153 ms (worker; main thread waited 0.045 ms)
  build quads 0.089 ms (main thread)
  upload      0.010 ms (main thread, 0.03 MB a frame)
  gpu draw    4.892 ms (particles pass)
```

(That's the software driver, so the GPU times are far higher than on a real GPU. The
`pass` line is the last frame; the `particles:` block is the average over the run.)

The `particles:` block splits the cost into the four stages a particle goes through
each frame, averaged over the run. "What it costs" below says what each one means.

## Reload effects

Every effect file and the textures and models it uses are watched. Save any of them and
the effects that use them reload within half a second. The Console logs
`Reloading Effects/laser.efkefc`. Scene effects start again from their first frame. This
works in the editor, during Play and in the player.

| To do this | In the editor | In a text file | On the command line |
|---|---|---|---|
| Add an effect to a scene | Create > Particle effect | a `ParticleEffect` block in the `.swscene` | |
| Pick the effect | Inspector > the effect list | `effect "Effects/laser.efkefc"` | |
| Edit an effect | Inspector > Show file, then open it in Effekseer | | |
| Play it again | Inspector > Restart | | |
| Reload everything | Inspector > Reload | | |
| Hide effects in the viewport | View > Particle effects | | |
| Check for mistakes | Inspector, Console | | `player --check` |
| See it | the viewport | | `player --screenshot out.png` |
| Measure it | Window > Stats > Particle effects, and the `particles` GPU pass | | `player --benchmark 5` |
| Stress test it | open `Scenes/particle-stress-16v16.swscene` | | `player --scene .../particle-stress-16v16.swscene --benchmark 20` |

## In the editor

While you edit, effects play in the viewport so you can see your changes. When Play
starts or stops, every effect starts again from its first frame, as it would in the game.
While Play is paused, effects freeze.

## What it costs

Measured on the sample's two lasers at 1280 x 720, on the software Vulkan driver
(llvmpipe), which is much slower than any real GPU:

| Part | Time per frame |
|---|---|
| GPU, `particles` pass | 0.41 ms |
| CPU, simulation (on a worker thread, alongside the rest of the frame) | 0.16 ms |
| CPU, building the triangles (main thread) | 0.08 ms |

With no effect on screen, the `particles` pass is left out of the frame, so it costs
nothing. Window > Stats shows the same numbers for your own effects while the editor
runs, under Particle effects and GPU time per pass.

Each frame, a particle goes through four stages, and `player --benchmark` times each:

| Stage | Where | What it is |
|---|---|---|
| simulate | a worker thread | Effekseer moves, spawns and ages every particle. The main thread only pays for it when it has to wait for the worker before building (`main thread waited`). |
| build quads | main thread | Each particle becomes a camera-facing quad: 4 vertices of 48 bytes. |
| upload | main thread | The renderer gathers the frame's quads and copies them into GPU memory. |
| gpu draw | GPU | The `particles` pass: every quad blended into the scene. Big, overlapping smoke costs the most here. |

### The particle stress test

`Games/Sample/Scenes/particle-stress-<N>v<N>.swscene` is a firefight: two teams of N
rifles behind cover, 30 m apart, every rifle firing about twelve rounds a second (muzzle
flash, flame, sparks, a tracer and smoke per shot), bullets landing on both lines of
cover (flash, sparks, dust), burning wrecks with smoke columns, and grenades going off.
The camera stands just behind one team, so the near flashes and smoke overlap the far
ones. `Games/Sample/Tools/MakeParticleStress.py` writes the scenes and the four
`firefight-*.efkefc` effects they use; change it and run it again rather than editing
the files (the effects carry no Effekseer editor data).

```sh
build/bin/player --project Games/Sample/Sample.swproject \
    --scene Games/Sample/Scenes/particle-stress-16v16.swscene \
    --benchmark 20 --vsync off --fps 0 --json out.json
```

In the editor, open the scene and look at Window > Stats > Particle effects.

Particles alive on average, measured with the null renderer (counts don't depend on the
GPU), and the CPU stages on the cloud build machine:

| Scene | Particles | Peak | simulate | build quads |
|---|---|---|---|---|
| 4v4 | 2,600 | 2,900 | 0.36 ms | 0.79 ms |
| 8v8 | 5,200 | 5,800 | 0.80 ms | 1.69 ms |
| 16v16 | 10,500 | 11,200 | 1.75 ms | 3.49 ms |
| 24v24 | 15,800 | 16,600 | 2.84 ms | 5.41 ms |
| 32v32 | 18,500 | 18,900 | 3.31 ms | 6.16 ms |

32v32 is at the limit: it wants more particles than fit, so some shots don't spawn
everything. In practice the pool fills at about 18,900 rather than 20,000, because
Effekseer hands out particles in blocks of 16 and counts the invisible "one shot"
emitters too.

How to read a run, to decide what to change first:

- **build quads and upload are most of the main thread's time**: build each particle's
  quad on the GPU instead (upload one small record per particle and expand it in the
  vertex shader). That removes most of the build cost and about three quarters of the
  bytes uploaded.
- **main thread waited is large, or you need more than 20,000 particles**: the
  simulation itself is the limit, which only moving it to compute shaders on the GPU
  fixes.
- **gpu draw is the biggest number**: it's the pixels, not the particle count. Fewer or
  smaller blended quads (smoke especially), or drawing particles at a lower resolution,
  help; neither change above does.

To keep frames steady, effects don't allocate memory each frame once they've settled.
Effekseer keeps its particles in a fixed pool, and the triangle lists and GPU buffers are
reused, growing only when a frame needs more. At most 20,000 particles are alive at once,
across every effect. Past that, new particles aren't spawned.

## Limits

- Distortion (heat haze, refraction) isn't drawn yet. Those parts of an effect are
  skipped, and `player --check` warns about them.
- Custom materials made in Effekseer's material editor (`.efkmat`) aren't drawn yet.
- Falloff, UV distortion, blend textures and normal maps are drawn without those touches.
- Effekseer's GPU particles aren't drawn yet.
- Sounds inside an effect aren't played. Put an `AudioSource` next to the effect instead.
- Soft particles (fading where particles meet solid geometry) aren't done yet.
- Particles are lit by the scene's first directional light and its ambient light. Point
  and spot lights don't light them.
- Effects are sorted back to front as a whole, so two blended effects in the same place
  can overlap in the wrong order.

## How it works

For people changing the engine:

- `Source/Runtime/Effects` is the module. `Effects` owns an Effekseer `Manager` with
  every running effect. The editor and the player each make one, and call `update()`
  and `render()` once per frame. `EffectsWorld` is `game.effects()`, one per Play session.
- `update()` matches the running effects to the world's `ParticleEffect`s, checks files
  for changes, and starts the frame's simulation as a job on the job system. `render()`
  waits for that job, then builds the triangles. Every other call waits for it too, so
  Effekseer is only ever used by one thread at a time.
- `EffekseerBridge` is a "capture" renderer. Effekseer's own sprite, ribbon, ring, track
  and model renderers write their vertices into it on the CPU, and it turns them into a
  `ParticleDrawList` (`Renderer/Particles.h`): world-space vertices, indices, and one
  batch per texture and blend mode.
- `Renderer::drawParticles` hands that list to the renderer plugin. The Vulkan renderer
  draws it in a `particles` render graph pass, after the `scene` pass, into the HDR scene
  color with depth testing (`RendererParticles.cpp`, `particle.vert`, `particle.frag`).
  The null renderer ignores it.
- Effekseer's textures are loaded with Shockwave's image loader into the renderer's
  bindless texture table. Effekseer can free a texture during the simulation job, so the
  handle is queued and freed on the main thread at the next `update()`.
- Effekseer counts time in frames at 60 per second. The simulation is given the real frame
  time in those units, so effects run at the same speed at any frame rate.
- `cmake/Effekseer.cmake` builds only Effekseer's runtime, from release 1.7.3.0. It's MIT
  licensed; the license is copied to `build/bin/licenses/Effekseer-LICENSE.txt`.

The sample's `laser.efkefc` is Effekseer's `Laser01` example, under the same MIT license.
Its three textures in `Content/Effects/Texture/` are drawn by
`Games/Sample/Tools/GenerateEffectTextures.py` and are part of this project, also under
MIT.
