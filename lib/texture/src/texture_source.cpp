// The state of a texture source: the references that a watcher takes, and the watch.

#include "texture/src/texture_source.h"

namespace pr_impl {

bool textureSourceRetainLive(const PrTextureSource* source) {
    uint32_t held = source->references.load();
    while (held != 0) {
        if (source->references.cas(held, held + 1))
            return true;
    }
    return false;
}

void textureSourceWatch(const PrTextureSource* source, TextureSourceReleasedFn released, void* PR_NULLABLE context,
        uint32_t hash) {
    PrTextureSource* watched = const_cast<PrTextureSource*>(source);
    watched->released = released;
    watched->releasedContext = context;
    watched->watchHash = hash;
}

} // namespace pr_impl
