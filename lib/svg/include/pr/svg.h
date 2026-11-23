// SVG: the documents that it is enabled for, and the events of its animations
//
// SVG2 - https://svgwg.org/svg2-draft/
// svg-animations - https://svgwg.org/specs/animations/

#pragma once

#include "pr/base.h"
#include "pr/dom.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// ---------------------------------------------------------------------------------------------------------------------
// TimeEvent : Event (svg-animations/#InterfaceTimeEvent)
//
// The event of an animation's begin, end or repeat: beginEvent, endEvent or repeatEvent. Its detail is the number of
// the repeat for repeatEvent, and 0 otherwise.

// A TimeEvent.
typedef struct PrSvgTimeEvent PrSvgTimeEvent;

// What a TimeEvent is created with: an Event's initializer, and the detail.
typedef struct PrSvgTimeEventInit {
    PrDomEventInit event;
    int32_t detail;
} PrSvgTimeEventInit;

// Returns the default initializer.
static inline PrSvgTimeEventInit prSvgTimeEventInitDefault(void) {
    PrSvgTimeEventInit init = {prDomEventInitDefault(), 0};
    return init;
}

// Creates an event of `type`, with one reference that the caller holds.
PrSvgTimeEvent* prSvgTimeEventCreate(PrStringView type, const PrSvgTimeEventInit* init);

// Returns the event as an Event, or an Event as a TimeEvent, or null if it is not one.
PrDomEvent* prSvgTimeEventAsEvent(PrSvgTimeEvent* event);
PrSvgTimeEvent* PR_NULLABLE prSvgTimeEventFromEvent(PrDomEvent* event);

// Returns the event's detail.
int32_t prSvgTimeEventDetail(const PrSvgTimeEvent* event);

// Initializes the event as initTimeEvent does (svg-animations/#__svg__TimeEvent__initTimeEvent): its type, and its
// detail. It neither bubbles nor can be canceled. Does nothing while the event is being dispatched.
void prSvgTimeEventInitTimeEvent(PrSvgTimeEvent* event, PrStringView type, int32_t detail);

// Gives the document SVG: its elements' presentation attributes style them (SVG2/styling.html#PresentationAttributes).
// A document that the DOM creates from it, such as by DOMImplementation, has SVG too. Gives the document CSS first
// (prCssEnable), and MathML, which SVG content holds through foreignObject (prMathMlEnable). Does nothing for a
// document that has SVG.
void prSvgEnable(PrDomDocument* document);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
