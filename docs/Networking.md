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
| `Network/Messages.h` | Wire protocol v1: the 32-byte header, all 21 message kinds, input commands, state records, the built-in pose state, and `encodeMessage()` / `decodeMessage()` |
| `Network/Transport.h` | `ITransport`, the interface gameplay networking will use for Steam and for tests |
| `Network/Loopback.h` | `LoopbackHub`: in-process transports on a virtual clock with seeded latency, jitter, loss, duplication, outages and bandwidth limits |
| `Network/NetworkCapture.h` | `.swnetcap` packet captures, and conversion to and from JSON |
| `Network/OnlineServices.h` | `IOnlineServices`, the lobby and identity interface Steam will implement; `makeUnavailableOnlineServices()` for builds without it |

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

Not built yet: Steam itself, hosting and joining, replicated entities, prediction,
lag compensation and the `player --net-*` session commands. Nothing advertises those
as working; `--network-info` lists them under `unsupported`.

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
project's `tick_rate`; `input_rate` equals it, and `snapshot_rate` is 10, 15, 20, 30 or
60 and divides it. `--check` also estimates the player-state bandwidth: `max_players`
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
15, 20, 30 or 60 and must divide the tick rate.

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
ctest --test-dir build -R network_ --output-on-failure
```

| Test | Covers |
|---|---|
| `network_wire` | Golden bytes for every message, made by an independent script; header and input command offsets; truncation at every length; bad headers, counts, ranges, strings, floats, quaternions; delivery and direction; endian vectors; tick wraparound; 200,000 fuzzed inputs; no allocation; the reference doc is current |
| `network_transport` | Connect, accept, reject, capacity; reliable order under every fault preset; unreliable loss, duplication, reordering and determinism; buffer lifetime; queue and pool limits; stale generations after close; budgets; outages; bandwidth; eleven clients; no allocation once connected |
| `network_capture` | Format; dump and encode round trip; redaction; malformed and truncated files; the size cap; no traffic from dumping |
| `network_settings` | Settings round trip, every range (tick 60..120, players 2..64), non-finite values, paths, unknown and duplicate keys with lines, transactional reads, atomic writes; cross-key rules; SHA-256 FIPS vectors; content digest against a Python-computed value, CRLF, links; manifest parsing and validation; registry ranges, duplicates, freeze, lookups, digest; the generic codec byte-equal to `encodePose`; scene rules; keys; `readyForSession` before any transport; no allocation in 1,000 capture/encode/decode/apply ticks |
| `network_cli` | On a copy of Arena: `--check` passing and failing with file:line for settings, manifest and scene mistakes; 120 Hz in both files; `--network-info` text and JSON; the checked-in NetworkSchema.md is current; mode combinations exit 2 |
