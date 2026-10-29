// CSSOM View: the geometry and scrolling of elements, as laid out by the presentation of their document.
//
// cssom-view-1 - https://drafts.csswg.org/cssom-view-1/
//
// Each query first brings the presentation of the element's document up to date, as a synchronous read must, and
// answers from that layout. An element whose document has no presentation, or that generates no box, has no geometry:
// its rectangles are empty and its measures are zero. Coordinates are in CSS pixels from the viewport's top left.

#pragma once

#include "pr/base.h"
#include "pr/dom.h"
#include "pr/dom_geometry.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// The offsets to scroll to, or by (cssom-view-1/#dictdef-scrolltooptions). An omitted offset keeps the current offset,
// or scrolls by zero.
typedef struct PrCssScrollToOptions {
    bool hasLeft;
    bool hasTop;
    double left;                    // used if hasLeft is set
    double top;                     // used if hasTop is set
} PrCssScrollToOptions;

// The position at which scrolling into view puts an element along an axis of a scrolling box
// (cssom-view-1/#enumdef-scrolllogicalposition).
enum {
    PrCssScrollLogicalPosition_Start,
    PrCssScrollLogicalPosition_Center,
    PrCssScrollLogicalPosition_End,
    PrCssScrollLogicalPosition_Nearest,     // the least scroll that shows it, or none if it is already shown
};
typedef uint8_t PrCssScrollLogicalPosition;

// How an element scrolls into view (cssom-view-1/#dictdef-scrollintoviewoptions).
typedef struct PrCssScrollIntoViewOptions {
    PrCssScrollLogicalPosition block;
    PrCssScrollLogicalPosition inlinePosition;
} PrCssScrollIntoViewOptions;

// Returns the default options: start along the block axis and nearest along the inline axis.
static inline PrCssScrollIntoViewOptions prCssScrollIntoViewOptionsDefault(void) {
    PrCssScrollIntoViewOptions value;
    value.block = PrCssScrollLogicalPosition_Start;
    value.inlinePosition = PrCssScrollLogicalPosition_Nearest;
    return value;
}

// ---------------------------------------------------------------------------------------------------------------------
// Element (cssom-view)

// Writes the border boxes of the element's boxes (cssom-view-1/#dom-element-getclientrects). If a transform rotates a
// box, its rectangle is the bounds of the transformed box. Writes up to `capacity` rectangles to `out` and returns the
// total count.
uint32_t prCssElementGetClientRects(PrDomElement* element, PrDomRect* PR_NULLABLE out, uint32_t capacity);

// Returns the bounds of the element's client rectangles that have an area. If none has an area, returns the first
// rectangle, or an empty rectangle if there is none (cssom-view-1/#dom-element-getboundingclientrect).
PrDomRect prCssElementGetBoundingClientRect(PrDomElement* element);

// Return how far the element's content is scrolled from its scroll origin (cssom-view-1/#dom-element-scrolltop). The
// value is below zero toward an origin at the right or the bottom, as in rtl or vertical-rl. The document's scrolling
// element reports the viewport's scroll. Return zero for an element that is not a scroll container.
double prCssElementScrollTop(PrDomElement* element);
double prCssElementScrollLeft(PrDomElement* element);

// Scroll the element along one axis, in the same way as prCssElementScroll.
void prCssElementSetScrollTop(PrDomElement* element, double value);
void prCssElementSetScrollLeft(PrDomElement* element, double value);

// Scroll the element's content to the offsets that `options` gives, within its range
// (cssom-view-1/#dom-element-scroll), or by those offsets (cssom-view-1/#dom-element-scrollby). The document's
// scrolling element scrolls the viewport. A scroll that moves the content fires scroll and scrollend at the next
// rendering update. A non-finite offset is treated as zero.
void prCssElementScroll(PrDomElement* element, const PrCssScrollToOptions* options);
void prCssElementScrollBy(PrDomElement* element, const PrCssScrollToOptions* options);

// Scrolls each scroll container that the element is in, innermost first, and then the viewport, to put the element's
// border box where `options` aligns it (cssom-view-1/#dom-element-scrollintoview).
void prCssElementScrollIntoView(PrDomElement* element, const PrCssScrollIntoViewOptions* options);

// Return the extent of the element's scrolling area, which is its padding box and the overflow that it scrolls through
// (cssom-view-1/#dom-element-scrollwidth). For the root element, the extent is the viewport's.
int32_t prCssElementScrollWidth(PrDomElement* element);
int32_t prCssElementScrollHeight(PrDomElement* element);

// Return the widths of the element's top and left borders (cssom-view-1/#dom-element-clienttop). Return zero for an
// inline box.
int32_t prCssElementClientTop(PrDomElement* element);
int32_t prCssElementClientLeft(PrDomElement* element);

// Return the extent of the element's padding box (cssom-view-1/#dom-element-clientwidth). For the root element, and for
// the body in quirks mode, the extent is the viewport's. Return zero for an inline box.
int32_t prCssElementClientWidth(PrDomElement* element);
int32_t prCssElementClientHeight(PrDomElement* element);

// ---------------------------------------------------------------------------------------------------------------------
// Document (cssom-view)

// Returns the element that scrolls the viewport (cssom-view-1/#dom-document-scrollingelement). This is the root element
// or, in quirks mode, the body if the body is not potentially scrollable. Returns null if there is none.
PrDomElement* PR_NULLABLE prCssDocumentScrollingElement(PrDomDocument* document);

// Writes the elements whose boxes are at (`x`, `y`), topmost first, followed by the root element if it is not among
// them (cssom-view-1/#dom-document-elementsfrompoint). An element in a shadow tree is retargeted against the document,
// to its host. There are none for a point outside the viewport. Writes up to `capacity` elements to `out` and returns
// the total count.
uint32_t prCssDocumentElementsFromPoint(PrDomDocument* document, double x, double y,
                                        PrDomElement* PR_NULLABLE* PR_NULLABLE out, uint32_t capacity);

// Returns the first of the elements at (`x`, `y`), or null (cssom-view-1/#dom-document-elementfrompoint).
PrDomElement* PR_NULLABLE prCssDocumentElementFromPoint(PrDomDocument* document, double x, double y);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
