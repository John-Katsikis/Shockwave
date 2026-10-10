#pragma once
namespace shockwave {
class Renderer;
class Assets;
// Shared editor/player starter assets. Files import after these are registered.
void createBuiltinAssets(Renderer& renderer, Assets& assets);
}
