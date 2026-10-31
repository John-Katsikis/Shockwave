#pragma once

// Postprocess volumes and profiles (A05, A06; docs/PostProcess.md, docs/Fog.md).
//
// A .swpostprocess file is a small text profile: exposure, auto exposure, white balance, a
// creative LUT, bloom and fog. It sets only the values it names; the rest come from its
// parent profile, then from the scene (the Environment's exposure and bloom) and the
// schema's defaults, which change nothing:
//
//     swpostprocess 1
//     parent "post/base.swpostprocess"
//     exposure 0.5
//     white_point 0.3457 0.3585
//     lut "post/warm.cube"
//     fog_density 0.02
//
// A PostProcessVolume applies a profile where the camera is: everywhere (unbounded) or
// inside a box placed by its Transform, fading in over blend_distance metres from the box's
// faces. Volumes blend by priority, lowest first; volumes of the same priority share by
// weight, and their group covers what's below it by its total weight, at most 1.

#include <Engine/World.h>
#include <Renderer/PostProcess.h>

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace shockwave {

class Renderer;
struct Environment;

struct PostProcessVolume {
    std::string profile;   // .swpostprocess path inside Content
    bool enabled = true;
    bool unbounded = false;   // applies everywhere, at weight
    glm::vec3 halfExtents{5.0f};   // metres, before the Transform's scale
    int priority = 0;   // higher blends over lower
    float blendDistance = 1.0f;   // metres inside the faces over which it fades in; 0: a hard edge
    float weight = 1.0f;   // 0..1
};
// Registers PostProcessVolume (scenes, the Inspector, player --schema). Called with the
// engine's built-in components.
void registerPostProcessComponents();

// ---- Profiles ----
struct PostProcessValues {
    float exposure = 0.0f;   // stops (the scene's Environment exposure by default)
    bool autoExposure = false;
    float autoCompensation = 0.0f;
    float autoMinEv = -8.0f, autoMaxEv = 8.0f;
    float autoTargetGray = 0.18f;
    float autoLowPercentile = 0.5f, autoHighPercentile = 0.95f;
    float autoBrightenTime = 2.0f, autoDarkenTime = 1.0f;
    int autoMetering = int(ExposureMetering::Average);
    bool autoResetOnCut = true;
    glm::vec2 whitePoint = kD65White;
    std::string lut;   // .cube path inside Content; empty: none
    int lutEncoding = int(LutEncoding::Srgb);
    float lutIntensity = 1.0f;
    float bloomIntensity = 0.04f;   // the Environment's by default
    float fogDensity = 0.0f, fogHeightFalloff = 0.0f, fogBaseHeight = 0.0f, fogMaxDensity = 1.0f;
    glm::vec3 fogColor{0.5f, 0.6f, 0.7f};
    float fogAlbedo = 0.8f, fogAnisotropy = 0.3f;
    float fogStart = 0.0f, fogSkyDistance = 1000.0f, fogVolumetricDistance = 64.0f;
};
enum class PostProcessValueType { Float, Vec2, Color3, Bool, Enum, Path };
struct PostProcessParamInfo {
    const char* key;
    PostProcessValueType type;
    size_t offset;   // in PostProcessValues
    float min, max;   // per component; min == max: any finite value
    std::vector<const char*> options;   // Enum
    const char* group;   // exposure, auto_exposure, white_balance, lut, bloom, fog
    const char* unit;   // "stops", "1/m", ... or ""
    const char* description;
};
const std::vector<PostProcessParamInfo>& postProcessParams();
const PostProcessParamInfo* findPostProcessParam(std::string_view key);

struct PostProcessFile {
    std::string parent;   // another .swpostprocess inside Content; empty: none
    PostProcessValues values;
    uint64_t set = 0;   // bit i: postProcessParams()[i] is given
    bool isSet(std::string_view key) const;
};
inline constexpr int kMaxPostProcessDepth = 8;   // parent chain
bool isPostProcessFile(const std::filesystem::path& path);   // .swpostprocess
// Strict: an unknown key or version, a value out of range, a key given twice or anything
// left over on a line fails with "line N: ...". White points, auto exposure and fog are
// also checked as a whole.
bool parsePostProcess(std::string_view text, PostProcessFile& file, std::string& error);
std::string writePostProcess(const PostProcessFile& file);   // canonical: schema order
// The format reference (docs/PostProcessSchema.md, player --postprocess-schema).
std::string postProcessSchemaMarkdown();

// Profiles and LUTs from a Content folder, loaded the first time they're asked for and
// reloaded when their files change (checked every half second by poll). With a renderer,
// LUTs are uploaded as textures; without one (tools, tests) they're only parsed. Main thread.
class PostProcessLibrary {
public:
    explicit PostProcessLibrary(Renderer* renderer = nullptr);
    ~PostProcessLibrary();
    PostProcessLibrary(const PostProcessLibrary&) = delete;
    PostProcessLibrary& operator=(const PostProcessLibrary&) = delete;

    void setContentFolder(const std::filesystem::path& folder);
    const std::filesystem::path& contentFolder() const { return m_folder; }
    // The profile with its parents applied (set: every key any of them gives). Null with
    // the reason when it, or a parent, is missing, doesn't parse, or the parents loop.
    struct Resolved {
        PostProcessValues values;
        uint64_t set = 0;
        std::vector<std::string> chain;   // the file, then its parents
    };
    std::shared_ptr<const Resolved> profile(std::string_view name, std::string* error = nullptr);
    struct LoadedLut {
        Lut lut;
        TextureHandle texture{};
    };
    std::shared_ptr<const LoadedLut> lut(std::string_view name, std::string* error = nullptr);
    // Reloads what changed on disk; true when anything did.
    bool poll(float dt);
    // Every .swpostprocess under the folder, as names inside it.
    std::vector<std::string> profileFiles() const;

private:
    struct Entry;
    Entry& entry(std::string_view name);
    std::shared_ptr<const Resolved>
    resolve(std::string_view name, int depth, std::vector<std::string>& seen, std::string& error);
    Renderer* m_renderer = nullptr;
    std::filesystem::path m_folder;
    std::map<std::string, std::unique_ptr<Entry>, std::less<>> m_entries;
    float m_sincePoll = 0.0f;
};

// ---- Blending at the camera ----
// How much a volume applies at a point: weight, times smoothstep(0, blend, the distance
// inside its nearest face) in world metres under any scale; 0 outside.
float postProcessVolumeWeight(const PostProcessVolume& volume, const glm::mat4& transform, const glm::vec3& point);
struct PostProcessContribution {
    Entity entity;
    std::string profile;
    int priority = 0;
    float weight = 0.0f;   // its own, before blending
};
struct PostProcessResult {
    PostProcessValues values;   // blended
    float autoExposure = 0.0f;   // 0..1, blended
    struct Layer {
        std::string lut;
        LutEncoding encoding = LutEncoding::Srgb;
        float weight = 0.0f;
    };
    std::vector<Layer> luts;   // every LUT with a share, largest first
    std::vector<PostProcessContribution> volumes;   // those with weight here
    std::vector<std::string> problems;   // volumes skipped, and why
};
// Base: the scene's Environment exposure and bloom, schema defaults for the rest.
PostProcessResult evaluatePostProcess(
    const World& world, const glm::vec3& camera, PostProcessLibrary& library, const Environment& environment);
// What the renderer is given for it (LUTs past kMaxActiveLuts merged into the largest two).
PostProcessDesc postProcessDesc(const PostProcessResult& result, PostProcessLibrary& library, float dt);
// Once a frame, after setCamera: evaluates the world's volumes at the camera and hands the
// result to the renderer. Logs a problem with a volume once.
void updatePostProcess(const World& world, Renderer& renderer, PostProcessLibrary& library, float dt);

}   // namespace shockwave
