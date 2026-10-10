# Frame pacing and profiling

Shockwave aims for frames that arrive evenly, not just quickly. A steady 60 fps feels
smoother than an average of 90 with hitches. So the engine measures the slowest frames,
not only the average, and gives you settings to trade speed for smoothness or low input
lag.

## Change the frame rate limit, vsync or latency

Each setting can be changed in the editor, in the `.swproject` file and on the player's
command line.

| Setting | Editor | `.swproject` | Command line |
|---|---|---|---|
| Vsync | Preferences > VSync; Project settings > Player VSync | `vsync true` | Player: `--vsync on\|off`. Editor: `--uncapped` turns vsync off for that run. |
| Frame limit | Preferences > Frame limit; Project settings > Player frame limit | `frame_limit 144` (0 = none, else 10 to 1000) | Player: `--fps 144` |
| Low latency | Preferences > Low latency; Project settings > Player low latency | `low_latency true` | Player: `--low-latency` |

Vsync waits for the screen's refresh before showing each frame, which stops tearing.

Preferences apply to the editor itself. Project settings are what the player uses when
it runs your game. A player command-line flag overrides the project for that one run.

```sh
./build/bin/player --fps 120 --low-latency
```

This runs the game capped at 120 fps with low latency on. With no project given, the
player uses the one in or above the working folder.

## What each setting does

**Frame limit** holds frames to a fixed rate.

- The limiter sleeps until about a millisecond before the next frame is due, then waits
  in a busy loop for the rest. Frames land within microseconds of their slot.
- Each frame is timed against a fixed schedule, so small errors never add up into drift.
- After a long stall, like loading or a breakpoint, the schedule restarts. It doesn't
  rush frames to catch up.
- With vsync on as well, whichever is slower wins.

**Low latency** makes input feel as fresh as possible.

- It waits for the previous frame to reach the screen before reading input for the next
  frame.
- With vsync on, it also uses the `FIFO_LATEST_READY` present mode when the driver
  offers it. That mode shows the newest finished frame at each refresh, instead of
  queueing frames up.
- It costs a little speed, so it's off by default.

**Smoothed time step.** The frame time your game sees is not the raw measurement.

- One frame far off the recent median is pulled back toward it.
- Frame times within 5% of the refresh period or the frame limit period are snapped to
  exactly that period. So motion moves by exactly one refresh's worth per refresh.
- The fixed simulation tick is not affected.

## See frame times in the editor

Open **Window > Stats**. It draws the last 300 frames as a graph, showing frame time,
GPU time, and the frame limit as a line. Hover the graph for exact values.

It also lists:

- the average frame rate, the 1% and 0.1% lows, and the worst frame
- jitter, CPU time and GPU time
- the present mode and how presentation is timed
- the GPU time of every render graph pass (each drawing step of a frame)

## Benchmark from the command line

```sh
./build/bin/player --benchmark 10 --json bench.json
```

This warms up for a second, then measures for 10 seconds in a hidden window with vsync
off. It prints a report, writes the same data to `bench.json`, and quits.

`--benchmark` takes the number of seconds, 10 by default. `--json file` is optional and
useful for scripts and CI. Add `--fps`, `--low-latency`, `--vsync on` or `--scene` to
measure a particular setup:

```sh
./build/bin/player --scene Games/Sample/Scenes/physics.swscene --benchmark 5 --fps 120 --low-latency
```

The report looks like this:

```text
benchmark: Scenes/main.swscene, 600 frames over 10.0 s
gpu:          ...
presentation: mailbox (uncapped, no tearing); present wait 2
frame limit:  none; low latency off
average:      ...
1% low:       ...
0.1% low:     ...
worst frame:  ...
jitter:       ...
cpu:          ...
gpu time:     ...
  pass scene                    ...
  pass tonemap                  ...
```

| Line | Meaning |
|---|---|
| `1% low` | The average frame rate of the slowest 1% of frames |
| `0.1% low` | The same for the slowest 0.1% |
| `jitter` | The standard deviation of frame time: how much frame times vary |

The JSON has the same fields: `average_fps`, `low_1_percent_fps`,
`low_0_1_percent_fps`, `worst_ms`, `jitter_ms`, `cpu_ms`, `gpu_ms`, `passes`, and more.

## Profile with Tracy

Tracy is a profiler that shows a timeline of what every thread does each frame. To use
it, build with profiling turned on:

```sh
cmake -B build-profile -DSHOCKWAVE_PROFILE=ON
cmake --build build-profile -j
```

This makes a separate build in `build-profile`. `SHOCKWAVE_PROFILE` is off by default.
Turning it on downloads [Tracy](https://github.com/wolfpld/tracy) v0.11.1.

Run the editor or player from that build, then connect the Tracy profiler app to it.
Tracy only collects data while a profiler is connected.

Built-in zones (named, timed sections) cover:

- each stage of the frame: waiting for the GPU, acquiring the next image, the fixed
  tick, the application's frame, gathering draws, recording and submitting
- each render graph pass
- each ECS system, by name
- the job system's worker threads

Add your own zones with the macros in `Core/Profile.h`. They compile to nothing when
profiling is off:

```cpp
#include <Core/Profile.h>

void MyGame::updateAi() {
    SW_PROFILE_SCOPE("AI");
    ...
}
```

## Use the pieces in code

`Core/FramePacing.h` has the building blocks, which you can use on their own:

| Type or function | What it does |
|---|---|
| `FrameLimiter` | Holds frames to a fixed rate |
| `FrameTimeSmoother` | Smooths the time step |
| `FrameStats`, `summarizeFrames` | Compute the statistics |

The engine drives them for the main loop with `Engine::setFrameLimit`,
`Engine::setLowLatency` and `Engine::frameStats()`. `Renderer::passTimings()` gives the
GPU time of each pass from the last finished frame.

## How presentation is timed

At startup the log says what your GPU and driver support, in a line starting with
`frame pacing: ...`.

- With `VK_KHR_present_id2` and `VK_KHR_present_wait2`, or the older `present_id` and
  `present_wait`, the engine can wait until a given frame is actually on screen. Low
  latency uses this, and the Stats window shows the real time between frames reaching
  the screen.
- Without present wait, as on some drivers and software renderers, low latency waits for
  the GPU to finish the previous frame instead. On-screen timing is then estimated from
  the CPU.
- `VK_EXT_present_timing`, which schedules a frame for an exact time, isn't used yet. The
  Vulkan headers the engine builds against don't have it.
