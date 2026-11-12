#include "pr/audio_memory.h"
#include "pr/audio_mixer.h"
#include "pr/float.h"
#include "pr/thread.h"

#include "tools/harness/test.h"

namespace pr_impl {

namespace {

constexpr uint32_t kRate = 48000;
constexpr double kPi = 3.14159265358979323846;

// A sine wave on one channel, with a timeline that starts at zero.
struct Tone {
    double frequency = 1000;
    uint64_t frames = 0;
    uint64_t limit = UINT64_MAX;
};

PrAudioChunk readTone(void* context, float* samples, uint32_t frames, PrAudioReadMode) {
    Tone* tone = static_cast<Tone*>(context);
    PrAudioChunk chunk{.time = double(tone->frames) / kRate};
    uint64_t left = tone->limit - tone->frames;
    chunk.frames = left < frames ? uint32_t(left) : frames;
    for (uint32_t i = 0; i < chunk.frames; ++i)
        samples[i] = float(prF64Sin(2 * kPi * tone->frequency * double(tone->frames + i) / kRate));
    tone->frames += chunk.frames;
    chunk.ended = left == 0;
    return chunk;
}

struct Fixture {
    // The tests render the mixer themselves, so the output renders only when a test advances it.
    PrAudioOutput* output = prAudioMemoryCreate(PrAudioFormat{kRate, 1}, PrAudioMemoryPace_Host);
    PrAudioMixer* mixer = prAudioMixerCreate(output);
    Tone tone;
    PrAudioSource* source = nullptr;

    Fixture() {
        source = prAudioSourceCreate(mixer, readTone, &tone);
        prAudioSourceSetPlaying(source, true);
    }

    ~Fixture() {
        prAudioSourceDestroy(source);
        prAudioMixerDestroy(mixer);
        prAudioOutputDestroy(output);
    }

    Array<float> render(uint32_t frames) {
        Array<float> samples(frames);
        prAudioMixerRender(mixer, samples.data(), frames);
        return samples;
    }
};

double rootMeanSquare(const Array<float>& samples, uint32_t from = 0) {
    double sum = 0;
    for (uint32_t i = from; i < samples.count(); ++i)
        sum += double(samples[i]) * double(samples[i]);
    return prF64Sqrt(sum / double(samples.count() - from));
}

// The frequency of the samples, by their crossings of zero.
double frequencyOf(const Array<float>& samples) {
    uint32_t crossings = 0;
    for (uint32_t i = 1; i < samples.count(); ++i)
        crossings += (samples[i - 1] < 0) != (samples[i] < 0) ? 1 : 0;
    return double(crossings) / 2 / (double(samples.count()) / kRate);
}

// The largest change from one sample to the next.
float largestStep(const Array<float>& samples) {
    float largest = 0;
    for (uint32_t i = 1; i < samples.count(); ++i) {
        float step = samples[i] > samples[i - 1] ? samples[i] - samples[i - 1] : samples[i - 1] - samples[i];
        largest = step > largest ? step : largest;
    }
    return largest;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a source plays once it starts, at its gain" * doctest::skip(!prThreadSupported())) {
    Fixture f;
    Array<float> samples = f.render(kRate / 4);
    CHECK(rootMeanSquare(samples, 1024) == doctest::Approx(0.707).epsilon(0.01));
    CHECK(frequencyOf(samples) == doctest::Approx(1000).epsilon(0.01));
    prAudioSourceSetGain(f.source, 0.5f);
    f.render(1024);
    CHECK(rootMeanSquare(f.render(kRate / 4)) == doctest::Approx(0.354).epsilon(0.01));
}

TEST_CASE("a source that stops is silent and keeps its place" * doctest::skip(!prThreadSupported())) {
    Fixture f;
    f.render(kRate / 4);
    prAudioSourceSetPlaying(f.source, false);
    f.render(1024);
    uint64_t place = f.tone.frames;
    CHECK(rootMeanSquare(f.render(kRate / 4)) == 0);
    CHECK(f.tone.frames == place);
    prAudioSourceSetPlaying(f.source, true);
    f.render(kRate / 4);
    CHECK(f.tone.frames == place + kRate / 4);
}

TEST_CASE("starting, stopping and running out do not click" * doctest::skip(!prThreadSupported())) {
    Fixture f;
    // A 1 kHz sine at 48 kHz steps by at most 0.131 from one sample to the next.
    constexpr float kLargestSineStep = 0.14f;
    CHECK(largestStep(f.render(kRate / 4)) < kLargestSineStep);
    prAudioSourceSetPlaying(f.source, false);
    CHECK(largestStep(f.render(kRate / 4)) < kLargestSineStep);
    prAudioSourceSetPlaying(f.source, true);
    CHECK(largestStep(f.render(kRate / 4)) < kLargestSineStep);
    f.tone.limit = f.tone.frames + 700;
    CHECK(largestStep(f.render(kRate / 4)) < kLargestSineStep);
}

TEST_CASE("sources add" * doctest::skip(!prThreadSupported())) {
    Fixture f;
    Tone second{.frequency = 3000};
    PrAudioSource* other = prAudioSourceCreate(f.mixer, readTone, &second);
    prAudioSourceSetPlaying(other, true);
    prAudioSourceSetGain(f.source, 0.25f);
    prAudioSourceSetGain(other, 0.25f);
    Array<float> samples = f.render(kRate / 4);
    // Two sines of different frequencies have twice the power of one, which is 0.25 at these gains.
    CHECK(rootMeanSquare(samples, 1024) == doctest::Approx(0.25).epsilon(0.02));
    prAudioSourceDestroy(other);
}

TEST_CASE("a rate that preserves pitch changes speed and not frequency" * doctest::skip(!prThreadSupported())) {
    Fixture f;
    prAudioSourceSetRate(f.source, 1.5, true);
    Array<float> samples = f.render(kRate);
    CHECK(prAudioSourceClock(f.source).position == doctest::Approx(1.5).epsilon(0.02));
    CHECK(frequencyOf(samples) == doctest::Approx(1000).epsilon(0.02));
    CHECK(rootMeanSquare(samples, 4096) == doctest::Approx(0.707).epsilon(0.05));
}

TEST_CASE("a slow rate that preserves pitch changes speed and not frequency" * doctest::skip(!prThreadSupported())) {
    Fixture f;
    prAudioSourceSetRate(f.source, 0.5, true);
    Array<float> samples = f.render(kRate);
    CHECK(prAudioSourceClock(f.source).position == doctest::Approx(0.5).epsilon(0.02));
    CHECK(frequencyOf(samples) == doctest::Approx(1000).epsilon(0.02));
}

TEST_CASE("a rate that does not preserve pitch changes speed and frequency" * doctest::skip(!prThreadSupported())) {
    Fixture f;
    prAudioSourceSetRate(f.source, 2, false);
    Array<float> samples = f.render(kRate);
    CHECK(prAudioSourceClock(f.source).position == doctest::Approx(2).epsilon(0.02));
    CHECK(frequencyOf(samples) == doctest::Approx(2000).epsilon(0.02));
}

TEST_CASE("the clock follows what plays, and a reset invalidates it" * doctest::skip(!prThreadSupported())) {
    Fixture f;
    CHECK(!prAudioSourceClock(f.source).valid);
    f.render(kRate / 2);
    PrAudioClock clock = prAudioSourceClock(f.source);
    CHECK(clock.valid);
    CHECK(!clock.ended);
    CHECK(clock.position == doctest::Approx(0.5).epsilon(0.02));
    prAudioSourceReset(f.source);
    CHECK(!prAudioSourceClock(f.source).valid);
}

TEST_CASE("the clock reports the end of a source" * doctest::skip(!prThreadSupported())) {
    Fixture f;
    f.tone.limit = 1000;
    f.render(4096);
    CHECK(prAudioSourceClock(f.source).ended);
}

TEST_CASE("a source's clock is stamped on the output's clock, when its block is heard") {
    Fixture f;
    prAudioMemoryAdvance(f.output, 500);
    PrAudioClock clock = prAudioSourceClock(f.source);
    CHECK(clock.stamp < prAudioMixerNow(f.mixer));
    CHECK(prAudioMixerNow(f.mixer) == doctest::Approx(500));
    // The position advances with the output's clock from the moment that it was heard.
    CHECK(clock.position + (prAudioMixerNow(f.mixer) - clock.stamp) / 1000 == doctest::Approx(0.5).epsilon(1e-9));
}
