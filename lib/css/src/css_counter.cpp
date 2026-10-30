// Counter representations: the text that counter(), counters() and a list item's marker write for a counter's value.

#include "css/src/css_counter.h"

#include "pr/array.h"
#include "pr/ascii.h"
#include "pr/float.h"
#include "pr/string_ops.h"
#include "pr/unicode_grapheme.h"

#include "css/src/css_at_rule.h"
#include "css/src/css_keyword.h"
#include "css/src/css_math.h"
#include "css/src/css_rule.h"
#include "css/src/css_unit.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

namespace {

// The number of hops along fallback styles before a representation uses decimal. Only a document's own styles can form
// a chain this long, or a cycle.
constexpr uint32_t kMaxFallbacks = 8;

// The symbol of a style that points, by the CssCounterSide it points to: a small triangle, which the disclosure styles
// take (css-counter-styles-3/#simple-symbolic leaves the symbol to the user agent).
constexpr PrStringView kTriangles[] = {
    "\xE2\x96\xB8",   // U+25B8 BLACK RIGHT-POINTING SMALL TRIANGLE
    "\xE2\x97\x82",   // U+25C2 BLACK LEFT-POINTING SMALL TRIANGLE
    "\xE2\x96\xBE",   // U+25BE BLACK DOWN-POINTING SMALL TRIANGLE
    "\xE2\x96\xB4",   // U+25B4 BLACK UP-POINTING SMALL TRIANGLE
};

// Returns the predefined style `name` names, or null. The disclosure styles, which the generated table gives no
// symbols, take a triangle that points: open to the block end, and closed to the inline end.
const CssCounterStyle* PR_NULLABLE predefinedStyle(PrStringView name) {
    const CssCounterStyle* style = cssCounterStylePredefined(name);
    if (style == nullptr || style->symbolCount != 0 || style->system != CssCounterSystem_Cyclic)
        return style;
    static const CssCounterStyle open = [] {
        CssCounterStyle made = *cssCounterStylePredefined("disclosure-open");
        made.symbols = &kTriangles[CssCounterSide_Down];
        made.symbolCount = 1;
        made.pointing = CssCounterPointing_BlockEnd;
        return made;
    }();
    static const CssCounterStyle closed = [] {
        CssCounterStyle made = *cssCounterStylePredefined("disclosure-closed");
        made.symbols = &kTriangles[CssCounterSide_Right];
        made.symbolCount = 1;
        made.pointing = CssCounterPointing_InlineEnd;
        return made;
    }();
    if (style->name == "disclosure-open")
        return &open;
    if (style->name == "disclosure-closed")
        return &closed;
    return style;
}

// Returns the number of code points in UTF-8 text.
uint32_t codePointsOf(PrStringView text) {
    uint32_t count = 0;
    for (size_t i = 0; i < text.size; ++i)
        count += (uint8_t(text.data[i]) & 0xC0) != 0x80 ? 1 : 0;
    return count;
}

// Returns the number of grapheme clusters in text.
uint32_t graphemesOf(PrStringView text) {
    uint32_t count = 0;
    for (size_t at = 0; at < text.size; at = prUnicodeGraphemeNext(text, at))
        ++count;
    return count;
}

// Returns true if `value` is in the style's range, or in the auto range of its system if it gives none
// (css-counter-styles-3/#counter-style-range).
bool inRange(const CssCounterStyle* style, int32_t value) {
    if (style->rangeCount == 0) {
        switch (style->system) {
            case CssCounterSystem_Alphabetic:
            case CssCounterSystem_Symbolic:
                return value >= 1;
            case CssCounterSystem_Additive:
                return value >= 0;
            case CssCounterSystem_Ethiopic:
                return value >= 1;
            default:
                return true;
        }
    }
    for (uint32_t i = 0; i < style->rangeCount; ++i) {
        if (value >= style->ranges[i].low && value <= style->ranges[i].high)
            return true;
    }
    return false;
}

// Returns true if the style writes a negative value as its absolute value inside the negative sign
// (css-counter-styles-3/#use-a-negative-sign).
bool usesNegativeSign(const CssCounterStyle* style) {
    switch (style->system) {
        case CssCounterSystem_Symbolic:
        case CssCounterSystem_Alphabetic:
        case CssCounterSystem_Numeric:
        case CssCounterSystem_Additive:
        case CssCounterSystem_Hebrew:
        case CssCounterSystem_ChineseInformal:
        case CssCounterSystem_ChineseFormal:
        case CssCounterSystem_JapaneseInformal:
        case CssCounterSystem_JapaneseFormal:
        case CssCounterSystem_KoreanInformal:
        case CssCounterSystem_KoreanFormal:
            return true;
        default:
            return false;
    }
}

// Appends the representation of `value`, which is not negative, in the longhand CJK style of `system` with `symbols`
// (css-counter-styles-3/#extended-range-optional). The groups of four digits from the ones place each take their group
// marker, and the digits other than 0 their digit markers. Then:
// - Informal Chinese drops the tens digit of a group from 10 to 19. Informal Japanese and Korean drop a 1 before a
//   digit marker, and informal Korean the 1 of a ten-thousands group of 1.
// - Chinese drops the zeros that end a group or the number, and writes one zero for each run of the others, across
//   groups. Japanese and Korean drop every zero.
// - Korean puts a space between groups.
bool appendCjk(TextBuffer* out, CssCounterSystem system, const PrStringView* symbols, uint32_t count, int64_t value) {
    if (count != 16 || value >= 10000000000000000)
        return false;
    if (value == 0) {
        textAppend(out, symbols[0]);
        return true;
    }
    bool chinese = system == CssCounterSystem_ChineseInformal || system == CssCounterSystem_ChineseFormal;
    bool korean = system == CssCounterSystem_KoreanInformal || system == CssCounterSystem_KoreanFormal;
    bool informal = system == CssCounterSystem_ChineseInformal || system == CssCounterSystem_JapaneseInformal ||
        system == CssCounterSystem_KoreanInformal;
    uint32_t groups[4] = {};
    uint32_t n = 0;
    for (int64_t rest = value; rest != 0; rest /= 10000)
        groups[n++] = uint32_t(rest % 10000);
    bool written = false;
    bool zero = false;              // true while zeros wait for the next digit to write them as one, in Chinese
    for (uint32_t i = n; i-- > 0;) {
        uint32_t group = groups[i];
        if (group == 0) {
            zero = chinese && written;
            continue;
        }
        if (korean && written)
            textAppend(out, " ");
        uint32_t digits[4] = {group % 10, group / 10 % 10, group / 100 % 10, group / 1000};
        uint32_t low = 0;
        while (digits[low] == 0)
            ++low;
        for (uint32_t place = 4; place-- > low;) {
            uint32_t digit = digits[place];
            if (digit == 0) {
                zero = chinese && written;
                continue;
            }
            if (zero)
                textAppend(out, symbols[0]);
            zero = false;
            bool dropped = informal && digit == 1 && ((chinese && place == 1 && group <= 19) ||
                (!chinese && place != 0) || (korean && place == 0 && i == 1 && group == 1));
            if (!dropped)
                textAppend(out, symbols[digit]);
            if (place != 0)
                textAppend(out, symbols[9 + place]);
            written = true;
        }
        if (i != 0)
            textAppend(out, symbols[12 + i]);
    }
    return true;
}

// Appends the representation of `value`, which is at least 1, in ethiopic-numeric with `symbols`
// (css-counter-styles-3/#ethiopic-numeric-counter-style). The groups of two digits from the least significant, by index
// from 0, keep their digits unless they are 0, or 1 in the most significant group or in an odd group. An odd group that
// is not 0 is followed by the odd groups' separator, and an even group other than the first by the even groups'.
bool appendEthiopic(TextBuffer* out, const PrStringView* symbols, uint32_t count, int64_t value) {
    if (count != 20 || value < 1)
        return false;
    if (value == 1) {
        textAppend(out, symbols[0]);
        return true;
    }
    uint32_t groups[8];
    uint32_t n = 0;
    for (int64_t rest = value; rest != 0; rest /= 100)
        groups[n++] = uint32_t(rest % 100);
    for (uint32_t i = n; i-- > 0;) {
        uint32_t group = groups[i];
        bool odd = (i & 1) != 0;
        bool dropped = group == 0 || (group == 1 && (i == n - 1 || odd));
        if (!dropped && group / 10 != 0)
            textAppend(out, symbols[9 + group / 10 - 1]);
        if (!dropped && group % 10 != 0)
            textAppend(out, symbols[group % 10 - 1]);
        if (odd && group != 0)
            textAppend(out, symbols[18]);
        else if (!odd && i != 0)
            textAppend(out, symbols[19]);
    }
    return true;
}

// Returns the symbols of a style in `flow`, toward which the symbol of a style that points turns.
uint32_t symbolsOf(const CssCounterStyle* style, CssCounterFlow flow, const PrStringView** out) {
    *out = style->symbols;
    if (style->pointing == CssCounterPointing_InlineEnd)
        *out = &kTriangles[flow.inlineEnd];
    else if (style->pointing == CssCounterPointing_BlockEnd)
        *out = &kTriangles[flow.blockEnd];
    return style->symbolCount;
}

// Appends the additive representation of `value`, from 1, by the style's tuples of weights below `below`. Returns false
// if they cannot represent it.
bool appendAdditiveBelow(TextBuffer* out, const CssCounterStyle* style, int64_t value, int32_t below) {
    uint32_t mark = out->text.count;
    for (uint32_t i = 0; i < style->additiveCount && value != 0; ++i) {
        const CssAdditiveTuple& tuple = style->additive[i];
        if (tuple.weight <= 0 || tuple.weight >= below || tuple.weight > value)
            continue;
        for (int64_t repeats = value / tuple.weight; repeats > 0; --repeats)
            textAppend(out, tuple.symbol);
        value %= tuple.weight;
    }
    if (value != 0)
        out->text.count = mark;
    return value == 0;
}

// Appends the initial representation of `value`, which is not negative if the style uses a negative sign
// (css-counter-styles-3/#initial-representation-for-the-counter-value). Returns false if the algorithm cannot represent
// it, or if the representation would be longer than kCssCounterMaxCodePoints.
bool appendInitial(TextBuffer* out, const CssCounterStyle* style, int64_t value, CssCounterFlow flow) {
    const PrStringView* symbols = nullptr;
    uint32_t count = symbolsOf(style, flow, &symbols);
    // A digit of each place, from the last place to the first.
    uint32_t digits[64];
    uint32_t places = 0;
    switch (style->system) {
        case CssCounterSystem_Cyclic: {
            if (count == 0)
                return false;
            int64_t at = (value - 1) % int64_t(count);
            textAppend(out, symbols[at < 0 ? at + count : at]);
            return true;
        }
        case CssCounterSystem_Fixed: {
            int64_t at = value - style->first;
            if (at < 0 || at >= int64_t(count))
                return false;
            textAppend(out, symbols[at]);
            return true;
        }
        case CssCounterSystem_Symbolic: {
            if (count == 0 || value < 1)
                return false;
            int64_t repeats = (value - 1) / int64_t(count) + 1;
            PrStringView symbol = symbols[(value - 1) % int64_t(count)];
            if (repeats * int64_t(codePointsOf(symbol)) > kCssCounterMaxCodePoints)
                return false;
            for (int64_t i = 0; i < repeats; ++i)
                textAppend(out, symbol);
            return true;
        }
        case CssCounterSystem_Alphabetic:
            if (count < 2 || value < 1)
                return false;
            while (value != 0) {
                --value;
                digits[places++] = uint32_t(value % count);
                value /= count;
            }
            break;
        case CssCounterSystem_Numeric:
            if (count < 2)
                return false;
            if (value == 0)
                digits[places++] = 0;
            while (value != 0) {
                digits[places++] = uint32_t(value % count);
                value /= count;
            }
            break;
        case CssCounterSystem_Hebrew: {
            // U+05F3 HEBREW PUNCTUATION GERESH, which the additive tuples of 1000 and above end with.
            int64_t thousands = value / 1000;
            int64_t rest = value % 1000;
            if (value < 1 || (thousands != 0 && !appendAdditiveBelow(out, style, thousands, 1000)))
                return false;
            if (thousands != 0)
                textAppend(out, "\xD7\xB3");
            return rest == 0 || appendAdditiveBelow(out, style, rest, 1000);
        }
        case CssCounterSystem_Additive: {
            if (value == 0) {
                for (uint32_t i = 0; i < style->additiveCount; ++i) {
                    if (style->additive[i].weight == 0) {
                        textAppend(out, style->additive[i].symbol);
                        return true;
                    }
                }
                return false;
            }
            uint32_t mark = out->text.count;
            uint32_t written = 0;
            for (uint32_t i = 0; i < style->additiveCount && value != 0; ++i) {
                const CssAdditiveTuple& tuple = style->additive[i];
                if (tuple.weight <= 0 || tuple.weight > value)
                    continue;
                int64_t repeats = value / tuple.weight;
                written += uint32_t(repeats > kCssCounterMaxCodePoints ? kCssCounterMaxCodePoints + 1 : repeats) *
                    codePointsOf(tuple.symbol);
                if (written > kCssCounterMaxCodePoints) {
                    out->text.count = mark;
                    return false;
                }
                for (int64_t k = 0; k < repeats; ++k)
                    textAppend(out, tuple.symbol);
                value -= repeats * tuple.weight;
            }
            if (value != 0)
                out->text.count = mark;
            return value == 0;
        }
        case CssCounterSystem_ChineseInformal:
        case CssCounterSystem_ChineseFormal:
        case CssCounterSystem_JapaneseInformal:
        case CssCounterSystem_JapaneseFormal:
        case CssCounterSystem_KoreanInformal:
        case CssCounterSystem_KoreanFormal:
            return appendCjk(out, style->system, symbols, count, value);
        case CssCounterSystem_Ethiopic:
            return appendEthiopic(out, symbols, count, value);
        default:
            return false;
    }
    uint32_t written = 0;
    for (uint32_t i = places; i > 0; --i)
        written += codePointsOf(symbols[digits[i - 1]]);
    if (written > kCssCounterMaxCodePoints)
        return false;
    for (uint32_t i = places; i > 0; --i)
        textAppend(out, symbols[digits[i - 1]]);
    return true;
}

// Appends the representation of `value` in `style` with its pad and negative sign, and returns true, or returns false
// and appends nothing if the style cannot represent the value.
bool appendRepresentation(TextBuffer* out, const CssCounterStyle* style, int32_t value, CssCounterFlow flow) {
    if (!inRange(style, value))
        return false;
    bool negative = value < 0 && usesNegativeSign(style);
    uint32_t mark = out->text.count;
    if (negative)
        textAppend(out, style->negative[0]);
    uint32_t start = out->text.count;
    if (!appendInitial(out, style, negative ? -int64_t(value) : int64_t(value), flow)) {
        out->text.count = mark;
        return false;
    }
    if (style->minimumLength != 0) {
        PrStringView text{out->text.data + start, out->text.count - start};
        uint32_t length = graphemesOf(text);
        if (negative)
            length += graphemesOf(style->negative[0]) + graphemesOf(style->negative[1]);
        uint32_t padding = style->minimumLength > length ? style->minimumLength - length : 0;
        if (padding * codePointsOf(style->padSymbol) + codePointsOf(text) > kCssCounterMaxCodePoints) {
            out->text.count = mark;
            return false;
        }
        // The pad goes between the negative sign and the representation.
        for (uint32_t i = 0; i < padding; ++i) {
            for (size_t b = style->padSymbol.size; b > 0; --b)
                out->text.insert(start, style->padSymbol.data[b - 1]);
        }
    }
    if (negative)
        textAppend(out, style->negative[1]);
    return true;
}

// Appends `value` in decimal, which represents every value.
void appendDecimal(TextBuffer* out, int32_t value) {
    if (value < 0)
        textAppendChar(out, '-');
    textAppendU64(out, value < 0 ? uint64_t(-int64_t(value)) : uint64_t(value));
}

// Builds the anonymous counter style that a computed symbols() defines (css-counter-styles-3/#symbols-function), with
// its symbols in `symbols`. Returns false if the function defines no style that the engine can represent with, such as
// one of images.
bool symbolsStyle(CssValueRun run, uint32_t at, Array<PrStringView>* symbols, CssCounterStyle* out) {
    *out = CssCounterStyle{
        .system = CssCounterSystem_Symbolic,
        .first = 1,
        .negative = {"-", {}},
        .suffix = " ",
        .fallback = "decimal",
    };
    CssValueRun arguments = cssValueChildren(run, at);
    for (uint32_t i = 0; i < arguments.count; i += arguments.values[i].next) {
        const CssValueNode* argument = &arguments.values[i];
        if (argument->kind == PrCssValueKind_Keyword) {
            switch (cssValueKeyword(argument)) {
                case PrCssKeyword_Cyclic: out->system = CssCounterSystem_Cyclic; break;
                case PrCssKeyword_Numeric: out->system = CssCounterSystem_Numeric; break;
                case PrCssKeyword_Alphabetic: out->system = CssCounterSystem_Alphabetic; break;
                case PrCssKeyword_Symbolic: out->system = CssCounterSystem_Symbolic; break;
                case PrCssKeyword_Fixed: out->system = CssCounterSystem_Fixed; break;
                default: break;
            }
        } else if (argument->kind == PrCssValueKind_String) {
            symbols->push(cssValueText(arguments, argument));
        } else {
            return false;
        }
    }
    out->symbols = symbols->data;
    out->symbolCount = symbols->count;
    return symbols->count != 0;
}

// Calls `fn` with the counter style that a computed <counter-style> names, among `styles` and the predefined styles, or
// with null for a name that no style has.
template <typename Fn>
void withStyle(const CssCounterStyles* PR_NULLABLE styles, CssValueRun counterStyle, Fn fn) {
    if (counterStyle.count != 0 && cssValueIsFunction(counterStyle.values, PrCssKeyword_Symbols)) {
        Array<PrStringView> symbols;
        CssCounterStyle style{};
        fn(symbolsStyle(counterStyle, 0, &symbols, &style) ? &style : nullptr);
        symbols.reset();
        return;
    }
    const CssCounterStyle* style = nullptr;
    if (counterStyle.count != 0 && counterStyle.values[0].kind == PrCssValueKind_Ident)
        style = cssCounterStyleFind(styles, cssValueText(counterStyle, counterStyle.values));
    fn(style);
}

// --- A document's counter styles ---

// The winning @counter-style rule of a name, and the state of its resolution.
struct Defined {
    PrStringView name;
    const CssRuleData* rule;
    uint8_t state;                  // 0 before its resolution, 1 while it resolves, 2 once resolved
    bool cycle;                     // true if it extends in a cycle, and so extends decimal
};

// Returns the value of the rule's descriptor, and sets `*declared`, or returns an empty run if the rule does not
// declare it.
CssValueRun descriptorOf(const CssRuleData* rule, PrCssDescriptor descriptor, bool* PR_NULLABLE declared = nullptr) {
    const CssDeclarationBlock* block = &rule->style;
    uint32_t at = cssDeclarationBlockFind(block, cssDescriptorDeclaredName(descriptor));
    if (declared != nullptr)
        *declared = at != block->count;
    return at != block->count ? cssDeclarationValue(block, &cssDeclarations(block)[at]) : CssValueRun{};
}

// Returns the name that the value at `node` gives: an identifier, or a keyword's name.
PrStringView nameOf(CssValueRun run, const CssValueNode* node) {
    if (node->kind == PrCssValueKind_Keyword)
        return cssKeywordName(cssValueKeyword(node));
    return node->kind == PrCssValueKind_Ident ? cssValueText(run, node) : PrStringView{};
}

// Sets `*out` to the text of the <symbol> at `node`, a string or an identifier, and returns true. Returns false for an
// image, which the engine does not draw in a counter.
bool symbolOf(CssValueRun run, const CssValueNode* node, PrStringView* out) {
    if (node->kind == PrCssValueKind_String) {
        *out = cssValueText(run, node);
        return true;
    }
    *out = nameOf(run, node);
    return node->kind == PrCssValueKind_Ident || node->kind == PrCssValueKind_Keyword;
}

// Sets `*out` to the integer that the value at `node` of `run` gives, clamped to the range of a counter's value, and
// returns true, or returns false if it gives none. The value is an integer, or a calculation, which rounds to the
// nearest integer (css-values-4/#calc-range). A rule has no element, so the lengths in a calculation resolve as
// absolute lengths, and as lengths relative to the initial font, whose size is `em`
// (css-values-4/#font-relative-lengths assumes an x-height and a character advance of half of it).
bool integerOf(CssValueRun run, const CssValueNode* node, double em, int32_t* out) {
    double number = node->number;
    if (node->kind == PrCssValueKind_Math) {
        // The calculation with its lengths in px simplifies again, which evaluates what its lengths kept from
        // simplifying when it was parsed, such as sign() of a length in em.
        CssValuePool pool{};
        CssValueSpan span = cssValuePoolAddRun(&pool, cssValueSubtree(run, uint32_t(node - run.values)));
        bool resolved = true;
        for (uint32_t i = span.start; i < span.start + span.count && resolved; ++i) {
            CssValueNode& leaf = pool.values.data[i];
            if (leaf.kind != PrCssValueKind_Dimension || cssUnitCategory(leaf.unit) != CssUnitCategory_Length)
                continue;
            double scale = cssUnitScale(leaf.unit);
            switch (leaf.unit) {
                case PrCssUnit_Em:
                case PrCssUnit_Rem:
                case PrCssUnit_Ic:
                case PrCssUnit_Ric: scale = em; break;
                case PrCssUnit_Ex:
                case PrCssUnit_Rex:
                case PrCssUnit_Ch:
                case PrCssUnit_Rch: scale = em / 2; break;
                default: break;
            }
            resolved = scale != 0;
            leaf = cssNumberValue(PrCssValueKind_Dimension, PrCssUnit_Px, leaf.number * scale);
        }
        CssMathScratch scratch{};
        CssMathSlot slot{.category = CssUnitCategory_Number, .percentages = CssUnitCategory_Unknown};
        resolved = resolved && cssMathAdopt(&pool, span.start, slot, &scratch) &&
            cssMathTo(cssValuePoolRun(&pool, CssValueSpan{span.start, pool.values.count - span.start}), 0,
                PrCssUnit_Number, &number);
        cssMathScratchReset(&scratch);
        cssValuePoolReset(&pool);
        if (!resolved || isNanF64(number))
            return false;
        number = roundF64(number);
    } else if (node->kind != PrCssValueKind_Integer) {
        return false;
    }
    *out = number < INT32_MIN ? INT32_MIN : number > INT32_MAX ? INT32_MAX : int32_t(number);
    return true;
}

// Returns true if no @counter-style rule may define `name`: decimal and the predefined symbolic styles
// (css-counter-styles-3/#the-counter-style-rule), and none.
bool nonOverridable(PrStringView name) {
    const PrStringView fixed[] = {
        "decimal", "disc", "square", "circle", "disclosure-open", "disclosure-closed",
        "none",
    };
    for (PrStringView other : fixed) {
        if (asciiEqualsFold(name, other))
            return true;
    }
    return false;
}

// Appends the symbols of a symbols descriptor to `out`. Returns false if one is an image.
bool symbolsOf(CssValueRun run, Array<PrStringView>* out) {
    for (uint32_t i = 0; i < run.count; i += run.values[i].next) {
        PrStringView symbol{};
        if (!symbolOf(run, &run.values[i], &symbol))
            return false;
        out->push(symbol);
    }
    return true;
}

// Appends the tuples of an additive-symbols descriptor to `out`. Returns false if one names an image, or if the weights
// do not strictly decrease, which makes the descriptor invalid (css-counter-styles-3/#counter-style-symbols).
bool additiveOf(CssValueRun run, double em, Array<CssAdditiveTuple>* out) {
    CssAdditiveTuple tuple{.weight = -1};
    bool symbol = false;
    auto close = [&]() {
        if (tuple.weight < 0 || !symbol || (out->count != 0 && tuple.weight >= out->last().weight))
            return false;
        out->push(tuple);
        tuple = CssAdditiveTuple{.weight = -1};
        symbol = false;
        return true;
    };
    for (uint32_t i = 0; i < run.count; i += run.values[i].next) {
        const CssValueNode* node = &run.values[i];
        if (cssValueIsLiteral(node, ',')) {
            if (!close())
                return false;
        } else if (integerOf(run, node, em, &tuple.weight)) {
        } else if (symbolOf(run, node, &tuple.symbol)) {
            symbol = true;
        } else {
            return false;
        }
    }
    return close();
}

// Reads a system descriptor: its system, a fixed system's first value, and the name an extends system extends, which it
// sets in `*extended`. Returns false for extends.
bool systemOf(CssValueRun run, double em, CssCounterStyle* style, PrStringView* extended) {
    style->system = CssCounterSystem_Symbolic;
    style->first = 1;
    for (uint32_t i = 0; i < run.count; i += run.values[i].next) {
        const CssValueNode* node = &run.values[i];
        if (integerOf(run, node, em, &style->first))
            continue;
        if (node->kind != PrCssValueKind_Keyword) {
            *extended = nameOf(run, node);
            continue;
        }
        switch (cssValueKeyword(node)) {
            case PrCssKeyword_Cyclic: style->system = CssCounterSystem_Cyclic; break;
            case PrCssKeyword_Numeric: style->system = CssCounterSystem_Numeric; break;
            case PrCssKeyword_Alphabetic: style->system = CssCounterSystem_Alphabetic; break;
            case PrCssKeyword_Additive: style->system = CssCounterSystem_Additive; break;
            case PrCssKeyword_Fixed: style->system = CssCounterSystem_Fixed; break;
            case PrCssKeyword_Extends: *extended = ""; break;
            case PrCssKeyword_Symbolic: break;
            default: *extended = nameOf(run, node); break;
        }
    }
    return extended->data == nullptr;
}

// The values of the descriptors of a @counter-style rule that decide whether it defines a style.
struct Algorithm {
    CssValueRun system;
    CssValueRun symbols;
    CssValueRun additive;
    bool hasSymbols;
    bool hasAdditive;
};

// Returns the values of the rule's descriptors that decide whether it defines a style.
Algorithm algorithmOf(const CssRuleData* rule) {
    Algorithm a{};
    a.system = descriptorOf(rule, PrCssDescriptor_CounterStyleSystem);
    a.symbols = descriptorOf(rule, PrCssDescriptor_CounterStyleSymbols, &a.hasSymbols);
    a.additive = descriptorOf(rule, PrCssDescriptor_CounterStyleAdditiveSymbols, &a.hasAdditive);
    return a;
}

// Returns true if descriptors with the values `a` define a counter style: an extends system with no symbols of its own,
// or a system with enough symbols for its algorithm (css-counter-styles-3/#counter-style-system), none of them an
// image.
bool definesStyle(const Algorithm& a, double em) {
    CssCounterStyle style{};
    PrStringView extended{};
    bool hasSymbols = a.hasSymbols;
    bool hasAdditive = a.hasAdditive;
    CssValueRun symbols = a.symbols;
    CssValueRun additive = a.additive;
    if (!systemOf(a.system, em, &style, &extended))
        return !hasSymbols && !hasAdditive;
    Array<PrStringView> listed;
    Array<CssAdditiveTuple> tuples;
    bool valid;
    if (style.system == CssCounterSystem_Additive)
        valid = additiveOf(additive, em, &tuples) && tuples.count != 0;
    else
        valid = symbolsOf(symbols, &listed);
    uint32_t least = style.system == CssCounterSystem_Alphabetic || style.system == CssCounterSystem_Numeric ? 2 : 1;
    valid = valid && (style.system == CssCounterSystem_Additive || listed.count >= least);
    listed.reset();
    tuples.reset();
    return valid;
}

// Sets the descriptors of the rule other than its system and symbols in `style`: negative, prefix, suffix, range, pad
// and fallback. A descriptor that the rule leaves out, or that names an image, keeps its value. Its range keeps its
// bounds in `storage`.
void applyDescriptors(const CssRuleData* rule, double em, CssCounterStyle* style, CssCounterStyleStorage* storage) {
    CssValueRun run = descriptorOf(rule, PrCssDescriptor_CounterStyleNegative);
    PrStringView negative[2] = {};
    uint32_t count = 0;
    bool symbols = true;
    for (uint32_t i = 0; i < run.count && count < 2; i += run.values[i].next)
        symbols = symbolOf(run, &run.values[i], &negative[count++]) && symbols;
    if (count != 0 && symbols) {
        style->negative[0] = negative[0];
        style->negative[1] = negative[1];
    }
    const PrCssDescriptor affixes[] = {PrCssDescriptor_CounterStylePrefix, PrCssDescriptor_CounterStyleSuffix};
    for (PrCssDescriptor descriptor : affixes) {
        run = descriptorOf(rule, descriptor);
        PrStringView affix{};
        if (run.count != 0 && symbolOf(run, run.values, &affix))
            (descriptor == PrCssDescriptor_CounterStylePrefix ? style->prefix : style->suffix) = affix;
    }
    // A range whose lower bound is above its upper bound makes the whole descriptor invalid.
    run = descriptorOf(rule, PrCssDescriptor_CounterStyleRange);
    if (run.count != 0 && cssValueIsKeyword(run.values, PrCssKeyword_Auto)) {
        style->ranges = nullptr;
        style->rangeCount = 0;
    } else if (run.count != 0) {
        storage->ranges.clear();
        int64_t bounds[2] = {};
        uint32_t bound = 0;
        bool valid = true;
        for (uint32_t i = 0; i <= run.count && valid; i = i < run.count ? i + run.values[i].next : i + 1) {
            if (i == run.count || cssValueIsLiteral(&run.values[i], ',')) {
                valid = bound == 2 && bounds[0] <= bounds[1];
                if (valid)
                    storage->ranges.push(CssCounterRange{int32_t(bounds[0]), int32_t(bounds[1])});
                bound = 0;
            } else if (bound < 2) {
                int32_t value = 0;
                bool finite = integerOf(run, &run.values[i], em, &value);
                bounds[bound] = finite ? value : bound == 0 ? INT32_MIN : INT32_MAX;
                ++bound;
            }
        }
        if (valid) {
            style->ranges = storage->ranges.data;
            style->rangeCount = storage->ranges.count;
        }
    }
    run = descriptorOf(rule, PrCssDescriptor_CounterStylePad);
    PrStringView pad{};
    int32_t length = -1;
    symbols = true;
    for (uint32_t i = 0; i < run.count; i += run.values[i].next) {
        if (!integerOf(run, &run.values[i], em, &length))
            symbols = symbolOf(run, &run.values[i], &pad) && symbols;
    }
    if (length >= 0 && symbols) {
        style->minimumLength = uint32_t(length);
        style->padSymbol = pad;
    }
    run = descriptorOf(rule, PrCssDescriptor_CounterStyleFallback);
    if (run.count != 0 && nameOf(run, run.values).size != 0)
        style->fallback = nameOf(run, run.values);
}

// Returns the index of the style among `defined` that `name` names, or kArrayMissing. A name that matches a predefined
// style ASCII case-insensitively names it by the predefined style's own name.
uint32_t definedNamed(const Array<Defined>& defined, PrStringView name) {
    for (uint32_t i = 0; i < defined.count; ++i) {
        if (defined.data[i].name == name)
            return i;
    }
    const CssCounterStyle* predefined = cssCounterStylePredefined(name);
    if (predefined == nullptr || predefined->name == name)
        return kArrayMissing;
    for (uint32_t i = 0; i < defined.count; ++i) {
        if (defined.data[i].name == predefined->name)
            return i;
    }
    return kArrayMissing;
}

// Resolves the style at `at` into `out`, and the styles it extends before it. `stack` holds the styles resolving, of
// which those from the one that extends a style already resolving form a cycle.
void resolveDefined(Array<Defined>& defined, uint32_t at, double em, CssCounterStyles* out, Array<uint32_t>& stack) {
    Defined& d = defined.data[at];
    if (d.state == 2)
        return;
    d.state = 1;
    stack.push(at);
    CssCounterStyle& style = out->styles.data[at];
    CssCounterStyleStorage& storage = out->storage.data[at];
    PrStringView extended{};
    CssCounterStyle own{};
    if (systemOf(descriptorOf(d.rule, PrCssDescriptor_CounterStyleSystem), em, &own, &extended)) {
        style = CssCounterStyle{
            .system = own.system,
            .first = own.first,
            .negative = {"-", {}},
            .suffix = ". ",
            .fallback = "decimal",
        };
        if (own.system == CssCounterSystem_Additive) {
            additiveOf(descriptorOf(d.rule, PrCssDescriptor_CounterStyleAdditiveSymbols), em, &storage.additive);
            style.additive = storage.additive.data;
            style.additiveCount = storage.additive.count;
        } else {
            symbolsOf(descriptorOf(d.rule, PrCssDescriptor_CounterStyleSymbols), &storage.symbols);
            style.symbols = storage.symbols.data;
            style.symbolCount = storage.symbols.count;
        }
    } else {
        // A style that extends one that no rule defines and that is not predefined extends decimal.
        uint32_t base = definedNamed(defined, extended);
        if (base != kArrayMissing && defined.data[base].state == 1) {
            for (uint32_t k = stack.count; k-- > 0 && stack.data[k] != base;)
                defined.data[stack.data[k]].cycle = true;
            defined.data[base].cycle = true;
        } else if (base != kArrayMissing) {
            resolveDefined(defined, base, em, out, stack);
        }
        const CssCounterStyle* from = base != kArrayMissing ? &out->styles.data[base] : predefinedStyle(extended);
        // The styles of a cycle extend decimal. A style that extends one of them takes it as resolved.
        if (d.cycle || from == nullptr)
            from = predefinedStyle("decimal");
        style = *from;
    }
    applyDescriptors(d.rule, em, &style, &storage);
    storage.name = prStringCopy(d.name);
    style.name = prStringView(&storage.name);
    stack.pop();
    d.state = 2;
}

} // namespace

void cssCounterStylesBuild(CssCounterStyles* out, const CssRuleData* const PR_NONNULL* PR_NULLABLE rules,
        uint32_t count, double em) {
    cssCounterStylesReset(out);
    // Of the rules of a name, the last that defines a style wins, in the place of the name's first rule.
    Array<Defined> defined;
    for (uint32_t i = 0; i < count; ++i) {
        const CssRuleData* rule = rules[i];
        CssValueRun prelude = cssValueListRun(&rule->prelude);
        PrStringView name = prelude.count != 0 ? nameOf(prelude, prelude.values) : PrStringView{};
        if (name.size == 0 || nonOverridable(name) || !definesStyle(algorithmOf(rule), em))
            continue;
        uint32_t at = 0;
        while (at < defined.count && defined.data[at].name != name)
            ++at;
        if (at == defined.count)
            defined.push(Defined{.name = name});
        defined.data[at].rule = rule;
    }
    for (uint32_t i = 0; i < defined.count; ++i) {
        out->styles.emplace() = CssCounterStyle{};
        out->storage.emplace() = CssCounterStyleStorage{};
    }
    Array<uint32_t> stack;
    for (uint32_t i = 0; i < defined.count; ++i)
        resolveDefined(defined, i, em, out, stack);
    stack.reset();
    defined.reset();
}

bool cssCounterStyleNameDefinable(PrStringView name) {
    return !nonOverridable(name);
}

bool cssCounterStyleRuleAccepts(const CssRuleData* rule, PrCssDescriptor descriptor, CssValueRun value) {
    Algorithm a = algorithmOf(rule);
    CssValueRun system = a.system;
    if (descriptor == PrCssDescriptor_CounterStyleSystem) {
        a.system = value;
    } else if (descriptor == PrCssDescriptor_CounterStyleSymbols) {
        a.symbols = value;
        a.hasSymbols = true;
    } else if (descriptor == PrCssDescriptor_CounterStyleAdditiveSymbols) {
        a.additive = value;
        a.hasAdditive = true;
    }
    // A descriptor's calculation resolves against the default initial font here, as no document's is at hand.
    if (!definesStyle(a, 16))
        return false;
    if (descriptor != PrCssDescriptor_CounterStyleSystem)
        return true;
    // A system may change an aspect of its algorithm, such as a fixed system's first value, or what it extends.
    CssCounterStyle before{};
    CssCounterStyle after{};
    PrStringView extendedBefore{};
    PrStringView extendedAfter{};
    bool ownBefore = systemOf(system, 16, &before, &extendedBefore);
    bool ownAfter = systemOf(value, 16, &after, &extendedAfter);
    return ownBefore == ownAfter && (!ownBefore || before.system == after.system);
}

void cssCounterStylesReset(CssCounterStyles* styles) {
    for (uint32_t i = 0; i < styles->storage.count; ++i) {
        CssCounterStyleStorage& storage = styles->storage.data[i];
        prStringReset(&storage.name);
        storage.symbols.reset();
        storage.additive.reset();
        storage.ranges.reset();
    }
    styles->storage.reset();
    styles->styles.reset();
}

const CssCounterStyle* PR_NULLABLE cssCounterStyleFind(const CssCounterStyles* PR_NULLABLE styles, PrStringView name) {
    if (styles != nullptr) {
        for (uint32_t i = 0; i < styles->styles.count; ++i) {
            if (styles->styles.data[i].name == name)
                return &styles->styles.data[i];
        }
    }
    const CssCounterStyle* predefined = predefinedStyle(name);
    // A name that matches a predefined style is lowercased, so that it names a document's style of that name.
    if (styles != nullptr && predefined != nullptr && predefined->name != name) {
        for (uint32_t i = 0; i < styles->styles.count; ++i) {
            if (styles->styles.data[i].name == predefined->name)
                return &styles->styles.data[i];
        }
    }
    return predefined;
}

void cssCounterRepresent(TextBuffer* out, const CssCounterStyles* PR_NULLABLE styles, const CssCounterStyle* style,
        int32_t value, CssCounterFlow flow) {
    for (uint32_t hop = 0; hop <= kMaxFallbacks; ++hop) {
        if (appendRepresentation(out, style, value, flow))
            return;
        const CssCounterStyle* fallback = cssCounterStyleFind(styles, style->fallback);
        if (fallback == nullptr || fallback == style)
            break;
        style = fallback;
    }
    appendDecimal(out, value);
}

void cssCounterAppend(TextBuffer* out, const CssCounterStyles* PR_NULLABLE styles, CssValueRun counterStyle,
        int32_t value, CssCounterFlow flow) {
    withStyle(styles, counterStyle, [&](const CssCounterStyle* PR_NULLABLE style) {
        if (style != nullptr)
            cssCounterRepresent(out, styles, style, value, flow);
        else
            appendDecimal(out, value);
    });
}

bool cssCounterAppendMarker(TextBuffer* out, const CssCounterStyles* PR_NULLABLE styles, CssValueRun listStyleType,
        int32_t value, CssCounterFlow flow) {
    if (listStyleType.count == 0 || cssValueIsKeyword(listStyleType.values, PrCssKeyword_None))
        return false;
    if (listStyleType.values[0].kind == PrCssValueKind_String) {
        textAppend(out, cssValueText(listStyleType, listStyleType.values));
        return true;
    }
    // An unknown style is decimal, with decimal's prefix and suffix.
    withStyle(styles, listStyleType, [&](const CssCounterStyle* PR_NULLABLE style) {
        if (style == nullptr)
            style = cssCounterStylePredefined("decimal");
        textAppend(out, style->prefix);
        cssCounterRepresent(out, styles, style, value, flow);
        textAppend(out, style->suffix);
    });
    return true;
}

} // namespace pr_impl

PR_ASSUME_NONNULL_END
