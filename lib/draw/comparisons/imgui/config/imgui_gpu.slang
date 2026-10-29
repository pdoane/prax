// Dear ImGui's shaders on lib/gpu: the vertex color times the texture's sample. A vertex shader reads its vertex from
// the draw data's vertices, which are in a buffer that the push data names.

#include "pr/gpu.slang"

// The push data of each draw: the scale and the translation from the display's coordinates to clip space, the buffer of
// the vertices and the byte offset of the draw's first, and the draw's texture and sampler.
struct Draw {
    float2 scale;
    float2 translate;
    uint vertices;
    uint offset;
    uint texture;
    uint sampler;
};
PR_GPU_PUSH(Draw, draw);

// An ImDrawVert: position, uv, and straight RGBA with red in the lowest byte.
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
VertexOut imguiVertex(uint index : SV_VertexID) {
    Vertex v = prGpuGet<ByteAddressBuffer>(draw.vertices).Load<Vertex>(draw.offset + index * 20);
    VertexOut o;
    o.position = float4(v.position * draw.scale + draw.translate, 0, 1);
    o.uv = v.uv;
    o.color = float4(v.color & 0xff, (v.color >> 8) & 0xff, (v.color >> 16) & 0xff, v.color >> 24) / 255;
    return o;
}

[shader("fragment")]
float4 imguiFragment(VertexOut i) : SV_Target {
    Texture2D<float4> source = prGpuGet<Texture2D<float4>>(draw.texture);
    return i.color * source.Sample(prGpuGet<SamplerState>(draw.sampler), i.uv);
}
