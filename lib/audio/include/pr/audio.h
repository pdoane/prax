// Audio: outputs that play sound on a device.
//
// An output pulls interleaved 32-bit float samples from a render function. The output calls that function on a
// real-time thread of its own, so the function must not wait on a lock or allocate.

#pragma once

#include "pr/base.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// The most channels that an output plays.
enum { kPrAudioMaxChannels = 8 };

// The layout of interleaved samples.
typedef struct PrAudioFormat {
    uint32_t sampleRate;    // frames per second
    uint32_t channels;      // samples per frame, at most kPrAudioMaxChannels
} PrAudioFormat;

// Writes `frames` frames to `samples`. Called on the output's real-time thread.
typedef void (*PrAudioRenderFn)(void* PR_NULLABLE context, float* samples, uint32_t frames);

// A device that plays sound. Each backend provides its own constructor.
typedef struct PrAudioOutput PrAudioOutput;

// The function table of an output, placed at the start of the backend's state. Each function member implements the
// public function of the same name. Callers use the functions below, never these members.
struct PrAudioOutput {
    PrAudioFormat format;
    void (*destroy)(PrAudioOutput* output);
    bool (*start)(PrAudioOutput* output, PrAudioRenderFn render, void* PR_NULLABLE context);
    void (*stop)(PrAudioOutput* output);
    double (*latency)(PrAudioOutput* output);
};

// The library's backends of outputs, which the build turns on (README.md), and Default, the platform's own or else the
// first that the build has.
enum {
    PrAudioOutputBackend_Default,
    PrAudioOutputBackend_Apple,     // Core Audio
    PrAudioOutputBackend_Windows,   // WASAPI
    PrAudioOutputBackend_Memory,    // keeps the samples in memory
    PrAudioOutputBackend_Count,
};
typedef uint8_t PrAudioOutputBackend;

// Returns the backend that `name` names, as prAudioOutputBackendName spells it, ignoring case, or Count if it names
// none.
PrAudioOutputBackend prAudioOutputBackendFromName(PrStringView name);

// Returns the name of a backend in lowercase, as a command line spells it.
PrStringView prAudioOutputBackendName(PrAudioOutputBackend backend);

// Returns true if the build has the backend. The build has Default if it has any backend.
bool prAudioOutputBackendIsBuilt(PrAudioOutputBackend backend);

// Creates an output of `format` by the backend. Returns null if the build does not have the backend, or the backend's
// constructor fails, as it does on a system without an output device.
PrAudioOutput* PR_NULLABLE prAudioOutputCreate(PrAudioOutputBackend backend, PrAudioFormat format);

// Frees the output, which stops it first.
void prAudioOutputDestroy(PrAudioOutput* PR_NULLABLE output);

// The layout of the samples that the output plays.
PrAudioFormat prAudioOutputFormat(const PrAudioOutput* output);

// Starts calling `render`. The output must be stopped. Returns false if the device does not start.
bool prAudioOutputStart(PrAudioOutput* output, PrAudioRenderFn render, void* PR_NULLABLE context);

// Stops calling the render function, and returns once no call is running. An output that is stopped may start again.
void prAudioOutputStop(PrAudioOutput* output);

// The time between a render call and the moment that its first sample is heard, in seconds.
double prAudioOutputLatency(PrAudioOutput* output);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
