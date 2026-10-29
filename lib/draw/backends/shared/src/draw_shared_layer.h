// The host's layers that groups draw into (PrDrawGroupLayer), as the backends that draw them share them

#pragma once

#include "pr/atomic.h"
#include "pr/draw.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Claims the drawing of the groups inside the layer whose revisions are past what its texture holds, up to the layer's
// revision. Raises the revision that the texture holds (`*layer.drawn`) to the layer's, atomically, and returns the
// revision that it held before. The groups whose revisions are past the result draw. A result at or past the layer's
// revision means that the texture holds them all, or that another renderer claimed them first, and none draw.
inline uint64_t drawSharedClaimLayer(const PrDrawGroupLayer& layer) {
    static_assert(sizeof(Atomic<uint64_t>) == sizeof(uint64_t) && alignof(Atomic<uint64_t>) == alignof(uint64_t));
    Atomic<uint64_t>* drawn = reinterpret_cast<Atomic<uint64_t>*>(layer.drawn);
    uint64_t held = drawn->load();
    while (held < layer.revision && !drawn->cas(held, layer.revision)) {
    }
    return held;
}

} // namespace pr_impl

PR_ASSUME_NONNULL_END
