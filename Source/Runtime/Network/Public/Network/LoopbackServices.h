#pragma once

// LoopbackOnlineServices: an IOnlineServices for tests and `player --net-test`, on a
// LoopbackHub instead of Steam. Its transports are the hub's, and its session tickets are
// made-up bytes naming who issued them and for whom, so mutual authentication runs the
// same messages and events it does on Steam without an account.
//
// Like Steam, results arrive only through pollEvent() after pump(), never inside a call.
// Each service can be told to fail in the ways a real one does (a ticket that can't be
// made, a validation that's refused or never answers), and it records the calls that
// matter to shutdown order, so tests can check them.
//
// A lobby it creates is its own (other services can't join it): enough for a host to
// make one, close it to joins and leave it.

#include <Network/Loopback.h>
#include <Network/OnlineServices.h>

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

namespace shockwave::net {

class LoopbackOnlineServices final : public IOnlineServices {
public:
    // The hub must outlive the service and every transport it makes.
    LoopbackOnlineServices(LoopbackHub& hub, PeerId identity);
    ~LoopbackOnlineServices() override;

    enum class Validation : uint8_t {
        Check,   // tickets issued by the peer, for us, succeed; anything else fails
        Refuse,   // every ticket fails with AuthenticationFailed
        NoAnswer,   // beginAuthSession is accepted, then nothing ever comes back
    };
    void setValidation(Validation validation) { m_validation = validation; }
    // requestAuthTicket fails at once (true), or its request fails on the next pump.
    void setTicketFailure(bool immediate, bool later) {
        m_ticketFailsNow = immediate;
        m_ticketFailsLater = later;
    }
    // createTransport fails.
    void setTransportFailure(bool fail) { m_transportFails = fail; }

    // For tests: what is still open, and the order things happened in.
    uint64_t pumps() const { return m_pumps; }
    size_t openAuthSessions() const { return m_authSessions.size(); }
    size_t liveTickets() const;
    const std::vector<std::string>& calls() const { return m_calls; }
    void clearCalls() { m_calls.clear(); }

    OnlineCapabilities capabilities() const override { return {false, true}; }
    PeerId localPeer() const override { return m_identity; }
    void pump() override;
    bool pollEvent(OnlineEvent& out) override;
    void cancel(RequestId request) override;

    NetResult<RequestId> createLobby(const LobbyCreateRequest& request) override;
    NetResult<RequestId> findLobbies(const LobbyQuery& query) override;
    NetResult<RequestId> joinLobby(LobbyId lobby) override;
    NetResult<void> leaveLobby(LobbyId lobby) override;
    NetResult<void> inviteFriend(LobbyId lobby) override;
    NetResult<void> setLobbyJoinable(LobbyId lobby, bool joinable) override;
    NetResult<void> setLobbyMetadata(LobbyId lobby, const std::string& key, const std::string& value) override;
    NetResult<std::vector<PeerId>> lobbyMembers(LobbyId lobby) const override;
    NetResult<LobbySummary> lobbyInfo(LobbyId lobby) const override;
    NetResult<std::unique_ptr<ITransport>> createTransport() override;

    NetResult<RequestId> requestAuthTicket(PeerId remote) override;
    NetResult<std::span<const uint8_t>> authTicket(RequestId ticket) const override;
    NetResult<void> beginAuthSession(PeerId remote, std::span<const uint8_t> ticket) override;
    void endAuthSession(PeerId remote) override;

    // A ticket's bytes: "SWLT", issuer and audience as little-endian u64s.
    static constexpr size_t kTicketBytes = 20;

private:
    struct Ticket {
        RequestId id;
        PeerId remote;
        std::vector<uint8_t> bytes;
        bool ready = false;
    };
    RequestId nextRequest();

    LoopbackHub& m_hub;
    PeerId m_identity;
    uint32_t m_generation;
    uint64_t m_nextRequest = 1;
    uint64_t m_pumps = 0;
    Validation m_validation = Validation::Check;
    bool m_ticketFailsNow = false, m_ticketFailsLater = false, m_transportFails = false;
    std::vector<Ticket> m_tickets;
    std::vector<PeerId> m_authSessions;
    std::deque<OnlineEvent> m_pending;   // reported on the next pump
    std::deque<OnlineEvent> m_ready;
    LobbyId m_lobby;
    bool m_lobbyJoinable = false;
    uint32_t m_lobbyCapacity = 0;
    std::vector<std::pair<std::string, std::string>> m_lobbyMetadata;
    std::vector<std::string> m_calls;
};

}   // namespace shockwave::net
