#pragma once

#include <filesystem>

namespace editor {

// Live source docs in a developer checkout, bundled docs when the build is relocated.
std::filesystem::path graphicsGuidePath();
void drawGraphicsGuide(bool& open);
// Opens docs/ShippingImplementation.md (the shipping plan) in the associated application.
void openShippingGuide();

}   // namespace editor
