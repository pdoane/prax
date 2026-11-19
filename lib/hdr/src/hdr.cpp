// Radiance HDR pictures.

#include "pr/hdr.h"

#include "pr/alloc.h"
#include "pr/assert.h"
#include "pr/float.h"
#include "pr/mem.h"
#include "pr/string_view.h"

namespace pr_impl {

namespace {

// The widths of a scanline that the newer run-length encoding stores.
constexpr uint32_t kMinRunWidth = 8;
constexpr uint32_t kMaxRunWidth = 0x7fff;

// The exponent that scales a mantissa to its own value: a pixel's value is its mantissa times two to the power of its
// exponent less this.
constexpr int32_t kExponentBias = 128 + 8;

// Returns the line of `bytes` at `*at`, without its line feed, and moves `*at` past it. Returns false if no line feed
// ends the line.
bool nextLine(PrByteView bytes, size_t* at, PrStringView* line) {
    size_t start = *at;
    for (size_t i = start; i < bytes.size; ++i) {
        if (bytes.data[i] != '\n')
            continue;
        *line = PrStringView{reinterpret_cast<const char*>(bytes.data) + start, i - start};
        *at = i + 1;
        return true;
    }
    return false;
}

bool startsWith(PrStringView text, PrStringView prefix) {
    return text.size >= prefix.size && prMemEquals(text.data, prefix.data, prefix.size);
}

// Returns the value of a header line of the form NAME=value, or a null view for any other line.
PrStringView variableValue(PrStringView line, PrStringView name) {
    if (!startsWith(line, name) || line.size == name.size || line.data[name.size] != '=')
        return PrStringView{};
    return PrStringView{line.data + name.size + 1, line.size - name.size - 1};
}

PrStringView trimmed(PrStringView text) {
    while (text.size != 0 && (text.data[0] == ' ' || text.data[0] == '\t')) {
        ++text.data;
        --text.size;
    }
    while (text.size != 0 && (text.data[text.size - 1] == ' ' || text.data[text.size - 1] == '\t' ||
            text.data[text.size - 1] == '\r'))
        --text.size;
    return text;
}

// Returns true if `text` is a decimal number as prF64Parse reads one: digits with an optional sign, fraction and
// exponent.
bool isNumber(PrStringView text) {
    size_t i = 0;
    if (i < text.size && (text.data[i] == '+' || text.data[i] == '-'))
        ++i;
    size_t digits = 0;
    for (; i < text.size && text.data[i] >= '0' && text.data[i] <= '9'; ++i)
        ++digits;
    if (i < text.size && text.data[i] == '.') {
        for (++i; i < text.size && text.data[i] >= '0' && text.data[i] <= '9'; ++i)
            ++digits;
    }
    if (digits == 0)
        return false;
    if (i < text.size && (text.data[i] == 'e' || text.data[i] == 'E')) {
        ++i;
        if (i < text.size && (text.data[i] == '+' || text.data[i] == '-'))
            ++i;
        size_t exponentDigits = 0;
        for (; i < text.size && text.data[i] >= '0' && text.data[i] <= '9'; ++i)
            ++exponentDigits;
        if (exponentDigits == 0)
            return false;
    }
    return i == text.size;
}

// Reads an axis from the start of `*text`, such as "-Y 512", and moves `*text` past it. Returns false for anything
// else, or a length that does not fit.
bool readAxis(PrStringView* text, PrHdrAxis* out) {
    PrStringView t = trimmed(*text);
    if (t.size < 4 || (t.data[0] != '+' && t.data[0] != '-') || (t.data[1] != 'X' && t.data[1] != 'Y') || t.data[2] != ' ')
        return false;
    out->decreasing = t.data[0] == '-';
    out->y = t.data[1] == 'Y';
    size_t i = 3;
    while (i < t.size && t.data[i] == ' ')
        ++i;
    uint64_t length = 0;
    size_t digits = 0;
    for (; i < t.size && t.data[i] >= '0' && t.data[i] <= '9'; ++i, ++digits) {
        length = length * 10 + uint64_t(t.data[i] - '0');
        if (length > 0x7fffffffu)
            return false;
    }
    if (digits == 0)
        return false;
    out->length = uint32_t(length);
    *text = PrStringView{t.data + i, t.size - i};
    return true;
}

// Reads one scanline of `width` pixels in the newer run-length encoding, after its four marker bytes, into `row` as
// four bytes a pixel. Each of the four components is stored for the whole scanline in turn, as runs: a count above 128
// repeats the next byte that many times less 128, and any other count copies that many bytes. Returns false if the
// bytes end early, a count is zero, or a run passes the end of the scanline.
bool readRuns(PrByteView bytes, size_t* at, uint32_t width, uint8_t* row) {
    for (uint32_t component = 0; component < 4; ++component) {
        uint32_t x = 0;
        while (x < width) {
            if (*at >= bytes.size)
                return false;
            uint32_t count = bytes.data[(*at)++];
            bool repeats = count > 128;
            if (repeats)
                count -= 128;
            size_t needed = repeats ? 1 : count;
            if (count == 0 || count > width - x || needed > bytes.size - *at)
                return false;
            for (uint32_t i = 0; i < count; ++i)
                row[(x + i) * 4 + component] = bytes.data[*at + (repeats ? 0 : i)];
            *at += needed;
            x += count;
        }
    }
    return true;
}

// Reads one scanline of `width` pixels stored as four bytes a pixel, into `row`. In the older run-length encoding, a
// pixel whose mantissas are all one repeats the pixel before it by the count in its exponent, and each such pixel that
// follows another shifts its count by eight more bits. Returns false if the bytes end early, a repeat has no pixel
// before it in the scanline, or a repeat passes the end of the scanline.
bool readFlat(PrByteView bytes, size_t* at, uint32_t width, uint8_t* row) {
    uint32_t shift = 0;
    for (uint32_t x = 0; x < width;) {
        if (bytes.size - *at < 4)
            return false;
        const uint8_t* pixel = bytes.data + *at;
        *at += 4;
        if (pixel[0] != 1 || pixel[1] != 1 || pixel[2] != 1) {
            prMemCopy(row + size_t(x) * 4, pixel, 4);
            ++x;
            shift = 0;
            continue;
        }
        if (x == 0 || shift > 24)
            return false;
        uint64_t count = uint64_t(pixel[3]) << shift;
        if (count > width - x)
            return false;
        for (uint64_t i = 0; i < count; ++i, ++x)
            prMemCopy(row + size_t(x) * 4, row + size_t(x - 1) * 4, 4);
        shift += 8;
    }
    return true;
}

// A picture, as its header and its resolution line describe it, and the bytes of its scanlines.
struct HdrPicture {
    PrByteView scanlines;   // the bytes after the resolution line
    PrUVec2 extent;
    PrHdrAxis scanlineAxis;
    PrHdrAxis pixelAxis;
    double exposure;
};

// Reads the header and the resolution line of the picture in `bytes`.
PrStatus readPicture(PrByteView bytes, HdrPicture* out) {
    *out = HdrPicture{.exposure = 1};
    size_t at = 0;
    PrStringView line{};
    if (!nextLine(bytes, &at, &line) || (trimmed(line) != "#?RADIANCE" && trimmed(line) != "#?RGBE"))
        return PrStatus_InvalidData;
    // The header's lines, up to an empty one. A line that is not a variable this decoder reads is skipped.
    for (;;) {
        if (!nextLine(bytes, &at, &line))
            return PrStatus_InvalidData;
        line = trimmed(line);
        if (line.size == 0)
            break;
        PrStringView format = variableValue(line, "FORMAT");
        if (format.data != nullptr && trimmed(format) != "32-bit_rle_rgbe")
            return trimmed(format) == "32-bit_rle_xyze" ? PrStatus_NotSupported : PrStatus_InvalidData;
        PrStringView exposure = trimmed(variableValue(line, "EXPOSURE"));
        if (exposure.data != nullptr && isNumber(exposure)) {
            double value = prF64Parse(exposure.data, exposure.size);
            // A header may hold several exposures, which multiply. One that is not above zero says nothing.
            if (value > 0 && prF64IsFinite(value))
                out->exposure *= value;
        }
    }
    if (!(out->exposure > 0) || !prF64IsFinite(out->exposure))
        out->exposure = 1;

    if (!nextLine(bytes, &at, &line))
        return PrStatus_InvalidData;
    PrHdrAxis major{};
    PrHdrAxis minor{};
    if (!readAxis(&line, &major) || !readAxis(&line, &minor) || trimmed(line).size != 0 || major.y == minor.y ||
            major.length == 0 || minor.length == 0)
        return PrStatus_InvalidData;
    out->extent = PrUVec2{minor.length, major.length};
    out->scanlineAxis = major;
    out->pixelAxis = minor;
    out->scanlines = PrByteView{bytes.data + at, bytes.size - at};
    return PrStatus_Ok;
}

// Decodes `image` into `pixels`. Returns false if its scanlines end early or are malformed.
bool decodePicture(const HdrPicture* image, PrPixels pixels) {
    uint32_t width = image->extent.x;
    PR_ASSERT(pixels.extent.x == width && pixels.extent.y == image->extent.y);
    PR_ASSERT(pixels.stride >= size_t(width) * 3 * sizeof(float));
    // The value of a mantissa of one at each exponent, over the exposure. Halving and doubling are exact.
    double scales[256];
    scales[kExponentBias] = 1;
    for (int32_t e = kExponentBias + 1; e < 256; ++e)
        scales[e] = scales[e - 1] * 2;
    for (int32_t e = kExponentBias - 1; e >= 0; --e)
        scales[e] = scales[e + 1] * 0.5;

    uint8_t* row = static_cast<uint8_t*>(prMemoryAlloc(size_t(width) * 4));
    PrByteView bytes = image->scanlines;
    size_t at = 0;
    bool ok = true;
    for (uint32_t y = 0; y < image->extent.y && ok; ++y) {
        // A scanline in the newer encoding begins with two bytes of 2 and its width, high byte first. Any other
        // scanline is flat.
        bool runs = width >= kMinRunWidth && width <= kMaxRunWidth && bytes.size - at >= 4 &&
            bytes.data[at] == 2 && bytes.data[at + 1] == 2 && (bytes.data[at + 2] & 0x80) == 0;
        if (runs) {
            uint32_t marked = uint32_t(bytes.data[at + 2]) << 8 | bytes.data[at + 3];
            at += 4;
            ok = marked == width && readRuns(bytes, &at, width, row);
        } else {
            ok = readFlat(bytes, &at, width, row);
        }
        if (!ok)
            break;
        uint8_t* out = pixels.data + size_t(y) * pixels.stride;
        for (uint32_t x = 0; x < width; ++x) {
            const uint8_t* pixel = row + size_t(x) * 4;
            float color[3] = {0, 0, 0};
            if (pixel[3] != 0) {
                double scale = scales[pixel[3]] / image->exposure;
                for (uint32_t channel = 0; channel < 3; ++channel) {
                    double value = (double(pixel[channel]) + 0.5) * scale;
                    color[channel] = value < double(kPrF32Max) ? float(value) : kPrF32Max;
                }
            }
            prMemCopy(out + size_t(x) * sizeof(color), color, sizeof(color));
        }
    }
    prMemoryFree(row);
    return ok;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

// C opaque types
struct PrHdrDecoder {
    HdrPicture picture;
    PrHdrInfo info;
};

bool prHdrHasSignature(PrByteView bytes) {
    const char radiance[] = {'#', '?', 'R', 'A', 'D', 'I', 'A', 'N'};
    const char rgbe[] = {'#', '?', 'R', 'G', 'B', 'E'};
    return (bytes.size >= sizeof(radiance) && prMemEquals(bytes.data, radiance, sizeof(radiance))) ||
        (bytes.size >= sizeof(rgbe) && prMemEquals(bytes.data, rgbe, sizeof(rgbe)));
}

PrHdrLimits prHdrLimitsDefault(void) {
    return PrHdrLimits{
        .pixels = 64u * 1024u * 1024u,
        .workingBytes = 512u * 1024u * 1024u,
        .outputBytes = 512u * 1024u * 1024u,
        .work = 1000000000u,
    };
}

PrStatus prHdrDecoderCreate(PrByteView bytes, const PrHdrLimits* PR_NULLABLE limits, uint64_t held,
        PrHdrDecoder* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    const PrHdrLimits bounds = limits != nullptr ? *limits : prHdrLimitsDefault();
    HdrPicture picture{};
    PrStatus status = readPicture(bytes, &picture);
    if (status != PrStatus_Ok)
        return status;
    // A read holds one scanline's components, four bytes a pixel of a scanline, and writes twelve bytes a pixel.
    const uint64_t pixels = uint64_t(picture.extent.x) * picture.extent.y;
    const uint64_t working = held + sizeof(PrHdrDecoder) + 4 * uint64_t(picture.extent.x);
    if (pixels > bounds.pixels || pixels > bounds.work || pixels * 12 > bounds.outputBytes ||
            working > bounds.workingBytes)
        return PrStatus_Limit;
    PrHdrDecoder* decoder = allocZeroed<PrHdrDecoder>();
    decoder->picture = picture;
    decoder->info = PrHdrInfo{
        .extent = picture.extent,
        .scanlineAxis = picture.scanlineAxis,
        .pixelAxis = picture.pixelAxis,
        .exposure = picture.exposure,
        .workingBytes = working,
    };
    *out = decoder;
    return PrStatus_Ok;
}

void prHdrDecoderDestroy(PrHdrDecoder* PR_NULLABLE decoder) {
    prMemoryFree(decoder);
}

const PrHdrInfo* prHdrDecoderInfo(const PrHdrDecoder* decoder) {
    return &decoder->info;
}

PrStatus prHdrDecoderRead(PrHdrDecoder* decoder, PrPixels output) {
    return decodePicture(&decoder->picture, output) ? PrStatus_Ok : PrStatus_InvalidData;
}
