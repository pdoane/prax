// The displays that a backend has reported.

#include "app/backends/shared/src/app_shared_displays.h"

#include "pr/alloc.h"

namespace pr_impl {

bool appDisplayListFind(const AppDisplayList& list, PrAppDisplayId id, PrStringView* out) {
    for (uint32_t i = 0; i < list.displays.count; ++i) {
        if (list.displays.data[i]->id == id) {
            *out = prRawStringView(&list.displays.data[i]->name);
            return true;
        }
    }
    return false;
}

PrStringView appDisplayListAdd(AppDisplayList& list, PrAppDisplayId id, PrStringView name) {
    PrStringView kept{};
    if (appDisplayListFind(list, id, &kept))
        return kept;
    AppKnownDisplay* display = allocZeroed<AppKnownDisplay>();
    display->id = id;
    display->name = prRawStringCopy(name);
    list.displays.push(display);
    return prRawStringView(&display->name);
}

void appDisplayListReset(AppDisplayList& list) {
    for (uint32_t i = 0; i < list.displays.count; ++i) {
        prRawStringReset(&list.displays.data[i]->name);
        prMemoryFree(list.displays.data[i]);
    }
    list.displays.reset();
}

} // namespace pr_impl
