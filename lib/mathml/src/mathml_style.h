// MathML styles: the user agent's sheet.
//
// mathml-core - https://w3c.github.io/mathml-core/

#pragma once

#include "pr/base.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// The user agent style sheet for MathML content (mathml-core/#user-agent-stylesheet), as MathML Core writes it, with
// two additions. MathML 4's labeled row, mlabeledtr (mathml4/#presm_mtr), is laid out as a table row, and the carries
// of elementary math are set as scripts are.
constexpr char kMathMlStyle[] = "@namespace url(http://www.w3.org/1998/Math/MathML);\n"
    // Universal rules
    "* { font-size: math; display: block math; writing-mode: horizontal-tb !important; }\n"
    // The math element
    "math { direction: ltr; text-indent: 0; letter-spacing: normal; line-height: normal; word-spacing: normal;"
    " font-family: math; font-size: inherit; font-style: normal; font-weight: normal; display: inline math;"
    " math-shift: normal; math-style: compact; math-depth: 0; }\n"
    "math[display=\"block\" i] { display: block math; math-style: normal; }\n"
    "math[display=\"inline\" i] { display: inline math; math-style: compact; }\n"
    // mrow-like elements
    "semantics > :not(:first-child) { display: none; }\n"
    "maction > :not(:first-child) { display: none; }\n"
    "merror { border: 1px solid red; background-color: lightYellow; }\n"
    "mphantom { visibility: hidden; }\n"
    // Elementary math, whose carries are set as scripts are (mathml4/#presm_mscarries)
    "mscarries { math-depth: add(1); math-style: compact; }\n"
    // Token elements
    "mi { text-transform: math-auto; }\n"
    // Tables
    "mtable { display: inline-table; math-style: compact; }\n"
    "mtr, mlabeledtr { display: table-row; }\n"
    "mtd { display: table-cell; text-align: center; padding: 0.5ex 0.4em; }\n"
    // Fractions
    "mfrac { padding-inline: 1px; }\n"
    "mfrac > * { math-depth: auto-add; math-style: compact; }\n"
    "mfrac > :nth-child(2) { math-shift: compact; }\n"
    // Other rules for scriptlevel, displaystyle and math-shift
    "mroot > :not(:first-child) { math-depth: add(2); math-style: compact; }\n"
    "mroot, msqrt { math-shift: compact; }\n"
    "msub > :not(:first-child), msup > :not(:first-child), msubsup > :not(:first-child),"
    " mmultiscripts > :not(:first-child), munder > :not(:first-child), mover > :not(:first-child),"
    " munderover > :not(:first-child) { math-depth: add(1); math-style: compact; }\n"
    "munder[accentunder=\"true\" i] > :nth-child(2), mover[accent=\"true\" i] > :nth-child(2),"
    " munderover[accentunder=\"true\" i] > :nth-child(2), munderover[accent=\"true\" i] > :nth-child(3)"
    " { font-size: inherit; }\n"
    "msub > :nth-child(2), msubsup > :nth-child(2), mmultiscripts > :nth-child(even),"
    " mmultiscripts > mprescripts ~ :nth-child(odd), mover[accent=\"true\" i] > :first-child,"
    " munderover[accent=\"true\" i] > :first-child { math-shift: compact; }\n"
    "mmultiscripts > mprescripts ~ :nth-child(even) { math-shift: inherit; }\n";

} // namespace pr_impl

PR_ASSUME_NONNULL_END
