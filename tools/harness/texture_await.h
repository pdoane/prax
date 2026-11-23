// Waiting for the asynchronous work of lib/texture in tests and tools. The libraries never block: a consumer asks,
// draws what there is, and draws again when the work arrives. A test or a capture that needs the finished result drives
// the work here instead. Each helper polls until its work completes, and gives up after a bound, so that a test whose
// work never completes fails instead of hanging.

#pragma once

#include "pr/atomic.h"
#include "pr/print.h"
#include "pr/texture.h"
#include "pr/texture_cache.h"
#include "pr/texture_catalog.h"
#include "pr/thread.h"
#include "pr/time.h"

PR_ASSUME_NONNULL_BEGIN

namespace harness {

using namespace pr;

// The longest that a helper waits, in milliseconds.
constexpr double kAwaitBoundMilliseconds = 30000;

// Calls `done` until it returns true, sleeping between calls. Returns false, and reports `what`, if it is still false
// after the bound.
template <typename Done>
bool awaitUntil(const char* what, Done done) {
    double start = prTimeMonotonicMilliseconds();
    while (!done()) {
        if (prTimeMonotonicMilliseconds() - start > kAwaitBoundMilliseconds) {
            printError("harness: gave up waiting for {}\n", what);
            return false;
        }
        prThreadSleep(0.25);
    }
    return true;
}

// Asks the cache for `source` at High priority until the ask is Ready or Failed, and returns that state. Writes the
// region of a Ready ask. Returns Pending if the bound passes first. A source whose bytes are still arriving is Ready
// only once the test has delivered them.
inline PrTextureCacheState textureCacheAwait(PrTextureCache* cache, const PrTextureSource* source,
        PrTextureCacheAsk ask, PrTextureRegion* out) {
    ask.priority = PrTextureCachePriority_High;
    PrTextureCacheState state = PrTextureCacheState_Pending;
    bool settled = awaitUntil("a texture", [&] {
        state = prTextureCacheRealize(cache, source, ask, out);
        return state == PrTextureCacheState_Ready || state == PrTextureCacheState_Failed;
    });
    return settled ? state : PrTextureCacheState_Pending;
}

// Waits until no realization of the cache is waiting or in flight. Returns false if the bound passes first.
inline bool textureCacheSettle(PrTextureCache* cache) {
    return awaitUntil("the texture cache to settle", [&] {
        PrTextureCacheInfo info = prTextureCacheInfo(cache);
        return info.waiting == 0 && info.inFlight == 0;
    });
}

// Waits until the read completes, and returns its status, or Limit if the bound passes first.
inline PrStatus textureReadAwait(PrTextureRead* read) {
    if (!awaitUntil("a texture read", [&] { return prTextureReadIsDone(read); }))
        return PrStatus_Limit;
    return prTextureReadStatus(read);
}

// What a callback of an open or an upload completes.
struct TextureAwaited {
    Atomic<bool> done;
    PrStatus status = PrStatus_Ok;
    const PrTextureSource* PR_NULLABLE source = nullptr;
    PrTexture texture{};
};

// Opens `bytes` as prTextureFormatSetOpen does and waits for the source. Returns the open's status, or Limit if the
// bound passes first. On failure, `*out` is null.
inline PrStatus textureOpenAwait(const PrTextureFormatSet* set, const PrBlobSource* bytes, PrStringView name,
        const PrTextureLimits* PR_NULLABLE limits, const PrTextureSource* PR_NULLABLE* PR_NONNULL out) {
    // The awaited state outlives a give-up, because the open may still complete.
    TextureAwaited* awaited = new TextureAwaited{};
    awaited->done.store(false);
    prTextureFormatSetOpen(set, bytes, name, limits,
        [](void* PR_NULLABLE context, PrStatus status, const PrTextureSource* PR_NULLABLE source) {
            TextureAwaited& a = *static_cast<TextureAwaited*>(context);
            a.status = status;
            a.source = source;
            a.done.store(true);
        }, awaited);
    *out = nullptr;
    if (!awaitUntil("a texture open", [&] { return awaited->done.load(); }))
        return PrStatus_Limit;
    *out = awaited->source;
    PrStatus status = awaited->status;
    delete awaited;
    return status;
}

// Uploads `source` as prTextureUpload does and waits for the texture. Returns the upload's status, or Limit if the
// bound passes first. On failure, `*out` is zero.
inline PrStatus textureUploadAwait(PrGpuDevice* device, const PrTextureSource* source, PrTextureUploadFlags flags,
        PrStringView name, PrTexture* out) {
    TextureAwaited* awaited = new TextureAwaited{};
    awaited->done.store(false);
    prTextureUpload(device, source, flags, name, [](void* PR_NULLABLE context, PrStatus status, PrTexture texture) {
        TextureAwaited& a = *static_cast<TextureAwaited*>(context);
        a.status = status;
        a.texture = texture;
        a.done.store(true);
    }, awaited);
    *out = PrTexture{};
    if (!awaitUntil("a texture upload", [&] { return awaited->done.load(); }))
        return PrStatus_Limit;
    *out = awaited->texture;
    PrStatus status = awaited->status;
    delete awaited;
    return status;
}

// Asks the catalog for the file at `path` until it is not Pending, and returns its state. Writes the source of a Ready
// name, with a reference for the caller. Returns Pending if the bound passes first.
inline PrTextureCatalogState textureCatalogAwait(PrTextureCatalog* catalog, PrStringView path,
        const PrTextureSource* PR_NULLABLE* PR_NONNULL out) {
    PrTextureCatalogState state = PrTextureCatalogState_Pending;
    awaitUntil("a catalog open", [&] {
        state = prTextureCatalogSource(catalog, path, out);
        return state != PrTextureCatalogState_Pending;
    });
    return state;
}

// Waits until the catalog has no open in flight. Returns false if the bound passes first.
inline bool textureCatalogSettle(PrTextureCatalog* catalog) {
    return awaitUntil("the texture catalog to settle", [&] { return prTextureCatalogOpening(catalog) == 0; });
}

} // namespace harness

PR_ASSUME_NONNULL_END
