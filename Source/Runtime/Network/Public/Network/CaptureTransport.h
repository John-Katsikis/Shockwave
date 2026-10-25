#pragma once

// A transport that records what it carries: every message sent and received through it
// goes into a packet capture (Network/NetworkCapture.h) as it happens, AuthTicket
// messages redacted. It changes nothing about the traffic, and a capture can never be
// replayed into a session: captures are for reading (player --net-dump).
//
// Peers are written as small opaque numbers (1, 2, 3... in the order they first appear),
// never as Steam IDs, so a capture can be shared without naming anyone.

#include <Network/NetworkCapture.h>
#include <Network/Transport.h>

#include <memory>

namespace shockwave::net {

struct CaptureCounters {
    uint64_t records = 0;
    uint64_t redacted = 0;
    uint64_t dropped = 0;   // after the file reached its size cap, or a write failed
};

// The writer must be open and outlive the transport. Recording starts at time 0 now.
std::unique_ptr<ITransport> makeCapturingTransport(std::unique_ptr<ITransport> inner, CaptureWriter& writer);
// What a transport made by makeCapturingTransport has recorded; null for any other.
const CaptureCounters* captureCounters(const ITransport& transport);

}   // namespace shockwave::net
