// WebP decoding: files of each feature, which gen_webp_vectors.py had libwebp's tools write, with the pixels that
// libwebp decodes them to; and files that break the format or the limits, which are refused.

#include "pr/array.h"
#include "pr/mem.h"
#include "pr/span.h"
#include "pr/string_ops.h"
#include "pr/webp.h"

#include "webp/tests/webp_vectors.h"

#include "tools/harness/test.h"

extern "C" int prTestWebpCApi(void);

namespace pr_impl {

namespace {

const Vector& named(const char* name) {
    for (const Vector& vector : vectors()) {
        if (PrStringView(vector.name) == name)
            return vector;
    }
    FAIL("no vector named ", name);
    return vectors()[0];
}

// Returns the offset of the first chunk of the FourCC `fourCc` in `bytes`.
uint32_t chunkAt(Span<const uint8_t> bytes, const char* fourCc) {
    const PrStringView text(reinterpret_cast<const char*>(bytes.data()), bytes.count());
    const size_t at = prStringFind(text, PrStringView(fourCc, 4));
    REQUIRE(at != text.size);
    return uint32_t(at);
}

Array<uint8_t> copyBytes(PrByteView bytes) {
    Array<uint8_t> out;
    out.append(bytes);
    return out;
}

Array<uint8_t> patched(Span<const uint8_t> bytes, uint32_t at, uint8_t value) {
    Array<uint8_t> out = copyBytes(PrByteView{bytes.data(), bytes.count()});
    out[at] = value;
    return out;
}

// Opens `bytes` within `limits` and returns the status of the open.
PrStatus openStatus(Span<const uint8_t> bytes, const PrWebpLimits* PR_NULLABLE limits) {
    PrWebpDecoder* decoder = nullptr;
    const PrStatus status = prWebpDecoderCreate({bytes.data(), bytes.count()}, limits, bytes.count(), &decoder);
    prWebpDecoderDestroy(decoder);
    return status;
}

// Opens `bytes` and reads every frame, expanded to Rgba8 one frame after another. Returns the status of the open or of
// the first read that is not Ok.
PrStatus readAll(Span<const uint8_t> bytes, Array<uint8_t>* out) {
    // A changed byte can claim a far larger image than the vectors' own, which this copy of its pixels would hold twice
    // over: past a million pixels, the image is refused, as it is under limits that small.
    PrWebpLimits limits = prWebpLimitsDefault();
    limits.pixels = 1u << 20;
    PrWebpDecoder* decoder = nullptr;
    const PrStatus opened = prWebpDecoderCreate({bytes.data(), bytes.count()}, &limits, bytes.count(), &decoder);
    if (decoder == nullptr)
        return opened;
    const PrWebpInfo& info = *prWebpDecoderInfo(decoder);
    const uint32_t size = info.alpha ? 4 : 3;
    const size_t pixels = size_t(info.extent.x) * info.extent.y;
    Array<uint8_t> native(pixels * size);
    // A truncated image reads too, as far as its bytes go.
    PrStatus status = PrStatus_Ok;
    for (uint32_t frame = 0; frame < info.frameCount && status == PrStatus_Ok; ++frame) {
        status = prWebpDecoderRead(decoder, frame, PrPixels{native.data(), info.extent.x * size, info.extent});
        for (size_t i = 0; status == PrStatus_Ok && i < pixels; ++i) {
            out->append(native.data() + i * size, 3);
            out->push(size == 4 ? native[uint32_t(i * 4 + 3)] : 0xFF);
        }
    }
    prWebpDecoderDestroy(decoder);
    return status == PrStatus_Ok ? opened : status;
}

Array<uint8_t> expected(const Vector& vector) {
    Array<uint8_t> out;
    for (uint32_t frame = 0; frame < vector.durations.count(); ++frame)
        out.append(vector.frames[frame]);
    return out;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("webp files decode to libwebp's pixels") {
    for (const Vector& vector : vectors()) {
        CAPTURE(vector.name);
        Array<uint8_t> pixels;
        REQUIRE(readAll(vector.bytes, &pixels) == PrStatus_Ok);
        CHECK(pixels == expected(vector));
    }
}

TEST_CASE("a webp's size, alpha, metadata and frames are its chunks'") {
    for (const Vector& vector : vectors()) {
        CAPTURE(vector.name);
        CHECK(prWebpHasSignature({vector.bytes.data(), vector.bytes.count()}));
        PrWebpDecoder* decoder = nullptr;
        REQUIRE(prWebpDecoderCreate({vector.bytes.data(), vector.bytes.count()}, nullptr, 0, &decoder) == PrStatus_Ok);
        const PrWebpInfo& info = *prWebpDecoderInfo(decoder);
        CHECK(info.extent.x == vector.width);
        CHECK(info.extent.y == vector.height);
        CHECK(info.alpha == vector.alpha);
        CHECK(info.frameCount == vector.durations.count());
        CHECK(info.animated == (PrStringView(vector.name) == "animation"));
        if (PrStringView(vector.name) != "extended metadata") {
            CHECK(info.profile.size == 0);
            CHECK(info.exif.size == 0);
        }
        for (uint32_t frame = 0; frame < info.frameCount; ++frame)
            CHECK(prWebpDecoderFrameDuration(decoder, frame) == vector.durations[frame]);
        prWebpDecoderDestroy(decoder);
    }
}

TEST_CASE("an extended webp gives its profile and its exif") {
    const Vector& vector = named("extended metadata");
    PrWebpDecoder* decoder = nullptr;
    REQUIRE(prWebpDecoderCreate({vector.bytes.data(), vector.bytes.count()}, nullptr, 0, &decoder) == PrStatus_Ok);
    const PrWebpInfo& info = *prWebpDecoderInfo(decoder);
    const PrStringView profile = "not an ICC profile";
    REQUIRE(info.profile.size == profile.size);
    CHECK(prMemEquals(info.profile.data, profile.data, profile.size));
    // The Exif metadata is a TIFF structure, without the "Exif\0\0" of a JPEG's segment.
    REQUIRE(info.exif.size >= 8);
    CHECK((prMemEquals(info.exif.data, "II*\0", 4) || prMemEquals(info.exif.data, "MM\0*", 4)));
    prWebpDecoderDestroy(decoder);
}

TEST_CASE("an animated webp composes its frames in order, and again from the start") {
    const Vector& vector = named("animation");
    PrWebpDecoder* decoder = nullptr;
    REQUIRE(prWebpDecoderCreate({vector.bytes.data(), vector.bytes.count()}, nullptr, 0, &decoder) == PrStatus_Ok);
    const PrWebpInfo& info = *prWebpDecoderInfo(decoder);
    CHECK(info.frameCount == 3);
    CHECK(info.playCount == 2);
    const uint32_t order[] = {0, 1, 2, 2, 1, 0, 2};
    Array<uint8_t> pixels(size_t(vector.width) * vector.height * 4);
    for (uint32_t frame : order) {
        CAPTURE(frame);
        prMemFill(pixels.data(), 0xCD, pixels.count());
        REQUIRE(prWebpDecoderRead(decoder, frame, PrPixels{pixels.data(), vector.width * 4, info.extent}) ==
            PrStatus_Ok);
        CHECK(pixels.span() == vector.frames[frame]);
    }
    prWebpDecoderDestroy(decoder);
}

TEST_CASE("a webp that ends early opens truncated or not at all, and never reads past its end") {
    for (const Vector& vector : vectors()) {
        CAPTURE(vector.name);
        for (uint32_t size = 0; size < vector.bytes.count(); ++size) {
            CAPTURE(size);
            // A copy of exactly the bytes kept, so that a read past them is a read past the allocation.
            const Array<uint8_t> bytes = copyBytes(PrByteView{vector.bytes.data(), size});
            Array<uint8_t> pixels;
            const PrStatus status = readAll(bytes.span(), &pixels);
            CHECK(status != PrStatus_Ok);
        }
    }
}

TEST_CASE("a webp with any byte changed decodes or fails, and never reads past its end") {
    const uint8_t kFlips[] = {0xFF, 0x01, 0x80};
    for (const Vector& vector : vectors()) {
        CAPTURE(vector.name);
        for (uint32_t at = 0; at < vector.bytes.count(); ++at) {
            for (uint8_t flip : kFlips) {
                CAPTURE(at);
                const Array<uint8_t> bytes = patched(vector.bytes, at, uint8_t(vector.bytes[at] ^ flip));
                Array<uint8_t> pixels;
                const PrStatus status = readAll(bytes.span(), &pixels);
                if (status == PrStatus_Ok)
                    CHECK(pixels.count() % 4 == 0);
            }
        }
    }
}

TEST_CASE("a webp's container is checked") {
    const Vector& lossless = named("lossless gradient");
    // A RIFF file of another form is no WebP.
    CHECK(!prWebpHasSignature({patched(lossless.bytes, 11, 'X').data(), lossless.bytes.count()}));
    CHECK(openStatus(patched(lossless.bytes, 11, 'X').span(), nullptr) == PrStatus_NotSupported);
    // The RIFF size counts at least the form and one chunk's header.
    CHECK(openStatus(patched(patched(lossless.bytes, 4, 3).span(), 5, 0).span(), nullptr) == PrStatus_InvalidData);
    // The first chunk is a bitstream or the extended header.
    CHECK(openStatus(patched(lossless.bytes, 12, 'Q').span(), nullptr) == PrStatus_InvalidData);
    // A chunk that runs past the RIFF size, inside the file, breaks the format.
    Array<uint8_t> overlong = copyBytes(PrByteView{lossless.bytes.data(), lossless.bytes.count()});
    overlong.append(PrByteView{reinterpret_cast<const uint8_t*>("padding!"), 8});
    overlong[16] = uint8_t(overlong[16] + 4);
    CHECK(openStatus(overlong.span(), nullptr) == PrStatus_InvalidData);
}

TEST_CASE("a webp's lossless header is checked") {
    const Vector& vector = named("lossless gradient");
    const uint32_t at = chunkAt(vector.bytes, "VP8L") + 8;
    // The signature byte, and the version, which is zero.
    CHECK(openStatus(patched(vector.bytes, at, 0x2E).span(), nullptr) == PrStatus_InvalidData);
    CHECK(openStatus(patched(vector.bytes, at + 4, uint8_t(vector.bytes[at + 4] | 0x20)).span(), nullptr) ==
        PrStatus_InvalidData);
    // A size that is not the canvas's.
    const Vector& extended = named("extended metadata");
    const uint32_t bitstream = chunkAt(extended.bytes, "VP8L") + 8;
    CHECK(openStatus(patched(extended.bytes, bitstream + 1, uint8_t(extended.bytes[bitstream + 1] ^ 1)).span(),
        nullptr) == PrStatus_InvalidData);
}

TEST_CASE("a webp's lossy frame header is checked") {
    const Vector& vector = named("lossy normal filter");
    const uint32_t at = chunkAt(vector.bytes, "VP8 ") + 8;
    // An interframe, an unknown profile, a frame that does not show, and a start code that is not VP8's.
    CHECK(openStatus(patched(vector.bytes, at, uint8_t(vector.bytes[at] | 1)).span(), nullptr) == PrStatus_InvalidData);
    CHECK(openStatus(patched(vector.bytes, at, uint8_t(vector.bytes[at] | 0x08)).span(), nullptr) ==
        PrStatus_InvalidData);
    CHECK(openStatus(patched(vector.bytes, at, uint8_t(vector.bytes[at] & ~0x10)).span(), nullptr) ==
        PrStatus_InvalidData);
    CHECK(openStatus(patched(vector.bytes, at + 3, 0x9C).span(), nullptr) == PrStatus_InvalidData);
    // A first partition longer than the chunk.
    CHECK(openStatus(patched(vector.bytes, at + 2, 0xFF).span(), nullptr) == PrStatus_InvalidData);
    // A size of zero.
    CHECK(openStatus(patched(patched(vector.bytes, at + 6, 0).span(), at + 7, 0).span(), nullptr) ==
        PrStatus_InvalidData);
}

TEST_CASE("a webp's animation frames lie inside the canvas, after the animation chunk") {
    const Vector& vector = named("animation");
    const uint32_t frame = chunkAt(vector.bytes, "ANMF") + 8;
    // The frame's x, in units of two pixels, past the canvas.
    CHECK(openStatus(patched(vector.bytes, frame, 2).span(), nullptr) == PrStatus_InvalidData);
    // A frame wider than the canvas.
    CHECK(openStatus(patched(vector.bytes, frame + 6, 4).span(), nullptr) == PrStatus_InvalidData);
    // A frame before any animation chunk.
    CHECK(openStatus(patched(vector.bytes, chunkAt(vector.bytes, "ANIM"), 'X').span(), nullptr) ==
        PrStatus_InvalidData);
}

TEST_CASE("a webp within the limits opens, and one past them is refused") {
    const Vector& animation = named("animation");
    PrWebpLimits limits = prWebpLimitsDefault();
    CHECK(openStatus(animation.bytes, &limits) == PrStatus_Ok);
    limits.frames = 2;
    CHECK(openStatus(animation.bytes, &limits) == PrStatus_Limit);
    limits = prWebpLimitsDefault();
    limits.work = 0;
    CHECK(openStatus(animation.bytes, &limits) == PrStatus_Limit);
    limits = prWebpLimitsDefault();
    limits.pixels = 15;
    CHECK(openStatus(animation.bytes, &limits) == PrStatus_Limit);
    limits = prWebpLimitsDefault();
    limits.workingBytes = 1024;
    CHECK(openStatus(animation.bytes, &limits) == PrStatus_Limit);
    limits = prWebpLimitsDefault();
    limits.inputBytes = animation.bytes.count() - 1;
    CHECK(openStatus(animation.bytes, &limits) == PrStatus_Limit);
    const Vector& extended = named("extended metadata");
    limits = prWebpLimitsDefault();
    limits.metadataBytes = 16;
    CHECK(openStatus(extended.bytes, &limits) == PrStatus_Limit);
}

TEST_CASE("a webp that claims a canvas too large for its bytes is refused before it allocates the canvas") {
    // An extended header of 16384 by 16384 pixels over a lossless bitstream of one pixel.
    const Vector& vector = named("extended metadata");
    Array<uint8_t> bytes = copyBytes(PrByteView{vector.bytes.data(), vector.bytes.count()});
    const uint32_t header = chunkAt(bytes.span(), "VP8X") + 8;
    bytes[header + 4] = 0xFF;
    bytes[header + 5] = 0x3F;
    bytes[header + 7] = 0xFF;
    bytes[header + 8] = 0x3F;
    CHECK(openStatus(bytes.span(), nullptr) == PrStatus_Limit);
    // A lossless bitstream of 16384 by 16384 pixels whose stream ends at once.
    const uint8_t huge[] = {
        'R', 'I', 'F', 'F', 18, 0, 0, 0, 'W', 'E', 'B', 'P', 'V', 'P', '8', 'L', 6, 0, 0, 0,
        0x2F, 0xFF, 0xFF, 0xFF, 0x0F, 0x00,
    };
    CHECK(openStatus(Span<const uint8_t>(huge), nullptr) == PrStatus_Limit);
    PrWebpLimits limits = prWebpLimitsDefault();
    limits.pixels = uint64_t(1) << 30;
    limits.outputBytes = uint64_t(1) << 33;
    limits.workingBytes = uint64_t(1) << 34;
    CHECK(openStatus(Span<const uint8_t>(huge), &limits) == PrStatus_InvalidData);
}

TEST_CASE("a lossy webp still image decodes to its planes, and no other image does") {
    const Vector& lossy = named("lossy alpha compressed");
    Array<uint8_t> luma(6 * 5);
    Array<uint8_t> blue(3 * 3);
    Array<uint8_t> red(3 * 3);
    PrWebpPlanes planes = {
        .luma = {luma.data(), 6, {6, 5}},
        .blueDifference = {blue.data(), 3, {3, 3}},
        .redDifference = {red.data(), 3, {3, 3}},
    };
    const PrByteView file = {lossy.bytes.data(), lossy.bytes.count()};
    REQUIRE(prWebpDecodePlanes(file, nullptr, &planes) == PrStatus_Ok);
    // The picture brightens to the right, and so does its luma, under transparent pixels as under opaque ones.
    uint32_t brighter = 0;
    for (uint32_t i = 1; i < 6; ++i)
        brighter += luma[i] > luma[i - 1] ? 1 : 0;
    CHECK(brighter >= 3);
    planes.luma.extent = {5, 5};
    CHECK(prWebpDecodePlanes(file, nullptr, &planes) == PrStatus_InvalidData);
    const Vector& lossless = named("lossless gradient");
    CHECK(prWebpDecodePlanes({lossless.bytes.data(), lossless.bytes.count()}, nullptr, &planes) ==
        PrStatus_NotSupported);
    const Vector& animation = named("animation");
    CHECK(prWebpDecodePlanes({animation.bytes.data(), animation.bytes.count()}, nullptr, &planes) ==
        PrStatus_NotSupported);
}

TEST_CASE("WebP's public header compiles as C") {
    CHECK(prTestWebpCApi() == 0);
}
