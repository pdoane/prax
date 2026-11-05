// The tree-sitter language of CMake, which links the grammar into a program that calls it.

#pragma once

#include "pr/base.h"
#include "pr/syntax.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Returns the language of CMake, made on the first call and never destroyed.
const PrSyntaxLanguage* prSyntaxTreeSitterCMake(void);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
