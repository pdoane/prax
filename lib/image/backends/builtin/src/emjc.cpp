// emjc decoding for the built-in codec.
//
// An image is a 16-byte header, then an LZFSE stream of four planes: the alpha of each pixel, a filter for each row,
// three residuals for each pixel, and an appendix. A pixel's color is coded as (base, p, q), a reversible transform of
// (r, g, b) in the manner of YCoCg-R: p = r - b, q = g - b - p / 2 with the half rounded toward zero, and base = g - q
// / 2 with the half rounded away from zero. A row's filter predicts each of its pixel's components from the pixels left
// of and above it, and the residuals are the differences from the predictions. A residual's byte holds its magnitude
// above the offset of its component, doubled, with the low bit set for a negative one. The appendix gives the offsets
// of the components whose residuals exceed what a byte holds: each of its bytes skips its high six bits' worth of
// components, then gives the next component an offset of 128 times its low two bits.

#include "image/backends/builtin/src/emjc.h"

#include "pr/alloc.h"
#include "pr/mem.h"

namespace pr_impl {

namespace {

constexpr uint8_t kSignature[4] = {'e', 'm', 'j', '1'};
constexpr uint32_t kHeaderSize = 16;

// The filters of a row. Each predicts a component from the same component of the pixels left of it and above it, and
// uses whichever of them is there at the image's edges.
enum {
    EmjcFilter_None,
    EmjcFilter_Gradient,    // the left pixel if the base changes less to it from the pixel above left, else the upper
    EmjcFilter_Left,
    EmjcFilter_Up,
    EmjcFilter_Average,     // the mean of left and upper, rounded toward zero from a half above
    EmjcFilter_Count,
};
typedef uint8_t EmjcFilter;

// The components whose residuals have an offset, read from the appendix in order.
struct Offsets {
    const uint8_t* at;
    const uint8_t* end;
    uint64_t next;          // the component of the next offset, or past the last component once the appendix ends
    int32_t value;
};

void advance(Offsets& o, uint64_t start) {
    o.next = ~uint64_t(0);
    if (o.at == o.end)
        return;
    o.next = start + (*o.at >> 2);
    o.value = 128 * (*o.at & 3);
    ++o.at;
}

// Returns the residual of component `index`, whose byte is `coded`.
inline int32_t residual(Offsets& o, uint64_t index, uint8_t coded) {
    int32_t offset = 0;
    if (index == o.next) {
        offset = o.value;
        advance(o, index + 1);
    }
    const int32_t magnitude = coded >> 1;
    return coded & 1 ? -magnitude - offset : magnitude + offset;
}

inline int32_t absolute(int32_t value) {
    return value < 0 ? -value : value;
}

// Returns half of `value` rounded toward zero, and half rounded away from it. The two sum to `value`.
inline int32_t halfToward(int32_t value) {
    return value / 2;
}
inline int32_t halfAway(int32_t value) {
    return value - value / 2;
}

} // namespace

bool emjcHasSignature(PrByteView bytes) {
    return bytes.size >= sizeof(kSignature) && prMemEquals(bytes.data, kSignature, sizeof(kSignature));
}

PrStatus emjcImage(PrByteView bytes, EmjcImage* out) {
    if (bytes.size < kHeaderSize || !emjcHasSignature(bytes))
        return PrStatus_InvalidData;
    out->width = prMemReadLeU16(bytes.data + 8);
    out->height = prMemReadLeU16(bytes.data + 10);
    out->appendixSize = prMemReadLeU16(bytes.data + 12);
    out->stream = PrByteView{bytes.data + kHeaderSize, bytes.size - kHeaderSize};
    return out->width != 0 && out->height != 0 ? PrStatus_Ok : PrStatus_InvalidData;
}

uint64_t emjcPlanesSize(const EmjcImage& image) {
    const uint64_t pixels = uint64_t(image.width) * image.height;
    return pixels * 4 + image.height + image.appendixSize;
}

bool emjcDecode(PrCompressCodec* codec, const EmjcImage& image, uint8_t* pixels, size_t pixelsSize) {
    const uint32_t width = image.width;
    const uint32_t height = image.height;
    const size_t count = size_t(width) * height;
    if (pixelsSize != count * 4)
        return false;
    const size_t planesSize = size_t(emjcPlanesSize(image));
    uint8_t* planes = static_cast<uint8_t*>(prMemoryAlloc(planesSize + 1));
    PrCompressRead read{};
    const PrStatus status = prCompressCodecDecode(codec, PrCompressFormat_Lzfse, image.stream, planes, planesSize + 1, &read);
    if (status != PrStatus_Ok || read.progress != PrCompressProgress_Done || read.written != planesSize) {
        prMemoryFree(planes);
        return false;
    }
    const uint8_t* alpha = planes;
    const uint8_t* filters = alpha + count;
    const uint8_t* residuals = filters + height;
    Offsets offsets{residuals + count * 3, residuals + count * 3 + image.appendixSize, 0, 0};
    advance(offsets, 0);

    // The (base, p, q) of the row above and of this row.
    int32_t* rows = static_cast<int32_t*>(prMemoryAlloc(sizeof(int32_t) * 6 * width));
    int32_t* above = rows;
    int32_t* row = rows + 3 * width;
    bool valid = true;
    for (uint32_t y = 0; y < height && valid; ++y) {
        const EmjcFilter filter = filters[y];
        valid = filter < EmjcFilter_Count;
        for (uint32_t x = 0; x < width && valid; ++x) {
            const size_t index = size_t(y) * width + x;
            int32_t* c = row + 3 * x;
            for (uint32_t k = 0; k < 3; ++k)
                c[k] = residual(offsets, index * 3 + k, residuals[index * 3 + k]);
            const int32_t* left = x > 0 ? c - 3 : nullptr;
            const int32_t* up = y > 0 ? above + 3 * x : nullptr;
            const int32_t* from = nullptr;
            if (filter == EmjcFilter_Left)
                from = left;
            else if (filter == EmjcFilter_Up)
                from = up;
            else if (filter != EmjcFilter_None)
                from = left != nullptr ? left : up;
            if (filter == EmjcFilter_Gradient && left != nullptr && up != nullptr) {
                const int32_t corner = up[-3];
                if (absolute(left[0] - corner) < absolute(up[0] - corner))
                    from = up;
            }
            if (filter == EmjcFilter_Average && left != nullptr && up != nullptr) {
                for (uint32_t k = 0; k < 3; ++k)
                    c[k] += (left[k] + up[k] + 1) / 2;
            } else if (from != nullptr) {
                for (uint32_t k = 0; k < 3; ++k)
                    c[k] += from[k];
            }
            const int32_t base = c[0];
            const int32_t p = c[1];
            const int32_t q = c[2];
            uint8_t* out = pixels + index * 4;
            // Components out of range, which no image that the transform made has, wrap.
            out[0] = uint8_t(base + halfAway(p) - halfToward(q));
            out[1] = uint8_t(base + halfAway(q));
            out[2] = uint8_t(base - halfToward(p) - halfToward(q));
            out[3] = alpha[index];
        }
        int32_t* swap = above;
        above = row;
        row = swap;
    }
    prMemoryFree(rows);
    prMemoryFree(planes);
    return valid;
}

} // namespace pr_impl
