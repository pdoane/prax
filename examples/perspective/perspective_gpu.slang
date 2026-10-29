// The perspective example's background, and the copy of the scene texture, each over a whole target. The background's
// arguments are the draw's uniforms (BackgroundArguments), and the copy's push data names the scene texture.

#include "pr/gpu.slang"

struct Background {
    float4 palette[4];
    float4 top;
    float4 bottom;
    float2 origin;
    float2 drift;
    float pixelRatio;
    float time;
    float height;
    float cellSize;
    float gap;
};
PR_GPU_UNIFORMS(Background, background);

struct Copy {
    uint scene;
};
PR_GPU_PUSH(Copy, copy);

[shader("vertex")]
float4 wholeVertex(uint id : SV_VertexID) : SV_Position {
    float2 p = float2((id << 1) & 2, id & 2);
    return float4(p * 2 - 1, 0, 1);
}

[shader("fragment")]
float4 backgroundPixel(float4 position : SV_Position) : SV_Target {
    float2 p = (floor(position.xy) + 0.5 + background.origin) / background.pixelRatio;
    float3 color = lerp(background.top.rgb, background.bottom.rgb, saturate(p.y / background.height));
    float2 q = p + background.drift * background.time;
    float2 cell = floor(q / background.cellSize);
    float2 f = q - cell * background.cellSize;
    if (all(f >= background.gap * 0.5) && all(f < background.cellSize - background.gap * 0.5)) {
        int column = int(cell.x);
        int row = int(cell.y);
        int k = ((column * 3 + row * 5) % 7 + 7) % 7;
        if (k < 4) {
            float a = 0.55 + 0.35 * sin(background.time * 1.3 + cell.x * 0.7 + cell.y * 1.1);
            color = lerp(color, background.palette[k].rgb, a);
        }
    }
    return float4(color, 1);
}

[shader("fragment")]
float4 copyPixel(float4 position : SV_Position) : SV_Target {
    return prGpuGet<Texture2D<float4>>(copy.scene).Load(int3(int2(position.xy), 0));
}
