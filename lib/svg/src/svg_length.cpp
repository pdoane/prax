// The SVG DOM's lengths: SVGLength and SVGAnimatedLength, reflecting the length attributes of SVG elements.
//
// SVG2 - https://svgwg.org/svg2-draft/types.html#InterfaceSVGLength

#include "pr/svg_length.h"

#include "pr/alloc.h"
#include "pr/ascii.h"
#include "pr/count_of.h"
#include "pr/float.h"
#include "pr/number_list.h"
#include "pr/string_ops.h"
#include "pr/text_buffer.h"

#include "css/src/css_serialize.h"
#include "dom/src/dom_node.h"
#include "svg/src/svg_parse.h"
#include "svg/src/svg_render.h"

namespace pr_impl {

namespace {

// A length attribute of an element's interface: its name, the direction that its percentages are of, and the value that
// it reflects if it is missing or does not parse.
struct LengthAttribute {
    PrDomTag tag;
    const char* name;
    SvgAxis axis;
    const char* initial;
};

constexpr LengthAttribute kAttributes[] = {
    {PrDomTag_Svg, "x", SvgAxis_X, "0"},
    {PrDomTag_Svg, "y", SvgAxis_Y, "0"},
    {PrDomTag_Svg, "width", SvgAxis_X, "100%"},
    {PrDomTag_Svg, "height", SvgAxis_Y, "100%"},
    {PrDomTag_Rect, "x", SvgAxis_X, "0"},
    {PrDomTag_Rect, "y", SvgAxis_Y, "0"},
    {PrDomTag_Rect, "width", SvgAxis_X, "0"},
    {PrDomTag_Rect, "height", SvgAxis_Y, "0"},
    {PrDomTag_Rect, "rx", SvgAxis_X, "0"},
    {PrDomTag_Rect, "ry", SvgAxis_Y, "0"},
    {PrDomTag_Circle, "cx", SvgAxis_X, "0"},
    {PrDomTag_Circle, "cy", SvgAxis_Y, "0"},
    {PrDomTag_Circle, "r", SvgAxis_Other, "0"},
    {PrDomTag_Ellipse, "cx", SvgAxis_X, "0"},
    {PrDomTag_Ellipse, "cy", SvgAxis_Y, "0"},
    {PrDomTag_Ellipse, "rx", SvgAxis_X, "0"},
    {PrDomTag_Ellipse, "ry", SvgAxis_Y, "0"},
    {PrDomTag_Line, "x1", SvgAxis_X, "0"},
    {PrDomTag_Line, "y1", SvgAxis_Y, "0"},
    {PrDomTag_Line, "x2", SvgAxis_X, "0"},
    {PrDomTag_Line, "y2", SvgAxis_Y, "0"},
    {PrDomTag_Image, "x", SvgAxis_X, "0"},
    {PrDomTag_Image, "y", SvgAxis_Y, "0"},
    {PrDomTag_Image, "width", SvgAxis_X, "0"},
    {PrDomTag_Image, "height", SvgAxis_Y, "0"},
    {PrDomTag_Use, "x", SvgAxis_X, "0"},
    {PrDomTag_Use, "y", SvgAxis_Y, "0"},
    {PrDomTag_Use, "width", SvgAxis_X, "0"},
    {PrDomTag_Use, "height", SvgAxis_Y, "0"},
    {PrDomTag_ForeignObject, "x", SvgAxis_X, "0"},
    {PrDomTag_ForeignObject, "y", SvgAxis_Y, "0"},
    {PrDomTag_ForeignObject, "width", SvgAxis_X, "0"},
    {PrDomTag_ForeignObject, "height", SvgAxis_Y, "0"},
    {PrDomTag_Pattern, "x", SvgAxis_X, "0"},
    {PrDomTag_Pattern, "y", SvgAxis_Y, "0"},
    {PrDomTag_Pattern, "width", SvgAxis_X, "0"},
    {PrDomTag_Pattern, "height", SvgAxis_Y, "0"},
    {PrDomTag_Mask, "x", SvgAxis_X, "-10%"},
    {PrDomTag_Mask, "y", SvgAxis_Y, "-10%"},
    {PrDomTag_Mask, "width", SvgAxis_X, "120%"},
    {PrDomTag_Mask, "height", SvgAxis_Y, "120%"},
    {PrDomTag_Filter, "x", SvgAxis_X, "-10%"},
    {PrDomTag_Filter, "y", SvgAxis_Y, "-10%"},
    {PrDomTag_Filter, "width", SvgAxis_X, "120%"},
    {PrDomTag_Filter, "height", SvgAxis_Y, "120%"},
    {PrDomTag_LinearGradient, "x1", SvgAxis_X, "0%"},
    {PrDomTag_LinearGradient, "y1", SvgAxis_Y, "0%"},
    {PrDomTag_LinearGradient, "x2", SvgAxis_X, "100%"},
    {PrDomTag_LinearGradient, "y2", SvgAxis_Y, "0%"},
    {PrDomTag_RadialGradient, "cx", SvgAxis_X, "50%"},
    {PrDomTag_RadialGradient, "cy", SvgAxis_Y, "50%"},
    {PrDomTag_RadialGradient, "r", SvgAxis_Other, "50%"},
    {PrDomTag_RadialGradient, "fx", SvgAxis_X, "50%"},
    {PrDomTag_RadialGradient, "fy", SvgAxis_Y, "50%"},
    {PrDomTag_RadialGradient, "fr", SvgAxis_Other, "0%"},
    {PrDomTag_Marker, "refX", SvgAxis_X, "0"},
    {PrDomTag_Marker, "refY", SvgAxis_Y, "0"},
    {PrDomTag_Marker, "markerWidth", SvgAxis_X, "3"},
    {PrDomTag_Marker, "markerHeight", SvgAxis_Y, "3"},
};

// The unit of each type of length, by PrSvgLengthType. Unknown and Number have none.
constexpr const char* kUnits[] = {"", "", "%", "em", "ex", "px", "cm", "mm", "in", "pt", "pc"};

// A length's value: a number times the unit of its type, or for Unknown, a value of another kind as its text.
struct LengthValue {
    PrSvgLengthType type;
    float factor;
    PrString text;              // Unknown: the value as written
};

// Parses `text` as a length. Returns false if it is not one.
bool parse(PrStringView text, LengthValue* out) {
    size_t at = 0;
    float number = 0;
    if (!numberListRead(text, &at, &number))
        return false;
    PrStringView unit = prStringTrimEnd(prStringSlice(text, at, text.size - at));
    for (uint32_t type = PrSvgLengthType_Percentage; type < countOf(kUnits); ++type) {
        if (prAsciiEqualsFold(unit, kUnits[type])) {
            *out = LengthValue{.type = PrSvgLengthType(type), .factor = number};
            return true;
        }
    }
    if (unit.size == 0) {
        *out = LengthValue{.type = PrSvgLengthType_Number, .factor = number};
        return true;
    }
    // Another unit of CSS, which a length takes (w3c/svgwg#1216).
    SvgLengthPercentage other{};
    if (!svgParseLength(text, nullptr, &other))
        return false;
    *out = LengthValue{.type = PrSvgLengthType_Unknown, .factor = 0, .text = prStringCopy(text)};
    return true;
}

// Returns the value as a string (SVG2/types.html#__svg__SVGLength__valueAsString).
PrString serialize(const LengthValue& value) {
    if (value.type == PrSvgLengthType_Unknown)
        return prStringClone(&value.text);
    PrTextBuffer out{};
    cssSerializeNumber(&out, double(value.factor));
    prTextBufferAppend(&out, kUnits[value.type]);
    PrString text = prTextBufferCopy(&out);
    prTextBufferReset(&out);
    return text;
}

// What a length's value resolves against: the viewport that its percentages are of, and its element's font size.
struct Basis {
    float percent;              // the length that 100% is
    float fontSize;
};

// Returns the length of a unit of `type` in user units, or 0 for Unknown.
float unitLength(PrSvgLengthType type, const Basis& basis) {
    switch (type) {
        case PrSvgLengthType_Number:
        case PrSvgLengthType_Px: return 1;
        case PrSvgLengthType_Percentage: return basis.percent / 100;
        case PrSvgLengthType_Ems: return basis.fontSize;
        case PrSvgLengthType_Exs: return basis.fontSize / 2;
        case PrSvgLengthType_Cm: return 96 / 2.54f;
        case PrSvgLengthType_Mm: return 96 / 25.4f;
        case PrSvgLengthType_In: return 96;
        case PrSvgLengthType_Pt: return 96.0f / 72;
        case PrSvgLengthType_Pc: return 16;
        default: return 0;
    }
}

// An animated length: an element's length attribute.
struct SvgAnimatedLength {
    PrSvgElement* element;      // holds a reference
    const LengthAttribute* attribute;
};

// A length that reflects an attribute, read only for its animated value, or a detached one, which holds its value.
struct SvgLength {
    PrSvgElement* PR_NULLABLE element;  // holds a reference; null if detached
    const LengthAttribute* PR_NULLABLE attribute;
    bool readOnly;
    LengthValue value;                  // the value of a detached length
};

// Returns the value of a length: the attribute's, or its initial value if it is missing or does not parse.
LengthValue valueOf(const SvgLength* length) {
    if (length->element == nullptr) {
        LengthValue value = length->value;
        value.text = prStringClone(&length->value.text);
        return value;
    }
    LengthValue value{};
    PrStringView text = svgAttribute(length->element, length->attribute->name);
    if (text.data == nullptr || !parse(text, &value))
        parse(length->attribute->initial, &value);
    return value;
}

// Sets a length's value, and writes a reflected attribute again. The call takes ownership of the value's text.
void setValue(SvgLength* length, LengthValue value) {
    if (length->element == nullptr) {
        prStringReset(&length->value.text);
        length->value = value;
        return;
    }
    PrString text = serialize(value);
    domSetAttribute(length->element, PrDomNamespace_None, length->attribute->name, prStringView(&text));
    prStringReset(&text);
    prStringReset(&value.text);
}

// Returns what a length resolves against: its element's viewport along the attribute's direction and its font size, or
// 100 and the initial font size if it is detached.
Basis basisOf(const SvgLength* length) {
    if (length->element == nullptr)
        return Basis{100, float(kCssMediumFontSize)};
    SvgLengthBasis basis{};
    svgElementLengthBasis(length->element, &basis);
    float percent;
    if (length->attribute->axis == SvgAxis_X)
        percent = basis.viewport.x;
    else if (length->attribute->axis == SvgAxis_Y)
        percent = basis.viewport.y;
    else
        percent = prF32Sqrt((basis.viewport.x * basis.viewport.x + basis.viewport.y * basis.viewport.y) / 2);
    return Basis{percent, basis.fontSize};
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

// C opaque types
struct PrSvgAnimatedLength : SvgAnimatedLength {};
struct PrSvgLength : SvgLength {};

PrSvgLength* prSvgLengthCreate(void) {
    PrSvgLength* length = allocZeroed<PrSvgLength>();
    length->value.type = PrSvgLengthType_Number;
    return length;
}

void prSvgLengthDestroy(PrSvgLength* PR_NULLABLE length) {
    if (length == nullptr)
        return;
    if (length->element != nullptr)
        prDomNodeRelease(prSvgElementAsNode(length->element));
    prStringReset(&length->value.text);
    prMemoryFree(length);
}

PrSvgAnimatedLength* PR_NULLABLE prSvgAnimatedLengthCreate(PrSvgElement* element, PrStringView name) {
    for (const LengthAttribute& attribute : kAttributes) {
        if (attribute.tag != element->tag || name != attribute.name)
            continue;
        PrSvgAnimatedLength* length = allocZeroed<PrSvgAnimatedLength>();
        prDomNodeRetain(prSvgElementAsNode(element));
        length->element = element;
        length->attribute = &attribute;
        return length;
    }
    return nullptr;
}

void prSvgAnimatedLengthDestroy(PrSvgAnimatedLength* PR_NULLABLE length) {
    if (length == nullptr)
        return;
    prDomNodeRelease(prSvgElementAsNode(length->element));
    prMemoryFree(length);
}

PrSvgLength* prSvgAnimatedLengthBaseVal(const PrSvgAnimatedLength* animated) {
    PrSvgLength* length = allocZeroed<PrSvgLength>();
    prDomNodeRetain(prSvgElementAsNode(animated->element));
    length->element = animated->element;
    length->attribute = animated->attribute;
    return length;
}

PrSvgLength* prSvgAnimatedLengthAnimVal(const PrSvgAnimatedLength* animated) {
    PrSvgLength* length = prSvgAnimatedLengthBaseVal(animated);
    length->readOnly = true;
    return length;
}

PrSvgLengthType prSvgLengthUnitType(const PrSvgLength* length) {
    LengthValue value = valueOf(length);
    prStringReset(&value.text);
    return value.type;
}

float prSvgLengthValue(const PrSvgLength* svgLength) {
    LengthValue value = valueOf(svgLength);
    float result = 0;
    if (value.type == PrSvgLengthType_Number) {
        result = value.factor;
    } else if (value.type == PrSvgLengthType_Unknown) {
        // Another unit of CSS resolves as SVG resolves the attribute.
        SvgLengthPercentage length{};
        Basis basis = basisOf(svgLength);
        if (svgParseLength(prStringView(&value.text), nullptr, &length))
            result = svgLengthResolve(length, basis.percent);
    } else {
        result = value.factor * unitLength(value.type, basisOf(svgLength));
    }
    prStringReset(&value.text);
    return result;
}

float prSvgLengthValueInSpecifiedUnits(const PrSvgLength* length) {
    LengthValue value = valueOf(length);
    prStringReset(&value.text);
    return value.factor;
}

PrString prSvgLengthValueAsString(const PrSvgLength* length) {
    LengthValue value = valueOf(length);
    PrString text = serialize(value);
    prStringReset(&value.text);
    return text;
}

PrStatus prSvgLengthSetValue(PrSvgLength* length, float value) {
    if (length->readOnly)
        return PrStatus_ReadOnly;
    setValue(length, LengthValue{.type = PrSvgLengthType_Number, .factor = value});
    return PrStatus_Ok;
}

PrStatus prSvgLengthSetValueInSpecifiedUnits(PrSvgLength* length, float value) {
    if (length->readOnly)
        return PrStatus_ReadOnly;
    LengthValue current = valueOf(length);
    prStringReset(&current.text);
    // A value of another kind becomes a number.
    PrSvgLengthType type = current.type == PrSvgLengthType_Unknown ? PrSvgLengthType_Number : current.type;
    setValue(length, LengthValue{.type = type, .factor = value});
    return PrStatus_Ok;
}

PrStatus prSvgLengthSetValueAsString(PrSvgLength* length, PrStringView text) {
    if (length->readOnly)
        return PrStatus_ReadOnly;
    LengthValue value{};
    if (!parse(text, &value))
        return PrStatus_Syntax;
    setValue(length, value);
    return PrStatus_Ok;
}

PrStatus prSvgLengthNewValueSpecifiedUnits(PrSvgLength* length, PrSvgLengthType type, float value) {
    if (length->readOnly)
        return PrStatus_ReadOnly;
    if (type == PrSvgLengthType_Unknown || type >= countOf(kUnits))
        return PrStatus_NotSupported;
    setValue(length, LengthValue{.type = type, .factor = value});
    return PrStatus_Ok;
}

PrStatus prSvgLengthConvertToSpecifiedUnits(PrSvgLength* length, PrSvgLengthType type) {
    if (length->readOnly)
        return PrStatus_ReadOnly;
    if (type == PrSvgLengthType_Unknown || type >= countOf(kUnits))
        return PrStatus_NotSupported;
    float user = prSvgLengthValue(length);
    float unit = unitLength(type, basisOf(length));
    setValue(length, LengthValue{.type = type, .factor = unit != 0 ? user / unit : 0});
    return PrStatus_Ok;
}
