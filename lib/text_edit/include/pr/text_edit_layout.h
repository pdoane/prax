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

// A text node that shows part of a flat text, from byte `start` of the text up to the next run's start.
typedef struct PrTextEditLayoutRun {
    PrLayoutNode node;
    uint32_t start;
} PrTextEditLayoutRun;

// Creates the geometry of the flat text `text`, which the text nodes of `runs` show in order under `root`, as styled
// text shows. The last node may show more after the text, as prTextEditGeometryFromLayoutText's may. `runs` and `text`
// must stay valid while the geometry is in use. A position's node may be any value, and each stop keeps it, since
// offsets count in the flat text. The place where two runs meet is one stop.
PrTextEditGeometry prTextEditGeometryFromLayoutRuns(PrLayoutTree* tree, PrLayoutNode root,
    const PrTextEditLayoutRun* runs, uint32_t count, PrStringView text);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
