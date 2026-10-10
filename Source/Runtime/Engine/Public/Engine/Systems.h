#pragma once

#include <Engine/Components.h>
#include <Engine/Schedule.h>

namespace shockwave {

class Renderer;
class Assets;

// The built-in gameplay systems: Spin and Bob.
void addBuiltinSystems(Schedule& schedule);

// Call at the start of each tick, before the schedule runs: remembers every Transform so
// renderWorld can blend the last two ticks and motion stays smooth at any frame rate.
void storePreviousTransforms(World& world, JobSystem& jobs);

// Draws every visible MeshRenderer and, when assets is supplied, ModelRenderer,
// where its parents put it, spread across the job system, and sets the clear color from the Environment. alpha is Application::onRender's;
// pass 1 to draw the current state with no blending (an editor that isn't playing).
// Set the camera first.
void renderWorld(World& world, Renderer& renderer, JobSystem& jobs, double alpha, const Assets* assets = nullptr);

// The first Camera with primary set (or failing that, the first Camera) that has a
// Transform; kNullEntity if the scene has none.
Entity primaryCamera(const World& world);
// The view and reverse-Z projection matrices for looking through camera e, ready for
// Renderer::setCamera. Scale in its transform (or its parents') is ignored. False if e
// isn't a camera with a Transform.
bool cameraMatrices(const World& world, Entity e, float aspect, glm::mat4& view, glm::mat4& projection);

}   // namespace shockwave
