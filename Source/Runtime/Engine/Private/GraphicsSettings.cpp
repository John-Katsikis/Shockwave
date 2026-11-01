#include <Engine/GraphicsSettings.h>

#include <Engine/Engine.h>
#include <Engine/GraphicsBudget.h>
#include <Core/Log.h>
#include <Projects/Project.h>

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <set>
#include <sstream>

namespace shockwave {
namespace {
template <class T>
bool number(const std::string& text, T& value) {
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    return ec == std::errc{} && end == text.data() + text.size() && std::isfinite(value);
}
template <class T>
std::string numberText(T value) {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::setprecision(std::numeric_limits<T>::max_digits10) << value;
    return out.str();
}
// An enum stored by its index into choices.
template <class E>
bool choice(const std::vector<const char*>& choices, const std::string& value, E& out) {
    for (size_t i = 0; i < choices.size(); ++i)
        if (value == choices[i]) {
            out = static_cast<E>(i);
            return true;
        }
    return false;
}
std::string list(const std::vector<const char*>& choices) {
    std::string text;
    for (size_t i = 0; i < choices.size(); ++i)
        text += (i ? (i + 1 == choices.size() ? " or " : ", ") : "") + std::string(choices[i]);
    return text;
}
const std::vector<const char*> kBool = {"true", "false"};
const std::vector<const char*> kShadowChoices = {"off", "low", "medium", "high", "ultra"};
const std::vector<const char*> kAntiAliasingChoices = {"none", "fxaa", "taa"};
const std::vector<const char*> kBloomChoices = {"off", "low", "medium", "high"};
const std::vector<const char*> kTextureQualityChoices = {"low", "medium", "high", "ultra"};
const std::vector<const char*> kAnisotropyChoices = {"1", "2", "4", "8", "16"};
const std::vector<const char*> kMaterialQualityChoices = {"low", "high"};
const std::vector<const char*> kAmbientOcclusionChoices = {"off", "low", "medium", "high"};
const std::vector<const char*> kDecalChoices = {"off", "low", "medium", "high"};
const std::vector<const char*> kLightCullingChoices = {"auto", "clustered", "brute_force"};
const std::vector<const char*> kOcclusionChoices = {"off", "on"};
const std::vector<const char*> kParticleLightingChoices = {"simple", "scene"};
const std::vector<const char*> kFogChoices = {"off", "analytic", "low", "medium", "high"};
const std::vector<const char*> kTransmissionChoices = {"off", "low", "medium", "high"};
const std::vector<const char*> kSkinChoices = {"off", "low", "high"};
const std::vector<const char*> kHairChoices = {"low", "high"};

bool boolean(const std::string& value, bool& out, const char* key, std::string& error) {
    if (value != "true" && value != "false") {
        error = std::string(key) + " must be true or false";
        return false;
    }
    out = value == "true";
    return true;
}

std::vector<GraphicsSettingInfo> makeInfo() {
    using S = GraphicsSettings;
    std::vector<GraphicsSettingInfo> info;
    info.push_back(
        {"render_scale",
         "Render scale",
         "Quality",
         "Scene resolution as a multiple of the output. Below 1 is faster and softer; above 1 supersamples. "
         "Game UI always draws at output resolution.",
         "0.25..2",
         {},
         GraphicsApply::Targets,
         true,
         [](const S& s) { return numberText(s.renderScale); },
         [](S& s, const std::string& v, std::string& error) {
             float x = 0;
             if (!number(v, x) || x < 0.25f || x > 2) {
                 error = "render_scale must be a number in 0.25..2";
                 return false;
             }
             s.renderScale = x;
             return true;
         }});
    info.push_back(
        {"shadow_quality", "Shadows", "Quality",
         "Shadow map size and filtering. Off keeps direct lighting but draws no shadow maps.",
         "off, low, medium, high, ultra", kShadowChoices, GraphicsApply::Targets, true,
         [](const S& s) { return std::string(kShadowChoices[std::min<size_t>(size_t(s.shadows), 4)]); },
         [](S& s, const std::string& v, std::string& error) {
             if (choice(kShadowChoices, v, s.shadows)) return true;
             error = "shadow_quality must be " + list(kShadowChoices);
             return false;
         }});
    info.push_back(
        {"anti_aliasing", "Anti-aliasing", "Quality",
         "Smooths jagged edges. FXAA is a fast pass after tone mapping. TAA jitters the camera and blends "
         "each frame with the last ones: steadier edges and fine detail, at the cost of some softness and "
         "ghosting on fast motion. Neither touches game UI, and they're never stacked.",
         "none, fxaa, taa", kAntiAliasingChoices, GraphicsApply::Targets, true,
         [](const S& s) { return std::string(antiAliasingName(s.antiAliasing)); },
         [](S& s, const std::string& v, std::string& error) {
             if (choice(kAntiAliasingChoices, v, s.antiAliasing)) return true;
             error = "anti_aliasing must be " + list(kAntiAliasingChoices);
             return false;
         }});
    info.push_back(
        {"bloom_quality", "Bloom", "Quality",
         "How finely bloom is filtered: low starts at quarter resolution with 4 levels, medium at half with 5, "
         "high at half with 6. Off skips bloom and frees its memory. The scene's bloom intensity stays as authored.",
         "off, low, medium, high", kBloomChoices, GraphicsApply::Targets, true,
         [](const S& s) { return std::string(bloomQualityName(s.bloomQuality)); },
         [](S& s, const std::string& v, std::string& error) {
             if (choice(kBloomChoices, v, s.bloomQuality)) return true;
             error = "bloom_quality must be " + list(kBloomChoices);
             return false;
         }});
    info.push_back(
        {"bloom_scale",
         "Bloom strength",
         "Quality",
         "Your multiplier on each scene's bloom intensity: 1 is as the artist set it, 0 turns bloom off.",
         "0..1",
         {},
         GraphicsApply::Uniform,
         false,
         [](const S& s) { return numberText(s.bloomScale); },
         [](S& s, const std::string& v, std::string& error) {
             float x = 0;
             if (!number(v, x) || x < 0 || x > 1) {
                 error = "bloom_scale must be a number in 0..1";
                 return false;
             }
             s.bloomScale = x;
             return true;
         }});
    info.push_back(
        {"ambient_occlusion", "Ambient occlusion", "Quality",
         "Darkens the light from the surroundings in creases, corners and under objects, worked out on the "
         "screen at half resolution: low takes 8 samples a pixel, medium 16, high 32. Direct light, shadows and "
         "glowing surfaces are unaffected. Off skips it and frees its memory.",
         "off, low, medium, high", kAmbientOcclusionChoices, GraphicsApply::Targets, true,
         [](const S& s) { return std::string(ambientOcclusionName(s.ambientOcclusion)); },
         [](S& s, const std::string& v, std::string& error) {
             if (choice(kAmbientOcclusionChoices, v, s.ambientOcclusion)) return true;
             error = "ambient_occlusion must be " + list(kAmbientOcclusionChoices);
             return false;
         }});
    info.push_back(
        {"baked_lighting", "Baked lighting", "Quality",
         "Light bounced around the scene, baked ahead of time into lightmaps and probe volumes. Off lights "
         "everything from the sky and environment alone. Costs one texture read a pixel; scenes without a bake "
         "look the same either way.",
         "true, false", kBool, GraphicsApply::Uniform, false,
         [](const S& s) { return std::string(s.bakedLighting ? "true" : "false"); },
         [](S& s, const std::string& v, std::string& error) {
             return boolean(v, s.bakedLighting, "baked_lighting", error);
         }});
    info.push_back(
        {"reflection_probes", "Reflection probes", "Quality",
         "Reflections captured inside rooms and corridors, blended over the sky's where a scene places reflection "
         "probes. Off reflects the sky and environment everywhere. Costs up to two cube reads a pixel; scenes "
         "without probes look the same either way.",
         "true, false", kBool, GraphicsApply::Uniform, false,
         [](const S& s) { return std::string(s.reflectionProbes ? "true" : "false"); },
         [](S& s, const std::string& v, std::string& error) {
             return boolean(v, s.reflectionProbes, "reflection_probes", error);
         }});
    info.push_back(
        {"decals_quality", "Decals", "Quality",
         "Marks projected onto surfaces: bullet holes, signs, grime. Low draws up to 64 a frame, medium 256, "
         "high 1024, the nearest first (gameplay markers before decoration). Off draws none and frees their "
         "memory; scenes without decals look the same either way.",
         "off, low, medium, high", kDecalChoices, GraphicsApply::Targets, true,
         [](const S& s) { return std::string(decalQualityName(s.decals)); },
         [](S& s, const std::string& v, std::string& error) {
             if (choice(kDecalChoices, v, s.decals)) return true;
             error = "decals_quality must be " + list(kDecalChoices);
             return false;
         }});
    info.push_back(
        {"light_culling", "Light culling", "Quality",
         "How each pixel finds the point and spot lights that reach it. Clustered sorts lights into a grid of "
         "screen tiles and distances first, so each pixel tests only those near it; brute force tests every "
         "light everywhere. Auto clusters when a frame has 8 or more of them. The picture is the same either "
         "way: only the cost differs.",
         "auto, clustered, brute_force", kLightCullingChoices, GraphicsApply::Uniform, false,
         [](const S& s) { return std::string(lightCullingName(s.lightCulling)); },
         [](S& s, const std::string& v, std::string& error) {
             if (choice(kLightCullingChoices, v, s.lightCulling)) return true;
             error = "light_culling must be " + list(kLightCullingChoices);
             return false;
         }});
    info.push_back(
        {"instancing", "Instancing", "Quality",
         "Draws copies of the same mesh with the same material in one call instead of one each, which saves the "
         "processor time spent issuing them. The picture is the same either way.",
         "true, false", kBool, GraphicsApply::Uniform, false,
         [](const S& s) { return std::string(s.instancing ? "true" : "false"); },
         [](S& s, const std::string& v, std::string& error) { return boolean(v, s.instancing, "instancing", error); }});
    info.push_back(
        {"occlusion_culling", "Occlusion culling", "Quality",
         "Skips objects the depth prepass proves are hidden behind others, deciding on the graphics card. It runs "
         "only when the prepass already does (ambient occlusion, TAA or decals on) and never hides anything that "
         "could show: the picture is the same either way.",
         "off, on", kOcclusionChoices, GraphicsApply::Uniform, false,
         [](const S& s) { return std::string(occlusionCullingName(s.occlusionCulling)); },
         [](S& s, const std::string& v, std::string& error) {
             if (choice(kOcclusionChoices, v, s.occlusionCulling)) return true;
             error = "occlusion_culling must be " + list(kOcclusionChoices);
             return false;
         }});
    info.push_back(
        {"graphics_memory_budget_mb",
         "Graphics memory budget",
         "Quality",
         "The most memory optional effects may take, in megabytes (MiB). Over it, effects are turned down one at a "
         "time, TAA's history first, until they fit; your settings stay as you chose them. 0 means no budget.",
         "0 (no budget) or 16..65536",
         {},
         GraphicsApply::Uniform,
         false,
         [](const S& s) { return std::to_string(s.memoryBudgetMb); },
         [](S& s, const std::string& v, std::string& error) {
             double x = 0;
             if (!number(v, x) || x != double(uint32_t(x)) || (x != 0 && (x < 16 || x > 65536))) {
                 error = "graphics_memory_budget_mb must be 0 (no budget) or a whole number in 16..65536";
                 return false;
             }
             s.memoryBudgetMb = uint32_t(x);
             return true;
         }});
    info.push_back(
        {"soft_particles", "Soft particles", "Quality",
         "Smoke, dust and glows fade out where they meet walls and floors, and as they reach the camera, "
         "instead of cutting a hard line through them. Effects choose how soft. Off draws the hard edges and "
         "saves a depth read a particle pixel.",
         "true, false", kBool, GraphicsApply::Uniform, true,
         [](const S& s) { return std::string(s.softParticles ? "true" : "false"); },
         [](S& s, const std::string& v, std::string& error) {
             return boolean(v, s.softParticles, "soft_particles", error);
         }});
    info.push_back(
        {"particle_distortion", "Heat haze", "Quality",
         "Effects that bend the scene behind them: heat above fire, shock waves. Off skips those parts of an "
         "effect, with no copy of the scene and no memory for it.",
         "true, false", kBool, GraphicsApply::Targets, true,
         [](const S& s) { return std::string(s.particleDistortion ? "true" : "false"); },
         [](S& s, const std::string& v, std::string& error) {
             return boolean(v, s.particleDistortion, "particle_distortion", error);
         }});
    info.push_back(
        {"particle_lighting", "Particle lighting", "Quality",
         "How lit particles (smoke, dust) are lit. Scene lights them by every light, so a muzzle flash or a "
         "lamp lights the smoke around it; simple uses the sun and the ambient light alone, which costs less.",
         "simple, scene", kParticleLightingChoices, GraphicsApply::Uniform, true,
         [](const S& s) { return std::string(particleLightingName(s.particleLighting)); },
         [](S& s, const std::string& v, std::string& error) {
             if (choice(kParticleLightingChoices, v, s.particleLighting)) return true;
             error = "particle_lighting must be " + list(kParticleLightingChoices);
             return false;
         }});
    info.push_back(
        {"color_grading", "Colour grading", "Quality",
         "The scene's colour look from its postprocess profiles: white balance and creative LUTs. Off shows the "
         "colours ungraded, exactly as without them.",
         "true, false", kBool, GraphicsApply::Uniform, false,
         [](const S& s) { return std::string(s.colorGrading ? "true" : "false"); },
         [](S& s, const std::string& v, std::string& error) {
             return boolean(v, s.colorGrading, "color_grading", error);
         }});
    info.push_back(
        {"auto_exposure", "Auto exposure", "Quality",
         "Lets scenes that ask for it adjust their brightness to what the camera sees, like an eye adapting. Off "
         "keeps each scene's fixed exposure, with no metering pass.",
         "true, false", kBool, GraphicsApply::Uniform, false,
         [](const S& s) { return std::string(s.autoExposure ? "true" : "false"); },
         [](S& s, const std::string& v, std::string& error) {
             return boolean(v, s.autoExposure, "auto_exposure", error);
         }});
    info.push_back(
        {"fog_quality", "Fog", "Quality",
         "How scenes with fog draw it. Analytic is the cheap closed form, lit by the sun; low, medium and high add a "
         "volume near the camera lit by every lamp, with the sun's shadows, at coarser to finer detail. Off draws "
         "no fog, with no passes or memory for it.",
         "off, analytic, low, medium, high", kFogChoices, GraphicsApply::Targets, true,
         [](const S& s) { return std::string(fogQualityName(s.fogQuality)); },
         [](S& s, const std::string& v, std::string& error) {
             if (choice(kFogChoices, v, s.fogQuality)) return true;
             error = "fog_quality must be " + list(kFogChoices);
             return false;
         }});
    info.push_back(
        {"clearcoat", "Clear coats", "Quality",
         "The clear varnish layer on materials that have one (car paint, lacquered wood): a second, sharper "
         "highlight over the surface. Off draws the surface under it, exactly as without the coat.",
         "true, false", kBool, GraphicsApply::Uniform, true,
         [](const S& s) { return std::string(s.clearcoat ? "true" : "false"); },
         [](S& s, const std::string& v, std::string& error) { return boolean(v, s.clearcoat, "clearcoat", error); }});
    info.push_back(
        {"transmission_quality", "Glass", "Quality",
         "How glass and other see-through materials show what's behind them. Low blurs it through rough glass; "
         "medium also bends it through thick glass; high blurs rough glass more smoothly. Off draws simplified "
         "tinted glass, with no copy of the scene and no memory for it.",
         "off, low, medium, high", kTransmissionChoices, GraphicsApply::Targets, true,
         [](const S& s) { return std::string(transmissionQualityName(s.transmissionQuality)); },
         [](S& s, const std::string& v, std::string& error) {
             if (choice(kTransmissionChoices, v, s.transmissionQuality)) return true;
             error = "transmission_quality must be " + list(kTransmissionChoices);
             return false;
         }});
    info.push_back(
        {"skin_quality", "Skin", "Quality",
         "Light spreading softly under skin, so faces don't look like plastic. Low blurs it with fewer samples. "
         "Off draws skin as an ordinary opaque surface, with no pass and no memory for it.",
         "off, low, high", kSkinChoices, GraphicsApply::Targets, true,
         [](const S& s) { return std::string(skinQualityName(s.skinQuality)); },
         [](S& s, const std::string& v, std::string& error) {
             if (choice(kSkinChoices, v, s.skinQuality)) return true;
             error = "skin_quality must be " + list(kSkinChoices);
             return false;
         }});
    info.push_back(
        {"hair_quality", "Hair", "Quality",
         "Hair cards' highlight. High draws it as a band across the strands, the way hair shines; low draws the round "
         "highlight of an ordinary surface.",
         "low, high", kHairChoices, GraphicsApply::Uniform, true,
         [](const S& s) { return std::string(hairQualityName(s.hairQuality)); },
         [](S& s, const std::string& v, std::string& error) {
             if (choice(kHairChoices, v, s.hairQuality)) return true;
             error = "hair_quality must be " + list(kHairChoices);
             return false;
         }});
    info.push_back(
        {"lod_error_pixels",
         "Mesh detail",
         "Quality",
         "How far, in rendered pixels, a simpler mesh level may differ from the full one. 0 always draws full "
         "detail. Needs imported levels of detail.",
         "0..100",
         {},
         GraphicsApply::Uniform,
         true,
         [](const S& s) { return numberText(s.lodErrorPixels); },
         [](S& s, const std::string& v, std::string& error) {
             float x = 0;
             if (!number(v, x) || x < 0 || x > 100) {
                 error = "lod_error_pixels must be a number in 0..100";
                 return false;
             }
             s.lodErrorPixels = x;
             return true;
         }});
    info.push_back(
        {"texture_quality", "Textures", "Quality",
         "How much of each material texture stays on the GPU: ultra keeps full size, high drops the largest mip "
         "level (a quarter of the memory), medium two, low three. UI textures always keep full size.",
         "low, medium, high, ultra", kTextureQualityChoices, GraphicsApply::Targets, true,
         [](const S& s) { return std::string(textureQualityName(s.textureQuality)); },
         [](S& s, const std::string& v, std::string& error) {
             if (choice(kTextureQualityChoices, v, s.textureQuality)) return true;
             error = "texture_quality must be " + list(kTextureQualityChoices);
             return false;
         }});
    info.push_back(
        {"material_quality", "Material detail", "Quality",
         "High draws every material layer. Low skips detail layers (the close-up micro texture some materials "
         "add) to save texture reads; base materials and second layers are unchanged.",
         "low, high", kMaterialQualityChoices, GraphicsApply::Uniform, true,
         [](const S& s) { return std::string(materialQualityName(s.materialQuality)); },
         [](S& s, const std::string& v, std::string& error) {
             if (choice(kMaterialQualityChoices, v, s.materialQuality)) return true;
             error = "material_quality must be " + list(kMaterialQualityChoices);
             return false;
         }});
    info.push_back(
        {"anisotropy", "Texture filtering", "Quality",
         "Anisotropic filtering: the most samples a texture seen at a grazing angle may take. Higher keeps floors "
         "and roads sharp into the distance. Nearest-filtered (pixel art) textures are unaffected.",
         "1, 2, 4, 8, 16", kAnisotropyChoices, GraphicsApply::Pipeline, true,
         [](const S& s) { return std::to_string(s.anisotropy); },
         [](S& s, const std::string& v, std::string& error) {
             for (const char* c : kAnisotropyChoices)
                 if (v == c) {
                     s.anisotropy = uint32_t(std::stoul(v));
                     return true;
                 }
             error = "anisotropy must be " + list(kAnisotropyChoices);
             return false;
         }});
    info.push_back(
        {"vsync", "VSync", "Display", "Synchronize presentation with the display, so frames never tear.", "true, false",
         kBool, GraphicsApply::Targets, false, [](const S& s) { return std::string(s.vsync ? "true" : "false"); },
         [](S& s, const std::string& v, std::string& error) { return boolean(v, s.vsync, "vsync", error); }});
    info.push_back(
        {"frame_limit",
         "Frame limit",
         "Display",
         "Frames per second at most; 0 means uncapped apart from VSync.",
         "0 or 10..1000",
         {},
         GraphicsApply::Uniform,
         false,
         [](const S& s) { return numberText(s.frameLimit); },
         [](S& s, const std::string& v, std::string& error) {
             double x = 0;
             if (!number(v, x) || x < 0 || (x > 0 && x < 10) || x > 1000) {
                 error = "frame_limit must be 0 (uncapped) or in 10..1000";
                 return false;
             }
             s.frameLimit = x;
             return true;
         }});
    info.push_back(
        {"low_latency", "Low latency", "Display",
         "Wait for each frame to reach the screen before reading input for the next, so input feels more "
         "immediate. Costs some frame rate when the GPU is the bottleneck.",
         "true, false", kBool, GraphicsApply::Uniform, false,
         [](const S& s) { return std::string(s.lowLatency ? "true" : "false"); },
         [](S& s, const std::string& v, std::string& error) {
             return boolean(v, s.lowLatency, "low_latency", error);
         }});
    return info;
}
}   // namespace

const std::vector<GraphicsSettingInfo>& graphicsSettingInfo() {
    static const std::vector<GraphicsSettingInfo> info = makeInfo();
    return info;
}
const GraphicsSettingInfo* findGraphicsSetting(const std::string& key) {
    for (const auto& info : graphicsSettingInfo())
        if (key == info.key) return &info;
    return nullptr;
}
bool setGraphicsSetting(
    GraphicsSettings& settings, const std::string& key, const std::string& value, std::string& error) {
    const GraphicsSettingInfo* info = findGraphicsSetting(key);
    if (!info) {
        error = "unknown graphics setting " + key;
        return false;
    }
    GraphicsSettings next = settings;
    if (!info->set(next, value, error)) return false;
    settings = next;
    error.clear();
    return true;
}
const char* graphicsApplyName(GraphicsApply apply) {
    switch (apply) {
    case GraphicsApply::Uniform: return "uniform";
    case GraphicsApply::Pipeline: return "pipeline";
    case GraphicsApply::Targets: return "targets";
    case GraphicsApply::Restart: return "restart";
    }
    return "?";
}
const char* graphicsPresetName(GraphicsPreset preset) {
    constexpr const char* names[] = {"low", "medium", "high", "ultra"};
    return names[std::min<size_t>(size_t(preset), 3)];
}
GraphicsSettings graphicsPreset(GraphicsPreset preset, GraphicsSettings base) {
    // Every preset smooths edges with FXAA: it costs little. TAA (G10) stays the player's choice.
    base.antiAliasing = AntiAliasing::Fxaa;
    switch (preset) {
    case GraphicsPreset::Low:
        base.renderScale = 0.67f;
        base.shadows = ShadowQuality::Low;
        base.lodErrorPixels = 2;
        base.bloomQuality = BloomQuality::Low;
        base.textureQuality = TextureQuality::Low;
        base.anisotropy = 4;
        base.materialQuality = MaterialQuality::Low;
        base.ambientOcclusion = AmbientOcclusion::Off;
        base.decals = DecalQuality::Low;
        base.softParticles = false;
        base.particleDistortion = false;
        base.particleLighting = ParticleLighting::Simple;
        base.fogQuality = FogQuality::Analytic;
        base.clearcoat = true;   // one more lobe, no pass
        base.transmissionQuality = TransmissionQuality::Low;
        base.skinQuality = SkinQuality::Off;
        base.hairQuality = HairQuality::Low;
        break;
    case GraphicsPreset::Medium:
        base.renderScale = 0.85f;
        base.shadows = ShadowQuality::Medium;
        base.lodErrorPixels = 1.5f;
        base.bloomQuality = BloomQuality::Medium;
        base.textureQuality = TextureQuality::Medium;
        base.anisotropy = 8;
        base.materialQuality = MaterialQuality::High;
        base.ambientOcclusion = AmbientOcclusion::Low;
        base.decals = DecalQuality::Medium;
        base.softParticles = true;
        base.particleDistortion = true;
        base.particleLighting = ParticleLighting::Scene;
        base.fogQuality = FogQuality::Low;
        base.clearcoat = true;
        base.transmissionQuality = TransmissionQuality::Medium;
        base.skinQuality = SkinQuality::Low;
        base.hairQuality = HairQuality::High;
        break;
    case GraphicsPreset::High:
        base.renderScale = 1;
        base.shadows = ShadowQuality::High;
        base.lodErrorPixels = 1;
        base.bloomQuality = BloomQuality::High;
        base.textureQuality = TextureQuality::High;
        base.anisotropy = 16;
        base.materialQuality = MaterialQuality::High;
        base.ambientOcclusion = AmbientOcclusion::Medium;
        base.decals = DecalQuality::High;
        base.softParticles = true;
        base.particleDistortion = true;
        base.particleLighting = ParticleLighting::Scene;
        base.fogQuality = FogQuality::Medium;
        base.clearcoat = true;
        base.transmissionQuality = TransmissionQuality::Medium;
        base.skinQuality = SkinQuality::High;
        base.hairQuality = HairQuality::High;
        break;
    case GraphicsPreset::Ultra:
        base.renderScale = 1;
        base.shadows = ShadowQuality::Ultra;
        base.lodErrorPixels = 0.5f;
        base.bloomQuality = BloomQuality::High;
        base.textureQuality = TextureQuality::Ultra;
        base.anisotropy = 16;
        base.materialQuality = MaterialQuality::High;
        base.ambientOcclusion = AmbientOcclusion::High;
        base.decals = DecalQuality::High;
        base.softParticles = true;
        base.particleDistortion = true;
        base.particleLighting = ParticleLighting::Scene;
        base.fogQuality = FogQuality::High;
        base.clearcoat = true;
        base.transmissionQuality = TransmissionQuality::High;
        base.skinQuality = SkinQuality::High;
        base.hairQuality = HairQuality::High;
        break;
    }
    return base;
}
std::optional<GraphicsPreset> matchingGraphicsPreset(const GraphicsSettings& settings) {
    for (int i = 0; i < 4; ++i) {
        const auto preset = static_cast<GraphicsPreset>(i);
        const GraphicsSettings candidate = graphicsPreset(preset, settings);
        bool same = true;
        for (const auto& info : graphicsSettingInfo())
            if (info.preset && info.get(candidate) != info.get(settings)) same = false;
        if (same) return preset;
    }
    return std::nullopt;
}
bool validateGraphicsSettings(const GraphicsSettings& s, std::string& error) {
    // Each value must survive its own text form: that's the range check, in one place.
    for (const auto& info : graphicsSettingInfo()) {
        GraphicsSettings copy = s;
        if (!info.set(copy, info.get(s), error)) return false;
    }
    error.clear();
    return true;
}
bool loadGraphicsSettings(const std::filesystem::path& file, GraphicsSettings& out, std::string& error) {
    error.clear();
    std::ifstream in(file);
    if (!in) {
        error = file.string() + ": can't open graphics settings";
        return false;
    }
    GraphicsSettings result = out;
    std::string line;
    int lineNumber = 0;
    bool header = false;
    std::set<std::string> seen;
    auto fail = [&](const std::string& message) {
        error = file.string() + ":" + std::to_string(lineNumber) + ": " + message;
        return false;
    };
    while (std::getline(in, line)) {
        ++lineNumber;
        if (auto hash = line.find('#'); hash != std::string::npos) line.erase(hash);
        std::istringstream row(line);
        std::string key, value, extra;
        if (!(row >> key)) continue;
        if (!(row >> value) || (row >> extra)) return fail("expected a key and one value");
        if (!header) {
            if (key != "swgraphics" || value != "1") return fail("expected swgraphics 1");
            header = true;
            continue;
        }
        if (!seen.insert(key).second) return fail("duplicate setting " + key);
        std::string problem;
        if (!setGraphicsSetting(result, key, value, problem)) return fail(problem);
    }
    if (!header) return fail("expected swgraphics 1");
    if (in.bad()) return fail("failed reading graphics settings");
    if (!validateGraphicsSettings(result, error)) return false;
    out = result;
    return true;
}
std::string graphicsSettingsText(const GraphicsSettings& s) {
    std::string text = "swgraphics 1\n";
    for (const auto& info : graphicsSettingInfo()) text += std::string(info.key) + " " + info.get(s) + "\n";
    return text;
}
bool saveGraphicsSettings(const std::filesystem::path& file, const GraphicsSettings& s, std::string& error) {
    if (!validateGraphicsSettings(s, error)) return false;
    if (file.empty()) {
        error = "No user settings file: run the standalone player to save preferences";
        return false;
    }
    std::error_code ec;
    if (!file.parent_path().empty()) std::filesystem::create_directories(file.parent_path(), ec);
    if (ec) {
        error = file.string() + ": " + ec.message();
        return false;
    }
    const auto temp = std::filesystem::path(
        file.string() + "." + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".tmp");
    {
        std::ofstream out(temp, std::ios::trunc);
        out << graphicsSettingsText(s);
        out.close();
        if (!out) {
            std::filesystem::remove(temp, ec);
            error = file.string() + ": couldn't write graphics settings";
            return false;
        }
    }
    std::filesystem::rename(temp, file, ec);
    if (ec) {
        error = file.string() + ": " + ec.message();
        std::filesystem::remove(temp, ec);
        return false;
    }
    error.clear();
    return true;
}
std::string graphicsSettingsSchema() {
    std::string text = R"(# Graphics settings schema

Generated by `player --graphics-schema docs/GraphicsSchema.md`.

Files start with `swgraphics 1`. One key/value per line; `#` starts a comment.
Unknown keys, duplicate keys, non-finite numbers and unsupported versions are errors.
Omitted keys inherit the preceding layer: project manifest, Config/Graphics.swgraphics,
user preferences, then command-line overrides. See [Graphics](Graphics.md).

Presets (low, medium, high, ultra) set the keys marked *preset* and keep the others.
*Applies* says how a change reaches the screen: `uniform` next frame; `targets` once new
images are made, the old ones freed after the frames using them finish. No setting needs a
restart, and none waits for the GPU to go idle except where noted in Graphics.md.

| Key | Values | Built-in default | Preset | Applies | Meaning |
|---|---|---|---|---|---|
)";
    const GraphicsSettings defaults;
    for (const auto& info : graphicsSettingInfo())
        text += std::string("| ") + info.key + " | " + info.values + " | " + info.get(defaults) + " | " +
                (info.preset ? "preset" : "") + " | " + graphicsApplyName(info.apply) + " | " + info.tooltip + " |\n";
    return text;
}
bool projectGraphicsSettings(const Project& project, GraphicsSettings& out, std::string& error) {
    GraphicsSettings settings;
    settings.shadows = static_cast<ShadowQuality>(project.shadowQuality);
    settings.lodErrorPixels = static_cast<float>(project.lodErrorPixels);
    settings.vsync = project.vsync;
    settings.frameLimit = project.frameLimit;
    settings.lowLatency = project.lowLatency;
    const auto file = project.root / "Config" / "Graphics.swgraphics";
    std::error_code ec;
    const bool exists = std::filesystem::exists(file, ec);
    if (ec) {
        error = file.string() + ": " + ec.message();
        return false;
    }
    if ((exists && !loadGraphicsSettings(file, settings, error)) || !validateGraphicsSettings(settings, error))
        return false;
    out = settings;
    return true;
}
GraphicsSettings Graphics::current() const {
    if (!m_engine) return m_headless;
    // What the renderer runs, with the player's own values back where the memory budget
    // turned something down: menus and saved files show what was chosen.
    GraphicsSettings s = running();
    s.memoryBudgetMb = m_wanted.memoryBudgetMb;
    std::string error;
    for (const EffectiveSetting& drop : m_drops)
        if (const GraphicsSettingInfo* info = findGraphicsSetting(drop.name)) info->set(s, drop.requested, error);
    return s;
}
GraphicsSettings Graphics::running() const {
    if (!m_engine) return m_headless;
    const auto& r = m_engine->renderer();
    GraphicsSettings s;
    s.renderScale = r.renderScale();
    s.shadows = r.shadowQuality();
    s.lodErrorPixels = r.lodSettings().maxErrorPixels;
    s.vsync = r.vsync();
    s.frameLimit = m_engine->frameLimit();
    s.lowLatency = m_engine->lowLatency();
    s.antiAliasing = r.antiAliasing();
    s.bloomQuality = r.bloomQuality();
    s.bloomScale = r.bloomScale();
    s.textureQuality = r.textureQuality();
    s.anisotropy = r.anisotropy();
    s.materialQuality = r.materialQuality();
    s.ambientOcclusion = r.ambientOcclusion();
    s.bakedLighting = r.bakedLighting();
    s.reflectionProbes = r.localReflections();
    s.decals = r.decalQuality();
    s.lightCulling = r.lightCulling();
    const ParticleSettings particles = r.particleSettings();
    s.softParticles = particles.softParticles;
    s.particleDistortion = particles.distortion;
    s.particleLighting = particles.lighting;
    const PostProcessSettings post = r.postProcessSettings();
    s.colorGrading = post.colorGrading;
    s.autoExposure = post.autoExposure;
    s.fogQuality = post.fog;
    const ShadingSettings shading = r.shadingSettings();
    s.clearcoat = shading.clearcoat;
    s.transmissionQuality = shading.transmission;
    s.skinQuality = shading.skin;
    s.hairQuality = shading.hair;
    const DrawSettings drawing = r.drawSettings();
    s.instancing = drawing.instancing;
    s.occlusionCulling = drawing.occlusion;
    return s;
}
GraphicsSettings Graphics::requested() const { return m_pending.value_or(current()); }
bool Graphics::apply(const GraphicsSettings& s, std::string& error) {
    if (!validateGraphicsSettings(s, error)) return false;
    ++m_generation;
    if (m_engine)
        m_pending = s;
    else
        m_headless = s;
    return true;
}
Graphics::Status Graphics::status() const {
    Status status;
    status.generation = m_generation;
    if (m_pending) {
        status.state = State::Pending;
        return status;
    }
    if (!m_engine) return status;
    for (EffectiveSetting& setting : m_engine->renderer().effectiveSettings()) {
        // Rounding the scene to whole pixels isn't a fallback.
        if (setting.reason.empty() || setting.name == "render_scale") continue;
        if (setting.reason.rfind("pending", 0) == 0) status.state = State::Pending;
        status.fallbacks.push_back(std::move(setting));
    }
    for (const EffectiveSetting& drop : m_drops) status.fallbacks.push_back(drop);
    if (status.state != State::Pending && !status.fallbacks.empty()) status.state = State::AppliedWithFallback;
    return status;
}
bool Graphics::configure(const GraphicsSettings& defaults, std::filesystem::path file, std::string& error) {
    if (!validateGraphicsSettings(defaults, error)) return false;
    m_defaults = defaults;
    m_file = std::move(file);
    return true;
}
bool Graphics::save(const GraphicsSettings& s, std::string& error) const {
    return saveGraphicsSettings(m_file, s, error);
}
void Graphics::commit() {
    if (!m_pending || !m_engine) return;
    const auto s = *m_pending;
    m_pending.reset();
    const auto previous = running();
    // A new choice starts the budget over: everything as asked, then fitted again.
    m_wanted = s;
    m_drops.clear();
    m_overBudget = false;
    m_budgetWait = 1;
    push(s, previous);
}
void Graphics::push(const GraphicsSettings& s, const GraphicsSettings& previous) {
    auto& r = m_engine->renderer();
    if (s.renderScale != previous.renderScale) r.setRenderScale(s.renderScale);
    if (s.shadows != previous.shadows) r.setShadowQuality(s.shadows);
    if (s.antiAliasing != previous.antiAliasing) r.setAntiAliasing(s.antiAliasing);
    if (s.bloomQuality != previous.bloomQuality || s.bloomScale != previous.bloomScale)
        r.setBloom(s.bloomQuality, s.bloomScale);
    if (s.textureQuality != previous.textureQuality) r.setTextureQuality(s.textureQuality);
    if (s.anisotropy != previous.anisotropy) r.setAnisotropy(s.anisotropy);
    if (s.materialQuality != previous.materialQuality) r.setMaterialQuality(s.materialQuality);
    if (s.ambientOcclusion != previous.ambientOcclusion) r.setAmbientOcclusion(s.ambientOcclusion);
    if (s.bakedLighting != previous.bakedLighting) r.setBakedLighting(s.bakedLighting);
    if (s.reflectionProbes != previous.reflectionProbes) r.setLocalReflections(s.reflectionProbes);
    if (s.decals != previous.decals) r.setDecalQuality(s.decals);
    if (s.lightCulling != previous.lightCulling) r.setLightCulling(s.lightCulling);
    if (s.softParticles != previous.softParticles || s.particleDistortion != previous.particleDistortion ||
        s.particleLighting != previous.particleLighting)
        r.setParticleSettings({s.softParticles, s.particleDistortion, s.particleLighting});
    if (s.colorGrading != previous.colorGrading || s.autoExposure != previous.autoExposure ||
        s.fogQuality != previous.fogQuality)
        r.setPostProcessSettings({s.colorGrading, s.autoExposure, s.fogQuality});
    if (s.clearcoat != previous.clearcoat || s.transmissionQuality != previous.transmissionQuality ||
        s.skinQuality != previous.skinQuality || s.hairQuality != previous.hairQuality)
        r.setShadingSettings({s.clearcoat, s.transmissionQuality, s.skinQuality, s.hairQuality});
    if (s.instancing != previous.instancing || s.occlusionCulling != previous.occlusionCulling)
        r.setDrawSettings({s.instancing, s.occlusionCulling});
    if (s.lodErrorPixels != previous.lodErrorPixels) {
        auto lod = r.lodSettings();
        lod.maxErrorPixels = s.lodErrorPixels;
        r.setLodSettings(lod);
    }
    if (s.vsync != previous.vsync) r.setVsync(s.vsync);
    // Refresh the smoother's period when VSync changes too.
    if (s.frameLimit != previous.frameLimit || s.vsync != previous.vsync) m_engine->setFrameLimit(s.frameLimit);
    if (s.lowLatency != previous.lowLatency) m_engine->setLowLatency(s.lowLatency);
}

void Graphics::afterFrame() {
    if (!m_engine || m_pending || m_wanted.memoryBudgetMb == 0) return;
    Renderer& r = m_engine->renderer();
    // The window's size changes what every effect costs: start over at the new size.
    const auto extent = std::pair(m_engine->window().pixelWidth(), m_engine->window().pixelHeight());
    if (extent != m_budgetExtent) {
        m_budgetExtent = extent;
        if (!m_drops.empty() || m_overBudget) {
            m_drops.clear();
            m_overBudget = false;
            push(m_wanted, running());
            m_budgetWait = 1;
            return;
        }
    }
    // Wait for the last change's images to be made and the old ones freed, then measure.
    // Once fitted, look again now and then: a scene can bring glass or skin in later.
    if (m_budgetWait > 0) {
        --m_budgetWait;
        return;
    }
    for (const EffectiveSetting& setting : r.effectiveSettings())
        if (setting.reason.rfind("pending", 0) == 0) return;
    const RendererMemoryReport memory = r.memoryReport();
    m_budgetedBytes = budgetedMemoryBytes(memory);
    const uint64_t budget = uint64_t(m_wanted.memoryBudgetMb) << 20;
    if (m_budgetedBytes <= budget || m_overBudget) {
        m_budgetWait = 60;
        return;
    }
    const GraphicsSettings now = running();
    GraphicsSettings lowered;
    const auto step = nextMemoryBudgetStep(now, memory, lowered);
    char reason[160];
    if (!step) {
        m_overBudget = true;
        std::snprintf(
            reason, sizeof(reason), "%.1f MiB with every optional effect at its lowest, over the %u MiB budget",
            double(m_budgetedBytes) / (1 << 20), m_wanted.memoryBudgetMb);
        SW_LOG(LogEngine, Warning, "graphics memory budget: {}", reason);
        m_budgetWait = 60;
        return;
    }
    const GraphicsSettingInfo* info = findGraphicsSetting(step->key);
    std::snprintf(
        reason, sizeof(reason), "memory budget: optional effects took %.1f MiB of %u",
        double(m_budgetedBytes) / (1 << 20), m_wanted.memoryBudgetMb);
    // One entry a setting: a second step on the same one updates it.
    auto drop =
        std::find_if(m_drops.begin(), m_drops.end(), [&](const EffectiveSetting& d) { return d.name == step->key; });
    if (drop == m_drops.end()) drop = m_drops.insert(m_drops.end(), {step->key, info->get(m_wanted), "", "", ""});
    drop->effective = step->value;
    drop->reason = reason;
    SW_LOG(
        LogEngine, Info, "graphics memory budget: {} {} -> {} ({})", step->key, drop->requested, step->value, reason);
    push(lowered, now);
    m_budgetWait = 1;
}
}   // namespace shockwave
