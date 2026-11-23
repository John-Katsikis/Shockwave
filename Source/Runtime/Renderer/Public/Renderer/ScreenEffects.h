#pragma once
// Screen effects (V1-V4, docs/Shadows.md, docs/Reflections.md, docs/PostProcess.md): what
// the player's contact_shadows, reflections_quality and motion_blur settings ask for, and
// what the last frame drew. Depth of field is a post-process profile's (dof_* keys) with
// the player's depth_of_field setting beside color_grading (Renderer/PostProcess.h).
//
// Each one off is exactly the frame without it: no pass, no image, no memory. An effect
// that's on but can't run this frame (no surface prepass, no shadow-casting sun, no motion
// vectors) draws as off and says why in the graphics report.
#include <cstdint>

namespace shockwave {

enum class ContactShadows : uint32_t { Off, On };
const char* contactShadowsName(ContactShadows value);   // "off", "on"

enum class ScreenReflections : uint32_t { Off, Low, Medium, High };
const char* screenReflectionsName(ScreenReflections quality);   // "off", "low", "medium", "high"
uint32_t screenReflectionSteps(ScreenReflections quality);   // 0, 16, 32, 64

struct ScreenEffectsSettings {
    ContactShadows contactShadows = ContactShadows::Off;
    ScreenReflections reflections = ScreenReflections::Off;
    float motionBlur = 0.0f;   // 0..1 of a 1/120 s shutter at 60 fps; 0 off
    bool operator==(const ScreenEffectsSettings&) const = default;
};

// The last frame's (Renderer::screenEffectsStats). A reason is "" when the effect ran or
// was off; otherwise why it was asked for and didn't run.
struct ScreenEffectsStats {
    bool contactShadows = false;
    const char* contactReason = "";
    uint64_t contactBytes = 0;
    bool reflections = false;
    const char* reflectionReason = "";
    bool reflectionHistory = false;   // last frame's scene was there to reflect
    uint64_t reflectionBytes = 0;
    bool motionBlur = false;
    const char* motionBlurReason = "";
    uint64_t motionBlurBytes = 0;
    bool depthOfField = false;
    const char* depthOfFieldReason = "";
    uint64_t depthOfFieldBytes = 0;
};

}   // namespace shockwave
