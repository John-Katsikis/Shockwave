#pragma once

// The GPU parts of the editor: what Stats shows about the GPU in use, and the GPU choice in
// Preferences. See Renderer/GpuSelection.h for how a GPU is picked.

namespace shockwave {
class Engine;
class Renderer;
}   // namespace shockwave

namespace editor {

struct Preferences;

// Under the GPU name in Stats: any warnings, and a "GPUs" section with every GPU found and
// why this one renders.
void drawGpuStats(const shockwave::Renderer& renderer);
// The GPU combo in Preferences. Picking one restarts the renderer on it straight away.
// Returns true when the preferences changed.
bool drawGpuPreference(shockwave::Engine& engine, Preferences& prefs);

}   // namespace editor
