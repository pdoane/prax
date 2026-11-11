// Draw lists: recorded commands that a backend renders.
//
// A command that cannot paint is not recorded, such as a fill of a shape with no area or a command whose color has no
// alpha. A command's coordinates are in the transform space in effect when it is recorded. Transforms, clips, groups
// and marks are pushed and popped in one nesting order. Each pop closes the most recent open push, which must be of the
// same kind.
//
// A list copies the data that a call passes into storage that it owns, which stays valid until the list changes. It
// records a path as a clone, and holds a reference to each image, font, list and filter that it names. A list that
// another list references, by an embed or as a source, is frozen and must not change again.

#pragma once

#include "pr/base.h"
#include "pr/color.h"
#include "pr/draw.h"
#include "pr/draw_image.h"
#include "pr/geom.h"
#include "pr/path.h"
#include "pr/path_polyline.h"
#include "pr/vec.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Creates an empty list with one reference.
PrDrawList* prDrawListCreate(void);

// Adds or releases a reference. The last release frees the list.
void prDrawListRetain(PrDrawList* list);
void prDrawListRelease(PrDrawList* PR_NULLABLE list);

// Holds a resource of the host's that the list's commands draw, such as the texture of a Texture source, until the list
// is emptied or freed, which calls `release` with `context` on the thread that does it. The list must not be frozen.
void prDrawListHold(PrDrawList* list, void (*PR_NONNULL release)(void* PR_NULLABLE context), void* PR_NULLABLE context);

// Empties the list and keeps the storage. The list must not be frozen.
void prDrawListClear(PrDrawList* list);

// Clears the target to `color`, straight alpha, within `rect` and the clip. Canvas 2D's clearRect is the case of a
// transparent color. Records a clip to `rect` around a Clear command.
void prDrawListClearRect(PrDrawList* list, PrRect rect, PrColor16 color);

// Whether a contour returns to its first point. A stroke joins the ends of a closed contour and caps those of an open
// one. A fill closes every contour.
enum {
    PrDrawContour_Open,
    PrDrawContour_Closed,
};
typedef uint8_t PrDrawContour;

// Records a shape, filled or stroked as `paint` says, in its color or its brush. A line has no fill and needs a stroke.
// An image brush whose source is `list` itself paints nothing. prDrawListPolylineContour records a polyline of one
// contour.
void prDrawListRect(PrDrawList* list, PrRect rect, PrDrawPaint paint);
void prDrawListCornerRect(PrDrawList* list, PrCornerRect shape, PrDrawPaint paint);
void prDrawListEllipse(PrDrawList* list, PrVec2 center, PrVec2 radii, float rotation, PrDrawPaint paint);
void prDrawListSector(PrDrawList* list, PrVec2 center, float innerRadius, float outerRadius, float startAngle,
    float sweep, PrDrawPaint paint);
void prDrawListCapsule(PrDrawList* list, PrVec2 start, PrVec2 end, float radius, PrDrawPaint paint);
void prDrawListLine(PrDrawList* list, PrVec2 start, PrVec2 end, PrDrawPaint paint);
void prDrawListPolyline(PrDrawList* list, const PrPathPolyline* polyline, PrPathFillRule rule, PrDrawPaint paint);
void prDrawListPolylineContour(PrDrawList* list, const PrVec2* points, uint32_t pointCount, PrDrawContour contour,
    PrPathFillRule rule, PrDrawPaint paint);
void prDrawListPath(PrDrawList* list, const PrPath* path, PrPathFillRule rule, PrDrawPaint paint);

// Records a box's border. Each side paints its own color or brush, and the border reads only the paint's operator,
// blend mode and edges.
void prDrawListBorder(PrDrawList* list, const PrDrawBorder* border, PrDrawPaint paint);

// Records a box's shadow or decoration, in the paint's color or brush. The paint has no stroke.
void prDrawListDropShadow(PrDrawList* list, const PrDrawShadow* shadow, PrDrawPaint paint);
void prDrawListInnerShadow(PrDrawList* list, const PrDrawShadow* shadow, PrDrawPaint paint);
void prDrawListDecoration(PrDrawList* list, const PrDrawDecoration* decoration, PrDrawPaint paint);

// Records a nine-slice, whose regions of `source` paint as one shape, times the paint's color. The paint has no brush
// and no stroke. Nothing is recorded if `source` is `list` itself.
void prDrawListNineSlice(PrDrawList* list, PrDrawSource source, const PrDrawNineSlice* slices, PrRect destination,
    PrDrawSampling sampling, PrDrawPaint paint);

// Records a command that replaces the target's pixels with the image's pixels, with the image's top left at the target
// point `origin`. Transforms, clips and compositing do not apply.
void prDrawListWritePixels(PrDrawList* list, const PrDrawImage* image, PrIVec2 origin);

// Records a run of glyphs, each placed at its position from `origin`, filled or stroked as `paint` says. Each glyph
// composites on its own. A filled color glyph paints its own colors, times the paint's alpha: its color's alpha, or its
// brush's opacity. If the font asks for the text color, the glyph uses the paint's color. The glyphs of an aliased font
// (prFontIsAliased) are recorded aliased, whatever the paint's edges.
void prDrawListGlyphs(PrDrawList* list, const PrFontGlyphRun* run, PrVec2 origin, PrDrawPaint paint);

// Plays the commands of `embedded` as if recorded here, in the current transform space moved by `offset`, and freezes
// `embedded`. The offset is in doubles, as transforms compose. Nothing is recorded if `embedded` is `list`, has open
// pushes or has no commands.
void prDrawListEmbed(PrDrawList* list, PrDrawList* embedded, PrDVec2 offset);

// Freezes the list: it must not change again. A frozen list may be embedded and read on several threads at once.
void prDrawListFreeze(PrDrawList* list);

// Pushes a transform, which applies before the current one until its pop. If the composed transform collapses the
// plane, nothing draws until its pop (prDrawTransformPush). A transform is held in doubles, and transforms compose in
// doubles. A transform in floats is widened to doubles. A projection is a group's (PrDrawGroup::projection).
void prDrawListPushTransformAffine2(PrDrawList* list, PrAffine2 transform);
void prDrawListPushTransformDAffine2(PrDrawList* list, PrDAffine2 transform);
void prDrawListPopTransform(PrDrawList* list);

// Pushes a transform that takes part in snapping as `flags` say: its origin rounded to a target pixel along the axes
// they name, or snapping inside it apart from it, or the rectangle from its origin to `extent` fitted to rounded edges
// (PrDrawSnapFlags). It pops with prDrawListPopTransform.
void prDrawListPushTransformSnapped(PrDrawList* list, PrDAffine2 transform, PrDrawSnapFlags flags);
void prDrawListPushTransformSnappedRect(PrDrawList* list, PrDAffine2 transform, PrVec2 extent);

// Pushes a clip, which applies until its pop. `targetRect` is in the target's pixels, regardless of the transforms.
void prDrawListPushClipRect(PrDrawList* list, PrRect rect, PrDrawEdges edges);
void prDrawListPushClipTargetRect(PrDrawList* list, PrRect targetRect, PrDrawEdges edges);
void prDrawListPushClipCornerRect(PrDrawList* list, PrCornerRect shape, PrDrawEdges edges);
void prDrawListPushClipPath(PrDrawList* list, const PrPath* path, PrPathFillRule rule, PrDrawEdges edges);
void prDrawListPopClip(PrDrawList* list);

// Pushes a group (PrDrawGroup), which applies until its pop. A group that composites in sRGB-linear is recorded as up
// to two groups that composite in sRGB, whose filter reads what lies beneath, and its pop closes both.
void prDrawListPushGroup(PrDrawList* list, const PrDrawGroup* group);
void prDrawListPopGroup(PrDrawList* list);

// Pushes a mark, which labels the commands until its pop with the producer's `key`, such as the node of a document that
// the commands present, or pops it. A mark changes nothing that a list draws. It carries the meaning of the content to
// a writer, which joins its marks to a structure (pr/draw_structure.h). A key may label many marks, and is not zero.
void prDrawListPushMark(PrDrawList* list, uint64_t key);
void prDrawListPopMark(PrDrawList* list);

// Returns the number of open pushes. A list renders only if there are none.
uint32_t prDrawListDepth(const PrDrawList* list);

// Returns the area that the list may cover, in its own space, or an empty rect if the list paints nothing. A group
// whose operator is unbounded (prDrawCompositeIsUnbounded) covers its bounds. A projected group covers its bounds,
// projected and cut behind the eye (prDrawProjectionBounds). A painting command whose operator is unbounded covers its
// clip. Written pixels (prDrawListWritePixels) cover their position in the target's space, unless they are inside a
// projected group. A list that plays a list that writes pixels is unbounded.
PrRect prDrawListBounds(const PrDrawList* list);

// Returns true if nothing that the list draws, played under `transform`, reaches `clip`, a rectangle in target pixels.
// A list that writes pixels reaches every clip. A consumer of a list skips an embedded list that misses the clip in
// effect. The list caches its bounds when it is frozen, so what lies outside the clip costs only this test.
bool prDrawListMisses(const PrDrawList* list, PrAffine2 transform, PrRect clip);

// Returns the area that `count` commands may cover, in their own space, as prDrawListBounds measures a list of them.
// The commands may belong to any list, and close every scope that they open.
PrRect prDrawCommandsBounds(const PrDrawCommand* PR_NULLABLE commands, uint32_t count);

// Returns true if a group in the list, or in a list that it plays, reads what lies beneath it. Such a group has a
// backdrop, or a filter that reads the backdrop. A projected group is not a backdrop root, so a group inside it counts.
bool prDrawListReadsBackdrop(const PrDrawList* list);

// Returns true if the list, or a list that it plays, composites with what it draws over, played under `transform`: a
// command or a group with an operator other than source-over or a blend mode other than normal, a knockout group, or a
// Clear of a color that is not opaque within a clip that covers pixels in part, whose edges mix the color with what
// lies under them. A clip lets through whole pixels if each clip in force is a rectangle, under a transform that keeps
// it axis-aligned, whose edges lie on pixels or are aliased, and it lies outside any group that draws in a space of its
// own (one with a projection, a filter or a backdrop). A renderer that cannot read its target draws such a list into a
// layer of its own.
bool prDrawListReadsDestination(const PrDrawList* list, const PrDrawTransform* transform);

// Returns true if the list, or a list that it plays, writes pixels (prDrawListWritePixels) outside a projected group.
// Written pixels land in the target's space wherever the list plays.
bool prDrawListWritesPixels(const PrDrawList* list);

// Returns a number that is unique to the list's current contents for the life of the process. It is a key for data
// derived from what the list draws. It changes whenever the list changes.
uint64_t prDrawListId(const PrDrawList* list);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
