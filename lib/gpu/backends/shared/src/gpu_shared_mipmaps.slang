// The shaders of the backends' shared mip levels: each level drawn from the level before it
// (prGpuCommandsGenerateMipmaps).

#include "pr/gpu.slang"

// The level before, with a linear sampler that clamps.
PR_GPU_TEXTURE(Texture2D<float4>, level, 0);

struct VertexOut {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

// A triangle that covers the target, with uv from (0, 0) at its top left to (1, 1) at its bottom right.
[shader("vertex")]
VertexOut mipVertex(uint corner : SV_VertexID) {
    float2 uv = float2((corner << 1) & 2, corner & 2);
    VertexOut out;
    out.position = float4(uv.x * 2 - 1, 1 - uv.y * 2, 0, 1);
    out.uv = uv;
    return out;
}

// The level before, sampled bilinearly at the middle of each two by two texels, which is their average where its size
// is even.
[shader("fragment")]
float4 mipFragment(VertexOut input) : SV_Target {
    return level.SampleLevel(levelSampler, input.uv, 0);
}
