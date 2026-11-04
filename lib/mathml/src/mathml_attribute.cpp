// MathML's attributes, parsed by their grammars.

#include "mathml/src/mathml_attribute.h"

#include "pr/ascii.h"
#include "pr/count_of.h"
#include "pr/float.h"
#include "pr/float_convert.h"
#include "pr/layout_style.h"
#include "pr/string_trim.h"
#include "pr/utf8.h"

#include "dom/src/dom_microsyntax.h"
#include "mathml/src/mathml_element.h"

namespace pr_impl {

namespace {

// A keyword of an attribute's grammar, and the value that it names.
struct Keyword {
    PrStringView text;
    uint8_t value;
};

// Writes the value of the keyword that `text` matches ASCII case-insensitively to `out`. Returns false if it matches
// none.
template <size_t N>
bool keywordOf(PrStringView text, const Keyword (&keywords)[N], uint8_t* out) {
    for (const Keyword& keyword : keywords) {
        if (prAsciiEqualsFold(text, keyword.text)) {
            *out = keyword.value;
            return true;
        }
    }
    return false;
}

// Returns the value of the keyword that `text` matches, or `otherwise`.
template <size_t N>
uint8_t keywordOr(PrStringView text, const Keyword (&keywords)[N], uint8_t otherwise) {
    uint8_t value = otherwise;
    return keywordOf(text, keywords, &value) ? value : otherwise;
}

// Returns `text` without the ASCII white space at either end.
PrStringView trimmed(PrStringView text) {
    return stringTrimBy(text, prAsciiIsSpace);
}

// A unit of a length's grammar.
struct Unit {
    PrStringView name;
    MathMlUnit unit;
    double px;                      // the px of one absolute unit, or 0 for a relative unit
};

// The CSS units of length (css-values-4/#lengths) that a MathML attribute takes.
constexpr Unit kUnits[] = {
    {"px", MathMlUnit_Px, 1}, {"cm", MathMlUnit_Px, 96 / 2.54}, {"mm", MathMlUnit_Px, 96 / 25.4},
    {"q", MathMlUnit_Px, 96 / 101.6}, {"in", MathMlUnit_Px, 96}, {"pt", MathMlUnit_Px, 96.0 / 72},
    {"pc", MathMlUnit_Px, 16}, {"em", MathMlUnit_Em, 0}, {"ex", MathMlUnit_Ex, 0}, {"ch", MathMlUnit_Ch, 0},
    {"ic", MathMlUnit_Ic, 0}, {"cap", MathMlUnit_Cap, 0}, {"rem", MathMlUnit_Rem, 0}, {"lh", MathMlUnit_Lh, 0},
    {"rlh", MathMlUnit_Rlh, 0}, {"vw", MathMlUnit_Vw, 0}, {"vh", MathMlUnit_Vh, 0}, {"vmin", MathMlUnit_Vmin, 0},
    {"vmax", MathMlUnit_Vmax, 0},
};

// MathML's pseudo-units of mpadded (mathml4/#presm_mpadded).
constexpr Unit kPseudoUnits[] = {
    {"width", MathMlUnit_Width, 0}, {"height", MathMlUnit_Height, 0}, {"depth", MathMlUnit_Depth, 0},
};

// The named spaces, from the narrowest, which are 1 to 7 eighteenths of an em (mathml4/#type_length). A negative space
// is the same name after "negative".
constexpr PrStringView kNamedSpaces[] = {
    "veryverythinmathspace", "verythinmathspace", "thinmathspace", "mediummathspace", "thickmathspace",
    "verythickmathspace", "veryverythickmathspace",
};

// Parses one dimension, percentage or zero at the start of `text`, multiplied by `scale`, into `out`. Writes the number
// of bytes that it takes to `taken`. A number without a unit is 0, or with `syntax`'s Unitless a multiple of 100%.
bool parseDimension(PrStringView text, MathMlLengthSyntax syntax, double scale, MathMlLength* out, size_t* taken) {
    // A sign is part of the number. What a leading + of the whole value means is decided before (mathMlParseLength).
    double number = 0;
    size_t length = prF64ParsePrefix(text, &number);
    if (length == 0 || !prF64IsFinite(number))
        return false;
    size_t end = length;
    while (end < text.size && (prAsciiIsLetter(text.data[end]) || text.data[end] == '%'))
        ++end;
    PrStringView unit = {text.data + length, end - length};
    *taken = end;
    if (unit.size == 0) {
        if ((syntax & MathMlLengthSyntaxFlag_Unitless) != 0) {
            mathMlLengthAdd(out, number * 100 * scale, MathMlUnit_Percent);
            return true;
        }
        if (number != 0)
            return false;
        mathMlLengthAdd(out, 0, MathMlUnit_Px);
        return true;
    }
    if (unit == "%") {
        mathMlLengthAdd(out, number * scale, MathMlUnit_Percent);
        return true;
    }
    if (unit.size > 1 && unit.data[0] == '%' && (syntax & MathMlLengthSyntaxFlag_Pseudo) != 0) {
        // A percentage of a pseudo-unit, as 50%height.
        PrStringView pseudo = {unit.data + 1, unit.size - 1};
        for (const Unit& u : kPseudoUnits) {
            if (prAsciiEqualsFold(pseudo, u.name)) {
                mathMlLengthAdd(out, number / 100 * scale, u.unit);
                return true;
            }
        }
        return false;
    }
    for (const Unit& u : kUnits) {
        if (prAsciiEqualsFold(unit, u.name)) {
            mathMlLengthAdd(out, u.px != 0 ? number * u.px * scale : number * scale, u.unit);
            return true;
        }
    }
    if ((syntax & MathMlLengthSyntaxFlag_Pseudo) != 0) {
        for (const Unit& u : kPseudoUnits) {
            if (prAsciiEqualsFold(unit, u.name)) {
                mathMlLengthAdd(out, number * scale, u.unit);
                return true;
            }
        }
    }
    return false;
}

// Parses the sum inside a calc(): terms joined by + or -, each a dimension, optionally multiplied by a number on either
// side (css-values-4/#calc-syntax), with white space around the operators.
bool parseCalc(PrStringView text, MathMlLengthSyntax syntax, MathMlLength* out) {
    size_t at = 0;
    double sign = 1;
    for (;;) {
        while (at < text.size && prAsciiIsSpace(text.data[at]))
            ++at;
        double scale = sign;
        // A number that multiplies the term: a number followed by *.
        double factor = 0;
        size_t length = prF64ParsePrefix(PrStringView{text.data + at, text.size - at}, &factor);
        size_t after = at + length;
        while (after < text.size && prAsciiIsSpace(text.data[after]))
            ++after;
        if (length != 0 && after < text.size && text.data[after] == '*') {
            scale *= factor;
            at = after + 1;
            while (at < text.size && prAsciiIsSpace(text.data[at]))
                ++at;
        }
        size_t taken = 0;
        MathMlLength term{};
        if (!parseDimension(PrStringView{text.data + at, text.size - at}, syntax & ~MathMlLengthSyntaxFlag_Unitless,
                1, &term, &taken))
            return false;
        at += taken;
        while (at < text.size && prAsciiIsSpace(text.data[at]))
            ++at;
        if (at < text.size && text.data[at] == '*') {
            ++at;
            while (at < text.size && prAsciiIsSpace(text.data[at]))
                ++at;
            length = prF64ParsePrefix(PrStringView{text.data + at, text.size - at}, &factor);
            if (length == 0)
                return false;
            scale *= factor;
            at += length;
            while (at < text.size && prAsciiIsSpace(text.data[at]))
                ++at;
        }
        mathMlLengthAdd(out, double(term.values[0]) * scale, term.units[0]);
        if (at == text.size)
            return true;
        // The operators of a sum need white space on both sides (css-values-4/#calc-syntax).
        if ((text.data[at] != '+' && text.data[at] != '-') || !prAsciiIsSpace(text.data[at - 1]) ||
                at + 1 == text.size || !prAsciiIsSpace(text.data[at + 1]))
            return false;
        sign = text.data[at] == '-' ? -1 : 1;
        ++at;
    }
}

// Returns the parsed value of the attribute `name` of the element or of the mstyle that gives its default.
PrStringView valueOf(const PrDomElement* element, PrStringView name) {
    return mathMlInheritedValue(element, name);
}

// The keywords of a horizontal alignment.
constexpr Keyword kAligns[] = {
    {"left", MathMlAlign_Left}, {"center", MathMlAlign_Center}, {"right", MathMlAlign_Right},
};

// The keywords of a vertical alignment.
constexpr Keyword kVerticalAligns[] = {
    {"top", MathMlVerticalAlign_Top}, {"bottom", MathMlVerticalAlign_Bottom}, {"center", MathMlVerticalAlign_Center},
    {"baseline", MathMlVerticalAlign_Baseline}, {"axis", MathMlVerticalAlign_Axis},
};

// The keywords of a table's lines.
constexpr Keyword kLines[] = {{"none", MathMlLine_None}, {"solid", MathMlLine_Solid}, {"dashed", MathMlLine_Dashed}};

// Returns the alignment that the item at `index` of the attribute value `value` names, or none.
MathMlAlign alignItem(PrStringView value, uint32_t index) {
    return keywordOr(mathMlListItem(value, index), kAligns, MathMlAlign_None);
}

// Returns the vertical alignment that the item at `index` of the attribute value `value` names, or none.
MathMlVerticalAlign verticalAlignItem(PrStringView value, uint32_t index) {
    return keywordOr(mathMlListItem(value, index), kVerticalAligns, MathMlVerticalAlign_None);
}

// Returns the value of the attribute `name` of the element, or none for a null element.
PrStringView ownValue(const PrDomElement* PR_NULLABLE element, PrStringView name) {
    return element != nullptr ? mathMlAttributeValue(element, name) : PrStringView{};
}

// Returns the value of the attribute `name` of the element or of the mstyle that gives its default, or none for a null
// element.
PrStringView inheritedValue(const PrDomElement* PR_NULLABLE element, PrStringView name) {
    return element != nullptr ? valueOf(element, name) : PrStringView{};
}

// Returns the element's parent if it is the MathML element `tag`.
const PrDomElement* PR_NULLABLE parentOf(const PrDomElement* element, PrDomTag tag) {
    const PrDomElement* parent = domNodeParentElement(element);
    return mathMlIs(parent, tag) ? parent : nullptr;
}

// Returns the row of the cell `element`, an mtr or an mlabeledtr, or null.
const PrDomElement* PR_NULLABLE rowOf(const PrDomElement* element) {
    const PrDomElement* row = parentOf(element, PrDomTag_Mtr);
    return row != nullptr ? row : parentOf(element, PrDomTag_Mlabeledtr);
}

// Returns the table of the row `row`, or null.
const PrDomElement* PR_NULLABLE tableOf(const PrDomElement* PR_NULLABLE row) {
    return row != nullptr ? parentOf(row, PrDomTag_Mtable) : nullptr;
}

// Returns the alignment and row that the align attribute of a table or a stack gives (mathml4/#presm_mtable).
MathMlTableAlign tableAlignOf(PrStringView text, MathMlVerticalAlign otherwise) {
    MathMlTableAlign out = {otherwise, 0};
    PrStringView align = mathMlListItem(text, 0);
    uint8_t value = 0;
    if (!keywordOf(align, kVerticalAligns, &value))
        return out;
    out.align = value;
    // A row number that is out of range or not an integer is ignored.
    size_t end = size_t(align.data - text.data) + align.size;
    PrStringView rest = trimmed(PrStringView{text.data + end, text.size - end});
    int32_t row = 0;
    if (rest.size != 0 && mathMlParseInteger(rest, &row))
        out.row = row;
    return out;
}

// The keywords of a line thickness (mathml4/#presm_mfrac), as percentages of the default thickness.
constexpr Keyword kThicknesses[] = {{"thin", 50}, {"medium", 100}, {"thick", 200}};

// Returns the length of a line thickness: a length, or a keyword as a percentage of the default.
MathMlLength thicknessOf(PrStringView text) {
    MathMlLength out{};
    uint8_t percent = 0;
    if (keywordOf(trimmed(text), kThicknesses, &percent)) {
        mathMlLengthAdd(&out, percent, MathMlUnit_Percent);
        return out;
    }
    mathMlParseLength(text, MathMlLengthSyntaxFlag_Unitless, &out);
    return out;
}

} // namespace

void mathMlLengthAdd(MathMlLength* length, double value, MathMlUnit unit) {
    for (uint32_t i = 0; i < length->count; ++i) {
        if (length->units[i] == unit) {
            length->values[i] += float(value);
            return;
        }
    }
    PR_ASSERT(length->count < kMathMlLengthMaxTerms);
    length->values[length->count] = float(value);
    length->units[length->count] = unit;
    ++length->count;
}

bool mathMlLengthHasPercent(const MathMlLength& length) {
    for (uint32_t i = 0; i < length.count; ++i) {
        if (length.units[i] == MathMlUnit_Percent)
            return true;
    }
    return false;
}

bool mathMlLengthHasPseudoUnit(const MathMlLength& length) {
    for (uint32_t i = 0; i < length.count; ++i) {
        if (length.units[i] >= MathMlUnit_Width)
            return true;
    }
    return false;
}

bool mathMlParseLength(PrStringView text, MathMlLengthSyntax syntax, MathMlLength* out) {
    *out = MathMlLength{};
    PrStringView value = trimmed(text);
    if (value.size == 0)
        return false;
    if ((syntax & MathMlLengthSyntaxFlag_Infinity) != 0 && prAsciiEqualsFold(value, "infinity")) {
        out->flags = MathMlLengthFlag_Infinite;
        return true;
    }
    // A leading + makes an increment, which no <length-percentage> writes. A leading - is a negative length, as MathML
    // Core reads it, unless a pseudo-unit follows, which only MathML 3's decrement writes.
    MathMlLengthFlags sign = 0;
    if ((syntax & MathMlLengthSyntaxFlag_Pseudo) != 0 && value.data[0] == '+') {
        sign = MathMlLengthFlag_Increment;
        value = PrStringView{value.data + 1, value.size - 1};
    } else if ((syntax & MathMlLengthSyntaxFlag_Pseudo) != 0 && value.data[0] == '-') {
        MathMlLength magnitude{};
        if (mathMlParseLength(PrStringView{value.data + 1, value.size - 1}, syntax, &magnitude) &&
                mathMlLengthHasPseudoUnit(magnitude) && magnitude.flags == 0) {
            *out = magnitude;
            out->flags = MathMlLengthFlag_Decrement;
            return true;
        }
    } else if (value.data[0] == '+') {
        return false;
    }
    bool negative = value.size > 8 && prAsciiEqualsFold(PrStringView{value.data, 8}, "negative");
    PrStringView named = negative ? PrStringView{value.data + 8, value.size - 8} : value;
    for (uint32_t i = 0; i < countOf(kNamedSpaces); ++i) {
        if (prAsciiEqualsFold(named, kNamedSpaces[i])) {
            mathMlLengthAdd(out, double(negative ? -1 : 1) * (i + 1) / 18, MathMlUnit_Em);
            out->flags = sign;
            return true;
        }
    }
    bool parsed = false;
    if (value.size > 6 && prAsciiEqualsFold(PrStringView{value.data, 5}, "calc(") && value.data[value.size - 1] == ')') {
        parsed = parseCalc(PrStringView{value.data + 5, value.size - 6}, syntax, out);
    } else {
        size_t taken = 0;
        parsed = parseDimension(value, syntax, 1, out, &taken) && taken == value.size;
    }
    if (!parsed) {
        *out = MathMlLength{};
        return false;
    }
    out->flags = sign;
    return true;
}

MathMlBoolean mathMlParseBoolean(PrStringView text) {
    if (prAsciiEqualsFold(text, "true"))
        return MathMlBoolean_True;
    return prAsciiEqualsFold(text, "false") ? MathMlBoolean_False : MathMlBoolean_None;
}

bool mathMlParseUnsigned(PrStringView text, uint32_t* out) {
    if (text.size == 0)
        return false;
    uint64_t value = 0;
    for (size_t i = 0; i < text.size; ++i) {
        if (!prAsciiIsDigit(text.data[i]))
            return false;
        value = value * 10 + uint64_t(text.data[i] - '0');
        if (value > 0xffffffffu)
            return false;
    }
    *out = uint32_t(value);
    return true;
}

bool mathMlParseInteger(PrStringView text, int32_t* out) {
    bool negative = text.size != 0 && text.data[0] == '-';
    uint32_t magnitude = 0;
    if (!mathMlParseUnsigned(negative ? PrStringView{text.data + 1, text.size - 1} : text, &magnitude) ||
            magnitude > 0x7fffffffu)
        return false;
    *out = negative ? -int32_t(magnitude) : int32_t(magnitude);
    return true;
}

bool mathMlParseScriptLevel(PrStringView text, int32_t* level, bool* relative) {
    bool sign = text.size != 0 && (text.data[0] == '+' || text.data[0] == '-');
    uint32_t magnitude = 0;
    if (!mathMlParseUnsigned(sign ? PrStringView{text.data + 1, text.size - 1} : text, &magnitude) ||
            magnitude > 0x7fffffffu)
        return false;
    *level = sign && text.data[0] == '-' ? -int32_t(magnitude) : int32_t(magnitude);
    *relative = sign;
    return true;
}

PrStringView mathMlListItem(PrStringView text, uint32_t index) {
    PrStringView last = {};
    size_t at = 0;
    for (uint32_t i = 0;; ++i) {
        while (at < text.size && prAsciiIsSpace(text.data[at]))
            ++at;
        if (at == text.size)
            return last;
        size_t start = at;
        while (at < text.size && !prAsciiIsSpace(text.data[at]))
            ++at;
        last = PrStringView{text.data + start, at - start};
        if (i == index)
            return last;
    }
}

uint32_t mathMlListLength(PrStringView text) {
    uint32_t count = 0;
    size_t at = 0;
    for (;;) {
        while (at < text.size && prAsciiIsSpace(text.data[at]))
            ++at;
        if (at == text.size)
            return count;
        while (at < text.size && !prAsciiIsSpace(text.data[at]))
            ++at;
        ++count;
    }
}

PrStringView mathMlAttributeValue(const PrDomElement* element, PrStringView name) {
    return domAttributeValue(element, name);
}

bool mathMlInherits(PrStringView name) {
    // The attributes whose defaults an mstyle gives (mathml4/#presm_mstyle). They are the attributes of mo that the
    // dictionary otherwise gives, and the attributes of the layout schemata that are not sizes of one element. The
    // sizes of an mspace or an mpadded, and the image of an mglyph, belong to the element alone.
    static constexpr PrStringView kInherited[] = {
        "form", "fence", "separator", "lspace", "rspace", "stretchy", "symmetric", "maxsize", "minsize", "largeop",
        "movablelimits", "accent", "accentunder", "linebreak", "lineleading", "linebreakstyle", "linebreakmultchar",
        "indentalign", "indentshift", "indenttarget", "indentalignfirst", "indentshiftfirst", "indentalignlast",
        "indentshiftlast", "linethickness", "numalign", "denomalign", "bevelled", "align", "rowalign", "columnalign",
        "columnwidth", "rowspacing", "columnspacing", "rowlines", "columnlines", "frame", "framespacing", "equalrows",
        "equalcolumns", "side", "minlabelspacing", "notation", "open", "close", "separators", "stackalign",
        "charalign", "charspacing", "longdivstyle", "location", "crossout", "decimalcharacter", "mathvariant",
        "infixlinebreakstyle",
    };
    for (PrStringView inherited : kInherited) {
        if (name == inherited)
            return true;
    }
    return false;
}

PrStringView mathMlInheritedValue(const PrDomElement* element, PrStringView name) {
    PrStringView value = mathMlAttributeValue(element, name);
    if (value.data != nullptr || !mathMlInherits(name) || mathMlIs(element, PrDomTag_Math))
        return value;
    for (const PrDomElement* at = domNodeParentElement(element); at != nullptr && at->space == PrDomNamespace_MathMl;
            at = domNodeParentElement(at)) {
        if (at->tag == PrDomTag_Mstyle || at->tag == PrDomTag_Math) {
            value = mathMlAttributeValue(at, name);
            if (value.data != nullptr || at->tag == PrDomTag_Math)
                return value;
        }
    }
    return PrStringView{};
}

MathMlBoolean mathMlBooleanAttribute(const PrDomElement* element, PrStringView name) {
    return mathMlParseBoolean(valueOf(element, name));
}

MathMlLength mathMlLengthAttribute(const PrDomElement* element, PrStringView name, MathMlLengthSyntax syntax) {
    MathMlLength out{};
    PrStringView value = valueOf(element, name);
    if (value.data != nullptr)
        mathMlParseLength(value, syntax, &out);
    return out;
}

MathMlOperatorAttributes mathMlOperatorAttributes(const PrDomElement* element) {
    static constexpr Keyword kForms[] = {
        {"infix", MathMlForm_Infix}, {"prefix", MathMlForm_Prefix}, {"postfix", MathMlForm_Postfix},
    };
    struct Flag {
        PrStringView name;
        MathMlOperatorFlags flag;
    };
    static constexpr Flag kFlags[] = {
        {"stretchy", MathMlOperatorFlag_Stretchy}, {"symmetric", MathMlOperatorFlag_Symmetric},
        {"largeop", MathMlOperatorFlag_LargeOp}, {"movablelimits", MathMlOperatorFlag_MovableLimits},
        {"fence", MathMlOperatorFlag_Fence}, {"separator", MathMlOperatorFlag_Separator},
    };
    MathMlOperatorAttributes out{};
    uint8_t form = 0;
    out.hasForm = keywordOf(valueOf(element, "form"), kForms, &form);
    out.form = form;
    for (const Flag& flag : kFlags) {
        MathMlBoolean value = mathMlBooleanAttribute(element, flag.name);
        if (value == MathMlBoolean_None)
            continue;
        out.present |= flag.flag;
        if (value == MathMlBoolean_True)
            out.flags |= flag.flag;
    }
    out.lspace = mathMlLengthAttribute(element, "lspace", MathMlLengthSyntax_Normal);
    out.rspace = mathMlLengthAttribute(element, "rspace", MathMlLengthSyntax_Normal);
    out.minsize = mathMlLengthAttribute(element, "minsize", MathMlLengthSyntaxFlag_Unitless);
    out.maxsize = mathMlLengthAttribute(element, "maxsize",
        MathMlLengthSyntaxFlag_Unitless | MathMlLengthSyntaxFlag_Infinity);
    out.accent = mathMlBooleanAttribute(element, "accent");
    return out;
}

MathMlLineBreaking mathMlLineBreaking(const PrDomElement* element) {
    static constexpr Keyword kBreaks[] = {
        {"auto", MathMlLineBreak_Auto}, {"newline", MathMlLineBreak_Newline}, {"nobreak", MathMlLineBreak_NoBreak},
        {"goodbreak", MathMlLineBreak_GoodBreak}, {"badbreak", MathMlLineBreak_BadBreak},
    };
    static constexpr Keyword kStyles[] = {
        {"before", MathMlLineBreakStyle_Before}, {"after", MathMlLineBreakStyle_After},
        {"duplicate", MathMlLineBreakStyle_Duplicate}, {"infixlinebreakstyle", MathMlLineBreakStyle_Infix},
    };
    static constexpr Keyword kIndents[] = {
        {"left", MathMlIndentAlign_Left}, {"center", MathMlIndentAlign_Center}, {"right", MathMlIndentAlign_Right},
        {"auto", MathMlIndentAlign_Auto}, {"id", MathMlIndentAlign_Id}, {"indentalign", MathMlIndentAlign_IndentAlign},
    };
    MathMlLineBreaking out{};
    out.lineBreak = keywordOr(trimmed(valueOf(element, "linebreak")), kBreaks, MathMlLineBreak_Auto);
    out.style = keywordOr(trimmed(valueOf(element, "linebreakstyle")), kStyles, MathMlLineBreakStyle_None);
    // indentalign itself takes no indentalign keyword.
    out.indentAlign = keywordOr(trimmed(valueOf(element, "indentalign")), kIndents, MathMlIndentAlign_None);
    if (out.indentAlign == MathMlIndentAlign_IndentAlign)
        out.indentAlign = MathMlIndentAlign_None;
    out.indentAlignFirst = keywordOr(trimmed(valueOf(element, "indentalignfirst")), kIndents, MathMlIndentAlign_None);
    out.indentAlignLast = keywordOr(trimmed(valueOf(element, "indentalignlast")), kIndents, MathMlIndentAlign_None);
    out.lineLeading = mathMlLengthAttribute(element, "lineleading", MathMlLengthSyntax_Normal);
    out.indentShift = mathMlLengthAttribute(element, "indentshift", MathMlLengthSyntax_Normal);
    out.indentShiftFirst = mathMlLengthAttribute(element, "indentshiftfirst", MathMlLengthSyntax_Normal);
    out.indentShiftLast = mathMlLengthAttribute(element, "indentshiftlast", MathMlLengthSyntax_Normal);
    out.multChar = valueOf(element, "linebreakmultchar");
    out.indentTarget = valueOf(element, "indenttarget");
    return out;
}

MathMlSpace mathMlSpace(const PrDomElement* element) {
    return MathMlSpace{
        .width = mathMlLengthAttribute(element, "width", MathMlLengthSyntax_Normal),
        .height = mathMlLengthAttribute(element, "height", MathMlLengthSyntax_Normal),
        .depth = mathMlLengthAttribute(element, "depth", MathMlLengthSyntax_Normal),
    };
}

MathMlFraction mathMlFraction(const PrDomElement* element) {
    MathMlFraction out{};
    PrStringView thickness = valueOf(element, "linethickness");
    if (thickness.data != nullptr)
        out.lineThickness = thicknessOf(thickness);
    out.numeratorAlign = keywordOr(trimmed(valueOf(element, "numalign")), kAligns, MathMlAlign_None);
    out.denominatorAlign = keywordOr(trimmed(valueOf(element, "denomalign")), kAligns, MathMlAlign_None);
    out.bevelled = mathMlBooleanAttribute(element, "bevelled") == MathMlBoolean_True;
    return out;
}

MathMlPadded mathMlPadded(const PrDomElement* element) {
    // The sizes belong to the mpadded alone. An mstyle's lspace is the default for its operators.
    PrStringView names[] = {"width", "height", "depth", "lspace", "voffset"};
    MathMlLength lengths[5] = {};
    for (uint32_t i = 0; i < 5; ++i) {
        PrStringView value = mathMlAttributeValue(element, names[i]);
        if (value.data != nullptr)
            mathMlParseLength(value, MathMlLengthSyntaxFlag_Pseudo, &lengths[i]);
    }
    return MathMlPadded{
        .width = lengths[0],
        .height = lengths[1],
        .depth = lengths[2],
        .lspace = lengths[3],
        .voffset = lengths[4],
    };
}

MathMlUnderOver mathMlUnderOver(const PrDomElement* element) {
    MathMlUnderOver out{};
    if (!mathMlIs(element, PrDomTag_Munder))
        out.accent = mathMlBooleanAttribute(element, "accent");
    if (!mathMlIs(element, PrDomTag_Mover))
        out.accentUnder = mathMlBooleanAttribute(element, "accentunder");
    out.align = keywordOr(trimmed(valueOf(element, "align")), kAligns, MathMlAlign_None);
    return out;
}

MathMlNotationFlags mathMlNotations(const PrDomElement* element) {
    struct Notation {
        PrStringView name;
        MathMlNotationFlags flag;
    };
    static constexpr Notation kNotations[] = {
        {"longdiv", MathMlNotationFlag_LongDiv}, {"actuarial", MathMlNotationFlag_Actuarial},
        {"phasorangle", MathMlNotationFlag_PhasorAngle}, {"radical", MathMlNotationFlag_Radical},
        {"box", MathMlNotationFlag_Box}, {"roundedbox", MathMlNotationFlag_RoundedBox},
        {"circle", MathMlNotationFlag_Circle}, {"left", MathMlNotationFlag_Left}, {"right", MathMlNotationFlag_Right},
        {"top", MathMlNotationFlag_Top}, {"bottom", MathMlNotationFlag_Bottom},
        {"updiagonalstrike", MathMlNotationFlag_UpDiagonalStrike},
        {"downdiagonalstrike", MathMlNotationFlag_DownDiagonalStrike},
        {"verticalstrike", MathMlNotationFlag_VerticalStrike},
        {"horizontalstrike", MathMlNotationFlag_HorizontalStrike}, {"madruwb", MathMlNotationFlag_Madruwb},
        {"text", MathMlNotationFlag_Text}, {"uparrow", MathMlNotationFlag_UpArrow},
        {"rightarrow", MathMlNotationFlag_RightArrow}, {"downarrow", MathMlNotationFlag_DownArrow},
        {"leftarrow", MathMlNotationFlag_LeftArrow}, {"northwestarrow", MathMlNotationFlag_NorthWestArrow},
        {"southwestarrow", MathMlNotationFlag_SouthWestArrow}, {"southeastarrow", MathMlNotationFlag_SouthEastArrow},
        {"northeastarrow", MathMlNotationFlag_NorthEastArrow},
        // MathML 3 named the north-east arrow updiagonalarrow.
        {"updiagonalarrow", MathMlNotationFlag_NorthEastArrow}, {"updownarrow", MathMlNotationFlag_UpDownArrow},
        {"leftrightarrow", MathMlNotationFlag_LeftRightArrow},
        {"northwestsoutheastarrow", MathMlNotationFlag_NorthWestSouthEastArrow},
        {"northeastsouthwestarrow", MathMlNotationFlag_NorthEastSouthWestArrow},
    };
    PrStringView text = valueOf(element, "notation");
    MathMlNotationFlags flags = 0;
    for (uint32_t i = 0;; ++i) {
        PrStringView item = mathMlListItem(text, i);
        PrStringView next = mathMlListItem(text, i + 1);
        for (const Notation& notation : kNotations) {
            if (prAsciiEqualsFold(item, notation.name))
                flags |= notation.flag;
        }
        // The list repeats its last item past its end.
        if (item.data == nullptr || next.data == item.data)
            return flags;
    }
}

PrStringView mathMlFencedOpen(const PrDomElement* element) {
    PrStringView value = valueOf(element, "open");
    return value.data != nullptr ? trimmed(value) : PrStringView("(");
}

PrStringView mathMlFencedClose(const PrDomElement* element) {
    PrStringView value = valueOf(element, "close");
    return value.data != nullptr ? trimmed(value) : PrStringView(")");
}

uint32_t mathMlFencedSeparator(const PrDomElement* element, uint32_t index) {
    PrStringView value = valueOf(element, "separators");
    if (value.data == nullptr)
        return ',';
    uint32_t found = 0;
    uint32_t count = 0;
    for (size_t at = 0; at < value.size;) {
        uint32_t codePoint = 0;
        at += prUtf8Decode(value, at, &codePoint);
        if (codePoint < 0x80 && prAsciiIsSpace(char(codePoint)))
            continue;
        found = codePoint;
        if (count++ == index)
            return found;
    }
    return found;
}

void mathMlCellSpans(const PrDomElement* element, uint32_t* columns, uint32_t* rows) {
    int64_t value = 0;
    *columns = 1;
    *rows = 1;
    if (domParseInteger(mathMlAttributeValue(element, "columnspan"), &value) && value >= 1)
        *columns = uint32_t(value < kPrLayoutTableMaxColumnSpan ? value : kPrLayoutTableMaxColumnSpan);
    if (domParseInteger(mathMlAttributeValue(element, "rowspan"), &value) && value >= 0)
        *rows = uint32_t(value < kPrLayoutTableMaxRowSpan ? value : kPrLayoutTableMaxRowSpan);
}

MathMlTableAlign mathMlTableAlign(const PrDomElement* element) {
    bool table = mathMlIs(element, PrDomTag_Mtable);
    return tableAlignOf(valueOf(element, "align"), table ? MathMlVerticalAlign_Axis : MathMlVerticalAlign_Baseline);
}

MathMlVerticalAlign mathMlCellRowAlign(const PrDomElement* element, uint32_t row) {
    MathMlVerticalAlign align = verticalAlignItem(ownValue(element, "rowalign"), 0);
    const PrDomElement* rowElement = rowOf(element);
    if (align == MathMlVerticalAlign_None)
        align = verticalAlignItem(ownValue(rowElement, "rowalign"), 0);
    if (align == MathMlVerticalAlign_None)
        align = verticalAlignItem(inheritedValue(tableOf(rowElement), "rowalign"), row);
    return align;
}

MathMlAlign mathMlCellColumnAlign(const PrDomElement* element, uint32_t column) {
    MathMlAlign align = alignItem(ownValue(element, "columnalign"), 0);
    const PrDomElement* rowElement = rowOf(element);
    if (align == MathMlAlign_None)
        align = alignItem(ownValue(rowElement, "columnalign"), column);
    if (align == MathMlAlign_None)
        align = alignItem(inheritedValue(tableOf(rowElement), "columnalign"), column);
    return align;
}

MathMlTable mathMlTable(const PrDomElement* element) {
    MathMlTable out{};
    out.frame = keywordOr(trimmed(valueOf(element, "frame")), kLines, MathMlLine_None);
    PrStringView spacing = valueOf(element, "framespacing");
    for (uint32_t i = 0; i < 2 && spacing.data != nullptr; ++i)
        mathMlParseLength(mathMlListItem(spacing, i), MathMlLengthSyntax_Normal, &out.frameSpacing[i]);
    out.equalRows = mathMlBooleanAttribute(element, "equalrows") == MathMlBoolean_True;
    out.equalColumns = mathMlBooleanAttribute(element, "equalcolumns") == MathMlBoolean_True;
    PrStringView width = valueOf(element, "width");
    if (!prAsciiEqualsFold(trimmed(width), "auto"))
        out.width = mathMlLengthAttribute(element, "width", MathMlLengthSyntax_Normal);
    out.displayStyle = mathMlParseBoolean(mathMlAttributeValue(element, "displaystyle"));
    out.side = trimmed(valueOf(element, "side"));
    out.minLabelSpacing = mathMlLengthAttribute(element, "minlabelspacing", MathMlLengthSyntax_Normal);
    return out;
}

MathMlLength mathMlTableRowSpacing(const PrDomElement* element, uint32_t gap) {
    MathMlLength out{};
    mathMlParseLength(mathMlListItem(valueOf(element, "rowspacing"), gap), MathMlLengthSyntax_Normal, &out);
    return out;
}

MathMlLength mathMlTableColumnSpacing(const PrDomElement* element, uint32_t gap) {
    MathMlLength out{};
    mathMlParseLength(mathMlListItem(valueOf(element, "columnspacing"), gap), MathMlLengthSyntax_Normal, &out);
    return out;
}

MathMlLine mathMlTableRowLine(const PrDomElement* element, uint32_t gap) {
    return keywordOr(mathMlListItem(valueOf(element, "rowlines"), gap), kLines, MathMlLine_None);
}

MathMlLine mathMlTableColumnLine(const PrDomElement* element, uint32_t gap) {
    return keywordOr(mathMlListItem(valueOf(element, "columnlines"), gap), kLines, MathMlLine_None);
}

uint32_t mathMlTableRowItems(const PrDomElement* element) {
    uint32_t align = mathMlListLength(valueOf(element, "rowalign"));
    uint32_t spacing = mathMlListLength(valueOf(element, "rowspacing"));
    uint32_t lines = mathMlListLength(valueOf(element, "rowlines"));
    uint32_t most = align > spacing ? align : spacing;
    return most > lines ? most : lines;
}

MathMlColumnWidth mathMlTableColumnWidth(const PrDomElement* element, uint32_t column, MathMlLength* length) {
    *length = MathMlLength{};
    PrStringView item = mathMlListItem(valueOf(element, "columnwidth"), column);
    if (prAsciiEqualsFold(item, "fit"))
        return MathMlColumnWidth_Fit;
    if (mathMlParseLength(item, MathMlLengthSyntax_Normal, length))
        return MathMlColumnWidth_Length;
    return MathMlColumnWidth_Auto;
}

MathMlStack mathMlStack(const PrDomElement* element) {
    static constexpr Keyword kStackAligns[] = {
        {"left", MathMlAlign_Left}, {"center", MathMlAlign_Center}, {"right", MathMlAlign_Right},
        {"decimalpoint", MathMlAlign_DecimalPoint},
    };
    // MathML 4 leaves loose and tight to the renderer. They are a quarter more and less than medium.
    static constexpr Keyword kSpacings[] = {{"loose", 125}, {"medium", 100}, {"tight", 75}};
    MathMlStack out{};
    out.align = mathMlTableAlign(element);
    out.stackAlign = keywordOr(trimmed(valueOf(element, "stackalign")), kStackAligns, MathMlAlign_DecimalPoint);
    out.charAlign = keywordOr(trimmed(valueOf(element, "charalign")), kAligns, MathMlAlign_Right);
    PrStringView spacing = trimmed(valueOf(element, "charspacing"));
    uint8_t percent = 0;
    if (keywordOf(spacing, kSpacings, &percent))
        mathMlLengthAdd(&out.charSpacing, percent, MathMlUnit_Percent);
    else if (spacing.data != nullptr)
        mathMlParseLength(spacing, MathMlLengthSyntax_Normal, &out.charSpacing);
    out.decimalCharacter = '.';
    PrStringView decimal = valueOf(element, "decimalcharacter");
    uint32_t codePoint = 0;
    if (decimal.size != 0 && prUtf8Decode(decimal, 0, &codePoint) == decimal.size)
        out.decimalCharacter = codePoint;
    return out;
}

MathMlLongDivStyle mathMlLongDivStyle(const PrDomElement* element) {
    static constexpr Keyword kStyles[] = {
        {"lefttop", MathMlLongDivStyle_LeftTop}, {"stackedrightright", MathMlLongDivStyle_StackedRightRight},
        {"mediumstackedrightright", MathMlLongDivStyle_MediumStackedRightRight},
        {"shortstackedrightright", MathMlLongDivStyle_ShortStackedRightRight},
        {"righttop", MathMlLongDivStyle_RightTop}, {"left/\\right", MathMlLongDivStyle_LeftSlashRight},
        {"left)(right", MathMlLongDivStyle_LeftParenRight}, {":right=right", MathMlLongDivStyle_ColonRightEqualRight},
        {"stackedleftleft", MathMlLongDivStyle_StackedLeftLeft},
        {"stackedleftlinetop", MathMlLongDivStyle_StackedLeftLineTop},
    };
    return keywordOr(trimmed(valueOf(element, "longdivstyle")), kStyles, MathMlLongDivStyle_LeftTop);
}

int32_t mathMlStackPosition(const PrDomElement* element) {
    int32_t value = 0;
    return mathMlParseInteger(trimmed(mathMlAttributeValue(element, "position")), &value) ? value : 0;
}

int32_t mathMlStackShift(const PrDomElement* element) {
    int32_t value = 0;
    return mathMlParseInteger(trimmed(mathMlAttributeValue(element, "shift")), &value) ? value : 0;
}

MathMlCarry mathMlCarry(const PrDomElement* element) {
    static constexpr Keyword kLocations[] = {
        {"w", MathMlCarryLocation_W}, {"nw", MathMlCarryLocation_Nw}, {"n", MathMlCarryLocation_N},
        {"ne", MathMlCarryLocation_Ne}, {"e", MathMlCarryLocation_E}, {"se", MathMlCarryLocation_Se},
        {"s", MathMlCarryLocation_S}, {"sw", MathMlCarryLocation_Sw},
    };
    static constexpr Keyword kCrossouts[] = {
        {"none", 0}, {"updiagonalstrike", MathMlCrossoutFlag_UpDiagonalStrike},
        {"downdiagonalstrike", MathMlCrossoutFlag_DownDiagonalStrike},
        {"verticalstrike", MathMlCrossoutFlag_VerticalStrike},
        {"horizontalstrike", MathMlCrossoutFlag_HorizontalStrike},
    };
    // An mscarry without a location or crossout takes that of its mscarries.
    const PrDomElement* carries = mathMlIs(element, PrDomTag_Mscarry) ? parentOf(element, PrDomTag_Mscarries) : nullptr;
    MathMlCarry out{.location = MathMlCarryLocation_N, .scriptSizeMultiplier = 0.6f};
    const PrDomElement* sources[] = {element, carries};
    bool located = false;
    bool crossed = false;
    for (const PrDomElement* source : sources) {
        if (source == nullptr)
            continue;
        uint8_t location = 0;
        if (!located && keywordOf(trimmed(mathMlAttributeValue(source, "location")), kLocations, &location)) {
            out.location = location;
            located = true;
        }
        PrStringView crossout = mathMlAttributeValue(source, "crossout");
        if (!crossed && crossout.data != nullptr) {
            crossed = true;
            for (uint32_t i = 0;; ++i) {
                PrStringView item = mathMlListItem(crossout, i);
                out.crossout |= keywordOr(item, kCrossouts, 0);
                if (item.data == nullptr || mathMlListItem(crossout, i + 1).data == item.data)
                    break;
            }
        }
    }
    const PrDomElement* multiplied = carries != nullptr ? carries : element;
    double multiplier = 0;
    PrStringView text = trimmed(mathMlAttributeValue(multiplied, "scriptsizemultiplier"));
    if (text.size != 0 && prF64ParsePrefix(text, &multiplier) == text.size && multiplier > 0)
        out.scriptSizeMultiplier = float(multiplier);
    return out;
}

MathMlStackLine mathMlStackLine(const PrDomElement* element) {
    MathMlStackLine out{};
    uint32_t length = 0;
    if (mathMlParseUnsigned(trimmed(mathMlAttributeValue(element, "length")), &length))
        out.length = length;
    out.leftOverhang = mathMlLengthAttribute(element, "leftoverhang", MathMlLengthSyntax_Normal);
    out.rightOverhang = mathMlLengthAttribute(element, "rightoverhang", MathMlLengthSyntax_Normal);
    PrStringView thickness = mathMlAttributeValue(element, "mslinethickness");
    if (thickness.data != nullptr)
        out.thickness = thicknessOf(thickness);
    return out;
}

MathMlGlyph mathMlGlyph(const PrDomElement* element) {
    return MathMlGlyph{
        .src = mathMlAttributeValue(element, "src"),
        .alt = mathMlAttributeValue(element, "alt"),
        .width = mathMlLengthAttribute(element, "width", MathMlLengthSyntax_Normal),
        .height = mathMlLengthAttribute(element, "height", MathMlLengthSyntax_Normal),
        .valign = mathMlLengthAttribute(element, "valign", MathMlLengthSyntax_Normal),
    };
}

bool mathMlTokenVariant(const PrDomElement* element, PrUnicodeMathVariant* out) {
    static constexpr Keyword kVariants[] = {
        {"bold", PrUnicodeMathVariant_Bold}, {"italic", PrUnicodeMathVariant_Italic},
        {"bold-italic", PrUnicodeMathVariant_BoldItalic}, {"script", PrUnicodeMathVariant_Script},
        {"bold-script", PrUnicodeMathVariant_BoldScript}, {"fraktur", PrUnicodeMathVariant_Fraktur},
        {"double-struck", PrUnicodeMathVariant_DoubleStruck}, {"bold-fraktur", PrUnicodeMathVariant_BoldFraktur},
        {"sans-serif", PrUnicodeMathVariant_SansSerif}, {"bold-sans-serif", PrUnicodeMathVariant_BoldSansSerif},
        {"sans-serif-italic", PrUnicodeMathVariant_SansSerifItalic},
        {"sans-serif-bold-italic", PrUnicodeMathVariant_SansSerifBoldItalic},
        {"monospace", PrUnicodeMathVariant_Monospace}, {"initial", PrUnicodeMathVariant_Initial},
        {"tailed", PrUnicodeMathVariant_Tailed}, {"looped", PrUnicodeMathVariant_Looped},
        {"stretched", PrUnicodeMathVariant_Stretched},
    };
    uint8_t variant = 0;
    if (!mathMlIsTokenElement(element) || !keywordOf(trimmed(valueOf(element, "mathvariant")), kVariants, &variant))
        return false;
    *out = variant;
    return true;
}

} // namespace pr_impl
