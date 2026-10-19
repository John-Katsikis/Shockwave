#pragma once

// The per-frame network pump an application (the player, the editor) runs from
// Application::onPump, in the order docs/Networking.md gives, every frame: in menus,
// while editing and while paused too, so online services and sessions never stall.
//
//   1. services.pump()                 once per frame, even outside Play
//   2. drain the services' events      to the session, or dropped when it isn't theirs
//   3. session.pumpNetwork(now)        receive and handle messages, timeouts
//   4. a simulation overrun ends network play (SimulationOverrun)
//   5. session.collectInput(frameDt)   GameMode::collectInput, before the fixed ticks
//
// Both pointers may be null. The engine version also picks the simulation time source:
// Monotonic while the session is online, Smoothed otherwise.

#include <GameFramework/PlaySession.h>
#include <Network/OnlineServices.h>

#include <cstdint>

namespace shockwave {

class Engine;
struct SimulationStep;

struct NetworkFrame {
    double nowSeconds = 0;   // monotonic
    double rawFrameDt = 0;
    bool simulationOverrun = false;   // SimulationStep::overrun
};

struct NetworkPumpCounters {
    uint64_t pumps = 0;   // frames pumped
    uint64_t serviceEvents = 0;   // events drained from the services
    uint64_t unclaimedEvents = 0;   // of those, ones no session took
};

void pumpNetworkFrame(
    net::IOnlineServices* services, PlaySession* session, const NetworkFrame& frame,
    NetworkPumpCounters* counters = nullptr);
void pumpNetworkFrame(
    Engine& engine, net::IOnlineServices* services, PlaySession* session, double rawFrameDt,
    NetworkPumpCounters* counters = nullptr);

}   // namespace shockwave
