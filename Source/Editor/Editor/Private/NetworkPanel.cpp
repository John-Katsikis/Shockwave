// Multiplayer in the editor (docs/Networking.md): the Multiplayer section of Project
// settings, which edits Config/Network.swnetwork row by row from the same descriptor table
// the player's --check and docs/NetworkSchema.md use, and "Check network setup", which runs
// the network part of `player --check` here. Only "Start Steam" starts Steam (for the saved
// steam_app_id, like `player --steam-status`); nothing else here opens a socket.
#include "Editor.h"

#include <Circuits/Circuit.h>
#include <Circuits/CircuitProgram.h>
#include <Engine/Components.h>
#include <Engine/Prefab.h>
#include <GameFramework/GameNetwork.h>
#include <Multiplayer/NetTest.h>
#include <Multiplayer/NetworkComponents.h>
#include <Multiplayer/NetworkSchema.h>
#include <Multiplayer/NetworkValidation.h>
#include <Network/Sha256.h>
#include <UI/Gui.h>

#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <ctime>
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
    // Each circuit only with the nodes its execution allows (docs/Circuits.md "Network play").
    m_world.each<const shockwave::Circuit>([&](shockwave::Entity e, const shockwave::Circuit& circuit) {
        if (circuit.circuit.empty()) return;
        const auto program = shockwave::circuitLibrary().load(circuit.circuit, shockwave::CircuitKind::Entity);
        if (!program) return;
        const auto* name = m_world.get<shockwave::Name>(e);
        for (const shockwave::CircuitProblem& problem : shockwave::circuitScopeProblems(*program, circuit.execution))
            problems.push_back(
                {false, scene + ": entity '" + (name ? name->value : "?") + "': " + circuit.circuit + ":" +
                            std::to_string(problem.line) + ": " + problem.message});
    });
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

// ---- Window > Network ----------------------------------------------------------------------
//
// Host, Find / Join, Invite and Leave go through OnlineMatch, the same code the player's
// --host and joins and a game's own menus use. Host plays the open scene (it must be a
// scene in the network manifest); a client must have the host's scene open too. Steam
// starts on the first Host, Find or Join, for the saved steam_app_id.

namespace editor {

bool Editor::prepareOnlineMatch() {
    const std::string scene = m_scenePath.empty() ? std::string() : m_project.relative(m_scenePath);
    if (m_match.configured() && scene == m_matchScene) return true;
    if (m_match.busy()) return true;   // the scene changes after this match
    if (scene.empty()) {
        warn("Save the scene first: online play plays a scene from the network manifest");
        return false;
    }
    if (!m_networkEdit.loaded) loadNetworkSettingsEdit();
    m_matchRegistry = std::make_unique<net::ReplicationRegistry>();
    std::vector<std::string> problems;
    net::MatchSetup setup;
    const shockwave::GameModule* module = m_gameModule ? &m_gameModule->module() : nullptr;
    if (!shockwave::buildReplicationRegistry(module, *m_matchRegistry, problems) ||
        !net::prepareMatchSetup(m_project, m_project.gameModule, *m_matchRegistry, scene, setup, problems)) {
        error("Can't play online: " + problems.front());
        m_match.setSetup({});
        m_matchScene.clear();
        return false;
    }
    m_match.setSetup(std::move(setup));
    m_match.setServiceProvider([this](std::string& why) -> net::IOnlineServices* {
        if (!m_steam) {
            if (!m_networkEdit.loaded) loadNetworkSettingsEdit();
            if (!net::steamBuild().sdk) {
                why = "this build has no Steamworks SDK (configure with -DSHOCKWAVE_STEAMWORKS=ON)";
                return nullptr;
            }
            startSteam();
        }
        if (!m_steam) why = m_steamMessage.empty() ? "Steam didn't start" : m_steamMessage;
        return m_steam.get();
    });
    m_matchScene = scene;
    return true;
}

void Editor::updateOnlineMatch() {
    if (!m_match.configured() || !m_session) return;
    if (m_match.takeEnded()) {
        m_lastMatchEnd = m_match.state() == net::MatchState::Failed ? m_match.error() : "left the match";
        if (playing()) stop();
        m_session->setNetworkStart({}, nullptr);   // the next Play is offline again
        if (m_capture) {
            log("Packet capture: " + std::to_string(m_capture->recordsWritten()) + " records in " +
                m_project.relative(m_capturePath));
            m_capture->close();
            m_capture.reset();
        }
    }
    net::NetworkStart start;
    if (!m_match.takeStart(start)) return;
    if (playing()) stop();
    if (m_captureNextMatch) {
        m_capturePath =
            m_project.statePath() / "Captures" / ("match-" + std::to_string(std::time(nullptr)) + ".swnetcap");
        std::error_code ec;
        fs::create_directories(m_capturePath.parent_path(), ec);
        m_capture = std::make_unique<net::CaptureWriter>();
        if (const auto opened = m_capture->open(m_capturePath); !opened) {
            error(std::string("Couldn't start the packet capture: ") + opened.error().message);
            m_capture.reset();
        } else {
            start.capture = m_capture.get();
            log("Capturing this match's messages to " + m_project.relative(m_capturePath) + " (tickets redacted)");
        }
    }
    m_session->setNetworkStart(std::move(start), m_steam.get());
    play();
    if (!playing()) {
        // play() said why; the match can't go on without its session.
        m_match.leave();
        (void)m_match.takeEnded();
        m_session->setNetworkStart({}, nullptr);
        m_lastMatchEnd = "the match couldn't start Play";
    }
}

void Editor::assignMissingNetworkKeysTool() {
    std::vector<net::NetworkKeyChange> preview;
    {
        shockwave::World copy = m_world;   // see first: an unchanged scene gets no undo step
        preview = net::assignNetworkKeys(copy);
    }
    if (preview.empty()) {
        log("Every network object already has a unique key");
        return;
    }
    pushUndo(m_world);
    const std::vector<net::NetworkKeyChange> changes = net::assignNetworkKeys(m_world);
    for (const net::NetworkKeyChange& c : changes)
        log(displayName(c.entity) + ": network key " + (c.before.empty() ? "(none)" : "\"" + c.before + "\"") + " -> " +
            c.after);
    log("Assigned " + std::to_string(changes.size()) + " network keys; valid unique keys were kept");
}

std::string Editor::networkDiagnostics() {
    std::string out = "Shockwave network diagnostics\n";
    const net::SteamBuild build = net::steamBuild();
    out += std::string("build: Steamworks ") + (build.sdk ? build.sdkVersion : "off") + "\n";
    out += "project: " + m_project.name + " (" + m_project.gameModule + ")\n";
    if (m_networkEdit.loaded && m_networkEdit.savedPresent)
        out += "settings:\n" + net::networkSettingsText(m_networkEdit.saved);
    out += std::string("match: ") + net::matchStateName(m_match.state()) + ", " + m_match.status() + "\n";
    if (m_match.lobby().valid()) out += "lobby: " + std::to_string(m_match.lobby().value) + "\n";
    if (m_steam)
        out += std::string("steam: ") + net::steamServiceStateName(m_steam->state()) + ", relay " +
               net::steamRelayStatusName(m_steam->relayStatus()) + ", signed in " +
               (m_steam->available() ? "yes" : "no") + "\n";
    const net::NetworkWorld* network = m_session ? m_session->network() : nullptr;
    if (network && network->online()) {
        const net::NetworkWorldStats& s = network->stats();
        out += std::string("session: ") + net::networkRoleName(network->role()) + ", " +
               net::networkStateName(network->state()) + ", player " + std::to_string(network->localPlayer()) + ", " +
               std::to_string(s.objects) + " objects\n";
        out += "messages: " + std::to_string(s.messagesReceived) + " in (" + std::to_string(s.bytesReceived) +
               " bytes), " + std::to_string(s.messagesSent) + " out; malformed " + std::to_string(s.malformed) +
               ", rejected " + std::to_string(s.rejected) + "\n";
        out += "prediction: " + std::to_string(s.corrections) + " corrections (max " + std::to_string(s.maxCorrection) +
               " m), " + std::to_string(s.replayedTicks) + " ticks replayed, " + std::to_string(s.predictionResets) +
               " resets\n";
        out += "input: " + std::to_string(s.inputFallbacks) + " fallback ticks, " + std::to_string(s.inputsLate) +
               " late\n";
        network->peerInfo(m_peers);
        for (const net::NetworkPeerInfo& p : m_peers)
            out += "peer slot " + std::to_string(p.slot) + ": " + p.status + ", rtt " + std::to_string(int(p.rttMs)) +
                   " ms, pending " + std::to_string(p.pendingReliableBytes) + "+" +
                   std::to_string(p.pendingUnreliableBytes) + " bytes, " + std::to_string(p.bytesReceived) + " in, " +
                   std::to_string(p.bytesSent) + " out\n";
        if (network->state() == net::NetworkState::Failed) out += "failure: " + network->failureText() + "\n";
    } else {
        out += "session: offline\n";
    }
    if (!m_lastMatchEnd.empty()) out += "last match: " + m_lastMatchEnd + "\n";
    return out;   // no tickets, no secrets: nothing here is one
}

void Editor::drawNetworkObjectNotes(shockwave::Entity e) {
    const net::NetworkWorld* network = m_session && playing() ? m_session->network() : nullptr;
    if (!network) return;
    const auto& object = *m_world.get<net::NetworkObject>(e);
    const net::NetworkIdentity* identity = network->identity(e);
    ImGui::SeparatorText("During Play");
    if (!network->online()) {
        ImGui::TextDisabled("Offline Play: this object acts on its own (local authority).");
        return;
    }
    if (!identity) {
        ImGui::TextDisabled("Not replicated in this session (%s).", net::networkRoleName(network->role()));
        return;
    }
    if (gui::beginPropertyGrid("network-status")) {
        gui::propertyLabel("Network ID");
        ImGui::Text("%llu (life %u)", static_cast<unsigned long long>(identity->id), unsigned(identity->incarnation));
        gui::propertyLabel("Owner");
        if (identity->ownerSlot == 0)
            ImGui::TextUnformatted("the host (match object)");
        else
            ImGui::Text(
                "player %u%s", unsigned(identity->ownerSlot),
                identity->ownerSlot == network->localPlayer() ? " (you)" : "");
        gui::propertyLabel("Here");
        ImGui::TextUnformatted(
            network->authority()              ? "authority: this machine decides its state"
            : network->isLocallyControlled(e) ? "predicted: your input moves it, the host corrects it"
                                              : "replica: shown from the host's snapshots");
        gui::propertyLabel("Profile");
        ImGui::TextUnformatted(object.profile.c_str());
        if (!network->authority()) {
            network->peerInfo(m_peers);
            gui::propertyLabel("Snapshot age");
            if (!m_peers.empty() && m_peers[0].snapshotAge >= 0)
                ImGui::Text("%.0f ms", m_peers[0].snapshotAge * 1000);
            else
                ImGui::TextDisabled("none yet");
        }
        ImGui::EndTable();
    }
}

void Editor::drawNetworkWindow() {
    ImGui::SetNextWindowSize(ImVec2(820, 520), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Network", &m_showNetwork)) {
        ImGui::End();
        return;
    }
    if (!m_hasProject) {
        ImGui::TextDisabled("Open a project to play it online.");
        ImGui::End();
        return;
    }
    const ImVec4 errorColor = gui::semanticColor(gui::currentTheme().error);
    const ImVec4 warningColor = gui::semanticColor(gui::currentTheme().warning);
    const net::NetworkWorld* network = m_session ? m_session->network() : nullptr;
    const bool online = network && network->online();

    // ---- Top: what we are, and the match buttons ----
    ImGui::Text(
        "%s  |  %s", online ? net::networkRoleName(network->role()) : "Offline",
        online ? net::networkStateName(network->state()) : net::matchStateName(m_match.state()));
    ImGui::SameLine();
    if (m_steam && m_steam->available()) {
        const std::string id = std::to_string(m_steam->localPeer().value);
        ImGui::TextDisabled("  SteamID %s", id.c_str());
        ImGui::SameLine();
        if (gui::button("Copy##steamid")) ImGui::SetClipboardText(id.c_str());
        ImGui::SetItemTooltip("Copy your SteamID (for developer direct joins)");
    } else {
        ImGui::TextDisabled("  Steam %s", m_steam ? "isn't signed in" : "isn't running (it starts on Host or Join)");
    }
    const bool busy = m_match.busy();
    ImGui::BeginDisabled(busy || playing());
    if (gui::button("Host")) {
        if (prepareOnlineMatch() && !m_match.host()) error(m_match.error());
    }
    ImGui::SetItemTooltip("Make a friends-only lobby and play the open scene as its host");
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!busy && !online);
    if (gui::button("Leave")) {
        m_match.leave();
        if (playing() && networkPlay()) stop();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(
        !(m_match.state() == net::MatchState::Hosting || m_match.state() == net::MatchState::Joined) ||
        !m_match.lobby().valid());
    if (gui::button("Invite")) {
        if (!m_match.invite()) error(m_match.error());
    }
    ImGui::SetItemTooltip("Open Steam's invite dialog for this lobby");
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("%s", m_match.status().c_str());
    if (m_match.state() == net::MatchState::Failed) ImGui::TextColored(errorColor, "%s", m_match.error().c_str());
    if (m_match.pendingJoin().valid()) {
        ImGui::TextColored(
            warningColor, "A friend's invite to lobby %llu is waiting.",
            static_cast<unsigned long long>(m_match.pendingJoin().value));
        ImGui::SameLine();
        if (gui::button("Leave and join")) {
            if (playing()) stop();
            if (!m_match.acceptPendingJoin()) error(m_match.error());
        }
        ImGui::SameLine();
        if (gui::button("Decline")) m_match.declinePendingJoin();
    }
    if (m_reloadAfterNetworkPlay)
        ImGui::TextColored(warningColor, "Game code changed: it reloads when you Stop (network play holds reloads).");
    if (!online && !m_lastMatchEnd.empty()) ImGui::TextDisabled("Last match: %s", m_lastMatchEnd.c_str());

    // ---- Join ----
    if (ImGui::CollapsingHeader("Find / Join", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::BeginDisabled(busy || playing() || m_match.searching());
        if (gui::button(m_match.searching() ? "Searching..." : "Find matches")) {
            if (prepareOnlineMatch() && !m_match.find()) error(m_match.error());
        }
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Lobbies of this game and protocol (at most 50); join one with the same scene open");
        if (!m_match.lobbies().empty() &&
            ImGui::BeginTable(
                "lobbies", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
                ImVec2(0, 140))) {
            ImGui::TableSetupColumn("Host");
            ImGui::TableSetupColumn("Players");
            ImGui::TableSetupColumn("Scene");
            ImGui::TableSetupColumn("State");
            ImGui::TableSetupColumn("");
            ImGui::TableHeadersRow();
            for (const net::LobbyListing& lobby : m_match.lobbies()) {
                ImGui::PushID(static_cast<int>(lobby.id.value & 0x7FFFFFFF));
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(
                    lobby.hostName.empty() ? "?" : lobby.hostName.c_str());   // untrusted: shown, never an ID
                ImGui::TableNextColumn();
                ImGui::Text("%u / %u", unsigned(lobby.players), unsigned(lobby.capacity));
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(lobby.scene.c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(lobby.state.c_str());
                ImGui::TableNextColumn();
                if (lobby.compatible) {
                    ImGui::BeginDisabled(busy || playing());
                    if (gui::button("Join")) {
                        if (prepareOnlineMatch() && !m_match.join(lobby.id)) error(m_match.error());
                    }
                    ImGui::EndDisabled();
                } else {
                    ImGui::TextColored(warningColor, "can't join");
                    ImGui::SetItemTooltip("%s", lobby.problem.c_str());
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        // Developer: by ID, labelled as such. Friends join through invites or the list.
        ImGui::TextDisabled("Developer:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(170);
        ImGui::InputTextWithHint("##lobbyid", "lobby ID", &m_joinLobbyText, ImGuiInputTextFlags_CharsDecimal);
        ImGui::SameLine();
        ImGui::BeginDisabled(busy || playing() || m_joinLobbyText.empty());
        if (gui::button("Join lobby")) {
            uint64_t id = 0;
            std::from_chars(m_joinLobbyText.data(), m_joinLobbyText.data() + m_joinLobbyText.size(), id);
            if (prepareOnlineMatch() && !m_match.join(net::LobbyId{id})) error(m_match.error());
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(170);
        ImGui::InputTextWithHint("##steamid", "host SteamID", &m_directJoinText, ImGuiInputTextFlags_CharsDecimal);
        ImGui::SameLine();
        ImGui::BeginDisabled(busy || playing() || m_directJoinText.empty());
        if (gui::button("Direct join (dev)")) {
            uint64_t id = 0;
            std::from_chars(m_directJoinText.data(), m_directJoinText.data() + m_directJoinText.size(), id);
            if (prepareOnlineMatch() && !m_match.joinDirect(net::PeerId{id})) error(m_match.error());
        }
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Developer only: connects to a host started with Host direct (dev), still authenticated");
        ImGui::SameLine();
        ImGui::BeginDisabled(busy || playing());
        if (gui::button("Host direct (dev)")) {
            if (prepareOnlineMatch() && !m_match.hostDirect()) error(m_match.error());
        }
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Developer only: host with no lobby, for Direct join");
    }

    // ---- Peers ----
    if (ImGui::CollapsingHeader("Peers", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (!online) {
            ImGui::TextDisabled("No network play.");
        } else {
            network->peerInfo(m_peers);
            // KiB/s from each peer's byte counters, once a second.
            m_peerRateAge += ImGui::GetIO().DeltaTime;
            if (m_peerRateAge >= 1.0) {
                std::vector<PeerRate> next;
                for (const net::NetworkPeerInfo& p : m_peers) {
                    PeerRate rate{p.slot, p.bytesReceived, p.bytesSent, 0, 0};
                    for (const PeerRate& before : m_peerRates)
                        if (before.slot == p.slot && p.bytesReceived >= before.in && p.bytesSent >= before.out) {
                            rate.inKiB = double(p.bytesReceived - before.in) / 1024.0 / m_peerRateAge;
                            rate.outKiB = double(p.bytesSent - before.out) / 1024.0 / m_peerRateAge;
                        }
                    next.push_back(rate);
                }
                m_peerRates = std::move(next);
                m_peerRateAge = 0;
            }
            if (ImGui::BeginTable(
                    "peers", 9, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
                    ImVec2(0, 190))) {
                for (const char* column :
                     {"Slot", "Name", "Status", "RTT", "In KiB/s", "Out KiB/s", "Pending", "Input age", "Snapshot age"})
                    ImGui::TableSetupColumn(column);
                ImGui::TableHeadersRow();
                for (const net::NetworkPeerInfo& p : m_peers) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    if (p.slot)
                        ImGui::Text("%u", unsigned(p.slot));
                    else
                        ImGui::TextDisabled("-");
                    ImGui::TableNextColumn();
                    const std::string name = m_steam ? m_steam->displayName(p.peer) : std::string();
                    ImGui::TextUnformatted(name.empty() ? (network->authority() ? "player" : "host") : name.c_str());
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(p.status);
                    ImGui::TableNextColumn();
                    if (p.rttMs > 0)
                        ImGui::Text("%.0f ms", p.rttMs);
                    else
                        ImGui::TextDisabled("-");
                    double in = 0, out = 0;
                    for (const PeerRate& r : m_peerRates)
                        if (r.slot == p.slot) in = r.inKiB, out = r.outKiB;
                    ImGui::TableNextColumn();
                    ImGui::Text("%.1f", in);
                    ImGui::TableNextColumn();
                    ImGui::Text("%.1f", out);
                    ImGui::TableNextColumn();
                    ImGui::Text(
                        "%u B", unsigned(p.pendingReliableBytes + p.pendingUnreliableBytes + p.queuedControlBytes));
                    ImGui::TableNextColumn();
                    if (p.inputAge >= 0)
                        ImGui::Text("%.0f ms", p.inputAge * 1000);
                    else
                        ImGui::TextDisabled("-");
                    ImGui::TableNextColumn();
                    if (p.snapshotAge >= 0)
                        ImGui::Text("%.0f ms", p.snapshotAge * 1000);
                    else
                        ImGui::TextDisabled("-");
                }
                ImGui::EndTable();
            }
            const net::NetworkWorldStats& s = network->stats();
            ImGui::TextDisabled(
                "Corrections %llu (max %.3f m), %llu ticks replayed, %llu input fallbacks, %llu invalid messages; "
                "budget %u B/s per peer",
                static_cast<unsigned long long>(s.corrections), double(s.maxCorrection),
                static_cast<unsigned long long>(s.replayedTicks), static_cast<unsigned long long>(s.inputFallbacks),
                static_cast<unsigned long long>(s.malformed),
                unsigned(network->start().settings.outboundBytesPerSecond));
            if (network->state() == net::NetworkState::Failed)
                ImGui::TextColored(errorColor, "%s", network->failureText().c_str());
        }
    }

    // ---- Footer ----
    ImGui::Separator();
    if (gui::button("Copy diagnostics")) {
        ImGui::SetClipboardText(networkDiagnostics().c_str());
        log("Copied network diagnostics to the clipboard");
    }
    ImGui::SameLine();
    if (m_capture) {
        ImGui::Text("Capturing: %llu records", static_cast<unsigned long long>(m_capture->recordsWritten()));
    } else {
        ImGui::Checkbox("Capture packets", &m_captureNextMatch);
        ImGui::SetItemTooltip("Record the next match's messages to Saved/Captures (session tickets redacted); "
                              "read them with player --net-dump");
    }
    ImGui::SameLine();
    if (gui::button("Run local test")) {
        net::NetTestOptions options;
        if (!m_networkEdit.loaded) loadNetworkSettingsEdit();
        if (m_networkEdit.savedPresent) options.settings = m_networkEdit.saved;
        net::NetTestReport report;
        if (!net::runNetTest(options, report))
            m_localTestResult = report.errors.empty() ? "the test didn't run" : report.errors.front();
        else
            m_localTestResult = std::string(report.ok ? "passed" : "FAILED") + ": host and " +
                                std::to_string(options.clients) + " clients over a simulated " + options.preset +
                                " network (player --net-test)";
    }
    ImGui::SetItemTooltip("The session handshake between in-process peers on a simulated network; no Steam");
    ImGui::SameLine();
    ImGui::BeginDisabled(playing() || isEditingPrefab());
    if (gui::button("Assign missing network keys")) assignMissingNetworkKeysTool();
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("New keys for network objects without one, or with a repeated one; valid keys stay");
    if (!m_localTestResult.empty()) ImGui::TextDisabled("Local test %s", m_localTestResult.c_str());
    ImGui::End();
}

}   // namespace editor
