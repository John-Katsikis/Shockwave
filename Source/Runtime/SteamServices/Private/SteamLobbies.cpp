// Steam lobbies (docs/SteamMultiplayerImplementation.md sections 4.2 and 4.3): create,
// find, join and leave, metadata, invites and membership. Each asynchronous call keeps a
// slot until Steam answers, so a cancelled call's late answer is recognised: a lobby it
// created or joined is left at once and never reported.
#include "SteamServicesImpl.h"

#include <Network/Messages.h>

#include <algorithm>
#include <cstring>

namespace shockwave::net {

namespace {

constexpr uint32_t kMaxLobbyResults = 50;
constexpr size_t kMaxLobbyKeyBytes = 255;   // k_nMaxLobbyKeyLength
constexpr size_t kMaxLobbyValueBytes = 8192;

SteamLobbyType lobbyType(LobbyVisibility visibility) {
    switch (visibility) {
    case LobbyVisibility::Private: return SteamLobbyType::Private;
    case LobbyVisibility::Friends: return SteamLobbyType::FriendsOnly;
    case LobbyVisibility::Public: return SteamLobbyType::Public;
    }
    return SteamLobbyType::Private;
}

}   // namespace

static NetResult<RequestId>
track(SteamServicesImpl& s, SteamServicesImpl::CallKind kind, uint64_t call, const char* failure, uint64_t lobby = 0) {
    if (!call) return NetError{NetErrorCode::Io, failure};
    auto slot = std::find_if(s.calls.begin(), s.calls.end(), [](const SteamServicesImpl::Call& c) {
        return c.kind == SteamServicesImpl::CallKind::None;
    });
    // Checked by the callers before starting the call; full here means a bug.
    if (slot == s.calls.end()) return NetError{NetErrorCode::CapacityExceeded, "too many Steam requests in flight"};
    *slot = {kind, call, s.newRequest(), false, lobby};
    return slot->request;
}

static bool callSlotFree(const SteamServicesImpl& s) {
    return std::any_of(s.calls.begin(), s.calls.end(), [](const SteamServicesImpl::Call& c) {
        return c.kind == SteamServicesImpl::CallKind::None;
    });
}

static SteamServicesImpl::Call* findCall(SteamServicesImpl& s, SteamServicesImpl::CallKind kind, uint64_t call) {
    for (SteamServicesImpl::Call& c : s.calls)
        if (c.kind == kind && c.call == call && call) return &c;
    return nullptr;
}

NetResult<RequestId> SteamServicesImpl::createLobby(const LobbyCreateRequest& request) {
    if (state != SteamServiceState::Client) return noLobbies();
    if (request.maxMembers < 2 || request.maxMembers > kMaxPlayers)
        return NetError{NetErrorCode::InvalidArgument, "a lobby holds 2 to 64 members"};
    if (!callSlotFree(*this)) return NetError{NetErrorCode::CapacityExceeded, "too many Steam requests in flight"};
    return track(
        *this, CallKind::CreateLobby, api->createLobby(lobbyType(request.visibility), request.maxMembers),
        "Steam couldn't start creating a lobby");
}

NetResult<RequestId> SteamServicesImpl::findLobbies(const LobbyQuery& query) {
    if (state != SteamServiceState::Client) return noLobbies();
    for (const auto& [key, value] : query.requiredMetadata)
        if (key.empty() || key.size() > kMaxLobbyKeyBytes || value.size() > kMaxLobbyValueBytes)
            return NetError{NetErrorCode::InvalidArgument, "lobby filter key or value too long"};
    if (!callSlotFree(*this)) return NetError{NetErrorCode::CapacityExceeded, "too many Steam requests in flight"};
    for (const auto& [key, value] : query.requiredMetadata) api->addLobbyListFilter(key, value);
    const uint32_t results = std::clamp<uint32_t>(query.maxResults, 1, kMaxLobbyResults);
    return track(*this, CallKind::FindLobbies, api->requestLobbyList(results), "Steam couldn't start a lobby search");
}

NetResult<RequestId> SteamServicesImpl::joinLobby(LobbyId lobby) {
    if (state != SteamServiceState::Client) return noLobbies();
    if (!lobby.valid()) return NetError{NetErrorCode::InvalidArgument, "no lobby ID"};
    if (!callSlotFree(*this)) return NetError{NetErrorCode::CapacityExceeded, "too many Steam requests in flight"};
    return track(
        *this, CallKind::JoinLobby, api->joinLobby(lobby.value), "Steam couldn't start joining the lobby", lobby.value);
}

NetResult<void> SteamServicesImpl::leaveLobby(LobbyId lobby) {
    if (state != SteamServiceState::Client) return noLobbies();
    if (!lobby.valid()) return NetError{NetErrorCode::InvalidArgument, "no lobby ID"};
    api->leaveLobby(lobby.value);
    for (uint64_t& joined : joinedLobbies)
        if (joined == lobby.value) joined = 0;
    OnlineEvent event;
    event.kind = OnlineEventKind::LobbyLeft;
    event.lobby = lobby;
    push(std::move(event));
    return {};
}

NetResult<void> SteamServicesImpl::inviteFriend(LobbyId lobby) {
    if (state != SteamServiceState::Client) return noLobbies();
    if (!lobby.valid()) return NetError{NetErrorCode::InvalidArgument, "no lobby ID"};
    api->inviteToLobby(lobby.value);
    return {};
}

NetResult<void> SteamServicesImpl::setLobbyJoinable(LobbyId lobby, bool joinable) {
    if (state != SteamServiceState::Client) return noLobbies();
    if (!api->setLobbyJoinable(lobby.value, joinable))
        return NetError{NetErrorCode::InvalidState, "Steam refused: we don't own that lobby"};
    return {};
}

NetResult<void> SteamServicesImpl::setLobbyMetadata(LobbyId lobby, const std::string& key, const std::string& value) {
    if (state != SteamServiceState::Client) return noLobbies();
    if (key.empty() || key.size() > kMaxLobbyKeyBytes || value.size() > kMaxLobbyValueBytes)
        return NetError{NetErrorCode::InvalidArgument, "lobby metadata key or value too long"};
    if (!api->setLobbyData(lobby.value, key, value))
        return NetError{NetErrorCode::InvalidState, "Steam refused: we don't own that lobby"};
    return {};
}

NetResult<std::vector<PeerId>> SteamServicesImpl::lobbyMembers(LobbyId lobby) const {
    if (state != SteamServiceState::Client) return noLobbies();
    std::vector<PeerId> members;
    const uint32_t count = api->lobbyMemberCount(lobby.value);
    members.reserve(count);
    for (uint32_t i = 0; i < count; ++i)
        if (const uint64_t id = api->lobbyMember(lobby.value, i)) members.push_back(PeerId{id});
    return members;
}

LobbySummary SteamServicesImpl::summary(LobbyId id) const {
    LobbySummary lobby;
    lobby.id = id;
    lobby.owner = PeerId{api->lobbyOwner(id.value)};
    lobby.members = api->lobbyMemberCount(id.value);
    lobby.capacity = api->lobbyMemberLimit(id.value);
    std::array<char, kMaxLobbyKeyBytes + 1> key{};
    std::array<char, kMaxLobbyValueBytes + 1> value{};
    const uint32_t entries = std::min<uint32_t>(api->lobbyDataCount(id.value), 64);
    for (uint32_t e = 0; e < entries; ++e) {
        key.fill('\0');
        value.fill('\0');
        if (api->lobbyDataByIndex(
                id.value, e, std::span(key).first(key.size() - 1), std::span(value).first(value.size() - 1)))
            lobby.metadata.emplace_back(key.data(), value.data());
    }
    return lobby;
}

NetResult<LobbySummary> SteamServicesImpl::lobbyInfo(LobbyId lobby) const {
    if (state != SteamServiceState::Client) return noLobbies();
    if (!lobby.valid() || std::find(joinedLobbies.begin(), joinedLobbies.end(), lobby.value) == joinedLobbies.end())
        return NetError{NetErrorCode::InvalidArgument, "not in that lobby"};
    return summary(lobby);
}

void SteamServicesImpl::cancelCall(RequestId request) {
    for (Call& c : calls)
        if (c.kind != CallKind::None && c.request == request) c.cancelled = true;
}

static void failed(SteamServicesImpl& s, RequestId request, const char* why) {
    OnlineEvent event;
    event.kind = OnlineEventKind::RequestFailed;
    event.request = request;
    event.error = {NetErrorCode::Io, why};
    s.push(std::move(event));
}

void SteamServicesImpl::lobbyCreated(uint64_t call, bool ok, uint64_t lobby) {
    Call* c = findCall(*this, CallKind::CreateLobby, call);
    if (!c) return;   // not ours, or from before a shutdown
    const Call done = *c;
    *c = {};
    if (done.cancelled) {
        if (ok && lobby) api->leaveLobby(lobby);   // we're its owner, and nobody wants it
        return;
    }
    if (!ok || !lobby) return failed(*this, done.request, "Steam couldn't create the lobby");
    if (auto slot = std::find(joinedLobbies.begin(), joinedLobbies.end(), 0); slot != joinedLobbies.end())
        *slot = lobby;
    OnlineEvent event;
    event.kind = OnlineEventKind::LobbyCreated;
    event.request = done.request;
    event.lobby = LobbyId{lobby};
    push(std::move(event));
}

void SteamServicesImpl::lobbyList(uint64_t call, bool ok, uint32_t count) {
    Call* c = findCall(*this, CallKind::FindLobbies, call);
    if (!c) return;
    const Call done = *c;
    *c = {};
    if (done.cancelled) return;
    if (!ok) return failed(*this, done.request, "Steam's lobby search failed");
    OnlineEvent event;
    event.kind = OnlineEventKind::LobbyList;
    event.request = done.request;
    count = std::min(count, kMaxLobbyResults);
    event.lobbies.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        const LobbyId id{api->lobbyByIndex(i)};
        if (id.valid()) event.lobbies.push_back(summary(id));
    }
    push(std::move(event));
}

void SteamServicesImpl::lobbyEntered(uint64_t call, bool ok, uint64_t lobby) {
    Call* c = findCall(*this, CallKind::JoinLobby, call);
    if (!c) return;
    const Call done = *c;
    *c = {};
    if (done.cancelled) {
        // Nobody wants this lobby, unless a later join asked for it again (cancel, then
        // join the same lobby): leaving then would take that join's membership away.
        const bool wanted = std::find(joinedLobbies.begin(), joinedLobbies.end(), lobby) != joinedLobbies.end() ||
                            std::any_of(calls.begin(), calls.end(), [&](const Call& other) {
                                return other.kind == CallKind::JoinLobby && !other.cancelled && other.lobby == lobby;
                            });
        if (ok && lobby && !wanted) api->leaveLobby(lobby);
        return;
    }
    if (!ok || !lobby) return failed(*this, done.request, "couldn't join the lobby: it's full, gone or private");
    if (auto slot = std::find(joinedLobbies.begin(), joinedLobbies.end(), 0); slot != joinedLobbies.end())
        *slot = lobby;
    OnlineEvent event;
    event.kind = OnlineEventKind::LobbyJoined;
    event.request = done.request;
    event.lobby = LobbyId{lobby};
    push(std::move(event));
}

void SteamServicesImpl::leaveAllLobbies() {
    for (uint64_t& lobby : joinedLobbies)
        if (lobby) {
            api->leaveLobby(lobby);
            lobby = 0;
        }
}

}   // namespace shockwave::net
