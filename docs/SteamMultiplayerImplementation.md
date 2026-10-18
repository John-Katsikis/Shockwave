# Steam and multiplayer implementation specification

Status: DESIGN ONLY. None of the APIs, command-line switches, files, or tests proposed
below exist merely because they are named here. This document authorizes no publishing,
Steam partner configuration changes, or SDK redistribution. Implement it in ordered
increments with the verification gates below. Preserve unrelated working-tree changes.

Repository examined: `650aae5d87a71093107e5ee6251f95929111aa05`, 2026-10-18.

## Design adjustments made during implementation

Recorded here as the document asks, newest last. Status of each batch is in
`docs/NetworkingVerification.md`.

1. Batch 0/1, 2026-10-18, at the project owner's request: protocol v1 accepts tick
   rates from 60 to 120 Hz, not only 60. `tick_rate` in `.swnetwork` and the
   Challenge message's `tick_rate` are validated to 60..120 (still equal to the
   project's `tick_rate`), and `snapshot_rate` must still be one of 10, 15, 20, 30, 60
   and divide it. Every place below that said "60" ticks per second now means the
   session's tick rate.
2. Batch 1: the message catalog and its codecs live in the Network module
   (`Network/Messages.h`) rather than Multiplayer's `NetworkProtocol.h`, because packet
   capture dumps (also in Network) must decode every message. Multiplayer's
   `NetworkProtocol.h` keeps game-level IDs and limits (profiles, commands, events).
3. Batch 1: zero allocation after connecting. `ReceivedMessage::bytes` points into the
   transport's preallocated buffer and is valid until the sink callback returns,
   instead of each message owning a heap copy; receivers copy what they keep.
4. Batch 1: rules the tables leave open, now fixed in the decoder: Welcome's
   `player_slot` is 2..12 (slot 1 is the host's own); `max_entities` is 12..4096 like
   `max_replicated_entities`; Spawn with `prefab_id` 0 needs an `authored_key` and a
   spawned prefab has an empty one; `profile_id` is 1..1023; an input command's
   `move_x`/`move_z` vector may be no longer than 128 (a normalised diagonal encodes
   as about 90, 90); Pong's `responder_send_us` can't precede `responder_receive_us`;
   AuthTicket holds 1..4096 bytes.
Re-read `AGENTS.md`, `docs/Architecture.md`, `docs/UI.md`, and the named integration
points before implementation: another contributor may have changed them.

## 0. Product contract and decisions

Target: bounded-map, twelve-player, six-versus-six shooter; Windows/Linux desktop;
low memory overhead; stable frame pacing; editor and text/CLI parity. First shipping
network topology: one authoritative listen host with up to eleven remote clients.
Steam P2P is the connection mechanism, NOT a distributed simulation authority model.
Clients connect to the host only. No twelve-way full mesh.

Required result:

1. Steam lobby creation, discovery, invites, joining, leaving, and actionable failures.
2. Steam Networking Sockets transport; no deprecated `ISteamNetworking` packet API.
3. Host-authoritative spawning, movement validation, combat, damage, scoring and respawn.
4. Immediate local movement prediction with reconciliation; buffered remote movement.
5. Server-side historical player hit testing with a bounded rewind window.
6. Registered replication descriptors and typed commands, separate from general scene
   reflection. No arbitrary remotely callable method names.
7. Arena is the first client of the shared API, not the place engine networking lives.
8. Deterministic network fault simulation and automated multiplayer tests without Steam.
9. Optional Steam SDK build. Offline builds, editor, tools and existing games still work.
10. Readable configuration, generated schemas, packet inspection, documented wire format.

Explicit exclusions from this implementation: ranked matchmaking; accounts/backend;
dedicated Steam GameServer deployment; host migration; voice; achievements; Steam Input;
cross-store invitations; seamless map travel; world streaming; arbitrary mod downloads;
general rollback of a dynamic Jolt world; destructible-world prediction; client-owned
physics authority; automatic replication of every registered component. Do not implement
stubs advertising these as working. Later dedicated-server code must reuse the same
authority simulation and protocol, but use the Steam GameServer lifecycle separately.

Performance intent: low processing overhead, not a promise of sub-millisecond Internet
synchronization. Record transport RTT, simulation waiting, interpolation delay, and CPU
encode/decode time separately. A 60 Hz tick alone is 16.67 ms. Do not label an in-process
send benchmark as end-to-end gameplay latency.

Mandatory exclusions from the design:

- Never send `sizeof(component)` bytes, pointers, vtables, GLM object representations,
  ECS handles, Jolt BodyIDs, Jolt contact caches or solver memory.
- Never deserialize network bytes directly into live ECS/Jolt storage.
- Never use TCP or a reliable ordered stream for periodic transform snapshots.
- Never let a network callback mutate the World, run a circuit or invoke game code.
- Never put Steam headers or Jolt headers in game-facing networking headers.
- Never add Vulkan/renderer-plugin dependencies to networking.
- Never assume a Steam identity proves the sender's gameplay claims are honest.

These are engineering decisions for Shockwave, not claims that Valve supplies a
replication or prediction system. Valve's open-source networking library explicitly
does not provide entity serialization/delta replication. [S4]

## 1. Verified current integration points

| Existing file | Current behavior | Required integration |
|---|---|---|
| `Source/Runtime/CMakeLists.txt` | Ordered modules; Physics before Circuits and GameFramework | Insert modules as section 2 specifies |
| `cmake/ShockwaveModule.cmake` | `shockwave_module`, public/private dependencies | Use it; no networking source glob in Player |
| `Source/Runtime/Engine/Public/Engine/World.h` | Sparse-set pools; Entity index/generation local to a World; structural writes forbidden during Schedule | Maintain explicit network-ID mapping; commit changes between jobs |
| `Source/Runtime/Engine/Private/Engine.cpp`, `Engine::run` | SDL input update, fixed ticks, then Application::onFrame | Add early application pump; current onFrame is too late for fresh tick input |
| `Source/Runtime/GameFramework/Private/PlaySession.cpp` | Schedule, navigation, physics, GameMode::fixedUpdate, circuits, audio | Insert receive/apply before simulation; replication capture after authoritative gameplay |
| `Source/Runtime/GameFramework/Public/GameFramework/GameMode.h` | GameContext session services; frame and fixed hooks | Add `network()` and an early input hook, preserve old defaults |
| `Source/Runtime/Physics/Private/Physics.cpp` | `step()` synchronizes, updates CharacterVirtual, advances Jolt, publishes contacts/writeback | Extract single-character stepping; expose safe state correction; never replay whole world |
| `Source/Runtime/Engine/Public/Engine/PhysicsComponents.h` | Character desired velocity/jump; velocity/grounded runtime only | Reconciliation needs more than assigning Transform |
| `Source/Runtime/Engine/Private/Systems.cpp` | `PreviousTransform` blended during draw gathering | Add presentation override in Engine, not renderer plugin |
| `Source/Runtime/Circuits/Public/Circuits/CircuitNodes.h` | CircuitHost injects services; Message is local scripting | Inject network facade; do not expose local Message directly to remote peers |
| `Source/Runtime/Circuits/Private/CircuitWorld.cpp` | Main-thread events; frame/tick execution, hot reload and debug pauses | Add execution scope and online reload/pause policy |
| `Source/Programs/Player/Private/Player.cpp` | Imports project assets before scene; owns PlaySession | Own online service separately; offline CLI must never sign into Steam |
| `Source/Editor/Editor/Private/PlayInEditor.cpp` | Stop restores authored World | Stop network first, then restore; no stale callbacks |
| `Source/Editor/Editor/Private/EditorCode.cpp` | Game-module hot reload | Defer while an online session exists |
| `Source/Runtime/Engine/Private/SceneFile.cpp` | Entity links by names or block ordinal; prefab expansion | Do not reuse those identifiers as network identities |
| `Games/Arena/Source/ArenaGame.cpp` | Exactly one local player; input read directly; combat mixed with effects | Refactor into shared command simulation and local presentation |
| `Source/Runtime/Packaging` | Copies binaries/project and validates package | Include Steam runtime only for Steam packages; exclude SDK and appid dev file |

`SteamAudio` already exists. It is spatial audio and is unrelated to Steamworks
identity, lobbies or sockets. Do not modify its configuration or reuse its option.

## 2. Module boundaries, files and ownership

Add three modules. Proposed names below are normative.

### 2.1 Network: platform-neutral messages and services

Location: `Source/Runtime/Network/`; add immediately after Core.
`shockwave_module(Network PUBLIC_DEPS Shockwave::Core)`.

Public headers under `Public/Network/`:

- `NetworkTypes.h`: opaque endpoint/connection/peer IDs, statuses, errors, capabilities.
- `Transport.h`: `ITransport`, `ReceivedMessage`, transport events and stats.
- `OnlineServices.h`: `IOnlineServices`, lobby descriptions/requests/events, service factory inputs.
- `Wire.h`: explicit bounded byte writer/reader; endian and IEEE754 conversion.
- `Loopback.h`: in-process transport factory and explicit shared test hub.
- `NetworkCapture.h`: versioned packet capture reader/writer and JSON conversion.

Private implementation: `Wire.cpp`, `Loopback.cpp`, `FaultLink.cpp`, `NetworkCapture.cpp`.
No Engine, Physics, Projects, SDL, Steam, ImGui or game dependency.
This module does NOT own replicated entities or gameplay schemas.

`ITransport` is a single networking session endpoint. Only its owning main thread may
call it. It has listen/connect/accept/close/poll/send/stats operations, all nonblocking.
Transport factories return `std::unique_ptr<ITransport>`; no raw owning pointers.
Separate connection generation from backend handles. An event includes endpoint
generation so stale events cannot target a new session reusing a slot.

Exact semantic API (use project Result conventions if one exists; otherwise define
`NetResult<T>` with value or structured `NetError`, no exception for routine failures):

```cpp
enum class Delivery { ReliableOrdered, Unreliable };
enum class SendResult { Queued, Dropped, Backpressure, NotConnected, Failed };
struct ConnectionId { uint32_t slot = 0, generation = 0; };
struct PeerId { uint64_t value = 0; }; // opaque; may hold SteamID64, never a player slot
struct TransportAddress { enum Kind { SteamPeer, Loopback } kind; uint64_t value; uint16_t port; };
struct TransportLimits { uint32_t maxConnections = 11; uint32_t maxMessageBytes = 16384; };
// Poll dispatches to a bounded sink; ReceivedMessage owns its bytes until destruction.
// One logical transport message == one wire message. No stream framing ambiguity.
// send() borrows bytes only until it returns. Caller may immediately reuse the buffer.
// close() invalidates ConnectionId immediately, even if the backend lingers.
```

Interface operations: `listen(address, limits)`, `connect(address)`, `accept(id)`,
`close(id, reason)`, `closeAll(reason)`, `poll(eventSink, receiveBudget)`,
`send(id, span<const byte>, delivery)`, `stats(id)`; `listen` succeeds without blocking
for remote peers. An incoming Connecting event must be accepted/rejected promptly.
Do not export a Steam handle as ConnectionId.

`IOnlineServices` owns platform availability, local PeerId, asynchronous lobby
operations and the transport factory. Expose `pump()`, `available()`, `localPeer()`,
`createLobby`, `findLobbies`, `joinLobby`, `leaveLobby`, `inviteFriend`,
`setLobbyJoinable`, `setLobbyMetadata`, `lobbyMembers`, `createTransport` and event drain.
Every asynchronous operation has a monotonically increasing RequestId and service
generation. Cancel invalidates delivery, even if the platform request completes later.
Unavailable implementation returns `Unavailable`, never a fake success.

### 2.2 SteamServices: optional SDK adapter

Location: `Source/Runtime/SteamServices/`; after Network.
`shockwave_module(SteamServices PUBLIC_DEPS Shockwave::Network)`; private Steam target
only when enabled. Public `SteamServices/SteamServices.h` declares a factory, settings
and build capabilities; implementation uses PIMPL. No SDK types in public signatures.

Private files: `SteamRuntime.cpp`, `SteamLobbies.cpp`, `SteamTransport.cpp`,
`SteamAuth.cpp`, `SteamFacade.h`; SDK-disabled factory in `SteamUnavailable.cpp`.
Compile mutually exclusive real/stub sources using explicit conditional source lists
or preprocessor guards compatible with shockwave_module's glob. Do not compile two
definitions of the factory. Internal facade allows fake lifecycle/callback tests.

The application (Player or Editor), not the game module, owns one online service for
the active project. It outlives all its transports and PlaySessions. At most one live
Steam runtime per process. Switching AppID requires shutting down the old service and
all sessions. No callbacks may point into a reloadable game library.

### 2.3 Multiplayer: simulation-facing replication

Location: `Source/Runtime/Multiplayer/`; after Physics, before Circuits/GameFramework.
`shockwave_module(Multiplayer PUBLIC_DEPS Shockwave::Engine Shockwave::Network
PRIVATE_DEPS Shockwave::Physics)`; add Physics publicly only if a public signature
actually requires a Physics header. Do not depend on SteamServices or GameFramework.

Public headers:

- `NetworkSettings.h`: parser/writer/validator/schema for `.swnetwork`.
- `NetworkComponents.h`: authored NetworkObject/NetworkSpawnPoint; runtime identity.
- `Replication.h`: descriptor registry, type catalog, command registry.
- `NetworkWorld.h`: session Subsystem, role/state, entity ownership, command queues.
- `Prediction.h`: command histories, clock mapping, reconciliation metrics.
- `Snapshot.h`: schema-driven snapshot capture/read without scene reflection.
- `NetworkProtocol.h`: wire IDs, limits and field layouts.
- `NetworkManifest.h`: explicit network prefab/scene compatibility catalog.

Private files separated by concern: Settings, Components, Registry, Protocol, Session,
Replication, Prediction, Interpolation, Manifest, Validation. Keep Steam calls out.

GameFramework depends publicly on Multiplayer. Circuits uses a forward-declared
NetworkWorld pointer in CircuitHost and a private Multiplayer dependency. Player and
Editor explicitly link SteamServices and construct the adapter. Engine depends on
neither Multiplayer nor SteamServices. `Application::onPump` is a generic lifecycle
hook, not `Engine::steam()`.

### 2.4 Build configuration

Add `cmake/Steamworks.cmake` and:

```text
SHOCKWAVE_STEAMWORKS=OFF       # default; fully usable offline engine
SHOCKWAVE_STEAMWORKS_SDK=""   # local directory containing public/steam/steam_api.h
SHOCKWAVE_STEAM_TESTS=OFF     # opt-in interactive/manual SDK tests
```

ON with absent headers/library must fail configure with an actionable message.
OFF must not search for/download/link any Steamworks SDK file. ON uses an imported
target for the matching SDK runtime/import library; copy redistributable runtime next
to executables and set relocatable loader paths. Verify target architecture, not just
host OS. Unsupported architecture is a configure error when ON; OFF remains usable
on ARM64/Jetson. Do not assume Steamworks Linux ARM64 redistributables exist.

Do not fetch a proprietary SDK from an unofficial mirror or commit it to the engine.
Developer supplies the SDK. Document the tested SDK version after actually compiling.
The SDK version is not known from this repository inspection; do not invent it.
Optional standalone GameNetworkingSockets/IP transport can be a future backend. Its
open-source distribution does not itself grant Steam authentication/signaling/SDR.
Do not link both implementations into one process casually. [S4]

## 3. Configuration, schema and stable identities

### 3.1 Config/Network.swnetwork

Missing file means offline-only. Strict versioned parser; transactional reads; atomic
temp-file/rename writes. Reject unknown keys, duplicates, invalid enum values and
nonfinite numbers with file:line. Never silently clamp invalid configuration.

Proposed complete version 1 example (APP_ID is an instruction placeholder, not syntax):

```text
shockwave_network 1
enabled 1
transport steam
steam_app_id APP_ID
steam_required 0
lobby_visibility friends
virtual_port 0
max_players 12
tick_rate 60
snapshot_rate 20
input_rate 60
interpolation_ms 100
max_extrapolation_ms 100
max_rewind_ms 200
connect_timeout_seconds 15
handshake_timeout_seconds 15
idle_timeout_seconds 10
max_replicated_entities 512
max_snapshot_bytes 1152
outbound_bytes_per_second 65536
max_pending_reliable_bytes 65536
max_receive_bytes_per_pump 262144
max_receive_messages_per_pump 256
manifest "Config/NetworkManifest.swnetmanifest"
```

Schema bounds:

| Key | Type/range | Default |
|---|---|---|
| enabled, steam_required | bool 0/1 | 0 |
| transport | steam or loopback | steam |
| steam_app_id | unsigned 32-bit integer; nonzero for enabled steam | 0 |
| lobby_visibility | private/friends/public | friends |
| virtual_port | integer 0..999 | 0 |
| max_players | integer 2..12; includes listen host | 12 |
| tick_rate | 60..120 for protocol v1; must equal project tick_rate | 60 |
| snapshot_rate | 10, 15, 20, 30 or 60; divides tick_rate | 20 |
| input_rate | exactly tick_rate in v1 | 60 |
| interpolation_ms | 50..200 | 100 |
| max_extrapolation_ms | 0..100 | 100 |
| max_rewind_ms | 0..250 | 200 |
| timeouts | finite seconds 1..120 | above |
| max_replicated_entities | 12..4096; actual per-map budget validated | 512 |
| max_snapshot_bytes | 512..1152; includes protocol header | 1152 |
| outbound_bytes_per_second | 16384..262144, per remote peer | 65536 |
| max_pending_reliable_bytes | 16384..262144, per peer | 65536 |
| max_receive_bytes_per_pump | 16384..1048576, endpoint total | 262144 |
| max_receive_messages_per_pump | 16..1024 | 256 |
| manifest | project-relative file inside project root | path above |

Runtime host/join addresses, lobby IDs, user tokens and fault simulation settings do
not belong in authored configuration. Put editor-local preferences in Saved. SteamID64
and lobby IDs must be decimal strings in JSON/UI, never double-precision JSON numbers.
Changing tick/schema/manifest requires a new session. Online overrides cannot silently
change host-authoritative gameplay or increase safety limits.

### 3.2 NetworkObject and NetworkSpawnPoint

Registered authored components, using the existing component metadata inspector:

```text
NetworkObject
  key "door-west"                  # required unique stable key for an authored root
  prefab "Prefabs/Net Door.swprefab" # optional on authored object; cataloged for dynamic spawn
  profile "game.door.v1"            # required registered replication profile
  mode Interpolated                # Static, Interpolated, PredictedCharacter
  always_relevant 1                # v1 requires true; interest management is deferred
NetworkSpawnPoint
  team 0                           # 0 or 1 for Arena; engine metadata range 0..31
  order 0                          # deterministic selection; duplicate team/order invalid
```

`key` is authored once, not derived from entity names, block positions or hierarchy
traversal. Editor creates UUID-form strings for new objects, and regenerates keys on
duplicate; manual descriptive keys are legal and validated unique. Renaming an entity
does not change its key. A prefab template may leave its root key empty: scene copies
must override it; runtime spawns receive a network ID from authority. Do NOT regenerate
an existing scene key whenever a prefab hot-reloads.

Version 1 permits one network root per prefab and forbids NetworkObject on nested
prefab parts. Replicate part state through that root's explicitly registered fields.
Require network simulation roots to be unparented, uniform unit scale, with presentation
children parented normally. This avoids unspecified local/world pose conversion.

Runtime-only, unregistered fields/components: `NetworkIdentity { uint64_t id;
uint16_t ownerSlot; uint32_t incarnation; }`, interpolation history, acknowledged input,
peer connections. IDs: host assigns increasing uint64 starting at 1, never reuses within
a session; zero means none. Player slots: host 1, remotes 2..12, zero means no player.
`incarnation` increments on respawn/teleport to invalidate prediction/hit history.
Steam PeerId, player slot, NetworkEntityId and local Entity are distinct types.
Maintain both networkId->Entity and Entity->networkId maps; check World::alive before use.

Authored objects are bound by key via the host's reliable initial catalog. Dynamic
objects are instantiated only from preloaded, allowlisted prefab IDs in the manifest.
A peer cannot request an arbitrary filesystem path. Static unreplicated world geometry
is identical by compatibility digest; do not send every wall every tick.

### 3.3 Explicit network manifest

New `Config/NetworkManifest.swnetmanifest`, generated and editable text:

```text
shockwave_network_manifest 1
game_protocol "arena.v1"
scene 1 "Scenes/arena-mp.swscene"
prefab 1 "Prefabs/Net Player.swprefab" "arena.player.v1"
prefab 2 "Prefabs/Net Crate.swprefab" "engine.rigidbody.v1"
```

IDs are explicit u16 nonzero, stable across builds; never directory enumeration order.
Duplicate IDs/paths, escaped paths or unregistered profiles are errors. Initial version
preloads the selected scene before joining and only advertises that scene. Joining an
incompatible loaded scene reports `MapNotLoaded`; do not implement blocking map travel
inside a Steam callback. A later async loader can extend this contract.

Compatibility digests: SHA-256 of canonical descriptor catalog plus SHA-256 of sorted
project-relative file paths and bytes for the selected scene, its prefab dependencies,
game circuits, manifest and explicit game_protocol. Include model source dependencies
used for collision, physics settings, movement/weapon constants and collision masks.
For v1 it is acceptable to hash ALL Content source files for correctness; exclude
Saved/cache/absolute paths/timestamps and user graphics preferences. Record that this
conservative choice rejects cosmetic-only content differences. Do not hash platform
game DLL bytes: Windows and Linux binaries differ. C++ gameplay semantic changes require
bumping game_protocol, checked into source. Hash is compatibility checking, not anti-cheat.
Provide canonical golden digest tests and a known-vector-tested SHA-256 implementation
or an existing audited dependency already available; do not use std::hash.

## 4. Steam lifecycle and lobby workflow

### 4.1 Lifecycle

Initialize only when an online action is explicitly requested, or a packaged Steam game
requires it. `--check`, schemas, captures, benchmarks and offline tests never initialize
Steam. `steam_required=0` permits offline play after failure; an explicit host/join
request still fails visibly, never silently creates an offline match.

Player startup may use `SteamAPI_RestartAppIfNecessary(appId)` only for normal packaged
Steam launch, before initializing the engine/window. Never relaunch the editor, tool,
test or debugger implicitly. Development uses a user-supplied `steam_appid.txt` in a
documented working directory. Never auto-write an arbitrary AppID or ship that file.
Call `SteamAPI_Init()`, check success, register callback owners, query local identity.
Pump `SteamAPI_RunCallbacks()` on the application's main thread, including menus and
editing. Dispose callback owners before `SteamAPI_Shutdown()`, exactly once. [S2]

Call `SteamNetworkingUtils()->InitRelayNetworkAccess()` during online initialization;
track relay availability without blocking the UI. Transport Connected is distinct from
application Ready and from lobby membership. Use structured status transitions. [S8]

SDK debug callbacks can originate on library threads: enqueue bounded diagnostic text;
never access game/UI objects from them. Platform callback handlers append service events;
main-thread drain performs session operations outside callback dispatch.

Application pump remains active during lobby menus and network waiting. Preload assets
BEFORE opening/advertising listen sockets. Do not spend seconds importing assets after
advertising a joinable lobby. Long blocking callbacks and loading stalls are defects.

### 4.2 Host

1. Validate settings, content, registered codecs and map; stop on errors.
2. Create Steam service if absent; fail visibly if unavailable.
3. Create poll group and P2P listener on configured virtual port; handle failure cleanup.
4. Create lobby asynchronously with total member limit max_players. Use a persistent
   CCallResult owner with RequestId/generation, not a stack callback.
5. After success set metadata; only then mark lobby joinable. On metadata failure leave
   lobby and close transport; do not present HostSucceeded early.
6. Host local player occupies slot 1. It uses the same command validation/gameplay code
   as remote players, through a local queue, without serializing through Steam.
7. Maintain reservations for Connecting/Handshake peers; lobby capacity alone is not
   the game-slot admission authority.

Lobby metadata keys: `sw_protocol="1"`, `sw_game`, `sw_schema`, `sw_content`,
`sw_scene` (numeric scene ID as decimal string), `sw_host` (SteamID64 string),
`sw_session` (128-bit random session nonce encoded hex), `sw_state` (waiting/playing),
`sw_port`, `sw_players`, `sw_capacity`. No secrets in metadata.

### 4.3 Discover/join/invite

FindLobbies filters sw_game/sw_protocol and available slots, returns bounded results
(max 50). JoinLobby completion precedes reading authoritative owner/current metadata.
Verify advertised host equals lobby owner for this no-migration design; reject stale or
incomplete metadata. Validate local schema/content/scene before connecting. API lobby
creation/join results and membership callbacks are asynchronous. [S3, S9]

Join uses `SteamNetworkingIdentity::SetSteamID64(host)` and `ConnectP2P` with the same
virtual port. Direct SteamID join is a developer-only explicit action; it still performs
the full application handshake and host admission policy. Production friends/private
lobbies require actual current membership checked on the host, not a claimed lobby ID.

Handle invites through `GameLobbyJoinRequested_t` and cold-launch `+connect_lobby <id>`;
validate the ID and route through the same join action. Show a pending join in the UI
when an existing match must be left; never replace the current session from a callback.
Invite uses Steam's lobby invitation/overlay APIs; don't implement OS URL shell commands
using untrusted IDs. [S10]

If lobby owner changes or original host disconnects: end match with HostLeft, clear
advertising and leave lobby. Steam's automatic owner reassignment is NOT host migration.
Reconnect is a new connection/admission/baseline and initially a new player slot; no
resume-token system in v1. Remove dead connections and reservations on every failure.

### 4.4 Transport callback handling

Steam facts: create P2P listener/connect, use a poll group, and accept or close incoming
Connecting connections promptly. Connected only means the transport is available. [S1]

Implement a state table:

| SDK condition | Adapter action |
|---|---|
| Connecting on our listen socket | Validate identity/capacity/listen generation; assign ConnectionId; queue Incoming; accept/reject during same pump |
| Connecting on our outgoing connection | Keep waiting; do not accept your own outgoing connection |
| FindingRoute | Report progress, keep deadline |
| Connected | Assign poll group (check result), emit Connected once |
| ClosedByPeer / ProblemDetectedLocally | Emit terminal event once, close handle even though SDK reports closed, invalidate mappings |
| Stale/foreign handle callback | Ignore safely, never dereference previous session data |

Use an owning map keyed by Steam handle + endpoint generation, not raw userdata pointers
to NetworkWorld. Handle association failures by closing the connection. Receive in
batches of at most 32, bounded by configured pump budgets. Use RAII to Release every
received SteamNetworkingMessage_t even on decode rejection or exception. The adapter
copies into bounded owned packet buffers before handing data to gameplay. Send uses
`SendMessageToConnection`; check its result. Explicitly document that this is bounded
copying, not a claimed zero-copy pipeline. [S5]

Reliable controls use `k_nSteamNetworkingSend_Reliable | k_nSteamNetworkingSend_NoNagle`.
Unreliable snapshots/inputs use `k_nSteamNetworkingSend_Unreliable |
k_nSteamNetworkingSend_NoNagle`. Verify constants in the supplied SDK headers; do not
invent numeric flag values. Latest-only coalescing occurs before sending; don't
continuously enqueue old unreliable state into a saturated connection. Poll connection
queue stats and stop submitting snapshots when pending sends exceed 32 KiB. Drain at
the next pump; discard obsolete snapshots. A flush is not an RTT guarantee. [S6]

Use one connection and the ordinary send API in v1; no large file transfers or chat
history over its reliable lane. Additional lanes are a future optimisation, not needed
to make the first prototype work.

### 4.5 Authentication

Bind PeerId to the authenticated transport identity, never to a player-supplied field.
For Steam v1 also mutually exchange session tickets between client and authority:
GetAuthSessionTicket for the intended remote identity, wait for its completion, then
send through the connected socket. BeginAuthSession acceptance alone is provisional;
wait for ValidateAuthTicketResponse before admitting gameplay. On disconnect call
EndAuthSession and cancel locally issued tickets; ignore callbacks from retired
session generations. Invalid/revoked tickets disconnect. [S7]

This spec deliberately chooses a fail-closed 15-second admission timeout if validation
does not complete. Report AuthUnavailable separately from InvalidTicket. It is a product
policy, not a claim that Valve requires that timeout. Cap each ticket at 4096 bytes;
reject oversized tickets safely and document any SDK incompatibility before changing
the protocol cap. Never log or capture ticket bytes. The host validates each client and
each client validates the host; clients have no direct gameplay connections to each
other. No Web API/publisher secret in engine binaries, project files, CI or lobby data.

## 5. Session lifecycle and threading

NetworkWorld is a PlaySession-owned Subsystem. State: Offline, Starting, Connecting,
Authenticating, Synchronizing, Active, Closing, Failed. Role is Offline, Authority or
Client, chosen before GameMode::beginPlay and immutable until Stop. Store role and
state separately. Offline `game.network()` always exists, reports local authority,
and has zero socket/background activity.

Player/Editor own IOnlineServices; `PlaySession::setNetworkStart(NetworkStart, services*)`
injects non-owning service reference before start. NetworkWorld owns its transport.
Services must outlive session. No global `NetworkWorld::current`.

GameContext gains `NetworkWorld& network() const`. GameMode gains default no-op
`collectInput(GameContext&, double frameDt)` called by PlaySession before fixed ticks.
Application gains default no-op `onPump(double rawFrameDt)` immediately after SDL input
update and before the accumulator loop. Application pump order:

```text
onlineServices.pump()                         # once per outer loop, even outside Play
drain online service events                   # never inside Steam callback
session.pumpNetwork(monotonicNow)             # transport receive/decode into queues
session.collectInput(rawFrameDt)              # main thread, latch local actions once
zero or more session.update(fixedDt)
session.frame(frameDt)                        # local presentation and existing hooks
```

Also drain transport (NOT another SteamAPI_RunCallbacks) before each catch-up fixed
tick, so one slow render doesn't delay an entire batch of incoming commands. Do not
add an application-owned busy networking thread in v1; Steam's library services the
socket internally. Main-thread bounded pumps and measured stalls are the starting
design. Worker threads never see Steam objects or mutable transport state.

For online sessions, simulation elapsed time uses clamped raw monotonic elapsed time,
not FrameTimeSmoother's visual smoothing. Add a generic EngineConfig time-source flag
defaulting to existing offline behavior. On an online stall >250 ms, report overload
and resynchronize/disconnect; do not integrate one giant physics step. Catch-up at most
8 fixed ticks per outer loop; preserve bounded residual debt, never silently call a
network tick that skipped simulation. If debt exceeds 250 ms, terminate the online
session as SimulationOverrun. Client clock discipline below adjusts scheduling without
changing fixedDt. Add tests for this behavior, preserve offline defaults.

Fixed tick contract, with all Schedule jobs complete at each boundary:

1. Drain and validate decoded messages; apply reliable lifecycle transactions.
2. Client: reconcile latest valid owner state before advancing its next prediction tick.
3. `storePreviousTransforms` for simulation interpolation.
4. NetworkWorld selects immutable validated input commands for this tick.
5. GameMode/Schedule pre-physics movement consumes commands, never polls hardware.
6. Navigation and physics run according to authority/prediction role (section 9).
7. GameMode::fixedUpdate handles authoritative shots/rules; clients presentation only.
8. Circuits execute according to execution scope, not identically on all peers.
9. Capture authoritative hit history AFTER movement, BEFORE applying any visual offsets.
10. Capture replication state AFTER gameplay/circuits changed health, doors and score.
11. Enqueue bounded snapshots/events; audio synchronizes; advance session tick.

No structural World changes during schedule jobs. Network mutation occurs at steps 1/2
and game-owned command flush boundaries. Registry descriptors are frozen for the
session; do not retain pointers into component vectors across mutation.

Online pause means local menu/input suppression, never pausing host simulation. Disable
editor Pause and circuit breakpoints during online Play with an explanatory tooltip.
Offline Pause retains existing semantics. Online clients continue pumping when menus,
overlay or unfocused windows suppress their local actions; submit neutral movement.

Online hot reload policy: defer game code, scene/prefab/circuit/collision-asset reload
until session Stop; show pending changes. Do not mutate schemas halfway through a
match. Renderer reload also should be deferred by the app while online if it stalls
the tick; no renderer module edits. Save/load commands fail `UnavailableDuringNetworkPlay`
before touching save files/world; offline save behavior unchanged.

Shutdown order (idempotent even after partial start): mark Closing/invalidate generation;
reject commands; disable lobby joins; best-effort goodbye; close connections/listener;
drain/release messages; destroy poll group; end/cancel auth sessions; leave lobby;
clear prediction/maps/history and callbacks; destroy NetworkWorld BEFORE PhysicsWorld;
then restore PlaySession's authored snapshot. Stop may queue disconnect handling but
must not wait indefinitely. Application finally destroys service and shuts Steam down.
Arrange Subsystem insertion order or explicit close() so reverse destruction honors it.

## 6. Wire protocol v1: implement before real gameplay

This entire section defines Shockwave's own protocol. Do not infer C++ memory layouts
from it. Write integers individually in little endian. Float32 uses IEEE754 bit_cast
to uint32 then endian encoding; assert the platform float representation at compile
time. No packed structs, unaligned pointer casts, native bool/size_t or C++ enums on wire.
All lengths are bytes. Reader methods return failure without advancing past the buffer.
Decode into temporary validated values; commit only after the whole message succeeds.

### 6.1 Common header: exactly 32 bytes

| Offset | Bytes | Meaning |
|---|---|---|
| 0 | 4 | ASCII SWMP |
| 4 | 2 | protocol version = 1 |
| 6 | 1 | MessageKind below |
| 7 | 1 | flags = 0; reject unsupported bits |
| 8 | 16 | session nonce; generated by host with OS entropy |
| 24 | 4 | sender simulation tick, 0 for pregame; never used as unquestioned authority |
| 28 | 4 | payload bytes; must equal received size minus 32 |

Hello may carry a zero nonce for developer direct join. All responses and subsequent
messages must carry the nonce from Challenge; lobby join verifies it against metadata.
Connection/endpoint generation remains local and is checked in addition to wire nonce.
Nonces distinguish sessions, not authentication credentials. Obtain cryptographic
randomness from the platform API; fail initialization if unavailable; no rand/time seeds.

Common field notation used below:

- u8/u16/u32/u64: exact unsigned width. i8/i16/i32: two's-complement signed width.
- f32: finite float; reject NaN/infinity before any arithmetic.
- string(N): u16 byte count, 0..N bytes of valid UTF-8, no NUL; never printf format text.
- bytes(N): u16 count followed by 0..N opaque bytes.
- digest: 32 bytes, SHA-256. vec3: three f32, xyz. quat: four f32, xyzw.
- entity: u64 nonzero network ID; player: u16 slot, zero only where explicitly allowed.
- No trailing bytes permitted for v1 known message kinds.
- u32 tick/sequence comparisons: newer(a,b) iff a != b and (uint32_t)(a-b) < 0x80000000.
  All windows <2^31; test wraparound. Do not rely on implementation-defined signed casts.

Hard limits independent of settings: reliable message <=16384 bytes incl header;
unreliable <=1152 bytes incl header; one game command payload <=256 bytes; game event
payload <=512; one state record <=512; initial baseline <=4 MiB; <=4096 live network
objects; <=32 incomplete snapshot groups; handshake has no entity allocations until
compatibility and authentication pass. Bound every count before reserve/allocation.

### 6.2 Message catalog

Delivery R = reliable ordered. U = unreliable. Role/state restrictions are mandatory.
Implement one descriptor table used by decoder, packet dump and generated schema.

| ID / name | Delivery / direction | Payload in exact order |
|---|---|---|
| 1 Hello | R client->host, new connection | game_protocol string(64), schema digest, content digest, scene_id u16, lobby_id u64 (0 developer direct), client_nonce 16 bytes |
| 2 Challenge | R host->client, compatible Hello | echoed client_nonce 16 bytes, game_protocol string(64), schema digest, content digest, scene_id u16, host_peer u64, tick_rate u16, snapshot_rate u16, interpolation_ms u16, max_rewind_ms u16 |
| 3 AuthTicket | R either direction, authenticating | ticket bytes(4096); identity comes from transport, not this message |
| 4 Welcome | R host->client after mutual auth | player_slot u16, baseline_id u32, server_tick u32, max_entities u16 |
| 5 BaselineBegin | R host->client | baseline_id u32, baseline_tick u32, object_count u16, total_state_bytes u32 |
| 6 Spawn | R host->client | control_serial u32, entity u64, incarnation u32, owner_slot u16, prefab_id u16 (0 authored), authored_key string(96), profile_id u16, initial state bytes(512) |
| 7 Despawn | R host->client | control_serial u32, entity u64, reason u8 |
| 8 Ownership | R host->client | control_serial u32, entity u64, incarnation u32, owner_slot u16 |
| 9 InitialState | R host->client, baseline/resync only | baseline_id u32, record_count u16, repeated state records from 6.3; fit message bound |
| 10 BaselineEnd | R host->client | baseline_id u32, final_control_serial u32, baseline_digest 32 bytes |
| 11 Ready | R client->host | baseline_id u32, baseline_digest 32 bytes |
| 12 Active | R host->client | start_tick u32, first_input_tick u32, controlled_entity u64, incarnation u32 |
| 13 Inputs | U client->host, active | count u8 (1..4), repeated input commands from 8.1 |
| 14 Snapshot | U host->client, active | snapshot_id u32, server_tick u32, required_control_serial u32, part_index u16, part_count u16, record_count u16, repeated state records |
| 15 Command | R client->host, active | command_seq u32, target entity u64, command_id u16, payload bytes(256) |
| 16 GameEvent | R host->client, active | event_seq u32, event_tick u32, subject entity u64 (0 for match event), event_id u16, payload bytes(512) |
| 17 Ping | U either, after Challenge | ping_id u32, origin_monotonic_us u64 |
| 18 Pong | U reply | ping_id u32, echoed_origin_us u64, responder_receive_us u64, responder_send_us u64, responder_tick u32 |
| 19 Reject | R host->client | reason_code u16, diagnostic string(160) |
| 20 Goodbye | R either | reason_code u16 |
| 21 ResyncRequest | R client->host | last_applied_server_tick u32, reason u8 |

Sender cannot choose reliable/unreliable arbitrarily: enforce catalog delivery and
direction. SDK transport reliability flags are a transport detail; do not trust an
in-message claim. Loopback tracks delivery metadata for equivalent tests.

Unknown message ID/version: disconnect with ProtocolMismatch, not silent skipping.
Reject diagnostics must not include absolute paths, tickets or unchecked remote text.
Define stable reason_code enum: Normal=0, ProtocolMismatch=1, ContentMismatch=2,
ServerFull=3, NotLobbyMember=4, AuthenticationFailed=5, AuthUnavailable=6,
Timeout=7, MalformedPacket=8, RateLimited=9, HostLeft=10, MapNotLoaded=11,
SimulationOverrun=12, ServiceUnavailable=13, UnsupportedFeature=14.
Map to valid SDK application close-code ranges privately; don't pass these directly
as undocumented Steam end-code numbers.

### 6.3 State records and profiles

Record: entity u64, incarnation u32, profile_id u16, state_bytes u16, codec payload.
Profile_id is explicit from the registered schema catalog. Every Snapshot record is
self-contained full state in v1. No delta baselines, no assumption that previous
unreliable packets arrived. Spawn also contains full initial state. InitialState can
refresh the same entity after Spawn; duplicate initial assignment must be harmless.

Builtin profiles:

- ID 1 `engine.pose.v1`: position vec3, rotation quat, linear_velocity vec3,
  angular_velocity vec3, flags u8 (bit0 sleeping, bit1 teleport; others zero).
  53 payload bytes, 69 bytes incl record header. Require quaternion norm 0.99..1.01,
  normalize after validation; positions abs <=32768 m, velocity components abs <=512
  m/s, angular velocity abs <=1024 rad/s. Reject out-of-domain authority state locally
  with diagnostics; don't transmit a silently clamped pose.
- ID 2 `engine.rigidbody.v1`: same pose payload; authority-only simulation. Mass,
  shape and friction remain prefab/content data, not constantly transmitted.
- IDs 3..31 reserved for engine; 32..1023 game profiles. Collision is a startup error.
- ID 32 `arena.player.v1`: pose payload above followed by yaw f32, pitch f32,
  health u16 (0..10000), ammo u16, reload_remaining_ticks u16,
  protection_remaining_ticks u16, team u8, life_state u8 (alive/dead),
  grounded u8 (0/1), reserved u8=0, kills u16, deaths u16.
  Pose velocity is the character's velocity. This profile is sufficient to seed
  client correction; it does not promise identical Jolt contact caches.
- ID 33 `arena.rules.v1`: blue_score u16, orange_score u16, remaining_ticks u32,
  round_serial u32, phase u8 (waiting/playing/finished). Singleton network root.

For each profile register explicit encoder, decoder/validator, capture and apply
callbacks, plus ordered field descriptors (name/type/range/semantic role). Registration
must not use runtime componentId as a protocol ID. Store descriptors once, freeze before
session start, precompute IDs and field access. Runtime replication calls typed
codecs; it does not serialize through scene text or look up field names every tick.

Proposed registration API in `Replication.h`:

```cpp
struct ReplicationProfile {
    uint16_t id;
    std::string name;                  // e.g. arena.player.v1
    uint16_t maxEncodedBytes;
    std::vector<NetFieldDesc> fields;  // ordered; schema/check/dump source of truth
    CaptureFn capture;               // World + Entity -> validated snapshot value
    EncodeFn encode;                 // snapshot value -> WireWriter
    DecodeFn decode;                 // WireReader -> validated snapshot value
    ApplyFn apply;                   // validated value -> replica, at safe boundary
};
```

Do not let void* callbacks outlive game code. Registry is owned by the application's
game-module context; create before loading scenes, destroy after session teardown and
before unloading the game library. Add `GameModule::registerNetworkTypes(Registry&)`
and `GameInstance`/PlaySession registry injection. All modules/executables must be
rebuilt for the changed ABI; bump the existing module ABI version if present, otherwise
introduce and validate one in GameModuleEntry/library loader. No reuse of an old binary.

Quantization/delta encoding are a later measured optimization. Add them only behind a
new codec/version and golden packet tests. Twelve player records at 20 Hz already
fit a modest bandwidth budget without dangerous raw-memory tricks.

### 6.4 Baseline, spawning, loss and ordering

After Welcome, freeze a bounded snapshot of the host's current object catalog/state
at tick B on the main thread. Serialize/transmit the copy incrementally; do not freeze
the actual game while the joining peer downloads. BaselineBegin -> Spawn catalog ->
InitialState chunks -> BaselineEnd. Hash canonical catalog and exact state bytes for
baseline_digest. Queue post-B lifecycle changes in reliable order for that peer.

Client stages the baseline in bounded temporary storage. Validate every prefab/profile,
unique ID/key/owner and count before committing. Spawn only local allowlisted prefabs;
bind authored roots by key; reject conflicting duplicates. No player input before
baseline complete. Ready acknowledges the exact baseline. Host sends all queued
lifecycle changes, then Active; send a fresh current-state refresh immediately after
activation. If queued mutations exceed limits, abort/restart the baseline once, then
disconnect a client that cannot catch up. Timeout total synchronization at 30 seconds.

control_serial starts at 1 and increases for Spawn/Despawn/Ownership. Reliable messages
are applied in order. Each Snapshot gives the latest required_control_serial. If a
snapshot arrives ahead of its lifecycle control, retain at most 32 groups/256 KiB for
at most 250 ms; then discard and request a rate-limited resync. Never infer that an
unknown entity should be spawned from an unreliable transform.

part_count is 1..256; part_index < part_count. Every part has one snapshot_id/tick.
Records may be applied independently once lifecycle requirements are satisfied;
track last accepted tick per entity/incarnation, not a global last packet. Receiving
part 1 must not make part 0 look stale. Each entity appears at most once per snapshot;
duplicate parts are ignored. Missing a part does not remove an entity or block newer
snapshots. Retire bookkeeping after 250 ms. Despawn tombstones cover the session because
IDs are not reused; bound them by total spawn budget (65536 per match), then roll to a
new match/session instead of unbounded allocation.

Respawn/teleport increments incarnation and sends a reliable lifecycle update with
full new state (use Spawn for the existing ID with strictly newer incarnation, defined
as ReplaceState, not a second entity). Clear all old histories/events for that incarnation.
Ownership changes are authority-only; v1 permits assignment at spawn/respawn, not ad-hoc
mid-life handoff. Client cannot create owner slots through its own messages.

### 6.5 Bounds, queues and scheduling

- Max 4 input records/message, max 120 input messages/second/peer, token-bucket burst 16.
- Max 20 reliable gameplay commands/second/peer, burst 10; handler-specific lower limits.
- Pre-auth receive budget 16 KiB/second, burst 8 KiB, max 4 pending peers beyond capacity.
- Reject malformed packet immediately. Count/rate-limit semantic command rejections;
  disconnect after 10 ownership/illegal-command violations in 10 seconds.
- Outbound token bucket per peer (default 64 KiB/s; burst 16 KiB). Budget includes wire
  header/payload but report SDK/transport overhead separately when available.
- Control traffic has priority, then local-owner correction, other players, dynamic props.
- Reserve 16 KiB/s of default peer budget for lifecycle/events. Control queue overflow
  disconnects with a clear error; it must never silently drop despawn/ownership.
- Replace obsolete pending snapshot state instead of appending endlessly. Fixed-size
  rings/pools after join; cap payload buffers. No per-entity heap allocation per tick.
- At maximum settings, even control traffic is bounded; slow peers cannot stall host.
- Default v1 sends every player at snapshot_rate; round-robin props within remaining
  budget. A static entity can be sent only in baseline and when explicitly changed.
- No occlusion-based interest filtering yet. It is correctness-sensitive for combat.
- ResyncRequest: <=1/second and <=3/minute before marking connection unhealthy.

## 7. Authoritative command model

Clients send intent. Only authority decides legal outcome. A remote request must never
contain trusted damage, target hit, score increment, world transform, entity owner,
asset path, script source, method pointer or arbitrary reflected property assignment.

Register commands with stable u16 ID, name, payload schema, direction, rate limit,
target profile allowlist and permissions; handler executes at the safe tick boundary.
Game-facing facade:

```cpp
network.role(); network.state(); network.localPlayer();
network.hasAuthority(entity); network.isLocallyControlled(entity);
network.submitCommand(commandId, targetNetworkId, typedPayload);
network.spawn(prefabId, initialPose, ownerSlot); // authority only; validated catalog
network.despawn(networkId);                    // authority only
network.peers(); network.stats();              // immutable main-thread views
```

On a client, submitCommand encodes into bounded outgoing queue. On host/offline, it
passes through the same permission/validation handler with a local authenticated
sender context. No game code receives an unchecked span of packet bytes. Sender slot
is supplied by NetworkWorld's connection mapping, not the serialized command.

Command payload types are explicit structs encoded field-by-field. Deduplicate
command_seq per connection; reliable retransmission must not operate a door twice.
Admission assigns each connection a fresh sequence domain. Reject unknown command IDs,
bad target profile, non-owned owner-only request, excessive distance, illegal match
state, dead player action or failed cooldown. A command can target a non-owned door
only if its registered policy explicitly allows interaction with range/LOS validation.

Arena non-input commands: ID 32 RequestTeam {team u8}, only while waiting; ID 33 ReadyToPlay
{ready u8}; ID 34 RequestRoundRestart {}, host-only and only after round end. Client
weapons use input commands, not a reliable Fire RPC per rendered frame.

No user-authored circuit may register a new remote command by sending a name. The
application's registry fixes allowed commands before networking starts. A circuit
node chooses from that catalog and maps explicitly typed fields. This is the extension
point for future trigger-method gameplay without opening every local method remotely.

## 8. Input, clock and prediction protocol

### 8.1 InputCommand exact layout

```text
target_tick u32                  # host tick domain; also command sequence number
incarnation u32                 # controlled pawn life/teleport generation
move_x i8, move_z i8             # -127..127; reject -128; normalized diagonal
yaw_u16 u16                     # [0,65535] maps to [-pi,pi)
pitch_i16 i16                   # [-32767,32767] maps to [-89deg,+89deg]; reject -32768
held_bits u16                   # bit0 fire, bit1 sprint; others zero
pressed_bits u16                # bit0 jump, bit1 reload; others zero
view_tick u32                   # remote interpolation time actually shown
view_fraction u16               # fraction of tick /65536 for lag compensation
```

24 bytes per command. Inputs packet count 1..4 contains newest plus up to 3 previous
unacknowledged commands, oldest first. Sender can't override dt, movement speed or
controlled_entity. Pawn is resolved from authenticated sender slot. Pitch/yaw decoding
uses finite bounded values; all movement parameters originate in authority game data.

InputSampler latches hardware once in collectInput after SDL event polling. Accumulate
mouse delta into current aim exactly once, regardless of how many fixed ticks occur.
Held states persist; pressed edges wait until the next generated fixed command and
are consumed once, including if a frame had zero ticks. Multiple catch-up ticks must
not replay jump/reload edges. Menus/unfocus send neutral movement/fire and release capture.
Keep local camera aim responsive per frame; authoritative simulation receives sampled
fixed-tick aim. Existing Arena frame() must stop directly changing gameplay jump/reload.

### 8.2 Tick mapping and authoritative consumption

Host tick is monotonic modulo u32 and advances exactly once per simulated 1/tick_rate second.
Client estimates host clock with Ping/Pong once per second (four initial probes at
100 ms intervals). Calculate RTT from local send/receive excluding responder dwell;
offset via midpoint timestamps, prefer the lowest-RTT valid sample of last 8, smooth
offset changes, reject negative/absurd samples. Never compare local monotonic epochs
without an offset estimate. Client timing cannot dictate authority's clock.

Client predicts ahead by `ceil(RTT * 0.5 * tick_rate) + 2` ticks, clamped 2..16. Active supplies
initial first_input_tick = host tick + lead; client has a complete state seed and starts
prediction at that target. Host ignores movement until the connection's active tick.
Clock discipline limits client to estimated host tick + lead; if ahead, do not generate
extra commands; if behind, run additional fixed prediction steps within the 8-step
outer-loop budget. Never vary movement dt to catch up. Sudden clock adjustment beyond
8 ticks triggers explicit history reset/resync rather than generating an input flood.

At authority tick T, accept only command for T. Queue future commands up to T+32;
ignore commands <= last processed tick. Deduplicate by target_tick/incarnation.
Missing input: reuse held movement/aim for at most two ticks, with pressed_bits=0 and
fire forced off; after that use neutral movement until fresh input. Gravity still runs.
Host runs one movement step per pawn per authority tick regardless of packet volume.
This prevents speed hacks by sending more commands. Commands >32 ticks ahead are
rejected; persistent violations disconnect. Inputs arriving late never re-run host
movement and never fire retrospective extra bullets.

Owner state is a full arena.player record tagged with authoritative Snapshot server_tick.
That tick acknowledges simulation of all command slots through that tick, including
missing-input fallback. No separate misleading "last received" acknowledgement. Client
may discard history <= state tick only after reconciliation. Keep command+predicted
state rings of 256 ticks per local player. Support wraparound and empty/gapped histories.

### 8.3 Reconciliation

For the newest valid owner record at T:

1. Reject stale incarnation or tick; if new incarnation, snap/reset prediction.
2. Locate predicted state AFTER tick T. Compare position, velocity and grounded state.
3. If distance <=0.02 m, velocity error <=0.05 m/s and grounded matches, retain current
   prediction, discard acknowledged history; still apply authoritative health/ammo.
4. Otherwise restore character to authoritative pose/velocity at T using Physics API,
   refresh contacts and replay saved commands T+1..current predicted tick. Do not replay
   game rules, damage, audio, UI, particles, network sends or other actors.
5. Replay bound 32 ticks per correction. If missing history or larger gap, clear history,
   snap, request resync and show a connection warning; no silent incorrect replay.
6. After replay, store a visual correction offset equal to pre-correction displayed
   position minus corrected simulation position. Decay exponentially with 50 ms half-life,
   max 150 ms; snap if offset >1 m or teleport/incarnation change.
7. Physics and raycasts use corrected simulation pose immediately. Only rendering/camera
   follow the decaying visual offset. Never feed the visual offset back into Jolt.

This is approximate prediction with authoritative correction, not cross-platform
deterministic lockstep. Test convergence across Windows/Linux; never promise bitwise
Jolt state equivalence merely because both peers use the same version.

## 9. Physics integration without global rollback

### 9.1 Required public Physics API additions

Add to `Physics/Physics.h`, implement only inside Physics module:

```cpp
struct CharacterMotionState {
    glm::vec3 position, velocity;
    bool grounded;
};
std::optional<CharacterMotionState> characterState(Entity) const;
bool correctCharacter(Entity, const CharacterMotionState&); // between steps only
bool predictCharacterStep(Entity, glm::vec3 desiredVelocity, bool jump, double fixedDt);
bool applyReplicaBodyPose(Entity, glm::vec3 position, glm::quat rotation,
                          glm::vec3 linearVelocity, glm::vec3 angularVelocity);
```

These are proposed PhysicsWorld methods. Check finite values and entity/type validity;
return false for stale/non-character requests. No JPH types escape. `correctCharacter`
updates Jolt AND ECS runtime state, clears pending jump, updates the internal placed
pose so synchronize() won't undo it. Rebuild/refresh contacts appropriately; Jolt's
RefreshContacts and SetLinearVelocity are relevant public operations. [S11]

Extract the shared CharacterVirtual movement algorithm from current PhysicsWorld::Impl::step
into a private helper. Both normal host stepping and predictCharacterStep call it.
predictCharacterStep must NOT call PhysicsSystem::Update, synchronize the whole World,
publish global contacts, advance timers, move other bodies or run Navigation. It steps
only that character against a stable collision set and writes back that character.
Use an explicit replay flag to suppress side effects/listener gameplay events.

Do not treat Jolt SaveState as a portable network format. Local debug snapshots may
use it privately, but no serialized Jolt buffers cross the socket. The current pinned
Jolt is v5.6.0; verify operations against that header, not a random newer example.

### 9.2 Collision contract for first multiplayer release

Support predicted characters against static world collision only. No player-player
movement collision, no moving platforms and no player-pushable props in predicted
movement in v1. This restriction must appear in --check and editor diagnostics, not
be hidden in a comment. Combat still hits players and dynamic props through separate
authority queries. Dynamic props remain host-simulated and remotely interpolated.

Use an explicit runtime Physics simulation policy injected by NetworkWorld:

- Offline: existing behavior unchanged.
- Authority: full Jolt step; character movement query filter excludes characters and
  non-static bodies for PredictedCharacter actors. Do not globally change other actors.
- Client: synchronize static geometry; manually step only locally owned predicted
  character; no full dynamic-world Jolt integration. Remote characters/props use
  kinematic query proxies updated from latest authoritative simulation state.
- Predicted character query filter ignores those proxies. Render interpolation never
  moves the query proxy into the delayed visual position.

Do not merely change authored RigidBody.motion to Kinematic in a client scene. Store
runtime overrides separately so authored scene and Stop restoration remain correct.
Do not disable every collider: static wall collision must remain active for prediction.
If a project's intended movement requires moving platforms/pushing, reject enabling
this v1 prediction profile with a clear unsupported-feature diagnostic. Extending this
later requires an isolated prediction collision world and histories for moving supports.

### 9.3 Remote interpolation / presentation

Maintain 32 samples per remote entity, sorted by server tick; discard duplicates and
too-old samples. Render at estimated server time minus configured interpolation_ms.
Lerp positions and slerp normalized rotations between bracketing samples. If no future
sample, extrapolate using velocity for <=max_extrapolation_ms, then freeze and flag
stale; don't invent accelerating motion. Teleport/incarnation change clears the buffer.

Add runtime-only `PresentationTransform` to Engine components, consumed by
renderWorld in Engine/Private/Systems.cpp in preference to PreviousTransform for the
entity's local draw transform. Network interpolation writes this component only for
network-controlled entities; offline entities keep existing interpolation. Propagate
through the existing parent matrix traversal. No changes inside Renderer or Vulkan.
Camera helpers and Arena updateView must use the same presentation pose for feet,
camera and view weapon, preventing the previously observed weapon snapping. Local aim
remains latest frame aim; body correction and visual interpolation aren't applied twice.

## 10. Arena multiplayer refactor and combat authority

Keep existing offline arena.swscene and its tests. Create `arena-mp.swscene` plus
`Prefabs/Net Player.swprefab`, multiplayer spawn points and a network rules root.
Avoid copying all implementation into a second giant GameMode. Extract shared logic:

- `ArenaInput.h/.cpp`: InputSampler -> InputCommand; no World mutation.
- `ArenaSimulation.h/.cpp`: movement intent, weapon timers, damage, teams, respawn.
- `ArenaNetwork.h/.cpp`: registered profiles/commands/events, host admission hooks.
- `ArenaPresentation.h/.cpp`: camera/viewmodel, tracers, sound, HUD and menu state.
- `ArenaGame.cpp`: lifecycle orchestration and authority/client policy.

Existing `ArenaFighter.player` means one local player; replace network assumptions with
ownerSlot plus locally-controlled lookup. Retain compatibility with old offline scenes.
Host can have twelve player actors but only one local primary camera/viewmodel/HUD.
Clients create presentation only for their assigned actor; remote viewmodels are not
replicated. No remote camera can become primary. All twelve third-person bodies exist.
Six slots per team enforced on authority. Optional bots run only on authority and
consume slots; no client bot AI. At join choose a team with room or reject; respawn
selects ordered valid team spawn points with explicit collision checks.

Weapon clocks are integer ticks online: cooldown, reload, spawn protection and round
time. Convert authored seconds once with ceil(seconds*tick_rate), never decrement by render
delta. Authority uses fire held plus cadence; jump/reload are command edges. Clients
may predict muzzle flash/recoil, but authoritative ammo/hits/health overwrite prediction.

### 10.1 Historical hit testing

After each authority movement step store player capsules, feet/eye pose, life incarnation,
alive/team/protection state for the last ceil(max_rewind_ms*tick_rate/1000)+2 ticks. Bound history
to 32 samples/player. Include the shooter's pose so the ray origin is host-derived.
Never rewind the live Jolt world or teleport every entity to test one shot.

On a valid fire command at host tick T:

1. Validate shooter alive, ownership, ammo, cadence, reload and match state. Consume
   ammo/cooldown exactly once on authority. Client never supplies damage.
2. Decode aim from that accepted input command. Ray origin = authoritative shooter eye
   at execution T; use a consistent weapon muzzle/eye obstruction rule.
3. Candidate target-history time = command's view_tick+fraction. Constrain to
   [T-max_rewind_ticks, T]. Also constrain to a plausible window around
   T - estimated_one_way_delay - advertised_interpolation_delay with tolerance 2 ticks.
   Reject impossible claims rather than letting client choose any old favorable pose.
4. Analytically ray-test historical player capsules at that time (interpolate between
   same-incarnation samples only). Capsule dimensions from host prefab. No Jolt state
   buffer mutation. Apply current game rules for friendly fire/protection/dead targets;
   reject history spanning death/respawn/teleport discontinuities.
5. Ray-test current static world and current authoritative dynamic blockers separately,
   excluding live player proxies. A historical player hit must precede the closest
   world obstruction. Historical moving cover is unsupported in v1 and documented;
   do not claim fully rewound destructible combat.
6. Select nearest valid target, apply host weapon damage, death/score/respawn changes.
7. Send reliable ShotResolved event and let the next snapshots carry durable state.

Clock estimates are advisory and bounded; low RTT and high RTT tests must exercise the
rewind clamps. Do not compensate the host's local player by an invented network delay.
Host has present-time view; use its actual view tick for its shots.

Arena event catalog, explicit u16 IDs starting at 32:

- ShotResolved(32): shooter entity u64, shooter incarnation u32, input_tick u32,
  shot_ordinal u16, origin vec3, end vec3, victim entity u64 (0 miss), damage u16.
- PlayerDied(33): victim entity, incarnation u32, killer entity (0 environment),
  respawn_tick u32.
- RoundChanged(34): round_serial u32, phase u8, winning_team u8 (255 none).

Presentation dedup key for local predicted shot is (shooter, incarnation, input_tick,
shot_ordinal). Confirmed event reuses/corrects it instead of playing a second sound or
flash. Remote shots create one effect. A hit marker requires authority confirmation.
Health/ammo/score derive from authoritative snapshots, not event arrival alone. Late
join does not replay old kill feed/audio. Score totals survive dropped snapshot packets
because subsequent snapshots contain full state.

## 11. Circuits integration

Do not broadcast CircuitWorld::send's arbitrary strings/values directly. Keep that API
local. Add `execution` metadata to Circuit component: Authority (default), LocalOwner,
Presentation. Offline executes all scopes. Online:

- Authority: only host dispatches gameplay tick/trigger/message handlers.
- LocalOwner: only locally controlled object's input/presentation handlers run.
- Presentation: runs on each peer, limited to explicit presentation-safe nodes.
- Game circuit defaults Authority; local menu logic should be a Presentation circuit.

Add a node capability classification enforced by compiler/runtime: presentation-safe,
authority mutation, network request. Reject presentation/local-owner circuits containing
direct damage/spawn/destroy/physics-force/gameplay property setters. UI/audio/effects
and read-only queries are allowed. Use explicit allowlists, not prefix name guesses.
Avoid pretending scopes alone stop a client from mutating gameplay through generic
component setters. Network requests still execute authority validators.

Nodes to implement: Network.IsAuthority, Network.IsLocalOwner, Network.LocalPlayer,
Network.State, Network.SendCommand (catalog-selected typed command), and events
Network.PeerJoined/PeerLeft/Ready/Disconnected plus registered confirmed game events.
Generate node descriptors from command/event metadata where possible. All callbacks
enqueue for the existing safe dispatch boundary; never recursively invoke circuits
from receive. Offline unknown/missing service reports an actionable diagnostic.
Regenerate node schema. Tests prove an unauthorised circuit cannot apply damage and
duplicate packets don't trigger a handler twice.

## 12. Human-facing editor and game UI

Follow docs/UI.md. New engine controls use gui::button/toolButton/property grids,
theme semantic colors, stable widget IDs and existing undo tracking. No custom hardcoded
cyan/red palette. Add `NetworkPanel.cpp` under Editor, not a networking dependency on UI.

### 12.1 Project settings: Multiplayer section

Edit all .swnetwork fields through the same descriptor table as parser/schema/validator.
Save configuration atomically. Show Steam build capability, SDK availability and service
status separately. Display AppID as text/integer, never floating point. Explain that
no-SDK builds support offline play and loopback tests. Offer Check network setup using
the same validator as CLI. Disable session-immutable fields while online; don't discard
edits silently. Unsaved settings apply only after explicit Save/next session.

### 12.2 Network window

Top: Offline / Host / Join / Leave, role, local identity (copy button), status and
progress/error. Host starts Play on the currently loaded valid network scene. Join
lists compatible lobbies with players/capacity/map/state; refresh is bounded and
asynchronous. Invite opens Steam lobby invite UI. Developer direct-ID join is clearly
labeled; no raw Steam ID required for ordinary friend invitations.

Peer table: slot, display name, team, status, RTT, inbound/outbound KiB/s, pending bytes,
last-input age, snapshot age. Display names are escaped untrusted text, <=64 UTF-8 bytes;
never used as widget IDs or network identity. Detailed metrics: correction count/max,
replay steps, buffer underruns, invalid messages and current bandwidth budget.

Footer: Copy diagnostics, Capture packets (explicit user action; tickets redacted),
Run local test. Fault simulation controls belong to local testing UI; do not expose
them as production matchmaking settings. Saved preferences retain panel layout and
test presets, not auth secrets.

### 12.3 Inspector and authoring

NetworkObject fields come from registered metadata. Add read-only during-Play status:
network ID, owner, authority/local role, last snapshot age and profile. Editor duplicate
generates a fresh key, with undo/redo preserving the chosen generated value. Prefab
instances save key overrides rather than modifying the source prefab's global identity.
Provide Assign missing network keys tool; leave valid existing keys unchanged. CLI
counterpart below. Show unsupported prediction collision configurations inline.

### 12.4 Arena UI

Use Arena's RmlUi, not ImGui, for player menus: Play offline, Host friends match,
Find/Join, Invite, Leave. Show Connecting/Authenticating/Loading state, retry/cancel
and specific errors. In-match menu opens without stopping the match. Preserve existing
graphics settings menu. Scoreboard lists twelve slots by team; joining/departure and
connection warnings update from shared service events. Display confirmed health/ammo;
predicted weapon effects must not fabricate confirmed hits.

On disconnect return to a safe menu/offline scene after stopping NetworkWorld and
restoring scene state. Do not continue running client gameplay as a new silent host.
Every menu action calls the same shared API used by CLI/tests. No direct Steam calls
inside ArenaGame, Rml handlers, editor panels or circuit nodes.

## 13. CLI and text contract

All switches in this section are TO BE IMPLEMENTED. They are not claims about current
player --help. Extend Main.cpp and Player.h with explicit mutually exclusive modes;
put execution in new PlayerNetwork.cpp. Do not put all code into Main.cpp.

| Invocation | Meaning / output |
|---|---|
| `player --network-schema docs/NetworkSchema.md` | Generate config, components, profiles, command/event IDs and wire layout; `-` writes stdout |
| `player --check --project <project>` | Offline validation including network config/scene/manifest/registry; no Steam startup |
| `player --network-info --project <project> --json out.json` | Build capabilities, config, digests and unsupported features; no Steam initialization |
| `player --steam-status --project <project> --json out.json` | Explicit initialize/query/pump Steam for bounded timeout, report identity/relay/auth availability, shutdown |
| `player --host --project <project>` | Load chosen map, then host online; failure exits nonzero |
| `player --join-lobby <decimal-id> --project <project>` | Load chosen map, then join; full handshake |
| `player --join-steam <decimal-id> --dev-direct-join --project <project>` | Developer direct connection; host also explicitly permits direct admission |
| `player --net-test --scenario <name> --seed 123 --json out.json` | Real multiplayer protocol over deterministic in-process LoopbackHub, no SDK/display |
| `player --net-capture <path.swnetcap> ...` | Capture local application messages; never AuthTicket bytes |
| `player --net-dump <path.swnetcap> --json out.json` | Decode offline capture, resolved fields and symbolic message names |
| `player --net-encode <canonical.json> --output out.swnetcap` | Encode canonical decoded records for tests/tool interoperability; reject unsupported/redacted payload encoding |
| `player --network-manifest --project <project> --output <path>` | Validate dependency closure, generate digest report/candidate catalog without overwriting source implicitly |
| `player --assign-network-keys --scene <file> --output <new-file>` | Fill missing/duplicate root keys deterministically from explicit seed or OS randomness; report mapping; preserve valid keys |

`--host`, join switches, package, screenshot, circuit tools and --net-test are mutually
exclusive; reject combinations instead of choosing one silently. Generic --json path
must select output for the active tool. --net-test bypasses engine/window/assets for
pure protocol scenarios; game scenarios use PlaySession and null-renderer fixture
assets without an SDL display. Never launch a Steam client from CTest.

Network JSON top level: version, scenario/mode, ok, errors[], warnings[], build,
config, seed, duration_ticks, peers[], counters, timings, memory. Numbers must be finite;
unknown metrics are null. Peer and entity u64 IDs are decimal strings. Return code
0 success, 1 runtime/validation failure, 2 invalid CLI arguments. Interactive errors
log via SW_LOG(LogNetwork/LogSteam); machine-readable results use stdout/files, not
logs mixed into JSON. CLI cancellation closes resources and exits cleanly.

Packet capture format: ASCII SWNC magic, u16 version=1, u16 reserved=0, then records:
u64 local_relative_microseconds, u8 direction, u8 delivery, u16 flags, u64 opaque_peer,
u32 wire_length, wire bytes. Record payload capped at 16384 bytes. Redacted ticket
records have flag bit0=1 and wire_length=0; dumper reports redacted and encoder cannot
reconstruct their secrets. Flags beyond bit0 invalid. Cap capture file at 64 MiB,
then stop with a visible diagnostic; optional rotated files are future work.
Normal decoded records must round-trip byte-for-byte through dump->encode, including
unknown data only when an explicit bounded raw representation is present. No packet
capture replay is allowed to send traffic automatically. Inspector displays binary
schema/fields; no opaque "trust us" cache formats.

### 13.1 --check rules (all deterministic and offline)

Implement shared `validateNetworkProject` and `validateNetworkWorld` functions used
by Player and editor. Never make the checker depend on Steam being installed/logged in.

- Settings parser/ranges and tick-rate equality; unsupported transport/build capability
  is a warning for content checking, fatal for --host or Steam packaging requirements.
- Nonzero AppID for enabled Steam configuration; AppID 480 produces a development-only
  warning and is rejected by a release Steam package unless explicit dev-package mode.
- Manifest IDs unique, files present and under root, profiles registered, cycles handled.
- All network scene root keys unique/nonempty; prefab templates may have empty root key.
- Profile required components present; NetworkObject root unparented/unit uniform scale.
- No network children in v1 prefab parts; no unsupported per-copy removal assumptions.
- PredictedCharacter requires CharacterController, static-only movement policy and
  compatible collision configuration; no RigidBody/Collider on the same character.
- Replica-visible moving physics objects must have a network profile; static-only
  decorative objects need none. A dynamic object cannot quietly act as a moving platform.
- Instance count fits settings; estimated player snapshot budget fits configured bytes/s.
- Spawn points valid, enough team slots for Arena, finite and not initially inside walls.
- Arena local presentation camera count exactly one; remote prefab has no primary camera.
- Profiles, commands and events: duplicate IDs/names, oversized payloads, missing field
  descriptors, unsupported field types and unstable component IDs rejected.
- Circuit scopes and node capability violations; missing command registration.
- Content digest dependencies readable; path traversal/symlink escape forbidden.

Use file:line where authored source locations are available. Do not create noisy
warnings for every offline scene that has no NetworkObject. Enabled networking only
validates network maps declared by the manifest, while ordinary scene checks still run.

## 14. Automated verification: exact cases and gates

Add test targets with shockwave_warnings and CTest TIMEOUT. Keep default tests free
of external accounts, Steam runtime, live Internet, display and GPU. Use real World,
PhysicsWorld and PlaySession for integration, not mock gameplay that merely increments
a test counter. SDL_AUDIO_DRIVER=dummy and SDL_VIDEO_DRIVER=dummy where needed.
Existing offline CTest suite must remain green; don't rewrite old expectations to
hide regressions. All test code uses explicit checks active under NDEBUG, not assert-only.

### 14.1 Files / target responsibilities

| File | CTest name | Required assertions |
|---|---|---|
| Tests/NetworkWireTest.cpp | network_wire | Golden bytes for every message; exact header offsets and 24-byte InputCommand; all truncation lengths; bad counts/lengths/flags/versions/directions; NaN/inf/quaternions; endian vectors; wraparound |
| Tests/NetworkTransportTest.cpp | network_transport | Loopback reliable ordering; unreliable loss/duplicate/reorder; send buffer lifetime; bounded queues; closed-generation events discarded; accept/reject paths |
| Tests/SteamAdapterTest.cpp | steam_adapter | Fake private facade: init fail, listener fail, poll-group fail, SDK send errors, release on every error, stale callback, auth cancel/revoke, late lobby result; exactly-once shutdown |
| Tests/NetworkSettingsTest.cpp | network_settings | Round-trip .swnetwork; transactional invalid read; paths; schema matches; IDs >2^53 survive JSON strings |
| Tests/NetworkReplicationTest.cpp | network_replication | Authored key mapping with different local entity indices; dynamic spawn/despawn; baseline staging; lost/duplicate/out-of-order snapshot pages; control barrier; tombstones; respawn incarnation |
| Tests/NetworkPredictionTest.cpp | network_prediction | Static wall/stairs/slope/jump; correction changes Jolt and ECS; replay does not step other bodies; no doubled jump, shot/audio or damage; missing history safe resync |
| Tests/NetworkSessionTest.cpp | network_session | Start/Stop100x, failure at every startup phase, world restored, no live endpoints/buffers, no SDK offline; menu pumps; online pause/reload/save policies |
| Tests/NetworkCaptureTest.cpp | network_capture | dump/encode exact round-trip; redaction; bad/truncated capture; file cap; no network side effects |
| Tests/NetworkCheck.cmake | network_cli_check | CLI fixture invalids and exit codes; schema outputs; no-Steam --check; bad mode combinations |
| Games/Arena/Tests/ArenaNetworkTest.cpp | arena_network | Host+11 clients, authority combat, teams/score, hit rewind, respawn, join/leave, UI state binding |
| Games/Arena/Tests/ArenaNetworkMenuTest.cpp | arena_network_menu | Actual Rml documents: host/join/cancel/leave/error states; graphics menu regression; narrow layout |

Implementation may split tests further but must not remove these behaviors.

### 14.2 LoopbackHub and deterministic faults

The test hub is an injected object, never a global singleton. Each endpoint has its
own identity, connection generation, queues and counters. Tests own a virtual clock
and seeded PRNG. A delivery scheduler works in integer microseconds; no sleeps needed.
Loopback serializes through the real codec even in the same process; don't pass World
pointers or C++ snapshot structs between peers.

Reliable fault model delays/retransmits messages while preserving ordering/exactly-once
delivery to application. Unreliable can drop/duplicate/reorder. A simple deterministic
reliable scheduler can model retransmission delay without implementing a new network
protocol, but must not deliver reliable traffic out of order under jitter. Connection
close drops pending delivery and emits terminal events once.

Presets (each direction, independently seeded; latency below is one-way):

| Name | Base latency | Uniform jitter | Unreliable loss | Duplication | Duration |
|---|---|---|---|---|---|
| clean | 0 ms | 0 | 0 | 0 | 3600 ticks |
| cafe | 30 ms | +/-10 ms | 1% | 0.2% | 7200 ticks |
| poor | 75 ms | +/-25 ms | 5% | 1% | 7200 ticks |
| hostile | 200 ms | +/-50 ms | 10% | 2% | 3600 ticks |

Clamp delay to >=0; jitter naturally causes reordering on unreliable traffic. Add
explicit burst outage (500 ms) and severe outage (12 seconds), bandwidth throttling,
connection reset during baseline, auth revocation, and adversarial malformed messages.
Use reliable loss only to increase simulated delivery/retry delay; Steam owns actual
retransmission in real builds.

Run host+11 client Worlds with shared read-only fixture content and separate runtime
maps. Global prefab registry must never store per-session network entities. Headless
test clients drive the same InputCommand/game code path using recorded inputs.

Acceptance invariants across every preset:

1. Host never exceeds 12 players; teams never exceed six.
2. A client cannot move/damage another player by claiming its ID/slot.
3. Spam inputs cannot accelerate simulation or weapon rate.
4. All surviving peers converge on health/score/ownership after traffic recovers.
5. Replayed/duplicate packets cannot resurrect despawned entities or old incarnations.
6. Late join obtains current durable state; no old audiovisual events replay.
7. Queues/history/memory remain under explicit caps, including malicious peers.
8. Recovery/disconnect is explicit; no infinite connection screens.
9. Offline mode behavior remains covered by existing Arena/physics/save tests.
10. Two independent PlaySessions never route an event into each other's World.

Prediction quantitative test: deterministic fixture with static floor, wall, stair and
slope; 10 seconds of walking/jumping then stand still. After final authoritative
correction and 250 ms recovery, position error <=0.05 m and velocity error <=0.05 m/s.
Record p50/p95/max moving correction magnitude rather than asserting identical Jolt
bits. Hostile profile may visibly degrade, but must remain bounded and authoritative.
Rewind tests include target behind static wall, out-of-window timestamps, player
respawn, a delayed real hit, and a forged old view time that would manufacture a hit.

### 14.3 Steam-required manual gate

Real Steam integration is NOT verified by loopback or fake SDK tests. Use two distinct
Steam accounts with access to the same test AppID, normally two machines; same-account
two-window tests are not a substitute. Do not ask an AI to create accounts or disclose
credentials. Document missing SDK/accounts/network as a blocked verification gate.

Cases: friends host/join/invite/cold launch; correct owner/peer identity; invalid AppID;
Steam not running; mismatched content/protocol; full lobby; rejected stranger;
host exits; client exits/rejoins; overlay open; unfocused window; route unavailable;
auth denied/revoked; Windows<->Linux; public test only in an appropriate private test
application/environment. Test Steam relay route when available and record route status;
do not claim relay testing just because P2P API was used.

UI visual gate: editor default and alternate theme, narrow Network panel, Play/Stop,
failed join, twelve-peer table, inspector selected. Arena menu at 1280x720 and a smaller
supported window; keyboard/controller navigation; no clipped labels. Save screenshots
to ignored Saved output and record exact commands/results in verification notes.

### 14.4 Performance gates

Measure Release builds. Compare offline Arena before/after with existing
`player --benchmark 5 --json ...`, same map/settings/machine; repeat enough runs to
identify variance. Network overhead goals are engineering targets, not fabricated
pass results: host network encode/decode/dispatch p95 <1 ms/tick on the measured desktop;
client p95 <0.5 ms/tick excluding prediction physics; incremental Network/Multiplayer
working buffers <=32 MiB at 12 peers/512 objects (asset and Jolt worlds reported separately).
Default 12-player traffic should fit 64 KiB/s outbound per remote peer; host aggregate
budget is eleven times this, plus transport overhead. Report actual upstream and CPU.

Record allocations after warm-up, queue highs, worst pump time, Steam callback duration,
replay cost, bytes by message kind, RTT distribution, loss estimate availability and
frame-time p95/p99. Never invent packet-loss percentage from application counters if
the transport doesn't expose enough information. SDK queue delay/RTT and application
snapshot age are separate columns. Benchmark Jetson in offline loopback mode even if
Steam SDK is unsupported there; do not equate that with Steam-on-Jetson validation.

## 15. Ordered implementation batches

Each batch is a reviewable commit or small group of commits. Before proceeding, run its
gate and record failures honestly. Do not implement a giant untested "multiplayer.cpp".
The user's request here was a plan; a later implementer still follows the active user's
instructions about branches/commits/permissions. This document does not require asking
for approval between every batch unless the active user says so.

### Batch 0: baseline and compatibility scaffolding

Read required docs/current code; record HEAD/status/build config and baseline CTest plus
Arena benchmark. Add capability design notes and no-op Network module skeleton. Choose
the real SDK version from the supplied local SDK; if none, proceed with SDK-independent
work and mark Steam compile/manual gates outstanding. Do not alter renderer files.
Gate: all existing tests; offline Engine/Player/Editor builds, both modular/static where
supported. No new SDK requirement in default configure.

### Batch 1: codecs and fake transport

Implement section 6 wire primitives/catalog, capture, LoopbackHub/FaultLink and limits.
Generate golden binary fixtures with independently specified expected bytes, not the
same writer under test. Add every-length truncation tests and wraparound tests.
Gate: network_wire, network_transport, network_capture; sanitizers on decoder fuzz input.
Fuzz bounded random input and seed valid mutated packets; assert no unbounded allocation.

### Batch 2: config, component metadata and registry

Implement .swnetwork/manifest parsers, schema, NetworkObject/SpawnPoint registration,
stable keys, profile/command registries and compatibility digest. Add GameModule callback
and ABI guard, metadata editor and --check/--network-info/--network-schema.
Gate: parser/metadata/CLI tests; schema regeneration produces no subsequent diff;
missing registration and invalid metadata fail before a socket opens.

### Batch 3: Steam adapter and service lifecycle

Implement optional CMake target/runtime distribution, real/stub factories, callbacks,
lobbies, connection states, authentication and transport. No gameplay authority yet.
Gate: SteamAdapterTest fake failures/leaks, SDK OFF build, SDK ON compile and two-account
message round-trip when resources exist. An unavailable manual gate must remain listed.

### Batch 4: application/session integration

Add onPump/collectInput, injected service/session settings, NetworkWorld role/state,
shutdown order, online pause/reload/save guards and monotonic simulation time policy.
Gate: startup-failure injection, Stop100x, offline save/hot-reload regressions, continued
pumps during menus/editing, no events after session destruction.

### Batch 5: authoritative replication without prediction

Implement baseline, key binding, allowlisted dynamic spawn, full snapshots, control
barriers, owner assignment, late join and budget scheduler. Add simple two-world fixture
with moving host-owned box, no Arena assumptions. Client may initially display delayed
movement; this batch is not claimed playable FPS netcode.
Gate: replication suite including parts arriving reversed, despawn-before-old-snapshot,
different entity creation orders, partial baseline teardown and malformed authority data.

### Batch 6: shared input-driven Arena simulation

Refactor local hardware polling out of gameplay, preserve offline behavior. Add network
Arena scene/prefabs, game profiles and host-controlled 6v6 actors. Use validated input
commands and authority combat without lag rewind initially. Add lobby/game UI skeleton.
Gate: offline Arena tests unchanged; host+11 clients cannot spoof damage/team/ownership;
ammo/cadence independent of render frame rate; one local camera per process.

### Batch 7: physics prediction and presentation

Implement Physics API/helper extraction and static collision policy, command histories,
tick mapping, reconciliation and Engine presentation override. Keep renderer untouched.
Gate: prediction tests, frame-rate-independent inputs, replay side-effect suppression,
headless correction convergence; visually inspect camera/viewmodel under network jitter.

### Batch 8: combat history, events and circuits

Implement historical capsule queries, world obstruction rule, deduped presentation,
durable scoreboard, respawn incarnations and scoped circuit commands/events.
Gate: rewind/security cases, no duplicate shots/effects, scope validation, schema/node
schema regenerated and checked. Dynamic cover limitation explicitly documented.

### Batch 9: complete editor/CLI/package experience

Finish section 12 UI, section 13 CLI, diagnostics, packet inspection, invitation paths,
Steam-enabled package copy/verification and offline fallback. GameModule reload pending
status and Stop teardown visible. No debug AppID or auth capture in packages.
Gate: real UI tests/screenshots, CLI invalids, moved-package smoke, SDK OFF package.

### Batch 10: impairment, real Steam and measured performance

Run all automated presets, 12-player soak (at least 30 simulated minutes), every manual
case possible, Release frame/network budgets. Fix correctness before compressing the
protocol. Record remaining platform/account gates exactly; don't replace them with
mock passes. Only then call the first multiplayer release complete.

## 16. File modification checklist

New modules/files are listed in section 2. Existing touch points must stay focused:

- Root CMakeLists and Runtime/CMakeLists: options/include/module order only.
- Engine.h/.cpp: generic early pump and optional online timing policy; no Steam symbols.
- Components.h/Systems.cpp: runtime presentation transform; no network transport code.
- Physics.h/.cpp: tested state APIs, per-character helper and simulation policies.
- GameMode.h, PlaySession.h/.cpp: service accessor/injection/boundaries/lifecycle.
- GameModule.h, GameModuleEntry.cpp, GameModuleLibrary.cpp: registry hook and ABI checks.
- Circuits component registration, CircuitNodes/CircuitWorld/compiler: explicit scopes
  and capability checks; no blanket permission to remotely call local nodes.
- Player Main.cpp/Player.h/Player.cpp + new PlayerNetwork.cpp: CLI and application glue.
- Editor Editor.h/application callbacks/PlayInEditor/EditorCode/Workspace + NetworkPanel:
  shared APIs, UI state, lifecycle guards; no duplicate protocol implementation.
- Packaging module: explicit Steam feature/runtime manifest; no SDK distribution.
- Arena split sources, network scene/prefabs/UI and tests: consumer of shared services.
- Tests/CMakeLists and relevant game CMake: register tests, set timeouts/dummy drivers.

Docs deliverables for implementation: `docs/Networking.md` user/API guide,
`docs/NetworkSchema.md` generated reference, `docs/NetworkProtocol.md` versioned field
reference from descriptors, updated Architecture/Projects/Packaging/SceneSchema/
CircuitNodes, Arena README instructions, `docs/NetworkingVerification.md` actual evidence.
This design document remains a design; don't falsely mark manual gates as implemented
by changing its wording. Update it explicitly when a tested design adjustment is made.

Final verification commands (proposed switches become runnable only after implementation):

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DSHOCKWAVE_STEAMWORKS=OFF
cmake --build build -j
ctest --test-dir build --output-on-failure
build/bin/player --project Games/Arena/Arena.swproject --check
build/bin/player --project Games/Arena/Arena.swproject --schema Games/Arena/Docs/SceneSchema.md
build/bin/player --project Games/Arena/Arena.swproject --network-schema docs/NetworkSchema.md
build/bin/player --project Games/Arena/Arena.swproject --node-schema Games/Arena/Docs/CircuitNodes.md
build/bin/player --net-test --scenario clean --seed 123 --json /tmp/net-clean.json
build/bin/player --net-test --scenario cafe --seed 123 --json /tmp/net-cafe.json
build/bin/player --net-test --scenario poor --seed 123 --json /tmp/net-poor.json
build/bin/player --net-test --scenario hostile --seed 123 --json /tmp/net-hostile.json
```

Use separate configured directories for Steam ON/OFF and modular/static matrices;
do not repeatedly mutate a user's primary build configuration without explaining it.
clang-format all modified C++ using repository style. Check git diff --check, review
changed files for unrelated edits, regenerate schemas with the correct game's registry.
Use unique temporary output directories in tests and close files before removing them
(Windows regression already fixed in this repository). Commit no captures, credentials,
SDK files, benchmark dumps or generated giant stress scenes.

## 17. Clarifications that prevent common implementation failures

- This is not a request to replace the scene format or implement level streaming first.
  Stable keys are added only where network identity is required.
- The transport is replaceable because NetworkWorld sees ITransport/IOnlineServices,
  not because every Steam API has been wrapped into a sprawling generic framework.
- Loopback is in-process test transport, not a promise of non-Steam Internet/LAN play.
- Twelve-player Steam play is not "verified" by twelve peers in one process.
- Host authority stops clients deciding damage, but a malicious listen host can cheat.
  Competitive trust ultimately needs dedicated authority; Steam authentication alone
  is not gameplay anti-cheat and does not make a client host trustworthy.
- No full-world deterministic physics claim. Static-world movement prediction is a
  deliberate bounded first capability; moving-platform support is unfinished until
  implemented and tested, not enabled by a checkbox.
- Low engine CPU overhead is measured separately from network latency. No benchmark
  should promise sub-millisecond Internet synchronization or hide interpolation delay.
- Do not add broad RPC reflection for convenience. A small registered typed command
  surface gives scripts methods to trigger without permitting arbitrary remote code.
- Features unavailable in a build must fail visibly rather than emulate success.
- A Steam manual-test blocker does not block finishing the SDK-independent modules,
  but it DOES block claiming the Steam integration is fully verified.

## 18. Primary reference sources

Checked 2026-10-18. These support API contracts; the protocol, module boundaries,
budgets and gameplay algorithms above are original proposed Shockwave design choices.
The supplied SDK headers are the compile-time source of truth. Record any discrepancy
and adapt the private adapter with tests; do not change public gameplay APIs casually.

- [S1: Steam Networking Sockets](https://partner.steamgames.com/doc/api/ISteamNetworkingSockets?l=english)
- [S2: Steamworks API lifecycle and callbacks](https://partner.steamgames.com/doc/sdk/api?l=english)
- [S3: Steam matchmaking and lobbies](https://partner.steamgames.com/doc/features/multiplayer/matchmaking?l=english)
- [S4: Valve GameNetworkingSockets repository and scope](https://github.com/ValveSoftware/GameNetworkingSockets)
- [S5: Valve sockets interface header, ownership and receive contracts](https://github.com/ValveSoftware/GameNetworkingSockets/blob/master/include/steam/isteamnetworkingsockets.h)
- [S6: Valve networking types and send flags](https://github.com/ValveSoftware/GameNetworkingSockets/blob/master/include/steam/steamnetworkingtypes.h)
- [S7: Steam authentication and ownership](https://partner.steamgames.com/doc/features/auth?l=english)
- [S8: Steam Networking Utils / relay initialization](https://partner.steamgames.com/doc/api/ISteamNetworkingUtils?l=english)
- [S9: Steam Matchmaking interface](https://partner.steamgames.com/doc/api/ISteamMatchmaking?l=english)
- [S10: Steam Friends / invitations](https://partner.steamgames.com/doc/api/ISteamFriends?l=english)
- [S11: Jolt v5.6.0 CharacterVirtual public API](https://github.com/jrouwe/JoltPhysics/blob/v5.6.0/Jolt/Physics/Character/CharacterVirtual.h)

## 19. Completion report template for the implementing agent

```text
Implemented batches:
Actual build configurations and SDK version:
Offline tests passed / total:
12-peer fault/soak tests and seed:
Steam real-account tests actually run:
Windows/Linux cross-play actually run:
UI screenshots inspected:
Release CPU/bandwidth/memory measurements:
Known unsupported features:
Outstanding verification gates (not mock substitutes):
Schema regeneration/diff results:
User commands for host/join/offline:
Files/commits changed:
```

Never report a planned test as a passed test. The final change must be understandable
and maintainable by the next contributor without this conversation.
