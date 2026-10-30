# Lighting: the offline lighting baker (G05, G06; docs/Lighting.md). Reads a scene through
# Engine/BakedLighting.h, unwraps lightmap UVs with xatlas, traces paths on the CPU over its
# own BVH, and writes the files of Assets/LightingData.h. No GPU, window or Steam needed.
shockwave_module(Lighting PUBLIC_DEPS Shockwave::Engine PRIVATE_DEPS xatlas)
