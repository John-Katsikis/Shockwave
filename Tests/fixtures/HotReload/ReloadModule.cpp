// A tiny game module built twice (RELOAD_VERSION 1 and 2) for HotReloadTest: version 2
// adds a field to its component and changes what its GameMode does, as an edit would.

#include <Engine/Components.h>
#include <GameFramework/GameModule.h>

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

void registerTypes() {
    registerComponent<ReloadProbe>(
        "ReloadProbe", {
                           field("a", &ReloadProbe::a),
#if RELOAD_VERSION >= 2
                           field("b", &ReloadProbe::b),
#endif
                       });
}

}   // namespace

const GameModule& projectGameModule() {
    static const GameModule module{
        .id = "reload_test",
        .registerTypes = registerTypes,
        .createAssets = {},
        .createGameInstance = [] { return std::make_unique<SimpleGameInstance<ProbeMode>>(); },
    };
    return module;
}

}   // namespace shockwave
