// The memory audio output: keeps what it would play in memory, for a run without a device.

#pragma once

#include "pr/audio.h"
#include "pr/base.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// What sets the pace at which a memory output renders.
enum {
    PrAudioMemoryPace_System,   // a thread of its own renders at the pace of the system's clock
    PrAudioMemoryPace_Host,     // the host's thread renders as the host moves the output's clock (prAudioMemoryAdvance)
};
typedef uint8_t PrAudioMemoryPace;

// Creates an output that renders at `pace` and appends what it renders to its samples. Its latency is zero. An output
// that its host paces keeps a clock of its own (prAudioOutputNow), which starts at zero.
PrAudioOutput* prAudioMemoryCreate(PrAudioFormat format, PrAudioMemoryPace pace);

// Moves the clock of an output that its host paces to `time`, in milliseconds. A started output first renders, on the
// caller's thread, the frames that play until `time`, and its clock must not move back. A stopped output's clock moves
// anywhere. `output` must come from prAudioMemoryCreate with PrAudioMemoryPace_Host.
void prAudioMemoryAdvance(PrAudioOutput* output, double time);

// The frames rendered so far, and their samples, valid until the output starts again or is destroyed. The output must
// be stopped. `output` must come from prAudioMemoryCreate.
uint64_t prAudioMemoryFrameCount(const PrAudioOutput* output);
const float* PR_NULLABLE prAudioMemorySamples(const PrAudioOutput* output);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
