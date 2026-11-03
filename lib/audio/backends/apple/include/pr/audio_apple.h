// The Core Audio output.

#pragma once

#include "pr/audio.h"
#include "pr/base.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Creates an output on the system's default device, which follows the default as the user changes it. The system
// converts `format` to the device's own. Returns null if the system has no output unit.
PrAudioOutput* PR_NULLABLE prAudioAppleCreate(PrAudioFormat format);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
