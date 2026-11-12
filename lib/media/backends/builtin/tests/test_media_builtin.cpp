#include "pr/array.h"
#include "pr/media_builtin.h"
#include "pr/mem.h"
#include "pr/span.h"

#include "tools/harness/test.h"

namespace pr_impl {

namespace {

void appendU32(Array<uint8_t>* out, uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8)
        out->push(uint8_t(value >> shift));
}

// Returns a box of `type` around `payload`.
Array<uint8_t> box(const char* type, const Array<uint8_t>& payload) {
    Array<uint8_t> out;
    appendU32(&out, 8 + payload.count());
    out.append(reinterpret_cast<const uint8_t*>(type), 4);
    out.append(payload.span());
    return out;
}

// Returns the parts one after another.
Array<uint8_t> join(Span<const Array<uint8_t>> parts) {
    Array<uint8_t> out;
    for (const Array<uint8_t>& part : parts)
        out.append(part.span());
    return out;
}

// Returns a track whose handler is `handler`, of `width` by `height` as its tkhd box declares.
Array<uint8_t> track(const char* handler, uint32_t width, uint32_t height) {
    Array<uint8_t> header(76);
    appendU32(&header, width << 16);
    appendU32(&header, height << 16);
    Array<uint8_t> named(8);
    named.append(reinterpret_cast<const uint8_t*>(handler), 4);
    named.resize(named.count() + 12);
    const Array<uint8_t> parts[] = {box("tkhd", header), box("mdia", box("hdlr", named))};
    return box("trak", join(parts));
}

// Returns a movie of `milliseconds` with the tracks.
Array<uint8_t> movie(uint32_t milliseconds, const Array<uint8_t>& tracks) {
    Array<uint8_t> header(12);
    appendU32(&header, 1000);
    appendU32(&header, milliseconds);
    header.resize(header.count() + 80);
    Array<uint8_t> brand({'i', 's', 'o', 'm', 0, 0, 0, 0});
    const Array<uint8_t> moov[] = {box("mvhd", header), tracks};
    const Array<uint8_t> parts[] = {box("ftyp", brand), box("moov", join(moov))};
    return join(parts);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("the built-in codec reads a movie's duration and its first video track's extent, and decodes no frames") {
    PrMediaCodec* codec = prMediaBuiltinCreate();
    const Array<uint8_t> tracks[] = {track("soun", 0, 0), track("vide", 320, 240), track("vide", 8, 8)};
    Array<uint8_t> bytes = movie(2500, join(tracks));
    PrMediaDecoder* decoder = nullptr;
    PrMediaInfo info{};
    REQUIRE(prMediaDecoderCreate(codec, bytes, &decoder, &info) == PrStatus_Ok);
    CHECK(info.duration == 2.5);
    CHECK(info.hasVideo);
    CHECK(info.hasAudio);
    CHECK(info.videoExtent.x == 320);
    CHECK(info.videoExtent.y == 240);
    CHECK(!prMediaDecoderHasFrames(decoder));
    PrMediaFrame frame{};
    CHECK(!prMediaDecoderFrame(decoder, 0, &frame));
    prMediaDecoderDestroy(decoder);

    Array<uint8_t> audio = movie(1000, track("soun", 0, 0));
    REQUIRE(prMediaDecoderCreate(codec, audio, &decoder, &info) == PrStatus_Ok);
    CHECK(!info.hasVideo);
    CHECK(info.videoExtent.x == 0);
    prMediaDecoderDestroy(decoder);

    // A resource without a movie header, or without a file type box, is not a movie.
    Array<uint8_t> untimed = box("ftyp", Array<uint8_t>(8));
    CHECK(prMediaDecoderCreate(codec, untimed, &decoder, &info) == PrStatus_NotSupported);
    CHECK(decoder == nullptr);
    Array<uint8_t> noise(64);
    prMemFill(noise.data(), 0x5a, noise.count());
    CHECK(prMediaDecoderCreate(codec, noise, &decoder, &info) == PrStatus_NotSupported);
    prMediaCodecDestroy(codec);
}

TEST_CASE("the built-in codec may read an MP4 type, whatever codecs it names, and no other type") {
    PrMediaCodec* codec = prMediaBuiltinCreate();
    CHECK(prMediaCodecSupportsType(codec, "video/mp4") == PrMediaSupport_Maybe);
    CHECK(prMediaCodecSupportsType(codec, " Audio/MP4 ; codecs=\"mp4a.40.2\"") == PrMediaSupport_Maybe);
    CHECK(prMediaCodecSupportsType(codec, "video/webm") == PrMediaSupport_No);
    CHECK(prMediaCodecSupportsType(codec, "") == PrMediaSupport_No);
    prMediaCodecDestroy(codec);
}
