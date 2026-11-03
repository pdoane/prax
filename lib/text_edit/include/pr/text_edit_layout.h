// The geometry of text that a layout tree shows, for motion (pr/text_edit_motion.h).

#pragma once

#include "pr/layout_query.h"
#include "pr/text_edit_motion.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Creates the geometry of the text nodes of `root` and its descendants as the last layout of `tree` placed them. The
// tree must stay valid while the geometry is in use, and a position's node is a PrLayoutNode.
PrTextEditGeometry prTextEditGeometryFromLayout(PrLayoutTree* tree, PrLayoutNode root);

// Creates the geometry of the flat text `text`, which the one text node `node` shows, perhaps with more after it so
// that a line has somewhere to place a caret. No stop lies past the end of `text`, which must stay valid while the
// geometry is in use. If the node's text differs from `text`, its lines are out of date, and the geometry answers no
// lines.
PrTextEditGeometry prTextEditGeometryFromLayoutText(PrLayoutTree* tree, PrLayoutNode node, PrStringView text);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
