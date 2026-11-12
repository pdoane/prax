#include "pr/float.h"
#include "pr/media_source.h"
#include "pr/string.h"

#include "media/tests/media_test_stream.h"

#include "tools/harness/test.h"

namespace pr_impl {

namespace {

// One second: 30 video frames at 90 kHz, and 43 audio frames of 1024 samples at 44.1 kHz.
constexpr uint32_t kFramesPerSecond = 30;
constexpr uint32_t kAudioPerSecond = 43;
constexpr uint64_t kSecond = 90000;

struct Fixture {
    PrMediaSourceBuffer* buffer = prMediaSourceBufferCreate(PrMediaByteStream_Mp4, 1 << 24);
    uint32_t inits = 0;
    bool firstInit = false;

    ~Fixture() { prMediaSourceBufferDestroy(buffer); }

    PrStatus append(const Bytes& bytes) {
        PrMediaSourceInitFn init = [](void* context, const PrMediaInitSegment*, bool first) {
            Fixture* f = static_cast<Fixture*>(context);
            ++f->inits;
            f->firstInit = first;
            return true;
        };
        return prMediaSourceBufferAppend(buffer, bytes, init, this);
    }

    // Appends the second of media that starts at `second`, in the stream's own time.
    PrStatus appendSecond(uint64_t second, uint32_t size = 4) {
        return append(mediaSegment(second * kSecond, kFramesPerSecond, size, kAudioPerSecond));
    }

    String buffered(bool ended = false) {
        PrMediaTimeRange ranges[8];
        uint32_t count = prMediaSourceBufferBuffered(buffer, ended, ranges, 8);
        String out = "{";
        for (uint32_t i = 0; i < count && i < 8; ++i)
            formatTo(out, " [{:.3f}, {:.3f})", ranges[i].start, ranges[i].end);
        out.append(" }");
        return out;
    }
};

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a source buffer's ranges are where every track has frames") {
    Fixture f;
    REQUIRE(f.append(initSegment()) == PrStatus_Ok);
    CHECK(f.inits == 1);
    CHECK(f.firstInit);
    CHECK(f.buffered() == "{ }");
    REQUIRE(f.appendSecond(0) == PrStatus_Ok);
    CHECK(f.buffered() == "{ [0.033, 0.998) }");
    REQUIRE(f.appendSecond(1) == PrStatus_Ok);
    CHECK(f.buffered() == "{ [0.033, 1.998) }");
    CHECK(prMediaSourceBufferHighestEnd(f.buffer) == doctest::Approx(2 + 1.0 / 30));
    // At the end of the stream, each track's last range runs to the highest end.
    CHECK(f.buffered(true) == "{ [0.033, 2.033) }");
    REQUIRE(prMediaSourceBufferTrackCount(f.buffer) == 2);
    CHECK(prMediaSourceBufferTrack(f.buffer, 0).codec == "avc1.4D401E");
    CHECK(prMediaSourceBufferTrack(f.buffer, 1).kind == PrMediaTrackKind_Audio);
}

TEST_CASE("a later initialization segment must agree with the first") {
    Fixture f;
    REQUIRE(f.append(initSegment()) == PrStatus_Ok);
    REQUIRE(f.append(initSegment()) == PrStatus_Ok);
    CHECK(f.inits == 2);
    CHECK_FALSE(f.firstInit);
    // A stream whose first box is not a box is corrupt.
    Bytes corrupt({0, 0, 0, 2, 'x', 'x', 'x', 'x'});
    CHECK(f.append(corrupt) == PrStatus_InvalidData);
}

TEST_CASE("removing a range takes the frames up to the next random access point") {
    Fixture f;
    REQUIRE(f.append(initSegment()) == PrStatus_Ok);
    REQUIRE(f.appendSecond(0) == PrStatus_Ok);
    REQUIRE(f.appendSecond(1) == PrStatus_Ok);
    prMediaSourceBufferRemove(f.buffer, 0.5, 0.6, 2);
    // Video frames depend on the key frame of their second, so the video goes up to the next second's key frame.
    CHECK(f.buffered() == "{ [0.033, 0.500) [1.033, 1.998) }");
}

TEST_CASE("frames outside the append window are dropped, and a group needs a key frame") {
    Fixture f;
    REQUIRE(f.append(initSegment()) == PrStatus_Ok);
    prMediaSourceBufferSetAppendWindow(f.buffer, PrMediaTimeRange{0.5, 1e9});
    REQUIRE(f.appendSecond(0) == PrStatus_Ok);
    CHECK(f.buffered() == "{ }");
    REQUIRE(f.appendSecond(1) == PrStatus_Ok);
    CHECK(f.buffered() == "{ [1.033, 1.998) }");
}

TEST_CASE("a timestamp offset moves the frames, and sequence mode places groups end to end") {
    Fixture f;
    REQUIRE(f.append(initSegment()) == PrStatus_Ok);
    prMediaSourceBufferSetTimestampOffset(f.buffer, 10);
    REQUIRE(f.appendSecond(0) == PrStatus_Ok);
    CHECK(f.buffered() == "{ [10.033, 10.998) }");

    Fixture s;
    REQUIRE(s.append(initSegment()) == PrStatus_Ok);
    prMediaSourceBufferSetMode(s.buffer, PrMediaAppendMode_Sequence);
    REQUIRE(s.appendSecond(5) == PrStatus_Ok);
    REQUIRE(s.appendSecond(5) == PrStatus_Ok);
    // The first group starts at zero at its first video frame, which drops the audio before it, and the second starts
    // where the first ended.
    CHECK(s.buffered() == "{ [0.013, 1.965) }");
}

TEST_CASE("appending the same range again replaces its frames") {
    Fixture f;
    REQUIRE(f.append(initSegment()) == PrStatus_Ok);
    REQUIRE(f.appendSecond(0, 4) == PrStatus_Ok);
    REQUIRE(f.appendSecond(0, 9) == PrStatus_Ok);
    CHECK(f.buffered() == "{ [0.033, 0.998) }");
    PrMediaBufferedFrame frame{};
    REQUIRE(prMediaSourceBufferNextFrame(f.buffer, 0, kPrF64Nan, 0.5, &frame));
    CHECK(frame.key);
    CHECK(frame.bytes.size == 9);
    prMediaBufferedFrameReset(&frame);
}

TEST_CASE("a reader takes frames in decode order from the random access point before a time") {
    Fixture f;
    REQUIRE(f.append(initSegment()) == PrStatus_Ok);
    REQUIRE(f.appendSecond(0) == PrStatus_Ok);
    REQUIRE(f.appendSecond(1) == PrStatus_Ok);
    PrMediaBufferedFrame frame{};
    REQUIRE(prMediaSourceBufferNextFrame(f.buffer, 0, kPrF64Nan, 1.5, &frame));
    CHECK(frame.key);
    CHECK(frame.decode == doctest::Approx(1.0));
    double decode = frame.decode;
    // The frame stays readable after the buffer removes it.
    prMediaSourceBufferRemove(f.buffer, 0, 3, 3);
    CHECK(frame.bytes.data[0] == 1);
    prMediaBufferedFrameReset(&frame);
    CHECK_FALSE(prMediaSourceBufferNextFrame(f.buffer, 0, decode, 0, &frame));
    CHECK(frame.data == nullptr);
}

TEST_CASE("eviction removes what plays before the current position") {
    Fixture f;
    REQUIRE(f.append(initSegment()) == PrStatus_Ok);
    for (uint64_t second = 0; second < 4; ++second)
        REQUIRE(f.appendSecond(second, 1000) == PrStatus_Ok);
    CHECK(prMediaSourceBufferEvict(f.buffer, 2.5, 1000));
    CHECK_FALSE(prMediaSourceBufferEvict(f.buffer, 2.5, size_t(1) << 30));
    prMediaSourceBufferDestroy(f.buffer);
    f.buffer = prMediaSourceBufferCreate(PrMediaByteStream_Mp4, 70000);
    REQUIRE(f.append(initSegment()) == PrStatus_Ok);
    for (uint64_t second = 0; second < 2; ++second)
        REQUIRE(f.appendSecond(second, 1000) == PrStatus_Ok);
    // The removal runs on to each track's next random access point after the video's key frame at 1.033.
    CHECK(prMediaSourceBufferEvict(f.buffer, 1.5, 30000));
    CHECK(f.buffered() == "{ [1.046, 1.998) }");
}
