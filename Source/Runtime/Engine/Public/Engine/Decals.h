#pragma once

// Decals in a world (G08, A03; docs/Decals.md): turning Decal components into what the
// renderer draws, impact marks spawned by gameplay, and their lifetimes. The renderer side,
// and the rules for how decals land and blend, are in Renderer/Decals.h.

#include <Engine/Components.h>
#include <Renderer/Decals.h>

#include <string>

namespace shockwave {

class Assets;
class Schedule;

// What a Decal draws: its box in the world (worldMatrix times its size) and what it paints.
// material is the resolved .swmaterial, if it has one. fade multiplies the opacity
// (DecalLifetime's fade-out). order breaks equal sort orders: decalOrder below.
DecalDesc decalDesc(
    const Decal& decal, const glm::mat4& worldMatrix, const MaterialAsset* material, uint64_t order, float fade = 1.0f);
// Equal sort orders draw in this order: authored decals by entity (scene order), and impact
// decals after them, in the order they were made.
uint64_t decalOrder(Entity e, const DecalLifetime* lifetime);
// How much of a DecalLifetime's opacity is left: 1 until its fade starts, 0 once it's over.
float decalLifetimeFade(const DecalLifetime& lifetime);

// What's wrong with a Decal, for `player --check` and the editor; empty when nothing.
// assets resolves its material (skipped without).
std::string decalProblem(const Decal& decal, const Assets* assets);

// ---- Impact marks ----

// A mark where something hit: a bullet hole, a scorch, a splat.
struct ImpactDecal {
    glm::vec3 position{0.0f};   // the hit point, on the surface
    glm::vec3 normal{0.0f, 1.0f, 0.0f};   // the surface's, pointing out of it
    // What was hit: the mark is stuck to it and moves with it (a door), and is removed with
    // it. Null or already gone: stuck to nothing.
    Entity receiver{};
    Decal decal;   // what it looks like; decal.size.z is how deep it reaches into the surface
    uint32_t seed = 0;   // turns it about the normal, so repeated marks don't line up; 0 keeps it upright
    float lifetime = 30.0f;   // seconds, then gone; 0 lasts until removed
    float fade = 2.0f;   // seconds of fading out before then
};
// How many impact marks a world keeps at most; making another removes the oldest.
inline constexpr uint32_t kDefaultMaxImpactDecals = 256;

// Makes the mark: an entity with a Transform (under the receiver when there is one), the
// Decal and a DecalLifetime. Its +Z faces along the normal, its +Y up as far as the normal
// allows (world +Y, or +Z on floors and ceilings), turned about the normal by the seed.
// Past maxImpactDecals, the oldest one goes first. Changes the world's structure: call it
// from an exclusive system or outside the schedule. Returns the entity, or null for a
// Decal decalProblem rejects or a zero normal.
Entity spawnImpactDecal(World& world, const ImpactDecal& impact, uint32_t maxImpactDecals = kDefaultMaxImpactDecals);
// The rotation spawnImpactDecal gives a mark (exposed for tests and the editor's placement).
glm::quat impactDecalRotation(const glm::vec3& normal, uint32_t seed);

// Ages every DecalLifetime by dt, destroying those whose time is up and those whose
// receiver has been destroyed. addBuiltinSystems runs it each tick.
void updateDecalLifetimes(World& world, float dt);

}   // namespace shockwave
