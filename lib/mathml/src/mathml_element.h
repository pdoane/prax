// MathML's element model: which elements are which, their in-flow children, and the layout that each element takes
//
// mathml-core - https://w3c.github.io/mathml-core/
// mathml4 - https://w3c.github.io/mathml/
//
// MathML Core defines the elements that it renders, and MathML 4 adds the presentation elements beyond them that the
// engine renders too (menclose, mfenced, mlabeledtr, the elementary math of mstack and mlongdiv). Any other element in
// the MathML namespace is an unknown MathML element, which renders as an mrow
// (mathml-core/#dfn-unknown-mathml-element).
//
// Layout decides which children are in flow from their boxes. A child whose display is none, or that is absolutely
// positioned, is not in flow (mathml-core/#layout-algorithms). The functions that read in-flow children take that
// decision as a MathMlFlow. The element model reads the DOM alone, so the presentation can call it on the document's
// thread while it projects boxes.

#pragma once

#include "pr/dom.h"
#include "pr/dom_tag.h"
#include "pr/raw_string.h"

#include "dom/src/dom_node.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// ---------------------------------------------------------------------------------------------------------------------
// Element sets

// Returns true if the node is an element in the MathML namespace.
inline bool mathMlIsElement(const PrDomNode* PR_NULLABLE node) {
    const PrDomElement* element = domElementFromNode(node);
    return element != nullptr && element->space == PrDomNamespace_MathMl;
}

// Returns true if the element is the MathML element `tag`.
inline bool mathMlIs(const PrDomElement* PR_NULLABLE element, PrDomTag tag) {
    return element != nullptr && element->space == PrDomNamespace_MathMl && element->tag == tag;
}

// Returns true if the MathML element is one that MathML Core defines (mathml-core/#dfn-mathml-core-elements).
bool mathMlIsCoreElement(const PrDomElement* element);

// Returns true if the MathML element is one that MathML Core or MathML 4 defines and the engine renders by its tag. An
// element that is neither is an unknown MathML element, which renders as an mrow.
bool mathMlIsKnownElement(const PrDomElement* element);

// Returns true if the MathML element is a grouping element (mathml-core/#dfn-grouping-elements), which lays out its
// in-flow children as an mrow does. An unknown MathML element is a grouping element.
bool mathMlIsGroupingElement(const PrDomElement* element);

// Returns true if the MathML element is a scripted element (mathml-core/#dfn-scripted-elements).
bool mathMlIsScriptedElement(const PrDomElement* element);

// Returns true if the MathML element is a radical element (mathml-core/#dfn-radical-elements).
bool mathMlIsRadicalElement(const PrDomElement* element);

// Returns true if the MathML element is a token element (mathml-core/#token-elements). A token's content is text and
// phrasing content, which CSS lays out (mathml-core/#layout-of-mtext). The annotation and annotation-xml elements lay
// out as an mtext does, but are not tokens.
bool mathMlIsTokenElement(const PrDomElement* element);

// Returns true if the MathML element wraps its in-flow children in an anonymous mrow box, as MathML Core specifies for
// some of its elements (mathml-core/#anonymous-mrow-boxes) and MathML 4 for menclose (mathml4/#presm_menclose).
bool mathMlWrapsInRow(const PrDomElement* element);

// ---------------------------------------------------------------------------------------------------------------------
// In-flow children

// What a child element is to its parent's math layout.
enum {
    MathMlChild_InFlow,
    MathMlChild_OutOfFlow,      // generates no box or an out-of-flow box, and is skipped
    MathMlChild_Contents,       // replaced by its own children, as display: contents does
};
typedef uint8_t MathMlChild;

// The caller's classification of the children of an element. `classify` says what a child element is, and is called
// with `context`. A null `classify` makes every child element in flow. Text and other nodes are never in-flow children
// of an element that is not a token.
struct MathMlFlow {
    MathMlChild (*PR_NULLABLE classify)(const void* PR_NULLABLE context, const PrDomElement* child);
    const void* PR_NULLABLE context;
};

// Returns the first in-flow child of `parent`, or the in-flow child after `child`, or null if there is none. A child
// classified as MathMlChild_Contents is passed through, and its in-flow children take its place among the parent's.
const PrDomElement* PR_NULLABLE mathMlFirstInFlow(const PrDomElement* parent, const MathMlFlow& flow);
const PrDomElement* PR_NULLABLE mathMlNextInFlow(const PrDomElement* parent, const PrDomElement* child,
    const MathMlFlow& flow);

// Returns the number of in-flow children of `parent`.
uint32_t mathMlInFlowCount(const PrDomElement* parent, const MathMlFlow& flow);

// Returns the in-flow child of `parent` at `index`, or null if it has fewer children.
const PrDomElement* PR_NULLABLE mathMlInFlowAt(const PrDomElement* parent, uint32_t index, const MathMlFlow& flow);

// ---------------------------------------------------------------------------------------------------------------------
// Layouts

// The layout algorithm of a MathML element whose display is block math or inline math. An element whose in-flow
// children make it invalid takes Row, because MathML Core lays out such an element as an mrow
// (mathml-core/#fractions-mfrac and the like).
enum {
    MathMlLayout_Row,               // mathml-core/#layout-of-mrow, for grouping elements and invalid elements
    MathMlLayout_Token,             // mathml-core/#layout-of-mtext: mi, mn, ms, mtext, annotation, annotation-xml
    MathMlLayout_Operator,          // mathml-core/#layout-of-operators: mo
    MathMlLayout_Space,             // mathml-core/#space-mspace
    MathMlLayout_Fraction,          // mathml-core/#fractions-mfrac, with exactly two in-flow children
    MathMlLayout_SquareRoot,        // mathml-core/#radicals-msqrt-mroot: msqrt, over its anonymous mrow
    MathMlLayout_Root,              // mathml-core/#radicals-msqrt-mroot: mroot, with a base and an index
    MathMlLayout_Padded,            // mathml-core/#adjust-space-around-content-mpadded, over its anonymous mrow
    MathMlLayout_Subscript,         // msub, with a base and a subscript
    MathMlLayout_Superscript,       // msup, with a base and a superscript
    MathMlLayout_SubSuperscript,    // msubsup, with a base, a subscript and a superscript
    MathMlLayout_Under,             // munder, with a base and an underscript
    MathMlLayout_Over,              // mover, with a base and an overscript
    MathMlLayout_UnderOver,         // munderover, with a base, an underscript and an overscript
    MathMlLayout_Multiscripts,      // a valid mmultiscripts (mathMlMultiscripts)
    MathMlLayout_Enclose,           // menclose, over its anonymous mrow (mathml4/#presm_menclose)
    MathMlLayout_Fenced,            // mfenced, its children between fences and separators (mathml4/#presm_mfenced)
    MathMlLayout_Stack,             // mstack (mathml4/#presm_mstack)
    MathMlLayout_LongDivision,      // mlongdiv, with a divisor, a result and the steps (mathml4/#presm_mlongdiv)
    MathMlLayout_StackLine,         // msline, a horizontal line in a stack (mathml4/#presm_msline)
    MathMlLayout_Glyph,             // mglyph, an image that stands for a character (MathML 3)
    MathMlLayout_Table,             // mtable, mtr, mlabeledtr and mtd, which CSS's table layout lays out
};
typedef uint8_t MathMlLayout;

// Returns the layout that the MathML element takes for its in-flow children. MathML's layouts apply only if the
// element's display is block math or inline math, which the caller decides from the element's style.
MathMlLayout mathMlLayout(const PrDomElement* element, const MathMlFlow& flow);

// The in-flow children of a valid mmultiscripts (mathml-core/#prescripts-and-tensor-indices-mmultiscripts): its base,
// then its postscripts and its prescripts, each a list of subscript and superscript pairs.
struct MathMlMultiscripts {
    const PrDomElement* PR_NULLABLE base;
    const PrDomElement* PR_NULLABLE prescripts;     // the mprescripts element, or null if there is none
    uint32_t postscriptPairs;
    uint32_t prescriptPairs;
};

// Writes the parts of a valid mmultiscripts to `out`. Returns false if `element` is not a valid mmultiscripts.
bool mathMlMultiscripts(const PrDomElement* element, const MathMlFlow& flow, MathMlMultiscripts* out);

// ---------------------------------------------------------------------------------------------------------------------
// Embellished operators and space-like elements

// Returns true if the MathML element is space-like (mathml-core/#dfn-space-like): an mtext or an mspace, or a grouping
// element or an mpadded all of whose in-flow children are space-like.
bool mathMlIsSpaceLike(const PrDomElement* element, const MathMlFlow& flow);

// Returns the core operator of the element if it is an embellished operator (mathml-core/#dfn-embellished-operator), or
// null if it is not one. An mo is its own core operator. A scripted element or an mfrac is an embellished operator if
// its first in-flow child is one. A grouping element or an mpadded is one if its in-flow children are one embellished
// operator and any number of space-like elements.
const PrDomElement* PR_NULLABLE mathMlCoreOperator(const PrDomElement* element, const MathMlFlow& flow);

// Returns true if the MathML element is an embellished operator.
inline bool mathMlIsEmbellishedOperator(const PrDomElement* element, const MathMlFlow& flow) {
    return mathMlCoreOperator(element, flow) != nullptr;
}

// ---------------------------------------------------------------------------------------------------------------------
// Text

// Writes to `out` the text of a token element whose content is text alone. The text is the data of its Text children,
// with the white space at either end removed and each run of white space within collapsed to one space
// (mathml4/#fund_collapse). Returns false if the element has a child element. The caller releases `out` with
// prRawStringReset.
bool mathMlTokenText(const PrDomElement* element, PrRawString* out);

// Returns the code point of a token element whose text (mathMlTokenText) is a single code point, or 0 if its text is
// anything else.
uint32_t mathMlTokenCodePoint(const PrDomElement* element);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
