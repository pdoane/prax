// OpenEXR files that tests write, attribute by attribute: scan lines without compression, of the channels, windows and
// chromaticities that a test gives, each channel's value at a pixel known.

#pragma once

#include "pr/array.h"
#include "pr/base.h"
#include "pr/float16.h"
#include "pr/mem.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

using namespace pr;

// A channel of a test file: its name, pixel type (0 uint, 1 half, 2 float) and sampling.
struct TestChannel {
    const char* name;
    uint32_t pixel;
    uint32_t sampling = 1;
};

// Writes OpenEXR files of scan lines without compression.
struct ExrWriter {
    Array<uint8_t> bytes;

    void putByte(uint8_t value) {
        bytes.push(value);
    }

    void putWord(uint32_t value) {
        uint8_t b[4];
        prMemWriteLeU32(b, value);
        bytes.append(PrByteView{b, 4});
    }

    void putLong(uint64_t value) {
        uint8_t b[8];
        prMemWriteLeU64(b, value);
        bytes.append(PrByteView{b, 8});
    }

    void putFloat(float value) {
        uint32_t bits = 0;
        prMemCopy(&bits, &value, 4);
        putWord(bits);
    }

    void text(const char* value) {
        bytes.append(PrByteView{reinterpret_cast<const uint8_t*>(value), strlenOf(value) + 1});
    }

    static size_t strlenOf(const char* value) {
        size_t n = 0;
        while (value[n] != 0)
            ++n;
        return n;
    }

    // Writes an attribute's name, type and size; its value follows.
    void attribute(const char* name, const char* type, uint32_t size) {
        text(name);
        text(type);
        putWord(size);
    }

    void box(const char* name, int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
        attribute(name, "box2i", 16);
        putWord(uint32_t(x0));
        putWord(uint32_t(y0));
        putWord(uint32_t(x1));
        putWord(uint32_t(y1));
    }

    void channels(const TestChannel* list, uint32_t count) {
        uint32_t size = 1;
        for (uint32_t i = 0; i < count; ++i)
            size += uint32_t(strlenOf(list[i].name)) + 1 + 16;
        attribute("channels", "chlist", size);
        for (uint32_t i = 0; i < count; ++i) {
            text(list[i].name);
            putWord(list[i].pixel);
            putWord(0);
            putWord(list[i].sampling);
            putWord(list[i].sampling);
        }
        putByte(0);
    }

    void required(uint8_t compression) {
        attribute("compression", "compression", 1);
        putByte(compression);
        attribute("lineOrder", "lineOrder", 1);
        putByte(0);
        attribute("pixelAspectRatio", "float", 4);
        putFloat(1);
        attribute("screenWindowCenter", "v2f", 8);
        putFloat(0);
        putFloat(0);
        attribute("screenWindowWidth", "float", 4);
        putFloat(1);
    }
};

// A scan line image to write: its channels, windows, and each channel's value at a pixel.
struct TestImage {
    const TestChannel* channels;
    uint32_t channelCount;
    int32_t data[4];
    int32_t display[4];
    bool chromaticities = false;
    float primaries[8] = {};
};

// The value of channel `c` at pixel (x, y), in the part's coordinates.
inline float sampleValue(uint32_t c, int32_t x, int32_t y) {
    return float(c + 1) + float(x) * 0.25f + float(y) * 0.0625f;
}

// Appends one sample of `pixel` of `value` to `out`.
inline void appendSample(Array<uint8_t>* out, uint32_t pixel, float value) {
    uint8_t b[4];
    if (pixel == 0) {
        prMemWriteLeU32(b, uint32_t(value * 16));
        out->append(PrByteView{b, 4});
    } else if (pixel == 1) {
        PrFloat16 h = prFloat16FromFloat(value);
        prMemCopy(b, &h, 2);
        out->append(PrByteView{b, 2});
    } else {
        prMemCopy(b, &value, 4);
        out->append(PrByteView{b, 4});
    }
}

// Writes the scan line file of `image`, one line a chunk, without compression. `missing` chunks of the last are left
// out, as a file whose writing stopped. Writes where the offset table starts to `*tableAt` if it is not null.
inline Array<uint8_t> writeImage(const TestImage& image, uint32_t missing = 0, size_t* PR_NULLABLE tableAt = nullptr) {
    ExrWriter w;
    w.putWord(20000630);
    w.putWord(2);
    w.channels(image.channels, image.channelCount);
    w.required(0);
    w.box("dataWindow", image.data[0], image.data[1], image.data[2], image.data[3]);
    w.box("displayWindow", image.display[0], image.display[1], image.display[2], image.display[3]);
    if (image.chromaticities) {
        w.attribute("chromaticities", "chromaticities", 32);
        for (float value : image.primaries)
            w.putFloat(value);
    }
    w.putByte(0);
    uint32_t lines = uint32_t(image.data[3] - image.data[1] + 1);
    size_t table = w.bytes.count();
    if (tableAt != nullptr)
        *tableAt = table;
    for (uint32_t i = 0; i < lines; ++i)
        w.putLong(0);
    for (uint32_t i = 0; i + missing < lines; ++i) {
        int32_t y = image.data[1] + int32_t(i);
        uint64_t offset = w.bytes.count();
        prMemWriteLeU64(w.bytes.data() + table + i * 8, offset);
        Array<uint8_t> pixels;
        for (uint32_t c = 0; c < image.channelCount; ++c) {
            const TestChannel& channel = image.channels[c];
            if (y % int32_t(channel.sampling) != 0)
                continue;
            for (int32_t x = image.data[0]; x <= image.data[2]; x += int32_t(channel.sampling))
                appendSample(&pixels, channel.pixel, sampleValue(c, x, y));
        }
        w.putWord(uint32_t(y));
        w.putWord(pixels.count());
        w.bytes.append(pixels.span());
    }
    return static_cast<Array<uint8_t>&&>(w.bytes);
}

} // namespace pr_impl

PR_ASSUME_NONNULL_END
