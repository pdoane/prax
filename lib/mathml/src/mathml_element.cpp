// MathML's element model: which elements are which, their in-flow children, and the layout that each element takes.

#include "mathml/src/mathml_element.h"

#include "pr/text_buffer.h"
#include "pr/utf8.h"

namespace pr_impl {

namespace {

// Returns true if the element's tag is one of `tags`.
template <size_t N>
bool isOneOf(const PrDomElement* element, const PrDomTag (&tags)[N]) {
    for (PrDomTag tag : tags) {
        if (element->tag == tag)
            return true;
    }
    return false;
}

// The elements of MathML Core (mathml-core/#dfn-mathml-core-elements).
constexpr PrDomTag kCoreElements[] = {
    PrDomTag_A, PrDomTag_Annotation, PrDomTag_AnnotationXml, PrDomTag_Maction, PrDomTag_Math, PrDomTag_Merror,
    PrDomTag_Mfrac, PrDomTag_Mi, PrDomTag_Mmultiscripts, PrDomTag_Mn, PrDomTag_Mo, PrDomTag_Mover, PrDomTag_Mpadded,
    PrDomTag_Mphantom, PrDomTag_Mprescripts, PrDomTag_Mroot, PrDomTag_Mrow, PrDomTag_Ms, PrDomTag_Mspace,
    PrDomTag_Msqrt, PrDomTag_Mstyle, PrDomTag_Msub, PrDomTag_Msubsup, PrDomTag_Msup, PrDomTag_Mtable, PrDomTag_Mtd,
    PrDomTag_Mtext, PrDomTag_Mtr, PrDomTag_Munder, PrDomTag_Munderover, PrDomTag_Semantics,
};

// The presentation elements of MathML 4 beyond Core that the engine renders by their tags. maligngroup and malignmark
// mark alignment points, which the engine does not align by, so they stay unknown elements and render as empty mrows.
constexpr PrDomTag kFullElements[] = {
    PrDomTag_Menclose, PrDomTag_Mfenced, PrDomTag_Mglyph, PrDomTag_Mlabeledtr, PrDomTag_Mlongdiv, PrDomTag_Mscarries,
    PrDomTag_Mscarry, PrDomTag_Msgroup, PrDomTag_Msline, PrDomTag_Msrow, PrDomTag_Mstack,
};

// Returns the child's classification, which a null `classify` makes in flow.
MathMlChild classified(const PrDomElement* child, const MathMlFlow& flow) {
    return flow.classify != nullptr ? flow.classify(flow.context, child) : MathMlChild_InFlow;
}

// Returns the first in-flow element at or after `node` in the children of `node`'s parent, descending into the children
// that pass their children through and climbing back out of them, up to `parent`.
const PrDomElement* PR_NULLABLE inFlowFrom(const PrDomElement* parent, const PrDomNode* PR_NULLABLE node,
        const MathMlFlow& flow) {
    while (node != nullptr) {
        const PrDomElement* element = domElementFromNode(node);
        if (element != nullptr) {
            MathMlChild kind = classified(element, flow);
            if (kind == MathMlChild_InFlow)
                return element;
            if (kind == MathMlChild_Contents && element->first != nullptr) {
                node = element->first;
                continue;
            }
        }
        // Past the last child of a passed-through element, climb out of it.
        while (node->next == nullptr && node->parent != parent && node->parent != nullptr)
            node = node->parent;
        node = node->next;
    }
    return nullptr;
}

// Returns true if `element` has exactly `count` in-flow children.
bool hasInFlow(const PrDomElement* element, uint32_t count, const MathMlFlow& flow) {
    return mathMlInFlowCount(element, flow) == count;
}

// Returns true if `c` is MathML's white space (mathml4/#fund_collapse).
bool isSpace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

} // namespace

bool mathMlIsCoreElement(const PrDomElement* element) {
    return element->space == PrDomNamespace_MathMl && isOneOf(element, kCoreElements);
}

bool mathMlIsKnownElement(const PrDomElement* element) {
    return mathMlIsCoreElement(element) || (element->space == PrDomNamespace_MathMl && isOneOf(element, kFullElements));
}

bool mathMlIsGroupingElement(const PrDomElement* element) {
    static constexpr PrDomTag kGrouping[] = {
        PrDomTag_A, PrDomTag_Maction, PrDomTag_Math, PrDomTag_Merror, PrDomTag_Mphantom, PrDomTag_Mprescripts,
        PrDomTag_Mrow, PrDomTag_Mstyle, PrDomTag_Semantics,
    };
    if (element->space != PrDomNamespace_MathMl)
        return false;
    return isOneOf(element, kGrouping) || !mathMlIsKnownElement(element);
}

bool mathMlIsScriptedElement(const PrDomElement* element) {
    static constexpr PrDomTag kScripted[] = {
        PrDomTag_Mmultiscripts, PrDomTag_Mover, PrDomTag_Msub, PrDomTag_Msubsup, PrDomTag_Msup, PrDomTag_Munder,
        PrDomTag_Munderover,
    };
    return element->space == PrDomNamespace_MathMl && isOneOf(element, kScripted);
}

bool mathMlIsRadicalElement(const PrDomElement* element) {
    return mathMlIs(element, PrDomTag_Msqrt) || mathMlIs(element, PrDomTag_Mroot);
}

bool mathMlIsTokenElement(const PrDomElement* element) {
    static constexpr PrDomTag kTokens[] = {
        PrDomTag_Mi, PrDomTag_Mn, PrDomTag_Mo, PrDomTag_Ms, PrDomTag_Mspace, PrDomTag_Mtext,
    };
    return element->space == PrDomNamespace_MathMl && isOneOf(element, kTokens);
}

bool mathMlWrapsInRow(const PrDomElement* element) {
    static constexpr PrDomTag kWrapping[] = {PrDomTag_Menclose, PrDomTag_Mpadded, PrDomTag_Msqrt, PrDomTag_Mtd};
    return element->space == PrDomNamespace_MathMl && isOneOf(element, kWrapping);
}

const PrDomElement* PR_NULLABLE mathMlFirstInFlow(const PrDomElement* parent, const MathMlFlow& flow) {
    return parent->first != nullptr ? inFlowFrom(parent, parent->first, flow) : nullptr;
}

const PrDomElement* PR_NULLABLE mathMlNextInFlow(const PrDomElement* parent, const PrDomElement* child,
        const MathMlFlow& flow) {
    const PrDomNode* node = child;
    while (node->next == nullptr && node->parent != parent && node->parent != nullptr)
        node = node->parent;
    return node->next != nullptr ? inFlowFrom(parent, node->next, flow) : nullptr;
}

uint32_t mathMlInFlowCount(const PrDomElement* parent, const MathMlFlow& flow) {
    uint32_t count = 0;
    for (const PrDomElement* child = mathMlFirstInFlow(parent, flow); child != nullptr;
            child = mathMlNextInFlow(parent, child, flow))
        ++count;
    return count;
}

const PrDomElement* PR_NULLABLE mathMlInFlowAt(const PrDomElement* parent, uint32_t index, const MathMlFlow& flow) {
    const PrDomElement* child = mathMlFirstInFlow(parent, flow);
    for (uint32_t i = 0; i < index && child != nullptr; ++i)
        child = mathMlNextInFlow(parent, child, flow);
    return child;
}

MathMlLayout mathMlLayout(const PrDomElement* element, const MathMlFlow& flow) {
    if (element->space != PrDomNamespace_MathMl)
        return MathMlLayout_Row;
    switch (element->tag) {
        case PrDomTag_Mi:
        case PrDomTag_Mn:
        case PrDomTag_Ms:
        case PrDomTag_Mtext:
        case PrDomTag_Annotation:
        case PrDomTag_AnnotationXml:
            return MathMlLayout_Token;
        case PrDomTag_Mo: return MathMlLayout_Operator;
        case PrDomTag_Mspace: return MathMlLayout_Space;
        case PrDomTag_Mfrac: return hasInFlow(element, 2, flow) ? MathMlLayout_Fraction : MathMlLayout_Row;
        case PrDomTag_Msqrt: return MathMlLayout_SquareRoot;
        case PrDomTag_Mroot: return hasInFlow(element, 2, flow) ? MathMlLayout_Root : MathMlLayout_Row;
        case PrDomTag_Mpadded: return MathMlLayout_Padded;
        case PrDomTag_Msub: return hasInFlow(element, 2, flow) ? MathMlLayout_Subscript : MathMlLayout_Row;
        case PrDomTag_Msup: return hasInFlow(element, 2, flow) ? MathMlLayout_Superscript : MathMlLayout_Row;
        case PrDomTag_Msubsup: return hasInFlow(element, 3, flow) ? MathMlLayout_SubSuperscript : MathMlLayout_Row;
        case PrDomTag_Munder: return hasInFlow(element, 2, flow) ? MathMlLayout_Under : MathMlLayout_Row;
        case PrDomTag_Mover: return hasInFlow(element, 2, flow) ? MathMlLayout_Over : MathMlLayout_Row;
        case PrDomTag_Munderover: return hasInFlow(element, 3, flow) ? MathMlLayout_UnderOver : MathMlLayout_Row;
        case PrDomTag_Mmultiscripts: {
            MathMlMultiscripts parts{};
            return mathMlMultiscripts(element, flow, &parts) ? MathMlLayout_Multiscripts : MathMlLayout_Row;
        }
        case PrDomTag_Menclose: return MathMlLayout_Enclose;
        case PrDomTag_Mfenced: return MathMlLayout_Fenced;
        case PrDomTag_Mstack: return MathMlLayout_Stack;
        // mlongdiv's first two in-flow children are the divisor and the result (mathml4/#presm_mlongdiv).
        case PrDomTag_Mlongdiv:
            return mathMlInFlowCount(element, flow) >= 2 ? MathMlLayout_LongDivision : MathMlLayout_Row;
        case PrDomTag_Msline: return MathMlLayout_StackLine;
        case PrDomTag_Mglyph: return MathMlLayout_Glyph;
        case PrDomTag_Mtable:
        case PrDomTag_Mtr:
        case PrDomTag_Mlabeledtr:
        case PrDomTag_Mtd:
            return MathMlLayout_Table;
        default: return MathMlLayout_Row;
    }
}

bool mathMlMultiscripts(const PrDomElement* element, const MathMlFlow& flow, MathMlMultiscripts* out) {
    *out = MathMlMultiscripts{};
    if (!mathMlIs(element, PrDomTag_Mmultiscripts))
        return false;
    const PrDomElement* child = mathMlFirstInFlow(element, flow);
    if (child == nullptr || mathMlIs(child, PrDomTag_Mprescripts))
        return false;
    out->base = child;
    uint32_t scripts[2] = {};
    for (child = mathMlNextInFlow(element, child, flow); child != nullptr; child = mathMlNextInFlow(element, child, flow)) {
        if (mathMlIs(child, PrDomTag_Mprescripts)) {
            if (out->prescripts != nullptr)
                return false;
            out->prescripts = child;
            continue;
        }
        ++scripts[out->prescripts != nullptr ? 1 : 0];
    }
    if (scripts[0] % 2 != 0 || scripts[1] % 2 != 0)
        return false;
    out->postscriptPairs = scripts[0] / 2;
    out->prescriptPairs = scripts[1] / 2;
    return true;
}

bool mathMlIsSpaceLike(const PrDomElement* element, const MathMlFlow& flow) {
    if (mathMlIs(element, PrDomTag_Mtext) || mathMlIs(element, PrDomTag_Mspace))
        return true;
    if (!mathMlIsGroupingElement(element) && !mathMlIs(element, PrDomTag_Mpadded))
        return false;
    for (const PrDomElement* child = mathMlFirstInFlow(element, flow); child != nullptr;
            child = mathMlNextInFlow(element, child, flow)) {
        if (!mathMlIsSpaceLike(child, flow))
            return false;
    }
    return true;
}

const PrDomElement* PR_NULLABLE mathMlCoreOperator(const PrDomElement* element, const MathMlFlow& flow) {
    if (element->space != PrDomNamespace_MathMl)
        return nullptr;
    if (element->tag == PrDomTag_Mo)
        return element;
    if (mathMlIsScriptedElement(element) || element->tag == PrDomTag_Mfrac) {
        const PrDomElement* first = mathMlFirstInFlow(element, flow);
        return first != nullptr ? mathMlCoreOperator(first, flow) : nullptr;
    }
    if (!mathMlIsGroupingElement(element) && element->tag != PrDomTag_Mpadded)
        return nullptr;
    const PrDomElement* core = nullptr;
    for (const PrDomElement* child = mathMlFirstInFlow(element, flow); child != nullptr;
            child = mathMlNextInFlow(element, child, flow)) {
        if (const PrDomElement* found = mathMlCoreOperator(child, flow)) {
            if (core != nullptr)
                return nullptr;
            core = found;
        } else if (!mathMlIsSpaceLike(child, flow)) {
            return nullptr;
        }
    }
    return core;
}

bool mathMlTokenText(const PrDomElement* element, PrRawString* out) {
    *out = prRawStringNull();
    PrTextBuffer text{};
    bool space = false;
    for (const PrDomNode* node = element->first; node != nullptr; node = node->next) {
        if (node->type == PrDomNodeType_Element) {
            prTextBufferReset(&text);
            return false;
        }
        if (!domIsText(node))
            continue;
        PrStringView data = prRawStringView(&static_cast<const PrDomCharacterData*>(node)->data);
        for (size_t i = 0; i < data.size; ++i) {
            if (isSpace(data.data[i])) {
                space = text.size != 0;
                continue;
            }
            if (space)
                prTextBufferAppendChar(&text, ' ');
            space = false;
            prTextBufferAppendChar(&text, data.data[i]);
        }
    }
    *out = text.size != 0 ? prTextBufferTake(&text) : prRawStringEmpty();
    prTextBufferReset(&text);
    return true;
}

uint32_t mathMlTokenCodePoint(const PrDomElement* element) {
    PrRawString text{};
    if (!mathMlTokenText(element, &text))
        return 0;
    PrStringView view = prRawStringView(&text);
    uint32_t codePoint = 0;
    uint32_t length = view.size != 0 ? prUtf8Decode(view, 0, &codePoint) : 0;
    if (length != view.size)
        codePoint = 0;
    prRawStringReset(&text);
    return codePoint;
}

} // namespace pr_impl
