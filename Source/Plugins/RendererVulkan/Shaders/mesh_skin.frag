#version 460
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference_uvec2 : require
#extension GL_EXT_scalar_block_layout : require
#extension GL_GOOGLE_include_directive : require

// The skin pass (A08, RendererShading.cpp): skin surfaces' diffuse light before their colour,
// and their colour and receiver, for the diffusion blur (skin.frag). mesh.glsl with
// MESH_SKIN_PASS.
#define MESH_SKIN_PASS
#include "mesh.glsl"
