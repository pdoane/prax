// Caches: values that a thread asks for by key, and that load on demand.

#include "pr/cache.h"

#include "pr/alloc.h"
#include "pr/array.h"
#include "pr/assert.h"
#include "pr/hash.h"
#include "pr/hash_index.h"
#include "pr/memory.h"
#include "pr/mutex.h"
#include "pr/string.h"
#include "pr/thread.h"

namespace pr_impl {

namespace {

struct Cache;

// A load, which the owner frees once a pump has taken it from the finished loads. `cancelled` is the owner's. The
// loader writes `value` and sets `finished` under the cache's mutex, as it hands the load to the cache.
struct CacheLoad {
    Cache* cache;
    PrString key;
    const void* PR_NULLABLE value;
    bool cancelled;
    bool finished;
};

// What the cache holds for a key. `load` is the load that has not finished, and `asked` is the number of the last pump
// that an ask came before.
struct CacheEntry {
    PrString key;
    PrCacheState state;
    const void* PR_NULLABLE value;
    CacheLoad* PR_NULLABLE load;
    void* PR_NULLABLE loaderState;
    uint64_t asked;
};

struct CacheTask {
    PrCacheTaskFn fn;
    void* PR_NULLABLE context;
};

// The owner's entries, by key, and what other threads hand it under `mutex`: the finished loads, the posted tasks, and
// the count of loads that have not finished, which `idle` signals the end of.
struct Cache {
    PrCacheDesc desc;
    Array<CacheEntry> entries;
    HashIndex hashes;
    uint64_t pumps;

    PrMutex mutex;
    PrCondition idle;
    Array<CacheLoad*> finished;
    Array<CacheTask> tasks;
    uint32_t loading;
};

uint32_t hashOf(PrStringView key) {
    return uint32_t(prHashBytes(key.data, key.size, 0));
}

uint32_t findEntry(const Cache* cache, PrStringView key) {
    return cache->hashes.find(hashOf(key), [&](uint32_t at) { return prStringView(&cache->entries.data[at].key) == key; });
}

// Removes the entry at `at`. The last entry takes its position.
void eraseEntry(Cache* cache, uint32_t at) {
    CacheEntry& entry = cache->entries.data[at];
    cache->hashes.erase(hashOf(prStringView(&entry.key)), at);
    prStringReset(&entry.key);
    uint32_t last = cache->entries.count - 1;
    if (at != last) {
        CacheEntry& moved = cache->entries.data[last];
        cache->hashes.relocate(hashOf(prStringView(&moved.key)), last, at);
        entry = moved;
    }
    cache->entries.pop();
}

// Releases what the entry holds, and cancels its load unless the load has finished, since a loader may free its state
// for the load once it finishes.
void dropEntry(Cache* cache, CacheEntry& entry) {
    if (entry.value != nullptr)
        cache->desc.release(cache->desc.context, entry.value);
    entry.value = nullptr;
    CacheLoad* load = entry.load;
    if (load == nullptr)
        return;
    load->cancelled = true;
    entry.load = nullptr;
    prMutexLock(&cache->mutex);
    bool finished = load->finished;
    prMutexUnlock(&cache->mutex);
    if (!finished && cache->desc.cancel != nullptr)
        cache->desc.cancel(cache->desc.context, entry.loaderState);
}

void freeLoad(CacheLoad* load) {
    prStringReset(&load->key);
    prMemoryFree(load);
}

// Runs the tasks posted so far, then adopts the loads that finished, which those tasks may have finished. A cancelled
// load's value is released. Returns the number of loads adopted or released.
uint32_t drain(Cache* cache) {
    prMutexLock(&cache->mutex);
    Array<CacheTask> tasks = cache->tasks;
    cache->tasks = Array<CacheTask>{};
    prMutexUnlock(&cache->mutex);
    for (uint32_t i = 0; i < tasks.count; ++i)
        tasks.data[i].fn(tasks.data[i].context);
    tasks.reset();
    prMutexLock(&cache->mutex);
    Array<CacheLoad*> finished = cache->finished;
    cache->finished = Array<CacheLoad*>{};
    prMutexUnlock(&cache->mutex);
    for (uint32_t i = 0; i < finished.count; ++i) {
        CacheLoad* load = finished.data[i];
        if (load->cancelled) {
            if (load->value != nullptr)
                cache->desc.release(cache->desc.context, load->value);
        } else {
            uint32_t at = findEntry(cache, prStringView(&load->key));
            PR_ASSERT(at != kHashMissing && cache->entries.data[at].load == load);
            CacheEntry& entry = cache->entries.data[at];
            entry.value = load->value;
            entry.state = load->value != nullptr ? PrCacheState_Ready : PrCacheState_Failed;
            entry.load = nullptr;
            entry.loaderState = nullptr;
        }
        freeLoad(load);
    }
    uint32_t count = finished.count;
    finished.reset();
    return count;
}

// Hands a finished load or a task to the owner under the mutex, then wakes the owner. The wake is read under the mutex,
// since the owner may free the cache as soon as the mutex is released.
template <typename Hand>
void handToOwner(Cache* cache, Hand hand) {
    prMutexLock(&cache->mutex);
    hand();
    PrCacheWakeFn wake = cache->desc.wake;
    void* wakeContext = cache->desc.context;
    prMutexUnlock(&cache->mutex);
    if (wake != nullptr)
        wake(wakeContext);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

// C opaque types
struct PrCacheLoad : CacheLoad {};
struct PrCache : Cache {};

PrCache* prCacheCreate(const PrCacheDesc* desc) {
    PR_ASSERT(desc->load != nullptr && desc->release != nullptr);
    PrCache* cache = allocZeroed<PrCache>();
    cache->desc = *desc;
    prMutexInit(&cache->mutex);
    prConditionInit(&cache->idle);
    return cache;
}

void prCacheDestroy(PrCache* PR_NULLABLE cache) {
    if (cache == nullptr)
        return;
    for (uint32_t i = 0; i < cache->entries.count; ++i)
        dropEntry(cache, cache->entries.data[i]);
    prMutexLock(&cache->mutex);
    while (cache->loading != 0)
        prConditionWait(&cache->idle, &cache->mutex);
    prMutexUnlock(&cache->mutex);
    // A task may post another, or finish a load, which the next round takes.
    while (true) {
        prMutexLock(&cache->mutex);
        bool empty = cache->tasks.count == 0 && cache->finished.count == 0;
        prMutexUnlock(&cache->mutex);
        if (empty)
            break;
        drain(cache);
    }
    for (uint32_t i = 0; i < cache->entries.count; ++i)
        prStringReset(&cache->entries.data[i].key);
    cache->entries.reset();
    cache->hashes.reset();
    cache->finished.reset();
    cache->tasks.reset();
    prConditionReset(&cache->idle);
    prMutexReset(&cache->mutex);
    prMemoryFree(cache);
}

const void* PR_NULLABLE prCacheValue(PrCache* cache, PrStringView key) {
    uint32_t at = findEntry(cache, key);
    if (at != kHashMissing) {
        CacheEntry& entry = cache->entries.data[at];
        entry.asked = cache->pumps;
        return entry.value;
    }
    PrCacheLoad* load = allocZeroed<PrCacheLoad>();
    load->cache = cache;
    load->key = prStringCopy(key);
    at = cache->entries.count;
    cache->entries.push(CacheEntry{
        .key = prStringCopy(key),
        .state = PrCacheState_Loading,
        .load = load,
        .asked = cache->pumps,
    });
    cache->hashes.insert(hashOf(key), at);
    prMutexLock(&cache->mutex);
    ++cache->loading;
    prMutexUnlock(&cache->mutex);
    void* state = cache->desc.load(cache->desc.context, key, load);
    // A load that finished during the call still waits for the pump, so its entry still names it.
    cache->entries.data[at].loaderState = state;
    return nullptr;
}

PrCacheState prCacheKeyState(const PrCache* cache, PrStringView key) {
    uint32_t at = findEntry(cache, key);
    return at != kHashMissing ? cache->entries.data[at].state : PrCacheState_None;
}

void prCacheRemove(PrCache* cache, PrStringView key) {
    uint32_t at = findEntry(cache, key);
    if (at == kHashMissing)
        return;
    dropEntry(cache, cache->entries.data[at]);
    eraseEntry(cache, at);
}

bool prCachePump(PrCache* cache) {
    bool landed = drain(cache) != 0;
    uint32_t idlePumps = cache->desc.idlePumps;
    for (uint32_t at = cache->entries.count; idlePumps != 0 && at-- > 0;) {
        CacheEntry& entry = cache->entries.data[at];
        if (entry.state != PrCacheState_Loading && cache->pumps - entry.asked > idlePumps) {
            dropEntry(cache, entry);
            eraseEntry(cache, at);
        }
    }
    ++cache->pumps;
    return landed;
}

void prCachePost(PrCache* cache, PrCacheTaskFn fn, void* PR_NULLABLE context) {
    handToOwner(cache, [&] { cache->tasks.push(CacheTask{fn, context}); });
}

void prCacheLoadFinish(PrCacheLoad* load, const void* PR_NULLABLE value) {
    Cache* cache = load->cache;
    handToOwner(cache, [&] {
        load->value = value;
        load->finished = true;
        cache->finished.push(load);
        if (--cache->loading == 0)
            prConditionBroadcast(&cache->idle);
    });
}
