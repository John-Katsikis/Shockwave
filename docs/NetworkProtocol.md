# Network protocol reference

Generated from `Source/Runtime/Network/Public/Network/Messages.h`; don't edit it by hand. Regenerate it with `build/bin/network_wire_test --write-reference docs/NetworkProtocol.md`.

Protocol version 1. Every integer is little endian, every float is IEEE 754 single precision written as its bits, and every length counts bytes. A `string(N)` or `bytes(N)` is a u16 byte count followed by that many bytes. Offsets are from the start of the payload, and stop once a field has a variable length. No message may have bytes after its last field.

Reliable messages are at most 16384 bytes and unreliable ones 1152, header included. Each kind's delivery and direction are fixed: a message that arrives another way is rejected. An unknown version or kind closes the connection with ProtocolMismatch; any other bad message with MalformedPacket.

## Header (32 bytes)

| Offset | Field | Type | Allowed |
|---|---|---|---|
| 0 | `magic` | 4 bytes | ASCII `SWMP` |
| 4 | `version` | u16 | 1 |
| 6 | `kind` | u8 | 1..23, below |
| 7 | `flags` | u8 | 0 |
| 8 | `session` | 16 bytes | the host's session nonce; zero only in a developer direct-join Hello |
| 24 | `sender_tick` | u32 | 0 before play |
| 28 | `payload_bytes` | u32 | message size minus 32 |

## Messages

| Kind | Name | Delivery | Direction | When |
|---|---|---|---|---|
| 1 | Hello | reliable | client to host | first message on a new connection |
| 2 | Challenge | reliable | host to client | answers a compatible Hello |
| 3 | AuthTicket | reliable | either way | authenticating |
| 4 | Welcome | reliable | host to client | after mutual authentication |
| 5 | BaselineBegin | reliable | host to client | starts a baseline |
| 6 | Spawn | reliable | host to client | baseline and active |
| 7 | Despawn | reliable | host to client | baseline and active |
| 8 | Ownership | reliable | host to client | baseline and active |
| 9 | InitialState | reliable | host to client | baseline or resync |
| 10 | BaselineEnd | reliable | host to client | ends a baseline |
| 11 | Ready | reliable | client to host | acknowledges a baseline |
| 12 | Active | reliable | host to client | starts play |
| 13 | Inputs | unreliable | client to host | active |
| 14 | Snapshot | unreliable | host to client | active |
| 15 | Command | reliable | client to host | active |
| 16 | GameEvent | reliable | host to client | active |
| 17 | Ping | unreliable | either way | after Challenge |
| 18 | Pong | unreliable | either way | answers a Ping |
| 19 | Reject | reliable | host to client | any time; the host closes after it |
| 20 | Goodbye | reliable | either way | any time; the sender closes after it |
| 21 | ResyncRequest | reliable | client to host | active |
| 22 | DeltaSnapshot | unreliable | host to client | active |
| 23 | SnapshotAck | unreliable | client to host | active |

### 1 Hello

| Offset | Field | Type | Allowed |
|---|---|---|---|
| 0 | `game_protocol` | string(64) | 1..64 bytes UTF-8, no NUL |
|  | `schema_digest` | digest (32 bytes, SHA-256) |  |
|  | `content_digest` | digest (32 bytes, SHA-256) |  |
|  | `scene_id` | u16 | >= 1 |
|  | `lobby_id` | u64 |  |
|  | `client_nonce` | 16 bytes |  |

### 2 Challenge

`snapshot_rate` must be 10, 15, 20, 30, 60 or 120, and divide `tick_rate`.

| Offset | Field | Type | Allowed |
|---|---|---|---|
| 0 | `client_nonce` | 16 bytes |  |
| 16 | `game_protocol` | string(64) | 1..64 bytes UTF-8, no NUL |
|  | `schema_digest` | digest (32 bytes, SHA-256) |  |
|  | `content_digest` | digest (32 bytes, SHA-256) |  |
|  | `scene_id` | u16 | >= 1 |
|  | `host_peer` | u64 | >= 1 |
|  | `tick_rate` | u16 | 60..120 |
|  | `snapshot_rate` | u16 | 10..120 |
|  | `interpolation_ms` | u16 | 25..200 |
|  | `max_rewind_ms` | u16 | 0..250 |

### 3 AuthTicket

The sender's identity comes from the transport, never from this message. Captures never store these bytes.

| Offset | Field | Type | Allowed |
|---|---|---|---|
| 0 | `ticket` | bytes(4096) | 1..4096 bytes |

### 4 Welcome

| Offset | Field | Type | Allowed |
|---|---|---|---|
| 0 | `player_slot` | u16 | 2..64 |
| 2 | `baseline_id` | u32 | >= 1 |
| 6 | `server_tick` | u32 |  |
| 10 | `max_entities` | u16 | 12..4096 |

### 5 BaselineBegin

| Offset | Field | Type | Allowed |
|---|---|---|---|
| 0 | `baseline_id` | u32 | >= 1 |
| 4 | `baseline_tick` | u32 |  |
| 8 | `object_count` | u16 | 0..4096 |
| 10 | `total_state_bytes` | u32 | 0..4194304 |

### 6 Spawn

An authored object (`prefab_id` 0) has an `authored_key`; a spawned prefab has an empty one.

| Offset | Field | Type | Allowed |
|---|---|---|---|
| 0 | `control_serial` | u32 | >= 1 |
| 4 | `entity` | u64 | >= 1 |
| 12 | `incarnation` | u32 |  |
| 16 | `owner_slot` | u16 | 0..64 |
| 18 | `prefab_id` | u16 |  |
| 20 | `authored_key` | string(96) | 0..96 bytes UTF-8, no NUL |
|  | `profile_id` | u16 | 1..1023 |
|  | `initial_state` | bytes(512) | 0..512 bytes |

### 7 Despawn

| Offset | Field | Type | Allowed |
|---|---|---|---|
| 0 | `control_serial` | u32 | >= 1 |
| 4 | `entity` | u64 | >= 1 |
| 12 | `reason` | u8 |  |

### 8 Ownership

| Offset | Field | Type | Allowed |
|---|---|---|---|
| 0 | `control_serial` | u32 | >= 1 |
| 4 | `entity` | u64 | >= 1 |
| 12 | `incarnation` | u32 |  |
| 16 | `owner_slot` | u16 | 0..64 |

### 9 InitialState

| Offset | Field | Type | Allowed |
|---|---|---|---|
| 0 | `baseline_id` | u32 | >= 1 |
| 4 | `records` | u16 count, then state records | each record self-contained |

### 10 BaselineEnd

| Offset | Field | Type | Allowed |
|---|---|---|---|
| 0 | `baseline_id` | u32 | >= 1 |
| 4 | `final_control_serial` | u32 |  |
| 8 | `baseline_digest` | digest (32 bytes, SHA-256) |  |

### 11 Ready

| Offset | Field | Type | Allowed |
|---|---|---|---|
| 0 | `baseline_id` | u32 | >= 1 |
| 4 | `baseline_digest` | digest (32 bytes, SHA-256) |  |

### 12 Active

| Offset | Field | Type | Allowed |
|---|---|---|---|
| 0 | `start_tick` | u32 |  |
| 4 | `first_input_tick` | u32 |  |
| 8 | `controlled_entity` | u64 |  |
| 16 | `incarnation` | u32 |  |

### 13 Inputs

| Offset | Field | Type | Allowed |
|---|---|---|---|
| 0 | `commands` | u8 count, then InputCommand x count | 1..4 commands |

### 14 Snapshot

`part_index` must be less than `part_count`.

| Offset | Field | Type | Allowed |
|---|---|---|---|
| 0 | `snapshot_id` | u32 |  |
| 4 | `server_tick` | u32 |  |
| 8 | `required_control_serial` | u32 |  |
| 12 | `part_index` | u16 | 0..255 |
| 14 | `part_count` | u16 | 1..256 |
| 16 | `records` | u16 count, then state records | each record self-contained |

### 15 Command

| Offset | Field | Type | Allowed |
|---|---|---|---|
| 0 | `command_seq` | u32 |  |
| 4 | `target` | u64 |  |
| 12 | `command_id` | u16 | >= 1 |
| 14 | `payload` | bytes(256) | 0..256 bytes |

### 16 GameEvent

| Offset | Field | Type | Allowed |
|---|---|---|---|
| 0 | `event_seq` | u32 |  |
| 4 | `event_tick` | u32 |  |
| 8 | `subject` | u64 |  |
| 16 | `event_id` | u16 | >= 1 |
| 18 | `payload` | bytes(512) | 0..512 bytes |

### 17 Ping

| Offset | Field | Type | Allowed |
|---|---|---|---|
| 0 | `ping_id` | u32 |  |
| 4 | `origin_monotonic_us` | u64 |  |

### 18 Pong

`responder_send_us` can't be before `responder_receive_us`.

| Offset | Field | Type | Allowed |
|---|---|---|---|
| 0 | `ping_id` | u32 |  |
| 4 | `echoed_origin_us` | u64 |  |
| 12 | `responder_receive_us` | u64 |  |
| 20 | `responder_send_us` | u64 |  |
| 28 | `responder_tick` | u32 |  |

### 19 Reject

| Offset | Field | Type | Allowed |
|---|---|---|---|
| 0 | `reason_code` | u16 reason | 0..14 |
| 2 | `diagnostic` | string(160) | 0..160 bytes UTF-8, no NUL |

### 20 Goodbye

| Offset | Field | Type | Allowed |
|---|---|---|---|
| 0 | `reason_code` | u16 reason | 0..14 |

### 21 ResyncRequest

| Offset | Field | Type | Allowed |
|---|---|---|---|
| 0 | `last_applied_server_tick` | u32 |  |
| 4 | `reason` | u8 |  |

### 22 DeltaSnapshot

`part_index` must be less than `part_count`, `first_slot + slot_count` at most 1024, `changed_count` at most `slot_count`. `baseline_tick` is 0 unless `flags` has bit 0 (has baseline), and then comes before `snapshot_tick`. The payload's bit layout is in docs/Networking.md (Delta snapshots).

| Offset | Field | Type | Allowed |
|---|---|---|---|
| 0 | `snapshot_tick` | u32 |  |
| 4 | `baseline_tick` | u32 |  |
| 8 | `required_control_serial` | u32 |  |
| 12 | `flags` | u8 bits | only 0x01 |
| 13 | `part_index` | u8 | 0..254 |
| 14 | `part_count` | u8 | >= 1 |
| 15 | `first_slot` | u16 | 0..1023 |
| 17 | `slot_count` | u16 | 0..1024 |
| 19 | `changed_count` | u16 | 0..1024 |
| 21 | `payload` | bytes(1152) | 0..1152 bytes |

### 23 SnapshotAck

Bit i of `received_bits` is `latest_tick - 1 - i`.

| Offset | Field | Type | Allowed |
|---|---|---|---|
| 0 | `latest_tick` | u32 |  |
| 4 | `received_bits` | u32 |  |

## Input command (24 bytes)

`yaw` 0..65535 maps to -pi..pi (exclusive), `pitch` -32767..32767 to -89..89 degrees. `move_x` and `move_z` together may be no longer than 128.

| Offset | Field | Type | Allowed |
|---|---|---|---|
| 0 | `target_tick` | u32 |  |
| 4 | `incarnation` | u32 |  |
| 8 | `move_x` | i8 | -127..127 |
| 9 | `move_z` | i8 | -127..127 |
| 10 | `yaw` | u16 |  |
| 12 | `pitch` | i16 | -32767..32767 |
| 14 | `held_bits` | u16 bits | only 0x0003 |
| 16 | `pressed_bits` | u16 bits | only 0x0003 |
| 18 | `view_tick` | u32 |  |
| 22 | `view_fraction` | u16 |  |

## State record (16-byte header)

| Offset | Field | Type | Allowed |
|---|---|---|---|
| 0 | `entity` | u64 | >= 1 |
| 8 | `incarnation` | u32 |  |
| 12 | `profile_id` | u16 | 1..1023 |
| 14 | `state_bytes` | u16 | 0..512 |
| 16 | `state` | state_bytes bytes |  |

Profiles 1..31 belong to the engine and 32..1023 to games.

### Profiles 1 engine.pose.v1 and 2 engine.rigidbody.v1 (53 bytes)

| Offset | Field | Type | Allowed |
|---|---|---|---|
| 0 | `position` | vec3 (3 x f32) | finite, abs <= 32768 |
| 12 | `rotation` | quat (4 x f32, xyzw) | length 0.99..1.01 |
| 28 | `linear_velocity` | vec3 (3 x f32) | finite, abs <= 512 |
| 40 | `angular_velocity` | vec3 (3 x f32) | finite, abs <= 1024 |
| 52 | `flags` | u8 bits | only 0x07 |

## Reason codes

| Code | Name |
|---|---|
| 0 | Normal |
| 1 | ProtocolMismatch |
| 2 | ContentMismatch |
| 3 | ServerFull |
| 4 | NotLobbyMember |
| 5 | AuthenticationFailed |
| 6 | AuthUnavailable |
| 7 | Timeout |
| 8 | MalformedPacket |
| 9 | RateLimited |
| 10 | HostLeft |
| 11 | MapNotLoaded |
| 12 | SimulationOverrun |
| 13 | ServiceUnavailable |
| 14 | UnsupportedFeature |
