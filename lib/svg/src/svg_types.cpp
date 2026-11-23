// The values of SVG's attributes, and the typed reads and writes of an element's attributes
//
// SVG2 - https://svgwg.org/svg2-draft/types.html

#include "svg/src/svg_types.h"

#include "pr/ascii.h"
#include "pr/float.h"
#include "pr/float_convert.h"
#include "pr/number_list.h"
#include "pr/raw_array.h"
#include "pr/string_ops.h"
#include "pr/text_buffer.h"

#include "css/src/css_presentation.h"
#include "css/src/css_presentation_hooks.h"
#include "css/src/css_property.h"
#include "css/src/css_serialize.h"
#include "css/src/css_unit.h"
#include "dom/src/dom_node.h"
#include "svg/src/svg_animate.h"
#include "svg/src/svg_animate_value.h"
#include "svg/src/svg_attribute.h"
#include "svg/src/svg_parse.h"
#include "svg/src/svg_render.h"
#include "svg/src/svg_tree.h"

namespace pr_impl {

namespace {

// ---------------------------------------------------------------------------------------------------------------------
// Text

// Appends a number as its shortest text as a float.
void appendFloat(PrTextBuffer* out, double value) {
    char text[kPrF64TextMaxBytes];
    prTextBufferAppend(out, PrStringView{text, uint32_t(prF32Format(float(value), text))});
}

// Returns the text of a buffer as a string, and resets the buffer.
PrRawString taken(PrTextBuffer* text) {
    PrRawString out = prTextBufferCopy(text);
    if (prRawStringIsNull(&out))
        out = prRawStringEmpty();
    prTextBufferReset(text);
    return out;
}

// Returns what the element reads the attribute as, if it is of `type`.
bool typedAs(const PrSvgElement* element, PrSvgAttribute attribute, PrSvgValueType type, SvgTypedAttribute* out) {
    return svgTypedAttribute(PrDomTag(element->tag), attribute, out) && out->type == type;
}

// Returns the attribute's text, without surrounding white space, or the null view if it is missing: its own, or the
// value that animations give it.
PrStringView textOf(const PrSvgElement* element, PrSvgAttribute attribute, PrSvgLayer layer) {
    if (attribute == PrSvgAttribute_Href)
        return layer == PrSvgLayer_Animated ? svgHref(element) : svgBaseHref(element);
    PrStringView name = prSvgAttributeName(attribute);
    return layer == PrSvgLayer_Animated ? svgAttribute(element, name) : svgBaseAttribute(element, name);
}

// Returns the value that the animations compose for the attribute, if a read of the animated value takes it and it is
// of `type`, whose components the read takes as they are. A value of another type, as a set writes, is read from its
// text.
const SvgAnimateValue* PR_NULLABLE composedOf(const PrSvgElement* element, PrSvgAttribute attribute, PrSvgLayer layer,
        PrSvgValueType type) {
    if (layer != PrSvgLayer_Animated)
        return nullptr;
    const SvgAnimateValue* value = svgAnimatedValue(element, prSvgAttributeName(attribute));
    return value != nullptr && value->valid && value->type == type ? value : nullptr;
}

// Writes the text of an attribute. An element's href is written to xlink:href if that is the attribute it has, since it
// is the one that the element's href reflects (SVG2/types.html#__svg__SVGURIReference__href).
void writeText(PrSvgElement* element, PrSvgAttribute attribute, PrStringView text) {
    if (attribute == PrSvgAttribute_Href && domAttribute(element, PrDomNamespace_None, "href") == nullptr &&
            domAttribute(element, PrDomNamespace_XLink, "href") != nullptr) {
        domSetAttribute(element, PrDomNamespace_XLink, "href", text);
        return;
    }
    domSetAttribute(element, PrDomNamespace_None, prSvgAttributeName(attribute), text);
}

// Writes the text of a buffer to the attribute, and resets the buffer.
void writeBuffer(PrSvgElement* element, PrSvgAttribute attribute, PrTextBuffer* text) {
    PrStringView view = prTextBufferView(text);
    writeText(element, attribute, view.data != nullptr ? view : PrStringView(""));
    prTextBufferReset(text);
}

// Reads the numbers of `text`, separated by white space or a comma, at most `most` of them. Returns the number read, or
// 0 if the text is not that many finite numbers.
uint32_t numbersOf(PrStringView text, double* out, uint32_t most) {
    uint32_t count = 0;
    size_t at = 0;
    while (text.data != nullptr) {
        // Numbers are separated by white space and at most one comma, which a number must follow.
        while (at < text.size && prAsciiIsSpace(text.data[at]))
            ++at;
        bool comma = count != 0 && at < text.size && text.data[at] == ',';
        if (comma)
            ++at;
        while (at < text.size && prAsciiIsSpace(text.data[at]))
            ++at;
        if (at == text.size)
            return comma ? 0 : count;
        double number = 0;
        size_t read = prF64ParsePrefix(PrStringView{text.data + at, text.size - at}, &number);
        // A <number> has a digit after its decimal point (css-syntax-3/#consume-number).
        if (read == 0 || text.data[at + read - 1] == '.' || !prF64IsFinite(number) || count == most)
            return 0;
        out[count++] = number;
        at += read;
    }
    return count;
}

// ---------------------------------------------------------------------------------------------------------------------
// Lengths and angles

// Returns the user units in one of `unit`.
float unitLength(PrCssUnit unit, PrSvgLengthBasis basis) {
    return float(svgLengthUnitScale(unit, basis));
}

void appendLength(PrTextBuffer* out, PrSvgLength length) {
    appendFloat(out, double(length.value));
    prTextBufferAppend(out, svgUnitName(svgIsLengthUnit(length.unit) ? length.unit : PrCssUnit(PrCssUnit_Number)));
}

// Returns true if `value` is in the range of the attribute's values that are not in error. An integer is truncated
// toward zero first.
bool inRange(const SvgTypedAttribute& typed, double value) {
    if (typed.type == PrSvgValueType_Integer || typed.type == PrSvgValueType_IntegerPair)
        value = prF64Trunc(value);
    switch (typed.range) {
        case SvgRange_NonNegative: return value >= 0;
        case SvgRange_Positive: return value > 0;
        default: return true;
    }
}

// Reads a length of an attribute: one that parses and is in the attribute's range.
bool lengthOf(PrStringView text, const SvgTypedAttribute& typed, PrSvgLength* out) {
    return text.data != nullptr && prSvgLengthParse(text, out) && inRange(typed, double(out->value));
}

// ---------------------------------------------------------------------------------------------------------------------
// Transforms

// The number of arguments that each function writes, by PrSvgTransformType.
constexpr uint32_t kTransformWritten[] = {0, 6, 2, 2, 3, 1, 1};

// Parses one transform function at `*at`, after white space and a comma, and advances past it. Returns false if there
// is none there, or it is in error.
bool readTransform(PrStringView text, size_t* at, PrSvgTransform* out) {
    size_t open = *at;
    while (open < text.size && text.data[open] != '(')
        ++open;
    size_t close = open;
    while (close < text.size && text.data[close] != ')')
        ++close;
    if (close == text.size)
        return false;
    PrStringView name = prStringTrim(PrStringView{text.data + *at, open - *at});
    uint8_t type = svgKeywordValue(SvgKeywords_TransformType, name);
    double numbers[6] = {};
    uint32_t count = type != 0 ? numbersOf(PrStringView{text.data + open + 1, close - open - 1}, numbers, 6) : 0;
    bool ok = false;
    switch (type) {
        case PrSvgTransformType_Matrix: ok = count == 6; break;
        case PrSvgTransformType_Translate:
        case PrSvgTransformType_Scale: ok = count == 1 || count == 2; break;
        case PrSvgTransformType_Rotate: ok = count == 1 || count == 3; break;
        case PrSvgTransformType_SkewX:
        case PrSvgTransformType_SkewY: ok = count == 1; break;
        default: break;
    }
    if (!ok)
        return false;
    *out = PrSvgTransform{.type = type};
    for (uint32_t i = 0; i < count; ++i)
        out->values[i] = float(numbers[i]);
    // A scale of one number scales both axes.
    if (type == PrSvgTransformType_Scale && count == 1)
        out->values[1] = out->values[0];
    *at = close + 1;
    return true;
}

bool validTransform(const PrSvgTransform& transform) {
    return transform.type >= PrSvgTransformType_Matrix && transform.type <= PrSvgTransformType_SkewY;
}

// ---------------------------------------------------------------------------------------------------------------------
// preserveAspectRatio

// ---------------------------------------------------------------------------------------------------------------------
// Lists

// Calls `item` with each item of a list of `type`, an item being a number, a length, or a point's two numbers. Returns
// false if the text does not parse, or if `item` returns false. Items are separated by white space and at most one
// comma, and a comma cannot begin or end the list. Transforms may be separated by several commas
// (SVG2/types.html#Microsyntaxes).
template <typename F>
bool eachItem(PrStringView text, PrSvgValueType type, F&& item) {
    if (text.data == nullptr)
        return true;
    size_t at = 0;
    auto space = [&] {
        while (at < text.size && prAsciiIsSpace(text.data[at]))
            ++at;
    };
    // Skips a separator. Returns false if the separator is a comma at the end of the text.
    auto skip = [&] {
        space();
        bool comma = false;
        while (at < text.size && text.data[at] == ',' && (!comma || type == PrSvgValueType_TransformList)) {
            comma = true;
            ++at;
            space();
        }
        return !(comma && at == text.size);
    };
    space();
    while (at < text.size) {
        size_t start = at;
        if (type == PrSvgValueType_TransformList) {
            PrSvgTransform transform{};
            if (!readTransform(text, &at, &transform) || !item(&transform))
                return false;
        } else {
            double number = 0;
            size_t read = prF64ParsePrefix(PrStringView{text.data + at, text.size - at}, &number);
            if (read == 0 || !prF64IsFinite(number))
                return false;
            at += read;
            if (type == PrSvgValueType_LengthList) {
                while (at < text.size && (text.data[at] == '%' || prAsciiIsLetter(text.data[at])))
                    ++at;
                PrSvgLength length{};
                if (!prSvgLengthParse(PrStringView{text.data + start, at - start}, &length) || !item(&length))
                    return false;
            } else if (type == PrSvgValueType_PointList) {
                // A last coordinate without its pair ends the list, which keeps the points before it.
                bool separated = skip();
                if (at == text.size)
                    return true;
                if (!separated)
                    return false;
                double y = 0;
                read = prF64ParsePrefix(PrStringView{text.data + at, text.size - at}, &y);
                if (read == 0 || !prF64IsFinite(y))
                    return false;
                at += read;
                PrDomPoint point{number, y, 0, 1};
                if (!item(&point))
                    return false;
            } else {
                float value = float(number);
                if (!item(&value))
                    return false;
            }
        }
        if (!skip())
            return false;
    }
    return true;
}

// The item of a list that the animations compose, from its components at `at` and its unit.
void composedItem(const double* at, PrCssUnit, float* out) {
    *out = float(at[0]);
}

void composedItem(const double* at, PrCssUnit unit, PrSvgLength* out) {
    *out = PrSvgLength{.value = float(at[0]), .unit = unit};
}

void composedItem(const double* at, PrCssUnit, PrDomPoint* out) {
    *out = PrDomPoint{at[0], at[1], 0, 1};
}

void composedItem(const double*, PrCssUnit, PrSvgTransform*) {}

// Writes the items of a list that the animations compose, as many as fit, and the number of its items to `count`.
// Returns false if they compose none of `type` that the read takes.
template <typename T>
bool composedList(const PrSvgElement* element, PrSvgAttribute attribute, PrSvgLayer layer, PrSvgValueType type,
        T* PR_NULLABLE items, uint32_t capacity, uint32_t* count) {
    const SvgAnimateValue* value = nullptr;
    if (type != PrSvgValueType_TransformList)
        value = composedOf(element, attribute, layer, type);
    if (value == nullptr)
        return false;
    uint32_t step = type == PrSvgValueType_PointList ? 2 : 1;
    *count = value->numbers.count / step;
    for (uint32_t i = 0; items != nullptr && i < *count && i < capacity; ++i)
        composedItem(value->numbers.data + i * step, value->units.data[i * step], &items[i]);
    return true;
}

// Writes the items of a list of `type` to `items`, as many as fit, and returns how many the list has. A list that does
// not parse is empty. A list of points instead keeps the points before its error, and drops a last coordinate without
// its pair (SVG2/shapes.html#DataTypePoints).
template <typename T>
uint32_t listOf(const PrSvgElement* element, PrSvgAttribute attribute, PrSvgLayer layer, PrSvgValueType type,
        T* PR_NULLABLE items, uint32_t capacity) {
    SvgTypedAttribute typed{};
    if (!typedAs(element, attribute, type, &typed))
        return 0;
    uint32_t count = 0;
    if (composedList<T>(element, attribute, layer, type, items, capacity, &count))
        return count;
    bool ok = eachItem(textOf(element, attribute, layer), type, [&](const void* item) {
        if (items != nullptr && count < capacity)
            items[count] = *static_cast<const T*>(item);
        ++count;
        return true;
    });
    return ok || type == PrSvgValueType_PointList ? count : 0;
}

// Writes `count` items to a list's attribute, each written by `append` and separated by `separator`.
template <typename T, typename F>
PrStatus writeList(PrSvgElement* element, PrSvgAttribute attribute, PrSvgValueType type, const T* PR_NULLABLE items,
        uint32_t count, PrStringView separator, F&& append) {
    SvgTypedAttribute typed{};
    if (!typedAs(element, attribute, type, &typed))
        return PrStatus_WrongType;
    PrTextBuffer text{};
    for (uint32_t i = 0; i < count; ++i) {
        if (i != 0)
            prTextBufferAppend(&text, separator);
        if (!append(&text, items[i])) {
            prTextBufferReset(&text);
            return PrStatus_InvalidData;
        }
    }
    writeBuffer(element, attribute, &text);
    return PrStatus_Ok;
}

// Calls `item` with each string of a requiredExtensions list, separated by white space, or of a systemLanguage list. A
// systemLanguage list is a set of comma-separated tokens
// (html/common-microsyntaxes.html#set-of-comma-separated-tokens), which keeps an empty token: an empty attribute holds
// one, and two commas hold one between them.
template <typename F>
void eachString(PrStringView text, PrSvgAttribute attribute, F&& item) {
    if (text.data == nullptr)
        return;
    if (attribute == PrSvgAttribute_SystemLanguage) {
        size_t start = 0;
        for (size_t at = 0; at <= text.size; ++at) {
            if (at == text.size || text.data[at] == ',') {
                item(prStringTrim(PrStringView{text.data + start, at - start}));
                start = at + 1;
            }
        }
        return;
    }
    size_t at = 0;
    while (at < text.size) {
        while (at < text.size && prAsciiIsSpace(text.data[at]))
            ++at;
        size_t start = at;
        while (at < text.size && !prAsciiIsSpace(text.data[at]))
            ++at;
        if (at != start)
            item(PrStringView{text.data + start, at - start});
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Scalars

// Reads one or two numbers of an attribute, or its initial value, and the second is the first if it is missing. Returns
// false if the element does not read the attribute as `type`.
bool pairOf(const PrSvgElement* element, PrSvgAttribute attribute, PrSvgLayer layer, PrSvgValueType type, double out[2]) {
    SvgTypedAttribute typed{};
    if (!typedAs(element, attribute, type, &typed))
        return false;
    uint32_t most = type == PrSvgValueType_NumberPair || type == PrSvgValueType_IntegerPair ? 2 : 1;
    uint32_t count = 0;
    if (const SvgAnimateValue* value = composedOf(element, attribute, layer, type)) {
        count = value->numbers.count < most ? value->numbers.count : most;
        for (uint32_t i = 0; i < count; ++i)
            out[i] = value->numbers.data[i];
    } else {
        count = numbersOf(textOf(element, attribute, layer), out, most);
    }
    // A value in error reads as the initial value. If one number of a pair is in error, the whole pair does.
    for (uint32_t i = 0; i < count; ++i) {
        if (!inRange(typed, out[i]))
            count = 0;
    }
    if (count == 0)
        count = numbersOf(typed.initial, out, most);
    if (count == 0)
        out[0] = 0;
    if (count < 2)
        out[1] = out[0];
    return true;
}

// Writes one number, or two if `second` differs from `first`.
PrStatus writeNumbers(PrSvgElement* element, PrSvgAttribute attribute, PrSvgValueType type, double first, double second) {
    SvgTypedAttribute typed{};
    if (!typedAs(element, attribute, type, &typed))
        return PrStatus_WrongType;
    PrTextBuffer text{};
    appendFloat(&text, first);
    if (second != first) {
        prTextBufferAppendChar(&text, ' ');
        appendFloat(&text, second);
    }
    writeBuffer(element, attribute, &text);
    return PrStatus_Ok;
}

// Returns the value of an enumeration's keyword, or of its initial value if the text is no keyword's. A marker's orient
// that is not a keyword is an angle.
uint8_t enumerationOf(const SvgTypedAttribute& typed, PrStringView text) {
    uint8_t value = text.data != nullptr ? svgKeywordValue(typed.keywords, prStringTrim(text)) : 0;
    if (typed.keywords == SvgKeywords_Orient)
        return value != 0 ? value : uint8_t(PrSvgOrient_Angle);
    return value != 0 ? value : svgKeywordValue(typed.keywords, typed.initial);
}

} // namespace

// ---------------------------------------------------------------------------------------------------------------------
// Units

bool svgIsLengthUnit(PrCssUnit unit) {
    // A length takes any CSS length unit (SVG2/types.html#InterfaceSVGLength).
    return unit == PrCssUnit_Number || unit == PrCssUnit_Percent || cssUnitCategory(unit) == CssUnitCategory_Length;
}

bool svgIsAngleUnit(PrCssUnit unit) {
    return unit == PrCssUnit_Number || cssUnitCategory(unit) == CssUnitCategory_Angle;
}

PrCssUnit svgUnitOf(PrStringView text) {
    if (text.size == 0)
        return PrCssUnit_Number;
    return text == "%" ? PrCssUnit(PrCssUnit_Percent) : cssUnitFromText(text);
}

PrCssUnit svgLengthUnitOf(PrStringView text) {
    PrCssUnit unit = svgUnitOf(text);
    return svgIsLengthUnit(unit) ? unit : PrCssUnit(PrCssUnit_Unknown);
}

PrCssUnit svgAngleUnitOf(PrStringView text) {
    PrCssUnit unit = svgUnitOf(text);
    return svgIsAngleUnit(unit) ? unit : PrCssUnit(PrCssUnit_Unknown);
}

PrStringView svgUnitName(PrCssUnit unit) {
    if (unit == PrCssUnit_Number || unit == PrCssUnit_Unknown)
        return "";
    return unit == PrCssUnit_Percent ? PrStringView("%") : cssUnitName(unit);
}

double svgLengthUnitScale(PrCssUnit unit, PrSvgLengthBasis basis) {
    switch (unit) {
        case PrCssUnit_Number: return 1;
        case PrCssUnit_Percent: return double(basis.percent) / 100;
        case PrCssUnit_Em: return double(basis.fontSize);
        case PrCssUnit_Ex: return double(basis.fontSize) / 2;
        default: break;
    }
    if (!svgIsLengthUnit(unit))
        return 0;
    if (cssUnitScale(unit) != 0)
        return cssUnitScale(unit);
    // The element's style resolves the units that depend on font metrics or on the viewport.
    PrCssPresentation* presentation = basis.element != nullptr ? cssPresentationOf(basis.element->document) : nullptr;
    double px = 0;
    if (presentation == nullptr || !cssPresentationUnitPx(presentation, basis.element, unit, &px))
        return 0;
    return px;
}

double svgAngleUnitDegrees(PrCssUnit unit) {
    if (unit == PrCssUnit_Number)
        return 1;
    return svgIsAngleUnit(unit) ? cssUnitScale(unit) : 0;
}

// ---------------------------------------------------------------------------------------------------------------------
// Transform lists

bool svgParseTransformList(PrStringView text, RawArray<PrSvgTransform>* out) {
    return eachItem(text, PrSvgValueType_TransformList, [&](const void* transform) {
        out->push(*static_cast<const PrSvgTransform*>(transform));
        return true;
    });
}

void svgAppendTransform(PrTextBuffer* out, const PrSvgTransform& transform) {
    prTextBufferAppend(out, svgKeywordName(SvgKeywords_TransformType, transform.type));
    prTextBufferAppendChar(out, '(');
    for (uint32_t i = 0; i < kTransformWritten[transform.type]; ++i) {
        if (i != 0)
            prTextBufferAppendChar(out, ' ');
        appendFloat(out, double(transform.values[i]));
    }
    prTextBufferAppendChar(out, ')');
}

} // namespace pr_impl

using namespace pr_impl;

// ---------------------------------------------------------------------------------------------------------------------
// Values

PrSvgLengthBasis prSvgLengthBasisDefault(void) {
    return PrSvgLengthBasis{.percent = 100, .fontSize = float(kCssMediumFontSize), .element = nullptr};
}

float prSvgLengthResolve(PrSvgLength length, PrSvgLengthBasis basis) {
    return length.value * unitLength(length.unit, basis);
}

PrSvgLength prSvgLengthConvert(PrSvgLength length, PrCssUnit unit, PrSvgLengthBasis basis) {
    float scale = unitLength(unit, basis);
    return PrSvgLength{.value = scale != 0 ? prSvgLengthResolve(length, basis) / scale : 0, .unit = unit};
}

bool prSvgLengthParse(PrStringView text, PrSvgLength* out) {
    size_t at = 0;
    float number = 0;
    if (text.data == nullptr || !numberListRead(text, &at, &number))
        return false;
    PrCssUnit unit = svgLengthUnitOf(prStringTrimEnd(prStringFrom(text, at)));
    if (unit == PrCssUnit_Unknown)
        return false;
    *out = PrSvgLength{.value = number, .unit = unit};
    return true;
}

PrRawString prSvgLengthSerialize(PrSvgLength length) {
    PrTextBuffer text{};
    appendLength(&text, length);
    return taken(&text);
}

bool prSvgLengthIsCssFunction(PrStringView text) {
    text = prStringTrim(text);
    size_t at = 0;
    while (at < text.size && (prAsciiIsLetter(text.data[at]) || prAsciiIsDigit(text.data[at]) || text.data[at] == '-'))
        ++at;
    return at != 0 && !prAsciiIsDigit(text.data[0]) && at < text.size && text.data[at] == '(' && text.data[text.size - 1] == ')';
}

float prSvgAngleDegrees(PrSvgAngle angle) {
    return float(double(angle.value) * svgAngleUnitDegrees(angle.unit));
}

PrSvgAngle prSvgAngleConvert(PrSvgAngle angle, PrCssUnit unit) {
    if (!svgIsAngleUnit(unit))
        return angle;
    return PrSvgAngle{.value = float(double(prSvgAngleDegrees(angle)) / svgAngleUnitDegrees(unit)), .unit = unit};
}

bool prSvgAngleParse(PrStringView text, PrSvgAngle* out) {
    size_t at = 0;
    float number = 0;
    if (text.data == nullptr || !numberListRead(text, &at, &number))
        return false;
    PrCssUnit unit = svgAngleUnitOf(prStringTrimEnd(prStringFrom(text, at)));
    if (unit == PrCssUnit_Unknown)
        return false;
    *out = PrSvgAngle{.value = number, .unit = unit};
    return true;
}

PrRawString prSvgAngleSerialize(PrSvgAngle angle) {
    PrTextBuffer text{};
    appendFloat(&text, double(angle.value));
    prTextBufferAppend(&text, svgUnitName(svgIsAngleUnit(angle.unit) ? angle.unit : PrCssUnit(PrCssUnit_Number)));
    return taken(&text);
}

PrDomMatrix prSvgTransformMatrix(const PrSvgTransform* transform) {
    const float* a = transform->values;
    double radians = double(a[0]) * kPrF64Pi / 180;
    double c = prF64Cos(radians);
    double s = prF64Sin(radians);
    double m[6] = {1, 0, 0, 1, 0, 0};
    switch (transform->type) {
        case PrSvgTransformType_Matrix:
            for (uint32_t i = 0; i < 6; ++i)
                m[i] = double(a[i]);
            break;
        case PrSvgTransformType_Translate:
            m[4] = double(a[0]);
            m[5] = double(a[1]);
            break;
        case PrSvgTransformType_Scale:
            m[0] = double(a[0]);
            m[3] = double(a[1]);
            break;
        case PrSvgTransformType_Rotate: {
            // A rotation about (cx, cy) is a translation to it, the rotation, and a translation back.
            double cx = double(a[1]);
            double cy = double(a[2]);
            m[0] = c;
            m[1] = s;
            m[2] = -s;
            m[3] = c;
            m[4] = cx - c * cx + s * cy;
            m[5] = cy - s * cx - c * cy;
            break;
        }
        case PrSvgTransformType_SkewX: m[2] = prF64Tan(radians); break;
        case PrSvgTransformType_SkewY: m[1] = prF64Tan(radians); break;
        default: break;
    }
    PrDomMatrix out{};
    prDomMatrixFromValues(m, 6, &out);
    return out;
}

float prSvgTransformAngle(const PrSvgTransform* transform) {
    switch (transform->type) {
        case PrSvgTransformType_Rotate:
        case PrSvgTransformType_SkewX:
        case PrSvgTransformType_SkewY: return transform->values[0];
        default: return 0;
    }
}

PrSvgTransform prSvgTransformFromMatrix(PrDomMatrix matrix) {
    const double* m = matrix.m;
    return PrSvgTransform{
        .type = PrSvgTransformType_Matrix,
        .values = {float(m[0]), float(m[1]), float(m[4]), float(m[5]), float(m[12]), float(m[13])},
    };
}

// ---------------------------------------------------------------------------------------------------------------------
// The typed attributes of elements

PrSvgValueType prSvgElementAttributeType(const PrSvgElement* element, PrSvgAttribute attribute) {
    SvgTypedAttribute typed{};
    svgTypedAttribute(PrDomTag(element->tag), attribute, &typed);
    return typed.type;
}

PrSvgLengthBasis prSvgElementLengthBasis(const PrSvgElement* element, PrSvgAttribute attribute) {
    SvgTypedAttribute typed{};
    svgTypedAttribute(PrDomTag(element->tag), attribute, &typed);
    SvgLengthContext basis{};
    svgElementLengthBasis(element, &basis);
    PrVec2 v = basis.viewport;
    float percent = typed.axis == SvgAxis_X ? v.x : typed.axis == SvgAxis_Y ? v.y : prF32Sqrt((v.x * v.x + v.y * v.y) / 2);
    return PrSvgLengthBasis{.percent = percent, .fontSize = basis.fontSize, .element = element};
}

bool prSvgElementGetBoolean(const PrSvgElement* element, PrSvgAttribute attribute, PrSvgLayer layer, bool* out) {
    SvgTypedAttribute typed{};
    if (!typedAs(element, attribute, PrSvgValueType_Boolean, &typed))
        return false;
    PrStringView text = prStringTrim(textOf(element, attribute, layer));
    *out = text == "true" || (text != "false" && typed.initial == "true");
    return true;
}

bool prSvgElementGetEnumeration(const PrSvgElement* element, PrSvgAttribute attribute, PrSvgLayer layer, uint8_t* out) {
    SvgTypedAttribute typed{};
    if (!svgTypedAttribute(PrDomTag(element->tag), attribute, &typed) || typed.keywords == SvgKeywords_None)
        return false;
    *out = enumerationOf(typed, textOf(element, attribute, layer));
    return true;
}

bool prSvgElementGetInteger(const PrSvgElement* element, PrSvgAttribute attribute, PrSvgLayer layer, int32_t* out) {
    double numbers[2] = {};
    if (!pairOf(element, attribute, layer, PrSvgValueType_Integer, numbers))
        return false;
    *out = int32_t(numbers[0]);
    return true;
}

bool prSvgElementGetIntegerPair(const PrSvgElement* element, PrSvgAttribute attribute, PrSvgLayer layer,
        int32_t out[PR_NONNULL 2]) {
    double numbers[2] = {};
    if (!pairOf(element, attribute, layer, PrSvgValueType_IntegerPair, numbers))
        return false;
    out[0] = int32_t(numbers[0]);
    out[1] = int32_t(numbers[1]);
    return true;
}

bool prSvgElementGetNumber(const PrSvgElement* element, PrSvgAttribute attribute, PrSvgLayer layer, float* out) {
    double numbers[2] = {};
    if (!pairOf(element, attribute, layer, PrSvgValueType_Number, numbers))
        return false;
    *out = float(numbers[0]);
    return true;
}

bool prSvgElementGetNumberPair(const PrSvgElement* element, PrSvgAttribute attribute, PrSvgLayer layer,
        float out[PR_NONNULL 2]) {
    double numbers[2] = {};
    if (!pairOf(element, attribute, layer, PrSvgValueType_NumberPair, numbers))
        return false;
    out[0] = float(numbers[0]);
    out[1] = float(numbers[1]);
    return true;
}

PrStatus prSvgElementSetBoolean(PrSvgElement* element, PrSvgAttribute attribute, bool value) {
    SvgTypedAttribute typed{};
    if (!typedAs(element, attribute, PrSvgValueType_Boolean, &typed))
        return PrStatus_WrongType;
    writeText(element, attribute, value ? "true" : "false");
    return PrStatus_Ok;
}

PrStatus prSvgElementSetInteger(PrSvgElement* element, PrSvgAttribute attribute, int32_t value) {
    return writeNumbers(element, attribute, PrSvgValueType_Integer, double(value), double(value));
}

PrStatus prSvgElementSetIntegerPair(PrSvgElement* element, PrSvgAttribute attribute, int32_t first, int32_t second) {
    return writeNumbers(element, attribute, PrSvgValueType_IntegerPair, double(first), double(second));
}

PrStatus prSvgElementSetNumber(PrSvgElement* element, PrSvgAttribute attribute, float value) {
    return writeNumbers(element, attribute, PrSvgValueType_Number, double(value), double(value));
}

PrStatus prSvgElementSetNumberPair(PrSvgElement* element, PrSvgAttribute attribute, float first, float second) {
    return writeNumbers(element, attribute, PrSvgValueType_NumberPair, double(first), double(second));
}

PrStatus prSvgElementSetEnumeration(PrSvgElement* element, PrSvgAttribute attribute, uint8_t value) {
    SvgTypedAttribute typed{};
    if (!svgTypedAttribute(PrDomTag(element->tag), attribute, &typed) || typed.keywords == SvgKeywords_None)
        return PrStatus_WrongType;
    if (typed.keywords == SvgKeywords_Orient && value == PrSvgOrient_Angle) {
        // An angle keeps the angle that the attribute names, or is 0.
        if (enumerationOf(typed, textOf(element, attribute, PrSvgLayer_Base)) != PrSvgOrient_Angle)
            writeText(element, attribute, "0");
        return PrStatus_Ok;
    }
    PrStringView keyword = svgKeywordName(typed.keywords, value);
    if (keyword.size == 0)
        return PrStatus_InvalidData;
    writeText(element, attribute, keyword);
    return PrStatus_Ok;
}

PrStringView prSvgElementGetString(const PrSvgElement* element, PrSvgAttribute attribute, PrSvgLayer layer) {
    SvgTypedAttribute typed{};
    if (!typedAs(element, attribute, PrSvgValueType_String, &typed))
        return PrStringView{nullptr, 0};
    // A string reflects the attribute's text exactly, with the whitespace around it that parsed values ignore.
    PrStringView name = prSvgAttributeName(attribute);
    PrStringView text{nullptr, 0};
    if (attribute == PrSvgAttribute_Href)
        text = textOf(element, attribute, layer);
    else if (layer == PrSvgLayer_Animated)
        text = svgAnimatedAttribute(element, name);
    if (text.data == nullptr && attribute != PrSvgAttribute_Href)
        text = domAttributeValue(element, name);
    return text.data != nullptr ? text : PrStringView("");
}

PrStatus prSvgElementSetString(PrSvgElement* element, PrSvgAttribute attribute, PrStringView value) {
    SvgTypedAttribute typed{};
    if (!typedAs(element, attribute, PrSvgValueType_String, &typed))
        return PrStatus_WrongType;
    writeText(element, attribute, value);
    return PrStatus_Ok;
}

bool prSvgElementGetLength(const PrSvgElement* element, PrSvgAttribute attribute, PrSvgLayer layer, PrSvgLength* out) {
    SvgTypedAttribute typed{};
    if (!typedAs(element, attribute, PrSvgValueType_Length, &typed))
        return false;
    if (const SvgAnimateValue* value = composedOf(element, attribute, layer, PrSvgValueType_Length)) {
        *out = PrSvgLength{.value = float(value->numbers.data[0]), .unit = value->units.data[0]};
        if (inRange(typed, double(out->value)))
            return true;
    }
    PrStringView text = textOf(element, attribute, layer);
    if (text.data != nullptr && prSvgLengthIsCssFunction(text))
        *out = PrSvgLength{.value = 0, .unit = PrCssUnit_Unknown};
    else if (!lengthOf(text, typed, out) && !lengthOf(typed.initial, typed, out))
        *out = PrSvgLength{.value = 0, .unit = PrCssUnit_Number};
    return true;
}

PrStatus prSvgElementSetLength(PrSvgElement* element, PrSvgAttribute attribute, PrSvgLength value) {
    SvgTypedAttribute typed{};
    if (!typedAs(element, attribute, PrSvgValueType_Length, &typed))
        return PrStatus_WrongType;
    if (!svgIsLengthUnit(value.unit) || !prF32IsFinite(value.value))
        return PrStatus_InvalidData;
    PrTextBuffer text{};
    appendLength(&text, value);
    writeBuffer(element, attribute, &text);
    return PrStatus_Ok;
}

PrRawString prSvgElementGetLengthText(const PrSvgElement* element, PrSvgAttribute attribute, PrSvgLayer layer) {
    PrSvgLength length{};
    if (!prSvgElementGetLength(element, attribute, layer, &length))
        return PrRawString{};
    if (length.unit == PrCssUnit_Unknown)
        return prRawStringCopy(prStringTrim(textOf(element, attribute, layer)));
    return prSvgLengthSerialize(length);
}

PrStatus prSvgElementSetLengthText(PrSvgElement* element, PrSvgAttribute attribute, PrStringView text) {
    SvgTypedAttribute typed{};
    if (!typedAs(element, attribute, PrSvgValueType_Length, &typed))
        return PrStatus_WrongType;
    PrSvgLength length{};
    if (prSvgLengthParse(text, &length))
        return prSvgElementSetLength(element, attribute, length);
    if (!prSvgLengthIsCssFunction(text))
        return PrStatus_Syntax;
    writeText(element, attribute, prStringTrim(text));
    return PrStatus_Ok;
}

bool prSvgElementGetAngle(const PrSvgElement* element, PrSvgAttribute attribute, PrSvgLayer layer, PrSvgAngle* out) {
    SvgTypedAttribute typed{};
    if (!typedAs(element, attribute, PrSvgValueType_Orient, &typed))
        return false;
    PrStringView text = textOf(element, attribute, layer);
    if (text.data == nullptr || !prSvgAngleParse(text, out))
        *out = PrSvgAngle{.value = 0, .unit = PrCssUnit_Number};
    return true;
}

PrStatus prSvgElementSetAngle(PrSvgElement* element, PrSvgAttribute attribute, PrSvgAngle value) {
    SvgTypedAttribute typed{};
    if (!typedAs(element, attribute, PrSvgValueType_Orient, &typed))
        return PrStatus_WrongType;
    if (!svgIsAngleUnit(value.unit) || !prF32IsFinite(value.value))
        return PrStatus_InvalidData;
    PrRawString text = prSvgAngleSerialize(value);
    writeText(element, attribute, prRawStringView(&text));
    prRawStringReset(&text);
    return PrStatus_Ok;
}

bool prSvgElementGetRect(const PrSvgElement* element, PrSvgAttribute attribute, PrSvgLayer layer, PrDomRect* out) {
    SvgTypedAttribute typed{};
    if (!typedAs(element, attribute, PrSvgValueType_Rect, &typed))
        return false;
    double numbers[4] = {};
    const SvgAnimateValue* value = composedOf(element, attribute, layer, PrSvgValueType_Rect);
    if (value != nullptr && value->numbers.count == 4) {
        for (uint32_t i = 0; i < 4; ++i)
            numbers[i] = value->numbers.data[i];
    } else if (numbersOf(textOf(element, attribute, layer), numbers, 4) != 4) {
        return false;
    }
    *out = PrDomRect{numbers[0], numbers[1], numbers[2], numbers[3]};
    return true;
}

PrStatus prSvgElementSetRect(PrSvgElement* element, PrSvgAttribute attribute, PrDomRect value) {
    SvgTypedAttribute typed{};
    if (!typedAs(element, attribute, PrSvgValueType_Rect, &typed))
        return PrStatus_WrongType;
    PrTextBuffer text{};
    double numbers[4] = {value.x, value.y, value.width, value.height};
    for (uint32_t i = 0; i < 4; ++i) {
        if (i != 0)
            prTextBufferAppendChar(&text, ' ');
        appendFloat(&text, numbers[i]);
    }
    writeBuffer(element, attribute, &text);
    return PrStatus_Ok;
}

bool prSvgElementGetPreserveAspectRatio(const PrSvgElement* element, PrSvgAttribute attribute, PrSvgLayer layer,
        PrSvgPreserveAspectRatio* out) {
    SvgTypedAttribute typed{};
    if (!typedAs(element, attribute, PrSvgValueType_PreserveAspectRatio, &typed))
        return false;
    if (!svgParseAspect(textOf(element, attribute, layer), out))
        svgParseAspect(typed.initial, out);
    return true;
}

PrStatus prSvgElementSetPreserveAspectRatio(PrSvgElement* element, PrSvgAttribute attribute, PrSvgPreserveAspectRatio value) {
    SvgTypedAttribute typed{};
    if (!typedAs(element, attribute, PrSvgValueType_PreserveAspectRatio, &typed))
        return PrStatus_WrongType;
    PrStringView align = svgKeywordName(SvgKeywords_Align, value.align);
    PrStringView meetOrSlice = svgKeywordName(SvgKeywords_MeetOrSlice, value.meetOrSlice);
    if (align.size == 0 || meetOrSlice.size == 0)
        return PrStatus_InvalidData;
    PrTextBuffer text{};
    prTextBufferAppend(&text, align);
    if (value.meetOrSlice == PrSvgMeetOrSlice_Slice)
        prTextBufferAppend(&text, " slice");
    writeBuffer(element, attribute, &text);
    return PrStatus_Ok;
}

uint32_t prSvgElementGetNumberList(const PrSvgElement* element, PrSvgAttribute attribute, PrSvgLayer layer,
        float* PR_NULLABLE items, uint32_t capacity) {
    return listOf(element, attribute, layer, PrSvgValueType_NumberList, items, capacity);
}

uint32_t prSvgElementGetLengthList(const PrSvgElement* element, PrSvgAttribute attribute, PrSvgLayer layer,
        PrSvgLength* PR_NULLABLE items, uint32_t capacity) {
    return listOf(element, attribute, layer, PrSvgValueType_LengthList, items, capacity);
}

uint32_t prSvgElementGetTransformList(const PrSvgElement* element, PrSvgAttribute attribute, PrSvgLayer layer,
        PrSvgTransform* PR_NULLABLE items, uint32_t capacity) {
    return listOf(element, attribute, layer, PrSvgValueType_TransformList, items, capacity);
}

uint32_t prSvgElementGetPointList(const PrSvgElement* element, PrSvgAttribute attribute, PrSvgLayer layer,
        PrDomPoint* PR_NULLABLE items, uint32_t capacity) {
    return listOf(element, attribute, layer, PrSvgValueType_PointList, items, capacity);
}

uint32_t prSvgElementGetStringList(const PrSvgElement* element, PrSvgAttribute attribute,
        PrStringView* PR_NULLABLE items, uint32_t capacity) {
    SvgTypedAttribute typed{};
    if (!typedAs(element, attribute, PrSvgValueType_StringList, &typed))
        return 0;
    uint32_t count = 0;
    eachString(textOf(element, attribute, PrSvgLayer_Base), attribute, [&](PrStringView item) {
        if (items != nullptr && count < capacity)
            items[count] = item;
        ++count;
    });
    return count;
}

PrStatus prSvgElementSetNumberList(PrSvgElement* element, PrSvgAttribute attribute, const float* PR_NULLABLE items,
        uint32_t count) {
    return writeList(element, attribute, PrSvgValueType_NumberList, items, count, " ", [](PrTextBuffer* text, float v) {
        appendFloat(text, double(v));
        return prF32IsFinite(v);
    });
}

PrStatus prSvgElementSetLengthList(PrSvgElement* element, PrSvgAttribute attribute,
        const PrSvgLength* PR_NULLABLE items, uint32_t count) {
    return writeList(element, attribute, PrSvgValueType_LengthList, items, count, " ",
        [](PrTextBuffer* text, PrSvgLength v) {
            appendLength(text, v);
            return svgIsLengthUnit(v.unit) && prF32IsFinite(v.value);
        });
}

PrStatus prSvgElementSetTransformList(PrSvgElement* element, PrSvgAttribute attribute,
        const PrSvgTransform* PR_NULLABLE items, uint32_t count) {
    return writeList(element, attribute, PrSvgValueType_TransformList, items, count, " ",
        [](PrTextBuffer* text, const PrSvgTransform& v) {
            if (!validTransform(v))
                return false;
            svgAppendTransform(text, v);
            return true;
        });
}

PrStatus prSvgElementSetPointList(PrSvgElement* element, PrSvgAttribute attribute, const PrDomPoint* PR_NULLABLE items,
        uint32_t count) {
    return writeList(element, attribute, PrSvgValueType_PointList, items, count, " ",
        [](PrTextBuffer* text, const PrDomPoint& v) {
            cssSerializeNumber(text, v.x);
            prTextBufferAppendChar(text, ',');
            cssSerializeNumber(text, v.y);
            return true;
        });
}

PrStatus prSvgElementSetStringList(PrSvgElement* element, PrSvgAttribute attribute,
        const PrStringView* PR_NULLABLE items, uint32_t count) {
    PrStringView separator = " ";
    if (attribute == PrSvgAttribute_SystemLanguage)
        separator = ", ";
    return writeList(element, attribute, PrSvgValueType_StringList, items, count, separator,
        [](PrTextBuffer* text, PrStringView v) {
            prTextBufferAppend(text, v);
            return true;
        });
}
