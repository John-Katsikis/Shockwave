#include <Multiplayer/Replication.h>

#include <Network/Sha256.h>
#include <Network/Wire.h>

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>

namespace shockwave::net {

namespace {

WireStatus status(WireStatusCode code, size_t offset, const char* field) { return {code, uint32_t(offset), field}; }

WireStatusCode statusFor(WireError error) {
    switch (error) {
    case WireError::None: return WireStatusCode::Ok;
    case WireError::Overflow: return WireStatusCode::Overflow;
    case WireError::Truncated: return WireStatusCode::Truncated;
    case WireError::NonFinite: return WireStatusCode::NonFinite;
    case WireError::BadString: return WireStatusCode::BadString;
    case WireError::TooLong: return WireStatusCode::OutOfRange;
    }
    return WireStatusCode::Invalid;
}

bool isSigned(NetFieldType type) {
    return type == NetFieldType::I8 || type == NetFieldType::I16 || type == NetFieldType::I32;
}

bool isFloat(NetFieldType type) {
    return type == NetFieldType::F32 || type == NetFieldType::Vec3 || type == NetFieldType::Quat;
}

int floatCount(NetFieldType type) {
    return type == NetFieldType::F32 ? 1 : type == NetFieldType::Vec3 ? 3 : type == NetFieldType::Quat ? 4 : 0;
}

// The full range a type can hold, for checking descriptors.
void typeLimits(NetFieldType type, double& lo, double& hi) {
    switch (type) {
    case NetFieldType::Bool: lo = 0, hi = 1; return;
    case NetFieldType::U8: lo = 0, hi = 255; return;
    case NetFieldType::U16: lo = 0, hi = 65535; return;
    case NetFieldType::U32: lo = 0, hi = 4294967295.0; return;
    case NetFieldType::U64: lo = 0, hi = 18446744073709551615.0; return;
    case NetFieldType::I8: lo = -128, hi = 127; return;
    case NetFieldType::I16: lo = -32768, hi = 32767; return;
    case NetFieldType::I32: lo = -2147483648.0, hi = 2147483647.0; return;
    default: lo = 0, hi = 0; return;
    }
}

// An integer field's value as a signed or unsigned 64-bit number, compared with its range
// without going through double for U64 (whose values double can't all hold).
bool intInRange(const NetFieldDesc& field, uint64_t raw) {
    if (field.mask != 0) return (raw & ~field.mask) == 0;
    if (isSigned(field.type)) {
        const int64_t v = static_cast<int64_t>(raw);
        return double(v) >= field.min && double(v) <= field.max;
    }
    if (field.type == NetFieldType::U64) {
        const uint64_t lo = field.min <= 0 ? 0 : static_cast<uint64_t>(field.min);
        const uint64_t hi = field.max >= 18446744073709551615.0 ? ~uint64_t(0) : static_cast<uint64_t>(field.max);
        return raw >= lo && raw <= hi;
    }
    return double(raw) >= field.min && double(raw) <= field.max;
}

// Checks one field's value; the status names it.
WireStatus checkValue(const NetFieldDesc& field, const NetFieldValue& value, size_t offset) {
    const char* name = field.name.c_str();
    if (isFloat(field.type)) {
        const int count = floatCount(field.type);
        const float bound = field.type == NetFieldType::Quat ? 1.01f : float(field.max);
        for (int i = 0; i < count; ++i) {
            if (!std::isfinite(value.f[size_t(i)])) return status(WireStatusCode::NonFinite, offset, name);
            if (std::fabs(value.f[size_t(i)]) > bound) return status(WireStatusCode::OutOfRange, offset, name);
        }
        if (field.type == NetFieldType::Quat) {
            const float n = std::sqrt(
                value.f[0] * value.f[0] + value.f[1] * value.f[1] + value.f[2] * value.f[2] + value.f[3] * value.f[3]);
            if (n < 0.99f || n > 1.01f) return status(WireStatusCode::Invalid, offset, name);
        }
        return {};
    }
    // Signed values live sign-extended in u; anything else out of the type's range is bad.
    uint64_t raw = value.u;
    switch (field.type) {
    case NetFieldType::Bool:
    case NetFieldType::U8:
        if (raw > 0xFF) return status(WireStatusCode::OutOfRange, offset, name);
        break;
    case NetFieldType::U16:
        if (raw > 0xFFFF) return status(WireStatusCode::OutOfRange, offset, name);
        break;
    case NetFieldType::U32:
        if (raw > 0xFFFFFFFFu) return status(WireStatusCode::OutOfRange, offset, name);
        break;
    case NetFieldType::I8:
    case NetFieldType::I16:
    case NetFieldType::I32: {
        double lo, hi;
        typeLimits(field.type, lo, hi);
        const int64_t v = static_cast<int64_t>(raw);
        if (double(v) < lo || double(v) > hi) return status(WireStatusCode::OutOfRange, offset, name);
        break;
    }
    default: break;
    }
    if (!intInRange(field, raw)) return status(WireStatusCode::OutOfRange, offset, name);
    return {};
}

bool writeValue(WireWriter& w, NetFieldType type, const NetFieldValue& v) {
    switch (type) {
    case NetFieldType::Bool:
    case NetFieldType::U8: return w.u8(uint8_t(v.u));
    case NetFieldType::U16: return w.u16(uint16_t(v.u));
    case NetFieldType::U32: return w.u32(uint32_t(v.u));
    case NetFieldType::U64: return w.u64(v.u);
    case NetFieldType::I8: return w.i8(int8_t(int64_t(v.u)));
    case NetFieldType::I16: return w.i16(int16_t(int64_t(v.u)));
    case NetFieldType::I32: return w.i32(int32_t(int64_t(v.u)));
    case NetFieldType::F32:
    case NetFieldType::Vec3:
    case NetFieldType::Quat:
        for (int i = 0; i < floatCount(type); ++i)
            if (!w.f32(v.f[size_t(i)])) return false;
        return true;
    }
    return false;
}

bool readValue(WireReader& r, NetFieldType type, NetFieldValue& v) {
    v = {};
    switch (type) {
    case NetFieldType::Bool:
    case NetFieldType::U8: {
        uint8_t x;
        if (!r.u8(x)) return false;
        v.u = x;
        return true;
    }
    case NetFieldType::U16: {
        uint16_t x;
        if (!r.u16(x)) return false;
        v.u = x;
        return true;
    }
    case NetFieldType::U32: {
        uint32_t x;
        if (!r.u32(x)) return false;
        v.u = x;
        return true;
    }
    case NetFieldType::U64: return r.u64(v.u);
    case NetFieldType::I8: {
        int8_t x;
        if (!r.i8(x)) return false;
        v.u = uint64_t(int64_t(x));
        return true;
    }
    case NetFieldType::I16: {
        int16_t x;
        if (!r.i16(x)) return false;
        v.u = uint64_t(int64_t(x));
        return true;
    }
    case NetFieldType::I32: {
        int32_t x;
        if (!r.i32(x)) return false;
        v.u = uint64_t(int64_t(x));
        return true;
    }
    case NetFieldType::F32:
    case NetFieldType::Vec3:
    case NetFieldType::Quat:
        for (int i = 0; i < floatCount(type); ++i)
            if (!r.f32(v.f[size_t(i)])) return false;
        return true;
    }
    return false;
}

uint32_t fieldsBytes(std::span<const NetFieldDesc> fields) {
    uint32_t bytes = 0;
    for (const NetFieldDesc& f : fields) bytes += netFieldBytes(f.type);
    return bytes;
}

bool validName(std::string_view name) {
    if (name.empty() || name.size() > 64) return false;
    bool dotSeen = false;
    char previous = '.';
    for (const char c : name) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.';
        if (!ok) return false;
        if (c == '.') {
            if (previous == '.') return false;
            dotSeen = true;
        }
        previous = c;
    }
    return dotSeen && name.back() != '.' && name.front() >= 'a' && name.front() <= 'z';
}

// "arena.player.v1" ends in a version.
bool versionedName(std::string_view name) {
    const size_t dot = name.rfind('.');
    if (dot == std::string_view::npos || dot + 2 > name.size() || name[dot + 1] != 'v') return false;
    for (size_t i = dot + 2; i < name.size(); ++i)
        if (name[i] < '0' || name[i] > '9') return false;
    return dot + 2 < name.size();
}

std::string number(double value) {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::setprecision(std::numeric_limits<double>::max_digits10) << value;
    return out.str();
}

void checkFields(
    std::string_view what, std::span<const NetFieldDesc> fields, bool allowEmpty, std::vector<std::string>& problems) {
    if (fields.empty() && !allowEmpty) problems.push_back(std::string(what) + " has no fields");
    if (fields.size() > kMaxNetFields)
        problems.push_back(
            std::string(what) + " has " + std::to_string(fields.size()) + " fields; at most " +
            std::to_string(kMaxNetFields));
    for (size_t i = 0; i < fields.size(); ++i) {
        const NetFieldDesc& f = fields[i];
        const std::string where = std::string(what) + " field '" + f.name + "'";
        bool nameOk = !f.name.empty() && f.name.size() <= 48;
        for (const char c : f.name) nameOk &= (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
        if (!nameOk) problems.push_back(where + ": names are lowercase letters, digits and _");
        for (size_t j = 0; j < i; ++j)
            if (fields[j].name == f.name) problems.push_back(where + " appears twice");
        if (uint8_t(f.type) > uint8_t(NetFieldType::Quat)) {
            problems.push_back(where + ": unsupported field type");
            continue;
        }
        if (isFloat(f.type)) {
            if (f.type != NetFieldType::Quat && (!std::isfinite(f.max) || f.max <= 0))
                problems.push_back(where + ": a float field needs a finite bound above 0");
            if (f.mask != 0) problems.push_back(where + ": only integer fields take a bit mask");
            continue;
        }
        double lo, hi;
        typeLimits(f.type, lo, hi);
        if (f.mask != 0) {
            if (f.type == NetFieldType::Bool || isSigned(f.type) || double(f.mask) > hi)
                problems.push_back(where + ": the bit mask doesn't fit the field");
            continue;
        }
        if (!(f.min <= f.max) || f.min < lo || f.max > hi || f.min != std::floor(f.min) || f.max != std::floor(f.max))
            problems.push_back(
                where + ": range " + number(f.min) + ".." + number(f.max) + " doesn't fit " + netFieldTypeName(f.type));
    }
}

}   // namespace

const char* netFieldTypeName(NetFieldType type) {
    switch (type) {
    case NetFieldType::Bool: return "bool";
    case NetFieldType::U8: return "u8";
    case NetFieldType::U16: return "u16";
    case NetFieldType::U32: return "u32";
    case NetFieldType::U64: return "u64";
    case NetFieldType::I8: return "i8";
    case NetFieldType::I16: return "i16";
    case NetFieldType::I32: return "i32";
    case NetFieldType::F32: return "f32";
    case NetFieldType::Vec3: return "vec3";
    case NetFieldType::Quat: return "quat";
    }
    return "?";
}

const char* netFieldRoleName(NetFieldRole role) {
    switch (role) {
    case NetFieldRole::None: return "";
    case NetFieldRole::Position: return "position";
    case NetFieldRole::Rotation: return "rotation";
    case NetFieldRole::LinearVelocity: return "linear velocity";
    case NetFieldRole::AngularVelocity: return "angular velocity";
    case NetFieldRole::Flags: return "flags";
    case NetFieldRole::Aim: return "aim";
    case NetFieldRole::Health: return "health";
    case NetFieldRole::Ammo: return "ammo";
    case NetFieldRole::Timer: return "timer (ticks)";
    case NetFieldRole::Team: return "team";
    case NetFieldRole::State: return "state";
    case NetFieldRole::Score: return "score";
    case NetFieldRole::Counter: return "counter";
    case NetFieldRole::EntityRef: return "network entity";
    case NetFieldRole::Reserved: return "reserved";
    }
    return "";
}

uint32_t netFieldBytes(NetFieldType type) {
    switch (type) {
    case NetFieldType::Bool:
    case NetFieldType::U8:
    case NetFieldType::I8: return 1;
    case NetFieldType::U16:
    case NetFieldType::I16: return 2;
    case NetFieldType::U32:
    case NetFieldType::I32:
    case NetFieldType::F32: return 4;
    case NetFieldType::U64: return 8;
    case NetFieldType::Vec3: return 12;
    case NetFieldType::Quat: return 16;
    }
    return 0;
}

const char* commandPermissionName(CommandPermission permission) {
    switch (permission) {
    case CommandPermission::AnyPlayer: return "any player";
    case CommandPermission::Owner: return "owner of the target";
    case CommandPermission::Host: return "host only";
    }
    return "?";
}

NetFieldDesc netInt(std::string name, NetFieldType type, double min, double max, NetFieldRole role) {
    return {std::move(name), type, min, max, role, 0};
}
NetFieldDesc netFloat(std::string name, double bound, NetFieldRole role) {
    return {std::move(name), NetFieldType::F32, -bound, bound, role, 0};
}
NetFieldDesc netVec3(std::string name, double bound, NetFieldRole role) {
    return {std::move(name), NetFieldType::Vec3, -bound, bound, role, 0};
}
NetFieldDesc netQuat(std::string name, NetFieldRole role) {
    return {std::move(name), NetFieldType::Quat, -1.01, 1.01, role, 0};
}
NetFieldDesc netBits(std::string name, uint8_t mask, NetFieldRole role) {
    return {std::move(name), NetFieldType::U8, 0, 0, role, mask};
}

std::vector<NetFieldDesc> poseFields() {
    return {
        netVec3("position", 32768, NetFieldRole::Position),
        netQuat("rotation"),
        netVec3("linear_velocity", 512, NetFieldRole::LinearVelocity),
        netVec3("angular_velocity", 1024, NetFieldRole::AngularVelocity),
        netBits("flags", PoseState::kSleeping | PoseState::kTeleport | PoseState::kGrounded),
    };
}

// ---- Codecs -------------------------------------------------------------------------------

WireStatus encodeFields(
    std::span<const NetFieldDesc> fields, const ProfileValue& value, std::span<uint8_t> out, uint32_t& written) {
    written = 0;
    if (fields.size() > kMaxNetFields) return status(WireStatusCode::Invalid, 0, "fields");
    WireWriter writer(out);
    for (size_t i = 0; i < fields.size(); ++i) {
        const size_t offset = writer.size();
        if (WireStatus s = checkValue(fields[i], value.fields[i], offset); !s) return s;
        if (!writeValue(writer, fields[i].type, value.fields[i]))
            return status(statusFor(writer.error()), offset, fields[i].name.c_str());
    }
    written = uint32_t(writer.size());
    return {};
}

WireStatus decodeFields(std::span<const NetFieldDesc> fields, std::span<const uint8_t> bytes, ProfileValue& out) {
    if (fields.size() > kMaxNetFields) return status(WireStatusCode::Invalid, 0, "fields");
    ProfileValue value;
    WireReader reader(bytes);
    for (size_t i = 0; i < fields.size(); ++i) {
        const size_t offset = reader.offset();
        if (!readValue(reader, fields[i].type, value.fields[i]))
            return status(statusFor(reader.error()), offset, fields[i].name.c_str());
        if (WireStatus s = checkValue(fields[i], value.fields[i], offset); !s) return s;
    }
    if (reader.remaining() != 0) return status(WireStatusCode::TrailingBytes, reader.offset(), "fields");
    out = value;
    return {};
}

WireStatus
encodeProfile(const ReplicationProfile& profile, const ProfileValue& value, std::span<uint8_t> out, uint32_t& written) {
    written = 0;
    if (profile.validate)
        if (const char* bad = profile.validate(value)) return status(WireStatusCode::Invalid, 0, bad);
    return encodeFields(profile.fields, value, out, written);
}

WireStatus decodeProfile(const ReplicationProfile& profile, std::span<const uint8_t> bytes, ProfileValue& out) {
    ProfileValue value;
    if (WireStatus s = decodeFields(profile.fields, bytes, value); !s) return s;
    if (profile.validate)
        if (const char* bad = profile.validate(value)) return status(WireStatusCode::Invalid, 0, bad);
    out = value;
    return {};
}

const char* captureProfile(
    const ReplicationProfile& profile, const World& world, Entity entity, const ReplicationContext& context,
    ProfileValue& out) {
    if (!profile.capture) return "the profile has no capture function";
    ProfileValue value;
    if (const char* why = profile.capture(world, entity, context, value)) return why;
    for (size_t i = 0; i < profile.fields.size(); ++i)
        if (!checkValue(profile.fields[i], value.fields[i], 0)) return "captured state is out of the profile's range";
    if (profile.validate)
        if (const char* bad = profile.validate(value)) return bad;
    out = value;
    return nullptr;
}

// ---- Registry -----------------------------------------------------------------------------

bool ReplicationRegistry::addProfile(ReplicationProfile profile, std::string& why) {
    if (m_frozen) return reject(why, "the replication registry is frozen; register before the session starts");
    if (profile.id < kFirstEngineProfile || profile.id > kLastGameProfile)
        return reject(why, "profile " + profile.name + ": ID " + std::to_string(profile.id) + " isn't in 1..1023");
    const bool engine = profile.name.starts_with("engine.");
    if (engine != (profile.id <= kLastEngineProfile))
        return reject(why, "profile " + profile.name + ": IDs 1..31 are for engine.* profiles, 32..1023 for games'");
    for (const ReplicationProfile& other : m_profiles) {
        if (other.id == profile.id)
            return reject(
                why, "profile " + profile.name + ": ID " + std::to_string(profile.id) + " is already " + other.name);
        if (other.name == profile.name) return reject(why, "profile " + profile.name + " is registered twice");
    }
    m_profiles.push_back(std::move(profile));
    return true;
}

bool ReplicationRegistry::addCommand(CommandDesc command, std::string& why) {
    if (m_frozen) return reject(why, "the replication registry is frozen; register before the session starts");
    if (command.id == 0) return reject(why, "command " + command.name + ": ID 0 isn't allowed");
    if ((command.id < kFirstGameCommand) != command.name.starts_with("engine."))
        return reject(why, "command " + command.name + ": IDs 1..31 are for engine.* commands, 32 and up for games'");
    for (const CommandDesc& other : m_commands) {
        if (other.id == command.id)
            return reject(
                why, "command " + command.name + ": ID " + std::to_string(command.id) + " is already " + other.name);
        if (other.name == command.name) return reject(why, "command " + command.name + " is registered twice");
    }
    m_commands.push_back(std::move(command));
    return true;
}

bool ReplicationRegistry::addEvent(EventDesc event, std::string& why) {
    if (m_frozen) return reject(why, "the replication registry is frozen; register before the session starts");
    if (event.id == 0) return reject(why, "event " + event.name + ": ID 0 isn't allowed");
    if ((event.id < kFirstGameEvent) != event.name.starts_with("engine."))
        return reject(why, "event " + event.name + ": IDs 1..31 are for engine.* events, 32 and up for games'");
    for (const EventDesc& other : m_events) {
        if (other.id == event.id)
            return reject(
                why, "event " + event.name + ": ID " + std::to_string(event.id) + " is already " + other.name);
        if (other.name == event.name) return reject(why, "event " + event.name + " is registered twice");
    }
    m_events.push_back(std::move(event));
    return true;
}

bool ReplicationRegistry::reject(std::string& why, std::string message) {
    why = message;
    m_rejected.push_back(std::move(message));
    return false;
}

bool ReplicationRegistry::freeze(std::vector<std::string>& problems) {
    if (m_frozen) return true;
    const size_t before = problems.size();
    // A registration that failed fails the freeze too, even if the caller ignored it.
    problems.insert(problems.end(), m_rejected.begin(), m_rejected.end());
    for (ReplicationProfile& p : m_profiles) {
        const std::string what = "profile " + p.name;
        if (!validName(p.name) || !versionedName(p.name))
            problems.push_back(what + ": names are lowercase and dotted and end in a version, like game.door.v1");
        checkFields(what, p.fields, false, problems);
        const uint32_t bytes = fieldsBytes(p.fields);
        if (bytes > kMaxStateBytes)
            problems.push_back(
                what + " encodes to " + std::to_string(bytes) + " bytes; a state record holds at most " +
                std::to_string(kMaxStateBytes));
        if (kStateRecordHeaderBytes + bytes > kMaxUnreliableMessageBytes - kSnapshotOverheadBytes)
            problems.push_back(what + " is too big for one record in a snapshot");
        if (!p.capture || !p.apply) problems.push_back(what + " needs both capture and apply functions");
        p.encodedBytes = uint16_t(std::min<uint32_t>(bytes, 0xFFFF));
    }
    for (CommandDesc& c : m_commands) {
        const std::string what = "command " + c.name;
        if (!validName(c.name)) problems.push_back(what + ": names are lowercase and dotted, like game.open_door");
        checkFields(what, c.payload, true, problems);
        const uint32_t bytes = fieldsBytes(c.payload);
        if (bytes > kMaxCommandPayloadBytes)
            problems.push_back(
                what + " payload is " + std::to_string(bytes) + " bytes; at most " +
                std::to_string(kMaxCommandPayloadBytes));
        if (c.ratePerSecond < 1 || c.ratePerSecond > kMaxCommandsPerSecond || c.burst < 1 || c.burst > kMaxCommandBurst)
            problems.push_back(what + ": rate must be 1..20 per second with a burst of 1..10");
        if (c.permission == CommandPermission::Owner && c.targetProfiles.empty())
            problems.push_back(what + ": an owner-only command needs target profiles");
        for (const std::string& target : c.targetProfiles)
            if (std::none_of(m_profiles.begin(), m_profiles.end(), [&](const auto& p) { return p.name == target; }))
                problems.push_back(what + ": target profile " + target + " isn't registered");
        c.payloadBytes = uint16_t(std::min<uint32_t>(bytes, 0xFFFF));
    }
    for (EventDesc& e : m_events) {
        const std::string what = "event " + e.name;
        if (!validName(e.name)) problems.push_back(what + ": names are lowercase and dotted, like game.door_opened");
        checkFields(what, e.payload, true, problems);
        const uint32_t bytes = fieldsBytes(e.payload);
        if (bytes > kMaxEventPayloadBytes)
            problems.push_back(
                what + " payload is " + std::to_string(bytes) + " bytes; at most " +
                std::to_string(kMaxEventPayloadBytes));
        e.payloadBytes = uint16_t(std::min<uint32_t>(bytes, 0xFFFF));
    }
    if (problems.size() != before) return false;

    std::sort(m_profiles.begin(), m_profiles.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
    std::sort(m_commands.begin(), m_commands.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
    std::sort(m_events.begin(), m_events.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
    m_profileIndex.fill(0);
    for (size_t i = 0; i < m_profiles.size(); ++i) m_profileIndex[m_profiles[i].id] = int16_t(i + 1);
    m_commandIndex.assign(m_commands.empty() ? 0 : size_t(m_commands.back().id) + 1, 0);
    for (size_t i = 0; i < m_commands.size(); ++i) m_commandIndex[m_commands[i].id] = uint16_t(i + 1);
    m_eventIndex.assign(m_events.empty() ? 0 : size_t(m_events.back().id) + 1, 0);
    for (size_t i = 0; i < m_events.size(); ++i) m_eventIndex[m_events[i].id] = uint16_t(i + 1);
    m_frozen = true;
    return true;
}

const ReplicationProfile* ReplicationRegistry::profile(uint16_t id) const {
    if (!m_frozen || id > kLastGameProfile || m_profileIndex[id] == 0) return nullptr;
    return &m_profiles[size_t(m_profileIndex[id] - 1)];
}

const ReplicationProfile* ReplicationRegistry::profile(std::string_view name) const {
    if (!m_frozen) return nullptr;
    for (const ReplicationProfile& p : m_profiles)
        if (p.name == name) return &p;
    return nullptr;
}

const CommandDesc* ReplicationRegistry::command(uint16_t id) const {
    if (!m_frozen || id >= m_commandIndex.size() || m_commandIndex[id] == 0) return nullptr;
    return &m_commands[m_commandIndex[id] - 1u];
}

const EventDesc* ReplicationRegistry::event(uint16_t id) const {
    if (!m_frozen || id >= m_eventIndex.size() || m_eventIndex[id] == 0) return nullptr;
    return &m_events[m_eventIndex[id] - 1u];
}

std::string ReplicationRegistry::catalogText() const {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    auto fields = [&](std::span<const NetFieldDesc> list) {
        for (const NetFieldDesc& f : list) {
            out << "  field " << f.name << ' ' << netFieldTypeName(f.type);
            if (f.mask)
                out << " mask " << f.mask;
            else
                out << ' ' << number(f.min) << ' ' << number(f.max);
            out << " role " << unsigned(f.role) << '\n';
        }
    };
    // Sorted by ID (freeze sorts), so registration order doesn't change the digest.
    out << "shockwave_network_catalog 1\nwire_protocol " << kProtocolVersion << '\n';
    for (const ReplicationProfile& p : m_profiles) {
        out << "profile " << p.id << ' ' << p.name << " bytes " << fieldsBytes(p.fields) << '\n';
        fields(p.fields);
    }
    for (const CommandDesc& c : m_commands) {
        out << "command " << c.id << ' ' << c.name << " permission " << unsigned(c.permission) << " rate "
            << c.ratePerSecond << ' ' << c.burst << '\n';
        for (const std::string& target : c.targetProfiles) out << "  target " << target << '\n';
        fields(c.payload);
    }
    for (const EventDesc& e : m_events) {
        out << "event " << e.id << ' ' << e.name << '\n';
        fields(e.payload);
    }
    return out.str();
}

Digest ReplicationRegistry::schemaDigest() const { return sha256(catalogText()); }

}   // namespace shockwave::net
