#include "pr/atomic.h"
#include "pr/audio_apple.h"
#include "pr/thread.h"

#include "tools/harness/test.h"

using namespace pr_impl;

TEST_CASE("the default device pulls frames at about its rate") {
    PrAudioOutput* output = prAudioAppleCreate(PrAudioFormat{48000, 2});
    if (output == nullptr) {
        MESSAGE("skipped: this system has no output device");
        return;
    }
    Atomic<uint64_t> pulled;
    pulled.storeRelaxed(0);
    PrAudioRenderFn render = [](void* context, float* samples, uint32_t frames) {
        // Silence, so that a test run makes no sound.
        for (uint32_t i = 0; i < frames * 2; ++i)
            samples[i] = 0;
        Atomic<uint64_t>* total = static_cast<Atomic<uint64_t>*>(context);
        total->storeRelaxed(total->loadRelaxed() + frames);
    };
    if (!prAudioOutputStart(output, render, &pulled)) {
        MESSAGE("skipped: the output device does not start");
        prAudioOutputDestroy(output);
        return;
    }
    prThreadSleep(500);
    prAudioOutputStop(output);
    uint64_t frames = pulled.loadRelaxed();
    CHECK(frames > 12000);
    CHECK(frames < 48000);
    CHECK(prAudioOutputLatency(output) >= 0);
    CHECK(prAudioOutputLatency(output) < 1);
    prAudioOutputDestroy(output);
}
