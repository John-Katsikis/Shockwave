#pragma once

// The optional Autodesk FBX SDK importer (F1b, docs/Assets.md). Compiled only with
// -DSHOCKWAVE_FBXSDK=ON, which defines SHOCKWAVE_HAS_FBXSDK=1 for the Assets module;
// loadModel calls it for .fbx files unless fbx_importer says assimp, and falls back to
// Assimp when it fails.

#include <Assets/Model.h>

#include <filesystem>
#include <string>

namespace shockwave {

// Fills out with what the Assimp path fills: meshes (split per material, triangulated,
// identical corners joined), materials and their textures (found the same way, F1),
// the node tree, skins (at most four joints a vertex, the strongest kept) and every take,
// sampled at the file's frame rate. Units and axes become Assimp's: centimetres, Y up.
// Embedded media are unpacked into extractFolder, a temporary folder the caller removes.
// On failure, error says why and out may hold part of a model (its dependencies are
// still worth watching).
bool importFbxWithSdk(
    const std::filesystem::path& file, ModelData& out, std::string& error, const std::filesystem::path& extractFolder);

}   // namespace shockwave
