#include "pr/media_apple.h"
#include "pr/media_backend.h"
#include "pr/thread.h"

#include "tools/harness/test.h"

#include <algorithm>
#include <string>
#include <utility>
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

// Returns an AAC file of `seconds` of a sine at `frequency`, or nothing if the system cannot encode one.
std::vector<uint8_t> encodeTone(double frequency, double seconds) {
    @autoreleasepool {
        NSString* path = [NSTemporaryDirectory()
            stringByAppendingPathComponent:[NSString stringWithFormat:@"prax-tone-%@.m4a", NSUUID.UUID.UUIDString]];
        NSURL* url = [NSURL fileURLWithPath:path];
        NSDictionary* settings = @{
            AVFormatIDKey: @(kAudioFormatMPEG4AAC),
            AVSampleRateKey: @44100,
            AVNumberOfChannelsKey: @1,
        };
        NSError* error = nil;
        std::vector<uint8_t> bytes;
        {
            AVAudioFile* file = [[AVAudioFile alloc] initForWriting:url settings:settings error:&error];
            if (file == nil)
                return {};
            AVAudioFormat* format = file.processingFormat;
            AVAudioFrameCount frames = AVAudioFrameCount(seconds * format.sampleRate);
            AVAudioPCMBuffer* buffer = [[AVAudioPCMBuffer alloc] initWithPCMFormat:format frameCapacity:frames];
            buffer.frameLength = frames;
            for (AVAudioFrameCount i = 0; i < frames; ++i)
                buffer.floatChannelData[0][i] = float(0.5 * sin(2 * M_PI * frequency * i / format.sampleRate));
            if (![file writeFromBuffer:buffer error:&error])
                return {};
        }
        NSData* data = [NSData dataWithContentsOfURL:url];
        [NSFileManager.defaultManager removeItemAtURL:url error:nil];
        if (data != nil) {
            const uint8_t* start = static_cast<const uint8_t*>(data.bytes);
            bytes.assign(start, start + data.length);
        }
        return bytes;
    }
}

// Reads the decoder's audio until `frames` frames arrive or the audio ends, waiting between reads. Writes where the
// first frame lies to `time`.
std::vector<float> readAudio(PrMediaDecoder* decoder, uint32_t frames, double* time) {
    std::vector<float> samples(frames);
    uint32_t got = 0;
    bool timed = false;
    for (int waits = 0; got < frames && waits < 2000;) {
        PrMediaAudioChunk chunk = prMediaDecoderAudioRead(decoder, samples.data() + got, frames - got);
        if (chunk.ended)
            break;
        if (chunk.frames == 0) {
            prThreadSleep(1);
            ++waits;
            continue;
        }
        if (!timed)
            *time = chunk.time;
        timed = true;
        got += chunk.frames;
    }
    samples.resize(got);
    return samples;
}

// The frequency of the samples at `rate`, by their crossings of zero.
double frequencyOf(const std::vector<float>& samples, double rate) {
    uint32_t crossings = 0;
    for (size_t i = 1; i < samples.size(); ++i)
        crossings += (samples[i - 1] < 0) != (samples[i] < 0) ? 1 : 0;
    return double(crossings) / 2 / (double(samples.size()) / rate);
}

// A track's compressed samples, as a media source's frames, with what a sample decoder needs to know of the track.
struct Compressed {
    PrMediaTrackInfo track{};
    std::string codec;
    std::vector<uint8_t> config;
    std::vector<std::vector<uint8_t>> bytes;
    std::vector<PrMediaBufferedFrame> frames;
};

// Returns the bytes of the descriptor of `tag` inside an MPEG-4 ES descriptor, or nothing.
std::vector<uint8_t> descriptorOf(const uint8_t* data, size_t size, uint8_t tag) {
    for (size_t at = 0; at + 2 < size; ++at) {
        if (data[at] != tag)
            continue;
        size_t length = 0;
        size_t i = at + 1;
        for (int k = 0; k < 4 && i < size; ++k, ++i) {
            length = length << 7 | (data[i] & 0x7f);
            if ((data[i] & 0x80) == 0) {
                ++i;
                break;
            }
        }
        if (length != 0 && i + length <= size)
            return std::vector<uint8_t>(data + i, data + i + length);
    }
    return {};
}

// Reads the first track of `kind` from a file's bytes without decoding it: its samples and its configuration record.
Compressed compressedOf(const std::vector<uint8_t>& file, NSString* kind) {
    Compressed out;
    @autoreleasepool {
        NSString* path = [NSTemporaryDirectory()
            stringByAppendingPathComponent:[NSString stringWithFormat:@"prax-samples-%@.mp4", NSUUID.UUID.UUIDString]];
        [[NSData dataWithBytes:file.data() length:file.size()] writeToFile:path atomically:NO];
        AVURLAsset* asset = [AVURLAsset URLAssetWithURL:[NSURL fileURLWithPath:path] options:nil];
        AVAssetTrack* track = nil;
        for (AVAssetTrack* candidate in asset.tracks) {
            if (track == nil && [candidate.mediaType isEqualToString:kind])
                track = candidate;
        }
        AVAssetReader* reader = [[AVAssetReader alloc] initWithAsset:asset error:nil];
        AVAssetReaderTrackOutput* output = [[AVAssetReaderTrackOutput alloc] initWithTrack:track outputSettings:nil];
        [reader addOutput:output];
        [reader startReading];
        CMFormatDescriptionRef format = (__bridge CMFormatDescriptionRef)track.formatDescriptions.firstObject;
        if ([kind isEqualToString:AVMediaTypeVideo]) {
            NSDictionary* atoms = (__bridge NSDictionary*)CMFormatDescriptionGetExtension(format,
                kCMFormatDescriptionExtension_SampleDescriptionExtensionAtoms);
            NSData* avcc = atoms[@"avcC"];
            out.config.assign(static_cast<const uint8_t*>(avcc.bytes),
                static_cast<const uint8_t*>(avcc.bytes) + avcc.length);
            char name[16];
            snprintf(name, sizeof name, "avc1.%02X%02X%02X", out.config[1], out.config[2], out.config[3]);
            out.codec = name;
            CMVideoDimensions size = CMVideoFormatDescriptionGetDimensions(format);
            out.track.kind = PrMediaTrackKind_Video;
            out.track.extent = PrUVec2{uint32_t(size.width), uint32_t(size.height)};
        } else {
            size_t cookieSize = 0;
            const uint8_t* cookie = static_cast<const uint8_t*>(CMAudioFormatDescriptionGetMagicCookie(format,
                &cookieSize));
            out.config = descriptorOf(cookie, cookieSize, 5);
            out.codec = "mp4a.40.2";
            const AudioStreamBasicDescription* stream = CMAudioFormatDescriptionGetStreamBasicDescription(format);
            out.track.kind = PrMediaTrackKind_Audio;
            out.track.sampleRate = uint32_t(stream->mSampleRate);
            out.track.channels = stream->mChannelsPerFrame;
        }
        for (CMSampleBufferRef sample = [output copyNextSampleBuffer]; sample != nullptr;
                sample = [output copyNextSampleBuffer]) {
            CMBlockBufferRef block = CMSampleBufferGetDataBuffer(sample);
            // An audio buffer holds many packets, which its packet descriptions place. Their size is in bytes.
            const AudioStreamPacketDescription* packets = nullptr;
            size_t packetBytes = 0;
            if (block != nullptr && CMSampleBufferGetAudioStreamPacketDescriptionsPtr(sample, &packets, &packetBytes) ==
                    noErr && packets != nullptr) {
                size_t packetCount = packetBytes / sizeof(AudioStreamPacketDescription);
                double start = CMTimeGetSeconds(CMSampleBufferGetPresentationTimeStamp(sample));
                double rate = out.track.sampleRate;
                for (size_t i = 0; i < packetCount; ++i) {
                    std::vector<uint8_t>& bytes = out.bytes.emplace_back(packets[i].mDataByteSize);
                    CMBlockBufferCopyDataBytes(block, size_t(packets[i].mStartOffset), bytes.size(), bytes.data());
                    double time = start + double(i) * 1024 / rate;
                    out.frames.push_back(PrMediaBufferedFrame{
                        .presentation = time,
                        .decode = time,
                        .duration = 1024 / rate,
                        .key = true,
                    });
                }
            } else if (block != nullptr && CMSampleBufferGetNumSamples(sample) == 1) {
                std::vector<uint8_t>& bytes = out.bytes.emplace_back(CMBlockBufferGetDataLength(block));
                CMBlockBufferCopyDataBytes(block, 0, bytes.size(), bytes.data());
                bool key = true;
                CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sample, false);
                if (attachments != nullptr && CFArrayGetCount(attachments) != 0) {
                    CFDictionaryRef first = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(attachments, 0));
                    key = !CFDictionaryContainsKey(first, kCMSampleAttachmentKey_NotSync);
                }
                out.frames.push_back(PrMediaBufferedFrame{
                    .presentation = CMTimeGetSeconds(CMSampleBufferGetPresentationTimeStamp(sample)),
                    .decode = CMTimeGetSeconds(CMSampleBufferGetDecodeTimeStamp(sample)),
                    .duration = CMTimeGetSeconds(CMSampleBufferGetDuration(sample)),
                    .key = key,
                });
            }
            CFRelease(sample);
        }
        [[NSFileManager defaultManager] removeItemAtPath:path error:nil];
    }
    for (size_t i = 0; i < out.frames.size(); ++i)
        out.frames[i].bytes = PrByteView(out.bytes[i]);
    out.track.codec = PrStringView(out.codec);
    out.track.config = PrByteView(out.config);
    return out;
}

// What a sample decoder gave: each image's presentation and the red at its center, and the samples.
struct Decoded {
    PrMediaCodec* codec = nullptr;
    std::vector<std::pair<double, uint8_t>> images;
    std::vector<float> samples;
};

PrMediaSampleSink sinkOf(Decoded* decoded) {
    PrMediaSampleSink sink{};
    sink.context = decoded;
    sink.image = [](void* context, void* image, double presentation) {
        Decoded* d = static_cast<Decoded*>(context);
        PrGpuImport imported{};
        d->codec->imageImportFn(d->codec, image, &imported);
        PrGpuImportPixels pixels{};
        REQUIRE(prGpuImportLock(&imported, &pixels));
        PrPixelView view = pixels.pixels;
        uint8_t red = view.data[(view.extent.y / 2) * view.stride + (view.extent.x / 2) * 4 + 2];
        prGpuImportUnlock(&imported);
        d->codec->imageReleaseFn(d->codec, image);
        d->images.emplace_back(presentation, red);
    };
    sink.samples = [](void* context, const float* samples, uint32_t frames, double) {
        Decoded* d = static_cast<Decoded*>(context);
        d->samples.insert(d->samples.end(), samples, samples + frames);
    };
    return sink;
}

// Returns the frame's extent.
PrUVec2 extentOf(const PrMediaFrame& frame) {
    PrGpuImportPixels pixels{};
    REQUIRE(prGpuImportLock(&frame.image, &pixels));
    PrUVec2 extent = pixels.pixels.extent;
    prGpuImportUnlock(&frame.image);
    return extent;
}

// Returns the red level at the frame's center.
uint8_t centerRed(const PrMediaFrame& frame) {
    PrGpuImportPixels pixels{};
    REQUIRE(prGpuImportLock(&frame.image, &pixels));
    PrPixelView view = pixels.pixels;
    uint8_t red = view.data[(view.extent.y / 2) * view.stride + (view.extent.x / 2) * 4 + 2];
    prGpuImportUnlock(&frame.image);
    return red;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("the AVFoundation codec decodes a video track's frames one at a time, as a media source gives them") {
    Compressed video = compressedOf(encodeMovie(), AVMediaTypeVideo);
    REQUIRE(video.frames.size() == size_t(kFrames));
    PrMediaCodec* codec = prMediaAppleCreate();
    CHECK(prMediaCodecSupportsSourceType(codec, "video/mp4; codecs=\"avc1.42E01E\"") == PrMediaSupport_Probably);
    CHECK(prMediaCodecSupportsSourceType(codec, "video/mp4; codecs=\"xyz1\"") == PrMediaSupport_No);
    PrMediaSampleDecoder* decoder = nullptr;
    REQUIRE(codec->sampleDecoderCreateFn(codec, &video.track, 0, 0, &decoder) == PrStatus_Ok);
    Decoded decoded;
    decoded.codec = codec;
    PrMediaSampleSink sink = sinkOf(&decoded);
    for (const PrMediaBufferedFrame& frame : video.frames)
        CHECK(decoder->decodeFn(decoder, &frame, &sink));
    decoder->flushFn(decoder);
    REQUIRE(decoded.images.size() == size_t(kFrames));
    // The decoder gives images in decode order, which B-frames make differ from presentation order. Each frame keeps
    // its time and its color through the decoder.
    std::sort(decoded.images.begin(), decoded.images.end());
    // The reader's timestamps include the encoder's delay, so the frames are one frame apart from the first, and
    // compression moves each red level by a few steps.
    double first = decoded.images[0].first;
    for (int32_t i = 0; i < kFrames; ++i) {
        CHECK(decoded.images[size_t(i)].first - first == doctest::Approx(double(i) / kFramesPerSecond));
        CHECK(int(decoded.images[size_t(i)].second) - int(redOf(i)) < 16);
        CHECK(int(decoded.images[size_t(i)].second) - int(redOf(i)) > -16);
    }
    decoder->destroyFn(decoder);
    prMediaCodecDestroy(codec);
}

TEST_CASE("the AVFoundation codec decodes an AAC track's frames one at a time, at the rate asked for") {
    Compressed audio = compressedOf(encodeTone(1000, 1), AVMediaTypeAudio);
    REQUIRE(!audio.frames.empty());
    REQUIRE(!audio.config.empty());
    PrMediaCodec* codec = prMediaAppleCreate();
    PrMediaSampleDecoder* decoder = nullptr;
    REQUIRE(codec->sampleDecoderCreateFn(codec, &audio.track, 48000, 1, &decoder) == PrStatus_Ok);
    Decoded decoded;
    decoded.codec = codec;
    PrMediaSampleSink sink = sinkOf(&decoded);
    for (const PrMediaBufferedFrame& frame : audio.frames)
        CHECK(decoder->decodeFn(decoder, &frame, &sink));
    // About a second at 48 kHz, less the frames that the decoder keeps for its delay.
    CHECK(decoded.samples.size() > 40000);
    CHECK(decoded.samples.size() < 52000);
    std::vector<float> middle(decoded.samples.begin() + 12000, decoded.samples.begin() + 36000);
    CHECK(frequencyOf(middle, 48000) == doctest::Approx(1000).epsilon(0.02));
    decoder->destroyFn(decoder);
    prMediaCodecDestroy(codec);
}

TEST_CASE("the AVFoundation codec decodes AAC at the rate asked for, and seeks in it") {
    std::vector<uint8_t> bytes = encodeTone(1000, 2);
    REQUIRE(!bytes.empty());
    PrMediaCodec* codec = prMediaAppleCreate();
    PrMediaDecoder* decoder = nullptr;
    PrMediaInfo info{};
    REQUIRE(prMediaDecoderCreate(codec, bytes, &decoder, &info) == PrStatus_Ok);
    CHECK(info.hasAudio);
    CHECK(!info.hasVideo);
    REQUIRE(prMediaDecoderAudioStart(decoder, 48000, 1, 0));
    double time = -1;
    std::vector<float> samples = readAudio(decoder, 24000, &time);
    REQUIRE(samples.size() == 24000);
    CHECK(time == doctest::Approx(0).epsilon(0.05));
    CHECK(frequencyOf(samples, 48000) == doctest::Approx(1000).epsilon(0.02));

    prMediaDecoderAudioSeek(decoder, 1.5);
    samples = readAudio(decoder, 12000, &time);
    REQUIRE(samples.size() == 12000);
    CHECK(time == doctest::Approx(1.5).epsilon(0.01));

    // Reading to the end reports the end.
    samples = readAudio(decoder, 96000, &time);
    CHECK(samples.size() < 24000);
    float nothing = 0;
    CHECK(prMediaDecoderAudioRead(decoder, &nothing, 1).ended);
    prMediaDecoderDestroy(decoder);
    prMediaCodecDestroy(codec);
}

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
    CHECK(extentOf(frame) == PrUVec2{uint32_t(kWidth), uint32_t(kHeight)});
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
