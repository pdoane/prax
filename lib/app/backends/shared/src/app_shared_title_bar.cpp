// The parts of a custom title bar.

#include "app/backends/shared/src/app_shared_title_bar.h"

namespace pr_impl {

void appTitleBarRegionsSet(AppTitleBarRegions& regions, const PrAppOutput& output) {
    regions.regions.clear();
    for (uint32_t i = 0; i < output.titleBarRegionCount; ++i)
        regions.regions.push(output.titleBarRegions[i]);
}

void appTitleBarRegionsReset(AppTitleBarRegions& regions) {
    regions.regions.reset();
}

PrAppTitleBarRegionKind appTitleBarRegionAt(const AppTitleBarRegions& regions, PrVec2 point) {
    for (uint32_t i = regions.regions.count; i > 0; --i) {
        const PrRect& rect = regions.regions.data[i - 1].rect;
        if (point.x >= rect.left && point.x < rect.right && point.y >= rect.top && point.y < rect.bottom)
            return regions.regions.data[i - 1].kind;
    }
    return PrAppTitleBarRegionKind_Client;
}

} // namespace pr_impl
