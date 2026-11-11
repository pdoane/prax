// The shaders of lib/gpu's tests.

#include "pr/gpu.slang"

struct Push {
    float4 color;
    uint texture;           // sampled by `sampled`
    uint sampler;
    uint buffer;            // read by `pulled` and `doubled`, or followed by `followed`
    uint output;            // written by `doubled`
    float4 rect;            // the quad's left, top, right and bottom in clip space
};
PR_GPU_PUSH(Push, push);

struct Uniforms {
    float4 tint;
};
PR_GPU_UNIFORMS(Uniforms, uniforms);

struct VertexOut {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

// A quad over the push data's rectangle, as a strip, with uv from (0, 0) at its first corner.
[shader("vertex")]
VertexOut quadVertex(uint corner : SV_VertexID) {
    float2 uv = float2(corner & 1, corner >> 1);
    VertexOut out;
    out.position = float4(lerp(push.rect.xy, push.rect.zw, uv), 0, 1);
    out.uv = uv;
    return out;
}

// A triangle whose corners are read from the buffer, two floats each, by the vertex index.
[shader("vertex")]
VertexOut pulled(uint vertex : SV_VertexID) {
    ByteAddressBuffer corners = prGpuGet<ByteAddressBuffer>(push.buffer);
    VertexOut out;
    out.position = float4(corners.Load<float2>(vertex * 8), 0, 1);
    out.uv = 0;
    return out;
}

[shader("fragment")]
float4 solid(VertexOut input) : SV_Target {
    return push.color;
}

[shader("fragment")]
float4 tinted(VertexOut input) : SV_Target {
    return push.color * uniforms.tint;
}

[shader("fragment")]
float4 sampled(VertexOut input) : SV_Target {
    Texture2D<float4> texture = prGpuGet<Texture2D<float4>>(push.texture);
    SamplerState sampler = prGpuGet<SamplerState>(push.sampler);
    return texture.SampleLevel(sampler, input.uv, 0);
}

// The color that the buffer names, through a second buffer: the first buffer holds the handle of a buffer and the
// offset of the color's four floats in it.
[shader("fragment")]
float4 followed(VertexOut input) : SV_Target {
    uint2 at = prGpuGet<ByteAddressBuffer>(push.buffer).Load<uint2>(0);
    return prGpuGet<ByteAddressBuffer>(at.x).Load<float4>(at.y);
}

// The target's color inverted, which reads what the draws before left.
[shader("fragment")]
float4 inverted(VertexOut input PR_GPU_TARGET_PARAM) : SV_Target {
    float4 target = prGpuTarget(input.position);
    return float4(target.a - target.rgb, target.a);
}

// Each word of the buffer, doubled, into the output.
[shader("compute")]
[numthreads(64, 1, 1)]
void doubled(uint3 id : SV_DispatchThreadID) {
    ByteAddressBuffer input = prGpuGet<ByteAddressBuffer>(push.buffer);
    RWByteAddressBuffer output = prGpuGet<RWByteAddressBuffer>(push.output);
    output.Store(id.x * 4, input.Load(id.x * 4) * 2);
}

// The same shaders over slots, which every device has. The slots are not the first of their kinds, so that a backend
// that places them at the wrong index fails.
PR_GPU_BUFFER(ByteAddressBuffer, slotCorners, 1);
PR_GPU_TEXTURE(Texture2D<float4>, slotImage, 2);
PR_GPU_BUFFER(StructuredBuffer<uint>, slotInput, 3);
PR_GPU_RW_BUFFER(RWByteAddressBuffer, slotOutput, 1);
PR_GPU_RW_TEXTURE(WTexture2D<float4>, slotWritten, 2);

// A triangle whose corners are read from a Buffer slot.
[shader("vertex")]
VertexOut slotPulled(uint vertex : SV_VertexID) {
    VertexOut out;
    out.position = float4(slotCorners.Load<float2>(vertex * 8), 0, 1);
    out.uv = 0;
    return out;
}

// The Texture slot, with its sampler.
[shader("fragment")]
float4 slotSampled(VertexOut input) : SV_Target {
    return slotImage.SampleLevel(slotImageSampler, input.uv, 0);
}

// Each word of a Buffer slot, doubled, into an RwBuffer slot.
[shader("compute")]
[numthreads(64, 1, 1)]
void slotDoubled(uint3 id : SV_DispatchThreadID) {
    slotOutput.Store(id.x * 4, slotInput[id.x] * 2);
}

// The push data's color, over the RwTexture slot, which it only writes.
[shader("compute")]
[numthreads(8, 8, 1)]
void slotFilled(uint3 id : SV_DispatchThreadID) {
    slotWritten.Store(id.xy, push.color);
}
