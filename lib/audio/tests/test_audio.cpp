#include "pr/audio.h"
#include "pr/audio_memory.h"
#include "pr/thread.h"
#include "pr/time.h"

#include "tools/harness/test.h"

#include <vector>

extern "C" int prTestAudioCApi(void);

namespace pr_impl {

namespace {

// The renders of an output: the frames of each, and the output's clock as each began.
struct Renders {
    PrAudioOutput* output;
    std::vector<uint32_t> frames;
    std::vector<double> times;
};

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("the public headers compile as C") {
    CHECK(prTestAudioCApi() == 0);
}

TEST_CASE("a backend's name round trips, ignoring case") {
    CHECK(prAudioOutputBackendFromName("MEMORY") == PrAudioOutputBackend_Memory);
    CHECK(prAudioOutputBackendName(PrAudioOutputBackend_Memory) == "memory");
    CHECK(prAudioOutputBackendFromName("nothing") == PrAudioOutputBackend_Count);
}

TEST_CASE("a memory output keeps what it renders, at the pace of the system's clock" *
        doctest::skip(!prThreadSupported())) {
    PrAudioOutput* output = prAudioOutputCreate(PrAudioOutputBackend_Memory, PrAudioFormat{48000, 2});
    REQUIRE(output != nullptr);
    CHECK(prAudioOutputLatency(output) == 0);
    CHECK(!prAudioOutputIsHostPaced(output));
    PrAudioRenderFn render = [](void*, float* samples, uint32_t frames) {
        for (uint32_t i = 0; i < frames * 2; ++i)
            samples[i] = 0.5f;
    };
    double started = prTimeMonotonicMilliseconds();
    REQUIRE(prAudioOutputStart(output, render, nullptr));
    prThreadSleep(100);
    prAudioOutputStop(output);
    double elapsed = prTimeMonotonicMilliseconds() - started;
    // The output holds every frame of the time that it ran, however late its thread ran.
    uint64_t frames = prAudioMemoryFrameCount(output);
    REQUIRE(frames >= 4800);
    CHECK(double(frames) <= elapsed / 1000 * 48000);
    CHECK(prAudioMemorySamples(output)[frames * 2 - 1] == 0.5f);
    prAudioOutputDestroy(output);
}

TEST_CASE("a memory output that its host paces renders on the host's thread as its clock moves") {
    PrAudioOutput* output = prAudioMemoryCreate(PrAudioFormat{48000, 1}, PrAudioMemoryPace_Host);
    CHECK(prAudioOutputIsHostPaced(output));
    CHECK(prAudioOutputNow(output) == 0);
    prAudioMemoryAdvance(output, 1000);
    CHECK(prAudioOutputNow(output) == 1000);
    Renders renders{.output = output};
    PrAudioRenderFn render = [](void* context, float* samples, uint32_t frames) {
        Renders* r = static_cast<Renders*>(context);
        r->frames.push_back(frames);
        r->times.push_back(prAudioOutputNow(r->output));
        for (uint32_t i = 0; i < frames; ++i)
            samples[i] = 0.5f;
    };
    REQUIRE(prAudioOutputStart(output, render, &renders));
    CHECK(renders.frames.empty());
    prAudioMemoryAdvance(output, 1100);
    uint64_t total = 0;
    for (size_t i = 0; i < renders.frames.size(); ++i) {
        // Each render begins where the frames before it end.
        CHECK(renders.times[i] == doctest::Approx(1000 + double(total) / 48));
        total += renders.frames[i];
    }
    CHECK(total == 4800);
    CHECK(prAudioOutputNow(output) == doctest::Approx(1100));
    prAudioOutputStop(output);
    CHECK(prAudioMemoryFrameCount(output) == 4800);
    // A stopped output's clock moves anywhere, and renders nothing.
    prAudioMemoryAdvance(output, 0);
    CHECK(prAudioOutputNow(output) == 0);
    CHECK(prAudioMemoryFrameCount(output) == 4800);
    prAudioOutputDestroy(output);
}
