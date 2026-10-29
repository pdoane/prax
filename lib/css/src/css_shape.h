// Basic shapes: a computed <basic-shape> as layout draws it.
//
// css-shapes-1 - https://drafts.csswg.org/css-shapes-1/
//
// A basic shape's percentages and keywords resolve against a reference box that only its user knows, such as the object
// bounding box of an SVG element or a border box that layout gives. A computed shape therefore converts to a layout
// shape (PrLayoutShape), which lib/layout resolves where it is drawn.

#pragma once

#include "pr/array.h"
#include "pr/draw.h"
#include "pr/geom.h"
#include "pr/layout_style.h"
#include "pr/path.h"

#include "css/src/css_value.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// What a length of a basic shape is a percentage of: the reference box's width, its height, or its normalized diagonal
// (css-shapes-1/#funcdef-basic-shape-circle).
enum {
    CssShapeAxis_X,
    CssShapeAxis_Y,
    CssShapeAxis_Diagonal,
};
typedef uint8_t CssShapeAxis;

// How a conversion makes the lengths of a computed basic shape into layout lengths.
struct CssShapeLengths {
    // Returns the layout length of `run`, a computed <length-percentage> along `axis`.
    PrLayoutLength (*PR_NONNULL length)(void* PR_NULLABLE context, CssValueRun run, CssShapeAxis axis);

    // Returns the layout length of one axis of a computed <position>, `run`, from the value at `index`. If that value
    // is an edge keyword, `offset` is the index of its offset, or kArrayMissing if it has none (cssPositionAxes).
    PrLayoutLength (*PR_NONNULL position)(void* PR_NULLABLE context, CssValueRun run, uint32_t index, uint32_t offset,
                                          CssShapeAxis axis);

    void* PR_NULLABLE context;
};

// Returns the operator with which a <compositing-operator> composites a mask layer onto the layers below it
// (css-masking-1/#the-mask-composite).
PrDrawComposite cssMaskCompositeOperator(PrCssKeyword keyword);

// Returns true if the value at `index` of `run` is a basic shape function (css-shapes-1/#typedef-basic-shape).
bool cssIsBasicShape(CssValueRun run, uint32_t index);

// Writes the computed basic shape at `index` of `run` to `out`, with its lengths converted by `lengths`. Appends a
// polygon's points to `points`, and the caller points `out->points` at them. Appends a path() to `path`, and the caller
// points `out->path` at it. A computed value holds xywh() and rect() as inset()
// (css-shapes-1/#basic-shape-computed-values), so they are not converted here. Returns false if the value is not a
// basic shape that the engine draws, such as shape().
bool cssShapeFromValue(CssValueRun run, uint32_t index, const CssShapeLengths& lengths, Array<PrLayoutLength>& points,
                       PrPath* path, PrLayoutShape* out);

// Appends the outline of the computed basic shape at `index` of `run`, resolved in the reference box `box`, to `out`,
// and writes the rule that fills it to `rule`: the shape's own fill rule for polygon() and path(), and nonzero for the
// others. A shape whose outline encloses nothing appends nothing. Returns false if the value is not a basic shape that
// the engine draws.
bool cssShapePath(CssValueRun run, uint32_t index, PrRect box, PrPath* out, PrPathFillRule* rule);

// Returns true if `property` is a physical corner's shape (css-borders-4/#corner-shape), one of the longhands that
// computed styles hold.
bool cssIsCornerShape(PrCssProperty property);

// Returns the superellipse parameter K of a computed <corner-shape-value> (css-borders-4/#typedef-corner-shape-value):
// a keyword, or superellipse() of a number, infinity or -infinity.
double cssCornerShapeParameter(CssValueRun run);

// Appends superellipse(`k`), with infinity and -infinity as their keywords.
void cssValuePoolAddSuperellipse(CssValuePool* pool, double k);

// Appends the corner shape at `progress` between the computed corner shapes `from` and `to`, as superellipse(). The
// parameters interpolate through their normalized half corners (css-borders-4/#corner-shape-interpolation), so a
// transition from a bevel to a square corner moves at an even pace.
void cssCornerShapeInterpolate(CssValuePool* out, CssValueRun from, CssValueRun to, double progress);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
