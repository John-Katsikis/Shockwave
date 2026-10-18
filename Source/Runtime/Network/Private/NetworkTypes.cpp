#include <Network/NetworkTypes.h>
#include <Network/Transport.h>

namespace shockwave::net {

const char* netErrorName(NetErrorCode code) {
    switch (code) {
    case NetErrorCode::None: return "none";
    case NetErrorCode::Unavailable: return "unavailable";
    case NetErrorCode::InvalidArgument: return "invalid argument";
    case NetErrorCode::InvalidState: return "invalid state";
    case NetErrorCode::NotConnected: return "not connected";
    case NetErrorCode::CapacityExceeded: return "capacity exceeded";
    case NetErrorCode::AddressInUse: return "address in use";
    case NetErrorCode::Closed: return "closed";
    case NetErrorCode::Malformed: return "malformed";
    case NetErrorCode::Io: return "i/o error";
    }
    return "unknown";
}

const char* reasonName(ReasonCode reason) {
    switch (reason) {
    case ReasonCode::Normal: return "Normal";
    case ReasonCode::ProtocolMismatch: return "ProtocolMismatch";
    case ReasonCode::ContentMismatch: return "ContentMismatch";
    case ReasonCode::ServerFull: return "ServerFull";
    case ReasonCode::NotLobbyMember: return "NotLobbyMember";
    case ReasonCode::AuthenticationFailed: return "AuthenticationFailed";
    case ReasonCode::AuthUnavailable: return "AuthUnavailable";
    case ReasonCode::Timeout: return "Timeout";
    case ReasonCode::MalformedPacket: return "MalformedPacket";
    case ReasonCode::RateLimited: return "RateLimited";
    case ReasonCode::HostLeft: return "HostLeft";
    case ReasonCode::MapNotLoaded: return "MapNotLoaded";
    case ReasonCode::SimulationOverrun: return "SimulationOverrun";
    case ReasonCode::ServiceUnavailable: return "ServiceUnavailable";
    case ReasonCode::UnsupportedFeature: return "UnsupportedFeature";
    }
    return nullptr;
}

const char* deliveryName(Delivery delivery) { return delivery == Delivery::Unreliable ? "unreliable" : "reliable"; }

const char* sendResultName(SendResult result) {
    switch (result) {
    case SendResult::Queued: return "queued";
    case SendResult::Dropped: return "dropped";
    case SendResult::Backpressure: return "backpressure";
    case SendResult::NotConnected: return "not connected";
    case SendResult::Failed: return "failed";
    }
    return "unknown";
}

}   // namespace shockwave::net
