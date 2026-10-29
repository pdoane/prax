// The gallery's controls: buttons, inputs, toggles, tabs, and the menus and popovers.

#pragma once

#include "pr/kit.h"

#include "examples/studio_gallery/gallery_state.h"

PR_ASSUME_NONNULL_BEGIN

namespace gallery {

// Create the sections under `parent`, each bound to the model's part that it shows.
void sectionButtons(PrKit* kit, PrLayoutNode parent);
void sectionInputs(PrKit* kit, PrLayoutNode parent, GalleryState& state);
void sectionToggles(PrKit* kit, PrLayoutNode parent, GalleryState& state);
void sectionTabs(PrKit* kit, PrLayoutNode parent, GalleryState& state);
void sectionMenus(PrKit* kit, PrLayoutNode parent, GalleryState& state);

} // namespace gallery

PR_ASSUME_NONNULL_END
