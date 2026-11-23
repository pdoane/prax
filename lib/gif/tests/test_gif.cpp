#include "pr/gif.h"
#include "pr/mem.h"

#include "tools/harness/animated_gif.h"

#include "tools/harness/test.h"

extern "C" int prTestGifCApi(void);

namespace pr_impl {

namespace {

// A 6x4 GIF87a of one frame with a global color table, and its screen as RGBA (gen_gif_vectors.py made it).
constexpr uint8_t kGlobalTable87aBytes[] = {
    0x47, 0x49, 0x46, 0x38, 0x37, 0x61, 0x06, 0x00, 0x04, 0x00, 0xF1, 0x00, 0x00, 0x00, 0x00,
    0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x2C, 0x00, 0x00, 0x00, 0x00,
    0x06, 0x00, 0x04, 0x00, 0x00, 0x02, 0x06, 0x44, 0x6C, 0xA7, 0x80, 0xBA, 0x57, 0x00, 0x3B,
};
constexpr uint8_t kGlobalTable87aScreen0[] = {
    0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0xFF,
};

} // namespace

} // namespace pr_impl

using namespace harness;
using namespace pr_impl;

TEST_CASE("a still image decodes to its screen as RGBA") {
    CHECK(prGifHasSignature(kGlobalTable87aBytes));
    PrGifDecoder* decoder = nullptr;
    REQUIRE(prGifDecoderCreate(kGlobalTable87aBytes, nullptr, sizeof(kGlobalTable87aBytes), &decoder) == PrStatus_Ok);
    const PrGifInfo* info = prGifDecoderInfo(decoder);
    CHECK(info->extent.x == 6);
    CHECK(info->extent.y == 4);
    CHECK(info->frameCount == 1);
    CHECK(info->playCount == 1);
    CHECK(prGifDecoderFrameDuration(decoder, 0) == 0);
    uint8_t out[28 * 4] = {};
    REQUIRE(prGifDecoderRead(decoder, 0, PrPixels{.data = out, .stride = 28, .extent = PrUVec2{6, 4}}) == PrStatus_Ok);
    for (uint32_t row = 0; row < 4; ++row)
        CHECK(prMemEquals(out + row * 28, kGlobalTable87aScreen0 + row * 24, 24));
    prGifDecoderDestroy(decoder);
}

TEST_CASE("bytes without the signature are not a GIF, a file without its trailer is truncated, and limits hold") {
    PrGifDecoder* decoder = nullptr;
    const uint8_t text[] = {'G', 'I', 'F', '9', '9', 'z'};
    CHECK(prGifDecoderCreate(text, nullptr, sizeof(text), &decoder) == PrStatus_NotSupported);
    PrByteView cut{kGlobalTable87aBytes, sizeof(kGlobalTable87aBytes) - 1};
    CHECK(prGifDecoderCreate(cut, nullptr, cut.size, &decoder) == PrStatus_Truncated);
    prGifDecoderDestroy(decoder);
    PrGifLimits limits = prGifLimitsDefault();
    limits.pixels = 23;
    decoder = nullptr;
    CHECK(prGifDecoderCreate(kGlobalTable87aBytes, &limits, sizeof(kGlobalTable87aBytes), &decoder) == PrStatus_Limit);
    CHECK(decoder == nullptr);
}

TEST_CASE("the public header compiles as C") {
    CHECK(prTestGifCApi() == 0);
}

TEST_CASE("a gif appended in pieces of any size draws its frames as they arrive, with work proportional to the file") {
    Array<uint8_t> gif = animatedGif(5, 10, true);
    PrGifDecoder* whole = nullptr;
    REQUIRE(prGifDecoderCreate(PrByteView{gif.data(), gif.count()}, nullptr, gif.count(), &whole) == PrStatus_Ok);
    const uint32_t kPieces[3] = {1, 6, 4096};
    for (uint32_t piece : kPieces) {
        CAPTURE(piece);
        PrGifDecoder* decoder = nullptr;
        REQUIRE(prGifDecoderCreateStream(nullptr, 0, &decoder) == PrStatus_Ok);
        uint32_t framesSeen = 0;
        PrStatus status = PrStatus_Truncated;
        for (uint32_t at = 0; at < gif.count(); at += piece) {
            uint32_t size = gif.count() - at < piece ? gif.count() - at : piece;
            status = prGifDecoderAppend(decoder, PrByteView{gif.data() + at, size});
            REQUIRE((status == PrStatus_Ok || status == PrStatus_Truncated));
            // The frames that have begun only grow.
            CHECK(prGifDecoderInfo(decoder)->frameCount >= framesSeen);
            framesSeen = prGifDecoderInfo(decoder)->frameCount;
        }
        CHECK(status == PrStatus_Ok);
        REQUIRE(framesSeen == 5);
        // Every frame reads as the decode of the whole file gives it, in any order.
        const uint32_t kOrder[5] = {4, 0, 2, 1, 3};
        for (uint32_t frame : kOrder) {
            uint8_t streamed[4 * 2 * 4] = {};
            uint8_t expected[4 * 2 * 4] = {};
            REQUIRE(prGifDecoderRead(decoder, frame, PrPixels{streamed, 16, {4, 2}}) == PrStatus_Ok);
            REQUIRE(prGifDecoderRead(whole, frame, PrPixels{expected, 16, {4, 2}}) == PrStatus_Ok);
            CHECK(prMemEquals(streamed, expected, sizeof(expected)));
        }
        CHECK(prGifDecoderWork(decoder) <= uint64_t(gif.count()) * 3 + 5 * 8 * 2);
        prGifDecoderDestroy(decoder);
    }
    prGifDecoderDestroy(whole);
}

TEST_CASE("playing the frames of an arriving gif in order composes each frame once") {
    const uint32_t kFrames = 60;
    Array<uint8_t> gif = animatedGif(kFrames, 10, true);
    PrGifDecoder* decoder = nullptr;
    REQUIRE(prGifDecoderCreateStream(nullptr, 0, &decoder) == PrStatus_Ok);
    // The bytes arrive in small pieces. After each piece, a player shows the next frame if a frame after it has begun,
    // so the frames play in order while more arrive.
    uint32_t shown = 0;
    for (uint32_t at = 0; at < gif.count(); at += 7) {
        uint32_t size = gif.count() - at < 7 ? gif.count() - at : 7;
        prGifDecoderAppend(decoder, PrByteView{gif.data() + at, size});
        while (shown + 1 < prGifDecoderInfo(decoder)->frameCount) {
            uint8_t pixels[4 * 2 * 4] = {};
            CHECK(prGifDecoderRead(decoder, shown, PrPixels{pixels, 16, {4, 2}}) == PrStatus_Truncated);
            ++shown;
        }
    }
    CHECK(shown == kFrames - 1);
    // Each of the frames that played was composed once: 8 pixels each, beside the work of the bytes.
    CHECK(prGifDecoderWork(decoder) <= uint64_t(gif.count()) * 3 + uint64_t(shown) * 8);
    prGifDecoderDestroy(decoder);
}
