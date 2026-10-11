// NavigationWorld: a Play session's navigation mesh and its crowd of NavAgents.
#include <Navigation/Navigation.h>

#include "NavMeshImpl.h"

#include <Core/Jobs.h>
#include <Core/Log.h>
#include <Core/Profile.h>
#include <Engine/Components.h>
#include <Engine/Hierarchy.h>

#include <DetourCommon.h>
#include <DetourCrowd.h>
#include <DetourNavMesh.h>
#include <DetourNavMeshQuery.h>

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <format>
#include <unordered_map>

namespace shockwave {

namespace {

uint64_t key(Entity e) { return (static_cast<uint64_t>(e.generation) << 32) | e.index; }

float flatDistance(const glm::vec3& a, const glm::vec3& b) { return glm::length(glm::vec2(a.x - b.x, a.z - b.z)); }

// The bits of a NavAgent that DetourCrowd keeps a copy of.
dtCrowdAgentParams crowdParams(const NavAgent& a) {
    dtCrowdAgentParams p{};
    p.radius = std::max(0.05f, a.radius);
    p.height = std::max(0.1f, a.height);
    p.maxAcceleration = std::max(0.0f, a.acceleration);
    p.maxSpeed = std::max(0.0f, a.speed);
    p.collisionQueryRange = p.radius * 12.0f;
    p.pathOptimizationRange = p.radius * 30.0f;
    p.separationWeight = std::max(0.0f, a.separation);
    p.updateFlags = DT_CROWD_ANTICIPATE_TURNS | DT_CROWD_OPTIMIZE_VIS | DT_CROWD_OPTIMIZE_TOPO;
    if (a.avoidance != NavAgent::Avoidance::None) p.updateFlags |= DT_CROWD_OBSTACLE_AVOIDANCE;
    if (a.separation > 0.0f) p.updateFlags |= DT_CROWD_SEPARATION;
    // Avoidance presets 0..2 are set up in the constructor (Low, Medium, High).
    p.obstacleAvoidanceType = static_cast<unsigned char>(
        a.avoidance == NavAgent::Avoidance::High     ? 2
        : a.avoidance == NavAgent::Avoidance::Medium ? 1
                                                     : 0);
    p.queryFilterType = 0;
    return p;
}

bool sameParams(const dtCrowdAgentParams& a, const dtCrowdAgentParams& b) {
    return a.radius == b.radius && a.height == b.height && a.maxAcceleration == b.maxAcceleration &&
           a.maxSpeed == b.maxSpeed && a.separationWeight == b.separationWeight && a.updateFlags == b.updateFlags &&
           a.obstacleAvoidanceType == b.obstacleAvoidanceType;
}

}   // namespace

struct NavigationWorld::Impl {
    World& world;
    JobSystem& jobs;
    const Assets* assets;
    std::shared_ptr<const NavMesh> mesh;
    bool baked = false;
    NavMeshSettings settings;
    dtCrowd* crowd = nullptr;
    struct Agent {
        Entity entity;
        int index = -1;
        dtCrowdAgentParams params{};
        glm::vec3 requested{0.0f};   // the goal last sent to the crowd
        bool moving = false;   // a move target is set
        bool arrived = false;
        glm::vec3 written{0.0f};   // where we last put the Transform (world space)
        bool reportedOff = false, reportedGoal = false;
        bool seen = false;
    };
    std::unordered_map<uint64_t, Agent> agents;
    std::vector<NavDiagnostic> diagnostics;
    bool checkedOnce = false;
    bool reportedFull = false;
    std::array<double, 120> stepTimes{};
    size_t stepCount = 0;
    double lastStepMs = 0.0;

    Impl(World& w, JobSystem& j, const Assets* a) : world(w), jobs(j), assets(a) {}
    ~Impl() { dtFreeCrowd(crowd); }

    void report(Entity e, std::string message) {
        SW_LOG(LogNavigation, Warning, "{}", message);
        diagnostics.push_back({e, std::move(message)});
    }

    glm::vec3 worldPosition(Entity e) const { return glm::vec3(worldMatrix(world, e)[3]); }

    // Writes a world-space position (and facing) back into e's Transform, through its parent.
    void place(Entity e, Transform& t, const glm::vec3& position, const glm::vec3* facing, float dt) {
        const Entity parent = parentOf(world, e);
        glm::quat worldRotation = t.rotation;
        glm::quat parentRotation(1.0f, 0.0f, 0.0f, 0.0f);
        if (parent.valid()) {
            const glm::mat4 parentWorld = worldMatrix(world, parent);
            t.position = glm::vec3(glm::inverse(parentWorld) * glm::vec4(position, 1.0f));
            parentRotation = Transform::fromMatrix(parentWorld).rotation;
            worldRotation = parentRotation * t.rotation;
        } else {
            t.position = position;
        }
        if (facing) {
            // Forward is -Z, as for cameras and lights.
            const glm::quat want = glm::angleAxis(std::atan2(-facing->x, -facing->z), glm::vec3(0, 1, 0));
            const float blend = 1.0f - std::exp(-12.0f * dt);
            worldRotation = glm::normalize(glm::slerp(worldRotation, want, blend));
            t.rotation = glm::normalize(glm::inverse(parentRotation) * worldRotation);
        }
    }

    bool goalOf(const NavAgent& a, glm::vec3& goal) const {
        if (a.target.valid() && world.alive(a.target) && world.has<Transform>(a.target)) {
            goal = worldPosition(a.target);
            return true;
        }
        if (a.hasDestination) {
            goal = a.destination;
            return true;
        }
        return false;
    }

    void setupCrowd() {
        float maxRadius = std::max(settings.agentRadius, 0.5f);
        world.each<const NavAgent>([&](const NavAgent& a) { maxRadius = std::max(maxRadius, a.radius); });
        crowd = dtAllocCrowd();
        if (!crowd || !crowd->init(std::max(1, settings.maxAgents), maxRadius, mesh->detour())) {
            report(kNullEntity, "couldn't start the crowd for " + std::to_string(settings.maxAgents) + " agents");
            dtFreeCrowd(crowd);
            crowd = nullptr;
            return;
        }
        // Low, Medium and High avoidance: more velocity samples find gaps better and cost more.
        dtObstacleAvoidanceParams params = *crowd->getObstacleAvoidanceParams(0);
        params.velBias = 0.5f;
        params.adaptiveDivs = 5, params.adaptiveRings = 2, params.adaptiveDepth = 1;
        crowd->setObstacleAvoidanceParams(0, &params);
        params.adaptiveDivs = 5, params.adaptiveRings = 2, params.adaptiveDepth = 2;
        crowd->setObstacleAvoidanceParams(1, &params);
        params.adaptiveDivs = 7, params.adaptiveRings = 3, params.adaptiveDepth = 3;
        crowd->setObstacleAvoidanceParams(2, &params);
    }

    void removeAgent(Agent& agent) {
        if (crowd && agent.index >= 0) crowd->removeAgent(agent.index);
        agent.index = -1;
    }

    void synchronize() {
        if (!crowd) {
            if (!checkedOnce) checkScene();
            return;
        }
        for (auto& [k, agent] : agents) agent.seen = false;
        world.each<NavAgent>([&](Entity e, NavAgent& a) {
            if (!a.enabled || !world.has<Transform>(e)) return;
            auto [it, added] = agents.try_emplace(key(e));
            Agent& agent = it->second;
            agent.seen = true;
            agent.entity = e;
            const dtCrowdAgentParams params = crowdParams(a);
            if (added || agent.index < 0) {
                const glm::vec3 position = worldPosition(e);
                agent.index = crowd->addAgent(&position.x, &params);
                agent.params = params;
                agent.written = position;
                agent.moving = false;
                if (agent.index < 0) {
                    if (!reportedFull)
                        report(
                            e, "more NavAgents than NavMeshSettings max_agents (" + std::to_string(settings.maxAgents) +
                                   "); the rest stand still");
                    reportedFull = true;
                    return;
                }
            } else if (!sameParams(params, agent.params)) {
                crowd->updateAgentParameters(agent.index, &params);
                agent.params = params;
            }
            const dtCrowdAgent* ca = crowd->getAgent(agent.index);
            a.onNavMesh = ca && ca->state != DT_CROWDAGENT_STATE_INVALID;
        });
        for (auto it = agents.begin(); it != agents.end();) {
            if (!it->second.seen) {
                removeAgent(it->second);
                it = agents.erase(it);
            } else
                ++it;
        }
        if (!checkedOnce) checkScene();
    }

    // The problems worth telling someone about once, when Play starts: agents that can't
    // move, goals they can't reach.
    void checkScene() {
        checkedOnce = true;
        world.each<const NavAgent>([&](Entity e, const NavAgent& a) {
            if (!a.enabled) return;
            if (!world.has<Transform>(e)) {
                report(e, "NavAgent has no Transform, so it can't be placed");
                return;
            }
            if (!mesh || mesh->empty()) return;
            if (a.radius > settings.agentRadius + 1e-3f)
                report(
                    e, std::format(
                           "NavAgent radius {:.2f} is wider than NavMeshSettings agent_radius {:.2f}, so it "
                           "will brush through walls",
                           a.radius, settings.agentRadius));
            const glm::vec3 position = worldPosition(e);
            const glm::vec3 extents(
                std::max(a.radius * 2.0f, 0.5f), std::max(a.height, 1.0f), std::max(a.radius * 2.0f, 0.5f));
            if (!mesh->nearestPoint(position, extents)) {
                report(e, "NavAgent stands off the navigation mesh, so it can't move. Place it on walkable ground");
                return;
            }
            glm::vec3 goal;
            if (!goalOf(a, goal)) return;
            if (!mesh->nearestPoint(goal, {2.0f, 4.0f, 2.0f}))
                report(e, "NavAgent's goal is off the navigation mesh; it will walk as near as it can");
            else if (!mesh->reachable(position, goal))
                report(e, "NavAgent can't reach its goal from where it stands; it will walk as near as it can");
        });
        if (!mesh && sceneUsesNavigation(world) && diagnostics.empty())
            report(kNullEntity, "no navigation mesh: the bake failed");
    }

    void step(float dt) {
        if (!crowd) return;
        SW_PROFILE_SCOPE("Navigation step");
        const auto started = std::chrono::steady_clock::now();
        synchronize();
        NavQueryLease query(mesh->impl());
        const dtQueryFilter* filter = crowd->getFilter(0);
        const float goalExtents[3] = {2.0f, 4.0f, 2.0f};
        for (auto& [k, agent] : agents) {
            if (agent.index < 0) continue;
            NavAgent& a = *world.get<NavAgent>(agent.entity);
            dtCrowdAgent* ca = crowd->getEditableAgent(agent.index);
            const glm::vec3 position = worldPosition(agent.entity);
            const bool physical =
                world.has<CharacterController>(agent.entity) && world.get<CharacterController>(agent.entity)->enabled;
            // Physics or game code moved it since the last tick: carry the crowd agent along.
            if (ca->state == DT_CROWDAGENT_STATE_INVALID || flatDistance(position, agent.written) > 2.0f) {
                crowd->removeAgent(agent.index);
                agent.index = crowd->addAgent(&position.x, &agent.params);
                agent.moving = false;
                if (agent.index < 0) continue;
                ca = crowd->getEditableAgent(agent.index);
            } else if (
                ca->state == DT_CROWDAGENT_STATE_WALKING &&
                (physical || glm::length(position - agent.written) > 1e-3f) && query) {
                ca->corridor.movePosition(&position.x, query.get(), filter);
                dtVcopy(ca->npos, ca->corridor.getPos());
            }
            a.onNavMesh = ca->state != DT_CROWDAGENT_STATE_INVALID;
            glm::vec3 goal;
            if (!goalOf(a, goal) || !a.onNavMesh) {
                if (agent.moving) crowd->resetMoveTarget(agent.index);
                agent.moving = false;
                agent.arrived = false;
                a.remainingDistance = 0.0f;
                continue;
            }
            const float distance = flatDistance(position, goal);
            a.remainingDistance = glm::length(goal - position);
            const float resume = a.stoppingDistance + std::max(0.25f, a.radius * 0.5f);
            if (distance <= a.stoppingDistance || (agent.arrived && distance <= resume)) {
                // Close enough: stop here rather than shuffle onto the exact point.
                if (agent.moving || !agent.arrived) {
                    const float zero[3] = {0.0f, 0.0f, 0.0f};
                    crowd->requestMoveVelocity(agent.index, zero);
                }
                agent.moving = false;
                agent.arrived = true;
                continue;
            }
            agent.arrived = false;
            // Ask for a new path when the goal has moved far enough to matter.
            const float repath = std::max(0.5f, a.stoppingDistance);
            if (agent.moving && glm::length(goal - agent.requested) < repath) continue;
            dtPolyRef ref = 0;
            float nearest[3];
            if (query) query->findNearestPoly(&goal.x, goalExtents, filter, &ref, nearest);
            if (!ref) continue;   // goal off the mesh: keep walking to the last one
            crowd->requestMoveTarget(agent.index, ref, nearest);
            agent.requested = goal;
            agent.moving = true;
        }

        crowd->update(dt, nullptr);

        for (auto& [k, agent] : agents) {
            if (agent.index < 0) continue;
            const dtCrowdAgent* ca = crowd->getAgent(agent.index);
            NavAgent& a = *world.get<NavAgent>(agent.entity);
            Transform& t = *world.get<Transform>(agent.entity);
            a.velocity = {ca->vel[0], ca->vel[1], ca->vel[2]};
            a.arrived = agent.arrived;
            if (ca->state != DT_CROWDAGENT_STATE_WALKING) continue;
            if (auto* character = world.get<CharacterController>(agent.entity); character && character->enabled) {
                character->desiredVelocity = {ca->vel[0], 0.0f, ca->vel[2]};
                agent.written = worldPosition(agent.entity);
                continue;
            }
            const glm::vec3 position(ca->npos[0], ca->npos[1], ca->npos[2]);
            const glm::vec3 flat(ca->vel[0], 0.0f, ca->vel[2]);
            const bool turn = a.faceMovement && glm::length(flat) > 0.1f;
            place(agent.entity, t, position, turn ? &flat : nullptr, dt);
            agent.written = position;
        }
        lastStepMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        stepTimes[stepCount++ % stepTimes.size()] = lastStepMs;
    }
};

NavigationWorld::NavigationWorld(World& world, JobSystem& jobs, const Assets* assets)
    : m_impl(std::make_unique<Impl>(world, jobs, assets)) {
    Impl& m = *m_impl;
    if (!sceneUsesNavigation(world)) return;
    m.settings = findNavMeshSettings(world);
    int settingsCount = 0;
    world.each<const NavMeshSettings>([&](Entity e, const NavMeshSettings&) {
        if (++settingsCount == 2) m.report(e, "more than one NavMeshSettings; only the first is used");
    });
    for (const std::string& problem : validateNavMeshSettings(m.settings)) m.report(kNullEntity, problem);
    const uint64_t hash = navInputHash(world, assets, m.settings);
    m.mesh = navcache::find(hash);
    if (!m.mesh) {
        std::string error;
        const NavGeometry geometry = collectNavGeometry(world, assets, m.settings);
        m.mesh = bakeNavMesh(geometry, m.settings, jobs, error);
        if (!m.mesh) {
            m.report(kNullEntity, "navigation mesh bake failed: " + error);
            return;
        }
        m.baked = true;
        const NavMeshStats& s = m.mesh->stats();
        SW_LOG(
            LogNavigation, Info, "baked the navigation mesh: {} tiles, {} polygons from {} triangles in {:.1f} ms",
            s.tiles, s.polygons, s.inputTriangles, s.bakeMs);
    }
    if (m.mesh->empty()) {
        m.report(
            kNullEntity, "the navigation mesh has no walkable polygons; check agent_radius, max_slope and "
                         "that the ground is static");
        return;
    }
    m.setupCrowd();
}

NavigationWorld::~NavigationWorld() = default;

void NavigationWorld::synchronize() { m_impl->synchronize(); }
void NavigationWorld::step(double dt) { m_impl->step(static_cast<float>(dt)); }

bool NavigationWorld::setDestination(Entity agent, const glm::vec3& destination) {
    NavAgent* a = m_impl->world.get<NavAgent>(agent);
    if (!a) return false;
    a->target = kNullEntity;
    a->destination = destination;
    a->hasDestination = true;
    return true;
}

bool NavigationWorld::setTarget(Entity agent, Entity target) {
    NavAgent* a = m_impl->world.get<NavAgent>(agent);
    if (!a) return false;
    a->target = target;
    return true;
}

bool NavigationWorld::stop(Entity agent) {
    NavAgent* a = m_impl->world.get<NavAgent>(agent);
    if (!a) return false;
    a->target = kNullEntity;
    a->hasDestination = false;
    return true;
}

bool NavigationWorld::findPath(
    const glm::vec3& start, const glm::vec3& end, std::vector<glm::vec3>& points, bool* partial) const {
    if (!m_impl->mesh) {
        points.clear();
        return false;
    }
    return m_impl->mesh->findPath(start, end, points, partial);
}

std::optional<glm::vec3> NavigationWorld::nearestPoint(const glm::vec3& p, const glm::vec3& extents) const {
    return m_impl->mesh ? m_impl->mesh->nearestPoint(p, extents) : std::nullopt;
}

std::optional<glm::vec3> NavigationWorld::raycast(const glm::vec3& start, const glm::vec3& end) const {
    return m_impl->mesh ? m_impl->mesh->raycast(start, end) : std::nullopt;
}

bool NavigationWorld::reachable(const glm::vec3& start, const glm::vec3& end) const {
    return m_impl->mesh && m_impl->mesh->reachable(start, end);
}

const NavMesh* NavigationWorld::navMesh() const { return m_impl->mesh.get(); }
std::shared_ptr<const NavMesh> NavigationWorld::sharedNavMesh() const { return m_impl->mesh; }
bool NavigationWorld::baked() const { return m_impl->baked; }
const std::vector<NavDiagnostic>& NavigationWorld::diagnostics() const { return m_impl->diagnostics; }

uint32_t NavigationWorld::agentCount() const {
    uint32_t count = 0;
    for (const auto& [k, agent] : m_impl->agents) count += agent.index >= 0;
    return count;
}

double NavigationWorld::lastStepMs() const { return m_impl->lastStepMs; }

double NavigationWorld::averageStepMs() const {
    const size_t n = std::min(m_impl->stepCount, m_impl->stepTimes.size());
    if (n == 0) return 0.0;
    double sum = 0.0;
    for (size_t i = 0; i < n; ++i) sum += m_impl->stepTimes[i];
    return sum / static_cast<double>(n);
}

bool NavigationWorld::agentPath(Entity entity, std::vector<glm::vec3>& points) const {
    points.clear();
    const Impl& m = *m_impl;
    if (!m.crowd) return false;
    const auto found = m.agents.find(key(entity));
    if (found == m.agents.end() || found->second.index < 0 || !found->second.moving) return false;
    const dtCrowdAgent* ca = m.crowd->getAgent(found->second.index);
    if (!ca || ca->state != DT_CROWDAGENT_STATE_WALKING || ca->corridor.getPathCount() == 0) return false;
    NavQueryLease query(m.mesh->impl());
    if (!query) return false;
    constexpr int kMax = 128;
    float straight[kMax * 3];
    int count = 0;
    query->findStraightPath(
        ca->npos, ca->corridor.getTarget(), ca->corridor.getPath(), ca->corridor.getPathCount(), straight, nullptr,
        nullptr, &count, kMax);
    for (int i = 0; i < count; ++i) points.emplace_back(straight[i * 3], straight[i * 3 + 1], straight[i * 3 + 2]);
    return points.size() >= 2;
}

}   // namespace shockwave
