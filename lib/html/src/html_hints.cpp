// Presentational hints: the attributes of HTML elements that map to CSS properties
//
// html - https://html.spec.whatwg.org/multipage/rendering.html

#include "html/src/html_hints.h"

#include "pr/ascii.h"
#include "pr/float_convert.h"
#include "pr/integer.h"
#include "pr/string_ops.h"
#include "pr/text_buffer.h"

#include "css/src/css_color.h"
#include "css/src/css_serialize.h"
#include "dom/src/dom_attribute.h"
#include "dom/src/dom_element_state.h"
#include "dom/src/dom_flat.h"
#include "dom/src/dom_microsyntax.h"
#include "dom/src/dom_node.h"

namespace pr_impl {
namespace {

// The kinds of element that take hints from another element. A table gives hints to its row groups, rows, cells and
// column groups (html/#tables-2). A body gives hints to the document's links (html/#the-page).
enum {
    DomTaker_Section,
    DomTaker_Row,
    DomTaker_Cell,
    DomTaker_ColumnGroup,
    DomTaker_Link = 0,
    DomTaker_Visited = 1,
    DomTaker_Active = 2,
    DomTaker_Count = 4,   // at most kCssHintTakers
};
typedef uint8_t DomTaker;

bool isHtml(const PrDomElement* element, PrDomTag tag) {
    return element != nullptr && element->space == PrDomNamespace_Html && element->tag == tag;
}

bool isTableSection(const PrDomElement* element) {
    return isHtml(element, PrDomTag_Thead) || isHtml(element, PrDomTag_Tbody) || isHtml(element, PrDomTag_Tfoot);
}

bool isCell(const PrDomElement* element) {
    return isHtml(element, PrDomTag_Td) || isHtml(element, PrDomTag_Th);
}

bool isHeading(const PrDomElement* element) {
    return element->space == PrDomNamespace_Html && element->tag >= PrDomTag_H1 && element->tag <= PrDomTag_H6;
}

// Parses `text` by the rules for parsing non-negative integers (html/#rules-for-parsing-non-negative-integers).
bool parseNonNegative(PrStringView text, int64_t* out) {
    return domParseInteger(text, out) && *out >= 0;
}

// Appends a declaration of each of the four properties of a box's sides that `prefix` and `suffix` name, as "border-"
// and "-width" name the border widths.
void addSides(TextBuffer* out, PrStringView prefix, PrStringView suffix, PrStringView value) {
    const PrStringView sides[] = {"top", "right", "bottom", "left"};
    for (PrStringView side : sides) {
        textAppend(out, prefix);
        textAppend(out, side);
        textAppend(out, suffix);
        textAppendChar(out, ':');
        textAppend(out, value);
        textAppendChar(out, ';');
    }
}

// Appends `property` mapped to the attribute `name` as a pixel length (html/#maps-to-the-pixel-length-property). A
// value beyond the range of a CSS integer is clamped to that range.
void addPixelLength(TextBuffer* out, const PrDomElement* element, PrStringView name, PrStringView property) {
    int64_t value = 0;
    if (parseNonNegative(domAttributeValue(element, name), &value))
        cssHintAddPixels(out, property, double(value < INT32_MAX ? value : INT32_MAX));
}

// How a dimension attribute treats zero.
enum {
    Zero_Keep,                      // zero maps (html/#maps-to-the-dimension-property)
    Zero_Ignore,                    // zero maps to nothing (html/#maps-to-the-dimension-property-(ignoring-zero))
};
typedef uint8_t Zero;

// Appends `property` mapped to the attribute `name` as a dimension: a pixel length, or a percentage.
void addDimension(TextBuffer* out, const PrDomElement* element, PrStringView name, PrStringView property,
                  Zero zero) {
    double value = 0;
    bool percentage = false;
    if (!domParseDimension(domAttributeValue(element, name), &value, &percentage))
        return;
    if (zero == Zero_Ignore && value == 0)
        return;
    textAppend(out, property);
    textAppendChar(out, ':');
    cssSerializeNumber(out, value);
    textAppend(out, percentage ? "%;" : "px;");
}

// How the width and height attributes that map to an aspect ratio are parsed.
enum {
    Ratio_Integers,                 // as non-negative integers (html/#map-to-the-aspect-ratio-property)
    Ratio_Dimensions,               // as dimension values, of which a percentage gives no ratio
                                    // (html/#map-to-the-aspect-ratio-property-(using-dimension-rules))
};
typedef uint8_t Ratio;

// Appends the aspect ratio that the width and height attributes map to.
void addAspectRatio(TextBuffer* out, const PrDomElement* element, Ratio parse) {
    bool dimensions = parse == Ratio_Dimensions;
    double ratio[2] = {};
    const PrStringView names[] = {"width", "height"};
    for (uint32_t i = 0; i < 2; ++i) {
        PrStringView value = domAttributeValue(element, names[i]);
        bool percentage = false;
        int64_t integer = 0;
        if (dimensions ? !domParseDimension(value, &ratio[i], &percentage) || percentage
                       : !parseNonNegative(value, &integer))
            return;
        if (!dimensions)
            ratio[i] = double(integer);
    }
    textAppend(out, "aspect-ratio:auto ");
    cssSerializeNumber(out, ratio[0]);
    textAppendChar(out, '/');
    cssSerializeNumber(out, ratio[1]);
    textAppendChar(out, ';');
}

// Appends `property` mapped to the color of the attribute `name`, if it parses as a legacy color value
// (html/#rules-for-parsing-a-legacy-colour-value).
void addColor(TextBuffer* out, const PrDomElement* element, PrStringView name, PrStringView property) {
    PrColor8 color{};
    if (!htmlParseLegacyColor(domAttributeValue(element, name), &color))
        return;
    constexpr char kHex[] = "0123456789abcdef";
    char hex[7] = {'#'};
    const uint8_t channels[] = {color.r, color.g, color.b};
    for (uint32_t i = 0; i < 3; ++i) {
        hex[1 + 2 * i] = kHex[channels[i] >> 4];
        hex[2 + 2 * i] = kHex[channels[i] & 15];
    }
    cssHintAdd(out, property, PrStringView{hex, 7});
}

// Appends the background image that a background attribute maps to, if it is not empty. The URL resolves against the
// document's base URL, as an image of a hint does.
void addBackground(TextBuffer* out, const PrDomElement* element) {
    PrStringView value = domAttributeValue(element, "background");
    if (value.size == 0)
        return;
    textAppend(out, "background-image:url(");
    cssSerializeString(out, value);
    textAppend(out, ");");
}

// The values of an align attribute that center text. Every element whose align attribute maps to text-align also takes
// "left", "right", "justify" and "center".
enum {
    Center_Center,                  // "center" alone, for a p or a heading
    Center_Middle,                  // also "middle", for a div (html/#flow-content-3)
    Center_Absmiddle,               // also "middle" and "absmiddle", for a table part (html/#tables-2)
};
typedef uint8_t Center;

// Returns the text-align value that an align attribute maps to, or the null view for a value that maps to none.
PrStringView textAlignOf(PrStringView align, Center center) {
    if (asciiEqualsFold(align, "left"))
        return "left";
    if (asciiEqualsFold(align, "right"))
        return "right";
    if (asciiEqualsFold(align, "justify"))
        return "justify";
    if (asciiEqualsFold(align, "center") || (center != Center_Center && asciiEqualsFold(align, "middle"))
        || (center == Center_Absmiddle && asciiEqualsFold(align, "absmiddle")))
        return "center";
    return PrStringView{};
}

// Appends the text-align that the element's align attribute maps to.
void addTextAlign(TextBuffer* out, const PrDomElement* element, Center center) {
    PrStringView align = textAlignOf(domAttributeValue(element, "align"), center);
    if (align.data != nullptr)
        cssHintAdd(out, "text-align", align);
}

// Appends the vertical-align that a table part's valign attribute maps to.
void addVerticalAlign(TextBuffer* out, const PrDomElement* element) {
    PrStringView valign = domAttributeValue(element, "valign");
    const PrStringView values[] = {"top", "middle", "bottom", "baseline"};
    for (PrStringView value : values) {
        if (asciiEqualsFold(valign, value))
            cssHintAdd(out, "vertical-align", value);
    }
}

// Appends what the align attribute of an embedded element maps to: a float, or a vertical alignment
// (html/#attributes-for-embedded-content-and-images).
void addEmbeddedAlign(TextBuffer* out, const PrDomElement* element) {
    PrStringView align = domAttributeValue(element, "align");
    if (asciiEqualsFold(align, "left") || asciiEqualsFold(align, "right"))
        cssHintAdd(out, "float", asciiEqualsFold(align, "left") ? "left" : "right");
    else if (asciiEqualsFold(align, "top") || asciiEqualsFold(align, "baseline") || asciiEqualsFold(align, "bottom"))
        cssHintAdd(out, "vertical-align", asciiEqualsFold(align, "top") ? "top"
                                   : asciiEqualsFold(align, "bottom") ? "bottom"
                                                                      : "baseline");
    else if (asciiEqualsFold(align, "texttop"))
        cssHintAdd(out, "vertical-align", "text-top");
    else if (asciiEqualsFold(align, "absmiddle") || asciiEqualsFold(align, "abscenter"))
        cssHintAdd(out, "vertical-align", "middle");
}

// Appends the margins that hspace and vspace map to.
void addSpaces(TextBuffer* out, const PrDomElement* element) {
    addDimension(out, element, "hspace", "margin-left", Zero_Keep);
    addDimension(out, element, "hspace", "margin-right", Zero_Keep);
    addDimension(out, element, "vspace", "margin-top", Zero_Keep);
    addDimension(out, element, "vspace", "margin-bottom", Zero_Keep);
}

// Appends the border that the border attribute of an image or object maps to, if it is greater than zero.
void addImageBorder(TextBuffer* out, const PrDomElement* element) {
    int64_t value = 0;
    if (!parseNonNegative(domAttributeValue(element, "border"), &value) || value == 0)
        return;
    TextBuffer width{};
    cssSerializeNumber(&width, double(value < INT32_MAX ? value : INT32_MAX));
    textAppend(&width, "px");
    addSides(out, "border-", "-width", textView(&width));
    addSides(out, "border-", "-style", "solid");
    textReset(&width);
}

// Appends the declaration of a counter with the `value` that an attribute gives it, clamped to the range of a CSS
// integer.
void addCounter(TextBuffer* out, PrStringView property, PrStringView counter, int64_t value) {
    int64_t clamped = value < INT32_MIN ? INT32_MIN : value > INT32_MAX ? INT32_MAX : value;
    textAppend(out, property);
    textAppendChar(out, ':');
    textAppend(out, counter);
    textAppendChar(out, ' ');
    if (clamped < 0)
        textAppendChar(out, '-');
    textAppendU64(out, clamped < 0 ? uint64_t(-clamped) : uint64_t(clamped));
    textAppendChar(out, ';');
}

// Appends the list-style-type that an ol, ul or li's type attribute maps to (html/#lists). An ol's types are compared
// exactly, so that "a" and "A" differ, and a ul's ASCII case-insensitively. An li takes either kind.
void addListType(TextBuffer* out, const PrDomElement* element) {
    PrStringView type = domAttributeValue(element, "type");
    if (type.data == nullptr)
        return;
    PrStringView style{};
    if (element->tag != PrDomTag_Ul) {
        style = type == "1" ? "decimal" : type == "a" ? "lower-alpha" : type == "A" ? "upper-alpha"
                : type == "i" ? "lower-roman" : type == "I" ? "upper-roman" : PrStringView{};
    }
    if (style.data == nullptr && element->tag != PrDomTag_Ol) {
        const PrStringView values[] = {"none", "disc", "circle", "square"};
        for (PrStringView value : values) {
            if (asciiEqualsFold(type, value))
                style = value;
        }
    }
    if (style.data != nullptr)
        cssHintAdd(out, "list-style-type", style);
}

// Appends the counters that an ol's start and reversed attributes and an li's value attribute map to
// (html/#css-list-rendering).
void addListCounters(TextBuffer* out, const PrDomElement* element) {
    int64_t value = 0;
    if (element->tag == PrDomTag_Li) {
        if (domParseInteger(domAttributeValue(element, "value"), &value))
            addCounter(out, "counter-set", "list-item", value);
        return;
    }
    if (element->tag != PrDomTag_Ol)
        return;
    bool start = domParseInteger(domAttributeValue(element, "start"), &value);
    if (domAttributeValue(element, "reversed").data != nullptr) {
        if (start)
            addCounter(out, "counter-reset", "reversed(list-item)", value < INT64_MAX ? value + 1 : value);
        else
            cssHintAdd(out, "counter-reset", "reversed(list-item)");
    } else if (start) {
        addCounter(out, "counter-reset", "list-item", value > INT64_MIN ? value - 1 : value);
    }
}

// Appends the font size that a font's size attribute maps to (html/#rules-for-parsing-a-legacy-font-size).
void addLegacyFontSize(TextBuffer* out, PrStringView input) {
    size_t at = 0;
    while (at < input.size && asciiIsSpace(input.data[at]))
        ++at;
    if (at == input.size)
        return;
    char mode = input.data[at] == '+' || input.data[at] == '-' ? input.data[at++] : 0;
    uint64_t digits = 0;
    if (parseU64Prefix(input.data + at, input.size - at, &digits) == 0)
        return;
    int64_t value = digits > 100 ? 100 : int64_t(digits);
    if (mode == '+')
        value += 3;
    else if (mode == '-')
        value = 3 - value;
    value = value > 7 ? 7 : value < 1 ? 1 : value;
    const PrStringView keywords[] = {"x-small", "small", "medium", "large", "x-large", "xx-large", "xxx-large"};
    cssHintAdd(out, "font-size", keywords[value - 1]);
}

// Returns true if the table's rules attribute is one of the values that style its borders.
bool hasRules(PrStringView rules) {
    const PrStringView values[] = {"none", "groups", "rows", "cols", "all"};
    for (PrStringView value : values) {
        if (asciiEqualsFold(rules, value))
            return true;
    }
    return false;
}

// Returns true if the table's border attribute is present and not equivalent to zero (html/#magic-border-selector). The
// attribute is equivalent to zero if it parses as the non-negative integer 0.
bool borderNonzero(const PrDomElement* table) {
    PrStringView border = domAttributeValue(table, "border");
    int64_t value = 0;
    return border.data != nullptr && (!parseNonNegative(border, &value) || value != 0);
}

// Appends a table's hints for itself (html/#tables-2).
void addTable(TextBuffer* out, const PrDomElement* table) {
    PrStringView align = domAttributeValue(table, "align");
    if (asciiEqualsFold(align, "left") || asciiEqualsFold(align, "right")) {
        cssHintAdd(out, "float", asciiEqualsFold(align, "left") ? "left" : "right");
    } else if (asciiEqualsFold(align, "center")) {
        cssHintAdd(out, "margin-left", "auto");
        cssHintAdd(out, "margin-right", "auto");
    }
    addPixelLength(out, table, "cellspacing", "border-spacing");
    addDimension(out, table, "height", "height", Zero_Keep);
    addDimension(out, table, "width", "width", Zero_Ignore);
    addColor(out, table, "bgcolor", "background-color");
    addBackground(out, table);
    PrStringView rules = domAttributeValue(table, "rules");
    PrStringView frame = domAttributeValue(table, "frame");
    // The frame values, each with the border styles of its sides in the order of the border-style shorthand.
    struct Frame {
        PrStringView value;
        PrStringView style;
    };
    const Frame frames[] = {{"void", "hidden"},
                            {"above", "outset hidden hidden hidden"},
                            {"below", "hidden hidden outset hidden"},
                            {"hsides", "outset hidden outset hidden"},
                            {"lhs", "hidden hidden hidden outset"},
                            {"rhs", "hidden outset hidden hidden"},
                            {"vsides", "hidden outset"},
                            {"box", "outset"},
                            {"border", "outset"}};
    const Frame* framed = nullptr;
    for (const Frame& f : frames) {
        if (asciiEqualsFold(frame, f.value))
            framed = &f;
    }
    if (hasRules(rules) || framed != nullptr)
        cssHintAdd(out, "border-color", "black");
    addColor(out, table, "bordercolor", "border-color");
    PrStringView border = domAttributeValue(table, "border");
    if (border.data != nullptr) {
        int64_t value = 0;
        TextBuffer width{};
        bool parsed = parseNonNegative(border, &value);
        cssSerializeNumber(&width, parsed ? double(value < INT32_MAX ? value : INT32_MAX) : 1);
        textAppend(&width, "px");
        addSides(out, "border-", "-width", textView(&width));
        textReset(&width);
    }
    // Later declarations win: a frame over the border attribute's style, and both over the rules attribute's.
    if (hasRules(rules)) {
        cssHintAdd(out, "border-style", "hidden");
        cssHintAdd(out, "border-collapse", "collapse");
    }
    if (borderNonzero(table))
        cssHintAdd(out, "border-style", "outset");
    if (framed != nullptr)
        cssHintAdd(out, "border-style", framed->style);
}

// Appends the hints that a table gives the elements of `taker`'s kind.
void addTableGiven(TextBuffer* out, const PrDomElement* table, DomTaker taker) {
    PrStringView rules = domAttributeValue(table, "rules");
    switch (taker) {
        case DomTaker_Section:
            if (asciiEqualsFold(rules, "groups")) {
                cssHintAdd(out, "border-block-width", "1px");
                cssHintAdd(out, "border-block-style", "solid");
            }
            break;
        case DomTaker_Row:
            if (asciiEqualsFold(rules, "rows")) {
                cssHintAdd(out, "border-block-width", "1px");
                cssHintAdd(out, "border-block-style", "solid");
            }
            break;
        case DomTaker_ColumnGroup:
            if (asciiEqualsFold(rules, "groups")) {
                cssHintAdd(out, "border-inline-width", "1px");
                cssHintAdd(out, "border-inline-style", "solid");
            }
            break;
        case DomTaker_Cell:
            if (hasRules(rules))
                cssHintAdd(out, "border-color", "black");
            if (borderNonzero(table)) {
                cssHintAdd(out, "border-width", "1px");
                cssHintAdd(out, "border-style", "inset");
            }
            if (asciiEqualsFold(rules, "none") || asciiEqualsFold(rules, "groups") || asciiEqualsFold(rules, "rows")) {
                cssHintAdd(out, "border-width", "1px");
                cssHintAdd(out, "border-style", "none");
            } else if (asciiEqualsFold(rules, "cols")) {
                cssHintAdd(out, "border-width", "1px");
                cssHintAdd(out, "border-block-style", "none");
                cssHintAdd(out, "border-inline-style", "solid");
            } else if (asciiEqualsFold(rules, "all")) {
                cssHintAdd(out, "border-width", "1px");
                cssHintAdd(out, "border-style", "solid");
            }
            addPixelLength(out, table, "cellpadding", "padding-top");
            addPixelLength(out, table, "cellpadding", "padding-right");
            addPixelLength(out, table, "cellpadding", "padding-bottom");
            addPixelLength(out, table, "cellpadding", "padding-left");
            break;
    }
}

// Appends a body's hints for itself (html/#the-page). For each axis, the first of its attributes that is present gives
// both margins. A margin attribute that fails to parse gives a margin of 8px. The user agent's sheet gives the same
// margin to a body without the attributes.
void addBody(TextBuffer* out, const PrDomElement* body) {
    addColor(out, body, "bgcolor", "background-color");
    addBackground(out, body);
    addColor(out, body, "text", "color");
    const PrStringView sources[2][2] = {{"marginheight", "topmargin"}, {"marginwidth", "leftmargin"}};
    const PrStringView margins[2][2] = {{"margin-top", "margin-bottom"}, {"margin-left", "margin-right"}};
    for (uint32_t axis = 0; axis < 2; ++axis) {
        PrStringView found = domAttributeValue(body, sources[axis][0]);
        if (found.data == nullptr)
            found = domAttributeValue(body, sources[axis][1]);
        if (found.data == nullptr)
            continue;
        int64_t value = 0;
        double pixels = parseNonNegative(found, &value) ? double(value < INT32_MAX ? value : INT32_MAX) : 8;
        cssHintAddPixels(out, margins[axis][0], pixels);
        cssHintAddPixels(out, margins[axis][1], pixels);
    }
}

// Appends an hr's hints (html/#the-hr-element-2).
void addRule(TextBuffer* out, const PrDomElement* hr) {
    PrStringView align = domAttributeValue(hr, "align");
    if (asciiEqualsFold(align, "left")) {
        cssHintAdd(out, "margin-left", "0");
        cssHintAdd(out, "margin-right", "auto");
    } else if (asciiEqualsFold(align, "right")) {
        cssHintAdd(out, "margin-left", "auto");
        cssHintAdd(out, "margin-right", "0");
    } else if (asciiEqualsFold(align, "center")) {
        cssHintAdd(out, "margin-left", "auto");
        cssHintAdd(out, "margin-right", "auto");
    }
    bool solid = domAttributeValue(hr, "color").data != nullptr || domAttributeValue(hr, "noshade").data != nullptr;
    if (solid)
        cssHintAdd(out, "border-style", "solid");
    int64_t size = 0;
    if (parseNonNegative(domAttributeValue(hr, "size"), &size)) {
        double pixels = double(size < INT32_MAX ? size : INT32_MAX);
        if (solid) {
            TextBuffer width{};
            cssSerializeNumber(&width, pixels / 2);
            textAppend(&width, "px");
            addSides(out, "border-", "-width", textView(&width));
            textReset(&width);
        } else if (size == 1) {
            cssHintAdd(out, "border-bottom-width", "0");
        } else if (size > 1) {
            cssHintAddPixels(out, "height", pixels - 2);
        }
    }
    addDimension(out, hr, "width", "width", Zero_Keep);
    addColor(out, hr, "color", "color");
}

// Appends the hints that the element's own attributes map to.
void addOwn(TextBuffer* out, const PrDomElement* element) {
    switch (element->tag) {
        case PrDomTag_Body: addBody(out, element); break;
        case PrDomTag_Pre:
            if (domAttributeValue(element, "wrap").data != nullptr)
                cssHintAdd(out, "white-space", "pre-wrap");
            break;
        case PrDomTag_Center: cssHintAdd(out, "text-align", "center"); break;
        case PrDomTag_Div: addTextAlign(out, element, Center_Middle); break;
        case PrDomTag_P: addTextAlign(out, element, Center_Center); break;
        case PrDomTag_Br: {
            PrStringView clear = domAttributeValue(element, "clear");
            if (asciiEqualsFold(clear, "left") || asciiEqualsFold(clear, "right"))
                cssHintAdd(out, "clear", asciiEqualsFold(clear, "left") ? "left" : "right");
            else if (asciiEqualsFold(clear, "all") || asciiEqualsFold(clear, "both"))
                cssHintAdd(out, "clear", "both");
            break;
        }
        case PrDomTag_Font: {
            addColor(out, element, "color", "color");
            PrStringView face = domAttributeValue(element, "face");
            if (face.data != nullptr)
                cssHintAdd(out, "font-family", face);
            PrStringView size = domAttributeValue(element, "size");
            if (size.data != nullptr)
                addLegacyFontSize(out, size);
            break;
        }
        case PrDomTag_Ol:
        case PrDomTag_Ul:
        case PrDomTag_Li:
            addListType(out, element);
            addListCounters(out, element);
            break;
        case PrDomTag_Table: addTable(out, element); break;
        case PrDomTag_Caption:
            if (asciiEqualsFold(domAttributeValue(element, "align"), "bottom"))
                cssHintAdd(out, "caption-side", "bottom");
            break;
        case PrDomTag_Col: addDimension(out, element, "width", "width", Zero_Keep); break;
        case PrDomTag_Thead:
        case PrDomTag_Tbody:
        case PrDomTag_Tfoot:
        case PrDomTag_Tr:
            addDimension(out, element, "height", "height", Zero_Keep);
            addTextAlign(out, element, Center_Absmiddle);
            addVerticalAlign(out, element);
            addColor(out, element, "bgcolor", "background-color");
            addBackground(out, element);
            break;
        case PrDomTag_Td:
        case PrDomTag_Th: {
            addDimension(out, element, "height", "height", Zero_Ignore);
            addDimension(out, element, "width", "width", Zero_Ignore);
            addTextAlign(out, element, Center_Absmiddle);
            addVerticalAlign(out, element);
            if (domAttributeValue(element, "nowrap").data != nullptr) {
                // In quirks mode, a fixed width keeps the cell's text wrapping.
                double width = 0;
                bool percentage = false;
                bool fixed = element->document->quirks == DomQuirks_Yes
                             && domParseDimension(domAttributeValue(element, "width"), &width, &percentage)
                             && width != 0 && !percentage;
                cssHintAdd(out, "white-space", fixed ? "normal" : "nowrap");
            }
            addColor(out, element, "bgcolor", "background-color");
            addBackground(out, element);
            break;
        }
        case PrDomTag_Hr: addRule(out, element); break;
        case PrDomTag_Embed:
            addEmbeddedAlign(out, element);
            addSpaces(out, element);
            addDimension(out, element, "width", "width", Zero_Keep);
            addDimension(out, element, "height", "height", Zero_Keep);
            break;
        case PrDomTag_Iframe: {
            addEmbeddedAlign(out, element);
            int64_t frameborder = 0;
            if (!domParseInteger(domAttributeValue(element, "frameborder"), &frameborder) || frameborder == 0) {
                if (domAttributeValue(element, "frameborder").data != nullptr)
                    addSides(out, "border-", "-width", "0");
            }
            addDimension(out, element, "width", "width", Zero_Keep);
            addDimension(out, element, "height", "height", Zero_Keep);
            break;
        }
        case PrDomTag_Img:
        case PrDomTag_Object:
            addEmbeddedAlign(out, element);
            addSpaces(out, element);
            addImageBorder(out, element);
            addDimension(out, element, "width", "width", Zero_Keep);
            addDimension(out, element, "height", "height", Zero_Keep);
            if (element->tag == PrDomTag_Img)
                addAspectRatio(out, element, Ratio_Dimensions);
            break;
        case PrDomTag_Input:
            if (domElementIsImageButton(element)) {
                addEmbeddedAlign(out, element);
                addSpaces(out, element);
                addImageBorder(out, element);
                addDimension(out, element, "width", "width", Zero_Keep);
                addDimension(out, element, "height", "height", Zero_Keep);
                addAspectRatio(out, element, Ratio_Dimensions);
            }
            break;
        case PrDomTag_Video:
            addDimension(out, element, "width", "width", Zero_Keep);
            addDimension(out, element, "height", "height", Zero_Keep);
            addAspectRatio(out, element, Ratio_Dimensions);
            break;
        case PrDomTag_Canvas: addAspectRatio(out, element, Ratio_Integers); break;
        case PrDomTag_Marquee:
            addColor(out, element, "bgcolor", "background-color");
            addDimension(out, element, "width", "width", Zero_Keep);
            addDimension(out, element, "height", "height", Zero_Keep);
            addSpaces(out, element);
            break;
        case PrDomTag_Textarea:
            if (asciiEqualsFold(domAttributeValue(element, "wrap"), "off"))
                cssHintAdd(out, "white-space", "pre");
            break;
        default:
            if (isHeading(element))
                addTextAlign(out, element, Center_Center);
            break;
    }
}

// Returns true if `name` is one of the attributes in `names`.
template <size_t N>
bool oneOf(PrStringView name, const PrStringView (&names)[N]) {
    for (PrStringView candidate : names) {
        if (name == candidate)
            return true;
    }
    return false;
}

// Returns true if the attribute named `name` maps to a hint of the element itself.
bool mapsOwn(const PrDomElement* element, PrStringView name) {
    static constexpr PrStringView kBody[] = {"bgcolor", "background", "text", "marginheight", "topmargin",
                                             "marginwidth", "leftmargin"};
    static constexpr PrStringView kFont[] = {"color", "face", "size"};
    static constexpr PrStringView kList[] = {"type", "start", "reversed", "value"};
    static constexpr PrStringView kTable[] = {"align", "cellspacing", "height", "width", "bgcolor", "background",
                                              "rules", "frame", "bordercolor", "border"};
    static constexpr PrStringView kPart[] = {"height", "align", "valign", "bgcolor", "background"};
    static constexpr PrStringView kCell[] = {"height", "width", "align", "valign", "nowrap", "bgcolor", "background"};
    static constexpr PrStringView kRule[] = {"align", "color", "noshade", "size", "width"};
    static constexpr PrStringView kEmbedded[] = {"align", "hspace", "vspace", "border", "width", "height", "type"};
    static constexpr PrStringView kIframe[] = {"align", "frameborder", "width", "height"};
    static constexpr PrStringView kSized[] = {"width", "height"};
    static constexpr PrStringView kMarquee[] = {"bgcolor", "width", "height", "hspace", "vspace"};
    switch (element->tag) {
        case PrDomTag_Body: return oneOf(name, kBody);
        case PrDomTag_Pre:
        case PrDomTag_Textarea: return name == "wrap";
        case PrDomTag_Div:
        case PrDomTag_P:
        case PrDomTag_Caption: return name == "align";
        case PrDomTag_Br: return name == "clear";
        case PrDomTag_Font: return oneOf(name, kFont);
        case PrDomTag_Ol:
        case PrDomTag_Ul:
        case PrDomTag_Li: return oneOf(name, kList);
        case PrDomTag_Table: return oneOf(name, kTable);
        case PrDomTag_Col: return name == "width";
        case PrDomTag_Thead:
        case PrDomTag_Tbody:
        case PrDomTag_Tfoot:
        case PrDomTag_Tr: return oneOf(name, kPart);
        case PrDomTag_Td:
        case PrDomTag_Th: return oneOf(name, kCell);
        case PrDomTag_Hr: return oneOf(name, kRule);
        case PrDomTag_Embed:
        case PrDomTag_Img:
        case PrDomTag_Object:
        case PrDomTag_Input: return oneOf(name, kEmbedded);
        case PrDomTag_Iframe: return oneOf(name, kIframe);
        case PrDomTag_Video:
        case PrDomTag_Canvas: return oneOf(name, kSized);
        case PrDomTag_Marquee: return oneOf(name, kMarquee);
        default: return isHeading(element) && name == "align";
    }
}

// Returns the table that gives hints to the element, or null if it takes none, and writes the kind of element that it
// is to `taker`. A row group and a column group take hints from their parent table, a row from the table that holds it
// or its row group, and a cell from its row's table.
const PrDomElement* PR_NULLABLE givingTable(const PrDomElement* element, DomTaker* taker) {
    const PrDomElement* at = element;
    if (isCell(at)) {
        *taker = DomTaker_Cell;
        at = domNodeParentElement(at);
        if (!isHtml(at, PrDomTag_Tr))
            return nullptr;
    } else if (isHtml(at, PrDomTag_Tr)) {
        *taker = DomTaker_Row;
    } else if (isTableSection(at) || isHtml(at, PrDomTag_Colgroup)) {
        *taker = isTableSection(at) ? DomTaker_Section : DomTaker_ColumnGroup;
        const PrDomElement* table = domNodeParentElement(at);
        return isHtml(table, PrDomTag_Table) ? table : nullptr;
    } else {
        return nullptr;
    }
    // `at` is a row.
    const PrDomElement* parent = domNodeParentElement(at);
    if (isTableSection(parent))
        parent = domNodeParentElement(parent);
    return isHtml(parent, PrDomTag_Table) ? parent : nullptr;
}

// Returns the element that gives hints to `element`, or null if none does, and writes the kind of element that it is to
// `taker`. A link takes the colors of its document's body.
const PrDomElement* PR_NULLABLE giver(const PrDomElement* element, DomTaker* taker) {
    if (element->space != PrDomNamespace_Html)
        return nullptr;
    switch (element->tag) {
        case PrDomTag_A:
        case PrDomTag_Area:
        case PrDomTag_Td:
        case PrDomTag_Th:
        case PrDomTag_Tr:
        case PrDomTag_Thead:
        case PrDomTag_Tbody:
        case PrDomTag_Tfoot:
        case PrDomTag_Colgroup:
            break;
        default:
            return nullptr;
    }
    if ((element->tag == PrDomTag_A || element->tag == PrDomTag_Area) && domElementLink(element)) {
        *taker = domElementActive(element) ? DomTaker_Active
                 : domElementVisited(element) ? DomTaker_Visited
                                              : DomTaker_Link;
        PrDomElement* body = domDocumentBody(element->document);
        return isHtml(body, PrDomTag_Body) ? body : nullptr;
    }
    return givingTable(element, taker);
}

// Appends the hints that a table or a body gives the elements of the kind `taker`.
void addGiven(TextBuffer* out, const PrDomElement* element, uint32_t taker) {
    if (element->tag == PrDomTag_Table) {
        addTableGiven(out, element, DomTaker(taker));
    } else if (taker < DomTaker_Count - 1) {
        const PrStringView names[] = {"link", "vlink", "alink"};
        addColor(out, element, names[taker], "color");
    }
}

// Returns true if an attribute of the element maps to a hint of its own, or the element is a center, whose hint no
// attribute gives.
bool hasOwn(const PrDomElement* element) {
    if (element->tag == PrDomTag_Center)
        return true;
    for (const PrDomNode* node = element->firstAttribute; node != nullptr; node = node->next) {
        if (mapsOwn(element, prStringView(&static_cast<const PrDomAttr*>(node)->name)))
            return true;
    }
    return false;
}

CssHintReach mapped(const PrDomElement* element, PrStringView name) {
    // A table's border, rules and cellpadding style its parts, and a body's link colors style the document's links.
    if (element->tag == PrDomTag_Table && (name == "border" || name == "rules" || name == "cellpadding"))
        return CssHintReach_Subtree;
    if (element->tag == PrDomTag_Body && (name == "link" || name == "vlink" || name == "alink"))
        return CssHintReach_Document;
    return mapsOwn(element, name) ? CssHintReach_Self : CssHintReach_None;
}

const PrDomElement* PR_NULLABLE giverOf(const PrDomElement* element, uint32_t* taker) {
    DomTaker kind = 0;
    const PrDomElement* found = giver(element, &kind);
    *taker = kind;
    return found;
}

constexpr CssHintProvider kProvider = {
    .hasOwn = hasOwn,
    .own = addOwn,
    .mapped = mapped,
    .giver = giverOf,
    .given = addGiven,
};

} // namespace

const CssHintProvider* htmlHints() {
    return &kProvider;
}

bool htmlParseLegacyColor(PrStringView text, PrColor8* out) {
    // Steps 2 to 4: an empty value, transparent, and a named color.
    size_t start = 0;
    size_t end = text.size;
    while (start < end && asciiIsSpace(text.data[start]))
        ++start;
    while (end > start && asciiIsSpace(text.data[end - 1]))
        --end;
    PrStringView input{text.data + start, end - start};
    if (input.size == 0 || asciiEqualsFold(input, "transparent"))
        return false;
    if (cssNamedColor(cssKeywordFromText(input), out))
        return true;
    auto hexValue = [](char c) {
        return c >= '0' && c <= '9'   ? c - '0'
               : c >= 'a' && c <= 'f' ? c - 'a' + 10
               : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                      : -1;
    };
    // Step 5: #rgb.
    if (input.size == 4 && input.data[0] == '#' && hexValue(input.data[1]) >= 0 && hexValue(input.data[2]) >= 0
        && hexValue(input.data[3]) >= 0) {
        *out = PrColor8{uint8_t(hexValue(input.data[1]) * 17), uint8_t(hexValue(input.data[2]) * 17),
                        uint8_t(hexValue(input.data[3]) * 17), 255};
        return true;
    }
    // Steps 6 to 10: a code point past U+FFFF becomes "00", at most 128 code units are kept, a leading '#' goes, any
    // other character becomes '0', and zeros pad the digits to a multiple of three.
    Array<char> digits;
    for (size_t i = 0; i < input.size && digits.count < 128;) {
        uint8_t byte = uint8_t(input.data[i]);
        size_t length = byte < 0x80 ? 1 : byte < 0xe0 ? 2 : byte < 0xf0 ? 3 : 4;
        if (i == 0 && byte == '#') {
            ++i;
            continue;
        }
        if (length == 4) {
            digits.push('0');
            digits.push('0');
        } else {
            digits.push(length == 1 && hexValue(char(byte)) >= 0 ? char(byte) : '0');
        }
        i += length;
    }
    if (digits.count > 128)
        digits.resize(128);
    while (digits.count == 0 || digits.count % 3 != 0)
        digits.push('0');
    // Steps 11 to 15: three components, each cut to its last 8 digits, its shared leading zeros removed down to two
    // digits, and then its first two digits.
    uint32_t length = digits.count / 3;
    uint32_t skip = length > 8 ? length - 8 : 0;
    length -= skip;
    auto at = [&](uint32_t component, uint32_t i) { return digits.data[component * (length + skip) + skip + i]; };
    uint32_t leading = 0;
    while (length - leading > 2 && at(0, leading) == '0' && at(1, leading) == '0' && at(2, leading) == '0')
        ++leading;
    uint8_t channels[3];
    for (uint32_t c = 0; c < 3; ++c) {
        int value = hexValue(at(c, leading));
        if (length - leading > 1)
            value = value * 16 + hexValue(at(c, leading + 1));
        channels[c] = uint8_t(value);
    }
    digits.reset();
    *out = PrColor8{channels[0], channels[1], channels[2], 255};
    return true;
}

} // namespace pr_impl
