// The fragment renderer's shaders: coverage, clips and paint as draw_fragment_frame.h defines them, and the filter
// steps. They use the names of the frame's enumerations, which the generated prelude defines from the C headers, and
// the records of draw_fragment_records.h, which the C++ side shares.
//
// The frame's data is one buffer that the uniforms name, and each array is read from its byte offset there, as typed
// records. Half floats are read as pairs in a uint. Textures are read through the frame's table of handles, which
// instances and records index, and samplers through the handles in the uniforms.

#include "pr/gpu.slang"

#include "draw/backends/fragment/src/draw_fragment_constants.h"
#include "draw/backends/fragment/src/draw_fragment_records.h"

// The weights of the channels in a color's luminance.
static const float3 kLuminance = float3(kFragmentLuminanceRed, kFragmentLuminanceGreen, kFragmentLuminanceBlue);

static const float kSqrtHalf = 0.70710678118654752;
static const float kPi = 3.14159265358979323846;

// A piece of a gradient's ramp (PrDrawRampPiece).
struct RampPiece {
    float start;
    float end;
    uint first;
    uint count;
};

// The push data of each draw (FragmentPush): the pass's target, its top left in the target's pixels and its size; the
// offset from a pixel of the pass's attachment to the target's pixel; the filter step; and the draw's first instance.
struct Draw {
    float4 into;
    float2 origin;
    uint filterIndex;
    uint firstInstance;
};
PR_GPU_PUSH(Draw, draw);

// The uniforms of a frame's draws (FragmentUniforms): each array's byte offset in the buffer that holds the frame's
// data, the samplers, and that buffer. Root data's arrays are of 4-vectors, so each element holds four words.
struct Frame {
    uint4 parts[5];
    uint4 samplers[8];
    uint data;
};
PR_GPU_UNIFORMS(Frame, frame);

// Returns the byte offset of a FragmentPart's array in the frame's data.
uint framePart(uint part) {
    return frame.parts[part >> 2][part & 3];
}

// An array of the frame's data, read by index.
struct Part<T> {
    uint offset;

    __subscript(uint i)->T {
        get { return prGpuLoad<T>(frame.data, offset + i * sizeof(T)); }
    }
};

// A run of the frame's data, read by byte offset.
struct Bytes {
    uint offset;

    uint Load(uint at) {
        return prGpuLoad<uint>(frame.data, offset + at);
    }

    T Load<T>(uint at) {
        return prGpuLoad<T>(frame.data, offset + at);
    }
};

// The frame's arrays, in the order of FragmentPart.
#define frameInstances (Part<FragmentInstance>(framePart(0)))
#define frameClips (Part<FragmentClip>(framePart(1)))
#define framePaints (Part<FragmentPaint>(framePart(2)))
#define frameClipShapes (Part<uint>(framePart(3)))
#define frameShapes (Part<FragmentClipShape>(framePart(4)))
#define frameValues (Bytes(framePart(5)))                  // the records that coverage reads, at float offsets
#define frameRampColors (Part<uint2>(framePart(6)))        // half4
#define frameCoverageBytes (Bytes(framePart(7)))
#define frameTiles (Part<float>(framePart(8)))
#define framePieces (Part<float4>(framePart(9)))
#define frameMaps (Part<FragmentMap>(framePart(10)))
#define frameMapData (Part<float>(framePart(11)))
#define frameMapIndices (Part<uint>(framePart(12)))
#define frameTable (Part<uint>(framePart(13)))             // the textures' handles
#define frameEntries (Part<FragmentTexture>(framePart(14)))
#define frameFilters (Part<FragmentFilter>(framePart(15)))
#define frameFilterData (Part<float>(framePart(16)))
#define frameRampPieces (Part<RampPiece>(framePart(17)))
#define frameRampTables (Part<uint2>(framePart(18)))       // half4
#define frameRampCells (Part<uint>(framePart(19)))

// The frame's texture at a zero-based index of its table.
Texture2D<float4> textureAt(uint index) {
    return prGpuGet<Texture2D<float4>>(frameTable[index]);
}

float4 halfColor(uint2 h) {
    return float4(f16tof32(h.x), f16tof32(h.x >> 16), f16tof32(h.y), f16tof32(h.y >> 16));
}

// The magnitude of `m` with the sign of `s`.
float copysignOf(float m, float s) {
    return asfloat((asuint(m) & 0x7fffffffu) | (asuint(s) & 0x80000000u));
}

float3 copysignOf(float3 m, float3 s) {
    return asfloat((asuint(m) & 0x7fffffffu) | (asuint(s) & 0x80000000u));
}

struct VertexOut {
    float4 position : SV_Position;
    float2 local : LOCAL;               // the pixel's point in the instance's local space
    float coverage : COVERAGE;          // Triangle: interpolated from its corners
    nointerpolation uint instance : INSTANCE;
    nointerpolation float4 clipRect : CLIP;
};

// The pass's target: its top left in the target's pixels, and its size.
float4 clipPosition(float2 position, float4 target) {
    float2 p = position - target.xy;
    return float4(p.x / target.z * 2 - 1, 1 - p.y / target.w * 2, 0, 1);
}

// An instance's corners as a strip: a quad's four, or a triangle's three with the last repeated. Every draw starts from
// instance zero, and passes its first instance in its push data.
[shader("vertex")]
VertexOut instanceVertex(uint corner : SV_VertexID, uint drawn : SV_InstanceID) {
    uint index = draw.firstInstance + drawn;
    FragmentInstance i = frameInstances[index];
    VertexOut result;
    float2 at;
    if ((i.flags & FragmentInstanceFlag_Coverage) == FragmentCoverage_Triangle) {
        uint c = min(corner, 2u);
        at = i.transform[c];
        result.local = at;
        result.coverage = i.bounds[c];
    } else {
        float2 local = float2((corner & 1) != 0 ? i.bounds.z : i.bounds.x, (corner & 2) != 0 ? i.bounds.w : i.bounds.y);
        at = i.transform[0] * local.x + i.transform[1] * local.y + i.transform[2];
        result.local = local;
        result.coverage = 1;
    }
    result.position = clipPosition(at, draw.into);
    result.instance = index;
    result.clipRect = i.clip != 0 ? frameClips[i.clip - 1].rect : float4(-1e30, -1e30, 1e30, 1e30);
    return result;
}

// Maps the point through the projective matrix.
float2 mapPoint(FragmentMatrix3 m, float2 p) {
    float3 h = float3(p, 1);
    float3 x = float3(m.m[0][0], m.m[0][1], m.m[0][2]);
    float3 y = float3(m.m[1][0], m.m[1][1], m.m[1][2]);
    float3 w = float3(m.m[2][0], m.m[2][1], m.m[2][2]);
    return float2(dot(x, h), dot(y, h)) / dot(w, h);
}

// The norm of a point of the first quadrant with exponent `n`, and its gradient: the max norm for an infinite `n`. It
// divides by the larger coordinate first, so that no power overflows.
float3 cornerNorm(float2 u, float n) {
    float m = max(u.x, u.y);
    if (!(m > 0))
        return float3(0, kSqrtHalf, kSqrtHalf);
    if (isinf(n))
        return u.x >= u.y ? float3(m, 1, 0) : float3(m, 0, 1);
    if (n == 1)
        return float3(u.x + u.y, 1, 1);
    float2 s = u / m;
    float g = m * pow(pow(s.x, n) + pow(s.y, n), 1 / n);
    return float3(g, pow(u.x / g, n - 1), pow(u.y / g, n - 1));
}

// The signed distance from the shape's edge, negative inside, in the shape's space, and the direction in which it
// grows. Within a corner it is the norm's value less one, over its gradient.
float3 shapedDistance(FragmentCornerRect s, float2 p) {
    float4 rect = s.rect;
    float2 lo = rect.xy;
    float2 hi = rect.zw;
    float2 q = p - (lo + hi) * 0.5;
    float2 side = float2(q.x < 0 ? -1.0 : 1.0, q.y < 0 ? -1.0 : 1.0);
    float2 halfSize = (hi - lo) * 0.5;
    uint k = q.x < 0 ? (q.y < 0 ? 0 : 3) : (q.y < 0 ? 1 : 2);
    float2 r = min(s.radii[k], halfSize);
    float2 a = abs(q);
    float shape = s.shapes[k];
    if (r.x > 0 && r.y > 0) {
        float2 u = (a - (halfSize - r)) / r;
        if (u.x > 0 && u.y > 0) {
            bool concave = shape < 0;
            float3 norm = cornerNorm(concave ? max(1 - u, 0.0) : u, exp2(abs(shape)));
            float2 gradient = norm.yz / r;
            float magnitude = max(length(gradient), 1e-6);
            float value = concave ? 1 - norm.x : norm.x - 1;
            return float3(value / magnitude, gradient / magnitude * side);
        }
    }
    float2 d = a - halfSize;
    if (d.x > 0 && d.y > 0)
        return float3(length(d), normalize(d) * side);
    return d.x > d.y ? float3(max(d.x, 0.0) + min(d.x, 0.0), side.x, 0) : float3(d.y, 0, side.y);
}

// The fraction of the pixel inside the shape at the point of its space. How fast the distance changes across pixels is
// its direction through the point's own rate of change, which has no kinks.
float shapedCoverage(FragmentCornerRect s, float2 p) {
    float3 d = shapedDistance(s, p);
    float2 across = float2(dot(d.yz, prGpuDdxFine(p)), dot(d.yz, prGpuDdyFine(p)));
    float g = length(across);
    return g > 0 ? clamp(0.5 - d.x / g, 0.0, 1.0) : (d.x < 0 ? 1.0 : 0.0);
}

// The fraction of the pixel inside a clip's shape, whose transform maps the pixel into its space.
float shapeCoverage(FragmentClipShape s, float2 pixel) {
    float c = shapedCoverage(s.shape, mapPoint(s.transform, pixel));
    return s.invert != 0 ? 1 - c : c;
}

// The fraction of the pixel on the inner side of the half plane a x + b y + c < 0, all of it for a plane with a and b
// zero.
float planeCoverage(float3 plane, float2 p) {
    float2 n = plane.xy;
    if (n.x == 0 && n.y == 0)
        return 1;
    float d = dot(n, p) + plane.z;
    float g = length(float2(dot(n, prGpuDdxFine(p)), dot(n, prGpuDdyFine(p))));
    return g > 0 ? clamp(0.5 - d / g, 0.0, 1.0) : (d < 0 ? 1.0 : 0.0);
}

// The area of the pixel whose top left is `corner` inside the rectangle.
float rectArea(float4 r, float2 corner) {
    float2 lo = max(corner, r.xy);
    float2 hi = min(corner + 1, r.zw);
    float2 overlap = clamp(hi - lo, 0.0, 1.0);
    return overlap.x * overlap.y;
}

float gaussian(float x, float sigma) {
    return exp(-(x * x) / (2 * sigma * sigma)) / (sqrt(2 * kPi) * sigma);
}

float2 erf2(float2 x) {
    float2 s = sign(x);
    float2 a = abs(x);
    float2 t = 1 + (0.278393 + (0.230389 + 0.078108 * (a * a)) * a) * a;
    t *= t;
    return s - s / (t * t);
}

// The blurred coverage along x of one row of a rounded rectangle, `y` from its middle.
float shadowRow(float x, float y, float sigma, float corner, float2 halfSize) {
    float delta = min(halfSize.y - corner - abs(y), 0.0);
    float curved = halfSize.x - corner + sqrt(max(0.0, corner * corner - delta * delta));
    float2 integral = 0.5 + 0.5 * erf2((x + float2(-curved, curved)) * (sqrt(0.5) / sigma));
    return integral.y - integral.x;
}

// The rounded rectangle's coverage blurred by a Gaussian: exact along x, and sampled at four rows along y.
float blurredCoverage(FragmentCornerRect s, float2 p, float sigma) {
    float4 rect = s.rect;
    float2 halfSize = (rect.zw - rect.xy) * 0.5;
    p -= (rect.xy + rect.zw) * 0.5;
    uint k = p.x < 0 ? (p.y < 0 ? 0 : 3) : (p.y < 0 ? 1 : 2);
    float corner = min((s.radii[k].x + s.radii[k].y) * 0.5, min(halfSize.x, halfSize.y));
    float low = p.y - halfSize.y;
    float high = p.y + halfSize.y;
    float start = clamp(-3 * sigma, low, high);
    float end = clamp(3 * sigma, low, high);
    float stride = (end - start) / 4;
    float y = start + stride * 0.5;
    float value = 0;
    for (int i = 0; i < 4; ++i) {
        value += shadowRow(p.x, p.y - y, sigma, corner, halfSize) * gaussian(y, sigma) * stride;
        y += stride;
    }
    return clamp(value, 0.0, 1.0);
}

// A Shadow instance's coverage at the point of its space: its shape blurred, or the outside of it within its opening,
// less its caster.
float shadowCoverage(FragmentShadow s, float2 p) {
    bool inner = s.inner != 0;
    float value = s.sigma > 0 ? blurredCoverage(s.shape, p, s.sigma) : shapedCoverage(s.shape, p);
    if (inner)
        value = 1 - value;
    float cut = shapedCoverage(s.caster, p);
    return value * (inner ? cut : 1 - cut);
}

// A gradient's parameter at a point of brush space, before it extends. Returns false where it paints nothing.
bool gradientParameter(FragmentPaint r, float2 b, inout float t) {
    float g[6] = r.gradient;
    switch (r.kind) {
    case FragmentPaintKind_Linear: {
        float2 d = float2(g[2] - g[0], g[3] - g[1]);
        float length2 = dot(d, d);
        if (!(length2 > 0))
            return false;
        t = dot(b - float2(g[0], g[1]), d) / length2;
        return true;
    }
    case FragmentPaintKind_Radial: {
        // The largest t whose circle, of nonnegative radius, passes through the point.
        float2 focal = float2(g[3], g[4]);
        float r0 = g[5];
        float2 cd = float2(g[0], g[1]) - focal;
        float dr = g[2] - r0;
        float2 pd = b - focal;
        float a = dot(cd, cd) - dr * dr;
        float bb = dot(pd, cd) + r0 * dr;
        float c = dot(pd, pd) - r0 * r0;
        if (dot(cd, cd) == 0 && dr == 0)
            return false;
        if (abs(a) < 1e-6 * max(dot(cd, cd), dr * dr)) {
            if (bb == 0)
                return false;
            float root = c / (2 * bb);
            if (!(r0 + root * dr >= 0))
                return false;
            t = root;
            return true;
        }
        float discriminant = bb * bb - a * c;
        if (discriminant < 0)
            return false;
        float root = sqrt(discriminant);
        float t1 = (bb + root) / a;
        float t2 = (bb - root) / a;
        if (r0 + max(t1, t2) * dr >= 0) {
            t = max(t1, t2);
            return true;
        }
        if (r0 + min(t1, t2) * dr >= 0) {
            t = min(t1, t2);
            return true;
        }
        return false;
    }
    case FragmentPaintKind_Conic: {
        float2 d = b - float2(g[0], g[1]);
        float turn = (atan2(d.y, d.x) - g[2]) / (2 * kPi);
        t = turn - floor(turn);
        return true;
    }
    default: {
        // Diamond.
        if (!(g[2] > 0 && g[3] > 0))
            return false;
        float2 d = abs(b - float2(g[0], g[1]));
        t = d.x / g[2] + d.y / g[3];
        return true;
    }
    }
}

// Maps `t` as the mode extends the gradient (prDrawGradientExtend). Returns false past the range of None, where it
// paints nothing.
bool extended(uint mode, inout float t) {
    if (mode == PrDrawExtend_Reflect) {
        float m = t - 2 * floor(t / 2);
        t = m > 1 ? 2 - m : m;
    } else if (mode == PrDrawExtend_Repeat) {
        t = t - floor(t);
    } else if (mode == PrDrawExtend_None) {
        return t >= 0 && t <= 1;
    } else {
        // Pad keeps which end a point lies past (prDrawGradientExtend).
        t = isnan(t) ? 0.0 : clamp(t, -1.0, 2.0);
    }
    return true;
}

// Returns the color at `t`, in [-1, 2], of the ramp `ramp`. The color interpolates between the table's steps, except in
// a step where pieces meet and past the ends, where it interpolates within the piece that holds `t`, and never across a
// piece's edge, as prDrawRampPieceAt and prDrawRampColor do.
float4 rampColor(uint ramp, float t) {
    const uint steps = kFragmentRampResolution;
    uint cells = ramp * (steps + 2);
    float at = t * float(steps);
    uint stepAt = min(uint(max(at, 0.0)), steps - 1);
    uint marked = t < 0 ? frameRampCells[cells + steps]
                : t > 1 ? frameRampCells[cells + steps + 1]
                : frameRampCells[cells + stepAt];
    if (marked == 0) {
        uint table = ramp * (steps + 1);
        return lerp(halfColor(frameRampTables[table + stepAt]), halfColor(frameRampTables[table + stepAt + 1]),
                    at - float(stepAt));
    }
    uint k = marked - 1;
    RampPiece p = frameRampPieces[k];
    while (t > p.end || (t == p.end && t < 1))
        p = frameRampPieces[++k];
    if (p.count == 1)
        return halfColor(frameRampColors[p.first]);
    float x = (t - p.start) / (p.end - p.start) * float(p.count - 1);
    float i = clamp(floor(x), 0.0, float(p.count - 2));
    float f = clamp(x - i, 0.0, 1.0);
    return lerp(halfColor(frameRampColors[p.first + uint(i)]), halfColor(frameRampColors[p.first + uint(i) + 1]), f);
}

// What an image's taps read: its texels in the first level, left, top, right and bottom, by the tile modes and with the
// period of their tiles.
struct ImageSource {
    float4 texels;
    float2 period;
    uint tileX;
    uint tileY;
    uint sampling;
    uint alpha;                 // PrDrawAlpha of the texels
};

// Returns a filtered color, premultiplied as the source's alpha says.
float4 premultiplied(float4 c, uint alpha) {
    if (alpha == PrDrawAlpha_Opaque)
        c.a = 1;
    else if (alpha == PrDrawAlpha_Straight)
        c.rgb *= c.a;
    return c;
}

// Catmull-Rom weights for the four taps around a fraction.
float4 cubicWeights(float t) {
    float t2 = t * t;
    float t3 = t2 * t;
    return float4(-t3 + 2 * t2 - t, 3 * t3 - 5 * t2 + 2, -3 * t3 + 4 * t2 + t, t3 - t2) / 2;
}

// ---------------------------------------------------------------------------------------------------------------------
// Images through the hardware's samplers (FragmentImageRead_Sampler)

// The address modes of the samplers, along one axis.
static const uint kAddressClamp = 0;
static const uint kAddressRepeat = 1;
static const uint kAddressMirror = 2;
static const uint kAddressZero = 3;

// Returns the address mode along an axis: the tile mode's if the source's texels span the texture whole along it, and
// transparent past the texture's edge otherwise. `spacing` is the gap between tiles, in texels.
uint addressOf(uint mode, float spacing, bool whole) {
    if (!whole || mode == PrDrawExtend_None)
        return kAddressZero;
    if (mode == PrDrawExtend_Pad)
        return kAddressClamp;
    if (spacing > 0)
        return kAddressZero;
    return mode == PrDrawExtend_Repeat ? kAddressRepeat : kAddressMirror;
}

// The driver's samplers, for each pair of address modes along x and y at x * 4 + y: bilinear samplers that read the
// first level, then trilinear and anisotropic ones that read along a footprint.
static const uint kAnisotropicSamplers = 16;

// Samples the frame's texture `image` bilinearly at `uv`, in normalized coordinates, with the address modes along each
// axis. A sample with `gradients` is trilinear and anisotropic along the footprint that `dx` and `dy` give, and any
// other sample reads the first level.
float4 hardwareSample(uint image, uint addressX, uint addressY, float2 uv, bool gradients, float2 dx, float2 dy) {
    uint index = addressX * 4 + addressY + (gradients ? kAnisotropicSamplers : 0);
    SamplerState s = prGpuGet<SamplerState>(frame.samplers[index >> 2][index & 3]);
    Texture2D<float4> t = textureAt(image);
    if (gradients)
        return t.SampleGrad(s, uv, dx, dy);
    return t.SampleLevel(s, uv, 0);
}

// Places a point `k` texels from the source's origin along one axis by the tile mode, and returns it in the texture's
// texels. Clears `lands` if the point lands on no texel. Only Nearest sampling tests it: a filter fades into the
// transparent texels that the address mode or the gutter gives.
float placeAxis(float k, float start, float size, float period, uint mode, bool whole, inout bool lands) {
    if (mode == PrDrawExtend_None) {
        lands = lands && k >= 0 && k < size;
        return start + k;
    }
    if (mode == PrDrawExtend_Pad)
        return start + (whole ? k : clamp(k, 0.5, max(size - 0.5, 0.5)));
    // The gap between tiles is split between the tiles on either side of it, so that each tile fades into its own half.
    float gap = period - size;
    float tile = floor((k + gap / 2) / period);
    float r = k - tile * period;
    lands = lands && r >= 0 && r < size;
    if (mode == PrDrawExtend_Reflect && fmod(abs(tile), 2.0) == 1)
        r = size - r;
    return start + r;
}

// Samples an image at (u, v), in the first level's texels from the source's origin. `dx` and `dy` are the change in (u,
// v) to the pixel's right and lower neighbors, which a Mipmap sample reads if `footprint`.
float4 sampleImage(ImageSource source, uint image, float2 uv, float2 dx, float2 dy, bool footprint) {
    uint width;
    uint height;
    uint levels;
    textureAt(image).GetDimensions(0, width, height, levels);
    float2 size = float2(width, height);
    float4 t = source.texels;
    bool wholeX = t.x == 0 && t.z == size.x;
    bool wholeY = t.y == 0 && t.w == size.y;
    // A Nearest sample places the center of the source's texel that holds the point.
    if (source.sampling == PrDrawSampling_Nearest)
        uv = floor(uv) + 0.5;
    bool lands = true;
    float2 at = float2(placeAxis(uv.x, t.x, t.z - t.x, source.period.x, source.tileX, wholeX, lands),
                       placeAxis(uv.y, t.y, t.w - t.y, source.period.y, source.tileY, wholeY, lands));
    uint addressX = addressOf(source.tileX, source.period.x - (t.z - t.x), wholeX);
    uint addressY = addressOf(source.tileY, source.period.y - (t.w - t.y), wholeY);
    // The texel that a Nearest sample reads is the one that holds the placed point, clamped to the source's texels,
    // whose far edge a reflected point can reach.
    if (source.sampling == PrDrawSampling_Nearest) {
        if (!lands)
            return (float4)0;
        float2 last = min(ceil(t.zw) - 1, size - 1);
        uint2 texel = uint2(clamp(floor(at), max(floor(t.xy), (float2)0), last));
        return premultiplied(textureAt(image).Load(int3(texel, 0)), source.alpha);
    }
    if (source.sampling == PrDrawSampling_Cubic) {
        // Each axis's middle two taps are one bilinear sample between them, weighted by their sum, which is positive.
        float2 first = floor(at - 0.5);
        float2 f = at - 0.5 - first;
        float4 wx = cubicWeights(f.x);
        float4 wy = cubicWeights(f.y);
        float3 cx = float3(first.x - 0.5, first.x + 0.5 + wx[2] / (wx[1] + wx[2]), first.x + 2.5);
        float3 cy = float3(first.y - 0.5, first.y + 0.5 + wy[2] / (wy[1] + wy[2]), first.y + 2.5);
        float3 kx = float3(wx[0], wx[1] + wx[2], wx[3]);
        float3 ky = float3(wy[0], wy[1] + wy[2], wy[3]);
        float4 result = (float4)0;
        for (int j = 0; j < 3; ++j) {
            for (int i = 0; i < 3; ++i) {
                float4 tap = hardwareSample(image, addressX, addressY, float2(cx[i], cy[j]) / size, false, 0, 0);
                result += premultiplied(tap, source.alpha) * (kx[i] * ky[j]);
            }
        }
        result.a = clamp(result.a, 0.0, 1.0);
        result.rgb = clamp(result.rgb, 0.0, result.a);
        return result;
    }
    bool gradients = source.sampling == PrDrawSampling_Mipmap && footprint;
    return premultiplied(hardwareSample(image, addressX, addressY, at / size, gradients, dx / size, dy / size),
                         source.alpha);
}

// ---------------------------------------------------------------------------------------------------------------------
// Images by texel reads (FragmentImageRead_Texels)

// How an image's taps find texels along one axis of a level, in the level's texels.
struct TapAxis {
    float start;                // the source's first texel
    float size;                 // the source's texels
    float period;               // Repeat, Reflect: from one tile's start to the next's
    uint mode;                  // PrDrawExtend
    int levelSize;
};

// The texel index for a tap `k` texels from the source's origin, placed by the axis's tile mode, or -1 if it hits no
// texel.
int tapIndex(float k, TapAxis a) {
    float position = k + 0.5;
    if (a.mode == PrDrawExtend_None) {
        if (position < 0 || position >= a.size)
            return -1;
    } else if (a.mode == PrDrawExtend_Repeat || a.mode == PrDrawExtend_Reflect) {
        // `k` is from the first copy (nearCopy). A tap in a gap or past it reads nothing, so that spaced copies fade
        // into their gaps on this path and the samplers' path alike.
        float tile = floor(position / a.period);
        position -= tile * a.period;
        if (position >= a.size || (tile != 0 && a.period > a.size))
            return -1;
        if (a.mode == PrDrawExtend_Reflect && fmod(abs(tile), 2.0) == 1)
            position = a.size - position;
    } else {
        position = clamp(position, 0.5, max(a.size - 0.5, 0.5));
    }
    int index = int(floor(a.start + position));
    int first = int(floor(a.start));
    int last = int(ceil(a.start + a.size)) - 1;
    index = index < first ? first : index > last ? last : index;
    return index < 0 ? 0 : index >= a.levelSize ? a.levelSize - 1 : index;
}

// A level of an image's texture, and its taps' axes.
struct ImageLevel {
    uint image;                 // the frame's texture
    uint level;
    TapAxis x;
    TapAxis y;
    uint alpha;                 // PrDrawAlpha of the texels
};

// A texel, premultiplied, or transparent for a tap outside the texels.
float4 levelTexel(ImageLevel l, int x, int y) {
    if (x < 0 || y < 0)
        return (float4)0;
    return premultiplied(textureAt(l.image).Load(int3(x, y, l.level)), l.alpha);
}

// Samples a level by texel reads at (u, v), in the first level's texels from the source's origin.
float4 readLevel(ImageSource r, uint image, uint level, uint sampling, float2 uv) {
    uint width;
    uint height;
    uint levels;
    textureAt(image).GetDimensions(level, width, height, levels);
    int2 size = int2(width, height);
    float scale = 1.0 / float(1u << level);
    ImageLevel l;
    l.image = image;
    l.level = level;
    l.x.start = r.texels.x * scale;
    l.x.size = (r.texels.z - r.texels.x) * scale;
    l.x.period = r.period.x * scale;
    l.x.mode = r.tileX;
    l.x.levelSize = size.x;
    l.y.start = r.texels.y * scale;
    l.y.size = (r.texels.w - r.texels.y) * scale;
    l.y.period = r.period.y * scale;
    l.y.mode = r.tileY;
    l.y.levelSize = size.y;
    l.alpha = r.alpha;
    uv *= scale;
    if (sampling == PrDrawSampling_Nearest)
        return levelTexel(l, tapIndex(floor(uv.x), l.x), tapIndex(floor(uv.y), l.y));
    float2 f = uv - 0.5;
    float2 first = floor(f);
    float2 t = f - first;
    if (sampling == PrDrawSampling_Cubic) {
        float4 wx = cubicWeights(t.x);
        float4 wy = cubicWeights(t.y);
        int xs[4];
        int ys[4];
        for (int i = 0; i < 4; ++i) {
            xs[i] = tapIndex(first.x + float(i - 1), l.x);
            ys[i] = tapIndex(first.y + float(i - 1), l.y);
        }
        float4 result = (float4)0;
        for (int j = 0; j < 4; ++j) {
            for (int i = 0; i < 4; ++i)
                result += levelTexel(l, xs[i], ys[j]) * (wx[i] * wy[j]);
        }
        result.a = clamp(result.a, 0.0, 1.0);
        result.rgb = clamp(result.rgb, 0.0, result.a);
        return result;
    }
    int left = tapIndex(first.x, l.x);
    int right = tapIndex(first.x + 1, l.x);
    int top = tapIndex(first.y, l.y);
    int bottom = tapIndex(first.y + 1, l.y);
    float4 upper = lerp(levelTexel(l, left, top), levelTexel(l, right, top), t.x);
    float4 lower = lerp(levelTexel(l, left, bottom), levelTexel(l, right, bottom), t.x);
    return lerp(upper, lower, t.y);
}

// Reads an image by texel reads at (u, v), in the first level's texels from the source's origin, between the two levels
// that `lod` falls between.
float4 readTrilinear(ImageSource source, uint image, float2 uv, float lod, uint levels) {
    uint low = uint(lod);
    float t = lod - float(low);
    float4 result = readLevel(source, image, low, PrDrawSampling_Linear, uv);
    if (t > 0 && low + 1 < levels)
        result = lerp(result, readLevel(source, image, low + 1, PrDrawSampling_Linear, uv), t);
    return result;
}

// The most taps along the major axis of a pixel's footprint in anisotropic sampling.
static const uint kMaxAnisotropy = 16;

// Reads an image by texel reads at (u, v), in the first level's texels from the source's origin. `dx` and `dy` are the
// change in (u, v) to the pixel's right and lower neighbors. Mipmap sampling is anisotropic, as the cpu backend's is:
// the level comes from the footprint's minor axis, and up to kMaxAnisotropy taps spread along the major axis.
float4 readImage(ImageSource source, uint image, float2 uv, float2 dx, float2 dy) {
    uint width;
    uint height;
    uint levels;
    textureAt(image).GetDimensions(0, width, height, levels);
    float a = length(dx);
    float b = length(dy);
    float major = max(a, b);
    float minor = min(a, b);
    if (source.sampling != PrDrawSampling_Mipmap || levels < 2 || !(major > 1)) {
        uint sampling = source.sampling == PrDrawSampling_Mipmap ? PrDrawSampling_Linear : source.sampling;
        return readLevel(source, image, 0, sampling, uv);
    }
    float2 axis = a >= b ? dx : dy;
    float ratio = minor > 0 ? ceil(major / minor) : float(kMaxAnisotropy);
    uint taps = uint(clamp(ratio, 1.0, float(kMaxAnisotropy)));
    float lod = clamp(log2(max(major / float(taps), minor)), 0.0, float(levels - 1));
    float4 result = (float4)0;
    for (uint i = 0; i < taps; ++i)
        result += readTrilinear(source, image, uv + axis * ((float(i) + 0.5) / float(taps) - 0.5), lod, levels);
    return result / float(taps);
}

// ---------------------------------------------------------------------------------------------------------------------
// Image paints

// Samples an image at (u, v) as the record's `read` says. `dx` and `dy` are the change in (u, v) to the pixel's right
// and lower neighbors, which Mipmap sampling reads if `footprint`.
float4 imageColor(FragmentPaint r, ImageSource source, uint image, float2 uv, float2 dx, float2 dy, bool footprint) {
    if (r.read == FragmentImageRead_Texels)
        return readImage(source, image, uv, footprint ? dx : (float2)0, footprint ? dy : (float2)0);
    return sampleImage(source, image, uv, dx, dy, footprint);
}

// The piece of an image drawn in pieces at a point of brush space, picked as fragment.h describes.
uint pieceAt(FragmentPaint r, float2 b) {
    uint best = 0;
    float nearest = asfloat(0x7f800000u);
    for (uint i = 0; i < r.pieceCount; ++i) {
        float4 d = framePieces[(r.pieces + i) * 2];
        if (b.x >= d.x && b.x < d.z && b.y >= d.y && b.y < d.w)
            return i;
        float distance = max(max(d.x - b.x, b.x - d.z), 0.0) + max(max(d.y - b.y, b.y - d.w), 0.0);
        if (distance < nearest) {
            nearest = distance;
            best = i;
        }
    }
    return best;
}

// An image in pieces' color at the pixel, whose center maps to `b` in brush space: its piece's texels, clamped to them.
float4 piecePaint(FragmentPaint r, float2 b, float2 pixel, uint image) {
    uint i = pieceAt(r, b);
    float4 d = framePieces[(r.pieces + i) * 2];
    float4 s = framePieces[(r.pieces + i) * 2 + 1];
    float2 scale = float2(s.z - s.x, s.w - s.y) / float2(d.z - d.x, d.w - d.y);
    float2 uv = (b - d.xy) * scale;
    ImageSource piece;
    piece.texels = s;
    piece.period = (float2)0;
    piece.tileX = PrDrawExtend_Pad;
    piece.tileY = PrDrawExtend_Pad;
    piece.sampling = r.sampling;
    piece.alpha = r.alpha;
    bool footprint = r.sampling == PrDrawSampling_Mipmap;
    float2 dx = 0;
    float2 dy = 0;
    if (footprint) {
        dx = (mapPoint(r.brush, pixel + float2(1, 0)) - b) * scale;
        dy = (mapPoint(r.brush, pixel + float2(0, 1)) - b) * scale;
    }
    return imageColor(r, piece, image, uv, dx, dy, footprint);
}

// Returns `k`, a distance along an axis that repeats from the first tile's start, moved into the first copy from the
// copy that holds it. A point in a reflected copy is mirrored. Half of each gap belongs to the copy on either side of
// it. A filter then places its taps from the copy's own texels. Without the move, a period that is not a whole number
// of texels would shift the taps of every later copy. A tap past the copy's edges still reads the neighboring copies.
float nearCopy(float k, float size, float period, uint mode) {
    if (mode != PrDrawExtend_Repeat && mode != PrDrawExtend_Reflect)
        return k;
    float copy = floor((k + (period - size) / 2) / period);
    k -= copy * period;
    return mode == PrDrawExtend_Reflect && fmod(abs(copy), 2.0) == 1 ? size - k : k;
}

// Moves `k`, a distance along an axis from the first tile's start, into the copy that holds it, and makes the axis's
// mode Pad. It serves tiles whose copies are filtered on their own (PrDrawTileFlag_Clamped). Returns false if `k` falls
// between copies. An axis that does not repeat is left as it is.
bool intoCopy(inout float k, float size, float period, inout uint mode) {
    if (mode != PrDrawExtend_Repeat && mode != PrDrawExtend_Reflect)
        return true;
    float copy = floor(k / period);
    k -= copy * period;
    // At a copy's start, the quotient can round down to the copy before, which leaves `k` a whole period into it.
    if (k >= period) {
        k -= period;
        copy += 1;
    }
    if (k >= size)
        return false;
    if (mode == PrDrawExtend_Reflect && fmod(abs(copy), 2.0) == 1)
        k = size - k;
    mode = PrDrawExtend_Pad;
    return true;
}

// An image brush's color at the pixel, whose center maps to `b` in brush space.
float4 imagePaint(FragmentPaint r, float2 b, float2 pixel, uint image) {
    float4 tileRect = r.tile;
    float4 texels = r.texels;
    float2 tile = tileRect.zw - tileRect.xy;
    float2 scale = (texels.zw - texels.xy) / tile;
    float2 period = (tile + r.spacing) * scale;
    float2 uv = (b - tileRect.xy) * scale;
    bool footprint = r.sampling == PrDrawSampling_Mipmap && r.map == 0;
    float2 dx = 0;
    float2 dy = 0;
    if (footprint) {
        dx = (mapPoint(r.brush, pixel + float2(1, 0)) - b) * scale;
        dy = (mapPoint(r.brush, pixel + float2(0, 1)) - b) * scale;
    }
    ImageSource source;
    source.texels = texels;
    source.period = period;
    source.tileX = r.tileX;
    source.tileY = r.tileY;
    source.sampling = r.sampling;
    source.alpha = r.alpha;
    // The point moves between copies in brush space, before it is scaled to texels, so that a point in any copy rounds
    // to the same texel coordinates as the matching point in the first copy.
    float2 local = b - tileRect.xy;
    float2 spaced = tile + r.spacing;
    if ((r.tileFlags & PrDrawTileFlag_Clamped) != 0) {
        float u = local.x;
        float v = local.y;
        if (!(intoCopy(u, tile.x, spaced.x, source.tileX) && intoCopy(v, tile.y, spaced.y, source.tileY)))
            return (float4)0;
        local = float2(u, v);
    }
    uv = float2(nearCopy(local.x, tile.x, spaced.x, source.tileX), nearCopy(local.y, tile.y, spaced.y, source.tileY))
         * scale;
    return imageColor(r, source, image, uv, dx, dy, footprint);
}

// A layer's pixel at a pixel of the target, transparent outside it.
float4 layerPixel(uint layer, float4 rect, float2 pixel) {
    float2 at = floor(pixel) - rect.xy;
    if (at.x < 0 || at.y < 0 || at.x >= rect.z || at.y >= rect.w)
        return (float4)0;
    return textureAt(layer).Load(int3(int2(at), 0));
}

// sRGB's transfer function and its inverse, mirrored below zero, as core defines them (PrColorTransfer_Srgb).
float linearOf(float c) {
    float m = abs(c);
    return copysignOf(m <= 0.04045 ? m / 12.92 : pow((m + 0.055) / 1.055, 2.4), c);
}

float encodedOf(float c) {
    float m = abs(c);
    return copysignOf(m <= 0.0031308 ? m * 12.92 : 1.055 * pow(m, 1 / 2.4) - 0.055, c);
}

// The premultiplied color scaled down in linear light so that no channel exceeds `limit` times SDR white, which keeps
// its hue (prColorLimit).
float4 limited(float4 c, float limit) {
    if (!(c.a > 0))
        return c;
    float3 s = c.rgb / c.a;
    float3 light = float3(linearOf(s.r), linearOf(s.g), linearOf(s.b));
    float largest = max(light.r, max(light.g, light.b));
    if (!(largest > limit))
        return c;
    light *= limit / largest;
    return float4(float3(encodedOf(light.r), encodedOf(light.g), encodedOf(light.b)) * c.a, c.a);
}

float maskValue(uint mode, float4 m) {
    float v;
    uint kind = mode & FragmentMaskFlag_Kind;
    if (kind == FragmentMask_Alpha) {
        v = m.a;
    } else if (kind == FragmentMask_Luminance) {
        v = dot(kLuminance, m.rgb);
    } else if (m.a > 0) {
        v = dot(kLuminance, float3(linearOf(m.r / m.a), linearOf(m.g / m.a), linearOf(m.b / m.a))) * m.a;
    } else {
        v = 0;
    }
    v = clamp(v, 0.0, 1.0);
    return (mode & FragmentMaskFlag_Invert) != 0 ? 1 - v : v;
}

float windingCoverage(float winding, bool evenOdd) {
    float magnitude = abs(winding);
    if (evenOdd) {
        magnitude -= 2 * floor(magnitude / 2);
        return magnitude > 1 ? 2 - magnitude : magnitude;
    }
    return min(magnitude, 1.0);
}

// The integral of clamp(u, 0, 1) from 0 to u.
float rampIntegral(float u) {
    return u <= 0 ? 0 : u < 1 ? u * u / 2 : u - 0.5;
}

// A pixel's coverage by a tile record's segments, which start where the row's winding leaves off.
float tileCoverage(uint data, uint height, int2 at, bool evenOdd) {
    uint count = uint(frameTiles[data]);
    float winding = frameTiles[data + 1 + uint(at.y)];
    uint s = data + 1 + height;
    float px = float(at.x);
    float row = float(at.y);
    for (uint i = 0; i < count; ++i, s += 4) {
        float x0 = frameTiles[s];
        float y0s = frameTiles[s + 1];
        float x1 = frameTiles[s + 2];
        float y1s = frameTiles[s + 3];
        float y0 = max(min(y0s, y1s), row);
        float y1 = min(max(y0s, y1s), row + 1);
        if (y1 <= y0)
            continue;
        float slope = (x1 - x0) / (y1s - y0s);
        float xa = x0 + (y0 - y0s) * slope;
        float xb = x0 + (y1 - y0s) * slope;
        float average = abs(xb - xa) < 1e-6 ? clamp(px + 1 - (xa + xb) * 0.5, 0.0, 1.0)
                                            : (rampIntegral(px + 1 - xa) - rampIntegral(px + 1 - xb)) / (xb - xa);
        winding += sign(y1s - y0s) * (y1 - y0) * average;
    }
    return windingCoverage(winding, evenOdd);
}

// The pixel of an entry's own texture at a pixel of the target, transparent outside a layer.
float4 entryPixel(uint texture, float2 pixel) {
    FragmentTexture e = frameEntries[texture - 1];
    return layerPixel(texture - 1, float4(float2(e.origin), float2(e.extent)), pixel);
}

// A layer's pixel at a pixel of the target, transparent outside it, or a texture's texel there, over what the entry
// stacks it on.
float4 layerAt(uint texture, float2 pixel) {
    FragmentTexture e = frameEntries[texture - 1];
    float4 c = entryPixel(texture, pixel);
    if (e.under != 0)
        return c + (1 - c.a) * entryPixel(e.under, pixel);
    return c + (1 - c.a) * halfColor(e.color);
}

// The half widths of a mapped stroke each side at a fraction of the way along it.
float2 mappedWidths(FragmentMap m, float fraction) {
    if (m.profileCount == 0)
        return (m.width / 2).xx;
    uint p = m.profile;
    if (fraction <= frameMapData[p])
        return float2(frameMapData[p + 1], frameMapData[p + 2]) * m.width;
    for (uint i = 1; i < m.profileCount; ++i) {
        uint a = p + (i - 1) * 3;
        uint b = p + i * 3;
        if (fraction <= frameMapData[b]) {
            float span = frameMapData[b] - frameMapData[a];
            float t = span > 0 ? (fraction - frameMapData[a]) / span : 1;
            float2 from = float2(frameMapData[a + 1], frameMapData[a + 2]);
            float2 to = float2(frameMapData[b + 1], frameMapData[b + 2]);
            return lerp(from, to, t) * m.width;
        }
    }
    uint last = p + (m.profileCount - 1) * 3;
    return float2(frameMapData[last + 1], frameMapData[last + 2]) * m.width;
}

// The point of a mapped stroke's unit square for a pixel's center, from the nearest point of its centerline. Returns
// false if the centerline does not reach the pixel.
bool strokeUnit(FragmentMap m, float2 p, inout float2 result) {
    int2 cell = int2(floor((p - m.origin) / m.cellSize));
    if (cell.x < 0 || cell.y < 0 || cell.x >= int(m.columns) || cell.y >= int(m.rows))
        return false;
    uint c = uint(cell.y) * m.columns + uint(cell.x);
    float best = m.reach * m.reach;
    bool found = false;
    float distance = 0;
    float offset = 0;
    float span = 0;
    float contour = 0;
    for (uint k = frameMapIndices[m.cells + c]; k < frameMapIndices[m.cells + c + 1]; ++k) {
        uint s = m.segments + frameMapIndices[k] * 6;
        float2 a = float2(frameMapData[s], frameMapData[s + 1]);
        float2 d = float2(frameMapData[s + 2], frameMapData[s + 3]) - a;
        float lengthSquared = dot(d, d);
        float2 pa = p - a;
        float t = lengthSquared > 0 ? clamp(dot(pa, d) / lengthSquared, 0.0, 1.0) : 0;
        float2 near = a + d * t - p;
        float squared = dot(near, near);
        // The cells list their segments in order, so the first of equals is the earliest.
        if (found ? squared >= best - 1e-3 : squared > best)
            continue;
        best = squared;
        found = true;
        span = sqrt(lengthSquared);
        distance = frameMapData[s + 4] + t * span;
        offset = span > 0 ? (d.x * pa.y - d.y * pa.x) / span : 0;
        contour = frameMapData[s + 5];
    }
    if (!found)
        return false;
    float along = contour > 0 ? distance / contour : 0;
    float2 widths = mappedWidths(m, along);
    float across = widths.x + widths.y > 0 ? (offset + widths.x) / (widths.x + widths.y) : 0.5;
    result = m.mapping == PrDrawStrokeMapping_Along ? float2(along, across) : float2(across, along);
    return true;
}

struct Shaded {
    float4 paint;
    float shape;
    float clip;
};

// Returns the area of the pixel on the positive side of a line, which is `d` from the pixel's center and changes by `g`
// per pixel, as prDrawHalfPlaneArea does.
float halfPlaneArea(float d, float2 g) {
    float span = sqrt(g.x * g.x + g.y * g.y);
    if (!(span > 0))
        return d > 0 ? 1.0 : 0.0;
    float distance = d / span;
    float u = max(abs(g.x), abs(g.y)) / span;
    float v = min(abs(g.x), abs(g.y)) / span;
    float reach = (u + v) / 2;
    float m = abs(distance);
    if (m >= reach)
        return distance > 0 ? 1.0 : 0.0;
    if (m <= (u - v) / 2)
        return 0.5 + distance / u;
    float t = reach - m;
    float corner = t * t / (2 * u * v);
    return distance > 0 ? 1 - corner : corner;
}

// The color of a sided ring at the point `p` in local space: the color of the side that the point lies on, blended
// across the line that divides it from its neighbor at the nearest corner by the area of the pixel on either side.
float4 sidedColor(FragmentSidedRing r, float2 p) {
    float2 split = r.split;
    uint corner = p.y < split.y ? (p.x < split.x ? 0u : 1u) : (p.x < split.x ? 3u : 2u);
    float3 divide = float3(r.divides[corner][0], r.divides[corner][1], r.divides[corner][2]);
    float d = dot(divide.xy, p) + divide.z;
    float2 g = float2(dot(divide.xy, prGpuDdxFine(p)), dot(divide.xy, prGpuDdyFine(p)));
    float after = halfPlaneArea(d, g);
    return lerp(r.colors[corner][0], r.colors[corner][1], after);
}

// The instance's coverage of the pixel. Bytes and Tiles coverage read the pixel of the quad nearest to the point: the
// root views that hold their records are not bounds checked, and a point can fall just outside the quad, as at its edge
// or in a helper lane that runs only for derivatives, where an index from it would read far outside the records.
float instanceCoverage(VertexOut input, FragmentInstance i, float2 pixel) {
    uint kind = i.flags & FragmentInstanceFlag_Coverage;
    uint v = i.data * 4;            // the record's byte offset in the values
    int2 at = int2(floor(input.local));
    switch (kind) {
    case FragmentCoverage_Rect: return rectArea(frameValues.Load<float4>(v), floor(pixel));
    case FragmentCoverage_Shape: return shapedCoverage(frameValues.Load<FragmentCornerRect>(v), input.local);
    case FragmentCoverage_Ring: {
        FragmentRing r = frameValues.Load<FragmentRing>(v);
        float ring = shapedCoverage(r.outer, input.local) - shapedCoverage(r.inner, input.local);
        float wedge = planeCoverage(float3(r.planes[0][0], r.planes[0][1], r.planes[0][2]), input.local)
                      * planeCoverage(float3(r.planes[1][0], r.planes[1][1], r.planes[1][2]), input.local)
                      * planeCoverage(float3(r.planes[2][0], r.planes[2][1], r.planes[2][2]), input.local);
        return clamp(ring, 0.0, 1.0) * (r.outside != 0 ? 1 - wedge : wedge);
    }
    case FragmentCoverage_SidedRing: {
        FragmentSidedRing r = frameValues.Load<FragmentSidedRing>(v);
        return clamp(shapedCoverage(r.outer, input.local) - shapedCoverage(r.inner, input.local), 0.0, 1.0);
    }
    case FragmentCoverage_Shadow: return shadowCoverage(frameValues.Load<FragmentShadow>(v), input.local);
    case FragmentCoverage_Triangle: return input.coverage;
    case FragmentCoverage_Atlas:
        return textureAt(i.data - 1).Load(int3(at, 0)).r;
    case FragmentCoverage_Bytes: {
        int2 cell = clamp(at, 0, int2(i.bounds.zw) - 1);
        uint byte = i.data + uint(cell.y) * uint(i.bounds.z) + uint(cell.x);
        return float((frameCoverageBytes.Load(byte & ~3u) >> ((byte & 3u) * 8)) & 0xffu) / 255;
    }
    case FragmentCoverage_TilesNonZero:
        return tileCoverage(i.data, uint(i.bounds.w), clamp(at, 0, int2(i.bounds.zw) - 1), false);
    case FragmentCoverage_TilesEvenOdd:
        return tileCoverage(i.data, uint(i.bounds.w), clamp(at, 0, int2(i.bounds.zw) - 1), true);
    default: return 1;
    }
}

Shaded shade(VertexOut input) {
    FragmentInstance i = frameInstances[input.instance];
    float2 pixel = input.position.xy + draw.origin;
    Shaded result;
    result.shape = instanceCoverage(input, i, pixel);
    result.clip = rectArea(input.clipRect, floor(pixel));
    if (i.clip != 0) {
        FragmentClip c = frameClips[i.clip - 1];
        for (uint k = 0; k < c.shapeCount; ++k)
            result.clip *= shapeCoverage(frameShapes[frameClipShapes[c.shapes + k]], pixel);
        if (c.mask != 0)
            result.clip *= layerAt(c.mask, pixel).a;
    }
    float4 color = halfColor(i.color);
    if ((i.flags & FragmentInstanceFlag_Coverage) == FragmentCoverage_SidedRing)
        color = sidedColor(frameValues.Load<FragmentSidedRing>(i.data * 4), input.local);
    if (i.paint == 0) {
        result.paint = color;
        return result;
    }
    FragmentPaint r = framePaints[i.paint - 1];
    // The point the brush transform places: the pixel's center, or its point of a mapped stroke's unit square.
    float2 placed = pixel;
    if (r.map != 0 && !strokeUnit(frameMaps[r.map - 1], pixel, placed)) {
        result.paint = (float4)0;
        return result;
    }
    float2 b = mapPoint(r.brush, placed);
    if (r.kind == FragmentPaintKind_Solid) {
        result.paint = color;
    } else if (r.kind == FragmentPaintKind_Image) {
        uint image = r.texture - 1;
        float4 texel = r.pieceCount != 0 ? piecePaint(r, b, pixel, image) : imagePaint(r, b, pixel, image);
        result.paint = (r.limit > 0 ? limited(texel, r.limit) : texel) * color;
    } else if (r.kind == FragmentPaintKind_Layer) {
        result.paint = layerAt(r.texture, pixel) * color;
    } else {
        float t = 0;
        bool paints = gradientParameter(r, b, t) && extended(r.extend, t);
        result.paint = paints ? rampColor(r.ramp, t) * color : (float4)0;
    }
    if (r.groupMask != FragmentMask_None)
        result.paint *= maskValue(r.groupMask, layerAt(r.groupMaskTexture, pixel));
    return result;
}

// ---------------------------------------------------------------------------------------------------------------------
// Blending and compositing, on premultiplied colors, by the formulas of the blend modes and operators.

float blendChannel(uint mode, float b, float s) {
    switch (mode) {
    case PrDrawBlendMode_Multiply: return b * s;
    case PrDrawBlendMode_Screen: return b + s - b * s;
    case PrDrawBlendMode_Overlay: return b <= 0.5 ? s * 2 * b : s + (2 * b - 1) - s * (2 * b - 1);
    case PrDrawBlendMode_Darken: return min(b, s);
    case PrDrawBlendMode_Lighten: return max(b, s);
    case PrDrawBlendMode_ColorDodge: return b <= 0 ? 0 : (s >= 1 ? 1 : min(1.0, b / (1 - s)));
    case PrDrawBlendMode_ColorBurn: return b >= 1 ? 1 : (s <= 0 ? 0 : 1 - min(1.0, (1 - b) / s));
    case PrDrawBlendMode_HardLight: return s <= 0.5 ? b * 2 * s : b + (2 * s - 1) - b * (2 * s - 1);
    case PrDrawBlendMode_SoftLight: {
        if (s <= 0.5)
            return b - (1 - 2 * s) * b * (1 - b);
        float d = b <= 0.25 ? ((16 * b - 12) * b + 4) * b : sqrt(b);
        return b + (2 * s - 1) * (d - b);
    }
    case PrDrawBlendMode_Difference: return abs(b - s);
    case PrDrawBlendMode_Exclusion: return b + s - 2 * b * s;
    case PrDrawBlendMode_HardMix: return b + s >= 1 ? 1 : 0;
    default: return s;
    }
}

float lum(float3 c) {
    return 0.3 * c.r + 0.59 * c.g + 0.11 * c.b;
}

float3 clipColor(float3 c) {
    float l = lum(c);
    float n = min(c.r, min(c.g, c.b));
    float x = max(c.r, max(c.g, c.b));
    if (n < 0 && l - n > 0)
        c = l + (c - l) * l / (l - n);
    if (x > 1 && x - l > 0)
        c = l + (c - l) * (1 - l) / (x - l);
    return c;
}

float3 setLum(float3 c, float l) {
    return clipColor(c + (l - lum(c)));
}

float sat(float3 c) {
    return max(c.r, max(c.g, c.b)) - min(c.r, min(c.g, c.b));
}

float3 setSat(float3 c, float s) {
    float low = min(c.r, min(c.g, c.b));
    float high = max(c.r, max(c.g, c.b));
    return high > low ? (c - low) * s / (high - low) : (float3)0;
}

float3 blendColor(uint mode, float3 b, float3 s) {
    switch (mode) {
    case PrDrawBlendMode_Hue: return setLum(setSat(s, sat(b)), lum(b));
    case PrDrawBlendMode_Saturation: return setLum(setSat(b, sat(s)), lum(b));
    case PrDrawBlendMode_Color: return setLum(s, lum(b));
    case PrDrawBlendMode_Luminosity: return setLum(b, lum(s));
    default: return float3(blendChannel(mode, b.r, s.r), blendChannel(mode, b.g, s.g), blendChannel(mode, b.b, s.b));
    }
}

float4 composite(float4 s, float4 d, uint op, uint mode) {
    if (mode != PrDrawBlendMode_Normal && s.a > 0) {
        float3 cs = s.rgb / s.a;
        float3 cb = d.a > 0 ? d.rgb / d.a : (float3)0;
        float3 mixed = clamp(blendColor(mode, cb, cs), 0.0, 1.0);
        s.rgb = ((1 - d.a) * cs + d.a * mixed) * s.a;
    }
    if (op == PrDrawComposite_PlusLighter)
        return min((float4)1, s + d);
    if (op == PrDrawComposite_PlusDarker) {
        float alpha = min(1.0, s.a + d.a);
        return float4(max((float3)0, alpha - (s.a - s.rgb) - (d.a - d.rgb)), alpha);
    }
    float fa = 1;
    float fb = 1 - s.a;
    switch (op) {
    case PrDrawComposite_Clear: fa = 0; fb = 0; break;
    case PrDrawComposite_Copy: fa = 1; fb = 0; break;
    case PrDrawComposite_SourceIn: fa = d.a; fb = 0; break;
    case PrDrawComposite_SourceOut: fa = 1 - d.a; fb = 0; break;
    case PrDrawComposite_SourceAtop: fa = d.a; fb = 1 - s.a; break;
    case PrDrawComposite_DestinationOver: fa = 1 - d.a; fb = 1; break;
    case PrDrawComposite_DestinationIn: fa = 0; fb = s.a; break;
    case PrDrawComposite_DestinationOut: fa = 0; fb = 1 - s.a; break;
    case PrDrawComposite_DestinationAtop: fa = 1 - d.a; fb = s.a; break;
    case PrDrawComposite_Xor: fa = 1 - d.a; fb = 1 - s.a; break;
    default: break;
    }
    return s * fa + d * fb;
}

[shader("fragment")]
float4 fragmentShade(VertexOut input) : SV_Target {
    Shaded s = shade(input);
    return s.paint * (s.shape * s.clip);
}

// Composites onto the destination, which it reads from the target.
[shader("fragment")]
float4 fragmentBlend(VertexOut input PR_GPU_TARGET_PARAM) : SV_Target {
    Shaded s = shade(input);
    float4 under = prGpuTarget(input.position);
    FragmentInstance i = frameInstances[input.instance];
    FragmentPaint r = framePaints[i.paint - 1];
    float covered = s.shape * s.clip;
    if (r.knockout != 0)
        return lerp(under, composite(s.paint, (float4)0, r.composite, r.blendMode), covered);
    return lerp(under, composite(s.paint, under, r.composite, r.blendMode), covered);
}

// ---------------------------------------------------------------------------------------------------------------------
// Filter steps, each computing its operation as draw_fragment.h defines it.

float4 toLinear(float4 c) {
    if (!(c.a > 0))
        return c;
    float3 s = c.rgb / c.a;
    return float4(float3(linearOf(s.r), linearOf(s.g), linearOf(s.b)) * c.a, c.a);
}

float4 toEncoded(float4 c) {
    if (!(c.a > 0))
        return c;
    float3 s = c.rgb / c.a;
    return float4(float3(encodedOf(s.r), encodedOf(s.g), encodedOf(s.b)) * c.a, c.a);
}

float4 straightOf(float4 c) {
    float a = clamp(c.a, 0.0, 1.0);
    return float4(a > 0 ? clamp(c.rgb / a, 0.0, 1.0) : (float3)0, a);
}

float4 premultiplied(float4 s) {
    float a = clamp(s.a, 0.0, 1.0);
    return float4(clamp(s.rgb, 0.0, 1.0) * a, a);
}

// The filter's inputs at pixels of the target, transparent outside their layers, in the space the step reads.
struct Inputs {
    FragmentFilter filter;
    uint flags;
};

// Where the input sits in the target's pixels: left, top, width and height.
float4 inputRect(Inputs inputs, uint which) {
    FragmentTexture e = frameEntries[inputs.filter.inputs[which] - 1];
    return float4(float2(e.origin), float2(e.extent));
}

float4 readInput(Inputs inputs, uint which, int2 p) {
    uint texture = inputs.filter.inputs[which];
    if (texture == 0)
        return (float4)0;
    float4 c = layerAt(texture, float2(p));
    bool toLight = (inputs.flags & FragmentFilterFlag_LinearInputs) != 0 && which != 2
                   && !(which == 0 && (inputs.flags & FragmentFilterFlag_RawFirst) != 0);
    return toLight ? toLinear(c) : c;
}

// The index along an axis for a read at `i` within [lo, hi), by an edge mode. Returns false where it reads nothing.
bool edgeIndex(int i, int lo, int hi, uint edge, inout int result) {
    if (i >= lo && i < hi) {
        result = i;
    } else if (edge == PrImageEdgeMode_Duplicate) {
        result = i < lo ? lo : hi - 1;
    } else if (edge == PrImageEdgeMode_Wrap) {
        int n = hi - lo;
        result = lo + (((i - lo) % n) + n) % n;
    } else if (edge == PrImageEdgeMode_Mirror) {
        int period = (hi - lo) * 2;
        int at = (((i - lo) % period) + period) % period;
        result = lo + (at < hi - lo ? at : period - 1 - at);
    } else {
        return false;
    }
    return true;
}

float4 blurAlong(Inputs inputs, int2 p, int2 axis) {
    FragmentFilter f = inputs.filter;
    float sigma = f.values[0];
    if (!(sigma > 0))
        return readInput(inputs, 0, p);
    uint edge = uint(f.values[1]);
    int lo = axis.x != 0 ? int(f.values[2]) : int(f.values[3]);
    int hi = axis.x != 0 ? int(f.values[4]) : int(f.values[5]);
    int radius = int(ceil(sigma * 4));
    float4 sum = (float4)0;
    float total = 0;
    for (int k = -radius; k <= radius; ++k) {
        float weight = exp(-float(k * k) / (2 * sigma * sigma));
        total += weight;
        int along = (axis.x != 0 ? p.x : p.y) + k;
        int at = 0;
        if (!edgeIndex(along, lo, hi, edge, at))
            continue;
        int2 q = axis.x != 0 ? int2(at, p.y) : int2(p.x, at);
        sum += readInput(inputs, 0, q) * weight;
    }
    return sum / total;
}

float4 morphologyAlong(Inputs inputs, int2 p, int2 axis) {
    FragmentFilter f = inputs.filter;
    int radius = int(f.values[0]);
    bool dilate = f.values[1] != 0;
    float4 best = (dilate ? 0.0 : 1.0).xxxx;
    for (int k = -radius; k <= radius; ++k) {
        float4 v = readInput(inputs, 0, p + axis * k);
        best = dilate ? max(best, v) : min(best, v);
    }
    return best;
}

float transferChannel(uint d, inout uint at, float c) {
    uint kind = uint(frameFilterData[d + at]);
    float slope = frameFilterData[d + at + 1];
    float intercept = frameFilterData[d + at + 2];
    float amplitude = frameFilterData[d + at + 3];
    float exponent = frameFilterData[d + at + 4];
    float offset = frameFilterData[d + at + 5];
    uint count = uint(frameFilterData[d + at + 6]);
    uint table = d + at + 7;
    at += 7 + count;
    switch (kind) {
    case PrDrawTransferKind_Table: {
        if (count == 1)
            return frameFilterData[table];
        float x = c * float(count - 1);
        uint k = uint(clamp(min(x, float(count - 2)), 0.0, float(count - 2)));
        return frameFilterData[table + k]
               + (x - float(k)) * (frameFilterData[table + k + 1] - frameFilterData[table + k]);
    }
    case PrDrawTransferKind_Discrete: {
        uint k = uint(c * float(count));
        return frameFilterData[table + (k >= count ? count - 1 : k)];
    }
    case PrDrawTransferKind_Linear: return slope * c + intercept;
    case PrDrawTransferKind_Gamma: return amplitude * pow(c, exponent) + offset;
    default: return c;
    }
}

float noise2(uint d, uint channel, float vx, float vy, bool stitch, int4 s) {
    uint lattice = d + 15;
    uint gradients = d + 15 + 514;
    // Stitching wraps the lattice points before they are masked into the lattice, since its bounds lie beyond the mask.
    float t = vx + 4096;
    int bx0 = int(t);
    int bx1 = bx0 + 1;
    float rx0 = t - float(int(t));
    float rx1 = rx0 - 1;
    t = vy + 4096;
    int by0 = int(t);
    int by1 = by0 + 1;
    float ry0 = t - float(int(t));
    float ry1 = ry0 - 1;
    if (stitch) {
        if (bx0 >= s.z) bx0 -= s.x;
        if (bx1 >= s.z) bx1 -= s.x;
        if (by0 >= s.w) by0 -= s.y;
        if (by1 >= s.w) by1 -= s.y;
    }
    bx0 &= 255;
    bx1 &= 255;
    by0 &= 255;
    by1 &= 255;
    int i = int(frameFilterData[lattice + bx0]);
    int j = int(frameFilterData[lattice + bx1]);
    int b00 = int(frameFilterData[lattice + i + by0]);
    int b10 = int(frameFilterData[lattice + j + by0]);
    int b01 = int(frameFilterData[lattice + i + by1]);
    int b11 = int(frameFilterData[lattice + j + by1]);
    float sx = rx0 * rx0 * (3 - 2 * rx0);
    float sy = ry0 * ry0 * (3 - 2 * ry0);
    uint g = gradients + channel * 514 * 2;
    float u = rx0 * frameFilterData[g + b00 * 2] + ry0 * frameFilterData[g + b00 * 2 + 1];
    float v = rx1 * frameFilterData[g + b10 * 2] + ry0 * frameFilterData[g + b10 * 2 + 1];
    float a = u + sx * (v - u);
    u = rx0 * frameFilterData[g + b01 * 2] + ry1 * frameFilterData[g + b01 * 2 + 1];
    v = rx1 * frameFilterData[g + b11 * 2] + ry1 * frameFilterData[g + b11 * 2 + 1];
    float b = u + sx * (v - u);
    return a + sy * (b - a);
}

float4 turbulence(uint d, float2 pixel) {
    float2 p = float2(frameFilterData[d] * pixel.x + frameFilterData[d + 2] * pixel.y + frameFilterData[d + 4],
                      frameFilterData[d + 1] * pixel.x + frameFilterData[d + 3] * pixel.y + frameFilterData[d + 5]);
    float2 frequency = float2(frameFilterData[d + 6], frameFilterData[d + 7]);
    uint octaves = uint(frameFilterData[d + 8]);
    bool fractal = frameFilterData[d + 9] != 0;
    bool stitching = frameFilterData[d + 10] != 0;
    int4 s = (int4)0;
    if (stitching) {
        float tileWidth = frameFilterData[d + 13] - frameFilterData[d + 11];
        float tileHeight = frameFilterData[d + 14] - frameFilterData[d + 12];
        if (frequency.x != 0 && tileWidth > 0) {
            float low = floor(tileWidth * frequency.x) / tileWidth;
            float high = ceil(tileWidth * frequency.x) / tileWidth;
            frequency.x = low > 0 && frequency.x / low < high / frequency.x ? low : high;
        }
        if (frequency.y != 0 && tileHeight > 0) {
            float low = floor(tileHeight * frequency.y) / tileHeight;
            float high = ceil(tileHeight * frequency.y) / tileHeight;
            frequency.y = low > 0 && frequency.y / low < high / frequency.y ? low : high;
        }
        s.x = int(tileWidth * frequency.x + 0.5);
        s.z = int(frameFilterData[d + 11] * frequency.x + 4096 + float(s.x));
        s.y = int(tileHeight * frequency.y + 0.5);
        s.w = int(frameFilterData[d + 12] * frequency.y + 4096 + float(s.y));
    }
    float4 result;
    for (uint channel = 0; channel < 4; ++channel) {
        float sum = 0;
        float vx = p.x * frequency.x;
        float vy = p.y * frequency.y;
        float ratio = 1;
        int4 st = s;
        for (uint octave = 0; octave < octaves; ++octave) {
            float value = noise2(d, channel, vx, vy, stitching, st);
            sum += (fractal ? value : abs(value)) / ratio;
            vx *= 2;
            vy *= 2;
            ratio *= 2;
            st.x *= 2;
            st.z = 2 * st.z - 4096;
            st.y *= 2;
            st.w = 2 * st.w - 4096;
        }
        result[channel] = fractal ? (sum + 1) / 2 : sum;
    }
    return premultiplied(result);
}

// The first input's alpha, reading its edge pixels beyond its layer.
float edgeAlpha(Inputs inputs, int x, int y) {
    float4 rect = inputRect(inputs, 0);
    int2 q = clamp(int2(x, y), int2(rect.xy), int2(rect.xy + rect.zw) - 1);
    return readInput(inputs, 0, q).a;
}

float4 lighting(Inputs inputs, uint d, int2 p) {
    bool specular = frameFilterData[d] != 0;
    float surface = frameFilterData[d + 1];
    float strength = frameFilterData[d + 2];
    float exponent = frameFilterData[d + 3];
    float3 color = float3(frameFilterData[d + 4], frameFilterData[d + 5], frameFilterData[d + 6]);
    uint kind = uint(frameFilterData[d + 7]);
    int x = p.x;
    int y = p.y;
    float nx = -surface / 4
               * ((edgeAlpha(inputs, x + 1, y - 1) + 2 * edgeAlpha(inputs, x + 1, y) + edgeAlpha(inputs, x + 1, y + 1))
                  - (edgeAlpha(inputs, x - 1, y - 1) + 2 * edgeAlpha(inputs, x - 1, y)
                     + edgeAlpha(inputs, x - 1, y + 1)));
    float ny = -surface / 4
               * ((edgeAlpha(inputs, x - 1, y + 1) + 2 * edgeAlpha(inputs, x, y + 1) + edgeAlpha(inputs, x + 1, y + 1))
                  - (edgeAlpha(inputs, x - 1, y - 1) + 2 * edgeAlpha(inputs, x, y - 1)
                     + edgeAlpha(inputs, x + 1, y - 1)));
    float3 n = normalize(float3(nx, ny, 1));
    float px = float(x);
    float py = float(y);
    float3 light = float3(frameFilterData[d + 10], frameFilterData[d + 11], frameFilterData[d + 12]);
    float3 aim = float3(frameFilterData[d + 13], frameFilterData[d + 14], frameFilterData[d + 15]);
    float z = surface * edgeAlpha(inputs, x, y) * frameFilterData[d + 18];
    float3 l;
    if (kind == PrDrawLightKind_Distant) {
        float azimuth = frameFilterData[d + 8];
        float elevation = frameFilterData[d + 9];
        l = float3(cos(azimuth) * cos(elevation), sin(azimuth) * cos(elevation), sin(elevation));
    } else {
        l = light - float3(px, py, z);
        float span = sqrt(dot(l, l));
        l = span > 0 ? l / span : (float3)0;
        if (kind == PrDrawLightKind_Spot) {
            float3 spot = aim - light;
            spot /= sqrt(dot(spot, spot));
            float along = -dot(l, spot);
            float factor = along < frameFilterData[d + 17] ? 0 : pow(max(along, 0.0), frameFilterData[d + 16]);
            color *= factor;
        }
    }
    if (specular) {
        float3 h = l + float3(0, 0, 1);
        float hLength = sqrt(dot(h, h));
        float factor = strength * pow(max(hLength > 0 ? dot(n, h) / hLength : 0.0, 0.0), exponent);
        float3 c = clamp(color * factor, 0.0, 1.0);
        return float4(c, max(c.r, max(c.g, c.b)));
    }
    float factor = strength * max(dot(n, l), 0.0);
    return premultiplied(float4(clamp(color * factor, 0.0, 1.0), 1));
}

[shader("fragment")]
float4 filterStep(VertexOut vertexIn) : SV_Target {
    FragmentFilter f = frameFilters[draw.filterIndex];
    Inputs inputs;
    inputs.filter = f;
    inputs.flags = f.flags;
    float2 pixel = vertexIn.position.xy + draw.origin;
    int2 p = int2(floor(pixel));
    uint d = f.data;
    float4 result;
    switch (f.op) {
    case FragmentFilterOp_Offset: {
        float2 shift = float2(f.values[0], f.values[1]);
        int2 whole = int2(floor(shift));
        float2 part = shift - float2(whole);
        int2 s = p - whole;
        float4 a = readInput(inputs, 0, s) * (1 - part.x) + readInput(inputs, 0, s - int2(1, 0)) * part.x;
        float4 b = readInput(inputs, 0, s - int2(0, 1)) * (1 - part.x) + readInput(inputs, 0, s - int2(1, 1)) * part.x;
        result = a * (1 - part.y) + b * part.y;
        break;
    }
    case FragmentFilterOp_Flood:
        result = float4(f.values[0], f.values[1], f.values[2], f.values[3]);
        break;
    case FragmentFilterOp_ColorMatrix: {
        float4 s = straightOf(readInput(inputs, 0, p));
        float4 m;
        for (uint r = 0; r < 4; ++r) {
            m[r] = frameFilterData[d + r * 5] * s.r + frameFilterData[d + r * 5 + 1] * s.g
                   + frameFilterData[d + r * 5 + 2] * s.b + frameFilterData[d + r * 5 + 3] * s.a
                   + frameFilterData[d + r * 5 + 4];
        }
        result = premultiplied(m);
        break;
    }
    case FragmentFilterOp_Transfer: {
        float4 s = straightOf(readInput(inputs, 0, p));
        uint at = 0;
        for (uint c = 0; c < 4; ++c)
            s[c] = transferChannel(d, at, s[c]);
        result = premultiplied(s);
        break;
    }
    case FragmentFilterOp_Composite:
        result = composite(readInput(inputs, 0, p), readInput(inputs, 1, p), uint(f.values[0]), uint(f.values[1]));
        break;
    case FragmentFilterOp_Arithmetic: {
        float4 a = readInput(inputs, 0, p);
        float4 b = readInput(inputs, 1, p);
        float4 k = float4(f.values[0], f.values[1], f.values[2], f.values[3]);
        float alpha = clamp(k.x * a.a * b.a + k.y * a.a + k.z * b.a + k.w, 0.0, 1.0);
        result = float4(clamp(k.x * a.rgb * b.rgb + k.y * a.rgb + k.z * b.rgb + k.w, 0.0, alpha), alpha);
        break;
    }
    case FragmentFilterOp_BlurX: result = blurAlong(inputs, p, int2(1, 0)); break;
    case FragmentFilterOp_BlurY: result = blurAlong(inputs, p, int2(0, 1)); break;
    case FragmentFilterOp_MorphologyX: result = morphologyAlong(inputs, p, int2(1, 0)); break;
    case FragmentFilterOp_MorphologyY: result = morphologyAlong(inputs, p, int2(0, 1)); break;
    case FragmentFilterOp_Convolution: {
        uint columns = uint(f.values[0]);
        uint rows = uint(f.values[1]);
        float divisor = f.values[2];
        if (divisor == 0) {
            for (uint i = 0; i < columns * rows; ++i)
                divisor += frameFilterData[d + i];
            if (divisor == 0)
                divisor = 1;
        }
        uint edge = uint(f.values[6]);
        bool preserve = f.values[7] != 0;
        float4 sum = (float4)0;
        for (uint j = 0; j < rows; ++j) {
            for (uint i = 0; i < columns; ++i) {
                int sx = 0;
                int sy = 0;
                if (!edgeIndex(p.x - int(f.values[4]) + int(i), int(f.values[8]), int(f.values[10]), edge, sx)
                    || !edgeIndex(p.y - int(f.values[5]) + int(j), int(f.values[9]), int(f.values[11]), edge, sy))
                    continue;
                float weight = frameFilterData[d + (rows - 1 - j) * columns + (columns - 1 - i)];
                float4 v = readInput(inputs, 0, int2(sx, sy));
                sum += (preserve ? straightOf(v) : v) * weight;
            }
        }
        float bias = f.values[3];
        if (preserve) {
            result = premultiplied(float4(sum.rgb / divisor + bias, readInput(inputs, 0, p).a));
        } else {
            float alpha = clamp(sum.a / divisor + bias, 0.0, 1.0);
            result = float4(clamp(sum.rgb / divisor + bias, 0.0, alpha), alpha);
        }
        break;
    }
    case FragmentFilterOp_Turbulence: result = turbulence(d, float2(p)); break;
    case FragmentFilterOp_Tile: {
        int2 lo = int2(int(f.values[0]), int(f.values[1]));
        int2 size = int2(int(f.values[2]), int(f.values[3])) - lo;
        if (size.x <= 0 || size.y <= 0) {
            result = (float4)0;
            break;
        }
        int2 q = lo + (((p - lo) % size) + size) % size;
        result = readInput(inputs, 0, q);
        break;
    }
    case FragmentFilterOp_Alpha: result = float4(0, 0, 0, readInput(inputs, 0, p).a); break;
    case FragmentFilterOp_InverseAlpha: result = float4(0, 0, 0, 1 - readInput(inputs, 0, p).a); break;
    case FragmentFilterOp_Colorize: {
        float alpha = clamp(readInput(inputs, 0, p).a, 0.0, 1.0) * f.values[3];
        result = float4(float3(f.values[0], f.values[1], f.values[2]) * alpha, alpha);
        break;
    }
    case FragmentFilterOp_Over: {
        float4 a = readInput(inputs, 0, p);
        result = a + readInput(inputs, 1, p) * (1 - a.a);
        break;
    }
    case FragmentFilterOp_Atop: {
        float4 a = readInput(inputs, 0, p);
        float4 b = readInput(inputs, 1, p);
        result = float4(a.rgb * b.a + b.rgb * (1 - a.a), b.a);
        break;
    }
    case FragmentFilterOp_Mask:
        result = readInput(inputs, 0, p) * clamp(readInput(inputs, 1, p).a, 0.0, 1.0);
        break;
    case FragmentFilterOp_Displacement: {
        float4 m = straightOf(readInput(inputs, 1, p));
        float x = float(p.x) + f.values[0] * (m[uint(f.values[2])] - 0.5);
        float y = float(p.y) + f.values[1] * (m[uint(f.values[3])] - 0.5);
        result = readInput(inputs, 0, int2(floor(float2(x, y) + 0.5)));
        break;
    }
    case FragmentFilterOp_Lighting: result = lighting(inputs, d, p); break;
    case FragmentFilterOp_Mix: {
        float t = (clamp(readInput(inputs, 2, p).a, 0.0, 1.0) - f.values[0]) / (f.values[1] - f.values[0]);
        result = lerp(readInput(inputs, 0, p), readInput(inputs, 1, p), clamp(t, 0.0, 1.0));
        break;
    }
    default: result = (float4)0; break;
    }
    return (f.flags & FragmentFilterFlag_LinearResult) != 0 ? toEncoded(result) : result;
}
