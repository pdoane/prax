// Presentation attributes: the attributes of SVG elements that map to CSS properties
//
// SVG2 - https://www.w3.org/TR/SVG2/styling.html#PresentationAttributes

#include "svg/src/svg_hints.h"

#include "pr/ascii.h"
#include "pr/bitset.h"
#include "pr/float_convert.h"
#include "pr/string_ops.h"
#include "pr/string_trim.h"
#include "pr/text_buffer.h"

#include "css/src/css_property.h"
#include "css/src/css_serialize.h"
#include "dom/src/dom_flat.h"
#include "dom/src/dom_node.h"

namespace pr_impl {
namespace {

// Returns true if `text` is a number alone, with no unit.
bool isBareNumber(PrStringView text) {
    for (size_t i = 0; i < text.size; ++i) {
        char c = text.data[i];
        if (!prAsciiIsDigit(c) && c != '.' && c != '+' && c != '-' && c != 'e' && c != 'E')
            return false;
    }
    return text.size != 0;
}

// Returns true if the SVG element is one of `tags`.
template <size_t N>
bool isSvgTag(const PrDomElement* element, const PrDomTag (&tags)[N]) {
    for (PrDomTag tag : tags) {
        if (element->tag == tag)
            return true;
    }
    return false;
}

// Returns the property that the attribute `name` of an SVG element is a presentation attribute of, or
// PrCssProperty_Count if it is none (SVG2/styling.html#PresentationAttributes). Every element has those of
// kCssPropertyPresentation. The geometry properties have presentation attributes only on the elements that they size
// (SVG2/geometry.html). The width and height of an inner svg element are not, since CSS does not lay it out
// (w3c/svgwg#1057). The transform property is spelled patternTransform on a pattern and gradientTransform on a
// gradient, and the path-length property is spelled pathLength on a path and the basic shapes
// (SVG2/paths.html#PathLengthProperty). The fill of an animation element is its own attribute.
PrCssProperty svgPresentationOf(const PrDomElement* element, PrStringView name) {
    static constexpr PrDomTag kCentered[] = {PrDomTag_Circle, PrDomTag_Ellipse};
    static constexpr PrDomTag kRounded[] = {PrDomTag_Ellipse, PrDomTag_Rect};
    static constexpr PrDomTag kPlaced[] = {PrDomTag_ForeignObject, PrDomTag_Image, PrDomTag_Rect};
    static constexpr PrDomTag kPositioned[] = {
        PrDomTag_ForeignObject, PrDomTag_Image, PrDomTag_Rect, PrDomTag_Svg,
        PrDomTag_Symbol,
    };
    static constexpr PrDomTag kGradients[] = {PrDomTag_LinearGradient, PrDomTag_RadialGradient};
    static constexpr PrDomTag kAnimations[] = {
        PrDomTag_Animate, PrDomTag_AnimateMotion, PrDomTag_AnimateTransform,
        PrDomTag_Set,
    };
    static constexpr PrDomTag kShapes[] = {
        PrDomTag_Circle, PrDomTag_Ellipse, PrDomTag_Line, PrDomTag_Path,
        PrDomTag_Polygon, PrDomTag_Polyline, PrDomTag_Rect,
    };
    if (name == "transform" && element->tag != PrDomTag_Pattern && !isSvgTag(element, kGradients))
        return PrCssProperty_Transform;
    if (name == "transform")
        return PrCssProperty_Count;
    if (name == "patternTransform")
        return element->tag == PrDomTag_Pattern ? PrCssProperty_Transform : PrCssProperty_Count;
    if (name == "gradientTransform")
        return isSvgTag(element, kGradients) ? PrCssProperty_Transform : PrCssProperty_Count;
    if (name == "pathLength")
        return isSvgTag(element, kShapes) ? PrCssProperty_PathLength : PrCssProperty_Count;
    PrCssProperty property = cssPropertyFromText(name);
    // Attribute names match case-sensitively.
    if (property == PrCssProperty_Count || cssPropertyName(property) != name)
        return PrCssProperty_Count;
    switch (property) {
        case PrCssProperty_Cx:
        case PrCssProperty_Cy:
            return isSvgTag(element, kCentered) ? property : PrCssProperty_Count;
        case PrCssProperty_R:
            return element->tag == PrDomTag_Circle ? property : PrCssProperty_Count;
        case PrCssProperty_Rx:
        case PrCssProperty_Ry:
            return isSvgTag(element, kRounded) ? property : PrCssProperty_Count;
        case PrCssProperty_X:
        case PrCssProperty_Y:
            return isSvgTag(element, kPositioned) ? property : PrCssProperty_Count;
        case PrCssProperty_Width:
        case PrCssProperty_Height:
            return isSvgTag(element, kPlaced) || domIsOuterSvg(element) ? property : PrCssProperty_Count;
        case PrCssProperty_D:
            return element->tag == PrDomTag_Path ? property : PrCssProperty_Count;
        case PrCssProperty_Fill:
            return isSvgTag(element, kAnimations) ? PrCssProperty_Count : property;
        default:
            break;
    }
    return bitsetTest(kCssPropertyPresentation, property) ? property : PrCssProperty_Count;
}

// Returns true if `text` holds one declaration's value and nothing after it: no semicolon, `!` or brace outside a
// string, a comment or a block. The hints are parsed as one declaration list, so such a character would end the
// declaration, mark it important or start another one. As the value of a property, the text is invalid
// (SVG2/styling.html#PresentationAttributes), and it declares nothing.
bool isOneValue(PrStringView text) {
    uint32_t depth = 0;
    for (size_t i = 0; i < text.size; ++i) {
        char c = text.data[i];
        if (c == '\\') {
            ++i;
        } else if (c == '"' || c == '\'') {
            for (++i; i < text.size && text.data[i] != c; ++i)
                i += text.data[i] == '\\' ? 1 : 0;
            if (i >= text.size)
                return false;
        } else if (c == '/' && i + 1 < text.size && text.data[i + 1] == '*') {
            size_t end = i + 2;
            while (end + 1 < text.size && !(text.data[end] == '*' && text.data[end + 1] == '/'))
                ++end;
            if (end + 1 >= text.size)
                return false;
            i = end + 1;
        } else if (c == '(' || c == '[') {
            ++depth;
        } else if (c == ')' || c == ']') {
            if (depth == 0)
                return false;
            --depth;
        } else if (depth == 0 && (c == ';' || c == '!' || c == '{' || c == '}')) {
            return false;
        }
    }
    return depth == 0;
}

// Reads an SVG transform function's numbers (css-transforms-1/#svg-syntax), each after an optional comma except the
// first, up to the closing parenthesis. Returns the count, or -1 if the arguments do not parse or number more than six.
int32_t svgTransformArguments(PrStringView text, size_t* pos, double* out) {
    int32_t count = 0;
    for (;;) {
        while (*pos < text.size && prAsciiIsSpace(text.data[*pos]))
            ++*pos;
        if (*pos < text.size && text.data[*pos] == ')') {
            ++*pos;
            return count;
        }
        if (count != 0 && *pos < text.size && text.data[*pos] == ',') {
            ++*pos;
            while (*pos < text.size && prAsciiIsSpace(text.data[*pos]))
                ++*pos;
        }
        size_t start = *pos;
        while (*pos < text.size && (prAsciiIsDigit(text.data[*pos]) || text.data[*pos] == '.' || text.data[*pos] == '-' ||
                text.data[*pos] == '+' || text.data[*pos] == 'e' || text.data[*pos] == 'E'))
            ++*pos;
        PrStringView number = {text.data + start, *pos - start};
        if (count == 6 || *pos == start || !prF64ParsePrefix(number, &out[count]))
            return -1;
        ++count;
    }
}

// Appends the transform list of an SVG transform attribute as a CSS transform list (css-transforms-1/#svg-syntax). SVG
// writes lengths and angles as numbers alone, separates functions and arguments with commas, and gives rotate() an
// optional center, which becomes a translation to the center and back. Returns false, appending nothing that the caller
// keeps, if the text is not in SVG's syntax.
bool svgTransform(PrTextBuffer* out, PrStringView text) {
    struct Function {
        PrStringView name;
        int32_t counts[2];      // the argument counts that the function takes
    };
    static constexpr Function kFunctions[] = {
        {"matrix", {6, 6}}, {"translate", {1, 2}}, {"scale", {1, 2}}, {"rotate", {1, 3}},
        {"skewX", {1, 1}}, {"skewY", {1, 1}},
    };
    size_t pos = 0;
    bool any = false;
    for (;;) {
        while (pos < text.size && (prAsciiIsSpace(text.data[pos]) || text.data[pos] == ','))
            ++pos;
        if (pos == text.size)
            return any;
        size_t start = pos;
        while (pos < text.size && prAsciiIsLetter(text.data[pos]))
            ++pos;
        PrStringView name{text.data + start, pos - start};
        while (pos < text.size && prAsciiIsSpace(text.data[pos]))
            ++pos;
        if (pos == text.size || text.data[pos] != '(')
            return false;
        ++pos;
        double n[6];
        int32_t count = svgTransformArguments(text, &pos, n);
        const Function* function = nullptr;
        for (const Function& f : kFunctions)
            function = f.name == name ? &f : function;
        if (function == nullptr || (count != function->counts[0] && count != function->counts[1]))
            return false;
        if (any)
            prTextBufferAppendChar(out, ' ');
        any = true;
        auto number = [&](double value, const char* unit) {
            cssSerializeNumber(out, value);
            prTextBufferAppend(out, unit);
        };
        if (name == "rotate" && count == 3) {
            prTextBufferAppend(out, "translate(");
            number(n[1], "px,");
            number(n[2], "px) rotate(");
            number(n[0], "deg) translate(");
            number(-n[1], "px,");
            number(-n[2], "px)");
            continue;
        }
        bool angles = name == "rotate" || name == "skewX" || name == "skewY";
        const char* unit = name == "translate" ? "px" : angles ? "deg" : "";
        prTextBufferAppend(out, name);
        prTextBufferAppendChar(out, '(');
        for (int32_t i = 0; i < count; ++i) {
            if (i != 0)
                prTextBufferAppendChar(out, ',');
            // A matrix's translation is a number in CSS as in SVG.
            number(n[i], unit);
        }
        prTextBufferAppendChar(out, ')');
    }
}

// Appends a filter declaration for a filter attribute's `value` if the value has bare numbers among the arguments of
// blur(), drop-shadow() and hue-rotate() (SVG2/types.html#presentation-attribute-css-value). The declaration writes
// each such number as an angle in degrees in hue-rotate(), and as a length in pixels otherwise. The numbers of a color
// function inside one of these functions are left as they are. A value without such numbers appends nothing.
void addSvgFilter(PrTextBuffer* out, PrStringView value) {
    PrTextBuffer rewritten{};
    PrCssKeyword functions[8];
    uint32_t depth = 0;
    bool changed = false;
    for (size_t i = 0; i < value.size;) {
        char c = value.data[i];
        if (c == '#') {
            // A hex color, whose digits are not a number.
            size_t end = i + 1;
            while (end < value.size && (prAsciiIsLetter(value.data[end]) || prAsciiIsDigit(value.data[end])))
                ++end;
            prTextBufferAppend(&rewritten, PrStringView{value.data + i, end - i});
            i = end;
            continue;
        }
        if (prAsciiIsLetter(c)) {
            size_t end = i;
            while (end < value.size &&
                    (prAsciiIsLetter(value.data[end]) || prAsciiIsDigit(value.data[end]) || value.data[end] == '-'))
                ++end;
            PrStringView name{value.data + i, end - i};
            prTextBufferAppend(&rewritten, name);
            if (end < value.size && value.data[end] == '(') {
                PrCssKeyword keyword = PrCssKeyword_Count;
                if (prAsciiEqualsFold(name, "blur"))
                    keyword = PrCssKeyword_Blur;
                else if (prAsciiEqualsFold(name, "drop-shadow"))
                    keyword = PrCssKeyword_DropShadow;
                else if (prAsciiEqualsFold(name, "hue-rotate"))
                    keyword = PrCssKeyword_HueRotate;
                if (depth < 8)
                    functions[depth] = keyword;
                ++depth;
                prTextBufferAppendChar(&rewritten, '(');
                end += 1;
            }
            i = end;
            continue;
        }
        bool sign =
            (c == '+' || c == '-') && i + 1 < value.size && (prAsciiIsDigit(value.data[i + 1]) || value.data[i + 1] == '.');
        if (prAsciiIsDigit(c) || c == '.' || sign) {
            size_t end = i + (sign ? 1 : 0);
            while (end < value.size && (prAsciiIsDigit(value.data[end]) || value.data[end] == '.'))
                ++end;
            prTextBufferAppend(&rewritten, PrStringView{value.data + i, end - i});
            bool unit = end < value.size && (prAsciiIsLetter(value.data[end]) || value.data[end] == '%');
            PrCssKeyword inside = depth != 0 && depth <= 8 ? functions[depth - 1] : PrCssKeyword_Count;
            if (!unit && inside != PrCssKeyword_Count) {
                prTextBufferAppend(&rewritten, inside == PrCssKeyword_HueRotate ? "deg" : "px");
                changed = true;
            }
            i = end;
            continue;
        }
        if (c == ')' && depth != 0)
            --depth;
        prTextBufferAppendChar(&rewritten, c);
        ++i;
    }
    if (changed)
        cssHintAdd(out, "filter", PrStringView{rewritten.data, rewritten.size});
    prTextBufferReset(&rewritten);
}

// Appends the hints of an SVG element's presentation attributes. An attribute is parsed as a value of its property,
// except that a number alone may also be a length in pixels, as SVG writes lengths. A value that parses neither way
// makes no declaration. A transform is in SVG's syntax alone, without CSS's units and functions
// (css-transforms-1/#svg-syntax). A path's d is path data, which the d property takes as the string of path()
// (w3c/svgwg#320). A pathLength is a number alone, which path-length takes as a length in pixels.
void addSvg(PrTextBuffer* out, const PrDomElement* element) {
    for (const PrDomNode* node = element->firstAttribute; node != nullptr; node = node->next) {
        const PrDomAttr* attribute = static_cast<const PrDomAttr*>(node);
        if (attribute->space != PrDomNamespace_None)
            continue;
        PrCssProperty property = svgPresentationOf(element, prStringView(&attribute->name));
        if (property == PrCssProperty_Count)
            continue;
        PrStringView value = stringTrimBy(prStringView(&attribute->value), prAsciiIsSpace);
        if (property == PrCssProperty_D) {
            prTextBufferAppend(out, "d:path(");
            cssSerializeString(out, value);
            prTextBufferAppend(out, ");");
            continue;
        }
        if (value.size == 0 || !isOneValue(value))
            continue;
        if (property == PrCssProperty_PathLength) {
            double number = 0;
            if (isBareNumber(value) && prF64ParsePrefix(value, &number))
                cssHintAddPixels(out, "path-length", number);
            continue;
        }
        if (property == PrCssProperty_Transform) {
            uint32_t mark = out->size;
            prTextBufferAppend(out, "transform:");
            if (svgTransform(out, value))
                prTextBufferAppendChar(out, ';');
            else
                out->size = mark;
            continue;
        }
        cssHintAdd(out, cssPropertyName(property), value);
        double number = 0;
        if (isBareNumber(value) && prF64ParsePrefix(value, &number))
            cssHintAddPixels(out, cssPropertyName(property), number);
        if (property == PrCssProperty_Filter)
            addSvgFilter(out, value);
    }
}

// Returns true if an attribute of the element is a presentation attribute.
bool hasOwn(const PrDomElement* element) {
    for (const PrDomNode* node = element->firstAttribute; node != nullptr; node = node->next) {
        if (svgPresentationOf(element, prStringView(&static_cast<const PrDomAttr*>(node)->name)) != PrCssProperty_Count)
            return true;
    }
    return false;
}

CssHintReach mapped(const PrDomElement* element, PrStringView name) {
    return svgPresentationOf(element, name) != PrCssProperty_Count ? CssHintReach_Self : CssHintReach_None;
}

constexpr CssHintProvider kProvider = {.hasOwn = hasOwn, .own = addSvg, .mapped = mapped};

} // namespace

const CssHintProvider* svgHints() {
    return &kProvider;
}

} // namespace pr_impl
