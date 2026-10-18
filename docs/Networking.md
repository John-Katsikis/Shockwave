# Networking

Shockwave's multiplayer is being built in batches from the design in
[SteamMultiplayerImplementation.md](SteamMultiplayerImplementation.md): one
authoritative listen host and up to eleven clients, connected through Steam. This page
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

Not built yet: Steam itself, `.swnetwork` settings, replicated entities, prediction,
the editor's Network window and the `player` network commands. Nothing advertises
those as working.

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
