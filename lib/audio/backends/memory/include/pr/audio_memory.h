// The memory audio output: keeps what it would play in memory, for a run without a device.

#pragma once

#include "pr/audio.h"
#include "pr/base.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Creates an output that renders on a thread of its own, at the pace of the system's clock, and appends what it renders
// to its samples. Its latency is zero.
PrAudioOutput* prAudioMemoryCreate(PrAudioFormat format);

// The frames rendered so far, and their samples, valid until the output starts again or is destroyed. The output must
// be stopped. `output` must come from prAudioMemoryCreate.
uint64_t prAudioMemoryFrameCount(const PrAudioOutput* output);
const float* PR_NULLABLE prAudioMemorySamples(const PrAudioOutput* output);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
