#pragma once

// Compiling material graphs to SPIR-V at edit time (W4a, docs/MaterialGraph.md). The editor,
// --check and the cook run glslangValidator, the program the build compiles the engine's
// shaders with (cmake/Shaders.cmake), and keep the result in the project's
// Saved/Cache/Shaders/<hash>.spv. A packaged game only reads those files: it never compiles.

#include <Assets/MaterialGraph.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace shockwave {

// The glslangValidator the build used (baked in when Shockwave was configured); empty if
// there was none. SHOCKWAVE_GLSLANG in the environment overrides it, "none" turning it off.
std::filesystem::path shaderCompilerTool();
// The engine's shader sources the graphs are spliced into (mesh.glsl and its includes).
std::filesystem::path engineShaderSources();
// A hash of the compiled scene shader (shaders/mesh.frag.spv beside the program): graphs
// compiled against another engine build get other cache names. 0 if it can't be read.
uint64_t engineSurfaceShaderHash();
// Where a graph's SPIR-V lives in a cache folder (Saved/Cache/Shaders).
std::filesystem::path materialGraphShaderFile(const std::filesystem::path& cacheFolder, uint64_t hash);

struct GraphShaderResult {
    bool ok = false;
    bool fromCache = false;   // read from the cache, not compiled now
    uint64_t hash = 0;
    std::filesystem::path file;   // the cache file
    std::vector<uint32_t> spirv;
    std::string error;   // the compiler's complaints, each mapped back to its node where it can be
    double milliseconds = 0;   // compiling, or reading the cache
};
// The graph's shader: from the cache when it's there, else compiled with
// shaderCompilerTool() and written to the cache (through a temporary name). allowCompile
// false only reads (a packaged game).
GraphShaderResult
materialGraphShader(const MaterialGraphCode& code, const std::filesystem::path& cacheFolder, bool allowCompile = true);

}   // namespace shockwave
