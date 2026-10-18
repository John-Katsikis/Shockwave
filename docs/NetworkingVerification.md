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

## Batch 2 (2026-10-18)

Base: `075f917` (master after PR #19), branch `claude/steam-multiplayer-batch-0-1-e551hk`.

Implemented:

- `Multiplayer` module after Physics (`Shockwave::Engine`, `Shockwave::Network`):
  `NetworkSettings.h` (`Config/Network.swnetwork`, one descriptor table for parser,
  writer, validator, editor and schema), `NetworkManifest.h` (`.swnetmanifest`, SHA-256
  content digest), `NetworkComponents.h` (`NetworkObject`, `NetworkSpawnPoint`, stable
  keys), `Replication.h` (registry, freeze, generic profile codec, `engine.pose.v1`,
  `engine.rigidbody.v1`), `NetworkValidation.h` (`--check` rules, `readyForSession`),
  `NetworkSchema.h` (generated `docs/NetworkSchema.md`, `--network-info`). SHA-256 in
  Network (`Network/Sha256.h`).
- Overrides: `tick_rate` 60..120 (design adjustment 1) and `max_players` 2..64 (design
  adjustment 5, which also widens the wire's player slots and transport connections).
- `GameModule::registerNetworkTypes` and `SW_NETWORK_TYPES`; `kPluginAbiVersion` 2.
  Arena registers `arena.player.v1` (77 bytes), `arena.rules.v1` (13 bytes), three
  commands and three events, and ships `Config/Network.swnetwork` (disabled) and
  `Config/NetworkManifest.swnetmanifest`.
- `player --check` network rules, `--network-info [--json]`, `--network-schema`.
- Editor: Project settings > Multiplayer (rows from the descriptor table, live rules,
  atomic Save, Revert, Check network setup); keys for new NetworkObjects, duplicates and
  placed prefab copies.

Results, in the same headless Linux container as Batch 1:

| Configuration | Result |
|---|---|
| Debug, modular, after the change | builds (player, editor, all games and tests), no warnings; CTest 35/37 |
| Debug, static (`-DSHOCKWAVE_MODULAR=OFF`), after | builds everything; CTest 34/34 |
| `network_*` tests and `player --check` on Arena with GCC `-fsanitize=address,undefined` | `network_settings`, `network_wire`, `network_transport`, `network_capture` pass; `player --check`, `--network-info` and the `network_cli` script on Arena pass; no sanitizer reports |
| Clang 18 `-fsyntax-only -Wall -Wextra -Wpedantic -Wshadow` | Sha256, NetworkSettings, NetworkManifest and Replication: no warnings. The files that include Renderer headers can't be compiled by Clang 18 with GCC 13's libstdc++ (`Renderer/Mesh.h:41`, a vector of the incomplete `MeshLod`, already on master); macOS CI compiles them with AppleClang |

The modular failures are the two from Batch 1 (`renderer_swap`, `save_games_cli`: no
display in the container).

Gate evidence:

- `ctest -R network_settings --output-on-failure`: passes. It covers every settings
  range including `tick_rate` 59/60/120/121 and `max_players` 1/2/64/65, non-finite
  numbers, unknown and duplicate keys with line numbers, transactional reads, atomic
  writes, SHA-256 FIPS 180-2 vectors, a content digest computed independently with
  Python `hashlib`, CRLF equivalence, links, manifest rules, registry ranges and freeze,
  the generic pose codec byte-equal to `encodePose`, scene rules, keys, and
  `readyForSession` refusing a missing, disabled or unfrozen setup before any transport
  exists.
- Zero heap allocations (counting `operator new`) over 1,000 ticks of registry lookup,
  capture, encode, decode and apply after freeze.
- `build/bin/player --project Games/Arena/Arena.swproject --check`: `ok
  Config/Network.swnetwork`, `0 problems in 2 scenes`, exit 0.
- `network_cli` (on a copy of Arena): each bad setting, manifest entry and scene network
  object fails `--check` with file:line; 120 Hz in both files passes; `--network-info`
  text and JSON; mode combinations exit 2.
- Schema regeneration: `player --network-schema -` matches the checked-in
  `docs/NetworkSchema.md` in both builds (`network_cli` fails otherwise); regenerating
  `docs/SceneSchema.md`, `Games/Arena/Docs/SceneSchema.md` and `docs/NetworkProtocol.md`
  twice gives no further diff.
- Arena's digests are the same in the modular and static builds: schema
  `d4e8cf98...a03361`, content `dc0c39bf...54dc4f2` (11 files).

Outstanding for this batch:

- The editor's Multiplayer section and key assignment are built but weren't looked at:
  the container has no display. Needs a visual check on a desktop.
- "Assign missing network keys" (editor and `--assign-network-keys`) is deferred to
  Batch 9 (design adjustment 10).
- `Games/RenderLab/Docs/SceneSchema.md` wasn't regenerated: RenderLab isn't in the
  default `SHOCKWAVE_GAMES`, so its module wasn't built.
- Session-immutable fields can't be disabled "while online" yet; there are no sessions
  until Batch 4.
- Not built on macOS or Windows here; CI covers those on push.
