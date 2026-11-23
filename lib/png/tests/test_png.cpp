#include "pr/array.h"
#include "pr/checksum.h"
#include "pr/compress_deflate.h"
#include "pr/mem.h"
#include "pr/png.h"

#include "tools/harness/test.h"

extern "C" int prTestPngCApi(void);

namespace pr_impl {

namespace {

using namespace pr;

// A set of the Deflate codec alone, which is all that PNG needs.
struct Compress {
    PrCompressCodecSet* set;

    Compress() {
        PrCompressCodec* deflate = prCompressDeflateCreate();
        set = prCompressCodecSetCreate(&deflate, 1);
    }
    ~Compress() { prCompressCodecSetDestroy(set); }
};

// Encodes `width` by `height` pixels of RGBA whose bytes count up from `seed`, with `flags`.
Array<uint8_t> encode(const Compress& compress, uint32_t width, uint32_t height, uint8_t seed, Array<uint8_t>* pixels,
        PrPngEncodeFlags flags = 0) {
    pixels->resize(width * height * 4);
    for (uint32_t i = 0; i < pixels->count(); ++i)
        (*pixels)[i] = uint8_t(seed + i);
    PrPixelView view{.data = pixels->data(), .stride = size_t(width) * 4, .extent = PrUVec2{width, height}};
    Array<uint8_t> out(uint32_t(prPngEncode(view, flags, compress.set, nullptr, 0)));
    CHECK(prPngEncode(view, flags, compress.set, out.data(), out.count()) == out.count());
    return out;
}

// Appends a chunk of `type` with `data` and its CRC.
void putChunk(Array<uint8_t>* out, const char* type, PrByteView data) {
    const uint8_t length[4] = {
        uint8_t(data.size >> 24), uint8_t(data.size >> 16), uint8_t(data.size >> 8), uint8_t(data.size),
    };
    out->append(length, 4);
    Array<uint8_t> body;
    body.append(reinterpret_cast<const uint8_t*>(type), 4);
    if (data.size != 0)
        body.append(data.data, uint32_t(data.size));
    out->append(body.data(), body.count());
    const uint32_t crc = prCrc32Update(0, PrByteView{body.data(), body.count()});
    const uint8_t check[4] = {uint8_t(crc >> 24), uint8_t(crc >> 16), uint8_t(crc >> 8), uint8_t(crc)};
    out->append(check, 4);
}

void putBeU32(uint8_t* at, uint32_t value) {
    at[0] = uint8_t(value >> 24);
    at[1] = uint8_t(value >> 16);
    at[2] = uint8_t(value >> 8);
    at[3] = uint8_t(value);
}

// Returns the data of the IDAT chunks of a PNG, joined.
Array<uint8_t> imageData(const Array<uint8_t>& png) {
    Array<uint8_t> data;
    for (uint32_t at = 8; at + 12 <= png.count();) {
        const uint32_t length = uint32_t(png[at]) << 24 | uint32_t(png[at + 1]) << 16 | uint32_t(png[at + 2]) << 8 |
            png[at + 3];
        if (prMemEquals(png.data() + at + 4, "IDAT", 4))
            data.append(png.data() + at + 8, length);
        at += length + 12;
    }
    return data;
}

// An animated PNG of `extent` whose frames each cover the image, opaque, of noise that compresses little, so that its
// data arrives over many pieces. The default image is the first frame, or if `hidden` is set, an image of its own
// before the frames, which the animation does not show. Frame `i`'s pixels are `frames[i]`. `flags` encodes each image,
// as interlaced if it says so.
struct Animated {
    Array<uint8_t> file;
    Array<Array<uint8_t>> frames;

    Animated(const Compress& compress, PrUVec2 extent, uint32_t count, bool hidden, PrPngEncodeFlags flags = 0) {
        const uint8_t signature[8] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
        file.append(signature, 8);
        uint32_t sequence = 0;
        uint32_t seed = 7;
        auto noise = [&] {
            Array<uint8_t> pixels(extent.x * extent.y * 4);
            for (uint32_t i = 0; i < pixels.count(); ++i) {
                seed = seed * 1103515245u + 12345u;
                pixels[i] = i % 4 == 3 ? 255 : uint8_t(seed >> 16);
            }
            return pixels;
        };
        auto encoded = [&](const Array<uint8_t>& pixels) {
            PrPixelView view{pixels.data(), size_t(extent.x) * 4, extent};
            Array<uint8_t> png(uint32_t(prPngEncode(view, flags, compress.set, nullptr, 0)));
            prPngEncode(view, flags, compress.set, png.data(), png.count());
            return png;
        };
        for (uint32_t frame = 0; frame < count; ++frame) {
            Array<uint8_t> pixels = noise();
            Array<uint8_t> png = encoded(pixels);
            if (frame == 0) {
                file.append(png.data() + 8, 25);
                uint8_t control[8] = {};
                putBeU32(control, count);
                putChunk(&file, "acTL", PrByteView{control, 8});
                if (hidden) {
                    Array<uint8_t> other = imageData(encoded(noise()));
                    putChunk(&file, "IDAT", PrByteView{other.data(), other.count()});
                }
            }
            uint8_t fctl[26] = {};
            putBeU32(fctl, sequence++);
            putBeU32(fctl + 4, extent.x);
            putBeU32(fctl + 8, extent.y);
            fctl[21] = 1;
            fctl[23] = 10;
            putChunk(&file, "fcTL", PrByteView{fctl, 26});
            Array<uint8_t> data = imageData(png);
            if (frame == 0 && !hidden) {
                putChunk(&file, "IDAT", PrByteView{data.data(), data.count()});
            } else {
                Array<uint8_t> fdat(4);
                putBeU32(fdat.data(), sequence++);
                fdat.append(data.data(), data.count());
                putChunk(&file, "fdAT", PrByteView{fdat.data(), fdat.count()});
            }
            frames.push(static_cast<Array<uint8_t>&&>(pixels));
        }
        putChunk(&file, "IEND", PrByteView{});
    }
};

// Returns the number of whole rows at the top of `got` that equal `expected`'s, and checks that every row after them
// that is not `expected`'s is `below`'s, the rows that show where the frame has not arrived.
uint32_t arrivedRows(const Array<uint8_t>& got, const Array<uint8_t>& expected, const Array<uint8_t>& below,
        uint32_t width, uint32_t height) {
    const uint32_t row = width * 4;
    uint32_t rows = 0;
    while (rows < height && prMemEquals(got.data() + rows * row, expected.data() + rows * row, row))
        ++rows;
    for (uint32_t y = rows; y < height; ++y)
        CHECK(prMemEquals(got.data() + y * row, below.data() + y * row, row));
    return rows;
}

// The passes of Adam7 (png 8.2): the pass from zero of each pixel of an 8 by 8 block, each pass's origin, its rows'
// step, and the block that a pixel of each pass stands for until a finer pass places its own.
constexpr uint8_t kTestAdam7[8][8] = {
    {0, 5, 3, 5, 1, 5, 3, 5}, {6, 6, 6, 6, 6, 6, 6, 6}, {4, 5, 4, 5, 4, 5, 4, 5}, {6, 6, 6, 6, 6, 6, 6, 6},
    {2, 5, 3, 5, 2, 5, 3, 5}, {6, 6, 6, 6, 6, 6, 6, 6}, {4, 5, 4, 5, 4, 5, 4, 5}, {6, 6, 6, 6, 6, 6, 6, 6},
};
constexpr uint32_t kTestOriginX[7] = {0, 4, 0, 2, 0, 1, 0};
constexpr uint32_t kTestOriginY[7] = {0, 0, 4, 0, 2, 0, 1};
constexpr uint32_t kTestStepY[7] = {8, 8, 8, 4, 4, 2, 2};
constexpr uint32_t kTestBlockW[7] = {8, 4, 4, 2, 2, 1, 1};
constexpr uint32_t kTestBlockH[7] = {8, 8, 4, 4, 2, 2, 1};

// How much of an image has arrived: `passes` passes whole, and `rows` rows of the next. A non-interlaced image has one
// pass.
struct Arrived {
    uint32_t passes = 0;
    uint32_t rows = 0;
};

// Returns the rows of pass `pass` of an image of `extent`.
uint32_t passRows(PrUVec2 extent, uint32_t pass) {
    if (extent.x <= kTestOriginX[pass] || extent.y <= kTestOriginY[pass])
        return 0;
    return (extent.y - kTestOriginY[pass] + kTestStepY[pass] - 1) / kTestStepY[pass];
}

bool placed(uint32_t x, uint32_t y, Arrived a) {
    const uint32_t pass = kTestAdam7[y % 8][x % 8];
    return pass < a.passes || (pass == a.passes && (y - kTestOriginY[pass]) / kTestStepY[pass] < a.rows);
}

// Returns what pixel (x, y) of an interlaced image shows once `a` has arrived, from the whole image `final`: the pixel
// of the finest pass placed whose block covers it, or transparent black if no such pixel is placed.
const uint8_t* expectedPixel(const Array<uint8_t>& final, PrUVec2 extent, uint32_t x, uint32_t y, Arrived a) {
    static const uint8_t kNone[4] = {};
    if (placed(x, y, a))
        return final.data() + (y * extent.x + x) * 4;
    for (uint32_t pass = (a.passes < 7 ? a.passes : 6) + 1; pass-- > 0;) {
        const uint32_t fx = x / kTestBlockW[pass] * kTestBlockW[pass];
        const uint32_t fy = y / kTestBlockH[pass] * kTestBlockH[pass];
        if (placed(fx, fy, a))
            return final.data() + (fy * extent.x + fx) * 4;
    }
    return kNone;
}

// Returns true if `got`, a picture of an interlaced image of `extent`, is exactly what the image shows once `a` has
// arrived. A non-interlaced image's arrived rows are its own, and the rest is transparent black.
bool showsArrived(const Array<uint8_t>& got, const Array<uint8_t>& final, PrUVec2 extent, bool interlaced, Arrived a) {
    for (uint32_t y = 0; y < extent.y; ++y) {
        for (uint32_t x = 0; x < extent.x; ++x) {
            const uint8_t* pixel = got.data() + (y * extent.x + x) * 4;
            static const uint8_t kNone[4] = {};
            const uint8_t* expected = kNone;
            if (interlaced)
                expected = expectedPixel(final, extent, x, y, a);
            else if (a.passes != 0 || y < a.rows)
                expected = final.data() + (y * extent.x + x) * 4;
            if (!prMemEquals(pixel, expected, 4))
                return false;
        }
    }
    return true;
}

// Moves `a` forward, from where it is, to the first amount that `got` shows exactly. Returns false if none does: the
// picture is not one that the arrival of the image's rows in order gives.
bool advanceTo(const Array<uint8_t>& got, const Array<uint8_t>& final, PrUVec2 extent, bool interlaced, Arrived* a) {
    const uint32_t passes = interlaced ? 7 : 1;
    for (;;) {
        if (showsArrived(got, final, extent, interlaced, *a))
            return true;
        if (a->passes >= passes)
            return false;
        uint32_t rows = interlaced ? passRows(extent, a->passes) : extent.y;
        if (a->rows < rows) {
            ++a->rows;
        } else {
            ++a->passes;
            a->rows = 0;
        }
    }
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("an encoded image decodes to its pixels, as 8-bit RGBA with alpha") {
    Compress compress;
    Array<uint8_t> pixels;
    Array<uint8_t> file = encode(compress, 5, 3, 7, &pixels);
    CHECK(prPngHasSignature(file));
    PrPngDecoder* decoder = nullptr;
    REQUIRE(prPngDecoderCreate(file, compress.set, nullptr, file.count(), &decoder) == PrStatus_Ok);
    const PrPngInfo* info = prPngDecoderInfo(decoder);
    CHECK(info->extent.x == 5);
    CHECK(info->extent.y == 3);
    CHECK(info->channels == 4);
    CHECK(info->sampleBytes == 1);
    CHECK(info->alpha);
    CHECK(info->frameCount == 1);
    CHECK(info->playCount == 1);
    CHECK_FALSE(info->srgb);
    CHECK_FALSE(info->hasCicp);
    CHECK(info->profile.size == 0);
    CHECK(info->exif.size == 0);
    CHECK(info->workingBytes >= file.count());
    CHECK(prPngDecoderFrameDuration(decoder, 0) == 0);
    // Rows of a wider stride keep the bytes between them.
    Array<uint8_t> out(size_t(24) * 3);
    prMemFill(out.data(), 0xee, out.count());
    REQUIRE(prPngDecoderRead(decoder, 0, PrPixels{.data = out.data(), .stride = 24, .extent = PrUVec2{5, 3}}) ==
        PrStatus_Ok);
    for (uint32_t row = 0; row < 3; ++row) {
        CHECK(prMemEquals(out.data() + row * 24, pixels.data() + row * 20, 20));
        CHECK(out[row * 24 + 20] == 0xee);
    }
    prPngDecoderDestroy(decoder);
}

TEST_CASE("bytes without the signature are not a PNG, and a file cut short is truncated") {
    Compress compress;
    Array<uint8_t> pixels;
    Array<uint8_t> file = encode(compress, 4, 4, 1, &pixels);
    PrPngDecoder* decoder = nullptr;
    const uint8_t text[] = {'n', 'o', 't', ' ', 'a', ' ', 'p', 'n', 'g'};
    CHECK(prPngDecoderCreate(text, compress.set, nullptr, sizeof(text), &decoder) == PrStatus_NotSupported);
    CHECK(decoder == nullptr);
    // The file without its IEND chunk ends before its image data is known to end.
    PrByteView cut{file.data(), file.count() - 12};
    REQUIRE(prPngDecoderCreate(cut, compress.set, nullptr, cut.size, &decoder) == PrStatus_Truncated);
    CHECK(decoder != nullptr);
    prPngDecoderDestroy(decoder);
}

TEST_CASE("an image past the limits is refused") {
    Compress compress;
    Array<uint8_t> pixels;
    Array<uint8_t> file = encode(compress, 8, 8, 3, &pixels);
    PrPngLimits limits = prPngLimitsDefault();
    limits.pixels = 63;
    PrPngDecoder* decoder = nullptr;
    CHECK(prPngDecoderCreate(file, compress.set, &limits, file.count(), &decoder) == PrStatus_Limit);
    CHECK(decoder == nullptr);
    limits = prPngLimitsDefault();
    limits.workingBytes = file.count();
    CHECK(prPngDecoderCreate(file, compress.set, &limits, file.count(), &decoder) == PrStatus_Limit);
}

TEST_CASE("a set without zlib stores the image data, which still decodes") {
    Compress compress;
    Array<uint8_t> pixels(size_t(2) * 2 * 4);
    prMemFill(pixels.data(), 9, pixels.count());
    PrCompressCodecSet* none = prCompressCodecSetCreate(nullptr, 0);
    PrPixelView view{.data = pixels.data(), .stride = 8, .extent = PrUVec2{2, 2}};
    Array<uint8_t> stored(uint32_t(prPngEncode(view, 0, none, nullptr, 0)));
    prPngEncode(view, 0, none, stored.data(), stored.count());
    prCompressCodecSetDestroy(none);
    PrPngDecoder* decoder = nullptr;
    REQUIRE(prPngDecoderCreate(stored, compress.set, nullptr, stored.count(), &decoder) == PrStatus_Ok);
    Array<uint8_t> out(pixels.count());
    REQUIRE(prPngDecoderRead(decoder, 0, PrPixels{.data = out.data(), .stride = 8, .extent = PrUVec2{2, 2}}) ==
        PrStatus_Ok);
    CHECK(out == pixels);
    prPngDecoderDestroy(decoder);
}

TEST_CASE("the public headers compile as C") {
    CHECK(prTestPngCApi() == 0);
}

TEST_CASE("an interlaced encoding stores the image in seven passes and decodes to its pixels") {
    Compress compress;
    Array<uint8_t> pixels;
    Array<uint8_t> file = encode(compress, 13, 11, 3, &pixels, PrPngEncodeFlag_Interlaced);
    CHECK(file[28] == 1);
    PrPngDecoder* decoder = nullptr;
    REQUIRE(prPngDecoderCreate(file, compress.set, nullptr, file.count(), &decoder) == PrStatus_Ok);
    Array<uint8_t> out(pixels.count());
    REQUIRE(prPngDecoderRead(decoder, 0, PrPixels{.data = out.data(), .stride = 13 * 4, .extent = PrUVec2{13, 11}}) ==
        PrStatus_Ok);
    CHECK(prMemEquals(out.data(), pixels.data(), pixels.count()));
    prPngDecoderDestroy(decoder);
}

TEST_CASE("a png appended in pieces of any size decodes as it arrives, with work proportional to the file") {
    Compress compress;
    const PrPngEncodeFlags kFlags[2] = {0, PrPngEncodeFlag_Interlaced};
    for (PrPngEncodeFlags flags : kFlags) {
        CAPTURE(flags);
        // Pixels whose bytes do not compress much, so that the file is about as large as its rows.
        Array<uint8_t> pixels(64 * 48 * 4);
        uint32_t seed = 3;
        for (uint32_t i = 0; i < pixels.count(); ++i) {
            seed = seed * 1103515245u + 12345u;
            pixels[i] = i % 4 == 3 ? 255 : uint8_t(seed >> 16);
        }
        PrPixelView view{.data = pixels.data(), .stride = 64 * 4, .extent = PrUVec2{64, 48}};
        Array<uint8_t> file(uint32_t(prPngEncode(view, flags, compress.set, nullptr, 0)));
        prPngEncode(view, flags, compress.set, file.data(), file.count());
        const uint32_t kPieces[3] = {1, 6, 4096};
        for (uint32_t piece : kPieces) {
            CAPTURE(piece);
            PrPngDecoder* decoder = nullptr;
            REQUIRE(prPngDecoderCreateStream(compress.set, nullptr, 0, &decoder) == PrStatus_Ok);
            CHECK(prPngDecoderInfo(decoder)->extent.x == 0);
            Array<uint8_t> out(pixels.count());
            uint32_t shown = 0;
            PrStatus status = PrStatus_Truncated;
            for (uint32_t at = 0; at < file.count(); at += piece) {
                uint32_t size = file.count() - at < piece ? file.count() - at : piece;
                status = prPngDecoderAppend(decoder, PrByteView{file.data() + at, size});
                REQUIRE((status == PrStatus_Ok || status == PrStatus_Truncated));
                // Halfway, the picture shows a part of the rows, or of every row in coarse blocks.
                if (prPngDecoderInfo(decoder)->extent.x != 0 && at < file.count() / 2 && at + size >= file.count() / 2) {
                    REQUIRE(prPngDecoderRead(decoder, 0, PrPixels{out.data(), 64 * 4, {64, 48}}) == PrStatus_Truncated);
                    for (uint32_t i = 3; i < out.count(); i += 4)
                        shown += out[i] != 0 ? 1 : 0;
                }
            }
            CHECK(status == PrStatus_Ok);
            CHECK(shown > 0);
            CHECK(shown < (flags != 0 ? 64u * 48u + 1 : 64u * 48u));
            REQUIRE(prPngDecoderRead(decoder, 0, PrPixels{out.data(), 64 * 4, {64, 48}}) == PrStatus_Ok);
            CHECK(prMemEquals(out.data(), pixels.data(), pixels.count()));
            // Each byte of the file is read a few times at most, and each byte of the rows twice, whatever the pieces.
            CHECK(prPngDecoderWork(decoder) <= uint64_t(file.count()) * 4 + (64 * 4 + 1) * 48 * 3 + 64);
            prPngDecoderDestroy(decoder);
        }
    }
}

TEST_CASE("each frame of an arriving animated png shows its rows as they arrive, as a still png does") {
    Compress compress;
    const PrUVec2 extent{48, 32};
    const bool kHidden[2] = {false, true};
    for (bool hidden : kHidden) {
        CAPTURE(hidden);
        Animated animated(compress, extent, 3, hidden);
        const Array<uint8_t>& file = animated.file;
        PrPngDecoder* decoder = nullptr;
        REQUIRE(prPngDecoderCreateStream(compress.set, nullptr, 0, &decoder) == PrStatus_Ok);
        Array<uint8_t> out(extent.x * extent.y * 4);
        const Array<uint8_t> transparent(extent.x * extent.y * 4);
        // The first frame begins with the first fcTL chunk, after the default image that the animation hides.
        uint32_t first = 0;
        while (!prMemEquals(file.data() + first, "fcTL", 4))
            ++first;
        // For each frame, the most rows that showed while it arrived and was not whole.
        uint32_t most[3] = {};
        uint32_t fewest[3] = {~0u, ~0u, ~0u};
        for (uint32_t at = 0; at < file.count(); at += 5) {
            uint32_t size = file.count() - at < 5 ? file.count() - at : 5;
            prPngDecoderAppend(decoder, PrByteView{file.data() + at, size});
            const PrPngInfo& info = *prPngDecoderInfo(decoder);
            if (info.extent.x == 0 || at < first + 40)
                continue;
            const uint32_t frame = info.frameCount - 1;
            if (frame >= 3 || at + size >= file.count())
                continue;
            REQUIRE(prPngDecoderRead(decoder, frame, PrPixels{out.data(), extent.x * 4, extent}) == PrStatus_Truncated);
            // Where the frame has not arrived, the frame before it shows, or nothing before the first.
            const Array<uint8_t>& below = frame == 0 ? transparent : animated.frames[frame - 1];
            uint32_t rows = arrivedRows(out, animated.frames[frame], below, extent.x, extent.y);
            most[frame] = rows > most[frame] ? rows : most[frame];
            fewest[frame] = rows < fewest[frame] ? rows : fewest[frame];
        }
        // Every frame showed a part of its rows before it was whole, and more of them as more arrived.
        for (uint32_t frame = 0; frame < 3; ++frame) {
            CAPTURE(frame);
            CHECK(most[frame] > fewest[frame]);
            CHECK(most[frame] > 0);
        }
        prPngDecoderDestroy(decoder);
    }
}

TEST_CASE("playing the frames of an arriving animated png in order decodes each frame once") {
    Compress compress;
    const PrUVec2 extent{8, 8};
    const uint32_t kFrames = 40;
    Animated animated(compress, extent, kFrames, false);
    const Array<uint8_t>& file = animated.file;
    PrPngDecoder* decoder = nullptr;
    REQUIRE(prPngDecoderCreateStream(compress.set, nullptr, 0, &decoder) == PrStatus_Ok);
    Array<uint8_t> out(extent.x * extent.y * 4);
    uint32_t shown = 0;
    for (uint32_t at = 0; at < file.count(); at += 9) {
        uint32_t size = file.count() - at < 9 ? file.count() - at : 9;
        prPngDecoderAppend(decoder, PrByteView{file.data() + at, size});
        while (prPngDecoderInfo(decoder)->extent.x != 0 && shown + 1 < prPngDecoderInfo(decoder)->frameCount) {
            REQUIRE(prPngDecoderRead(decoder, shown, PrPixels{out.data(), extent.x * 4, extent}) == PrStatus_Truncated);
            CHECK(prMemEquals(out.data(), animated.frames[shown].data(), out.count()));
            ++shown;
        }
    }
    CHECK(shown == kFrames - 1);
    // Each frame that played was decoded once, beside the work of the bytes and the rows that they inflated into.
    CHECK(prPngDecoderWork(decoder) <= uint64_t(file.count()) * 4 + (8 * 4 + 1) * 8 * 3 * kFrames + 64 * shown);
    prPngDecoderDestroy(decoder);
}

TEST_CASE("an arriving png shows exactly the rows that have arrived, and an interlaced one fills each block from them") {
    Compress compress;
    const PrUVec2 extent{61, 47};
    Array<uint8_t> pixels(extent.x * extent.y * 4);
    uint32_t seed = 11;
    for (uint32_t i = 0; i < pixels.count(); ++i) {
        seed = seed * 1103515245u + 12345u;
        pixels[i] = i % 4 == 3 ? 255 : uint8_t(seed >> 16);
    }
    const PrPngEncodeFlags kFlags[2] = {0, PrPngEncodeFlag_Interlaced};
    for (PrPngEncodeFlags flags : kFlags) {
        const bool interlaced = flags != 0;
        CAPTURE(interlaced);
        PrPixelView view{pixels.data(), extent.x * 4, extent};
        Array<uint8_t> file(uint32_t(prPngEncode(view, flags, compress.set, nullptr, 0)));
        prPngEncode(view, flags, compress.set, file.data(), file.count());
        PrPngDecoder* decoder = nullptr;
        REQUIRE(prPngDecoderCreateStream(compress.set, nullptr, 0, &decoder) == PrStatus_Ok);
        Array<uint8_t> got(pixels.count());
        Arrived arrived;
        uint32_t reads = 0;
        // At every append of three bytes, the picture is what some amount of the rows in order shows, and that amount
        // only grows.
        for (uint32_t at = 0; at < file.count(); at += 3) {
            const uint32_t size = file.count() - at < 3 ? file.count() - at : 3;
            prPngDecoderAppend(decoder, PrByteView{file.data() + at, size});
            if (prPngDecoderInfo(decoder)->extent.x == 0)
                continue;
            prPngDecoderRead(decoder, 0, PrPixels{got.data(), extent.x * 4, extent});
            CAPTURE(at);
            REQUIRE(advanceTo(got, pixels, extent, interlaced, &arrived));
            ++reads;
        }
        CHECK(prMemEquals(got.data(), pixels.data(), pixels.count()));
        CHECK(reads > 100);
        prPngDecoderDestroy(decoder);
    }
}

TEST_CASE("the arriving frame of an interlaced animated png fills each block of its rows, over the frame before it") {
    Compress compress;
    const PrUVec2 extent{40, 24};
    Animated animated(compress, extent, 2, true, PrPngEncodeFlag_Interlaced);
    const Array<uint8_t>& file = animated.file;
    PrPngDecoder* decoder = nullptr;
    REQUIRE(prPngDecoderCreateStream(compress.set, nullptr, 0, &decoder) == PrStatus_Ok);
    Array<uint8_t> got(extent.x * extent.y * 4);
    Arrived arrived[2];
    uint32_t first = 0;
    while (!prMemEquals(file.data() + first, "fcTL", 4))
        ++first;
    for (uint32_t at = 0; at < file.count(); at += 3) {
        const uint32_t size = file.count() - at < 3 ? file.count() - at : 3;
        prPngDecoderAppend(decoder, PrByteView{file.data() + at, size});
        if (prPngDecoderInfo(decoder)->extent.x == 0 || at < first + 40 || at + size >= file.count())
            continue;
        const uint32_t frame = prPngDecoderInfo(decoder)->frameCount - 1;
        prPngDecoderRead(decoder, frame, PrPixels{got.data(), extent.x * 4, extent});
        CAPTURE(at);
        CAPTURE(frame);
        // The rows of the frame that have arrived show as an interlaced still does, and the frame before it shows below
        // them: the frames cover the image, are opaque and replace what is under them.
        const Array<uint8_t> none(got.count());
        const Array<uint8_t>& below = frame == 0 ? none : animated.frames[frame - 1];
        bool matched = false;
        for (;;) {
            const Arrived a = arrived[frame];
            const uint32_t rows = a.passes != 0 ? extent.y : (a.rows * 8 < extent.y ? a.rows * 8 : extent.y);
            bool same = true;
            for (uint32_t y = 0; y < extent.y && same; ++y) {
                for (uint32_t x = 0; x < extent.x && same; ++x) {
                    const uint8_t* expected = below.data() + (y * extent.x + x) * 4;
                    if (y < rows)
                        expected = expectedPixel(animated.frames[frame], extent, x, y, a);
                    same = prMemEquals(got.data() + (y * extent.x + x) * 4, expected, 4);
                }
            }
            if (same) {
                matched = true;
                break;
            }
            if (a.passes >= 7)
                break;
            if (arrived[frame].rows < passRows(extent, a.passes)) {
                ++arrived[frame].rows;
            } else {
                ++arrived[frame].passes;
                arrived[frame].rows = 0;
            }
        }
        REQUIRE(matched);
    }
    CHECK(arrived[0].passes + arrived[1].passes > 4);
    prPngDecoderDestroy(decoder);
}
