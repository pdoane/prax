// Image processing: operations on pixels in memory.

#include "pr/image_process.h"

#include "pr/assert.h"
#include "pr/color_space.h"
#include "pr/float.h"
#include "pr/float16.h"
#include "pr/mem.h"
#include "pr/raw_array.h"

namespace pr_impl {

namespace {

// Below this sigma, a blur is the exact Gaussian. At or above it, the blur is three box passes, which approximate the
// Gaussian to within a few percent at a cost independent of the sigma.
constexpr float kBoxSigma = 2;

// Returns a row as float pixels.
float* floatsOf(uint8_t* row) {
    return static_cast<float*>(static_cast<void*>(row));
}

// Returns the index that a read at `i` of `count` values uses under the edge mode, or -1 for transparent.
int32_t edgeIndex(int32_t i, int32_t count, PrImageEdgeMode edge) {
    if (i >= 0 && i < count)
        return i;
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

// Blurs `count` values `stride` apart by a Gaussian of `sigma`, reading beyond them according to the edge mode.
void blurLine(float* values, uint32_t count, size_t stride, float sigma, PrImageEdgeMode edge, RawArray<float>& scratch) {
    if (!(sigma > 0) || count == 0)
        return;
    uint32_t d = uint32_t(prF32Floor(sigma * 3 * prF32Sqrt(2 * kPrF32Pi) / 4 + 0.5f));
    int32_t pad = sigma < kBoxSigma ? int32_t(prF32Ceil(sigma * 3)) : int32_t(d * 2 + 2);
    uint32_t padded = count + uint32_t(pad) * 2;
    scratch.clear();
    scratch.reserve(padded * 2);
    float* in = scratch.data;
    float* out = scratch.data + padded;
    for (int32_t i = -pad; i < int32_t(count) + pad; ++i) {
        int32_t at = edgeIndex(i, int32_t(count), edge);
        in[i + pad] = at >= 0 ? values[size_t(at) * stride] : 0;
    }
    if (sigma < kBoxSigma) {
        float kernel[16];
        float sum = 0;
        for (int32_t k = -pad; k <= pad; ++k) {
            kernel[k + pad] = prF32Exp(-float(k * k) / (2 * sigma * sigma));
            sum += kernel[k + pad];
        }
        for (uint32_t i = 0; i < count; ++i) {
            float total = 0;
            for (int32_t k = -pad; k <= pad; ++k)
                total += in[int32_t(i) + pad + k] * kernel[k + pad];
            values[i * stride] = total / sum;
        }
        return;
    }
    for (uint32_t pass = 0; pass < 3; ++pass) {
        // A box of width d. If d is even, the second box is shifted so that the three boxes are centered together.
        int32_t before = int32_t(d / 2);
        int32_t after = int32_t(d) - before - 1;
        if (d % 2 == 0 && pass == 1) {
            int32_t swap = before;
            before = after;
            after = swap;
        }
        float sum = 0;
        for (int32_t k = -before; k <= after; ++k) {
            if (k >= 0 && k < int32_t(padded))
                sum += in[k];
        }
        for (uint32_t i = 0; i < padded; ++i) {
            out[i] = sum / float(d);
            int32_t leaving = int32_t(i) - before;
            int32_t entering = int32_t(i) + after + 1;
            if (leaving >= 0 && leaving < int32_t(padded))
                sum -= in[leaving];
            if (entering >= 0 && entering < int32_t(padded))
                sum += in[entering];
        }
        float* swap = in;
        in = out;
        out = swap;
    }
    for (uint32_t i = 0; i < count; ++i)
        values[i * stride] = in[i + uint32_t(pad)];
}

// The lobes of the resampling filter.
constexpr float kLobes = 3;

// The filter's weight at `x` source pixels from the sample, at the filter's scale.
float lanczos(float x) {
    x = prF32Abs(x);
    if (x < 1e-6f)
        return 1;
    if (x >= kLobes)
        return 0;
    float pi = kPrF32Pi * x;
    return kLobes * prF32Sin(pi) * prF32Sin(pi / kLobes) / (pi * pi);
}

// The linear value of each 8-bit sRGB code. The values rise with the code, so a search finds the code of a value.
struct SrgbTable {
    float linear[256];

    SrgbTable() {
        for (uint32_t i = 0; i < 256; ++i)
            linear[i] = prColorTransferDecode(PrColorTransfer_Srgb, float(i) / 255);
    }

    // Returns the code whose linear value is nearest to `value`.
    uint8_t encode(float value) const {
        if (value <= 0)
            return 0;
        if (value >= 1)
            return 255;
        uint32_t low = 0;
        uint32_t high = 255;
        while (high - low > 1) {
            uint32_t middle = (low + high) / 2;
            if (linear[middle] <= value)
                low = middle;
            else
                high = middle;
        }
        return value - linear[low] <= linear[high] - value ? uint8_t(low) : uint8_t(high);
    }
};

const SrgbTable& srgbTable() {
    static const SrgbTable table;
    return table;
}

// The source pixels that one output pixel reads along an axis. They are `count` pixels starting at `first`. `weights`
// is the index of their first weight.
struct Taps {
    uint32_t first;
    uint32_t count;
    uint32_t weights;
};

// Writes the taps and the weights of each of `to` output pixels along an axis of `from` source pixels. The weights of
// an output pixel sum to one. A tap beyond the edge adds its weight to that of the edge pixel. Returns the greatest
// number of taps of any output pixel.
uint32_t tapsOf(uint32_t from, uint32_t to, RawArray<Taps>& taps, RawArray<float>& weights) {
    float scale = float(from) / float(to);
    float widen = scale > 1 ? scale : 1;
    float support = kLobes * widen;
    uint32_t most = 0;
    taps.resize(to);
    for (uint32_t i = 0; i < to; ++i) {
        float center = (float(i) + 0.5f) * scale;
        int32_t low = int32_t(prF32Floor(center - support));
        int32_t high = int32_t(prF32Ceil(center + support));
        int32_t first = low < 0 ? 0 : low < int32_t(from) ? low : int32_t(from) - 1;
        int32_t last = high > int32_t(from) ? int32_t(from) : high <= first ? first + 1 : high;
        Taps& tap = taps.data[i];
        tap.first = uint32_t(first);
        tap.count = uint32_t(last - first);
        tap.weights = weights.count;
        for (uint32_t k = 0; k < tap.count; ++k)
            weights.push(0.0f);
        float sum = 0;
        for (int32_t j = low; j < high; ++j) {
            float weight = lanczos((float(j) + 0.5f - center) / widen);
            int32_t at = j < first ? first : j >= last ? last - 1 : j;
            weights.data[tap.weights + uint32_t(at - first)] += weight;
            sum += weight;
        }
        // If every weight is zero because the sample lies between pixels, the sample reads its nearest pixel.
        if (prF32Abs(sum) < 1e-6f) {
            uint32_t nearest = uint32_t(center) < from ? uint32_t(center) : from - 1;
            weights.count = tap.weights;
            tap.first = nearest;
            tap.count = 1;
            weights.push(1.0f);
            sum = 1;
        }
        for (uint32_t k = 0; k < tap.count; ++k)
            weights.data[tap.weights + k] /= sum;
        most = tap.count > most ? tap.count : most;
    }
    return most;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

void prImageBlur(PrPixels pixels, PrPixelFormat format, PrVec2 sigma, PrImageEdgeMode edge) {
    PR_ASSERT(format == PrPixelFormat_RgbaF32 || format == PrPixelFormat_RF32);
    PR_ASSERT(pixels.stride % sizeof(float) == 0);
    uint32_t channels = format == PrPixelFormat_RgbaF32 ? 4 : 1;
    size_t rowFloats = pixels.stride / sizeof(float);
    RawArray<float> scratch;
    for (uint32_t c = 0; c < channels; ++c) {
        if (sigma.x > 0) {
            for (uint32_t y = 0; y < pixels.extent.y; ++y)
                blurLine(floatsOf(pixels.data + size_t(y) * pixels.stride) + c, pixels.extent.x, channels, sigma.x,
                    edge, scratch);
        }
        if (sigma.y > 0 && pixels.extent.y != 0) {
            for (uint32_t x = 0; x < pixels.extent.x; ++x)
                blurLine(floatsOf(pixels.data) + size_t(x) * channels + c, pixels.extent.y, rowFloats, sigma.y, edge, scratch);
        }
    }
    scratch.reset();
}

void prImageMipLevel(PrPixelView above, PrPixels out, PrPixelFormat format) {
    PR_ASSERT(out.extent.x == (above.extent.x > 1 ? above.extent.x / 2 : 1));
    PR_ASSERT(out.extent.y == (above.extent.y > 1 ? above.extent.y / 2 : 1));
    PR_ASSERT(format == PrPixelFormat_Rgba8 || format == PrPixelFormat_Bgra8 || format == PrPixelFormat_RgbaF16);
    bool half = format == PrPixelFormat_RgbaF16;
    for (uint32_t y = 0; y < out.extent.y; ++y) {
        uint32_t y0 = y * 2 < above.extent.y ? y * 2 : above.extent.y - 1;
        uint32_t y1 = y0 + 1 < above.extent.y ? y0 + 1 : y0;
        const uint8_t* row0 = above.data + size_t(y0) * above.stride;
        const uint8_t* row1 = above.data + size_t(y1) * above.stride;
        uint8_t* row = out.data + size_t(y) * out.stride;
        for (uint32_t x = 0; x < out.extent.x; ++x) {
            uint32_t x0 = x * 2 < above.extent.x ? x * 2 : above.extent.x - 1;
            uint32_t x1 = x0 + 1 < above.extent.x ? x0 + 1 : x0;
            if (half) {
                PrFloat16 block[4][4];
                prMemCopy(block[0], row0 + x0 * 8, 8);
                prMemCopy(block[1], row0 + x1 * 8, 8);
                prMemCopy(block[2], row1 + x0 * 8, 8);
                prMemCopy(block[3], row1 + x1 * 8, 8);
                PrFloat16 mean[4];
                for (uint32_t k = 0; k < 4; ++k) {
                    float sum = prFloat16ToFloat(block[0][k]) + prFloat16ToFloat(block[1][k]) +
                        prFloat16ToFloat(block[2][k]) + prFloat16ToFloat(block[3][k]);
                    mean[k] = prFloat16FromFloat(sum / 4);
                }
                prMemCopy(row + x * 8, mean, 8);
                continue;
            }
            for (uint32_t k = 0; k < 4; ++k) {
                uint32_t sum = row0[x0 * 4 + k] + row0[x1 * 4 + k] + row1[x0 * 4 + k] + row1[x1 * 4 + k];
                row[x * 4 + k] = uint8_t((sum + 2) / 4);
            }
        }
    }
}

void prImageResize(PrPixelView source, PrPixels output) {
    PR_ASSERT(source.extent.x > 0 && source.extent.y > 0 && output.extent.x > 0 && output.extent.y > 0);
    const SrgbTable& table = srgbTable();
    RawArray<Taps> across;
    RawArray<Taps> down;
    RawArray<float> weights;
    tapsOf(source.extent.x, output.extent.x, across, weights);
    uint32_t window = tapsOf(source.extent.y, output.extent.y, down, weights);

    // A ring of source rows, resampled across in premultiplied linear light. Each row is in the slot of its row number
    // modulo the size of the ring. The rows that an output row reads are consecutive and fit in the ring, and they only
    // move down.
    uint32_t width = output.extent.x * 4;
    RawArray<float> ring;
    RawArray<int64_t> held;
    RawArray<float> line;
    ring.resize(uint32_t(size_t(window) * width));
    held.resize(window);
    line.resize(uint32_t(size_t(source.extent.x) * 4));
    for (uint32_t i = 0; i < window; ++i)
        held.data[i] = -1;
    RawArray<float> sum;
    sum.resize(width);

    for (uint32_t y = 0; y < output.extent.y; ++y) {
        const Taps& vertical = down.data[y];
        for (uint32_t k = 0; k < width; ++k)
            sum.data[k] = 0;
        for (uint32_t t = 0; t < vertical.count; ++t) {
            uint32_t row = vertical.first + t;
            float* resampled = ring.data + size_t(row % window) * width;
            if (held.data[row % window] != int64_t(row)) {
                held.data[row % window] = row;
                const uint8_t* in = source.data + size_t(row) * source.stride;
                for (uint32_t x = 0; x < source.extent.x; ++x) {
                    float alpha = float(in[x * 4 + 3]) / 255;
                    line.data[x * 4 + 0] = table.linear[in[x * 4 + 0]] * alpha;
                    line.data[x * 4 + 1] = table.linear[in[x * 4 + 1]] * alpha;
                    line.data[x * 4 + 2] = table.linear[in[x * 4 + 2]] * alpha;
                    line.data[x * 4 + 3] = alpha;
                }
                for (uint32_t x = 0; x < output.extent.x; ++x) {
                    const Taps& horizontal = across.data[x];
                    float pixel[4] = {0, 0, 0, 0};
                    for (uint32_t h = 0; h < horizontal.count; ++h) {
                        float weight = weights.data[horizontal.weights + h];
                        const float* from = line.data + size_t(horizontal.first + h) * 4;
                        for (uint32_t c = 0; c < 4; ++c)
                            pixel[c] += from[c] * weight;
                    }
                    for (uint32_t c = 0; c < 4; ++c)
                        resampled[x * 4 + c] = pixel[c];
                }
            }
            float weight = weights.data[vertical.weights + t];
            for (uint32_t k = 0; k < width; ++k)
                sum.data[k] += resampled[k] * weight;
        }
        uint8_t* out = output.data + size_t(y) * output.stride;
        for (uint32_t x = 0; x < output.extent.x; ++x) {
            const float* pixel = sum.data + size_t(x) * 4;
            float alpha = prF32Clamp(pixel[3], 0, 1);
            float divide = alpha > 0 ? 1 / alpha : 0;
            for (uint32_t c = 0; c < 3; ++c)
                out[x * 4 + c] = table.encode(pixel[c] * divide);
            out[x * 4 + 3] = uint8_t(alpha * 255 + 0.5f);
        }
    }
    sum.reset();
    line.reset();
    held.reset();
    ring.reset();
    weights.reset();
    down.reset();
    across.reset();
}

void prImageNoise(float seed, PrImageNoise* out) {
    PrImageNoise& noise = *out;
    // The Park-Miller generator that seeds the noise, and the number of points in the noise's lattice.
    constexpr int64_t kModulus = 2147483647;
    constexpr int64_t kMultiplier = 16807;
    constexpr int64_t kQuotient = 127773;
    constexpr int64_t kRemainder = 2836;
    constexpr int32_t kLattice = kPrImageNoiseLattice;
    auto next = [&](int64_t s) {
        int64_t result = kMultiplier * (s % kQuotient) - kRemainder * (s / kQuotient);
        return result <= 0 ? result + kModulus : result;
    };
    // A seed is clamped to within twice the modulus either way, and the steps below bring it into the range of the
    // generator.
    int64_t s = int64_t(prF64Min(prF64Max(double(prF32Round(seed)), -2.0 * double(kModulus)), 2.0 * double(kModulus)));
    if (s <= 0)
        s = -(s % (kModulus - 1)) + 1;
    if (s > kModulus - 1)
        s = kModulus - 1;
    int32_t i = 0;
    for (uint32_t k = 0; k < 4; ++k) {
        for (i = 0; i < kLattice; ++i) {
            noise.lattice[i] = i;
            for (uint32_t j = 0; j < 2; ++j) {
                s = next(s);
                noise.gradients[k][i][j] = float((s % (kLattice + kLattice)) - kLattice) / kLattice;
            }
            float* g = noise.gradients[k][i];
            float length = prF32Sqrt(g[0] * g[0] + g[1] * g[1]);
            if (length > 0) {
                g[0] /= length;
                g[1] /= length;
            }
        }
    }
    while (--i) {
        int32_t k = noise.lattice[i];
        s = next(s);
        int32_t j = int32_t(s % kLattice);
        noise.lattice[i] = noise.lattice[j];
        noise.lattice[j] = k;
    }
    for (i = 0; i < kLattice + 2; ++i) {
        noise.lattice[kLattice + i] = noise.lattice[i];
        for (uint32_t k = 0; k < 4; ++k) {
            noise.gradients[k][kLattice + i][0] = noise.gradients[k][i][0];
            noise.gradients[k][kLattice + i][1] = noise.gradients[k][i][1];
        }
    }
}
