// What the screen-effect passes share (V1-V4, RendererScreenEffects.cpp): the push
// constants, the noise and the depth buffer's surfaces. Include after frame.glsl and
// screen.glsl, with a sampler2D named depthTexture (the scene's reverse-Z depth).

// Must match ScreenPushConstants in RendererScreenEffects.cpp.
layout(push_constant, scalar) uniform PushConstants {
    mat4 matrix;   // reflections: view space to last frame's clip space
    FrameBuffer frame;
    uvec2 size;   // the image this pass writes
    vec4 params;
    vec4 params2;
    uint mode;
    uint steps;
    uint flags;
    uint pad;
} pc;

// The scene's shadow filter's noise (mesh.glsl), 0..1 at a pixel centre.
float interleavedGradientNoise(vec2 p) {
    return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
}

// The depth buffer's surface at uv in view space; false for sky. Like the CPU twins in
// Renderer/ScreenEffects.cpp: the texel's centre, not uv itself.
bool surfaceAt(vec2 uv, out vec3 surface) {
    ivec2 size = ivec2(pc.frame.renderSize);
    ivec2 texel = clamp(ivec2(uv * vec2(size)), ivec2(0), size - 1);
    vec2 centre = (vec2(texel) + 0.5) / vec2(size);
    return viewPositionFromDepth(centre, texelFetch(depthTexture, texel, 0).r, pc.frame.inverseProjection, surface);
}

// Where a view-space point lands on screen; false behind the camera or off screen.
bool projectView(vec3 point, out vec2 uv) {
    uv = vec2(0.0);
    vec4 clip = pc.frame.projection * vec4(point, 1.0);
    if (!(clip.w > 1e-6)) return false;
    uv = screenUvFromClip(clip);
    return all(greaterThanEqual(uv, vec2(0.0))) && all(lessThanEqual(uv, vec2(1.0)));
}
