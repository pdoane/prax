#include "pr/media_builtin.h"

#include "doctest/doctest.h"

#include <string.h>
#include <vector>

namespace pr_impl {

namespace {

void appendU32(std::vector<uint8_t>* out, uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8)
        out->push_back(uint8_t(value >> shift));
}

// Returns a box of `type` around `payload`.
std::vector<uint8_t> box(const char* type, const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> out;
    appendU32(&out, uint32_t(8 + payload.size()));
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

std::vector<uint8_t> join(std::initializer_list<std::vector<uint8_t>> parts) {
    std::vector<uint8_t> out;
    for (const std::vector<uint8_t>& part : parts)
        out.insert(out.end(), part.begin(), part.end());
    return out;
}

// Returns a track whose handler is `handler`, of `width` by `height` as its tkhd box declares.
std::vector<uint8_t> track(const char* handler, uint32_t width, uint32_t height) {
    std::vector<uint8_t> header(76);
    appendU32(&header, width << 16);
    appendU32(&header, height << 16);
    std::vector<uint8_t> named(8);
    named.insert(named.end(), handler, handler + 4);
    named.resize(named.size() + 12);
    return box("trak", join({box("tkhd", header), box("mdia", box("hdlr", named))}));
}

// Returns a movie of `milliseconds` with the tracks.
std::vector<uint8_t> movie(uint32_t milliseconds, const std::vector<uint8_t>& tracks) {
    std::vector<uint8_t> header(12);
    appendU32(&header, 1000);
    appendU32(&header, milliseconds);
    header.resize(header.size() + 80);
    std::vector<uint8_t> brand = {'i', 's', 'o', 'm', 0, 0, 0, 0};
    return join({box("ftyp", brand), box("moov", join({box("mvhd", header), tracks}))});
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("the built-in codec reads a movie's duration and its first video track's extent, and decodes no frames") {
    PrMediaCodec* codec = prMediaBuiltinCreate();
    std::vector<uint8_t> bytes = movie(2500, join({track("soun", 0, 0), track("vide", 320, 240), track("vide", 8, 8)}));
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

    std::vector<uint8_t> audio = movie(1000, track("soun", 0, 0));
    REQUIRE(prMediaDecoderCreate(codec, audio, &decoder, &info) == PrStatus_Ok);
    CHECK(!info.hasVideo);
    CHECK(info.videoExtent.x == 0);
    prMediaDecoderDestroy(decoder);

    // A resource without a movie header, or without a file type box, is not a movie.
    std::vector<uint8_t> untimed = box("ftyp", std::vector<uint8_t>(8));
    CHECK(prMediaDecoderCreate(codec, untimed, &decoder, &info) == PrStatus_NotSupported);
    CHECK(decoder == nullptr);
    std::vector<uint8_t> noise(64, 0x5a);
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
