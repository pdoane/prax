// Decoded audio: a queue of interleaved samples from a decoding thread to a reader on an output's real-time thread.

#include "pr/media_audio.h"

#include "pr/alloc.h"
#include "pr/atomic.h"
#include "pr/mem.h"
#include "pr/memory.h"
#include "pr/thread.h"

namespace pr_impl {

namespace {

// The seconds of samples that the queue holds.
constexpr double kAheadSeconds = 2;

// How often a writer looks again at a full queue or for a new request, in milliseconds. The reader cannot signal it,
// because the reader runs on a real-time thread.
constexpr double kPollMilliseconds = 10;

// How often a reader that waits looks again for the writer's frames, in milliseconds. The writer does not signal it, so
// that a writer has nothing to do for a reader that never waits.
constexpr double kReaderPollMilliseconds = 1;

struct MediaAudioQueue {
    float* samples;             // interleaved, by frame modulo `capacity`
    uint32_t capacity;          // in frames
    uint32_t channels;
    uint32_t sampleRate;
    Atomic<uint64_t> written;   // the frames written since the queue was created
    Atomic<uint64_t> consumed;  // the frames read or skipped since the queue was created

    // Where the timeline of the frames from `baseFrame` on begins. The writer changes them while a request is pending.
    Atomic<uint64_t> baseFrame;
    Atomic<double> baseTime;

    Atomic<double> seekSeconds;
    Atomic<uint32_t> seekWanted;
    Atomic<uint32_t> seekDone;
    Atomic<bool> ended;
    Atomic<bool> stopping;
};

} // namespace

} // namespace pr_impl

using namespace pr_impl;

// C opaque types
struct PrMediaAudioQueue : MediaAudioQueue {};

PrMediaAudioQueue* prMediaAudioQueueCreate(uint32_t sampleRate, uint32_t channels, double seconds) {
    PrMediaAudioQueue* queue = allocZeroed<PrMediaAudioQueue>();
    queue->capacity = uint32_t(sampleRate * kAheadSeconds);
    queue->channels = channels;
    queue->sampleRate = sampleRate;
    queue->samples = static_cast<float*>(prMemoryAlloc(size_t(queue->capacity) * channels * sizeof(float)));
    queue->seekSeconds.store(seconds);
    // The first request is pending until the writer begins.
    queue->seekWanted.store(1);
    return queue;
}

void prMediaAudioQueueDestroy(PrMediaAudioQueue* PR_NULLABLE queue) {
    if (queue == nullptr)
        return;
    prMemoryFree(queue->samples);
    prMemoryFree(queue);
}

uint32_t prMediaAudioQueueSampleRate(const PrMediaAudioQueue* queue) {
    return queue->sampleRate;
}

uint32_t prMediaAudioQueueChannels(const PrMediaAudioQueue* queue) {
    return queue->channels;
}

void prMediaAudioQueueSeek(PrMediaAudioQueue* queue, double seconds) {
    queue->seekSeconds.store(seconds);
    queue->seekWanted.store(queue->seekWanted.loadRelaxed() + 1);
}

void prMediaAudioQueueStop(PrMediaAudioQueue* queue) {
    queue->stopping.store(true);
}

bool prMediaAudioQueueIsStopping(const PrMediaAudioQueue* queue) {
    return queue->stopping.load();
}

PrMediaAudioSeek prMediaAudioQueueRequest(const PrMediaAudioQueue* queue) {
    // The id is read first, so that a request made between the two reads is served again with its own seconds.
    uint32_t id = queue->seekWanted.load();
    return PrMediaAudioSeek{id, queue->seekSeconds.load()};
}

bool prMediaAudioQueueIsInterrupted(const PrMediaAudioQueue* queue, uint32_t id) {
    return queue->stopping.load() || queue->seekWanted.load() != id;
}

void prMediaAudioQueueBegin(PrMediaAudioQueue* queue, uint32_t id, double time) {
    queue->baseTime.store(time);
    queue->baseFrame.store(queue->written.loadRelaxed());
    queue->ended.store(false);
    queue->seekDone.store(id);
}

bool prMediaAudioQueueWrite(PrMediaAudioQueue* queue, uint32_t id, const float* samples, uint32_t frames) {
    uint32_t channels = queue->channels;
    while (frames != 0) {
        uint64_t written = queue->written.loadRelaxed();
        uint64_t held = written - queue->consumed.load();
        if (prMediaAudioQueueIsInterrupted(queue, id))
            return false;
        if (held >= queue->capacity) {
            prThreadSleep(kPollMilliseconds);
            continue;
        }
        uint32_t at = uint32_t(written % queue->capacity);
        uint32_t count = queue->capacity - at;
        count = count < queue->capacity - uint32_t(held) ? count : queue->capacity - uint32_t(held);
        count = count < frames ? count : frames;
        prMemCopy(queue->samples + size_t(at) * channels, samples, size_t(count) * channels * sizeof(float));
        queue->written.store(written + count);
        samples += size_t(count) * channels;
        frames -= count;
    }
    return true;
}

void prMediaAudioQueueFinish(PrMediaAudioQueue* queue, uint32_t id) {
    queue->ended.store(true);
    while (!prMediaAudioQueueIsInterrupted(queue, id))
        prThreadSleep(kPollMilliseconds);
}

PrMediaAudioChunk prMediaAudioQueueRead(PrMediaAudioQueue* queue, float* samples, uint32_t frames) {
    PrMediaAudioChunk chunk{};
    if (queue->seekWanted.load() != queue->seekDone.load())
        return chunk;
    uint32_t channels = queue->channels;
    uint64_t base = queue->baseFrame.load();
    uint64_t read = queue->consumed.loadRelaxed();
    // Frames from before the last request are skipped.
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
        prMemCopy(samples, queue->samples + size_t(at) * channels, size_t(run) * channels * sizeof(float));
        samples += size_t(run) * channels;
        read += run;
        count -= run;
    }
    queue->consumed.store(read);
    return chunk;
}

void prMediaAudioQueueWait(const PrMediaAudioQueue* queue, uint32_t frames) {
    uint64_t wanted = frames < queue->capacity ? frames : queue->capacity;
    while (!queue->stopping.load()) {
        if (queue->seekWanted.load() == queue->seekDone.load()) {
            uint64_t base = queue->baseFrame.load();
            uint64_t read = queue->consumed.loadRelaxed();
            read = read < base ? base : read;
            if (queue->ended.load() || queue->written.load() - read >= wanted)
                return;
        }
        prThreadSleep(kReaderPollMilliseconds);
    }
}
