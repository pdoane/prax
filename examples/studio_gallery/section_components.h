// The gallery's components: the tree, the inspector, the toolbar, tables, and tags and badges.

#pragma once

#include "pr/kit.h"

#include "examples/studio_gallery/gallery_state.h"

PR_ASSUME_NONNULL_BEGIN

namespace gallery {

// Create the sections under `parent`, each bound to the model's part that it shows.
void sectionTree(PrKit* kit, PrLayoutNode parent, GalleryState& state);
void sectionInspector(PrKit* kit, PrLayoutNode parent, GalleryState& state);
void sectionToolbar(PrKit* kit, PrLayoutNode parent, GalleryState& state);
void sectionTables(PrKit* kit, PrLayoutNode parent, GalleryState& state);
void sectionTags(PrKit* kit, PrLayoutNode parent, GalleryState& state);

} // namespace gallery

PR_ASSUME_NONNULL_END
