// Where a caret goes: motion through positions in shown text, over an interface to its geometry, so that one set of
// rules serves flat text and a tree of nodes in every script and writing mode, and is tested without a window.

#pragma once

#include "pr/text.h"
#include "pr/text_edit.h"
#include "pr/text_edit_command.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// ---------------------------------------------------------------------------------------------------------------------
// Geometry

// A caret in shown text: before byte `offset` of the text of `node`, which the geometry names. Flat text has one node.
typedef struct PrTextEditPosition {
    uint64_t node;
    uint32_t offset;
    PrTextEditAffinity affinity;
} PrTextEditPosition;

// A place on a line where a caret can stand. `along` grows in the line-right direction, in units that all lines share.
typedef struct PrTextEditLineStop {
    PrTextEditPosition position;
    float along;
} PrTextEditLineStop;

// How lines lie on the page, which decides what each arrow key means.
enum {
    PrTextEditFlow_HorizontalTb,    // lines run right and stack downward
    PrTextEditFlow_VerticalRl,      // lines run down and stack leftward
    PrTextEditFlow_VerticalLr,      // lines run down and stack rightward
    PrTextEditFlow_SidewaysLr,      // lines run up and stack rightward
};
typedef uint8_t PrTextEditFlow;

// A visual line. Its stops are in order of `along`, and are valid until the next call of the geometry. Where two parts
// of the text meet, the line has a stop at the same `along` for each part.
typedef struct PrTextEditLine {
    const PrTextEditLineStop* stops;
    uint32_t stopCount;
    PrTextEditFlow flow;
    PrTextDirection direction;      // the paragraph's: a right-to-left line starts at its line-right end

    // The stops that come first and last in the order of the text.
    uint32_t startStop;
    uint32_t endStop;
} PrTextEditLine;

// Which line a geometry answers for, relative to the line that holds a position, in content order.
enum {
    PrTextEditLineStep_Same,
    PrTextEditLineStep_Next,
    PrTextEditLineStep_Previous,
};
typedef uint8_t PrTextEditLineStep;

// Writes a line relative to the one that holds `at`. Returns false if there is no such line, or if the text is not
// shown as it stands.
typedef bool (*PrTextEditLineFn)(void* PR_NULLABLE context, PrTextEditPosition at, PrTextEditLineStep step,
    PrTextEditLine* out);

// Returns the text of `node`, in which offsets count. It is valid until the text changes.
typedef PrStringView (*PrTextEditNodeTextFn)(void* PR_NULLABLE context, uint64_t node);

// Returns the node next to `node` in content order, by `step`, or zero if there is none. Writes whether no block
// boundary separates the two to `contiguous`, so that a word or a cluster can span them.
typedef uint64_t (*PrTextEditNodeStepFn)(void* PR_NULLABLE context, uint64_t node, PrTextEditLineStep step,
    bool* contiguous);

// Frees what the geometry holds.
typedef void (*PrTextEditGeometryDestroyFn)(void* PR_NULLABLE context);

// How text is shown, as motion asks about it. `nodeStep` and `destroy` may be null: without `nodeStep`, the text is one
// node.
typedef struct PrTextEditGeometry {
    void* PR_NULLABLE context;
    PrTextEditLineFn line;
    PrTextEditNodeTextFn nodeText;
    PrTextEditNodeStepFn PR_NULLABLE nodeStep;
    PrTextEditGeometryDestroyFn PR_NULLABLE destroy;
} PrTextEditGeometry;

// Creates the geometry of `text` as one node with no reordering or wrapping: each line of its bytes is a line, and it
// runs in the paragraph's direction. It is for text that no layout shows as it stands, such as masked text. `text` must
// stay valid while the geometry is in use.
PrTextEditGeometry prTextEditGeometryFromText(PrStringView text, PrTextDirection direction);

// Frees what a geometry holds.
void prTextEditGeometryReset(PrTextEditGeometry* geometry);

// ---------------------------------------------------------------------------------------------------------------------
// Motion

// How the arrows along a line move through bidi text. Away from a change of direction, the three are the same.
enum {
    // Through the text in order, one cluster at a time. The arrow that points the way the paragraph runs goes forward,
    // so the caret jumps at a change of direction.
    PrTextEditArrowMotion_Logical,

    // As Logical, with an extra stop where the direction changes, which reaches both of the places that one offset has
    // there.
    PrTextEditArrowMotion_LogicalStops,

    // Along the line as it shows, so the caret never jumps, and the offset runs backward through a run against the
    // paragraph's direction.
    PrTextEditArrowMotion_Visual,
};
typedef uint8_t PrTextEditArrowMotion;

// Where a caret goes, independent of the keys. Before and after are in the order of the text.
enum {
    PrTextEditMotion_None,
    PrTextEditMotion_Left,          // one cluster along the line as it shows, onto the line that continues the text
    PrTextEditMotion_Right,
    PrTextEditMotion_ClusterBefore,
    PrTextEditMotion_ClusterAfter,
    PrTextEditMotion_StopBefore,    // as Cluster, stopping first at a change of direction (LogicalStops)
    PrTextEditMotion_StopAfter,
    PrTextEditMotion_WordLeft,      // a word before or after, by the way the paragraph runs
    PrTextEditMotion_WordRight,
    PrTextEditMotion_WordBefore,
    PrTextEditMotion_WordAfter,
    PrTextEditMotion_LineLeft,      // the start or end of the visual line, by the way the paragraph runs
    PrTextEditMotion_LineRight,
    PrTextEditMotion_LineStart,     // the first place of the visual line in the order of the text
    PrTextEditMotion_LineEnd,
    PrTextEditMotion_LinePrevious,  // the nearest place to the goal on the visual line before, or the start of the text
    PrTextEditMotion_LineNext,
    PrTextEditMotion_TextStart,
    PrTextEditMotion_TextEnd,
};
typedef uint8_t PrTextEditMotion;

// Returns the motion that `command` asks for at `at`. An arrow runs along the line or between lines as the writing mode
// of the line lays them out, and `arrowMotion` says how it moves along one. Returns None for a command that is not a
// motion.
PrTextEditMotion prTextEditMotionFromCommand(const PrTextEditGeometry* geometry, PrTextEditPosition at,
    PrTextEditCommand command, PrTextEditArrowMotion arrowMotion);

// Returns where `motion` takes a caret at `at`. `goal` is the position along the line that motion between lines keeps.
// A first such motion passes `hasGoal` false, and this writes both. Other motions leave them alone.
PrTextEditPosition prTextEditMotionMove(const PrTextEditGeometry* geometry, PrTextEditPosition at,
    PrTextEditMotion motion, float* goal, bool* hasGoal);

// An end of a selection that holds text.
enum {
    PrTextEditSelectionEnd_None,
    PrTextEditSelectionEnd_Low,
    PrTextEditSelectionEnd_High,
};
typedef uint8_t PrTextEditSelectionEnd;

// Returns the end of a selection that holds text that a collapsing `motion` goes toward, in a paragraph of `direction`,
// or None for a motion that goes from the caret.
PrTextEditSelectionEnd prTextEditMotionCollapseEnd(PrTextEditMotion motion, PrTextDirection direction);

// What a motion does to a selection's anchor.
enum {
    PrTextEditSelectionMove_Collapse,   // the anchor moves with the caret
    PrTextEditSelectionMove_Extend,     // the anchor stays
};
typedef uint8_t PrTextEditSelectionMove;

// Returns the selection of the text of `node` after `motion`. A collapsing motion by cluster, of a selection that holds
// text, goes to the end of the selection that the motion goes toward. A collapsing motion between lines moves from that
// end.
PrTextEditSelection prTextEditSelectionMove(const PrTextEditGeometry* geometry, uint64_t node,
    PrTextEditSelection selection, PrTextEditMotion motion, PrTextEditSelectionMove kind);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
