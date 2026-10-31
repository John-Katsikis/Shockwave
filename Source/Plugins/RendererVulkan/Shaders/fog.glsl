// Fog (A06, Renderer/PostProcess.h, docs/Fog.md): the closed form along a ray and the froxel
// volume's look-up. Include after frame.glsl. Must match fogOpticalDepth, analyticFog and
// henyeyGreenstein in Source/Runtime/Renderer/Private/PostProcess.cpp.
//
// A fog value is vec4(S, T): light scattered toward the camera, and transmittance. A
// surface's radiance L becomes S + T L; additive light only dims, T E.
const uint kFogNone = 0u;
const uint kFogAnalytic = 1u;
const uint kFogFroxels = 2u;   // the volume to volumetricDistance, analytic past it

layout(buffer_reference, scalar) readonly buffer FroxelCells {
    uvec2 cells[];   // packHalf2x16: (S.r, S.g), (S.b, T), integrated from the camera to the cell's far side
};

float fogPhase(float g, float mu) {
    g = clamp(g, -0.9, 0.9);
    mu = clamp(mu, -1.0, 1.0);
    float d = 1.0 + g * g - 2.0 * g * mu;
    return (1.0 - g * g) / (4.0 * 3.14159265 * d * sqrt(d));
}

float fogDensityAt(FrameBuffer frame, float y) {
    float e = clamp(-frame.fogHeightFalloff * (y - frame.fogBaseHeight), -30.0, 30.0);
    return min(frame.fogDensity * exp(e), frame.fogMaxDensity);
}

// Optical depth along a unit direction from a point, for len metres (7.8).
float fogOpticalDepth(FrameBuffer frame, float originY, float directionY, float len) {
    if (!(len > 0.0)) return 0.0;
    float e = clamp(-frame.fogHeightFalloff * (originY - frame.fogBaseHeight), -30.0, 30.0);
    float sigma0 = frame.fogDensity * exp(e);
    float x = max(frame.fogHeightFalloff * directionY * len, -30.0);
    float ratio = abs(x) < 1e-3 ? 1.0 - x * (0.5 - x * (1.0 / 6.0 - x / 24.0)) : (1.0 - exp(-x)) / x;
    return clamp(min(sigma0 * len * ratio, frame.fogMaxDensity * len), 0.0, 64.0);
}

// The analytic fog's light for a ray direction (camera to point): its own colour plus the
// directional lights it scatters toward the camera, unshadowed. Constant along the ray.
vec3 fogRayLight(FrameBuffer frame, vec3 direction) {
    vec3 light = frame.fogColor;
    for (uint i = 0u; i < frame.directionalCount; ++i) {
        LightData l = frame.lights[i];
        // mu: the light's propagation against the way to the camera (toward the sun: 1).
        light += frame.fogAlbedo * l.color * fogPhase(frame.fogAnisotropy, dot(l.direction, -direction));
    }
    return light;
}

// The closed form between distances near and far along a ray from the camera.
vec4 analyticFogSegment(FrameBuffer frame, vec3 direction, float near, float far) {
    near = max(near, frame.fogStart);
    if (!(far > near)) return vec4(0.0, 0.0, 0.0, 1.0);
    vec3 origin = frame.cameraPosition + direction * near;
    float t = exp(-fogOpticalDepth(frame, origin.y, direction.y, far - near));
    return vec4(fogRayLight(frame, direction) * (1.0 - t), t);
}

vec4 froxelCell(FrameBuffer frame, uint x, uint y, uint slice) {
    uvec2 packed = FroxelCells(frame.froxels).cells[(slice * frame.froxelTilesY + y) * frame.froxelTilesX + x];
    return vec4(unpackHalf2x16(packed.x), unpackHalf2x16(packed.y));
}

// The volume's value at a slice boundary b (0: the near plane, nothing yet), bilinear across tiles.
vec4 froxelBoundary(FrameBuffer frame, vec2 tile, uint b) {
    if (b == 0u) return vec4(0.0, 0.0, 0.0, 1.0);
    vec2 p = clamp(tile - 0.5, vec2(0.0), vec2(frame.froxelTilesX - 1u, frame.froxelTilesY - 1u));
    uvec2 i0 = uvec2(floor(p));
    uvec2 i1 = min(i0 + 1u, uvec2(frame.froxelTilesX - 1u, frame.froxelTilesY - 1u));
    vec2 f = p - vec2(i0);
    vec4 a = mix(froxelCell(frame, i0.x, i0.y, b - 1u), froxelCell(frame, i1.x, i0.y, b - 1u), f.x);
    vec4 c = mix(froxelCell(frame, i0.x, i1.y, b - 1u), froxelCell(frame, i1.x, i1.y, b - 1u), f.x);
    return mix(a, c, f.y);
}

// Fog between the camera and a point: fragCoord in render pixels, the point's view depth
// (for the volume's slices) and ray distance (for the closed form).
vec4 sceneFog(FrameBuffer frame, vec2 fragCoord, vec3 worldPos) {
    if (frame.fogMode == kFogNone) return vec4(0.0, 0.0, 0.0, 1.0);
    vec3 offset = worldPos - frame.cameraPosition;
    float distance = length(offset);
    vec3 direction = distance > 1e-6 ? offset / distance : frame.cameraForward;
    if (frame.fogMode == kFogAnalytic) return analyticFogSegment(frame, direction, 0.0, distance);
    // Froxels: the volume to its far plane, then the closed form for the rest of the ray.
    float depth = max(dot(offset, frame.cameraForward), 1e-4);
    float c = float(frame.froxelSlices) * log(max(depth, 1e-6) / frame.froxelNear) /
              log(frame.fogVolumetricDistance / frame.froxelNear);
    c = clamp(c, 0.0, float(frame.froxelSlices));
    vec2 tile = fragCoord / float(frame.froxelTile);
    uint b0 = uint(floor(c));
    vec4 near = froxelBoundary(frame, tile, b0);
    vec4 far = froxelBoundary(frame, tile, min(b0 + 1u, frame.froxelSlices));
    vec4 volume = mix(near, far, c - float(b0));
    // The rest past the volume (depth beyond its far plane), along the ray.
    float volumeEnd = frame.fogVolumetricDistance * distance / depth;
    vec4 rest = analyticFogSegment(frame, direction, volumeEnd, distance);
    return vec4(volume.rgb + volume.a * rest.rgb, volume.a * rest.a);
}

// The sky, as if fogSkyDistance away along the direction.
vec4 skyFog(FrameBuffer frame, vec2 fragCoord, vec3 direction) {
    if (frame.fogMode == kFogNone) return vec4(0.0, 0.0, 0.0, 1.0);
    return sceneFog(frame, fragCoord, frame.cameraPosition + direction * frame.fogSkyDistance);
}
