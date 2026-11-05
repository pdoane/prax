// Tone maps an HDR image with a compute shader, in 8x8 groups.

#define GROUP_SIZE 8

cbuffer Exposure : register(b0)
{
    float exposure;
    float whitePoint;
    uint2 size;
};

Texture2D<float4> hdr : register(t0);
RWTexture2D<float4> output : register(u0);

// Reinhard's operator, extended to a white point.
float3 reinhard(float3 color, float white)
{
    float3 numerator = color * (1.0f + color / (white * white));
    return numerator / (1.0f + color);
}

float luminance(float3 c)
{
    return dot(c, float3(0.2126f, 0.7152f, 0.0722f));
}

[numthreads(GROUP_SIZE, GROUP_SIZE, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= size))
        return;
    float4 color = hdr[id.xy] * exp2(exposure);
    float3 mapped = reinhard(color.rgb, whitePoint);
    [unroll]
    for (int i = 0; i < 3; ++i)
        mapped[i] = pow(saturate(mapped[i]), 1.0f / 2.2f);
    output[id.xy] = float4(mapped, luminance(mapped));
}
