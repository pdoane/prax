#include "pr/array.h"
#include "pr/atomic.h"
#include "pr/cache.h"
#include "pr/span.h"
#include "pr/string.h"
#include "pr/task.h"

#include "tools/harness/test.h"

extern "C" int prTestCacheCApi(void);

namespace pr_impl {

namespace {

using namespace pr;

// A loader that keeps its loads until the test finishes them, and records the cache's calls.
struct Loader {
    Array<PrCacheLoad*> loads;
    Array<String> keys;
    Array<String> released;
    uint32_t cancels = 0;
    Atomic<uint32_t> wakes{};
    bool finishOnCancel = true;
};

void* loadValue(void* PR_NULLABLE context, PrStringView key, PrCacheLoad* load) {
    Loader& loader = *static_cast<Loader*>(context);
    loader.keys.emplace(key);
    loader.loads.push(load);
    return load;
}

void cancelValue(void* PR_NULLABLE context, void* PR_NULLABLE state) {
    Loader& loader = *static_cast<Loader*>(context);
    ++loader.cancels;
    if (loader.finishOnCancel)
        prCacheLoadFinish(static_cast<PrCacheLoad*>(state), nullptr);
}

void releaseValue(void* PR_NULLABLE context, const void* value) {
    Loader& loader = *static_cast<Loader*>(context);
    loader.released.emplace(static_cast<const char*>(value));
}

void wakeOwner(void* PR_NULLABLE context) {
    static_cast<Loader*>(context)->wakes.fetchAdd(1);
}

PrCacheDesc descOf(Loader& loader, uint32_t idlePumps) {
    return PrCacheDesc{
        .load = loadValue,
        .cancel = cancelValue,
        .release = releaseValue,
        .wake = wakeOwner,
        .context = &loader,
        .idlePumps = idlePumps,
    };
}

// The text of the value at `key`, or "(null)". The tests' values are string literals.
PrStringView textAt(PrCache* cache, PrStringView key) {
    const char* value = static_cast<const char*>(prCacheValue(cache, key));
    return value != nullptr ? value : "(null)";
}

// Returns true if `texts` holds `expected`, in order.
bool textsAre(const Array<String>& texts, Span<const char* const> expected) {
    if (texts.count() != expected.count())
        return false;
    for (uint32_t i = 0; i < expected.count(); ++i) {
        if (texts[i] != expected[i])
            return false;
    }
    return true;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("cache: the public headers compile as C") {
    CHECK(prTestCacheCApi() == 0);
}

TEST_CASE("cache: the first ask loads a key, and the pump after the load finishes adopts its value") {
    Loader loader;
    PrCacheDesc desc = descOf(loader, 0);
    PrCache* cache = prCacheCreate(&desc);
    CHECK(textAt(cache, "a") == "(null)");
    CHECK(textAt(cache, "a") == "(null)");
    const char* const keys[] = {"a"};
    REQUIRE(textsAre(loader.keys, keys));
    CHECK(prCacheKeyState(cache, "a") == PrCacheState_Loading);
    CHECK(!prCachePump(cache));
    prCacheLoadFinish(loader.loads[0], "apple");
    CHECK(loader.wakes.load() == 1);
    CHECK(textAt(cache, "a") == "(null)");
    CHECK(prCachePump(cache));
    CHECK(textAt(cache, "a") == "apple");
    CHECK(prCacheKeyState(cache, "a") == PrCacheState_Ready);
    CHECK(prCacheKeyState(cache, "b") == PrCacheState_None);
    CHECK(loader.keys.count() == 1);
    prCacheDestroy(cache);
    const char* const released[] = {"apple"};
    CHECK(textsAre(loader.released, released));
}

TEST_CASE("cache: a load that fails leaves its key failed until it is removed") {
    Loader loader;
    PrCacheDesc desc = descOf(loader, 0);
    PrCache* cache = prCacheCreate(&desc);
    prCacheValue(cache, "a");
    prCacheLoadFinish(loader.loads[0], nullptr);
    prCachePump(cache);
    CHECK(prCacheKeyState(cache, "a") == PrCacheState_Failed);
    CHECK(textAt(cache, "a") == "(null)");
    CHECK(loader.keys.count() == 1);
    prCacheRemove(cache, "a");
    CHECK(prCacheKeyState(cache, "a") == PrCacheState_None);
    prCacheValue(cache, "a");
    CHECK(loader.keys.count() == 2);
    prCacheDestroy(cache);
}

TEST_CASE("cache: a pump evicts the values that more than its idle pumps have passed without an ask for") {
    Loader loader;
    PrCacheDesc desc = descOf(loader, 1);
    PrCache* cache = prCacheCreate(&desc);
    prCacheValue(cache, "a");
    prCacheValue(cache, "b");
    prCacheLoadFinish(loader.loads[0], "apple");
    prCacheLoadFinish(loader.loads[1], "banana");
    prCachePump(cache);
    CHECK(textAt(cache, "a") == "apple");
    prCachePump(cache);
    CHECK(textAt(cache, "a") == "apple");
    prCachePump(cache);
    const char* const released[] = {"banana"};
    CHECK(textsAre(loader.released, released));
    CHECK(prCacheKeyState(cache, "b") == PrCacheState_None);
    CHECK(textAt(cache, "a") == "apple");
    prCacheDestroy(cache);
}

TEST_CASE("cache: removing a key that loads cancels the load, and its late value is released") {
    Loader loader;
    loader.finishOnCancel = false;
    PrCacheDesc desc = descOf(loader, 0);
    PrCache* cache = prCacheCreate(&desc);
    prCacheValue(cache, "a");
    prCacheRemove(cache, "a");
    CHECK(loader.cancels == 1);
    prCacheValue(cache, "a");
    prCacheLoadFinish(loader.loads[0], "stale");
    prCacheLoadFinish(loader.loads[1], "fresh");
    prCachePump(cache);
    const char* const released[] = {"stale"};
    CHECK(textsAre(loader.released, released));
    CHECK(textAt(cache, "a") == "fresh");
    prCacheDestroy(cache);
}

TEST_CASE("cache: posted tasks run at the pump, before it adopts the loads that they finish") {
    Loader loader;
    PrCacheDesc desc = descOf(loader, 0);
    PrCache* cache = prCacheCreate(&desc);
    prCacheValue(cache, "a");
    prCachePost(cache, [](void* PR_NULLABLE context) {
        Loader& loader = *static_cast<Loader*>(context);
        prCacheLoadFinish(loader.loads[0], "apple");
    }, &loader);
    CHECK(loader.wakes.load() == 1);
    CHECK(prCachePump(cache));
    CHECK(textAt(cache, "a") == "apple");
    prCacheDestroy(cache);
}

TEST_CASE("cache: destroying a cache cancels its loads and waits for those that finish on other threads") {
    Loader loader;
    loader.finishOnCancel = false;
    PrTaskPool* pool = prTaskPoolCreate(PrTaskPoolBackend_Default, 2);
    REQUIRE(pool != nullptr);
    PrCacheDesc desc = descOf(loader, 0);
    PrCache* cache = prCacheCreate(&desc);
    const char* const keys[] = {"a", "b", "c"};
    for (const char* key : keys)
        prCacheValue(cache, key);
    for (PrCacheLoad* load : loader.loads) {
        prTaskPoolPost(pool, [](void* PR_NULLABLE context) {
            prCacheLoadFinish(static_cast<PrCacheLoad*>(context), "late");
        }, load);
    }
    prCacheDestroy(cache);
    // A load that the pool finishes before the destroy reaches it is not canceled.
    CHECK(loader.cancels <= 3);
    const char* const released[] = {"late", "late", "late"};
    CHECK(textsAre(loader.released, released));
    prTaskPoolDestroy(pool);
}
