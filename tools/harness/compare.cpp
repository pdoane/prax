// The corpora's comparison, reproducing Pillow's steps: the paste onto white (libImaging/Paste.c), the blur
// (libImaging/BoxBlur.c) and the fade (libImaging/Blend.c). Like Pillow, it blurs four channels per pixel at once: red,
// green, blue and one unused channel. Rows are split among threads.

#include "tools/harness/compare.h"

#include "pr/float.h"
#include "pr/mem.h"
#include "pr/memory.h"
#include "pr/thread.h"

using namespace pr;

namespace harness {

namespace {

uint32_t minU32(uint32_t a, uint32_t b) {
    return a < b ? a : b;
}

// An image of four channels per pixel, with rows `width` pixels apart. It owns its values until freed.
struct Image {
    uint32_t width = 0;
    uint32_t height = 0;
    uint8_t* values = nullptr;

    uint8_t* row(uint32_t y) { return values + size_t(y) * width * 4; }
    const uint8_t* row(uint32_t y) const { return values + size_t(y) * width * 4; }
};

Image imageCreate(uint32_t width, uint32_t height) {
    Image image{width, height, nullptr};
    image.values = static_cast<uint8_t*>(prMemoryAlloc(size_t(width) * height * 4 + 1));
    return image;
}

void imageFree(Image& image) {
    prMemoryFree(image.values);
    image.values = nullptr;
}

// Runs `each(first, last)` over the rows [0, count), split among threads, including the calling thread.
template <typename Each>
void parallelRows(uint32_t count, Each each) {
    constexpr uint32_t kMaxThreads = 8;
    struct Rows {
        const Each* each;
        uint32_t first;
        uint32_t last;
    };
    uint32_t threads = minU32(kMaxThreads, prThreadHardwareCount());
    threads = minU32(threads, count / 64);
    threads = threads == 0 ? 1 : threads;
    uint32_t per = (count + threads - 1) / threads;
    PrThread workers[kMaxThreads] = {};
    Rows rows[kMaxThreads] = {};
    bool started[kMaxThreads] = {};
    for (uint32_t t = 1; t < threads; ++t) {
        rows[t] = Rows{&each, t * per, minU32(count, t * per + per)};
        if (rows[t].first < rows[t].last) {
            prThreadStart(&workers[t], [](void* context) {
                Rows* r = static_cast<Rows*>(context);
                (*r->each)(r->first, r->last);
            }, &rows[t]);
            started[t] = true;
        }
    }
    each(0, minU32(count, per));
    for (uint32_t t = 1; t < threads; ++t)
        if (started[t])
            prThreadJoin(&workers[t]);
}

// Pillow's extended box blur along one line of four-channel pixels, with an integer radius and fractional edge weights
// in 8.24 fixed point.
void boxBlurLine(uint8_t* out, const uint8_t* in, int32_t last, int32_t radius, int32_t edgeA,
                 int32_t edgeB, uint32_t whole, uint32_t edge) {
    uint32_t acc[4];
    for (uint32_t c = 0; c < 4; ++c) {
        acc[c] = uint32_t(in[c]) * uint32_t(radius + 1);
        for (int32_t x = 0; x < edgeA - 1; ++x)
            acc[c] += in[size_t(x) * 4 + c];
        acc[c] += uint32_t(in[size_t(last) * 4 + c]) * uint32_t(radius - edgeA + 1);
    }
    auto step = [&](int32_t x, int32_t subtract, int32_t add, int32_t left, int32_t right) {
        for (uint32_t c = 0; c < 4; ++c) {
            auto at = [&](int32_t i) { return uint32_t(in[size_t(i) * 4 + c]); };
            acc[c] += at(add) - at(subtract);
            uint32_t bulk = acc[c] * whole + (at(left) + at(right)) * edge;
            out[size_t(x) * 4 + c] = uint8_t((bulk + (1u << 23)) >> 24);
        }
    };
    if (edgeA <= edgeB) {
        for (int32_t x = 0; x < edgeA; ++x)
            step(x, 0, x + radius, 0, x + radius + 1);
        for (int32_t x = edgeA; x < edgeB; ++x)
            step(x, x - radius - 1, x + radius, x - radius - 1, x + radius + 1);
        for (int32_t x = edgeB; x <= last; ++x)
            step(x, x - radius - 1, last, x - radius - 1, last);
    } else {
        for (int32_t x = 0; x < edgeB; ++x)
            step(x, 0, x + radius, 0, x + radius + 1);
        for (int32_t x = edgeB; x < edgeA; ++x)
            step(x, 0, last, 0, last);
        for (int32_t x = edgeA; x <= last; ++x)
            step(x, x - radius - 1, last, x - radius - 1, last);
    }
}

// Blurs every row of the image in place `passes` times with a box of `floatRadius`.
void boxBlurRows(Image& image, float floatRadius, uint32_t passes) {
    int32_t radius = int32_t(floatRadius);
    uint32_t whole = uint32_t(float(1u << 24) / (floatRadius * 2 + 1));
    uint32_t edge = ((1u << 24) - uint32_t(radius * 2 + 1) * whole) / 2;
    int32_t width = int32_t(image.width);
    int32_t edgeA = radius + 1 < width ? radius + 1 : width;
    int32_t edgeB = width - radius - 1 > 0 ? width - radius - 1 : 0;
    parallelRows(image.height, [&](uint32_t first, uint32_t last) {
        size_t bytes = size_t(image.width) * 4;
        uint8_t* line = static_cast<uint8_t*>(prMemoryAlloc(bytes));
        for (uint32_t y = first; y < last; ++y) {
            uint8_t* row = image.row(y);
            for (uint32_t pass = 0; pass < passes; ++pass) {
                boxBlurLine(line, row, width - 1, radius, edgeA, edgeB, whole, edge);
                memCopy(row, line, bytes);
            }
        }
        prMemoryFree(line);
    });
}

Image transposed(const Image& image) {
    Image t = imageCreate(image.height, image.width);
    parallelRows(t.height, [&](uint32_t first, uint32_t last) {
        for (uint32_t y = first; y < last; ++y) {
            uint8_t* to = t.row(y);
            for (uint32_t x = 0; x < t.width; ++x)
                memCopy(to + size_t(x) * 4, image.row(x) + size_t(y) * 4, 4);
        }
    });
    return t;
}

// Returns the box radius whose three passes approximate a Gaussian of `sigma` (Gwosdek et al.), in Pillow's arithmetic.
float boxRadius(float sigma) {
    float sigma2 = sigma * sigma / 3;
    float length = float(sqrtF64(12.0 * double(sigma2) + 1.0));
    float whole = float(floorF64((double(length) - 1.0) / 2.0));
    float fraction = (2 * whole + 1) * (whole * (whole + 1) - 3 * sigma2);
    fraction /= 6 * (sigma2 - (whole + 1) * (whole + 1));
    return whole + fraction;
}

// Implements Pillow's GaussianBlur: three box passes along the rows, then three along the columns.
Image gaussianBlur(const Image& image, float sigma) {
    float radius = boxRadius(sigma);
    Image rows = imageCreate(image.width, image.height);
    memCopy(rows.values, image.values, size_t(image.width) * image.height * 4);
    boxBlurRows(rows, radius, 3);
    Image columns = transposed(rows);
    imageFree(rows);
    boxBlurRows(columns, radius, 3);
    Image out = transposed(columns);
    imageFree(columns);
    return out;
}

// Pastes straight RGBA onto white through its alpha, producing RGB plus an unused channel. Uses Pillow's blend of 8-bit
// values, with its rounding division by 255.
Image onWhite(const uint8_t* rgba, uint32_t width, uint32_t height, size_t stride) {
    Image image = imageCreate(width, height);
    parallelRows(height, [&](uint32_t first, uint32_t last) {
        for (uint32_t y = first; y < last; ++y) {
            const uint8_t* from = rgba + size_t(y) * stride;
            uint8_t* to = image.row(y);
            for (uint32_t x = 0; x < width; ++x) {
                const uint8_t* px = from + size_t(x) * 4;
                uint32_t a = px[3];
                for (uint32_t c = 0; c < 3; ++c) {
                    uint32_t t = 255u * (255u - a) + uint32_t(px[c]) * a + 128u;
                    to[size_t(x) * 4 + c] = uint8_t(((t >> 8) + t) >> 8);
                }
                to[size_t(x) * 4 + 3] = 0;
            }
        }
    });
    return image;
}

} // namespace

ImageDifference compareImages(const uint8_t* reference, const uint8_t* rendered, uint32_t width,
                              uint32_t height, size_t stride, const ImageComparison& comparison,
                              uint8_t* PR_NULLABLE diff) {
    Image referenceWhite = onWhite(reference, width, height, stride);
    Image renderedWhite = onWhite(rendered, width, height, stride);
    Image a = gaussianBlur(referenceWhite, comparison.blurRadius);
    Image b = gaussianBlur(renderedWhite, comparison.blurRadius);
    // Each row keeps its own sums. They are added in order afterward, so that the total does not depend on how the rows
    // are split.
    struct Row {
        uint64_t sum;
        uint32_t changed;
        uint32_t content;
        uint32_t changedContent;
    };
    Row* rows = static_cast<Row*>(prMemoryAllocZeroed(sizeof(Row) * height + 1));
    parallelRows(height, [&](uint32_t first, uint32_t last) {
        for (uint32_t y = first; y < last; ++y) {
            Row r{};
            const uint8_t* pa = a.row(y);
            const uint8_t* pb = b.row(y);
            const uint8_t* ra = referenceWhite.row(y);
            const uint8_t* rb = renderedWhite.row(y);
            uint8_t* out = diff != nullptr ? diff + size_t(y) * stride : nullptr;
            for (uint32_t x = 0; x < width; ++x) {
                uint32_t delta = 0;
                bool content = false;
                for (uint32_t c = 0; c < 3; ++c) {
                    size_t i = size_t(x) * 4 + c;
                    uint32_t d = pa[i] > pb[i] ? pa[i] - pb[i] : pb[i] - pa[i];
                    r.sum += d;
                    delta = d > delta ? d : delta;
                    // Content: a pixel at which either unblurred image differs from white by more than 2.
                    content = content || 255 - ra[i] > 2 || 255 - rb[i] > 2;
                }
                bool differs = delta > comparison.channelDelta;
                r.changed += differs ? 1 : 0;
                r.content += content ? 1 : 0;
                r.changedContent += differs && content ? 1 : 0;
                if (out == nullptr)
                    continue;
                uint8_t* px = out + size_t(x) * 4;
                if (differs) {
                    px[0] = 0xe5;
                    px[1] = 0x22;
                    px[2] = 0x22;
                } else {
                    // Pillow's blend toward white at 0.82, which truncates.
                    for (uint32_t c = 0; c < 3; ++c) {
                        float value = ra[size_t(x) * 4 + c];
                        px[c] = uint8_t(value + 0.82f * (255 - value));
                    }
                }
                px[3] = 255;
            }
            rows[y] = r;
        }
    });
    uint64_t sum = 0;
    size_t changed = 0;
    size_t content = 0;
    size_t changedContent = 0;
    for (uint32_t y = 0; y < height; ++y) {
        sum += rows[y].sum;
        changed += rows[y].changed;
        content += rows[y].content;
        changedContent += rows[y].changedContent;
    }
    prMemoryFree(rows);
    imageFree(referenceWhite);
    imageFree(renderedWhite);
    imageFree(a);
    imageFree(b);
    size_t total = size_t(width) * height;
    ImageDifference d{};
    d.changedRatio = total != 0 ? double(changed) / double(total) : 0;
    d.contentChangedRatio =
        content != 0 ? double(changedContent) / double(content) : d.changedRatio;
    d.mae = total != 0 ? double(sum) / double(total) / 3 : 0;
    return d;
}

PixelDifference countDifferences(const uint8_t* reference, const uint8_t* rendered,
                                 uint32_t width, uint32_t height, size_t stride) {
    Image a = onWhite(reference, width, height, stride);
    Image b = onWhite(rendered, width, height, stride);
    // Pillow's weights of red, green and blue in luma, in 16-bit fixed point.
    constexpr uint32_t kLuma[3] = {19595, 38470, 7471};
    PixelDifference d{};
    for (size_t i = 0; i < size_t(width) * height; ++i) {
        uint32_t largest = 0;
        uint32_t luma = 0x8000;
        for (uint32_t c = 0; c < 3; ++c) {
            uint32_t x = a.values[i * 4 + c];
            uint32_t y = b.values[i * 4 + c];
            uint32_t delta = x > y ? x - y : y - x;
            largest = delta > largest ? delta : largest;
            luma += delta * kLuma[c];
        }
        if (luma >> 16 == 0)
            continue;
        d.maxDifference = largest > d.maxDifference ? largest : d.maxDifference;
        ++d.differingPixels;
    }
    imageFree(a);
    imageFree(b);
    return d;
}

bool imageDrawsAnything(const uint8_t* rgba, uint32_t width, uint32_t height, size_t stride) {
    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t* row = rgba + size_t(y) * stride;
        for (uint32_t x = 0; x < width; ++x) {
            const uint8_t* px = row + size_t(x) * 4;
            for (uint32_t c = 0; c < 3; ++c) {
                if (px[3] != 0 && px[c] < 253)
                    return true;
            }
        }
    }
    return false;
}

void straightenBgra(uint8_t* pixels, uint32_t width, uint32_t height, size_t stride) {
    for (uint32_t y = 0; y < height; ++y) {
        uint8_t* row = pixels + size_t(y) * stride;
        for (uint32_t x = 0; x < width; ++x) {
            uint8_t* px = row + size_t(x) * 4;
            uint32_t a = px[3];
            uint8_t rgb[3];
            for (uint32_t c = 0; c < 3; ++c)
                rgb[c] = uint8_t(a == 0 ? 0 : (uint32_t(px[2 - c]) * 255 + a / 2) / a);
            px[0] = rgb[0];
            px[1] = rgb[1];
            px[2] = rgb[2];
        }
    }
}

} // namespace harness
