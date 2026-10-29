// JavaScript binding of forms.
//
// html - https://html.spec.whatwg.org/multipage/forms.html

#pragma once

#include "pr/html.h"

#include "quickjs.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Binds the classes of forms: the form controls, forms, FormData, and the events of forms. HTMLElement must be bound
// first.
void jsHtmlFormsBind(JSContext* ctx);

// Return the class of a form element or a form event, or zero for any other.
JSClassID jsHtmlFormsElementClass(PrDomElement* element);
JSClassID jsHtmlFormsEventClass(PrDomEvent* event);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
