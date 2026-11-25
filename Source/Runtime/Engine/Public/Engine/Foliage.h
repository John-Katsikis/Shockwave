#pragma once

// Foliage in a level (W2a, docs/Foliage.md): the component that places a .swfoliage asset
// (Assets::foliage, Assets/FoliageData.h), how a frame draws it, and the checks player
// --check runs on it.

#include <Engine/World.h>

#include <glm/glm.hpp>

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace shockwave {

class Assets;
class Renderer;
struct FoliageData;

// The instances' positions are relative to the entity's world transform.
struct Foliage {
    std::string asset;   // .swfoliage path inside Content
    bool visible = true;
};
void registerFoliageComponents();

// What a frame drew (Renderer-independent; the graphics report shows the last one's).
struct FoliageStats {
    uint32_t foliages = 0;   // components drawn
    uint32_t cells = 0, cellsDrawn = 0;   // in range and in view
    uint32_t instances = 0, instancesDrawn = 0;
    uint32_t calls = 0;   // drawMeshInstances calls
    double submitMs = 0.0;   // CPU time spent choosing and submitting them
};
const FoliageStats& lastFoliageStats();

// Draws every visible Foliage in the world: for each type's cells within its distance
// (times foliage_distance) and inside the view, the instances that survive density and
// fade, one drawMeshInstances call per model part (per 2048 instances). Cells beyond half the distance use the
// model's simplest level. include: as renderWorld's.
void drawFoliage(
    const World& world, Renderer& renderer, const Assets& assets,
    const std::function<bool(const World&, Entity)>& include = {});

// The instances one cell keeps for a camera at distance metres, with density and the
// type's fade: their indices into foliage.instances. The rule drawFoliage uses.
void foliageCellKeeps(
    const FoliageData& foliage, size_t cell, float distance, float density, float distanceScale,
    std::vector<uint32_t>& keep);

// What's wrong with the scene's foliage, one line each: an asset that isn't loaded (with
// the asset folder's reason), a type whose model isn't loaded.
std::vector<std::string> foliageProblems(
    const World& world, const Assets& assets, const std::function<std::string(std::string_view)>& whyNotLoaded = {});

}   // namespace shockwave
