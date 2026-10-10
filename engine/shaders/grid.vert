#version 460
#extension GL_EXT_scalar_block_layout : require

// The editor grid: one quad on the ground plane (y = 0), centred under the camera and as
// wide as the fade distance, so the grid looks endless. No buffers: the six corners come
// from gl_VertexIndex.
layout(push_constant, scalar) uniform PushConstants {
    mat4 viewProjection;
    vec4 cameraPosition;   // xyz; w: half the quad's size
    vec4 params;           // x: cell size, y: cells per major line, z: opacity
} pc;

layout(location = 0) out vec3 vWorld;

const vec2 kCorners[6] = vec2[](vec2(-1, -1), vec2(1, -1), vec2(1, 1), vec2(-1, -1), vec2(1, 1), vec2(-1, 1));

void main() {
    // Snap the centre to whole major cells so the lines don't swim as the camera moves.
    float major = pc.params.x * pc.params.y;
    vec2 centre = floor(pc.cameraPosition.xz / major) * major;
    vec2 xz = centre + kCorners[gl_VertexIndex] * pc.cameraPosition.w;
    vWorld = vec3(xz.x, 0.0, xz.y);
    gl_Position = pc.viewProjection * vec4(vWorld, 1.0);
}
