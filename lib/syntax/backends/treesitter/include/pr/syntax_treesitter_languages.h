// Every tree-sitter language that the backend builds in, for a host that offers them all, which links every grammar. A
// host that offers some names each one's header instead.

#pragma once

#include "pr/base.h"
#include "pr/syntax.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Adds every built-in language to `registry`.
void prSyntaxTreeSitterAddLanguages(PrSyntaxRegistry* registry);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
