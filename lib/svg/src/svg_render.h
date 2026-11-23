// Rendering SVG: the content of the outermost svg elements in a presentation, and of the documents that have no
// presentation.
//
// SVG2 - https://svgwg.org/svg2-draft/render.html

#pragma once

#include "pr/svg.h"
#include "pr/svg_element.h"
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

// Notes that the animations changed the value of the element's attribute `name`, one that is not a CSS property, as a
// change to the attribute itself is noted. The instances of the element in use elements' shadow trees take the value
// too.
void svgContentAttributeAnimated(PrDomElement* element, PrStringView name);

// Returns true if `element` is an outermost svg element: an svg element with no svg element above it in its SVG
// document fragment (prSvgElementOwnerSvgElement).
bool svgIsOutermostSvg(const PrDomElement* element);

// Returns true if `element` is an outermost svg element whose magnification and panning transform was set
// (prSvgElementSetCurrentScale).
bool svgIsMagnified(const PrDomElement* element);

// Notes that the magnification and panning transform of the outermost svg element `element` changed, so that its
// content draws again.
void svgContentMagnified(PrDomElement* element);

// Creates an image of the outermost svg element `svg`, as prSvgImageCreate does, whose document's media queries read
// `base`, the environment of the document that embeds it, or a screen of no preferences if it is null. The image's own
// viewport and color scheme replace those of `base`.
const PrSvgImage* svgImageCreate(PrSvgElement* svg, PrStringView fragment, const PrSvgDrawOptions* PR_NULLABLE options,
    const CssMediaEnvironment* PR_NULLABLE base);

// Returns true if `image` draws the same as a build in `base` would, with a dark color scheme if `dark` is set, laid
// out at `size`. That is so if every media feature that the image's build read has the same value in `base`, and the
// scheme and the size are the same if the build read them. A `size` of zero is the image's natural size, or else the
// default object size.
bool svgImageServes(const PrSvgImage* image, const CssMediaEnvironment* PR_NULLABLE base, bool dark, PrVec2 size);

// Returns `natural` in layout units, each rounded to the nearest unit, so that a size a rounding error short of a whole
// pixel covers that pixel.
PrLayoutNaturalSize svgLayoutNaturalSize(PrSvgNaturalSize natural);

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

// Finds the node of `element` as svgQueryBegin does. If no tree draws the element, builds the element alone in its
// parent's user space, so that its geometry can be measured. It is built even if it is not displayed or not in the
// document, and its percentages resolve against the natural size of its outermost svg element. Returns false if the
// element still has no node.
bool svgQueryBeginGeometry(SvgQuery* query, const PrDomElement* element);

// Frees what the query built.
void svgQueryEnd(SvgQuery* query);

// Writes where the query's node is drawn to `out`. Returns false if it is not drawn.
bool svgQueryPlace(const SvgQuery& query, SvgPlace* out);

// Writes the width and height that a rect, image or foreignObject element is drawn with, in its user space, to `out`
// (CssBoxProvider::usedSize). Returns false for another element, or one that no tree draws.
bool svgElementUsedSize(const PrDomElement* element, PrVec2* out);

// Maps `count` points in place from the space that the query's tree is drawn in, the content box of its outermost svg
// element, to viewport coordinates, through every transform around the element's box. Returns false for a document that
// has no presentation, which has no viewport.
bool svgQueryToClient(const SvgQuery& query, PrDVec2* points, uint32_t count);

// What the lengths of an element resolve against: the size of the viewport that its percentages are of, and the font
// size of its computed style.
struct SvgLengthContext {
    PrVec2 viewport;
    float fontSize;
};

// Writes what the lengths of `element` resolve against to `out`: its viewport as the tree that draws it places it, or
// for an element that no tree draws, the viewport that its nearest drawn ancestor gives its children, or else zero; and
// its font size, or the initial font size if it has no style. Returns false if no tree draws the element.
bool svgElementLengthBasis(const PrDomElement* element, SvgLengthContext* out);

// Writes what the lengths of `element` resolve against as the presentation last drew it, without bringing it up to
// date, for a reader inside the presentation's update: its viewport in the tree that the last update built, or the
// viewport that its nearest drawn ancestor gives its children, or else the natural size of its outermost svg element;
// and its font size as the last update styled it.
void svgElementLengthBasisAsDrawn(const PrDomElement* element, SvgLengthContext* out);

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
