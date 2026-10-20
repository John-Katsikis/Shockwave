// Multiplayer in the editor (docs/Networking.md): the Multiplayer section of Project
// settings, which edits Config/Network.swnetwork row by row from the same descriptor table
// the player's --check and docs/NetworkSchema.md use, and "Check network setup", which runs
// the network part of `player --check` here. Only "Start Steam" starts Steam (for the saved
// steam_app_id, like `player --steam-status`); nothing else here opens a socket.
#include "Editor.h"

#include <Engine/Prefab.h>
#include <GameFramework/GameNetwork.h>
#include <Multiplayer/NetworkSchema.h>
#include <Multiplayer/NetworkValidation.h>
#include <Network/Sha256.h>
#include <UI/Gui.h>

#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <cmath>
#include <type_traits>

namespace editor {

namespace fs = std::filesystem;
namespace gui = shockwave::gui;
namespace net = shockwave::net;

void Editor::loadNetworkSettingsEdit() {
    m_networkEdit = {};
    m_networkEdit.loaded = true;
    const fs::path file = net::networkSettingsPath(m_project);
    std::error_code ec;
    if (!fs::exists(file, ec)) return;   // offline only until "Add multiplayer settings"
    net::NetworkSettingsFile read;
    std::string err;
    if (!net::readNetworkSettings(file, read, err)) {
        // Never overwritten by a Save until the file is fixed by hand or reverted.
        if (err.rfind(file.string(), 0) == 0) err.replace(0, file.string().size(), m_project.relative(file));
        m_networkEdit.loadError = err;
        return;
    }
    m_networkEdit.present = m_networkEdit.savedPresent = true;
    m_networkEdit.saved = m_networkEdit.edit = read.settings;
}

void Editor::checkNetworkSetup() {
    m_networkEdit.checkResults.clear();
    std::vector<net::NetProblem> problems;
    net::ReplicationRegistry registry;
    std::vector<std::string> registryProblems;
    const shockwave::GameModule* module = m_gameModule ? &m_gameModule->module() : nullptr;
    const bool frozen = shockwave::buildReplicationRegistry(module, registry, registryProblems);
    for (const std::string& problem : registryProblems)
        problems.push_back({false, "game module network types: " + problem});
    net::NetworkProject network;
    net::loadNetworkProject(m_project, network, problems);
    if (frozen) net::validateNetworkProject(m_project, network, registry, problems);
    // The open scene against the predicted-character collision rule.
    bool predictedSpawns = false;
    if (network.manifest)
        for (const net::ManifestPrefab& entry : network.manifest->prefabs) {
            std::string error;
            if (const auto prefab = shockwave::prefabs().load(entry.path, &error))
                predictedSpawns = predictedSpawns || net::predictedPrefab(prefab->world, prefab->root);
        }
    const std::string scene = m_scenePath.empty() ? std::string("the open scene") : m_project.relative(m_scenePath);
    net::validatePredictionWorld(m_world, scene, predictedSpawns, problems);
    if (network.manifest) {
        net::ContentDigest content;
        if (net::computeContentDigest(m_project, *network.manifest, network.manifestShown, content, problems))
            m_networkEdit.checkResults.push_back(
                {false, "content digest " + net::toHex(content.digest).substr(0, 16) + "... (" +
                            std::to_string(content.files.size()) + " files)"});
    }
    if (frozen)
        m_networkEdit.checkResults.push_back(
            {false, "registry: " + std::to_string(registry.profiles().size()) + " profiles, " +
                        std::to_string(registry.commands().size()) + " commands, " +
                        std::to_string(registry.events().size()) + " events; schema digest " +
                        net::toHex(registry.schemaDigest()).substr(0, 16) + "..."});
    m_networkEdit.checkProblems = std::move(problems);
    m_networkEdit.checked = true;
    if (!module)
        m_networkEdit.checkResults.push_back({true, "the game module isn't loaded, so only engine types were checked"});
    log(net::hasErrors(m_networkEdit.checkProblems) ? "Network setup has problems; see Project settings"
                                                    : "Network setup checked: no problems");
}

void Editor::startSteam() {
    m_steamMessage.clear();
    const NetworkSettingsEdit& n = m_networkEdit;
    net::SteamServicesSettings settings;
    settings.appId = n.saved.steamAppId;
    settings.virtualPort = uint16_t(n.saved.virtualPort);
    settings.maxUnreliableBytes = n.saved.maxSnapshotBytes;
    auto steam = net::SteamServices::create();
    const net::NetResult<void> started = steam->initializeClient(settings);
    if (!started) {
        m_steamMessage = started.error().message;
        if (*steam->lastError()) m_steamMessage += std::string(" (Steam said: ") + steam->lastError() + ")";
        error("Steam didn't start: " + m_steamMessage);
        return;
    }
    m_steam = std::move(steam);
    log("Steam started for AppID " + std::to_string(settings.appId));
}

void Editor::drawNetworkProjectSettings() {
    if (!m_networkEdit.loaded) loadNetworkSettingsEdit();
    NetworkSettingsEdit& n = m_networkEdit;
    const ImVec4 errorColor = gui::semanticColor(gui::currentTheme().error);
    const ImVec4 warningColor = gui::semanticColor(gui::currentTheme().warning);
    ImGui::SeparatorText("Multiplayer");
    const net::NetworkBuild build = net::networkBuild();
    ImGui::TextDisabled(
        "This build: Steam %s, loopback %s, up to %u players", build.steamworks ? "yes" : "no",
        build.loopback ? "yes" : "no", unsigned(shockwave::net::kMaxPlayers));
    if (!n.loadError.empty()) {
        ImGui::TextColored(errorColor, "%s", n.loadError.c_str());
        ImGui::TextDisabled("Fix the file by hand; the editor won't overwrite it.");
        if (gui::button("Reload##network")) loadNetworkSettingsEdit();
        return;
    }
    if (!n.present) {
        ImGui::TextDisabled("No Config/Network.swnetwork: this project plays offline only.");
        if (gui::button("Add multiplayer settings")) {
            n.present = true;
            n.edit = {};
            n.edit.tickRate = uint32_t(std::clamp(std::lround(m_projectEdit.tickRate), 60L, 120L));
            n.edit.inputRate = n.edit.tickRate;
        }
    } else if (gui::beginPropertyGrid("network")) {
        for (const net::NetworkSettingField& field : net::networkSettingFields()) {
            gui::propertyLabel(field.label);
            ImGui::PushID(field.key);
            std::visit(
                [&](auto member) {
                    using T = std::remove_reference_t<decltype(n.edit.*member)>;
                    T& value = n.edit.*member;
                    if constexpr (std::is_same_v<T, bool>) {
                        ImGui::Checkbox("##v", &value);
                    } else if constexpr (std::is_enum_v<T>) {
                        int index = int(value);
                        const char* shown = size_t(index) < field.choices.size() ? field.choices[size_t(index)] : "?";
                        if (ImGui::BeginCombo("##v", shown)) {
                            for (size_t i = 0; i < field.choices.size(); ++i)
                                if (ImGui::Selectable(field.choices[i], int(i) == index)) value = T(i);
                            ImGui::EndCombo();
                        }
                    } else if constexpr (std::is_same_v<T, uint32_t>) {
                        // Shown as typed; out-of-range values are listed below, not clamped.
                        int64_t v = value;
                        if (ImGui::InputScalar("##v", ImGuiDataType_S64, &v, nullptr, nullptr, "%lld"))
                            value = uint32_t(std::clamp<int64_t>(v, 0, int64_t(UINT32_MAX)));
                    } else if constexpr (std::is_same_v<T, double>) {
                        ImGui::InputDouble("##v", &value, 1.0, 5.0, "%.1f s");
                    } else {
                        ImGui::InputText("##v", &value);
                    }
                },
                field.member);
            ImGui::SetItemTooltip("%s (%s)", field.meaning, field.key);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    // The same rules --check applies, against the tick rate being edited above.
    std::vector<net::NetProblem> problems;
    if (n.present) net::validateNetworkSettings(n.edit, m_projectEdit.tickRate, "Network.swnetwork", nullptr, problems);
    for (const net::NetProblem& problem : problems)
        ImGui::TextColored(problem.warning ? warningColor : errorColor, "%s", problem.text.c_str());

    const bool changed = n.present != n.savedPresent || (n.present && !(n.edit == n.saved));
    ImGui::BeginDisabled(!changed || !n.present || net::hasErrors(problems) || playing());
    if (gui::button("Save multiplayer settings")) {
        std::string err;
        if (net::writeNetworkSettings(net::networkSettingsPath(m_project), n.edit, err)) {
            n.saved = n.edit;
            n.savedPresent = true;
            log("Saved " + m_project.relative(net::networkSettingsPath(m_project)));
        } else {
            error("Couldn't save the network settings: " + err);
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!changed);
    if (gui::button("Revert##network")) {
        n.present = n.savedPresent;
        n.edit = n.saved;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (gui::button("Check network setup")) checkNetworkSetup();
    ImGui::SetItemTooltip("The saved files, as player --check and player --network-info see them");
    if (n.checked) {
        for (const net::NetProblem& line : n.checkResults)
            if (line.warning)
                ImGui::TextColored(warningColor, "%s", line.text.c_str());
            else
                ImGui::TextDisabled("%s", line.text.c_str());
        for (const net::NetProblem& problem : n.checkProblems)
            ImGui::TextColored(problem.warning ? warningColor : errorColor, "%s", problem.text.c_str());
        if (!net::hasErrors(n.checkProblems)) ImGui::TextDisabled("No network problems.");
    }

    // Steam itself: started only from here, for the saved settings.
    ImGui::SeparatorText("Steam");
    if (!m_steam) {
        const bool canStart = build.steamworks && n.savedPresent && n.saved.steamAppId != 0;
        ImGui::BeginDisabled(!canStart);
        if (gui::button("Start Steam")) startSteam();
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Signs in to Steam for the saved steam_app_id and checks relay network access");
        if (!build.steamworks)
            ImGui::TextDisabled("This build has no Steamworks SDK (configure with -DSHOCKWAVE_STEAMWORKS=ON).");
        else if (!n.savedPresent || n.saved.steamAppId == 0)
            ImGui::TextDisabled("Save a steam_app_id first.");
        else
            ImGui::TextDisabled("Steam isn't running.");
    } else {
        ImGui::Text(
            "Steam %s, SteamID %llu, signed in %s, relay %s", net::steamServiceStateName(m_steam->state()),
            static_cast<unsigned long long>(m_steam->localPeer().value), m_steam->available() ? "yes" : "no",
            net::steamRelayStatusName(m_steam->relayStatus()));
        // The session's transport and auth sessions are Steam's: it stops first.
        ImGui::BeginDisabled(networkPlay());
        if (gui::button("Stop Steam")) {
            m_steam.reset();
            log("Steam shut down");
        }
        ImGui::EndDisabled();
        if (networkPlay()) ImGui::SetItemTooltip("Stop network play first");
    }
    if (const net::NetworkWorld* network = m_session ? m_session->network() : nullptr; network && network->online()) {
        const net::NetworkWorldStats& stats = network->stats();
        ImGui::Text(
            "Network play: %s, %s, player %u, %u network objects", net::networkRoleName(network->role()),
            net::networkStateName(network->state()), unsigned(network->localPlayer()), unsigned(stats.objects));
        if (network->authority())
            ImGui::Text(
                "Players in the game: %u of %u welcomed", unsigned(stats.activePeers), unsigned(stats.admitted));
        if (network->state() == net::NetworkState::Failed)
            ImGui::TextColored(
                errorColor, "%s (%s)", network->failureText().c_str(),
                net::reasonName(network->failure()) ? net::reasonName(network->failure()) : "?");
    }
    if (!m_steamMessage.empty()) ImGui::TextColored(errorColor, "%s", m_steamMessage.c_str());
}

}   // namespace editor
