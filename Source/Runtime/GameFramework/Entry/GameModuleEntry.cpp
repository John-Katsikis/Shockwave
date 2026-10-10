// Compiled into every game module (shockwave_game_module in CMake): makes the module's
// projectGameModule() loadable as the plugin game_<id>.

#include <Core/Plugin.h>
#include <GameFramework/GameModule.h>

namespace shockwave {
namespace {
const PluginDesc plugin{
    .kind = "game",
    .name = projectGameModule().id.data(),   // ids are string literals, so this ends in a NUL
    .description = "game module",
    .api = &projectGameModule(),
};
}   // namespace
}   // namespace shockwave

SHOCKWAVE_PLUGIN(shockwave::plugin)
