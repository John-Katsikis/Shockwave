#version 460
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_scalar_block_layout : require

// Game UI: premultiplied color times texture, blended in sRGB like a web browser. The
// target is a UNORM view of the display image and UI textures are UNORM too, so values
// pass through unconverted.
layout(set = 0, binding = 0) uniform texture2D textures[];
layout(set = 0, binding = 1) uniform sampler linearSampler;

layout(buffer_reference, scalar) readonly buffer UiVertices {
    uint unused[];
};

// Must match UiPushConstants in RendererImpl.h.
layout(push_constant, scalar) uniform PushConstants {
    mat4 transform;
    UiVertices vertices;
    uint texture;
    uint pad;
} pc;

layout(location = 0) in vec4 inColor;
layout(location = 1) in vec2 inUv;
layout(location = 0) out vec4 outColor;

void main() {
    // Level 0: glyph atlases and UI images are drawn at their own size.
    outColor = inColor * textureLod(sampler2D(textures[pc.texture], linearSampler), inUv, 0.0);
}
