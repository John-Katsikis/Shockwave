#include <GameFramework/NetworkPump.h>

#include <Engine/Engine.h>

namespace shockwave {

void pumpNetworkFrame(
    net::IOnlineServices* services, PlaySession* session, const NetworkFrame& frame, NetworkPumpCounters* counters) {
    if (counters) ++counters->pumps;
    if (services) {
        services->pump();
        net::OnlineEvent event;
        while (services->pollEvent(event)) {
            const bool claimed = (frame.match && frame.match->handleOnlineEvent(event)) ||
                                 (session && session->handleOnlineEvent(event));
            if (counters) {
                ++counters->serviceEvents;
                counters->unclaimedEvents += !claimed;
            }
        }
    }
    if (session) {
        session->pumpNetwork(frame.nowSeconds);
        if (frame.simulationOverrun && session->online())
            session->network()->fail(net::ReasonCode::SimulationOverrun, "the simulation fell more than 250 ms behind");
    }
    if (frame.match) frame.match->update(session && session->active() ? session->network() : nullptr);
    if (session) session->collectInput(frame.rawFrameDt);
}

void pumpNetworkFrame(
    Engine& engine, net::IOnlineServices* services, PlaySession* session, double rawFrameDt,
    NetworkPumpCounters* counters, net::OnlineMatch* match) {
    const bool online = session && session->online();
    NetworkFrame frame;
    frame.nowSeconds = engine.frameStartSeconds();
    frame.rawFrameDt = rawFrameDt;
    frame.simulationOverrun = online && engine.simulationStep().overrun;
    frame.match = match;
    pumpNetworkFrame(services, session, frame, counters);
    // From the next frame on; a session that just started or stopped switches here.
    engine.setSimulationTime(session && session->online() ? SimulationTime::Monotonic : SimulationTime::Smoothed);
}

}   // namespace shockwave
