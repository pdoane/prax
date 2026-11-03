// The content of outermost svg elements in a presentation
//
// SVG2 - https://www.w3.org/TR/SVG2/
//
// The presentation keeps a record for each outermost svg element that has a box (CssPresentationSvg), and lib/svg fills
// it: the content that draws the box, the element's natural size, and the layout roots of the foreignObject elements
// that the content draws. The browsing context runs lib/svg's step between the presentation's boxes and its layout, and
// that step brings the records up to date. lib/svg follows the document's tree itself (dom_hooks.h). The presentation
// notes for it what the presentation alone decides: which SVG elements' styles changed, which foreignObject elements
// gained or lost a box, and when everything that the content reads may have changed. The clipPaths, masks and filters
// that the effects of boxes name by URL come from the references that lib/svg gives the presentation
// (CssPresentationReferences).

#pragma once

#include "pr/css_presentation.h"
#include "pr/dom.h"
#include "pr/geom.h"
#include "pr/layout_style.h"
#include "pr/layout_tree.h"
#include "pr/raw_array.h"
#include "pr/raw_string.h"
#include "pr/string_view.h"

#include "dom/src/dom_node.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// The kinds of element that an effect of a box names by URL (css-masking-1/#the-clip-path,
// css-masking-1/#the-mask-image, filter-effects-1/#FilterProperty).
enum {
    CssPresentationReferenceKind_ClipPath,
    CssPresentationReferenceKind_Mask,
    CssPresentationReferenceKind_Filter,
};
typedef uint8_t CssPresentationReferenceKind;

// Resolves what the effects of boxes name by URL. lib/svg gives a presentation its references when it attaches to it
// (cssPresentationSetReferences).
struct CssPresentationReferences {
    // Writes to `out` the source of the element of `kind` that `url` names in a property of the box of `element`. The
    // caller owns a reference to the source. Returns false if `url` names no such element. The source's functions run
    // on the thread that records the scene. They read only what changes while the presentation holds its layout tree.
    bool (*PR_NONNULL reference)(void* PR_NULLABLE context, const PrDomElement* element, PrStringView url,
        CssPresentationReferenceKind kind, PrLayoutSource* out);
    void* PR_NULLABLE context;
};

// What the presentation notes for lib/svg: the style of an SVG element changed, or a foreignObject element's box was
// made or erased, which changes the layout root that its content is.
enum {
    CssPresentationSvgNoteKind_Style,
    CssPresentationSvgNoteKind_Box,
};
typedef uint8_t CssPresentationSvgNoteKind;

// A noted change. The note holds the element (domNodeHold) until lib/svg takes it.
struct CssPresentationSvgNote {
    PrDomElement* element;
    CssPresentationSvgNoteKind kind;
};

// A foreignObject whose content the content of an svg element draws: the element, the layout root of its content, and
// the size of its positioning rectangle in user units, which the root's containing block takes.
struct CssPresentationSvgForeignObject {
    const PrDomElement* element;
    PrLayoutNode root;
    PrVec2 extent;
};

// What a hit test of the content of an svg element finds under a point (SVG2/interact.html#PointerEventsProp). The
// element is an element of the content, or of a use element's shadow tree if the point is on an instance. For a
// foreignObject, `root` is the layout root of its content, and `rootPoint` the point from the top left of the root's
// containing block.
struct CssPresentationSvgHit {
    const PrDomElement* element;
    PrLayoutNode root;          // kPrLayoutNodeNone unless the element is a foreignObject with content
    PrVec2 rootPoint;
};

// The functions of the content of an outermost svg element, which lib/svg makes. `content` is the content that lib/svg
// gave the record.
struct CssPresentationSvgContentFunctions {
    // Draws the content for its box (PrLayoutPaintFn), with the content as the context. It reads only the content and
    // the drawings of the layout roots that it draws (prLayoutNodeDraw), so it runs on whichever thread records the
    // scene.
    PrLayoutPaintFn PR_NONNULL paint;

    // Hit tests `point`, in CSS pixels from the top left of the element's content box, against the content as of its
    // last update. Writes up to `capacity` hits to `out`, topmost first, and returns their number.
    uint32_t (*PR_NONNULL hitTest)(const void* content, PrVec2 point, CssPresentationSvgHit* PR_NULLABLE out, uint32_t capacity);

    // Writes the bounding box of `element`, an element of the content, in viewport coordinates, to `out`
    // (cssom-view-1/#dom-element-getclientrects). Returns false if the content does not draw the element.
    bool (*PR_NONNULL clientRect)(const void* content, const PrDomElement* element, PrDomRect* out);

    // Maps `count` points in place from the containing block of the content of `foreignObject`, an element of the
    // content, to viewport coordinates, through the foreignObject's place in the content and every transform around it.
    // Returns false if the content does not draw the foreignObject.
    bool (*PR_NONNULL foreignToClient)(const void* content, const PrDomElement* foreignObject, PrDVec2* points, uint32_t count);

    // Returns true if the content, as last brought up to date, matches content built from scratch. Writes the first
    // difference to `report` otherwise, replacing its contents.
    bool (*PR_NONNULL verify)(void* content, PrRawString* report);

    // Frees the content. The presentation calls it once no box draws the content.
    void (*PR_NONNULL release)(void* content);
};

// An outermost svg element that has a box, and its content.
struct CssPresentationSvg {
    const PrDomElement* element;

    // The content and its functions, which lib/svg sets when it first brings the record up to date. Null until then.
    void* PR_NULLABLE content;
    const CssPresentationSvgContentFunctions* PR_NULLABLE functions;

    // What lib/svg last brought up to date: the element's natural dimensions, the foreignObject elements whose content
    // the content draws, and the extent of the box that the content was brought up to date for, which hit testing and
    // the queries of its elements map through.
    PrLayoutNaturalSize natural;
    RawArray<CssPresentationSvgForeignObject> foreignObjects;
    PrLayoutVec extent;

    // The content needs bringing up to date: it is new, or its box's extent changed.
    bool stale;
};

// What bringing the content of an outermost svg element up to date gave.
struct CssPresentationSvgUpdate {
    bool changed;               // what the content draws may have changed
    uint32_t built;             // the nodes of its SVG tree built (PrCssPresentationStats::svgNodesBuilt)
    const CssPresentationSvgForeignObject* PR_NULLABLE foreignObjects;
    uint32_t foreignObjectCount;
};

// Returns the presentation's records, of the outermost svg elements that have boxes, in no order, and writes their
// number to `count`.
CssPresentationSvg* PR_NONNULL const* PR_NULLABLE cssPresentationSvgs(const PrCssPresentation* presentation, uint32_t* count);

// Returns the record of the outermost svg element `svg`, or null if it has no box.
CssPresentationSvg* PR_NULLABLE cssPresentationSvgOf(const PrCssPresentation* presentation, const PrDomElement* svg);

// Sets the natural dimensions of the element of `svg`, which its box is laid out again for if they changed.
void cssPresentationSvgSetNatural(PrCssPresentation* presentation, CssPresentationSvg* svg, PrLayoutNaturalSize natural);

// Returns the size in CSS pixels that the content of `svg` is built for: the extent that layout last gave its content
// box, or before the box's first layout, its natural size if it has both a width and a height.
PrVec2 cssPresentationSvgExtent(const PrCssPresentation* presentation, const CssPresentationSvg* svg);

// Takes what bringing the content of `svg` up to date gave. The box draws the content, and the box and the roots of the
// foreignObject elements are laid out again if what layout reads of them changed.
void cssPresentationSvgUpdated(PrCssPresentation* presentation, CssPresentationSvg* svg, const CssPresentationSvgUpdate& update);

// Notes that the content of `svg` needs bringing up to date, so that the next update does.
void cssPresentationSvgStale(PrCssPresentation* presentation, CssPresentationSvg* svg);

// Returns the changes noted since cssPresentationClearSvgNotes last forgot them, and writes their number to `count`.
const CssPresentationSvgNote* PR_NULLABLE cssPresentationSvgNotes(const PrCssPresentation* presentation, uint32_t* count);
void cssPresentationClearSvgNotes(PrCssPresentation* presentation);

// Returns true if everything that the content reads may have changed since the last call: the document's fonts, or what
// colors resolve for.
bool cssPresentationTakeSvgInvalid(PrCssPresentation* presentation);

// Sets the references that resolve what the effects of the presentation's boxes name by URL, or clears them if null.
// The references must outlive the presentation or be cleared first.
void cssPresentationSetReferences(PrCssPresentation* presentation,
    const CssPresentationReferences* PR_NULLABLE references);

// Sets what lib/svg keeps for the presentation, or null. lib/svg frees it before the presentation is destroyed. Notes
// are kept only while the presentation has it.
void cssPresentationSetSvgState(PrCssPresentation* presentation, void* PR_NULLABLE state);
void* PR_NULLABLE cssPresentationSvgState(const PrCssPresentation* presentation);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
