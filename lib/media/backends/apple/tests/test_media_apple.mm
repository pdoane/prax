#include "pr/media_apple.h"
#include "pr/thread.h"

#include "doctest/doctest.h"

#include <vector>

#import <AVFoundation/AVFoundation.h>

namespace pr_impl {

namespace {

constexpr int32_t kFramesPerSecond = 30;
constexpr int32_t kFrames = 30;
constexpr size_t kWidth = 64;
constexpr size_t kHeight = 48;

// The red level of frame `index`, which tells the frames apart after lossy compression.
uint8_t redOf(int32_t index) {
    return uint8_t(index * 8);
}

// Returns an H.264 MPEG-4 movie of kFrames frames of kWidth by kHeight, each a solid color whose red tells it apart, or
// nothing if the system cannot encode one.
std::vector<uint8_t> encodeMovie() {
    @autoreleasepool {
        NSString* path = [NSTemporaryDirectory()
            stringByAppendingPathComponent:[NSString stringWithFormat:@"prax-media-%@.mp4", NSUUID.UUID.UUIDString]];
        NSURL* url = [NSURL fileURLWithPath:path];
        NSError* error = nil;
        AVAssetWriter* writer = [AVAssetWriter assetWriterWithURL:url fileType:AVFileTypeMPEG4 error:&error];
        if (writer == nil)
            return {};
        NSDictionary* settings = @{
            AVVideoCodecKey: AVVideoCodecTypeH264,
            AVVideoWidthKey: @(kWidth),
            AVVideoHeightKey: @(kHeight),
        };
        AVAssetWriterInput* input = [AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeVideo
            outputSettings:settings];
        NSDictionary* attributes = @{(id)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA)};
        AVAssetWriterInputPixelBufferAdaptor* adaptor =
            [AVAssetWriterInputPixelBufferAdaptor assetWriterInputPixelBufferAdaptorWithAssetWriterInput:input
                sourcePixelBufferAttributes:attributes];
        [writer addInput:input];
        if (![writer startWriting])
            return {};
        [writer startSessionAtSourceTime:kCMTimeZero];
        for (int32_t i = 0; i < kFrames; ++i) {
            while (!input.readyForMoreMediaData)
                prThreadSleep(1);
            CVPixelBufferRef buffer = nullptr;
            CVPixelBufferPoolCreatePixelBuffer(nullptr, adaptor.pixelBufferPool, &buffer);
            if (buffer == nullptr)
                return {};
            CVPixelBufferLockBaseAddress(buffer, 0);
            uint8_t* pixels = static_cast<uint8_t*>(CVPixelBufferGetBaseAddress(buffer));
            size_t stride = CVPixelBufferGetBytesPerRow(buffer);
            for (size_t y = 0; y < kHeight; ++y) {
                for (size_t x = 0; x < kWidth; ++x) {
                    uint8_t* p = pixels + y * stride + x * 4;
                    p[0] = 0;
                    p[1] = 128;
                    p[2] = redOf(i);
                    p[3] = 255;
                }
            }
            CVPixelBufferUnlockBaseAddress(buffer, 0);
            [adaptor appendPixelBuffer:buffer withPresentationTime:CMTimeMake(i, kFramesPerSecond)];
            CVPixelBufferRelease(buffer);
        }
        [input markAsFinished];
        dispatch_semaphore_t done = dispatch_semaphore_create(0);
        [writer finishWritingWithCompletionHandler:^{
            dispatch_semaphore_signal(done);
        }];
        dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
        NSData* data = [NSData dataWithContentsOfURL:url];
        [NSFileManager.defaultManager removeItemAtURL:url error:nil];
        if (writer.status != AVAssetWriterStatusCompleted || data == nil)
            return {};
        const uint8_t* bytes = static_cast<const uint8_t*>(data.bytes);
        return std::vector<uint8_t>(bytes, bytes + data.length);
    }
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

// Returns the red level at the frame's center.
uint8_t centerRed(const PrMediaFrame& frame) {
    return frame.pixels.data[(frame.pixels.extent.y / 2) * frame.pixels.stride + (frame.pixels.extent.x / 2) * 4 + 2];
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("the AVFoundation codec reads a movie from memory and decodes the frame that shows at each time") {
    std::vector<uint8_t> bytes = encodeMovie();
    REQUIRE(!bytes.empty());
    PrMediaCodec* codec = prMediaAppleCreate();
    CHECK(prMediaCodecSupportsType(codec, "video/mp4") != PrMediaSupport_No);
    CHECK(prMediaCodecSupportsType(codec, "video/mp4; codecs=\"avc1.42E01E\"") == PrMediaSupport_Probably);
    CHECK(prMediaCodecSupportsType(codec, "text/plain") == PrMediaSupport_No);
    PrMediaDecoder* decoder = nullptr;
    PrMediaInfo info{};
    REQUIRE(prMediaDecoderCreate(codec, bytes, &decoder, &info) == PrStatus_Ok);
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
    CHECK(centerRed(frame) <= redOf(0) + 16);
    uint64_t first = frame.serial;
    // The same time answers the same frame.
    REQUIRE(prMediaDecoderFrame(decoder, 0.01, &frame));
    CHECK(frame.serial == first);

    // Playing forward reaches each frame in turn, and a time between frames answers the earlier one.
    REQUIRE(waitForFrame(decoder, 10.5 / kFramesPerSecond, &frame));
    CHECK(frame.time == doctest::Approx(10.0 / kFramesPerSecond));
    CHECK(int(centerRed(frame)) == doctest::Approx(redOf(10)).epsilon(0.15));

    // A time before what the decoder kept seeks back.
    REQUIRE(waitForFrame(decoder, 2.0 / kFramesPerSecond, &frame));
    CHECK(frame.time == doctest::Approx(2.0 / kFramesPerSecond));
    CHECK(frame.serial != first);
    prMediaDecoderDestroy(decoder);

    // Bytes that are no container that AVFoundation reads are not supported.
    std::vector<uint8_t> noise(256, 0x5a);
    CHECK(prMediaDecoderCreate(codec, noise, &decoder, &info) == PrStatus_NotSupported);
    CHECK(decoder == nullptr);
    prMediaCodecDestroy(codec);
}
