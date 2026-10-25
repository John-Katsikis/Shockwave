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
| `Network/CaptureTransport.h` | `makeCapturingTransport()`: an `ITransport` that records what passes through it into a capture, tickets redacted |
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
| `Multiplayer/NetworkWorld.h` | `NetworkWorld`, a Play session's network side as `game.network()`: role, state, the connection handshake, replication (network IDs, the baseline, spawns, owners, snapshots, budgets), input commands, game commands, the Ping/Pong tick clock, prediction and reconciliation of the local character, presentation (`PresentationTransform`), lag-compensated shots (`resolveShot`), game events (`sendEvent`, `readGameEvent`) and shutdown order |
| `Multiplayer/PresentationDedup.h` | `PresentationDedup`: each shot's effects shown once, whether this machine predicted it or the host announced it |
| `Multiplayer/NetTest.h` | The `player --net-test` scenarios |
| `Multiplayer/OnlineMatch.h` | `OnlineMatch`: hosting, finding, joining, inviting and leaving, shared by the player's command line, the editor's Network window and game menus (`game.onlineMatch`) |

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
to its clients (see [Replication](#replication)). Clients send input commands and game
commands, which the host checks and simulates (see [Input and commands](#input-and-commands)).
A client moves its own `PredictedCharacter` at once with each command and the host
corrects it; every other object is drawn interpolated behind the host's clock (see
[Prediction](#prediction)). The host tests shots where the shooter saw its targets and
tells everyone what happened with game events, which circuits can listen to (see
[Combat and events](#combat-and-events)). Arena plays 6v6 this way on its multiplayer
scene, in tests.
Matches are hosted and joined through Steam lobbies from the player's command line, the
editor's Network window and Arena's menu (see [Hosting and joining](#hosting-and-joining)).
Delta snapshots are a library tested on its own; sessions send full-state snapshots, and
`--network-info` lists that under `unsupported`.

## Hosting and joining

`OnlineMatch` (`Multiplayer/OnlineMatch.h`) is the one implementation of the lobby side.
The player and the editor each own one and hand it to game code as `game.onlineMatch`;
it's null where the application offers none (tools, tests, a project with no network
scene), and menus grey their online buttons then. It never opens a transport or starts
Play itself: it hands the application a `NetworkStart`, and the application plays the
match scene with it.

- **Host**: makes a friends-only lobby for `max_players`, sets its details (`sw_protocol`,
  `sw_game`, `sw_schema`, `sw_content`, `sw_scene`, `sw_host`, `sw_session`, `sw_state`,
  `sw_port`, `sw_players`, `sw_capacity`), then plays the match scene as host. The lobby
  only opens to joins once the session listens, and `sw_players` follows the players.
- **Find**: at most 50 lobbies of this game and protocol. Each listing says whether it's
  compatible and, when not, why (a different build, content or scene, or full).
- **Join**: enters the lobby, checks its details against ours (and that the advertised
  host is the lobby's owner) before connecting, then plays the match scene as a client of
  the owner. A mismatch fails before any connection.
- **Invite** opens Steam's invite dialog for our lobby. A friend's invite accepted in
  Steam, or a cold start with `+connect_lobby ID`, joins at once when idle; during a match
  it waits (`pendingJoin()`) until the player accepts or declines it. A callback never
  replaces the match being played.
- **Leave** (also Cancel while connecting) leaves the lobby; the application stops Play.
  A session that fails or ends (the host left, a timeout, a refusal) ends the match with
  the reason in `error()`, and the application goes back to the offline scene, whose menu
  says what happened. When the host leaves, the match ends for everyone (no host
  migration).

Which scene a match plays is `match_scene` in `Config/Network.swnetwork` (a scene in the
manifest; empty means the manifest's first). Steam starts only when someone asks to play
online, never for tools, `--check` or tests, unless a packaged game sets `steam_required`.

Game code calls it from menu handlers:

```cpp
game.ui().on("host", [&game](const UIEvent&) {
    if (game.onlineMatch) game.onlineMatch->host();   // false, with error(), when it can't begin
});
// Every frame: game.onlineMatch->state(), status(), error(), lobbies(), pendingJoin()
```

Arena's menu (`Games/Arena/Source/ArenaOnlineMenu.cpp`) has Host friends match, Find/Join
(six listed matches, refused ones with the reason), Invite, Cancel, Retry, Leave match /
play offline, and Accept/Decline for a friend's invite.

The editor's **Window > Network** (or `editor --network`) has the same requests for the
open project: Host plays the match scene, Find lists lobbies with Join, Invite and Leave,
and a developer section for joining a lobby by ID and direct joins. While playing it shows
a peer table (slot, name, status, RTT, KiB/s each way, queued bytes, input and snapshot
age) and the session's counters. Its footer has Copy diagnostics, Capture packets (the
next match, to `Saved/Captures/`), Run local test (the loopback handshake) and Assign
missing network keys for the open scene, with undo. It warns when the game module was
rebuilt and a reload is pending, and keeps the last match's end reason. The Inspector
shows a `NetworkObject`'s network ID, owner, role here, profile and snapshot age during
Play.

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
build/bin/player --project <file.swproject> --net-test [--scenario handshake|rejections|stop-start|replication|prediction]
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
incarnations and poses (`max position error 0.000000 m`); `prediction` gives every
machine Jolt physics, a floor, a wall and a trigger, and a predicted character for each
client, which walk, sprint and jump by script through the preset's faults and then stand
still: every client must end, and draw every character, where the host has it, with no
correction offset left, its replays running no game code and making no contact events
(`prediction: ... corrections (... ticks replayed, max ... m) ... final error 0.0000 m`). Each prints one line per peer and what was left open after stopping (packets in
flight, auth sessions, tickets: all must be 0), and exits 1 when anything is wrong, 2
for an unknown scenario or preset.

```sh
build/bin/player --project <file.swproject> --scene <network scene> --net-preview [clean|cafe|poor|hostile]
```

```sh
build/bin/player --project <file.swproject> --host [--scene <network scene>] [--net-capture file.swnetcap]
build/bin/player --project <file.swproject> --join-lobby <lobby ID>
build/bin/player --project <file.swproject> +connect_lobby <lobby ID>
build/bin/player --project <file.swproject> --host --dev-direct-join
build/bin/player --project <file.swproject> --join-steam <host SteamID64> --dev-direct-join
```

`--host` starts Steam, makes a lobby and plays the match scene (or `--scene`) as host;
`--join-lobby` joins one (Steam passes `+connect_lobby` when a friend launches the game
from an invite). A command-line match that can't start or that ends exits 1 with the
reason (`can't host: this build has no Steamworks SDK ...`, `can't play online: ...
multiplayer isn't enabled`), and 0 when you close the window. The developer pair skips
lobbies: `--host --dev-direct-join` admits direct connections (still authenticated), and
`--join-steam` connects to such a host by its Steam ID. Without `--dev-direct-join`,
`--join-steam` is refused. `--net-capture` records this machine's session to a capture.

```sh
build/bin/player --net-dump file.swnetcap [--json out.json]
build/bin/player --net-encode decoded.json --output file.swnetcap
build/bin/player --project <file.swproject> --network-manifest --output candidate.swnetmanifest
build/bin/player --project <file.swproject> --assign-network-keys --scene <file.swscene> --output <new.swscene> [--seed N]
```

`--net-dump` decodes a capture (no project needed); `--net-encode` turns that JSON back
into the identical file, refusing redacted ticket records. `--network-manifest` writes a
candidate manifest with the project's network scenes and prefabs and prints its schema
and content digests; it never overwrites the manifest in use. `--assign-network-keys`
writes a copy of the scene with every missing or duplicate key filled (valid unique keys
kept) and prints each change; `--seed` makes the keys repeatable. Neither rewrites its
input.

Exit codes: 0 success, 1 a failure (a refused join, a check, a file), 2 bad arguments:
an unknown option, a missing value, or modes that can't be combined (`--host`, the joins,
`--package`, `--screenshot`, `--check`, `--net-test` and the tools each run on their own).

`--net-preview` plays the scene as a client of a host running in the same process, over
the loopback hub with the preset's latency, jitter and loss (default `poor`: 75 ms one
way, 25 ms jitter, 5% loss). It's for seeing prediction and interpolation with your own
eyes before real hosting arrives: your character is predicted and corrected, the host's
own player is drawn interpolated. The scene must be in the network manifest; the
settings' `enabled` and `transport` don't matter. It doesn't combine with the tools,
`--save` or `--load` (exit 2).

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

The host decides what happens; clients show it, a network trip and a snapshot interval
late, except for the character a client predicts (see [Prediction](#prediction)). Each network object
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
- A client's physics doesn't simulate replicas (`PhysicsNetworkMode::Client`): dynamic
  bodies and other characters are proxies that follow the snapshots, so the host's
  simulation isn't fought. Their components stay as authored.

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

## Input and commands

Gameplay on the host runs on input commands, one per player per tick, and never on a
client's claims. A client's game calls `game.network().sendInput(command)` once per tick
with its 24-byte `InputCommand` (movement, aim, held fire and sprint, pressed jump and
reload, its pawn's incarnation); offline and on the host it returns 0 and does nothing.

- The client picks the host tick the command is for. It pings the host four times
  100 ms apart once Active, then once a second. The host's clock is taken from the
  answer with the lowest of the last 8 round trips (the round trip less the host's
  dwell, the tick at its midpoint) and smoothed: each answer moves the estimate a
  quarter of the way, and a jump of more than 8 ticks is taken at once. Commands aim
  half a round trip plus 2 ticks ahead (2 to 16; `stats().inputLead`). When the client
  is ahead it sends nothing that tick; when the clock has moved past the next tick it
  fills the skipped ticks (up to 4 commands, presses on the newest only); a jump of
  more than 8 ticks starts over. Each Inputs message carries the newest command and up
  to 3 before it, so one lost message loses nothing.
- The host keeps 128 ticks per player. Its game calls `takeInput(slot, incarnation, out)`
  once per pawn per tick: the command for this tick, or, when it's missing, the last one
  for up to 2 ticks with fire off and no presses, then a neutral command with the last
  aim. Inputs for ticks already run, for another life of the pawn, or more than 32 ticks
  ahead are not used; a client that keeps sending far-ahead inputs, or a move longer
  than the wire's circle, is sent away.
- Tick counts here (16, 8, 32, and the 32-tick replay limit below) are for 60 Hz; at
  `tick_rate` 120 they double, so a faster tick copes with the same latency.

The engine's `InputSampler` (`GameFramework/InputSampler.h`) makes the commands from the
local devices: `collect(readInputFrame(input, actionNames, active), sensitivity)` once
per frame from `GameMode::collectInput` turns the aim by that frame's mouse movement
once and latches held buttons, and `next(incarnation)` once per fixed tick gives each
press to exactly one command, however many ticks a frame runs. Menus and an uncaptured
mouse make the frame neutral and drop its presses. `commandYaw`, `commandPitch` and
`commandMoveDirection` read a command back for the rules.

Game commands are the registry's commands (`ReplicationRegistry::addCommand`): an ID, a
typed payload, who may send it (any player, the target's owner, or the host), the
profiles it may target, and a rate. `submitCommand(id, target, payload)` checks them
locally and sends the command; on the host (and offline) it goes through the same
checks as player 1. The host checks each received command again: an unknown ID, a
repeated sequence number or a payload outside its ranges sends the sender away; a
refused permission, target or rate is dropped and counted in `stats().commandsRefused`.
The game reads accepted commands with `pollCommand(received)` (64 wait at most) and
decides what they mean, for example "only while the round is waiting".

The host can send a player away with `disconnect(slot, reason)`, as Arena does with
`ServerFull` for a 13th player.

## Prediction

A client moves its own character as soon as it presses a key, and the host corrects it
when they disagree. A character is predicted when its `NetworkObject` mode is
`PredictedCharacter` (it has a `CharacterController` and no RigidBody or Collider), the
local player owns it, and the game has given its movement rule:

```cpp
// The same rule the host's simulation applies to the same command.
game.network().setPredictionMove([](const World& world, Entity pawn, const InputCommand& c) {
    return net::PredictedMove{commandMoveDirection(c) * 5.5f, (c.pressedBits & InputCommand::kPressedJump) != 0};
});
```

- Each command `sendInput()` sends is predicted at once: the rule gives a desired
  velocity and jump, `PhysicsWorld::predictCharacterStep` moves the character one tick
  (the same movement code the host's step uses), and the command and resulting
  position, velocity and grounded state are kept for 256 ticks.
- The host's snapshot for tick T carries the character's state after T (with a
  `grounded` pose flag). When it's within 0.02 m and 0.05 m/s of the prediction for T,
  with the same grounded, nothing changes. Otherwise the character is put at the host's
  state (`correctCharacter`) and the commands after T are replayed (`stats().corrections`,
  `replayedTicks`). Replays move the character only: the game's rule isn't called, and
  physics makes no contacts or sensor events. Health, ammo and everything else in the
  record apply as they arrive.
- More than 32 ticks to replay, or no prediction for T, snaps to the host's state, asks
  for a resync (at most once a second) and sets `predictionWarning()` for 3 seconds
  (`predictionResets`). A respawn (new incarnation) starts prediction again.
- The jump a correction would make is hidden: what's drawn keeps a visual offset that
  halves every 50 ms and is gone 150 ms after the correction (`correctionOffset()`).
  Over 1 m, it snaps. Physics, ray casts and the camera's simulation position use the
  corrected state at once; only drawing uses the offset.
- Online, predicted characters collide with static bodies only, on the host (its step)
  and in prediction alike, so the two agree: they pass through dynamic bodies and other
  characters, and moving platforms aren't supported in v1. `--check` and the editor's
  Check network setup say so: a non-sensor kinematic body in a network scene with
  predicted characters is an error, a dynamic one a warning.

`PlaySession` wires it up: `setPhysics(physics())` picks the physics network mode from
the role (`Offline`, `Authority`, `Client`), `prepareStep()` before each physics step
says which characters are predicted, and `present(alpha)` each frame, before the game's
`frame()`, writes `PresentationTransform` for the predicted character (blended between
ticks, plus the offset) and for every interpolated object. `renderWorld` draws
`PresentationTransform` in place of its own blend; `presentedTransform(world, e, alpha)`
(`Engine/Systems.h`) is the same pose for a camera that follows a body. Arena's camera
and rifle use it.

## Combat and events

### Shots

The host decides every hit. After each tick (`endTick`) it records every player's
capsule in a `RewindHistoryBuffer`: for each slot, the first object it owns that has a
`CharacterController` and that the game describes:

```cpp
// The host asks after every tick: team, alive, spawn protection, eye height.
game.network().setCombatTargets([](const World& world, Entity pawn, net::CombatTarget& out) {
    const auto* f = world.get<Fighter>(pawn);
    if (!f) return false;   // not a target
    out = {uint8_t(f->team), f->health > 0, f->protectionTicks > 0, 1.55f};
    return true;
});
```

The capsule comes from the controller (radius and half height, times the Transform's
scale) at the Transform's position. Then, for a shot, the game asks with the command the
shot was fired with:

```cpp
net::ShotQuery q{shooter, eye, direction, 60.0f, &command};   // origin and aim: the host's
net::ShotResult r;
if (game.network().resolveShot(q, r) && r.player) { /* r.hit was hit in life r.hitIncarnation */ }
```

- A client stamps each command's `view_tick` and `view_fraction` with the time its
  interpolation was showing remote players (`sendInput` does it). The host tests the
  players' capsules at that time, blending the two recorded ticks around it
  (`r.rewind` is `Exact`).
- That time has to be one the client could have seen. Relative to the tick the command
  arrived at the host, it can't be newer than the interpolation delay (less a tolerance)
  or older than the client's round trip plus the interpolation delay (plus the tolerance).
  The tolerance is `kViewToleranceTicks` (2 at 60 Hz, scaled to the tick rate) plus one
  snapshot interval. Too new is moved to the newest plausible time (`Clamped`); too old,
  or no view time at all, tests no players (`Refused`): a client can't claim an old view
  to land a hit it never saw. The history's own limit, `max_rewind_ms`, applies as well.
- The round trip is measured by the host: it pings each client four times when it becomes
  Active and then once a second, and uses the worst of the last 8 (`roundTrip(slot)`,
  `stats().rttMs` for the worst client).
- Everything else is tested as it is now, with a Jolt ray that ignores the shooter and
  every live character (their recorded capsules stand in for them). A player counts only
  if the shot reaches it before that world hit: a target behind a static wall can't be
  hit, whatever the client saw.
- The host's own player, and shots with no command (bots), see the present: the latest
  recorded tick (`Present`). Offline, or before anything is recorded, it's a physics ray.
- `r.hitIncarnation` names the life that was hit. A player who respawned since the view
  time is in a new life; the game should refuse the hit (Arena does). Dead players
  aren't in the history; spawn protection is recorded but the game decides what it means.
- Counted in `stats()`: `shotsResolved`, `playerHits`, `worldHits`, `shotsRewound`,
  `shotsClamped`, `shotsRefused`.

Moving cover isn't rewound in v1: doors, platforms and other moving bodies are tested
where they are now, not where they were when the shooter saw the target. A client can
hit someone who had just stepped behind a closing door, or miss someone a moving body
had just uncovered. Only players are rewound.

### Game events

The host tells everyone what happened with registered events (`EventDesc`, with a typed
payload, in the game's `registerNetworkTypes`). Durable things, such as health, ammo and
scores, go in replicated state; events are for what happens once, such as a shot, a
kill or a round ending.

```cpp
net::ProfileValue v;   // fields in the event's payload order
v.fields[0].u = id;
game.network().sendEvent(kShotResolved, shooter, v);   // host (and offline) only

uint64_t cursor = game.network().gameEventCursor();    // in beginPlay: only new events
net::ReceivedGameEvent e;
while (game.network().readGameEvent(cursor, e)) { /* e.eventId, e.sequence, e.payload */ }
```

- `sendEvent` checks the event is registered and its payload in range, keeps it for the
  host's own readers, and sends it reliably to every Active client. A client that joins
  later gets the durable state in its baseline and snapshots, never old events, so no
  old kill feed or sound replays.
- Events are numbered from 1 for the session. A client keeps each once, in order: a copy
  or an older number is dropped (`stats().duplicateEvents`). A payload outside its ranges,
  or an event this build doesn't know, sends the client away.
- Each machine keeps the last 128 (`kGameEventHistory`). Each reader has its own cursor,
  so the game and circuits both see every event once; a reader that falls further behind
  skips to the oldest kept (`stats().gameEventsMissed`).
- Session events (joins, leaves, state changes) can be read the same way with
  `readEvent(cursor, event)` and `eventCursor()`, besides the game's `pollEvent`.

### Showing each shot once

`PresentationDedup` names a shot by its shooter's network ID, life, input tick and an
ordinal within that tick. A client shows its own shot at once (`predicted(key)`: flash,
sound, tracer) and the host's event for it a round trip later then only corrects it
(`confirmed(key, window)` returns `Predicted`): a hit marker, never a second sound. A
prediction can land a tick or two off the host's when the weapon timers drift; with a
window, the nearest unconfirmed prediction of that shooter, life and ordinal within it
matches. Someone else's shot is `New` once and `Duplicate` after.

Arena does this: `arena.shot_resolved` (shooter, life, input tick, ordinal, origin, end,
victim, damage) for every player's shot, `arena.player_died` for the kill feed and
`arena.round_changed`. Its HUD's health, ammo and scores still come from snapshots.

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

A client draws every network object it doesn't predict this way, except Static ones
(up to 512 objects; past that they're drawn at their newest state).
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

A Steam game's package (see [Packaging.md](Packaging.md#steam)) ships Valve's runtime
library from a Steam-enabled engine, never the SDK, `steam_appid.txt` or captures. AppID
480 only goes into a development package (`player --package --dev-package`). A packaged
game started outside Steam is started again by Steam (`SteamAPI_RestartAppIfNecessary`),
except with AppID 480. An engine built without the SDK packages a game that plays
offline.

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

`player --net-capture` records a session (`--host`, the joins or `--net-preview`'s
client) with `makeCapturingTransport()`. Peers appear as opaque numbers (1, 2, ...) in
order of appearance, never Steam IDs. `--net-dump` and `--net-encode` are the command-line
form of the JSON conversion. Captures are never packaged.

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
| `network_cli` | On a copy of Arena: `--check` passing and failing with file:line for settings, manifest and scene mistakes, and circuits using nodes their execution doesn't allow; 120 Hz in both files; `--network-info` text and JSON; the checked-in NetworkSchema.md and Arena's CircuitNodes.md are current; `--steam-status` failing without an AppID or the SDK; every `--net-test` scenario (replication with five clients on the `poor` preset), its JSON and bad arguments; mode combinations exit 2 |
| `network_session` | Real PlaySessions with physics over the loopback hub: Start/Stop 100 times offline and 100 times online (alternating who stops first) with the world restored and nothing left open; start failing for no services, no transport, unavailable services, no authentication on Steam, a taken address and a throwing `beginPlay`; 13 handshake failures, each with its reason; the online pause, save and breakpoint rules; a client in a menu still pumping and collecting input while the host simulates; no events after Stop or `close()`; the shutdown order with a lobby; `SimulationOverrun`; the simulation clock offline (identical to the old loop) and online; a host's falling physics box followed by the client's kinematic copy; the `--net-test` scenarios |
| `network_replication` | Worlds built in different orders binding authored objects by key; IDs in key order; spawns, despawns (and objects destroyed without one), respawn incarnations, owners and a leaving player's objects; a late client getting earlier spawns and despawns; a small budget (players every snapshot, the rest in turn, bytes within the budget) with 0 heap allocations over nine seconds; a baseline torn down halfway leaving the client's world unchanged; a scripted host sending snapshot parts reversed and duplicated, older snapshots, a despawn before an old snapshot, snapshots ahead of their Spawn (held, then applied) and one that never gets it (expires, one resync), old and new incarnations; eleven kinds of malformed authority data, each failing with its reason and closing the connection |
| `network_prediction` | Physics' network API: host steps and client predictions of the same walk and jumps agreeing within 0.1 mm, predicted characters passing through a dynamic box that blocks them offline, `characterState`, `correctCharacter`, bad arguments, client proxies that don't fall and keep their authored components, `applyReplicaBodyPose`, mode changes rebuilding the same bodies, no contact events from prediction; the `--check` rule's errors, warnings and quiet cases; the engine `InputSampler` at 24 to 1000 fps at 60 and 120 Hz (each press in one command, held controls within a frame, the same aim); the `prediction` net-test on all four presets at 60 and 120 Hz (convergence, drawing, replays running no game code, the offset gone within 150 ms) |
| `network_combat` | Lag compensation at 20 ms and 120 ms round trips, with Jolt on the host: a delayed real hit, the present position missed with the same claimed view, a future view clamped, no view time and a forged old one refused, one out of the window refused, the host's own shot at the present, a target behind a static wall, a dead target, a hit from before a respawn naming the old life; a view 12 ticks back a hit at 120 ms but refused at 20 ms; game events under loss and jitter arriving once each in order, refused for an unknown ID, an out-of-range payload or a client sender, none replayed to a late joiner, a reader that falls behind; a scripted host's duplicated and old events kept once; `PresentationDedup`; circuit scopes when compiled, and at run time on a client: a LocalOwner circuit that sets health and the host's circuit not running, the host's events running a Presentation circuit's handler once each, a duplicated event once |
| `network_arena` | Arena on its multiplayer scene with a host and eleven clients, each a real PlaySession with Jolt: six a side, one primary camera per process following the local pawn, each client predicting only its own pawn; a full team and a host-only restart refused; the round starting when everyone is ready; four inputs a tick moving a pawn no faster; ten seconds of autoplay with every client ending on the host's teams, ammo, health, kills and positions, the clients' shots rewound to their view times (none refused) and every shot's event reaching every client once; clients sent away for a shot event, an ownership claim, an unknown command, far-ahead inputs and an over-long move; a client that stops sending (two ticks of fallback, then standing, fire never held); the offline input path at 24, 30, 60, 144, 240 and 1000 fps (same shots and reloads, each press in exactly one command, catch-up ticks, menus) |
| `online_match` | `OnlineMatch` over the Steam mock with real sessions: host, find (compatible and full listings), join, the lobby's details and player count, invite, leave; a mismatched build refused before connecting; invites while idle and while playing; developer direct joins; no services; a capture of a client's session (tickets redacted, peers opaque) round-tripping through JSON; the peer table |
| `arena_online_menu` | Arena's online menu in the real RmlUi document over the Steam mock: every button clicked, searching, the lobby list with a refused lobby's reason, join, cancel and join again, a friend's invite declined, leave, a failed host with Retry, invite; greyed out without a match control; the panels side by side at 720p |
| `package_steam` | A copy of Arena packaged as a Steam game: AppID 480 refused, then a development package; the runtime library shipped (Steam builds) or the offline-only warning (others); no `steam_appid.txt` or captures shipped; the folder moved and run from elsewhere (`--check`, and `--host` failing with the reason without the SDK) |
| `test_steam_services` | Without the SDK, `create()` fails visibly; then on `MockSteamApi`: init failure, bad settings, double init, a dedicated server, shutdown exactly once, an idle `framePass` allocating nothing; lobbies (create, metadata, find, join, members, leave); late lobby results after cancel and after a restart; auth both ways, a ticket for someone else, sizes, duplicates, revocation, cancellation, the 15-second timeout, stale results, everything cancelled and ended at shutdown |
| `test_sdr_transport_mock` | The connection state table (Incoming, refusals when full or late, Rejected with the host's reason, no listener, our own Connecting, Connected and Closed once, stale handles, queue overflow); listen, poll-group, connect and accept failures and their cleanup; sizes, send flags and every Steam send result; the 32 KiB backpressure; budgets and the ring buffer; every Steam message released once, including when the sink throws; broadcast; RTT; shutdown with transports alive; 0 allocations over 10,000 messages from four clients with echoes |
| `test_snapshot_delta` | Quantization limits and round trips; 64 moving players at 120 Hz in one part under 1152 bytes; keyframes split to fit 1152 and 512; 1024 worst-case slots; loss, reordering, duplicates and lost acks with every completed snapshot identical to the host's; acks and baselines leaving the history; respawns and despawns; every kind of malformed part; one encoder shared by clients; interpolation (Hermite, slerp, extrapolation limit, out-of-order and duplicate samples, teleports, the server clock, the 3-tick jitter buffer); host and client time per tick (under 150 us asserted in optimised builds) |
| `test_rewind_buffer` | Ring sizes; exact ticks, fractions and milliseconds at 120 and 60 Hz; future, slightly-old and too-old times; respawns, deaths and teleports; crouching; ray-capsule geometry; raycasts hitting the past and missing the present, skipping the shooter, picking the nearest; tick wraparound |
| `test_replication_allocations` | 0 heap bytes over 10,000 ticks of 64 players with four clients (encoding, loss, decoding, acks, interpolation, rewind recording, queries and raycasts) |
