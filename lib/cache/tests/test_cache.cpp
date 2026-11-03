#include "pr/cache.h"
#include "pr/task.h"

#include "doctest/doctest.h"

#include <atomic>
#include <string>
#include <vector>

extern "C" int prTestCacheCApi(void);

namespace pr_impl {

namespace {

// A loader that keeps its loads until the test finishes them, and records the cache's calls.
struct Loader {
    std::vector<PrCacheLoad*> loads;
    std::vector<std::string> keys;
    std::vector<std::string> released;
    uint32_t cancels = 0;
    std::atomic<uint32_t> wakes{0};
    bool finishOnCancel = true;
};

void* loadValue(void* PR_NULLABLE context, PrStringView key, PrCacheLoad* load) {
    Loader& loader = *static_cast<Loader*>(context);
    loader.keys.emplace_back(key.data, key.size);
    loader.loads.push_back(load);
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
    loader.released.push_back(static_cast<const char*>(value));
}

void wakeOwner(void* PR_NULLABLE context) {
    ++static_cast<Loader*>(context)->wakes;
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
std::string textAt(PrCache* cache, PrStringView key) {
    const char* value = static_cast<const char*>(prCacheValue(cache, key));
    return value != nullptr ? value : "(null)";
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
    REQUIRE(loader.keys == std::vector<std::string>{"a"});
    CHECK(prCacheKeyState(cache, "a") == PrCacheState_Loading);
    CHECK(!prCachePump(cache));
    prCacheLoadFinish(loader.loads[0], "apple");
    CHECK(loader.wakes == 1);
    CHECK(textAt(cache, "a") == "(null)");
    CHECK(prCachePump(cache));
    CHECK(textAt(cache, "a") == "apple");
    CHECK(prCacheKeyState(cache, "a") == PrCacheState_Ready);
    CHECK(prCacheKeyState(cache, "b") == PrCacheState_None);
    CHECK(loader.keys.size() == 1);
    prCacheDestroy(cache);
    CHECK(loader.released == std::vector<std::string>{"apple"});
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
    CHECK(loader.keys.size() == 1);
    prCacheRemove(cache, "a");
    CHECK(prCacheKeyState(cache, "a") == PrCacheState_None);
    prCacheValue(cache, "a");
    CHECK(loader.keys.size() == 2);
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
    CHECK(loader.released == std::vector<std::string>{"banana"});
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
    CHECK(loader.released == std::vector<std::string>{"stale"});
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
    CHECK(loader.wakes == 1);
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
    for (const char* key : {"a", "b", "c"})
        prCacheValue(cache, key);
    for (PrCacheLoad* load : loader.loads) {
        prTaskPoolPost(pool, [](void* PR_NULLABLE context) {
            prCacheLoadFinish(static_cast<PrCacheLoad*>(context), "late");
        }, load);
    }
    prCacheDestroy(cache);
    // A load that the pool finishes before the destroy reaches it is not canceled.
    CHECK(loader.cancels <= 3);
    CHECK(loader.released == std::vector<std::string>{"late", "late", "late"});
    prTaskPoolDestroy(pool);
}
