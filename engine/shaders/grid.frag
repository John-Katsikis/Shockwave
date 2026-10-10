#version 460
#extension GL_EXT_scalar_block_layout : require

layout(push_constant, scalar) uniform PushConstants {
    mat4 viewProjection;
    vec4 cameraPosition;
    vec4 params;
} pc;

layout(location = 0) in vec3 vWorld;
layout(location = 0) out vec4 outColor;

// 1 on a line one pixel wide, 0 away from it, antialiased by screen-space derivatives.
float lines(vec2 coord) {
    vec2 width = fwidth(coord);
    vec2 g = abs(fract(coord - 0.5) - 0.5) / max(width, vec2(1e-6));
    return 1.0 - min(min(g.x, g.y), 1.0);
}

float axisLine(float coord) {
    return 1.0 - min(abs(coord) / max(fwidth(coord), 1e-6), 1.0);
}

void main() {
    float cell = pc.params.x;
    float major = cell * pc.params.y;
    vec2 p = vWorld.xz;

    // Minor lines fade out before they get dense enough to shimmer.
    vec2 density = fwidth(p / cell);
    float minorFade = 1.0 - smoothstep(0.15, 0.45, max(density.x, density.y));
    float minor = lines(p / cell) * minorFade;
    float majorLine = lines(p / major);

    vec3 color = vec3(0.32, 0.33, 0.36);
    float alpha = max(minor * 0.35, majorLine * 0.7);

    // The world axes: X in red (along z = 0), Z in blue (along x = 0).
    float xAxis = axisLine(p.y);
    float zAxis = axisLine(p.x);
    color = mix(color, vec3(0.80, 0.20, 0.22), xAxis);
    color = mix(color, vec3(0.20, 0.36, 0.85), zAxis);
    alpha = max(alpha, max(xAxis, zAxis) * 0.9);

    // Fade with distance from the camera, and at grazing angles where lines alias.
    float distance = length(vWorld - pc.cameraPosition.xyz);
    float radius = pc.cameraPosition.w;
    float fade = 1.0 - smoothstep(radius * 0.35, radius * 0.95, distance);
    vec3 toCamera = normalize(pc.cameraPosition.xyz - vWorld);
    fade *= smoothstep(0.0, 0.12, abs(toCamera.y));

    alpha *= fade * pc.params.z;
    if (alpha < 0.002) discard;
    outColor = vec4(color, alpha);
}
