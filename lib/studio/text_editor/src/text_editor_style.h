// The style classes of the editor: what one theme colors, whatever the language and whatever its grammar's queries call
// it, and the colors of the theme.

#pragma once

#include "pr/base.h"
#include "pr/color.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// A style class.
enum {
    TextEditorStyleClass_None,
    TextEditorStyleClass_Keyword,
    TextEditorStyleClass_Type,
    TextEditorStyleClass_Function,
    TextEditorStyleClass_Member,
    TextEditorStyleClass_Variable,
    TextEditorStyleClass_String,
    TextEditorStyleClass_Escape,
    TextEditorStyleClass_Number,
    TextEditorStyleClass_Constant,
    TextEditorStyleClass_Comment,
    TextEditorStyleClass_Attribute,
    TextEditorStyleClass_Operator,
    TextEditorStyleClass_Punctuation,
    TextEditorStyleClass_Heading,
    TextEditorStyleClass_Strong,
    TextEditorStyleClass_Emphasis,
    TextEditorStyleClass_Link,                  // the text that a link reads as
    TextEditorStyleClass_LinkTarget,            // where a link points
    TextEditorStyleClass_Inserted,              // a diff's added lines
    TextEditorStyleClass_Deleted,               // a diff's removed lines
    TextEditorStyleClass_Changed,               // a diff's changed lines
    TextEditorStyleClass_Count,
};
typedef uint8_t TextEditorStyleClass;

// Returns the class of a syntax category's name: by the leading words of a few names whose class differs from their
// first word's, such as `variable.member`, then by its first word, as both Neovim's queries and Helix's name
// categories, or for markup, which Neovim names under `text` and Helix under `markup`, by its second.
TextEditorStyleClass textEditorStyleClassFromCategory(PrStringView name);

// Returns the color of a class's text in a dark theme or a light one, or zero alpha for the text's own color.
PrColor16 textEditorStyleClassColor(TextEditorStyleClass style, bool dark);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
