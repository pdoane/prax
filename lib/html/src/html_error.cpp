// Parse errors
//
// html/#parse-errors - https://html.spec.whatwg.org/multipage/parsing.html#parse-errors

#include "html/src/html_error.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

extern const PrStringView kHtmlErrorName[HtmlErrorCode_Count] = {
    "abrupt-closing-of-empty-comment",
    "abrupt-doctype-public-identifier",
    "abrupt-doctype-system-identifier",
    "absence-of-digits-in-numeric-character-reference",
    "cdata-in-html-content",
    "character-reference-outside-unicode-range",
    "control-character-in-input-stream",
    "control-character-reference",
    "disallowed-processing-instruction-target",
    "duplicate-attribute",
    "end-tag-with-attributes",
    "end-tag-with-trailing-solidus",
    "eof-before-tag-name",
    "eof-in-cdata",
    "eof-in-comment",
    "eof-in-doctype",
    "eof-in-processing-instruction",
    "eof-in-script-html-comment-like-text",
    "eof-in-tag",
    "incorrectly-closed-comment",
    "incorrectly-opened-comment",
    "invalid-character-sequence-after-doctype-name",
    "invalid-first-character-of-processing-instruction-target",
    "invalid-first-character-of-tag-name",
    "invalid-processing-instruction-target",
    "missing-attribute-value",
    "missing-doctype-name",
    "missing-doctype-public-identifier",
    "missing-doctype-system-identifier",
    "missing-end-tag-name",
    "missing-quote-before-doctype-public-identifier",
    "missing-quote-before-doctype-system-identifier",
    "missing-semicolon-after-character-reference",
    "missing-whitespace-after-doctype-public-keyword",
    "missing-whitespace-after-doctype-system-keyword",
    "missing-whitespace-before-doctype-name",
    "missing-whitespace-between-attributes",
    "missing-whitespace-between-doctype-public-and-system-identifiers",
    "nested-comment",
    "non-void-html-element-start-tag-with-trailing-solidus",
    "noncharacter-character-reference",
    "noncharacter-in-input-stream",
    "null-character-reference",
    "surrogate-character-reference",
    "surrogate-in-input-stream",
    "unexpected-character-after-doctype-system-identifier",
    "unexpected-character-in-attribute-name",
    "unexpected-character-in-unquoted-attribute-value",
    "unexpected-equals-sign-before-attribute-name",
    "unexpected-null-character",
    "unexpected-solidus-in-tag",
    "unknown-named-character-reference",
    {nullptr, 0},   // HtmlErrorCode_Unnamed
};

} // namespace pr_impl

PR_ASSUME_NONNULL_END
