// Host textures: pictures that a host imports to the device and draws through draw lists.

#include "pr/draw_host_texture.h"

#include "pr/alloc.h"
#include "pr/atomic.h"

namespace pr_impl {

using namespace pr;

struct DrawHostTexture {
    Atomic<uint32_t> references;
    PrGpuDevice* device;
    PrGpuTexture texture;
    PrTextureAlpha alpha;
};

} // namespace pr_impl

using namespace pr_impl;

// C opaque types
struct PrDrawHostTexture : DrawHostTexture {};

PrStatus prDrawHostTextureImport(PrGpuDevice* device, const PrGpuImport* image, PrTextureAlpha alpha,
        PrDrawHostTexture* PR_NULLABLE* PR_NONNULL texture) {
    PrDrawHostTexture* previous = *texture;
    // Only the caller holds the earlier texture, so no list draws it and the import may replace what it shows.
    if (previous != nullptr && previous->device == device && previous->references.load() == 1) {
        PrStatus status = prGpuTextureImport(device, image, nullptr, nullptr, &previous->texture);
        if (status == PrStatus_Ok)
            previous->alpha = alpha;
        return status;
    }
    PrGpuTexture made{};
    PrStatus status = prGpuTextureImport(device, image, nullptr, nullptr, &made);
    if (status != PrStatus_Ok)
        return status;
    PrDrawHostTexture* next = allocZeroed<PrDrawHostTexture>();
    next->references.storeRelaxed(1);
    next->device = device;
    next->texture = made;
    next->alpha = alpha;
    prDrawHostTextureRelease(previous);
    *texture = next;
    return PrStatus_Ok;
}

void prDrawHostTextureRetain(PrDrawHostTexture* texture) {
    texture->references.fetchAdd(1);
}

void prDrawHostTextureRelease(PrDrawHostTexture* PR_NULLABLE texture) {
    if (texture == nullptr || texture->references.fetchSub(1) != 1)
        return;
    prGpuTextureRelease(texture->device, texture->texture);
    prMemoryFree(texture);
}

PrUVec2 prDrawHostTextureExtent(const PrDrawHostTexture* texture) {
    return prGpuTextureExtent(texture->device, texture->texture);
}

PrGpuDevice* prDrawHostTextureDevice(const PrDrawHostTexture* texture) {
    return texture->device;
}

PrDrawSource prDrawSourceHostTexture(PrDrawList* list, PrDrawHostTexture* texture) {
    prDrawHostTextureRetain(texture);
    prDrawListHold(list, [](void* PR_NULLABLE held) {
        prDrawHostTextureRelease(static_cast<PrDrawHostTexture*>(held));
    }, texture);
    PrUVec2 extent = prDrawHostTextureExtent(texture);
    return prDrawSourceTexture(PrTexture{.texture = texture->texture, .alpha = texture->alpha}, extent,
        PrRect{0, 0, float(extent.x), float(extent.y)});
}
