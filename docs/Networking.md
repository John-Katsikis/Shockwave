# Networking

Shockwave's multiplayer is being built in batches from the design in
[SteamMultiplayerImplementation.md](SteamMultiplayerImplementation.md): one
authoritative listen host and up to 63 clients (2 to 64 players), connected through Steam. This page
covers what exists today. [NetworkingVerification.md](NetworkingVerification.md) records
what has been tested and what hasn't.

## What works now

The `Network` module (`Source/Runtime/Network`, `Shockwave::Network`) is the
platform-neutral bottom layer. It depends only on Core: no Engine, Physics, SDL,
renderer or Steam.

| Header | What it gives you |
|---|---|
| `Network/NetworkTypes.h` | `NetResult<T>` and `NetError` (routine failures are values, not exceptions), `PeerId`, the protocol's `ReasonCode`s, `secureRandomBytes()` for session nonces |
| `Network/Wire.h` | `WireWriter` and `WireReader`: bounded little-endian byte codecs that never allocate; `validUtf8()`; `tickNewer()` for wrapping tick comparisons |
| `Network/Messages.h` | Wire protocol v1: the 32-byte header, all 23 message kinds, input commands, state records, the built-in pose state, and `encodeMessage()` / `decodeMessage()` |
| `Network/Transport.h` | `ITransport`, the interface gameplay networking will use for Steam and for tests |
| `Network/Loopback.h` | `LoopbackHub`: in-process transports on a virtual clock with seeded latency, jitter, loss, duplication, outages and bandwidth limits |
| `Network/NetworkCapture.h` | `.swnetcap` packet captures, and conversion to and from JSON |
| `Network/OnlineServices.h` | `IOnlineServices`, the lobby and identity interface Steam will implement; `makeUnavailableOnlineServices()` for builds without it |
| `Network/LoopbackServices.h` | `LoopbackOnlineServices`: an `IOnlineServices` on a `LoopbackHub`, with made-up session tickets and switchable failures, for tests and `--net-test` |

The `Multiplayer` module (`Source/Runtime/Multiplayer`, `Shockwave::Multiplayer`) sits
on Engine and Network and holds what a game configures (Batch 2):

| Header | What it gives you |
|---|---|
| `Multiplayer/NetworkSettings.h` | `Config/Network.swnetwork`: the settings, their descriptor table, transactional parsing, atomic writing and validation |
| `Multiplayer/NetworkManifest.h` | `.swnetmanifest`: the scenes and prefabs a session may use, and the SHA-256 content digest peers compare |
| `Multiplayer/NetworkComponents.h` | The `NetworkObject` and `NetworkSpawnPoint` components and their stable keys |
| `Multiplayer/Replication.h` | `ReplicationRegistry`: replication profiles, commands and events with stable IDs, frozen before a session; the profile codec |
| `Multiplayer/NetworkValidation.h` | The checks `player --check` and the editor share, and `readyForSession()` |
| `Multiplayer/NetworkSchema.h` | The generated [NetworkSchema.md](NetworkSchema.md) and `--network-info`'s report |
| `Multiplayer/SnapshotDelta.h` | Quantized entity states, the host's snapshot history, delta encoding against acknowledged baselines, and the client's decoder |
| `Multiplayer/Interpolation.h` | `InterpolationEngine`: remote objects shown a jitter buffer behind the server clock |
| `Multiplayer/RewindHistory.h` | `RewindHistoryBuffer`: player capsules over the last `max_rewind_ms`, for lag-compensated hit checks |
| `Multiplayer/NetworkWorld.h` | `NetworkWorld`, a Play session's network side as `game.network()`: role, state, the connection handshake, replication (network IDs, the baseline, spawns, owners, snapshots, budgets) and shutdown order |
| `Multiplayer/NetTest.h` | The `player --net-test` scenarios |

The `SteamServices` module (`Source/Runtime/SteamServices`, `Shockwave::SteamServices`)
sits on Network and is Steam (Batch 3). The Player and Editor link it; nothing else does.

| Header | What it gives you |
|---|---|
| `SteamServices/SteamServices.h` | `SteamServices`, the `IOnlineServices` for Steam: `initializeClient()` (SteamAPI_Init) or `initializeDedicatedServer()` (SteamGameServer_Init, anonymous logon), `framePass()` to run callbacks, lobbies, auth tickets, `shutdown()` |
| `SteamServices/SteamNetworkAdapter.h` | `SteamNetworkAdapter`, the `ITransport` over Steam Datagram Relay P2P sockets: listen, connect, accept, poll, send, broadcast, RTT |
| `SteamServices/SteamApi.h` | `SteamApi`, every Steamworks call the module makes, with no SDK types |
| `SteamServices/MockSteamApi.h` | `MockSteamApi` and `MockSteamNetwork`: Steam played in-process for tests and SDK-less builds |

A Play session can go online as a host or a client, run the connection handshake (see
[Play sessions online](#play-sessions-online)) and replicate the host's network objects
to its clients (see [Replication](#replication)): clients show the host's state a
snapshot behind, without prediction. Nothing yet starts a session from the player or
the editor: the `--host` and `--join` commands, input commands, prediction and the
Arena game come in later batches. Nothing advertises those as working; `--network-info`
lists them under `unsupported`. Delta snapshots, interpolation and rewind history are
libraries tested on their own; no session uses them yet.

## Network settings

A project without `Config/Network.swnetwork` plays offline only. Arena's:

```text
shockwave_network 1
enabled 0
transport steam
steam_app_id 0
max_players 12
tick_rate 60
snapshot_rate 20
input_rate 60
manifest "Config/NetworkManifest.swnetmanifest"
```

Every key, its range and default is in [NetworkSchema.md](NetworkSchema.md). Keys left
out keep their defaults; unknown or repeated keys, values out of range and non-finite
numbers are errors with file:line, and nothing is clamped. `max_players` is 2 to 64
(1v1 up to 32v32), the listen host included. `tick_rate` is 60 to 120 and must equal the
project's `tick_rate`; `input_rate` equals it, and `snapshot_rate` is 10, 15, 20, 30, 60
or 120 and divides it, and `interpolation_ms` (25 to 200) should hold at least two
snapshot intervals or `--check` warns. `--check` also estimates the player-state bandwidth: `max_players`
records of the game's largest per-player profile at `snapshot_rate` must fit in
`outbound_bytes_per_second` minus 16384 bytes/s kept for everything else. Arena at 64
players needs `outbound_bytes_per_second` of at least about 141000 at 20 snapshots a
second, and `--check` says so.

In the editor, **File > Project settings** has a Multiplayer section with a row per key,
the same rules shown live, **Save multiplayer settings** (through a temporary file),
**Revert** and **Check network setup**, which runs the network part of `--check`.

## The network manifest

```text
shockwave_network_manifest 1
game_protocol "arena.v1"
scene 1 "Scenes/arena.swscene"
prefab 1 "Prefabs/Net Crate.swprefab" "engine.rigidbody.v1"
```

Scene paths are relative to the project folder, prefab paths to Content; IDs are 1 to
65535 and unique per kind. A prefab's root must have a `NetworkObject` whose profile
is the one listed. `game_protocol` names the gameplay rules: bump it when the game's
C++ changes how it plays, since code isn't hashed.

The content digest is SHA-256 over every file in Content (dotfiles skipped, links are
an error), the manifest's scenes and the manifest, with text files read with LF line
ends, so a Windows checkout matches a Linux one. A texture change makes builds
incompatible in v1. The exact bytes hashed are in [NetworkSchema.md](NetworkSchema.md).

## Network objects

```text
entity "West door"
  Transform
  NetworkObject
    key "door-west"
    profile "engine.pose.v1"
    mode Interpolated
    always_relevant 1
end
```

`key` is the object's identity across peers and never comes from names, file order or
hierarchy, so renaming an entity keeps it. The editor gives a new `NetworkObject` a
random key and each duplicate or placed prefab copy a fresh one; a prefab's root leaves
it empty. In v1 a network object is an unparented root with scale 1 1 1, prefab parts
can't be network objects, `always_relevant` is 1, and a `PredictedCharacter` has a
`CharacterController` and no `RigidBody`. In a scene with network objects every moving
`RigidBody` needs one too. `NetworkSpawnPoint` (team 0..31, order 0..4095) marks where
players appear; no two share a team and order.

## Replication profiles, commands and events

A profile is the state one kind of object replicates: an ordered list of typed fields
with ranges, which is at once the wire layout, the `--check` rules and the generated
docs. Commands are what a client may ask the host; events are what the host tells
clients happened. None of them is a way to call arbitrary code.

The engine registers `engine.pose.v1` (ID 1) and `engine.rigidbody.v1` (ID 2). A game
module adds its own with `SW_NETWORK_TYPES`:

```cpp
#include <Shockwave/Game.h>

void registerNetworkTypes(shockwave::net::ReplicationRegistry& registry) {
    using namespace shockwave::net;
    std::string why;
    ReplicationProfile rules;
    rules.id = 33;   // games use 32..1023; 1..31 are engine.*
    rules.name = "mygame.rules.v1";
    rules.fields = {netInt("score", NetFieldType::U16, 0, 65535, NetFieldRole::Score)};
    rules.requiredComponents = {"MyRules"};
    rules.capture = captureRules;   // plain functions: no captures, no allocation
    rules.apply = applyRules;
    if (!registry.addProfile(std::move(rules), why)) SW_LOG(LogNetwork, Error, "{}", why);
}
SW_NETWORK_TYPES(registerNetworkTypes)
```

`freeze()` checks the whole catalog (names, ID ranges, field ranges against their
types, at most 32 fields, command rates of at most 20 a second with bursts of at most
10, target profiles) and builds constant-time lookups. Any failed registration fails
it too. After that nothing can be added, and capture, encode, decode, apply and lookups
allocate nothing. Peers compare the registry's schema digest, SHA-256 of its canonical
catalog text. Arena registers `arena.player.v1`, `arena.rules.v1`, three lobby commands
and three match events (`Games/Arena/Source/ArenaNetwork.cpp`).

## Command line

```sh
build/bin/player --project Games/Arena/Arena.swproject --check
build/bin/player --project Games/Arena/Arena.swproject --network-info [--json out.json]
build/bin/player --project Games/Arena/Arena.swproject --network-schema docs/NetworkSchema.md
```

`--check` validates the settings, the manifest and its prefabs, the registry, every
scene's network objects and the content digest, and prints problems as file:line.
Warnings (Valve's test AppID 480, a rewind window shorter than interpolation) don't
fail it. `--network-info` prints the build's capabilities, the settings, both digests,
the registry and the bandwidth estimate; in JSON, 64-bit values are strings. It exits
1 on a problem. `--network-schema` writes the reference for the project's game module
(`-` for standard output); `network_cli` fails when the checked-in one differs. Both run
on their own: combined with another mode they exit with 2. None of them opens a socket
or starts Steam.

```sh
build/bin/player --project <file.swproject> --steam-status [client|server]
```

`--steam-status` is the one tool that does start Steam: as a client (or a dedicated
server) for the project's `steam_app_id`, then it waits up to 10 seconds for sign-in and
relay network access, prints `steam client, AppID ..., SteamID ..., signed in yes, relay
current`, and shuts Steam down. It exits 1 without an AppID, in a build without the SDK
(`steam unavailable: this build has no Steamworks SDK ...`), or when Steam isn't running.
In the editor, Project settings > Multiplayer > Start Steam does the same and keeps
Steam running until Stop Steam or the project closes.

```sh
build/bin/player --project <file.swproject> --net-test [--scenario handshake|rejections|stop-start|replication]
    [--seed N] [--preset clean|cafe|poor|hostile] [--clients N] [--json out.json]
```

`--net-test` runs the real session handshake between `NetworkWorld`s over an in-process
loopback hub, on a virtual clock, with the project's network settings (tick rate, player
count, timeouts) and `LoopbackOnlineServices` for mutual authentication. No Steam,
sockets, window or game code; the same seed gives the same run. `handshake` joins a host
and `--clients` clients (default 3) through the preset's faults (default `cafe`);
`rejections` makes one client fail in each way a join can (protocol, content, schema
and scene mismatches, a full server, a refused ticket, a ticket never answered, the host
stopping mid-handshake, no host) and checks the reason; `stop-start` joins and stops 100
times; `replication` gives every machine its own world (built in a different order) with
a moving authored box and a static lamp, spawns, moves, respawns and despawns crates
owned by the players while `--clients` clients join (the last one late), then stops
moving and checks that every client has exactly the host's objects, owners,
incarnations and poses (`max position error 0.000000 m`). Each prints one line per peer and what was left open after stopping (packets in
flight, auth sessions, tickets: all must be 0), and exits 1 when anything is wrong, 2
for an unknown scenario or preset.

## Play sessions online

`PlaySession` (the engine side of Play, in the editor and the player) always has a
`NetworkWorld` while playing, reached from game code as `game.network()`. Offline it
says so (`role()` is `Offline`, `authority()` is true, `localSlot()` is 1) and does
nothing at all. To play online, the application calls
`session.setNetworkStart(start, services)` before `start()`:

- `NetworkStart` holds the role (`Authority` or `Client`), the network settings, what
  peers compare (game protocol, schema and content digests, the manifest's scene ID), the
  address to listen on or connect to, the lobby, and whether to authenticate.
- `services` is the application's `IOnlineServices` (Steam, or loopback in tests). The
  session borrows it; it must outlive the session.

`start()` adds the `NetworkWorld` after physics (so it is destroyed first) and before
`GameMode::beginPlay` (so the game sees its role), and the host listens or the client
connects there. If that fails, `start()` throws and the authored world is restored, as
for any other start failure.

The handshake, all reliable messages:

| Step | Client | Host |
|---|---|---|
| 1 | connects; state `connecting` | accepts, up to 4 connections beyond capacity |
| 2 | sends Hello (game protocol, digests, scene, lobby, a random nonce); state `authenticating` | checks them, the lobby and that there's room, or sends Reject with the reason |
| 3 | checks the Challenge (its nonce, the host's identity, digests, scene, tick and snapshot rates) | sends Challenge with its settings and a random session nonce used in every later header |
| 4 | asks for a ticket for the host, sends it, validates the host's | the same the other way |
| 5 | once welcomed and the host validated: state `synchronizing` | once the client's ticket validated and its own was sent: Welcome with a player slot (2 up) |
| 6 | stages the baseline, checks its digest, then binds and spawns it; sends Ready | right after Welcome: BaselineBegin, a Spawn for every network object, BaselineEnd with a digest |
| 7 | state `active`; snapshots apply from here | on a matching Ready: the lifecycle changes since the baseline, then Active (with the object this player controls) and a full snapshot |

Each phase has a timeout: `connect_timeout_seconds`, `handshake_timeout_seconds` (a
ticket never answered fails with `AuthUnavailable`) and 30 seconds from Welcome to
Active (the host gives up first and says `Timeout`).
Before a client is admitted it gets a 16 KiB/s receive budget. A client fails with the
host's reason (`ProtocolMismatch`, `ContentMismatch`, `MapNotLoaded`, `ServerFull`,
`NotLobbyMember`, `AuthenticationFailed`, `AuthUnavailable`, `HostLeft`, ...), visible as
`state()` `Failed` and `failure()`; it stays in network play until Stop. The host is
`active` as soon as it listens and reports `PlayerJoined` and `PlayerLeft` events
(`game.network().pollEvent()`).

The application pumps every frame from `Application::onPump`, which the engine calls
right after reading input and before the fixed ticks, in menus and while editing too.
`pumpNetworkFrame()` (`GameFramework/NetworkPump.h`) does it in order: the services'
`pump()`, their events to the session (`handleOnlineEvent`), `pumpNetwork(now)` for
messages and timeouts, then `GameMode::collectInput(game, frameDt)` so the game latches
its input once per frame. Each fixed tick also drains what arrived since, so a slow
frame doesn't hold back a batch of ticks.

While a session is online:

- Time: the engine's `SimulationClock` uses raw monotonic time instead of the smoothed
  frame time, runs at most 8 ticks a frame and carries the rest as debt. A stall over
  250 ms, or debt over 250 ms, ends network play with `SimulationOverrun` instead of
  catching up. Offline time is exactly as before.
- Pause: `pause()` refuses (the host never pauses for one player). A menu calls
  `game.network().setLocalInputSuppressed(true)`; the session keeps running, and
  `collectInput` should then send neutral input. The editor's Pause button is disabled,
  with a tooltip.
- Circuit breakpoints are off.
- Saving, loading and deleting saves fail with `UnavailableDuringNetworkPlay` before
  touching a file or the world.
- Reloads wait until Stop: the editor holds back game code reloads (it logs that one is
  pending), asset and prefab changes, and the engine holds back renderer swaps.

Stop closes the network first, in a fixed order: the state becomes `closing` and late
events are ignored from then on; the host's lobby is closed to joins; Goodbye goes to
every peer; connections and the listener close; whatever arrived is drained and dropped;
auth sessions end and ticket requests are cancelled; the lobby is left; the transport is
destroyed. Then `endPlay` runs and the authored world comes back. Nothing is reported
after that.

## Replication

The host decides what happens; clients show it. There is no prediction yet, so a client
sees the host's state a network trip and a snapshot interval late. Each network object
(an entity with a `NetworkObject`, see [Network objects](#network-objects)) gets a
`NetworkIdentity` on every machine: a network ID (counting up from 1, never reused in
a session), an owner slot (0 for nobody, 1 the host, 2 up the clients) and an
incarnation that goes up on respawn, teleport and owner changes.

- Authored objects (placed in the scene) are numbered by the host at start, in key
  order. A client binds each one by its `key`, whatever its local entity is; a key the
  client's scene lacks, or a different profile, ends its session with `ContentMismatch`.
  Authored objects the host despawned before the client joined are destroyed on the
  client when it commits the baseline.
- Spawned objects come only from the manifest's prefabs (`NetworkStart::prefabs`, by
  ID); a client never spawns anything from a path, or from a snapshot.

Game code on the host (or offline) changes them through `game.network()`:

```cpp
auto crate = game.network().spawn(1, at, ownerSlot);   // a manifest prefab, owned by a player or 0
game.network().respawn(entity, &at);                    // a new incarnation, sent reliably
game.network().setOwner(entity, slot);
game.network().despawn(entity);                         // or destroy it: it's despawned at the end of the tick
game.network().markChanged(entity);                     // a Static object changed: sent once
```

Clients can't call them (they return an error). Everyone can ask `networkId(entity)`,
`entity(networkId)`, `identity(entity)`, `hasAuthority(entity)`,
`isLocallyControlled(entity)` (the owner is the local player), `localPlayer()` and, on a
client, `controlledEntity()`, the object Active said this player controls. Offline,
`spawn()` makes a plain copy of the prefab.

How state travels:

- Lifecycle changes (Spawn, Despawn, Ownership, and Spawn again for a respawn) are
  reliable and in order. Each carries the next control serial; a client that sees a gap
  fails with `MalformedPacket`.
- The baseline is the host's objects frozen at Welcome: BaselineBegin, one Spawn each
  (with its full state), BaselineEnd with the newest control serial and a SHA-256 digest
  of the catalog. The client stages it without touching its world, checks every prefab,
  key, profile, owner and state and the digest, and only then binds, spawns and applies
  it. Changes after the baseline follow it in the same queue. A client that leaves or
  fails halfway has an unchanged world.
- Snapshots are unreliable full state (state records, as in the
  [wire protocol](#the-wire-protocol)), on every `tick_rate / snapshot_rate`th tick, split
  into parts of at most `max_snapshot_bytes`. Each part names the control serial its
  records need. A client applies records one object at a time, keeping each object's
  newest tick, so reordered and duplicate parts change nothing and a lost part holds
  nothing else back; records of despawned objects or old incarnations are ignored. A
  part ahead of its control serial waits (up to 32 parts, 250 ms); one that waits too
  long is dropped and the client asks for a resync (at most once a second and three
  times a minute), which the host answers with a full snapshot. Static objects go in
  the baseline and again only after `markChanged()`.
- Replicas of dynamic rigid bodies become kinematic on clients, so the host's
  simulation isn't fought.

Budgets, per client, from the settings: lifecycle messages wait in a queue and go out
in order within `outbound_bytes_per_second` (16 KiB burst) and while the transport has
less than `max_pending_reliable_bytes` unsent. Snapshots get the rest of the budget,
leaving 16 KiB/s (or half of a smaller budget) for lifecycle messages: players' objects
go in every snapshot, the others take turns. A record goes to a client only after the
lifecycle change it depends on has been sent to it. A client whose queue overflows (its
baseline plus `max_pending_reliable_bytes`) is dropped with `RateLimited`; it can join
again. No heap memory is allocated per tick once a session is running.

The host fills `NetworkStart` with the frozen registry its schema digest came from
(`registry`), the manifest's prefabs (`prefabs`) and, optionally, a `spawnPrefab`
function (the default spawns the prefab file). A world is needed to replicate: without
one (as in the handshake scenarios) the baseline is empty.

## The wire protocol

Every message is a 32-byte header (`SWMP`, version, kind, flags, session nonce, sender
tick, payload length) followed by its fields, each written byte by byte in little
endian. Floats are their IEEE 754 bits. Nothing is ever copied out of a C++ struct's
memory, so every platform produces the same bytes.

[NetworkProtocol.md](NetworkProtocol.md) lists every message and field with its offset,
type and allowed range. It is generated from the field lists in `Messages.h`, the same
lists the encoder, the decoder and capture dumps use, so it can't drift from the code.
The `network_wire` test fails when it's out of date. To regenerate it:

```sh
build/bin/network_wire_test --write-reference docs/NetworkProtocol.md
```

Decoding checks everything before you see a value: lengths, counts, ranges, UTF-8,
finite floats, and that the message came by the delivery and in the direction the
catalog fixes for its kind. A message is decoded into a temporary `DecodedMessage`, so
a bad one changes nothing. Strings and byte fields in it point into the received
bytes.

```cpp
using namespace shockwave::net;

std::array<uint8_t, kMaxReliableMessageBytes> buffer;
uint32_t size = 0;
Goodbye goodbye{ReasonCode::Normal};
if (WireStatus status = encodeMessage(goodbye, session, tick, buffer, size); !status)
    SW_LOG(LogNetwork, Error, "can't encode: {} ({})", wireStatusName(status.code), status.field);

DecodedMessage message;
DecodeOptions options{.delivery = received.delivery, .receiver = PeerRole::Client};
if (WireStatus status = decodeMessage(received.bytes, options, message); !status)
    transport.close(received.connection, status.reason());   // ProtocolMismatch or MalformedPacket
```

Protocol v1 sessions run at a tick rate from 60 to 120 Hz. The snapshot rate is 10,
15, 20, 30, 60 or 120 and must divide the tick rate.

## Delta snapshots

A snapshot of every replicated slot would be 4.4 KB for 64 players, nearly four times
what an unreliable message may carry (1152 bytes). So the host sends each client only
what changed since the newest snapshot that client acknowledged, and a keyframe (from
empty slots) when it has none.

- Each slot is a `NetEntityState`, 32 bytes, `alignas(16)`, with no pointers or strings:
  position in 1/1024 m (within 32 km), rotation as "smallest three" (two index bits and
  three 10-bit components), velocity in 1/64 m/s, angular velocity in 1/32 rad/s, the
  pose flags and an incarnation (0 is an empty slot). `quantizeState()` and
  `dequantizeState()` convert from and to `PoseState`.
- The host stores every tick in one `SnapshotHistory` (the last 64 ticks). One
  `SnapshotDeltaEncoder` per session turns a tick and a baseline into parts; clients
  acknowledging the same tick share the same parts, so a tick costs one encode per
  distinct baseline. Each client has a `SnapshotDeltaCompressor`, which holds the newest
  tick it acknowledged and asks the encoder.
- The client's `SnapshotDeltaDecoder` keeps the last 64 complete snapshots. It decodes a
  part straight from the received bytes into staging, and only a part that is right in
  every respect changes anything. It answers with `SnapshotAck`: the newest complete
  tick and a bit for each of the 32 before it.

A `DeltaSnapshot` names its tick, its baseline tick (when `flags` has bit 0), the part's
index and count, and the slots it covers, `first_slot` to `first_slot + slot_count - 1`.
Parts of one snapshot cover different slots, and slots no part covers are unchanged. The
payload is a bit stream, least significant bit first, zero-padded to a byte:

| Bits | Meaning |
|---|---|
| 1 per covered slot | changed |
| then, if changed: 1 | fresh: decoded against an empty slot (a respawn, a new or reused slot, a keyframe) instead of the baseline |
| 6 | which fields follow: position 1, rotation 2, velocity 4, angular velocity 8, flags 16, incarnation 32. Fresh with none means the slot was emptied |
| position | per axis, the difference as a 5-bit width n and the low n - 1 bits of its zigzag value (the top bit is implied) |
| rotation | 1 bit relative; then three 4-bit-width differences, or 32 raw bits when that would be smaller or the index changed |
| velocity, angular velocity | per axis, like position with 5-bit widths |
| flags, incarnation | 8 and 16 raw bits |

Only fields that changed are sent, and the decoder refuses a part whose field bits
don't match what changed, a delta for an empty or different incarnation, values out of
range, extra bytes or set padding bits.

With 64 players all running, strafing and turning at 120 Hz and acks six ticks old, a
delta is about 870 bytes, one part; a keyframe is two parts. `test_snapshot_delta`
prints the sizes it measures.

## Interpolation

Remote objects are shown slightly in the past, so a late or lost snapshot still leaves a
sample ahead to move towards. `InterpolationEngine` keeps 32 samples per slot, in tick
order whatever order they arrive in. `observeServerTick()` estimates the server clock
from the earliest arrivals; `renderTime()` is that clock minus `interpolation_ms`
(25 ms at 120 Hz is a 3-tick jitter buffer). `sample()` blends the two samples around
that time: positions along cubic Hermite curves through both velocities (or linearly),
rotations by the shortest slerp, velocities linearly. Past the newest sample an object
keeps its velocity for up to `max_extrapolation_ms` and then holds, reported as `Held`.
A teleport, a new incarnation or an emptied slot clears the slot's samples.

## Rewind history

For lag compensation the host records every player's capsule each tick in a
`RewindHistoryBuffer`: the last `max_rewind_ms` (at most 32 ticks, 250 ms at 120 Hz).
`queryAtTimestamp(ms, hitboxes)` returns every capsule as the shooter saw it, blending
the two ticks around the time; `raycast()` returns the nearest living player a ray hits
then, ignoring the shooter. Tick 0 is 0 ms. A time after the latest tick is clamped to
it, one up to two ticks older than the window is clamped to its edge, and anything older
is rejected, so a client can't pick an old, favourable pose. Between a death, respawn
or teleport and the tick before it a player can't be hit. Static-world cover is the
caller's: compare the hit distance with the world ray's.

None of these three allocates after construction.

## Transports

`ITransport` is one networking endpoint, used from the main thread only. Every call
returns at once:

- `listen()`, `connect()`, then `accept()` or `close()` each `Incoming` event during the
  same pump; an incoming connection left unanswered is refused at the next `poll()`.
- `send()` copies the bytes before it returns. One send is one message at the other end.
- `poll(sink, budget)` hands events and messages to a `TransportSink`, up to a message
  and byte budget. A message's bytes are valid until the sink returns.
- `close()` invalidates the `ConnectionId` immediately. Every id carries a generation,
  so a slot reused by a new connection never receives an old connection's events.

### Testing with the loopback hub

A `LoopbackHub` is an object the test owns, never a global. Advance its clock yourself;
nothing sleeps, and a given seed gives the same deliveries on every platform.

```cpp
LoopbackHubConfig config;
config.seed = 123;
config.faults = findFaultPreset("poor")->profile;   // clean, cafe, poor, hostile
LoopbackHub hub(config);
auto host = hub.createTransport(PeerId{1});
auto client = hub.createTransport(PeerId{2});
(void)host->listen({TransportAddress::Loopback, 1, 0}, {});
ConnectionId toHost = client->connect({TransportAddress::Loopback, 1, 0}).value();
// ... each tick: send, host->poll(sink, {}), client->poll(sink, {}), hub.advance(16667);
```

Reliable messages may be delayed (a "lost" one waits for its retry) but always arrive
once and in order. Unreliable ones can be dropped, duplicated and reordered.
`hub.addOutage(start, duration)` drops unreliable traffic and holds reliable traffic
for a while; `FaultProfile::bytesPerSecond` throttles a link. The hub allocates its
buffers up front, so once peers are connected, sending and polling allocate nothing;
past the configured limits, `send()` reports `Backpressure` or `Dropped`.

## Steam

Steam is optional at build time. By default (`SHOCKWAVE_STEAMWORKS=OFF`) nothing looks
for, downloads or links the Steamworks SDK, `SteamServices::create()` makes a service
whose initialisation fails with `Unavailable`, and everything else works offline. To
build with Steam, download the SDK yourself from Valve's partner site (the engine never
fetches or ships it) and configure:

```sh
cmake -S . -B build -DSHOCKWAVE_STEAMWORKS=ON -DSHOCKWAVE_STEAMWORKS_SDK=/path/to/sdk
```

Configure fails with a message saying what's wrong if the folder has no
`public/steam/steam_api.h`, no runtime library for the target, or the target is a
platform Valve ships no library for (Linux on ARM64, for example). The runtime library
is copied into `build/bin`. During development Steam reads the AppID from a
`steam_appid.txt` in the working folder, which you write yourself; SteamServices checks
Steam started for the same AppID as `steam_app_id` and fails if not.

How SteamServices behaves (docs/SteamMultiplayerImplementation.md sections 4.1 to 4.5):

- Nothing starts Steam until `initializeClient()` or `initializeDedicatedServer()`: not
  `--check`, the schemas, `--network-info`, captures, benchmarks or tests. At most one
  Steam runtime runs per process. `shutdown()` closes every transport, cancels tickets,
  ends auth sessions, leaves lobbies, unregisters callbacks and shuts Steam down, exactly
  once; the destructor calls it.
- `framePass()` runs Steam's callbacks on the main thread. Connection changes are queued
  for the transport that owns them; everything else is acted on after Steam's dispatch
  returns, and reported through `pollEvent()`. An idle `framePass()` allocates nothing.
- Every request returns a `RequestId` carrying the service's generation. `cancel()` means
  its result is never reported: a lobby created or joined after a cancel is left at once.
  Results from before a restart are ignored. A dedicated server has no lobbies.
- Authentication goes both ways: each side calls `requestAuthTicket(peer)`, waits for
  `AuthTicketReady`, sends `authTicket()` over the transport, and the receiver calls
  `beginAuthSession(peer, ticket)`. Only `AuthSucceeded` admits a peer. An invalid,
  cancelled or revoked ticket is `AuthFailed` with `AuthenticationFailed`; no answer
  within 15 seconds, or Steam unable to decide, is `AuthFailed` with `AuthUnavailable`.
  Tickets are 1 to 4096 bytes, are never logged, and are zeroed when cancelled.
  `endAuthSession(peer)` on disconnect also cancels the tickets given to that peer.

How `SteamNetworkAdapter` behaves:

- `listen()` opens a P2P listen socket on the virtual port (`virtual_port`) and a poll
  group; if the socket fails the poll group is destroyed again. `connect()` is
  `ConnectP2P` to a SteamID64. The peer's identity is the one Steam authenticated.
- The connection state table: a peer connecting to our listener becomes `Incoming` (or is
  refused with `ServerFull` when the listener is full, or with no identity); one nobody
  accepts by the next poll is refused with `Timeout`; our own connect's Connecting is
  ignored; Connected joins the poll group (closing the connection if Steam refuses) and
  is reported once; ClosedByPeer and ProblemDetectedLocally are reported once, with the
  peer's `ReasonCode`, and the handle is closed. Callbacks for handles nobody owns are
  ignored. If more state changes arrive than the queue holds, the next poll re-reads
  every connection's state from Steam.
- `poll()` receives up to 32 messages at a time, copies them into a ring buffer sized
  when the adapter is made, and releases every Steam message straight away, whatever
  happens next (a bad size, a closed connection, a sink that throws). What the budget
  doesn't cover waits in the ring for the next poll. It's one bounded copy per message,
  not zero copy.
- `send()` uses `k_nSteamNetworkingSend_ReliableNoNagle` or `UnreliableNoNagle`.
  Unreliable messages are at most `max_snapshot_bytes` (1152), and are dropped while more
  than 32 KiB waits on that connection, so old state never piles up. Steam's queue
  limit gives `Backpressure` (reliable) or `Dropped` (unreliable).
- `stats()` has Steam's RTT and queued bytes; `broadcast()` sends to every connected peer.
- Once the adapter exists, polling and sending allocate nothing.

`MockSteamApi` plays Steam in-process: listen sockets, connections, a message pool,
tickets, lobbies and a virtual clock, with faults to switch on (failed init, listen
socket, poll group, send results, tickets, validation, held lobby results, injected
callbacks) and counts of everything opened and released. It doesn't model loss,
latency, relays or NAT: that's the two-account manual test in
[NetworkingVerification.md](NetworkingVerification.md).

## Packet captures

A `.swnetcap` file records the application messages a peer sent and received (see the
format in `NetworkCapture.h`). Authentication tickets are never stored: their records
are marked redacted and hold no bytes. `captureToJson()` decodes every record by name
and field, with 64-bit IDs as decimal strings; `captureFromJson()` turns that JSON back
into the identical capture. Nothing in this API can send traffic.

The `player --net-capture`, `--net-dump` and `--net-encode` commands that will use it
come in a later batch.

## Tests

```sh
ctest --test-dir build -R "network_|test_steam|test_sdr|test_snapshot|test_rewind|test_replication" --output-on-failure
```

| Test | Covers |
|---|---|
| `network_wire` | Golden bytes for every message, made by an independent script; header and input command offsets; truncation at every length; bad headers, counts, ranges, strings, floats, quaternions; delivery and direction; endian vectors; tick wraparound; 200,000 fuzzed inputs; no allocation; the reference doc is current |
| `network_transport` | Connect, accept, reject, capacity; reliable order under every fault preset; unreliable loss, duplication, reordering and determinism; buffer lifetime; queue and pool limits; stale generations after close; budgets; outages; bandwidth; eleven clients; no allocation once connected |
| `network_capture` | Format; dump and encode round trip; redaction; malformed and truncated files; the size cap; no traffic from dumping |
| `network_settings` | Settings round trip, every range (tick 60..120, players 2..64), non-finite values, paths, unknown and duplicate keys with lines, transactional reads, atomic writes; cross-key rules; SHA-256 FIPS vectors; content digest against a Python-computed value, CRLF, links; manifest parsing and validation; registry ranges, duplicates, freeze, lookups, digest; the generic codec byte-equal to `encodePose`; scene rules; keys; `readyForSession` before any transport; no allocation in 1,000 capture/encode/decode/apply ticks |
| `network_cli` | On a copy of Arena: `--check` passing and failing with file:line for settings, manifest and scene mistakes; 120 Hz in both files; `--network-info` text and JSON; the checked-in NetworkSchema.md is current; `--steam-status` failing without an AppID or the SDK; every `--net-test` scenario (replication with five clients on the `poor` preset), its JSON and bad arguments; mode combinations exit 2 |
| `network_session` | Real PlaySessions with physics over the loopback hub: Start/Stop 100 times offline and 100 times online (alternating who stops first) with the world restored and nothing left open; start failing for no services, no transport, unavailable services, no authentication on Steam, a taken address and a throwing `beginPlay`; 13 handshake failures, each with its reason; the online pause, save and breakpoint rules; a client in a menu still pumping and collecting input while the host simulates; no events after Stop or `close()`; the shutdown order with a lobby; `SimulationOverrun`; the simulation clock offline (identical to the old loop) and online; a host's falling physics box followed by the client's kinematic copy; the `--net-test` scenarios |
| `network_replication` | Worlds built in different orders binding authored objects by key; IDs in key order; spawns, despawns (and objects destroyed without one), respawn incarnations, owners and a leaving player's objects; a late client getting earlier spawns and despawns; a small budget (players every snapshot, the rest in turn, bytes within the budget) with 0 heap allocations over nine seconds; a baseline torn down halfway leaving the client's world unchanged; a scripted host sending snapshot parts reversed and duplicated, older snapshots, a despawn before an old snapshot, snapshots ahead of their Spawn (held, then applied) and one that never gets it (expires, one resync), old and new incarnations; eleven kinds of malformed authority data, each failing with its reason and closing the connection |
| `test_steam_services` | Without the SDK, `create()` fails visibly; then on `MockSteamApi`: init failure, bad settings, double init, a dedicated server, shutdown exactly once, an idle `framePass` allocating nothing; lobbies (create, metadata, find, join, members, leave); late lobby results after cancel and after a restart; auth both ways, a ticket for someone else, sizes, duplicates, revocation, cancellation, the 15-second timeout, stale results, everything cancelled and ended at shutdown |
| `test_sdr_transport_mock` | The connection state table (Incoming, refusals when full or late, Rejected with the host's reason, no listener, our own Connecting, Connected and Closed once, stale handles, queue overflow); listen, poll-group, connect and accept failures and their cleanup; sizes, send flags and every Steam send result; the 32 KiB backpressure; budgets and the ring buffer; every Steam message released once, including when the sink throws; broadcast; RTT; shutdown with transports alive; 0 allocations over 10,000 messages from four clients with echoes |
| `test_snapshot_delta` | Quantization limits and round trips; 64 moving players at 120 Hz in one part under 1152 bytes; keyframes split to fit 1152 and 512; 1024 worst-case slots; loss, reordering, duplicates and lost acks with every completed snapshot identical to the host's; acks and baselines leaving the history; respawns and despawns; every kind of malformed part; one encoder shared by clients; interpolation (Hermite, slerp, extrapolation limit, out-of-order and duplicate samples, teleports, the server clock, the 3-tick jitter buffer); host and client time per tick (under 150 us asserted in optimised builds) |
| `test_rewind_buffer` | Ring sizes; exact ticks, fractions and milliseconds at 120 and 60 Hz; future, slightly-old and too-old times; respawns, deaths and teleports; crouching; ray-capsule geometry; raycasts hitting the past and missing the present, skipping the shooter, picking the nearest; tick wraparound |
| `test_replication_allocations` | 0 heap bytes over 10,000 ticks of 64 players with four clients (encoding, loss, decoding, acks, interpolation, rewind recording, queries and raycasts) |
