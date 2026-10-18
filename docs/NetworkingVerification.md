# Networking verification

What has actually been run for each batch of
[SteamMultiplayerImplementation.md](SteamMultiplayerImplementation.md), and what is
still outstanding. Planned tests are never listed as passed.

## Batches 0 and 1 (2026-10-18)

Base: `3182ff7` (master after PR #18), branch `claude/steam-multiplayer-batch-0-1-e551hk`.

Implemented:

- Batch 0: `Network` module after Core (`Shockwave::Core` only), with `NetworkTypes.h`,
  `Transport.h`, `OnlineServices.h` and an Unavailable online service. No Steam SDK is
  searched for, downloaded or linked; the default configure is unchanged.
- Batch 1: `Wire.h`, `Messages.h` (32-byte header, all 21 message kinds, 24-byte input
  command, state records, pose profile), `Loopback.h` (`LoopbackHub`, `FaultLink`,
  presets clean/cafe/poor/hostile, outages, bandwidth), `NetworkCapture.h` (`.swnetcap`,
  JSON dump and encode), generated `docs/NetworkProtocol.md`.
- Tick-rate override: protocol v1 accepts 60..120 Hz (design adjustment 1).

Build configurations, all in a headless Linux cloud container (Ubuntu, GCC 13, 4 cores,
no GPU or display; SDL configured with `-DSDL_X11=OFF -DSDL_WAYLAND=OFF
-DSDL_UNIX_CONSOLE_BUILD=ON` because the X11/Wayland development packages aren't
installed there):

| Configuration | Result |
|---|---|
| Debug, modular (default), before the change | builds; CTest 30/32 |
| Debug, modular, clean rebuild after the change | builds, no warnings; CTest 33/35 |
| Debug, static (`-DSHOCKWAVE_MODULAR=OFF`), after | builds, including player and editor; CTest 31/32 |
| `network_*` tests with GCC `-fsanitize=address,undefined` | 3/3 pass, no reports |
| `network_*` tests with Clang 18, `-Wall -Wextra -Wpedantic -Wshadow` | 3/3 pass, no warnings |

The failures are the same before and after, and come from the container having no
display: `renderer_swap` (`SDL_CreateWindow` needs `VK_KHR_surface`; modular only) and
`save_games_cli` (`SDL_Init`: no video device). Neither touches networking.

Network tests: `network_wire`, `network_transport`, `network_capture` pass in both the
modular and static builds. Golden bytes come from `Tests/fixtures/network_golden.py`
(Python `struct`), not the C++ writer. The wire fuzz pass is 200,000 seeded inputs
(random and mutated valid messages); allocation counters show zero heap allocations
while encoding/decoding and while sending/polling 7,200 ticks over a `poor` loopback
link after connecting.

Outstanding for these batches:

- No Steamworks SDK was supplied: the SDK version is unknown and every Steam compile
  and manual gate is open (Batch 3 and 10).
- Baseline Arena benchmark (`player --benchmark`) not run: the container has no GPU,
  and the desktop wasn't used to avoid unrequested engine runs.
- Not built or run on macOS or Windows here; CI covers those on the next push.
- Not run on a desktop with a display, so `renderer_swap` and `save_games_cli` weren't
  rechecked on real hardware.
