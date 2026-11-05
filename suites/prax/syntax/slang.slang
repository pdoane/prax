// A material interface and two implementations, specialized by a generic shading entry point.

import common;

interface IMaterial
{
    float3 shade(float3 normal, float3 light);
}

struct Lambert : IMaterial
{
    float3 albedo;

    float3 shade(float3 normal, float3 light)
    {
        return albedo * max(dot(normal, light), 0.0);
    }
}

struct Toon : IMaterial
{
    float3 albedo;
    int bands = 3;

    float3 shade(float3 normal, float3 light)
    {
        float lit = max(dot(normal, light), 0.0);
        return albedo * floor(lit * bands) / float(bands);
    }
}

struct Vertex
{
    float3 position : POSITION;
    float3 normal : NORMAL;
};

ParameterBlock<Lambert> material;
uniform float4x4 viewProjection;
static const float3 kLight = normalize(float3(0.3, 1.0, 0.5));

[shader("vertex")]
float4 vertexMain(Vertex v, out float3 normal : NORMAL) : SV_Position
{
    normal = v.normal;
    return mul(viewProjection, float4(v.position, 1.0));
}

[shader("fragment")]
float4 fragmentMain<M : IMaterial>(float3 normal : NORMAL, M surface) : SV_Target
{
    return float4(surface.shade(normalize(normal), kLight), 1.0);
}
