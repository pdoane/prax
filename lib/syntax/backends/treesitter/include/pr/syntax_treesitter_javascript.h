// The tree-sitter language of JavaScript, which links the grammar into a program that calls it.

#pragma once

#include "pr/base.h"
#include "pr/syntax.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Returns the language of JavaScript, made on the first call and never destroyed.
const PrSyntaxLanguage* prSyntaxTreeSitterJavaScript(void);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
