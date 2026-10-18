#include "ArenaGame.h"

#include <Shockwave/Game.h>

namespace {
class ArenaInstance final : public shockwave::GameInstance {
public:
    std::unique_ptr<shockwave::GameMode> createGameMode(const shockwave::World&) override {
        return arena::createGame();
    }
};
const shockwave::detail::AutoComponent arenaTypes{arena::registerTypes};
}   // namespace
SW_GAME("arena", ArenaInstance)
