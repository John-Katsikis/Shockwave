#pragma once

#include <GameFramework/GameMode.h>

#include <memory>
#include <string>
#include <string_view>

namespace shockwave {
class Input;
}

namespace demo {

// Config/Input.swinput's actions.
inline constexpr const char* kForward = "demo_forward";
inline constexpr const char* kBack = "demo_back";
inline constexpr const char* kLeft = "demo_left";
inline constexpr const char* kRight = "demo_right";
inline constexpr const char* kLookLeft = "demo_look_left";
inline constexpr const char* kLookRight = "demo_look_right";
inline constexpr const char* kLookUp = "demo_look_up";
inline constexpr const char* kLookDown = "demo_look_down";
inline constexpr const char* kFire = "demo_fire";
inline constexpr const char* kReload = "demo_reload";
inline constexpr const char* kUse = "demo_use";
inline constexpr const char* kSprint = "demo_sprint";

// What a prompt shows for an action: the key or button bound to it on the device the
// player used last ("E", "Left mouse", "A" or "Cross", "Right trigger"), so rebinding
// changes the prompt. "?" when nothing binds it.
std::string bindingLabel(const shockwave::Input& input, std::string_view action, bool gamepad);

std::unique_ptr<shockwave::GameMode> createGame();

}   // namespace demo
