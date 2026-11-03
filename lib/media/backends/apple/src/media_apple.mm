// The AVFoundation media codec.

#include "pr/media_apple.h"

#include "pr/alloc.h"
#include "pr/array.h"
#include "pr/ascii.h"
#include "pr/assert.h"
#include "pr/float.h"
#include "pr/memory.h"
#include "pr/mutex.h"
#include "pr/thread.h"

#include "media/backends/shared/src/media_shared_audio.h"

#import <AVFoundation/AVFoundation.h>
#import <CoreVideo/CoreVideo.h>

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
    Array<AppleFrame> frames;   // decoded, in time order, from the last at or before `wanted` on
    double wanted;              // the time last asked for
    double seekTo;              // a time to restart decoding at, or NaN
    double start;               // the time that decoding last started at
    double through;             // the time of the last frame decoded since then, or -infinity
    bool dropped;               // frames decoded since `start` were dropped from the front
    bool ended;                 // every frame since `start` is decoded
    bool stopping;
    uint64_t serials;

    // The user's alone: the buffer of the frame last returned, retained and with its base address locked.
    CVPixelBufferRef shown;

    // The audio, from when it starts.
    MediaAudioQueue queue;
    PrThread audioThread;
    bool audioStarted;
};

PrMediaAppleSource* sourceOf(const AppleDecoder* decoder) {
    return (__bridge PrMediaAppleSource*)decoder->source;
}

double secondsOf(CMTime time) {
    return CMTIME_IS_NUMERIC(time) ? CMTimeGetSeconds(time) : kNanF64;
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
void releaseFrames(Array<AppleFrame>* frames) {
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
    NSDictionary* settings = @{(id)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA)};
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
            if (!isNanF64(d->seekTo)) {
                double at = d->seekTo;
                d->seekTo = kNanF64;
                releaseFrames(&d->frames);
                d->start = at;
                d->through = -kInfinityF64;
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
            double time = sample != nullptr ? secondsOf(CMSampleBufferGetPresentationTimeStamp(sample)) : kNanF64;
            if (buffer != nullptr)
                CVPixelBufferRetain(buffer);
            if (sample != nullptr)
                CFRelease(sample);
            prMutexLock(&d->mutex);
            if (!isNanF64(d->seekTo)) {
                CVPixelBufferRelease(buffer);
                continue;
            }
            if (sample == nullptr) {
                d->ended = true;
                continue;
            }
            if (buffer == nullptr || isNanF64(time))
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
AVAssetReaderTrackOutput* PR_NULLABLE startReadingAudio(PrMediaAppleSource* source, const MediaAudioQueue* queue,
        double seconds, AVAssetReader* PR_NULLABLE __strong* out) {
    NSError* error = nil;
    AVAssetReader* reader = [[AVAssetReader alloc] initWithAsset:source.asset error:&error];
    if (reader == nil)
        return nil;
    reader.timeRange = CMTimeRangeMake(CMTimeMakeWithSeconds(seconds, 90000), kCMTimePositiveInfinity);
    AudioChannelLayout layout{};
    layout.mChannelLayoutTag = queue->channels == 1 ? kAudioChannelLayoutTag_Mono : kAudioChannelLayoutTag_Stereo;
    NSDictionary* settings = @{
        AVFormatIDKey: @(kAudioFormatLinearPCM),
        AVSampleRateKey: @(queue->sampleRate),
        AVNumberOfChannelsKey: @(queue->channels),
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
    MediaAudioQueue* queue = &d->queue;
    Array<float> samples{};
    PrMediaAppleSource* source = sourceOf(d);
    while (!queue->stopping.load()) {
        uint32_t seek = queue->seekWanted.load();
        double seconds = queue->seekSeconds.load();
        @autoreleasepool {
            AVAssetReader* reader = nil;
            AVAssetReaderTrackOutput* output = startReadingAudio(source, queue, seconds, &reader);
            bool begun = false;
            while (output != nil && !mediaAudioQueueInterrupted(queue, seek)) {
                CMSampleBufferRef sample = nullptr;
                @autoreleasepool {
                    sample = [output copyNextSampleBuffer];
                }
                if (sample == nullptr)
                    break;
                uint32_t frames = uint32_t(CMSampleBufferGetNumSamples(sample));
                double time = secondsOf(CMSampleBufferGetPresentationTimeStamp(sample));
                samples.resize(frames * queue->channels);
                CMBlockBufferRef block = CMSampleBufferGetDataBuffer(sample);
                bool copied = block != nullptr && CMBlockBufferCopyDataBytes(block, 0,
                    size_t(samples.count) * sizeof(float), samples.data) == kCMBlockBufferNoErr;
                CFRelease(sample);
                if (!copied || isNanF64(time))
                    continue;
                // A reader starts at a packet's boundary, so the frames before the time asked for are dropped.
                uint32_t skip = 0;
                if (time < seconds) {
                    double extra = (seconds - time) * queue->sampleRate;
                    skip = extra >= frames ? frames : uint32_t(extra);
                }
                if (skip == frames)
                    continue;
                if (!begun) {
                    mediaAudioQueueBegin(queue, seek, time + double(skip) / queue->sampleRate);
                    begun = true;
                }
                if (!mediaAudioQueueWrite(queue, seek, samples.data + size_t(skip) * queue->channels, frames - skip))
                    break;
            }
            if (!begun)
                mediaAudioQueueBegin(queue, seek, seconds);
            [reader cancelReading];
        }
        mediaAudioQueueFinish(queue, seek);
    }
    samples.reset();
}

PrMediaSupport supportsType(PrMediaCodec*, PrStringView type) {
    NSString* text = [[NSString alloc] initWithBytes:type.data length:type.size encoding:NSUTF8StringEncoding];
    if (text == nil || ![AVURLAsset isPlayableExtendedMIMEType:text])
        return PrMediaSupport_No;
    // A type that names its codecs, which the system plays, is probably playable.
    return asciiContainsFold(type, "codecs=") ? PrMediaSupport_Probably : PrMediaSupport_Maybe;
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
        d->seekTo = kNanF64;
        d->through = -kInfinityF64;
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
        d->queue.stopping.store(true);
        prThreadJoin(&d->audioThread);
        mediaAudioQueueReset(&d->queue);
    }
    if (sourceOf(d).video != nil) {
        prMutexLock(&d->mutex);
        d->stopping = true;
        prConditionSignal(&d->wake);
        prMutexUnlock(&d->mutex);
        prThreadJoin(&d->thread);
    }
    if (d->shown != nullptr) {
        CVPixelBufferUnlockBaseAddress(d->shown, kCVPixelBufferLock_ReadOnly);
        CVPixelBufferRelease(d->shown);
    }
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
    if (isNanF64(d->seekTo) && (before || past))
        d->seekTo = seconds;
    // The frame that shows is the last that starts at or before the time. Before the first frame, the first shows.
    uint32_t at = UINT32_MAX;
    if (isNanF64(d->seekTo)) {
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
    bool done = frame.buffer == nullptr && isNanF64(d->seekTo) && d->ended && d->frames.count == 0;
    prConditionSignal(&d->wake);
    prMutexUnlock(&d->mutex);
    if (frame.buffer == nullptr) {
        out->current = done;
        return false;
    }
    if (d->shown != nullptr) {
        CVPixelBufferUnlockBaseAddress(d->shown, kCVPixelBufferLock_ReadOnly);
        CVPixelBufferRelease(d->shown);
    }
    d->shown = frame.buffer;
    CVPixelBufferLockBaseAddress(frame.buffer, kCVPixelBufferLock_ReadOnly);
    PrUVec2 extent{uint32_t(CVPixelBufferGetWidth(frame.buffer)), uint32_t(CVPixelBufferGetHeight(frame.buffer))};
    const uint8_t* pixels = static_cast<const uint8_t*>(CVPixelBufferGetBaseAddress(frame.buffer));
    *out = PrMediaFrame{
        .pixels = PrPixelView{pixels, CVPixelBufferGetBytesPerRow(frame.buffer), extent},
        .time = frame.time,
        .serial = frame.serial,
        .current = current,
    };
    return pixels != nullptr;
}

bool decoderAudioStart(PrMediaCodec*, PrMediaDecoder* decoder, uint32_t sampleRate, uint32_t channels, double seconds) {
    AppleDecoder* d = static_cast<AppleDecoder*>(decoder);
    PR_ASSERT(!d->audioStarted);
    if (sourceOf(d).audio == nil || channels > 2)
        return false;
    mediaAudioQueueInit(&d->queue, sampleRate, channels, seconds);
    d->audioStarted = true;
    prThreadStart(&d->audioThread, [](void* context) { decodeAudio(static_cast<AppleDecoder*>(context)); }, d);
    return true;
}

void decoderAudioSeek(PrMediaCodec*, PrMediaDecoder* decoder, double seconds) {
    mediaAudioQueueSeek(&static_cast<AppleDecoder*>(decoder)->queue, seconds);
}

PrMediaAudioChunk decoderAudioRead(PrMediaCodec*, PrMediaDecoder* decoder, float* samples, uint32_t frames) {
    return mediaAudioQueueRead(&static_cast<AppleDecoder*>(decoder)->queue, samples, frames);
}

constexpr PrMediaCodec kApple = {
    .supportsType = supportsType,
    .decoderCreate = decoderCreate,
    .decoderDestroy = decoderDestroy,
    .decoderFrame = decoderFrame,
    .decoderAudioStart = decoderAudioStart,
    .decoderAudioSeek = decoderAudioSeek,
    .decoderAudioRead = decoderAudioRead,
};

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrMediaCodec* prMediaAppleCreate(void) {
    // The codec holds no state, so every caller shares one table.
    return const_cast<PrMediaCodec*>(&kApple);
}
