#include "pr/audio.h"
#include "pr/audio_memory.h"
#include "pr/thread.h"

#include "tools/harness/test.h"

extern "C" int prTestAudioCApi(void);

using namespace pr_impl;

TEST_CASE("the public headers compile as C") {
    CHECK(prTestAudioCApi() == 0);
}

TEST_CASE("a backend's name round trips, ignoring case") {
    CHECK(prAudioOutputBackendFromName("MEMORY") == PrAudioOutputBackend_Memory);
    CHECK(prAudioOutputBackendName(PrAudioOutputBackend_Memory) == "memory");
    CHECK(prAudioOutputBackendFromName("nothing") == PrAudioOutputBackend_Count);
}

TEST_CASE("a memory output keeps what it renders, at the pace of the clock" * doctest::skip(!prThreadSupported())) {
    PrAudioOutput* output = prAudioOutputCreate(PrAudioOutputBackend_Memory, PrAudioFormat{48000, 2});
    REQUIRE(output != nullptr);
    CHECK(prAudioOutputLatency(output) == 0);
    PrAudioRenderFn render = [](void*, float* samples, uint32_t frames) {
        for (uint32_t i = 0; i < frames * 2; ++i)
            samples[i] = 0.5f;
    };
    REQUIRE(prAudioOutputStart(output, render, nullptr));
    prThreadSleep(100);
    prAudioOutputStop(output);
    uint64_t frames = prAudioMemoryFrameCount(output);
    CHECK(frames > 2400);
    CHECK(frames < 48000);
    CHECK(prAudioMemorySamples(output)[frames * 2 - 1] == 0.5f);
    prAudioOutputDestroy(output);
}
