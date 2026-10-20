// Input and commands (Multiplayer/NetworkWorld.h, docs/Networking.md "Input and
// commands"): a client's InputCommands stamped with the host tick they're for, the host's
// one-input-per-player-per-tick consumption with its short fallback, registered commands
// checked against the registry before game code sees them, and the Ping/Pong clock that
// tells a client which host tick its next input can still reach.
#include <Multiplayer/NetworkWorld.h>

#include "NetworkWorldImpl.h"

#include <Core/Log.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace shockwave::net {

namespace {

constexpr uint32_t kMaxPingsAnswered = 10;   // a second, per peer; more are ignored
constexpr uint32_t kMaxPingsPerSecond = 30;   // more than this is a flood: rejected
constexpr double kMaxRttSeconds = 5;
// In ticks at 60 Hz (NetworkWorld::scaledTicks).
constexpr uint32_t kMinLead = 2, kMaxLead = 16;
constexpr uint32_t kResetTicks = 8;   // a clock jump bigger than this restarts the inputs
constexpr double kClockSlew = 0.25;   // how much of a new clock estimate each Pong takes

InputCommand neutral(uint32_t tick, uint32_t incarnation, const InputCommand* aim) {
    InputCommand c;
    c.targetTick = tick;
    c.incarnation = incarnation;
    if (aim) {
        c.yaw = aim->yaw;
        c.pitch = aim->pitch;
        c.viewTick = aim->viewTick;
        c.viewFraction = aim->viewFraction;
    }
    return c;
}

size_t commandIndex(const ReplicationRegistry& registry, const CommandDesc& desc) {
    return size_t(&desc - registry.commands().data());
}

}   // namespace

Peer* NetworkWorld::Impl::peerForSlot(uint16_t slot) {
    if (slot == 0 || !host()) return nullptr;
    for (Peer& p : peers)
        if (p.admitted() && p.slot == slot) return &p;
    return nullptr;
}

void NetworkWorld::Impl::clearInput() {
    clockSamples = clockNext = pingId = pingsSent = 0;
    nextPing = 0;
    haveClockOffset = false;
    clockOffset = 0;
    firstInputTick = lastInputTick = recentCount = clientCommandSeq = 0;
    sentInput = inputClocked = false;
    clientTokens.clear();
    clientTokensAt.clear();
    for (ReceivedCommand& c : commands) c = ReceivedCommand{};
    commandHead = commandCount = 0;
}

// ---- Clock ----------------------------------------------------------------------------------

double NetworkWorld::Impl::bestRtt() const {
    const uint32_t n = std::min<uint32_t>(clockSamples, uint32_t(clock.size()));
    double best = n ? clock[0].rtt : 0;
    for (uint32_t i = 1; i < n; ++i) best = std::min(best, clock[i].rtt);
    return best;
}

bool NetworkWorld::Impl::estimatedHostTick(double& out) const {
    if (!haveClockOffset) return false;
    out = clockOffset + now * double(settings().tickRate);
    return true;
}

void NetworkWorld::Impl::sendPings() {
    if (host() || !activated || now < nextPing) return;
    Ping ping;
    ping.pingId = ++pingId;
    ping.originUs = uint64_t(std::max(0.0, now) * 1e6);
    if (send(peers[0].connection, ping)) ++stats.pingsSent;
    // Four quick probes at first, then one a second.
    nextPing = now + (++pingsSent < 4 ? 0.1 : 1.0);
}

void NetworkWorld::Impl::pingMessage(Peer& p, const Ping& ping) {
    if (p.pingWindow < 0 || now - p.pingWindow >= 1) {
        p.pingWindow = now;
        p.pings = 0;
    }
    if (++p.pings > kMaxPingsPerSecond) return reject(p, ReasonCode::RateLimited, "too many pings");
    if (p.pings > kMaxPingsAnswered) return;
    Pong pong;
    pong.pingId = ping.pingId;
    pong.echoedOriginUs = ping.originUs;
    pong.responderReceiveUs = pong.responderSendUs = uint64_t(std::max(0.0, now) * 1e6);
    pong.responderTick = tick + 1;   // the next tick the host simulates
    if (send(p.connection, pong)) ++stats.pingsAnswered;
}

void NetworkWorld::Impl::pongMessage(const Pong& pong) {
    // Only answers to our own recent pings count; anything else is ignored, not trusted.
    if (pong.pingId == 0 || pong.pingId > pingId || pingId - pong.pingId > 16) return;
    const double nowUs = std::max(0.0, now) * 1e6;
    if (double(pong.echoedOriginUs) > nowUs) return;
    const double dwell = double(pong.responderSendUs - pong.responderReceiveUs) / 1e6;
    const double rtt = (nowUs - double(pong.echoedOriginUs)) / 1e6 - dwell;
    if (!(rtt >= 0) || rtt > kMaxRttSeconds) return;
    ++stats.pongsReceived;
    ClockSample& sample = clock[clockNext];
    clockNext = (clockNext + 1) % uint32_t(clock.size());
    ++clockSamples;
    sample.rtt = rtt;
    sample.at = now;
    sample.hostTick = double(pong.responderTick) + rtt * 0.5 * double(settings().tickRate);
    stats.rttMs = bestRtt() * 1000;
    // The clock follows the sample with the lowest round trip of the last eight (the one
    // least delayed by queues), smoothed so one sample moves predicted ticks only a
    // little. A jump of more than kResetTicks is taken at once.
    const uint32_t n = std::min<uint32_t>(clockSamples, uint32_t(clock.size()));
    const ClockSample* best = &clock[0];
    for (uint32_t i = 1; i < n; ++i)
        if (clock[i].rtt < best->rtt) best = &clock[i];
    const double raw = best->hostTick - best->at * double(settings().tickRate);
    if (!haveClockOffset ||
        std::abs(raw - clockOffset) > double(NetworkWorld::scaledTicks(kResetTicks, settings().tickRate))) {
        clockOffset = raw;
        haveClockOffset = true;
    } else {
        clockOffset += (raw - clockOffset) * kClockSlew;
    }
}

// ---- Inputs ---------------------------------------------------------------------------------

uint32_t NetworkWorld::sendInput(InputCommand command) {
    Impl& impl = *m_impl;
    if (m_role != NetworkRole::Client || m_closed || !m_transport || !impl.activated) return 0;
    const uint32_t rate = m_start.settings.tickRate;
    uint32_t desired;
    double hostTick = 0;
    const bool clocked = impl.estimatedHostTick(hostTick);
    if (clocked) {
        // Arrives a one-way trip from now, with two ticks to spare.
        const uint32_t lead =
            std::clamp(uint32_t(std::ceil(impl.bestRtt() * 0.5 * rate)) + 2, kMinLead, scaledTicks(kMaxLead, rate));
        impl.stats.inputLead = lead;
        desired = uint32_t(int64_t(std::floor(hostTick)) + lead);
    } else {
        desired = impl.sentInput ? impl.lastInputTick + 1 : impl.firstInputTick;
    }
    if (newer(impl.firstInputTick, desired)) desired = impl.firstInputTick;
    uint32_t first = desired;   // the first tick this call sends
    if (impl.sentInput) {
        const uint32_t next = impl.lastInputTick + 1;
        const uint32_t gap = newer(desired, next) ? desired - next : next - desired;
        if (gap > scaledTicks(kResetTicks, rate)) {
            impl.recentCount = 0;   // the clock jumped: start over rather than flood
            // The first measurement replacing the guess is expected; a later jump isn't.
            if (impl.inputClocked)
                impl.resetPrediction("the host's clock jumped");
            else
                impl.clearPrediction();
        } else if (newer(next, desired + 1)) {
            return 0;   // already ahead of where the host will be
        } else if (newer(next, desired)) {
            first = desired = next;   // one tick ahead: keep the ticks consecutive
        } else {
            // Behind: fill the ticks the clock skipped (at most one message's worth), so
            // the host doesn't fall back and the prediction doesn't skip them.
            first = desired - next + 1 > kMaxCatchUpInputs ? desired - (kMaxCatchUpInputs - 1) : next;
        }
    }
    const Entity predicted = impl.predictsControlled() ? impl.controlledEntityNow() : kNullEntity;
    for (uint32_t target = first;; ++target) {
        InputCommand c = command;
        if (target != desired) {
            // A filled tick repeats the held controls; presses go to the newest only.
            c.pressedBits = 0;
            ++impl.stats.catchUpInputs;
        }
        c.targetTick = target;
        if (impl.recentCount == kMaxInputsPerMessage) {
            std::rotate(impl.recentInputs.begin(), impl.recentInputs.begin() + 1, impl.recentInputs.end());
            --impl.recentCount;
        }
        impl.recentInputs[impl.recentCount++] = c;
        ++impl.stats.inputsSent;
        if (predicted.valid()) impl.predict(c, predicted);
        if (target == desired) break;
    }
    impl.lastInputTick = desired;
    impl.sentInput = true;
    impl.inputClocked = clocked;
    // The newest and up to three earlier ones, oldest first.
    Inputs message;
    message.count = uint8_t(impl.recentCount);
    message.commands = impl.recentInputs;
    if (impl.send(impl.peers[0].connection, message)) ++impl.stats.inputMessagesSent;
    return desired;
}

void NetworkWorld::Impl::inputsMessage(Peer& p, const Inputs& m) {
    if (p.phase != Peer::Phase::Active) {
        stats.inputsLate += m.count;   // movement starts at Active's first_input_tick
        return;
    }
    const uint32_t limit = tick + 1 + NetworkWorld::scaledTicks(NetworkWorld::kMaxInputLeadTicks, settings().tickRate);
    bool ahead = false;
    for (uint8_t i = 0; i < m.count; ++i) {
        const InputCommand& c = m.commands[i];
        if (c.invalid()) return reject(p, ReasonCode::MalformedPacket, "an input command breaks its rules");
        ++stats.inputsReceived;
        if (newer(p.firstInputTick, c.targetTick) || (p.tookAny && !newer(c.targetTick, p.lastTaken))) {
            ++stats.inputsLate;   // never re-runs a tick, never fires late bullets
            continue;
        }
        if (newer(c.targetTick, limit)) {
            ++stats.inputsAhead;
            ahead = true;
            continue;
        }
        const size_t slot = c.targetTick % NetworkWorld::kInputHistory;
        if (p.haveInput[slot] && p.inputs[slot].targetTick == c.targetTick) continue;   // a resend: first wins
        p.inputs[slot] = c;
        p.haveInput[slot] = true;
    }
    if (!ahead) return;
    // A client whose clock runs ahead now and then gets its inputs ignored; one whose
    // messages are far ahead for half a second's worth of ticks within one second is
    // cheating or broken.
    if (p.aheadWindow < 0 || now - p.aheadWindow >= 1) {
        p.aheadWindow = now;
        p.aheadViolations = 0;
    }
    if (++p.aheadViolations > settings().tickRate / 2)
        reject(p, ReasonCode::RateLimited, "inputs keep arriving too far ahead");
}

bool NetworkWorld::takeInput(uint16_t slot, uint32_t incarnation, InputCommand& out) {
    Impl& impl = *m_impl;
    const uint32_t t = impl.tick;
    Peer* p = impl.peerForSlot(slot);
    if (!p || p->phase != Peer::Phase::Active) {
        out = neutral(t, incarnation, nullptr);
        return false;
    }
    if (p->tookAny && !newer(t, p->lastTaken)) {
        out = neutral(t, incarnation, p->haveLastInput ? &p->lastInput : nullptr);   // asked twice this tick
        return false;
    }
    p->lastTaken = t;
    p->tookAny = true;
    const size_t index = t % kInputHistory;
    if (p->haveInput[index] && p->inputs[index].targetTick == t && !newer(p->firstInputTick, t)) {
        p->haveInput[index] = false;
        if (p->inputs[index].incarnation == incarnation) {
            out = p->inputs[index];
            p->lastInput = out;
            p->haveLastInput = true;
            p->missing = 0;
            ++impl.stats.inputsUsed;
            return true;
        }
    }
    ++impl.stats.inputFallbacks;
    ++p->missing;
    const bool recent = p->haveLastInput && p->lastInput.incarnation == incarnation;
    if (recent && p->missing <= kInputFallbackTicks) {
        out = p->lastInput;
        out.heldBits &= uint16_t(~InputCommand::kHeldFire);
        out.pressedBits = 0;
        out.targetTick = t;
    } else {
        out = neutral(t, incarnation, recent ? &p->lastInput : nullptr);
    }
    return false;
}

uint32_t NetworkWorld::tick() const { return m_impl->tick; }

bool NetworkWorld::disconnect(uint16_t slot, ReasonCode reason) {
    Peer* p = m_closed ? nullptr : m_impl->peerForSlot(slot);
    if (!p) return false;
    m_impl->reject(*p, reason, "the host sent this player away");
    return true;
}

// ---- Commands -------------------------------------------------------------------------------

bool NetworkWorld::Impl::commandAllowed(
    const CommandDesc& desc, uint16_t sender, uint64_t target, Entity& targetEntity, std::vector<double>& tokens,
    std::vector<double>& tokensAt, const char*& why) {
    if (desc.permission == CommandPermission::Host && sender != 1) {
        why = "only the host's player may send it";
        return false;
    }
    World* world = w.m_world;
    if (w.online()) {
        targetEntity = kNullEntity;
        if (target != 0) {
            const NetObject* o = object(target);
            if (!o) {
                why = "its target isn't a network object";
                return false;
            }
            targetEntity = o->entity;
            if (std::find(desc.targetProfiles.begin(), desc.targetProfiles.end(), o->profile->name) ==
                desc.targetProfiles.end()) {
                why = "it can't target that kind of object";
                return false;
            }
            if (desc.permission == CommandPermission::Owner && o->owner != sender) {
                why = "only the target's owner may send it";
                return false;
            }
        } else if (!desc.targetProfiles.empty() || desc.permission == CommandPermission::Owner) {
            why = "it needs a target";
            return false;
        }
    } else if (!desc.targetProfiles.empty()) {
        // Offline there are no network IDs: the entity itself, with the right profile.
        const NetworkObject* n =
            world && world->alive(targetEntity) ? world->get<NetworkObject>(targetEntity) : nullptr;
        if (!n || std::find(desc.targetProfiles.begin(), desc.targetProfiles.end(), n->profile) ==
                      desc.targetProfiles.end()) {
            why = "it can't target that entity";
            return false;
        }
    } else {
        targetEntity = kNullEntity;
    }
    if (!w.online()) return true;   // offline there's nobody to protect the host from
    const size_t index = commandIndex(*w.m_start.registry, desc);
    if (tokens.size() <= index) {
        const size_t n = w.m_start.registry->commands().size();
        tokens.resize(n, -1);
        tokensAt.resize(n, 0);
    }
    if (tokens[index] < 0) {
        tokens[index] = desc.burst;
    } else {
        tokens[index] = std::min<double>(desc.burst, tokens[index] + (now - tokensAt[index]) * desc.ratePerSecond);
    }
    tokensAt[index] = now;
    if (tokens[index] < 1) {
        why = "it's being sent too often";
        return false;
    }
    tokens[index] -= 1;
    return true;
}

bool NetworkWorld::Impl::queueCommand(
    const CommandDesc& desc, uint16_t sender, uint64_t target, Entity targetEntity, const ProfileValue& payload) {
    if (commandCount == NetworkWorld::kMaxQueuedCommands) {
        ++stats.commandsRefused;
        return false;
    }
    ReceivedCommand& c = commands[(commandHead + commandCount++) % NetworkWorld::kMaxQueuedCommands];
    c.command = &desc;
    c.commandId = desc.id;
    c.senderSlot = sender;
    c.target = target;
    c.targetEntity = targetEntity;
    c.tick = tick;
    c.payload = payload;
    ++stats.commandsAccepted;
    return true;
}

void NetworkWorld::Impl::commandMessage(Peer& p, const Command& m) {
    const ReplicationRegistry* registry = w.m_start.registry;
    const CommandDesc* desc = registry ? registry->command(m.commandId) : nullptr;
    // The registries match (the schema digest), so these only come from a broken or
    // hostile client.
    if (!desc) return reject(p, ReasonCode::MalformedPacket, "a command this game doesn't have");
    if (p.haveCommandSeq && !newer(m.commandSeq, p.commandSeq))
        return reject(p, ReasonCode::MalformedPacket, "a command sent twice");
    p.commandSeq = m.commandSeq;
    p.haveCommandSeq = true;
    ProfileValue payload;
    if (!decodeFields(desc->payload, m.payload, payload))
        return reject(p, ReasonCode::MalformedPacket, "a command's payload is outside its ranges");
    if (p.phase != Peer::Phase::Active) {
        ++stats.commandsRefused;
        return;
    }
    Entity targetEntity = kNullEntity;
    const char* why = nullptr;
    if (!commandAllowed(*desc, p.slot, m.target, targetEntity, p.commandTokens, p.commandTokensAt, why)) {
        ++stats.commandsRefused;
        SW_LOG(LogNetwork, Verbose, "Refused {} from player {}: {}", desc->name, p.slot, why);
        return;
    }
    queueCommand(*desc, p.slot, m.target, targetEntity, payload);
}

NetResult<void>
NetworkWorld::submitCommand(uint16_t commandId, uint64_t target, const ProfileValue& payload, Entity targetEntity) {
    Impl& impl = *m_impl;
    const ReplicationRegistry* registry = m_start.registry;
    if (!registry || !registry->frozen())
        return NetError{NetErrorCode::InvalidState, "the session has no frozen registry"};
    const CommandDesc* desc = registry->command(commandId);
    if (!desc) return NetError{NetErrorCode::InvalidArgument, "no such command"};
    // Encoding checks every field's range; the host decodes the same bytes.
    std::array<uint8_t, kMaxCommandPayloadBytes> bytes{};
    uint32_t written = 0;
    if (!encodeFields(desc->payload, payload, bytes, written))
        return NetError{NetErrorCode::InvalidArgument, "the payload is outside its ranges"};
    ProfileValue checked;
    if (!decodeFields(desc->payload, std::span(bytes.data(), written), checked))
        return NetError{NetErrorCode::InvalidArgument, "the payload doesn't decode"};
    const char* why = nullptr;
    if (m_role == NetworkRole::Client) {
        if (m_closed || !m_transport || !impl.activated)
            return NetError{NetErrorCode::InvalidState, "not in the game yet"};
        Entity unused = kNullEntity;
        if (!impl.commandAllowed(*desc, m_localSlot, target, unused, impl.clientTokens, impl.clientTokensAt, why))
            return NetError{NetErrorCode::InvalidArgument, why};
        Command message;
        message.commandSeq = ++impl.clientCommandSeq;
        message.target = target;
        message.commandId = commandId;
        message.payload = std::span(bytes.data(), written);
        if (!impl.send(impl.peers[0].connection, message))
            return NetError{NetErrorCode::Unavailable, "couldn't send the command"};
        ++impl.stats.commandsSent;
        return {};
    }
    if (online() && (m_closed || !m_transport))
        return NetError{NetErrorCode::InvalidState, "the session isn't hosting"};
    if (!impl.commandAllowed(*desc, 1, target, targetEntity, impl.clientTokens, impl.clientTokensAt, why)) {
        ++impl.stats.commandsRefused;
        return NetError{NetErrorCode::InvalidArgument, why};
    }
    ++impl.stats.commandsSent;
    if (!impl.queueCommand(*desc, 1, target, targetEntity, checked))
        return NetError{NetErrorCode::Unavailable, "too many commands are waiting"};
    return {};
}

bool NetworkWorld::pollCommand(ReceivedCommand& out) {
    Impl& impl = *m_impl;
    if (impl.commandCount == 0) return false;
    out = impl.commands[impl.commandHead];
    impl.commandHead = (impl.commandHead + 1) % kMaxQueuedCommands;
    --impl.commandCount;
    return true;
}

}   // namespace shockwave::net
