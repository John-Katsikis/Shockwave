// The player's flags for networking and online play: rows of the table in PlayerFlags.h.
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

void addNetworkFlags(std::vector<Flag>& flags) {
    add(flags, {"--rotations"}, 1, [](ParseState& s, FlagArgs& a) -> std::optional<int> {
        s.netTest.rotations = uint32_t(wholeNumber(a.next(), 100, "--rotations"));
        return {};
    });

    add(flags, {"--net-set"}, 1, [](ParseState& s, FlagArgs& a) -> std::optional<int> {
        s.netTest.sets.push_back(a.next());
        return {};
    });

    add(flags, {"--network-info"}, 0, [](ParseState& s, FlagArgs&) -> std::optional<int> {
        s.networkInfo = true;

        return {};
    });

    add(flags, {"--network-schema"}, 1, [](ParseState& s, FlagArgs& a) -> std::optional<int> {
        s.networkSchema = a.next();

        return {};
    });

    add(flags, {"--steam-status"}, 0, [](ParseState& s, FlagArgs& a) -> std::optional<int> {
        s.steamStatus = 1;
        if (a.more() && (std::string_view(a.peek()) == "client" || std::string_view(a.peek()) == "server"))
            s.steamStatus = std::string_view(a.next()) == "server" ? 2 : 1;

        return {};
    });

    add(flags, {"--net-preview"}, 0, [](ParseState& s, FlagArgs& a) -> std::optional<int> {
        s.tool.netPreview = "poor";
        if (a.more() && a.peek()[0] != '-') s.tool.netPreview = a.next();

        return {};
    });

    add(flags, {"--net-test"}, 0, [](ParseState& s, FlagArgs&) -> std::optional<int> {
        s.netTest.requested = true;

        return {};
    });

    add(flags, {"--scenario"}, 1, [](ParseState& s, FlagArgs& a) -> std::optional<int> {
        s.netTest.scenario = a.next();

        return {};
    });

    add(flags, {"--preset"}, 1, [](ParseState& s, FlagArgs& a) -> std::optional<int> {
        s.netTest.preset = a.next();

        return {};
    });

    add(flags, {"--seed"}, 1, [](ParseState& s, FlagArgs& a) -> std::optional<int> {
        s.netTest.seed = s.tool.keySeed = wholeNumber(a.next(), UINT64_MAX, "--seed");
        s.tool.keySeedGiven = true;

        return {};
    });

    add(flags, {"--host"}, 0, [](ParseState& s, FlagArgs&) -> std::optional<int> {
        if (s.tool.online != Tool::Online::None)
            throw player::UsageError("--host, --join-lobby and --join-steam are one each");
        s.tool.online = Tool::Online::Host;

        return {};
    });

    add(flags, {"--join-lobby"}, 1, [](ParseState& s, FlagArgs& a) -> std::optional<int> {
        if (s.tool.online != Tool::Online::None)
            throw player::UsageError("--host, --join-lobby and --join-steam are one each");
        s.tool.online = Tool::Online::JoinLobby;
        s.tool.onlineId = steamNumber(a.next(), "--join-lobby");
        s.joinLobby = true;

        return {};
    });

    add(flags, {"--join-steam"}, 1, [](ParseState& s, FlagArgs& a) -> std::optional<int> {
        if (s.tool.online != Tool::Online::None)
            throw player::UsageError("--host, --join-lobby and --join-steam are one each");
        s.tool.online = Tool::Online::JoinDirect;
        s.tool.onlineId = steamNumber(a.next(), "--join-steam");
        s.joinSteam = true;

        return {};
    });

    add(flags, {"+connect_lobby"}, 1, [](ParseState& s, FlagArgs& a) -> std::optional<int> {
        s.tool.connectLobby = steamNumber(a.next(), "+connect_lobby");   // Steam's own launch option

        return {};
    });

    add(flags, {"--dev-direct-join"}, 0, [](ParseState& s, FlagArgs&) -> std::optional<int> {
        s.devDirectJoin = true;

        return {};
    });

    add(flags, {"--net-capture"}, 1, [](ParseState& s, FlagArgs& a) -> std::optional<int> {
        s.tool.netCapture = a.next();

        return {};
    });

    add(flags, {"--net-dump"}, 1, [](ParseState& s, FlagArgs& a) -> std::optional<int> {
        s.netDump = a.next();

        return {};
    });

    add(flags, {"--net-encode"}, 1, [](ParseState& s, FlagArgs& a) -> std::optional<int> {
        s.netEncode = a.next();

        return {};
    });

    add(flags, {"--network-manifest"}, 0, [](ParseState& s, FlagArgs&) -> std::optional<int> {
        s.tool.mode = Tool::Mode::NetworkManifest;

        return {};
    });

    add(flags, {"--assign-network-keys"}, 0, [](ParseState& s, FlagArgs&) -> std::optional<int> {
        s.tool.mode = Tool::Mode::AssignNetworkKeys;

        return {};
    });

    add(flags, {"--clients"}, 1, [](ParseState& s, FlagArgs& a) -> std::optional<int> {
        s.netTest.clients = uint32_t(wholeNumber(a.next(), 63, "--clients"));

        return {};
    });
}

}   // namespace player
