// The shader of lib/texture's test of a procedural source: a compute shader that fills a texture.

#include "pr/gpu.slang"

struct Push {
    uint2 extent;           // of the texture, in texels
    uint cell;              // the side of a square of the checkerboard, in texels
    uint _pad;
};
PR_GPU_PUSH(Push, push);

PR_GPU_RW_TEXTURE(WTexture2D<float4>, written, 0);

// Writes a checkerboard. A square is white if its column and its row add up to an even number, and black otherwise.
[shader("compute")]
[numthreads(8, 8, 1)]
void checker(uint3 id : SV_DispatchThreadID) {
    if (id.x >= push.extent.x || id.y >= push.extent.y)
        return;
    uint sum = id.x / push.cell + id.y / push.cell;
    float value = (sum & 1) == 0 ? 1.0 : 0.0;
    written.Store(id.xy, float4(value, value, value, 1));
}
