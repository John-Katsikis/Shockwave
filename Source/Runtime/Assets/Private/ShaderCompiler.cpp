#include <Assets/ShaderCompiler.h>

#include <Core/Process.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>

namespace shockwave {

namespace fs = std::filesystem;

namespace {

bool readWords(const fs::path& path, std::vector<uint32_t>& words) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bytes.empty() || bytes.size() % 4) return false;
    words.resize(bytes.size() / 4);
    std::memcpy(words.data(), bytes.data(), bytes.size());
    return words[0] == 0x07230203u;   // SPIR-V's magic number
}

// The node a compiler message is about: the generated line ends "// <id> (<type>)".
std::string nodeOfLine(const std::vector<std::string>& lines, int line) {
    if (line < 1 || size_t(line) > lines.size()) return {};
    const std::string& text = lines[size_t(line) - 1];
    const size_t mark = text.rfind("// ");
    if (mark == std::string::npos) return {};
    const size_t space = text.find(' ', mark + 3);
    return text.substr(mark + 3, space == std::string::npos ? std::string::npos : space - mark - 3);
}

}   // namespace

fs::path shaderCompilerTool() {
    if (const char* env = std::getenv("SHOCKWAVE_GLSLANG"); env && *env)
        return std::string_view(env) == "none" ? fs::path() : fs::path(env);
#ifdef SHOCKWAVE_GLSLANG_PATH
    return fs::path(SHOCKWAVE_GLSLANG_PATH);
#else
    return {};
#endif
}

fs::path engineShaderSources() {
#ifdef SHOCKWAVE_SHADER_SOURCE_DIR
    return fs::path(SHOCKWAVE_SHADER_SOURCE_DIR);
#else
    return {};
#endif
}

uint64_t engineSurfaceShaderHash() {
    static std::once_flag once;
    static uint64_t hash = 0;
    std::call_once(once, [] {
        std::vector<uint32_t> words;
        if (!readWords(currentExecutable().parent_path() / "shaders" / "mesh.frag.spv", words)) return;
        uint64_t h = 1469598103934665603ull;
        for (uint32_t w : words)
            for (int i = 0; i < 4; ++i) {
                h ^= (w >> (8 * i)) & 0xFFu;
                h *= 1099511628211ull;
            }
        hash = h;
    });
    return hash;
}

fs::path materialGraphShaderFile(const fs::path& cacheFolder, uint64_t hash) {
    char name[32];
    std::snprintf(name, sizeof(name), "%016llx.spv", static_cast<unsigned long long>(hash));
    return cacheFolder / name;
}

GraphShaderResult materialGraphShader(const MaterialGraphCode& code, const fs::path& cacheFolder, bool allowCompile) {
    const auto start = std::chrono::steady_clock::now();
    GraphShaderResult r;
    const std::string source = materialGraphShaderSource(code);
    r.hash = materialGraphHash(source, engineSurfaceShaderHash());
    r.file = materialGraphShaderFile(cacheFolder, r.hash);
    auto finish = [&] {
        r.milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        return r;
    };
    if (readWords(r.file, r.spirv)) {
        r.ok = r.fromCache = true;
        return finish();
    }
    if (!allowCompile) {
        r.error = "its compiled shader " + r.file.filename().string() + " isn't in the package (cook the game again)";
        return finish();
    }
    const fs::path tool = shaderCompilerTool();
    if (tool.empty()) {
        r.error = "compiling material graphs needs glslangValidator, which Shockwave wasn't configured with "
                  "(install glslang, or set SHOCKWAVE_GLSLANG to it)";
        return finish();
    }
    const fs::path sources = engineShaderSources();
    std::error_code ec;
    if (sources.empty() || !fs::is_regular_file(sources / "mesh.glsl", ec)) {
        r.error = "compiling material graphs needs the engine's shader sources (mesh.glsl), which aren't at " +
                  sources.string();
        return finish();
    }
    fs::create_directories(cacheFolder, ec);
    // Names of this call's own, so programs compiling the same graph at once (tests in
    // parallel, the editor beside --check) never write each other's files.
    static std::atomic<uint64_t> calls{0};
    char unique[64];
    std::snprintf(
        unique, sizeof(unique), "-%llx-%llx",
        static_cast<unsigned long long>(std::chrono::steady_clock::now().time_since_epoch().count()),
        static_cast<unsigned long long>(calls.fetch_add(1) + std::hash<std::thread::id>{}(std::this_thread::get_id())));
    const fs::path stem = fs::path(r.file).replace_extension().string() + unique;
    const fs::path frag = fs::path(stem.string() + ".frag");
    const fs::path temporary = fs::path(stem.string() + ".spv.tmp");
    {
        std::ofstream out(frag, std::ios::binary);
        out << source;
        if (!out) {
            r.error = "can't write " + frag.string();
            return finish();
        }
    }
    ProcessOptions run;
    run.executable = tool;
    run.arguments = {"-V", "--target-env",     "vulkan1.3",  "-I" + sources.string(),
                     "-o", temporary.string(), frag.string()};
    run.timeoutSeconds = 60;
    std::vector<std::string> output;
    run.onLine = [&](std::string_view line) { output.emplace_back(line); };
    const ProcessResult result = runProcess(run);
    if (!result.ok()) {
        // "ERROR: <file>:<line>: <message>": lines in the generated part name their node.
        std::vector<std::string> lines;
        std::istringstream in(source);
        for (std::string line; std::getline(in, line);) lines.push_back(line);
        const std::string fragName = frag.filename().string();
        for (const std::string& line : output) {
            if (line.rfind("ERROR:", 0) != 0 || line.find("compilation terminated") != std::string::npos) continue;
            std::string message = line.substr(6);
            const size_t at = message.find(fragName + ":");
            std::string node;
            if (at != std::string::npos) {
                const size_t number = at + fragName.size() + 1;
                node = nodeOfLine(lines, std::atoi(message.c_str() + number));
                const size_t colon = message.find(':', number);
                if (colon != std::string::npos) message = message.substr(colon + 1);
            }
            while (!message.empty() && message.front() == ' ') message.erase(message.begin());
            r.error += (r.error.empty() ? "" : "; ") + (node.empty() ? message : "node '" + node + "': " + message);
        }
        if (r.error.empty())
            r.error = result.started ? "glslangValidator failed (exit " + std::to_string(result.exitCode) + ")"
                                     : "can't run " + tool.string() + ": " + result.error;
        fs::remove(temporary, ec);
        fs::remove(frag, ec);
        return finish();
    }
    fs::remove(frag, ec);
    if (!readWords(temporary, r.spirv)) {
        fs::remove(temporary, ec);
        r.error = "glslangValidator wrote no SPIR-V";
        return finish();
    }
    fs::rename(temporary, r.file, ec);
    if (ec && fs::is_regular_file(r.file)) {   // another program got there first (Windows won't replace)
        fs::remove(temporary, ec);
        ec.clear();
    }
    if (ec) {
        r.error = "can't write " + r.file.string() + ": " + ec.message();
        return finish();
    }
    r.ok = true;
    return finish();
}

}   // namespace shockwave
