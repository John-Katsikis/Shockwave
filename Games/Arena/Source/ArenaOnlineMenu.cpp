#include "ArenaOnlineMenu.h"

#include <cctype>
#include <string>

namespace arena {
using namespace shockwave;
namespace {
std::string upper(std::string text) {
    for (char& c : text) c = char(std::toupper(static_cast<unsigned char>(c)));
    return text;
}
}   // namespace

void OnlineMenu::bind(GameContext& game) {
    auto& ui = game.ui();
    net::OnlineMatch* match = game.onlineMatch;
    ui.on("arena_host", [this, match](const UIEvent&) {
        if (!match) return;
        m_last = Last::Host;
        match->host();
    });
    ui.on("arena_find", [this, match](const UIEvent&) {
        if (!match) return;
        m_last = Last::Find;
        match->find();
    });
    for (int i = 0; i != kRows; ++i) {
        ui.on("arena_join_" + std::to_string(i + 1), [this, match, i](const UIEvent&) {
            if (!match || size_t(i) >= match->lobbies().size()) return;
            const net::LobbyListing& lobby = match->lobbies()[size_t(i)];
            if (!lobby.compatible) return;
            m_last = Last::Join;
            m_lastLobby = lobby.id;
            match->join(lobby.id);
        });
    }
    ui.on("arena_invite", [match](const UIEvent&) {
        if (match) match->invite();
    });
    // Cancel and Leave are the same request: the application stops Play and goes back
    // to the offline scene when a match was being played. A session started without the
    // match (a developer preview) just closes.
    ui.on("arena_cancel", [match, &game](const UIEvent&) {
        if (match && match->busy())
            match->leave();
        else if (game.network().online())
            game.network().close();
    });
    ui.on("arena_retry", [this, match](const UIEvent&) {
        if (!match) return;
        switch (m_last) {
        case Last::Host: match->host(); break;
        case Last::Find: match->find(); break;
        case Last::Join: match->join(m_lastLobby); break;
        case Last::None: break;
        }
    });
    ui.on("arena_accept_invite", [match](const UIEvent&) {
        if (match) match->acceptPendingJoin();
    });
    ui.on("arena_decline_invite", [match](const UIEvent&) {
        if (match) match->declinePendingJoin();
    });
    refresh(game);
}

void OnlineMenu::refresh(GameContext& game) {
    auto& ui = game.ui();
    const net::OnlineMatch* match = game.onlineMatch;
    const bool available = match && match->configured();
    ui.set("arena_online_available", available);
    if (!available) {
        const bool session = game.network().online();
        ui.set(
            "arena_online_status", session ? std::string("PLAYING ONLINE (DEVELOPER SESSION)")
                                           : std::string("ONLINE PLAY ISN'T AVAILABLE HERE: THIS GAME HAS NO "
                                                         "MULTIPLAYER SET UP"));
        for (const char* flag :
             {"arena_online_idle", "arena_online_busy", "arena_online_hosting", "arena_online_failed",
              "arena_invite_pending"})
            ui.set(flag, false);
        ui.set("arena_online_in_match", session);
        ui.set("arena_lobby_count", 0);
        return;
    }
    const net::MatchState state = match->state();
    const bool failed = state == net::MatchState::Failed;
    const bool busy = state == net::MatchState::CreatingLobby || state == net::MatchState::JoiningLobby ||
                      state == net::MatchState::Starting;
    const bool inMatch = state == net::MatchState::Hosting || state == net::MatchState::Joined;
    ui.set("arena_online_idle", state == net::MatchState::Idle || failed);
    ui.set("arena_online_busy", busy);
    ui.set("arena_online_in_match", inMatch);
    ui.set("arena_online_hosting", state == net::MatchState::Hosting && !match->directOnly());
    ui.set("arena_online_failed", !busy && !inMatch && !match->error().empty() && m_last != Last::None);
    ui.set("arena_invite_pending", match->pendingJoin().valid());
    std::string status = match->status();
    if (!match->error().empty())
        status = match->error();
    else if (match->searching())
        status = "searching for matches";
    ui.set("arena_online_status", upper(status));
    // The search's results while idle; a match in flight hides them.
    const auto& lobbies = match->lobbies();
    const int rows = busy || inMatch ? 0 : int(std::min<size_t>(lobbies.size(), kRows));
    ui.set("arena_lobby_count", rows);
    for (int i = 0; i != kRows; ++i) {
        std::string row;
        if (i < rows) {
            const net::LobbyListing& lobby = lobbies[size_t(i)];
            row = (lobby.hostName.empty() ? std::string("A FRIEND") : upper(lobby.hostName)) + "  " +
                  std::to_string(lobby.players) + "/" + std::to_string(lobby.capacity) + "  " +
                  (lobby.compatible ? upper(lobby.state.empty() ? std::string("join") : lobby.state)
                                    : "CAN'T JOIN: " + upper(lobby.problem));
        }
        ui.set("arena_lobby_" + std::to_string(i + 1), row);
    }
}
}   // namespace arena
