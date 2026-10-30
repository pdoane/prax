#include "pr/cache.h"

int prTestCacheCApi(void);

static void* load(void* context, PrStringView key, PrCacheLoad* handle) {
    (void)context;
    (void)key;
    prCacheLoadFinish(handle, NULL);
    return NULL;
}

static void release(void* context, const void* value) {
    (void)context;
    (void)value;
}

// The public headers compiled as C. A load that fails leaves its key failed.
int prTestCacheCApi(void) {
    PrCacheDesc desc = {0};
    desc.load = load;
    desc.release = release;
    PrCache* cache = prCacheCreate(&desc);
    int result = prCacheValue(cache, PR_SV("key")) != NULL;
    prCachePump(cache);
    result |= prCacheKeyState(cache, PR_SV("key")) != PrCacheState_Failed;
    prCacheDestroy(cache);
    prCacheDestroy(NULL);
    return result;
}
