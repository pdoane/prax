// The triangles renderer's shaders, as draw_triangles_frame.h specifies them: the vertex color times the texture's
// sample. The pipeline's vertex layout reads the frame's vertices. The fragment shader samples the draw's texture,
// which Texture slot 0 holds with its sampler.

#include "pr/gpu.slang"

// The push data of each draw: the target's extent in pixels.
struct Draw {
    float2 size;
};
PR_GPU_PUSH(Draw, draw);

PR_GPU_TEXTURE(Texture2D<float4>, drawTexture, 0);

// A vertex, as TrianglesVertex lays it out. The layout reads the straight RGBA color as normalized floats.
struct Vertex {
    PR_GPU_ATTRIBUTE(float2, position, 0);
    PR_GPU_ATTRIBUTE(float2, uv, 1);
    PR_GPU_ATTRIBUTE(float4, color, 2);
};

struct VertexOut {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
};

[shader("vertex")]
VertexOut trianglesVertex(Vertex v) {
    VertexOut o;
    o.position = float4(v.position.x / draw.size.x * 2 - 1, 1 - v.position.y / draw.size.y * 2, 0, 1);
    o.uv = v.uv;
    o.color = v.color;
    return o;
}

[shader("fragment")]
float4 trianglesFragment(VertexOut i) : SV_Target {
    return i.color * drawTexture.Sample(drawTextureSampler, i.uv);
}
