#pragma once

// What Diagnostics.cpp shares with the crash handler: fixed memory the handler can read
// without locks or allocation.
#include <Core/Diagnostics.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

namespace shockwave::diagnostics {

std::string utcTimestamp();   // "2026-11-11T22:54:12Z"

namespace detail {
// The ring: g_ringWritten bytes have gone in, the last kRingBytes of them are kept.
extern char g_ring[kRingBytes];
extern std::atomic<uint64_t> g_ringWritten;
// The context as text, double buffered: writers fill the inactive one, then flip.
inline constexpr size_t kContextTextBytes = 16 * 1024;
extern char g_contextText[2][kContextTextBytes];
extern std::atomic<size_t> g_contextLength[2];
extern std::atomic<int> g_contextActive;
}   // namespace detail

}   // namespace shockwave::diagnostics
