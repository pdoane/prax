// Audio mixing: the sources that share one output.
//
// A mixer sums its sources into its output. It runs each source at its own gain and rate, and ramps every change so
// that the change does not click. The mixer runs its output only while it has sources.
//
// Calls that change a source come from one thread at a time. The output's thread reads the sources without waiting, and
// a render that finds a change in progress plays silence for that render.

#pragma once

#include "pr/audio.h"
#include "pr/base.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// The lowest and highest rates that a source plays at.
static const double kPrAudioSourceMinRate = 0.25;
static const double kPrAudioSourceMaxRate = 4;

// The sources that share one output.
typedef struct PrAudioMixer PrAudioMixer;

// One stream of samples in a mixer.
typedef struct PrAudioSource PrAudioSource;

// The samples that one read of a source gave.
typedef struct PrAudioChunk {
    uint32_t frames;
    double time;            // where the first frame lies on the source's timeline, in seconds
    bool ended;             // no frames follow until the source is reset
} PrAudioChunk;

// Writes up to `frames` frames to `samples`, in the mixer's format. Returns no frames if none are ready. Called on the
// output's real-time thread, so the function must not wait on a lock or allocate.
typedef PrAudioChunk (*PrAudioSourceReadFn)(void* PR_NULLABLE context, float* samples, uint32_t frames);

// What a source plays, as of one moment.
typedef struct PrAudioClock {
    double position;        // the time on the source's timeline that is heard at `stamp`, in seconds
    double stamp;           // the moment, as prTimeMonotonicMilliseconds gives it
    bool valid;             // the source has played since it was created or last reset
    bool ended;             // the source has played all of its frames
} PrAudioClock;

// Creates a mixer that plays on `output`, which must outlive the mixer and which the mixer alone starts and stops.
PrAudioMixer* prAudioMixerCreate(PrAudioOutput* output);

// Frees the mixer. Its sources must be destroyed first.
void prAudioMixerDestroy(PrAudioMixer* PR_NULLABLE mixer);

// Writes the sum of the sources of the mixer that `context` points to to `samples`. This is the mixer's render
// function, which a test may call in place of an output. Samples are limited to the range from -1 to 1.
void prAudioMixerRender(void* PR_NULLABLE context, float* samples, uint32_t frames);

// The layout of the mixer's samples.
PrAudioFormat prAudioMixerFormat(const PrAudioMixer* mixer);

// Creates a source that reads through `read`, stopped, at a gain and a rate of one, and starts the output if the source
// is the only one. Returns null if the output does not start.
PrAudioSource* PR_NULLABLE prAudioSourceCreate(PrAudioMixer* mixer, PrAudioSourceReadFn read, void* PR_NULLABLE context);

// Removes the source from its mixer and frees it. Once this returns, no call of its read function is running.
void prAudioSourceDestroy(PrAudioSource* PR_NULLABLE source);

// Starts or stops the source. A source that stops fades out and keeps its place.
void prAudioSourceSetPlaying(PrAudioSource* source, bool playing);

// Sets the gain that the source's samples are multiplied by.
void prAudioSourceSetGain(PrAudioSource* source, float gain);

// Sets the rate of the source, from kPrAudioSourceMinRate to kPrAudioSourceMaxRate. If `preservesPitch` is true, the
// source changes speed at its own pitch. Otherwise its pitch follows the rate.
void prAudioSourceSetRate(PrAudioSource* source, double rate, bool preservesPitch);

// Drops what the source has read but not played, because the next read continues elsewhere on its timeline.
void prAudioSourceReset(PrAudioSource* source);

// What the source plays now. Safe to call while the output renders.
PrAudioClock prAudioSourceClock(const PrAudioSource* source);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
