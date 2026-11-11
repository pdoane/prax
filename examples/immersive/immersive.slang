// The immersive example's shaders: a cube and a floor, drawn for each view of the frame.

#include "pr/gpu.slang"

struct Uniforms {
    float4x4 viewProjection[4];     // by view, from the world to clip space
    float4x4 model;                 // the cube's, from its own space to the world
    float4 floor;                   // the floor's height in the world in x
};
PR_GPU_UNIFORMS(Uniforms, uniforms);

// The draw's first view, for a pass of one view that draws one of several views.
struct Push {
    uint firstView;
};
PR_GPU_PUSH(Push, push);

struct VertexOut {
    float4 position : SV_Position;
    float3 world : WORLD;
    float3 color : COLOR;
    PR_GPU_VIEW_OUTPUT
};

// The corners of a face's two triangles, in the face's own u and v.
static const float2 kCorners[6] = {
    float2(-1, -1), float2(1, -1), float2(-1, 1), float2(-1, 1), float2(1, -1), float2(1, 1),
};

// The cube's faces, by face: +x, -x, +y, -y, +z and -z.
static const float3 kFaceColors[6] = {
    float3(0.95, 0.35, 0.3), float3(0.3, 0.75, 0.95), float3(0.95, 0.85, 0.3),
    float3(0.45, 0.9, 0.45), float3(0.8, 0.45, 0.95), float3(0.95, 0.6, 0.3),
};

VertexOut place(float3 world, float3 color, uint view) {
    VertexOut out;
    out.position = mul(uniforms.viewProjection[view], float4(world, 1));
    out.world = world;
    out.color = color;
    prGpuViewOutput(out);
    return out;
}

// A cube of side two in its own space, six faces of two triangles each.
[shader("vertex")]
VertexOut cubeVertex(uint vertex : SV_VertexID PR_GPU_VIEW_PARAM) {
    uint face = vertex / 6;
    float2 corner = kCorners[vertex % 6];
    uint axis = face / 2;
    float side = (face & 1) != 0 ? -1 : 1;
    float3 local;
    local[axis] = side;
    local[(axis + 1) % 3] = corner.x;
    local[(axis + 2) % 3] = corner.y;
    float3 world = mul(uniforms.model, float4(local, 1)).xyz;
    return place(world, kFaceColors[face], push.firstView + prGpuView());
}

// The floor: a square 20 meters across, at the floor's height.
[shader("vertex")]
VertexOut floorVertex(uint vertex : SV_VertexID PR_GPU_VIEW_PARAM) {
    float2 corner = kCorners[vertex] * 10;
    return place(float3(corner.x, uniforms.floor.x, corner.y), float3(0, 0, 0), push.firstView + prGpuView());
}

// The cube's faces, each in its color.
[shader("fragment")]
float4 cubeFragment(VertexOut input) : SV_Target {
    return float4(input.color, 1);
}

// Tiles half a meter across, with lines between them, fading into the distance.
[shader("fragment")]
float4 floorFragment(VertexOut input) : SV_Target {
    float2 tile = input.world.xz * 2;
    float2 cell = abs(frac(tile) - 0.5);
    float line = smoothstep(0.47, 0.5, max(cell.x, cell.y));
    float checker = (int(floor(tile.x)) + int(floor(tile.y))) & 1;
    float fade = saturate(1 - length(input.world.xz) / 10);
    float3 color = lerp(float3(0.1, 0.11, 0.14), float3(0.14, 0.15, 0.19), checker);
    color = lerp(color, float3(0.4, 0.45, 0.55), line);
    return float4(lerp(float3(0.03, 0.035, 0.05), color, fade), 1);
}
