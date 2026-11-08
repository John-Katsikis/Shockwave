// Reactor Run's module, with the types registered
// from a function so the tests can link the same sources without the module.
#include "DemoComponents.h"
#include "DemoGame.h"

#include <Shockwave/Game.h>

namespace {
class DemoInstance final : public shockwave::GameInstance {
public:
    std::unique_ptr<shockwave::GameMode> createGameMode(const shockwave::World&) override { return demo::createGame(); }
};
const shockwave::detail::AutoComponent demoTypes{demo::registerTypes};
}   // namespace
SW_GAME("shippingdemo", DemoInstance)
