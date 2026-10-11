#pragma once

#include <Assets/Model.h>

#include <glm/glm.hpp>

#include <memory>
#include <span>
#include <vector>

namespace shockwave {

// Where an animated model's nodes are right now: one matrix per node of its model, in
// the model's own space (ModelData::nodes order). The Animation module writes it every
// frame for entities with an Animator; renderWorld draws the entity's ModelRenderer in
// this pose, skinning its skinned meshes. Without one a model shows its rest pose.
// Runtime only: never saved, and not in the Inspector.
struct ModelPose {
    std::shared_ptr<const ModelAsset> model;   // the model it was posed for; ignored once that's reloaded
    std::vector<glm::mat4> nodes;
};

// The joint matrices that pose a skinned mesh, as Renderer::drawSkinnedMesh wants them:
// for each of the mesh's bones, its node's matrix times the bone's inverse bind matrix.
// nodes: model-space node matrices (a ModelPose's, or the model's rest pose).
void skinMatrices(const ModelMesh& mesh, std::span<const glm::mat4> nodes, std::vector<glm::mat4>& joints);

}   // namespace shockwave
