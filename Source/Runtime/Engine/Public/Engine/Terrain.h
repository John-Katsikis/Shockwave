#pragma once

// Heightmap terrain in a level (W1a, docs/Terrain.md). The data and its render chunks live
// in the asset registry (Assets::terrain, Assets/TerrainData.h); this is the component that
// places one, and the checks player --check runs on it.

#include <Engine/World.h>

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace shockwave {

class Assets;

// The terrain's corner with the lowest X and Z sits at the entity's world position. It is
// never rotated or scaled: those parts of the Transform are ignored (and reported).
struct Terrain {
    std::string asset;   // .swterrain path inside Content
    bool visible = true;
    bool castShadows = true;
};
// Registers Terrain (scenes, the Inspector, player --schema). Called with the engine's
// built-in components.
void registerTerrainComponents();

// What's wrong with the scene's terrains, one line each: a Terrain whose asset isn't
// loaded (with the asset folder's reason when given), a rotated or scaled Transform, a
// Terrain collider without a Terrain component (or on a body that isn't Static), a material
// that isn't loaded.
std::vector<std::string> terrainProblems(
    const World& world, const Assets& assets, const std::function<std::string(std::string_view)>& whyNotLoaded = {});

}   // namespace shockwave
