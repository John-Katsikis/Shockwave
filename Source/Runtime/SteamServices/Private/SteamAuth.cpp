// Steam session-ticket authentication (docs/SteamMultiplayerImplementation.md section 4.5).
// Each side asks for a ticket for the other's identity and sends it over the connected
// transport; the receiver begins an auth session, which is only provisional until Steam's
// ValidateAuthTicketResponse. A validation that doesn't come in time fails closed with
// AuthUnavailable; an invalid or revoked ticket fails with AuthenticationFailed. Ticket
// bytes are never logged.
#include "SteamServicesImpl.h"

#include <algorithm>
#include <cmath>

namespace shockwave::net {

namespace {

// Responses that say Steam couldn't decide, rather than that the ticket is bad.
bool unavailable(SteamAuthResponse response) {
    return response == SteamAuthResponse::UserNotConnectedToSteam || response == SteamAuthResponse::VacCheckTimedOut ||
           response == SteamAuthResponse::NetworkIdentityFailure;
}

}   // namespace

NetResult<RequestId> SteamServicesImpl::requestAuthTicket(PeerId remote) {
    if (state == SteamServiceState::Off) return notRunning();
    if (!remote.valid()) return NetError{NetErrorCode::InvalidArgument, "no peer to make a ticket for"};
    auto ticket = std::find_if(tickets.begin(), tickets.end(), [](const Ticket& t) { return t.handle == 0; });
    if (ticket == tickets.end()) return NetError{NetErrorCode::CapacityExceeded, "too many auth tickets"};
    uint32_t size = 0;
    const uint32_t handle = api->getAuthSessionTicket(remote.value, ticket->bytes, size);
    if (!handle) return NetError{NetErrorCode::Io, "Steam couldn't make an auth ticket"};
    if (size == 0 || size > kMaxAuthTicketBytes) {
        api->cancelAuthTicket(handle);
        return NetError{NetErrorCode::Malformed, "Steam's auth ticket doesn't fit wire protocol v1 (1 to 4096 bytes)"};
    }
    ticket->handle = handle;
    ticket->request = newRequest();
    ticket->remote = remote;
    ticket->ready = false;
    ticket->size = size;
    return ticket->request;
}

NetResult<std::span<const uint8_t>> SteamServicesImpl::authTicket(RequestId request) const {
    if (!current(request)) return NetError{NetErrorCode::InvalidArgument, "no such ticket"};
    for (const Ticket& t : tickets)
        if (t.handle && t.request == request) {
            if (!t.ready) return NetError{NetErrorCode::InvalidState, "the ticket isn't ready yet"};
            return std::span<const uint8_t>(t.bytes.data(), t.size);
        }
    return NetError{NetErrorCode::InvalidArgument, "no such ticket"};
}

bool SteamServicesImpl::cancelTicket(RequestId request) {
    for (Ticket& t : tickets)
        if (t.handle && t.request == request) {
            api->cancelAuthTicket(t.handle);
            t = {};
            return true;
        }
    return false;
}

void SteamServicesImpl::ticketReady(uint32_t handle, bool ok) {
    auto ticket =
        std::find_if(tickets.begin(), tickets.end(), [&](const Ticket& t) { return t.handle && t.handle == handle; });
    if (ticket == tickets.end()) return;   // cancelled, or from before a shutdown
    OnlineEvent event;
    event.request = ticket->request;
    event.peer = ticket->remote;
    if (ok) {
        ticket->ready = true;
        event.kind = OnlineEventKind::AuthTicketReady;
    } else {
        api->cancelAuthTicket(ticket->handle);
        *ticket = {};
        event.kind = OnlineEventKind::RequestFailed;
        event.error = {NetErrorCode::Io, "Steam couldn't make an auth ticket"};
    }
    push(std::move(event));
}

NetResult<void> SteamServicesImpl::beginAuthSession(PeerId remote, std::span<const uint8_t> ticket) {
    if (state == SteamServiceState::Off) return notRunning();
    if (!remote.valid()) return NetError{NetErrorCode::InvalidArgument, "no peer to authenticate"};
    if (ticket.empty() || ticket.size() > kMaxAuthTicketBytes)
        return NetError{NetErrorCode::InvalidArgument, "an auth ticket is 1 to 4096 bytes"};
    if (std::any_of(sessions.begin(), sessions.end(), [&](const AuthSession& s) { return s.peer == remote; }))
        return NetError{NetErrorCode::InvalidState, "that peer is already authenticating; end its session first"};
    auto session = std::find_if(sessions.begin(), sessions.end(), [](const AuthSession& s) { return !s.peer.valid(); });
    if (session == sessions.end()) return NetError{NetErrorCode::CapacityExceeded, "too many auth sessions"};
    switch (api->beginAuthSession(ticket, remote.value)) {
    case SteamBeginAuth::Ok: break;
    case SteamBeginAuth::Duplicate:
        return NetError{NetErrorCode::InvalidState, "Steam already has an auth session for that peer"};
    case SteamBeginAuth::Expired: return NetError{NetErrorCode::InvalidArgument, "the auth ticket has expired"};
    case SteamBeginAuth::InvalidTicket:
    case SteamBeginAuth::InvalidVersion:
    case SteamBeginAuth::GameMismatch: return NetError{NetErrorCode::InvalidArgument, "Steam rejected the auth ticket"};
    }
    session->peer = remote;
    session->validated = false;
    session->deadlineUs = api->nowUs() + uint64_t(std::llround(settings.authTimeoutSeconds * 1e6));
    return {};
}

void SteamServicesImpl::endAuthSession(PeerId remote) {
    if (state == SteamServiceState::Off || !remote.valid()) return;
    for (AuthSession& s : sessions)
        if (s.peer == remote) {
            api->endAuthSession(remote.value);
            s = {};
        }
    // And the tickets we gave that peer.
    for (Ticket& t : tickets)
        if (t.handle && t.remote == remote) {
            api->cancelAuthTicket(t.handle);
            t = {};
        }
}

void SteamServicesImpl::authValidated(uint64_t steamId, SteamAuthResponse response) {
    auto session = std::find_if(sessions.begin(), sessions.end(), [&](const AuthSession& s) {
        return s.peer.valid() && s.peer.value == steamId;
    });
    if (session == sessions.end()) return;   // ended, or from before a shutdown
    OnlineEvent event;
    event.peer = session->peer;
    if (response == SteamAuthResponse::Ok) {
        if (session->validated) return;   // Steam repeats itself; once is enough
        session->validated = true;
        event.kind = OnlineEventKind::AuthSucceeded;
    } else {
        // Before or after succeeding: a revoked ticket, a ban, signed in elsewhere.
        api->endAuthSession(steamId);
        *session = {};
        event.kind = OnlineEventKind::AuthFailed;
        event.reason = unavailable(response) ? ReasonCode::AuthUnavailable : ReasonCode::AuthenticationFailed;
    }
    push(std::move(event));
}

void SteamServicesImpl::expireAuthSessions() {
    const uint64_t now = api->nowUs();
    for (AuthSession& s : sessions)
        if (s.peer.valid() && !s.validated && now >= s.deadlineUs) {
            api->endAuthSession(s.peer.value);
            OnlineEvent event;
            event.kind = OnlineEventKind::AuthFailed;
            event.peer = s.peer;
            event.reason = ReasonCode::AuthUnavailable;
            s = {};
            push(std::move(event));
        }
}

void SteamServicesImpl::endAllAuth() {
    for (AuthSession& s : sessions)
        if (s.peer.valid()) {
            api->endAuthSession(s.peer.value);
            s = {};
        }
    for (Ticket& t : tickets)
        if (t.handle) {
            api->cancelAuthTicket(t.handle);
            t = {};
        }
}

}   // namespace shockwave::net
