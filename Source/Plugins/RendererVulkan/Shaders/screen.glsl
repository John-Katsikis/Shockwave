// The screen-space coordinate contract (G03): the GLSL twin of Renderer/ScreenSpace.h.
// Screen UV: u right, v down, (0, 0) the top-left corner of the image. Reverse-Z: depth 0
// is background and has no position.

vec2 screenUvFromClip(vec4 clip) {
    vec2 ndc = clip.xy / clip.w;
    return vec2(0.5 * ndc.x + 0.5, 0.5 - 0.5 * ndc.y);
}

// False for background (depth <= 0) and non-finite results.
bool viewPositionFromDepth(vec2 uv, float depth, mat4 inverseProjection, out vec3 viewPosition) {
    viewPosition = vec3(0.0);
    if (!(depth > 0.0)) return false;
    vec4 h = inverseProjection * vec4(2.0 * uv.x - 1.0, 1.0 - 2.0 * uv.y, depth, 1.0);
    if (!(abs(h.w) > 1e-20)) return false;
    viewPosition = h.xyz / h.w;
    return all(lessThan(abs(viewPosition), vec3(1e30)));
}

// Unit vectors in two numbers (octahedral mapping), for the surface normal target.
vec2 octEncode(vec3 n) {
    n /= abs(n.x) + abs(n.y) + abs(n.z);
    vec2 e = n.xy;
    if (n.z < 0.0) e = (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    return e;
}
vec3 octDecode(vec2 e) {
    vec3 n = vec3(e, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0.0) n.xy = (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    return normalize(n);
}
