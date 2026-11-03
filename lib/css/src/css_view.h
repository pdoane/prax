// CSSOM View: the geometry and scrolling of elements and the viewport, as laid out by the presentation of their
// document.
//
// cssom-view-1 - https://drafts.csswg.org/cssom-view-1/

#pragma once

#include "pr/css_view.h"
#include "pr/dom.h"
#include "pr/dom_geometry.h"
#include "pr/layout_style.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Return the extent of the viewport that the document's presentation lays the document out in, and how far the viewport
// is scrolled, after bringing the presentation up to date. Return zero if the document has no presentation.
PrLayoutVec cssViewViewportExtent(PrDomDocument* document);
PrLayoutVec cssViewViewportScroll(PrDomDocument* document);

// Scroll the viewport to `offset`, or by `delta`, within its range (cssom-view-1/#dom-window-scroll).
void cssViewScrollViewport(PrDomDocument* document, PrLayoutVec offset);
void cssViewScrollViewportBy(PrDomDocument* document, PrLayoutVec delta);

// Returns true if the element is being rendered (html/#being-rendered), as of a layout that this call brings up to
// date.
bool cssViewIsRendered(PrDomElement* element);

// Returns the text of an element that is being rendered (html/#get-the-text-steps).
PrString cssViewRenderedText(PrDomElement* element);

// Returns the element that an HTML element's offsets are measured from (cssom-view-1/#dom-htmlelement-offsetparent), or
// null.
PrDomElement* PR_NULLABLE cssViewOffsetParent(PrDomElement* element);

// Returns the element's border box relative to its offset parent's padding box, as laid out before any transform or
// scroll (cssom-view-1/#dom-htmlelement-offsettop). Its size is the bounds of the element's boxes. The body's offset is
// zero.
PrDomRect cssViewOffsetRect(PrDomElement* element);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
