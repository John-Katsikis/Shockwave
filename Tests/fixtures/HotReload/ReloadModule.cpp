// A tiny game module built twice (RELOAD_VERSION 1 and 2) for HotReloadTest: version 2
// adds a field to its component and changes what its GameMode does, as an edit would.
// Version 1 is written the long way (projectGameModule, registerComponent) and version 2
// with SW_COMPONENT and SW_GAME, so reloading also covers switching between the two.

#include <Shockwave/Game.h>

namespace shockwave {
namespace {

struct ReloadProbe {
    float a = 1.0f;
#if RELOAD_VERSION >= 2
    float b = 7.0f;   // new in version 2
#endif
};

class ProbeMode final : public GameMode {
public:
    // Marks every probe, so the test can see which version's code ran.
    void beginPlay(GameContext& ctx) override {
        ctx.world.each<ReloadProbe>([](ReloadProbe& probe) { probe.a = 100.0f * RELOAD_VERSION; });
    }
};

#if RELOAD_VERSION == 1
void registerTypes() { registerComponent<ReloadProbe>("ReloadProbe", {field("a", &ReloadProbe::a)}); }
#endif

}   // namespace

#if RELOAD_VERSION == 1
const GameModule& projectGameModule() {
    static const GameModule module{
        .id = "reload_test",
        .registerTypes = registerTypes,
        .createAssets = {},
        .createGameInstance = [] { return std::make_unique<SimpleGameInstance<ProbeMode>>(); },
    };
    return module;
}
#endif

}   // namespace shockwave

#if RELOAD_VERSION >= 2
using shockwave::ProbeMode;
using shockwave::ReloadProbe;
SW_COMPONENT(ReloadProbe, SW_FIELD(a), SW_FIELD(b, 0, 10))
SW_GAME("reload_test", ProbeMode)
#endif
