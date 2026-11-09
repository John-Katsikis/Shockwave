// Reactor Run's rules: see DemoRules.h for the order of a tick and what wins when.
#include "DemoRules.h"

#include <Engine/Hierarchy.h>
#include <Engine/Levels.h>
#include <Engine/PhysicsComponents.h>
#include <Physics/Physics.h>

#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace demo {
using namespace shockwave;

namespace {

glm::vec3 aimDirection(float yaw, float pitch) { return glm::quat(glm::vec3(pitch, yaw, 0)) * glm::vec3(0, 0, -1); }

const char* kindNoun(ObjectiveKind kind) { return objectiveKindName(kind); }

std::string named(const World& world, Entity e) {
    const auto* n = world.get<Name>(e);
    return n ? "\"" + n->value + "\"" : "entity " + std::to_string(e.index);
}

// Turns colliders under (and on) an entity on or off, and shows or hides its meshes.
void setSolid(World& world, Entity e, bool solid) {
    if (auto* c = world.get<Collider>(e)) c->enabled = solid;
    if (auto* m = world.get<MeshRenderer>(e)) m->visible = solid;
    if (auto* m = world.get<ModelRenderer>(e)) m->visible = solid;
    for (const Entity child : childrenOf(world, e)) setSolid(world, child, solid);
}

}   // namespace

DemoRules::DemoRules(World& world, PhysicsWorld* physics, float tickRate)
    : m_world(world), m_physics(physics), m_tickRate(std::max(1.0f, tickRate)) {}

int DemoRules::ticksFor(float seconds) const {
    return std::max(1, static_cast<int>(std::ceil(std::max(0.0f, seconds) * m_tickRate - 1e-4f)));
}

bool DemoRules::isGameplayLevel(const World& world) {
    bool any = false;
    const_cast<World&>(world).each<const DemoPlayer>([&](Entity, const DemoPlayer&) { any = true; });
    const_cast<World&>(world).each<const CampaignProgress>([&](Entity, const CampaignProgress&) { any = true; });
    return any;
}

std::vector<std::string> DemoRules::validate(const World& cworld, const Remembered& remembered) {
    World& world = const_cast<World&>(cworld);
    std::vector<std::string> problems;
    if (!isGameplayLevel(world)) return problems;
    auto finite = [](float v) { return std::isfinite(v); };
    int players = 0, levels = 0;
    world.each<const DemoPlayer>([&](Entity e, const DemoPlayer& p) {
        ++players;
        if (!world.has<CharacterController>(e) || !world.has<Health>(e) || !world.has<Loadout>(e) ||
            !world.has<Transform>(e) || world.has<Parent>(e))
            problems.push_back(
                "the player " + named(world, e) + " needs a root Transform, CharacterController, Health and Loadout");
        if (!finite(p.speed) || p.speed <= 0 || !finite(p.sensitivity) || p.sensitivity <= 0 || !finite(p.eyeHeight))
            problems.push_back("the player's speed, sensitivity and eye height must be positive numbers");
    });
    world.each<const CampaignProgress>([&](Entity, const CampaignProgress& c) {
        ++levels;
        if (c.next.empty()) problems.push_back("CampaignProgress needs \"next\": the level its exit leads to");
    });
    if (players != 1)
        problems.push_back("a gameplay level needs exactly one DemoPlayer, not " + std::to_string(players));
    if (levels != 1)
        problems.push_back("a gameplay level needs exactly one CampaignProgress, not " + std::to_string(levels));
    world.each<const Health>([&](Entity e, const Health& h) {
        if (!finite(h.max) || h.max <= 0 || !finite(h.current))
            problems.push_back("Health on " + named(world, e) + " needs a positive max");
    });
    world.each<const Loadout>([&](Entity e, const Loadout& l) {
        if (l.magazine < 1 || l.ammo < 0 || l.ammo > l.magazine || l.reserve < 0 || !finite(l.damage) ||
            l.damage <= 0 || !finite(l.interval) || l.interval <= 0 || !finite(l.reloadSeconds) ||
            l.reloadSeconds <= 0 || !finite(l.range) || l.range <= 0)
            problems.push_back(
                "Loadout on " + named(world, e) +
                " has a bad value (ammo within its magazine, positive damage, interval, reload, range)");
    });
    std::set<std::string> ids;
    std::map<std::string, int> groups, pickupGroups;
    world.each<const Health>([&](Entity, const Health& h) {
        if (!h.group.empty()) ++groups[h.group];
    });
    world.each<const InteractionTarget>([&](Entity e, const InteractionTarget& t) {
        if (t.kind == InteractionKind::Pickup) {
            if (t.item != "ammo" && t.item != "health")
                problems.push_back(
                    "the pickup " + named(world, e) + " gives \"" + t.item + "\": the items are ammo and health");
            if (t.amount <= 0) problems.push_back("the pickup " + named(world, e) + " needs an amount above 0");
            if (!t.group.empty()) ++pickupGroups[t.group];
        } else if (t.action.empty()) {
            problems.push_back(named(world, e) + " needs the action that uses it");
        }
        if (!finite(t.radius) || t.radius <= 0) problems.push_back(named(world, e) + " needs a positive radius");
        if (!world.has<Transform>(e)) problems.push_back(named(world, e) + " needs a Transform");
    });
    world.each<const ObjectiveProgress>([&](Entity e, const ObjectiveProgress& o) {
        const std::string what = "objective \"" + o.id + "\"";
        if (o.id.empty()) problems.push_back("an objective on " + named(world, e) + " has no id");
        if (!o.id.empty() && !ids.insert(o.id).second) problems.push_back(what + " is there twice");
        if (o.text.empty()) problems.push_back(what + " has no text for the HUD");
        const bool place = o.kind == ObjectiveKind::Reach || o.kind == ObjectiveKind::Hold;
        if ((place || o.kind == ObjectiveKind::Use) &&
            (!o.target.valid() || !world.alive(o.target) || !world.has<Transform>(o.target)))
            problems.push_back(what + " (" + kindNoun(o.kind) + ") needs a target with a Transform");
        if (o.kind == ObjectiveKind::Use && o.target.valid() && world.alive(o.target)) {
            const auto* t = world.get<InteractionTarget>(o.target);
            if (!t || t->kind != InteractionKind::Use)
                problems.push_back(what + " uses " + named(world, o.target) + ", which isn't a Use InteractionTarget");
        }
        if (place && (!finite(o.radius) || o.radius <= 0)) problems.push_back(what + " needs a positive radius");
        if (o.kind == ObjectiveKind::Hold && (!finite(o.seconds) || o.seconds <= 0))
            problems.push_back(what + " (hold) needs seconds above 0");
        if (o.kind == ObjectiveKind::Destroy || o.kind == ObjectiveKind::Collect) {
            const auto& counted = o.kind == ObjectiveKind::Destroy ? groups : pickupGroups;
            const auto it = counted.find(o.group);
            const auto& gone = o.kind == ObjectiveKind::Destroy ? remembered.destroyed : remembered.collected;
            const auto was = gone.find(o.group);
            const int have = (it == counted.end() ? 0 : it->second) + (was == gone.end() ? 0 : was->second);
            if (o.group.empty())
                problems.push_back(what + " needs the group it counts");
            else if (have == 0 && !o.complete)
                problems.push_back(
                    what + " counts group \"" + o.group + "\", which nothing in the level has" +
                    (o.kind == ObjectiveKind::Destroy ? " (Health.group)" : " (a pickup's group)"));
            else if (o.required > have && !o.complete)
                problems.push_back(
                    what + " needs " + std::to_string(o.required) + " of \"" + o.group + "\" and the level has " +
                    std::to_string(have));
        }
    });
    world.each<const DemoGate>([&](Entity e, const DemoGate& g) {
        if (!ids.count(g.objective))
            problems.push_back(
                "the gate " + named(world, e) + " opens on objective \"" + g.objective +
                "\", which isn't in the level");
    });
    world.each<const DemoEnemy>([&](Entity e, const DemoEnemy& a) {
        if (!world.has<Health>(e) || !world.has<Transform>(e) || !world.has<Collider>(e))
            problems.push_back("the enemy " + named(world, e) + " needs Health, a Transform and a Collider");
        if (!finite(a.speed) || !finite(a.sight) || !finite(a.fireInterval) || a.fireInterval <= 0)
            problems.push_back("the enemy " + named(world, e) + " has a bad speed, sight or fire interval");
    });
    return problems;
}

void DemoRules::scan(const Remembered& remembered) {
    m_player = m_level = kNullEntity;
    m_world.each<DemoPlayer>([&](Entity e, DemoPlayer&) { m_player = e; });
    m_world.each<CampaignProgress>([&](Entity e, CampaignProgress&) { m_level = e; });
    // What Destroy and Collect objectives start from: everything of their group that's
    // here as the level starts, plus what an earlier visit took out for good (counted as
    // done). A save keeps the total.
    auto count = [](const std::map<std::string, int>& in, const std::string& group) {
        const auto it = in.find(group);
        return it == in.end() ? 0 : it->second;
    };
    m_world.each<ObjectiveProgress>([&](Entity, ObjectiveProgress& o) {
        if (o.complete || o.total > 0) return;
        if (o.kind == ObjectiveKind::Destroy) o.total = alive(o.group) + count(remembered.destroyed, o.group);
        if (o.kind == ObjectiveKind::Collect) {
            o.total = count(remembered.collected, o.group);
            m_world.each<const InteractionTarget>([&](Entity, const InteractionTarget& t) {
                if (t.kind == InteractionKind::Pickup && t.group == o.group && !t.used) ++o.total;
            });
        }
    });
    applyGates(nullptr);
    m_playerEye = eye();
}

CampaignProgress* DemoRules::progress() const {
    return m_level.valid() ? m_world.get<CampaignProgress>(m_level) : nullptr;
}

glm::vec3 DemoRules::positionOf(Entity e) const {
    if (!e.valid() || !m_world.alive(e) || !m_world.has<Transform>(e)) return glm::vec3(0);
    return glm::vec3(worldMatrix(m_world, e)[3]);
}

glm::vec3 DemoRules::eye() const {
    const auto* p = m_player.valid() ? m_world.get<DemoPlayer>(m_player) : nullptr;
    return positionOf(m_player) + glm::vec3(0, p ? p->eyeHeight : 1.55f, 0);
}

int DemoRules::alive(const std::string& group) const {
    int n = 0;
    m_world.each<const Health>([&](Entity, const Health& h) { n += h.group == group && h.current > 0; });
    return n;
}

std::vector<Entity> DemoRules::objectives() const {
    std::vector<std::pair<int, Entity>> list;
    m_world.each<const ObjectiveProgress>([&](Entity e, const ObjectiveProgress& o) { list.push_back({o.order, e}); });
    std::stable_sort(list.begin(), list.end(), [](const auto& a, const auto& b) {
        return a.first != b.first ? a.first < b.first : a.second.index < b.second.index;
    });
    std::vector<Entity> out;
    for (const auto& [order, e] : list) out.push_back(e);
    return out;
}

Entity DemoRules::activeObjective() const {
    for (const Entity e : objectives())
        if (!m_world.get<ObjectiveProgress>(e)->complete) return e;
    return kNullEntity;
}

bool DemoRules::allComplete() const { return !activeObjective().valid(); }

bool DemoRules::usableNow(Entity e) const {
    const auto* t = m_world.get<InteractionTarget>(e);
    if (!t || t->used || t->kind == InteractionKind::Pickup) return false;
    if (t->kind == InteractionKind::Exit) return allComplete();
    // A Use target waits for its objective's turn, when one names it.
    const Entity active = activeObjective();
    bool named = false;
    for (const Entity o : objectives()) {
        const auto* p = m_world.get<ObjectiveProgress>(o);
        if (p->kind == ObjectiveKind::Use && p->target == e) {
            named = true;
            if (o == active) return true;
        }
    }
    return !named;
}

Entity DemoRules::usable() const {
    if (!m_player.valid()) return kNullEntity;
    const glm::vec3 feet = positionOf(m_player);
    Entity best = kNullEntity;
    float bestDistance = 1e9f;
    m_world.each<const InteractionTarget>([&](Entity e, const InteractionTarget& t) {
        if (!usableNow(e)) return;
        const float d = glm::distance(positionOf(e), feet);
        if (d <= t.radius && d < bestDistance) {
            best = e;
            bestDistance = d;
        }
    });
    return best;
}

Entity DemoRules::healthOwner(Entity hit) const {
    for (int depth = 0; hit.valid() && depth < 16; ++depth) {
        if (m_world.has<Health>(hit)) return hit;
        const auto* parent = m_world.get<Parent>(hit);
        hit = parent ? parent->entity : kNullEntity;
    }
    return kNullEntity;
}

Entity DemoRules::aimedAt(float yaw, float pitch) const {
    const auto* gun = m_player.valid() ? m_world.get<Loadout>(m_player) : nullptr;
    if (!gun || !m_physics) return kNullEntity;
    const auto hit = m_physics->raycast(eye(), aimDirection(yaw, pitch), gun->range, m_player);
    return hit ? healthOwner(hit->entity) : kNullEntity;
}

void DemoRules::think(Entity self, DemoEnemy& ai, const Transform& at, const Health& health) const {
    ai.move = glm::vec3(0);
    ai.fire = false;
    if (health.current <= 0 || !m_player.valid() || !m_physics) return;
    const auto* life = m_world.get<Health>(m_player);
    if (!life || life->current <= 0) return;
    const glm::vec3 from = at.position + glm::vec3(0, ai.eyeHeight, 0);
    const glm::vec3 to = m_playerEye - from;
    const float distance = glm::length(to);
    if (distance < 1e-3f || distance > ai.sight) return;
    const glm::vec3 dir = to / distance;
    const auto hit = m_physics->raycast(from, dir, distance + 0.5f, self);
    const bool sees = hit && healthOwner(hit->entity) == m_player;
    if (!sees) return;
    ai.alerted = true;
    glm::vec3 flat(to.x, 0, to.z);
    if (glm::length(flat) > ai.stopDistance && glm::length(flat) > 1e-3f) ai.move = glm::normalize(flat) * ai.speed;
    ai.fire = ai.cooldownTicks <= 0;
}

TickEvents DemoRules::tick(const PlayerCommand& c) {
    TickEvents events;
    CampaignProgress* level = progress();
    if (!m_player.valid() || !m_world.alive(m_player) || !level) return events;
    if (level->state != CampaignProgress::Playing) {
        if (auto* cc = m_world.get<CharacterController>(m_player)) cc->desiredVelocity = glm::vec3(0);
        return events;
    }
    ++level->ticks;
    m_dead.clear();
    m_useHandled = false;
    movePlayer(c, events);
    fire(c, events);
    enemies(events);
    pickups(events);
    deaths(events);
    const auto* life = m_world.get<Health>(m_player);
    if (life && life->current <= 0) {
        level->state = CampaignProgress::Down;
        events.down = true;
        if (auto* cc = m_world.get<CharacterController>(m_player)) cc->desiredVelocity = glm::vec3(0);
    } else {
        objectivesStep(events);
        if (c.use && !m_useHandled) useExit(events);
    }
    m_playerEye = eye();
    return events;
}

void DemoRules::movePlayer(const PlayerCommand& c, TickEvents&) {
    auto& p = *m_world.get<DemoPlayer>(m_player);
    p.yaw = std::isfinite(c.yaw) ? c.yaw : p.yaw;
    p.pitch = std::isfinite(c.pitch) ? std::clamp(c.pitch, -1.45f, 1.45f) : p.pitch;
    glm::vec2 move = c.move;
    if (!std::isfinite(move.x) || !std::isfinite(move.y)) move = glm::vec2(0);
    if (glm::length(move) > 1) move = glm::normalize(move);
    const glm::vec3 forward(-std::sin(p.yaw), 0, -std::cos(p.yaw)), right(std::cos(p.yaw), 0, -std::sin(p.yaw));
    const float speed = p.speed * (c.sprint && move.y > 0.1f ? p.sprint : 1.0f);
    if (auto* cc = m_world.get<CharacterController>(m_player))
        cc->desiredVelocity = (forward * move.y + right * move.x) * speed;
}

void DemoRules::fire(const PlayerCommand& c, TickEvents& events) {
    auto* gun = m_world.get<Loadout>(m_player);
    if (!gun) return;
    if (gun->cooldownTicks > 0) --gun->cooldownTicks;
    if (gun->reloadTicks > 0 && --gun->reloadTicks == 0) {
        const int moved = std::min(gun->magazine - gun->ammo, gun->reserve);
        gun->ammo += moved;
        gun->reserve -= moved;
        events.reloaded = true;
    }
    const bool reloading = gun->reloadTicks > 0;
    auto startReload = [&] {
        if (reloading || gun->ammo >= gun->magazine || gun->reserve <= 0) return;
        gun->reloadTicks = ticksFor(gun->reloadSeconds);
        events.reloadStarted = true;
    };
    if (c.reload) startReload();
    if (!c.fire || reloading || gun->cooldownTicks > 0) return;
    if (gun->ammo <= 0) {
        // An empty magazine reloads by itself when there's a reserve, else clicks.
        if (gun->reserve > 0)
            startReload();
        else
            events.dryFire = true;
        gun->cooldownTicks = ticksFor(gun->interval);
        return;
    }
    --gun->ammo;
    ++gun->shots;
    ++events.shots;
    gun->cooldownTicks = ticksFor(gun->interval);
    const auto& p = *m_world.get<DemoPlayer>(m_player);
    const auto hit =
        m_physics ? m_physics->raycast(eye(), aimDirection(p.yaw, p.pitch), gun->range, m_player) : std::nullopt;
    if (!hit) return;
    const Entity owner = healthOwner(hit->entity);
    if (!owner.valid() || owner == m_player) return;
    auto& health = *m_world.get<Health>(owner);
    if (health.current <= 0) return;
    health.current = std::max(0.0f, health.current - gun->damage);
    health.hurtTicks = 0;
    ++gun->hits;
    events.hit = true;
    if (health.current <= 0) {
        events.kill = true;
        if (m_world.has<DemoEnemy>(owner))
            if (CampaignProgress* level = progress()) ++level->kills;
    }
}

void DemoRules::enemies(TickEvents& events) {
    const float dt = 1.0f / m_tickRate;
    auto* life = m_world.get<Health>(m_player);
    m_world.each<DemoEnemy, Transform, Health>([&](Entity, DemoEnemy& ai, Transform& at, Health& health) {
        if (ai.cooldownTicks > 0) --ai.cooldownTicks;
        if (health.current <= 0) return;
        if (glm::length(ai.move) > 0) {
            at.position += ai.move * dt;
            at.rotation = glm::quat(glm::vec3(0, std::atan2(-ai.move.x, -ai.move.z), 0));
        }
        if (ai.fire && ai.cooldownTicks <= 0 && life && life->current > 0) {
            life->current = std::max(0.0f, life->current - ai.damage);
            life->hurtTicks = 0;
            events.hurt = true;
            ai.cooldownTicks = ticksFor(ai.fireInterval);
        }
        ai.fire = false;
        ai.move = glm::vec3(0);
    });
}

void DemoRules::pickups(TickEvents& events) {
    const glm::vec3 feet = positionOf(m_player);
    auto* life = m_world.get<Health>(m_player);
    auto* gun = m_world.get<Loadout>(m_player);
    m_world.each<InteractionTarget>([&](Entity e, InteractionTarget& t) {
        if (t.kind != InteractionKind::Pickup || t.used) return;
        if (glm::distance(positionOf(e), feet) > t.radius) return;
        // A full player leaves it there for later.
        if (t.item == "health") {
            if (!life || life->current >= life->max) return;
            life->current = std::min(life->max, life->current + float(t.amount));
        } else if (t.item == "ammo") {
            if (!gun || gun->reserve >= gun->maxReserve) return;
            gun->reserve = std::min(gun->maxReserve, gun->reserve + t.amount);
        } else {
            return;
        }
        t.used = true;
        events.pickups.push_back(t.item);
        m_dead.push_back(e);
    });
}

void DemoRules::deaths(TickEvents& events) {
    m_world.each<const Health>([&](Entity e, const Health& h) {
        if (h.current <= 0 && e != m_player) m_dead.push_back(e);
    });
    for (const Entity e : m_dead) {
        if (!m_world.alive(e)) continue;
        if (const auto* id = m_world.get<PersistentIdentity>(e); id && !id->id.empty()) {
            if (const auto* h = m_world.get<Health>(e); h && !h->group.empty())
                events.killed.push_back({id->id, h->group});
            else if (const auto* t = m_world.get<InteractionTarget>(e);
                     t && t->kind == InteractionKind::Pickup && !t->group.empty())
                events.collected.push_back({id->id, t->group});
        }
        destroyWithChildren(m_world, e);
    }
    m_dead.clear();
}

void DemoRules::objectivesStep(TickEvents& events) {
    const Entity active = activeObjective();
    const glm::vec3 feet = positionOf(m_player);
    for (const Entity e : objectives()) {
        auto& o = *m_world.get<ObjectiveProgress>(e);
        if (o.complete) continue;
        bool done = false;
        switch (o.kind) {
        case ObjectiveKind::Destroy:
        case ObjectiveKind::Collect: {
            int left = 0;
            if (o.kind == ObjectiveKind::Destroy) {
                left = alive(o.group);
            } else {
                m_world.each<const InteractionTarget>([&](Entity, const InteractionTarget& t) {
                    left += t.kind == InteractionKind::Pickup && t.group == o.group && !t.used;
                });
            }
            o.count = std::max(0, o.total - left);
            const int needed = o.required > 0 ? std::min(o.required, o.total) : o.total;
            done = o.count >= needed && e == active;
            break;
        }
        case ObjectiveKind::Reach: done = e == active && glm::distance(positionOf(o.target), feet) <= o.radius; break;
        case ObjectiveKind::Hold:
            if (e == active && glm::distance(positionOf(o.target), feet) <= o.radius) ++o.ticks;
            done = o.ticks >= ticksFor(o.seconds);
            break;
        case ObjectiveKind::Use: {
            const auto* t = m_world.get<InteractionTarget>(o.target);
            done = t && t->used;
            break;
        }
        }
        if (done) {
            o.complete = true;
            events.completed.push_back(o.id);
            break;   // one objective a tick: the next starts on the next tick
        }
    }
    applyGates(&events);
}

void DemoRules::applyGates(TickEvents* events) {
    std::set<std::string> complete;
    m_world.each<const ObjectiveProgress>([&](Entity, const ObjectiveProgress& o) {
        if (o.complete) complete.insert(o.id);
    });
    m_world.each<DemoGate>([&](Entity e, DemoGate& g) {
        const bool open = complete.count(g.objective) > 0;
        if (open && !g.open && events) events->opened.push_back(g.objective);
        // Children are looked up only when it changes, and on a scan (after a load).
        if (open != g.open || !events) setSolid(m_world, e, !open);
        g.open = open;
    });
}

void DemoRules::useExit(TickEvents& events) {
    const Entity e = usable();
    if (!e.valid()) return;
    auto& t = *m_world.get<InteractionTarget>(e);
    m_useHandled = true;
    if (t.kind == InteractionKind::Use) {
        t.used = true;
        // Its objective completes on the next tick's step, in order.
        return;
    }
    if (t.kind == InteractionKind::Exit && allComplete()) {
        t.used = true;
        progress()->state = CampaignProgress::Won;
        events.won = true;
    }
}

}   // namespace demo
