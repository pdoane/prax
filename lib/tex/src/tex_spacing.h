// TeX's spacing between atoms, laid out on a formula's MathML operators.

#pragma once

#include "tex/src/tex_parse.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Lays out TeX's spacing on a parsed tree (The TeXbook, Appendix G, rules 5, 6 and 20). In each list, the binary
// operators that TeX treats as ordinary become Ord, and an operator name gains the U+2061 that applies it to the next
// atom. Each pair of adjacent atoms gets the space that TeX's table gives their classes. The space goes to an
// operator's lspace or rspace, preferring the one that the operator dictionary already gives, or to an <mspace> if
// neither atom is an operator. Each operator's form is decided as MathML Core decides it, so that the writer can omit
// what the dictionary already says.
void texTreeSpace(TexTree* tree);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
