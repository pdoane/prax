#include "pr/media.h"

#include "tools/harness/test.h"

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
