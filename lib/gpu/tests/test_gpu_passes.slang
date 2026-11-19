// The shaders of lib/gpu's tests of what a 3D renderer's passes use: samples and their resolve, depth alone, depth read
// by texel, indirect draws whose arguments a dispatch writes, and every Texture slot at once. They reach resources
// through slots alone, so that they run on every device, and are a source of their own, apart from test_gpu.slang's
// slots.

#include "pr/gpu.slang"

struct Push {
    float4 color;
    float4 rect;            // the quad's left, top, right and bottom in clip space
    float depth;            // the quad's depth
    uint pattern;           // the Texture slots that hold one, as bits
    uint2 unused;
};
PR_GPU_PUSH(Push, push);

PR_GPU_TEXTURE(Texture2D<float4>, slot0, 0);
PR_GPU_TEXTURE(Texture2D<float4>, slot1, 1);
PR_GPU_TEXTURE(Texture2D<float4>, slot2, 2);
PR_GPU_TEXTURE(Texture2D<float4>, slot3, 3);
PR_GPU_TEXTURE(Texture2D<float4>, slot4, 4);
PR_GPU_TEXTURE(Texture2D<float4>, slot5, 5);
PR_GPU_TEXTURE(Texture2D<float4>, slot6, 6);
PR_GPU_TEXTURE(Texture2D<float4>, slot7, 7);
PR_GPU_RW_BUFFER(RWByteAddressBuffer, arguments, 0);

struct VertexOut {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

// A quad over the push data's rectangle at its depth, as a strip, with uv from (0, 0) at its first corner. A draw from
// any first vertex draws the same quad.
[shader("vertex")]
VertexOut passQuad(uint vertex : SV_VertexID) {
    uint corner = vertex & 3;
    float2 uv = float2(corner & 1, corner >> 1);
    VertexOut out;
    out.position = float4(lerp(push.rect.xy, push.rect.zw, uv), push.depth, 1);
    out.uv = uv;
    return out;
}

// A triangle of the push data's rectangle, from its left edge to a point at the middle of its right edge, whose edges
// cut pixels at a slant.
[shader("vertex")]
VertexOut passTriangle(uint vertex : SV_VertexID) {
    float2 uv = vertex == 0 ? float2(0, 0) : vertex == 1 ? float2(1, 0.5) : float2(0, 1);
    VertexOut out;
    out.position = float4(lerp(push.rect.xy, push.rect.zw, uv), push.depth, 1);
    out.uv = uv;
    return out;
}

[shader("fragment")]
float4 passSolid(VertexOut input) : SV_Target {
    return push.color;
}

// The depth in Texture slot 0, read by texel, as red.
[shader("fragment")]
float4 passDepthRead(VertexOut input) : SV_Target {
    return float4(slot0.Load(int3(int2(input.position.xy), 0)).r, 0, 0, 1);
}

// The Texture slots, each sampled at its one texel, summed by their bits: the red of slot i weighs 2^i / 255.
[shader("fragment")]
float4 passEightSlots(VertexOut input) : SV_Target {
    float2 at = float2(0.5, 0.5);
    float sum = slot0.SampleLevel(slot0Sampler, at, 0).r * 1 + slot1.SampleLevel(slot1Sampler, at, 0).r * 2 +
        slot2.SampleLevel(slot2Sampler, at, 0).r * 4 + slot3.SampleLevel(slot3Sampler, at, 0).r * 8 +
        slot4.SampleLevel(slot4Sampler, at, 0).r * 16 + slot5.SampleLevel(slot5Sampler, at, 0).r * 32 +
        slot6.SampleLevel(slot6Sampler, at, 0).r * 64 + slot7.SampleLevel(slot7Sampler, at, 0).r * 128;
    return float4(sum / 255, 0, 0, 1);
}

// Writes two draws' arguments into the RwBuffer slot, 32 bytes apart: the first draws no vertices and the second a quad
// from vertex 4, both as plain draws and as indexed draws of the first indices.
[shader("compute")]
[numthreads(1, 1, 1)]
void passArguments(uint3 id : SV_DispatchThreadID) {
    arguments.Store4(0, uint4(0, 1, 0, 0));
    arguments.Store4(32, uint4(4, 1, 4, 0));
    arguments.Store4(64, uint4(0, 1, 0, 0));
    arguments.Store(80, 0);
    arguments.Store4(96, uint4(4, 1, 0, 4));
    arguments.Store(112, 0);
}
