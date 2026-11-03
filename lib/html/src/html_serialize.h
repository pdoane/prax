// Serializing HTML fragments
//
// html - https://html.spec.whatwg.org/multipage/parsing.html#serialising-html-fragments

#pragma once

#include "pr/dom.h"
#include "pr/text_buffer.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Appends the serialization of `node` itself to `out`. For an element, this is the serialization that
// html/#dom-element-outerhtml gives.
void htmlSerializeNode(PrTextBuffer* out, const PrDomNode* node);

// Appends the serialization of the children of `node` to `out` (html/#html-fragment-serialisation-algorithm). For a
// template, those are the children of its contents.
void htmlSerializeChildren(PrTextBuffer* out, const PrDomNode* node);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
