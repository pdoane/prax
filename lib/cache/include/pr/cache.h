// Caches: values that a thread asks for by key each time it builds what it shows, such as a window's images for each
// frame, and that load on demand.
//
// A cache has one owner thread, which makes every call except prCacheLoadFinish and prCachePost. The first ask for a
// key starts a load, and the ask returns null until the load finishes. A loader may finish a load from any thread. The
// owner's pump adopts the finished loads and evicts the values that recent pumps have not asked for.

#pragma once

#include "pr/base.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// A cache of values by key.
typedef struct PrCache PrCache;

// One load of a key's value, which its loader finishes once.
typedef struct PrCacheLoad PrCacheLoad;

// Starts loading the value at `key`, and returns the loader's state for the load, which `cancel` receives. The loader
// finishes the load with prCacheLoadFinish, during the call or later. Called on the owner's thread. The loader calls no
// other function of the cache during the call.
typedef void* PR_NULLABLE (*PrCacheLoadFn)(void* PR_NULLABLE context, PrStringView key, PrCacheLoad* load);

// Asks the loader to stop a load that has not finished, because the cache no longer wants its value. The loader still
// finishes the load, during the call or later. The cache does not cancel a load that has finished, so a loader may free
// its state for a load once it finishes the load on the owner's thread. Called on the owner's thread. The loader calls
// no function of the cache during the call except prCacheLoadFinish.
typedef void (*PrCacheCancelFn)(void* PR_NULLABLE context, void* PR_NULLABLE state);

// Releases a value that a load finished with. Called on the owner's thread.
typedef void (*PrCacheReleaseFn)(void* PR_NULLABLE context, const void* value);

// Asks the owner to pump the cache, after a load finishes or a task is posted. Called on any thread.
typedef void (*PrCacheWakeFn)(void* PR_NULLABLE context);

// A task that runs on the owner's thread (prCachePost).
typedef void (*PrCacheTaskFn)(void* PR_NULLABLE context);

// How a cache loads, releases and evicts its values. The callbacks receive `context`.
typedef struct PrCacheDesc {
    PrCacheLoadFn load;
    PrCacheCancelFn PR_NULLABLE cancel;     // null if a load cannot stop early
    PrCacheReleaseFn release;
    PrCacheWakeFn PR_NULLABLE wake;
    void* PR_NULLABLE context;

    // The number of pumps in a row that may pass without an ask for a key before the pump evicts its value. Zero keeps
    // each value until its key is removed.
    uint32_t idlePumps;
} PrCacheDesc;

// What a cache holds for a key.
enum {
    PrCacheState_None,          // nothing: the next ask starts a load
    PrCacheState_Loading,
    PrCacheState_Ready,         // a value
    PrCacheState_Failed,        // a load that finished without a value, until the key is evicted or removed
};
typedef uint8_t PrCacheState;

// Creates an empty cache. The calling thread is its owner.
PrCache* prCacheCreate(const PrCacheDesc* desc);

// Cancels the loads that have not finished, waits for them to finish, runs the posted tasks, releases every value and
// frees the cache.
void prCacheDestroy(PrCache* PR_NULLABLE cache);

// Returns the value at `key`, or null while it loads or if its load failed. The first ask for a key starts its load.
// The value is valid until the next pump or the removal of its key.
const void* PR_NULLABLE prCacheValue(PrCache* cache, PrStringView key);

// Returns what the cache holds for `key`. Unlike prCacheValue, this is not an ask, so it neither loads nor keeps a
// value.
PrCacheState prCacheKeyState(const PrCache* cache, PrStringView key);

// Removes the key's value, or cancels its load, so that the next ask loads it again.
void prCacheRemove(PrCache* cache, PrStringView key);

// Runs the tasks posted before the call, adopts the loads that finished, and evicts the idle values. Returns true if a
// load finished, which changes what an ask returns.
bool prCachePump(PrCache* cache);

// Posts `fn` to run with `context` on the owner's thread, in the order of posting, at the next pump. Any thread may
// post, until the cache is destroyed.
void prCachePost(PrCache* cache, PrCacheTaskFn fn, void* PR_NULLABLE context);

// Finishes the load with `value`, which the cache takes, or with null if the load failed. The next pump adopts the
// value, or releases it if the load was cancelled. Any thread may finish a load, and the load is freed at the next
// pump.
void prCacheLoadFinish(PrCacheLoad* load, const void* PR_NULLABLE value);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
