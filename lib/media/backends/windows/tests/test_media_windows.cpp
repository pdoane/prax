#include "pr/media_windows.h"
#include "pr/thread.h"

#include "doctest/doctest.h"

#include <vector>

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <objbase.h>
#include <shlwapi.h>

namespace pr_impl {

namespace {

constexpr int32_t kFramesPerSecond = 30;
constexpr int32_t kFrames = 30;
constexpr uint32_t kWidth = 64;

// A height that H.264 codes in more rows than display, so that a frame must crop to the area that displays.
constexpr uint32_t kHeight = 50;

// The red level of frame `index`, which tells the frames apart after lossy compression.
uint8_t redOf(int32_t index) {
    return uint8_t(index * 8);
}

// Appends frame `index` to the writer's stream, a solid color whose red tells it apart. Returns false if it cannot.
bool writeFrame(IMFSinkWriter* writer, DWORD stream, int32_t index) {
    DWORD size = kWidth * kHeight * 4;
    IMFMediaBuffer* buffer = nullptr;
    if (FAILED(MFCreateMemoryBuffer(size, &buffer)))
        return false;
    BYTE* pixels = nullptr;
    buffer->Lock(&pixels, nullptr, nullptr);
    for (DWORD i = 0; i < size; i += 4) {
        pixels[i] = 0;
        pixels[i + 1] = 128;
        pixels[i + 2] = redOf(index);
        pixels[i + 3] = 255;
    }
    buffer->Unlock();
    buffer->SetCurrentLength(size);
    IMFSample* sample = nullptr;
    bool ok = SUCCEEDED(MFCreateSample(&sample));
    if (ok) {
        sample->AddBuffer(buffer);
        sample->SetSampleTime(LONGLONG(index) * 10000000 / kFramesPerSecond);
        sample->SetSampleDuration(10000000 / kFramesPerSecond);
        ok = SUCCEEDED(writer->WriteSample(stream, sample));
        sample->Release();
    }
    buffer->Release();
    return ok;
}

// Returns a video media type of the movie's extent and rate, which the caller's fields complete.
IMFMediaType* videoType(const GUID& subtype) {
    IMFMediaType* type = nullptr;
    MFCreateMediaType(&type);
    type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    type->SetGUID(MF_MT_SUBTYPE, subtype);
    type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(type, MF_MT_FRAME_SIZE, kWidth, kHeight);
    MFSetAttributeRatio(type, MF_MT_FRAME_RATE, kFramesPerSecond, 1);
    MFSetAttributeRatio(type, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    return type;
}

// Returns an H.264 MPEG-4 movie of kFrames frames of kWidth by kHeight, or nothing if the system cannot encode one.
std::vector<uint8_t> encodeMovie() {
    std::vector<uint8_t> bytes;
    IStream* memory = SHCreateMemStream(nullptr, 0);
    IMFByteStream* stream = nullptr;
    IMFAttributes* attributes = nullptr;
    IMFSinkWriter* writer = nullptr;
    if (memory == nullptr || FAILED(MFCreateMFByteStreamOnStream(memory, &stream)))
        return bytes;
    MFCreateAttributes(&attributes, 1);
    attributes->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4);
    bool ok = SUCCEEDED(MFCreateSinkWriterFromURL(nullptr, stream, attributes, &writer));
    attributes->Release();
    if (ok) {
        IMFMediaType* output = videoType(MFVideoFormat_H264);
        output->SetUINT32(MF_MT_AVG_BITRATE, 2000000);
        IMFMediaType* input = videoType(MFVideoFormat_RGB32);
        DWORD index = 0;
        ok = SUCCEEDED(writer->AddStream(output, &index)) &&
            SUCCEEDED(writer->SetInputMediaType(index, input, nullptr)) && SUCCEEDED(writer->BeginWriting());
        for (int32_t i = 0; i < kFrames && ok; ++i)
            ok = writeFrame(writer, index, i);
        ok = ok && SUCCEEDED(writer->Finalize());
        input->Release();
        output->Release();
        writer->Release();
    }
    STATSTG stat{};
    if (ok && SUCCEEDED(memory->Stat(&stat, STATFLAG_NONAME))) {
        bytes.resize(size_t(stat.cbSize.QuadPart));
        ULONG read = 0;
        memory->Seek(LARGE_INTEGER{}, STREAM_SEEK_SET, nullptr);
        memory->Read(bytes.data(), ULONG(bytes.size()), &read);
        bytes.resize(read);
    }
    stream->Release();
    memory->Release();
    return bytes;
}

// Asks for the frame at `seconds` until the decoder answers the frame that shows there, for up to five seconds. A
// decoder answers the last frame that it decoded until it catches up.
bool waitForFrame(PrMediaDecoder* decoder, double seconds, PrMediaFrame* out) {
    for (int i = 0; i < 5000; ++i) {
        if (prMediaDecoderFrame(decoder, seconds, out) && out->current)
            return true;
        prThreadSleep(1);
    }
    return false;
}

// Returns the byte `channel` of the frame's center pixel.
uint8_t centerChannel(const PrMediaFrame& frame, uint32_t channel) {
    return frame.pixels.data[(frame.pixels.extent.y / 2) * frame.pixels.stride + (frame.pixels.extent.x / 2) * 4 + channel];
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("the Media Foundation codec reads a movie from memory and decodes the frame that shows at each time") {
    PrMediaCodec* codec = prMediaWindowsCreate();
    REQUIRE(codec != nullptr);
    std::vector<uint8_t> bytes = encodeMovie();
    REQUIRE(!bytes.empty());
    PrMediaDecoder* decoder = nullptr;
    PrMediaInfo info{};
    REQUIRE(prMediaDecoderCreate(codec, PrByteView{bytes.data(), bytes.size()}, &decoder, &info) == PrStatus_Ok);
    CHECK(info.hasVideo);
    CHECK(!info.hasAudio);
    CHECK(info.videoExtent.x == kWidth);
    CHECK(info.videoExtent.y == kHeight);
    CHECK(info.duration == doctest::Approx(1.0).epsilon(0.05));
    CHECK(prMediaDecoderHasFrames(decoder));

    PrMediaFrame frame{};
    REQUIRE(waitForFrame(decoder, 0, &frame));
    CHECK(frame.time == 0);
    CHECK(frame.pixels.extent.x == kWidth);
    CHECK(frame.pixels.extent.y == kHeight);
    CHECK(centerChannel(frame, 2) <= redOf(0) + 16);
    CHECK(centerChannel(frame, 3) == 255);
    uint64_t first = frame.serial;
    // The same time answers the same frame.
    REQUIRE(prMediaDecoderFrame(decoder, 0.01, &frame));
    CHECK(frame.serial == first);

    // Playing forward reaches each frame in turn, and a time between frames answers the earlier one.
    REQUIRE(waitForFrame(decoder, 10.5 / kFramesPerSecond, &frame));
    CHECK(frame.time == doctest::Approx(10.0 / kFramesPerSecond));
    CHECK(int(centerChannel(frame, 2)) == doctest::Approx(redOf(10)).epsilon(0.15));

    // A time before what the decoder kept seeks back.
    REQUIRE(waitForFrame(decoder, 2.5 / kFramesPerSecond, &frame));
    CHECK(frame.time == doctest::Approx(2.0 / kFramesPerSecond));
    CHECK(frame.serial != first);

    // A time far past what the decoder reached seeks ahead, and the frames from the key frame up to it are dropped.
    REQUIRE(waitForFrame(decoder, 27.5 / kFramesPerSecond, &frame));
    CHECK(frame.time == doctest::Approx(27.0 / kFramesPerSecond));
    CHECK(int(centerChannel(frame, 2)) == doctest::Approx(redOf(27)).epsilon(0.15));

    // The last frame shows from its time on.
    REQUIRE(waitForFrame(decoder, 0.99, &frame));
    CHECK(frame.time == doctest::Approx(29.0 / kFramesPerSecond));
    prMediaDecoderDestroy(decoder);

    // Bytes that are no container that Media Foundation reads are not supported, and a container's start is corrupt.
    std::vector<uint8_t> noise(256, 0x5a);
    CHECK(prMediaDecoderCreate(codec, PrByteView{noise.data(), noise.size()}, &decoder, &info) ==
        PrStatus_NotSupported);
    CHECK(decoder == nullptr);
    std::vector<uint8_t> truncated(bytes.begin(), bytes.begin() + 16);
    CHECK(prMediaDecoderCreate(codec, PrByteView{truncated.data(), truncated.size()}, &decoder, &info) != PrStatus_Ok);
    CHECK(decoder == nullptr);
    prMediaCodecDestroy(codec);
}

TEST_CASE("the Media Foundation codec decodes for a thread in a single-threaded apartment, as a window's thread is") {
    PrThread thread{};
    bool decoded = false;
    prThreadStart(&thread, [](void* context) {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        PrMediaCodec* codec = prMediaWindowsCreate();
        std::vector<uint8_t> bytes = encodeMovie();
        PrMediaDecoder* decoder = nullptr;
        PrMediaInfo info{};
        if (prMediaDecoderCreate(codec, PrByteView{bytes.data(), bytes.size()}, &decoder, &info) == PrStatus_Ok) {
            PrMediaFrame frame{};
            *static_cast<bool*>(context) = waitForFrame(decoder, 5.5 / kFramesPerSecond, &frame);
            prMediaDecoderDestroy(decoder);
        }
        prMediaCodecDestroy(codec);
        CoUninitialize();
    }, &decoded);
    prThreadJoin(&thread);
    CHECK(decoded);
}

TEST_CASE("the Media Foundation codec answers a type by its container and the decoders that the system has") {
    PrMediaCodec* codec = prMediaWindowsCreate();
    REQUIRE(codec != nullptr);
    CHECK(prMediaCodecSupportsType(codec, "video/mp4") == PrMediaSupport_Maybe);
    CHECK(prMediaCodecSupportsType(codec, " Video/MP4 ; codecs=\"avc1.42E01E, mp4a.40.2\"") == PrMediaSupport_Probably);
    CHECK(prMediaCodecSupportsType(codec, "video/mp4; codecs=\"nope\"") == PrMediaSupport_No);
    CHECK(prMediaCodecSupportsType(codec, "text/plain") == PrMediaSupport_No);
    CHECK(prMediaCodecSupportsType(codec, "") == PrMediaSupport_No);
    prMediaCodecDestroy(codec);
}
