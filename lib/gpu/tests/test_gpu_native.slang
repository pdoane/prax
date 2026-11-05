// The shaders of the tests of recording into the application's command buffers and passes, which each backend runs.

#include "pr/gpu.slang"

// The root data of every test shader. Each shader reads the fields that it needs.
struct Push {
    uint output;            // the storage handle of the buffer that `fill` writes
    uint count;
    uint scale;
};
PR_GPU_PUSH(Push, push);

struct Uniforms {
    float4 color;
};
PR_GPU_UNIFORMS(Uniforms, uniforms);

// Writes each index times the scale.
[shader("compute")]
[numthreads(64, 1, 1)]
void fill(uint3 id : SV_DispatchThreadID) {
    if (id.x < push.count)
        prGpuGet<RWByteAddressBuffer>(push.output).Store(id.x * 4, id.x * push.scale);
}

struct VertexOut {
    float4 position : SV_Position;
};

// A strip of four vertices that covers the upper half of clip space, where y is positive.
[shader("vertex")]
VertexOut upperHalf(uint vertex : SV_VertexID) {
    float2 corner = float2(float(vertex & 1), float(vertex >> 1));
    VertexOut out;
    out.position = float4(corner.x * 2 - 1, corner.y, 0, 1);
    return out;
}

[shader("fragment")]
float4 solid(VertexOut input) : SV_Target {
    return uniforms.color;
}
