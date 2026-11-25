// The player's flags for graphics, pictures and the window: rows of the table in PlayerFlags.h.
// Each body is what the flag did in main()'s old else-if chain, messages unchanged.
#include "PlayerFlags.h"

#include <Core/Diagnostics.h>

#include <Assets/AssetFolder.h>
#include <Assets/BuiltinAssets.h>
#include <Engine/Engine.h>
#include <Engine/ReleaseTier.h>
#include <Engine/SceneFile.h>
#include <Engine/Systems.h>
#include <GameFramework/GameModule.h>
#include <GameFramework/GameModuleLibrary.h>
#include <GameFramework/PlaySession.h>
#include <GameFramework/SaveGame.h>
#include <Physics/Physics.h>
#include <Projects/Project.h>
#include <Renderer/GpuSelection.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace player {

void addGraphicsFlags(std::vector<Flag>& flags) {
    add(flags, {"--no-pipeline-warmup"}, 0, [](ParseState& s, FlagArgs&) -> std::optional<int> {
        s.noPipelineWarmup = true;
        return {};
    });

    add(flags, {"--graphics"}, 1, [](ParseState& s, FlagArgs& a) -> std::optional<int> {
        s.graphicsFile = a.next();
        return {};
    });

    add(flags, {"--graphics-schema"}, 1, [](ParseState& s, FlagArgs& a) -> std::optional<int> {
        s.graphicsSchema = a.next();
        return {};
    });

    add(flags, {"--material-schema"}, 1, [](ParseState& s, FlagArgs& a) -> std::optional<int> {
        s.materialSchema = a.next();
        return {};
    });

    add(flags, {"--graphics-info"}, 0, [](ParseState& s, FlagArgs&) -> std::optional<int> {
        s.graphicsInfo = true;
        return {};
    });

    add(flags, {"--no-user-graphics"}, 0, [](ParseState& s, FlagArgs&) -> std::optional<int> {
        s.noUserGraphics = true;
        return {};
    });

    add(flags, {"--graphics-preset"}, 1, [](ParseState& s, FlagArgs& a) -> std::optional<int> {
        s.graphicsPresetArg = a.next();
        return {};
    });

    add(flags, {"--no-pipeline-cache"}, 0, [](ParseState& s, FlagArgs&) -> std::optional<int> {
        s.noPipelineCache = true;
        return {};
    });

    add(flags, {"--clear-pipeline-cache"}, 0, [](ParseState& s, FlagArgs&) -> std::optional<int> {
        s.clearPipelineCache = true;
        return {};
    });

    add(flags, {"--graphics-set"}, 1, [](ParseState& s, FlagArgs& a) -> std::optional<int> {
        const std::string value = a.next();
        const auto equals = value.find('=');
        if (equals == std::string::npos || equals == 0)
            throw std::runtime_error("--graphics-set takes key=value, like anti_aliasing=fxaa");
        s.graphicsSets.emplace_back(value.substr(0, equals), value.substr(equals + 1));

        return {};
    });

    add(flags, {"--anti-aliasing"}, 1, [](ParseState& s, FlagArgs& a) -> std::optional<int> {
        s.graphicsSets.emplace_back("anti_aliasing", a.next());
        return {};
    });

    add(flags, {"--render-scale"}, 1, [](ParseState& s, FlagArgs& a) -> std::optional<int> {
        const std::string value = a.next();
        const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), s.renderScaleOverride);
        if (ec != std::errc{} || end != value.data() + value.size() || !std::isfinite(s.renderScaleOverride) ||
            s.renderScaleOverride < 0.25f || s.renderScaleOverride > 2)
            throw std::runtime_error("--render-scale takes a number in 0.25..2");

        return {};
    });

    add(flags, {"--renderer"}, 1, [](ParseState& s, FlagArgs& a) -> std::optional<int> {
        s.renderer = a.next();

        return {};
    });

    add(flags, {"--renderers"}, 0, [](ParseState&, FlagArgs&) -> std::optional<int> {
        for (const std::string& name : shockwave::Renderer::availableBackends()) std::puts(name.c_str());
        return 0;
    });

    add(flags, {"--gpu"}, 1, [](ParseState&, FlagArgs& a) -> std::optional<int> {
        shockwave::setRequestedGpu(a.next());

        return {};
    });

    add(flags, {"--gpus"}, 0, [](ParseState& s, FlagArgs&) -> std::optional<int> {
        s.listGpus = true;

        return {};
    });

    add(flags, {"--screenshot"}, 1, [](ParseState& s, FlagArgs& a) -> std::optional<int> {
        s.tool.screenshot = a.next();

        return {};
    });

    add(flags, {"--camera"}, 1, [](ParseState& s, FlagArgs& a) -> std::optional<int> {
        s.tool.camera = a.next();

        return {};
    });

    add(flags, {"--fixed-step"}, 0, [](ParseState& s, FlagArgs&) -> std::optional<int> {
        s.tool.fixedStep = true;

        return {};
    });

    add(flags, {"--image-stats"}, 1, [](ParseState& s, FlagArgs& a) -> std::optional<int> {
        s.imageStats = a.next();

        return {};
    });

    add(flags, {"--region"}, 1, [](ParseState& s, FlagArgs& a) -> std::optional<int> {
        // x,y,w,h as fractions of the image, for --image-stats.
        const std::string value = a.next();
        size_t start = 0;
        for (int k = 0; k < 4; ++k) {
            const size_t comma = k < 3 ? value.find(',', start) : value.size();
            const char* first = value.data() + start;
            const char* last = value.data() + std::min(comma, value.size());
            const auto [end, ec] = std::from_chars(first, last, s.imageRegion[size_t(k)]);
            if (comma == std::string::npos || ec != std::errc{} || end != last ||
                !(s.imageRegion[size_t(k)] >= 0 && s.imageRegion[size_t(k)] <= 1))
                throw player::UsageError("--region takes x,y,width,height as fractions from 0 to 1");
            start = comma + 1;
        }
        if (s.imageRegion[2] <= 0 || s.imageRegion[3] <= 0 || s.imageRegion[0] + s.imageRegion[2] > 1.000001 ||
            s.imageRegion[1] + s.imageRegion[3] > 1.000001)
            throw player::UsageError("--region must be a non-empty part of the image");

        return {};
    });

    add(flags, {"--postprocess-schema"}, 1, [](ParseState& s, FlagArgs& a) -> std::optional<int> {
        s.postProcessSchema = a.next();

        return {};
    });

    add(flags, {"--postprocess-info"}, 1, [](ParseState& s, FlagArgs& a) -> std::optional<int> {
        s.postProcessInfo = a.next();

        return {};
    });

    add(flags, {"--terrain-info"}, 1, [](ParseState& s, FlagArgs& a) -> std::optional<int> {
        s.terrainInfo = a.next();

        return {};
    });

    add(flags, {"--terrain-import"}, 1, [](ParseState& s, FlagArgs& a) -> std::optional<int> {
        s.terrainImport = a.next();

        return {};
    });

    add(flags, {"--spacing"}, 1, [](ParseState& s, FlagArgs& a) -> std::optional<int> {
        const std::string value = a.next();
        const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), s.terrainSpacing);
        if (ec != std::errc{} || end != value.data() + value.size() || !(s.terrainSpacing >= 0.1f) ||
            !(s.terrainSpacing <= 100.0f))
            throw player::UsageError("--spacing takes metres between terrain points, 0.1 to 100");

        return {};
    });

    add(flags, {"--height-range"}, 2, [](ParseState& s, FlagArgs& a) -> std::optional<int> {
        const std::string low = a.next(), high = a.next();
        const auto [lowEnd, lowEc] = std::from_chars(low.data(), low.data() + low.size(), s.terrainLow);
        const auto [highEnd, highEc] = std::from_chars(high.data(), high.data() + high.size(), s.terrainHigh);
        if (lowEc != std::errc{} || highEc != std::errc{} || lowEnd != low.data() + low.size() ||
            highEnd != high.data() + high.size() || !std::isfinite(s.terrainLow) || !std::isfinite(s.terrainHigh) ||
            std::abs(s.terrainLow) > 100000.0f || std::abs(s.terrainHigh) > 100000.0f ||
            !(s.terrainHigh > s.terrainLow))
            throw player::UsageError("--height-range takes the lowest and highest metres, low below high");

        return {};
    });

    add(flags, {"--lut-export-neutral"}, 2, [](ParseState& s, FlagArgs& a) -> std::optional<int> {
        s.lutNeutralSize = uint32_t(wholeNumber(a.next(), 65, "--lut-export-neutral"));
        s.lutNeutral = a.next();

        return {};
    });

    add(flags, {"--effect-info"}, 1, [](ParseState& s, FlagArgs& a) -> std::optional<int> {
        s.effectInfo = a.next();

        return {};
    });

    add(flags, {"--material-info"}, 1, [](ParseState& s, FlagArgs& a) -> std::optional<int> {
        s.materialInfo = a.next();

        return {};
    });

    add(flags, {"--texture-info"}, 1, [](ParseState& s, FlagArgs& a) -> std::optional<int> {
        s.textureInfo = a.next();

        return {};
    });

    add(flags, {"--image-diff"}, 2, [](ParseState& s, FlagArgs& a) -> std::optional<int> {
        s.imageReference = a.next();
        s.imageCandidate = a.next();

        return {};
    });

    add(flags, {"--tolerance"}, 1, [](ParseState& s, FlagArgs& a) -> std::optional<int> {
        const std::string value = a.next();
        const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), s.imageTolerance);
        if (ec != std::errc{} || end != value.data() + value.size() || !std::isfinite(s.imageTolerance) ||
            s.imageTolerance < 0 || s.imageTolerance > 255)
            throw player::UsageError("--tolerance takes an RMSE from 0 to 255");

        return {};
    });

    add(flags, {"--view"}, 1, [](ParseState& s, FlagArgs& a) -> std::optional<int> {
        const std::string view = a.next();
        if (view == "lit")
            s.tool.view = shockwave::DebugView::Lit;
        else if (view == "unlit")
            s.tool.view = shockwave::DebugView::Unlit;
        else if (view == "normals")
            s.tool.view = shockwave::DebugView::Normals;
        else if (view == "lighting")
            s.tool.view = shockwave::DebugView::Lighting;
        else if (view == "cascades")
            s.tool.view = shockwave::DebugView::ShadowCascades;
        else if (view == "ao")
            s.tool.view = shockwave::DebugView::AmbientOcclusion;
        else if (view == "indirect")
            s.tool.view = shockwave::DebugView::IndirectDiffuse;
        else if (view == "reflections")
            s.tool.view = shockwave::DebugView::Reflections;
        else if (view == "decals")
            s.tool.view = shockwave::DebugView::Decals;
        else if (view == "motion")
            s.tool.view = shockwave::DebugView::Motion;
        else if (view == "reactive")
            s.tool.view = shockwave::DebugView::Reactive;
        else if (view == "clusters")
            s.tool.view = shockwave::DebugView::Clusters;
        else if (view == "fog")
            s.tool.view = shockwave::DebugView::Fog;
        else if (view == "transmission")
            s.tool.view = shockwave::DebugView::Transmission;
        else if (view == "shading")
            s.tool.view = shockwave::DebugView::ShadingFamilies;
        else if (view == "contact_shadows")
            s.tool.view = shockwave::DebugView::ContactShadows;
        else if (view == "screen_reflections")
            s.tool.view = shockwave::DebugView::ScreenReflections;
        else if (view == "navmesh")
            s.tool.showNavMesh = true;
        else
            throw std::runtime_error("--view takes lit, unlit, normals, lighting, cascades, ao, indirect, "
                                     "reflections, decals, motion, reactive, clusters, fog, transmission, "
                                     "shading, contact_shadows, screen_reflections or navmesh");

        return {};
    });

    add(flags, {"--shadows"}, 1, [](ParseState& s, FlagArgs& a) -> std::optional<int> {
        s.tool.shadows = shockwave::parseShadowQuality(a.next());
        if (s.tool.shadows < 0) throw std::runtime_error("--shadows takes off, low, medium, high or ultra");

        return {};
    });

    add(flags, {"--size"}, 1, [](ParseState& s, FlagArgs& a) -> std::optional<int> {
        const std::string value = a.next();
        const size_t x = value.find('x');
        int w = 0, h = 0;
        if (x == std::string::npos || std::from_chars(value.data(), value.data() + x, w).ec != std::errc{} ||
            std::from_chars(value.data() + x + 1, value.data() + value.size(), h).ec != std::errc{} || w < 16 || h < 16)
            throw std::runtime_error("--size takes WIDTHxHEIGHT, like 1280x720");
        s.size = {w, h};

        return {};
    });

    add(flags, {"--fps"}, 1, [](ParseState& s, FlagArgs& a) -> std::optional<int> {
        const std::string value = a.next();
        double fps = -1;
        const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), fps);
        if (ec != std::errc{} || end != value.data() + value.size() || !std::isfinite(fps) || fps < 0 ||
            (fps > 0 && fps < 10) || fps > 1000)
            throw std::runtime_error("--fps takes 0 (no limit) or a frame rate from 10 to 1000");
        s.fpsOverride = fps;

        return {};
    });

    add(flags, {"--low-latency"}, 0, [](ParseState& s, FlagArgs&) -> std::optional<int> {
        s.lowLatency = true;

        return {};
    });

    add(flags, {"--vsync"}, 1, [](ParseState& s, FlagArgs& a) -> std::optional<int> {
        const std::string value = a.next();
        if (value != "on" && value != "off") throw std::runtime_error("--vsync takes on or off");
        s.vsyncOverride = value == "on" ? 1 : 0;

        return {};
    });

    add(flags, {"--optimise", "--optimize"}, 0, [](ParseState& s, FlagArgs&) -> std::optional<int> {
        s.tool.optimise = true;

        return {};
    });

    add(flags, {"--lod-colors"}, 0, [](ParseState& s, FlagArgs&) -> std::optional<int> {
        s.tool.lodColors = true;

        return {};
    });

    add(flags, {"--graphics-report"}, 0, [](ParseState& s, FlagArgs&) -> std::optional<int> {
        s.tool.graphicsReportFrame = 10;

        return {};
    });

    add(flags, {"--benchmark"}, 0, [](ParseState& s, FlagArgs& a) -> std::optional<int> {
        s.tool.benchmarkSeconds = 10;
        if (a.more() && a.peek()[0] != '-') {
            const std::string value = a.next();
            const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), s.tool.benchmarkSeconds);
            if (ec != std::errc{} || end != value.data() + value.size() || s.tool.benchmarkSeconds <= 0)
                throw std::runtime_error("--benchmark takes a number of seconds");
        }

        return {};
    });

    add(flags, {"--json"}, 1, [](ParseState& s, FlagArgs& a) -> std::optional<int> {
        s.tool.benchmarkJson = a.next();

        return {};
    });

    add(flags, {"--ui-preview"}, 0, [](ParseState& s, FlagArgs&) -> std::optional<int> {
        s.tool.uiPreview = true;

        return {};
    });

    add(flags, {"--ui-nav"}, 1, [](ParseState& s, FlagArgs& a) -> std::optional<int> {
        // "down,down,confirm": what the arrow keys or d-pad, Enter and Back would do.
        std::string steps = a.next();
        for (size_t at = 0; at <= steps.size();) {
            const size_t comma = std::min(steps.find(',', at), steps.size());
            const std::string step = steps.substr(at, comma - at);
            at = comma + 1;
            if (step == "up")
                s.tool.uiNav.push_back(shockwave::UINav::Up);
            else if (step == "down")
                s.tool.uiNav.push_back(shockwave::UINav::Down);
            else if (step == "left")
                s.tool.uiNav.push_back(shockwave::UINav::Left);
            else if (step == "right")
                s.tool.uiNav.push_back(shockwave::UINav::Right);
            else if (step == "confirm")
                s.tool.uiNav.push_back(shockwave::UINav::Confirm);
            else if (step == "back")
                s.tool.uiNav.push_back(shockwave::UINav::Back);
            else
                throw std::runtime_error("--ui-nav takes steps like up,down,left,right,confirm,back");
        }

        return {};
    });

    add(flags, {"--print-frame"}, 0, [](ParseState& s, FlagArgs&) -> std::optional<int> {
        s.tool.printFrame = true;

        return {};
    });
}

}   // namespace player
