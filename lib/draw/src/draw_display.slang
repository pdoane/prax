// The display shader (pr/draw_display.h): a frame of lib/draw's colors mapped to a display, over a whole target. Its
// arguments are the draw's uniforms (DisplayArguments), which name the frame's texture and the table's.

#include "pr/gpu.slang"

struct Arguments {
    float4 toGamut[3];
    float4 fromGamut[3];
    float4 toOutput[3];
    float limit;
    uint output;
    float pqWhite;
    uint frameTexture;
    uint tableTexture;
};
PR_GPU_UNIFORMS(Arguments, arguments);

#define frame (prGpuGet<Texture2D<float4>>(arguments.frameTexture))
#define table (prGpuGet<Texture2D<float4>>(arguments.tableTexture))

static const float kPi = 3.14159265358979323846;

// The encodings that DisplayOutput numbers.
static const uint kOutputEncoded = 0;
static const uint kOutputLinear = 1;

// The weights of red, green and blue in a color's luminance, as color.h gives them.
static const float3 kLuminance = float3(0.2125, 0.7154, 0.0721);

float3 times(float4 rows[3], float3 v) {
    return float3(dot(rows[0].xyz, v), dot(rows[1].xyz, v), dot(rows[2].xyz, v));
}

// sRGB's transfer function and its inverse, mirrored below zero (PrColorTransfer_Srgb).
float3 linearOf(float3 c) {
    float3 m = abs(c);
    return sign(c) * select(m <= 0.04045, m / 12.92, pow((m + 0.055) / 1.055, 2.4));
}

float3 encodedOf(float3 c) {
    float3 m = abs(c);
    return sign(c) * select(m <= 0.0031308, m * 12.92, 1.055 * pow(m, 1 / 2.4) - 0.055);
}

// Linear sRGB in OKLab, and back, by Ottosson's matrices for linear sRGB (css-color-4/#color-conversion-code).
float3 oklabOf(float3 rgb) {
    float3 lms = float3(dot(float3(0.4122214708, 0.5363325363, 0.0514459929), rgb),
                        dot(float3(0.2119034982, 0.6806995451, 0.1073969566), rgb),
                        dot(float3(0.0883024619, 0.2817188376, 0.6299787005), rgb));
    lms = sign(lms) * pow(abs(lms), (1.0 / 3).xxx);
    return float3(dot(float3(0.2104542553, 0.7936177850, -0.0040720468), lms),
                  dot(float3(1.9779984951, -2.4285922050, 0.4505937099), lms),
                  dot(float3(0.0259040371, 0.7827717662, -0.8086757660), lms));
}

float3 linearOfOklab(float3 lab) {
    float3 lms = float3(dot(float3(1, 0.3963377774, 0.2158037573), lab),
                        dot(float3(1, -0.1055613458, -0.0638541728), lab),
                        dot(float3(1, -0.0894841775, -1.2914855480), lab));
    lms = lms * lms * lms;
    return float3(dot(float3(4.0767416621, -3.3077115913, 0.2309699292), lms),
                  dot(float3(-1.2684380046, 2.6097574011, -0.3413193965), lms),
                  dot(float3(-0.0041960863, -0.7034186147, 1.7076147010), lms));
}

// The color in linear sRGB tone mapped for the headroom (prColorToneMap): past the knee, halfway from SDR white to the
// headroom, its luminance approaches the headroom without reaching it.
float3 toneMapped(float3 light) {
    float luminance = dot(kLuminance, light);
    float knee = (1 + arguments.limit) / 2;
    if (!(luminance > knee))
        return light;
    float mapped = arguments.limit;
    if (arguments.limit > knee) {
        float x = (luminance - knee) / (arguments.limit - knee);
        mapped = knee + (arguments.limit - knee) * x / (1 + x);
    }
    return light * (mapped / luminance);
}

// The most chroma that the gamut's table holds at an OKLCH lightness in [0, 1] and a hue in degrees, between its
// texels. A column of the table is a hue, and the hues wrap. A row is a lightness, from 0 to 1.
float tableChroma(float lightness, float hue) {
    uint hues;
    uint lightnesses;
    table.GetDimensions(hues, lightnesses);
    float x = hue / 360 * hues;
    float y = clamp(lightness, 0.0, 1.0) * (lightnesses - 1);
    float2 at = floor(float2(x, y));
    float2 t = float2(x, y) - at;
    uint x0 = uint(at.x) % hues;
    uint x1 = (x0 + 1) % hues;
    uint y0 = min(uint(at.y), lightnesses - 1);
    uint y1 = min(y0 + 1, lightnesses - 1);
    float low = lerp(table.Load(int3(x0, y0, 0)).r, table.Load(int3(x1, y0, 0)).r, t.x);
    float high = lerp(table.Load(int3(x0, y1, 0)).r, table.Load(int3(x1, y1, 0)).r, t.x);
    return lerp(low, high, t.y);
}

// The color in linear sRGB mapped into the display's gamut, whose white is the headroom: a color past it keeps its
// OKLCH lightness and hue and takes the most chroma that the gamut holds there, and is then clipped to the gamut. That
// lies within a just noticeable difference of CSS Color 4's mapping (prColorGamutMap). OKLab scales by the cube root of
// linear light, so the table of SDR white serves every headroom.
float3 gamutMapped(float3 light) {
    const float kTolerance = 1e-4;
    float3 inGamut = times(arguments.toGamut, light);
    if (all(inGamut >= -kTolerance) && all(inGamut <= arguments.limit + kTolerance))
        return light;
    float3 start = oklabOf(light);
    float scale = pow(arguments.limit, 1.0 / 3);
    if (start.x >= scale)
        return times(arguments.fromGamut, arguments.limit.xxx);
    if (start.x <= 0)
        return (float3)0;
    float chroma = length(start.yz);
    float hue = atan2(start.z, start.y) * (180 / kPi);
    hue = hue < 0 ? hue + 360 : hue;
    float bound = scale * tableChroma(start.x / scale, hue);
    float3 mapped = linearOfOklab(float3(start.x, start.yz * (min(chroma, bound) / chroma)));
    return times(arguments.fromGamut, clamp(times(arguments.toGamut, mapped), 0.0, arguments.limit));
}

// SMPTE ST 2084's inverse EOTF, which encodes luminance where 1 is 10,000 cd/m². Luminance below zero is black.
float3 pqOf(float3 luminance) {
    const float m1 = 0.1593017578125;
    const float m2 = 78.84375;
    const float c1 = 0.8359375;
    const float c2 = 18.8515625;
    const float c3 = 18.6875;
    float3 p = pow(saturate(luminance), m1);
    return pow((c1 + c2 * p) / (1 + c3 * p), m2);
}

[shader("vertex")]
float4 wholeVertex(uint id : SV_VertexID) : SV_Position {
    float2 p = float2((id << 1) & 2, id & 2);
    return float4(p * 2 - 1, 0, 1);
}

[shader("fragment")]
float4 convertPixel(float4 position : SV_Position) : SV_Target {
    float3 light = gamutMapped(toneMapped(linearOf(frame.Load(int3(int2(position.xy), 0)).rgb)));
    float3 c = arguments.output == kOutputEncoded  ? encodedOf(light)
               : arguments.output == kOutputLinear ? light
                                                   : pqOf(times(arguments.toOutput, light) * arguments.pqWhite);
    return float4(c, 1);
}
