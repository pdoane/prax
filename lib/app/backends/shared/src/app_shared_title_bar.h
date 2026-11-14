// The parts of a custom title bar, which a backend keeps from each frame's output and hit-tests against.

#pragma once

#include "pr/app.h"
#include "pr/base.h"
#include "pr/raw_array.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// The title bar regions of the window's last frame, in its logical px.
struct AppTitleBarRegions {
    RawArray<PrAppTitleBarRegion> regions;
};

// Replaces the regions with those of `output`.
void appTitleBarRegionsSet(AppTitleBarRegions& regions, const PrAppOutput& output);

// Frees the regions.
void appTitleBarRegionsReset(AppTitleBarRegions& regions);

// Returns the kind of the last region that holds `point`, or Client if none holds it.
PrAppTitleBarRegionKind appTitleBarRegionAt(const AppTitleBarRegions& regions, PrVec2 point);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
