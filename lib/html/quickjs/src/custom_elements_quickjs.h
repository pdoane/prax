// JavaScript binding of custom elements.
//
// html - https://html.spec.whatwg.org/multipage/custom-elements.html

#pragma once

#include "pr/html.h"

#include "quickjs.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Binds the classes of custom elements.
void jsCustomElementsBind(JSContext* ctx);

// Creates the custom element registry of `window`, whose JavaScript object is `object`
// (html/#dom-window-customelements). From then on, the registry defines the custom elements of each document of the
// window. Returns false, with an exception pending, if the registry cannot be created.
bool jsCustomElementsSetGlobal(JSContext* ctx, JSValueConst object, PrHtmlWindow* window);

// Returns the custom element registry of the window whose object is `object` (html/#dom-window-customelements).
JSValue jsCustomElements(JSContext* ctx, JSValueConst object);

// Constructs an autonomous custom element from the current global's registry (html/#html-element-constructors).
JSValue jsCustomElementsConstruct(JSContext* ctx, JSValueConst newTarget, int argc,
                                  JSValueConst* argv);

// Attaches the ElementInternals of the custom element `self`, and returns it (html/#dom-attachinternals).
JSValue jsCustomElementsAttachInternals(JSContext* ctx, JSValueConst self, int argc,
                                        JSValueConst* argv);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
