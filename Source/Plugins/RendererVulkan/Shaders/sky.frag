#version 460
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference_uvec2 : require
#extension GL_EXT_scalar_block_layout : require
#extension GL_GOOGLE_include_directive : require

layout(set = 0, binding = 1) uniform sampler linearSampler;
layout(set = 0, binding = 3) uniform textureCube environmentCube;

#include "frame.glsl"
#include "fog.glsl"

// Must match SkyPushConstants in RendererEnvironment.cpp.
layout(push_constant, scalar) uniform PushConstants {
    mat4 inverseViewProjection;
    FrameBuffer frame;
    vec4 background;   // w 1: no environment map, the clear colour instead (fog still applies)
} pc;

layout(location = 0) in vec4 vDirection;
layout(location = 0) out vec4 outColor;

void main() {
    vec3 direction = normalize(vDirection.xyz / vDirection.w);
    vec3 sky = pc.background.w > 0.5 ? pc.background.rgb
                                     : textureLod(samplerCube(environmentCube, linearSampler), direction, 0.0).rgb;
    // Fog (A06): the sky counts as fogSkyDistance away. The fog view shows transmittance.
    FrameBuffer frame = pc.frame;
    if (frame.fogMode != kFogNone && (frame.debugView == 0u || frame.debugView == 12u)) {
        vec4 fog = skyFog(frame, gl_FragCoord.xy, direction);
        sky = frame.debugView == 12u ? vec3(fog.a) : fog.rgb + fog.a * sky;
    } else if (frame.debugView == 12u) {
        sky = vec3(1.0);
    }
    outColor = vec4(sky, 1.0);
}
