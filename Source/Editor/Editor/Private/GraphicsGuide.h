#pragma once

#include <filesystem>

namespace editor {

// Live source docs in a developer checkout, bundled docs when the build is relocated.
std::filesystem::path graphicsGuidePath();
void drawGraphicsGuide(bool& open);

}   // namespace editor
