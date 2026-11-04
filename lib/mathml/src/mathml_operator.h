// MathML's operators: the dictionary's categories, the form of an embellished operator, and the properties that its
// core operator's attributes and the dictionary resolve to
//
// mathml-core - https://w3c.github.io/mathml-core/
//
// The properties of an embellished operator come from the attributes of its core operator, and otherwise from the
// operator dictionary entry of the core operator's content in the embellished operator's form
// (mathml-core/#dictionary-based-attributes). The form comes from the position of the embellished operator among the
// in-flow children of its parent, so the caller passes the outermost element of the embellished operator, which is the
// child of the element whose layout spaces it.

#pragma once

#include "pr/dom.h"
#include "pr/string_view.h"

#include "dom/src/dom_node.h"
#include "mathml/src/mathml_attribute.h"
#include "mathml/src/mathml_dictionary.h"
#include "mathml/src/mathml_element.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Returns the category of an operator whose content is `content`, UTF-8, in `form`, by the algorithm to determine the
// category of an operator (mathml-core/#operator-dictionary). Content of one or two UTF-16 code units is looked up, and
// a second character of U+0338 or U+20D2 is dropped first.
MathMlOperatorCategory mathMlOperatorCategory(PrStringView content, MathMlForm form);

// Returns the form of the embellished operator `element` (mathml-core/#dfn-form). The form attribute of its core
// operator decides it if present. Otherwise the operator is prefix if it is the first and postfix if it is the last of
// more than one in-flow child of a grouping element, an mpadded or an element that wraps its children in an anonymous
// mrow, ignoring space-like children. A script is postfix, and any other operator is infix. Writes to `explicitForm`
// whether the form attribute gave the form.
MathMlForm mathMlOperatorForm(const PrDomElement* element, const MathMlFlow& flow, bool* PR_NULLABLE explicitForm);

// The axis that an operator stretches along (mathml-core/#dfn-stretch-axis).
enum {
    MathMlStretchAxis_Block,
    MathMlStretchAxis_Inline,
};
typedef uint8_t MathMlStretchAxis;

// The properties of an embellished operator (mathml-core/#dictionary-based-attributes).
//
// The spaces are the dictionary's, in eighteenths of an em of the core operator's font, unless the core operator's
// lspace or rspace attribute is present, whose percentage is of the dictionary's space. The sizes are the attributes'.
// An absent minsize is 100% and an absent maxsize is infinite. Their percentages are of the operator's unstretched size
// (mathml-core/#algorithm-for-stretching-operators-along-the-block-axis). The font-relative lengths of every attribute
// are of the core operator's font, not of the embellished operator's.
struct MathMlOperator {
    const PrDomElement* PR_NULLABLE core;
    uint32_t codePoint;             // the core operator's content if it is a single code point, or 0
    MathMlForm form;
    bool explicitForm;              // the core operator's form attribute gave the form
    MathMlOperatorCategory category;
    MathMlOperatorFlags flags;
    MathMlStretchAxis axis;
    MathMlBoolean accent;           // the core operator's accent attribute, else the dictionary's (mathml4/#presm_mo)
    uint8_t lspace;                 // the dictionary's leading space, in eighteenths of an em
    uint8_t rspace;                 // the dictionary's trailing space, in eighteenths of an em
    MathMlLength lspaceAttribute;
    MathMlLength rspaceAttribute;
    MathMlLength minsize;
    MathMlLength maxsize;
};

// Writes the properties of the embellished operator `element` to `out`. Returns false if `element` is not an
// embellished operator, and `out` is then meaningless.
bool mathMlOperator(const PrDomElement* element, const MathMlFlow& flow, MathMlOperator* out);

// Returns the attributes of the munder, mover or munderover `element`, with an absent accent or accentunder resolved as
// MathML 4 resolves it (mathml4/#presm_munder, mathml4/#presm_mover). An absent attribute takes the accent of the core
// operator of its script if the script is an embellished operator, and is false otherwise.
MathMlUnderOver mathMlUnderOverAccents(const PrDomElement* element, const MathMlFlow& flow);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
