// Arena's network types (plan sections 6.3, 7 and 10): the player and rules profiles, the
// lobby commands and the match events. Sessions don't use them until Batch 4; until then
// they're what `player --check`, --network-info and docs/NetworkSchema.md describe.
#include "ArenaGame.h"

#include <Engine/Components.h>
#include <Engine/PhysicsComponents.h>
#include <Multiplayer/Replication.h>
#include <Shockwave/Game.h>

#include <algorithm>
#include <cmath>

namespace arena {
namespace {

using namespace shockwave;
using namespace shockwave::net;

// arena.player.v1 field indices, after the five pose fields.
enum PlayerField : size_t {
    kYaw = 5,
    kPitch,
    kHealth,
    kAmmo,
    kReloadTicks,
    kProtectionTicks,
    kTeam,
    kLifeState,
    kGrounded,
    kReserved,
    kKills,
    kDeaths,
};

enum RulesField : size_t { kBlueScore, kOrangeScore, kRemainingTicks, kRoundSerial, kPhase };
enum Phase : uint64_t { kWaiting, kPlaying, kFinished };

uint64_t ticks(float seconds, uint32_t tickRate) {
    return seconds > 0 ? uint64_t(std::ceil(double(seconds) * tickRate)) : 0;
}

const char* capturePlayer(const World& world, Entity e, const ReplicationContext& context, ProfileValue& out) {
    if (const char* why = capturePoseFields(world, e, out)) return why;
    const ArenaFighter* fighter = world.get<ArenaFighter>(e);
    if (!fighter) return "the entity has no ArenaFighter";
    // Aim still lives in the GameMode, not in components; Batch 6 moves it into state
    // the host owns. Until then it's sent as straight ahead.
    out.fields[kYaw].f[0] = 0;
    out.fields[kPitch].f[0] = 0;
    // Health below zero only means dead; the profile carries 0 and the life state.
    out.fields[kHealth].u = uint64_t(std::lround(std::max(0.0f, fighter->health)));
    const ArenaWeapon* weapon = world.get<ArenaWeapon>(e);
    out.fields[kAmmo].u = weapon ? uint64_t(std::max(0, weapon->ammo)) : 0;
    out.fields[kReloadTicks].u = weapon ? ticks(weapon->reload, context.tickRate) : 0;
    out.fields[kProtectionTicks].u = ticks(fighter->protection, context.tickRate);
    out.fields[kTeam].u = uint64_t(fighter->team);
    out.fields[kLifeState].u = fighter->health > 0 ? 0 : 1;
    const CharacterController* character = world.get<CharacterController>(e);
    out.fields[kGrounded].u = character && character->grounded ? 1 : 0;
    out.fields[kReserved].u = 0;
    out.fields[kKills].u = uint64_t(std::max(0, fighter->kills));
    out.fields[kDeaths].u = uint64_t(std::max(0, fighter->deaths));
    return nullptr;
}

void applyPlayer(World& world, Entity e, const ReplicationContext& context, const ProfileValue& value) {
    applyPoseFields(world, e, value);
    const float tick = 1.0f / float(context.tickRate);
    if (ArenaFighter* fighter = world.get<ArenaFighter>(e)) {
        fighter->health = value.fields[kLifeState].u == 0 ? float(value.fields[kHealth].u) : 0.0f;
        fighter->protection = float(value.fields[kProtectionTicks].u) * tick;
        fighter->team = int(value.fields[kTeam].u);
        fighter->kills = int(value.fields[kKills].u);
        fighter->deaths = int(value.fields[kDeaths].u);
    }
    if (ArenaWeapon* weapon = world.get<ArenaWeapon>(e)) {
        weapon->ammo = int(value.fields[kAmmo].u);
        weapon->reload = float(value.fields[kReloadTicks].u) * tick;
    }
}

const char* captureRules(const World& world, Entity e, const ReplicationContext& context, ProfileValue& out) {
    const ArenaRules* rules = world.get<ArenaRules>(e);
    if (!rules) return "the entity has no ArenaRules";
    out.fields[kBlueScore].u = uint64_t(std::max(0, rules->blue));
    out.fields[kOrangeScore].u = uint64_t(std::max(0, rules->orange));
    out.fields[kRemainingTicks].u = ticks(rules->remaining, context.tickRate);
    out.fields[kRoundSerial].u = 0;   // Arena plays one round per Play until Batch 8 adds restarts
    out.fields[kPhase].u = rules->finished ? kFinished : kPlaying;
    return nullptr;
}

void applyRules(World& world, Entity e, const ReplicationContext& context, const ProfileValue& value) {
    ArenaRules* rules = world.get<ArenaRules>(e);
    if (!rules) return;
    rules->blue = int(value.fields[kBlueScore].u);
    rules->orange = int(value.fields[kOrangeScore].u);
    rules->remaining = float(value.fields[kRemainingTicks].u) / float(context.tickRate);
    rules->finished = value.fields[kPhase].u == kFinished;
}

void report(bool ok, const std::string& why) {
    if (!ok) SW_LOG(LogNetwork, Error, "Arena network types: {}", why);
}

void registerNetworkTypes(ReplicationRegistry& registry) {
    std::string why;
    ReplicationProfile player;
    player.id = 32;
    player.name = "arena.player.v1";
    player.fields = poseFields();
    player.fields.insert(
        player.fields.end(), {
                                 netFloat("yaw", 3.1416, NetFieldRole::Aim),
                                 netFloat("pitch", 1.5534, NetFieldRole::Aim),   // 89 degrees
                                 netInt("health", NetFieldType::U16, 0, 10000, NetFieldRole::Health),
                                 netInt("ammo", NetFieldType::U16, 0, 1000, NetFieldRole::Ammo),
                                 netInt("reload_remaining_ticks", NetFieldType::U16, 0, 65535, NetFieldRole::Timer),
                                 netInt("protection_remaining_ticks", NetFieldType::U16, 0, 65535, NetFieldRole::Timer),
                                 netInt("team", NetFieldType::U8, 0, 1, NetFieldRole::Team),
                                 netInt("life_state", NetFieldType::U8, 0, 1, NetFieldRole::State),   // 0 alive, 1 dead
                                 netInt("grounded", NetFieldType::U8, 0, 1, NetFieldRole::State),
                                 netInt("reserved", NetFieldType::U8, 0, 0, NetFieldRole::Reserved),
                                 netInt("kills", NetFieldType::U16, 0, 65535, NetFieldRole::Score),
                                 netInt("deaths", NetFieldType::U16, 0, 65535, NetFieldRole::Score),
                             });
    player.requiredComponents = {"Transform", "CharacterController", "ArenaFighter"};
    player.perPlayer = true;
    player.capture = capturePlayer;
    player.apply = applyPlayer;
    report(registry.addProfile(std::move(player), why), why);

    ReplicationProfile rules;
    rules.id = 33;
    rules.name = "arena.rules.v1";
    rules.fields = {
        netInt("blue_score", NetFieldType::U16, 0, 65535, NetFieldRole::Score),
        netInt("orange_score", NetFieldType::U16, 0, 65535, NetFieldRole::Score),
        netInt("remaining_ticks", NetFieldType::U32, 0, 4294967295.0, NetFieldRole::Timer),
        netInt("round_serial", NetFieldType::U32, 0, 4294967295.0, NetFieldRole::Counter),
        netInt("phase", NetFieldType::U8, 0, 2, NetFieldRole::State),   // 0 waiting, 1 playing, 2 finished
    };
    rules.requiredComponents = {"ArenaRules"};
    rules.capture = captureRules;
    rules.apply = applyRules;
    report(registry.addProfile(std::move(rules), why), why);

    CommandDesc team;
    team.id = 32;
    team.name = "arena.request_team";
    team.payload = {netInt("team", NetFieldType::U8, 0, 1, NetFieldRole::Team)};
    team.when = "while the round is waiting, if that team has room";
    report(registry.addCommand(std::move(team), why), why);

    CommandDesc ready;
    ready.id = 33;
    ready.name = "arena.ready_to_play";
    ready.payload = {netInt("ready", NetFieldType::U8, 0, 1, NetFieldRole::State)};
    ready.when = "while the round is waiting";
    report(registry.addCommand(std::move(ready), why), why);

    CommandDesc restart;
    restart.id = 34;
    restart.name = "arena.request_round_restart";
    restart.permission = CommandPermission::Host;
    restart.ratePerSecond = 1;
    restart.burst = 1;
    restart.when = "after the round has finished";
    report(registry.addCommand(std::move(restart), why), why);

    EventDesc shot;
    shot.id = 32;
    shot.name = "arena.shot_resolved";
    shot.payload = {
        netInt("shooter", NetFieldType::U64, 1, 18446744073709551615.0, NetFieldRole::EntityRef),
        netInt("shooter_incarnation", NetFieldType::U32, 0, 4294967295.0, NetFieldRole::Counter),
        netInt("input_tick", NetFieldType::U32, 0, 4294967295.0, NetFieldRole::Counter),
        netInt("shot_ordinal", NetFieldType::U16, 0, 65535, NetFieldRole::Counter),
        netVec3("origin", 32768, NetFieldRole::Position),
        netVec3("end", 32768, NetFieldRole::Position),
        netInt("victim", NetFieldType::U64, 0, 18446744073709551615.0, NetFieldRole::EntityRef),   // 0: a miss
        netInt("damage", NetFieldType::U16, 0, 10000, NetFieldRole::Health),
    };
    report(registry.addEvent(std::move(shot), why), why);

    EventDesc died;
    died.id = 33;
    died.name = "arena.player_died";
    died.payload = {
        netInt("victim", NetFieldType::U64, 1, 18446744073709551615.0, NetFieldRole::EntityRef),
        netInt("incarnation", NetFieldType::U32, 0, 4294967295.0, NetFieldRole::Counter),
        netInt("killer", NetFieldType::U64, 0, 18446744073709551615.0, NetFieldRole::EntityRef),   // 0: environment
        netInt("respawn_tick", NetFieldType::U32, 0, 4294967295.0, NetFieldRole::Timer),
    };
    report(registry.addEvent(std::move(died), why), why);

    EventDesc round;
    round.id = 34;
    round.name = "arena.round_changed";
    round.payload = {
        netInt("round_serial", NetFieldType::U32, 0, 4294967295.0, NetFieldRole::Counter),
        netInt("phase", NetFieldType::U8, 0, 2, NetFieldRole::State),
        netInt("winning_team", NetFieldType::U8, 0, 255, NetFieldRole::Team),   // 255: none
    };
    report(registry.addEvent(std::move(round), why), why);
}

}   // namespace

}   // namespace arena

SW_NETWORK_TYPES(arena::registerNetworkTypes)
