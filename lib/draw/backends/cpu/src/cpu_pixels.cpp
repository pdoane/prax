// Pixels for the CPU backend: device boxes, coverage, targets, paints, textures and compositing.

#include "draw/backends/cpu/src/cpu_pixels.h"

#include "pr/alloc.h"
#include "pr/color_space.h"
#include "pr/draw_lower.h"
#include "pr/float.h"
#include "pr/float16.h"
#include "pr/mem.h"
#include "pr/path_stroke.h"
#include "pr/vec.h"

namespace pr_impl {
namespace {

constexpr float kInverse255 = 1.0f / 255;

// ---------------------------------------------------------------------------------------------------------------------
// Blending

// The blend modes that act on each channel.
float blendChannel(PrDrawBlendMode mode, float b, float s) {
    switch (mode) {
        case PrDrawBlendMode_Multiply:
            return b * s;
        case PrDrawBlendMode_Screen:
            return b + s - b * s;
        case PrDrawBlendMode_Overlay:
            return blendChannel(PrDrawBlendMode_HardLight, s, b);
        case PrDrawBlendMode_Darken:
            return minF32(b, s);
        case PrDrawBlendMode_Lighten:
            return maxF32(b, s);
        case PrDrawBlendMode_ColorDodge:
            if (b <= 0)
                return 0;
            return s >= 1 ? 1 : minF32(1, b / (1 - s));
        case PrDrawBlendMode_ColorBurn:
            if (b >= 1)
                return 1;
            return s <= 0 ? 0 : 1 - minF32(1, (1 - b) / s);
        case PrDrawBlendMode_HardLight:
            return s <= 0.5f ? b * 2 * s : blendChannel(PrDrawBlendMode_Screen, b, 2 * s - 1);
        case PrDrawBlendMode_SoftLight: {
            if (s <= 0.5f)
                return b - (1 - 2 * s) * b * (1 - b);
            float d = b <= 0.25f ? ((16 * b - 12) * b + 4) * b : sqrtF32(b);
            return b + (2 * s - 1) * (d - b);
        }
        case PrDrawBlendMode_Difference:
            return absF32(b - s);
        case PrDrawBlendMode_Exclusion:
            return b + s - 2 * b * s;
        case PrDrawBlendMode_HardMix:
            return b + s >= 1 ? 1 : 0;
        default:
            return s;
    }
}

// The blend modes that act on the whole color, with their helpers: luminosity, saturation and the clip into gamut.
float lum(const float c[3]) {
    return 0.3f * c[0] + 0.59f * c[1] + 0.11f * c[2];
}

void clipColor(float c[3]) {
    float l = lum(c);
    float n = minF32(c[0], minF32(c[1], c[2]));
    float x = maxF32(c[0], maxF32(c[1], c[2]));
    for (uint32_t i = 0; i < 3; ++i) {
        if (n < 0 && l - n > 0)
            c[i] = l + (c[i] - l) * l / (l - n);
        if (x > 1 && x - l > 0)
            c[i] = l + (c[i] - l) * (1 - l) / (x - l);
    }
}

void setLum(const float c[3], float l, float out[3]) {
    float d = l - lum(c);
    for (uint32_t i = 0; i < 3; ++i)
        out[i] = c[i] + d;
    clipColor(out);
}

float sat(const float c[3]) {
    return maxF32(c[0], maxF32(c[1], c[2])) - minF32(c[0], minF32(c[1], c[2]));
}

void setSat(const float c[3], float s, float out[3]) {
    uint32_t order[3] = {0, 1, 2};
    for (uint32_t i = 0; i < 2; ++i) {
        for (uint32_t j = 0; j < 2 - i; ++j) {
            if (c[order[j]] > c[order[j + 1]]) {
                uint32_t swap = order[j];
                order[j] = order[j + 1];
                order[j + 1] = swap;
            }
        }
    }
    float low = c[order[0]];
    float middle = c[order[1]];
    float high = c[order[2]];
    out[0] = out[1] = out[2] = 0;
    if (high > low) {
        out[order[1]] = (middle - low) * s / (high - low);
        out[order[2]] = s;
    }
}

void blendColor(PrDrawBlendMode mode, const float b[3], const float s[3], float out[3]) {
    float t[3];
    switch (mode) {
        case PrDrawBlendMode_Hue:
            setSat(s, sat(b), t);
            setLum(t, lum(b), out);
            return;
        case PrDrawBlendMode_Saturation:
            setSat(b, sat(s), t);
            setLum(t, lum(b), out);
            return;
        case PrDrawBlendMode_Color:
            setLum(s, lum(b), out);
            return;
        case PrDrawBlendMode_Luminosity:
            setLum(b, lum(s), out);
            return;
        default:
            for (uint32_t i = 0; i < 3; ++i)
                out[i] = blendChannel(mode, b[i], s[i]);
            return;
    }
}

// The Porter-Duff factors for the source and destination.
void factors(PrDrawComposite op, float sa, float da, float* fa, float* fb) {
    switch (op) {
        case PrDrawComposite_Clear: *fa = 0; *fb = 0; return;
        case PrDrawComposite_Copy: *fa = 1; *fb = 0; return;
        case PrDrawComposite_SourceIn: *fa = da; *fb = 0; return;
        case PrDrawComposite_SourceOut: *fa = 1 - da; *fb = 0; return;
        case PrDrawComposite_SourceAtop: *fa = da; *fb = 1 - sa; return;
        case PrDrawComposite_DestinationOver: *fa = 1 - da; *fb = 1; return;
        case PrDrawComposite_DestinationIn: *fa = 0; *fb = sa; return;
        case PrDrawComposite_DestinationOut: *fa = 0; *fb = 1 - sa; return;
        case PrDrawComposite_DestinationAtop: *fa = 1 - da; *fb = sa; return;
        case PrDrawComposite_Xor: *fa = 1 - da; *fb = 1 - sa; return;
        default: *fa = 1; *fb = 1 - sa; return;
    }
}

// Blends the source with the destination and composites it onto the destination, for one operator. Whether the blend
// mode is Normal is a compile-time parameter of the loop.
template <PrDrawComposite Op, bool Normal>
inline void compositeWith(const float* s, const float* d, PrDrawBlendMode blend, float* out) {
    float source[4] = {s[0], s[1], s[2], s[3]};
    if (!Normal && s[3] > 0) {
        float cs[3];
        float cb[3];
        float inverse = 1 / s[3];
        float inverseB = d[3] > 0 ? 1 / d[3] : 0;
        for (uint32_t i = 0; i < 3; ++i) {
            cs[i] = s[i] * inverse;
            cb[i] = d[i] * inverseB;
        }
        float mixed[3];
        blendColor(blend, cb, cs, mixed);
        for (uint32_t i = 0; i < 3; ++i)
            source[i] = ((1 - d[3]) * cs[i] + d[3] * clampF32(mixed[i], 0, 1)) * s[3];
    }
    if constexpr (Op == PrDrawComposite_PlusLighter) {
        for (uint32_t i = 0; i < 4; ++i)
            out[i] = minF32(1, source[i] + d[i]);
    } else if constexpr (Op == PrDrawComposite_PlusDarker) {
        float alpha = minF32(1, source[3] + d[3]);
        for (uint32_t i = 0; i < 3; ++i)
            out[i] = maxF32(0, alpha - (source[3] - source[i]) - (d[3] - d[i]));
        out[3] = alpha;
    } else {
        float fa = 0;
        float fb = 0;
        factors(Op, source[3], d[3], &fa, &fb);
        for (uint32_t i = 0; i < 4; ++i)
            out[i] = source[i] * fa + d[i] * fb;
    }
}

template <PrDrawComposite Op, bool Normal>
void compositeLoop(uint8_t* p, PrPixelFormat format, const float* source,
                   uint32_t sourceStride, const uint8_t* PR_NULLABLE shape,
                   const uint8_t* PR_NULLABLE clip, uint32_t count, PrDrawBlendMode blend,
                   bool knockout, bool layer) {
    // Where a layer is transparent, these operators change the pixel, and the others leave it unchanged.
    constexpr bool kReachesOutside = Op == PrDrawComposite_Clear || Op == PrDrawComposite_Copy
                                     || Op == PrDrawComposite_SourceIn || Op == PrDrawComposite_SourceOut
                                     || Op == PrDrawComposite_DestinationIn
                                     || Op == PrDrawComposite_DestinationAtop;
    const float transparent[4] = {0, 0, 0, 0};
    const uint32_t bytes = cpuPixelBytes(format);
    for (uint32_t i = 0; i < count; ++i, p += bytes, source += sourceStride) {
        uint32_t shapeAt = shape != nullptr ? shape[i] : 255;
        uint32_t clipAt = clip != nullptr ? clip[i] : 255;
        if (clipAt == 0 || (shapeAt == 0 && !(layer && kReachesOutside)))
            continue;
        float s = float(shapeAt) * kInverse255;
        float c = float(clipAt) * kInverse255;
        float d[4];
        cpuReadPixel(p, format, d);
        float result[4];
        if (knockout) {
            compositeWith<Op, Normal>(source, transparent, blend, result);
            float mix = s * c;
            for (uint32_t k = 0; k < 4; ++k)
                result[k] = d[k] + (result[k] - d[k]) * mix;
        } else if (layer) {
            float scaled[4] = {source[0] * s, source[1] * s, source[2] * s, source[3] * s};
            compositeWith<Op, Normal>(scaled, d, blend, result);
            for (uint32_t k = 0; k < 4; ++k)
                result[k] = d[k] + (result[k] - d[k]) * c;
        } else {
            compositeWith<Op, Normal>(source, d, blend, result);
            float mix = s * c;
            for (uint32_t k = 0; k < 4; ++k)
                result[k] = d[k] + (result[k] - d[k]) * mix;
        }
        cpuWritePixel(p, format, result);
    }
}

// Returns the product of two coverages in 255ths. A null coverage is 255 throughout.
uint32_t coverageProduct(const uint8_t* PR_NULLABLE shape, const uint8_t* PR_NULLABLE clip,
                         uint32_t i) {
    uint32_t s = shape != nullptr ? shape[i] : 255;
    return clip != nullptr ? cpuDivide255(s * clip[i]) : s;
}

template <PrDrawComposite Op>
struct OperatorOf {
    static constexpr PrDrawComposite kOp = Op;
};

// Calls `f` with the OperatorOf the operator, so that each operator's code is compiled once.
template <typename F>
void withOperator(PrDrawComposite op, F f) {
    switch (op) {
        case PrDrawComposite_Clear:
            f(OperatorOf<PrDrawComposite_Clear>{});
            return;
        case PrDrawComposite_Copy:
            f(OperatorOf<PrDrawComposite_Copy>{});
            return;
        case PrDrawComposite_SourceIn:
            f(OperatorOf<PrDrawComposite_SourceIn>{});
            return;
        case PrDrawComposite_SourceOut:
            f(OperatorOf<PrDrawComposite_SourceOut>{});
            return;
        case PrDrawComposite_SourceAtop:
            f(OperatorOf<PrDrawComposite_SourceAtop>{});
            return;
        case PrDrawComposite_DestinationOver:
            f(OperatorOf<PrDrawComposite_DestinationOver>{});
            return;
        case PrDrawComposite_DestinationIn:
            f(OperatorOf<PrDrawComposite_DestinationIn>{});
            return;
        case PrDrawComposite_DestinationOut:
            f(OperatorOf<PrDrawComposite_DestinationOut>{});
            return;
        case PrDrawComposite_DestinationAtop:
            f(OperatorOf<PrDrawComposite_DestinationAtop>{});
            return;
        case PrDrawComposite_Xor:
            f(OperatorOf<PrDrawComposite_Xor>{});
            return;
        case PrDrawComposite_PlusLighter:
            f(OperatorOf<PrDrawComposite_PlusLighter>{});
            return;
        case PrDrawComposite_PlusDarker:
            f(OperatorOf<PrDrawComposite_PlusDarker>{});
            return;
        default:
            f(OperatorOf<PrDrawComposite_SourceOver>{});
            return;
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Textures

void releaseTexture(CpuTexture* texture) {
    for (uint32_t i = 0; i < texture->levels.count; ++i)
        prMemoryFree(texture->levels.data[i].data);
    texture->levels.reset();
    prMemoryFree(texture);
}

CpuTexture* newTexture(CpuTextureCache& cache, uint64_t key, bool fromList) {
    CpuTexture* texture = allocZeroed<CpuTexture>();
    texture->key = key;
    texture->fromList = fromList;
    texture->lastUsed = cache.render;
    cache.textures.push(texture);
    return texture;
}

// Converts an image's pixels to a level of premultiplied RGBA: half floats for an image in half floats, and otherwise 8
// bits, clamped.
void convertImage(const PrDrawImage* image, CpuTextureLevel& level) {
    uint32_t width = prDrawImageExtent(image).x;
    uint32_t height = prDrawImageExtent(image).y;
    PrPixelFormat format = prDrawImageFormat(image);
    PrDrawAlpha alpha = prDrawImageAlpha(image);
    const PrPixelView source = prDrawImagePixels(image);
    const uint8_t* pixels = source.data;
    size_t stride = source.stride;
    level.width = width;
    level.height = height;
    level.format = format == PrPixelFormat_RgbaF16 ? PrPixelFormat_RgbaF16 : PrPixelFormat_Rgba8;
    const uint32_t bytes = cpuPixelBytes(level.format);
    level.data = static_cast<uint8_t*>(prMemoryAlloc(size_t(width) * height * bytes));
    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t* row = pixels + y * stride;
        uint8_t* out = level.data + size_t(y) * width * bytes;
        for (uint32_t x = 0; x < width; ++x) {
            float c[4];
            if (format == PrPixelFormat_RgbaF16) {
                PrFloat16 stored[4];
                memCopy(stored, row + x * 8, sizeof(stored));
                for (uint32_t i = 0; i < 4; ++i)
                    c[i] = prFloat16ToFloat(stored[i]);
            } else {
                bool bgra = format == PrPixelFormat_Bgra8;
                const uint8_t* p = row + x * 4;
                c[0] = float(p[bgra ? 2 : 0]) / 255;
                c[1] = float(p[1]) / 255;
                c[2] = float(p[bgra ? 0 : 2]) / 255;
                c[3] = float(p[3]) / 255;
            }
            if (alpha == PrDrawAlpha_Opaque)
                c[3] = 1;
            c[3] = clampF32(c[3], 0, 1);
            bool half = level.format == PrPixelFormat_RgbaF16;
            if (alpha != PrDrawAlpha_Premultiplied) {
                for (uint32_t i = 0; i < 3; ++i)
                    c[i] = (half ? c[i] : clampF32(c[i], 0, 1)) * c[3];
            }
            cpuWritePixel(out + x * bytes, level.format, c);
        }
    }
}

// Returns the texel at (x, y), or transparent if either index is -1.
void texel(const CpuTextureLevel& level, int32_t x, int32_t y, float out[4]) {
    if (x < 0 || y < 0) {
        out[0] = out[1] = out[2] = out[3] = 0;
        return;
    }
    cpuReadPixel(level.data + (size_t(y) * level.width + uint32_t(x)) * cpuPixelBytes(level.format), level.format, out);
}

// How a sampler maps to texels: from the source's origin, repeating as the tile modes specify. A pixel's footprint: the
// steps from its center to the centers of its neighbors to the right and below.
struct Footprint {
    PrVec2 across;
    PrVec2 down;
};

struct Sampler {
    const CpuTexture* texture;
    float originX;              // the source's texel origin
    float originY;
    float width;                // the source's size in texels
    float height;
    float periodX;              // Repeat, Reflect: the distance from one tile's start to the next's, in texels
    float periodY;
    PrDrawExtend modeX;
    PrDrawExtend modeY;
    PrDrawSampling sampling;
};

// Returns the texel index along one axis for a tap `k` texels from the source's origin, or -1 if it hits no texel. For
// Repeat and Reflect, `k` is from the first copy (nearCopy). A tap past the copy reads the neighboring copy if the
// copies abut, and nothing if a gap separates them. Spaced copies therefore fade into their gaps.
int32_t tapIndex(float k, float origin, float size, float period, PrDrawExtend mode,
                 uint32_t levelSize) {
    float position = k + 0.5f;
    switch (mode) {
        case PrDrawExtend_None:
            if (position < 0 || position >= size)
                return -1;
            break;
        case PrDrawExtend_Repeat:
        case PrDrawExtend_Reflect: {
            float tile = floorF32(position / period);
            position -= tile * period;
            if (position >= size || (tile != 0 && period > size))
                return -1;
            if (mode == PrDrawExtend_Reflect && fmodF32(absF32(tile), 2) == 1)
                position = size - position;
            break;
        }
        default:
            position = clampF32(position, 0.5f, maxF32(size - 0.5f, 0.5f));
            break;
    }
    int32_t index = int32_t(floorF32(origin + position));
    int32_t first = int32_t(floorF32(origin));
    int32_t last = int32_t(ceilF32(origin + size)) - 1;
    index = index < first ? first : index > last ? last : index;
    index = index < 0 ? 0 : index >= int32_t(levelSize) ? int32_t(levelSize) - 1 : index;
    return index;
}

// Returns `k`, a distance along an axis that repeats from the first tile's start, moved into the first copy from the
// copy that holds it. A point in a reflected copy is mirrored. Half of each gap belongs to the copy on either side of
// it. A filter then places its taps from the copy's own texels. Without the move, a period that is not a whole number
// of texels would shift the taps of every later copy. A tap past the copy's edges still reads the neighboring copies.
float nearCopy(float k, float size, float period, PrDrawExtend mode) {
    if (mode != PrDrawExtend_Repeat && mode != PrDrawExtend_Reflect)
        return k;
    float copy = floorF32((k + (period - size) / 2) / period);
    k -= copy * period;
    return mode == PrDrawExtend_Reflect && fmodF32(absF32(copy), 2) == 1 ? size - k : k;
}

// Moves `k`, a distance along an axis from the first tile's start, into the copy that holds it, and makes the axis's
// mode Pad. It serves tiles whose copies are filtered on their own (PrDrawTileFlag_Clamped). Returns false if `k` falls
// between copies. An axis that does not repeat is left as it is.
bool intoCopy(float& k, float size, float period, PrDrawExtend& mode) {
    if (mode != PrDrawExtend_Repeat && mode != PrDrawExtend_Reflect)
        return true;
    float copy = floorF32(k / period);
    k -= copy * period;
    // At a copy's start, the quotient can round down to the copy before, which leaves `k` a whole period into it.
    if (k >= period) {
        k -= period;
        copy += 1;
    }
    if (k >= size)
        return false;
    if (mode == PrDrawExtend_Reflect && fmodF32(absF32(copy), 2) == 1)
        k = size - k;
    mode = PrDrawExtend_Pad;
    return true;
}

int32_t tapX(const Sampler& s, const CpuTextureLevel& level, float scale, float k) {
    return tapIndex(k, s.originX * scale, s.width * scale, s.periodX * scale, s.modeX,
                    level.width);
}

int32_t tapY(const Sampler& s, const CpuTextureLevel& level, float scale, float k) {
    return tapIndex(k, s.originY * scale, s.height * scale, s.periodY * scale, s.modeY,
                    level.height);
}

// Catmull-Rom weights for the four taps around a fraction.
void cubicWeights(float t, float w[4]) {
    float t2 = t * t;
    float t3 = t2 * t;
    w[0] = (-t3 + 2 * t2 - t) / 2;
    w[1] = (3 * t3 - 5 * t2 + 2) / 2;
    w[2] = (-3 * t3 + 4 * t2 + t) / 2;
    w[3] = (t3 - t2) / 2;
}

// Samples one level at (u, v), in level-0 texels from the source's origin.
void sampleLevel(const Sampler& s, uint32_t levelIndex, PrDrawSampling sampling, float u, float v,
                 float out[4]) {
    const CpuTextureLevel& level = s.texture->levels.data[levelIndex];
    float scale = 1.0f / float(1u << levelIndex);
    u *= scale;
    v *= scale;
    if (sampling == PrDrawSampling_Nearest) {
        texel(level, tapX(s, level, scale, floorF32(u)), tapY(s, level, scale, floorF32(v)), out);
        return;
    }
    float fu = u - 0.5f;
    float fv = v - 0.5f;
    float x0 = floorF32(fu);
    float y0 = floorF32(fv);
    float tx = fu - x0;
    float ty = fv - y0;
    out[0] = out[1] = out[2] = out[3] = 0;
    if (sampling == PrDrawSampling_Cubic) {
        float wx[4];
        float wy[4];
        cubicWeights(tx, wx);
        cubicWeights(ty, wy);
        int32_t xs[4];
        int32_t ys[4];
        for (int32_t i = 0; i < 4; ++i) {
            xs[i] = tapX(s, level, scale, x0 + float(i - 1));
            ys[i] = tapY(s, level, scale, y0 + float(i - 1));
        }
        for (int32_t j = 0; j < 4; ++j) {
            for (int32_t i = 0; i < 4; ++i) {
                float c[4];
                texel(level, xs[i], ys[j], c);
                float w = wx[i] * wy[j];
                for (uint32_t k = 0; k < 4; ++k)
                    out[k] += c[k] * w;
            }
        }
        out[3] = clampF32(out[3], 0, 1);
        for (uint32_t k = 0; k < 3; ++k)
            out[k] = clampF32(out[k], 0, out[3]);
        return;
    }
    int32_t left = tapX(s, level, scale, x0);
    int32_t right = tapX(s, level, scale, x0 + 1);
    int32_t top = tapY(s, level, scale, y0);
    int32_t bottom = tapY(s, level, scale, y0 + 1);
    float c00[4];
    float c10[4];
    float c01[4];
    float c11[4];
    texel(level, left, top, c00);
    texel(level, right, top, c10);
    texel(level, left, bottom, c01);
    texel(level, right, bottom, c11);
    for (uint32_t k = 0; k < 4; ++k) {
        float upper = c00[k] + (c10[k] - c00[k]) * tx;
        float lower = c01[k] + (c11[k] - c01[k]) * tx;
        out[k] = upper + (lower - upper) * ty;
    }
}

// The most taps along the major axis of a pixel's footprint in anisotropic sampling.
constexpr uint32_t kMaxAnisotropy = 16;

// Samples at (u, v), in level-0 texels, between the two levels that `lod` falls between.
void sampleTrilinear(const Sampler& s, float u, float v, float lod, float out[4]) {
    uint32_t low = uint32_t(lod);
    float t = lod - float(low);
    sampleLevel(s, low, PrDrawSampling_Linear, u, v, out);
    if (t > 0 && low + 1 < s.texture->levels.count) {
        float high[4];
        sampleLevel(s, low + 1, PrDrawSampling_Linear, u, v, high);
        for (uint32_t k = 0; k < 4; ++k)
            out[k] += (high[k] - out[k]) * t;
    }
}

// Samples at (u, v), in level-0 texels, for a pixel whose neighbors to the right and below are `across` and `down`
// texels away. Mipmap sampling is anisotropic. Its level comes from the footprint's minor axis, and up to
// kMaxAnisotropy taps spread along the major axis cover the rest, so a footprint stretched along one axis stays sharp
// along the other.
void sample(const Sampler& s, float u, float v, PrVec2 across, PrVec2 down, float out[4]) {
    float a = sqrtF32(across.x * across.x + across.y * across.y);
    float b = sqrtF32(down.x * down.x + down.y * down.y);
    float major = maxF32(a, b);
    float minor = minF32(a, b);
    if (s.sampling != PrDrawSampling_Mipmap || s.texture->levels.count < 2 || !(major > 1)) {
        PrDrawSampling sampling =
            s.sampling == PrDrawSampling_Mipmap ? PrDrawSampling_Linear : s.sampling;
        sampleLevel(s, 0, sampling, u, v, out);
        return;
    }
    PrVec2 axis = a >= b ? across : down;
    float ratio = minor > 0 ? ceilF32(major / minor) : float(kMaxAnisotropy);
    uint32_t taps = uint32_t(clampF32(ratio, 1, float(kMaxAnisotropy)));
    float lod = clampF32(log2F32(maxF32(major / float(taps), minor)), 0, float(s.texture->levels.count - 1));
    if (taps == 1) {
        sampleTrilinear(s, u, v, lod, out);
        return;
    }
    out[0] = out[1] = out[2] = out[3] = 0;
    for (uint32_t i = 0; i < taps; ++i) {
        float t = (float(i) + 0.5f) / float(taps) - 0.5f;
        float tap[4];
        sampleTrilinear(s, u + axis.x * t, v + axis.y * t, lod, tap);
        for (uint32_t k = 0; k < 4; ++k)
            out[k] += tap[k] / float(taps);
    }
}

// Computes the point in the unit square along a stroke that its centerline assigns to a device point. Returns false if
// the stroke does not reach the point.
bool strokePoint(const CpuPlacement& placement, float x, float y, PrVec2* out) {
    PrPathMeasureNearest nearest{};
    if (!prPathMeasureNearest(placement.measure, PrVec2{x, y}, placement.reach, &nearest))
        return false;
    PrPathMeasureContour contour = prPathMeasureContour(placement.measure, nearest.contour);
    float along = contour.length > 0 ? (nearest.distance - contour.start) / contour.length : 0;
    PrPathStrokeWidth width = prPathStrokeWidthAt(placement.stroke, along);
    float left = width.left * placement.strokeScale;
    float right = width.right * placement.strokeScale;
    float across = left + right > 0 ? (nearest.offset + left) / (left + right) : 0.5f;
    *out = placement.mapping == PrDrawStrokeMapping_Along ? PrVec2{along, across}
                                                          : PrVec2{across, along};
    return true;
}

// Computes the points of the stage's space for `count` pixel centers from (x, y) rightward. A point whose x is NaN is
// one that the placement does not reach. If `footprints` is not null, it receives each pixel's footprint in the stage's
// space: zero along a stroke, where it is not measured.
void placeSpan(const CpuPlacement& placement, int32_t x, int32_t y, uint32_t count, PrVec2* points,
               Footprint* PR_NULLABLE footprints) {
    const PrAffine2& m = placement.transform;
    float cy = float(y) + 0.5f;
    for (uint32_t i = 0; i < count; ++i) {
        float cx = float(x + int32_t(i)) + 0.5f;
        PrVec2 at{cx, cy};
        // The steps to the neighboring pixels in the space that the transform maps from.
        PrVec2 across{1, 0};
        PrVec2 down{0, 1};
        bool reached = true;
        switch (placement.kind) {
            case PlacementKind_Projective: {
                reached = prMatrix3MapPoint(&placement.unproject, at, &at);
                if (reached && footprints != nullptr) {
                    PrVec2 right{};
                    PrVec2 below{};
                    reached = prMatrix3MapPoint(&placement.unproject, PrVec2{cx + 1, cy}, &right)
                              && prMatrix3MapPoint(&placement.unproject, PrVec2{cx, cy + 1}, &below);
                    across = right - at;
                    down = below - at;
                }
                break;
            }
            case PlacementKind_Stroke:
                reached = strokePoint(placement, cx, cy, &at);
                across = down = PrVec2{0, 0};
                break;
            default:
                break;
        }
        points[i] = reached ? prAffine2MapPoint(m, at) : PrVec2{kNanF32, kNanF32};
        if (footprints != nullptr) {
            footprints[i] = Footprint{{m.a * across.x + m.c * across.y, m.b * across.x + m.d * across.y},
                                      {m.a * down.x + m.c * down.y, m.b * down.x + m.d * down.y}};
        }
    }
}

// Writes the colors of a border's sides at the points, which `placement` places in the border's space. Across the line
// between two sides, the colors blend by the area of the pixel on either side of it (prDrawHalfPlaneArea).
void sidesSpan(const CpuSource& source, const CpuPlacement& placement, const PrVec2* points, uint32_t count,
               float* out) {
    const PrDrawBorderSides& s = *source.sides;
    const PrAffine2& m = placement.transform;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t corner = prDrawBorderSidesCorner(&s, points[i]);
        const float* line = s.lines[corner];
        float d = line[0] * points[i].x + line[1] * points[i].y + line[2];
        PrVec2 g{line[0] * m.a + line[1] * m.b, line[0] * m.c + line[1] * m.d};
        // The weight is a coverage, which is in 255ths here as every coverage is.
        float after = floorF32(prDrawHalfPlaneArea(d, g) * 255 + 0.5f) / 255;
        const PrColor& a = s.colors[corner][0];
        const PrColor& b = s.colors[corner][1];
        float* o = out + i * 4;
        o[0] = a.r + (b.r - a.r) * after;
        o[1] = a.g + (b.g - a.g) * after;
        o[2] = a.b + (b.b - a.b) * after;
        o[3] = a.a + (b.a - a.a) * after;
    }
}

void gradientSpan(const CpuSource& source, const PrVec2* points, uint32_t count, float* out) {
    uint32_t piece = 0;
    for (uint32_t i = 0; i < count; ++i) {
        float* o = out + i * 4;
        float t = 0;
        if (isNanF32(points[i].x) || !prDrawGradientParameter(source.brush, points[i], &t)
            || !prDrawGradientExtend(source.brush->extend, &t)) {
            o[0] = o[1] = o[2] = o[3] = 0;
            continue;
        }
        piece = prDrawRampPieceAt(source.rampPieces, t, piece);
        PrColor c = prDrawRampColor(&source.rampPieces[piece], source.ramp, t);
        o[0] = c.r;
        o[1] = c.g;
        o[2] = c.b;
        o[3] = c.a;
    }
}

// Returns the piece that holds a point. A piece's left and top edges are inside it and its right and bottom edges are
// outside, so that pieces that share an edge do not both hold the point. If no piece holds the point, returns the
// nearest piece by the sum of the distances along each axis. `hint` is the last piece found, which the next pixel in a
// row most likely falls in too.
uint32_t pieceAt(const CpuSource& source, PrVec2 p, uint32_t hint) {
    auto holds = [&](uint32_t i) {
        const PrRect& d = source.pieces[i].destination;
        return p.x >= d.left && p.x < d.right && p.y >= d.top && p.y < d.bottom;
    };
    if (hint < source.pieceCount && holds(hint))
        return hint;
    uint32_t best = 0;
    float nearest = kF32Max;
    for (uint32_t i = 0; i < source.pieceCount; ++i) {
        if (holds(i))
            return i;
        const PrRect& d = source.pieces[i].destination;
        float dx = maxF32(maxF32(d.left - p.x, p.x - d.right), 0);
        float dy = maxF32(maxF32(d.top - p.y, p.y - d.bottom), 0);
        if (dx + dy < nearest) {
            nearest = dx + dy;
            best = i;
        }
    }
    return best;
}

// Writes a texture's colors at the points. `footprints` are the values that placeSpan computes, or null if the sampling
// uses no mip level.
void textureSpan(const CpuSource& source, const PrVec2* points, const Footprint* PR_NULLABLE footprints, uint32_t count,
                 float* out) {
    if (source.pieces != nullptr) {
        // Each piece maps its own texels onto its destination.
        Sampler s{source.texture, 0, 0, 0, 0, 0, 0, PrDrawExtend_Pad, PrDrawExtend_Pad,
                  source.sampling};
        uint32_t piece = 0;
        for (uint32_t i = 0; i < count; ++i) {
            float* o = out + i * 4;
            PrVec2 p = points[i];
            if (isNanF32(p.x)) {
                o[0] = o[1] = o[2] = o[3] = 0;
                continue;
            }
            piece = pieceAt(source, p, piece);
            const PrDrawImageRect& r = source.pieces[piece];
            s.originX = r.source.left;
            s.originY = r.source.top;
            s.width = prRectWidth(r.source);
            s.height = prRectHeight(r.source);
            float sx = s.width / prRectWidth(r.destination);
            float sy = s.height / prRectHeight(r.destination);
            Footprint f = footprints != nullptr ? footprints[i] : Footprint{};
            sample(s, (p.x - r.destination.left) * sx, (p.y - r.destination.top) * sy,
                   PrVec2{f.across.x * sx, f.across.y * sy}, PrVec2{f.down.x * sx, f.down.y * sy}, o);
        }
        return;
    }
    const PrDrawTiles& tiles = source.tiles;
    float tileWidth = prRectWidth(tiles.tile);
    float tileHeight = prRectHeight(tiles.tile);
    float width = prRectWidth(source.texels);
    float height = prRectHeight(source.texels);
    float scaleX = width / tileWidth;
    float scaleY = height / tileHeight;
    Sampler s{source.texture,
              source.texels.left,
              source.texels.top,
              width,
              height,
              (tileWidth + tiles.spacing.x) * scaleX,
              (tileHeight + tiles.spacing.y) * scaleY,
              tiles.x,
              tiles.y,
              source.sampling};
    bool clamped = (tiles.flags & PrDrawTileFlag_Clamped) != 0;
    float periodX = tileWidth + tiles.spacing.x;
    float periodY = tileHeight + tiles.spacing.y;
    for (uint32_t i = 0; i < count; ++i) {
        float* o = out + i * 4;
        PrVec2 p = points[i];
        if (isNanF32(p.x)) {
            o[0] = o[1] = o[2] = o[3] = 0;
            continue;
        }
        Footprint f = footprints != nullptr ? footprints[i] : Footprint{};
        // The point moves between copies in brush space, before it is scaled to texels, so that a point in any copy
        // rounds to the same texel coordinates as the matching point in the first copy.
        float u = p.x - tiles.tile.left;
        float v = p.y - tiles.tile.top;
        Sampler at = s;
        if (clamped
            && !(intoCopy(u, tileWidth, periodX, at.modeX) && intoCopy(v, tileHeight, periodY, at.modeY))) {
            o[0] = o[1] = o[2] = o[3] = 0;
            continue;
        }
        u = nearCopy(u, tileWidth, periodX, at.modeX) * scaleX;
        v = nearCopy(v, tileHeight, periodY, at.modeY) * scaleY;
        sample(at, u, v, PrVec2{f.across.x * scaleX, f.across.y * scaleY}, PrVec2{f.down.x * scaleX, f.down.y * scaleY},
               o);
    }
}

} // namespace

// ---------------------------------------------------------------------------------------------------------------------
// Boxes and coverage

bool cpuIsRectPolyline(const PrPathPolyline& polyline) {
    if (polyline.contourCount != 1 || polyline.pointCount != 4)
        return false;
    const PrVec2* p = polyline.points;
    for (uint32_t i = 0; i < 4; ++i) {
        PrVec2 a = p[i];
        PrVec2 b = p[(i + 1) % 4];
        if (a.x != b.x && a.y != b.y)
            return false;
    }
    return true;
}

CpuCoverageRow cpuCoverageRow(const CpuCoverage& c, int32_t y) {
    if (y < c.box.top || y >= c.box.bottom || prIRectIsEmpty(c.box))
        return CpuCoverageRow{nullptr, 0, 0};
    const uint8_t* data =
        c.data != nullptr ? c.data + size_t(y - c.box.top) * c.stride : nullptr;
    return CpuCoverageRow{data, c.box.left, c.box.right};
}

uint8_t* cpuTargetRow(const CpuTarget& t, int32_t y) {
    return t.data + size_t(y - t.box.top) * t.stride;
}

uint8_t* cpuTargetPixel(const CpuTarget& t, int32_t x, int32_t y) {
    return cpuTargetRow(t, y) + size_t(x - t.box.left) * cpuPixelBytes(t.format);
}

void cpuReadPixel(const uint8_t* p, PrPixelFormat format, float* out) {
    if (format == PrPixelFormat_RgbaF16) {
        PrFloat16 stored[4];
        memCopy(stored, p, sizeof(stored));
        for (uint32_t i = 0; i < 4; ++i)
            out[i] = prFloat16ToFloat(stored[i]);
        return;
    }
    bool bgra = format == PrPixelFormat_Bgra8;
    out[0] = float(p[bgra ? 2 : 0]) * kInverse255;
    out[1] = float(p[1]) * kInverse255;
    out[2] = float(p[bgra ? 0 : 2]) * kInverse255;
    out[3] = float(p[3]) * kInverse255;
}

void cpuWritePixel(uint8_t* p, PrPixelFormat format, const float* rgba) {
    float alpha = clampF32(rgba[3], 0, 1);
    if (format == PrPixelFormat_RgbaF16) {
        PrFloat16 stored[4] = {prFloat16FromFloat(rgba[0]), prFloat16FromFloat(rgba[1]), prFloat16FromFloat(rgba[2]),
                               prFloat16FromFloat(alpha)};
        memCopy(p, stored, sizeof(stored));
        return;
    }
    bool bgra = format == PrPixelFormat_Bgra8;
    auto byteOf = [&](float v) { return uint8_t(clampF32(v, 0, alpha) * 255 + 0.5f); };
    p[bgra ? 2 : 0] = byteOf(rgba[0]);
    p[1] = byteOf(rgba[1]);
    p[bgra ? 0 : 2] = byteOf(rgba[2]);
    p[3] = uint8_t(alpha * 255 + 0.5f);
}

void cpuCopyTarget(const CpuTarget& from, const CpuTarget& to) {
    PR_ASSERT(from.format == to.format);
    PrIRect box = prIRectIntersect(from.box, to.box);
    for (int32_t y = box.top; y < box.bottom; ++y)
        memCopy(cpuTargetPixel(to, box.left, y), cpuTargetPixel(from, box.left, y),
                size_t(prIRectWidth(box)) * cpuPixelBytes(from.format));
}

// ---------------------------------------------------------------------------------------------------------------------
// Pixels

CpuPixel cpuPixelFromRgba(const float* rgba, PrPixelFormat format) {
    float alpha = clampF32(rgba[3], 0, 1);
    auto byteOf = [&](float v) { return uint32_t(clampF32(v, 0, alpha) * 255 + 0.5f); };
    uint32_t r = byteOf(rgba[0]);
    uint32_t g = byteOf(rgba[1]);
    uint32_t b = byteOf(rgba[2]);
    uint32_t a = uint32_t(alpha * 255 + 0.5f);
    return format == PrPixelFormat_Bgra8 ? CpuPixel{{b, g, r, a}} : CpuPixel{{r, g, b, a}};
}

void cpuStorePixel(uint8_t* p, CpuPixel color) {
    for (uint32_t i = 0; i < 4; ++i)
        p[i] = uint8_t(color.channel[i]);
}

void cpuScalePixel(uint8_t* p, PrPixelFormat format, uint32_t scale) {
    if (format != PrPixelFormat_RgbaF16) {
        for (uint32_t k = 0; k < 4; ++k)
            p[k] = uint8_t(cpuDivide255(p[k] * scale));
        return;
    }
    float c[4];
    cpuReadPixel(p, format, c);
    for (float& channel : c)
        channel *= float(scale) * kInverse255;
    cpuWritePixel(p, format, c);
}

void cpuSourceOver(uint8_t* p, CpuPixel color, uint32_t coverage) {
    uint32_t alpha = cpuDivide255(color.channel[3] * coverage);
    for (uint32_t i = 0; i < 4; ++i) {
        uint32_t source = cpuDivide255(color.channel[i] * coverage);
        p[i] = uint8_t(source + cpuDivide255(p[i] * (255 - alpha)));
    }
}

void cpuCompositeColors(const float* s, const float* d, PrDrawComposite op, PrDrawBlendMode blend,
                        float* out) {
    withOperator(op, [&](auto o) {
        if (blend == PrDrawBlendMode_Normal)
            compositeWith<decltype(o)::kOp, true>(s, d, blend, out);
        else
            compositeWith<decltype(o)::kOp, false>(s, d, blend, out);
    });
}

const uint8_t* PR_NULLABLE cpuCoverageSpan(const CpuCoverage& coverage, int32_t x, int32_t y,
                                           uint32_t count, uint8_t* scratch) {
    CpuCoverageRow row = cpuCoverageRow(coverage, y);
    int32_t end = x + int32_t(count);
    if (x >= row.left && end <= row.right)
        return row.data != nullptr ? row.data + (x - row.left) : nullptr;
    int32_t from = x > row.left ? x : row.left;
    int32_t to = end < row.right ? end : row.right;
    if (from >= to) {
        memZero(scratch, count);
        return scratch;
    }
    memZero(scratch, size_t(from - x));
    if (row.data != nullptr)
        memCopy(scratch + (from - x), row.data + (from - row.left), size_t(to - from));
    else
        memFill(scratch + (from - x), 255, size_t(to - from));
    memZero(scratch + (to - x), size_t(end - to));
    return scratch;
}

void cpuCompositeSpan(uint8_t* p, PrPixelFormat format, const float* source,
                      uint32_t sourceStride, const uint8_t* PR_NULLABLE shape,
                      const uint8_t* PR_NULLABLE clip, uint32_t count, PrDrawComposite op,
                      PrDrawBlendMode blend, bool knockout, bool layer) {
    if (op == PrDrawComposite_SourceOver && blend == PrDrawBlendMode_Normal && !knockout
        && format != PrPixelFormat_RgbaF16) {
        // In 255ths throughout.
        CpuPixel color = cpuPixelFromRgba(source, format);
        for (uint32_t i = 0; i < count; ++i, p += 4) {
            uint32_t c = coverageProduct(shape, clip, i);
            if (c == 0)
                continue;
            if (sourceStride != 0)
                color = cpuPixelFromRgba(source + i * sourceStride, format);
            if (color.channel[3] != 0)
                cpuSourceOver(p, color, c);
        }
        return;
    }
    withOperator(op, [&](auto o) {
        if (blend == PrDrawBlendMode_Normal)
            compositeLoop<decltype(o)::kOp, true>(p, format, source, sourceStride, shape, clip,
                                                  count, blend, knockout, layer);
        else
            compositeLoop<decltype(o)::kOp, false>(p, format, source, sourceStride, shape, clip,
                                                   count, blend, knockout, layer);
    });
}

void cpuCompositePixels(uint8_t* p, PrPixelFormat format, const uint8_t* source,
                        const uint8_t* PR_NULLABLE shape, const uint8_t* PR_NULLABLE clip,
                        uint32_t count, PrDrawComposite op, PrDrawBlendMode blend, bool knockout) {
    PR_ASSERT(count <= kShadeSpan);
    if (op == PrDrawComposite_SourceOver && blend == PrDrawBlendMode_Normal && !knockout
        && format != PrPixelFormat_RgbaF16) {
        for (uint32_t i = 0; i < count; ++i, p += 4, source += 4) {
            uint32_t c = coverageProduct(shape, clip, i);
            if (c != 0 && source[3] != 0)
                cpuSourceOver(p, CpuPixel{{source[0], source[1], source[2], source[3]}}, c);
        }
        return;
    }
    float colors[kShadeSpan * 4];
    for (uint32_t i = 0; i < count; ++i)
        cpuReadPixel(source + i * cpuPixelBytes(format), format, colors + i * 4);
    cpuCompositeSpan(p, format, colors, 4, shape, clip, count, op, blend, knockout, true);
}

// ---------------------------------------------------------------------------------------------------------------------
// Textures

CpuTexture* cpuTextureFromImage(CpuTextureCache& cache, const PrDrawImage* image) {
    uint64_t key = prDrawImageId(image);
    uint32_t found = cache.images.find(key, [&](uint32_t i) { return cache.textures.data[i]->key; });
    if (found != kHashMissing) {
        cache.textures.data[found]->lastUsed = cache.render;
        return cache.textures.data[found];
    }
    cache.images.insert(key, cache.textures.count);
    CpuTexture* texture = newTexture(cache, key, false);
    CpuTextureLevel level{};
    convertImage(image, level);
    texture->levels.push(level);
    return texture;
}

CpuTexture* cpuListTexture(CpuTextureCache& cache, uint64_t id, PrRect region, uint32_t width, uint32_t height,
                           PrPixelFormat format) {
    for (uint32_t i = 0; i < cache.textures.count; ++i) {
        CpuTexture* texture = cache.textures.data[i];
        const CpuTextureLevel& level = texture->levels.data[0];
        if (texture->fromList && texture->key == id && level.width == width && level.height == height
            && level.format == format && memEquals(&texture->region, &region, sizeof(PrRect))) {
            texture->lastUsed = cache.render;
            return texture;
        }
    }
    CpuTexture* texture = newTexture(cache, id, true);
    texture->region = region;
    uint8_t* data = static_cast<uint8_t*>(prMemoryAllocZeroed(size_t(width) * height * cpuPixelBytes(format)));
    CpuTextureLevel level{data, width, height, format};
    texture->levels.push(level);
    return texture;
}

void cpuPrepareMips(CpuTexture& texture, PrDrawSampling sampling) {
    if (sampling != PrDrawSampling_Mipmap || texture.levels.count != 1)
        return;
    while (true) {
        const CpuTextureLevel above = texture.levels.last();
        if (above.width == 1 && above.height == 1)
            break;
        CpuTextureLevel level{};
        level.width = above.width > 1 ? above.width / 2 : 1;
        level.height = above.height > 1 ? above.height / 2 : 1;
        level.format = above.format;
        const uint32_t bytes = cpuPixelBytes(level.format);
        level.data = static_cast<uint8_t*>(prMemoryAlloc(size_t(level.width) * level.height * bytes));
        prImageMipLevel(PrPixelView{above.data, size_t(above.width) * bytes, {above.width, above.height}},
                        PrPixels{level.data, size_t(level.width) * bytes, {level.width, level.height}}, level.format);
        texture.levels.push(level);
    }
}

void cpuBeginTextures(CpuTextureCache& cache) {
    ++cache.render;
}

void cpuEndTextures(CpuTextureCache& cache) {
    uint32_t kept = 0;
    for (uint32_t i = 0; i < cache.textures.count; ++i) {
        CpuTexture* texture = cache.textures.data[i];
        if (texture->lastUsed == cache.render)
            cache.textures.data[kept++] = texture;
        else
            releaseTexture(texture);
    }
    cache.textures.count = kept;
    cache.images.index.clear();
    for (uint32_t i = 0; i < kept; ++i) {
        if (!cache.textures.data[i]->fromList)
            cache.images.insert(cache.textures.data[i]->key, i);
    }
}

void cpuDestroyTextures(CpuTextureCache& cache) {
    for (uint32_t i = 0; i < cache.textures.count; ++i)
        releaseTexture(cache.textures.data[i]);
    cache.textures.reset();
    cache.images.reset();
}

// ---------------------------------------------------------------------------------------------------------------------
// Paint

void cpuShade(const CpuPaint& paint, int32_t x, int32_t y, uint32_t count, float* out) {
    PR_ASSERT(count <= kShadeSpan);
    for (uint32_t i = 0; i < count; ++i)
        memCopy(out + i * 4, paint.color, sizeof(paint.color));
    PrVec2 points[kShadeSpan];
    Footprint footprints[kShadeSpan];
    float colors[kShadeSpan * 4];
    for (uint32_t k = 0; k < paint.stageCount; ++k) {
        const CpuStage& stage = paint.stages[k];
        switch (stage.source.kind) {
            case SourceKind_Gradient:
                placeSpan(stage.placement, x, y, count, points, nullptr);
                gradientSpan(stage.source, points, count, colors);
                break;
            case SourceKind_Sides:
                placeSpan(stage.placement, x, y, count, points, nullptr);
                sidesSpan(stage.source, stage.placement, points, count, colors);
                break;
            default: {
                Footprint* mips = stage.source.sampling == PrDrawSampling_Mipmap ? footprints : nullptr;
                placeSpan(stage.placement, x, y, count, points, mips);
                textureSpan(stage.source, points, mips, count, colors);
                for (uint32_t i = 0; stage.source.limit > 0 && i < count; ++i) {
                    float* c = colors + i * 4;
                    if (!(c[3] > 0))
                        continue;
                    PrColor straight = prColorLimit(PrColor{c[0] / c[3], c[1] / c[3], c[2] / c[3], c[3]},
                                                    stage.source.limit);
                    c[0] = straight.r * c[3];
                    c[1] = straight.g * c[3];
                    c[2] = straight.b * c[3];
                }
                break;
            }
        }
        for (uint32_t i = 0; i < count * 4; ++i)
            out[i] *= colors[i];
    }
}

float cpuMaskValue(const PrDrawMask* mask, const float* rgba) {
    if (mask->mode == PrDrawMaskMode_Alpha)
        return rgba[3];
    if (mask->space != PrColorSpace_SrgbLinear)
        return kPrColorLuminanceRed * rgba[0] + kPrColorLuminanceGreen * rgba[1]
               + kPrColorLuminanceBlue * rgba[2];
    if (!(rgba[3] > 0))
        return 0;
    float linear[3];
    for (uint32_t i = 0; i < 3; ++i)
        linear[i] = prColorTransferDecode(PrColorTransfer_Srgb, clampF32(rgba[i] / rgba[3], 0, 1));
    return (kPrColorLuminanceRed * linear[0] + kPrColorLuminanceGreen * linear[1] + kPrColorLuminanceBlue * linear[2])
           * rgba[3];
}

} // namespace pr_impl
