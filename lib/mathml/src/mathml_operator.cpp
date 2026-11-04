// MathML's operators: the dictionary's categories, the form of an embellished operator, and its properties.

#include "mathml/src/mathml_operator.h"

#include "pr/utf8.h"

namespace pr_impl {

namespace {

// The maximum number of code points in a dictionary's content, which is at most two UTF-16 code units.
constexpr uint32_t kMaxContent = 2;

// Writes the code points of `content` to `out`, up to kMaxContent, and returns their number in UTF-16 code units, which
// is more than two for content that the dictionary cannot hold.
uint32_t contentOf(PrStringView content, uint32_t out[PR_NONNULL kMaxContent], uint32_t* count) {
    uint32_t units = 0;
    *count = 0;
    for (size_t at = 0; at < content.size && units <= 2;) {
        uint32_t codePoint = 0;
        at += prUtf8Decode(content, at, &codePoint);
        units += codePoint > 0xffff ? 2 : 1;
        if (*count < kMaxContent)
            out[(*count)++] = codePoint;
    }
    return units;
}

// Returns true if the element gives the embellished operators among its children forms by their position in a row. Such
// an element is a grouping element, an mpadded, or an element that wraps its children in an anonymous mrow
// (mathml-core/#anonymous-mrow-boxes), which Core treats as a grouping element.
bool isRowParent(const PrDomElement* element) {
    return mathMlIsGroupingElement(element) || mathMlIs(element, PrDomTag_Mpadded) || mathMlWrapsInRow(element);
}

// Returns the category of (`content`, `form`). If that is Default and `fallback` is set, returns the category under the
// first of infix, postfix and prefix that the dictionary lists (mathml-core/#dictionary-based-attributes).
MathMlOperatorCategory categoryOf(PrStringView content, MathMlForm form, bool fallback) {
    MathMlOperatorCategory category = mathMlOperatorCategory(content, form);
    constexpr MathMlForm kFallbacks[] = {MathMlForm_Infix, MathMlForm_Postfix, MathMlForm_Prefix};
    for (uint32_t i = 0; i < 3 && fallback && category == MathMlOperatorCategory_Default; ++i)
        category = mathMlOperatorCategory(content, kFallbacks[i]);
    return category;
}

} // namespace

MathMlOperatorCategory mathMlOperatorCategory(PrStringView content, MathMlForm form) {
    uint32_t codePoints[kMaxContent] = {};
    uint32_t count = 0;
    uint32_t units = contentOf(content, codePoints, &count);
    if (units != 1 && units != 2)
        return MathMlOperatorCategory_Default;
    uint32_t key = codePoints[0];
    if (units == 1 && key >= 0x320 && key <= 0x3ff)
        return MathMlOperatorCategory_Default;
    if (units == 2) {
        if (count == 1) {
            // The dictionary lists only two operators outside the BMP, both Arabic and postfix.
            if ((key == 0x1eef0 || key == 0x1eef1) && form == MathMlForm_Postfix)
                return MathMlOperatorCategory_I;
            return MathMlOperatorCategory_Default;
        }
        if (codePoints[1] == 0x338 || codePoints[1] == 0x20d2) {
            // The negated forms of an operator take its category.
        } else if (key < 0x80 && codePoints[1] < 0x80) {
            key = mathMlDictionaryAsciiPair(char(key), char(codePoints[1]));
            if (key == 0)
                return MathMlOperatorCategory_Default;
        } else {
            return MathMlOperatorCategory_Default;
        }
    }
    if (form == MathMlForm_Infix && (key == 0x7c || key == 0x223c))
        return MathMlOperatorCategory_ForceDefault;
    if (key > 0x3ff && (key < 0x2000 || key > 0x2bff))
        return MathMlOperatorCategory_Default;
    return mathMlDictionaryCategory(key, form);
}

MathMlForm mathMlOperatorForm(const PrDomElement* element, const MathMlFlow& flow, bool* PR_NULLABLE explicitForm) {
    if (explicitForm != nullptr)
        *explicitForm = false;
    const PrDomElement* core = mathMlCoreOperator(element, flow);
    if (core != nullptr) {
        MathMlOperatorAttributes attributes = mathMlOperatorAttributes(core);
        if (attributes.hasForm) {
            if (explicitForm != nullptr)
                *explicitForm = true;
            return attributes.form;
        }
    }
    const PrDomElement* parent = domNodeParentElement(element);
    if (parent == nullptr || parent->space != PrDomNamespace_MathMl)
        return MathMlForm_Infix;
    if (isRowParent(parent)) {
        // The first and the last of the in-flow children that are not space-like, if there are more than one.
        const PrDomElement* first = nullptr;
        const PrDomElement* last = nullptr;
        uint32_t count = 0;
        for (const PrDomElement* child = mathMlFirstInFlow(parent, flow); child != nullptr;
                child = mathMlNextInFlow(parent, child, flow)) {
            if (mathMlIsSpaceLike(child, flow))
                continue;
            first = first != nullptr ? first : child;
            last = child;
            ++count;
        }
        if (count > 1 && element == first)
            return MathMlForm_Prefix;
        if (count > 1 && element == last)
            return MathMlForm_Postfix;
        return MathMlForm_Infix;
    }
    if (mathMlIsScriptedElement(parent) && mathMlFirstInFlow(parent, flow) != element)
        return MathMlForm_Postfix;
    return MathMlForm_Infix;
}

bool mathMlOperator(const PrDomElement* element, const MathMlFlow& flow, MathMlOperator* out) {
    *out = MathMlOperator{};
    const PrDomElement* core = mathMlCoreOperator(element, flow);
    if (core == nullptr)
        return false;
    out->core = core;
    out->form = mathMlOperatorForm(element, flow, &out->explicitForm);
    // The dictionary reads the core operator's content if it is text alone (mathml-core/#dictionary-based-attributes).
    // Any other content is Default.
    PrRawString text{};
    if (mathMlTokenText(core, &text)) {
        PrStringView content = prRawStringView(&text);
        out->category = categoryOf(content, out->form, !out->explicitForm);
        uint32_t codePoint = 0;
        if (content.size != 0 && prUtf8Decode(content, 0, &codePoint) == content.size)
            out->codePoint = codePoint;
    }
    prRawStringReset(&text);
    MathMlCategoryValues values = mathMlCategoryValues(out->category);
    out->lspace = values.lspace;
    out->rspace = values.rspace;
    out->flags = values.flags;
    if (out->codePoint != 0 && mathMlDictionaryIsFence(out->codePoint))
        out->flags |= MathMlOperatorFlag_Fence;
    if (out->codePoint != 0 && mathMlDictionaryIsSeparator(out->codePoint))
        out->flags |= MathMlOperatorFlag_Separator;
    MathMlOperatorAttributes attributes = mathMlOperatorAttributes(core);
    out->flags = MathMlOperatorFlags((out->flags & ~attributes.present) | (attributes.flags & attributes.present));
    out->accent = attributes.accent;
    if (out->accent == MathMlBoolean_None) {
        bool listed = out->codePoint != 0 && mathMlDictionaryIsAccent(out->codePoint);
        out->accent = listed ? MathMlBoolean_True : MathMlBoolean_False;
    }
    out->lspaceAttribute = attributes.lspace;
    out->rspaceAttribute = attributes.rspace;
    out->minsize = attributes.minsize;
    out->maxsize = attributes.maxsize;
    out->axis = MathMlStretchAxis_Block;
    if (out->codePoint != 0 && mathMlIsInlineAxis(out->codePoint))
        out->axis = MathMlStretchAxis_Inline;
    return true;
}

MathMlUnderOver mathMlUnderOverAccents(const PrDomElement* element, const MathMlFlow& flow) {
    MathMlUnderOver out = mathMlUnderOver(element);
    const PrDomElement* base = mathMlFirstInFlow(element, flow);
    const PrDomElement* first = base != nullptr ? mathMlNextInFlow(element, base, flow) : nullptr;
    const PrDomElement* second = first != nullptr ? mathMlNextInFlow(element, first, flow) : nullptr;
    const PrDomElement* under = mathMlIs(element, PrDomTag_Mover) ? nullptr : first;
    const PrDomElement* over = mathMlIs(element, PrDomTag_Mover) ? first : second;
    auto accentOf = [&](const PrDomElement* PR_NULLABLE script) -> MathMlBoolean {
        MathMlOperator op{};
        if (script == nullptr || !mathMlOperator(script, flow, &op))
            return MathMlBoolean_False;
        return op.accent;
    };
    if (!mathMlIs(element, PrDomTag_Munder) && out.accent == MathMlBoolean_None)
        out.accent = accentOf(over);
    if (!mathMlIs(element, PrDomTag_Mover) && out.accentUnder == MathMlBoolean_None)
        out.accentUnder = accentOf(under);
    return out;
}

} // namespace pr_impl
