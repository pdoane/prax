// The Studio Kit gallery's view: its sidebar and the scrolling column of its sections, declared from its model every
// round.

#pragma once

#include "pr/kit.h"
#include "pr/kit_surface.h"

#include "examples/studio_gallery/gallery_state.h"

PR_ASSUME_NONNULL_BEGIN

namespace gallery {

// Declares the gallery under the screen's ground, and applies the round's input to the model.
void buildGallery(PrKit* kit, PrLayoutNode ground, GalleryState& state);

} // namespace gallery

PR_ASSUME_NONNULL_END
