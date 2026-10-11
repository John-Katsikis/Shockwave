# 7. What happens in one frame

**In this chapter:** the engine's main loop, step by step, and why game rules run on a
fixed tick while drawing runs as fast as it can.

## Two clocks

Your screen might refresh 60 times a second, or 144, or 240. If the game moved things a
fixed amount each frame, it would run four times faster on a 240 Hz monitor. If it moved
things by "speed times frame time", it would be fairer, but physics and collisions
would still come out slightly different on every machine, because tiny time steps and
big ones don't add up the same way.

So Shockwave keeps two clocks:

- The **fixed tick** runs the simulation at a steady rate: the project's `tick_rate`,
  60 times a second by default. Every tick is exactly 1/60 of a second long, on every
  computer. Game rules, systems and physics live here.
- The **frame** runs as fast as the screen allows (or a frame limit you set). Drawing,
  cameras, UI and input presses live here.

On a fast machine there might be two or three frames per tick. On a slow one there might
be two ticks in one frame, to catch up. Either way, the game plays the same.

## Smooth motion between ticks

If the screen draws three frames for every tick, things would visibly jump once per
tick. To hide that, the renderer **blends**: it remembers every `Transform` from the
previous tick, and draws each object part of the way between where it was and where it
is now. That fraction is called `alpha`. At 240 fps you see silky motion, even though
the simulation only moved 60 times.

That's why chapter 5's movement felt slightly wrong: it changed the Transform in
`frame`, outside the tick, so the blending worked against it. Chapter 8 moves it into
the tick.

## The loop, step by step

This is `Engine::run`, in `Source/Runtime/Engine/Private/Engine.cpp`, in plain words.
Each numbered step happens once per frame.

1. **Swap the renderer if asked.** If you rebuilt the renderer plugin, it's swapped
   here, between frames, where nothing is being drawn (chapter 14).
2. **Wait for the right moment.** The frame limiter sleeps until it's time for the next
   frame. In low-latency mode, it also waits until the last frame is actually on
   screen, so input is read as late as possible.
3. **Read input.** Every window, keyboard, mouse and controller event since last frame
   is handed to the input system (and to the editor UI).
4. **Count how much time passed**, and add it to a bucket called the *accumulator*.
5. **Run fixed ticks.** While the bucket holds at least one tick's worth of time, run
   one tick and take that time out. During Play, one tick is:
   1. remember every Transform (for blending);
   2. run the schedule: built-in systems and yours, in parallel stages;
   3. steer navigation agents;
   4. step the physics;
   5. call your GameMode's `fixedUpdate`;
   6. hand the audio thread a snapshot of where sounds and the listener are.
6. **Run the frame hook.** Your GameMode's `frame`, plus game UI, effects and animation.
7. **Begin the GPU frame.** If the window is minimized, skip drawing and nap.
8. **Build the editor UI** (editor only). Every panel is drawn in code, every frame
   (chapter 15).
9. **Gather what to draw.** Every visible mesh, model and light is collected, in
   parallel, at its blended position.
10. **Draw and present.** The renderer records the GPU work and shows the finished
    picture (chapter 13).
11. **Record timing.** CPU time, GPU time and present time go into the frame
    statistics behind **Window > Stats** and `player --benchmark`.

One more safety rule: if a frame takes very long (you hit a breakpoint, say), at most a
quarter of a second is added to the bucket. Otherwise the game would try to run hundreds
of ticks at once to catch up, fall further behind, and never recover.

## Who calls what

The engine doesn't know about editors or games. It calls the hooks of whatever
`Application` it's running:

| Engine step | Application hook | The editor does | The player does |
|---|---|---|---|
| each fixed tick | `onUpdate(dt)` | runs the Play session if playing | runs the Play session |
| each frame | `onFrame(dt)` | game UI, effects, animation, hot reload checks | the same, minus the editor parts |
| UI | `onGui()` | draws every panel | nothing: the player has no editor UI |
| drawing | `onRender(renderer, alpha)` | the scene from the editor or game camera | the scene from the game camera |

And during Play, the **PlaySession** turns those into your GameMode's hooks:
`onUpdate` becomes systems, navigation, physics and `fixedUpdate`, and `onFrame` becomes `frame`.

## Which threads do what

- The **main thread** owns the window, the UI, loading scenes, starting and stopping
  Play, and every GameMode hook.
- The **job system** (one worker per CPU core) runs systems, gathers draws, decodes
  assets and steps physics.
- **Audio** mixes on its own thread, from the snapshot taken each tick.

`SW_LOG` and `Renderer::drawMesh` are safe to call from any thread.

## Recap

- Game rules run on a **fixed tick** (60 a second by default); drawing runs every
  **frame**.
- The renderer blends between the last two ticks, so motion is smooth at any frame
  rate.
- One frame: pace, read input, run ticks, frame hook, UI, gather, draw, record stats.
- The main thread runs your hooks; systems and drawing fan out across every core.

Next: [Physics: solid ground and a walking player](08-physics.md).
