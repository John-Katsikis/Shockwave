#pragma once

// What every model importer shares (Model.cpp's Assimp path and FbxSdkImport.cpp), so a
// model reads the same whichever importer read its file: texture lookup (F1), tangents,
// bounds and the rest pose of skinned meshes.

#include <Assets/Model.h>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace shockwave::modelimport {

// Adds path to list once, absolute and normalised.
void dependency(std::vector<std::filesystem::path>& list, const std::filesystem::path& path);
void expand(Bounds& bounds, const glm::vec3& point);
Bounds emptyBounds();

// The file a texture path saved in a model names: where the file says (relative paths
// are read from fileFolder), else the nearest match by name around modelFolder (F1,
// docs/Assets.md). Records the dependency and the warning; when it's nowhere, adds it to
// model.missingTextures and returns nothing. convertedBlend: fileFolder is a .blend's
// temporary conversion, so a missing texture is expected beside the .blend instead.
std::optional<std::filesystem::path> locateTexture(
    ModelData& model, const std::string& savedPath, const std::filesystem::path& modelFolder,
    const std::filesystem::path& fileFolder, bool convertedBlend = false);

// Fills each vertex's tangent: from the file's tangents and bitangents where given (one
// per vertex), else worked out from UV0, mirrored UVs included.
void computeTangents(ModelMesh& mesh, const glm::vec3* fileTangents, const glm::vec3* fileBitangents);

// A skinned mesh's vertices where the model's rest pose puts them, in the model's space.
std::vector<glm::vec3> restPositions(const ModelData& model, const ModelMesh& mesh);

// A model with skinned meshes: its bounds from the rest pose, since skinned vertices sit
// where their joints put them rather than where their node is. Needs bones resolved.
void skinnedBounds(ModelData& model);

}   // namespace shockwave::modelimport
