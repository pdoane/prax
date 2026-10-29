// HTML's subscription to a document
//
// html - https://html.spec.whatwg.org/

#pragma once

#include "pr/dom.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Subscribes HTML to the changes of the document that HTML's elements follow (dom_hooks.h), which a document that the
// DOM creates from it inherits. The links that the document already holds take their sheets as if the parser had just
// inserted them. Does nothing for a document that HTML follows.
void htmlEnable(PrDomDocument* document);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
