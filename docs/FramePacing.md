# Frame pacing and profiling

Shockwave aims for frames that arrive evenly, not just quickly. A steady 60 fps feels
smoother than an average of 90 with hitches, so the engine measures the slow tail of
frames (1% and 0.1% lows) as well as the average, and every setting below can be changed
from the editor, the `.swproject` manifest and the player's command line.

## Settings

| Setting | Editor | `.swproject` | Player flag |
|---|---|---|---|
| Vsync | Preferences > VSync; Project settings > Player VSync | `vsync true` | `--vsync on\|off`, `--uncapped` (editor) |
| Frame limit | Preferences > Frame limit; Project settings > Player frame limit | `frame_limit 144` (0 = none, else 10 to 1000) | `--fps 144` |
| Low latency | Preferences > Low latency; Project settings > Player low latency | `low_latency true` | `--low-latency` |

Editor preferences apply to the editor itself; the project settings are what the
player uses when it runs the game. A player flag overrides the project for that run.

- **Frame limit** holds frames to a fixed rate with absolute deadlines: the limiter
  sleeps until about a millisecond before each frame is due, then spins the rest, so
  frames land within microseconds of their slot and small errors never add up into
  drift. After a long stall (loading, a breakpoint) the schedule restarts instead of
  rushing frames to catch up. With vsync on as well, the slower of the two wins.
- **Low latency** waits for the previous frame to reach the screen before reading input
  for the next one, so input is as fresh as possible when the frame is shown. With vsync
  it also uses the `FIFO_LATEST_READY` present mode when the driver offers it, which
  shows the newest finished frame at each refresh instead of queueing them. It costs a
  little throughput, so it's off by default.
- **Smoothed delta.** The time step games see is not the raw frame time: one frame far
  off the recent median is clamped towards it, and frame times within 5% of the refresh
  or limiter period are snapped to it exactly, so motion advances by exactly one refresh
  per refresh. The fixed simulation tick is unaffected.

## How presentation is timed

At startup the log says what the GPU and driver offer, as `frame pacing: ...`:

- `VK_KHR_present_id2` / `VK_KHR_present_wait2` (or the older `present_id` /
  `present_wait`): the engine can wait until a given frame is actually on screen. Low
  latency uses this, and the Stats window reports the real interval between frames
  reaching the screen.
- Without present wait (some drivers, software renderers), low latency waits for the GPU
  to finish the previous frame instead, and on-screen timing is estimated from the CPU.
- `VK_EXT_present_timing` (scheduling a frame for an exact time) isn't used yet: the
  Vulkan headers the engine builds against don't have it.

## Seeing it

**Editor:** **Window > Stats** draws the last 300 frames as a graph (frame time, GPU time,
and the frame limit as a line; hover for exact values) and lists the average, 1% and
0.1% lows, worst frame, jitter, CPU and GPU time, the present mode and how it's timed,
and the GPU time of every render graph pass.

**Command line:** `player --benchmark [seconds]` warms up for a second, measures for the
given time (default 10) in a hidden window with vsync off, prints a report and quits.
`--json file` also writes it as JSON for scripts and CI. Combine it with `--fps`,
`--low-latency`, `--vsync on` and `--scene` to measure a particular setup.

```sh
./build/bin/player --benchmark 10 --json bench.json
./build/bin/player --scene Games/Sample/Scenes/physics.swscene --benchmark 5 --fps 120 --low-latency
```

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

`1% low` is the frame rate of the slowest 1% of frames averaged together; `jitter` is
the standard deviation of frame time. The JSON has the same fields
(`average_fps`, `low_1_percent_fps`, `low_0_1_percent_fps`, `worst_ms`, `jitter_ms`,
`cpu_ms`, `gpu_ms`, `passes`, ...).

## Profiling with Tracy

For a timeline of what every thread does each frame, build with
`-DSHOCKWAVE_PROFILE=ON` (off by default; it fetches [Tracy](https://github.com/wolfpld/tracy)
v0.11.1), run the editor or player, and connect the Tracy profiler to it. Tracy only
collects data while a profiler is connected.

```sh
cmake -B build-profile -DSHOCKWAVE_PROFILE=ON
cmake --build build-profile -j
```

Zones cover the frame's stages (waiting for the GPU, acquire, fixed tick, the
application's frame, gathering draws, recording and submitting), each render graph pass,
each ECS system by name, and job system workers by thread. Add your own with the macros
in `Core/Profile.h`, which compile to nothing when profiling is off:

```cpp
#include <Core/Profile.h>

void MyGame::updateAi() {
    SW_PROFILE_SCOPE("AI");
    ...
}
```

## In code

`Core/FramePacing.h` has the pieces, usable on their own: `FrameLimiter`,
`FrameTimeSmoother`, and `FrameStats` / `summarizeFrames` for the statistics.
`Engine::setFrameLimit`, `Engine::setLowLatency` and `Engine::frameStats()` drive them
for the main loop, and `Renderer::passTimings()` gives the GPU time of each pass from
the last finished frame.
