// The tree-sitter language of Objective-C, which links the grammar into a program that calls it.

#pragma once

#include "pr/base.h"
#include "pr/syntax.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Returns the language of Objective-C, made on the first call and never destroyed.
const PrSyntaxLanguage* prSyntaxTreeSitterObjC(void);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
