// Drags: the drag that a window starts, and a window's answer to a drag.

#include "app/backends/shared/src/app_shared_drag.h"

namespace pr_impl {

void appDragPendingSet(AppDragPending& pending, const PrAppDragDesc& desc) {
    pending.pending = true;
    appDataStoreSet(pending.content, desc.content);
    appStringsClear(pending.promises);
    for (uint32_t i = 0; i < desc.promiseCount; ++i)
        appStringsAdd(pending.promises, desc.promises[i].name);
    pending.operations = desc.operations;
    pending.preview.clear();
    pending.previewExtent = PrUVec2{};
    if (desc.preview.data != nullptr) {
        for (uint32_t y = 0; y < desc.preview.extent.y; ++y)
            pending.preview.append(desc.preview.data + size_t(y) * desc.preview.stride, desc.preview.extent.x * 4);
        pending.previewExtent = desc.preview.extent;
    }
    pending.previewOffset = desc.previewOffset;
}

void appDragPendingReset(AppDragPending& pending) {
    appDataStoreReset(pending.content);
    appStringsReset(pending.promises);
    pending.preview.reset();
    pending = AppDragPending{};
}

PrAppDragOperation appDragAnswer(PrAppDragOperation wanted, PrAppDragOperationFlags allowed) {
    if (wanted == PrAppDragOperation_None || (allowed & (1u << wanted)) == 0)
        return PrAppDragOperation_None;
    return wanted;
}

} // namespace pr_impl
