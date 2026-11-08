// Reactor Run's registered components: the fields listed here are saved in scenes, saves
// and checkpoints, appear in the Inspector, and are range-checked by --check.
#include "DemoComponents.h"

#include <Engine/Components.h>

namespace demo {
using namespace shockwave;

const char* objectiveKindName(ObjectiveKind kind) {
    switch (kind) {
    case ObjectiveKind::Reach: return "reach";
    case ObjectiveKind::Destroy: return "destroy";
    case ObjectiveKind::Collect: return "collect";
    case ObjectiveKind::Use: return "use";
    case ObjectiveKind::Hold: return "hold";
    }
    return "reach";
}

void registerTypes() {
    const char* category = "Reactor Run";
    registerComponent<DemoPlayer>(
        "DemoPlayer",
        {field("speed", &DemoPlayer::speed, 0.05f, 0.5f, 12), field("sprint", &DemoPlayer::sprint, 0.01f, 1, 3),
         field("sensitivity", &DemoPlayer::sensitivity, 0.005f, 0.01f, 1),
         field("stick_speed", &DemoPlayer::stickSpeed, 1, 10, 720),
         field("eye_height", &DemoPlayer::eyeHeight, 0.01f, 0.5f, 2.5f), field("yaw", &DemoPlayer::yaw),
         field("pitch", &DemoPlayer::pitch)},
        category);
    registerComponent<Health>(
        "Health",
        {field("max", &Health::max, 1, 1, 10000), field("current", &Health::current, 1, 0, 10000),
         field("group", &Health::group)},
        category);
    registerComponent<Loadout>(
        "Loadout",
        {field("magazine", &Loadout::magazine, 1, 1, 200), field("ammo", &Loadout::ammo, 1, 0, 200),
         field("reserve", &Loadout::reserve, 1, 0, 999), field("max_reserve", &Loadout::maxReserve, 1, 0, 999),
         field("damage", &Loadout::damage, 1, 1, 1000), field("interval", &Loadout::interval, 0.01f, 0.03f, 5),
         field("reload_seconds", &Loadout::reloadSeconds, 0.05f, 0.1f, 10), field("range", &Loadout::range, 1, 1, 500),
         field("cooldown_ticks", &Loadout::cooldownTicks), field("reload_ticks", &Loadout::reloadTicks),
         field("shots", &Loadout::shots), field("hits", &Loadout::hits)},
        category);
    registerComponent<ObjectiveProgress>(
        "ObjectiveProgress",
        {field("id", &ObjectiveProgress::id),
         enumField("kind", &ObjectiveProgress::kind, {"Reach", "Destroy", "Collect", "Use", "Hold"}),
         field("text", &ObjectiveProgress::text), field("order", &ObjectiveProgress::order, 1, 0, 1000),
         field("group", &ObjectiveProgress::group), field("required", &ObjectiveProgress::required, 1, 0, 1000),
         field("target", &ObjectiveProgress::target), field("radius", &ObjectiveProgress::radius, 0.05f, 0.1f, 100),
         field("seconds", &ObjectiveProgress::seconds, 0.1f, 0, 3600), field("count", &ObjectiveProgress::count),
         field("total", &ObjectiveProgress::total), field("ticks", &ObjectiveProgress::ticks),
         field("complete", &ObjectiveProgress::complete)},
        category);
    registerComponent<InteractionTarget>(
        "InteractionTarget",
        {enumField("kind", &InteractionTarget::kind, {"Use", "Pickup", "Exit"}),
         field("prompt", &InteractionTarget::prompt), field("action", &InteractionTarget::action),
         field("radius", &InteractionTarget::radius, 0.05f, 0.2f, 20), field("item", &InteractionTarget::item),
         field("amount", &InteractionTarget::amount, 1, 0, 999), field("group", &InteractionTarget::group),
         field("used", &InteractionTarget::used)},
        category);
    registerComponent<DemoEnemy>(
        "DemoEnemy",
        {field("speed", &DemoEnemy::speed, 0.05f, 0, 10), field("sight", &DemoEnemy::sight, 0.5f, 1, 200),
         field("stop_distance", &DemoEnemy::stopDistance, 0.1f, 0.5f, 50),
         field("fire_interval", &DemoEnemy::fireInterval, 0.05f, 0.1f, 20),
         field("damage", &DemoEnemy::damage, 1, 0, 1000), field("eye_height", &DemoEnemy::eyeHeight, 0.01f, 0.1f, 3),
         field("cooldown_ticks", &DemoEnemy::cooldownTicks), field("alerted", &DemoEnemy::alerted)},
        category);
    registerComponent<DemoGate>(
        "DemoGate", {field("objective", &DemoGate::objective), field("open", &DemoGate::open)}, category);
    registerComponent<CampaignProgress>(
        "CampaignProgress",
        {field("complete_id", &CampaignProgress::completeId), field("next", &CampaignProgress::next),
         field("spawn", &CampaignProgress::spawn), field("results", &CampaignProgress::results),
         field("state", &CampaignProgress::state), field("ticks", &CampaignProgress::ticks),
         field("kills", &CampaignProgress::kills)},
        category);
}

}   // namespace demo
