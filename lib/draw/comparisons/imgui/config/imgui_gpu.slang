// Dear ImGui's shaders on lib/gpu: the vertex color times the straight color of the texture's sample. There is one
// fragment shader for each alpha that a texture's texels have (ImGuiGpuAlpha). A vertex shader reads its vertex from
// the draw data's vertices, which Buffer slot 0 binds. Texture slot 0 binds the draw's texture and sampler.

#include "pr/gpu.slang"

// The push data of each draw: the scale and the translation from the display's coordinates to clip space, and the byte
// offset of the draw's first vertex.
struct Draw {
    float2 scale;
    float2 translate;
    uint offset;
    uint pad;
};
PR_GPU_PUSH(Draw, draw);

PR_GPU_BUFFER(ByteAddressBuffer, vertices, 0);
PR_GPU_TEXTURE(Texture2D<float4>, source, 0);

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
    Vertex v = vertices.Load<Vertex>(draw.offset + index * 20);
    VertexOut o;
    o.position = float4(v.position * draw.scale + draw.translate, 0, 1);
    o.uv = v.uv;
    o.color = float4(v.color & 0xff, (v.color >> 8) & 0xff, (v.color >> 16) & 0xff, v.color >> 24) / 255;
    return o;
}

// The fragment shader for a texture of straight texels, such as Dear ImGui's own textures.
[shader("fragment")]
float4 imguiFragment(VertexOut i) : SV_Target {
    return i.color * source.Sample(sourceSampler, i.uv);
}

// The fragment shader for a texture of premultiplied texels, which divides the sample's color by its alpha.
[shader("fragment")]
float4 imguiFragmentPremultiplied(VertexOut i) : SV_Target {
    float4 texel = source.Sample(sourceSampler, i.uv);
    if (texel.a > 0)
        texel.rgb /= texel.a;
    return i.color * texel;
}

// The fragment shader for a texture without alpha, which reads the sample's alpha as one whatever the texture stores.
[shader("fragment")]
float4 imguiFragmentOpaque(VertexOut i) : SV_Target {
    return i.color * float4(source.Sample(sourceSampler, i.uv).rgb, 1);
}
