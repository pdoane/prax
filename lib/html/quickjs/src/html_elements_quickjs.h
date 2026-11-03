// JavaScript binding of the element interfaces of content, tables and select controls.
//
// html - https://html.spec.whatwg.org/multipage/semantics.html

#pragma once

#include "pr/html.h"

#include "quickjs.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Defines a legacy factory function on the global, whose prototype is that of `constructor`
// (webidl/#create-a-legacy-factory-function).
void jsHtmlDefineFactory(JSContext* ctx, const char* name, JSCFunction* function, int length, JSValueConst constructor);

// Binds the classes of these interfaces. The class of HTMLElement is bound first.
void jsHtmlElementsBind(JSContext* ctx);

// Returns the class of an HTML element of these interfaces, or zero for any other.
JSClassID jsHtmlElementsClass(PrDomElement* element);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
