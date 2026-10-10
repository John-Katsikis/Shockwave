#include <Physics/Physics.h>

#include <Assets/Assets.h>
#include <Engine/Components.h>
#include <Engine/Hierarchy.h>

#include <Jolt/Jolt.h>

#include <glm/gtc/matrix_transform.hpp>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemWithBarrier.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <functional>
#include <map>
#include <mutex>
#include <stdexcept>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace shockwave {
namespace {
uint64_t key(Entity e) { return uint64_t(e.generation) << 32 | e.index; }
Entity entity(uint64_t k) { return {uint32_t(k), uint32_t(k >> 32)}; }
JPH::Vec3 vec(const glm::vec3& v) { return {v.x, v.y, v.z}; }
glm::vec3 vec(JPH::Vec3Arg v) { return {v.GetX(), v.GetY(), v.GetZ()}; }
JPH::Quat quat(const glm::quat& q) { return {q.x, q.y, q.z, q.w}; }
glm::quat quat(JPH::QuatArg q) { return {q.GetW(), q.GetX(), q.GetY(), q.GetZ()}; }
bool finite(const glm::vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
bool near(const glm::vec3& a, const glm::vec3& b) { return glm::length(a - b) < 1e-5f; }
bool near(const glm::quat& a, const glm::quat& b) { return std::abs(glm::dot(a, b)) > 1.0f - 1e-6f; }
void trace(const char* format, ...) {
    va_list args;
    va_start(args, format);
    std::fputs("Jolt: ", stderr);
    std::vfprintf(stderr, format, args);
    std::fputc('\n', stderr);
    va_end(args);
}
struct Runtime {
    Runtime() {
        JPH::RegisterDefaultAllocator();
        JPH::Trace = trace;
#ifdef JPH_ENABLE_ASSERTS
        JPH::AssertFailed = [](const char* expression, const char* message, const char* file, JPH::uint line) {
            std::fprintf(stderr, "Jolt assertion: %s (%s) at %s:%u\n", expression, message ? message : "", file, line);
            return true;
        };
#endif
        JPH::Factory::sInstance = new JPH::Factory();
        JPH::RegisterTypes();
    }
    ~Runtime() {
        JPH::UnregisterTypes();
        delete JPH::Factory::sInstance;
        JPH::Factory::sInstance = nullptr;
    }
};
Runtime& runtime() {
    static Runtime instance;
    return instance;
}
// Jolt uses Shockwave's existing workers, not an additional thread pool. A Jolt
// barrier can finish before queued Execute wrappers release their references;
// drain those wrappers before destroying physics state or publishing ECS changes.
class Jobs final : public JPH::JobSystemWithBarrier {
public:
    explicit Jobs(shockwave::JobSystem& jobs) : JPH::JobSystemWithBarrier(8), m_jobs(jobs) {}
    ~Jobs() override { drain(); }
    int GetMaxConcurrency() const override { return int(m_jobs.threadCount()); }
    JobHandle
    CreateJob(const char* name, JPH::ColorArg color, const JobFunction& fn, JPH::uint32 dependencies) override {
        auto* job = new Job(name, color, this, fn, dependencies);
        JobHandle handle(job);
        if (!dependencies) QueueJob(job);
        return handle;
    }
    void drain() { m_jobs.wait(m_pending); }

protected:
    void QueueJob(Job* job) override {
        job->AddRef();
        m_jobs.run(
            [job] {
                job->Execute();
                job->Release();
            },
            &m_pending);
    }
    void QueueJobs(Job** jobs, JPH::uint count) override {
        for (JPH::uint i = 0; i < count; ++i) QueueJob(jobs[i]);
    }
    void FreeJob(Job* job) override { delete job; }

private:
    shockwave::JobSystem& m_jobs;
    JobCounter m_pending;
};
class Layers final : public JPH::BroadPhaseLayerInterface {
public:
    JPH::uint GetNumBroadPhaseLayers() const override { return 2; }
    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override {
        return JPH::BroadPhaseLayer(JPH::uint8(layer));
    }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override {
        return layer.GetValue() == 0 ? "Static" : "Moving";
    }
#endif
};
class BroadFilter final : public JPH::ObjectVsBroadPhaseLayerFilter {
    bool ShouldCollide(JPH::ObjectLayer object, JPH::BroadPhaseLayer broad) const override {
        return object != 0 || broad.GetValue() != 0;
    }
};
class PairFilter final : public JPH::ObjectLayerPairFilter {
    bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override { return a != 0 || b != 0; }
};
struct Pose {
    glm::vec3 position{0}, scale{1};
    glm::quat rotation{1, 0, 0, 0};
};
Pose pose(const glm::mat4& matrix) {
    Pose p;
    p.position = glm::vec3(matrix[3]);
    glm::mat3 rotation;
    for (int i = 0; i < 3; ++i) {
        p.scale[i] = glm::length(glm::vec3(matrix[i]));
        if (!std::isfinite(p.scale[i]) || p.scale[i] < .0001f)
            throw std::runtime_error("Physics requires finite, nonzero transform scales");
        rotation[i] = glm::vec3(matrix[i]) / p.scale[i];
    }
    if (!finite(p.position) || std::abs(glm::dot(rotation[0], rotation[1])) > .0001f ||
        std::abs(glm::dot(rotation[0], rotation[2])) > .0001f || std::abs(glm::dot(rotation[1], rotation[2])) > .0001f)
        throw std::runtime_error("Physics transforms cannot contain shear; use uniform parent scale");
    if (glm::determinant(rotation) < 0) {
        p.scale.x = -p.scale.x;
        rotation[0] = -rotation[0];
    }
    p.rotation = glm::normalize(glm::quat_cast(rotation));
    return p;
}
bool uniform(const glm::vec3& scale) {
    const auto s = glm::abs(scale);
    return std::abs(s.x - s.y) < .0001f && std::abs(s.x - s.z) < .0001f;
}
void filterValid(int layer, int mask) {
    if (layer < 0 || layer > 15 || mask < 0 || mask > 65535)
        throw std::runtime_error("Collision layer must be 0..15 and mask 0..65535");
}
bool rigidConfig(const RigidBody& a, const RigidBody& b) {
    return std::tie(
               a.motion, a.mass, a.friction, a.restitution, a.linearDamping, a.angularDamping, a.gravityFactor,
               a.collisionLayer, a.collisionMask, a.sensor, a.continuous, a.enabled) ==
           std::tie(
               b.motion, b.mass, b.friction, b.restitution, b.linearDamping, b.angularDamping, b.gravityFactor,
               b.collisionLayer, b.collisionMask, b.sensor, b.continuous, b.enabled);
}
bool characterConfig(const CharacterController& a, const CharacterController& b) {
    return std::tie(
               a.radius, a.halfHeight, a.mass, a.maxSlope, a.stepHeight, a.jumpSpeed, a.maxPushForce, a.collisionLayer,
               a.collisionMask, a.enabled) ==
           std::tie(
               b.radius, b.halfHeight, b.mass, b.maxSlope, b.stepHeight, b.jumpSpeed, b.maxPushForce, b.collisionLayer,
               b.collisionMask, b.enabled);
}
JPH::RefConst<JPH::Shape> checked(const JPH::ShapeSettings::ShapeResult& result) {
    if (result.HasError()) throw std::runtime_error(result.GetError().c_str());
    return result.Get();
}
}   // namespace
struct PhysicsWorld::Impl {
    World& world;
    const Assets* assets;
    Jobs jobs;
    JPH::TempAllocatorImplWithMallocFallback temporary{16 * 1024 * 1024};
    Layers layers;
    BroadFilter broadFilter;
    PairFilter pairFilter;
    JPH::PhysicsSystem system;
    struct Entry {
        Entity entity;
        JPH::BodyID body;
        JPH::Ref<JPH::CharacterVirtual> character;
        bool isCharacter = false, invalidPlacement = false;
        Collider collider;
        RigidBody rigid;
        CharacterController controller;
        Pose placed;
        std::shared_ptr<const ModelAsset> model;
        std::string error;
    };
    std::unordered_map<uint64_t, Entry> entries;
    std::vector<PhysicsDiagnostic> problems;
    std::vector<ContactEvent> events;
    using TriggerKey = std::pair<uint64_t, uint64_t>;
    std::map<TriggerKey, ContactEvent> triggers;
    struct Listener final : public JPH::ContactListener {
        Impl& owner;
        std::mutex mutex;
        using Key = std::array<uint32_t, 4>;
        std::map<Key, ContactEvent> active;
        std::vector<ContactEvent> pending;
        explicit Listener(Impl& impl) : owner(impl) {}
        static Key key(const JPH::SubShapeIDPair& pair) {
            return {
                pair.GetBody1ID().GetIndexAndSequenceNumber(), pair.GetSubShapeID1().GetValue(),
                pair.GetBody2ID().GetIndexAndSequenceNumber(), pair.GetSubShapeID2().GetValue()};
        }
        JPH::ValidateResult OnContactValidate(
            const JPH::Body& a, const JPH::Body& b, JPH::RVec3Arg, const JPH::CollideShapeResult&) override {
            return owner.allowed(a.GetUserData(), b.GetUserData())
                       ? JPH::ValidateResult::AcceptAllContactsForThisBodyPair
                       : JPH::ValidateResult::RejectAllContactsForThisBodyPair;
        }
        void
        record(ContactEvent::Type type, const JPH::Body& a, const JPH::Body& b, const JPH::ContactManifold& manifold) {
            if (a.IsSensor() || b.IsSensor()) return;   // sleeping-safe overlap events below
            ContactEvent event{
                type,
                entity(a.GetUserData()),
                entity(b.GetUserData()),
                vec(manifold.GetWorldSpaceContactPointOn1(0)),
                vec(manifold.mWorldSpaceNormal),
                false};
            Key k{
                a.GetID().GetIndexAndSequenceNumber(), manifold.mSubShapeID1.GetValue(),
                b.GetID().GetIndexAndSequenceNumber(), manifold.mSubShapeID2.GetValue()};
            std::lock_guard lock(mutex);
            active[k] = event;
            pending.push_back(event);
        }
        void OnContactAdded(
            const JPH::Body& a, const JPH::Body& b, const JPH::ContactManifold& m, JPH::ContactSettings&) override {
            record(ContactEvent::Type::Begin, a, b, m);
        }
        void OnContactPersisted(
            const JPH::Body& a, const JPH::Body& b, const JPH::ContactManifold& m, JPH::ContactSettings&) override {
            record(ContactEvent::Type::Stay, a, b, m);
        }
        void OnContactRemoved(const JPH::SubShapeIDPair& pair) override {
            std::lock_guard lock(mutex);
            auto it = active.find(key(pair));
            if (it == active.end()) return;
            auto event = it->second;
            event.type = ContactEvent::Type::End;
            pending.push_back(event);
            active.erase(it);
        }
        void publish(std::vector<ContactEvent>& result) {
            std::lock_guard lock(mutex);
            result.swap(pending);
            pending.clear();
        }
    } listener;
    Impl(World& w, JobSystem& workers, const Assets* a, PhysicsConfig config)
        : world(w), assets(a), jobs(workers), listener(*this) {
        if (!config.maxBodies || !config.maxBodyPairs || !config.maxContactConstraints)
            throw std::invalid_argument("Physics capacity must be positive");
        system.Init(
            config.maxBodies, 0, config.maxBodyPairs, config.maxContactConstraints, layers, broadFilter, pairFilter);
        system.SetContactListener(&listener);
    }
    ~Impl() {
        jobs.drain();
        system.SetContactListener(nullptr);
        for (auto& [k, e] : entries) {
            (void)k;
            remove(e);
        }
    }
    void remove(Entry& e) {
        if (e.character)
            e.character = nullptr;
        else if (!e.body.IsInvalid()) {
            auto& bi = system.GetBodyInterface();
            bi.RemoveBody(e.body);
            bi.DestroyBody(e.body);
        }
        e.body = JPH::BodyID();
    }
    std::pair<int, int> category(const Entry& e) const {
        return e.isCharacter ? std::pair{e.controller.collisionLayer, e.controller.collisionMask}
                             : std::pair{e.rigid.collisionLayer, e.rigid.collisionMask};
    }
    bool allowed(uint64_t a, uint64_t b) const {
        auto ia = entries.find(a), ib = entries.find(b);
        if (ia == entries.end() || ib == entries.end()) return false;
        const auto [la, ma] = category(ia->second);
        const auto [lb, mb] = category(ib->second);
        return (ma & (1 << lb)) && (mb & (1 << la));
    }
    struct QueryFilter final : public JPH::BodyFilter {
        const Impl& owner;
        uint64_t ignore;
        bool sensors;
        int mask;
        std::optional<uint64_t> source;
        QueryFilter(const Impl& p, Entity e, bool s, int m, std::optional<uint64_t> from = {})
            : owner(p), ignore(key(e)), sensors(s), mask(m), source(from) {}
        bool ShouldCollideLocked(const JPH::Body& body) const override {
            const auto k = body.GetUserData();
            const auto it = owner.entries.find(k);
            if (k == ignore || it == owner.entries.end() || !owner.world.alive(it->second.entity) ||
                (!sensors && body.IsSensor()))
                return false;
            if (!(mask & (1 << owner.category(it->second).first))) return false;
            return !source || owner.allowed(*source, k);
        }
    };
    JPH::RefConst<JPH::Shape> shape(const Entry& e) {
        const auto& c = e.collider;
        const auto s = glm::abs(e.placed.scale);
        if (!finite(c.offset)) throw std::runtime_error("Collider offset must be finite");
        JPH::RefConst<JPH::Shape> result;
        switch (c.shape) {
        case Collider::Shape::Box: {
            const auto h = c.halfExtents * s;
            if (!finite(h) || std::min({h.x, h.y, h.z}) <= 0)
                throw std::runtime_error("Box half extents must be positive and finite");
            result = checked(JPH::BoxShapeSettings(vec(h), std::min(.05f, std::min({h.x, h.y, h.z}) * .2f)).Create());
            break;
        }
        case Collider::Shape::Sphere:
        case Collider::Shape::Capsule:
            if (!uniform(s) || !std::isfinite(c.radius) || c.radius <= 0)
                throw std::runtime_error("Sphere/capsule requires positive radius and uniform world scale");
            if (c.shape == Collider::Shape::Sphere)
                result = checked(JPH::SphereShapeSettings(c.radius * s.x).Create());
            else {
                if (!std::isfinite(c.halfHeight) || c.halfHeight <= 0)
                    throw std::runtime_error("Capsule half height must be positive and finite");
                result = checked(JPH::CapsuleShapeSettings(c.halfHeight * s.x, c.radius * s.x).Create());
            }
            break;
        case Collider::Shape::ConvexModel:
        case Collider::Shape::TriangleModel: {
            if (!e.model) throw std::runtime_error("Model collider requires an imported ModelRenderer asset");
            if (c.shape == Collider::Shape::TriangleModel && e.rigid.motion != RigidBody::Motion::Static)
                throw std::runtime_error("Triangle model colliders must be Static; use Convex model for moving bodies");
            JPH::Array<JPH::Vec3> points;
            JPH::TriangleList triangles;
            const auto scale = glm::scale(glm::mat4(1), e.placed.scale);
            for (const auto& node : e.model->data.nodes)
                for (auto index : node.meshes) {
                    const auto& mesh = e.model->data.meshes[index].data;
                    const auto matrix = scale * node.world;
                    if (c.shape == Collider::Shape::ConvexModel) {
                        for (const auto& v : mesh.vertices)
                            points.push_back(vec(glm::vec3(matrix * glm::vec4(v.position, 1))));
                    } else
                        for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
                            auto a = vec(glm::vec3(matrix * glm::vec4(mesh.vertices[mesh.indices[i]].position, 1)));
                            auto b = vec(glm::vec3(matrix * glm::vec4(mesh.vertices[mesh.indices[i + 1]].position, 1)));
                            auto d = vec(glm::vec3(matrix * glm::vec4(mesh.vertices[mesh.indices[i + 2]].position, 1)));
                            if (glm::determinant(matrix) < 0) std::swap(b, d);
                            triangles.emplace_back(a, b, d);
                        }
                }
            if (c.shape == Collider::Shape::ConvexModel) {
                if (points.size() < 4) throw std::runtime_error("Convex model needs at least four non-coplanar points");
                result = checked(JPH::ConvexHullShapeSettings(points).Create());
            } else {
                if (triangles.empty()) throw std::runtime_error("Triangle model has no triangles");
                result = checked(JPH::MeshShapeSettings(triangles).Create());
            }
            break;
        }
        default: throw std::runtime_error("Unknown collider shape");
        }
        if (glm::length(c.offset) > 0)
            result = checked(
                JPH::RotatedTranslatedShapeSettings(vec(c.offset * e.placed.scale), JPH::Quat::sIdentity(), result)
                    .Create());
        return result;
    }
    void create(Entry& e) {
        filterValid(category(e).first, category(e).second);
        if (e.isCharacter) {
            const auto& c = e.controller;
            if (!uniform(e.placed.scale) || e.placed.scale.x <= 0)
                throw std::runtime_error("CharacterController requires positive uniform world scale");
            if (!std::isfinite(c.radius) || c.radius <= 0 || !std::isfinite(c.halfHeight) || c.halfHeight <= 0 ||
                !std::isfinite(c.mass) || c.mass <= 0 || !std::isfinite(c.maxSlope) || c.maxSlope < 0 ||
                c.maxSlope >= 90 || !std::isfinite(c.stepHeight) || c.stepHeight < 0 || !std::isfinite(c.jumpSpeed) ||
                c.jumpSpeed < 0 || !std::isfinite(c.maxPushForce) || c.maxPushForce < 0)
                throw std::runtime_error("Invalid character dimensions, mass, slope or movement settings");
            const float s = std::abs(e.placed.scale.x), r = c.radius * s, h = c.halfHeight * s;
            auto capsule = checked(JPH::CapsuleShapeSettings(h, r).Create());
            auto standing = checked(
                JPH::RotatedTranslatedShapeSettings(JPH::Vec3(0, h + r, 0), JPH::Quat::sIdentity(), capsule).Create());
            JPH::CharacterVirtualSettings settings;
            settings.mShape = standing;
            settings.mInnerBodyShape = standing;
            settings.mInnerBodyLayer = 1;
            settings.mMass = c.mass;
            settings.mMaxStrength = c.maxPushForce;
            settings.mMaxSlopeAngle = glm::radians(c.maxSlope);
            settings.mSupportingVolume = JPH::Plane(JPH::Vec3::sAxisY(), -r);
            settings.mEnhancedInternalEdgeRemoval = true;
            e.character = new JPH::CharacterVirtual(
                &settings, vec(e.placed.position), JPH::Quat::sIdentity(), key(e.entity), &system);
            e.body = e.character->GetInnerBodyID();
            if (e.body.IsInvalid()) {
                e.character = nullptr;
                throw std::runtime_error("Jolt body capacity exhausted");
            }
            return;
        }
        const auto& r = e.rigid;
        if (int(r.motion) < 0 || int(r.motion) > 2 || !std::isfinite(r.mass) || r.mass <= 0 ||
            !std::isfinite(r.friction) || r.friction < 0 || !std::isfinite(r.restitution) || r.restitution < 0 ||
            r.restitution > 1 || !std::isfinite(r.linearDamping) || r.linearDamping < 0 || r.linearDamping > 1 ||
            !std::isfinite(r.angularDamping) || r.angularDamping < 0 || r.angularDamping > 1 ||
            !std::isfinite(r.gravityFactor) || !finite(r.linearVelocity) || !finite(r.angularVelocity))
            throw std::runtime_error("Invalid rigid body motion, mass, material, damping or velocity");
        const auto motion = r.motion == RigidBody::Motion::Static    ? JPH::EMotionType::Static
                            : r.motion == RigidBody::Motion::Dynamic ? JPH::EMotionType::Dynamic
                                                                     : JPH::EMotionType::Kinematic;
        JPH::BodyCreationSettings settings(
            shape(e), vec(e.placed.position), quat(e.placed.rotation), motion,
            motion == JPH::EMotionType::Static ? 0 : 1);
        settings.mUserData = key(e.entity);
        settings.mIsSensor = r.sensor;
        settings.mFriction = r.friction;
        settings.mRestitution = r.restitution;
        settings.mLinearDamping = r.linearDamping;
        settings.mAngularDamping = r.angularDamping;
        settings.mGravityFactor = r.gravityFactor;
        settings.mLinearVelocity = vec(r.linearVelocity);
        settings.mAngularVelocity = vec(r.angularVelocity);
        settings.mMotionQuality = r.continuous && !r.sensor && r.motion == RigidBody::Motion::Dynamic
                                      ? JPH::EMotionQuality::LinearCast
                                      : JPH::EMotionQuality::Discrete;
        settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
        settings.mMassPropertiesOverride.mMass = r.mass;
        // Sensors with kinematic motion may also overlap static scene geometry.
        settings.mCollideKinematicVsNonDynamic = r.sensor;
        e.body = system.GetBodyInterface().CreateAndAddBody(
            settings, motion == JPH::EMotionType::Static ? JPH::EActivation::DontActivate : JPH::EActivation::Activate);
        if (e.body.IsInvalid()) throw std::runtime_error("Jolt body capacity exhausted");
    }
    void synchronize() {
        problems.clear();
        std::unordered_set<uint64_t> seen;
        world.forEachEntity([&](Entity id) {
            const auto* collider = world.get<Collider>(id);
            const auto* controller = world.get<CharacterController>(id);
            const auto* rigid = world.get<RigidBody>(id);
            if (!collider && !controller && !rigid) return;
            if ((controller && !controller->enabled) || (collider && !collider->enabled) || (rigid && !rigid->enabled))
                return;
            seen.insert(key(id));
            Entry candidate;
            candidate.entity = id;
            candidate.isCharacter = controller != nullptr;
            candidate.rigid.motion = RigidBody::Motion::Static;
            if (collider) candidate.collider = *collider;
            if (rigid) candidate.rigid = *rigid;
            if (controller) candidate.controller = *controller;
            try {
                if (controller && (collider || rigid))
                    throw std::runtime_error("CharacterController cannot share an entity with Collider or RigidBody");
                if (!controller && !collider) throw std::runtime_error("RigidBody requires a Collider");
                if (!world.has<Transform>(id)) throw std::runtime_error("Physics requires a Transform");
                candidate.placed = pose(worldMatrix(world, id));
                const auto parent = parentOf(world, id);
                if (parent.valid() && (controller || candidate.rigid.motion == RigidBody::Motion::Dynamic) &&
                    !uniform(pose(worldMatrix(world, parent)).scale))
                    throw std::runtime_error("Dynamic bodies/characters require uniform parent world scale");
                if (assets)
                    if (const auto* instance = world.get<ModelRenderer>(id))
                        candidate.model = assets->model(instance->model);
            } catch (const std::exception& ex) {
                candidate.error = ex.what();
                candidate.invalidPlacement = true;
            }
            auto it = entries.find(key(id));
            const bool rebuild =
                it == entries.end() || it->second.isCharacter != candidate.isCharacter ||
                it->second.collider != candidate.collider || !rigidConfig(it->second.rigid, candidate.rigid) ||
                !characterConfig(it->second.controller, candidate.controller) ||
                !near(it->second.placed.scale, candidate.placed.scale) || it->second.model != candidate.model ||
                it->second.invalidPlacement != candidate.invalidPlacement ||
                (!candidate.error.empty() && it->second.error != candidate.error);
            if (rebuild) {
                if (it != entries.end()) {
                    remove(it->second);
                    entries.erase(it);
                }
                it = entries.emplace(key(id), std::move(candidate)).first;
                if (it->second.error.empty()) try {
                        create(it->second);
                    } catch (const std::exception& ex) {
                        it->second.error = ex.what();
                    }
            } else if (it->second.error.empty()) {
                auto& e = it->second;
                auto& bi = system.GetBodyInterface();
                if (e.character) {
                    if (!near(e.placed.position, candidate.placed.position))
                        e.character->SetPosition(vec(candidate.placed.position));
                } else if (!e.body.IsInvalid()) {
                    // Kinematic targets are moved with velocity in step(); dynamic
                    // Transform edits are explicit teleports and preserve velocity.
                    if (e.rigid.motion != RigidBody::Motion::Kinematic &&
                        (!near(e.placed.position, candidate.placed.position) ||
                         !near(e.placed.rotation, candidate.placed.rotation)))
                        bi.SetPositionAndRotation(
                            e.body, vec(candidate.placed.position), quat(candidate.placed.rotation),
                            JPH::EActivation::Activate);
                    if (!near(e.rigid.linearVelocity, candidate.rigid.linearVelocity) ||
                        !near(e.rigid.angularVelocity, candidate.rigid.angularVelocity))
                        bi.SetLinearAndAngularVelocity(
                            e.body, vec(candidate.rigid.linearVelocity), vec(candidate.rigid.angularVelocity));
                    e.rigid.linearVelocity = candidate.rigid.linearVelocity;
                    e.rigid.angularVelocity = candidate.rigid.angularVelocity;
                }
                e.placed = candidate.placed;
            }
            if (!it->second.error.empty()) problems.push_back({id, it->second.error});
        });
        for (auto it = entries.begin(); it != entries.end();) {
            if (!seen.contains(it->first)) {
                remove(it->second);
                it = entries.erase(it);
            } else
                ++it;
        }
    }
    void sensorEvents() {
        std::map<TriggerKey, ContactEvent> current;
        for (const auto& [k, e] : entries) {
            if (e.isCharacter || !e.rigid.sensor || e.body.IsInvalid()) continue;
            JPH::RefConst<JPH::Shape> shape;
            JPH::RMat44 transform;
            {
                JPH::BodyLockRead lock(system.GetBodyLockInterface(), e.body);
                if (!lock.Succeeded()) continue;
                shape = lock.GetBody().GetShape();
                transform = lock.GetBody().GetCenterOfMassTransform();
            }
            QueryFilter filter(*this, e.entity, false, e.rigid.collisionMask, k);
            JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> collector;
            JPH::CollideShapeSettings settings;
            settings.mBackFaceMode = JPH::EBackFaceMode::CollideWithBackFaces;
            system.GetNarrowPhaseQuery().CollideShape(
                shape, JPH::Vec3::sOne(), transform, settings, JPH::RVec3::sZero(), collector, {}, {}, filter);
            for (const auto& hit : collector.mHits) {
                JPH::BodyLockRead lock(system.GetBodyLockInterface(), hit.mBodyID2);
                if (!lock.Succeeded()) continue;
                const auto& other = lock.GetBody();
                if (e.rigid.motion == RigidBody::Motion::Static && other.IsStatic()) continue;
                const auto target = other.GetUserData();
                const TriggerKey pair{k, target};
                auto normal = hit.mPenetrationAxis;
                normal = normal.NormalizedOr(JPH::Vec3::sAxisY());
                current[pair] = {
                    triggers.contains(pair) ? ContactEvent::Type::Stay : ContactEvent::Type::Begin,
                    e.entity,
                    entity(target),
                    vec(hit.mContactPointOn2),
                    vec(normal),
                    true};
            }
        }
        for (const auto& [k, event] : current) {
            (void)k;
            events.push_back(event);
        }
        for (const auto& [k, event] : triggers)
            if (!current.contains(k)) {
                auto end = event;
                end.type = ContactEvent::Type::End;
                events.push_back(end);
            }
        triggers = std::move(current);
    }
    void writeBack() {
        std::unordered_map<uint64_t, glm::mat4> simulated;
        for (auto& [k, e] : entries) {
            if (!e.error.empty() || e.body.IsInvalid()) continue;
            JPH::RVec3 position;
            JPH::Quat rotation;
            if (e.character) {
                position = e.character->GetPosition();
                rotation = quat(e.placed.rotation);
            } else
                system.GetBodyInterface().GetPositionAndRotation(e.body, position, rotation);
            e.placed.position = vec(position);
            e.placed.rotation = quat(rotation);
            if (e.character || e.rigid.motion == RigidBody::Motion::Dynamic)
                simulated[k] = glm::translate(glm::mat4(1), e.placed.position) * glm::mat4_cast(e.placed.rotation) *
                               glm::scale(glm::mat4(1), e.placed.scale);
            if (auto* r = world.get<RigidBody>(e.entity)) {
                r->linearVelocity = vec(system.GetBodyInterface().GetLinearVelocity(e.body));
                r->angularVelocity = vec(system.GetBodyInterface().GetAngularVelocity(e.body));
                e.rigid.linearVelocity = r->linearVelocity;
                e.rigid.angularVelocity = r->angularVelocity;
            }
        }
        // Resolve the simulated parent matrices before changing any local transform.
        // This also handles non-physical intermediate parents between physical bodies.
        std::function<glm::mat4(Entity, int)> parentWorld = [&](Entity id, int depth) -> glm::mat4 {
            if (!id.valid() || depth >= kMaxHierarchyDepth) return glm::mat4(1);
            if (auto it = simulated.find(key(id)); it != simulated.end()) return it->second;
            const auto* t = world.get<Transform>(id);
            return parentWorld(parentOf(world, id), depth + 1) * (t ? t->matrix() : glm::mat4(1));
        };
        std::vector<std::pair<Entity, Transform>> updates;
        for (const auto& [k, matrix] : simulated) {
            const auto id = entity(k);
            const auto local = pose(glm::inverse(parentWorld(parentOf(world, id), 0)) * matrix);
            auto t = *world.get<Transform>(id);
            t.position = local.position;
            t.rotation = local.rotation;
            updates.emplace_back(id, t);
        }
        for (const auto& [id, t] : updates) *world.get<Transform>(id) = t;
    }
    void step(double dt) {
        if (!std::isfinite(dt) || dt <= 0 || dt > std::numeric_limits<float>::max() || float(dt) <= 0)
            throw std::invalid_argument("Physics dt must be positive and finite");
        synchronize();
        PhysicsSettings settings;
        bool configured = false;
        world.each<const PhysicsSettings>([&](const auto& value) {
            if (!configured) {
                settings = value;
                configured = true;
            }
        });
        if (!finite(settings.gravity) || settings.collisionSteps < 1 || settings.collisionSteps > 8)
            throw std::invalid_argument("PhysicsSettings needs finite gravity and 1..8 collision steps");
        system.SetGravity(vec(settings.gravity));
        const float seconds = float(dt);
        for (auto& [k, e] : entries) {
            (void)k;
            if (!e.error.empty()) continue;
            if (e.character) {
                auto* c = world.get<CharacterController>(e.entity);
                if (!finite(c->desiredVelocity))
                    throw std::invalid_argument("Character desired velocity must be finite");
                auto& character = *e.character;
                character.UpdateGroundVelocity();
                const auto ground = character.GetGroundVelocity();
                auto velocity = JPH::Vec3(0, character.GetLinearVelocity().GetY(), 0);
                if (character.GetGroundState() == JPH::CharacterBase::EGroundState::OnGround &&
                    velocity.GetY() - ground.GetY() < .1f) {
                    velocity = ground;
                    if (c->jumpRequested) velocity += JPH::Vec3(0, c->jumpSpeed, 0);
                }
                velocity += vec(settings.gravity) * seconds + JPH::Vec3(c->desiredVelocity.x, 0, c->desiredVelocity.z);
                character.SetLinearVelocity(velocity);
                c->jumpRequested = false;
                JPH::CharacterVirtual::ExtendedUpdateSettings update;
                update.mWalkStairsStepUp = JPH::Vec3(0, c->stepHeight * std::abs(e.placed.scale.x), 0);
                QueryFilter filter(*this, e.entity, false, c->collisionMask, k);
                character.ExtendedUpdate(
                    seconds, vec(settings.gravity), update, system.GetDefaultBroadPhaseLayerFilter(1),
                    system.GetDefaultLayerFilter(1), filter, {}, temporary);
                c->grounded = character.GetGroundState() == JPH::CharacterBase::EGroundState::OnGround;
                c->velocity = vec(character.GetLinearVelocity());
            } else if (!e.body.IsInvalid() && e.rigid.motion == RigidBody::Motion::Kinematic)
                system.GetBodyInterface().MoveKinematic(
                    e.body, vec(e.placed.position), quat(e.placed.rotation), seconds);
        }
        if (system.GetNumBodies()) {
            const auto error = system.Update(seconds, settings.collisionSteps, &temporary, &jobs);
            jobs.drain();
            if (error != JPH::EPhysicsUpdateError::None)
                throw std::runtime_error(
                    "Jolt update capacity exceeded; increase PhysicsConfig body-pair/contact limits");
        }
        listener.publish(events);
        sensorEvents();
        writeBack();
        std::stable_sort(events.begin(), events.end(), [](const auto& a, const auto& b) {
            return std::tuple{key(a.a), key(a.b), a.type} < std::tuple{key(b.a), key(b.b), b.type};
        });
    }
};

PhysicsWorld::PhysicsWorld(World& world, JobSystem& jobs, const Assets* assets, PhysicsConfig config) {
    (void)runtime();
    m_impl = std::make_unique<Impl>(world, jobs, assets, config);
}
PhysicsWorld::~PhysicsWorld() = default;
void PhysicsWorld::synchronize() { m_impl->synchronize(); }
void PhysicsWorld::step(double dt) { m_impl->step(dt); }
bool PhysicsWorld::addImpulse(Entity id, const glm::vec3& impulse) {
    const auto it = m_impl->entries.find(key(id));
    if (!m_impl->world.alive(id) || !finite(impulse) || it == m_impl->entries.end() || it->second.body.IsInvalid() ||
        it->second.isCharacter || it->second.rigid.motion != RigidBody::Motion::Dynamic)
        return false;
    m_impl->system.GetBodyInterface().AddImpulse(it->second.body, vec(impulse));
    return true;
}
bool PhysicsWorld::addForce(Entity id, const glm::vec3& force) {
    const auto it = m_impl->entries.find(key(id));
    if (!m_impl->world.alive(id) || !finite(force) || it == m_impl->entries.end() || it->second.body.IsInvalid() ||
        it->second.isCharacter || it->second.rigid.motion != RigidBody::Motion::Dynamic)
        return false;
    m_impl->system.GetBodyInterface().AddForce(it->second.body, vec(force));
    return true;
}
bool PhysicsWorld::setVelocity(Entity id, const glm::vec3& linear, const glm::vec3& angular) {
    const auto it = m_impl->entries.find(key(id));
    if (!m_impl->world.alive(id) || !finite(linear) || !finite(angular) || it == m_impl->entries.end() ||
        it->second.body.IsInvalid() || it->second.isCharacter || it->second.rigid.motion != RigidBody::Motion::Dynamic)
        return false;
    m_impl->system.GetBodyInterface().SetLinearAndAngularVelocity(it->second.body, vec(linear), vec(angular));
    return true;
}
std::optional<RayHit> PhysicsWorld::raycast(
    const glm::vec3& origin, const glm::vec3& direction, float distance, Entity ignore, bool sensors, int mask) const {
    if (!finite(origin) || !finite(direction) || glm::length(direction) < 1e-8f || !std::isfinite(distance) ||
        distance <= 0 || mask < 0 || mask > 65535)
        return {};
    Impl::QueryFilter filter(*m_impl, ignore, sensors, mask);
    JPH::RayCastResult result;
    JPH::RRayCast ray(vec(origin), vec(glm::normalize(direction) * distance));
    if (!m_impl->system.GetNarrowPhaseQuery().CastRay(ray, result, {}, {}, filter)) return {};
    JPH::BodyLockRead lock(m_impl->system.GetBodyLockInterface(), result.mBodyID);
    if (!lock.Succeeded()) return {};
    const auto point = ray.GetPointOnRay(result.mFraction);
    const auto& body = lock.GetBody();
    return RayHit{
        entity(body.GetUserData()), vec(point), vec(body.GetWorldSpaceSurfaceNormal(result.mSubShapeID2, point)),
        result.mFraction * distance};
}
const std::vector<ContactEvent>& PhysicsWorld::contacts() const { return m_impl->events; }
const std::vector<PhysicsDiagnostic>& PhysicsWorld::diagnostics() const { return m_impl->problems; }
uint32_t PhysicsWorld::bodyCount() const { return m_impl->system.GetNumBodies(); }
uint32_t PhysicsWorld::activeBodyCount() const { return m_impl->system.GetNumActiveBodies(JPH::EBodyType::RigidBody); }
uint32_t PhysicsWorld::characterCount() const {
    uint32_t count = 0;
    for (const auto& [k, e] : m_impl->entries) {
        (void)k;
        count += e.character != nullptr;
    }
    return count;
}
}   // namespace shockwave
