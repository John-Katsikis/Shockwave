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
// In Stats: each graphics setting as asked for and as running (a fallback, and why, in the
// warning colour), what the GPU supports, and GPU memory by category. Like
// player --graphics-report.
void drawGraphicsReport(const shockwave::Renderer& renderer);

}   // namespace editor
