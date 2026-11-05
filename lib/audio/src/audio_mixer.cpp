// Audio mixing: the sources that share one output.

#include "pr/audio_mixer.h"

#include "pr/alloc.h"
#include "pr/assert.h"
#include "pr/atomic.h"
#include "pr/float.h"
#include "pr/mem.h"
#include "pr/memory.h"
#include "pr/raw_array.h"
#include "pr/thread.h"

namespace pr_impl {

namespace {

// The most frames that the mixer renders at once. A gain ramps across one block.
constexpr uint32_t kBlockFrames = 512;

// The frames over which sound that stops short fades to silence, so that it does not click.
constexpr uint32_t kFadeFrames = 64;

// The length of a time-stretching window and the distance searched either side of its ideal start, in seconds.
constexpr double kStretchWindowSeconds = 0.04;
constexpr double kStretchSearchSeconds = 0.01;

// The frames between those that the search for a window's start compares, which makes the search cheap enough for a
// real-time thread.
constexpr uint32_t kStretchSearchStep = 4;

constexpr double kPi = 3.14159265358979323846;

struct AudioMixer;

// A source, and the state that plays it at a rate other than one. `queue` holds frames read and not yet played. At a
// rate other than one, frames play from `queue` through time-stretching or resampling.
struct AudioSource {
    AudioMixer* mixer;
    PrAudioSourceReadFn read;
    void* context;
    float gain;
    float gainNow;              // the gain at the end of the last block
    double rate;
    bool preservesPitch;
    bool playing;

    float* queue;               // by frame and channel
    uint32_t queueFrames;
    uint32_t queueCapacity;
    double queueTime;           // where the queue's first frame lies on the source's timeline
    double readAt;              // the frame of the queue that plays next, which may lie between two frames
    bool ended;

    // Time-stretching by waveform similarity overlap-add. Each step takes a window of the queue near its ideal start,
    // choosing the start that best continues the waveform, and overlaps it with the window before.
    uint32_t window;
    uint32_t hop;               // half of a window, the frames that one step plays
    uint32_t search;
    float* fade;                // by frame of a hop: the gain of the window that fades in
    float* overlap;             // by frame and channel: the second half of the last window, which fades out
    float* ready;               // by frame and channel: the frames of the last step not yet played
    uint32_t readyFrames;       // the frames of `ready` left to play, which are its last
    double readyTime;           // where the first frame of `ready` lies on the source's timeline
    int64_t lastStart;          // the frame of the queue that the last window started at, or -1 before the first

    // The last frame played, which fades out if the sound stops short.
    float last[kPrAudioMaxChannels];
    uint32_t tailFrames;        // the frames of that fade left to play
    float tail[kPrAudioMaxChannels];

    // The clock, which the output's thread writes and any thread reads, by a sequence that is odd during a write.
    Atomic<uint32_t> clockSequence;
    Atomic<double> clockPosition;
    Atomic<double> clockStamp;
    Atomic<bool> clockValid;
    Atomic<bool> clockEnded;
};

struct AudioMixer {
    PrAudioOutput* output;
    PrAudioFormat format;
    RawArray<AudioSource*> sources;
    float* block;               // by frame and channel
    double latency;             // the output's latency, read when it starts
    PrAudioReadMode readMode;   // how the sources are read, by the output's pace, read when it starts

    // Held by a change to the sources, or by a render. A render does not wait for it.
    Atomic<uint32_t> busy;
};

float* allocSamples(size_t count) {
    return static_cast<float*>(prMemoryAllocZeroed(count * sizeof(float)));
}

void lock(AudioMixer* mixer) {
    while (mixer->busy.exchange(1) != 0)
        prThreadSleep(0);
}

void unlock(AudioMixer* mixer) {
    mixer->busy.store(0);
}

PrAudioClock readClock(const AudioSource* source) {
    PrAudioClock clock{};
    for (;;) {
        uint32_t before = source->clockSequence.load();
        clock.position = source->clockPosition.load();
        clock.stamp = source->clockStamp.load();
        clock.valid = source->clockValid.load();
        clock.ended = source->clockEnded.load();
        if ((before & 1) == 0 && source->clockSequence.load() == before)
            return clock;
    }
}

void publishClock(AudioSource* source, double position, double stamp, bool valid, bool ended) {
    uint32_t sequence = source->clockSequence.loadRelaxed();
    source->clockSequence.store(sequence + 1);
    source->clockPosition.store(position);
    source->clockStamp.store(stamp);
    source->clockValid.store(valid);
    source->clockEnded.store(ended);
    source->clockSequence.store(sequence + 2);
}

// Reads until the queue holds `frames` frames or the source has none ready. Returns true if it holds them.
bool fillQueue(AudioSource* source, uint32_t frames) {
    PR_ASSERT(frames <= source->queueCapacity);
    uint32_t channels = source->mixer->format.channels;
    while (source->queueFrames < frames) {
        float* at = source->queue + size_t(source->queueFrames) * channels;
        PrAudioChunk chunk = source->read(source->context, at, source->queueCapacity - source->queueFrames,
            source->mixer->readMode);
        source->ended = chunk.ended;
        if (chunk.frames == 0)
            return false;
        if (source->queueFrames == 0)
            source->queueTime = chunk.time;
        source->queueFrames += chunk.frames;
    }
    return true;
}

// Drops the frames before `frames` from the queue.
void dropFromQueue(AudioSource* source, uint32_t frames) {
    if (frames == 0)
        return;
    uint32_t channels = source->mixer->format.channels;
    prMemMove(source->queue, source->queue + size_t(frames) * channels,
        size_t(source->queueFrames - frames) * channels * sizeof(float));
    source->queueFrames -= frames;
    source->queueTime += double(frames) / source->mixer->format.sampleRate;
    source->readAt -= frames;
}

// Plays frames at a rate of one: those that the queue still holds from another rate, and then the source's own. Writes
// where the first frame lies to `time`, and returns the frames written.
uint32_t playDirect(AudioSource* source, float* out, uint32_t frames, double* time) {
    PrAudioFormat format = source->mixer->format;
    uint32_t written = 0;
    bool timed = false;
    uint32_t held = uint32_t(source->readAt);
    if (source->queueFrames > held) {
        uint32_t count = source->queueFrames - held < frames ? source->queueFrames - held : frames;
        prMemCopy(out, source->queue + size_t(held) * format.channels, size_t(count) * format.channels * sizeof(float));
        *time = source->queueTime + double(held) / format.sampleRate;
        timed = true;
        written = count;
        source->readAt = held + count;
        dropFromQueue(source, held + count);
    }
    while (written < frames) {
        PrAudioChunk chunk = source->read(source->context, out + size_t(written) * format.channels, frames - written,
            source->mixer->readMode);
        source->ended = chunk.ended;
        if (chunk.frames == 0)
            break;
        if (!timed)
            *time = chunk.time;
        timed = true;
        written += chunk.frames;
    }
    return written;
}

// Plays frames with a pitch that follows the rate, by linear interpolation.
uint32_t playResampled(AudioSource* source, float* out, uint32_t frames, double* time) {
    PrAudioFormat format = source->mixer->format;
    uint32_t written = 0;
    while (written < frames) {
        uint32_t whole = uint32_t(source->readAt);
        if (!fillQueue(source, whole + 2))
            break;
        if (written == 0)
            *time = source->queueTime + source->readAt / format.sampleRate;
        float part = float(source->readAt - whole);
        const float* from = source->queue + size_t(whole) * format.channels;
        for (uint32_t c = 0; c < format.channels; ++c)
            out[size_t(written) * format.channels + c] = from[c] + (from[format.channels + c] - from[c]) * part;
        ++written;
        source->readAt += source->rate;
        // The queue moves only when it nears its end, because moving it costs a copy.
        whole = uint32_t(source->readAt);
        if (whole + 2 >= source->queueCapacity)
            dropFromQueue(source, whole < source->queueFrames ? whole : source->queueFrames);
    }
    return written;
}

// Returns the frame of the queue, near `ideal`, whose first hop best continues the waveform after the last window.
int64_t bestStart(const AudioSource* source, int64_t ideal) {
    uint32_t channels = source->mixer->format.channels;
    int64_t low = ideal - source->search < 0 ? 0 : ideal - source->search;
    int64_t high = ideal + source->search;
    int64_t best = ideal;
    float bestScore = -kPrF32Infinity;
    const float* target = source->queue + (source->lastStart + source->hop) * channels;
    for (int64_t start = low; start <= high; ++start) {
        const float* candidate = source->queue + start * channels;
        float score = 0;
        for (uint32_t i = 0; i < source->hop; i += kStretchSearchStep)
            score += candidate[size_t(i) * channels] * target[size_t(i) * channels];
        if (score > bestScore) {
            bestScore = score;
            best = start;
        }
    }
    return best;
}

// Takes the next window from the queue, and leaves a hop of frames in `ready`. Returns false if the queue cannot hold
// the window yet.
bool stretchStep(AudioSource* source) {
    PrAudioFormat format = source->mixer->format;
    uint32_t channels = format.channels;
    int64_t ideal = int64_t(source->readAt);
    int64_t needed = ideal + source->search + source->window;
    if (source->lastStart >= 0 && source->lastStart + 2 * source->hop > needed)
        needed = source->lastStart + 2 * source->hop;
    if (!fillQueue(source, uint32_t(needed)))
        return false;
    int64_t start = source->lastStart >= 0 ? bestStart(source, ideal) : ideal;
    const float* window = source->queue + start * channels;
    for (uint32_t i = 0; i < source->hop; ++i) {
        float in = source->fade[i];
        for (uint32_t c = 0; c < channels; ++c) {
            size_t at = size_t(i) * channels + c;
            source->ready[at] = source->overlap[at] * (1 - in) + window[at] * in;
        }
    }
    prMemCopy(source->overlap, window + size_t(source->hop) * channels, size_t(source->hop) * channels * sizeof(float));
    source->readyFrames = source->hop;
    source->readyTime = source->queueTime + double(start) / format.sampleRate;
    source->lastStart = start;
    source->readAt += source->hop * source->rate;
    int64_t keep = int64_t(source->readAt) - source->search;
    keep = keep < start ? keep : start;
    if (keep > 0) {
        dropFromQueue(source, uint32_t(keep));
        source->lastStart -= keep;
    }
    return true;
}

// Plays frames at the rate with the source's own pitch.
uint32_t playStretched(AudioSource* source, float* out, uint32_t frames, double* time) {
    PrAudioFormat format = source->mixer->format;
    uint32_t written = 0;
    while (written < frames) {
        if (source->readyFrames == 0 && !stretchStep(source))
            break;
        uint32_t from = source->hop - source->readyFrames;
        uint32_t count = source->readyFrames < frames - written ? source->readyFrames : frames - written;
        if (written == 0)
            *time = source->readyTime + double(from) * source->rate / format.sampleRate;
        prMemCopy(out + size_t(written) * format.channels, source->ready + size_t(from) * format.channels,
            size_t(count) * format.channels * sizeof(float));
        source->readyFrames -= count;
        written += count;
    }
    return written;
}

// Adds one block of the source to `mix`. The block's first frame is heard at `stamp` on the output's clock, plus the
// output's latency.
void mixSource(AudioMixer* mixer, AudioSource* source, float* mix, uint32_t frames, double stamp) {
    uint32_t channels = mixer->format.channels;
    float target = source->playing ? source->gain : 0;
    uint32_t written = 0;
    double time = 0;
    if (source->playing || source->gainNow != 0) {
        if (source->rate == 1)
            written = playDirect(source, mixer->block, frames, &time);
        else if (source->preservesPitch)
            written = playStretched(source, mixer->block, frames, &time);
        else
            written = playResampled(source, mixer->block, frames, &time);
    }
    if (written != 0) {
        // The block's first frame is heard after the output's latency.
        publishClock(source, time - mixer->latency * source->rate, stamp, true, false);
        float step = (target - source->gainNow) / float(frames);
        for (uint32_t i = 0; i < written; ++i) {
            float gain = source->gainNow + step * float(i);
            for (uint32_t c = 0; c < channels; ++c)
                mix[size_t(i) * channels + c] += mixer->block[size_t(i) * channels + c] * gain;
        }
        for (uint32_t c = 0; c < channels; ++c)
            source->last[c] =
                mixer->block[size_t(written - 1) * channels + c] * (source->gainNow + step * float(written));
    }
    if (written < frames) {
        // The sound stopped short, so it fades from its last frame, and ramps up again when it returns.
        if (source->gainNow != 0 && written != 0) {
            prMemCopy(source->tail, source->last, sizeof(source->tail));
            source->tailFrames = kFadeFrames;
        }
        source->gainNow = 0;
        if (source->ended && written == 0 && source->playing) {
            PrAudioClock clock = readClock(source);
            publishClock(source, clock.position, clock.stamp, clock.valid, true);
        }
    } else {
        source->gainNow = target;
    }
    for (uint32_t i = written; i < frames && source->tailFrames != 0; ++i) {
        float gain = float(source->tailFrames) / kFadeFrames;
        for (uint32_t c = 0; c < channels; ++c)
            mix[size_t(i) * channels + c] += source->tail[c] * gain;
        --source->tailFrames;
    }
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

// C opaque types
struct PrAudioMixer : AudioMixer {};
struct PrAudioSource : AudioSource {};

PrAudioMixer* prAudioMixerCreate(PrAudioOutput* output) {
    PrAudioMixer* mixer = allocZeroed<PrAudioMixer>();
    mixer->output = output;
    mixer->format = prAudioOutputFormat(output);
    mixer->block = allocSamples(size_t(kBlockFrames) * mixer->format.channels);
    return mixer;
}

void prAudioMixerDestroy(PrAudioMixer* PR_NULLABLE mixer) {
    if (mixer == nullptr)
        return;
    PR_ASSERT(mixer->sources.count == 0);
    mixer->sources.reset();
    prMemoryFree(mixer->block);
    prMemoryFree(mixer);
}

void prAudioMixerRender(void* PR_NULLABLE context, float* samples, uint32_t frames) {
    AudioMixer* mixer = static_cast<PrAudioMixer*>(context);
    uint32_t channels = mixer->format.channels;
    prMemZero(samples, size_t(frames) * channels * sizeof(float));
    if (mixer->busy.exchange(1) != 0)
        return;
    double now = prAudioOutputNow(mixer->output);
    for (uint32_t at = 0; at < frames; at += kBlockFrames) {
        uint32_t count = frames - at < kBlockFrames ? frames - at : kBlockFrames;
        float* mix = samples + size_t(at) * channels;
        double stamp = now + double(at) * 1000 / mixer->format.sampleRate;
        for (uint32_t i = 0; i < mixer->sources.count; ++i)
            mixSource(mixer, mixer->sources.data[i], mix, count, stamp);
    }
    unlock(mixer);
    for (size_t i = 0; i < size_t(frames) * channels; ++i) {
        if (samples[i] > 1)
            samples[i] = 1;
        else if (samples[i] < -1)
            samples[i] = -1;
    }
}

PrAudioFormat prAudioMixerFormat(const PrAudioMixer* mixer) {
    return mixer->format;
}

double prAudioMixerNow(const PrAudioMixer* mixer) {
    return prAudioOutputNow(mixer->output);
}

PrAudioSource* PR_NULLABLE prAudioSourceCreate(PrAudioMixer* mixer, PrAudioSourceReadFn read, void* PR_NULLABLE context) {
    if (mixer->sources.count == 0) {
        if (!prAudioOutputStart(mixer->output, prAudioMixerRender, mixer))
            return nullptr;
        mixer->latency = prAudioOutputLatency(mixer->output);
        mixer->readMode = prAudioOutputIsHostPaced(mixer->output) ? PrAudioReadMode_Wait : PrAudioReadMode_Ready;
    }
    PrAudioFormat format = mixer->format;
    PrAudioSource* source = allocZeroed<PrAudioSource>();
    source->mixer = mixer;
    source->read = read;
    source->context = context;
    source->gain = 1;
    source->rate = 1;
    source->preservesPitch = true;
    source->window = uint32_t(format.sampleRate * kStretchWindowSeconds) & ~1u;
    source->hop = source->window / 2;
    source->search = uint32_t(format.sampleRate * kStretchSearchSeconds);
    source->lastStart = -1;
    // A window at the far end of the search, and a step at the highest rate past it.
    source->queueCapacity = source->window + 2 * source->search + uint32_t(source->hop * kPrAudioSourceMaxRate) + 2;
    source->queue = allocSamples(size_t(source->queueCapacity) * format.channels);
    source->fade = allocSamples(source->hop);
    for (uint32_t i = 0; i < source->hop; ++i) {
        float wave = float(prF64Sin((double(i) + 0.5) / source->hop * kPi / 2));
        source->fade[i] = wave * wave;
    }
    source->overlap = allocSamples(size_t(source->hop) * format.channels);
    source->ready = allocSamples(size_t(source->hop) * format.channels);
    lock(mixer);
    mixer->sources.push(source);
    unlock(mixer);
    return source;
}

void prAudioSourceDestroy(PrAudioSource* PR_NULLABLE source) {
    if (source == nullptr)
        return;
    AudioMixer* mixer = source->mixer;
    lock(mixer);
    for (uint32_t i = 0; i < mixer->sources.count; ++i) {
        if (mixer->sources.data[i] == source) {
            mixer->sources.erase(i);
            break;
        }
    }
    bool last = mixer->sources.count == 0;
    unlock(mixer);
    if (last)
        prAudioOutputStop(mixer->output);
    prMemoryFree(source->queue);
    prMemoryFree(source->fade);
    prMemoryFree(source->overlap);
    prMemoryFree(source->ready);
    prMemoryFree(source);
}

void prAudioSourceSetPlaying(PrAudioSource* source, bool playing) {
    if (source->playing == playing)
        return;
    lock(source->mixer);
    source->playing = playing;
    unlock(source->mixer);
}

void prAudioSourceSetGain(PrAudioSource* source, float gain) {
    if (source->gain == gain)
        return;
    lock(source->mixer);
    source->gain = gain;
    unlock(source->mixer);
}

void prAudioSourceSetRate(PrAudioSource* source, double rate, bool preservesPitch) {
    PR_ASSERT(rate >= kPrAudioSourceMinRate && rate <= kPrAudioSourceMaxRate);
    if (source->rate == rate && source->preservesPitch == preservesPitch)
        return;
    lock(source->mixer);
    source->rate = rate;
    source->preservesPitch = preservesPitch;
    source->lastStart = -1;
    source->readyFrames = 0;
    prMemZero(source->overlap, size_t(source->hop) * source->mixer->format.channels * sizeof(float));
    unlock(source->mixer);
}

void prAudioSourceReset(PrAudioSource* source) {
    AudioMixer* mixer = source->mixer;
    lock(mixer);
    source->queueFrames = 0;
    source->readAt = 0;
    source->lastStart = -1;
    source->readyFrames = 0;
    source->ended = false;
    prMemZero(source->overlap, size_t(source->hop) * mixer->format.channels * sizeof(float));
    if (source->gainNow != 0) {
        prMemCopy(source->tail, source->last, sizeof(source->tail));
        source->tailFrames = kFadeFrames;
    }
    source->gainNow = 0;
    publishClock(source, 0, prAudioOutputNow(mixer->output), false, false);
    unlock(mixer);
}

PrAudioClock prAudioSourceClock(const PrAudioSource* source) {
    return readClock(source);
}
