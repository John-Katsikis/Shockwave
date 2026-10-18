#pragma once

#include <Shockwave/Game.h>

namespace arena {
// The game's presentation of the shared graphics API. No renderer/backend access.
class GraphicsMenu {
public:
    void bind(shockwave::GameContext& game);
    void close(shockwave::GameContext& game);
    bool visible() const { return m_visible; }

private:
    void open(shockwave::GameContext& game);
    void refresh(shockwave::GameContext& game);
    shockwave::GraphicsSettings m_draft;
    bool m_visible = false;
};
}   // namespace arena
