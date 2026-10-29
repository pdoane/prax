// The gallery's data views: the collection of five thousand assets, the timeline and the node graph.

#pragma once

#include "pr/kit.h"

#include "examples/studio_gallery/gallery_state.h"

PR_ASSUME_NONNULL_BEGIN

namespace gallery {

// Create the sections under `parent`, each bound to the model's part that it shows.
void sectionCollections(PrKit* kit, PrLayoutNode parent, GalleryState& state);
void sectionTimeline(PrKit* kit, PrLayoutNode parent, GalleryState& state);
void sectionGraph(PrKit* kit, PrLayoutNode parent, GalleryState& state);

} // namespace gallery

PR_ASSUME_NONNULL_END
