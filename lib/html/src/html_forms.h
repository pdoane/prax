// Forms: form controls, forms, and their entry lists
//
// html - https://html.spec.whatwg.org/multipage/forms.html

#pragma once

#include "pr/dom.h"
#include "pr/string_view.h"
#include "pr/text_buffer.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Appends a color input's value after the steps of html/#update-a-color-well-control-color, given `text`, the value or
// value attribute that the steps start from.
void htmlColorWellValue(const PrDomElement* input, PrStringView text, TextBuffer* out);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
