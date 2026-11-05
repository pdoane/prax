// The AVFoundation media codec.

#include "pr/media_apple.h"

#include "pr/alloc.h"
#include "pr/ascii.h"
#include "pr/assert.h"
#include "pr/count_of.h"
#include "pr/float.h"
#include "pr/media_audio.h"
#include "pr/memory.h"
#include "pr/mime.h"
#include "pr/mutex.h"
#include "pr/raw_array.h"
#include "pr/thread.h"

#import <AVFoundation/AVFoundation.h>
#import <AudioToolbox/AudioToolbox.h>
#import <CoreVideo/CoreVideo.h>
#import <VideoToolbox/VideoToolbox.h>

// Serves a resource's bytes from memory to an asset, through a URL that no other loader handles.
@interface PrMediaAppleLoader : NSObject <AVAssetResourceLoaderDelegate>
@property(nonatomic, strong) NSData* data;
@property(nonatomic, copy) NSString* contentType;   // the resource's uniform type identifier
@end

@implementation PrMediaAppleLoader

- (BOOL)resourceLoader:(AVAssetResourceLoader*)resourceLoader
        shouldWaitForLoadingOfRequestedResource:(AVAssetResourceLoadingRequest*)request {
    NSData* data = self.data;
    AVAssetResourceLoadingContentInformationRequest* info = request.contentInformationRequest;
    if (info != nil) {
        info.contentType = self.contentType;
        info.contentLength = (long long)data.length;
        info.byteRangeAccessSupported = YES;
    }
    AVAssetResourceLoadingDataRequest* range = request.dataRequest;
    if (range != nil) {
        NSUInteger offset = NSUInteger(range.currentOffset);
        NSUInteger end = NSUInteger(range.requestedOffset) + NSUInteger(range.requestedLength);
        if (range.requestsAllDataToEndOfResource || end > data.length)
            end = data.length;
        if (offset < end)
            [range respondWithData:[data subdataWithRange:NSMakeRange(offset, end - offset)]];
    }
    [request finishLoading];
    return YES;
}

@end

// What a decoder reads: its resource as an asset, the loader that serves the asset its bytes, and its video track.
@interface PrMediaAppleSource : NSObject
@property(nonatomic, strong) AVURLAsset* asset;
@property(nonatomic, strong) PrMediaAppleLoader* loader;
@property(nonatomic, strong) dispatch_queue_t queue;    // where the loader serves the asset
@property(nonatomic, strong) AVAssetTrack* video;       // nil for a resource without video
@property(nonatomic, strong) AVAssetTrack* audio;       // nil for a resource without audio
@end

@implementation PrMediaAppleSource
@end

namespace pr_impl {

namespace {

// The frames that a decoder keeps decoded ahead of the time last asked for.
constexpr uint32_t kFramesAhead = 4;

// How far past the last decoded frame a request may be before the decoder seeks to it rather than decoding toward it,
// in seconds.
constexpr double kSeekAhead = 1;

// A decoded frame, which holds a reference to its pixel buffer.
struct AppleFrame {
    CVPixelBufferRef buffer;
    double time;
    uint64_t serial;
};

struct AppleDecoder : PrMediaDecoder {
    void* source;               // the PrMediaAppleSource, by a retained bridge

    // The decoding thread, which `mutex` and `wake` coordinate with the decoder's user.
    PrThread thread;
    PrMutex mutex;
    PrCondition wake;

    // Guarded by `mutex`.
    RawArray<AppleFrame> frames; // decoded, in time order, from the last at or before `wanted` on
    double wanted;               // the time last asked for
    double seekTo;               // a time to restart decoding at, or NaN
    double start;                // the time that decoding last started at
    double through;              // the time of the last frame decoded since then, or -infinity
    bool dropped;                // frames decoded since `start` were dropped from the front
    bool ended;                  // every frame since `start` is decoded
    bool stopping;
    uint64_t serials;

    // The user's alone: the buffer of the frame last returned, retained and with its base address locked.
    CVPixelBufferRef shown;

    // The audio, from when it starts.
    PrMediaAudioQueue* PR_NULLABLE queue;
    PrThread audioThread;
    bool audioStarted;
};

PrMediaAppleSource* sourceOf(const AppleDecoder* decoder) {
    return (__bridge PrMediaAppleSource*)decoder->source;
}

double secondsOf(CMTime time) {
    return CMTIME_IS_NUMERIC(time) ? CMTimeGetSeconds(time) : kPrF64Nan;
}

// Returns the uniform type identifier of the container that the bytes begin with, or nil for none that AVFoundation
// reads.
NSString* PR_NULLABLE containerType(PrByteView bytes) {
    const uint8_t* b = bytes.data;
    if (bytes.size >= 12 && memcmp(b + 4, "ftyp", 4) == 0)
        return memcmp(b + 8, "qt  ", 4) == 0 ? AVFileTypeQuickTimeMovie : AVFileTypeMPEG4;
    if (bytes.size >= 8 && (memcmp(b + 4, "moov", 4) == 0 || memcmp(b + 4, "mdat", 4) == 0))
        return AVFileTypeQuickTimeMovie;
    if (bytes.size >= 12 && memcmp(b, "RIFF", 4) == 0 && memcmp(b + 8, "WAVE", 4) == 0)
        return AVFileTypeWAVE;
    if (bytes.size >= 3 && (memcmp(b, "ID3", 3) == 0 || (b[0] == 0xff && (b[1] & 0xe0) == 0xe0)))
        return AVFileTypeMPEGLayer3;
    if (bytes.size >= 4 && memcmp(b, "\x1a\x45\xdf\xa3", 4) == 0)
        return @"org.webmproject.webm";
    return nil;
}

// Releases the frames, and empties the list.
void releaseFrames(RawArray<AppleFrame>* frames) {
    for (uint32_t i = 0; i < frames->count; ++i)
        CVPixelBufferRelease(frames->data[i].buffer);
    frames->clear();
}

// Returns a reader of the video track's frames as Bgra8 from `seconds` on, started, or nil if it cannot read them.
AVAssetReaderTrackOutput* PR_NULLABLE startReading(PrMediaAppleSource* source, double seconds,
        AVAssetReader* PR_NULLABLE __strong* out) {
    NSError* error = nil;
    AVAssetReader* reader = [[AVAssetReader alloc] initWithAsset:source.asset error:&error];
    if (reader == nil)
        return nil;
    reader.timeRange = CMTimeRangeMake(CMTimeMakeWithSeconds(seconds, 90000), kCMTimePositiveInfinity);
    // Frames backed by IOSurfaces, which a Metal device imports without copying them.
    NSDictionary* settings = @{
        (id)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA),
        (id)kCVPixelBufferIOSurfacePropertiesKey: @{},
        (id)kCVPixelBufferMetalCompatibilityKey: @YES,
    };
    AVAssetReaderTrackOutput* output = [[AVAssetReaderTrackOutput alloc] initWithTrack:source.video
        outputSettings:settings];
    output.alwaysCopiesSampleData = NO;
    if (![reader canAddOutput:output])
        return nil;
    [reader addOutput:output];
    if (![reader startReading])
        return nil;
    *out = reader;
    return output;
}

// Decodes frames while fewer than kFramesAhead wait past the time last asked for, and restarts at each seek, until the
// decoder stops.
void decodeFrames(AppleDecoder* d) {
    @autoreleasepool {
        PrMediaAppleSource* source = sourceOf(d);
        AVAssetReader* reader = nil;
        AVAssetReaderTrackOutput* output = nil;
        prMutexLock(&d->mutex);
        while (!d->stopping) {
            if (!prF64IsNan(d->seekTo)) {
                double at = d->seekTo;
                d->seekTo = kPrF64Nan;
                releaseFrames(&d->frames);
                d->start = at;
                d->through = -kPrF64Infinity;
                d->dropped = false;
                d->ended = false;
                prMutexUnlock(&d->mutex);
                [reader cancelReading];
                reader = nil;
                output = startReading(source, at, &reader);
                prMutexLock(&d->mutex);
                if (output == nil)
                    d->ended = true;
                continue;
            }
            uint32_t ahead = 0;
            for (uint32_t i = 0; i < d->frames.count; ++i)
                ahead += d->frames.data[i].time > d->wanted ? 1 : 0;
            if (d->ended || ahead >= kFramesAhead) {
                prConditionWait(&d->wake, &d->mutex);
                continue;
            }
            prMutexUnlock(&d->mutex);
            CMSampleBufferRef sample = nullptr;
            @autoreleasepool {
                sample = [output copyNextSampleBuffer];
            }
            CVPixelBufferRef buffer = sample != nullptr ? CMSampleBufferGetImageBuffer(sample) : nullptr;
            double time = sample != nullptr ? secondsOf(CMSampleBufferGetPresentationTimeStamp(sample)) : kPrF64Nan;
            if (buffer != nullptr)
                CVPixelBufferRetain(buffer);
            if (sample != nullptr)
                CFRelease(sample);
            prMutexLock(&d->mutex);
            if (!prF64IsNan(d->seekTo)) {
                CVPixelBufferRelease(buffer);
                continue;
            }
            if (sample == nullptr) {
                d->ended = true;
                continue;
            }
            if (buffer == nullptr || prF64IsNan(time))
                continue;
            d->frames.push(AppleFrame{buffer, time, ++d->serials});
            d->through = time;
        }
        prMutexUnlock(&d->mutex);
        [reader cancelReading];
    }
}

// Returns a reader of the audio track's samples as interleaved floats from `seconds` on, started, or nil if it cannot
// read them.
AVAssetReaderTrackOutput* PR_NULLABLE startReadingAudio(PrMediaAppleSource* source, const PrMediaAudioQueue* queue,
        double seconds, AVAssetReader* PR_NULLABLE __strong* out) {
    NSError* error = nil;
    AVAssetReader* reader = [[AVAssetReader alloc] initWithAsset:source.asset error:&error];
    if (reader == nil)
        return nil;
    reader.timeRange = CMTimeRangeMake(CMTimeMakeWithSeconds(seconds, 90000), kCMTimePositiveInfinity);
    AudioChannelLayout layout{};
    layout.mChannelLayoutTag =
        prMediaAudioQueueChannels(queue) == 1 ? kAudioChannelLayoutTag_Mono : kAudioChannelLayoutTag_Stereo;
    NSDictionary* settings = @{
        AVFormatIDKey: @(kAudioFormatLinearPCM),
        AVSampleRateKey: @(prMediaAudioQueueSampleRate(queue)),
        AVNumberOfChannelsKey: @(prMediaAudioQueueChannels(queue)),
        AVChannelLayoutKey: [NSData dataWithBytes:&layout length:sizeof(layout)],
        AVLinearPCMBitDepthKey: @32,
        AVLinearPCMIsFloatKey: @YES,
        AVLinearPCMIsBigEndianKey: @NO,
        AVLinearPCMIsNonInterleaved: @NO,
    };
    AVAssetReaderTrackOutput* output = [[AVAssetReaderTrackOutput alloc] initWithTrack:source.audio
        outputSettings:settings];
    output.alwaysCopiesSampleData = NO;
    if (![reader canAddOutput:output])
        return nil;
    [reader addOutput:output];
    if (![reader startReading])
        return nil;
    *out = reader;
    return output;
}

// Decodes the audio into the queue, and starts over at each seek, until the decoder stops.
void decodeAudio(AppleDecoder* d) {
    PrMediaAudioQueue* queue = d->queue;
    RawArray<float> samples{};
    PrMediaAppleSource* source = sourceOf(d);
    while (!prMediaAudioQueueIsStopping(queue)) {
        PrMediaAudioSeek request = prMediaAudioQueueRequest(queue);
        uint32_t seek = request.id;
        double seconds = request.seconds;
        @autoreleasepool {
            AVAssetReader* reader = nil;
            AVAssetReaderTrackOutput* output = startReadingAudio(source, queue, seconds, &reader);
            bool begun = false;
            while (output != nil && !prMediaAudioQueueIsInterrupted(queue, seek)) {
                CMSampleBufferRef sample = nullptr;
                @autoreleasepool {
                    sample = [output copyNextSampleBuffer];
                }
                if (sample == nullptr)
                    break;
                uint32_t frames = uint32_t(CMSampleBufferGetNumSamples(sample));
                double time = secondsOf(CMSampleBufferGetPresentationTimeStamp(sample));
                samples.resize(frames * prMediaAudioQueueChannels(queue));
                CMBlockBufferRef block = CMSampleBufferGetDataBuffer(sample);
                bool copied = block != nullptr && CMBlockBufferCopyDataBytes(block, 0,
                    size_t(samples.count) * sizeof(float), samples.data) == kCMBlockBufferNoErr;
                CFRelease(sample);
                if (!copied || prF64IsNan(time))
                    continue;
                // A reader starts at a packet's boundary, so the frames before the time asked for are dropped.
                uint32_t skip = 0;
                if (time < seconds) {
                    double extra = (seconds - time) * prMediaAudioQueueSampleRate(queue);
                    skip = extra >= frames ? frames : uint32_t(extra);
                }
                if (skip == frames)
                    continue;
                if (!begun) {
                    prMediaAudioQueueBegin(queue, seek, time + double(skip) / prMediaAudioQueueSampleRate(queue));
                    begun = true;
                }
                if (!prMediaAudioQueueWrite(queue, seek, samples.data + size_t(skip) * prMediaAudioQueueChannels(queue),
                        frames - skip))
                    break;
            }
            if (!begun)
                prMediaAudioQueueBegin(queue, seek, seconds);
            [reader cancelReading];
        }
        prMediaAudioQueueFinish(queue, seek);
    }
    samples.reset();
}

PrMediaSupport supportsType(PrMediaCodec*, PrStringView type) {
    NSString* text = [[NSString alloc] initWithBytes:type.data length:type.size encoding:NSUTF8StringEncoding];
    if (text == nil || ![AVURLAsset isPlayableExtendedMIMEType:text])
        return PrMediaSupport_No;
    // A type that names its codecs, which the system plays, is probably playable.
    return prAsciiContainsFold(type, "codecs=") ? PrMediaSupport_Probably : PrMediaSupport_Maybe;
}

// Waits for the asset to load the values of `keys`. Returns false if any failed to load.
bool loadValues(id<AVAsynchronousKeyValueLoading> object, NSArray<NSString*>* keys) {
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    [object loadValuesAsynchronouslyForKeys:keys completionHandler:^{
        dispatch_semaphore_signal(done);
    }];
    dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
    for (NSString* key in keys) {
        if ([object statusOfValueForKey:key error:nil] != AVKeyValueStatusLoaded)
            return false;
    }
    return true;
}

PrStatus decoderCreate(PrMediaCodec*, PrByteView bytes, PrMediaDecoder* PR_NULLABLE* PR_NONNULL out, PrMediaInfo* info) {
    @autoreleasepool {
        NSString* type = containerType(bytes);
        if (type == nil)
            return PrStatus_NotSupported;
        PrMediaAppleSource* source = [[PrMediaAppleSource alloc] init];
        source.loader = [[PrMediaAppleLoader alloc] init];
        source.loader.data = [NSData dataWithBytes:bytes.data length:bytes.size];
        source.loader.contentType = type;
        source.queue = dispatch_queue_create("prax.media.loader", DISPATCH_QUEUE_SERIAL);
        source.asset = [AVURLAsset URLAssetWithURL:[NSURL URLWithString:@"prax-media://resource"] options:nil];
        [source.asset.resourceLoader setDelegate:source.loader queue:source.queue];
        if (!loadValues(source.asset, @[@"tracks", @"duration", @"playable"]))
            return PrStatus_NotSupported;
        if (!source.asset.playable)
            return PrStatus_InvalidData;
        double duration = secondsOf(source.asset.duration);
        info->duration = duration;
        NSArray<AVAssetTrack*>* tracks = source.asset.tracks;
        for (AVAssetTrack* track in tracks) {
            if ([track.mediaType isEqualToString:AVMediaTypeAudio]) {
                info->hasAudio = true;
                if (source.audio == nil)
                    source.audio = track;
            }
            if (![track.mediaType isEqualToString:AVMediaTypeVideo] || source.video != nil)
                continue;
            if (!loadValues(track, @[@"naturalSize", @"preferredTransform"]))
                continue;
            source.video = track;
            CGRect shown = CGRectApplyAffineTransform(
                CGRectMake(0, 0, track.naturalSize.width, track.naturalSize.height), track.preferredTransform);
            info->hasVideo = true;
            info->videoExtent = PrUVec2{
                uint32_t(lround(fabs(shown.size.width))),
                uint32_t(lround(fabs(shown.size.height))),
            };
        }
        AppleDecoder* d = allocZeroed<AppleDecoder>();
        d->source = (__bridge_retained void*)source;
        d->seekTo = kPrF64Nan;
        d->through = -kPrF64Infinity;
        d->ended = source.video == nil;
        prMutexInit(&d->mutex);
        prConditionInit(&d->wake);
        if (source.video != nil) {
            d->seekTo = 0;
            prThreadStart(&d->thread, [](void* context) { decodeFrames(static_cast<AppleDecoder*>(context)); }, d);
        }
        *out = d;
        return PrStatus_Ok;
    }
}

void decoderDestroy(PrMediaCodec*, PrMediaDecoder* decoder) {
    AppleDecoder* d = static_cast<AppleDecoder*>(decoder);
    if (d->audioStarted) {
        prMediaAudioQueueStop(d->queue);
        prThreadJoin(&d->audioThread);
        prMediaAudioQueueDestroy(d->queue);
    }
    if (sourceOf(d).video != nil) {
        prMutexLock(&d->mutex);
        d->stopping = true;
        prConditionSignal(&d->wake);
        prMutexUnlock(&d->mutex);
        prThreadJoin(&d->thread);
    }
    if (d->shown != nullptr)
        CVPixelBufferRelease(d->shown);
    releaseFrames(&d->frames);
    d->frames.reset();
    prConditionReset(&d->wake);
    prMutexReset(&d->mutex);
    @autoreleasepool {
        PrMediaAppleSource* source = (__bridge_transfer PrMediaAppleSource*)d->source;
        [source.asset.resourceLoader setDelegate:nil queue:nil];
    }
    prMemoryFree(d);
}

bool decoderFrame(PrMediaCodec*, PrMediaDecoder* decoder, double seconds, PrMediaFrame* out) {
    AppleDecoder* d = static_cast<AppleDecoder*>(decoder);
    prMutexLock(&d->mutex);
    d->wanted = seconds;
    // A time before what decoding kept, or far past what it reached, restarts decoding there.
    bool before = seconds < d->start || (d->dropped && d->frames.count != 0 && seconds < d->frames.data[0].time);
    bool past = !d->ended && seconds > d->through + kSeekAhead && seconds > d->start + kSeekAhead;
    if (prF64IsNan(d->seekTo) && (before || past))
        d->seekTo = seconds;
    // The frame that shows is the last that starts at or before the time. Before the first frame, the first shows.
    uint32_t at = UINT32_MAX;
    if (prF64IsNan(d->seekTo)) {
        for (uint32_t i = 0; i < d->frames.count && d->frames.data[i].time <= seconds; ++i)
            at = i;
        if (at == UINT32_MAX && d->frames.count != 0 && !d->dropped)
            at = 0;
    }
    AppleFrame frame{};
    bool current = false;
    if (at != UINT32_MAX) {
        // The frame shows at the time if the frame after it is decoded, or none follows, or it is the first frame and
        // shows before its own time.
        current = at + 1 < d->frames.count || d->ended || d->frames.data[at].time > seconds;
        for (uint32_t i = 0; i < at; ++i)
            CVPixelBufferRelease(d->frames.data[i].buffer);
        d->frames.erase(0, at);
        d->dropped |= at != 0;
        frame = d->frames.data[0];
        CVPixelBufferRetain(frame.buffer);
    }
    // Without a frame, none will come once decoding has ended with nothing kept.
    bool done = frame.buffer == nullptr && prF64IsNan(d->seekTo) && d->ended && d->frames.count == 0;
    prConditionSignal(&d->wake);
    prMutexUnlock(&d->mutex);
    if (frame.buffer == nullptr) {
        out->current = done;
        return false;
    }
    if (d->shown != nullptr)
        CVPixelBufferRelease(d->shown);
    d->shown = frame.buffer;
    *out = PrMediaFrame{
        .image = PrGpuImport{.kind = PrGpuImportKind_CoreVideo, .handle = frame.buffer},
        .time = frame.time,
        .serial = frame.serial,
        .current = current,
    };
    return true;
}

bool decoderAudioStart(PrMediaCodec*, PrMediaDecoder* decoder, uint32_t sampleRate, uint32_t channels, double seconds) {
    AppleDecoder* d = static_cast<AppleDecoder*>(decoder);
    PR_ASSERT(!d->audioStarted);
    if (sourceOf(d).audio == nil || channels > 2)
        return false;
    d->queue = prMediaAudioQueueCreate(sampleRate, channels, seconds);
    d->audioStarted = true;
    prThreadStart(&d->audioThread, [](void* context) { decodeAudio(static_cast<AppleDecoder*>(context)); }, d);
    return true;
}

void decoderAudioSeek(PrMediaCodec*, PrMediaDecoder* decoder, double seconds) {
    prMediaAudioQueueSeek(static_cast<AppleDecoder*>(decoder)->queue, seconds);
}

PrMediaAudioChunk decoderAudioRead(PrMediaCodec*, PrMediaDecoder* decoder, float* samples, uint32_t frames) {
    return prMediaAudioQueueRead(static_cast<AppleDecoder*>(decoder)->queue, samples, frames);
}

// ---------------------------------------------------------------------------------------------------------------------
// Sample decoders, for media sources

// A decoder of a video track's coded frames, which VideoToolbox decodes into Bgra8 pixel buffers.
struct AppleSampleVideo : PrMediaSampleDecoder {
    CMVideoFormatDescriptionRef format;
    VTDecompressionSessionRef session;
    const PrMediaSampleSink* PR_NULLABLE sink;      // during a decode, what receives its images
};

// A decoder of an audio track's coded frames, which an audio converter decodes into interleaved floats.
struct AppleSampleAudio : PrMediaSampleDecoder {
    AudioConverterRef converter;
    uint32_t inputChannels;
    uint32_t channels;
    RawArray<float> output;

    // The frame being decoded, which the converter's input callback gives it once.
    const PrMediaBufferedFrame* PR_NULLABLE input;
    AudioStreamPacketDescription packet;
};

// The status that the input callback returns once it has given its frame, which ends a conversion.
constexpr OSStatus kInputDone = 'done';

// The video codec types and the boxes of their configuration records, by the sample entry that names them.
struct VideoCodec {
    const char* entry;
    CMVideoCodecType type;
    const char* record;
};

constexpr VideoCodec kVideoCodecs[] = {
    {"avc1", kCMVideoCodecType_H264, "avcC"},
    {"avc3", kCMVideoCodecType_H264, "avcC"},
    {"hvc1", kCMVideoCodecType_HEVC, "hvcC"},
    {"hev1", kCMVideoCodecType_HEVC, "hvcC"},
    {"av01", 'av01', "av1C"},
    {"vp09", 'vp09', "vpcC"},
};

// Returns the video codec that a codecs parameter's codec names, or null for none that VideoToolbox decodes.
const VideoCodec* PR_NULLABLE videoCodecOf(PrStringView codec) {
    for (const VideoCodec& known : kVideoCodecs) {
        PrStringView entry = known.entry;
        if (codec.size >= 4 && prAsciiEqualsFold(PrStringView{codec.data, 4}, entry))
            return &known;
    }
    return nullptr;
}

// Returns true if this system decodes the codec type, which VideoToolbox answers for the codecs that it decodes only on
// some hardware.
bool decodesVideo(CMVideoCodecType type) {
    if (type == 'vp09')
        VTRegisterSupplementalVideoDecoderIfAvailable(type);
    if (type == kCMVideoCodecType_H264 || type == kCMVideoCodecType_HEVC)
        return true;
    return VTIsHardwareDecodeSupported(type);
}

// Returns true if the codecs parameter's codec is an audio codec that the converter decodes: AAC or MP3 in MP4.
bool decodesAudio(PrStringView codec) {
    return prAsciiStartsWithFold(codec, "mp4a.40") || prAsciiEqualsFold(codec, "mp4a.69") ||
        prAsciiEqualsFold(codec, "mp4a.6b") || prAsciiEqualsFold(codec, "mp3");
}

void videoOutput(void* PR_NULLABLE refcon, void* PR_NULLABLE, OSStatus status, VTDecodeInfoFlags,
        CVImageBufferRef PR_NULLABLE image, CMTime presentation, CMTime) {
    AppleSampleVideo* d = static_cast<AppleSampleVideo*>(refcon);
    if (status != noErr || image == nullptr || d->sink == nullptr || d->sink->image == nullptr)
        return;
    CVPixelBufferRetain(image);
    d->sink->image(d->sink->context, image, secondsOf(presentation));
}

PrStatus videoDecoderCreate(const PrMediaTrackInfo* track, PrMediaSampleDecoder* PR_NULLABLE* PR_NONNULL out) {
    const VideoCodec* codec = videoCodecOf(track->codec);
    if (codec == nullptr || track->config.size == 0 || !decodesVideo(codec->type))
        return PrStatus_NotSupported;
    @autoreleasepool {
        NSDictionary* atoms = @{
            [NSString stringWithUTF8String:codec->record]: [NSData dataWithBytes:track->config.data
                length:track->config.size],
        };
        NSDictionary* extensions = @{(id)kCMFormatDescriptionExtension_SampleDescriptionExtensionAtoms: atoms};
        CMVideoFormatDescriptionRef format = nullptr;
        if (CMVideoFormatDescriptionCreate(kCFAllocatorDefault, codec->type, int32_t(track->extent.x),
                int32_t(track->extent.y), (__bridge CFDictionaryRef)extensions, &format) != noErr)
            return PrStatus_InvalidData;
        AppleSampleVideo* d = allocZeroed<AppleSampleVideo>();
        d->kind = PrMediaTrackKind_Video;
        d->format = format;
        NSDictionary* attributes = @{(id)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA)};
        VTDecompressionOutputCallbackRecord callback{videoOutput, d};
        if (VTDecompressionSessionCreate(kCFAllocatorDefault, format, nullptr, (__bridge CFDictionaryRef)attributes,
                &callback, &d->session) != noErr) {
            CFRelease(format);
            prMemoryFree(d);
            return PrStatus_NotSupported;
        }
        *out = d;
        return PrStatus_Ok;
    }
}

// Reads the sample rate and channels that an AudioSpecificConfig gives (ISO/IEC 14496-3, 1.6.2.1). Leaves them for a
// config that does not say.
void readAudioConfig(PrByteView config, uint32_t* sampleRate, uint32_t* channels) {
    constexpr uint32_t kRates[] = {
        96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000,
        7350,
    };
    if (config.size < 2)
        return;
    uint32_t bits = uint32_t(config.data[0]) << 8 | config.data[1];
    uint32_t type = bits >> 11;
    uint32_t at = 5;
    if (type == 31)
        at += 6;
    if (config.size < 4)
        return;
    uint32_t word = uint32_t(config.data[0]) << 24 | uint32_t(config.data[1]) << 16 | uint32_t(config.data[2]) << 8 |
        config.data[3];
    uint32_t index = (word >> (32 - at - 4)) & 15;
    at += 4;
    if (index < countOf(kRates)) {
        *sampleRate = kRates[index];
    } else if (index == 15) {
        if (config.size < 6)
            return;
        return;
    }
    uint32_t channelConfig = (word >> (32 - at - 4)) & 15;
    if (channelConfig >= 1 && channelConfig <= 6)
        *channels = channelConfig;
    else if (channelConfig == 7)
        *channels = 8;
}

// Appends an MPEG-4 descriptor's tag and length, in four bytes of seven bits each (ISO/IEC 14496-1, 8.3.3).
void appendDescriptor(RawArray<uint8_t>* out, uint8_t tag, uint32_t length) {
    out->push(tag);
    out->push(uint8_t(0x80 | ((length >> 21) & 0x7f)));
    out->push(uint8_t(0x80 | ((length >> 14) & 0x7f)));
    out->push(uint8_t(0x80 | ((length >> 7) & 0x7f)));
    out->push(uint8_t(length & 0x7f));
}

// Writes the ES descriptor of an AAC stream whose AudioSpecificConfig is `config`, which the converter takes as its
// magic cookie, as an esds box holds it.
void esdsOf(PrByteView config, RawArray<uint8_t>* out) {
    uint32_t specific = uint32_t(config.size);
    uint32_t decoderConfig = 13 + 5 + specific;
    uint32_t elementary = 3 + 5 + decoderConfig + 5 + 1;
    appendDescriptor(out, 3, elementary);
    out->push(0);
    out->push(0);
    out->push(0);
    appendDescriptor(out, 4, decoderConfig);
    const uint8_t fields[13] = {0x40, 0x15, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    out->append(fields, 13);
    appendDescriptor(out, 5, specific);
    if (specific != 0)
        out->append(config.data, specific);
    appendDescriptor(out, 6, 1);
    out->push(2);
}

PrStatus audioDecoderCreate(const PrMediaTrackInfo* track, uint32_t sampleRate, uint32_t channels,
        PrMediaSampleDecoder* PR_NULLABLE* PR_NONNULL out) {
    if (!decodesAudio(track->codec) || sampleRate == 0 || channels == 0 || channels > 2)
        return PrStatus_NotSupported;
    bool aac = prAsciiStartsWithFold(track->codec, "mp4a.40");
    uint32_t inputRate = track->sampleRate != 0 ? track->sampleRate : 44100;
    uint32_t inputChannels = track->channels != 0 ? track->channels : 2;
    if (aac)
        readAudioConfig(track->config, &inputRate, &inputChannels);
    AudioStreamBasicDescription input{};
    input.mSampleRate = inputRate;
    input.mFormatID = aac ? kAudioFormatMPEG4AAC : kAudioFormatMPEGLayer3;
    input.mChannelsPerFrame = inputChannels;
    AudioStreamBasicDescription output{};
    output.mSampleRate = sampleRate;
    output.mFormatID = kAudioFormatLinearPCM;
    output.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    output.mBitsPerChannel = 32;
    output.mChannelsPerFrame = channels;
    output.mFramesPerPacket = 1;
    output.mBytesPerFrame = channels * sizeof(float);
    output.mBytesPerPacket = output.mBytesPerFrame;
    AudioConverterRef converter = nullptr;
    if (AudioConverterNew(&input, &output, &converter) != noErr)
        return PrStatus_NotSupported;
    if (aac && track->config.size != 0) {
        RawArray<uint8_t> cookie{};
        esdsOf(track->config, &cookie);
        AudioConverterSetProperty(converter, kAudioConverterDecompressionMagicCookie, cookie.count, cookie.data);
        cookie.reset();
    }
    // A mono track plays in both channels of a stereo output.
    if (inputChannels == 1 && channels == 2) {
        SInt32 map[2] = {0, 0};
        AudioConverterSetProperty(converter, kAudioConverterChannelMap, sizeof(map), map);
    }
    AppleSampleAudio* d = allocZeroed<AppleSampleAudio>();
    d->kind = PrMediaTrackKind_Audio;
    d->converter = converter;
    d->inputChannels = inputChannels;
    d->channels = channels;
    *out = d;
    return PrStatus_Ok;
}

OSStatus audioInput(AudioConverterRef, UInt32* packets, AudioBufferList* data,
        AudioStreamPacketDescription* PR_NULLABLE* PR_NULLABLE descriptions, void* PR_NULLABLE context) {
    AppleSampleAudio* d = static_cast<AppleSampleAudio*>(context);
    if (d->input == nullptr) {
        *packets = 0;
        return kInputDone;
    }
    data->mNumberBuffers = 1;
    data->mBuffers[0].mData = const_cast<uint8_t*>(d->input->bytes.data);
    data->mBuffers[0].mDataByteSize = UInt32(d->input->bytes.size);
    data->mBuffers[0].mNumberChannels = d->inputChannels;
    d->packet = AudioStreamPacketDescription{0, 0, UInt32(d->input->bytes.size)};
    if (descriptions != nullptr)
        *descriptions = &d->packet;
    *packets = 1;
    d->input = nullptr;
    return noErr;
}

bool audioDecode(AppleSampleAudio* d, const PrMediaBufferedFrame* frame, const PrMediaSampleSink* sink) {
    constexpr uint32_t kChunkFrames = 4096;
    d->input = frame;
    d->output.clear();
    for (;;) {
        uint32_t at = d->output.count;
        d->output.resize(at + kChunkFrames * d->channels);
        AudioBufferList list{};
        list.mNumberBuffers = 1;
        list.mBuffers[0].mNumberChannels = d->channels;
        list.mBuffers[0].mDataByteSize = UInt32(kChunkFrames * d->channels * sizeof(float));
        list.mBuffers[0].mData = d->output.data + at;
        UInt32 frames = kChunkFrames;
        OSStatus status = AudioConverterFillComplexBuffer(d->converter, audioInput, d, &frames, &list, nullptr);
        d->output.resize(at + frames * d->channels);
        if (status != noErr || frames == 0)
            break;
    }
    d->input = nullptr;
    uint32_t frames = d->output.count / d->channels;
    if (frames != 0 && sink->samples != nullptr)
        sink->samples(sink->context, d->output.data, frames, frame->presentation);
    return true;
}

PrMediaSupport supportsSourceType(PrMediaCodec*, PrStringView type) {
    PrRawString parameter{};
    if (!prMimeTypeParameter(type, "codecs", &parameter))
        return PrMediaSupport_Maybe;
    PrStringView codecs = prRawStringView(&parameter);
    PrMediaSupport support = PrMediaSupport_Probably;
    size_t from = 0;
    while (from <= codecs.size && support != PrMediaSupport_No) {
        size_t end = from;
        while (end < codecs.size && codecs.data[end] != ',')
            ++end;
        size_t begin = from;
        while (begin < end && prAsciiIsHttpSpace(codecs.data[begin]))
            ++begin;
        size_t last = end;
        while (last > begin && prAsciiIsHttpSpace(codecs.data[last - 1]))
            --last;
        PrStringView codec{codecs.data + begin, last - begin};
        const VideoCodec* video = videoCodecOf(codec);
        if (!(video != nullptr && decodesVideo(video->type)) && !decodesAudio(codec))
            support = PrMediaSupport_No;
        from = end + 1;
    }
    prRawStringReset(&parameter);
    return support;
}

PrStatus sampleDecoderCreate(PrMediaCodec*, const PrMediaTrackInfo* track, uint32_t sampleRate, uint32_t channels,
        PrMediaSampleDecoder* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    if (track->kind == PrMediaTrackKind_Video)
        return videoDecoderCreate(track, out);
    if (track->kind == PrMediaTrackKind_Audio)
        return audioDecoderCreate(track, sampleRate, channels, out);
    return PrStatus_NotSupported;
}

void sampleDecoderDestroy(PrMediaCodec*, PrMediaSampleDecoder* decoder) {
    if (decoder->kind == PrMediaTrackKind_Video) {
        AppleSampleVideo* d = static_cast<AppleSampleVideo*>(decoder);
        VTDecompressionSessionInvalidate(d->session);
        CFRelease(d->session);
        CFRelease(d->format);
        prMemoryFree(d);
        return;
    }
    AppleSampleAudio* d = static_cast<AppleSampleAudio*>(decoder);
    AudioConverterDispose(d->converter);
    d->output.reset();
    prMemoryFree(d);
}

bool sampleDecode(PrMediaCodec*, PrMediaSampleDecoder* decoder, const PrMediaBufferedFrame* frame,
        const PrMediaSampleSink* sink) {
    if (decoder->kind == PrMediaTrackKind_Audio)
        return audioDecode(static_cast<AppleSampleAudio*>(decoder), frame, sink);
    AppleSampleVideo* d = static_cast<AppleSampleVideo*>(decoder);
    CMBlockBufferRef block = nullptr;
    if (CMBlockBufferCreateWithMemoryBlock(kCFAllocatorDefault, nullptr, frame->bytes.size, kCFAllocatorDefault,
            nullptr, 0, frame->bytes.size, kCMBlockBufferAssureMemoryNowFlag, &block) != kCMBlockBufferNoErr)
        return false;
    CMBlockBufferReplaceDataBytes(frame->bytes.data, block, 0, frame->bytes.size);
    CMSampleTimingInfo timing{
        CMTimeMakeWithSeconds(frame->duration, 90000),
        CMTimeMakeWithSeconds(frame->presentation, 90000),
        CMTimeMakeWithSeconds(frame->decode, 90000),
    };
    size_t size = frame->bytes.size;
    CMSampleBufferRef sample = nullptr;
    OSStatus status = CMSampleBufferCreateReady(kCFAllocatorDefault, block, d->format, 1, 1, &timing, 1, &size, &sample);
    CFRelease(block);
    if (status != noErr)
        return false;
    // Decoding without the asynchronous flag returns once the frame's images have reached the output callback.
    d->sink = sink;
    VTDecodeInfoFlags flags = 0;
    status = VTDecompressionSessionDecodeFrame(d->session, sample, 0, nullptr, &flags);
    VTDecompressionSessionWaitForAsynchronousFrames(d->session);
    d->sink = nullptr;
    CFRelease(sample);
    return status == noErr;
}

void sampleDecoderFlush(PrMediaCodec*, PrMediaSampleDecoder* decoder) {
    if (decoder->kind == PrMediaTrackKind_Audio) {
        AudioConverterReset(static_cast<AppleSampleAudio*>(decoder)->converter);
        return;
    }
    // The frames that the session held back for reordering are dropped, since no sink receives them.
    AppleSampleVideo* d = static_cast<AppleSampleVideo*>(decoder);
    d->sink = nullptr;
    VTDecompressionSessionFinishDelayedFrames(d->session);
    VTDecompressionSessionWaitForAsynchronousFrames(d->session);
}

// An image is a CVPixelBuffer, which a device imports as it is.
void imageImport(PrMediaCodec*, void* image, PrGpuImport* out) {
    *out = PrGpuImport{.kind = PrGpuImportKind_CoreVideo, .handle = image};
}

void imageRelease(PrMediaCodec*, void* image) {
    CVPixelBufferRelease(static_cast<CVPixelBufferRef>(image));
}

constexpr PrMediaCodec kApple = {
    .supportsType = supportsType,
    .decoderCreate = decoderCreate,
    .decoderDestroy = decoderDestroy,
    .decoderFrame = decoderFrame,
    .decoderAudioStart = decoderAudioStart,
    .decoderAudioSeek = decoderAudioSeek,
    .decoderAudioRead = decoderAudioRead,
    .supportsSourceType = supportsSourceType,
    .sampleDecoderCreate = sampleDecoderCreate,
    .sampleDecoderDestroy = sampleDecoderDestroy,
    .sampleDecode = sampleDecode,
    .sampleDecoderFlush = sampleDecoderFlush,
    .imageImport = imageImport,
    .imageRelease = imageRelease,
};

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrMediaCodec* prMediaAppleCreate(void) {
    // The codec holds no state, so every caller shares one table.
    return const_cast<PrMediaCodec*>(&kApple);
}
