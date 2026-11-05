#include "pr/media.h"
#include "pr/media_audio.h"
#include "pr/thread.h"

#include "tools/harness/test.h"

#include <vector>

extern "C" int prTestMediaCApi(void);

TEST_CASE("the public header compiles as C") {
    CHECK(prTestMediaCApi() == 0);
}

TEST_CASE("a backend is named as a command line spells it, and Default is built if any backend is") {
    for (PrMediaCodecBackend backend = 0; backend < PrMediaCodecBackend_Count; ++backend)
        CHECK(prMediaCodecBackendFromName(prMediaCodecBackendName(backend)) == backend);
    CHECK(prMediaCodecBackendFromName("BUILTIN") == PrMediaCodecBackend_Builtin);
    CHECK(prMediaCodecBackendFromName("none") == PrMediaCodecBackend_Count);
    bool any = prMediaCodecBackendIsBuilt(PrMediaCodecBackend_Apple) ||
        prMediaCodecBackendIsBuilt(PrMediaCodecBackend_Windows) ||
        prMediaCodecBackendIsBuilt(PrMediaCodecBackend_Builtin);
    CHECK(prMediaCodecBackendIsBuilt(PrMediaCodecBackend_Default) == any);
    PrMediaCodec* codec = prMediaCodecCreate(PrMediaCodecBackend_Default);
    CHECK((codec != nullptr) == any);
    prMediaCodecDestroy(codec);
}

TEST_CASE("a reader that waits for an audio queue's writer reads every frame that it asks for, or else the end" *
        doctest::skip(!prThreadSupported())) {
    PrMediaAudioQueue* queue = prMediaAudioQueueCreate(48000, 1, 0);
    // The writer is slower than the reader, which waits for it.
    PrThread writer;
    prThreadStart(&writer, [](void* context) {
        PrMediaAudioQueue* q = static_cast<PrMediaAudioQueue*>(context);
        std::vector<float> samples(1000, 0.5f);
        prThreadSleep(20);
        prMediaAudioQueueBegin(q, 1, 0);
        prMediaAudioQueueWrite(q, 1, samples.data(), 1000);
        prThreadSleep(20);
        prMediaAudioQueueWrite(q, 1, samples.data(), 1000);
        prMediaAudioQueueFinish(q, 1);
    }, queue);
    std::vector<float> read(1500);
    prMediaAudioQueueWait(queue, 1500);
    CHECK(prMediaAudioQueueRead(queue, read.data(), 1500).frames == 1500);
    prMediaAudioQueueWait(queue, 1500);
    PrMediaAudioChunk last = prMediaAudioQueueRead(queue, read.data(), 1500);
    CHECK(last.frames == 500);
    CHECK(last.time == doctest::Approx(1500.0 / 48000));
    prMediaAudioQueueWait(queue, 1500);
    CHECK(prMediaAudioQueueRead(queue, read.data(), 1500).ended);
    prMediaAudioQueueStop(queue);
    prThreadJoin(&writer);
    prMediaAudioQueueDestroy(queue);
}
