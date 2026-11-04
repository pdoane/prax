// What MathML's elements give their boxes beyond their computed style
//
// mathml-core - https://w3c.github.io/mathml-core/
// mathml4 - https://w3c.github.io/mathml/

#include "mathml/src/mathml_boxes.h"

#include "pr/ascii.h"
#include "pr/float.h"
#include "pr/layout_style.h"
#include "pr/string_trim.h"
#include "pr/text_transform.h"
#include "pr/utf8.h"

#include "css/src/css_presentation.h"
#include "css/src/css_unit.h"
#include "dom/src/dom_node.h"
#include "dom/src/dom_tag.h"
#include "mathml/src/mathml_attribute.h"
#include "mathml/src/mathml_element.h"
#include "mathml/src/mathml_operator.h"

namespace pr_impl {

namespace {

// What the lengths of a MathML element's attributes resolve against.
struct LengthBases {
    // The style whose font the font-relative units use, and the root's style for the root-relative units, or a null
    // root for the initial font.
    const CssComputedStyle* PR_NONNULL style;
    const CssComputedStyle* PR_NULLABLE root;

    // The measure of the font metrics that ex, cap, ch and ic read, or null for their fallbacks (cssFontMetricPx).
    const CssFontMeasure* PR_NULLABLE measure;

    CssViewport viewport;
    double percent;                 // the px value of 100%

    // The content's width, height and depth in px, which mpadded's pseudo-units multiply.
    double width;
    double height;
    double depth;
};

// Returns `length` in px against `bases`. An absent length is 0. An increment or a decrement is the px to add to the
// default, negative for a decrement.
double lengthPx(const MathMlLength& length, const LengthBases& bases) {
    CssFontMetrics metrics{};
    metrics.measure = bases.measure;
    CssStyleContext context{};
    context.metrics = &metrics;
    context.font = cssStyleFontBases(bases.style);
    context.root = bases.root != nullptr ? cssStyleFontBases(bases.root) : kCssInitialFontBases;
    context.viewport = bases.viewport;
    double vw = double(bases.viewport.width) / 100;
    double vh = double(bases.viewport.height) / 100;
    double px = 0;
    for (uint32_t i = 0; i < length.count; ++i) {
        double scale = 1;
        switch (length.units[i]) {
            case MathMlUnit_Px: scale = 1; break;
            case MathMlUnit_Em: scale = context.font.em; break;
            case MathMlUnit_Ex: scale = cssFontMetricPx(context, false, CssFontMetric_Ex); break;
            case MathMlUnit_Ch: scale = cssFontMetricPx(context, false, CssFontMetric_Ch); break;
            case MathMlUnit_Ic: scale = cssFontMetricPx(context, false, CssFontMetric_Ic); break;
            case MathMlUnit_Cap: scale = cssFontMetricPx(context, false, CssFontMetric_Cap); break;
            case MathMlUnit_Rem: scale = context.root.em; break;
            case MathMlUnit_Lh: scale = context.font.lh; break;
            case MathMlUnit_Rlh: scale = context.root.lh; break;
            case MathMlUnit_Vw: scale = vw; break;
            case MathMlUnit_Vh: scale = vh; break;
            case MathMlUnit_Vmin: scale = prF64Min(vw, vh); break;
            case MathMlUnit_Vmax: scale = prF64Max(vw, vh); break;
            case MathMlUnit_Percent: scale = bases.percent / 100; break;
            case MathMlUnit_Width: scale = bases.width; break;
            case MathMlUnit_Height: scale = bases.height; break;
            case MathMlUnit_Depth: scale = bases.depth; break;
            default: break;
        }
        px += double(length.values[i]) * scale;
    }
    return (length.flags & MathMlLengthFlag_Decrement) != 0 ? -px : px;
}

// Writes the leading and trailing space of the embellished operator `op` in px
// (mathml-core/#dictionary-based-attributes). Each space is the lspace or rspace attribute if present, whose percentage
// is of the dictionary's space, or else the dictionary's space. `bases` are those of the core operator, whose font the
// spaces are relative to.
void operatorSpacing(const MathMlOperator& op, const LengthBases& bases, double* lspace, double* rspace) {
    double em = cssStyleFontBases(bases.style).em;
    // In script styles, at a math depth above zero, TeX omits medium and thick spaces around an operator and keeps thin
    // ones (TeXbook Chapter 18, the table of spacing between atoms, whose entries in parentheses are omitted in script
    // styles). A dictionary space wider than a thin space is such a medium or thick space.
    CssValueRun depth = cssComputedValue(bases.style, PrCssProperty_MathDepth);
    bool script = depth.count != 0 && depth.values[0].number > 0;
    uint8_t spaces[2] = {op.lspace, op.rspace};
    for (uint8_t& space : spaces) {
        if (script && space > 3)
            space = 0;
    }
    double dictionary[2] = {em * spaces[0] / 18, em * spaces[1] / 18};
    const MathMlLength* attributes[2] = {&op.lspaceAttribute, &op.rspaceAttribute};
    double* out[2] = {lspace, rspace};
    for (uint32_t side = 0; side < 2; ++side) {
        *out[side] = dictionary[side];
        if (mathMlLengthIsPresent(*attributes[side])) {
            LengthBases relative = bases;
            relative.percent = dictionary[side];
            *out[side] = lengthPx(*attributes[side], relative);
        }
    }
}

// Returns the layout kind of a MathML element whose layout is `layout`.
PrLayoutMathKind mathKindOf(const PrDomElement* element, MathMlLayout layout) {
    if (mathMlIs(element, PrDomTag_Mprescripts))
        return PrLayoutMathKind_Prescripts;
    switch (layout) {
        case MathMlLayout_Token:
        case MathMlLayout_Glyph: return PrLayoutMathKind_Token;
        case MathMlLayout_Operator: return PrLayoutMathKind_Operator;
        case MathMlLayout_Space: return PrLayoutMathKind_Space;
        case MathMlLayout_Fraction: return PrLayoutMathKind_Fraction;
        case MathMlLayout_SquareRoot: return PrLayoutMathKind_SquareRoot;
        case MathMlLayout_Root: return PrLayoutMathKind_Root;
        case MathMlLayout_Padded: return PrLayoutMathKind_Padded;
        case MathMlLayout_Subscript: return PrLayoutMathKind_Subscript;
        case MathMlLayout_Superscript: return PrLayoutMathKind_Superscript;
        case MathMlLayout_SubSuperscript: return PrLayoutMathKind_SubSuperscript;
        case MathMlLayout_Under: return PrLayoutMathKind_Under;
        case MathMlLayout_Over: return PrLayoutMathKind_Over;
        case MathMlLayout_UnderOver: return PrLayoutMathKind_UnderOver;
        case MathMlLayout_Multiscripts: return PrLayoutMathKind_Multiscripts;
        case MathMlLayout_Enclose: return PrLayoutMathKind_Enclose;
        case MathMlLayout_Fenced: return PrLayoutMathKind_Fenced;
        case MathMlLayout_Stack: return PrLayoutMathKind_Stack;
        case MathMlLayout_LongDivision: return PrLayoutMathKind_LongDivision;
        default: return PrLayoutMathKind_Row;
    }
}

PrLayoutMathAlign mathAlignOf(MathMlAlign align) {
    switch (align) {
        case MathMlAlign_Left: return PrLayoutMathAlign_Left;
        case MathMlAlign_Right: return PrLayoutMathAlign_Right;
        default: return PrLayoutMathAlign_Center;
    }
}

// Returns an attribute's length as a layout length. An absent length, or one that mixes a percentage with other terms,
// is `absent`. An infinite length is none. A percentage alone stays a percentage of what layout resolves it against,
// and anything else is in px.
PrLayoutLength mathLengthOf(const MathMlLength& length, const LengthBases& bases, PrLayoutLength absent) {
    if (!mathMlLengthIsPresent(length))
        return absent;
    if ((length.flags & MathMlLengthFlag_Infinite) != 0)
        return prLayoutLengthNone();
    if (length.count == 1 && length.units[0] == MathMlUnit_Percent)
        return prLayoutLengthPercent(length.values[0]);
    if (mathMlLengthHasPercent(length))
        return absent;
    return prLayoutLengthPx(float(lengthPx(length, bases)));
}

// Returns a dimension of an mpadded, with its pseudo-unit terms as a factor of one pseudo-unit and the rest in px
// (mathml4/#presm_mpadded). A percentage, which MathML Core leaves undefined, makes the dimension absent.
PrLayoutMathPadding paddingOf(const MathMlLength& length, const LengthBases& bases) {
    PrLayoutMathPadding padding{};
    if (!mathMlLengthIsPresent(length) || mathMlLengthHasPercent(length))
        return padding;
    MathMlLength rest = length;
    rest.count = 0;
    for (uint32_t i = 0; i < length.count; ++i) {
        PrLayoutMathPseudo pseudo = PrLayoutMathPseudo_None;
        if (length.units[i] == MathMlUnit_Width)
            pseudo = PrLayoutMathPseudo_Width;
        else if (length.units[i] == MathMlUnit_Height)
            pseudo = PrLayoutMathPseudo_Height;
        else if (length.units[i] == MathMlUnit_Depth)
            pseudo = PrLayoutMathPseudo_Depth;
        if (pseudo == PrLayoutMathPseudo_None) {
            rest.values[rest.count] = length.values[i];
            rest.units[rest.count] = length.units[i];
            ++rest.count;
        } else {
            padding.pseudo = pseudo;
            padding.factor += length.values[i];
        }
    }
    rest.flags = 0;
    padding.length = rest.count != 0 ? prLayoutUnitFromPxDouble(lengthPx(rest, bases)) : PrLayoutUnit{0};
    padding.mode = PrLayoutMathPaddingMode_Set;
    if ((length.flags & MathMlLengthFlag_Increment) != 0)
        padding.mode = PrLayoutMathPaddingMode_Increase;
    else if ((length.flags & MathMlLengthFlag_Decrement) != 0)
        padding.mode = PrLayoutMathPaddingMode_Decrease;
    return padding;
}

// Returns the fence or separator that an mo with content `content` in `form` would be in the expanded form of an
// mfenced. Its spaces and properties are those that the operator dictionary gives it
// (mathml-core/#dictionary-based-attributes), against the mfenced's font. Its character is the first of `content`, or
// none if `content` is empty.
PrLayoutMathFence fenceOf(PrStringView content, MathMlForm form, bool fence, const LengthBases& bases) {
    PrLayoutMathFence out{};
    if (content.size == 0 || prUtf8Decode(content, 0, &out.codePoint) == 0)
        return PrLayoutMathFence{};
    MathMlCategoryValues values = mathMlCategoryValues(mathMlOperatorCategory(content, form));
    MathMlOperator op{.lspace = values.lspace, .rspace = values.rspace};
    double lspace = 0;
    double rspace = 0;
    operatorSpacing(op, bases, &lspace, &rspace);
    out.leftSpace = prLayoutUnitFromPxDouble(lspace);
    out.rightSpace = prLayoutUnitFromPxDouble(rspace);
    if ((values.flags & MathMlOperatorFlag_Stretchy) != 0)
        out.flags |= PrLayoutMathOperatorFlag_Stretchy;
    if ((values.flags & MathMlOperatorFlag_Symmetric) != 0)
        out.flags |= PrLayoutMathOperatorFlag_Symmetric;
    if (fence)
        out.flags |= PrLayoutMathOperatorFlag_Fence;
    return out;
}

// Sets the fences and separators of the mfenced `element` as the mo elements of its expanded form would be
// (mathml4/#presm_mfenced). The opening fence is a prefix fence, the closing fence is a postfix fence, and the
// separator between two arguments is infix. A separators attribute that lists no character puts no separators between
// them.
void fencesOf(const PrDomElement* element, const MathMlFlow& flow, const LengthBases& bases, PrLayoutMathStyle* math) {
    math->fences[PrLayoutMathFenceSlot_Open] = fenceOf(mathMlFencedOpen(element), MathMlForm_Prefix, true, bases);
    math->fences[PrLayoutMathFenceSlot_Close] = fenceOf(mathMlFencedClose(element), MathMlForm_Postfix, true, bases);
    uint32_t arguments = mathMlInFlowCount(element, flow);
    for (uint32_t i = 0; i + 1 < arguments && PrLayoutMathFenceSlot_Separators + i < kPrLayoutMathMaxFences; ++i) {
        uint32_t codePoint = mathMlFencedSeparator(element, i);
        if (codePoint == 0)
            break;
        char bytes[kPrUtf8MaxBytes];
        uint32_t length = prUtf8Encode(codePoint, bytes);
        math->fences[PrLayoutMathFenceSlot_Separators + i] =
            fenceOf(PrStringView{bytes, length}, MathMlForm_Infix, false, bases);
        math->separatorCount = uint8_t(i + 1);
    }
}

// Sets how a line breaks at the embellished operator or mspace whose attributes `source` holds
// (mathml4/#presm_linebreaking). `penalty` is the penalty of an automatic break there. A linebreak of newline forces a
// break, nobreak forbids one, goodbreak favors one and badbreak makes one cost more.
void breakOf(const PrDomElement* source, int32_t penalty, const LengthBases& bases, PrLayoutMathStyle* math) {
    MathMlLineBreaking breaking = mathMlLineBreaking(source);
    switch (breaking.lineBreak) {
        case MathMlLineBreak_Newline: penalty = kPrLayoutMathForcedBreak; break;
        case MathMlLineBreak_NoBreak: penalty = kPrLayoutMathNoBreak; break;
        case MathMlLineBreak_GoodBreak: penalty = -500; break;
        case MathMlLineBreak_BadBreak: penalty = penalty < 1000 ? penalty + 1000 : kPrLayoutMathNoBreak - 1; break;
        default: break;
    }
    math->breakPenalty = penalty;
    switch (breaking.style) {
        case MathMlLineBreakStyle_Before: math->breakStyle = PrLayoutMathBreakStyle_Before; break;
        case MathMlLineBreakStyle_After: math->breakStyle = PrLayoutMathBreakStyle_After; break;
        case MathMlLineBreakStyle_Duplicate: math->breakStyle = PrLayoutMathBreakStyle_Duplicate; break;
        default: break;
    }
    auto indentOf = [](MathMlIndentAlign align, PrLayoutMathIndent otherwise) -> PrLayoutMathIndent {
        switch (align) {
            case MathMlIndentAlign_Left: return PrLayoutMathIndent_Left;
            case MathMlIndentAlign_Center: return PrLayoutMathIndent_Center;
            case MathMlIndentAlign_Right: return PrLayoutMathIndent_Right;
            case MathMlIndentAlign_Auto: return PrLayoutMathIndent_Auto;
            default: return otherwise;
        }
    };
    math->indent = indentOf(breaking.indentAlign, PrLayoutMathIndent_Auto);
    // The first and last lines take indentalign and indentshift unless their own attributes say otherwise
    // (mathml4/#presm_linebreaking).
    math->indentFirst = indentOf(breaking.indentAlignFirst, math->indent);
    math->indentLast = indentOf(breaking.indentAlignLast, math->indent);
    math->indentShift = prLayoutUnitFromPxDouble(lengthPx(breaking.indentShift, bases));
    math->indentShiftFirst = math->indentShift;
    if (mathMlLengthIsPresent(breaking.indentShiftFirst))
        math->indentShiftFirst = prLayoutUnitFromPxDouble(lengthPx(breaking.indentShiftFirst, bases));
    math->indentShiftLast = math->indentShift;
    if (mathMlLengthIsPresent(breaking.indentShiftLast))
        math->indentShiftLast = prLayoutUnitFromPxDouble(lengthPx(breaking.indentShiftLast, bases));
    if (mathMlLengthIsPresent(breaking.lineLeading) && !mathMlLengthHasPercent(breaking.lineLeading))
        math->lineLeading = prLayoutUnitFromPxDouble(prF64Max(0, lengthPx(breaking.lineLeading, bases)));
    // linebreakmultchar is drawn for an invisible times at a break. Its default is the invisible times itself.
    uint32_t multChar = 0;
    PrStringView mult = stringTrimBy(breaking.multChar, prAsciiIsSpace);
    if (mult.size != 0 && prUtf8Decode(mult, 0, &multChar) != 0 && multChar != 0x2062)
        math->breakChar = multChar;
}

// Returns the penalty of an automatic line break at the embellished operator `op`. It is TeX's \relpenalty of 500 at a
// relation and its \binoppenalty of 700 at a binary operator, the infix operators to which the dictionary gives thick
// and medium spaces on either side (mathml-core/#operator-dictionary-categories-values). A line breaks at no other
// operator unless its attributes ask for it.
int32_t breakPenaltyOf(const MathMlOperator& op) {
    if (op.form != MathMlForm_Infix || op.lspace != op.rspace)
        return kPrLayoutMathNoBreak;
    if (op.lspace == 5)
        return 500;
    if (op.lspace == 4)
        return 700;
    return kPrLayoutMathNoBreak;
}

// Sets the alignment of the rows and the characters of the mstack or mlongdiv `element` (mathml4/#presm_mstack). The
// medium character spacing, which MathML 4 leaves to the renderer, is 0.04em, so that digits keep nearly the spacing of
// their font, as arithmetic is typeset.
void stackOf(const PrDomElement* element, const LengthBases& bases, PrLayoutMathStyle* math) {
    MathMlStack stack = mathMlStack(element);
    switch (stack.stackAlign) {
        case MathMlAlign_Left: math->stackAlign = PrLayoutMathAlign_Left; break;
        case MathMlAlign_Center: math->stackAlign = PrLayoutMathAlign_Center; break;
        case MathMlAlign_Right: math->stackAlign = PrLayoutMathAlign_Right; break;
        default: math->stackAlign = PrLayoutMathAlign_DecimalPoint; break;
    }
    math->charAlign = stack.charAlign == MathMlAlign_None ? PrLayoutMathAlign_Right : mathAlignOf(stack.charAlign);
    LengthBases spacing = bases;
    spacing.percent = cssStyleFontBases(bases.style).em * 0.04;
    double charSpacing = spacing.percent;
    if (mathMlLengthIsPresent(stack.charSpacing))
        charSpacing = prF64Max(0, lengthPx(stack.charSpacing, spacing));
    math->charSpacing = prLayoutUnitFromPxDouble(charSpacing);
    math->decimalPoint = stack.decimalCharacter;
    switch (stack.align.align) {
        case MathMlVerticalAlign_Top: math->tableAlign = PrLayoutMathTableAlign_Top; break;
        case MathMlVerticalAlign_Bottom: math->tableAlign = PrLayoutMathTableAlign_Bottom; break;
        case MathMlVerticalAlign_Center: math->tableAlign = PrLayoutMathTableAlign_Center; break;
        case MathMlVerticalAlign_Axis: math->tableAlign = PrLayoutMathTableAlign_Axis; break;
        default: math->tableAlign = PrLayoutMathTableAlign_Baseline; break;
    }
    math->tableRow = stack.align.row;
    if (mathMlIs(element, PrDomTag_Mlongdiv))
        math->longDivision = PrLayoutMathLongDivision(mathMlLongDivStyle(element));
}

// Returns the number of element children of `parent` before `child`.
uint32_t elementIndexOf(const PrDomElement* parent, const PrDomElement* child) {
    uint32_t index = 0;
    for (const PrDomNode* node = parent->first; node != nullptr && node != child; node = node->next)
        index += mathMlIsElement(node) ? 1 : 0;
    return index;
}

// Marks the MathML element `element` with its part if it is an item of an mstack or an mlongdiv
// (mathml4/#presm_elementary). The items are the children that the stack's rows, groups and carries pass through
// (mathml_hints.cpp). The position of an item's row is the sum of the positions of its msrow or mscarries and of each
// enclosing msgroup, plus each msgroup's shift once for each of its children before the one that holds the item
// (mathml4/#presm_msgroup).
void stackPartOf(const PrDomElement* element, const MathMlFlow& flow, const LengthBases& bases, PrLayoutMathStyle* math) {
    const PrDomElement* child = element;
    const PrDomElement* parent = domNodeParentElement(element);
    while (mathMlIs(parent, PrDomTag_Mstyle) && mathMlIs(domNodeParentElement(parent), PrDomTag_Msrow)) {
        child = parent;
        parent = domNodeParentElement(parent);
    }
    const PrDomElement* row = nullptr;
    int32_t position = 0;
    if (mathMlIs(parent, PrDomTag_Msrow) || mathMlIs(parent, PrDomTag_Mscarries)) {
        row = parent;
        position += mathMlStackPosition(parent);
        child = parent;
        parent = domNodeParentElement(parent);
    }
    while (mathMlIs(parent, PrDomTag_Msgroup)) {
        position += mathMlStackPosition(parent) + mathMlStackShift(parent) * int32_t(elementIndexOf(parent, child));
        child = parent;
        parent = domNodeParentElement(parent);
    }
    if (!mathMlIs(parent, PrDomTag_Mstack) && !mathMlIs(parent, PrDomTag_Mlongdiv))
        return;
    // A row, a group or carries in a stack pass their children through, which the projection gave no box otherwise.
    if (row != nullptr && mathMlIs(parent, PrDomTag_Mlongdiv) && elementIndexOf(parent, child) < 2)
        return;
    if (child == element && mathMlIs(parent, PrDomTag_Mlongdiv) && elementIndexOf(parent, child) < 2) {
        bool divisor = elementIndexOf(parent, child) == 0;
        math->stackPart = divisor ? PrLayoutMathStackPart_Divisor : PrLayoutMathStackPart_Result;
        math->stackFlags = PrLayoutMathStackFlag_RowStart;
        return;
    }
    math->stackPosition = mathMlStackPosition(element) + position;
    if (row == nullptr || mathMlFirstInFlow(row, flow) == element)
        math->stackFlags |= PrLayoutMathStackFlag_RowStart;
    if (mathMlIs(element, PrDomTag_Msline)) {
        math->stackPart = PrLayoutMathStackPart_Line;
        MathMlStackLine line = mathMlStackLine(element);
        math->lineColumns = line.length;
        math->overhangs[0] = prLayoutUnitFromPxDouble(lengthPx(line.leftOverhang, bases));
        math->overhangs[1] = prLayoutUnitFromPxDouble(lengthPx(line.rightOverhang, bases));
        math->lineThickness = mathLengthOf(line.thickness, bases, prLayoutLengthAuto());
    } else if (mathMlIs(row, PrDomTag_Mscarries)) {
        math->stackPart = PrLayoutMathStackPart_Carry;
        math->stackPosition = position;
        MathMlCarry carry = mathMlCarry(mathMlIs(element, PrDomTag_Mscarry) ? element : row);
        static constexpr PrLayoutMathCarryLocation kLocations[] = {
            PrLayoutMathCarryLocation_N, PrLayoutMathCarryLocation_W, PrLayoutMathCarryLocation_Nw,
            PrLayoutMathCarryLocation_N, PrLayoutMathCarryLocation_Ne, PrLayoutMathCarryLocation_E,
            PrLayoutMathCarryLocation_Se, PrLayoutMathCarryLocation_S, PrLayoutMathCarryLocation_Sw,
        };
        math->carryLocation = kLocations[carry.location];
        if ((carry.crossout & MathMlCrossoutFlag_UpDiagonalStrike) != 0)
            math->stackFlags |= PrLayoutMathStackFlag_UpDiagonalStrike;
        if ((carry.crossout & MathMlCrossoutFlag_DownDiagonalStrike) != 0)
            math->stackFlags |= PrLayoutMathStackFlag_DownDiagonalStrike;
        if ((carry.crossout & MathMlCrossoutFlag_VerticalStrike) != 0)
            math->stackFlags |= PrLayoutMathStackFlag_VerticalStrike;
        if ((carry.crossout & MathMlCrossoutFlag_HorizontalStrike) != 0)
            math->stackFlags |= PrLayoutMathStackFlag_HorizontalStrike;
    } else {
        math->stackPart = mathMlIs(element, PrDomTag_Mn) ? PrLayoutMathStackPart_Number : PrLayoutMathStackPart_Digit;
    }
}

// Returns true if the run holds the keyword.
bool hasKeyword(CssValueRun run, PrCssKeyword keyword) {
    for (uint32_t i = 0; i < run.count; ++i) {
        if (cssValueIsKeyword(&run.values[i], keyword))
            return true;
    }
    return false;
}

// Classifies a child of a MathML element for its parent's math layout by the child's computed style `style`, which is
// null if the child has none. A child with no box or an out-of-flow box takes no part (mathml-core/#layout-algorithms),
// and display: contents passes its children through.
MathMlChild childFromStyle(const CssComputedStyle* PR_NULLABLE style) {
    if (style == nullptr)
        return MathMlChild_OutOfFlow;
    CssValueRun display = cssComputedValue(style, PrCssProperty_Display);
    if (hasKeyword(display, PrCssKeyword_None))
        return MathMlChild_OutOfFlow;
    if (hasKeyword(display, PrCssKeyword_Contents))
        return MathMlChild_Contents;
    PrCssKeyword position = cssComputedFirstKeyword(style, PrCssProperty_Position);
    if (position == PrCssKeyword_Absolute || position == PrCssKeyword_Fixed)
        return MathMlChild_OutOfFlow;
    return MathMlChild_InFlow;
}

// Classifies a child of a MathML element for its parent's math layout by the child's computed style in `context`, the
// presentation.
MathMlChild classifyChild(const void* PR_NULLABLE context, const PrDomElement* child) {
    return childFromStyle(cssPresentationNodeStyle(static_cast<const PrCssPresentation*>(context), child));
}

// Returns `record` with the math group of the MathML element `element`, whose style is `style`
// (mathml-core/#layout-algorithms). The group holds the algorithm of its math box, the values of its attributes, the
// properties of its operator if it is an embellished operator, and the alignment of its table if it is an mtable or an
// mtd. The record of an element that math layout does not read is returned unchanged.
PrLayoutStyleIndex mathRecord(const CssBoxRecordContext& context, const PrDomElement* element,
        const CssComputedStyle* style, PrLayoutStyleIndex record) {
    PrLayoutTree* tree = context.tree;
    PrLayoutStyle groups = prLayoutStyleRecord(tree, record);
    PrLayoutDisplay display = prLayoutBoxStyleRecord(tree, groups.groups[PrLayoutGroup_Box]).display;
    PrLayoutMathStyle math = prLayoutMathStyleDefault();
    MathMlFlow flow{.classify = classifyChild, .context = context.presentation};
    LengthBases bases{.style = style, .measure = context.measure, .viewport = context.viewport, .percent = 0};
    if (display == PrLayoutDisplay_BlockMath || display == PrLayoutDisplay_InlineMath) {
        MathMlLayout layout = mathMlLayout(element, flow);
        math.kind = mathKindOf(element, layout);
        MathMlOperator op{};
        if (mathMlOperator(element, flow, &op)) {
            math.embellished = true;
            if ((op.flags & MathMlOperatorFlag_Stretchy) != 0)
                math.operatorFlags |= PrLayoutMathOperatorFlag_Stretchy;
            if ((op.flags & MathMlOperatorFlag_Symmetric) != 0)
                math.operatorFlags |= PrLayoutMathOperatorFlag_Symmetric;
            if ((op.flags & MathMlOperatorFlag_LargeOp) != 0)
                math.operatorFlags |= PrLayoutMathOperatorFlag_LargeOp;
            if ((op.flags & MathMlOperatorFlag_MovableLimits) != 0)
                math.operatorFlags |= PrLayoutMathOperatorFlag_MovableLimits;
            if ((op.flags & MathMlOperatorFlag_Fence) != 0)
                math.operatorFlags |= PrLayoutMathOperatorFlag_Fence;
            if (op.axis == MathMlStretchAxis_Inline)
                math.operatorFlags |= PrLayoutMathOperatorFlag_InlineAxis;
            if (op.form == MathMlForm_Prefix)
                math.operatorFlags |= PrLayoutMathOperatorFlag_Prefix;
            else if (op.form == MathMlForm_Postfix)
                math.operatorFlags |= PrLayoutMathOperatorFlag_Postfix;
            if (op.core != nullptr)
                breakOf(op.core, breakPenaltyOf(op), bases, &math);
            // The spaces and sizes are relative to the core operator's font.
            LengthBases core = bases;
            if (op.core != nullptr && op.core != element) {
                if (const CssComputedStyle* coreStyle = cssPresentationNodeStyle(context.presentation, op.core))
                    core.style = coreStyle;
            }
            double lspace = 0;
            double rspace = 0;
            operatorSpacing(op, core, &lspace, &rspace);
            math.leftSpace = prLayoutUnitFromPxDouble(lspace);
            math.rightSpace = prLayoutUnitFromPxDouble(rspace);
            math.minSize = mathLengthOf(op.minsize, core, prLayoutLengthPercent(100));
            math.maxSize = mathLengthOf(op.maxsize, core, prLayoutLengthNone());
        }
        switch (math.kind) {
            case PrLayoutMathKind_Fraction: {
                MathMlFraction fraction = mathMlFraction(element);
                math.lineThickness = mathLengthOf(fraction.lineThickness, bases, prLayoutLengthAuto());
                math.numeratorAlign = mathAlignOf(fraction.numeratorAlign);
                math.denominatorAlign = mathAlignOf(fraction.denominatorAlign);
                if (fraction.bevelled)
                    math.scriptFlags |= PrLayoutMathScriptFlag_Bevelled;
                break;
            }
            case PrLayoutMathKind_Space: {
                // A height that is absent, invalid or a percentage requests no line-ascent (mathml-core/#space-mspace).
                MathMlSpace space = mathMlSpace(element);
                if (mathMlLengthIsPresent(space.height) && !mathMlLengthHasPercent(space.height))
                    math.spaceAscent = prLayoutUnitFromPxDouble(prF64Max(0, lengthPx(space.height, bases)));
                breakOf(element, kPrLayoutMathNoBreak, bases, &math);
                break;
            }
            case PrLayoutMathKind_Padded: {
                MathMlPadded padded = mathMlPadded(element);
                math.padding[PrLayoutMathPadded_Width] = paddingOf(padded.width, bases);
                math.padding[PrLayoutMathPadded_Height] = paddingOf(padded.height, bases);
                math.padding[PrLayoutMathPadded_Depth] = paddingOf(padded.depth, bases);
                math.padding[PrLayoutMathPadded_LeftSpace] = paddingOf(padded.lspace, bases);
                math.padding[PrLayoutMathPadded_VerticalOffset] = paddingOf(padded.voffset, bases);
                break;
            }
            case PrLayoutMathKind_Under:
            case PrLayoutMathKind_Over:
            case PrLayoutMathKind_UnderOver: {
                MathMlUnderOver underOver = mathMlUnderOverAccents(element, flow);
                if (underOver.accent == MathMlBoolean_True)
                    math.scriptFlags |= PrLayoutMathScriptFlag_Accent;
                if (underOver.accentUnder == MathMlBoolean_True)
                    math.scriptFlags |= PrLayoutMathScriptFlag_AccentUnder;
                math.scriptAlign = mathAlignOf(underOver.align);
                break;
            }
            case PrLayoutMathKind_Enclose:
                math.notations = PrLayoutMathNotationFlags(mathMlNotations(element));
                break;
            case PrLayoutMathKind_Fenced:
                fencesOf(element, flow, bases, &math);
                break;
            case PrLayoutMathKind_Stack:
            case PrLayoutMathKind_LongDivision:
                stackOf(element, bases, &math);
                break;
            default:
                break;
        }
        stackPartOf(element, flow, bases, &math);
    } else if (display == PrLayoutDisplay_TableCell && mathMlIs(element, PrDomTag_Mtd)) {
        math.kind = PrLayoutMathKind_Row;
    } else if ((display == PrLayoutDisplay_Table || display == PrLayoutDisplay_InlineTable) &&
            mathMlIs(element, PrDomTag_Mtable)) {
        math.kind = PrLayoutMathKind_Table;
        MathMlTableAlign align = mathMlTableAlign(element);
        switch (align.align) {
            case MathMlVerticalAlign_Top: math.tableAlign = PrLayoutMathTableAlign_Top; break;
            case MathMlVerticalAlign_Bottom: math.tableAlign = PrLayoutMathTableAlign_Bottom; break;
            case MathMlVerticalAlign_Center: math.tableAlign = PrLayoutMathTableAlign_Center; break;
            case MathMlVerticalAlign_Baseline: math.tableAlign = PrLayoutMathTableAlign_Baseline; break;
            default: math.tableAlign = PrLayoutMathTableAlign_Axis; break;
        }
        math.tableRow = align.row;
    } else {
        return record;
    }
    groups.groups[PrLayoutGroup_Math] = prLayoutMathStyleIntern(tree, &math);
    // The base of an accent that is a single dotted letter takes its dotless form (OpenType MATH, dtls).
    const PrDomElement* parent = domElementFromNode(static_cast<const PrDomNode*>(element)->parent);
    if (mathMlIs(element, PrDomTag_Mi) && parent != nullptr &&
            (mathMlIs(parent, PrDomTag_Mover) || mathMlIs(parent, PrDomTag_Munderover)) &&
            mathMlFirstInFlow(parent, flow) == element &&
            mathMlUnderOverAccents(parent, flow).accent == MathMlBoolean_True) {
        PrLayoutFontStyle font = prLayoutFontStyleRecord(tree, groups.groups[PrLayoutGroup_Font]);
        font.mathFlags |= PrLayoutMathFlag_Dotless;
        groups.groups[PrLayoutGroup_Font] = prLayoutFontStyleIntern(tree, &font);
    }
    // Elementary math lays out its columns left to right whatever the direction, as numbers are written
    // (mathml4/#presm_elementary).
    if (math.kind == PrLayoutMathKind_Stack || math.kind == PrLayoutMathKind_LongDivision) {
        PrLayoutFontStyle font = prLayoutFontStyleRecord(tree, groups.groups[PrLayoutGroup_Font]);
        if (font.direction != PrTextDirection_Ltr) {
            font.direction = PrTextDirection_Ltr;
            groups.groups[PrLayoutGroup_Font] = prLayoutFontStyleIntern(tree, &font);
        }
    }
    return prLayoutStyleIntern(tree, &groups);
}

// mtd spans columns and rows as a td does (mathml-core/#entry-in-table-or-matrix-mtd), and an mglyph is replaced by its
// image (mathml4/#presm_mglyph).
void traits(const PrDomElement* element, CssBoxTraits* out) {
    if (element->tag == PrDomTag_Mglyph) {
        out->kind = CssBoxKind_Replaced;
        return;
    }
    if (element->tag == PrDomTag_Mtd) {
        uint32_t columns = 1;
        uint32_t rows = 1;
        mathMlCellSpans(element, &columns, &rows);
        out->flags |= CssBoxTraitFlag_Spans;
        out->columnSpan = uint16_t(columns);
        out->rowSpan = uint16_t(rows);
        return;
    }
}

bool reads(const PrDomElement* element, PrStringView name) {
    return element->tag == PrDomTag_Mtd && (name == "columnspan" || name == "rowspan");
}

// Returns true if the MathML element is a table or a row of one, whose children, the rows and the cells, read neither
// one another nor what is in them.
bool holdsTableParts(const PrDomElement* element) {
    return element->tag == PrDomTag_Mtable || element->tag == PrDomTag_Mtr || element->tag == PrDomTag_Mlabeledtr;
}

// Returns true if the MathML element is a part of a table. The math group of a table, a row or a cell reads neither its
// children nor its siblings, and no element outside it reads what is in it, because none of them is an embellished
// operator or space-like. A cell's children are a row, and read one another as a row's do.
bool isTablePart(const PrDomElement* element) {
    return holdsTableParts(element) || element->tag == PrDomTag_Mtd;
}

// Returns true if the MathML element is an munder, an mover or an munderover, whose base reads the accent that its
// scripts give it (mathRecord's dotless base).
bool isUnderOver(const PrDomElement* element) {
    return element->tag == PrDomTag_Munder || element->tag == PrDomTag_Mover || element->tag == PrDomTag_Munderover;
}

// Calls `fn` with each MathML element whose math group a change at `element` can reach: `element` and its MathML
// ancestors up to the first part of a table, and the children of some of them.
//
// A change to which children are in flow, or to which of them are embellished operators or space-like, changes the
// layout and the embellishment of their parent, and the forms of their siblings' operators
// (mathml-core/#embellished-operators), so `structure` reaches the children of every ancestor. A change to the content
// of an element alone, such as the text of a token or an attribute, changes neither. It changes the properties of the
// embellished operators around it, which are among its ancestors, and the accent of an munder, an mover or an
// munderover, which their bases read, so it reaches the children of those alone.
void eachMathReach(PrDomElement* element, bool structure, CssBoxReachFn fn, void* PR_NULLABLE context) {
    for (PrDomNode* at = element; at != nullptr && mathMlIsElement(at); at = at->parent) {
        PrDomElement* ancestor = domElementFromNode(at);
        fn(context, ancestor);
        if ((structure || isUnderOver(ancestor)) && !holdsTableParts(ancestor)) {
            for (PrDomNode* child = at->first; child != nullptr; child = child->next) {
                if (mathMlIsElement(child))
                    fn(context, domElementFromNode(child));
            }
        }
        if (isTablePart(ancestor))
            break;
    }
}

// A change to an element's children reaches the math groups around it. A change to the children of a token changes its
// content alone, because a token's children are its text and phrasing content (mathml-core/#token-elements). A change
// to its attributes changes its content too, because an operator's attributes give the embellished operators around it
// their properties. A change to its attributes also reaches the MathML elements below it whose math groups read those
// attributes. These are the elements in a math element or an mstyle, whose attributes give their defaults
// (mathml4/#presm_mstyle, mathMlInheritedValue), and the parts of elementary math, which the position and shift of an
// msgroup or msrow move and the attributes of their stack align (mathml4/#presm_elementary).
void reach(PrDomElement* element, CssBoxChange change, CssBoxReachFn fn, void* PR_NULLABLE context) {
    if (change == CssBoxChange_Style) {
        // The style of an operator reaches the embellished operators whose core it is, which are among the ancestors
        // that may take their core operator from a child (mathMlCoreOperator).
        for (PrDomNode* at = element->parent; mathMlIsElement(at); at = at->parent) {
            PrDomElement* ancestor = domElementFromNode(at);
            if (!mathMlIsScriptedElement(ancestor) && !mathMlIsGroupingElement(ancestor) &&
                    ancestor->tag != PrDomTag_Mfrac && ancestor->tag != PrDomTag_Mpadded)
                break;
            fn(context, ancestor);
        }
        return;
    }
    bool structure = change == CssBoxChange_Children && !mathMlIsTokenElement(element);
    eachMathReach(element, structure, fn, context);
    if (change != CssBoxChange_Attributes)
        return;
    switch (element->tag) {
        case PrDomTag_Math:
        case PrDomTag_Mstyle:
        case PrDomTag_Mstack:
        case PrDomTag_Mlongdiv:
        case PrDomTag_Msgroup:
        case PrDomTag_Msrow:
        case PrDomTag_Mscarries:
            break;
        default:
            return;
    }
    PrDomNode* root = element;
    for (PrDomNode* at = root->first; at != nullptr; at = domNodeNext(root, at)) {
        if (mathMlIsElement(at))
            fn(context, domElementFromNode(at));
    }
}

// A child that enters or leaves the flow of its MathML parent changes the math groups of the parent, of its siblings
// and of the parent's ancestors.
bool childStyleReaches(const CssComputedStyle* old, const CssComputedStyle* style) {
    return childFromStyle(old) != childFromStyle(style);
}

// The spaces and sizes of an embellished operator are relative to the font of its core operator (mathRecord), so a
// change to an operator's font, or to its math depth, which is in the font's group, reaches the embellished operators
// around it.
bool styleReaches(const PrDomElement* element, const CssComputedStyle* old, const CssComputedStyle* style) {
    return element->tag == PrDomTag_Mo && old->groups[CssStyleGroup_Font] != style->groups[CssStyleGroup_Font];
}

constexpr CssBoxProvider kBoxes = {
    .tags = kDomTagsMathMlBoxes,
    .traits = traits,
    .reads = reads,
    .record = mathRecord,
    .reach = reach,
    .childStyleReaches = childStyleReaches,
    .styleReaches = styleReaches,
};

} // namespace

const CssBoxProvider* mathMlBoxes() {
    return &kBoxes;
}

} // namespace pr_impl
