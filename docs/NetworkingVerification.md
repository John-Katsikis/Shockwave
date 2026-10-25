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
- Clients stopped at Synchronizing until Batch 5's baseline (adjustment 23); they now
  reach Active.

## Plan Batch 5 (2026-10-19): replication without prediction

Base: `20bbb10` (master after PR #23), branch `claude/steam-multiplayer-batch-0-1-e551hk`.
Plan sections 3.2, 6.3 to 6.5 and 7 (the authority half) and the Batch 5 gate; design
adjustments 30 to 45.

Implemented, in `NetworkWorld` (`Private/NetworkReplication.cpp`, state in
`Private/NetworkWorldImpl.h`):

- Network IDs, owner slots and incarnations (`NetworkIdentity` on every machine);
  authored objects numbered in key order and bound by key on clients; spawns only from
  the manifest's prefabs.
- The baseline: frozen at Welcome, staged and checked (counts, IDs, keys, prefabs,
  profiles, owners, state ranges, SHA-256 digest) before anything touches the client's
  world; Ready, then the queued lifecycle changes, then Active and a full snapshot.
- Lifecycle messages in control-serial order (Spawn, Despawn, Ownership, Spawn as a
  respawn); a leaving player's objects lose their owner; objects destroyed without
  `despawn()` are despawned at the end of the tick.
- Full-state snapshots at `snapshot_rate`, split into parts; per-object newest ticks;
  barriers per part with a 32-part, 250 ms hold and rate-limited resync requests.
- Per-peer budget: an ordered control queue within `outbound_bytes_per_second` and
  `max_pending_reliable_bytes`, snapshots in the rest with a control reserve, players
  every snapshot and other objects in turn.
- The game's API on `game.network()`: `spawn`, `despawn`, `respawn`, `setOwner`,
  `markChanged`, `networkId`, `entity`, `identity`, `hasAuthority`,
  `isLocallyControlled`, `localPlayer`, `controlledEntity`. `PlaySession` gives the
  network world its World and calls `endTick` after gameplay.
- `player --net-test --scenario replication`; the editor's network play line shows the
  player slot, object count and players in the game.
- Test `network_replication`; `network_session` now replicates its physics box.

Results in the headless Linux container (GCC 13):

| Configuration | Result |
|---|---|
| Debug, modular | builds everything, no warnings; CTest 41/44 when run in parallel (`renderer_swap` and `save_games_cli`, the known no-display failures, and `package_cli`, which timed out under the parallel load and passes alone); the network set (`-R "network_\|test_steam\|test_sdr\|test_snapshot\|test_rewind\|test_replication"`) 12/12 |
| Debug with `-fsanitize=address,undefined`: `network_replication`, `network_session`, `player --net-test --scenario replication --preset hostile --clients 11` | pass, no sanitizer reports (after fixing a null `memcpy` source in `Sha256::update` for an empty span) |
| Steamworks on (SDK 1.65): `editor`, `player`, `network_replication`, `network_session` | build and pass |

The gate:

| Gate item | Evidence |
|---|---|
| Parts arriving reversed | `network_replication` (scripted host): part 1 then part 0 of one snapshot both apply; a duplicate part and an older snapshot change nothing and count as stale |
| Despawn before an old snapshot | a Despawn followed by an older snapshot naming the object: it stays gone, the session carries on |
| Different entity creation orders | host and clients build their scenes in different orders with unrelated entities first; every authored object has the same network ID everywhere, IDs follow key order, local entity handles differ; the same in every `--net-test --scenario replication` run |
| Partial baseline teardown | a 300-object baseline at 4 KiB/s, the host stopping halfway: the client fails with HostLeft, its staged objects are gone, its world has exactly its entities from before, nothing spawned or bound |
| Malformed authority data | 11 cases, each ending network play with its reason and the connection closed: a prefab outside the manifest, an authored key the scene lacks (ContentMismatch), a wrong baseline digest, one object twice, a non-unit quaternion, an owner over `max_players`, more objects than `max_entities`, a control serial gap, a snapshot of an object never spawned, a despawned ID spawned again, Active naming another player's object; the world untouched for the 7 before the commit |
| Control barriers | a snapshot ahead of its Spawn is held, then applied when the Spawn arrives; one that never gets its serial expires after 250 ms and sends one resync request; records of an old incarnation are ignored after a respawn |
| Owners and late join | crates owned by the client report `isLocallyControlled`; a late client gets the crates spawned before it and destroys the authored object the host despawned before; a leaving player's objects get owner 0; clients can't spawn, despawn, respawn or change owners |
| Budget scheduler | at 24 KiB/s, 41 moving objects: snapshot bytes stay within the 12 KiB/s left after the control reserve, the budget holds records back, the owned crate goes every snapshot, and everything converges once movement stops; 0 heap allocations over nine seconds of replication |
| Moving host-owned box, two worlds | `network_session`: a real PlaySession host's physics box falls; the client's copy (made kinematic) ends within 1 cm of it; `--net-test --scenario replication` with a moving box |

`--net-test --scenario replication --clients 11 --seed 9` (6 network objects at the end,
16 spawns, 12 despawns, a late joiner): every preset ends with every client exactly on
the host's state (max position error 0.000000 m). Under `clean` nothing is held; under
`poor` 34 parts waited for their Spawn and none expired; under `hostile` (200 ms each
way, 10% loss) 108 waited, 28 expired and 10 resyncs were sent. The late client is
Active 83 ms (clean) to 2.1 s (hostile) after it starts. A hostile run takes 0.14 s.

Outstanding for this batch:

- No real Steam session, and still nothing in the player or editor starts network play
  (adjustment 25): `--host` and the join commands fill `NetworkStart::registry` and
  `prefabs` from the project in a later batch.
- The editor's network play line hasn't been looked at (no display here).
- No performance measurement on the desktop; the plan's per-tick targets are Batch 10's.

## Plan Batch 6 (2026-10-20): shared input-driven Arena simulation

Base: `fadf720` (master after PR #24), branch `claude/steam-multiplayer-batch-0-1-e551hk`.
Plan Batch 6 and the input half of sections 6 and 7; design adjustments 46 to 53.

Implemented:

- In `NetworkWorld` (`Private/NetworkInput.cpp`): `sendInput` with a Ping/Pong tick
  estimate and up to 4 commands a message; the host's 64-tick input ring per player
  and `takeInput` with the 2-tick fallback; `submitCommand` / `pollCommand` checked
  against the frozen registry (ID, sequence, payload ranges, permission, target
  profiles, rate); `disconnect`; Ping answers with rate limits; new `stats()` counters.
  Client replicas of characters are not simulated.
- Arena split into `ArenaInput` (devices read once a frame into one command per tick),
  `ArenaSim` (movement, combat, reloads, respawns and rounds from commands, timers in
  ticks), `ArenaPresentation` (camera, rifle, tracers, sounds, bodies, HUD) and the game
  mode (who simulates, joins and leaves, lobby commands).
- `Scenes/arena-mp.swscene` (manifest scene 2) with six `NetworkSpawnPoint`s a team, the
  `Net Player` prefab (manifest prefab 1), host-spawned pawns for up to 6v6, commands
  `arena.request_team`, `arena.ready_to_play` and `arena.request_round_restart`.
- The pause menu's match panel: network line, READY, team buttons, LEAVE MATCH, rosters,
  and greyed Host / Find / Invite.
- `--check` rejects a multiplayer scene without spawn points for both teams
  (`arena_cli`); `--network-info` no longer lists input commands as unsupported.
- Test `network_arena`.

Results in the headless Linux container (GCC 13):

| Configuration | Result |
|---|---|
| Debug, modular | builds everything, no warnings; CTest 43/45 when run in parallel (`renderer_swap` and `save_games_cli`, the known no-display failures; `package_cli` failed once under the parallel load and passes alone); the network set 13/13; `arena`, `arena_graphics`, `arena_cli` pass |
| Debug with `-fsanitize=address,undefined`: `network_arena`, `network_session`, `network_replication`, `arena`, `player --net-test --scenario replication --preset hostile --clients 11` | pass, no sanitizer reports |
| Steamworks on (SDK 1.65): `editor`, `player`, `network_arena`, `network_session` | build and pass |
| `player --check` on Arena | 0 problems in 3 scenes, `Net Player` prefab ok |

The gate:

| Gate item | Evidence |
|---|---|
| Offline Arena tests unchanged | `arena` (`Tests/ArenaTest.cpp`) passes without changes to the test; `arena_cli` and `arena_graphics` pass |
| Host and 11 clients can't spoof damage | `network_arena`: a client sending a shot GameEvent (host to client only) is sent away (MalformedPacket) and the victim's health doesn't drop; clients run no combat, and after ten seconds of autoplay every client shows exactly the host's teams, ammo, health, kills, deaths and positions |
| ... team | a request for the full team is refused and the team stays; a host-only round restart is refused by the client's own world and, sent on the wire anyway, by the host (`commandsRefused`), the round unchanged |
| ... ownership | a client sending Ownership is sent away and the pawn keeps its owner; an unknown command ID, far-ahead inputs (RateLimited) and an over-long move get the sender sent away |
| ... speed | four commands a tick, all sprinting, move a pawn no further than one a tick would |
| Ammo and cadence independent of the render frame rate | the offline input path at 24, 30, 60, 144, 240 and 1000 fps over the same 360 ticks: the same shots (within one at the end of a held fire) and reloads, each jump and reload press in exactly one command; catch-up ticks don't replay a press, a frame without a tick keeps it, menus give neutral commands |
| One local camera per process | each of the 12 machines has exactly one primary camera, no pawn carries one, and it sits at the local pawn's eye |
| Missing input | a client that stops sending: two ticks of its last input with fire off, then standing with fire never held |

Outstanding for this batch:

- Nothing in the player or editor starts network play yet (Batch 9), so the 6v6 match
  runs only in tests, over the loopback hub.
- The match panel in the pause menu hasn't been looked at (no display here).
- Clients see others' shots only as state changes (adjustment 50) and their own pawn a
  round trip late: prediction is Batch 7.

## Plan Batch 7 (2026-10-20): physics prediction and presentation

Base: `86eb9f6` (master after PR #25), branch `claude/steam-multiplayer-batch-0-1-e551hk`.
Plan Batch 7, sections 8.1 to 8.3 and 9.1 to 9.3; design adjustments 54 to 65.

Implemented:

- Physics (`Physics/Physics.h`): `CharacterMotionState`, `PhysicsNetworkMode`
  (Offline, Authority, Client), `setPredictedCharacters`, `characterState`,
  `correctCharacter`, `predictCharacterStep`, `applyReplicaBodyPose`. The character
  movement step is one private helper used by `step()` and `predictCharacterStep`;
  predicted characters move against static bodies only; Client mode runs no Jolt update
  and keeps proxies, with runtime overrides only.
- `NetworkWorld` (`Private/NetworkPrediction.cpp`): `setPhysics`, `setPredictionMove`,
  `prepareStep`, `present`, `predicting`, `correctionOffset`, `predictionWarning`; a
  256-tick command and predicted-state history, reconciliation with replay (32 ticks at
  60 Hz), snaps with resync and warning, the 50 ms / 150 ms visual offset; the smoothed
  Pong clock and catch-up commands (`NetworkInput.cpp`); sessions interpolating remote
  objects through `InterpolationEngine`.
- Engine: runtime `PresentationTransform`, preferred by `renderWorld`;
  `presentedTransform()`. No renderer changes.
- `PlaySession` calls `setPhysics`, `prepareStep` before each physics step and `present`
  before the game's frame.
- `GameFramework/InputSampler.h`: the engine input sampler (moved from Arena).
- The `--check` and editor rule for moving bodies next to predicted characters;
  `--net-test --scenario prediction`; `player --net-preview`.
- `PoseState::kGrounded`; regenerated `NetworkProtocol.md`, `NetworkSchema.md` and
  `CircuitNodes.md` (it had fallen behind the network components).
- Arena: Net Player is a `PredictedCharacter`; clients predict with `ArenaSim::pawnVelocity`;
  the camera and rifle follow the drawn pose; the status line shows the prediction warning.
- Test `network_prediction`; `network_arena`, `network_session`, `network_cli`,
  `network_wire` and `network_settings` updated.

Results in the headless Linux container (GCC 13):

| Configuration | Result |
|---|---|
| Debug, modular | builds everything, no warnings; CTest 44/46 (`renderer_swap` and `save_games_cli`, the known no-display failures); the network and Arena set 18/18 |
| Debug with `-fsanitize=address,undefined` | the network, `test_*` and Arena set (18 tests) pass, no sanitizer reports |
| Steamworks on (SDK 1.65) | builds; the network, Steam and Arena set (14 tests) passes |
| `player --check` on Arena | 0 problems in 3 scenes |

The prediction net-test, 4 clients, seed 7 (`network_prediction`):

| Preset | Hz | Predicted | Checked | Matched | Corrections | Replayed | Largest correction | Largest offset shown |
|---|---|---|---|---|---|---|---|---|
| clean | 60 | 3592 | 1792 | 1780 | 4 | 8 | 0.067 m | 0.067 m |
| cafe | 60 | 3522 | 1727 | 1709 | 7 | 22 | 0.200 m | 0.201 m |
| poor | 60 | 3379 | 1524 | 1476 | 16 | 75 | 0.267 m | 0.684 m |
| hostile | 60 | 3035 | 1070 | 916 | 96 | 1815 | 0.817 m | 0.817 m |
| clean | 120 | 7192 | 3592 | 3580 | 4 | 8 | 0.034 m | 0.034 m |
| cafe | 120 | 7059 | 3425 | 3356 | 48 | 297 | 0.117 m | 0.344 m |
| poor | 120 | 6773 | 2473 | 2335 | 83 | 1284 | 0.175 m | 0.727 m |
| hostile | 120 | 6076 | 1513 | 1312 | 120 | 4486 | 0.642 m | 0.674 m |

On every run every client ends 0.0000 m from the host and draws every character within
1.5 mm of it (position quantization). On the clean network the only corrections are the
first ticks, before the client has measured the host's clock (each one a tick the host
had no input for).

The gate:

| Gate item | Evidence |
|---|---|
| Prediction tests | `network_prediction`: the host's step and a client's `predictCharacterStep` agree within 0.1 mm over 240 ticks of walking and two jumps; a predicted character walks through a dynamic box that stops it offline; `correctCharacter` moves the character, its Transform and velocity; bad arguments are refused; a client's dynamic box doesn't fall and its component stays Dynamic; `applyReplicaBodyPose`; the `--check` rule; `network_arena`: each client predicts only its own pawn |
| Frame-rate-independent inputs | the engine `InputSampler` at 24, 30, 60, 144, 240 and 1000 fps, at 60 and 120 Hz: each press in exactly one command, fire and movement held for the same ticks within a frame, the aim turned by the same amount (1e-4 rad) and each command carrying it as of its tick; `network_arena`'s offline path at the same rates (same shots and reloads) |
| Replay side-effect suppression | in the prediction scenario the game's move rule runs exactly once per predicted tick, never for the replayed ones (up to 4486 replayed); clients see no contact or sensor events while the host's characters set off the trigger (`network_prediction` also checks it directly) |
| Headless correction convergence | the table above: every preset at 60 and 120 Hz converges to 0.0000 m once inputs stop, with no correction offset left; no offset is shown more than 150 ms after its correction, and none past 1 m |
| Visual inspection of camera and viewmodel under network jitter | not done here (no display). `player --net-preview poor` (or `hostile`) on `Scenes/arena-mp.swscene` is for the project owner to look at |

Outstanding for this batch:

- The visual check above, on the desktop.
- Predicted characters pass through each other and through dynamic bodies online, and
  moving platforms aren't supported (section 9.2); `--check` reports them.
- At 120 Hz on the hostile preset (200 ms one way) most inputs still arrive late: the
  lead tops out at 32 ticks there (267 ms), so the host falls back often and corrects.

## Plan Batch 8 (2026-10-22): combat history, events and circuits

Base: `eb38df2` (master after PR #26), branch `claude/steam-multiplayer-batch-0-1-e551hk`.
Plan Batch 8, sections 10.1 and 11; design adjustments 66 to 73.

Implemented:

- `NetworkWorld` (`Private/NetworkCombat.cpp`): `setCombatTargets`, the host's rewind
  history recorded in `endTick`, `resolveShot` (view time window from the shooter's
  measured round trip, analytic capsule test, current-world obstruction ignoring live
  characters), the host's own pings to every client (`roundTrip`), `sendEvent` /
  `readGameEvent` with reliable delivery to Active clients, sequence dedup and a 128-event
  ring, and cursor reads of session events (`readEvent`). Clients stamp `view_tick` and
  `view_fraction` from their interpolation clock.
- `Multiplayer/PresentationDedup.h`.
- Physics: `raycast` with a list of entities to ignore.
- Circuits: `Circuit.execution`, `NodeCapability` with an explicit presentation list,
  `circuitScopeProblems`, run-time scope gating, `Network.IsAuthority`, `IsLocalOwner`,
  `LocalPlayer`, `State`, `PeerJoined`, `PeerLeft`, `Ready`, `Disconnected`, and generated
  `Network.Command.<name>` / `Network.Event.<name>` nodes (`registerNetworkNodes`, called
  when a registry freezes, and by the player and editor at start). `SW_PRESENTATION_NODE`.
- The `--check` and editor (Check network setup) rule for circuits using nodes their
  execution doesn't allow.
- Arena: shots go through `resolveShot` (a hit on an ended life is refused); the host sends
  `arena.shot_resolved`, `arena.player_died` and `arena.round_changed`; clients show their
  own shots at once and every other shot once from the event, with hit markers only from
  the host, and take the kill feed and round messages from events. The old "ammo went
  down, so play a shot" client path is gone.
- Regenerated `docs/CircuitNodes.md` and `Games/Arena/Docs/CircuitNodes.md`, now checked
  by `circuits_cli` and `network_cli`. `NetworkSchema.md` and `NetworkProtocol.md` are
  unchanged (the events and messages already existed).
- Test `network_combat`; `network_arena`, `network_cli` and `circuits_cli` extended.

Results in the headless Linux container (GCC 13):

| Configuration | Result |
|---|---|
| Debug, modular | builds everything, no warnings; CTest 45/47 (`renderer_swap` and `save_games_cli`, the known no-display failures) |
| Debug with `-fsanitize=address,undefined` | the network, `test_*`, Arena and circuits set (20 tests) pass, no sanitizer reports |
| Steamworks on (SDK 1.65) | builds; the same 20 tests pass |
| `player --check` | Arena 0 problems in 3 scenes, Sample 0 problems in 18 scenes |

`network_combat` measured the host's round trip to client A as 33 ms (3 ticks) on a hub
with 10 ms each way and 133 ms (9 ticks) with 60 ms each way: the tick quantisation of
both ends adds about a tick.

The gate:

| Gate item | Evidence |
|---|---|
| Rewind and security cases | `network_combat` at both latencies: a shot at where the target was 5 ticks ago hits (Exact) while the same claimed view aimed at its present position misses; a target behind a static wall can't be hit and the shot stops at the wall; a future view time is clamped; a zero view time, a forged old one (round trip + 9 ticks) and one 200 ticks back are refused with no player hit; a dead target can't be hit; a hit from before a respawn names the old life and one after it the new life; the host's own shot sees the present; a view 12 ticks back is a real hit at 133 ms but refused at 33 ms (the low and high RTT clamps). `network_arena`: eleven autoplaying clients' shots rewound to their view times through Jolt, more than ten player hits, none refused |
| No duplicate shots or effects | `network_combat`: `PresentationDedup` (prediction then confirmation, copies, other shooters, ordinals and lives, the window and nearest match, a full ring); events under 20% reliable loss, 20% duplication and jitter arriving once each in order; a scripted host's duplicated and older events kept once. `network_arena`: every client received at least one event per shot fired and no duplicates |
| Scope validation | `network_combat`: Set is authority, Get presentation, commands requests; a LocalOwner or Presentation circuit that sets health is refused, a LocalOwner circuit may send a command and a Presentation one may not, anyone may hear events; at run time on a client an owner's circuit that sets health and the host's circuit don't run (health stays 100) and the reason is reported, while the host's circuit applies it. `network_cli`: `--check` fails with file:line for a Presentation circuit that moves things or sends a command, passes for LocalOwner with a command and Authority with anything |
| Unauthorised circuit can't apply damage; duplicate packets don't trigger a handler twice | the run-time cases above; three host events run a client's Presentation handler three times with each payload and never again, and an event packet sent twice by a scripted host runs it once |
| Schema and node schema regenerated and checked | `circuits_cli` compares `player --node-schema` with `docs/CircuitNodes.md`, `network_cli` Arena's with `Games/Arena/Docs/CircuitNodes.md` (which lists the generated `Network.Command.arena.*` and `Network.Event.arena.*` nodes) and the network schema with `docs/NetworkSchema.md` |
| Dynamic cover limitation documented | `docs/Networking.md` "Shots": moving bodies are tested where they are now, only players are rewound |

Outstanding for this batch:

- How shots, tracers and hit markers look and sound with real latency, on the desktop:
  `player --net-preview poor` on Arena's multiplayer scene.
- No `--net-test` combat scenario: combat is tested by `network_combat` and through Arena
  in `network_arena`.
- Moving cover isn't rewound (adjustment 73).

## Plan Batch 9 (2026-10-25): editor, command line, invites and packaging

Implemented:

- `Multiplayer/OnlineMatch.h`: host (friends lobby, details, joinable once Active,
  player count), find (bounded, checked listings), join (details checked before
  connecting), invite, leave and cancel, pending joins from invites and `+connect_lobby`,
  developer direct hosting and joining. `IOnlineServices::lobbyInfo` and `displayName`;
  Steam's persona names; `SteamAPI_RestartAppIfNecessary` for packaged launches.
- `Network/CaptureTransport.h` and `NetworkStart::capture`; `NetworkWorld::peerInfo`.
- Player: `--host`, `--join-lobby`, `+connect_lobby`, `--join-steam --dev-direct-join`,
  `--host --dev-direct-join`, `--net-capture`, `--net-dump`, `--net-encode`,
  `--network-manifest`, `--assign-network-keys [--seed]`, the `match_scene` setting,
  strict mode combinations (exit 2) and `--help` for all of them.
- Editor: Window > Network (`--network`) with Host / Find / Join / Invite / Leave, the
  developer joins, the peer table and session counters, Copy diagnostics, Capture packets,
  Run local test, Assign missing network keys (with undo), the reload-pending warning and
  the last match's end; the Inspector's NetworkObject Play status; Development package in
  Build > Package game.
- Arena: the online menu (`ArenaOnlineMenu.cpp`) on `game.onlineMatch`, beside the main
  menu offline and beside the match panel in the match scene; a failed match reopens the
  menu with the reason. No Steam calls in Arena.
- Packaging: `PackageOptions::steam`, AppID 480 only in development packages
  (`--dev-package`), the Steam runtime required from a Steam-enabled engine, offline-only
  packages from one without the SDK, `steam_appid.txt` and captures never shipped.
- Fixed: cancelling a lobby join and joining the same lobby again lost the membership
  (adjustment 83).
- Docs: `Networking.md` (Hosting and joining, the command line, captures, Steam
  packages, tests), `Packaging.md` (Steam), Arena's README, adjustments 74 to 86,
  regenerated `NetworkSchema.md` (`match_scene`, the unsupported list).
- Tests `online_match`, `arena_online_menu`, `package_steam`; `network_cli` extended.

Results in the headless Linux container (GCC 13.3):

| Configuration | Result |
|---|---|
| Debug, modular | builds everything, no warnings; CTest 48/50 (`renderer_swap` and `save_games_cli`, the known no-display failures) |
| Debug with `-fsanitize=address,undefined` | the network, `test_*`, Arena, online and circuits set passes with no sanitizer reports, except `package_steam`, which packages 1.4 GB of sanitizer binaries and passes its first steps but times out at 300 s |
| Steamworks on (SDK 1.65) | builds; `arena_online_menu`, `network_cli`, `package_steam`, `test_steam_services`, `online_match` pass |
| `player --check` | Arena 0 problems in 3 scenes, Sample 0 problems in 18 scenes |

The gate:

| Gate item | Evidence |
|---|---|
| Real UI tests | `arena_online_menu` loads Arena's `menu.rml` in RmlUi and clicks every online button over the Steam mock with a real host session: Find shows "SEARCHING FOR MATCHES", then two listings (one compatible, one "CAN'T JOIN: ..." which does nothing when clicked); Join shows Cancel and hides Host and the list; Cancel returns to offline; Join again reaches "PLAYING AS PLAYER 2" with Leave; a friend's invite during the match shows Accept/Decline and Decline keeps the match; Leave ends it; a Host that fails shows the reason and Retry, which hosts; Invite opens Steam's dialog once; with no match control the section is greyed with its reason and the offline menu is untouched; at 1280x720 the panels fit and don't overlap, offline and in the match scene |
| Screenshots | Not from the cloud: the editor needs a Vulkan window and the container has none (adjustment 86). Outstanding on the desktop |
| CLI invalid arguments | `network_cli`: unknown options, missing values, `--host` with a join, two joins, `--dev-direct-join` alone or with `--join-lobby`, `+connect_lobby` with `--host`, `--host` with `--check`, `--net-test`, `--load` or `--net-preview`, `--output` with nothing that writes, `--network-manifest` without `--output` or onto the manifest, `--assign-network-keys` without a scene or onto it; each exits 2 with its message. `--dev-package` without `--package` exits 2. Host failures exit 1 with the reason (multiplayer off; no Steamworks SDK) |
| Packet inspection | `network_cli`: a `--net-preview` run with `--net-capture`, dumped with `--net-dump --json` and encoded back with `--net-encode`: byte-identical. `online_match`: a client's capture has its ticket redacted and opaque peers, and round-trips through JSON |
| Moved-package smoke | `package_steam`: Arena packaged as a development Steam package, the folder renamed to `moved here` and started from a third folder: its `--check` passes with 0 problems |
| SDK-off package | `package_steam` on the default build: the package warns it plays offline only, has no Steam runtime, and its `--host` exits 1 with "this build has no Steamworks SDK". On the Steam build the same test finds `libsteam_api.so` in `Binaries/` |
| No debug AppID or auth capture in packages | `package_steam`: AppID 480 refused without `--dev-package` and nothing left behind; `steam_appid.txt`, a `.swnetcap` in Content and one in `Saved/Captures` all absent from the package; Verify fails on either |

Outstanding for this batch:

- On the desktop: `editor --network` (Window > Network) on Arena with `enabled 1` and
  `steam_app_id 480`, and Arena's menu (P) Host and Find; how the menu and the editor
  window look.
- Two Steam accounts hosting, finding, inviting and joining for real: Batch 10.
- A packaged Steam game launched from Steam (the restart path) needs a real AppID.

## Plan Batch 10 (2026-10-25): soaks, fault cases and budgets

Implemented:

- The 12-player soak: `arena_network_test --soak clean|cafe|poor|hostile|throttled|outage|all
  [seconds]` and CTest `network_arena_soak` (`--soak all 20`), adjustment 93.
- Cost and memory counters: `NetworkWorldStats::workNanos` and `replayNanos`,
  `NetworkWorld::memoryBytes()` (adjustment 92).
- Fixed from the soak: `view_tick` 0 sent in a session's first moments (adjustment 87);
  the host sending honest clients away for resync requests a resend brought close
  together (88); no idle timeout once Active, so a 12 s outage left everyone waiting
  (89); every shot refused once a client's view was older than `max_rewind_ms` (90).
- Fixed from the case list: a ticket revoked after validating was ignored (91).
- Tests: `network_arena_soak`; `network_session` revocation by host and client;
  `network_replication` memory at 12 players and 512 objects.

Results in the headless Linux container (GCC 13.3, 4 cores; twelve sessions in one
process share them):

| Configuration | Result |
|---|---|
| Debug, modular | builds; CTest 49/51 (`renderer_swap` and `save_games_cli`, the known no-display failures) |
| Debug with `-fsanitize=address,undefined` | `network_session`, `network_replication`, `network_combat`, `network_arena` and `--soak all 20` pass with no sanitizer reports |
| Release, `--soak all 20` | 171 checks pass |
| Release, 30 simulated minutes, cafe | 33 checks pass; 79 rounds, 85,190 shots, 9,498 hits; 293 corrections (max 0.045 m), 0 prediction resets |
| Release, 30 simulated minutes, poor | 33 checks pass; 88 rounds, 75,403 shots, 10,197 hits; 237 corrections (max 0.045 m), 0 resets |
| Release, 30 simulated minutes, hostile | 32 checks pass; 87 rounds, 60,877 shots, 10,020 hits; 95 corrections (max 0.045 m), 7 resets |

Release network cost per tick (microseconds; one tick per frame at 60 fps):

| Preset (run) | Host p50 / p95 / p99 / max | Client p50 / p95 / p99 / max | Client replay max | Out per client (max) | Late join |
|---|---|---|---|---|---|
| clean (20 s) | 10 / 36 / 54 / 290 | 3 / 23 / 38 / 54 | 7 | 28.9 KiB/s | 0.10 s |
| cafe (20 s) | 12 / 42 / 58 / 138 | 6 / 28 / 45 / 209 | 17 | 27.8 KiB/s | 0.45 s |
| poor (20 s) | 12 / 40 / 63 / 127 | 6 / 24 / 34 / 179 | 30 | 26.8 KiB/s | 0.90 s |
| hostile (20 s) | 13 / 45 / 61 / 400 | 4 / 27 / 52 / 173 | 46 | 26.1 KiB/s | 2.63 s |
| throttled (20 s) | 12 / 35 / 51 / 87 | 6 / 22 / 35 / 48 | 14 | 28.5 KiB/s | 0.48 s |
| cafe (30 min) | 13 / 45 / 58 / 4141 | 6 / 29 / 40 / 2159 | 49 | 29.5 KiB/s | 0.45 s |
| poor (30 min) | 13 / 45 / 60 / 3878 | 6 / 28 / 44 / 618 | 61 | 29.0 KiB/s | 0.87 s |
| hostile (30 min, before p99 was reported) | 14 / 47 / - / 4100 | 4 / 27 / - / 1171 | 98 | 28.2 KiB/s | 2.58 s |

Against 14.4:

| Target | Measured |
|---|---|
| Host network p95 < 1 ms/tick | 35 to 45 us on every preset (whole network cost, adjustment 92). The rare 4 ms maximums in 30-minute runs weren't traced; twelve sessions share four cores here |
| Client p95 < 0.5 ms/tick, prediction physics excluded | 22 to 29 us; replay itself at most 61 us |
| Buffers <= 32 MiB at 12 peers, 512 objects | `network_replication`: host 1.57 MiB, client 1.85 MiB with 512 objects. Arena's 13 objects: host 0.89, client 1.84. Transport, World and Jolt not included; the whole 12-session soak process peaked at 147 MiB RSS |
| <= 64 KiB/s outbound per peer | 26 to 30 KiB/s per client; about 310 KiB/s for the host's eleven |
| Offline Arena before and after (`player --benchmark`) | Not measured: needs the desktop's GPU |
| Steam callback duration, SDK RTT and queue delay, loss | Not measured: needs real Steam |

The 14.2 cases:

| Case | Evidence |
|---|---|
| clean, cafe, poor, hostile | `network_arena_soak`, and 30 minutes of cafe, poor and hostile above |
| 500 ms burst outage | in every soak, at a quarter of the run |
| 12 s severe outage | `--soak outage`: every client fails with Timeout within the outage, the host frees each slot, keeps its own player and keeps listening; three new players join and match its state |
| Bandwidth throttling | the throttled soak (64 KiB/s a direction a link); `network_replication` budget test |
| Connection reset during baseline | `network_replication` "the host goes away halfway through a slow baseline": the client fails and its world is untouched |
| Auth revocation | `network_session` (host and client side), `test_steam_services` (Steam's late VacBanned) |
| Malformed messages | `network_wire` (truncation, bad enums), `network_replication` (ten bad authority messages), `network_session` (handshake failures), `network_arena` (forged events, ownership claims, unknown commands, far-ahead inputs) |
| Invariant 1, 12 players and six a side | checked every second in each soak |
| 2, no claiming another's ID | `network_arena` ownership claim |
| 3, spam inputs | `network_arena` four inputs a tick; far-ahead inputs |
| 4, convergence | each soak once traffic recovers |
| 5, no resurrected entities | `network_replication` despawned ID spawned again; `duplicateEvents` 0 in soaks |
| 6, late join | each soak's late joiner: current state, fewer events than the host sent |
| 7, caps | pawns, admitted, hub pools and buffers in each soak; `network_replication` zero allocations while replicating |
| 8, explicit recovery | the severe outage; the resync limit no longer drops honest clients |
| 9, offline | the Arena, physics and save tests |
| 10, independent sessions | every network test runs its sessions in one process with separate Worlds |

Outstanding for this batch (manual gates; no mock stands in for them):

- Two Steam accounts on two machines, same test AppID: host, find, invite, join,
  play, leave, rejoin. Blocked in the cloud: no Steam client or accounts.
- Windows host with a Linux client and back, and a relayed (SDR) connection: needs
  both machines.
- Arena offline `player --benchmark` before and after networking on the desktop's 3080,
  and a 12-player Release session's frame time p95/p99.
- Batch 9's UI look on the desktop (`editor --network`, Arena's online menu).
