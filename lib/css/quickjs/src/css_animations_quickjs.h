// The bindings of Web Animations (pr/css_animations.h): the classes of timelines, effects, animations and their events,
// and the members that they add to Element, Document and ShadowRoot.

#pragma once

#include "pr/base.h"

#include "quickjs.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Defines the classes, after the classes of the DOM, and adds the members to the DOM's interfaces.
void cssAnimationsBind(JSContext* ctx);

// Return the class of an EventTarget that is an animation, or of an event that is an AnimationPlaybackEvent, or 0 if it
// is neither.
JSClassID cssAnimationsTargetClass(void* target);
JSClassID cssAnimationsEventClass(void* event);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
