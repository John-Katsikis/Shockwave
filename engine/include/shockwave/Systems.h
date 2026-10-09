#pragma once

#include <shockwave/Components.h>
#include <shockwave/Schedule.h>

namespace shockwave {

class Renderer;

// The built-in gameplay systems: Spin and Bob.
void addBuiltinSystems(Schedule& schedule);

// Call at the start of each tick, before the schedule runs: remembers every Transform so
// renderWorld can blend the last two ticks and motion stays smooth at any frame rate.
void storePreviousTransforms(World& world, JobSystem& jobs);

// Draws every visible MeshRenderer where its parents put it, spread across the job
// system, and sets the clear color from the Environment. alpha is Game::onRender's;
// pass 1 to draw the current state with no blending (an editor that isn't playing).
// Set the camera first.
void renderWorld(World& world, Renderer& renderer, JobSystem& jobs, double alpha);

} // namespace shockwave
