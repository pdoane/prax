// Audio: outputs that play sound on a device.

#include "pr/audio.h"

#include "pr/ascii.h"
#include "pr/assert.h"
#include "pr/time.h"

#if defined(PR_AUDIO_HAS_APPLE)
#include "pr/audio_apple.h"
#endif
#if defined(PR_AUDIO_HAS_MEMORY)
#include "pr/audio_memory.h"
#endif
#if defined(PR_AUDIO_HAS_WINDOWS)
#include "pr/audio_windows.h"
#endif

namespace pr_impl {

namespace {

constexpr PrStringView kBackendNames[PrAudioOutputBackend_Count] = {"default", "apple", "windows", "memory"};

// The backends that the build has, the platform's own first, which Default creates the first of, and then Default, so
// that a build without backends creates none.
constexpr PrAudioOutputBackend kBuilt[] = {
#if defined(PR_AUDIO_HAS_APPLE)
    PrAudioOutputBackend_Apple,
#endif
#if defined(PR_AUDIO_HAS_WINDOWS)
    PrAudioOutputBackend_Windows,
#endif
#if defined(PR_AUDIO_HAS_MEMORY)
    PrAudioOutputBackend_Memory,
#endif
    PrAudioOutputBackend_Default,
};

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrAudioOutputBackend prAudioOutputBackendFromName(PrStringView name) {
    for (PrAudioOutputBackend backend = 0; backend < PrAudioOutputBackend_Count; ++backend) {
        if (prAsciiEqualsFold(kBackendNames[backend], name))
            return backend;
    }
    return PrAudioOutputBackend_Count;
}

PrStringView prAudioOutputBackendName(PrAudioOutputBackend backend) {
    return backend < PrAudioOutputBackend_Count ? kBackendNames[backend] : PrStringView{};
}

bool prAudioOutputBackendIsBuilt(PrAudioOutputBackend backend) {
    for (PrAudioOutputBackend built : kBuilt) {
        if (built != PrAudioOutputBackend_Default && (built == backend || backend == PrAudioOutputBackend_Default))
            return true;
    }
    return false;
}

PrAudioOutput* PR_NULLABLE prAudioOutputCreate(PrAudioOutputBackend backend, PrAudioFormat format) {
    PR_ASSERT(format.sampleRate != 0 && format.channels != 0 && format.channels <= kPrAudioMaxChannels);
    switch (backend == PrAudioOutputBackend_Default ? kBuilt[0] : backend) {
#if defined(PR_AUDIO_HAS_APPLE)
        case PrAudioOutputBackend_Apple: return prAudioAppleCreate(format);
#endif
#if defined(PR_AUDIO_HAS_WINDOWS)
        case PrAudioOutputBackend_Windows: return prAudioWindowsCreate(format);
#endif
#if defined(PR_AUDIO_HAS_MEMORY)
        case PrAudioOutputBackend_Memory: return prAudioMemoryCreate(format, PrAudioMemoryPace_System);
#endif
        default: return nullptr;
    }
}

void prAudioOutputDestroy(PrAudioOutput* PR_NULLABLE output) {
    if (output != nullptr)
        output->destroy(output);
}

PrAudioFormat prAudioOutputFormat(const PrAudioOutput* output) {
    return output->format;
}

bool prAudioOutputStart(PrAudioOutput* output, PrAudioRenderFn render, void* PR_NULLABLE context) {
    return output->start(output, render, context);
}

void prAudioOutputStop(PrAudioOutput* output) {
    output->stop(output);
}

double prAudioOutputLatency(PrAudioOutput* output) {
    return output->latency(output);
}

double prAudioOutputNow(const PrAudioOutput* output) {
    return output->now != nullptr ? output->now(output) : prTimeMonotonicMilliseconds();
}

bool prAudioOutputIsHostPaced(const PrAudioOutput* output) {
    return output->hostPaced;
}
