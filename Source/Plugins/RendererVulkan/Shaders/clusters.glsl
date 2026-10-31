// Clustered local lighting's lists (G11, Renderer/Clusters.h, RendererClusters.cpp): what
// clusters.comp writes and mesh.frag reads. Include after frame.glsl.
//
// Cluster (x, y, s) is tile (x, y) of clusterTile render pixels at slice s of positive view
// distance, logarithmic from clusterNear to clusterFar. Its list is kClusterWords words: the
// count (top bit: overflowed, so test every local light), then light indices four a word,
// eight bits each, in light table order.
const uint kClusterCapacity = 64u;
const uint kClusterWords = 1u + kClusterCapacity / 4u;
const uint kClusterOverflow = 0x80000000u;
const uint kNoCluster = 0xFFFFFFFFu;

layout(buffer_reference, scalar) buffer ClusterLists {
    uint words[];
};

uint clusterSlice(FrameBuffer frame, float z) {
    float t = log(max(z, 1e-6) / frame.clusterNear) / log(frame.clusterFar / frame.clusterNear);
    return uint(clamp(floor(t * float(frame.clusterSlices)), 0.0, float(frame.clusterSlices - 1u)));
}

// The first word of the list for the pixel at fragCoord (render pixels) whose surface is z
// metres in front of the camera; kNoCluster: test every local light (no lists this frame,
// or past clusterFar).
uint clusterListStart(FrameBuffer frame, vec2 fragCoord, float z) {
    if (frame.clusters == uvec2(0u) || !(z < frame.clusterFar)) return kNoCluster;
    uvec2 tile = min(uvec2(fragCoord) / frame.clusterTile, uvec2(frame.clusterTilesX - 1u, frame.clusterTilesY - 1u));
    uint slice = clusterSlice(frame, z);
    return ((slice * frame.clusterTilesY + tile.y) * frame.clusterTilesX + tile.x) * kClusterWords;
}
