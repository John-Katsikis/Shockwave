#pragma once

#include <Shockwave/Game.h>

namespace arena {
// The game's presentation of the shared match API (game.onlineMatch): Host friends match,
// Find/Join, Invite, Leave, Cancel/Retry and a friend's invite. Every button calls the same
// OnlineMatch the player's command line and the editor's Network window use; nothing here
// talks to Steam. Without a match control (offline builds, tools, a project with no network
// scene) the section stays greyed out and says why.
class OnlineMenu {
public:
    static constexpr int kRows = 6;   // lobbies listed in the menu

    void bind(shockwave::GameContext& game);
    // Every frame: the menu's values from the match's state.
    void refresh(shockwave::GameContext& game);

private:
    enum class Last { None, Host, Find, Join };
    Last m_last = Last::None;   // what Retry repeats
    shockwave::net::LobbyId m_lastLobby;
};
}   // namespace arena
