// The decoder of a media source: it reads the coded frames of track buffers as they arrive, and decodes them with a
// codec's sample decoders.
//
// A video thread decodes frames in decode order from the random access point before the time last asked for, and keeps
// a few decoded frames past that time, in presentation order. An audio thread decodes into the queue that an output
// reads. Neither thread waits for data that is not buffered yet: each looks again a little later, so that frames that
// script appends meanwhile are decoded.

#include "media/src/media_source_decoder.h"

#include "pr/alloc.h"
#include "pr/atomic.h"
#include "pr/float.h"
#include "pr/media_audio.h"
#include "pr/memory.h"
#include "pr/mutex.h"
#include "pr/raw_array.h"
#include "pr/thread.h"

namespace pr_impl {

namespace {

// The frames that the video thread keeps decoded past the time last asked for.
constexpr uint32_t kFramesAhead = 6;

// How far past the last decoded frame a request may be before the video thread jumps to it rather than decoding toward
// it, in seconds.
constexpr double kJumpAhead = 1;

// How long a thread that has nothing to decode waits before it looks for new frames again, in milliseconds.
constexpr double kPollMilliseconds = 10;

// A decoded frame of video, which holds the codec's image.
struct SourceImage {
    void* image;
    double presentation;
    uint64_t serial;
};

struct SourceDecoder : PrMediaDecoder {
    PrMediaCodec* backend;

    // The video track and its decoding thread, which `mutex` and `wake` coordinate with the decoder's user.
    PrMediaSourceBuffer* PR_NULLABLE videoBuffer;
    uint32_t videoTrack;
    PrMediaSampleDecoder* PR_NULLABLE video;
    PrThread videoThread;
    PrMutex mutex;
    PrCondition wake;

    // Guarded by `mutex`.
    RawArray<SourceImage> images;   // in presentation order
    double wanted;                  // the time last asked for
    double jumpTo;                  // a time to restart decoding at, or NaN
    double lastDecode;              // the decode timestamp of the last frame decoded, or NaN before the first
    double through;                 // the latest presentation decoded since the last jump, or -infinity
    bool stopping;
    uint64_t serials;

    // The image last returned, which stays locked while it shows, and its pixels. The list does not release the image
    // that shows: it sets `shownDropped` instead, and the user releases the image once another replaces it. Guarded by
    // `mutex`, except `shownPixels`, which is the user's alone.
    void* PR_NULLABLE shown;
    bool shownDropped;
    PrPixelView shownPixels;

    // The audio track, and its thread once the audio starts.
    PrMediaSourceBuffer* PR_NULLABLE audioBuffer;
    uint32_t audioTrack;
    PrMediaSampleDecoder* PR_NULLABLE audio;
    PrMediaAudioQueue* PR_NULLABLE queue;
    PrThread audioThread;
    bool audioStarted;
    Atomic<bool> sourceEnded;
};

// Releases an image that the list or a sink held, unless it shows, which the user releases later. The caller holds the
// mutex.
void releaseImage(SourceDecoder* d, void* image) {
    if (image == d->shown) {
        d->shownDropped = true;
        return;
    }
    d->backend->imageRelease(d->backend, image);
}

void releaseImages(SourceDecoder* d) {
    for (uint32_t i = 0; i < d->images.count; ++i)
        releaseImage(d, d->images.data[i].image);
    d->images.clear();
}

// Collects the images that a frame decodes to, for the video thread to keep.
struct ImageSink {
    RawArray<SourceImage> images;
};

void receiveImage(void* PR_NULLABLE context, void* image, double presentation) {
    ImageSink* sink = static_cast<ImageSink*>(context);
    sink->images.push(SourceImage{image, presentation, 0});
}

// Inserts the image in presentation order, replacing one of the same time.
void keepImage(SourceDecoder* d, SourceImage image) {
    uint32_t at = d->images.count;
    while (at != 0 && d->images.data[at - 1].presentation > image.presentation)
        --at;
    if (at != 0 && d->images.data[at - 1].presentation == image.presentation) {
        releaseImage(d, d->images.data[at - 1].image);
        d->images.data[at - 1] = image;
        return;
    }
    d->images.insert(at, image);
}

void decodeVideo(SourceDecoder* d) {
    ImageSink sink{};
    PrMediaSampleSink handlers{.image = receiveImage, .context = &sink};
    prMutexLock(&d->mutex);
    while (!d->stopping) {
        double start = d->wanted;
        if (!prF64IsNan(d->jumpTo)) {
            start = d->jumpTo;
            d->jumpTo = kPrF64Nan;
            releaseImages(d);
            d->lastDecode = kPrF64Nan;
            d->through = -kPrF64Infinity;
            prMutexUnlock(&d->mutex);
            d->backend->sampleDecoderFlush(d->backend, d->video);
            prMutexLock(&d->mutex);
        }
        uint32_t ahead = 0;
        for (uint32_t i = 0; i < d->images.count; ++i)
            ahead += d->images.data[i].presentation > d->wanted ? 1 : 0;
        if (ahead >= kFramesAhead) {
            prConditionWait(&d->wake, &d->mutex);
            continue;
        }
        double lastDecode = d->lastDecode;
        prMutexUnlock(&d->mutex);
        PrMediaBufferedFrame frame{};
        bool found = prMediaSourceBufferNextFrame(d->videoBuffer, d->videoTrack, lastDecode, start, &frame);
        bool decoded = false;
        if (found) {
            // A frame after a gap in decode order starts again at a random access point.
            if (!prF64IsNan(lastDecode) && !frame.key && frame.decode > lastDecode + kJumpAhead)
                d->backend->sampleDecoderFlush(d->backend, d->video);
            decoded = d->backend->sampleDecode(d->backend, d->video, &frame, &handlers);
        } else {
            prThreadSleep(kPollMilliseconds);
        }
        prMutexLock(&d->mutex);
        bool jumped = !prF64IsNan(d->jumpTo);
        for (uint32_t i = 0; i < sink.images.count; ++i) {
            if (jumped) {
                releaseImage(d, sink.images.data[i].image);
                continue;
            }
            SourceImage image = sink.images.data[i];
            image.serial = ++d->serials;
            d->through = image.presentation > d->through ? image.presentation : d->through;
            keepImage(d, image);
        }
        sink.images.clear();
        // A frame that does not decode is passed over, as a decoder conceals a damaged frame.
        if (found && !jumped)
            d->lastDecode = frame.decode;
        (void)decoded;
        prMediaBufferedFrameReset(&frame);
        // Only the last image at or before the time asked for can show until the time moves.
        while (d->images.count >= 2 && d->images.data[1].presentation <= d->wanted) {
            releaseImage(d, d->images.data[0].image);
            d->images.erase(0, 1);
        }
    }
    prMutexUnlock(&d->mutex);
    sink.images.reset();
}

// Collects the samples that a frame decodes to, for the audio thread to write to its queue.
struct SampleSink {
    RawArray<float> samples;
    uint32_t channels;
    double time;                    // where the first sample lies, once `timed`
    bool timed;
};

void receiveSamples(void* PR_NULLABLE context, const float* samples, uint32_t frames, double time) {
    SampleSink* sink = static_cast<SampleSink*>(context);
    if (!sink->timed) {
        sink->time = time;
        sink->timed = true;
    }
    if (frames != 0)
        sink->samples.append(samples, frames * sink->channels);
}

void decodeAudio(SourceDecoder* d) {
    PrMediaAudioQueue* queue = d->queue;
    uint32_t channels = prMediaAudioQueueChannels(queue);
    SampleSink sink{};
    sink.channels = channels;
    PrMediaSampleSink handlers{.samples = receiveSamples, .context = &sink};
    while (!prMediaAudioQueueIsStopping(queue)) {
        PrMediaAudioSeek request = prMediaAudioQueueRequest(queue);
        uint32_t seek = request.id;
        double seconds = request.seconds;
        d->backend->sampleDecoderFlush(d->backend, d->audio);
        double lastDecode = kPrF64Nan;
        bool begun = false;
        bool finished = false;
        while (!prMediaAudioQueueIsInterrupted(queue, seek)) {
            PrMediaBufferedFrame frame{};
            if (!prMediaSourceBufferNextFrame(d->audioBuffer, d->audioTrack, lastDecode, seconds, &frame)) {
                // The audio ends where the buffered audio does once the source has ended. Until then, more may come.
                if (d->sourceEnded.load()) {
                    finished = true;
                    break;
                }
                prThreadSleep(kPollMilliseconds);
                continue;
            }
            lastDecode = frame.decode;
            sink.samples.clear();
            sink.timed = false;
            d->backend->sampleDecode(d->backend, d->audio, &frame, &handlers);
            prMediaBufferedFrameReset(&frame);
            uint32_t frames = sink.samples.count / channels;
            if (frames == 0 || !sink.timed)
                continue;
            // A frame that starts before the time asked for is trimmed to it.
            uint32_t skip = 0;
            if (sink.time < seconds) {
                double extra = (seconds - sink.time) * prMediaAudioQueueSampleRate(queue);
                skip = extra >= frames ? frames : uint32_t(extra);
            }
            if (skip == frames)
                continue;
            if (!begun) {
                prMediaAudioQueueBegin(queue, seek, sink.time + double(skip) / prMediaAudioQueueSampleRate(queue));
                begun = true;
            }
            if (!prMediaAudioQueueWrite(queue, seek, sink.samples.data + size_t(skip) * channels, frames - skip))
                break;
        }
        if (!begun)
            prMediaAudioQueueBegin(queue, seek, seconds);
        if (finished)
            prMediaAudioQueueFinish(queue, seek);
    }
    sink.samples.reset();
}

void decoderDestroy(PrMediaCodec*, PrMediaDecoder* decoder) {
    SourceDecoder* d = static_cast<SourceDecoder*>(decoder);
    if (d->audioStarted) {
        prMediaAudioQueueStop(d->queue);
        prThreadJoin(&d->audioThread);
        prMediaAudioQueueDestroy(d->queue);
    }
    if (d->video != nullptr) {
        prMutexLock(&d->mutex);
        d->stopping = true;
        prConditionSignal(&d->wake);
        prMutexUnlock(&d->mutex);
        prThreadJoin(&d->videoThread);
    }
    releaseImages(d);
    d->images.reset();
    if (d->shown != nullptr) {
        d->backend->imageUnlock(d->backend, d->shown);
        d->backend->imageRelease(d->backend, d->shown);
    }
    if (d->video != nullptr)
        d->backend->sampleDecoderDestroy(d->backend, d->video);
    if (d->audio != nullptr)
        d->backend->sampleDecoderDestroy(d->backend, d->audio);
    prConditionReset(&d->wake);
    prMutexReset(&d->mutex);
    prMemoryFree(d);
}

bool decoderFrame(PrMediaCodec*, PrMediaDecoder* decoder, double seconds, PrMediaFrame* out) {
    SourceDecoder* d = static_cast<SourceDecoder*>(decoder);
    prMutexLock(&d->mutex);
    d->wanted = seconds;
    // A time well before the earliest image kept, or far past the last decoded, restarts decoding there.
    bool before = d->images.count != 0 && seconds < d->images.data[0].presentation - kJumpAhead;
    bool past = seconds > d->through + kJumpAhead && !prF64IsNan(d->lastDecode);
    if (prF64IsNan(d->jumpTo) && (before || past))
        d->jumpTo = seconds;
    // The image that shows is the last that starts at or before the time. Before the first image, the first shows.
    uint32_t at = UINT32_MAX;
    if (prF64IsNan(d->jumpTo)) {
        for (uint32_t i = 0; i < d->images.count && d->images.data[i].presentation <= seconds + 1e-6; ++i)
            at = i;
        if (at == UINT32_MAX && d->images.count != 0)
            at = 0;
    }
    SourceImage image{};
    bool current = false;
    if (at != UINT32_MAX) {
        // The image shows at the time if a later image is decoded, so that none between them is missing, or if it shows
        // before its own time.
        current = at + 1 < d->images.count || d->images.data[at].presentation > seconds;
        for (uint32_t i = 0; i < at; ++i)
            releaseImage(d, d->images.data[i].image);
        d->images.erase(0, at);
        image = d->images.data[0];
    }
    void* previous = d->shown;
    bool previousDropped = d->shownDropped;
    if (image.image != nullptr && image.image != previous) {
        d->shown = image.image;
        d->shownDropped = false;
    }
    prConditionSignal(&d->wake);
    prMutexUnlock(&d->mutex);
    if (image.image == nullptr) {
        out->current = false;
        return false;
    }
    if (image.image != previous) {
        if (previous != nullptr) {
            d->backend->imageUnlock(d->backend, previous);
            if (previousDropped)
                d->backend->imageRelease(d->backend, previous);
        }
        d->shownPixels = PrPixelView{};
        if (!d->backend->imageLock(d->backend, image.image, &d->shownPixels))
            d->shownPixels = PrPixelView{};
    }
    *out = PrMediaFrame{
        .pixels = d->shownPixels,
        .time = image.presentation,
        .serial = image.serial,
        .current = current,
    };
    return d->shownPixels.data != nullptr;
}

bool decoderAudioStart(PrMediaCodec*, PrMediaDecoder* decoder, uint32_t sampleRate, uint32_t channels, double seconds) {
    SourceDecoder* d = static_cast<SourceDecoder*>(decoder);
    if (d->audioBuffer == nullptr || d->audioStarted)
        return false;
    PrMediaTrackInfo track = prMediaSourceBufferTrack(d->audioBuffer, d->audioTrack);
    PrMediaSampleDecoder* audio = nullptr;
    if (d->backend->sampleDecoderCreate(d->backend, &track, sampleRate, channels, &audio) != PrStatus_Ok)
        return false;
    d->audio = audio;
    d->queue = prMediaAudioQueueCreate(sampleRate, channels, seconds);
    d->audioStarted = true;
    prThreadStart(&d->audioThread, [](void* context) { decodeAudio(static_cast<SourceDecoder*>(context)); }, d);
    return true;
}

void decoderAudioSeek(PrMediaCodec*, PrMediaDecoder* decoder, double seconds) {
    prMediaAudioQueueSeek(static_cast<SourceDecoder*>(decoder)->queue, seconds);
}

PrMediaAudioChunk decoderAudioRead(PrMediaCodec*, PrMediaDecoder* decoder, float* samples, uint32_t frames) {
    return prMediaAudioQueueRead(static_cast<SourceDecoder*>(decoder)->queue, samples, frames);
}

// The table that dispatches the public functions of a source's decoders. Its codec functions are never called, since a
// source's decoders are not created through it.
constexpr PrMediaCodec kSourceCodec = {
    .supportsType = [](PrMediaCodec*, PrStringView) -> PrMediaSupport { return PrMediaSupport_No; },
    .decoderCreate = [](PrMediaCodec*, PrByteView, PrMediaDecoder* PR_NULLABLE* PR_NONNULL, PrMediaInfo*) -> PrStatus {
        return PrStatus_NotSupported;
    },
    .decoderDestroy = decoderDestroy,
    .decoderFrame = decoderFrame,
    .decoderAudioStart = decoderAudioStart,
    .decoderAudioSeek = decoderAudioSeek,
    .decoderAudioRead = decoderAudioRead,
};

// The table of a source's decoder that decodes no frames, because its codec decodes no samples.
constexpr PrMediaCodec kSourceCodecWithoutSamples = {
    .supportsType = [](PrMediaCodec*, PrStringView) -> PrMediaSupport { return PrMediaSupport_No; },
    .decoderCreate = [](PrMediaCodec*, PrByteView, PrMediaDecoder* PR_NULLABLE* PR_NONNULL, PrMediaInfo*) -> PrStatus {
        return PrStatus_NotSupported;
    },
    .decoderDestroy = decoderDestroy,
};

} // namespace

PrStatus mediaSourceDecoderCreate(PrMediaCodec* codec, PrMediaSourceBuffer* PR_NONNULL const* PR_NONNULL buffers,
        uint32_t count, PrMediaDecoder* PR_NULLABLE* PR_NONNULL out, PrMediaInfo* info) {
    *out = nullptr;
    SourceDecoder* d = allocZeroed<SourceDecoder>();
    d->backend = codec;
    d->jumpTo = kPrF64Nan;
    d->lastDecode = kPrF64Nan;
    d->through = -kPrF64Infinity;
    info->duration = kPrF64Nan;
    prMutexInit(&d->mutex);
    prConditionInit(&d->wake);
    for (uint32_t b = 0; b < count; ++b) {
        for (uint32_t t = 0; t < prMediaSourceBufferTrackCount(buffers[b]); ++t) {
            PrMediaTrackInfo track = prMediaSourceBufferTrack(buffers[b], t);
            if (track.kind == PrMediaTrackKind_Video && d->videoBuffer == nullptr) {
                d->videoBuffer = buffers[b];
                d->videoTrack = t;
                info->hasVideo = true;
                info->videoExtent = track.extent;
            } else if (track.kind == PrMediaTrackKind_Audio && d->audioBuffer == nullptr) {
                d->audioBuffer = buffers[b];
                d->audioTrack = t;
                info->hasAudio = true;
            }
        }
    }
    bool samples = codec->sampleDecoderCreate != nullptr;
    d->codec = const_cast<PrMediaCodec*>(samples ? &kSourceCodec : &kSourceCodecWithoutSamples);
    if (samples && d->videoBuffer != nullptr) {
        PrMediaTrackInfo track = prMediaSourceBufferTrack(d->videoBuffer, d->videoTrack);
        PrMediaSampleDecoder* video = nullptr;
        if (codec->sampleDecoderCreate(codec, &track, 0, 0, &video) == PrStatus_Ok) {
            d->video = video;
            prThreadStart(&d->videoThread, [](void* context) { decodeVideo(static_cast<SourceDecoder*>(context)); }, d);
        }
    }
    *out = d;
    return PrStatus_Ok;
}

void mediaSourceDecoderSetEnded(PrMediaDecoder* decoder, bool ended) {
    if (decoder->codec != &kSourceCodec && decoder->codec != &kSourceCodecWithoutSamples)
        return;
    static_cast<SourceDecoder*>(decoder)->sourceEnded.store(ended);
}

} // namespace pr_impl
