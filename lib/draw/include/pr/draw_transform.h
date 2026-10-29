// Draw transforms: the current transform of a backend's walk through a list.

#pragma once

#include "pr/base.h"
#include "pr/draw_commands.h"
#include "pr/geom.h"
#include "pr/vec.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// The current transform during a backend's walk of a list: the composition of the transform pushes from the root.
typedef struct PrDrawTransform {
    PrDAffine2 daffine;         // the composition in doubles, which pushes compose with
    PrAffine2 affine;           // `daffine` in floats, used to map geometry

    // The snap space: the composition without the pushes that snap apart, in which snapped edges round to target pixels
    // (PrDrawEdges_Snapped).
    PrDAffine2 snap;

    // True if the transform collapses the plane toward a line or a point, is not finite, or is pushed inside a
    // degenerate transform. Nothing under it draws until its pop.
    bool degenerate;
} PrDrawTransform;

// Returns the walk's transform for `transform` alone, as at the root.
PrDrawTransform prDrawTransformFromAffine2(PrAffine2 transform);
PrDrawTransform prDrawTransformFromDAffine2(PrDAffine2 transform);

// Returns the current transform inside the PushTransformAffine2 command `push`. The command's transform is applied
// before `current`.
PrDrawTransform prDrawTransformPush(const PrDrawTransform* current, const PrDrawCommand* push);

// Returns the current transform moved by `offset`, which is applied before `current`, as inside an Embed command.
PrDrawTransform prDrawTransformMoved(const PrDrawTransform* current, PrDVec2 offset);

// A command whose snapped edges have been rounded, and what it points to that changed.
typedef struct PrDrawSnapped {
    PrDrawCommand command;
    PrCornerRect cornerRect;
    PrDrawBorder border;
    PrDrawBrush brush;
} PrDrawSnapped;

// Returns `command` with the edges it snaps rounded to target pixels under `transform` (PrDrawEdges_Snapped), as a copy
// in `storage`, or `command` itself if it snaps nothing. The copy is valid while `command` and `storage` are.
const PrDrawCommand* prDrawTransformSnap(const PrDrawTransform* transform, const PrDrawCommand* command,
                                         PrDrawSnapped* storage);

// Returns true if `command` changes nothing under the current transform. This is the case if the transform is
// degenerate and the command draws nothing under a degenerate transform (prDrawCommandSkipsCollapsed).
bool prDrawTransformDrawsNothing(const PrDrawTransform* transform, const PrDrawCommand* command);

// Returns true if the affine `transform` collapses the plane toward a line or a point, or is not finite. A transform
// pushed inside one that collapses is also degenerate, whatever it composes to.
bool prDrawAffine2Collapses(PrAffine2 transform);

// Returns true if `command` draws nothing inside a degenerate transform. This holds for every command except the
// transform pushes and pops and the pops of clips and groups. A clip or group push that draws nothing still opens a
// scope that its pop closes, and such a clip lets nothing through.
bool prDrawCommandSkipsCollapsed(const PrDrawCommand* command);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
