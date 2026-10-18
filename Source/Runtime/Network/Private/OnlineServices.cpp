#include <Network/OnlineServices.h>

namespace shockwave::net {

namespace {

class UnavailableOnlineServices final : public IOnlineServices {
public:
    explicit UnavailableOnlineServices(const char* reason) : m_error{NetErrorCode::Unavailable, reason} {}

    OnlineCapabilities capabilities() const override { return {}; }
    PeerId localPeer() const override { return {}; }
    void pump() override {}
    bool pollEvent(OnlineEvent&) override { return false; }
    void cancel(RequestId) override {}

    NetResult<RequestId> createLobby(const LobbyCreateRequest&) override { return m_error; }
    NetResult<RequestId> findLobbies(const LobbyQuery&) override { return m_error; }
    NetResult<RequestId> joinLobby(LobbyId) override { return m_error; }
    NetResult<void> leaveLobby(LobbyId) override { return m_error; }
    NetResult<void> inviteFriend(LobbyId) override { return m_error; }
    NetResult<void> setLobbyJoinable(LobbyId, bool) override { return m_error; }
    NetResult<void> setLobbyMetadata(LobbyId, const std::string&, const std::string&) override { return m_error; }
    NetResult<std::vector<PeerId>> lobbyMembers(LobbyId) const override { return m_error; }
    NetResult<std::unique_ptr<ITransport>> createTransport() override { return m_error; }

private:
    NetError m_error;
};

}   // namespace

std::unique_ptr<IOnlineServices> makeUnavailableOnlineServices(const char* reason) {
    return std::make_unique<UnavailableOnlineServices>(reason);
}

}   // namespace shockwave::net
