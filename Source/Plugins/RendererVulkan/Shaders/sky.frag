#version 460

layout(set = 0, binding = 1) uniform sampler linearSampler;
layout(set = 0, binding = 3) uniform textureCube environmentCube;

layout(location = 0) in vec4 vDirection;
layout(location = 0) out vec4 outColor;

void main() {
    vec3 direction = normalize(vDirection.xyz / vDirection.w);
    outColor = vec4(textureLod(samplerCube(environmentCube, linearSampler), direction, 0.0).rgb, 1.0);
}
