// Syntax definitions
//
// css-properties-values-api-1 - https://drafts.css-houdini.org/css-properties-values-api-1/

#pragma once

#include "pr/string_view.h"
#include "pr/text_buffer.h"

#include "css/src/css_value_def.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

struct CssParser;

// How a syntax component repeats (css-properties-values-api-1/#multipliers).
enum {
    CssSyntaxMultiplier_None,
    CssSyntaxMultiplier_Space,   // +
    CssSyntaxMultiplier_Comma,   // #
};
typedef uint8_t CssSyntaxMultiplier;

// A syntax component (css-properties-values-api-1/#syntax-component): a data type, or an ident whose value is in the
// definition's `idents`.
struct CssSyntaxComponent {
    CssValueDef type;          // CssValueDef_Count for an ident
    CssSyntaxMultiplier multiplier;
    uint32_t identStart;       // the offset of an ident's first byte in `idents`
    uint32_t identSize;
};

// A syntax definition (css-properties-values-api-1/#syntax-definition): the universal one, or a list of components in
// written order. A value must match one of the components.
struct CssSyntaxDefinition {
    Array<CssSyntaxComponent> components;
    TextBuffer idents;          // the bytes of the components' idents
    bool universal;             // true for `*`, which accepts any value
};

// Frees the definition's memory and empties it.
void cssSyntaxDefinitionReset(CssSyntaxDefinition* definition);

// Parses `text` as a syntax string (css-properties-values-api-1/#consume-a-syntax-definition). Returns false, leaving
// `out` empty, if `text` is not one.
bool cssSyntaxDefinitionParse(CssParser* parser, PrStringView text, CssSyntaxDefinition* out);

// Returns true if a registration of `definition` accepts `value` as its initial value
// (css-properties-values-api-1/#the-registerproperty-function). The value must parse by the definition and be
// computationally independent (css-properties-values-api-1/#computationally-independent).
bool cssSyntaxInitialValueValid(CssParser* parser, const CssSyntaxDefinition* definition,
                                PrStringView value);

// Returns true if `value` parses by `definition` (css-values-5/#parse-with-a-syntax). A CSS-wide keyword does not.
bool cssSyntaxValueMatches(CssParser* parser, const CssSyntaxDefinition* definition, PrStringView value);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
