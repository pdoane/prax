// QOI decoding over images that an encoder here writes, as the specification's reference encoder chooses its chunks.

#include "pr/array.h"
#include "pr/mem.h"
#include "pr/qoi.h"

#include "tools/harness/test.h"

extern "C" int prTestQoiCApi(void);

namespace pr_impl {

namespace {

struct Rgba {
    uint8_t r;
    uint8_t g;
    uint8_t b;
    uint8_t a;
    bool operator==(const Rgba& o) const { return r == o.r && g == o.g && b == o.b && a == o.a; }
};

uint32_t hashOf(Rgba p) {
    return (uint32_t(p.r) * 3 + uint32_t(p.g) * 5 + uint32_t(p.b) * 7 + uint32_t(p.a) * 11) % 64;
}

void putBe32(Array<uint8_t>* out, uint32_t v) {
    out->push(uint8_t(v >> 24));
    out->push(uint8_t(v >> 16));
    out->push(uint8_t(v >> 8));
    out->push(uint8_t(v));
}

// Returns the header of an image.
Array<uint8_t> header(uint32_t width, uint32_t height, uint8_t channels, uint8_t colorspace) {
    Array<uint8_t> out({'q', 'o', 'i', 'f'});
    putBe32(&out, width);
    putBe32(&out, height);
    out.push(channels);
    out.push(colorspace);
    return out;
}

// Returns a file of `pixels`, coded with every kind of chunk where it fits, and the end marker.
Array<uint8_t> encode(const Array<Rgba>& pixels, uint32_t width, uint32_t height, uint8_t channels, uint8_t colorspace = 0) {
    Array<uint8_t> out = header(width, height, channels, colorspace);
    Rgba seen[64] = {};
    Rgba before{0, 0, 0, 255};
    uint32_t run = 0;
    for (uint32_t i = 0; i < pixels.count(); ++i) {
        const Rgba p = pixels[i];
        if (p == before) {
            ++run;
            if (run == 62 || i + 1 == pixels.count()) {
                out.push(uint8_t(0xc0 | (run - 1)));
                run = 0;
            }
            continue;
        }
        if (run != 0) {
            out.push(uint8_t(0xc0 | (run - 1)));
            run = 0;
        }
        const uint32_t hash = hashOf(p);
        if (seen[hash] == p) {
            out.push(uint8_t(hash));
        } else {
            seen[hash] = p;
            if (p.a == before.a) {
                const int32_t dr = int8_t(uint8_t(p.r - before.r));
                const int32_t dg = int8_t(uint8_t(p.g - before.g));
                const int32_t db = int8_t(uint8_t(p.b - before.b));
                const int32_t drg = dr - dg;
                const int32_t dbg = db - dg;
                if (dr >= -2 && dr <= 1 && dg >= -2 && dg <= 1 && db >= -2 && db <= 1) {
                    out.push(uint8_t(0x40 | (dr + 2) << 4 | (dg + 2) << 2 | (db + 2)));
                } else if (drg >= -8 && drg <= 7 && dg >= -32 && dg <= 31 && dbg >= -8 && dbg <= 7) {
                    out.push(uint8_t(0x80 | (dg + 32)));
                    out.push(uint8_t((drg + 8) << 4 | (dbg + 8)));
                } else {
                    out.push(0xfe);
                    out.push(p.r);
                    out.push(p.g);
                    out.push(p.b);
                }
            } else {
                out.push(0xff);
                out.push(p.r);
                out.push(p.g);
                out.push(p.b);
                out.push(p.a);
            }
        }
        before = p;
    }
    const uint8_t end[8] = {0, 0, 0, 0, 0, 0, 0, 1};
    out.append(PrByteView{end, 8});
    return out;
}

// A picture that every kind of chunk codes: runs, repeats of pixels from before, small and larger differences, and
// changes of alpha.
Array<Rgba> samplePixels(uint32_t count) {
    Array<Rgba> pixels;
    uint32_t state = 12345;
    for (uint32_t i = 0; i < count; ++i) {
        state = state * 1103515245 + 12345;
        const uint32_t kind = (state >> 16) % 6;
        Rgba p = i == 0 ? Rgba{0, 0, 0, 255} : pixels[i - 1];
        if (kind == 1) {
            p.r = uint8_t(p.r + 1);
            p.b = uint8_t(p.b - 2);
        } else if (kind == 2) {
            p.g = uint8_t(p.g + 20);
            p.r = uint8_t(p.r + 23);
        } else if (kind == 3) {
            p = Rgba{uint8_t(state >> 8), uint8_t(state >> 12), uint8_t(state >> 20), p.a};
        } else if (kind == 4) {
            p.a = uint8_t(state >> 24);
        } else if (kind == 5 && i >= 7) {
            p = pixels[i - 7];
        }
        pixels.push(p);
    }
    return pixels;
}

struct Decoded {
    PrStatus status = PrStatus_Ok;
    PrQoiInfo info{};
    Array<uint8_t> pixels;
};

Decoded decodeQoi(const Array<uint8_t>& file, const PrQoiLimits* limits = nullptr) {
    Decoded decoded;
    PrQoiDecoder* decoder = nullptr;
    decoded.status = prQoiDecoderCreate(PrByteView{file.data(), file.count()}, limits, file.count(), &decoder);
    if (decoded.status != PrStatus_Ok)
        return decoded;
    decoded.info = *prQoiDecoderInfo(decoder);
    const PrUVec2 extent = decoded.info.extent;
    const size_t stride = size_t(extent.x) * decoded.info.channels;
    decoded.pixels.resize(uint32_t(stride * extent.y));
    decoded.status = prQoiDecoderRead(decoder, PrPixels{decoded.pixels.data(), stride, extent});
    prQoiDecoderDestroy(decoder);
    return decoded;
}

// Returns the pixels as `channels` bytes each.
Array<uint8_t> packed(const Array<Rgba>& pixels, uint32_t channels) {
    Array<uint8_t> out;
    for (const Rgba& p : pixels) {
        out.push(p.r);
        out.push(p.g);
        out.push(p.b);
        if (channels == 4)
            out.push(p.a);
    }
    return out;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a qoi of every kind of chunk decodes to its pixels") {
    const uint32_t width = 23;
    const uint32_t height = 17;
    Array<Rgba> pixels = samplePixels(width * height);
    Array<uint8_t> file = encode(pixels, width, height, 4);
    CHECK(prQoiHasSignature(PrByteView{file.data(), file.count()}));
    Decoded decoded = decodeQoi(file);
    REQUIRE(decoded.status == PrStatus_Ok);
    CHECK(decoded.info.extent == PrUVec2{width, height});
    CHECK(decoded.info.channels == 4);
    CHECK(decoded.pixels == packed(pixels, 4));
}

TEST_CASE("a qoi of three channels decodes to rgb, and its colorspace is reported") {
    Array<Rgba> pixels = samplePixels(64);
    for (Rgba& p : pixels)
        p.a = 255;
    Array<uint8_t> file = encode(pixels, 8, 8, 3, 1);
    Decoded decoded = decodeQoi(file);
    REQUIRE(decoded.status == PrStatus_Ok);
    CHECK(decoded.info.channels == 3);
    CHECK(decoded.info.colorspace == PrQoiColorspace_Linear);
    CHECK(decoded.pixels == packed(pixels, 3));
}

TEST_CASE("a run of 62 and runs across rows decode") {
    Array<Rgba> pixels;
    for (uint32_t i = 0; i < 200; ++i)
        pixels.push(i < 130 ? Rgba{9, 8, 7, 255} : Rgba{1, 2, 3, 4});
    Array<uint8_t> file = encode(pixels, 10, 20, 4);
    Decoded decoded = decodeQoi(file);
    REQUIRE(decoded.status == PrStatus_Ok);
    CHECK(decoded.pixels == packed(pixels, 4));
}

TEST_CASE("a qoi whose chunks end early is truncated, and the rest is transparent black") {
    Array<Rgba> pixels = samplePixels(16);
    for (Rgba& p : pixels)
        p = Rgba{200, 100, 50, 255};
    pixels[0] = Rgba{10, 20, 30, 255};
    Array<uint8_t> file = header(4, 4, 4, 0);
    // The first pixel, then the start of an RGB chunk that the end cuts short.
    const uint8_t chunks[] = {0xfe, 10, 20, 30, 0xfe, 200};
    file.append(PrByteView{chunks, sizeof(chunks)});
    Decoded decoded = decodeQoi(file);
    CHECK(decoded.status == PrStatus_Truncated);
    REQUIRE(decoded.pixels.count() == 64);
    CHECK(decoded.pixels[0] == 10);
    CHECK(decoded.pixels[3] == 255);
    for (uint32_t i = 4; i < 64; ++i)
        CHECK(decoded.pixels[i] == 0);
}

TEST_CASE("a run past the last pixel is invalid") {
    Array<uint8_t> file = header(2, 2, 4, 0);
    file.push(0xc0 | 4);
    CHECK(decodeQoi(file).status == PrStatus_InvalidData);
}

TEST_CASE("a malformed qoi header does not open") {
    Array<uint8_t> file = header(2, 2, 4, 0);
    file[12] = 5;
    CHECK(decodeQoi(file).status == PrStatus_InvalidData);
    file = header(2, 2, 4, 2);
    CHECK(decodeQoi(file).status == PrStatus_InvalidData);
    file = header(0, 2, 4, 0);
    CHECK(decodeQoi(file).status == PrStatus_InvalidData);
    file = header(2, 2, 4, 0);
    file.resize(13);
    CHECK(decodeQoi(file).status == PrStatus_InvalidData);
    file[0] = 'Q';
    CHECK(decodeQoi(file).status == PrStatus_NotSupported);
}

TEST_CASE("a qoi past the limits does not open") {
    Array<uint8_t> file = header(100, 100, 4, 0);
    PrQoiLimits limits = prQoiLimitsDefault();
    limits.pixels = 9999;
    CHECK(decodeQoi(file, &limits).status == PrStatus_Limit);
    limits = prQoiLimitsDefault();
    limits.outputBytes = 39999;
    CHECK(decodeQoi(file, &limits).status == PrStatus_Limit);
    limits = prQoiLimitsDefault();
    limits.work = 9999;
    CHECK(decodeQoi(file, &limits).status == PrStatus_Limit);
    limits = prQoiLimitsDefault();
    limits.workingBytes = file.count();
    CHECK(decodeQoi(file, &limits).status == PrStatus_Limit);
}

TEST_CASE("the qoi header compiles as C") {
    CHECK(prTestQoiCApi() == 0);
}
