// The gallery's feedback: progress, tooltips, dialogs, the status bar and console, images, and duplicate keys.

#pragma once

#include "pr/kit.h"

#include "examples/studio_gallery/gallery_state.h"

PR_ASSUME_NONNULL_BEGIN

namespace gallery {

// Create the sections under `parent`, each bound to the model's part that it shows.
void sectionProgress(PrKit* kit, PrLayoutNode parent, GalleryState& state);
void sectionTooltips(PrKit* kit, PrLayoutNode parent);
void sectionDialogs(PrKit* kit, PrLayoutNode parent, GalleryState& state);
void sectionStatus(PrKit* kit, PrLayoutNode parent);
void sectionImages(PrKit* kit, PrLayoutNode parent, GalleryState& state);
void sectionKeys(PrKit* kit, PrLayoutNode parent, GalleryState& state);

} // namespace gallery

PR_ASSUME_NONNULL_END
