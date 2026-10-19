// Builds without the Steamworks SDK: no SDK backend, so SteamServices::create() makes a
// service whose initialisation fails with Unavailable. Compiled into every build; the SDK
// backend (SteamSdk.cpp) replaces it when SHOCKWAVE_ENABLE_STEAM is defined.
#ifndef SHOCKWAVE_ENABLE_STEAM

#include "SteamServicesImpl.h"

namespace shockwave::net {

std::unique_ptr<SteamApi> makeSdkSteamApi() { return nullptr; }

SteamBuild steamBuild() { return {false, ""}; }

}   // namespace shockwave::net

#endif
