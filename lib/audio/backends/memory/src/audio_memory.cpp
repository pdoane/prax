// The memory audio output: keeps what it would play in memory, for a run without a device.

#include "pr/audio_memory.h"

#include "pr/alloc.h"
#include "pr/array.h"
#include "pr/atomic.h"
#include "pr/memory.h"
#include "pr/thread.h"
#include "pr/time.h"

namespace pr_impl {

namespace {

// The time between renders, in milliseconds, as a device's buffer would set it.
constexpr double kRenderMilliseconds = 10;

struct MemoryOutput : PrAudioOutput {
    PrThread thread;
    PrAudioRenderFn render;
    void* context;
    Array<float> samples;       // interleaved
    Atomic<bool> stopping;
    bool running;
};

void renderLoop(MemoryOutput* output) {
    uint32_t channels = output->format.channels;
    double start = prTimeMonotonicMilliseconds();
    uint64_t rendered = 0;
    while (!output->stopping.load()) {
        double elapsed = prTimeMonotonicMilliseconds() - start;
        uint64_t due = uint64_t(elapsed / 1000 * output->format.sampleRate);
        if (due > rendered) {
            uint32_t frames = uint32_t(due - rendered);
            uint32_t at = output->samples.count;
            output->samples.resize(at + frames * channels);
            output->render(output->context, output->samples.data + at, frames);
            rendered = due;
        }
        prThreadSleep(kRenderMilliseconds);
    }
}

bool start(PrAudioOutput* base, PrAudioRenderFn render, void* PR_NULLABLE context) {
    MemoryOutput* output = static_cast<MemoryOutput*>(base);
    output->render = render;
    output->context = context;
    output->stopping.store(false);
    output->running = true;
    prThreadStart(&output->thread, [](void* context) { renderLoop(static_cast<MemoryOutput*>(context)); }, output);
    return true;
}

void stop(PrAudioOutput* base) {
    MemoryOutput* output = static_cast<MemoryOutput*>(base);
    if (!output->running)
        return;
    output->stopping.store(true);
    prThreadJoin(&output->thread);
    output->running = false;
}

double latency(PrAudioOutput*) {
    return 0;
}

void destroy(PrAudioOutput* base) {
    MemoryOutput* output = static_cast<MemoryOutput*>(base);
    stop(output);
    output->samples.reset();
    prMemoryFree(output);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrAudioOutput* prAudioMemoryCreate(PrAudioFormat format) {
    MemoryOutput* output = allocZeroed<MemoryOutput>();
    output->format = format;
    output->destroy = destroy;
    output->start = start;
    output->stop = stop;
    output->latency = latency;
    return output;
}

uint64_t prAudioMemoryFrameCount(const PrAudioOutput* output) {
    return static_cast<const MemoryOutput*>(output)->samples.count / output->format.channels;
}

const float* PR_NULLABLE prAudioMemorySamples(const PrAudioOutput* output) {
    return static_cast<const MemoryOutput*>(output)->samples.data;
}
