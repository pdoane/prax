// The memory audio output: keeps what it would play in memory, for a run without a device.

#include "pr/audio_memory.h"

#include "pr/alloc.h"
#include "pr/assert.h"
#include "pr/atomic.h"
#include "pr/audio_backend.h"
#include "pr/memory.h"
#include "pr/raw_array.h"
#include "pr/thread.h"
#include "pr/time.h"

namespace pr_impl {

namespace {

// The time between renders, in milliseconds, as a device's buffer would set it.
constexpr double kRenderMilliseconds = 10;

struct MemoryOutput : PrAudioOutput {
    PrAudioFormat format;
    PrAudioMemoryPace pace;
    PrThread thread;
    PrAudioRenderFn render;
    void* context;
    RawArray<float> samples;       // interleaved
    Atomic<bool> stopping;
    bool running;

    // The time at which the output last started, from which `rendered` frames have played, in milliseconds on the
    // output's clock. An output that its host paces keeps its clock in `time` while it is stopped.
    double startTime;
    uint64_t rendered;
    double time;
};

// Appends the `frames` frames that the render function writes.
void renderFrames(MemoryOutput* output, uint32_t frames) {
    uint32_t at = output->samples.count;
    output->samples.resize(at + frames * output->format.channels);
    output->render(output->context, output->samples.data + at, frames);
}

// Renders the frames that the system's clock plays while the output runs. Once the output stops, it renders the frames
// up to the stop, so that the output holds every frame of the time that it ran, however late its thread runs.
void renderLoop(MemoryOutput* output) {
    for (;;) {
        bool stopping = output->stopping.load();
        double elapsed = prTimeMonotonicMilliseconds() - output->startTime;
        uint64_t due = uint64_t(elapsed / 1000 * output->format.sampleRate);
        if (due > output->rendered) {
            renderFrames(output, uint32_t(due - output->rendered));
            output->rendered = due;
        }
        if (stopping)
            return;
        prThreadSleep(kRenderMilliseconds);
    }
}

double hostNow(const PrAudioOutput* base) {
    const MemoryOutput* output = static_cast<const MemoryOutput*>(base);
    if (!output->running)
        return output->time;
    return output->startTime + double(output->rendered) * 1000 / output->format.sampleRate;
}

bool start(PrAudioOutput* base, PrAudioRenderFn render, void* PR_NULLABLE context) {
    MemoryOutput* output = static_cast<MemoryOutput*>(base);
    output->render = render;
    output->context = context;
    output->running = true;
    output->rendered = 0;
    if (output->pace == PrAudioMemoryPace_Host) {
        output->startTime = output->time;
        return true;
    }
    output->startTime = prTimeMonotonicMilliseconds();
    output->stopping.store(false);
    prThreadStart(&output->thread, [](void* context) { renderLoop(static_cast<MemoryOutput*>(context)); }, output);
    return true;
}

void stop(PrAudioOutput* base) {
    MemoryOutput* output = static_cast<MemoryOutput*>(base);
    if (!output->running)
        return;
    if (output->pace == PrAudioMemoryPace_Host) {
        output->time = hostNow(output);
    } else {
        output->stopping.store(true);
        prThreadJoin(&output->thread);
    }
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

PrAudioFormat formatOf(const PrAudioOutput* output) {
    return static_cast<const MemoryOutput*>(output)->format;
}

bool isHostPaced(const PrAudioOutput*) {
    return true;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrAudioOutput* prAudioMemoryCreate(PrAudioFormat format, PrAudioMemoryPace pace) {
    MemoryOutput* output = allocZeroed<MemoryOutput>();
    output->format = format;
    output->destroyFn = destroy;
    output->formatFn = formatOf;
    output->startFn = start;
    output->stopFn = stop;
    output->latencyFn = latency;
    output->pace = pace;
    if (pace == PrAudioMemoryPace_Host) {
        output->nowFn = hostNow;
        output->isHostPacedFn = isHostPaced;
    }
    return output;
}

void prAudioMemoryAdvance(PrAudioOutput* output, double time) {
    MemoryOutput* memory = static_cast<MemoryOutput*>(output);
    PR_ASSERT(memory->pace == PrAudioMemoryPace_Host);
    if (!memory->running) {
        memory->time = time;
        return;
    }
    PR_ASSERT(time >= memory->startTime);
    // The frames render in pieces of a device's buffer, so that the clock moves through the time as a device's does.
    uint64_t due = uint64_t((time - memory->startTime) / 1000 * memory->format.sampleRate);
    uint64_t piece = uint64_t(kRenderMilliseconds / 1000 * memory->format.sampleRate);
    while (memory->rendered < due) {
        uint64_t frames = due - memory->rendered < piece ? due - memory->rendered : piece;
        renderFrames(memory, uint32_t(frames));
        memory->rendered += frames;
    }
}

uint64_t prAudioMemoryFrameCount(const PrAudioOutput* output) {
    const MemoryOutput* memory = static_cast<const MemoryOutput*>(output);
    return memory->samples.count / memory->format.channels;
}

const float* PR_NULLABLE prAudioMemorySamples(const PrAudioOutput* output) {
    return static_cast<const MemoryOutput*>(output)->samples.data;
}
