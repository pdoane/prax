// The interface that audio output backends implement. Callers use the functions of pr/audio.h, which call these. Only
// backends include this header.

#pragma once

#include "pr/audio.h"
#include "pr/base.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// The functions of an output's backend, at the start of its state. Each implements the function of pr/audio.h that it
// is named for. A null `nowFn` means that the output's clock is the system's monotonic clock, and a null
// `isHostPacedFn` that the output is not paced by its host.
struct PrAudioOutput {
    void (*destroyFn)(PrAudioOutput* output);
    PrAudioFormat (*formatFn)(const PrAudioOutput* output);
    bool (*startFn)(PrAudioOutput* output, PrAudioRenderFn render, void* PR_NULLABLE context);
    void (*stopFn)(PrAudioOutput* output);
    double (*latencyFn)(PrAudioOutput* output);
    double (*PR_NULLABLE nowFn)(const PrAudioOutput* output);
    bool (*PR_NULLABLE isHostPacedFn)(const PrAudioOutput* output);
};

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
