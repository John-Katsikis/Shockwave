// The frame's shared data: camera, lights, shadows and the screen-space contract's
// matrices. Read by mesh.frag and ssao.comp through a device address.
// Must match LightData, ShadowView and FrameData in RendererImpl.h.
struct LightData {
    vec3 position;
    uint type;        // 0 directional, 1 point, 2 spot
    vec3 direction;   // where the light shines
    float range;
    vec3 color;       // color times intensity
    float cosOuter;
    float cosInner;
    uint shadowView;  // first shadow map; 0xFFFFFFFF: casts none
    float shadowStrength;
    float shadowSoftness;   // filter radius, texels
    float shadowBias;       // normal offset, texels
    float pad0, pad1, pad2;
};

struct ShadowView {
    mat4 viewProjection;
    vec4 rect;          // uv offset (xy) and scale (zw) in its atlas
    float texelSize;    // world size of a texel (at distance 1 when perspective)
    uint perspective;
    float pad0, pad1;
};

layout(buffer_reference, scalar) readonly buffer ShadowViews {
    ShadowView views[];
};

layout(buffer_reference, scalar) readonly buffer FrameBuffer {
    vec3 cameraPosition;
    uint lightCount;
    vec3 skyAmbient;
    uint debugView;   // 0 lit, 1 unlit, 2 normals, 3 lighting only, 4 shadow cascades, 5 ambient occlusion, 6 indirect, 7 reflections, 8 decals, 9 motion, 10 reactive, 11 clusters, 12 fog, 13 transmission, 14 shading families
    vec3 groundAmbient;
    uint cascadeCount;
    vec3 cameraForward;
    float shadowDistance;
    vec4 cascadeEnds;   // view depth where each sun cascade ends
    uint cascadeAtlas;
    uint localAtlas;
    uint shadowTaps;
    uint brdfTable;   // bindless slot of image-based lighting's BRDF table
    uvec2 shadowViews;   // a ShadowViews: MoltenVK reads references kept inside buffers as null
    float specularLevels;   // mip levels of specularCube
    uint aoTexture;   // bindless slot of the final ambient occlusion (G04); 0: none this frame
    mat4 view;
    mat4 projection;
    mat4 inverseProjection;
    vec2 renderSize;   // the scene targets' size in pixels
    float aoRadius;   // metres
    float aoIntensity;
    uvec2 reflectionProbes;   // a ReflectionProbes (reflection.glsl, G07)
    uint reflectionProbeCount;
    // G08: bindless slots of the decal buffers (decal.frag); 0: no decals this frame.
    uint decalAlbedo;   // premultiplied linear colour and coverage
    uint decalMaterial;   // premultiplied roughness (r), metallic (g), coverage (a)
    // G09: this frame's camera jitter in pixels (Renderer/Temporal.h); 0 without TAA.
    vec2 jitter;
    // G11: lights[0, directionalCount) are directional, the rest point and spot lights.
    uint directionalCount;
    // G11: the cluster lists (clusters.glsl), (0, 0): every pixel loops over every light.
    uvec2 clusters;
    uint clusterTile;   // pixels
    uint clusterTilesX, clusterTilesY, clusterSlices;
    float clusterNear, clusterFar;   // positive view distances
    // A06: fog (fog.glsl, Renderer/PostProcess.h). fogMode 0 none, 1 analytic, 2 froxels.
    uint fogMode;
    float fogDensity, fogHeightFalloff, fogBaseHeight, fogMaxDensity;
    vec3 fogColor;
    float fogAlbedo, fogAnisotropy, fogStart, fogSkyDistance;
    float fogVolumetricDistance;   // the froxel volume's far plane, metres of view depth
    uint froxelTile, froxelTilesX, froxelTilesY, froxelSlices;
    float froxelNear;
    uvec2 froxels;   // FroxelCells
    // A07b: the transmission pass's copy of the opaque scene (with mips) and the scene's
    // depth, bindless slots; 0: no transmission pass this frame.
    uint transmissionBackground;
    uint sceneDepth;
    float transmissionLevels;   // mips of the copy
    uint shadingPad;
    LightData lights[];
};
