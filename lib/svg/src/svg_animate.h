// SVG animation: the time containers of a document's outermost svg elements, the animation elements that they time, and
// the values that those elements give their targets' attributes
//
// svg-animations - https://svgwg.org/specs/animations/
//
// Each outermost svg element is a time container, whose time starts when the document's animations first update after
// it is created (svg-animations/#DocumentBegin), and which a script can pause and seek. A sample of a document walks
// the intervals of its animation elements to their containers' times, queues the events that they cross, and computes
// the value of each animated attribute. The animations of one attribute compose in the order of their begin times, then
// in document order (svg-animations/#RelationshipToSMILAnimation). An attribute that maps to a CSS property reaches the
// target's style as declarations of CSS values that follow its style attribute (cssElementAnimatedStyle). Every
// animated attribute, CSS or not, reads back as a value through svgAnimatedValue, which the renderer and the typed
// attributes read, or as text through svgAnimatedAttribute. An animateMotion adds a transform outside the target's own.
//
// The document's timeline drives the samples (CssAnimationDriver). Seeking, beginning or ending an element from script,
// and a change to the tree sample at once, so that a script reads the values of the new time.

#pragma once

#include "pr/dom.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

struct SvgAnimations;
struct SvgAnimateValue;

// Creates the animations of `document`, which register with its timeline, or frees them as the document goes.
SvgAnimations* svgAnimationsCreate(PrDomDocument* document);
void svgAnimationsDestroy(SvgAnimations* PR_NULLABLE animations);

// Records that the document's readiness changed. The time containers begin once it is complete.
void svgAnimationsReadinessChanged(SvgAnimations* animations);

// Records that the document's tree changed: `node` was inserted, or an attribute of `element` named `name` changed. The
// animation elements are found again before the next sample, which runs before a script reads a value, and before a
// script runs if the change adds an animation element or changes one.
void svgAnimationsTreeChanged(SvgAnimations* animations, const PrDomNode* node);
void svgAnimationsAttributeChanged(SvgAnimations* animations, PrDomElement* element, PrStringView name);

// Drops what the animations hold of an element that is being freed: the time container of an svg element.
void svgAnimationsElementDestroyed(SvgAnimations* animations, PrDomElement* element);

// Drops what the animations hold of the elements of the subtree of `node`, which is about to leave the document, but
// for the time containers of its svg elements, which keep their time.
void svgAnimationsRemoving(SvgAnimations* animations, PrDomNode* node);

// Pause the time container of the outermost svg element that holds `svg`, or run it again. Return whether it is paused,
// and its time in seconds. Seek it to `seconds`, sampling its animations there at once.
void svgAnimationsPause(PrDomElement* svg, bool paused);
bool svgAnimationsPaused(PrDomElement* svg);
double svgAnimationsTime(PrDomElement* svg);
void svgAnimationsSeek(PrDomElement* svg, double seconds);

// Return the element that an animation element animates, or null. Return the begin of its current interval, or false if
// it has none. Return its container's time. Return its simple duration, or false if it is indefinite.
PrDomElement* PR_NULLABLE svgAnimationTarget(PrDomElement* animation);
bool svgAnimationStartTime(PrDomElement* animation, double* out);
double svgAnimationTime(PrDomElement* animation);
bool svgAnimationSimpleDuration(PrDomElement* animation, double* out);

// Adds a begin time, or an end time if `end`, `offset` seconds after the container's time, and samples at once.
void svgAnimationAddTime(PrDomElement* animation, bool end, double offset);

// Return the value that the animations give the attribute `name` of `element`, as its text or as the value that they
// compose, or the null view or null if none animates it. Each is valid until the next sample.
PrStringView svgAnimatedAttribute(const PrDomElement* element, PrStringView name);
const SvgAnimateValue* PR_NULLABLE svgAnimatedValue(const PrDomElement* element, PrStringView name);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
