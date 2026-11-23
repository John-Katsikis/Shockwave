// The player's flags for projects, packaging, lighting, circuits, audio and the reference schemas: rows of the table in PlayerFlags.h.
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

void addProjectFlags(std::vector<Flag>& flags) {
    add(flags, {"--new-project"}, 1, [](ParseState& s, FlagArgs& a) -> std::optional<int> {
        s.newProjectFolder = a.next();
        return {};
    });

    add(flags, {"--name"}, 1, [](ParseState& s, FlagArgs& a) -> std::optional<int> {
        s.newProjectName = a.next();
        return {};
    });

    add(flags, {"--module"}, 1, [](ParseState& s, FlagArgs& a) -> std::optional<int> {
        s.newProjectModule = a.next();
        return {};
    });

    add(flags, {"--project"}, 1, [](ParseState& s, FlagArgs& a) -> std::optional<int> {
        s.manifest = a.next();
        return {};
    });

    add(flags, {"--frames"}, 1, [](ParseState& s, FlagArgs& a) -> std::optional<int> {
        const std::string value = a.next();
        const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), s.frames);
        if (ec != std::errc{} || end != value.data() + value.size() || s.frames <= 0)
            throw std::runtime_error("--frames requires a positive integer");

        return {};
    });

    add(flags, {"--audio-volume"}, 1, [](ParseState& s, FlagArgs& a) -> std::optional<int> {
        const std::string value = a.next();
        const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), s.audioVolume);
        if (ec != std::errc{} || end != value.data() + value.size() || !std::isfinite(s.audioVolume) ||
            s.audioVolume < 0 || s.audioVolume > 1)
            throw std::runtime_error("--audio-volume requires a number in 0..1");
        s.audioVolumeSet = true;

        return {};
    });

    add(flags, {"--game-module"}, 1, [](ParseState& s, FlagArgs& a) -> std::optional<int> {
        s.gameModuleFile = a.next();

        return {};
    });

    add(flags, {"--package"}, 0, [](ParseState& s, FlagArgs&) -> std::optional<int> {
        s.package.requested = true;

        return {};
    });

    add(flags, {"--output"}, 1, [](ParseState& s, FlagArgs& a) -> std::optional<int> {
        s.output = a.next();

        return {};
    });

    add(flags, {"--no-build"}, 0, [](ParseState& s, FlagArgs&) -> std::optional<int> {
        s.package.noBuild = true;

        return {};
    });

    add(flags, {"--no-archive"}, 0, [](ParseState& s, FlagArgs&) -> std::optional<int> {
        s.package.noArchive = true;

        return {};
    });

    add(flags, {"--dev-package"}, 0, [](ParseState& s, FlagArgs&) -> std::optional<int> {
        s.package.development = true;

        return {};
    });

    add(flags, {"--check"}, 0, [](ParseState& s, FlagArgs&) -> std::optional<int> {
        s.tool.mode = Tool::Mode::Check;

        return {};
    });

    add(flags, {"--bake-lighting"}, 0, [](ParseState& s, FlagArgs&) -> std::optional<int> {
        s.tool.mode = Tool::Mode::BakeLighting;

        return {};
    });

    add(flags, {"--quality"}, 1, [](ParseState& s, FlagArgs& a) -> std::optional<int> {
        s.tool.bakeQuality = a.next();
        if (s.tool.bakeQuality != "preview" && s.tool.bakeQuality != "final")
            throw player::UsageError("--quality takes preview or final");

        return {};
    });

    add(flags, {"--capture-reflections"}, 0, [](ParseState& s, FlagArgs&) -> std::optional<int> {
        s.tool.captureReflections = true;

        return {};
    });

    add(flags, {"--lighting-info"}, 0, [](ParseState& s, FlagArgs&) -> std::optional<int> {
        s.tool.mode = Tool::Mode::LightingInfo;

        return {};
    });

    add(flags, {"--lighting-assign-ids"}, 0, [](ParseState& s, FlagArgs&) -> std::optional<int> {
        s.tool.mode = Tool::Mode::LightingAssignIds;

        return {};
    });

    add(flags, {"--lighting-schema"}, 1, [](ParseState& s, FlagArgs& a) -> std::optional<int> {
        s.lightingSchema = a.next();

        return {};
    });

    add(flags, {"--lighting-export"}, 1, [](ParseState& s, FlagArgs& a) -> std::optional<int> {
        s.lightingExport = a.next();

        return {};
    });

    add(flags, {"--lighting-import"}, 1, [](ParseState& s, FlagArgs& a) -> std::optional<int> {
        s.lightingImport = a.next();

        return {};
    });

    add(flags, {"--bake-navmesh"}, 0, [](ParseState& s, FlagArgs&) -> std::optional<int> {
        s.tool.mode = Tool::Mode::BakeNavMesh;

        return {};
    });

    add(flags, {"--schema"}, 1, [](ParseState& s, FlagArgs& a) -> std::optional<int> {
        s.tool.mode = Tool::Mode::Schema;
        s.tool.schemaOut = a.next();

        return {};
    });

    add(flags, {"--animations"}, 0, [](ParseState& s, FlagArgs& a) -> std::optional<int> {
        s.tool.mode = Tool::Mode::Animations;
        while (a.more() && a.peek()[0] != '-') s.tool.animationModels.push_back(a.next());

        return {};
    });

    add(flags, {"--node-schema"}, 1, [](ParseState& s, FlagArgs& a) -> std::optional<int> {
        s.tool.mode = Tool::Mode::NodeSchema;
        s.tool.schemaOut = a.next();

        return {};
    });

    add(flags, {"--circuit"}, 1, [](ParseState& s, FlagArgs& a) -> std::optional<int> {
        s.tool.circuit = a.next();
        if (s.tool.mode == Tool::Mode::Play) s.tool.mode = Tool::Mode::CircuitDump;

        return {};
    });

    add(flags, {"--run-circuit"}, 0, [](ParseState& s, FlagArgs&) -> std::optional<int> {
        s.tool.mode = Tool::Mode::CircuitRun;

        return {};
    });

    add(flags, {"--circuit-ticks", "--circuit-steps"}, 1, [](ParseState& s, FlagArgs& a) -> std::optional<int> {
        int count = 0;
        const std::string value = a.next();
        const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), count);
        if (ec != std::errc{} || end != value.data() + value.size() || count < 0 || count > 10000)
            throw std::runtime_error(a.arg + " takes an integer from 0 to 10000");
        if (a.arg == "--circuit-ticks")
            s.tool.circuitTicks = count;
        else
            s.tool.circuitSteps = count;

        return {};
    });

    add(flags, {"--send-message"}, 1, [](ParseState& s, FlagArgs& a) -> std::optional<int> {
        s.tool.circuitMessage = a.next();

        return {};
    });

    add(flags, {"--break-node"}, 1, [](ParseState& s, FlagArgs& a) -> std::optional<int> {
        s.tool.circuitBreakpoint = a.next();

        return {};
    });

    add(flags, {"--dump"}, 0, [](ParseState& s, FlagArgs&) -> std::optional<int> {
        s.tool.mode = Tool::Mode::CircuitDump;

        return {};
    });

    add(flags, {"--input-schema"}, 1, [](ParseState&, FlagArgs& a) -> std::optional<int> {
        return player::writeInputSchema(a.peek());
    });

    add(flags, {"--devices"}, 0, [](ParseState&, FlagArgs& a) -> std::optional<int> {
        double seconds = 0.0;
        if (a.more() && a.peek()[0] != '-') {
            const std::string value = a.peek();
            const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), seconds);
            if (ec != std::errc{} || end != value.data() + value.size() || seconds < 0.0)
                throw std::runtime_error("--devices takes a number of seconds to listen for");
        }
        return player::listDevices(seconds);
    });

    add(flags, {"--scene"}, 1, [](ParseState& s, FlagArgs& a) -> std::optional<int> {
        s.tool.scene = std::filesystem::absolute(a.next()).lexically_normal();

        return {};
    });

    add(flags, {"--audio-mix"}, 1, [](ParseState& s, FlagArgs& a) -> std::optional<int> {
        s.tool.mode = Tool::Mode::AudioMix;
        s.tool.audioMix = a.next();

        return {};
    });

    add(flags, {"--seconds"}, 1, [](ParseState& s, FlagArgs& a) -> std::optional<int> {
        const std::string value = a.next();
        const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), s.tool.audioSeconds);
        if (ec != std::errc{} || end != value.data() + value.size() || !(s.tool.audioSeconds > 0) ||
            s.tool.audioSeconds > 600)
            throw std::runtime_error("--seconds takes a number of seconds up to 600");

        return {};
    });

    add(flags, {"--audio-hrtf"}, 1, [](ParseState& s, FlagArgs& a) -> std::optional<int> {
        const std::string value = a.next();
        if (value != "on" && value != "off") throw std::runtime_error("--audio-hrtf takes on or off");
        s.tool.audioHrtf = value == "on" ? 1 : 0;

        return {};
    });

    add(flags, {"--help"}, 0, [](ParseState&, FlagArgs&) -> std::optional<int> {
        printHelp();
        return 0;
    });
}

}   // namespace player
