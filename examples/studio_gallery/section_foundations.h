// The gallery's foundations: the principles, color, type, space and icons that every widget is drawn from.

#pragma once

#include "pr/kit.h"

#include "examples/studio_gallery/gallery_state.h"

PR_ASSUME_NONNULL_BEGIN

namespace gallery {

// Create the sections under `parent`, each bound to the model's part that it shows.
void sectionPrinciples(PrKit* kit, PrLayoutNode parent);
void sectionColor(PrKit* kit, PrLayoutNode parent, GalleryState& state);
void sectionType(PrKit* kit, PrLayoutNode parent);
void sectionSpace(PrKit* kit, PrLayoutNode parent, GalleryState& state);
void sectionIcons(PrKit* kit, PrLayoutNode parent);

} // namespace gallery

PR_ASSUME_NONNULL_END
