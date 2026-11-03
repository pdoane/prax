// Filters for the CPU backend: the evaluation of filter graphs over a group's offscreen, and blurs.
//
// A graph is evaluated node by node into images of premultiplied RGBA floats over the offscreen's pixels. Each node
// computes in the color space that it names. The last node's image becomes the offscreen. Lengths in the group's space
// are scaled into pixels by the group's transform, which is a scale and a translation.
//
// A Gaussian with a small sigma is a convolution with its kernel. A Gaussian with a larger sigma uses three box blurs,
// which approximate it to within a few percent at a cost independent of the sigma.

#include "draw/backends/cpu/src/cpu_filter.h"

#include "pr/alloc.h"
#include "pr/color_space.h"
#include "pr/float.h"
#include "pr/mem.h"
#include "pr/raw_array.h"

namespace pr_impl {
namespace {

// The widest window that morphology uses, in pixels.
constexpr int32_t kMaxMorphology = 256;

// The number of blurred levels that a progressive blur mixes between.
constexpr uint32_t kProgressiveLevels = 5;

// The mask for indices into the noise lattice, and the shift that keeps points positive.
constexpr int32_t kLatticeMask = kPrImageNoiseLattice - 1;
constexpr int32_t kPerlinShift = 0x1000;

// An image of premultiplied RGBA over the offscreen's pixels.
struct Image {
    float* PR_NULLABLE data;
    bool owned;
};

struct Evaluation {
    const CpuFilterInputs* inputs;
    const CpuTarget* layer;
    uint32_t width;
    uint32_t height;
    float scaleX;               // pixels per unit of the group's space
    float scaleY;
    PrIRect region;         // the filter's region, in the offscreen's pixels relative to its origin
    PrRect graph;           // the filter's region in the group's space
    Image source;
    Image sourceAlpha;
    Image backdrop;
    RawArray<Image> results;
    RawArray<PrIRect> regions;     // the region of each result, in the offscreen's pixels relative to its origin
    RawArray<float> line;
    RawArray<float> scratch;
};

size_t pixelCount(const Evaluation& e) {
    return size_t(e.width) * e.height;
}

Image newImage(const Evaluation& e) {
    return Image{static_cast<float*>(prMemoryAllocZeroed(pixelCount(e) * 4 * sizeof(float))), true};
}

Image copyImage(const Evaluation& e, const Image& from) {
    Image image = newImage(e);
    prMemCopy(image.data, from.data, pixelCount(e) * 4 * sizeof(float));
    return image;
}

void release(Image& image) {
    if (image.owned && image.data != nullptr)
        prMemoryFree(image.data);
    image.data = nullptr;
}

// Returns a target's pixels as an image over the offscreen's box, transparent where the target has no pixels.
Image imageOf(const Evaluation& e, const CpuTarget& target) {
    Image image = newImage(e);
    const PrIRect& box = e.layer->box;
    PrIRect within = prIRectIntersect(box, target.box);
    for (int32_t y = within.top; y < within.bottom; ++y) {
        float* out = image.data + (size_t(y - box.top) * e.width + size_t(within.left - box.left)) * 4;
        for (int32_t x = within.left; x < within.right; ++x, out += 4)
            cpuReadPixel(cpuTargetPixel(target, x, y), target.format, out);
    }
    return image;
}

// Returns the pixels of a rectangle of the group's space.
PrIRect pixelsOf(const Evaluation& e, PrRect rect) {
    PrIRect device = prIRectRoundOut(prAffine2MapRect(e.inputs->transform, rect));
    const PrIRect& box = e.layer->box;
    return prIRectIntersect(PrIRect{
        device.left - box.left, device.top - box.top, device.right - box.left,
        device.bottom - box.top,
    }, PrIRect{0, 0, int32_t(e.width), int32_t(e.height)});
}

// Makes the image transparent outside the box.
void keepWithin(const Evaluation& e, Image& image, PrIRect keep) {
    for (uint32_t y = 0; y < e.height; ++y) {
        float* row = image.data + size_t(y) * e.width * 4;
        bool inside = int32_t(y) >= keep.top && int32_t(y) < keep.bottom;
        for (uint32_t x = 0; x < e.width; ++x) {
            if (inside && int32_t(x) >= keep.left && int32_t(x) < keep.right)
                continue;
            row[x * 4] = row[x * 4 + 1] = row[x * 4 + 2] = row[x * 4 + 3] = 0;
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Color spaces

// Converts premultiplied colors between sRGB's encoding and linear light, in place.
void convertSpace(const Evaluation& e, Image& image, bool linear) {
    size_t count = pixelCount(e);
    for (size_t i = 0; i < count; ++i) {
        float* p = image.data + i * 4;
        if (!(p[3] > 0))
            continue;
        for (uint32_t c = 0; c < 3; ++c) {
            float straight = p[c] / p[3];
            if (linear)
                p[c] = prColorTransferDecode(PrColorTransfer_Srgb, straight) * p[3];
            else
                p[c] = prColorTransferEncode(PrColorTransfer_Srgb, straight) * p[3];
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Area operations

int32_t edgeIndex(int32_t i, int32_t count, PrImageEdgeMode edge) {
    if (i >= 0 && i < count)
        return i;
    if (count <= 0)
        return -1;
    if (edge == PrImageEdgeMode_Duplicate)
        return i < 0 ? 0 : count - 1;
    if (edge == PrImageEdgeMode_Wrap)
        return ((i % count) + count) % count;
    if (edge == PrImageEdgeMode_Mirror) {
        int32_t period = count * 2;
        int32_t at = ((i % period) + period) % period;
        return at < count ? at : period - 1 - at;
    }
    return -1;
}

// Replaces the image outside `region` with what the edge mode reads there from the pixels inside it.
void extend(const Evaluation& e, Image& image, PrIRect region, PrImageEdgeMode edge) {
    int32_t w = region.right - region.left;
    int32_t h = region.bottom - region.top;
    for (int32_t y = 0; y < int32_t(e.height); ++y) {
        bool rowInside = y >= region.top && y < region.bottom;
        int32_t sy = w > 0 ? edgeIndex(y - region.top, h, edge) : -1;
        for (int32_t x = 0; x < int32_t(e.width); ++x) {
            if (rowInside && x >= region.left && x < region.right)
                continue;
            float* p = image.data + (size_t(y) * e.width + uint32_t(x)) * 4;
            int32_t sx = sy >= 0 ? edgeIndex(x - region.left, w, edge) : -1;
            if (sx < 0) {
                p[0] = p[1] = p[2] = p[3] = 0;
                continue;
            }
            prMemCopy(p, image.data + (size_t(region.top + sy) * e.width + uint32_t(region.left + sx)) * 4,
                4 * sizeof(float));
        }
    }
}

// Returns the edge mode that reads beyond the offscreen once an image is extended beyond its region.
PrImageEdgeMode beyondOffscreen(PrImageEdgeMode edge) {
    return edge == PrImageEdgeMode_None ? PrImageEdgeMode_None : PrImageEdgeMode_Duplicate;
}

// Blurs every channel of the image.
void blurImage(Evaluation& e, Image& image, float sigmaX, float sigmaY, PrImageEdgeMode edge) {
    PrPixels pixels{reinterpret_cast<uint8_t*>(image.data), size_t(e.width) * 4 * sizeof(float), {e.width, e.height}};
    prImageBlur(pixels, PrPixelFormat_RgbaF32, PrVec2{sigmaX, sigmaY}, edge);
}

// Replaces each value with the smallest or largest of its channel within `radius` pixels along one axis.
void morphLine(float* values, uint32_t count, size_t stride, int32_t radius, PrDrawMorphologyOp op, RawArray<float>& scratch) {
    scratch.clear();
    scratch.reserve(count);
    for (uint32_t i = 0; i < count; ++i)
        scratch.data[i] = values[i * stride];
    bool dilate = op == PrDrawMorphologyOp_Dilate;
    for (uint32_t i = 0; i < count; ++i) {
        float best = dilate ? 0 : 1;
        for (int32_t k = -radius; k <= radius; ++k) {
            int32_t at = int32_t(i) + k;
            float v = at >= 0 && at < int32_t(count) ? scratch.data[at] : 0;
            best = dilate ? prF32Max(best, v) : prF32Min(best, v);
        }
        values[i * stride] = best;
    }
}

void morphImage(Evaluation& e, Image& image, float radiusX, float radiusY, PrDrawMorphologyOp op) {
    int32_t rx = int32_t(prF32Min(prF32Floor(radiusX), float(kMaxMorphology)));
    int32_t ry = int32_t(prF32Min(prF32Floor(radiusY), float(kMaxMorphology)));
    for (uint32_t c = 0; c < 4; ++c) {
        if (rx > 0) {
            for (uint32_t y = 0; y < e.height; ++y)
                morphLine(image.data + size_t(y) * e.width * 4 + c, e.width, 4, rx, op, e.scratch);
        }
        if (ry > 0) {
            for (uint32_t x = 0; x < e.width; ++x)
                morphLine(image.data + size_t(x) * 4 + c, e.height, size_t(e.width) * 4, ry, op, e.scratch);
        }
    }
}

// Moves the image by (dx, dy) pixels, sampled between pixels, with `fill` alpha entering from outside the image.
Image shifted(const Evaluation& e, const Image& from, float dx, float dy) {
    Image image = newImage(e);
    int32_t ix = int32_t(prF32Floor(dx));
    int32_t iy = int32_t(prF32Floor(dy));
    float fx = dx - float(ix);
    float fy = dy - float(iy);
    auto at = [&](int32_t x, int32_t y, uint32_t c) {
        if (x < 0 || y < 0 || x >= int32_t(e.width) || y >= int32_t(e.height))
            return 0.0f;
        return from.data[(size_t(y) * e.width + uint32_t(x)) * 4 + c];
    };
    for (uint32_t y = 0; y < e.height; ++y) {
        for (uint32_t x = 0; x < e.width; ++x) {
            int32_t sx = int32_t(x) - ix;
            int32_t sy = int32_t(y) - iy;
            for (uint32_t c = 0; c < 4; ++c) {
                float a = at(sx, sy, c) * (1 - fx) + at(sx - 1, sy, c) * fx;
                float b = at(sx, sy - 1, c) * (1 - fx) + at(sx - 1, sy - 1, c) * fx;
                image.data[(size_t(y) * e.width + x) * 4 + c] = a * (1 - fy) + b * fy;
            }
        }
    }
    return image;
}

// Repeats the part of the image inside the tile across the whole image.
Image tiled(const Evaluation& e, const Image& from, PrIRect tile) {
    Image image = newImage(e);
    int32_t w = tile.right - tile.left;
    int32_t h = tile.bottom - tile.top;
    if (w <= 0 || h <= 0)
        return image;
    for (uint32_t y = 0; y < e.height; ++y) {
        int32_t ty = tile.top + ((int32_t(y) - tile.top) % h + h) % h;
        for (uint32_t x = 0; x < e.width; ++x) {
            int32_t tx = tile.left + ((int32_t(x) - tile.left) % w + w) % w;
            prMemCopy(image.data + (size_t(y) * e.width + x) * 4,
                from.data + (size_t(ty) * e.width + uint32_t(tx)) * 4, 4 * sizeof(float));
        }
    }
    return image;
}

// Replaces the image with its alpha alone, as black.
Image alphaOf(const Evaluation& e, const Image& from) {
    Image image = newImage(e);
    size_t count = pixelCount(e);
    for (size_t i = 0; i < count; ++i)
        image.data[i * 4 + 3] = from.data[i * 4 + 3];
    return image;
}

// Replaces the image with the inverse of its alpha, one minus the alpha, as black.
Image inverseAlphaOf(const Evaluation& e, const Image& from) {
    Image image = newImage(e);
    size_t count = pixelCount(e);
    for (size_t i = 0; i < count; ++i)
        image.data[i * 4 + 3] = 1 - from.data[i * 4 + 3];
    return image;
}

// Colors an alpha image with the straight color times each pixel's alpha, in linear light if `linear` is set. The color
// is clamped to [0, 1] unless the offscreen holds half floats.
void colorize(const Evaluation& e, Image& image, PrColor color, bool linear) {
    size_t count = pixelCount(e);
    float alpha = prF32Clamp(color.a, 0, 1);
    bool extended = e.layer->format == PrPixelFormat_RgbaF16;
    float r = extended ? color.r : prF32Clamp(color.r, 0, 1);
    float g = extended ? color.g : prF32Clamp(color.g, 0, 1);
    float b = extended ? color.b : prF32Clamp(color.b, 0, 1);
    if (linear) {
        r = prColorTransferDecode(PrColorTransfer_Srgb, r);
        g = prColorTransferDecode(PrColorTransfer_Srgb, g);
        b = prColorTransferDecode(PrColorTransfer_Srgb, b);
    }
    for (size_t i = 0; i < count; ++i) {
        float* p = image.data + i * 4;
        float a = prF32Clamp(p[3], 0, 1) * alpha;
        p[0] = r * a;
        p[1] = g * a;
        p[2] = b * a;
        p[3] = a;
    }
}

// Composites `top` source-over `bottom`, into `bottom`.
void over(const Evaluation& e, const Image& top, Image& bottom) {
    size_t count = pixelCount(e);
    for (size_t i = 0; i < count; ++i) {
        const float* s = top.data + i * 4;
        float* d = bottom.data + i * 4;
        for (uint32_t c = 0; c < 4; ++c)
            d[c] = s[c] + d[c] * (1 - s[3]);
    }
}

// Composites `top` source-atop `bottom`, into `bottom`. `top` is a color that `bottom` has not yet masked.
void atop(const Evaluation& e, const Image& top, Image& bottom) {
    size_t count = pixelCount(e);
    for (size_t i = 0; i < count; ++i) {
        const float* s = top.data + i * 4;
        float* d = bottom.data + i * 4;
        for (uint32_t c = 0; c < 3; ++c)
            d[c] = s[c] * d[3] + d[c] * (1 - s[3]);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Noise

struct Stitch {
    int32_t width;
    int32_t height;
    int32_t wrapX;
    int32_t wrapY;
};

float sCurve(float t) {
    return t * t * (3 - 2 * t);
}

// Returns the noise of one channel at a point of noise space. Stitching wraps the lattice points before they are masked
// into the lattice, since its bounds lie beyond the mask.
float noise2(const PrImageNoise& n, uint32_t channel, float vx, float vy, const Stitch* PR_NULLABLE s) {
    float t = vx + kPerlinShift;
    int32_t bx0 = int32_t(t);
    int32_t bx1 = bx0 + 1;
    float rx0 = t - float(int32_t(t));
    float rx1 = rx0 - 1;
    t = vy + kPerlinShift;
    int32_t by0 = int32_t(t);
    int32_t by1 = by0 + 1;
    float ry0 = t - float(int32_t(t));
    float ry1 = ry0 - 1;
    if (s != nullptr) {
        if (bx0 >= s->wrapX)
            bx0 -= s->width;
        if (bx1 >= s->wrapX)
            bx1 -= s->width;
        if (by0 >= s->wrapY)
            by0 -= s->height;
        if (by1 >= s->wrapY)
            by1 -= s->height;
    }
    bx0 &= kLatticeMask;
    bx1 &= kLatticeMask;
    by0 &= kLatticeMask;
    by1 &= kLatticeMask;
    int32_t i = n.lattice[bx0];
    int32_t j = n.lattice[bx1];
    int32_t b00 = n.lattice[i + by0];
    int32_t b10 = n.lattice[j + by0];
    int32_t b01 = n.lattice[i + by1];
    int32_t b11 = n.lattice[j + by1];
    float sx = sCurve(rx0);
    float sy = sCurve(ry0);
    const float* q = n.gradients[channel][b00];
    float u = rx0 * q[0] + ry0 * q[1];
    q = n.gradients[channel][b10];
    float v = rx1 * q[0] + ry0 * q[1];
    float a = u + sx * (v - u);
    q = n.gradients[channel][b01];
    u = rx0 * q[0] + ry1 * q[1];
    q = n.gradients[channel][b11];
    v = rx1 * q[0] + ry1 * q[1];
    float b = u + sx * (v - u);
    return a + sy * (b - a);
}

// Returns the turbulence of one channel at a point of the group's space, stitched over the tile.
float turbulence(const PrImageNoise& n, uint32_t channel, PrVec2 point, PrVec2 frequency,
        uint32_t octaves, PrDrawTurbulenceKind kind, bool stitching, PrRect tile) {
    Stitch stitch{};
    const Stitch* s = nullptr;
    if (stitching) {
        float tileWidth = prRectWidth(tile);
        float tileHeight = prRectHeight(tile);
        auto adjust = [](float f, float size) {
            if (f == 0 || size <= 0)
                return f;
            float low = prF32Floor(size * f) / size;
            float high = prF32Ceil(size * f) / size;
            return low > 0 && f / low < high / f ? low : high;
        };
        frequency.x = adjust(frequency.x, tileWidth);
        frequency.y = adjust(frequency.y, tileHeight);
        stitch.width = int32_t(tileWidth * frequency.x + 0.5f);
        stitch.wrapX = int32_t(tile.left * frequency.x + kPerlinShift + float(stitch.width));
        stitch.height = int32_t(tileHeight * frequency.y + 0.5f);
        stitch.wrapY = int32_t(tile.top * frequency.y + kPerlinShift + float(stitch.height));
        s = &stitch;
    }
    float sum = 0;
    float vx = point.x * frequency.x;
    float vy = point.y * frequency.y;
    float ratio = 1;
    for (uint32_t octave = 0; octave < octaves; ++octave) {
        float value = noise2(n, channel, vx, vy, s);
        sum += (kind == PrDrawTurbulenceKind_FractalNoise ? value : prF32Abs(value)) / ratio;
        vx *= 2;
        vy *= 2;
        ratio *= 2;
        if (s != nullptr) {
            stitch.width *= 2;
            stitch.wrapX = 2 * stitch.wrapX - kPerlinShift;
            stitch.height *= 2;
            stitch.wrapY = 2 * stitch.wrapY - kPerlinShift;
        }
    }
    return sum;
}

// ---------------------------------------------------------------------------------------------------------------------
// Nodes

// Returns the region of an input: the graph's for the group's inputs, and the region of the node that made a result.
PrIRect regionOf(const Evaluation& e, int32_t which) {
    return which >= 0 ? e.regions.data[which] : e.region;
}

const Image& inputOf(Evaluation& e, int32_t which) {
    switch (which) {
        case PrDrawFilterInput_Source:
            return e.source;
        case PrDrawFilterInput_SourceAlpha:
            if (e.sourceAlpha.data == nullptr)
                e.sourceAlpha = alphaOf(e, e.source);
            return e.sourceAlpha;
        case PrDrawFilterInput_Backdrop:
            if (e.backdrop.data == nullptr)
                e.backdrop = e.inputs->backdrop != nullptr ? imageOf(e, *e.inputs->backdrop) : newImage(e);
            return e.backdrop;
        default:
            return e.results.data[which];
    }
}

// Writes the straight channels of a premultiplied pixel.
void unpremultiply(const float* p, float out[4]) {
    out[3] = prF32Clamp(p[3], 0, 1);
    for (uint32_t c = 0; c < 3; ++c)
        out[c] = out[3] > 0 ? prF32Clamp(p[c] / out[3], 0, 1) : 0;
}

void premultiply(const float straight[4], float* out) {
    float a = prF32Clamp(straight[3], 0, 1);
    for (uint32_t c = 0; c < 3; ++c)
        out[c] = prF32Clamp(straight[c], 0, 1) * a;
    out[3] = a;
}

float transfer(const PrDrawTransfer& t, float c) {
    switch (t.kind) {
        case PrDrawTransferKind_Table: {
            if (t.valueCount == 1)
                return t.values[0];
            float at = c * float(t.valueCount - 1);
            uint32_t k = prF32Min(at, float(t.valueCount - 2)) < 0 ? 0 : uint32_t(prF32Min(at, float(t.valueCount - 2)));
            return t.values[k] + (at - float(k)) * (t.values[k + 1] - t.values[k]);
        }
        case PrDrawTransferKind_Discrete: {
            uint32_t k = uint32_t(c * float(t.valueCount));
            return t.values[k >= t.valueCount ? t.valueCount - 1 : k];
        }
        case PrDrawTransferKind_Linear:
            return t.slope * c + t.intercept;
        case PrDrawTransferKind_Gamma:
            return t.amplitude * prF32Pow(c, t.exponent) + t.offset;
        default:
            return c;
    }
}

void colorMatrix(const Evaluation& e, const float* matrix, Image& image) {
    size_t count = pixelCount(e);
    for (size_t i = 0; i < count; ++i) {
        float* p = image.data + i * 4;
        float s[4];
        unpremultiply(p, s);
        float out[4];
        for (uint32_t r = 0; r < 4; ++r) {
            const float* m = matrix + r * 5;
            out[r] = m[0] * s[0] + m[1] * s[1] + m[2] * s[2] + m[3] * s[3] + m[4];
        }
        premultiply(out, p);
    }
}

void componentTransfer(const Evaluation& e, const PrDrawTransfer* transfers, Image& image) {
    size_t count = pixelCount(e);
    for (size_t i = 0; i < count; ++i) {
        float* p = image.data + i * 4;
        float s[4];
        unpremultiply(p, s);
        for (uint32_t c = 0; c < 4; ++c)
            s[c] = transfer(transfers[c], s[c]);
        premultiply(s, p);
    }
}

void convolve(Evaluation& e, const PrDrawFilterNode& node, const Image& in, PrImageEdgeMode edge, Image& out) {
    const PrDrawFilterConvolution& k = node.convolution;
    float divisor = k.divisor;
    if (divisor == 0) {
        for (uint32_t i = 0; i < k.extent.x * k.extent.y; ++i)
            divisor += k.kernel[i];
        if (divisor == 0)
            divisor = 1;
    }
    int32_t w = int32_t(e.width);
    int32_t h = int32_t(e.height);
    for (int32_t y = 0; y < h; ++y) {
        for (int32_t x = 0; x < w; ++x) {
            float sum[4] = {0, 0, 0, 0};
            for (uint32_t j = 0; j < k.extent.y; ++j) {
                for (uint32_t i = 0; i < k.extent.x; ++i) {
                    int32_t sx = edgeIndex(x - int32_t(k.target.x) + int32_t(i), w, edge);
                    int32_t sy = edgeIndex(y - int32_t(k.target.y) + int32_t(j), h, edge);
                    if (sx < 0 || sy < 0)
                        continue;
                    // The kernel is applied rotated by a half turn, which is the definition of a convolution.
                    float weight = k.kernel[(k.extent.y - 1 - j) * k.extent.x + (k.extent.x - 1 - i)];
                    const float* p = in.data + (size_t(sy) * e.width + uint32_t(sx)) * 4;
                    if (k.preserveAlpha) {
                        float s[4];
                        unpremultiply(p, s);
                        for (uint32_t c = 0; c < 3; ++c)
                            sum[c] += s[c] * weight;
                    } else {
                        for (uint32_t c = 0; c < 4; ++c)
                            sum[c] += p[c] * weight;
                    }
                }
            }
            float* o = out.data + (size_t(y) * e.width + uint32_t(x)) * 4;
            const float* original = in.data + (size_t(y) * e.width + uint32_t(x)) * 4;
            if (k.preserveAlpha) {
                float result[4] = {
                    sum[0] / divisor + k.bias, sum[1] / divisor + k.bias,
                    sum[2] / divisor + k.bias, original[3],
                };
                premultiply(result, o);
            } else {
                o[3] = prF32Clamp(sum[3] / divisor + k.bias, 0, 1);
                for (uint32_t c = 0; c < 3; ++c)
                    o[c] = prF32Clamp(sum[c] / divisor + k.bias, 0, o[3]);
            }
        }
    }
}

void displace(const Evaluation& e, const PrDrawFilterNode& node, const Image& in, const Image& map, Image& out) {
    float scaleX = node.displacement.scale * e.scaleX;
    float scaleY = node.displacement.scale * e.scaleY;
    for (uint32_t y = 0; y < e.height; ++y) {
        for (uint32_t x = 0; x < e.width; ++x) {
            float m[4];
            unpremultiply(map.data + (size_t(y) * e.width + x) * 4, m);
            float sx = float(x) + scaleX * (m[node.displacement.x] - 0.5f);
            float sy = float(y) + scaleY * (m[node.displacement.y] - 0.5f);
            int32_t ix = int32_t(prF32Floor(sx + 0.5f));
            int32_t iy = int32_t(prF32Floor(sy + 0.5f));
            float* o = out.data + (size_t(y) * e.width + x) * 4;
            if (ix < 0 || iy < 0 || ix >= int32_t(e.width) || iy >= int32_t(e.height))
                continue;
            prMemCopy(o, in.data + (size_t(iy) * e.width + uint32_t(ix)) * 4, 4 * sizeof(float));
        }
    }
}

void light(const Evaluation& e, const PrDrawFilterNode& node, const Image& in, Image& out) {
    const PrDrawFilterLighting& l = node.lighting;
    const PrAffine2& m = e.inputs->transform;
    float scale = prF32Sqrt(e.scaleX * e.scaleY);
    float surface = l.surfaceScale;
    auto alphaAt = [&](int32_t x, int32_t y) {
        x = x < 0 ? 0 : x >= int32_t(e.width) ? int32_t(e.width) - 1 : x;
        y = y < 0 ? 0 : y >= int32_t(e.height) ? int32_t(e.height) - 1 : y;
        return in.data[(size_t(y) * e.width + uint32_t(x)) * 4 + 3];
    };
    // The light's position and target in pixels, with their depth scaled in the same way as lengths.
    float lx = m.a * l.light.position.x + m.e - float(e.layer->box.left);
    float ly = m.d * l.light.position.y + m.f - float(e.layer->box.top);
    float lz = l.light.position.z * scale;
    float tx = m.a * l.light.pointsAt.x + m.e - float(e.layer->box.left);
    float ty = m.d * l.light.pointsAt.y + m.f - float(e.layer->box.top);
    float tz = l.light.pointsAt.z * scale;
    float sLength = prF32Sqrt((tx - lx) * (tx - lx) + (ty - ly) * (ty - ly) + (tz - lz) * (tz - lz));
    float spot[3] = {(tx - lx) / sLength, (ty - ly) / sLength, (tz - lz) / sLength};
    float coneCos = l.light.coneAngle > 0 ? prF32Cos(l.light.coneAngle) : -1;
    for (int32_t y = 0; y < int32_t(e.height); ++y) {
        for (int32_t x = 0; x < int32_t(e.width); ++x) {
            float nx = -surface / 4 * ((alphaAt(x + 1, y - 1) + 2 * alphaAt(x + 1, y) + alphaAt(x + 1, y + 1)) -
                (alphaAt(x - 1, y - 1) + 2 * alphaAt(x - 1, y) + alphaAt(x - 1, y + 1)));
            float ny = -surface / 4 * ((alphaAt(x - 1, y + 1) + 2 * alphaAt(x, y + 1) + alphaAt(x + 1, y + 1)) -
                (alphaAt(x - 1, y - 1) + 2 * alphaAt(x, y - 1) + alphaAt(x + 1, y - 1)));
            float nLength = prF32Sqrt(nx * nx + ny * ny + 1);
            float n[3] = {nx / nLength, ny / nLength, 1 / nLength};
            float z = surface * alphaAt(x, y) * scale;
            float lv[3];
            PrColor lightColor = l.color;
            float color[3] = {lightColor.r, lightColor.g, lightColor.b};
            if (l.light.kind == PrDrawLightKind_Distant) {
                lv[0] = prF32Cos(l.light.azimuth) * prF32Cos(l.light.elevation);
                lv[1] = prF32Sin(l.light.azimuth) * prF32Cos(l.light.elevation);
                lv[2] = prF32Sin(l.light.elevation);
            } else {
                lv[0] = lx - float(x);
                lv[1] = ly - float(y);
                lv[2] = lz - z;
                float length = prF32Sqrt(lv[0] * lv[0] + lv[1] * lv[1] + lv[2] * lv[2]);
                for (float& c : lv)
                    c = length > 0 ? c / length : 0;
                if (l.light.kind == PrDrawLightKind_Spot) {
                    float along = -(lv[0] * spot[0] + lv[1] * spot[1] + lv[2] * spot[2]);
                    float factor = along < coneCos ? 0 : prF32Pow(prF32Max(along, 0), l.light.spotExponent);
                    for (float& c : color)
                        c *= factor;
                }
            }
            float result[4];
            if (l.kind == PrDrawLightingKind_Specular) {
                float h[3] = {lv[0], lv[1], lv[2] + 1};
                float hLength = prF32Sqrt(h[0] * h[0] + h[1] * h[1] + h[2] * h[2]);
                float dot = hLength > 0 ? (n[0] * h[0] + n[1] * h[1] + n[2] * h[2]) / hLength : 0;
                float factor = l.constant * prF32Pow(prF32Max(dot, 0), l.specularExponent);
                for (uint32_t c = 0; c < 3; ++c)
                    result[c] = prF32Clamp(color[c] * factor, 0, 1);
                result[3] = prF32Max(result[0], prF32Max(result[1], result[2]));
            } else {
                float dot = n[0] * lv[0] + n[1] * lv[1] + n[2] * lv[2];
                float factor = l.constant * prF32Max(dot, 0);
                for (uint32_t c = 0; c < 3; ++c)
                    result[c] = prF32Clamp(color[c] * factor, 0, 1);
                result[3] = 1;
            }
            float* o = out.data + (size_t(y) * e.width + uint32_t(x)) * 4;
            if (l.kind == PrDrawLightingKind_Specular) {
                // Specular light is a color whose alpha is its brightest channel, so it is already premultiplied.
                for (uint32_t c = 0; c < 4; ++c)
                    o[c] = result[c];
            } else {
                premultiply(result, o);
            }
        }
    }
}

void imageNode(Evaluation& e, const PrDrawFilterNode& node, Image& out) {
    const PrRect& r = node.image.rect;
    PrAffine2 back{};
    if (!prAffine2Invert(e.inputs->transform, &back) || prRectIsEmpty(r))
        return;
    // A list renders at the size in pixels that its rect covers.
    PrRect covered = prAffine2MapRect(e.inputs->transform, r);
    PrRect texels{};
    const CpuTexture* texture = e.inputs->sourceTexture(e.inputs->context, &node.image.source,
        uint32_t(prF32Ceil(prRectWidth(covered))), uint32_t(prF32Ceil(prRectHeight(covered))), &texels);
    if (texture == nullptr)
        return;
    const CpuTextureLevel& level = texture->levels.data[0];
    auto texelAt = [&](int32_t x, int32_t y, uint32_t c) {
        x = x < 0 ? 0 : x >= int32_t(level.width) ? int32_t(level.width) - 1 : x;
        y = y < 0 ? 0 : y >= int32_t(level.height) ? int32_t(level.height) - 1 : y;
        float texel[4];
        cpuReadPixel(level.data + (size_t(y) * level.width + uint32_t(x)) * cpuPixelBytes(level.format), level.format, texel);
        return texel[c];
    };
    for (uint32_t y = 0; y < e.height; ++y) {
        for (uint32_t x = 0; x < e.width; ++x) {
            PrVec2 p = prAffine2MapPoint(back, PrVec2{
                float(x) + 0.5f + float(e.layer->box.left),
                float(y) + 0.5f + float(e.layer->box.top),
            });
            if (p.x < r.left || p.x >= r.right || p.y < r.top || p.y >= r.bottom)
                continue;
            float u = texels.left + (p.x - r.left) / prRectWidth(r) * prRectWidth(texels) - 0.5f;
            float v = texels.top + (p.y - r.top) / prRectHeight(r) * prRectHeight(texels) - 0.5f;
            float* o = out.data + (size_t(y) * e.width + x) * 4;
            if ((node.image.sampling & PrDrawSamplingFlag_Kind) == PrDrawSampling_Nearest) {
                for (uint32_t c = 0; c < 4; ++c)
                    o[c] = texelAt(int32_t(prF32Round(u)), int32_t(prF32Round(v)), c);
                continue;
            }
            int32_t x0 = int32_t(prF32Floor(u));
            int32_t y0 = int32_t(prF32Floor(v));
            float fx = u - float(x0);
            float fy = v - float(y0);
            for (uint32_t c = 0; c < 4; ++c) {
                float top = texelAt(x0, y0, c) * (1 - fx) + texelAt(x0 + 1, y0, c) * fx;
                float bottom = texelAt(x0, y0 + 1, c) * (1 - fx) + texelAt(x0 + 1, y0 + 1, c) * fx;
                o[c] = top * (1 - fy) + bottom * fy;
            }
        }
    }
}

// Fills the region with noise. A stitched tile is the node's region in the group's space, or the graph's.
void turbulenceNode(const Evaluation& e, const PrDrawFilterNode& node, PrRect graph, PrIRect region, Image& out) {
    PrAffine2 back{};
    if (!prAffine2Invert(e.inputs->transform, &back))
        return;
    PrImageNoise* noise = alloc<PrImageNoise>();
    prImageNoise(node.turbulence.seed, noise);
    const PrIRect& box = e.layer->box;
    PrRect tile = prRectIsEmpty(node.region) ? graph : node.region;
    for (int32_t y = region.top; y < region.bottom; ++y) {
        for (int32_t x = region.left; x < region.right; ++x) {
            PrVec2 p = prAffine2MapPoint(back, PrVec2{float(x + box.left), float(y + box.top)});
            float s[4];
            for (uint32_t c = 0; c < 4; ++c) {
                float value = turbulence(*noise, c, p, node.turbulence.baseFrequency, node.turbulence.octaves,
                    node.turbulence.kind, node.turbulence.stitch, tile);
                s[c] = node.turbulence.kind == PrDrawTurbulenceKind_FractalNoise ? (value + 1) / 2 : value;
            }
            premultiply(s, out.data + (size_t(y) * e.width + uint32_t(x)) * 4);
        }
    }
    prMemoryFree(noise);
}

void progressiveBlur(Evaluation& e, const PrDrawFilterNode& node, const Image& in, Image& out) {
    RawArray<float> alpha;
    alpha.reserve(uint32_t(pixelCount(e)));
    alpha.count = uint32_t(pixelCount(e));
    const PrIRect& box = e.layer->box;
    e.inputs->brushAlpha(e.inputs->context, &node.progressive.brush, e.inputs->transform, box, alpha.data);
    // Levels of blur from no sigma to the largest sigma, mixed by the brush's alpha.
    Image levels[kProgressiveLevels];
    float sigmas[kProgressiveLevels];
    for (uint32_t i = 0; i < kProgressiveLevels; ++i) {
        sigmas[i] = i == 0 ? 0 : node.progressive.sigma / float(1u << (kProgressiveLevels - 1 - i));
        levels[i] = copyImage(e, in);
        blurImage(e, levels[i], sigmas[i] * e.scaleX, sigmas[i] * e.scaleY, PrImageEdgeMode_None);
    }
    size_t count = pixelCount(e);
    for (size_t i = 0; i < count; ++i) {
        float wanted = prF32Clamp(alpha.data[i], 0, 1) * node.progressive.sigma;
        uint32_t k = 0;
        while (k + 2 < kProgressiveLevels && sigmas[k + 1] < wanted)
            ++k;
        float span = sigmas[k + 1] - sigmas[k];
        float t = span > 0 ? prF32Clamp((wanted - sigmas[k]) / span, 0, 1) : 0;
        for (uint32_t c = 0; c < 4; ++c)
            out.data[i * 4 + c] = levels[k].data[i * 4 + c] + (levels[k + 1].data[i * 4 + c] - levels[k].data[i * 4 + c]) * t;
    }
    for (Image& level : levels)
        release(level);
    alpha.reset();
}

// Returns the number of entries of `inputs` that a node reads.
uint32_t inputsOf(PrDrawFilterKind kind) {
    switch (kind) {
        case PrDrawFilterKind_Composite:
        case PrDrawFilterKind_Arithmetic:
        case PrDrawFilterKind_Blend:
        case PrDrawFilterKind_DisplacementMap:
            return 2;
        case PrDrawFilterKind_Flood:
        case PrDrawFilterKind_Merge:
        case PrDrawFilterKind_Turbulence:
        case PrDrawFilterKind_Image:
            return 0;
        default:
            return 1;
    }
}

// Evaluates one node into a new image.
Image evaluate(Evaluation& e, const PrDrawFilterNode& node, PrIRect region) {
    bool linear = node.space == PrColorSpace_SrgbLinear;
    // The node's inputs, in the space that it computes in.
    Image in[2] = {{nullptr, false}, {nullptr, false}};
    uint32_t inputs = inputsOf(node.kind);
    for (uint32_t i = 0; i < inputs; ++i) {
        const Image& from = inputOf(e, node.inputs[i]);
        in[i] = Image{from.data, false};
        // A displacement map moves the pixels of its first input unconverted, and reads its map in the node's space.
        if (linear && !(node.kind == PrDrawFilterKind_DisplacementMap && i == 0)) {
            in[i] = copyImage(e, from);
            convertSpace(e, in[i], true);
        }
    }
    Image out{nullptr, false};
    size_t count = pixelCount(e);
    float sx = e.scaleX;
    float sy = e.scaleY;
    switch (node.kind) {
        case PrDrawFilterKind_Offset:
            out = shifted(e, in[0], node.offset.x * e.inputs->transform.a, node.offset.y * e.inputs->transform.d);
            break;
        case PrDrawFilterKind_Flood: {
            out = newImage(e);
            PrColor flood = node.flood;
            float color[4];
            if (e.layer->format == PrPixelFormat_RgbaF16) {
                color[3] = prF32Clamp(flood.a, 0, 1);
                color[0] = flood.r * color[3];
                color[1] = flood.g * color[3];
                color[2] = flood.b * color[3];
            } else {
                float straight[4] = {flood.r, flood.g, flood.b, flood.a};
                premultiply(straight, color);
            }
            if (linear) {
                for (uint32_t c = 0; c < 3; ++c) {
                    float straight = color[3] > 0 ? color[c] / color[3] : 0;
                    color[c] = prColorTransferDecode(PrColorTransfer_Srgb, straight) * color[3];
                }
            }
            for (size_t i = 0; i < count; ++i)
                prMemCopy(out.data + i * 4, color, sizeof(color));
            break;
        }
        case PrDrawFilterKind_ColorMatrix:
            out = copyImage(e, in[0]);
            colorMatrix(e, &node.matrix.m[0][0], out);
            break;
        case PrDrawFilterKind_ComponentTransfer:
            out = copyImage(e, in[0]);
            componentTransfer(e, node.transfer, out);
            break;
        case PrDrawFilterKind_Composite:
        case PrDrawFilterKind_Blend: {
            out = newImage(e);
            PrDrawComposite op = node.kind == PrDrawFilterKind_Composite ? node.composite : PrDrawComposite_SourceOver;
            PrDrawBlendMode blend = node.kind == PrDrawFilterKind_Blend ? node.blend : PrDrawBlendMode_Normal;
            for (size_t i = 0; i < count; ++i)
                cpuCompositeColors(in[0].data + i * 4, in[1].data + i * 4, op, blend, out.data + i * 4);
            break;
        }
        case PrDrawFilterKind_Arithmetic:
            out = newImage(e);
            for (size_t i = 0; i < count; ++i) {
                const float* a = in[0].data + i * 4;
                const float* b = in[1].data + i * 4;
                float* o = out.data + i * 4;
                o[3] = prF32Clamp(node.k[0] * a[3] * b[3] + node.k[1] * a[3] + node.k[2] * b[3] + node.k[3], 0, 1);
                for (uint32_t c = 0; c < 3; ++c)
                    o[c] = prF32Clamp(node.k[0] * a[c] * b[c] + node.k[1] * a[c] + node.k[2] * b[c] + node.k[3], 0, o[3]);
            }
            break;
        case PrDrawFilterKind_Merge:
            out = newImage(e);
            for (uint32_t k = 0; k < node.merge.inputCount; ++k) {
                Image layer = copyImage(e, inputOf(e, node.merge.inputs[k]));
                if (linear)
                    convertSpace(e, layer, true);
                over(e, layer, out);
                release(layer);
            }
            break;
        case PrDrawFilterKind_Blur:
            out = copyImage(e, in[0]);
            extend(e, out, regionOf(e, node.inputs[0]), node.blur.edge);
            blurImage(e, out, node.blur.sigma.x * sx, node.blur.sigma.y * sy, beyondOffscreen(node.blur.edge));
            break;
        case PrDrawFilterKind_ProgressiveBlur:
            out = newImage(e);
            progressiveBlur(e, node, in[0], out);
            break;
        case PrDrawFilterKind_Morphology:
            out = copyImage(e, in[0]);
            morphImage(e, out, node.morphology.radius.x * sx, node.morphology.radius.y * sy, node.morphology.op);
            break;
        case PrDrawFilterKind_Convolution: {
            Image extended = copyImage(e, in[0]);
            extend(e, extended, regionOf(e, node.inputs[0]), node.convolution.edge);
            out = newImage(e);
            convolve(e, node, extended, beyondOffscreen(node.convolution.edge), out);
            release(extended);
            break;
        }
        case PrDrawFilterKind_Turbulence:
            out = newImage(e);
            turbulenceNode(e, node, e.graph, region, out);
            break;
        case PrDrawFilterKind_Tile: {
            // The tile in the offscreen's pixels, clipped to the offscreen.
            PrIRect device = prIRectRoundOut(prAffine2MapRect(e.inputs->transform, node.tile));
            const PrIRect& box = e.layer->box;
            PrIRect tile = prIRectIntersect(PrIRect{
                device.left - box.left, device.top - box.top,
                device.right - box.left, device.bottom - box.top,
            }, PrIRect{0, 0, int32_t(e.width), int32_t(e.height)});
            out = tiled(e, in[0], tile);
            break;
        }
        case PrDrawFilterKind_Image:
            out = newImage(e);
            imageNode(e, node, out);
            if (linear)
                convertSpace(e, out, true);
            break;
        case PrDrawFilterKind_DropShadow: {
            Image alpha = alphaOf(e, in[0]);
            blurImage(e, alpha, node.shadow.sigma.x * sx, node.shadow.sigma.y * sy, PrImageEdgeMode_None);
            out = shifted(e, alpha, node.shadow.offset.x * e.inputs->transform.a,
                node.shadow.offset.y * e.inputs->transform.d);
            release(alpha);
            colorize(e, out, node.shadow.color, linear);
            over(e, in[0], out);
            break;
        }
        case PrDrawFilterKind_InnerShadow: {
            Image empty = inverseAlphaOf(e, in[0]);
            Image moved = shifted(e, empty, node.shadow.offset.x * e.inputs->transform.a,
                node.shadow.offset.y * e.inputs->transform.d);
            release(empty);
            blurImage(e, moved, node.shadow.sigma.x * sx, node.shadow.sigma.y * sy, PrImageEdgeMode_Duplicate);
            colorize(e, moved, node.shadow.color, linear);
            out = copyImage(e, in[0]);
            atop(e, moved, out);
            release(moved);
            break;
        }
        case PrDrawFilterKind_OuterGlow: {
            Image alpha = alphaOf(e, in[0]);
            morphImage(e, alpha, node.glow.spread * sx, node.glow.spread * sy, true);
            blurImage(e, alpha, node.glow.sigma * sx, node.glow.sigma * sy, PrImageEdgeMode_None);
            colorize(e, alpha, node.glow.color, linear);
            out = alpha;
            over(e, in[0], out);
            break;
        }
        case PrDrawFilterKind_InnerGlow: {
            Image edge = inverseAlphaOf(e, in[0]);
            morphImage(e, edge, node.glow.spread * sx, node.glow.spread * sy, true);
            blurImage(e, edge, node.glow.sigma * sx, node.glow.sigma * sy, PrImageEdgeMode_Duplicate);
            colorize(e, edge, node.glow.color, linear);
            out = copyImage(e, in[0]);
            atop(e, edge, out);
            release(edge);
            break;
        }
        case PrDrawFilterKind_Feather: {
            Image alpha = alphaOf(e, in[0]);
            float half = node.feather / 2;
            morphImage(e, alpha, half * sx, half * sy, false);
            blurImage(e, alpha, half * sx, half * sy, PrImageEdgeMode_None);
            out = copyImage(e, in[0]);
            for (size_t i = 0; i < count; ++i) {
                float m = prF32Clamp(alpha.data[i * 4 + 3], 0, 1);
                for (uint32_t c = 0; c < 4; ++c)
                    out.data[i * 4 + c] *= m;
            }
            release(alpha);
            break;
        }
        case PrDrawFilterKind_DisplacementMap:
            out = newImage(e);
            displace(e, node, in[0], in[1], out);
            break;
        case PrDrawFilterKind_Lighting:
            out = newImage(e);
            light(e, node, in[0], out);
            break;
        default:
            out = newImage(e);
            break;
    }
    for (Image& image : in)
        release(image);
    if (linear && node.kind != PrDrawFilterKind_DisplacementMap)
        convertSpace(e, out, false);
    keepWithin(e, out, region);
    return out;
}

} // namespace

int32_t cpuFilterReach(const PrDrawFilter* filter, float scale) {
    PrRect outset = prDrawFilterOutset(filter);
    float reach = prF32Max(prF32Max(-outset.left, -outset.top), prF32Max(outset.right, outset.bottom));
    return int32_t(prF32Ceil(prF32Min(reach * scale, 1 << 28))) + 1;
}

void cpuApplyFilter(const PrDrawFilter* filter, const CpuFilterInputs& inputs, const CpuTarget& layer) {
    Evaluation e{};
    e.inputs = &inputs;
    e.layer = &layer;
    e.width = prIRectWidth(layer.box);
    e.height = prIRectHeight(layer.box);
    if (e.width == 0 || e.height == 0)
        return;
    e.scaleX = prF32Abs(inputs.transform.a);
    e.scaleY = prF32Abs(inputs.transform.d);
    PrIRect whole{0, 0, int32_t(e.width), int32_t(e.height)};
    PrRect region = prDrawFilterRegion(filter);
    if (prRectIsEmpty(region))
        region = inputs.bounds;
    e.region = prRectIsEmpty(region) ? whole : pixelsOf(e, region);
    e.graph = region;
    PrAffine2 back{};
    if (prRectIsEmpty(region) && prAffine2Invert(inputs.transform, &back))
        e.graph = prAffine2MapRect(back, prRectFromIRect(layer.box));
    e.source = imageOf(e, layer);
    const PrDrawFilterNode* nodes = prDrawFilterNodes(filter);
    for (uint32_t i = 0; i < prDrawFilterNodeCount(filter); ++i) {
        PrIRect nodeRegion;
        if (prRectIsEmpty(nodes[i].region))
            nodeRegion = e.region;
        else
            nodeRegion = prIRectIntersect(e.region, pixelsOf(e, nodes[i].region));
        e.results.push(evaluate(e, nodes[i], nodeRegion));
        e.regions.push(nodeRegion);
    }
    const Image& result = e.results.last();
    for (uint32_t y = 0; y < e.height; ++y) {
        uint8_t* row = cpuTargetRow(layer, layer.box.top + int32_t(y));
        for (uint32_t x = 0; x < e.width; ++x)
            cpuWritePixel(row + size_t(x) * cpuPixelBytes(layer.format), layer.format,
                result.data + (size_t(y) * e.width + x) * 4);
    }
    for (uint32_t i = 0; i < e.results.count; ++i)
        release(e.results.data[i]);
    release(e.source);
    release(e.sourceAlpha);
    release(e.backdrop);
    e.results.reset();
    e.regions.reset();
    e.line.reset();
    e.scratch.reset();
}

void cpuBlurCoverage(const CpuCoverage& coverage, float sigmaX, float sigmaY) {
    uint32_t width = prIRectWidth(coverage.box);
    uint32_t height = prIRectHeight(coverage.box);
    if (width == 0 || height == 0 || coverage.data == nullptr)
        return;
    RawArray<float> values;
    values.reserve(width * height);
    values.count = width * height;
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x)
            values.data[y * width + x] = float(coverage.data[y * coverage.stride + x]);
    }
    PrPixels pixels{reinterpret_cast<uint8_t*>(values.data), size_t(width) * sizeof(float), {width, height}};
    prImageBlur(pixels, PrPixelFormat_RF32, PrVec2{sigmaX, sigmaY}, PrImageEdgeMode_None);
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x)
            coverage.data[y * coverage.stride + x] = uint8_t(prF32Clamp(values.data[y * width + x], 0, 255) + 0.5f);
    }
    values.reset();
}

} // namespace pr_impl
