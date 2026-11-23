// Depth of field (V2, RendererScreenEffects.cpp, Renderer/DepthOfField.h): what the three
// passes share. params: x focus distance, y focus range, z largest blur in full-resolution
// pixels, w falloff (metres).

// Signed blur radius in full-resolution pixels: negative in front of the focus.
float circleOfConfusion(float viewDistance) {
    if (isinf(viewDistance)) return pc.params.z;
    float offset = viewDistance - pc.params.x;
    float outside = abs(offset) - pc.params.y;
    if (outside <= 0.0) return 0.0;
    float radius = min(outside / max(pc.params.w, 1e-4), 1.0) * pc.params.z;
    return offset < 0.0 ? -radius : radius;
}

float cocAtTexel(ivec2 texel) {
    ivec2 size = ivec2(pc.frame.renderSize);
    texel = clamp(texel, ivec2(0), size - 1);
    vec2 centre = (vec2(texel) + 0.5) / vec2(size);
    vec3 position;
    float distance = 1.0 / 0.0;
    if (viewPositionFromDepth(centre, texelFetch(depthTexture, texel, 0).r, pc.frame.inverseProjection, position))
        distance = -position.z;
    return circleOfConfusion(distance);
}
