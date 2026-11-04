#include "pr/media_segment.h"

#include "media/tests/media_test_stream.h"

#include "tools/harness/test.h"

#include <string>

namespace pr_impl {

namespace {

struct Collected {
    std::vector<std::string> codecs;
    std::vector<std::string> languages;
    std::vector<PrMediaTrackKind> kinds;
    double duration = 0;
    std::vector<PrMediaCodedFrame> frames;
    std::vector<Bytes> bytes;
};

PrMediaSegmentHandlers handlersFor(Collected* out) {
    PrMediaSegmentHandlers handlers{};
    handlers.context = out;
    handlers.initSegment = [](void* context, const PrMediaInitSegment* segment) {
        Collected* c = static_cast<Collected*>(context);
        c->duration = segment->duration;
        for (uint32_t i = 0; i < segment->trackCount; ++i) {
            c->codecs.emplace_back(segment->tracks[i].codec.data, segment->tracks[i].codec.size);
            c->languages.emplace_back(segment->tracks[i].language.data, segment->tracks[i].language.size);
            c->kinds.push_back(segment->tracks[i].kind);
        }
    };
    handlers.codedFrames = [](void* context, const PrMediaCodedFrame* frames, uint32_t count) {
        Collected* c = static_cast<Collected*>(context);
        for (uint32_t i = 0; i < count; ++i) {
            c->frames.push_back(frames[i]);
            c->bytes.emplace_back(frames[i].bytes.data, frames[i].bytes.data + frames[i].bytes.size);
        }
    };
    return handlers;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("an MP4 byte stream's initialization segment names its tracks' codecs") {
    CHECK(prMediaByteStreamFromType("video/mp4; codecs=\"avc1.4D401E\"") == PrMediaByteStream_Mp4);
    CHECK(prMediaByteStreamFromType("AUDIO/MP4") == PrMediaByteStream_Mp4);
    CHECK(prMediaByteStreamFromType("video/x-unknown") == PrMediaByteStream_None);
    PrMediaSegmentParser* parser = prMediaSegmentParserCreate(PrMediaByteStream_Mp4);
    Collected c;
    PrMediaSegmentHandlers handlers = handlersFor(&c);
    Bytes init = initSegment();
    REQUIRE(prMediaSegmentParserAppend(parser, init, &handlers) == PrStatus_Ok);
    CHECK(c.codecs == std::vector<std::string>{"avc1.4D401E", "mp4a.40.2"});
    CHECK(c.languages == std::vector<std::string>{"eng", "eng"});
    CHECK(c.kinds == std::vector<PrMediaTrackKind>{PrMediaTrackKind_Video, PrMediaTrackKind_Audio});
    CHECK(c.duration != c.duration);
    CHECK_FALSE(prMediaSegmentParserIsParsingMedia(parser));
    prMediaSegmentParserDestroy(parser);
}

TEST_CASE("an MP4 media segment's samples are coded frames in decode order, with their times and bytes") {
    PrMediaSegmentParser* parser = prMediaSegmentParserCreate(PrMediaByteStream_Mp4);
    Collected c;
    PrMediaSegmentHandlers handlers = handlersFor(&c);
    Bytes stream = cat({initSegment(), mediaSegment(90000, 3, 5)});
    REQUIRE(prMediaSegmentParserAppend(parser, stream, &handlers) == PrStatus_Ok);
    REQUIRE(c.frames.size() == 4);
    CHECK(c.frames[0].track == kVideoTrack);
    CHECK(c.frames[0].decode == doctest::Approx(1.0));
    CHECK(c.frames[0].presentation == doctest::Approx(1.0 + 1.0 / 30));
    CHECK(c.frames[0].duration == doctest::Approx(1.0 / 30));
    CHECK(c.frames[0].key);
    CHECK_FALSE(c.frames[1].key);
    CHECK(c.frames[2].decode == doctest::Approx(1.0 + 2.0 / 30));
    CHECK(c.bytes[1] == Bytes(5, 2));
    CHECK(c.frames[3].track == kAudioTrack);
    CHECK(c.frames[3].key);
    CHECK(c.frames[3].decode == doctest::Approx(1.0));
    CHECK(c.frames[3].duration == doctest::Approx(1024.0 / kAudioTimescale));
    CHECK(c.bytes[3] == Bytes(7, 0xaa));
    prMediaSegmentParserDestroy(parser);
}

TEST_CASE("a byte stream split anywhere parses as when it arrives whole") {
    Bytes stream = cat({initSegment(), mediaSegment(0, 4, 3), mediaSegment(4 * 3000, 4, 3)});
    for (size_t split = 1; split < stream.size(); split += 7) {
        PrMediaSegmentParser* parser = prMediaSegmentParserCreate(PrMediaByteStream_Mp4);
        Collected c;
        PrMediaSegmentHandlers handlers = handlersFor(&c);
        REQUIRE(prMediaSegmentParserAppend(parser, PrByteView{stream.data(), split}, &handlers) == PrStatus_Ok);
        REQUIRE(prMediaSegmentParserAppend(parser, PrByteView{stream.data() + split, stream.size() - split},
            &handlers) == PrStatus_Ok);
        CHECK(c.frames.size() == 10);
        CHECK(c.codecs.size() == 2);
        CHECK_FALSE(prMediaSegmentParserIsParsingMedia(parser));
        prMediaSegmentParserDestroy(parser);
    }
}

TEST_CASE("a media segment before an initialization segment, or a corrupt box, is an error") {
    PrMediaSegmentParser* parser = prMediaSegmentParserCreate(PrMediaByteStream_Mp4);
    Collected c;
    PrMediaSegmentHandlers handlers = handlersFor(&c);
    Bytes media = mediaSegment(0, 2, 3);
    CHECK(prMediaSegmentParserAppend(parser, media, &handlers) == PrStatus_InvalidData);
    prMediaSegmentParserReset(parser);
    Bytes corrupt = {0, 0, 0, 4, 'f', 'r', 'e', 'e'};
    CHECK(prMediaSegmentParserAppend(parser, corrupt, &handlers) == PrStatus_InvalidData);
    prMediaSegmentParserDestroy(parser);
}

TEST_CASE("a parser waiting for the rest of a media segment drops it when reset") {
    PrMediaSegmentParser* parser = prMediaSegmentParserCreate(PrMediaByteStream_Mp4);
    Collected c;
    PrMediaSegmentHandlers handlers = handlersFor(&c);
    Bytes init = initSegment();
    Bytes media = mediaSegment(0, 2, 3);
    REQUIRE(prMediaSegmentParserAppend(parser, init, &handlers) == PrStatus_Ok);
    REQUIRE(prMediaSegmentParserAppend(parser, PrByteView{media.data(), media.size() / 2}, &handlers) == PrStatus_Ok);
    CHECK(prMediaSegmentParserIsParsingMedia(parser));
    prMediaSegmentParserReset(parser);
    CHECK_FALSE(prMediaSegmentParserIsParsingMedia(parser));
    REQUIRE(prMediaSegmentParserAppend(parser, media, &handlers) == PrStatus_Ok);
    CHECK(c.frames.size() == 3);
    prMediaSegmentParserDestroy(parser);
}
