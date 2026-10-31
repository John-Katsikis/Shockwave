// Auto exposure (A05, Renderer/PostProcess.h, RendererPostProcess.cpp): the persistent state
// exposure_histogram.comp fills and exposure_adapt.comp reads, adapts and clears. One for the
// renderer; tonemap.frag reads ev. Must match ExposureState in RendererImpl.h.
const uint kExposureBins = 128u;
const float kHistogramMinLog2 = -12.0;
const float kHistogramMaxLog2 = 16.0;
const float kLuminanceFloor = 0.000244140625;

layout(buffer_reference, scalar) buffer ExposureState {
    uint histogram[kExposureBins];   // metering weight (1/256ths) per log2 luminance bin
    float ev;   // the exposure in use, stops
    float target;   // the last metered target
    float meanLog2;
    uint valid;   // the last metering had weight
    uint clamped;   // weight outside the histogram's range
    uint total;   // weight metered
    uint initialized;   // ev has been set once
    uint resets;
};

// Must match ExposureReadback in RendererImpl.h: a copy of the state for the CPU's stats.
layout(buffer_reference, scalar) writeonly buffer ExposureReadback {
    float ev;
    float target;
    float meanLog2;
    uint valid;
    uint clamped;
    uint total;
    uint resets;
    uint pad;
};

// Must match ExposurePushConstants in RendererPostProcess.cpp.
layout(push_constant, scalar) uniform PushConstants {
    ExposureState state;
    ExposureReadback readback;
    uint sceneTexture;
    uint metering;   // 0 average, 1 center
    uvec2 size;   // render pixels
    float dt;   // seconds
    float compensation, minEv, maxEv, targetGray, lowPercentile, highPercentile, brightenTime, darkenTime;
    uint reset;   // jump to the target (a cut, a new view, the first frame)
} pc;
