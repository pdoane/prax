// The triangles renderer's shaders, as draw_triangles_frame.h specifies them: the vertex color times the texture's
// sample. A vertex shader reads its vertex from the frame's vertices, which are in a buffer that the push data names.

#include "pr/gpu.slang"

// The push data of each draw: the target's extent in pixels, the buffer of the vertices and their byte offset in it,
// and the draw's texture and sampler.
struct Draw {
    float2 size;
    uint vertices;
    uint offset;
    uint texture;
    uint sampler;
};
PR_GPU_PUSH(Draw, draw);

// A vertex as TrianglesVertex lays it out: position, uv, and straight RGBA with red in the lowest byte.
struct Vertex {
    float2 position;
    float2 uv;
    uint color;
};

struct VertexOut {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
};

[shader("vertex")]
VertexOut trianglesVertex(uint index : SV_VertexID) {
    Vertex v = prGpuGet<ByteAddressBuffer>(draw.vertices).Load<Vertex>(draw.offset + index * 20);
    VertexOut o;
    o.position = float4(v.position.x / draw.size.x * 2 - 1, 1 - v.position.y / draw.size.y * 2, 0, 1);
    o.uv = v.uv;
    o.color = float4(v.color & 0xff, (v.color >> 8) & 0xff, (v.color >> 16) & 0xff, v.color >> 24) / 255;
    return o;
}

[shader("fragment")]
float4 trianglesFragment(VertexOut i) : SV_Target {
    Texture2D<float4> source = prGpuGet<Texture2D<float4>>(draw.texture);
    return i.color * source.Sample(prGpuGet<SamplerState>(draw.sampler), i.uv);
}
