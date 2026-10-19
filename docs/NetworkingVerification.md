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

## Batch 3 (2026-10-19)

Base: `039b897` (master after PR #20), branch `claude/steam-multiplayer-batch-0-1-e551hk`.

Implemented:

- `SteamServices` module after Network (`Shockwave::Network` only), linked by the Player
  and Editor. `SteamServices` (PIMPL `SteamServicesImpl`): `initializeClient()`,
  `initializeDedicatedServer()`, `framePass()`, `shutdown()` exactly once, lobbies,
  session tickets and auth sessions with the 15-second fail-closed timeout.
  `SteamNetworkAdapter` (PIMPL `SteamNetworkAdapterImpl`): the section 4.4 state table,
  poll group, 32-message receive batches into a preallocated ring with every Steam
  message released, ReliableNoNagle and UnreliableNoNagle sends limited to
  `max_snapshot_bytes`, 32 KiB unreliable backpressure, broadcast, RTT.
- `SteamApi` facade (no SDK types), its SDK backend `Private/SteamSdk.cpp` (compiled only
  with `SHOCKWAVE_ENABLE_STEAM`), the SDK-off factory `SteamUnavailable.cpp`, and
  `MockSteamApi`/`MockSteamNetwork`.
- `cmake/Steamworks.cmake`: `SHOCKWAVE_STEAMWORKS` (off), `SHOCKWAVE_STEAMWORKS_SDK`,
  `SHOCKWAVE_STEAM_TESTS`; imported target, runtime copied to `build/bin`, target
  architecture check.
- `IOnlineServices` authentication calls and events (design adjustment 13).
- `player --steam-status [client|server]`; editor Project settings > Multiplayer > Start
  Steam / Stop Steam; `--network-info` and `networkBuild()` report the SDK.
- Tests `test_steam_services` and `test_sdr_transport_mock`.

Results, in the same headless Linux container as Batches 1 and 2:

| Configuration | Result |
|---|---|
| Debug, modular, after the change | builds (player, editor, all games and tests), no warnings; CTest 37/39 |
| Debug, static (`-DSHOCKWAVE_MODULAR=OFF`), after | builds everything, no warnings; CTest 26/27 |
| `test_steam_services`, `test_sdr_transport_mock`, `network_transport` with GCC `-fsanitize=address,undefined` (leak checking on) | pass, no sanitizer reports |
| Clang 18 `-fsyntax-only -Wall -Wextra -Wpedantic -Wshadow` on every SteamServices source and both tests | no warnings |
| Configure with `SHOCKWAVE_STEAMWORKS=ON`: no SDK path; a folder without `public/steam/steam_api.h`; a folder with the header but no runtime; `SHOCKWAVE_STEAM_TESTS=ON` without Steamworks | each fails configure with a message saying what to fix |

The failures are the known no-display ones: `renderer_swap` and `save_games_cli` in the
modular build, `save_games_cli` in the static one (`SDL_Init failed: No available video
device`, from `player --load`; nothing networking).

Gate evidence:

- `ctest -R "network_|test_steam|test_sdr" --output-on-failure`: 7/7 pass in both builds.
- The real factory in this SDK-less build: `steamBuild().sdk` is false and
  `initializeClient()` fails with Unavailable naming `-DSHOCKWAVE_STEAMWORKS=ON`; the
  rest of `test_steam_services` and all of `test_sdr_transport_mock` run on
  `MockSteamApi`.
- Fake-backend failures (plan section 14.1): init failure, listen socket failure (its
  poll group is destroyed again), poll-group failure at listen, connect and on
  Connected, every SDK send result, release of every message on every error path
  (empty, oversize, closed connection, a throwing sink), stale and foreign callbacks,
  state-change queue overflow, auth cancel and revocation, the auth timeout at 15 s and
  not at 14.9 s, late lobby results after cancel and after a restart, shutdown exactly
  once with transports alive.
- Allocation check: 0 heap allocations (counting `operator new`) over a continuous
  10,000-message poll loop (four clients, mixed reliable and unreliable messages up to
  1152 bytes, the host echoing every tenth back reliably), with every received Steam
  message released (received == released, none outstanding). An idle `framePass()`
  allocates nothing over 1,000 frames. Checked that the test fails when one allocation
  or one missed release is put into `poll()`.
- `player --project Games/Arena/Arena.swproject --steam-status`: exit 1, `steam_app_id
  is 0`; with `steam_app_id 480` (in `network_cli`'s copy): exit 1, `build steamworks
  no` and `steam unavailable: this build has no Steamworks SDK`. `--steam-status` with
  another mode exits 2. Arena's settings are unchanged (`enabled 0`, AppID 0).

Steam build with SDK v1.65 (Yianni's unzipped SDK, copied into the cloud container for
the build only; nothing from it is in the repository):

| Configuration | Result |
|---|---|
| Debug, modular, `-DSHOCKWAVE_STEAMWORKS=ON -DSHOCKWAVE_STEAMWORKS_SDK=<sdk>`, GCC 13 | configure reports `Steamworks SDK v1.65`; builds everything, no warnings, `libsteam_api.so` copied to `build/bin` and linked by player and editor; CTest 37/39 (the same two no-display failures) |
| GCC and Clang 18 `-fsyntax-only -Wall -Wextra -Wpedantic -Wshadow` on `SteamSdk.cpp` with the 1.65 headers | no warnings |

- The first compile against the real headers failed: `CSteamID(uint64_t)` is ambiguous
  on Linux, where `uint64_t` is `unsigned long` and the SDK's `uint64` is
  `unsigned long long`. Fixed with one conversion helper. `capabilities().steamworksBuilt`
  now reports whether the service runs on the SDK, so a mock in a Steam build says no.
- `ctest -R "network_|test_steam|test_sdr"`: 7/7 pass; in this build `test_steam_services`
  checks `steamBuild().sdk` is true.
- `player --steam-status` and `--steam-status server` with `steam_app_id 480` and
  `steam_appid.txt`, with no Steam client in the container: the SDK prints that Steam
  isn't running, the player exits 1 with `build steamworks yes, SDK v1.65`, `steam
  unavailable: Steam didn't start: is the Steam client running and signed in?` (client)
  and `the Steam game server didn't start` (server). Signing in for real needs a desktop
  with Steam running.

Outstanding for this batch:

- Tested only against Steamworks SDK v1.65 (Readme "v1.65 23th July 2026") on Linux
  x86_64; see "Steam build with SDK v1.65" below. Not built with the SDK on Windows,
  macOS or Linux ARM64 (the `linuxarm64` runtime is picked by configure but untested).
- The two-account manual gate (section 14.3: friends host, join, invite, auth denial,
  relay route status, Windows to Linux) is blocked: it needs the SDK, a test AppID and
  two Steam accounts. `-DSHOCKWAVE_STEAM_TESTS=ON` adds a `steam_status` CTest for it.
- The editor's Steam rows weren't looked at: the container has no display.
- `SteamAPI_RestartAppIfNecessary` and packaging the Steam runtime next to a packaged
  game wait for Batch 9.
- Not built on macOS or Windows here; CI covers those on push.

## The owner's "Batch 4" (2026-10-19): delta snapshots, interpolation, rewind history

Base: `2a9703f` (master after PR #21), branch `claude/steam-multiplayer-batch-0-1-e551hk`.
The owner's Batch 4 brief; see design adjustments 16 to 21 for how it maps onto the plan.
Plan Batch 4 (session integration) is the next section.

Implemented:

- Wire messages `DeltaSnapshot` (22) and `SnapshotAck` (23) in protocol v1, with golden
  packets, sample cases and the regenerated [NetworkProtocol.md](NetworkProtocol.md).
- `Multiplayer/SnapshotDelta.h`: `NetEntityState` (32 bytes, `alignas(16)`),
  quantization, `SnapshotHistory`, `SnapshotDeltaEncoder` (shared, one encode per
  distinct baseline per tick), `SnapshotDeltaCompressor` (per-client acks),
  `SnapshotDeltaDecoder` (staged, zero-copy from the received payload).
- `Multiplayer/Interpolation.h`: `InterpolationEngine`, the server clock and the jitter
  buffer.
- `Multiplayer/RewindHistory.h`: `RewindHistoryBuffer`, `queryAtTimestamp`, raycasts,
  `rayCapsule`.
- Settings: `snapshot_rate` 120, `interpolation_ms` from 25, and a `--check` warning
  under two snapshot intervals; [NetworkSchema.md](NetworkSchema.md) regenerated.
- Tests `test_snapshot_delta`, `test_rewind_buffer`, `test_replication_allocations`.

Results in the headless Linux container (GCC 13):

| Configuration | Result |
|---|---|
| Debug, modular | builds everything, no warnings; CTest 40/42 (the known no-display failures `renderer_swap` and `save_games_cli`); `-R "network_\|test_steam\|test_sdr\|test_snapshot\|test_rewind\|test_replication"` 10/10 |
| Release (`-O3`), the three new tests | pass, no warnings |
| Debug with `-fsanitize=address,undefined`: `test_snapshot_delta`, `test_rewind_buffer`, `network_wire`, `network_settings` | pass, no sanitizer reports |

Measured (Release, `test_snapshot_delta`; the container is shared, so worst cases are noisy):

| What | Result |
|---|---|
| 64 players all moving at 120 Hz, acks 6 ticks old, 2,400 ticks | every delta in 1 part; largest 896 bytes, mean 870 (a full v1 snapshot is 4,466); payload, header and bitmask included, so under the 1152-byte cap |
| Keyframe of the same 64 players | 2 parts, 1,321 bytes; at `max_snapshot_bytes` 512, more parts, each under 512 |
| 1,024 slots of worst-case values | every part within 1152 (and 512); deltas between them decode exactly |
| 10% loss, 3% duplicates, reordering, 15% lost acks, respawns and despawns, 3,000 ticks | 2,689 snapshots completed, each identical to the host's; 10 keyframes |
| Host, 63 clients with acks 3 to 10 ticks old (8 baselines) | 71 us per tick mean (8 encodes of about 8 us); the budget is 150 us |
| Client, decode + apply + interpolate 64 players | 20 to 23 us per tick mean |
| Allocations (`test_replication_allocations`, Debug): 9,900 ticks after 100 of warm-up, 64 players, 4 clients with acks 3 to 40 ticks old, 5% loss, 10% lost acks, interpolation of all 64, rewind recording, 8 timestamp queries and 16 raycasts per tick | 0 allocations, 0 bytes |

Bugs the tests found and that are fixed:

- A position delta across the full range needs 28 width bits, and the decoder allowed 27.
- The rewind ring indexed `tick % capacity`, which collides when the tick counter wraps;
  it is now `tick & 31` with 32 entries.
- Inserting a late interpolation sample into a full slot overwrote the newest one.
- The first encoder wrote bit by bit (27 us per client); it now writes up to 56 bits at
  a time.

Outstanding for this batch:

- No session sends deltas, interpolates or rewinds yet: that is plan Batches 4 to 8.
  `--check`'s bandwidth estimate still counts full v1 records, so 64 players at 120 Hz
  snapshots fail it even though deltas would fit.
- Timings come from one Linux container, not from the target machines.
- No editor change: the new `snapshot_rate` and `interpolation_ms` ranges reach Project
  settings > Multiplayer through the settings table, but the container has no display,
  so nobody has looked.

## Plan Batch 4 (2026-10-19): session integration

Base: `35dec6c` (master after PR #22), branch `claude/steam-multiplayer-batch-0-1-e551hk`.
Plan section 5 and its Batch 4 gate; design adjustments 22 to 29.

Implemented:

- Engine: `Application::onPump(rawFrameDt)`, called after input and before the fixed
  ticks; `Engine/SimulationClock.h` with `EngineConfig::simulationTime` and
  `Engine::setSimulationTime` (Smoothed by default, Monotonic online: raw time, 8 ticks
  a frame at most, overruns); renderer swaps held while Monotonic.
- `Multiplayer/NetworkWorld.h`: roles, states, events, the handshake with mutual
  authentication, admission limits, timeouts and the shutdown order.
- GameFramework: `GameContext::network()`, `GameMode::collectInput`,
  `PlaySession::setNetworkStart`, `pumpNetwork`, `handleOnlineEvent`, `collectInput`,
  drain before each tick, close first in `stop()`; online pause, save and breakpoint
  rules; `GameFramework/NetworkPump.h`.
- Circuits: `CircuitWorld::setBreakpointsAllowed`.
- Editor: pumps from `onPump`; Pause disabled online with a tooltip; game code, asset and
  prefab reloads held until Stop; Stop Steam disabled during network play; the session's
  role and state under Project settings > Multiplayer > Steam.
- Player: pumps from `onPump`; `--net-test` (`Multiplayer/NetTest.h`).
- Network: `LoopbackOnlineServices`.
- Test `network_session`; `network_cli` runs `--net-test`.

Results in the headless Linux container (GCC 13):

| Configuration | Result |
|---|---|
| Debug, modular | builds everything, no warnings; CTest 41/43 (the known no-display failures `renderer_swap` and `save_games_cli`); `-R "network_\|test_steam\|test_sdr\|test_snapshot\|test_rewind\|test_replication"` 11/11 |
| Debug with `-fsanitize=address,undefined`: `network_session`, `player --net-test --scenario rejections` | pass, no sanitizer reports |
| Steamworks on (SDK 1.65): `editor`, `player`, `network_session`, `test_steam_services` | build and pass |

The gate:

| Gate item | Evidence |
|---|---|
| Startup-failure injection | `network_session`: start throws and restores the world for no services, no transport, unavailable services, authentication off on Steam, a taken address and a throwing `beginPlay` (whose listener is then free again); 13 handshake failures end in the expected reason (protocol, content, scene and settings mismatches; a client or host ticket that can't be made or fails later; either side refusing a ticket; either side never answering, after 15 s; no baseline in time; a full server); `--net-test --scenario rejections` covers 9 more over NetworkWorlds, including the host stopping mid-handshake and no host |
| Stop 100 times | 100 offline Plays and 100 online host-and-client Plays in `network_session`, alternating who stops first, each restoring the world with no auth session, ticket or packet left; `--net-test --scenario stop-start` 100 rounds |
| Offline save and hot reload | `save_games` and `hot_reload` pass unchanged; offline Pause, saving and breakpoints checked again in `network_session`; the offline clock gives exactly the old loop's ticks and remainder |
| Pumps during menus and editing | a client with local input suppressed keeps pumping, collects (neutral) input every frame and stays connected while the host simulates every tick; services are pumped with a session that is editing and with none |
| No events after the session | after Stop the session takes no service events and has no network world; a closed NetworkWorld reports nothing and ignores pumps, failures and events; Stop closes a lobby to joins first and leaves it last |

`--net-test` (`handshake`, default `cafe` preset, 3 clients): all three synchronizing
in 233 to 250 ms of virtual time; `--clients 11` passes under `clean`, `poor` and
`hostile`. The 100-round `stop-start` scenario takes 0.07 s.

Outstanding for this batch:

- No real Steam session: the handshake runs through `IOnlineServices` and `ITransport`,
  which `SteamServices` implements, but two Steam accounts on two machines haven't
  joined. Nothing in the player or editor starts network play yet (adjustment 25).
- The editor changes (Pause disabled online, the network play line) haven't been
  looked at: the container has no display, and nothing can start online Play from the
  editor yet anyway.
- Clients stop at Synchronizing until Batch 5's baseline (adjustment 23).
