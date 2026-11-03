// What the backends share to decode audio: a queue of samples from a decoding thread to an output's real-time thread.

#pragma once

#include "pr/array.h"
#include "pr/atomic.h"
#include "pr/float.h"
#include "pr/media.h"
#include "pr/mem.h"
#include "pr/memory.h"
#include "pr/thread.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// The seconds of samples that a decoder keeps decoded ahead of the output.
constexpr double kMediaAudioAheadSeconds = 2;

// How often a decoding thread looks again at a full queue or for a new seek, in milliseconds. The reader cannot signal,
// because it runs on a real-time thread.
constexpr double kMediaAudioPollMilliseconds = 10;

// A wait-free queue with one writer, the decoding thread, and one reader, the output's thread. Another thread may ask
// for a seek. Between the request and the writer's mediaAudioQueueBegin, reads return nothing, so that no sample from
// before the seek plays after it.
struct MediaAudioQueue {
    float* samples;             // by frame modulo `capacity`, and channel
    uint32_t capacity;          // in frames
    uint32_t channels;
    uint32_t sampleRate;
    Atomic<uint64_t> written;   // the frames written since the queue began
    Atomic<uint64_t> consumed;  // the frames read or skipped since the queue began

    // Where the timeline of the frames from `baseFrame` on begins. The writer changes them while a seek is pending.
    Atomic<uint64_t> baseFrame;
    Atomic<double> baseTime;

    Atomic<double> seekSeconds;
    Atomic<uint32_t> seekWanted;
    Atomic<uint32_t> seekDone;
    Atomic<bool> ended;
    Atomic<bool> stopping;
};

// Readies the queue for samples of `sampleRate` and `channels`, with a first seek to `seconds` pending.
inline void mediaAudioQueueInit(MediaAudioQueue* queue, uint32_t sampleRate, uint32_t channels, double seconds) {
    queue->capacity = uint32_t(sampleRate * kMediaAudioAheadSeconds);
    queue->channels = channels;
    queue->sampleRate = sampleRate;
    queue->samples = static_cast<float*>(prMemoryAlloc(size_t(queue->capacity) * channels * sizeof(float)));
    queue->seekSeconds.store(seconds);
    // The first seek is pending until the writer begins.
    queue->seekWanted.store(1);
}

// Frees the samples.
inline void mediaAudioQueueReset(MediaAudioQueue* queue) {
    prMemoryFree(queue->samples);
    queue->samples = nullptr;
}

// Asks the writer to continue from `seconds`.
inline void mediaAudioQueueSeek(MediaAudioQueue* queue, double seconds) {
    queue->seekSeconds.store(seconds);
    queue->seekWanted.store(queue->seekWanted.loadRelaxed() + 1);
}

// Returns true if the writer should stop what it decodes and begin again, or stop.
inline bool mediaAudioQueueInterrupted(const MediaAudioQueue* queue, uint32_t seek) {
    return queue->stopping.load() || queue->seekWanted.load() != seek;
}

// Called by the writer: frames written from now on begin at `time` on the timeline, and complete the seek `seek`.
inline void mediaAudioQueueBegin(MediaAudioQueue* queue, uint32_t seek, double time) {
    queue->baseTime.store(time);
    queue->baseFrame.store(queue->written.loadRelaxed());
    queue->ended.store(false);
    queue->seekDone.store(seek);
}

// Called by the writer: appends `frames` frames, waiting while the queue is full. Returns false if the writer was
// interrupted before all were written.
inline bool mediaAudioQueueWrite(MediaAudioQueue* queue, uint32_t seek, const float* samples, uint32_t frames) {
    uint32_t channels = queue->channels;
    while (frames != 0) {
        uint64_t written = queue->written.loadRelaxed();
        uint64_t held = written - queue->consumed.load();
        if (mediaAudioQueueInterrupted(queue, seek))
            return false;
        if (held >= queue->capacity) {
            prThreadSleep(kMediaAudioPollMilliseconds);
            continue;
        }
        uint32_t at = uint32_t(written % queue->capacity);
        uint32_t count = queue->capacity - at;
        count = count < queue->capacity - uint32_t(held) ? count : queue->capacity - uint32_t(held);
        count = count < frames ? count : frames;
        memCopy(queue->samples + size_t(at) * channels, samples, size_t(count) * channels * sizeof(float));
        queue->written.store(written + count);
        samples += size_t(count) * channels;
        frames -= count;
    }
    return true;
}

// Called by the writer once it has written the last frame: waits for the next seek, or for the decoder to stop.
inline void mediaAudioQueueFinish(MediaAudioQueue* queue, uint32_t seek) {
    queue->ended.store(true);
    while (!mediaAudioQueueInterrupted(queue, seek))
        prThreadSleep(kMediaAudioPollMilliseconds);
}

// Called by the reader. Never waits.
inline PrMediaAudioChunk mediaAudioQueueRead(MediaAudioQueue* queue, float* samples, uint32_t frames) {
    PrMediaAudioChunk chunk{};
    if (queue->seekWanted.load() != queue->seekDone.load())
        return chunk;
    uint32_t channels = queue->channels;
    uint64_t base = queue->baseFrame.load();
    uint64_t read = queue->consumed.loadRelaxed();
    // Frames from before the last seek are skipped.
    read = read < base ? base : read;
    bool ended = queue->ended.load();
    uint64_t held = queue->written.load() - read;
    uint32_t count = held < frames ? uint32_t(held) : frames;
    chunk.frames = count;
    chunk.time = queue->baseTime.load() + double(read - base) / queue->sampleRate;
    chunk.ended = ended && count == 0;
    while (count != 0) {
        uint32_t at = uint32_t(read % queue->capacity);
        uint32_t run = queue->capacity - at < count ? queue->capacity - at : count;
        memCopy(samples, queue->samples + size_t(at) * channels, size_t(run) * channels * sizeof(float));
        samples += size_t(run) * channels;
        read += run;
        count -= run;
    }
    queue->consumed.store(read);
    return chunk;
}

} // namespace pr_impl

PR_ASSUME_NONNULL_END
