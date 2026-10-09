#pragma once

#include <imgui.h>

#include <cstdint>

// Helpers for Dear ImGui inside Shockwave (EngineConfig::gui, Game::onGui).
//
// The window's swapchain is sRGB, so colors written by the GUI are treated as linear and
// encoded on output. ImGui colors are authored in sRGB, so every color handed to ImGui
// must go through gui::color() first, or it comes out washed out. The engine already does
// this for the built-in theme.
//
// Threading: ImGui is main-thread only. Call it from Game::onGui.
namespace shockwave::gui {

// sRGB color (0..1 components, as a color picker shows them) to what ImGui needs here.
ImVec4 color(float r, float g, float b, float a = 1.0f);
ImU32 colorU32(float r, float g, float b, float a = 1.0f);

// The Shockwave look: dark, with an electric blue accent. Applied by the engine at
// startup; call again after ImGui::GetStyle() changes to restore it.
void applyTheme(float dpiScale = 1.0f);   // dpiScale: SDL_GetWindowDisplayScale

// The accent color, ready for ImGui (already converted).
ImVec4 accent();

} // namespace shockwave::gui
