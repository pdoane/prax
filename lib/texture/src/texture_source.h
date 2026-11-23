// The state of a texture source, for the parts of the library that name a source without holding a reference to it.

#pragma once

#include "pr/atomic.h"
#include "pr/texture.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Called with the watcher's context after the last reference to a source is released and before the source is freed. It
// runs on the thread that releases the reference.
typedef void (*TextureSourceReleasedFn)(void* PR_NULLABLE context, const PrTextureSource* source);

// A source: its reference count and its description. The formats and the previews of the description are stored in the
// source.
struct TextureSource {
    mutable Atomic<uint32_t> references;    // mutable because holders retain and release a const source
    uint64_t id;
    PrTextureSourceDesc desc;
    const PrTextureSource* PR_NULLABLE previews[kPrTextureSourceMaxPreviews];

    // The watcher that is told when the source is released, or null. A catalog is a watcher. It names the source
    // without holding a reference to it. `watchHash` belongs to the watcher and is the hash of the name. These are set
    // once, before the source is shared (textureSourceWatch).
    TextureSourceReleasedFn PR_NULLABLE released;
    void* PR_NULLABLE releasedContext;
    uint32_t watchHash;
};

// Takes a reference to `source` if the source still has a reference. Returns true if it took one. A watcher takes a
// reference in this way for each caller that it hands the source to.
bool textureSourceRetainLive(const PrTextureSource* source);

// Registers `released` to be called with `context` when `source` is released. Stores `hash` for the watcher. Call it
// once, before the source is shared.
void textureSourceWatch(const PrTextureSource* source, TextureSourceReleasedFn released, void* PR_NULLABLE context,
    uint32_t hash);

} // namespace pr_impl

// C opaque types
struct PrTextureSource : pr_impl::TextureSource {};

PR_ASSUME_NONNULL_END
