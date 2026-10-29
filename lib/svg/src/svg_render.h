// Rendering SVG: the content of the outermost svg elements in a presentation, and of the documents that have no
// presentation.
//
// SVG2 - https://svgwg.org/svg2-draft/render.html

#pragma once

#include "pr/svg.h"
#include "pr/svg_render.h"

#include "css/src/css_presentation_svg.h"
#include "dom/src/dom_node.h"
#include "svg/src/svg_cascade.h"
#include "svg/src/svg_query.h"
#include "svg/src/svg_tree.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// What lib/svg keeps for a presentation: the references that the presentation's boxes name. The content of the
// presentation's outermost svg elements is kept in their records (css_presentation_svg.h).
struct SvgPresentation;

// Attaches lib/svg to `presentation`, which then draws the content of its outermost svg elements and resolves what the
// effects of its boxes name by URL. Destroy the state after the presentation, whose boxes hold its references.
SvgPresentation* svgPresentationCreate(PrCssPresentation* presentation);
void svgPresentationDestroy(SvgPresentation* PR_NULLABLE state);

// Brings the content of the presentation's outermost svg elements up to date with the changes noted since the last
// call, between cssPresentationUpdateBoxes and cssPresentationUpdateLayout. Returns true if the boxes that name a
// reference are to be projected again first. Does nothing for a presentation that lib/svg is not attached to.
bool svgPresentationUpdate(PrCssPresentation* presentation);

// Notes that the image that an image or feImage element shows changed, which the browsing context loads. The state
// follows the rest of the document's changes itself, once it draws or resolves anything (dom_hooks.h).
void svgContentImageChanged(PrDomElement* element);

// The tree that holds an element's node, for a query of what the element draws: the content of the outermost svg
// element in the presentation of the element's document, brought up to date, or a tree built for the query if the
// document has no presentation.
struct SvgQuery {
    const SvgTree* PR_NULLABLE tree;    // null if no tree holds a node of the element
    SvgNodeIndex node;

    // The outermost svg element whose content the tree is, and the viewport that the tree is drawn in: the content box
    // of the element's box, or its natural size without a presentation.
    const PrDomElement* PR_NULLABLE outermost;
    PrVec2 viewport;
    PrCssPresentation* PR_NULLABLE presentation;    // the presentation of the document, or null

    // The tree and the cascade of a document that has no presentation, which the query owns.
    SvgTree own;
    SvgCascade cascade;
    bool owned;
};

// Finds the node of `element` in the tree that draws it. Returns false, with a null tree, if no tree draws the element,
// as when it is not rendered or is outside an svg element. The caller ends the query with svgQueryEnd in either case.
bool svgQueryBegin(SvgQuery* query, const PrDomElement* element);

// Frees what the query built.
void svgQueryEnd(SvgQuery* query);

// Writes where the query's node is drawn to `out`. Returns false if it is not drawn.
bool svgQueryPlace(const SvgQuery& query, SvgPlace* out);

// Maps `count` points in place from the space that the query's tree is drawn in, the content box of its outermost svg
// element, to viewport coordinates, through every transform around the element's box. Returns false for a document that
// has no presentation, which has no viewport.
bool svgQueryToClient(const SvgQuery& query, PrDVec2* points, uint32_t count);

// What the lengths of an element resolve against: the size of the viewport that its percentages are of, and the font
// size of its computed style.
struct SvgLengthBasis {
    PrVec2 viewport;
    float fontSize;
};

// Writes what the lengths of `element` resolve against to `out`: its viewport as the tree that draws it places it, or
// zero if no tree draws it, and its font size, or the initial font size if it has no style. Returns false if no tree
// draws the element.
bool svgElementLengthBasis(const PrDomElement* element, SvgLengthBasis* out);

// Maps `count` points in place from the containing block of the content of `foreignObject` to viewport coordinates,
// through the foreignObject's positioning rectangle and every transform around it, as the content is drawn. Returns
// false if no presentation draws the foreignObject.
bool svgForeignToClient(const PrDomElement* foreignObject, PrDVec2* points, uint32_t count);

// Writes the bounding box of `element`, an element of SVG content, in viewport coordinates to `out`: its object
// bounding box, mapped through every transform as it is drawn (cssom-view-1/#dom-element-getclientrects). Returns false
// if no presentation draws it.
bool svgClientBounds(const PrDomElement* element, PrRect* out);

} // namespace pr_impl

// C opaque types. An element in the SVG namespace.
struct PrSvgElement : PrDomElement {};

PR_ASSUME_NONNULL_END
