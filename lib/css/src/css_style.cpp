// Computed Style
//
// css-cascade-5 - https://drafts.csswg.org/css-cascade-5/
// css-values-4 - https://drafts.csswg.org/css-values-4/
// css-fonts-4 - https://drafts.csswg.org/css-fonts-4/
// css-color-4 - https://drafts.csswg.org/css-color-4/
// css-backgrounds-3 - https://drafts.csswg.org/css-backgrounds-3/
// css-inline-3 - https://drafts.csswg.org/css-inline-3/

#include "css/src/css_style.h"

#include "pr/alloc.h"
#include "pr/atomic.h"
#include "pr/bit_cast.h"
#include "pr/color_space.h"
#include "pr/count_of.h"
#include "pr/float.h"
#include "pr/hash.h"
#include "pr/mem.h"
#include "pr/memory.h"
#include "pr/search.h"
#include "pr/string_ops.h"
#include "pr/text_buffer.h"

#include "css/src/css_color.h"
#include "css/src/css_color_value.h"
#include "css/src/css_math.h"
#include "css/src/css_path.h"
#include "css/src/css_rule.h"
#include "css/src/css_shape.h"
#include "css/src/css_unit.h"

namespace pr_impl {

namespace {

// No value or offset of a <position>'s axis.
constexpr uint32_t kCssPositionNone = 0xffffffffu;

// The seed for hashing a group's values.
constexpr uint64_t kStyleSeed = 0x6a09e667f3bcc909ull;

// The sizes of the absolute-size keywords from xx-small to xxx-large (css-fonts-4/#absolute-size-mapping).
constexpr double kAbsoluteSizes[] = {9, 10, 13, 16, 18, 24, 32, 48};

// The ratio between adjacent font sizes that `larger` and `smaller` step by (css-fonts-4/#typedef-relative-size).
constexpr double kRelativeSizeRatio = 1.2;

// The widths of thin, medium and thick borders (css-backgrounds-3/#typedef-line-width).
constexpr float kThinBorder = 1;
constexpr float kMediumBorder = 3;
constexpr float kThickBorder = 5;

CssValueNode pxValue(double px) {
    return cssNumberValue(PrCssValueKind_Dimension, PrCssUnit_Px, px);
}

// Writes the length in px of `number` in `unit`, which is a length unit. `context` resolves relative units. Returns
// false for a unit that the context does not resolve, such as a container unit. A unit that it resolves may have a
// scale of zero, such as `lh` on lines of no height.
bool absoluteLength(double number, PrCssUnit unit, const CssStyleContext& context, double* out) {
    const CssFontBases& font = context.font;
    const CssFontBases& root = context.root;
    double vw = double(context.viewport.width) / 100;
    double vh = double(context.viewport.height) / 100;
    double scale = 0;
    switch (unit) {
        case PrCssUnit_Em: scale = font.em; break;
        case PrCssUnit_Ex: scale = cssFontMetricPx(context, false, CssFontMetric_Ex); break;
        case PrCssUnit_Cap: scale = cssFontMetricPx(context, false, CssFontMetric_Cap); break;
        case PrCssUnit_Ch: scale = cssFontMetricPx(context, false, CssFontMetric_Ch); break;
        case PrCssUnit_Ic: scale = cssFontMetricPx(context, false, CssFontMetric_Ic); break;
        case PrCssUnit_Lh: scale = font.lh; break;
        case PrCssUnit_Rem: scale = root.em; break;
        case PrCssUnit_Rex: scale = cssFontMetricPx(context, true, CssFontMetric_Ex); break;
        case PrCssUnit_Rcap: scale = cssFontMetricPx(context, true, CssFontMetric_Cap); break;
        case PrCssUnit_Rch: scale = cssFontMetricPx(context, true, CssFontMetric_Ch); break;
        case PrCssUnit_Ric: scale = cssFontMetricPx(context, true, CssFontMetric_Ic); break;
        case PrCssUnit_Rlh: scale = root.lh; break;
        case PrCssUnit_Vw:
        case PrCssUnit_Svw:
        case PrCssUnit_Lvw:
        case PrCssUnit_Dvw:
        case PrCssUnit_Vi:
        case PrCssUnit_Svi:
        case PrCssUnit_Lvi:
        case PrCssUnit_Dvi: scale = vw; break;
        case PrCssUnit_Vh:
        case PrCssUnit_Svh:
        case PrCssUnit_Lvh:
        case PrCssUnit_Dvh:
        case PrCssUnit_Vb:
        case PrCssUnit_Svb:
        case PrCssUnit_Lvb:
        case PrCssUnit_Dvb: scale = vh; break;
        case PrCssUnit_Vmin:
        case PrCssUnit_Svmin:
        case PrCssUnit_Lvmin:
        case PrCssUnit_Dvmin: scale = minF64(vw, vh); break;
        case PrCssUnit_Vmax:
        case PrCssUnit_Svmax:
        case PrCssUnit_Lvmax:
        case PrCssUnit_Dvmax: scale = maxF64(vw, vh); break;
        default:
            scale = cssUnitScale(unit);
            // An absolute unit has a scale. Any other unit is one that the context does not resolve.
            if (scale == 0 && number != 0)
                return false;
            break;
    }
    if (context.readsViewport != nullptr && cssUnitViewportRelative(unit))
        *context.readsViewport = true;
    bool fontRelative = unit == PrCssUnit_Em || unit == PrCssUnit_Ex || unit == PrCssUnit_Cap || unit == PrCssUnit_Ch
                        || unit == PrCssUnit_Ic || unit == PrCssUnit_Lh;
    if (context.readsFont != nullptr && fontRelative)
        *context.readsFont = true;
    *out = number * scale;
    return true;
}

bool isLength(const CssValueNode* value) {
    return value->kind == PrCssValueKind_Dimension && cssUnitCategory(value->unit) == CssUnitCategory_Length;
}

// Adds a copy of the values of `run` from `begin` to `end`, with their lengths in px. Each calculation that holds only
// lengths is folded into one length. A folded calculation takes fewer values, so each container that holds one is given
// the size of its copy.
void addAbsoluteRange(CssValuePool* pool, CssValueRun run, uint32_t begin, uint32_t end,
                      const CssStyleContext& context) {
    for (uint32_t i = begin; i < end;) {
        const CssValueNode* value = &run.values[i];
        if (value->kind == PrCssValueKind_Math) {
            // A calculation whose lengths all resolve, with nothing relative to a percentage, folds into a length.
            CssValuePool scratch{};
            CssValueSpan span = cssValuePoolAddRun(&scratch, cssValueSubtree(run, i));
            bool resolved = true;
            for (uint32_t k = span.start; k < span.start + span.count; ++k) {
                CssValueNode& leaf = scratch.values.data[k];
                double px = 0;
                if (isLength(&leaf)) {
                    if (absoluteLength(leaf.number, leaf.unit, context, &px))
                        leaf = pxValue(px);
                    else
                        resolved = false;
                }
            }
            // A calculation of a percentage alone folds into that percentage (css-values-4/#calc-serialize).
            double px = 0;
            double percent = 0;
            CssValueRun copy = cssValuePoolRun(&scratch, span);
            if (resolved && cssMathTo(copy, 0, PrCssUnit_Px, &px))
                cssValuePoolAdd(pool, pxValue(px));
            else if (copy.count == 2 && copy.values[1].kind == PrCssValueKind_Percentage
                     && cssMathTo(copy, 0, PrCssUnit_Percent, &percent))
                cssValuePoolAdd(pool, cssNumberValue(PrCssValueKind_Percentage, PrCssUnit_Number, percent));
            else
                cssValuePoolAddRun(pool, copy);
            cssValuePoolReset(&scratch);
            i += value->next;
            continue;
        }
        double px = 0;
        if (isLength(value) && absoluteLength(value->number, value->unit, context, &px)) {
            cssValuePoolAdd(pool, pxValue(px));
            ++i;
            continue;
        }
        uint32_t at = pool->values.count;
        CssValueRun one{value, 1, run.text};
        cssValuePoolAddRun(pool, one);
        if (value->next > 1) {
            addAbsoluteRange(pool, run, i + 1, i + value->next, context);
            pool->values.data[at].next = pool->values.count - at;
        }
        i += value->next > 1 ? value->next : 1;
    }
}

// Adds a copy of `run` with its lengths in px (addAbsoluteRange).
void addAbsolute(CssValuePool* pool, CssValueRun run, const CssStyleContext& context) {
    addAbsoluteRange(pool, run, 0, run.count, context);
}

// Returns the computed font size (css-fonts-4/#font-size-prop). The size is absolute, or relative to the parent's font
// size for a relative keyword, an em or a percentage. While font-size computes, `context.font` holds the parent's
// bases.
double computeFontSize(CssValueRun run, const CssStyleContext& context) {
    double parent = context.font.em;
    switch (cssValueRunKeyword(run)) {
        case PrCssKeyword_XxSmall: return kAbsoluteSizes[0];
        case PrCssKeyword_XSmall: return kAbsoluteSizes[1];
        case PrCssKeyword_Small: return kAbsoluteSizes[2];
        case PrCssKeyword_Medium: return kAbsoluteSizes[3];
        case PrCssKeyword_Large: return kAbsoluteSizes[4];
        case PrCssKeyword_XLarge: return kAbsoluteSizes[5];
        case PrCssKeyword_XxLarge: return kAbsoluteSizes[6];
        case PrCssKeyword_XxxLarge: return kAbsoluteSizes[7];
        case PrCssKeyword_Larger: return parent * kRelativeSizeRatio;
        case PrCssKeyword_Smaller: return parent / kRelativeSizeRatio;
        default: break;
    }
    CssValuePool pool{};
    addAbsolute(&pool, run, context);
    double px = parent;
    if (!cssResolveLength(cssValuePoolRun(&pool, CssValueSpan{0, pool.values.count}), 0, parent, &px))
        px = parent;
    cssValuePoolReset(&pool);
    return maxF64(0, px);
}

// The width that font-width's `normal` names, in percent.
constexpr double kNormalFontWidth = 100;

// Returns the computed font-width (css-fonts-4/#font-width-prop). A keyword computes to the percentage that it names,
// and a percentage computes to itself.
double computeFontWidth(CssValueRun run) {
    if (run.count != 1)
        return kNormalFontWidth;
    if (run.values[0].kind == PrCssValueKind_Percentage)
        return run.values[0].number;
    return cssFontWidthOfKeyword(cssValueRunKeyword(run));
}

// Returns the computed font weight (css-fonts-4/#font-weight-prop) as a number. bolder and lighter step from the
// parent's weight.
float computeFontWeight(CssValueRun run, const CssStyleContext& context) {
    float parent = context.parentWeight;
    switch (cssValueRunKeyword(run)) {
        case PrCssKeyword_Normal: return 400;
        case PrCssKeyword_Bold: return 700;
        case PrCssKeyword_Bolder: return parent < 350 ? 400 : parent < 550 ? 700 : maxF32(parent, 900);
        case PrCssKeyword_Lighter: return parent < 550 ? minF32(parent, 100) : parent < 750 ? 400 : 700;
        default: break;
    }
    if (run.count != 0 && (run.values[0].kind == PrCssValueKind_Number || run.values[0].kind == PrCssValueKind_Integer))
        return float(run.values[0].number);
    double number = 0;
    if (run.count != 0 && run.values[0].kind == PrCssValueKind_Math && cssMathTo(run, 0, PrCssUnit_Number, &number))
        return float(number);
    return parent;
}

// Returns the computed integer of a z-index other than auto (css-position-3/#stacking). A calculation is rounded to the
// nearest integer, with a tie rounded toward positive infinity (css-values-4/#calc-type-checking). A value outside the
// range of 32 bits is clamped into it (css-values-4/#numeric-ranges).
double computeZIndex(CssValueRun specified) {
    double number = 0;
    if (specified.count != 0 && specified.values[0].kind == PrCssValueKind_Math)
        cssMathTo(specified, 0, PrCssUnit_Number, &number);
    else if (specified.count != 0)
        number = specified.values[0].number;
    if (isNanF64(number))
        return 0;
    return minF64(maxF64(floorF64(number + 0.5), double(INT32_MIN)), double(INT32_MAX));
}

// Appends the computed line height (css-inline-3/#line-height-property). normal computes to itself. A number, or a
// calculation of numbers, computes to the number. A length or percentage, or a calculation of them, computes to the
// length, and a percentage is relative to the element's own font size. Numbers and lengths are clamped to zero
// (css-values-4/#calc-range).
void computeLineHeight(CssValuePool* pool, CssValueRun specified, const CssStyleContext& context) {
    const CssValueNode& value = specified.values[0];
    double number = value.number;
    if (value.kind == PrCssValueKind_Number || value.kind == PrCssValueKind_Integer
        || (value.kind == PrCssValueKind_Math && cssMathTo(specified, 0, PrCssUnit_Number, &number))) {
        cssValuePoolAdd(pool, cssNumberValue(PrCssValueKind_Number, PrCssUnit_Number, maxF64(0, number)));
        return;
    }
    if (value.kind == PrCssValueKind_Keyword) {
        cssValuePoolAddRun(pool, specified);
        return;
    }
    CssValuePool absolute{};
    addAbsolute(&absolute, specified, context);
    double px = 0;
    if (cssResolveLength(cssValuePoolRun(&absolute, CssValueSpan{0, absolute.values.count}), 0, context.font.em, &px))
        cssValuePoolAdd(pool, pxValue(maxF64(0, px)));
    else
        cssValuePoolAddRun(pool, cssValuePoolRun(&absolute, CssValueSpan{0, absolute.values.count}));
    cssValuePoolReset(&absolute);
}

// Adds one axis of a computed <position>: `value`, a length or percentage measured from the start edge, or an edge
// keyword with the offset `offset` from that edge, or none. A keyword's position computes to a length-percentage from
// the start edge: a near edge's offset, 50% for center, and a far edge's offset taken from 100%
// (css-values-4/#position).
void addPositionAxis(CssValuePool* pool, CssValueRun run, uint32_t value, uint32_t offset) {
    const CssValueNode* node = &run.values[value];
    if (node->kind != PrCssValueKind_Keyword) {
        cssValuePoolAddRun(pool, cssValueSubtree(run, value));
        return;
    }
    PrCssKeyword keyword = cssValueKeyword(node);
    bool far = keyword == PrCssKeyword_Right || keyword == PrCssKeyword_Bottom;
    if (keyword == PrCssKeyword_Center || offset == kCssPositionNone) {
        double percent = keyword == PrCssKeyword_Center ? 50 : far ? 100 : 0;
        cssValuePoolAdd(pool, cssNumberValue(PrCssValueKind_Percentage, PrCssUnit_Number, percent));
        return;
    }
    const CssValueNode* by = &run.values[offset];
    if (!far) {
        cssValuePoolAddRun(pool, cssValueSubtree(run, offset));
        return;
    }
    if (by->kind == PrCssValueKind_Percentage) {
        cssValuePoolAdd(pool, cssNumberValue(PrCssValueKind_Percentage, PrCssUnit_Number, 100 - by->number));
        return;
    }
    // calc(100% - offset): a sum of 100% and the negated offset, whose own calculation it takes in place of its math
    // function.
    CssValueNode math = {};
    math.kind = PrCssValueKind_Math;
    math.aux = CssValueAuxFlags(CssUnitCategory_Length);
    uint32_t root = cssValuePoolAdd(pool, math);
    CssValueNode sum = {};
    sum.kind = PrCssValueKind_Calc;
    sum.aux = PrCssCalc_Sum;
    uint32_t sumAt = cssValuePoolAdd(pool, sum);
    cssValuePoolAdd(pool, cssNumberValue(PrCssValueKind_Percentage, PrCssUnit_Number, 100));
    CssValueNode negate = {};
    negate.kind = PrCssValueKind_Calc;
    negate.aux = PrCssCalc_Negate;
    uint32_t negateAt = cssValuePoolAdd(pool, negate);
    CssValueRun operand = cssValueSubtree(run, offset);
    if (by->kind == PrCssValueKind_Math)
        operand = cssValueChildren(run, offset);
    cssValuePoolAddRun(pool, operand);
    pool->values.data[negateAt].next = pool->values.count - negateAt;
    pool->values.data[sumAt].next = pool->values.count - sumAt;
    pool->values.data[root].next = pool->values.count - root;
}

// Adds a computed <position>, written as one or two values or as edge keywords with offsets, as its horizontal and its
// vertical length-percentage from the top left (css-values-4/#position).
void addPosition(CssValuePool* pool, CssValueRun run) {
    uint32_t items[4];
    uint32_t count = 0;
    for (uint32_t i = 0; i < run.count && count < 4; i += run.values[i].next)
        items[count++] = i;
    auto keyword = [&](uint32_t item) {
        const CssValueNode* value = &run.values[items[item]];
        return value->kind == PrCssValueKind_Keyword ? cssValueKeyword(value) : PrCssKeyword_Count;
    };
    auto vertical = [&](uint32_t item) {
        return keyword(item) == PrCssKeyword_Top || keyword(item) == PrCssKeyword_Bottom;
    };
    auto horizontal = [&](uint32_t item) {
        return keyword(item) == PrCssKeyword_Left || keyword(item) == PrCssKeyword_Right;
    };
    uint32_t value[2] = {kCssPositionNone, kCssPositionNone};
    uint32_t offset[2] = {kCssPositionNone, kCssPositionNone};
    if (count == 1) {
        value[vertical(0) ? 1 : 0] = items[0];
    } else if (count == 2 && keyword(0) != PrCssKeyword_Count && keyword(1) != PrCssKeyword_Count) {
        bool swapped = vertical(0) || horizontal(1);
        value[0] = items[swapped ? 1 : 0];
        value[1] = items[swapped ? 0 : 1];
    } else if (count == 2) {
        value[0] = items[0];
        value[1] = items[1];
    } else {
        // Keywords, each with the offset that follows it. center has none, and takes the axis that the other leaves.
        uint32_t found = 0;
        uint32_t keywords[4];
        uint32_t offsets[4];
        bool axes[4] = {};
        bool centered[4] = {};
        for (uint32_t i = 0; i < count; ++i) {
            if (keyword(i) == PrCssKeyword_Count)
                continue;
            keywords[found] = items[i];
            axes[found] = vertical(i);
            centered[found] = keyword(i) == PrCssKeyword_Center;
            offsets[found] = i + 1 < count && keyword(i + 1) == PrCssKeyword_Count ? items[i + 1] : kCssPositionNone;
            ++found;
        }
        if (found == 2 && centered[0])
            axes[0] = !axes[1];
        if (found == 2 && centered[1])
            axes[1] = !axes[0];
        for (uint32_t i = 0; i < found; ++i) {
            value[axes[i] ? 1 : 0] = keywords[i];
            offset[axes[i] ? 1 : 0] = offsets[i];
        }
    }
    for (uint32_t axis = 0; axis < 2; ++axis) {
        if (value[axis] == kCssPositionNone)
            cssValuePoolAdd(pool, cssNumberValue(PrCssValueKind_Percentage, PrCssUnit_Number, 50));
        else
            addPositionAxis(pool, run, value[axis], offset[axis]);
    }
}

// Returns true if `property` takes a <line-width> (css-backgrounds-3/#typedef-line-width).
bool isLineWidth(PrCssProperty property) {
    return property == PrCssProperty_BorderTopWidth || property == PrCssProperty_BorderRightWidth
           || property == PrCssProperty_BorderBottomWidth || property == PrCssProperty_BorderLeftWidth
           || property == PrCssProperty_OutlineWidth || property == PrCssProperty_ColumnRuleWidth;
}

PrColor8 color8Of(PrColor color) {
    auto channel = [](float c) { return uint8_t(clampF32(c, 0, 1) * 255 + 0.5f); };
    return PrColor8{channel(color.r), channel(color.g), channel(color.b), channel(color.a)};
}

// Writes the context's element's accent color, and returns false if accent-color is auto or names AccentColor itself.
bool accentOf(const CssColorContext& context, PrColor* out) {
    if (context.style == nullptr)
        return false;
    CssValueRun accent = cssComputedValue(context.style, PrCssProperty_AccentColor);
    if (accent.count == 0 || cssValueIsKeyword(accent.values, PrCssKeyword_Auto)
        || cssValueIsKeyword(accent.values, PrCssKeyword_Accentcolor))
        return false;
    return cssResolveColor(accent, 0, context, out);
}

// The sRGB color that a system color stands for in a light or a dark color scheme (css-color-4/#css-system-colors), as
// 8-bit channels. A deprecated system color stands for the color that it computes to. AccentColor and AccentColorText
// follow the element's accent color, if it has one.
PrColor systemColor(PrCssKeyword keyword, const CssColorContext& context) {
    struct SchemeColors {
        uint32_t light;
        uint32_t dark;
    };
    SchemeColors rgb{};
    PrColor accent{};
    switch (cssSystemColorComputed(keyword)) {
        case PrCssKeyword_Canvas:           rgb = {0xffffff, 0x1e1e1e}; break;
        case PrCssKeyword_Canvastext:       rgb = {0x000000, 0xffffff}; break;
        case PrCssKeyword_Linktext:         rgb = {0x0000ee, 0x9e9eff}; break;
        case PrCssKeyword_Visitedtext:      rgb = {0x551a8b, 0xd0adf0}; break;
        case PrCssKeyword_Activetext:       rgb = {0xff0000, 0xff9e9e}; break;
        case PrCssKeyword_Buttonface:       rgb = {0xefefef, 0x6b6b6b}; break;
        case PrCssKeyword_Buttontext:       rgb = {0x000000, 0xffffff}; break;
        case PrCssKeyword_Buttonborder:     rgb = {0x767676, 0x6b6b6b}; break;
        case PrCssKeyword_Field:            rgb = {0xffffff, 0x3b3b3b}; break;
        case PrCssKeyword_Fieldtext:        rgb = {0x000000, 0xffffff}; break;
        case PrCssKeyword_Graytext:         rgb = {0x6d6d6d, 0x8e8e8e}; break;
        case PrCssKeyword_Highlight:        rgb = {0xb3d7ff, 0x3f638b}; break;
        case PrCssKeyword_Highlighttext:    rgb = {0x000000, 0xffffff}; break;
        case PrCssKeyword_Mark:             rgb = {0xffff00, 0x666600}; break;
        case PrCssKeyword_Marktext:         rgb = {0x000000, 0xffffff}; break;
        case PrCssKeyword_Selecteditem:     rgb = {0x0075ff, 0x99c8ff}; break;
        case PrCssKeyword_Selecteditemtext: rgb = {0xffffff, 0x000000}; break;
        case PrCssKeyword_Accentcolor:
            if (accentOf(context, &accent))
                return PrColor{accent.r, accent.g, accent.b, 1};
            rgb = {0x0075ff, 0x99c8ff};
            break;
        case PrCssKeyword_Accentcolortext:
            if (accentOf(context, &accent)) {
                // White or black, whichever contrasts more with the accent color.
                float luminance = kPrColorLuminanceRed * clampF32(accent.r, 0, 1)
                                  + kPrColorLuminanceGreen * clampF32(accent.g, 0, 1)
                                  + kPrColorLuminanceBlue * clampF32(accent.b, 0, 1);
                return luminance > 0.5f ? PrColor{0, 0, 0, 1} : PrColor{1, 1, 1, 1};
            }
            rgb = {0xffffff, 0x000000};
            break;
        default:
            return PrColor{0, 0, 0, 1};
    }
    uint32_t value = cssUsesDarkScheme(context.style, context.environment) ? rgb.dark : rgb.light;
    return prColorFromColor8(PrColor8{uint8_t(value >> 16), uint8_t(value >> 8), uint8_t(value), 255});
}

uint64_t hashValue(const CssValueNode& value, const char* PR_NULLABLE text, uint64_t hash) {
    uint64_t head = uint64_t(value.kind) | uint64_t(value.unit) << 8 | uint64_t(value.aux) << 16
                    | uint64_t(value.next) << 32;
    hash = hashBytes(&head, sizeof(head), hash);
    if (cssValueHasText(value.kind))
        return hashBytes(text + value.text.start, value.text.length, hash);
    if (cssValueIsNumeric(value.kind)) {
        uint64_t bits = bitCast<uint64_t>(value.number);
        return hashBytes(&bits, sizeof(bits), hash);
    }
    if (value.kind == PrCssValueKind_UnicodeRange)
        return hashBytes(&value.range, sizeof(value.range), hash);
    return hash;
}

bool sameValue(const CssValueNode& a, const char* PR_NULLABLE aText, const CssValueNode& b,
               const char* PR_NULLABLE bText) {
    if (a.kind != b.kind || a.unit != b.unit || a.aux != b.aux || a.next != b.next)
        return false;
    if (cssValueHasText(a.kind))
        return a.text.length == b.text.length && memEquals(aText + a.text.start, bText + b.text.start, a.text.length);
    if (cssValueIsNumeric(a.kind))
        return bitCast<uint64_t>(a.number) == bitCast<uint64_t>(b.number);
    if (a.kind == PrCssValueKind_UnicodeRange)
        return a.range.first == b.range.first && a.range.last == b.range.last;
    return true;
}

// Returns true if the interned group holds the values that the builder built.
bool sameGroup(const CssStyleValues* group, const CssStyleBuilder* builder) {
    for (uint32_t i = 0; i < builder->spans.count; ++i) {
        CssValueSpan a = group->spans[i];
        CssValueSpan b = builder->spans.data[i];
        if (a.count != b.count)
            return false;
        for (uint32_t k = 0; k < a.count; ++k) {
            if (!sameValue(group->values[a.start + k], group->text, builder->pool.values.data[b.start + k],
                           builder->pool.text.data))
                return false;
        }
    }
    return true;
}

// Returns the part of a table that interns the groups whose hash is `hash`. The part is chosen by the hash's top bits,
// because the index uses the low bits.
CssStyleTablePart& partOf(CssStyleTable* table, uint32_t hash) {
    return table->parts[hash >> (32 - kCssStyleTablePartBits)];
}

// Frees a group that nothing holds, and takes it out of its table.
void freeGroup(CssStyleTable* table, CssStyleValues* group) {
    CssStyleTablePart& part = partOf(table, group->hash);
    prMutexLock(&part.mutex);
    part.index.erase(group->hash, group->slot);
    part.groups.data[group->slot] = nullptr;
    part.free.push(group->slot);
    prMutexUnlock(&part.mutex);
    prMemoryFree(group);
}

// Takes a reference to a group or a set that the table interns. Returns false if its last reference was released and it
// is being freed.
template <typename Interned>
bool retainLive(const Interned* interned) {
    uint32_t count = interned->references.load();
    while (count != 0) {
        if (interned->references.cas(count, count + 1))
            return true;
    }
    return false;
}

// Returns the group that the builder has built, interned in its table under `key`. The caller holds one reference to
// it.
const CssStyleValues* intern(CssStyleBuilder* builder, CssStyleGroup group, uint32_t key) {
    CssStyleTable* table = builder->table;
    CssStyleTablePart& part = partOf(table, key);
    prMutexLock(&part.mutex);
    // A group whose last reference was released is being removed from the table, and is treated as absent.
    uint32_t found = part.index.find(key, [&](uint32_t at) {
        CssStyleValues* candidate = part.groups.data[at];
        return candidate->group == group && sameGroup(candidate, builder) && retainLive(candidate);
    });
    if (found != kHashMissing) {
        CssStyleValues* existing = part.groups.data[found];
        prMutexUnlock(&part.mutex);
        return existing;
    }
    uint32_t slots = builder->spans.count;
    uint32_t valueCount = builder->pool.values.count;
    uint32_t textSize = builder->pool.text.count;
    size_t spans = sizeof(CssStyleValues);
    size_t values = spans + sizeof(CssValueSpan) * slots;
    values = (values + alignof(CssValueNode) - 1) & ~(alignof(CssValueNode) - 1);
    size_t text = values + sizeof(CssValueNode) * valueCount;
    uint8_t* bytes = static_cast<uint8_t*>(prMemoryAlloc(text + textSize));
    CssStyleValues* made = reinterpret_cast<CssStyleValues*>(bytes);
    made->references.storeRelaxed(1);
    made->hash = key;
    made->group = group;
    made->valueCount = valueCount;
    made->textSize = textSize;
    made->spans = reinterpret_cast<CssValueSpan*>(bytes + spans);
    made->values = reinterpret_cast<CssValueNode*>(bytes + values);
    made->text = textSize != 0 ? reinterpret_cast<char*>(bytes + text) : nullptr;
    memCopy(made->spans, builder->spans.data, sizeof(CssValueSpan) * slots);
    if (valueCount != 0)
        memCopy(made->values, builder->pool.values.data, sizeof(CssValueNode) * valueCount);
    if (textSize != 0)
        memCopy(made->text, builder->pool.text.data, textSize);
    uint32_t at = 0;
    if (part.free.count != 0) {
        at = part.free.last();
        part.free.pop();
        part.groups.data[at] = made;
    } else {
        at = part.groups.count;
        part.groups.push(made);
    }
    made->owner = table;
    made->slot = at;
    part.index.insert(key, at);
    prMutexUnlock(&part.mutex);
    return made;
}

// Parses the initial value of `property` into `pool`, which is empty, and returns it. The initial font family is the
// user agent's (css-fonts-4/#font-family-prop).
CssValueRun initialOf(CssValuePool* pool, PrCssProperty property) {
    PrStringView initial = property == PrCssProperty_FontFamily ? "serif" : cssPropertyInitial(property);
    bool valid = cssParseDeclaredValue(cssPropertyDeclaredName(property), initial, pool, nullptr);
    PR_ASSERT(valid);
    return valid ? cssValuePoolRun(pool, CssValueSpan{0, pool->values.count}) : CssValueRun{};
}

// The computed initial value of every property that the engine renders, in one pool. An initial value computes in the
// initial context, so the values are the same for every table. They are computed once in the process, by the first
// table that needs them, and never freed.
struct InitialValues {
    CssValuePool pool;
    CssValueSpan spans[PrCssProperty_Count];   // by property
};

// The initial values once computed, or null. A thread that loses the race to publish them frees its own.
Atomic<InitialValues*> gInitialValues;

// Returns the computed initial values, computing them on first use.
const InitialValues* initialValues() {
    InitialValues* found = gInitialValues.load();
    if (found != nullptr)
        return found;
    InitialValues* made = allocZeroed<InitialValues>();
    CssStyleContext context{};
    context.font = kCssInitialFontBases;
    context.root = kCssInitialFontBases;
    context.parentWeight = 400;
    context.parentColor = PrColor{0, 0, 0, 1};
    CssValuePool parsed{};
    for (uint32_t g = 0; g < CssStyleGroup_Count; ++g) {
        CssStyleGroupProperties properties = kCssStyleGroupProperties[g];
        for (uint32_t i = 0; i < properties.count; ++i) {
            PrCssProperty property = properties.properties[i];
            parsed.values.count = 0;
            parsed.text.count = 0;
            uint32_t start = made->pool.values.count;
            cssStyleComputeValue(&made->pool, property, initialOf(&parsed, property), context);
            made->spans[property] = CssValueSpan{start, made->pool.values.count - start};
        }
    }
    cssValuePoolReset(&parsed);
    InitialValues* expected = nullptr;
    if (gInitialValues.cas(expected, made))
        return made;
    cssValuePoolReset(&made->pool);
    prMemoryFree(made);
    return expected;
}

// Returns the group of initial values of the properties of `group`, interned in the builder's table.
const CssStyleValues* computeInitial(CssStyleBuilder* builder, CssStyleGroup group) {
    const InitialValues* initial = initialValues();
    cssStyleBegin(builder);
    CssStyleGroupProperties properties = kCssStyleGroupProperties[group];
    for (uint32_t i = 0; i < properties.count; ++i)
        cssStyleAddValue(builder, cssValuePoolRun(&initial->pool, initial->spans[properties.properties[i]]));
    return cssStyleEnd(builder, group);
}

PrStringView entryName(const CssCustomProperties* set, const CssCustomEntry& entry) {
    return PrStringView{set->text + entry.name, entry.nameSize};
}

CssCustomRow entryRow(const CssCustomProperties* set, const CssCustomEntry& entry) {
    CssCustomRow row{};
    row.hash = entry.hash;
    row.name = entryName(set, entry);
    row.invalid = entry.valueSize == kCssCustomInvalid;
    if (!row.invalid)
        row.value = PrStringView{set->text + entry.value, entry.valueSize};
    return row;
}

// The seed and the multiplier for hashing an entry's name and value into its set's contents.
constexpr uint64_t kCustomContentsSeed = 0x6a09e667f3bcc909ull;
constexpr uint64_t kCustomContentsMix = 0x9e3779b97f4a7c15ull;

// Returns the hash that a valid entry adds to its set's contents. The hash is even, so that a sum of hashes leaves the
// low bit free to mark the sum as computed.
uint64_t entryContents(uint32_t hash, PrStringView value) {
    uint64_t seed = kCustomContentsSeed ^ (uint64_t(hash) * kCustomContentsMix);
    return hashBytes(value.data, value.size, seed) & ~uint64_t(1);
}

// Returns the index of the set's own entry named `name` with hash `hash`, or kHashMissing if there is none.
uint32_t findEntry(const CssCustomProperties* set, uint32_t hash, PrStringView name) {
    uint32_t low = partitionPoint(0, set->count, [&](uint32_t i) {
        const CssCustomEntry& entry = set->entries[i];
        return cssCustomBefore(entry.hash, entryName(set, entry), hash, name);
    });
    if (low == set->count)
        return kHashMissing;
    const CssCustomEntry& entry = set->entries[low];
    return entry.hash == hash && entryName(set, entry) == name ? low : kHashMissing;
}

// The seed for hashing a set's base and own entries, to intern the set.
constexpr uint64_t kCustomKeySeed = 0xbb67ae8584caa73bull;

// Returns true if the set's own entries are `rows`.
bool sameRows(const CssCustomProperties* set, const CssCustomRow* PR_NULLABLE rows, uint32_t count) {
    if (set->count != count)
        return false;
    for (uint32_t i = 0; i < count; ++i) {
        CssCustomRow own = entryRow(set, set->entries[i]);
        if (own.hash != rows[i].hash || own.invalid != rows[i].invalid || own.name != rows[i].name
            || (!own.invalid && own.value != rows[i].value))
            return false;
    }
    return true;
}

// Returns the contents of `set` (CssCustomProperties::contents), and computes them if no comparison has yet. The
// contents are the base's sum, less the hash of each base entry that an own entry hides, plus the hashes of the valid
// own entries. Threads that compute the contents at the same time store the same value.
uint64_t contentsOf(const CssCustomProperties* PR_NULLABLE set) {
    if (set == nullptr)
        return 1;
    uint64_t contents = set->contents.loadRelaxed();
    if (contents != 0)
        return contents;
    const CssCustomProperties* base = set->base;
    contents = base != nullptr ? contentsOf(base) & ~uint64_t(1) : 0;
    for (uint32_t i = 0; i < set->count; ++i) {
        const CssCustomEntry& entry = set->entries[i];
        if (base != nullptr) {
            uint32_t hidden = findEntry(base, entry.hash, entryName(set, entry));
            if (hidden != kHashMissing && base->entries[hidden].valueSize != kCssCustomInvalid)
                contents -= entryContents(entry.hash, entryRow(base, base->entries[hidden]).value);
        }
        if (entry.valueSize != kCssCustomInvalid)
            contents += entryContents(entry.hash, entryRow(set, entry).value);
    }
    contents |= 1;
    set->contents.storeRelaxed(contents);
    return contents;
}

// A set split into the entries above its base, if any, and the base. A set with no base is treated entirely as a base.
struct Layers {
    const CssCustomProperties* PR_NULLABLE own;
    const CssCustomProperties* PR_NULLABLE base;
};

Layers layersOf(const CssCustomProperties* PR_NULLABLE set) {
    if (set == nullptr || set->base == nullptr)
        return Layers{nullptr, set};
    return Layers{set, set->base};
}

// Looks up `name` in the own entries, then the base. Returns false if neither holds a valid value.
bool layeredValue(const Layers& layers, uint32_t hash, PrStringView name, PrStringView* out) {
    if (layers.own != nullptr) {
        uint32_t at = findEntry(layers.own, hash, name);
        if (at != kHashMissing) {
            const CssCustomEntry& entry = layers.own->entries[at];
            if (entry.valueSize == kCssCustomInvalid)
                return false;
            *out = PrStringView{layers.own->text + entry.value, entry.valueSize};
            return true;
        }
    }
    return layers.base != nullptr && cssCustomValue(layers.base, name, out);
}

// Returns true if every name in the own entries of `of` has the same value in both sets.
bool ownEntriesAgree(const Layers& of, const Layers& with) {
    for (uint32_t i = 0; of.own != nullptr && i < of.own->count; ++i) {
        const CssCustomEntry& entry = of.own->entries[i];
        PrStringView name = entryName(of.own, entry);
        PrStringView x{};
        PrStringView y{};
        bool hasX = layeredValue(of, entry.hash, name, &x);
        bool hasY = layeredValue(with, entry.hash, name, &y);
        if (hasX != hasY || (hasX && x != y))
            return false;
    }
    return true;
}

// Returns true if both sets have the same own entries in the same order. This is the common case of an element that is
// restyled without a change to its custom properties.
bool sameOwnEntries(const Layers& x, const Layers& y) {
    if (x.own == nullptr || y.own == nullptr || x.own->count != y.own->count)
        return false;
    for (uint32_t i = 0; i < x.own->count; ++i) {
        CssCustomRow p = entryRow(x.own, x.own->entries[i]);
        CssCustomRow q = entryRow(y.own, y.own->entries[i]);
        if (p.hash != q.hash || p.invalid != q.invalid || p.name != q.name || p.value != q.value)
            return false;
    }
    return true;
}

// A flow-relative side, in the order of a CssSideFamily's members.
enum {
    LogicalSide_BlockStart,
    LogicalSide_BlockEnd,
    LogicalSide_InlineStart,
    LogicalSide_InlineEnd,
};
typedef uint8_t LogicalSide;

// A physical side, in the order of a CssSideFamily's members.
enum {
    PhysicalSide_Top,
    PhysicalSide_Right,
    PhysicalSide_Bottom,
    PhysicalSide_Left,
};
typedef uint8_t PhysicalSide;

// Returns the physical side that the flow-relative side `side` maps to in `writingMode` and `direction`
// (css-writing-modes-4/#logical-to-physical). The block-start side is the top in horizontal-tb, the right in
// vertical-rl and sideways-rl, and the left in vertical-lr and sideways-lr. In ltr, the inline-start side is the left
// in horizontal-tb, and the top in the vertical modes except sideways-lr, where it is the bottom. In rtl, the
// inline-start side is the opposite side.
PhysicalSide physicalSide(LogicalSide side, PrCssKeyword writingMode, PrCssKeyword direction) {
    bool horizontal = writingMode != PrCssKeyword_VerticalRl && writingMode != PrCssKeyword_VerticalLr
                      && writingMode != PrCssKeyword_SidewaysRl && writingMode != PrCssKeyword_SidewaysLr;
    bool end = side == LogicalSide_BlockEnd || side == LogicalSide_InlineEnd;
    PhysicalSide start = PhysicalSide_Top;
    if (side == LogicalSide_BlockStart || side == LogicalSide_BlockEnd) {
        bool leftward = writingMode == PrCssKeyword_VerticalLr || writingMode == PrCssKeyword_SidewaysLr;
        start = horizontal ? PhysicalSide_Top : leftward ? PhysicalSide_Left : PhysicalSide_Right;
    } else {
        bool rtl = direction == PrCssKeyword_Rtl;
        bool backward = writingMode == PrCssKeyword_SidewaysLr ? !rtl : rtl;
        start = horizontal ? (rtl ? PhysicalSide_Right : PhysicalSide_Left)
                           : (backward ? PhysicalSide_Bottom : PhysicalSide_Top);
    }
    return end ? PhysicalSide((start + 2) % 4) : start;
}

// Resolves the color-hdr() at `index` for the display's headroom (css-color-hdr-1/#headroom-interpolation). Each of its
// two items is a color and, in either order, the headroom in stops that the color is for, which is 0 if omitted
// (csswg-drafts#13597).
bool resolveColorHdr(CssValueRun run, uint32_t index, const CssColorContext& context, PrColor* out) {
    const CssValueNode* function = &run.values[index];
    PrColorHdr hdr{};
    uint32_t item = 0;
    bool colored[2] = {false, false};
    for (uint32_t at = index + 1; at < index + function->next && item < 2; at += run.values[at].next) {
        const CssValueNode* value = &run.values[at];
        if (value->kind == PrCssValueKind_Literal) {
            ++item;
        } else if (value->kind == PrCssValueKind_Number || value->kind == PrCssValueKind_Integer) {
            hdr.headrooms[item] = float(value->number);
        } else {
            PrColor color{};
            if (!cssResolveColor(run, at, context, &color))
                return false;
            hdr.colors[item] = prColor16FromColor(color);
            colored[item] = true;
        }
    }
    if (!colored[0] || !colored[1])
        return false;
    if (context.environment.readsOutside != nullptr)
        *context.environment.readsOutside = true;
    *out = prColorHdrResolve(&hdr, context.environment.headroom);
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// Resolving colors

constexpr uint32_t kMissingAlpha = 1u << 3;

CssColor srgbColor(PrColor color) {
    return CssColor{PrColorSpace_Srgb, {double(color.r), double(color.g), double(color.b)}, double(color.a), 0};
}

// The categories of analogous components (css-color-4/#analogous-components). None is a component that has no analog in
// another space: HWB's whiteness and blackness.
enum {
    ComponentCategory_None,
    ComponentCategory_Red,
    ComponentCategory_Green,
    ComponentCategory_Blue,
    ComponentCategory_Lightness,
    ComponentCategory_Colorfulness,
    ComponentCategory_Hue,
    ComponentCategory_OpponentA,
    ComponentCategory_OpponentB,
};
typedef uint8_t ComponentCategory;

// Returns the category of each component of a space's model.
const ComponentCategory* componentCategories(PrColorSpace space) {
    static constexpr ComponentCategory kRgb[3] = {ComponentCategory_Red, ComponentCategory_Green,
                                                  ComponentCategory_Blue};
    static constexpr ComponentCategory kLab[3] = {ComponentCategory_Lightness, ComponentCategory_OpponentA,
                                                  ComponentCategory_OpponentB};
    static constexpr ComponentCategory kLch[3] = {ComponentCategory_Lightness, ComponentCategory_Colorfulness,
                                                  ComponentCategory_Hue};
    static constexpr ComponentCategory kHsl[3] = {ComponentCategory_Hue, ComponentCategory_Colorfulness,
                                                  ComponentCategory_Lightness};
    static constexpr ComponentCategory kHwb[3] = {ComponentCategory_Hue, ComponentCategory_None,
                                                  ComponentCategory_None};
    switch (prColorSpaceModel(space)) {
        case PrColorModel_Lab: return kLab;
        case PrColorModel_Lch: return kLch;
        case PrColorModel_Hsl: return kHsl;
        case PrColorModel_Hwb: return kHwb;
        default: return kRgb;
    }
}

// Sets a hue that its color leaves powerless to missing, as for a color that a conversion produced
// (css-color-4/#powerless). The colorfulness that made it powerless becomes zero.
void markPowerless(CssColor& color) {
    switch (color.space) {
        case PrColorSpace_Lch:
        case PrColorSpace_Oklch:
        case PrColorSpace_Jzczhz: {
            double epsilon = color.space == PrColorSpace_Lch ? 0.0015 : color.space == PrColorSpace_Oklch ? 0.000004
                                                                                                          : 0.0000002;
            if (color.c[1] <= epsilon) {
                if (color.c[1] > 0)
                    color.c[1] = 0;
                color.missing |= 1u << 2;
            }
            break;
        }
        case PrColorSpace_Hsl:
            // A conversion leaves a white or a black within rounding of its lightness, with any saturation.
            if (absF64(color.c[1]) <= 0.001 || color.c[2] <= 0.001 || color.c[2] >= 100 - 0.001) {
                if (color.c[1] > 0 && color.c[1] <= 0.001)
                    color.c[1] = 0;
                color.missing |= 1u;
            }
            break;
        case PrColorSpace_Hwb:
            if (color.c[1] + color.c[2] >= 100 - 0.001)
                color.missing |= 1u;
            break;
        default:
            break;
    }
}

// Converts a color to `space` for interpolation or for a relative color (css-color-4/#interpolation-missing): its
// missing analogous components, and a missing analogous set, are carried forward as missing. A conversion to another
// space makes a powerless hue missing. Missing components count as zero while converting.
CssColor convertCarrying(const CssColor& color, PrColorSpace space) {
    if (color.space == space)
        return color;
    CssColor out{space, {}, color.alpha, color.missing & kMissingAlpha};
    double in[3];
    for (uint32_t i = 0; i < 3; ++i)
        in[i] = (color.missing & (1u << i)) != 0 ? 0 : color.c[i];
    prColorConvert(color.space, space, in, out.c);
    markPowerless(out);
    const ComponentCategory* from = componentCategories(color.space);
    const ComponentCategory* to = componentCategories(space);
    uint32_t unmatchedFrom = 0;
    uint32_t unmatchedTo = 0b111;
    for (uint32_t i = 0; i < 3; ++i) {
        bool matched = false;
        for (uint32_t j = 0; j < 3 && from[i] != ComponentCategory_None; ++j) {
            if (to[j] != from[i])
                continue;
            matched = true;
            unmatchedTo &= ~(1u << j);
            if ((color.missing & (1u << i)) != 0)
                out.missing |= 1u << j;
        }
        if (!matched)
            unmatchedFrom |= 1u << i;
    }
    // An analogous set: the components left on each side. If all of the color's are missing, so are all of the space's.
    if (unmatchedFrom != 0 && (color.missing & unmatchedFrom) == unmatchedFrom)
        out.missing |= unmatchedTo;
    return out;
}

// Interpolates from `a` to `b` by `t` in `space`, with hues taking `hueMethod` (css-color-4/#interpolation). A
// component missing in one color takes the other's, and one missing in both stays missing.
CssColor interpolate(const CssColor& a, const CssColor& b, double t, PrColorSpace space,
                     PrColorHueMethod hueMethod) {
    CssColor x = convertCarrying(a, space);
    CssColor y = convertCarrying(b, space);
    CssColor out{space, {}, 0, 0};
    for (uint32_t i = 0; i < 4; ++i) {
        uint32_t bit = 1u << i;
        double* xi = i < 3 ? &x.c[i] : &x.alpha;
        double* yi = i < 3 ? &y.c[i] : &y.alpha;
        if ((x.missing & bit) != 0 && (y.missing & bit) != 0)
            out.missing |= bit;
        else if ((x.missing & bit) != 0)
            *xi = *yi;
        else if ((y.missing & bit) != 0)
            *yi = *xi;
    }
    uint32_t hue = prColorSpaceHueChannel(space);
    if (hue < 3 && (out.missing & (1u << hue)) == 0) {
        double h1 = prColorHue(x.c[hue]);
        double h2 = prColorHue(y.c[hue]);
        double d = h2 - h1;
        switch (hueMethod) {
            case PrColorHueMethod_Longer:
                if (d > 0 && d < 180)
                    h1 += 360;
                else if (d > -180 && d <= 0)
                    h2 += 360;
                break;
            case PrColorHueMethod_Increasing:
                if (d < 0)
                    h2 += 360;
                break;
            case PrColorHueMethod_Decreasing:
                if (d > 0)
                    h1 += 360;
                break;
            default:
                if (d > 180)
                    h1 += 360;
                else if (d < -180)
                    h2 += 360;
                break;
        }
        x.c[hue] = h1;
        y.c[hue] = h2;
    }
    // Premultiplied, except for the hue. A missing alpha counts as opaque.
    double ax = (out.missing & kMissingAlpha) != 0 ? 1 : x.alpha;
    double ay = (out.missing & kMissingAlpha) != 0 ? 1 : y.alpha;
    double alpha = ax + (ay - ax) * t;
    for (uint32_t i = 0; i < 3; ++i) {
        if (i == hue) {
            out.c[i] = prColorHue(x.c[i] + (y.c[i] - x.c[i]) * t);
            continue;
        }
        double value = x.c[i] * ax + (y.c[i] * ay - x.c[i] * ax) * t;
        out.c[i] = alpha != 0 ? value / alpha : value;
    }
    out.alpha = (out.missing & kMissingAlpha) != 0 ? 0 : alpha;
    return out;
}

// Returns the extended sRGB of a resolved color, with straight alpha in [0, 1]. A missing component is zero.
PrColor toSrgb(const CssColor& color) {
    double c[3];
    for (uint32_t i = 0; i < 3; ++i)
        c[i] = (color.missing & (1u << i)) != 0 ? 0 : color.c[i];
    double alpha = (color.missing & kMissingAlpha) != 0 ? 0 : color.alpha;
    double rgb[3];
    prColorConvert(color.space, PrColorSpace_Srgb, c, rgb);
    return PrColor{float(rgb[0]), float(rgb[1]), float(rgb[2]), clampF32(float(alpha), 0, 1)};
}

bool resolveColorValue(CssValueRun run, uint32_t index, const CssColorContext& in, CssColor* out);

// Returns the value of a color channel written at `at` in the units that `syntax` reads, or false if it is not a
// number, percentage, angle or calculation of them. `none` is missing. A calculation reads the relative color's
// component keywords as their values in `components`, which are NaN where missing, and a missing component counts as 0
// there.
bool channelOf(CssValueRun run, uint32_t at, const CssColorChannelSyntax& syntax, const PrCssKeyword* PR_NULLABLE names,
               const double* PR_NULLABLE components, double* value, bool* missing) {
    const CssValueNode* node = &run.values[at];
    *missing = false;
    if (node->kind == PrCssValueKind_Keyword) {
        PrCssKeyword keyword = cssValueKeyword(node);
        if (keyword == PrCssKeyword_None) {
            *missing = true;
            return true;
        }
        for (uint32_t i = 0; names != nullptr && i < 4; ++i) {
            if (names[i] != keyword)
                continue;
            if (isNanF64(components[i])) {
                *missing = true;
            } else {
                CssValueNode number = cssNumberValue(PrCssValueKind_Number, PrCssUnit_Number, components[i]);
                *value = cssColorChannelValue(&number, syntax);
            }
            return true;
        }
        return false;
    }
    if (cssValueIsNumeric(node->kind)) {
        *value = cssColorChannelValue(node, syntax);
        return true;
    }
    if (node->kind != PrCssValueKind_Math)
        return false;
    // A calculation: its component keywords become their numbers, in a copy of it.
    Array<CssValueNode> copy;
    copy.append(node, node->next);
    for (uint32_t i = 0; i < copy.count; ++i) {
        CssValueNode& v = copy.data[i];
        if (v.kind != PrCssValueKind_Keyword || names == nullptr)
            continue;
        for (uint32_t k = 0; k < 4; ++k) {
            if (names[k] == cssValueKeyword(&v)) {
                double number = isNanF64(components[k]) ? 0 : components[k];
                v = cssNumberValue(PrCssValueKind_Number, PrCssUnit_Number, number);
            }
        }
    }
    CssValueRun calculation{copy.data, copy.count, run.text};
    double number = 0;
    CssValueNode result{};
    bool ok = false;
    if (cssMathTo(calculation, 0, PrCssUnit_Number, &number)) {
        result = cssNumberValue(PrCssValueKind_Number, PrCssUnit_Number, number);
        ok = true;
    } else if (cssMathTo(calculation, 0, PrCssUnit_Percent, &number)) {
        result = cssNumberValue(PrCssValueKind_Percentage, PrCssUnit_Number, number);
        ok = true;
    } else if (cssMathTo(calculation, 0, PrCssUnit_Deg, &number)) {
        result = cssNumberValue(PrCssValueKind_Dimension, PrCssUnit_Deg, number);
        ok = true;
    }
    copy.reset();
    if (ok)
        *value = cssColorChannelValue(&result, syntax);
    return ok;
}

// Resolves a color written with its components (css-color-4/#color-syntax).
CssColor colorOfChannels(CssValueRun run, uint32_t index) {
    CssColorChannels channels = cssColorChannelsFromValue(run, index);
    CssColor color{channels.space, {}, 1, 0};
    for (uint32_t i = 0; i < 4; ++i) {
        double value = 0;
        bool missing = false;
        if (!channelOf(run, channels.at[i], *channels.meaning[i], nullptr, nullptr, &value, &missing))
            value = 0;
        if (missing)
            color.missing |= 1u << i;
        (i < 3 ? color.c[i] : color.alpha) = value;
    }
    return color;
}

// Resolves a relative color (css-color-5/#relative-colors): `rgb(from <color> ...)` and the other functions of a color
// space, `color(from <color> <space> ...)`, and `alpha(from <color> / <alpha>)`. The origin converts to the function's
// space, and its components, by their keywords, are the values of the channels' expressions.
bool relativeColor(CssValueRun run, uint32_t index, const CssColorContext& in, CssColor* out) {
    const CssValueNode* function = &run.values[index];
    PrCssKeyword name = cssValueKeyword(function);
    uint32_t end = index + function->next;
    uint32_t at = index + 2;
    CssColor origin{};
    if (at >= end || !resolveColorValue(run, at, in, &origin))
        return false;
    at += run.values[at].next;
    PrColorSpace space = origin.space;
    bool legacy = false;
    if (name == PrCssKeyword_Color) {
        // A custom color space parses, and is not supported.
        const CssValueNode* named = &run.values[at];
        space = named->kind == PrCssValueKind_Keyword ? cssColorSpaceFromKeyword(cssValueKeyword(named))
                                                      : PrColorSpace_Count;
        if (space == PrColorSpace_Count)
            return false;
        ++at;
    } else if (name != PrCssKeyword_Alpha) {
        space = cssColorSpaceFromKeyword(name);
        legacy = space == PrColorSpace_Srgb;
        if (space == PrColorSpace_Count)
            return false;
    }
    CssColor converted = convertCarrying(origin, space);
    const CssColorChannelSyntax* syntax = cssColorChannels(space, legacy);
    // The components' keywords, and their values in the numbers that the channels read.
    PrColorModel model = prColorSpaceModel(space);
    PrCssKeyword names[4] = {kCssColorComponents[model][0], kCssColorComponents[model][1],
                             kCssColorComponents[model][2], PrCssKeyword_Alpha};
    double components[4];
    for (uint32_t i = 0; i < 4; ++i) {
        bool missing = (converted.missing & (1u << i)) != 0;
        double value = i < 3 ? converted.c[i] : converted.alpha;
        double scale = i < 3 ? syntax[i].number : kCssColorAlpha.number;
        components[i] = missing ? kNanF64 : value / scale;
    }
    CssColor color = converted;
    color.missing = converted.missing;
    uint32_t channel = name == PrCssKeyword_Alpha ? 3 : 0;
    for (; at < end; at += run.values[at].next) {
        if (cssValueIsLiteral(&run.values[at], '/')) {
            channel = 3;
            continue;
        }
        if (channel > 3)
            return false;
        const CssColorChannelSyntax& meaning = channel < 3 ? syntax[channel] : kCssColorAlpha;
        double value = 0;
        bool missing = false;
        if (!channelOf(run, at, meaning, names, components, &value, &missing))
            return false;
        uint32_t bit = 1u << channel;
        color.missing = missing ? color.missing | bit : color.missing & ~bit;
        (channel < 3 ? color.c[channel] : color.alpha) = value;
        ++channel;
    }
    *out = color;
    return true;
}

// Resolves color-mix() (css-color-5/#color-mix): the colors mixed in turn in its interpolation space, which is Oklab if
// none is given, each by its share of the percentages normalized to 100%. If the percentages sum to less than 100%, the
// result's alpha is scaled by their sum.
bool mixColors(CssValueRun run, uint32_t index, const CssColorContext& in, CssColor* out) {
    const CssValueNode* function = &run.values[index];
    uint32_t end = index + function->next;
    uint32_t at = index + 1;
    PrColorSpace space = PrColorSpace_Oklab;
    PrColorHueMethod hueMethod = PrColorHueMethod_Shorter;
    if (at < end && cssValueIsKeyword(&run.values[at], PrCssKeyword_In)) {
        // A custom color space parses, and is not supported.
        const CssValueNode* named = &run.values[at + 1];
        space = named->kind == PrCssValueKind_Keyword ? cssColorSpaceFromKeyword(cssValueKeyword(named))
                                                      : PrColorSpace_Count;
        if (space == PrColorSpace_Count)
            return false;
        at += 2;
        if (at + 1 < end && cssValueIsKeyword(&run.values[at + 1], PrCssKeyword_Hue)) {
            PrCssKeyword method = cssValueKeyword(&run.values[at]);
            hueMethod = method == PrCssKeyword_Longer       ? PrColorHueMethod_Longer
                        : method == PrCssKeyword_Increasing ? PrColorHueMethod_Increasing
                        : method == PrCssKeyword_Decreasing ? PrColorHueMethod_Decreasing
                                                            : PrColorHueMethod_Shorter;
            at += 2;
        }
        ++at;
    }
    constexpr uint32_t kMaxItems = 16;
    CssColor colors[kMaxItems];
    double shares[kMaxItems];
    bool given[kMaxItems];
    uint32_t count = 0;
    while (at < end && count < kMaxItems) {
        if (!resolveColorValue(run, at, in, &colors[count]))
            return false;
        at += run.values[at].next;
        given[count] = false;
        shares[count] = 0;
        if (at < end && !cssValueIsLiteral(&run.values[at], ',')) {
            const CssValueNode* percentage = &run.values[at];
            double number = percentage->number;
            if (percentage->kind == PrCssValueKind_Math && !cssMathTo(run, at, PrCssUnit_Percent, &number))
                return false;
            shares[count] = number;
            given[count] = true;
            at += percentage->next;
        }
        ++count;
        if (at < end && cssValueIsLiteral(&run.values[at], ','))
            ++at;
    }
    if (count == 0)
        return false;
    // Percentage normalization (css-color-5/#color-mix-percent-norm).
    double sum = 0;
    uint32_t omitted = 0;
    for (uint32_t i = 0; i < count; ++i) {
        sum += shares[i];
        omitted += given[i] ? 0 : 1;
    }
    if (omitted != 0) {
        double share = sum < 100 ? (100 - sum) / double(omitted) : 0;
        for (uint32_t i = 0; i < count; ++i) {
            if (!given[i])
                shares[i] = share;
        }
        sum = 0;
        for (uint32_t i = 0; i < count; ++i)
            sum += shares[i];
    }
    if (!(sum > 0))
        return false;
    double multiplier = sum < 100 ? sum / 100 : 1;
    CssColor mixed = convertCarrying(colors[0], space);
    double weight = shares[0];
    for (uint32_t i = 1; i < count; ++i) {
        if (weight + shares[i] > 0)
            mixed = interpolate(mixed, colors[i], shares[i] / (weight + shares[i]), space, hueMethod);
        weight += shares[i];
    }
    if ((mixed.missing & kMissingAlpha) == 0)
        mixed.alpha *= multiplier;
    *out = mixed;
    return true;
}

// Resolves device-cmyk() by its naive conversion to sRGB (css-color-5/#cmyk-rgb).
bool deviceCmyk(CssValueRun run, uint32_t index, CssColor* out) {
    const CssValueNode* function = &run.values[index];
    double cmyk[4] = {};
    double alpha = 1;
    uint32_t channel = 0;
    constexpr CssColorChannelSyntax kInk = {1.0, 0.01, 0.0, 1.0, false};
    for (uint32_t at = index + 1; at < index + function->next; at += run.values[at].next) {
        const CssValueNode* value = &run.values[at];
        if (cssValueIsLiteral(value, '/')) {
            channel = 4;
            continue;
        }
        if (value->kind == PrCssValueKind_Literal)
            continue;
        double number = 0;
        bool missing = false;
        if (!channelOf(run, at, channel < 4 ? kInk : kCssColorAlpha, nullptr, nullptr, &number, &missing))
            return false;
        if (channel < 4)
            cmyk[channel] = missing ? 0 : number;
        else
            alpha = missing ? 0 : number;
        ++channel;
    }
    double k = cmyk[3];
    *out = CssColor{PrColorSpace_Srgb,
                         {1 - minF64(1, cmyk[0] * (1 - k) + k), 1 - minF64(1, cmyk[1] * (1 - k) + k),
                          1 - minF64(1, cmyk[2] * (1 - k) + k)},
                         alpha,
                         0};
    return true;
}

// Returns the WCAG 2.1 relative luminance of an sRGB color, its channels clamped to the gamut.
double relativeLuminance(PrColor color) {
    auto linear = [](float channel) {
        return prColorTransferDecodeDouble(PrColorTransfer_Srgb, double(clampF32(channel, 0, 1)));
    };
    return 0.2126 * linear(color.r) + 0.7152 * linear(color.g) + 0.0722 * linear(color.b);
}

bool resolveColorValue(CssValueRun run, uint32_t index, const CssColorContext& in, CssColor* out) {
    if (index >= run.count)
        return false;
    const CssValueNode* value = &run.values[index];
    if (value->kind == PrCssValueKind_Color) {
        *out = colorOfChannels(run, index);
        return true;
    }
    if (value->kind == PrCssValueKind_Keyword) {
        PrCssKeyword keyword = cssValueKeyword(value);
        if (keyword == PrCssKeyword_Currentcolor) {
            *out = srgbColor(in.current);
            return true;
        }
        if (keyword == PrCssKeyword_Transparent) {
            *out = srgbColor(PrColor{0, 0, 0, 0});
            return true;
        }
        if (cssIsSystemColor(keyword)) {
            *out = srgbColor(systemColor(keyword, in));
            return true;
        }
        PrColor8 named{};
        if (!cssNamedColor(keyword, &named))
            return false;
        *out = srgbColor(prColorFromColor8(named));
        return true;
    }
    if (value->kind != PrCssValueKind_Function)
        return false;
    PrCssKeyword name = cssValueKeyword(value);
    const CssValueNode* argument = index + 1 < run.count ? &run.values[index + 1] : nullptr;
    if (argument != nullptr && cssValueIsKeyword(argument, PrCssKeyword_From))
        return relativeColor(run, index, in, out);
    switch (name) {
        case PrCssKeyword_ColorHdr: {
            PrColor color{};
            if (!resolveColorHdr(run, index, in, &color))
                return false;
            *out = srgbColor(color);
            return true;
        }
        case PrCssKeyword_ColorMix:
            return mixColors(run, index, in, out);
        case PrCssKeyword_LightDark: {
            // The first color in a light color scheme, and the second in a dark one (css-color-5/#light-dark).
            uint32_t at = index + 1;
            if (cssUsesDarkScheme(in.style, in.environment))
                at += run.values[at].next + 1;
            return resolveColorValue(run, at, in, out);
        }
        case PrCssKeyword_DeviceCmyk:
            return deviceCmyk(run, index, out);
        case PrCssKeyword_ContrastColor: {
            // White or black, whichever contrasts more with the color by WCAG 2.1's ratio; white on a tie
            // (css-color-5/#contrast-color).
            CssColor base{};
            if (!resolveColorValue(run, index + 1, in, &base))
                return false;
            double luminance = relativeLuminance(toSrgb(base));
            double withWhite = 1.05 / (luminance + 0.05);
            double withBlack = (luminance + 0.05) / 0.05;
            *out = srgbColor(withWhite >= withBlack ? PrColor{1, 1, 1, 1} : PrColor{0, 0, 0, 1});
            return true;
        }
        default:
            return false;
    }
}

// Returns true if a color reads what only the element where it is used decides: its color scheme, through a system
// color or light-dark(); the display, through color-hdr(); or currentcolor, unless `current` allows it.
bool readsElement(CssValueRun run, bool current) {
    for (uint32_t i = 0; i < run.count; ++i) {
        const CssValueNode* value = &run.values[i];
        if (value->kind == PrCssValueKind_Keyword) {
            PrCssKeyword keyword = cssValueKeyword(value);
            if (cssIsSystemColor(keyword) || (!current && keyword == PrCssKeyword_Currentcolor))
                return true;
        } else if (value->kind == PrCssValueKind_Function) {
            PrCssKeyword name = cssValueKeyword(value);
            if (name == PrCssKeyword_LightDark || name == PrCssKeyword_ColorHdr)
                return true;
        }
    }
    return false;
}

// Returns true if `property` takes an <opacity-value>, a number or a percentage that computes to a number in [0, 1].
bool isOpacity(PrCssProperty property) {
    return property == PrCssProperty_Opacity || property == PrCssProperty_FillOpacity
           || property == PrCssProperty_StrokeOpacity || property == PrCssProperty_StopOpacity
           || property == PrCssProperty_FloodOpacity;
}

// Writes the number that an <opacity-value> stands for: a number, or a percentage as a fraction, either of which may be
// a calculation. Returns false for anything else, such as a CSS-wide keyword.
bool opacityOf(CssValueRun run, double* out) {
    if (run.count == 0 || run.values[0].next != run.count)
        return false;
    const CssValueNode* value = &run.values[0];
    if (value->kind == PrCssValueKind_Number || value->kind == PrCssValueKind_Integer) {
        *out = value->number;
        return true;
    }
    if (value->kind == PrCssValueKind_Percentage) {
        *out = value->number / 100;
        return true;
    }
    if (value->kind != PrCssValueKind_Math)
        return false;
    double percent = 0;
    if (cssMathTo(run, 0, PrCssUnit_Percent, &percent)) {
        *out = percent / 100;
        return true;
    }
    return cssMathTo(run, 0, PrCssUnit_Number, out);
}

// Adds a computed <length-percentage> of `percent` percent and `px` pixels: the percentage alone if the length is zero,
// the length alone if the percentage is zero and `keepPercent` is false, and otherwise calc() of both.
void addLengthSum(CssValuePool* pool, double percent, double px, bool keepPercent) {
    if (px == 0) {
        cssValuePoolAdd(pool, cssNumberValue(PrCssValueKind_Percentage, PrCssUnit_Number, percent));
        return;
    }
    if (percent == 0 && !keepPercent) {
        cssValuePoolAdd(pool, pxValue(px));
        return;
    }
    CssValueNode math = {};
    math.kind = PrCssValueKind_Math;
    math.aux = CssValueAuxFlags(CssUnitCategory_Length);
    uint32_t root = cssValuePoolAdd(pool, math);
    CssValueNode sum = {};
    sum.kind = PrCssValueKind_Calc;
    sum.aux = PrCssCalc_Sum;
    uint32_t sumAt = cssValuePoolAdd(pool, sum);
    cssValuePoolAdd(pool, cssNumberValue(PrCssValueKind_Percentage, PrCssUnit_Number, percent));
    cssValuePoolAdd(pool, pxValue(px));
    pool->values.data[sumAt].next = pool->values.count - sumAt;
    pool->values.data[root].next = pool->values.count - root;
}

// Returns true if the value at `index` is a zero length or percentage.
bool isZeroLength(CssValueRun run, uint32_t index) {
    const CssValueNode* value = &run.values[index];
    return (value->kind == PrCssValueKind_Dimension || value->kind == PrCssValueKind_Percentage
            || value->kind == PrCssValueKind_Number || value->kind == PrCssValueKind_Integer)
           && value->number == 0;
}

// Adds the computed value of a basic shape function of a clip-path, whose lengths are already absolute
// (css-shapes-1/#basic-shape-computed-values). xywh() and rect() compute to the equivalent inset(), whose insets are
// written in the fewest values that give them, and a `round` clause whose radii are all zero is dropped. A radius that
// a calculation makes negative is zero, and a center is a pair of length-percentages.
void addShape(CssValuePool* pool, CssValueRun run, uint32_t index) {
    const CssValueNode* function = &run.values[index];
    PrCssKeyword name = cssValueKeyword(function);
    uint32_t end = index + function->next;
    // The arguments up to `round`, and the radii after it.
    uint32_t args[8];
    uint32_t count = 0;
    uint32_t round = kArrayMissing;
    for (uint32_t i = index + 1; i < end; i += run.values[i].next) {
        if (run.values[i].kind == PrCssValueKind_Keyword && cssValueKeyword(&run.values[i]) == PrCssKeyword_Round) {
            round = i;
            break;
        }
        if (count < 8)
            args[count++] = i;
    }
    bool roundsNothing = round != kArrayMissing;
    for (uint32_t i = round != kArrayMissing ? round + 1 : end; i < end; i += run.values[i].next)
        roundsNothing = roundsNothing && (cssValueIsLiteral(&run.values[i], '/') || isZeroLength(run, i));
    auto addRound = [&]() {
        if (round == kArrayMissing || roundsNothing)
            return;
        cssValuePoolAddRun(pool, CssValueRun{run.values + round, end - round, run.text});
    };
    bool rect = name == PrCssKeyword_Rect;
    if ((name == PrCssKeyword_Xywh || rect) && count == 4) {
        // Each argument as a percentage and a length: auto is 0% for a near edge and 100% for a far one.
        double percent[4] = {};
        double px[4] = {};
        for (uint32_t k = 0; k < 4; ++k) {
            const CssValueNode* value = &run.values[args[k]];
            if (value->kind == PrCssValueKind_Keyword) {
                percent[k] = rect && (k == 1 || k == 2) ? 100 : 0;
                continue;
            }
            double none = 0;
            double whole = 0;
            cssResolveLength(run, args[k], 0, &none);
            cssResolveLength(run, args[k], 100, &whole);
            px[k] = none;
            percent[k] = whole - none;
        }
        // The insets from the top, right, bottom and left.
        double insetPercent[4];
        double insetPx[4];
        bool keep[4] = {false, true, true, false};
        uint32_t near[4] = {kArrayMissing, kArrayMissing, kArrayMissing, kArrayMissing};
        auto set = [&](uint32_t side, double p, double length, uint32_t from) {
            insetPercent[side] = p;
            insetPx[side] = length;
            near[side] = from;
        };
        if (rect) {
            // rect(t r b l) is inset(t calc(100% - r) calc(100% - b) l).
            set(0, percent[0], px[0], args[0]);
            set(1, 100 - percent[1], -px[1], kArrayMissing);
            set(2, 100 - percent[2], -px[2], kArrayMissing);
            set(3, percent[3], px[3], args[3]);
        } else {
            // xywh(x y w h) is inset(y calc(100% - x - w) calc(100% - y - h) x).
            set(0, percent[1], px[1], args[1]);
            set(1, 100 - percent[0] - percent[2], -px[0] - px[2], kArrayMissing);
            set(2, 100 - percent[1] - percent[3], -px[1] - px[3], kArrayMissing);
            set(3, percent[0], px[0], args[0]);
        }
        CssValuePool insets[4] = {};
        for (uint32_t k = 0; k < 4; ++k) {
            const CssValueNode* original = near[k] != kArrayMissing ? &run.values[near[k]] : nullptr;
            if (original != nullptr && original->kind != PrCssValueKind_Keyword && original->kind != PrCssValueKind_Math
                && original->kind != PrCssValueKind_Number && original->kind != PrCssValueKind_Integer)
                cssValuePoolAdd(&insets[k], *original);
            else if (original != nullptr && original->kind != PrCssValueKind_Keyword && insetPercent[k] == 0)
                cssValuePoolAdd(&insets[k], pxValue(insetPx[k]));
            else
                addLengthSum(&insets[k], insetPercent[k], insetPx[k], keep[k] || original == nullptr);
        }
        auto same = [&](uint32_t a, uint32_t b) {
            return cssValueRunEquals(cssValuePoolRun(&insets[a], CssValueSpan{0, insets[a].values.count}),
                                     cssValuePoolRun(&insets[b], CssValueSpan{0, insets[b].values.count}));
        };
        uint32_t written = !same(1, 3) ? 4 : !same(0, 2) ? 3 : !same(0, 1) ? 2 : 1;
        uint32_t at = cssValuePoolAdd(pool, cssFunctionValue(PrCssKeyword_Inset));
        for (uint32_t k = 0; k < written; ++k)
            cssValuePoolAddRun(pool, cssValuePoolRun(&insets[k], CssValueSpan{0, insets[k].values.count}));
        addRound();
        pool->values.data[at].next = pool->values.count - at;
        for (CssValuePool& inset : insets)
            cssValuePoolReset(&inset);
        return;
    }
    uint32_t at = cssValuePoolAdd(pool, *function);
    bool beforeAt = true;
    for (uint32_t i = index + 1; i < end; i += run.values[i].next) {
        const CssValueNode* value = &run.values[i];
        bool lead = i == index + 1;
        beforeAt = beforeAt && !cssValueIsKeyword(value, PrCssKeyword_At);
        // A circle's or an ellipse's radius that a calculation makes negative is zero.
        bool radius = (name == PrCssKeyword_Circle || name == PrCssKeyword_Ellipse) && beforeAt;
        if (radius && (value->kind == PrCssValueKind_Math || value->kind == PrCssValueKind_Dimension)) {
            double px = 0;
            if (cssMathTo(run, i, PrCssUnit_Px, &px) && px < 0) {
                cssValuePoolAdd(pool, pxValue(0));
                continue;
            }
        }
        // A circle's or an ellipse's center computes to a horizontal and a vertical length-percentage
        // (css-shapes-1/#basic-shape-computed-values).
        if (!beforeAt) {
            cssValuePoolAdd(pool, *value);
            addPosition(pool, CssValueRun{run.values + i + value->next, end - i - value->next, run.text});
            break;
        }
        // `round 0` in a polygon() rounds nothing, so it is dropped. Without a fill rule before it, the comma after it
        // is dropped too, and the polygon has no prelude.
        if (name == PrCssKeyword_Polygon && value->kind == PrCssValueKind_Keyword
            && cssValueKeyword(value) == PrCssKeyword_Round && i + value->next < end
            && isZeroLength(run, i + value->next)) {
            i += value->next;
            uint32_t after = i + run.values[i].next;
            if (lead && after < end && cssValueIsLiteral(&run.values[after], ','))
                i = after;
            continue;
        }
        cssValuePoolAddRun(pool, cssValueSubtree(run, i));
    }
    pool->values.data[at].next = pool->values.count - at;
}

// Returns true if `property` is the radius of a physical corner.
bool isCornerRadius(PrCssProperty property) {
    return property == PrCssProperty_BorderTopLeftRadius || property == PrCssProperty_BorderTopRightRadius
           || property == PrCssProperty_BorderBottomRightRadius || property == PrCssProperty_BorderBottomLeftRadius;
}

// Adds the computed value of a clip-path: its lengths absolute, and each basic shape as addShape computes it.
void computeClipPath(CssValuePool* pool, CssValueRun specified, const CssStyleContext& context) {
    CssValuePool absolute{};
    addAbsolute(&absolute, specified, context);
    CssValueRun run = cssValuePoolRun(&absolute, CssValueSpan{0, absolute.values.count});
    for (uint32_t i = 0; i < run.count; i += run.values[i].next) {
        const CssValueNode* value = &run.values[i];
        bool shape = value->kind == PrCssValueKind_Function
                     && (cssValueKeyword(value) == PrCssKeyword_Inset || cssValueKeyword(value) == PrCssKeyword_Xywh
                         || cssValueKeyword(value) == PrCssKeyword_Rect || cssValueKeyword(value) == PrCssKeyword_Circle
                         || cssValueKeyword(value) == PrCssKeyword_Ellipse
                         || cssValueKeyword(value) == PrCssKeyword_Polygon);
        if (shape)
            addShape(pool, run, i);
        else
            cssValuePoolAddRun(pool, cssValueSubtree(run, i));
    }
    cssValuePoolReset(&absolute);
}

// Adds the computed value of a <filter-value-list> (filter-effects-1/#computed-values-of-filter-functions). Each filter
// function takes its omitted arguments' defaults, its amount as a number, clamped if the function clamps it, and its
// lengths absolute. drop-shadow() takes its color first, then its three lengths. A calculation that does not resolve
// here stays as it is, and so does a URL.
void computeFilter(CssValuePool* pool, CssValueRun specified, const CssStyleContext& context) {
    CssValuePool absolute{};
    addAbsolute(&absolute, specified, context);
    CssValueRun run = cssValuePoolRun(&absolute, CssValueSpan{0, absolute.values.count});
    for (uint32_t i = 0; i < run.count; i += run.values[i].next) {
        const CssValueNode* value = &run.values[i];
        CssValueRun item{value, value->next, run.text};
        PrCssKeyword name = value->kind == PrCssValueKind_Function ? cssValueKeyword(value) : PrCssKeyword_Count;
        uint32_t first = i + 1;
        bool given = value->next > 1;
        // Adds the length at `index` in px, or as it is if it does not resolve.
        auto addLength = [&](uint32_t index) {
            double px = 0;
            if (cssResolveLength(run, index, 0, &px))
                cssValuePoolAdd(pool, pxValue(px));
            else
                cssValuePoolAddRun(pool, CssValueRun{run.values + index, run.values[index].next, run.text});
        };
        uint32_t at = pool->values.count;
        switch (name) {
            case PrCssKeyword_Blur:
                cssValuePoolAdd(pool, *value);
                if (given)
                    addLength(first);
                else
                    cssValuePoolAdd(pool, pxValue(0));
                break;
            case PrCssKeyword_HueRotate: {
                cssValuePoolAdd(pool, *value);
                // A zero may be written without a unit.
                double degrees = 0;
                bool zero = given && (run.values[first].kind == PrCssValueKind_Number
                                      || run.values[first].kind == PrCssValueKind_Integer);
                if (!given || zero || cssMathTo(run, first, PrCssUnit_Deg, &degrees))
                    cssValuePoolAdd(pool, cssNumberValue(PrCssValueKind_Dimension, PrCssUnit_Deg, degrees));
                else
                    cssValuePoolAddRun(pool, CssValueRun{run.values + first, run.values[first].next, run.text});
                break;
            }
            case PrCssKeyword_Brightness:
            case PrCssKeyword_Contrast:
            case PrCssKeyword_Saturate:
            case PrCssKeyword_Grayscale:
            case PrCssKeyword_Invert:
            case PrCssKeyword_Opacity:
            case PrCssKeyword_Sepia: {
                cssValuePoolAdd(pool, *value);
                bool clamps = name == PrCssKeyword_Grayscale || name == PrCssKeyword_Invert
                              || name == PrCssKeyword_Opacity || name == PrCssKeyword_Sepia;
                double amount = 1;
                if (given && !opacityOf(CssValueRun{run.values + first, run.values[first].next, run.text}, &amount)) {
                    cssValuePoolAddRun(pool, CssValueRun{run.values + first, run.values[first].next, run.text});
                    break;
                }
                amount = maxF64(amount, 0);
                cssValuePoolAdd(pool, cssNumberValue(PrCssValueKind_Number, PrCssUnit_Number,
                                                     clamps ? minF64(amount, 1) : amount));
                break;
            }
            case PrCssKeyword_DropShadow: {
                cssValuePoolAdd(pool, *value);
                uint32_t lengths[3] = {kArrayMissing, kArrayMissing, kArrayMissing};
                uint32_t count = 0;
                uint32_t color = kArrayMissing;
                for (uint32_t k = first; k < i + value->next; k += run.values[k].next) {
                    const CssValueNode* argument = &run.values[k];
                    bool length = argument->kind == PrCssValueKind_Dimension
                                  || (argument->kind == PrCssValueKind_Math
                                      && cssValueMathCategory(argument) == CssUnitCategory_Length)
                                  || argument->kind == PrCssValueKind_Number
                                  || argument->kind == PrCssValueKind_Integer;
                    if (length && count < 3)
                        lengths[count++] = k;
                    else if (!length)
                        color = k;
                }
                // A named color computes to its sRGB value, and any other color as the color property's does.
                PrColor named{};
                CssValueRun colorRun = color != kArrayMissing
                                           ? CssValueRun{run.values + color, run.values[color].next, run.text}
                                           : CssValueRun{};
                if (color == kArrayMissing)
                    cssValuePoolAdd(pool, cssKeywordValue(PrCssKeyword_Currentcolor));
                else if (colorRun.values[0].kind == PrCssValueKind_Keyword && !readsElement(colorRun, false)
                         && cssResolveColor(colorRun, 0, CssColorContext{.current = context.parentColor}, &named))
                    cssValuePoolAddColor(pool, color8Of(named));
                else
                    cssValuePoolAddRun(pool, colorRun);
                for (uint32_t k = 0; k < 3; ++k) {
                    if (lengths[k] != kArrayMissing)
                        addLength(lengths[k]);
                    else
                        cssValuePoolAdd(pool, pxValue(0));
                }
                break;
            }
            default:
                cssValuePoolAddRun(pool, item);
                continue;
        }
        pool->values.data[at].next = pool->values.count - at;
    }
    cssValuePoolReset(&absolute);
}

// Returns true if `property` is an SVG property whose lengths may be written as numbers alone, in px
// (SVG2/painting.html#StrokeProperties): the stroke's width, dashes and dash offset.
bool isSvgLength(PrCssProperty property) {
    return property == PrCssProperty_StrokeWidth || property == PrCssProperty_StrokeDasharray
           || property == PrCssProperty_StrokeDashoffset || property == PrCssProperty_PathLength;
}

// Adds the computed lengths of `specified`, a stroke's width, dash offset or dashes, or a path-length: absolute lengths
// or percentages, with a number alone taken as px. Only a dash offset may be negative, so a calculation of any other
// that resolves below zero computes to zero (css-values-4/#calc-range). A dash array's lengths are separated by commas
// (SVG2/painting.html#StrokeDasharrayProperty), however they were written.
void computeSvgLengths(CssValuePool* pool, PrCssProperty property, CssValueRun specified,
                       const CssStyleContext& context) {
    CssValuePool absolute{};
    addAbsolute(&absolute, specified, context);
    CssValueRun run = cssValuePoolRun(&absolute, CssValueSpan{0, absolute.values.count});
    bool nonNegative = property != PrCssProperty_StrokeDashoffset;
    bool dashes = property == PrCssProperty_StrokeDasharray;
    bool first = true;
    for (uint32_t i = 0; i < run.count; i += run.values[i].next) {
        const CssValueNode* value = &run.values[i];
        if (value->kind == PrCssValueKind_Literal)
            continue;
        if (dashes && !first)
            cssValuePoolAdd(pool, cssLiteralValue(','));
        first = false;
        if (value->kind == PrCssValueKind_Number || value->kind == PrCssValueKind_Integer) {
            cssValuePoolAdd(pool, pxValue(nonNegative ? maxF64(value->number, 0) : value->number));
        } else if (nonNegative && value->kind == PrCssValueKind_Dimension && value->number < 0) {
            cssValuePoolAdd(pool, pxValue(0));
        } else {
            cssValuePoolAddRun(pool, cssValueSubtree(run, i));
        }
    }
    cssValuePoolReset(&absolute);
}

// Returns true if `specified` is a value of a property that takes <image> values, and an item of it is filter()
// (filter-effects-1/#FilterCSSImageValue).
bool hasFilterImage(PrCssProperty property, CssValueRun specified) {
    bool images = property == PrCssProperty_BackgroundImage || property == PrCssProperty_MaskImage
                  || property == PrCssProperty_ListStyleImage || property == PrCssProperty_BorderImageSource
                  || property == PrCssProperty_MaskBorderSource || property == PrCssProperty_Content;
    for (uint32_t i = 0; images && i < specified.count; i += specified.values[i].next) {
        if (cssValueIsFunction(&specified.values[i], PrCssKeyword_Filter))
            return true;
    }
    return false;
}

// Adds the computed value of a value that holds images. A filter() takes its input's computed value and its filter
// list's (filter-effects-1/#FilterCSSImageValue), and anything else takes its lengths absolute.
void computeImages(CssValuePool* pool, CssValueRun specified, const CssStyleContext& context) {
    for (uint32_t i = 0; i < specified.count; i += specified.values[i].next) {
        const CssValueNode* value = &specified.values[i];
        CssValueRun item = cssValueSubtree(specified, i);
        if (!cssValueIsFunction(value, PrCssKeyword_Filter)) {
            addAbsolute(pool, item, context);
            continue;
        }
        uint32_t at = cssValuePoolAdd(pool, *value);
        uint32_t comma = 1;
        while (comma < item.count && !cssValueIsLiteral(&item.values[comma], ','))
            comma += item.values[comma].next;
        computeImages(pool, CssValueRun{item.values + 1, comma - 1, item.text}, context);
        if (comma < item.count) {
            cssValuePoolAdd(pool, item.values[comma]);
            computeFilter(pool, CssValueRun{item.values + comma + 1, item.count - comma - 1, item.text}, context);
        }
        pool->values.data[at].next = pool->values.count - at;
    }
}

// Adds the computed paint-order (SVG2/painting.html#PaintOrder): the layers in the order given, each one not given
// after them in the order fill, stroke, markers. It is written as the fewest keywords that imply that order, which is
// the order without its last keyword, and without the one before it as well if it follows the normal order.
void computePaintOrder(CssValuePool* pool, CssValueRun specified) {
    PrCssKeyword order[3] = {};
    uint32_t count = 0;
    for (uint32_t i = 0; i < specified.count && count < 3; i += specified.values[i].next) {
        if (specified.values[i].kind == PrCssValueKind_Keyword)
            order[count++] = cssValueKeyword(&specified.values[i]);
    }
    constexpr PrCssKeyword kNormal[] = {PrCssKeyword_Fill, PrCssKeyword_Stroke, PrCssKeyword_Markers};
    for (PrCssKeyword layer : kNormal) {
        bool given = false;
        for (uint32_t i = 0; i < count; ++i)
            given = given || order[i] == layer;
        if (!given)
            order[count++] = layer;
    }
    // The last layer is implied by the first two, and the second by the first if the second and third are in the normal
    // order.
    uint32_t written = 2;
    auto rank = [&](PrCssKeyword layer) {
        return layer == PrCssKeyword_Fill ? 0 : layer == PrCssKeyword_Stroke ? 1 : 2;
    };
    if (rank(order[1]) < rank(order[2]))
        written = 1;
    for (uint32_t i = 0; i < written; ++i)
        cssValuePoolAdd(pool, cssKeywordValue(order[i]));
}

// Adds the computed value of a <paint> (SVG2/painting.html#SpecifyingPaint): as specified, with each color absolute as
// a color property's computes, and each URL as resolved.
void computePaint(CssValuePool* pool, CssValueRun specified, const CssStyleContext& context) {
    for (uint32_t i = 0; i < specified.count; i += specified.values[i].next) {
        CssValueRun value = cssValueSubtree(specified, i);
        const CssValueNode* node = value.values;
        PrCssKeyword keyword = node->kind == PrCssValueKind_Keyword ? cssValueKeyword(node) : PrCssKeyword_Count;
        bool literal = node->kind == PrCssValueKind_Url || node->kind == PrCssValueKind_Color
                       || cssValueIsFunction(node, PrCssKeyword_Url) || cssValueIsFunction(node, PrCssKeyword_Src)
                       || keyword == PrCssKeyword_None || keyword == PrCssKeyword_ContextFill
                       || keyword == PrCssKeyword_ContextStroke || keyword == PrCssKeyword_Currentcolor;
        if (literal) {
            cssValuePoolAddRun(pool, value);
            continue;
        }
        PrColor color{};
        CssColor components{};
        if (keyword != PrCssKeyword_Count && cssIsSystemColor(keyword)) {
            cssValuePoolAdd(pool, cssKeywordValue(cssSystemColorComputed(keyword)));
        } else if (keyword != PrCssKeyword_Count
                   && cssResolveColor(value, 0, CssColorContext{.current = context.parentColor}, &color)) {
            cssValuePoolAddColor(pool, color8Of(color));
        } else if (node->kind == PrCssValueKind_Function && !readsElement(value, false)
                   && resolveColorValue(value, 0, CssColorContext{.current = context.parentColor}, &components)) {
            cssValuePoolAddColorComponents(pool, components);
        } else {
            cssValuePoolAddRun(pool, value);
        }
    }
}

// Returns the generic family that a keyword of font-family names, or None for a keyword that names none.
PrFontGeneric genericOf(PrCssKeyword keyword) {
    switch (keyword) {
        case PrCssKeyword_Serif: return PrFontGeneric_Serif;
        case PrCssKeyword_SansSerif: return PrFontGeneric_SansSerif;
        case PrCssKeyword_Monospace: return PrFontGeneric_Monospace;
        case PrCssKeyword_Cursive: return PrFontGeneric_Cursive;
        case PrCssKeyword_Fantasy: return PrFontGeneric_Fantasy;
        case PrCssKeyword_SystemUi: return PrFontGeneric_SystemUi;
        case PrCssKeyword_Math: return PrFontGeneric_Math;
        case PrCssKeyword_Emoji: return PrFontGeneric_Emoji;
        case PrCssKeyword_UiSerif: return PrFontGeneric_UiSerif;
        case PrCssKeyword_UiSansSerif: return PrFontGeneric_UiSansSerif;
        case PrCssKeyword_UiMonospace: return PrFontGeneric_UiMonospace;
        case PrCssKeyword_UiRounded: return PrFontGeneric_UiRounded;
        case PrCssKeyword_Fangsong: return PrFontGeneric_Fangsong;
        case PrCssKeyword_Kai: return PrFontGeneric_Kai;
        case PrCssKeyword_KhmerMul: return PrFontGeneric_KhmerMul;
        case PrCssKeyword_Nastaliq: return PrFontGeneric_Nastaliq;
        default: return PrFontGeneric_None;
    }
}

// The feature that a keyword of font-variant-ligatures, -numeric, -east-asian or -alternates turns on or off
// (css-fonts-4/#font-variant-ligatures-prop and the sections after it), with `on` false for a no- keyword, or a tag of
// zero for any other keyword.
struct VariantFeature {
    PrCssKeyword keyword;
    uint32_t tag;
    bool on;
};

constexpr VariantFeature kVariantFeatures[] = {
    {PrCssKeyword_CommonLigatures, PR_FONT_TAG('l', 'i', 'g', 'a'), true},
    {PrCssKeyword_NoCommonLigatures, PR_FONT_TAG('l', 'i', 'g', 'a'), false},
    {PrCssKeyword_DiscretionaryLigatures, PR_FONT_TAG('d', 'l', 'i', 'g'), true},
    {PrCssKeyword_NoDiscretionaryLigatures, PR_FONT_TAG('d', 'l', 'i', 'g'), false},
    {PrCssKeyword_HistoricalLigatures, PR_FONT_TAG('h', 'l', 'i', 'g'), true},
    {PrCssKeyword_NoHistoricalLigatures, PR_FONT_TAG('h', 'l', 'i', 'g'), false},
    {PrCssKeyword_Contextual, PR_FONT_TAG('c', 'a', 'l', 't'), true},
    {PrCssKeyword_NoContextual, PR_FONT_TAG('c', 'a', 'l', 't'), false},
    {PrCssKeyword_LiningNums, PR_FONT_TAG('l', 'n', 'u', 'm'), true},
    {PrCssKeyword_OldstyleNums, PR_FONT_TAG('o', 'n', 'u', 'm'), true},
    {PrCssKeyword_ProportionalNums, PR_FONT_TAG('p', 'n', 'u', 'm'), true},
    {PrCssKeyword_TabularNums, PR_FONT_TAG('t', 'n', 'u', 'm'), true},
    {PrCssKeyword_DiagonalFractions, PR_FONT_TAG('f', 'r', 'a', 'c'), true},
    {PrCssKeyword_StackedFractions, PR_FONT_TAG('a', 'f', 'r', 'c'), true},
    {PrCssKeyword_Ordinal, PR_FONT_TAG('o', 'r', 'd', 'n'), true},
    {PrCssKeyword_SlashedZero, PR_FONT_TAG('z', 'e', 'r', 'o'), true},
    {PrCssKeyword_Jis78, PR_FONT_TAG('j', 'p', '7', '8'), true},
    {PrCssKeyword_Jis83, PR_FONT_TAG('j', 'p', '8', '3'), true},
    {PrCssKeyword_Jis90, PR_FONT_TAG('j', 'p', '9', '0'), true},
    {PrCssKeyword_Jis04, PR_FONT_TAG('j', 'p', '0', '4'), true},
    {PrCssKeyword_Simplified, PR_FONT_TAG('s', 'm', 'p', 'l'), true},
    {PrCssKeyword_Traditional, PR_FONT_TAG('t', 'r', 'a', 'd'), true},
    {PrCssKeyword_FullWidth, PR_FONT_TAG('f', 'w', 'i', 'd'), true},
    {PrCssKeyword_ProportionalWidth, PR_FONT_TAG('p', 'w', 'i', 'd'), true},
    {PrCssKeyword_Ruby, PR_FONT_TAG('r', 'u', 'b', 'y'), true},
    {PrCssKeyword_HistoricalForms, PR_FONT_TAG('h', 'i', 's', 't'), true},
};

void pushFeature(Array<PrTextFeature>& features, uint32_t tag, uint32_t value) {
    features.push(PrTextFeature{.tag = tag, .value = value, .start = 0, .end = UINT32_MAX});
}

// Appends the features that the keywords of a computed font-variant longhand turn on or off.
void variantFeatures(CssValueRun run, Array<PrTextFeature>& features) {
    for (uint32_t i = 0; i < run.count; i += run.values[i].next) {
        const CssValueNode* value = &run.values[i];
        if (value->kind != PrCssValueKind_Keyword)
            continue;
        PrCssKeyword keyword = cssValueKeyword(value);
        // font-variant-ligatures: none turns off every ligature and contextual alternate.
        if (keyword == PrCssKeyword_None) {
            static constexpr uint32_t kLigatures[] = {PR_FONT_TAG('l', 'i', 'g', 'a'), PR_FONT_TAG('c', 'l', 'i', 'g'),
                                                      PR_FONT_TAG('d', 'l', 'i', 'g'), PR_FONT_TAG('h', 'l', 'i', 'g'),
                                                      PR_FONT_TAG('c', 'a', 'l', 't')};
            for (uint32_t tag : kLigatures)
                pushFeature(features, tag, 0);
            continue;
        }
        for (const VariantFeature& f : kVariantFeatures) {
            if (f.keyword != keyword)
                continue;
            pushFeature(features, f.tag, f.on ? 1 : 0);
            // Common ligatures are liga and clig together.
            if (f.tag == PR_FONT_TAG('l', 'i', 'g', 'a'))
                pushFeature(features, PR_FONT_TAG('c', 'l', 'i', 'g'), f.on ? 1 : 0);
            break;
        }
    }
}

PrCssKeyword keywordOfRun(CssValueRun run) {
    return run.count != 0 && run.values[0].kind == PrCssValueKind_Keyword ? cssValueKeyword(run.values)
                                                                          : PrCssKeyword_Count;
}

} // namespace

CssStyleSlot cssStyleSlot(PrCssProperty property) {
    PR_ASSERT(property < PrCssProperty_Count);
    return kCssStyleSlots[property];
}

CssStyleGroupProperties cssStyleGroupProperties(CssStyleGroup group) {
    return kCssStyleGroupProperties[group];
}

uint32_t cssCustomHash(PrStringView name) {
    return uint32_t(hashBytes(name.data, name.size, 0));
}

bool cssCustomBefore(uint32_t hashA, PrStringView a, uint32_t hashB, PrStringView b) {
    if (hashA != hashB)
        return hashA < hashB;
    size_t size = a.size < b.size ? a.size : b.size;
    int order = size != 0 ? memCompare(a.data, b.data, size) : 0;
    return order != 0 ? order < 0 : a.size < b.size;
}

const CssCustomProperties* cssCustomPropertiesCreate(CssStyleTable* table, const CssCustomProperties* PR_NULLABLE base,
                                                     const CssCustomRow* PR_NULLABLE rows, uint32_t count) {
    PR_ASSERT(base == nullptr || base->base == nullptr);
    uint64_t hash = kCustomKeySeed ^ uint64_t(uintptr_t(base));
    for (uint32_t i = 0; i < count; ++i) {
        hash = hashBytes(rows[i].name.data, rows[i].name.size, hash ^ rows[i].hash);
        hash = rows[i].invalid ? hash * kCustomContentsMix : hashBytes(rows[i].value.data, rows[i].value.size, hash);
    }
    uint32_t key = uint32_t(hash);
    CssCustomTablePart& part = table->customParts[key >> (32 - kCssStyleTablePartBits)];
    prMutexLock(&part.mutex);
    // A set whose last reference was released is being removed from the table, and is treated as absent.
    uint32_t found = part.index.find(key, [&](uint32_t at) {
        const CssCustomProperties* candidate = part.sets.data[at];
        return candidate->base == base && sameRows(candidate, rows, count) && retainLive(candidate);
    });
    if (found != kHashMissing) {
        const CssCustomProperties* existing = part.sets.data[found];
        prMutexUnlock(&part.mutex);
        return existing;
    }
    size_t bytes = 0;
    for (uint32_t i = 0; i < count; ++i)
        bytes += rows[i].name.size + (rows[i].invalid ? 0 : rows[i].value.size);
    size_t entriesBytes = sizeof(CssCustomProperties) + sizeof(CssCustomEntry) * count;
    uint8_t* memory = static_cast<uint8_t*>(prMemoryAlloc(entriesBytes + bytes));
    CssCustomProperties* made = reinterpret_cast<CssCustomProperties*>(memory);
    made->references.storeRelaxed(1);
    made->base = base;
    if (base != nullptr)
        cssCustomPropertiesRetain(base);
    made->count = count;
    made->entries = reinterpret_cast<CssCustomEntry*>(memory + sizeof(CssCustomProperties));
    made->text = reinterpret_cast<char*>(memory + entriesBytes);
    made->contents.storeRelaxed(0);
    uint32_t at = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const CssCustomRow& row = rows[i];
        PR_ASSERT(i == 0 || cssCustomBefore(rows[i - 1].hash, rows[i - 1].name, row.hash, row.name));
        CssCustomEntry& entry = made->entries[i];
        entry.hash = row.hash;
        entry.name = at;
        entry.nameSize = uint32_t(row.name.size);
        if (row.name.size != 0)
            memCopy(made->text + at, row.name.data, row.name.size);
        at += entry.nameSize;
        entry.value = at;
        entry.valueSize = row.invalid ? kCssCustomInvalid : uint32_t(row.value.size);
        if (!row.invalid && row.value.size != 0) {
            memCopy(made->text + at, row.value.data, row.value.size);
            at += uint32_t(row.value.size);
        }
    }
    made->key = key;
    made->owner = table;
    uint32_t slot = 0;
    if (part.free.count != 0) {
        slot = part.free.last();
        part.free.pop();
        part.sets.data[slot] = made;
    } else {
        slot = part.sets.count;
        part.sets.push(made);
    }
    made->slot = slot;
    part.index.insert(key, slot);
    prMutexUnlock(&part.mutex);
    return made;
}

bool cssCustomValue(const CssCustomProperties* PR_NULLABLE custom, PrStringView name, PrStringView* out) {
    uint32_t hash = cssCustomHash(name);
    for (const CssCustomProperties* set = custom; set != nullptr; set = set->base) {
        uint32_t at = findEntry(set, hash, name);
        if (at == kHashMissing)
            continue;
        const CssCustomEntry& entry = set->entries[at];
        if (entry.valueSize == kCssCustomInvalid)
            return false;
        *out = PrStringView{set->text + entry.value, entry.valueSize};
        return true;
    }
    return false;
}

bool cssCustomPropertiesEqual(const CssCustomProperties* PR_NULLABLE a, const CssCustomProperties* PR_NULLABLE b) {
    if (a == b)
        return true;
    // Over one base, the sets can differ only at the names of their own entries.
    Layers x = layersOf(a);
    Layers y = layersOf(b);
    if (x.base == y.base)
        return sameOwnEntries(x, y) || (ownEntriesAgree(x, y) && ownEntriesAgree(y, x));
    if (contentsOf(a) != contentsOf(b))
        return false;
    CssCustomCursor p = cssCustomCursor(a);
    CssCustomCursor q = cssCustomCursor(b);
    for (;;) {
        CssCustomRow m{};
        CssCustomRow n{};
        bool more = p.next(&m);
        if (more != q.next(&n))
            return false;
        if (!more)
            return true;
        if (m.hash != n.hash || m.name != n.name || m.value != n.value)
            return false;
    }
}

void cssCustomPropertiesRetain(const CssCustomProperties* properties) {
    properties->references.fetchAddRelaxed(1);
}

void cssCustomPropertiesRelease(const CssCustomProperties* PR_NULLABLE properties) {
    if (properties == nullptr || properties->references.fetchSub(1) != 1)
        return;
    if (CssStyleTable* table = properties->owner) {
        CssCustomTablePart& part = table->customParts[properties->key >> (32 - kCssStyleTablePartBits)];
        prMutexLock(&part.mutex);
        part.index.erase(properties->key, properties->slot);
        part.sets.data[properties->slot] = nullptr;
        part.free.push(properties->slot);
        prMutexUnlock(&part.mutex);
    }
    cssCustomPropertiesRelease(properties->base);
    prMemoryFree(const_cast<CssCustomProperties*>(properties));
}

bool CssCustomCursor::next(CssCustomRow* out) {
    for (;;) {
        bool hasOwn = own != nullptr && at < own->count;
        bool hasBase = base != nullptr && baseAt < base->count;
        if (!hasOwn && !hasBase)
            return false;
        CssCustomRow row{};
        if (hasOwn && hasBase) {
            CssCustomRow mine = entryRow(own, own->entries[at]);
            CssCustomRow theirs = entryRow(base, base->entries[baseAt]);
            if (cssCustomBefore(theirs.hash, theirs.name, mine.hash, mine.name)) {
                row = theirs;
                ++baseAt;
            } else {
                // An own entry hides the base's entry of the same name.
                if (mine.hash == theirs.hash && mine.name == theirs.name)
                    ++baseAt;
                row = mine;
                ++at;
            }
        } else if (hasOwn) {
            row = entryRow(own, own->entries[at++]);
        } else {
            row = entryRow(base, base->entries[baseAt++]);
        }
        if (!row.invalid) {
            *out = row;
            return true;
        }
    }
}

CssCustomCursor cssCustomCursor(const CssCustomProperties* PR_NULLABLE custom) {
    return CssCustomCursor{custom, custom != nullptr ? custom->base : nullptr, 0, 0};
}

void cssComputedStyleSetFontBases(CssComputedStyle* style) {
    CssValueRun size = cssComputedValue(style, PrCssProperty_FontSize);
    double em = size.count == 1 ? size.values[0].number : kCssMediumFontSize;
    style->font = cssFontBases(em, cssLineHeightPx(cssComputedValue(style, PrCssProperty_LineHeight), em));
}

CssValueRun cssComputedValue(const CssComputedStyle* style, PrCssProperty property) {
    CssStyleSlot slot = cssStyleSlot(property);
    if (slot.group == CssStyleGroup_Count) {
        CssValueRun mode = cssComputedValue(style, PrCssProperty_WritingMode);
        CssValueRun direction = cssComputedValue(style, PrCssProperty_Direction);
        property = cssPhysicalProperty(property, cssValueKeyword(mode.values), cssValueKeyword(direction.values));
        slot = cssStyleSlot(property);
    }
    PR_ASSERT(slot.group != CssStyleGroup_Count);
    return cssStyleValue(style->groups[slot.group], slot.index);
}

PrCssProperty cssPhysicalProperty(PrCssProperty property, PrCssKeyword writingMode, PrCssKeyword direction) {
    if (!cssIsFlowRelative(property))
        return property;
    for (const CssSideFamily& family : kCssSideFamilies) {
        for (LogicalSide side = 0; side < 4; ++side) {
            if (family.logical[side] == property)
                return family.physical[physicalSide(side, writingMode, direction)];
        }
    }
    bool horizontal = physicalSide(LogicalSide_BlockStart, writingMode, direction) == PhysicalSide_Top;
    for (const CssAxisFamily& family : kCssAxisFamilies) {
        for (uint32_t axis = 0; axis < 2; ++axis) {
            if (family.logical[axis] == property)
                return family.physical[horizontal ? axis : 1 - axis];
        }
    }
    for (const CssCornerFamily& family : kCssCornerFamilies) {
        for (uint32_t block = 0; block < 2; ++block) {
            for (uint32_t inlineSide = 0; inlineSide < 2; ++inlineSide) {
                if (family.logical[block][inlineSide] != property)
                    continue;
                // The corner's two physical sides, one vertical and one horizontal.
                PhysicalSide a = physicalSide(block == 0 ? LogicalSide_BlockStart : LogicalSide_BlockEnd, writingMode,
                                              direction);
                PhysicalSide b = physicalSide(inlineSide == 0 ? LogicalSide_InlineStart : LogicalSide_InlineEnd,
                                              writingMode, direction);
                PhysicalSide vertical = a == PhysicalSide_Top || a == PhysicalSide_Bottom ? a : b;
                PhysicalSide horizontalSide = vertical == a ? b : a;
                return family.physical[vertical == PhysicalSide_Bottom][horizontalSide == PhysicalSide_Right];
            }
        }
    }
    return property;
}

bool cssIsFlowRelative(PrCssProperty property) {
    return bitsetTest(kCssPropertyFlowRelative, property);
}

void cssComputedStyleRetain(const CssComputedStyle* style) {
    style->references.fetchAddRelaxed(1);
}

void cssComputedStyleRelease(const CssComputedStyle* PR_NULLABLE style) {
    if (style == nullptr || style->references.fetchSub(1) != 1)
        return;
    for (uint32_t g = 0; g < CssStyleGroup_Count; ++g)
        cssStyleValuesRelease(style->groups[g]);
    cssCustomPropertiesRelease(style->custom);
    cssImportantPropertiesRelease(style->important);
    prMemoryFree(const_cast<CssComputedStyle*>(style));
}

bool cssStyleIsImportant(const CssComputedStyle* style, PrCssProperty property) {
    const CssImportantProperties* important = style->important;
    if (important == nullptr)
        return false;
    uint32_t low = lowerBound(important->properties, important->count, property);
    return low < important->count && important->properties[low] == property;
}

const CssImportantProperties* cssImportantPropertiesCreate(const PrCssProperty* properties, uint32_t count) {
    CssImportantProperties* important = static_cast<CssImportantProperties*>(
        prMemoryAlloc(sizeof(CssImportantProperties) + sizeof(PrCssProperty) * count));
    important->references.storeRelaxed(1);
    important->count = count;
    memCopy(important->properties, properties, sizeof(PrCssProperty) * count);
    return important;
}

void cssImportantPropertiesRetain(const CssImportantProperties* important) {
    important->references.fetchAddRelaxed(1);
}

void cssImportantPropertiesRelease(const CssImportantProperties* PR_NULLABLE important) {
    if (important != nullptr && important->references.fetchSub(1) == 1)
        prMemoryFree(const_cast<CssImportantProperties*>(important));
}

void cssStyleTableInit(CssStyleTable* table) {
    for (CssStyleTablePart& part : table->parts)
        prMutexInit(&part.mutex);
    for (CssCustomTablePart& part : table->customParts)
        prMutexInit(&part.mutex);
    CssStyleBuilder builder{};
    builder.table = table;
    for (uint32_t g = 0; g < CssStyleGroup_Count; ++g)
        table->initial[g] = computeInitial(&builder, CssStyleGroup(g));
    cssStyleBuilderReset(&builder);
}

void cssStyleTableReset(CssStyleTable* table) {
    for (uint32_t g = 0; g < CssStyleGroup_Count; ++g) {
        cssStyleValuesRelease(table->initial[g]);
        table->initial[g] = nullptr;
    }
    // The groups that elements still hold outlive the table, and belong to no index.
    for (CssStyleTablePart& part : table->parts) {
        for (uint32_t i = 0; i < part.groups.count; ++i) {
            if (part.groups.data[i] != nullptr)
                part.groups.data[i]->owner = nullptr;
        }
        part.groups.reset();
        part.index.reset();
        part.free.reset();
        prMutexReset(&part.mutex);
    }
    for (CssCustomTablePart& part : table->customParts) {
        for (uint32_t i = 0; i < part.sets.count; ++i) {
            if (part.sets.data[i] != nullptr)
                part.sets.data[i]->owner = nullptr;
        }
        part.sets.reset();
        part.index.reset();
        part.free.reset();
        prMutexReset(&part.mutex);
    }
}

void cssStyleBuilderReset(CssStyleBuilder* builder) {
    cssValuePoolReset(&builder->pool);
    builder->spans.reset();
    for (uint32_t g = 0; g < CssStyleGroup_Count; ++g) {
        for (const CssStyleValues*& recent : builder->recent[g]) {
            cssStyleValuesRelease(recent);
            recent = nullptr;
        }
    }
}

const CssStyleValues* cssStyleInitial(CssStyleBuilder* builder, CssStyleGroup group) {
    const CssStyleValues* initial = builder->table->initial[group];
    cssStyleValuesRetain(initial);
    return initial;
}

void cssStyleBegin(CssStyleBuilder* builder) {
    builder->pool.values.count = 0;
    builder->pool.text.count = 0;
    builder->spans.clear();
}

void cssStyleAddComputed(CssStyleBuilder* builder, PrCssProperty property, CssValueRun specified,
                         const CssStyleContext& context) {
    uint32_t start = builder->pool.values.count;
    cssStyleComputeValue(&builder->pool, property, specified, context);
    builder->spans.push(CssValueSpan{start, builder->pool.values.count - start});
}

void cssStyleComputeValue(CssValuePool* pool, PrCssProperty property, CssValueRun specified,
                          const CssStyleContext& context) {
    PrCssKeyword keyword = cssValueRunKeyword(specified);
    if (property == PrCssProperty_FontSize) {
        cssValuePoolAdd(pool, pxValue(computeFontSize(specified, context)));
    } else if (property == PrCssProperty_FontWeight) {
        cssValuePoolAdd(pool, cssNumberValue(PrCssValueKind_Number, PrCssUnit_Number,
                                             double(computeFontWeight(specified, context))));
    } else if (property == PrCssProperty_FontWidth) {
        cssValuePoolAdd(pool,
                        cssNumberValue(PrCssValueKind_Percentage, PrCssUnit_Number, computeFontWidth(specified)));
    } else if (property == PrCssProperty_LineHeight) {
        computeLineHeight(pool, specified, context);
    } else if (isLineWidth(property)) {
        // An absolute length, never negative (css-backgrounds-3/#border-width), which a calculation clamps to
        // (css-values-4/#calc-range). Layout snaps it as a border width at the target pixel ratio, so the computed
        // value is independent of the ratio, and its resolved value is snapped.
        uint32_t start = pool->values.count;
        if (keyword == PrCssKeyword_Thin || keyword == PrCssKeyword_Medium || keyword == PrCssKeyword_Thick) {
            float width = keyword == PrCssKeyword_Thin     ? kThinBorder
                          : keyword == PrCssKeyword_Medium ? kMediumBorder
                                                           : kThickBorder;
            cssValuePoolAdd(pool, pxValue(double(width)));
        } else {
            addAbsolute(pool, specified, context);
        }
        CssValueNode& width = pool->values.data[start];
        if (pool->values.count == start + 1 && width.kind == PrCssValueKind_Dimension && width.unit == PrCssUnit_Px
            && width.number < 0)
            width = pxValue(0);
    } else if ((property == PrCssProperty_ObjectPosition || property == PrCssProperty_BackgroundPosition
                || property == PrCssProperty_MaskPosition)
               && keyword != PrCssKeyword_Inherit) {
        // Each position of the list computes to two length-percentages, its lengths absolute. A list written that way
        // takes its lengths absolute and nothing more.
        bool keywords = false;
        for (uint32_t i = 0; i < specified.count && !keywords; ++i)
            keywords = specified.values[i].kind == PrCssValueKind_Keyword;
        if (!keywords) {
            addAbsolute(pool, specified, context);
        } else {
            CssValuePool absolute{};
            addAbsolute(&absolute, specified, context);
            CssValueRun run = cssValuePoolRun(&absolute, CssValueSpan{0, absolute.values.count});
            uint32_t start = 0;
            for (uint32_t i = 0; i <= run.count; i = i < run.count ? i + run.values[i].next : i + 1) {
                if (i < run.count && !cssValueIsLiteral(&run.values[i], ','))
                    continue;
                addPosition(pool, CssValueRun{run.values + start, i - start, run.text});
                if (i < run.count)
                    cssValuePoolAdd(pool, cssLiteralValue(','));
                start = i + 1;
            }
            cssValuePoolReset(&absolute);
        }
    } else if (property == PrCssProperty_WritingMode
               && (keyword == PrCssKeyword_Lr || keyword == PrCssKeyword_LrTb || keyword == PrCssKeyword_Rl
                   || keyword == PrCssKeyword_RlTb || keyword == PrCssKeyword_Tb || keyword == PrCssKeyword_TbRl)) {
        // SVG 1.1's values compute to the values that they stand for (css-writing-modes-4/#svg-writing-mode).
        bool vertical = keyword == PrCssKeyword_Tb || keyword == PrCssKeyword_TbRl;
        cssValuePoolAdd(pool, cssKeywordValue(vertical ? PrCssKeyword_VerticalRl : PrCssKeyword_HorizontalTb));
    } else if (property == PrCssProperty_BorderSpacing && keyword == PrCssKeyword_Count) {
        // Two absolute lengths, horizontal then vertical. A single length is used for both
        // (css-tables-3/#border-spacing-property).
        uint32_t start = pool->values.count;
        addAbsolute(pool, specified, context);
        if (pool->values.count == start + 1)
            cssValuePoolAdd(pool, pool->values.data[start]);
    } else if (cssIsColorProperty(property) && keyword == PrCssKeyword_Count && specified.count != 0
               && specified.values[0].kind == PrCssValueKind_Function
               && !readsElement(specified, property == PrCssProperty_Color)) {
        // A color function computes to the color it resolves to, unless it reads what only the element where it is used
        // decides: its color scheme, the display, or outside the color property, currentcolor
        // (css-color-5/#resolving-mix, css-color-5/#resolving-rcs).
        CssColor color{};
        if (resolveColorValue(specified, 0, CssColorContext{.current = context.parentColor}, &color))
            cssValuePoolAddColorComponents(pool, color);
        else
            cssValuePoolAddRun(pool, specified);
    } else if (cssIsColorProperty(property) && keyword != PrCssKeyword_Count
               && (keyword != PrCssKeyword_Currentcolor || property == PrCssProperty_Color)) {
        // A named color computes to its sRGB value, and `color: currentcolor` to the parent's color
        // (css-color-4/#resolving-color-values). currentcolor elsewhere stays a keyword, and so does a system color,
        // which resolves in the element's color scheme. A deprecated system color computes to the one that it names.
        PrColor color{};
        if (cssIsSystemColor(keyword))
            cssValuePoolAdd(pool, cssKeywordValue(cssSystemColorComputed(keyword)));
        else if (cssResolveColor(specified, 0, CssColorContext{.current = context.parentColor}, &color))
            cssValuePoolAddColor(pool, color8Of(color));
        else
            cssValuePoolAddRun(pool, specified);
    } else if (property == PrCssProperty_ZIndex && keyword == PrCssKeyword_Count) {
        cssValuePoolAdd(pool, cssNumberValue(PrCssValueKind_Integer, PrCssUnit_Number, computeZIndex(specified)));
    } else if (isOpacity(property)) {
        // The number, or the percentage as a fraction, clamped to [0, 1] (css-color-4/#transparency).
        double number = 1;
        if (!opacityOf(specified, &number)) {
            addAbsolute(pool, specified, context);
        } else {
            cssValuePoolAdd(pool, cssNumberValue(PrCssValueKind_Number, PrCssUnit_Number,
                                                 minF64(maxF64(number, 0), 1)));
        }
    } else if ((property == PrCssProperty_LetterSpacing || property == PrCssProperty_WordSpacing)
               && keyword == PrCssKeyword_Normal) {
        // normal is no spacing, a length of zero (css-text-4/#letter-spacing-property).
        cssValuePoolAdd(pool, pxValue(0));
    } else if (property == PrCssProperty_Fill || property == PrCssProperty_Stroke) {
        computePaint(pool, specified, context);
    } else if (isSvgLength(property) && keyword == PrCssKeyword_Count) {
        computeSvgLengths(pool, property, specified, context);
    } else if ((property == PrCssProperty_Filter || property == PrCssProperty_BackdropFilter)
               && keyword == PrCssKeyword_Count) {
        computeFilter(pool, specified, context);
    } else if (keyword == PrCssKeyword_Count && hasFilterImage(property, specified)) {
        computeImages(pool, specified, context);
    } else if (property == PrCssProperty_ClipPath && keyword == PrCssKeyword_Count) {
        computeClipPath(pool, specified, context);
    } else if (isCornerRadius(property) && keyword == PrCssKeyword_Count) {
        // A horizontal and a vertical radius, which serialize as one if they are equal
        // (css-backgrounds-3/#border-radius).
        uint32_t start = pool->values.count;
        addAbsolute(pool, specified, context);
        CssValueRun run = cssValuePoolRun(pool, CssValueSpan{start, pool->values.count - start});
        uint32_t second = run.count != 0 ? run.values[0].next : 0;
        if (second != 0 && second < run.count
            && cssValueRunEquals(cssValueSubtree(run, 0), cssValueSubtree(run, second)))
            pool->values.count = start + second;
    } else if (cssIsCornerShape(property)) {
        // The corresponding superellipse(), with its parameter's calculation resolved
        // (css-borders-4/#propdef-corner-top-left-shape).
        CssValuePool absolute{};
        addAbsolute(&absolute, specified, context);
        cssValuePoolAddSuperellipse(
            pool, cssCornerShapeParameter(cssValuePoolRun(&absolute, CssValueSpan{0, absolute.values.count})));
        cssValuePoolReset(&absolute);
    } else if (property == PrCssProperty_PaintOrder && keyword == PrCssKeyword_Count) {
        computePaintOrder(pool, specified);
    } else if (property == PrCssProperty_D && keyword == PrCssKeyword_Count) {
        // A path computes in absolute coordinates, so that paths of the same commands written relatively and absolutely
        // are of one shape, which interpolates (SVG2/paths.html#PathDataAnimation).
        cssValuePoolAddAbsolutePath(pool, specified, 0);
    } else {
        addAbsolute(pool, specified, context);
    }
}

void cssStyleComputeInitial(CssValuePool* pool, PrCssProperty property, const CssStyleContext& context) {
    CssValuePool parsed{};
    cssStyleComputeValue(pool, property, initialOf(&parsed, property), context);
    cssValuePoolReset(&parsed);
}

void cssStyleAddValue(CssStyleBuilder* builder, CssValueRun computed) {
    uint32_t start = builder->pool.values.count;
    cssValuePoolAddRun(&builder->pool, computed);
    builder->spans.push(CssValueSpan{start, builder->pool.values.count - start});
}

CssValueRun cssStyleLastValue(const CssStyleBuilder* builder) {
    PR_ASSERT(builder->spans.count != 0);
    return cssValuePoolRun(&builder->pool, builder->spans.last());
}

const CssStyleValues* cssStyleEnd(CssStyleBuilder* builder, CssStyleGroup group) {
    PR_ASSERT(builder->spans.count == kCssStyleGroupProperties[group].count);
    uint64_t hash = kStyleSeed ^ group;
    for (uint32_t i = 0; i < builder->spans.count; ++i) {
        CssValueSpan span = builder->spans.data[i];
        hash = hashBytes(&span.count, sizeof(span.count), hash);
        for (uint32_t k = 0; k < span.count; ++k)
            hash = hashValue(builder->pool.values.data[span.start + k], builder->pool.text.data, hash);
    }
    uint32_t key = uint32_t(hash);
    const CssStyleValues*& recent = builder->recent[group][key % kCssStyleBuilderRecent];
    if (recent != nullptr && recent->hash == key && sameGroup(recent, builder)) {
        cssStyleValuesRetain(recent);
        return recent;
    }
    const CssStyleValues* interned = intern(builder, group, key);
    cssStyleValuesRetain(interned);
    cssStyleValuesRelease(recent);
    recent = interned;
    return interned;
}

void cssStyleValuesRetain(const CssStyleValues* values) {
    values->references.fetchAddRelaxed(1);
}

void cssStyleValuesRelease(const CssStyleValues* PR_NULLABLE values) {
    if (values == nullptr || values->references.fetchSub(1) != 1)
        return;
    CssStyleValues* group = const_cast<CssStyleValues*>(values);
    if (group->owner != nullptr)
        freeGroup(group->owner, group);
    else
        prMemoryFree(group);
}

void cssFontFamilies(CssValueRun run, TextBuffer* names, Array<PrFontFamily>* out) {
    names->text.clear();
    out->clear();
    Array<uint32_t> starts;             // start of each named family's name, or UINT32_MAX for a generic
    Array<PrFontGeneric> generics;
    bool open = false;
    for (uint32_t i = 0; i < run.count; i += run.values[i].next) {
        const CssValueNode* value = &run.values[i];
        if (cssValueIsLiteral(value, ',')) {
            open = false;
            continue;
        }
        PrFontGeneric generic = PrFontGeneric_None;
        if (value->kind == PrCssValueKind_Keyword)
            generic = genericOf(cssValueKeyword(value));
        else if (cssValueIsFunction(value, PrCssKeyword_Generic) && i + 1 < run.count
                 && run.values[i + 1].kind == PrCssValueKind_Keyword)
            generic = genericOf(cssValueKeyword(&run.values[i + 1]));
        if (generic != PrFontGeneric_None) {
            starts.push(UINT32_MAX);
            generics.push(generic);
            continue;
        }
        PrStringView name = cssValueHasText(value->kind) ? cssValueText(run, value) : PrStringView{};
        if (open) {
            textAppendChar(names, ' ');
        } else {
            starts.push(names->text.count);
            generics.push(PrFontGeneric_None);
        }
        textAppend(names, name);
        open = true;
    }
    for (uint32_t i = 0; i < starts.count; ++i) {
        if (starts.data[i] == UINT32_MAX) {
            out->push(prFontFamilyGeneric(generics.data[i]));
            continue;
        }
        uint32_t end = names->text.count;
        for (uint32_t k = i + 1; k < starts.count; ++k) {
            if (starts.data[k] != UINT32_MAX) {
                end = starts.data[k];
                break;
            }
        }
        out->push(
            prFontFamilyFromName(PrStringView{names->text.data + starts.data[i], end - starts.data[i]}));
    }
    starts.reset();
    generics.reset();
}

PrTextFeatureSet* PR_NULLABLE cssFontFeatureSet(const CssComputedStyle* style) {
    PrTextFeatureSetDesc desc{};
    switch (keywordOfRun(cssComputedValue(style, PrCssProperty_FontVariantCaps))) {
        case PrCssKeyword_SmallCaps: desc.caps = PrTextCaps_Small; break;
        case PrCssKeyword_AllSmallCaps: desc.caps = PrTextCaps_AllSmall; break;
        case PrCssKeyword_PetiteCaps: desc.caps = PrTextCaps_Petite; break;
        case PrCssKeyword_AllPetiteCaps: desc.caps = PrTextCaps_AllPetite; break;
        case PrCssKeyword_Unicase: desc.caps = PrTextCaps_Unicase; break;
        case PrCssKeyword_TitlingCaps: desc.caps = PrTextCaps_Titling; break;
        default: desc.caps = PrTextCaps_Normal; break;
    }
    PrCssKeyword position = keywordOfRun(cssComputedValue(style, PrCssProperty_FontVariantPosition));
    desc.position = position == PrCssKeyword_Sub     ? PrTextPosition_Subscript
                    : position == PrCssKeyword_Super ? PrTextPosition_Superscript
                                                     : PrTextPosition_Normal;
    PrCssKeyword kerning = keywordOfRun(cssComputedValue(style, PrCssProperty_FontKerning));
    desc.kerning = kerning == PrCssKeyword_Normal ? PrTextKerning_Normal
                   : kerning == PrCssKeyword_None ? PrTextKerning_None
                                                  : PrTextKerning_Auto;
    desc.synthesis = 0;
    if (keywordOfRun(cssComputedValue(style, PrCssProperty_FontSynthesisSmallCaps)) != PrCssKeyword_None)
        desc.synthesis |= PrTextSynthesisFlag_SmallCaps;
    if (keywordOfRun(cssComputedValue(style, PrCssProperty_FontSynthesisPosition)) != PrCssKeyword_None)
        desc.synthesis |= PrTextSynthesisFlag_Position;
    Array<PrTextFeature> features;
    variantFeatures(cssComputedValue(style, PrCssProperty_FontVariantLigatures), features);
    variantFeatures(cssComputedValue(style, PrCssProperty_FontVariantNumeric), features);
    variantFeatures(cssComputedValue(style, PrCssProperty_FontVariantAlternates), features);
    variantFeatures(cssComputedValue(style, PrCssProperty_FontVariantEastAsian), features);
    // font-feature-settings: a tag, and its value, on or off, for each feature
    // (css-fonts-4/#font-feature-settings-prop).
    CssValueRun settings = cssComputedValue(style, PrCssProperty_FontFeatureSettings);
    for (uint32_t i = 0; i < settings.count; i += settings.values[i].next) {
        const CssValueNode* value = &settings.values[i];
        if (value->kind != PrCssValueKind_String)
            continue;
        PrStringView tag = cssValueText(settings, value);
        if (tag.size != 4)
            continue;
        uint32_t on = 1;
        uint32_t next = i + value->next;
        if (next < settings.count) {
            const CssValueNode* setting = &settings.values[next];
            if (setting->kind == PrCssValueKind_Integer || setting->kind == PrCssValueKind_Number)
                on = setting->number > 0 ? uint32_t(setting->number) : 0;
            else if (setting->kind == PrCssValueKind_Keyword)
                on = cssValueKeyword(setting) == PrCssKeyword_Off ? 0 : 1;
        }
        pushFeature(features,
                    PR_FONT_TAG(uint8_t(tag.data[0]), uint8_t(tag.data[1]), uint8_t(tag.data[2]), uint8_t(tag.data[3])),
                    on);
    }
    bool defaults = desc.caps == PrTextCaps_Normal && desc.position == PrTextPosition_Normal
                    && desc.kerning == PrTextKerning_Auto
                    && desc.synthesis == (PrTextSynthesisFlag_SmallCaps | PrTextSynthesisFlag_Position)
                    && features.count == 0;
    desc.features = features.data;
    desc.featureCount = features.count;
    PrTextFeatureSet* set = defaults ? nullptr : prTextFeatureSetCreate(&desc);
    features.reset();
    return set;
}

PrTextBreakStyle cssTextBreakStyle(const CssComputedStyle* style) {
    PrTextBreakStyle out{};
    switch (keywordOfRun(cssComputedValue(style, PrCssProperty_WordBreak))) {
        case PrCssKeyword_BreakAll: out.wordBreak = PrTextWordBreak_BreakAll; break;
        case PrCssKeyword_KeepAll: out.wordBreak = PrTextWordBreak_KeepAll; break;
        case PrCssKeyword_BreakWord: out.wordBreak = PrTextWordBreak_BreakWord; break;
        default: out.wordBreak = PrTextWordBreak_Normal; break;
    }
    switch (keywordOfRun(cssComputedValue(style, PrCssProperty_OverflowWrap))) {
        case PrCssKeyword_Anywhere: out.overflowWrap = PrTextOverflowWrap_Anywhere; break;
        case PrCssKeyword_BreakWord: out.overflowWrap = PrTextOverflowWrap_BreakWord; break;
        default: out.overflowWrap = PrTextOverflowWrap_Normal; break;
    }
    return out;
}

double cssStyleFontSize(const CssComputedStyle* style) {
    return style->font.em;
}

float cssStyleFontWeight(const CssComputedStyle* style) {
    CssValueRun run = cssComputedValue(style, PrCssProperty_FontWeight);
    return run.count == 1 ? float(run.values[0].number) : 400;
}

double cssFontWidthOfKeyword(PrCssKeyword keyword) {
    switch (keyword) {
        case PrCssKeyword_UltraCondensed: return 50;
        case PrCssKeyword_ExtraCondensed: return 62.5;
        case PrCssKeyword_Condensed: return 75;
        case PrCssKeyword_SemiCondensed: return 87.5;
        case PrCssKeyword_SemiExpanded: return 112.5;
        case PrCssKeyword_Expanded: return 125;
        case PrCssKeyword_ExtraExpanded: return 150;
        case PrCssKeyword_UltraExpanded: return 200;
        default: return kNormalFontWidth;
    }
}

float cssStyleFontWidth(const CssComputedStyle* style) {
    CssValueRun run = cssComputedValue(style, PrCssProperty_FontWidth);
    return run.count == 1 && run.values[0].kind == PrCssValueKind_Percentage ? float(run.values[0].number)
                                                                            : float(kNormalFontWidth);
}

bool cssIsColorProperty(PrCssProperty property) {
    switch (property) {
        case PrCssProperty_Color:
        case PrCssProperty_AccentColor:
        case PrCssProperty_BackgroundColor:
        case PrCssProperty_BorderTopColor:
        case PrCssProperty_BorderRightColor:
        case PrCssProperty_BorderBottomColor:
        case PrCssProperty_BorderLeftColor:
        case PrCssProperty_OutlineColor:
        case PrCssProperty_TextDecorationColor:
        case PrCssProperty_StopColor:
        case PrCssProperty_FloodColor:
        case PrCssProperty_LightingColor:
            return true;
        default:
            return false;
    }
}

CssColorContext cssStyleColorContext(const CssComputedStyle* style, const CssColorEnvironment& environment) {
    CssColorContext context{.current = PrColor{0, 0, 0, 1}, .style = style, .environment = environment};
    cssResolveColor(cssComputedValue(style, PrCssProperty_Color), 0, context, &context.current);
    return context;
}

PrColor cssStyleColor(const CssComputedStyle* style, const CssColorEnvironment& environment) {
    return cssStyleColorContext(style, environment).current;
}

bool cssUsesDarkScheme(const CssComputedStyle* PR_NULLABLE style, const CssColorEnvironment& environment) {
    // normal uses the page's schemes. A list that names no scheme the engine knows uses the default, light. Where both
    // schemes remain, the user's preference decides (css-color-adjust-1/#color-scheme-resolution).
    CssColorSchemeFlags schemes = environment.pageSchemes;
    CssValueRun scheme = style != nullptr ? cssComputedValue(style, PrCssProperty_ColorScheme) : CssValueRun{};
    if (scheme.count != 0 && !cssValueIsKeyword(scheme.values, PrCssKeyword_Normal)) {
        schemes = 0;
        for (uint32_t i = 0; i < scheme.count; ++i) {
            if (cssValueIsKeyword(&scheme.values[i], PrCssKeyword_Light))
                schemes |= CssColorSchemeFlag_Light;
            else if (cssValueIsKeyword(&scheme.values[i], PrCssKeyword_Dark))
                schemes |= CssColorSchemeFlag_Dark;
        }
    }
    constexpr CssColorSchemeFlags both = CssColorSchemeFlag_Light | CssColorSchemeFlag_Dark;
    if (schemes != both)
        return schemes == CssColorSchemeFlag_Dark;
    if (environment.readsOutside != nullptr)
        *environment.readsOutside = true;
    return environment.prefersDark;
}

double cssLineHeightPx(CssValueRun computed, double fontSize) {
    if (computed.count == 1 && computed.values[0].kind == PrCssValueKind_Number)
        return computed.values[0].number * fontSize;
    if (computed.count == 1 && computed.values[0].kind == PrCssValueKind_Dimension
        && computed.values[0].unit == PrCssUnit_Px)
        return computed.values[0].number;
    return fontSize * kCssNormalLineHeight;
}

double cssStyleLineHeight(const CssComputedStyle* style) {
    return style->font.lh;
}

CssFontBases cssStyleFontBases(const CssComputedStyle* style) {
    return cssFontBases(style->font.em, style->font.lh, style);
}

double cssFontMetricPx(const CssStyleContext& context, bool root, CssFontMetric metric) {
    const CssFontBases& bases = root ? context.root : context.font;
    const CssComputedStyle* style = bases.style;
    // An advance is measured down the line if its glyph is set upright. "0" is set upright only under text-orientation
    // upright, and the ideograph under mixed too (css-writing-modes-4/#text-orientation).
    bool upright = false;
    if (style != nullptr && (metric == CssFontMetric_Ch || metric == CssFontMetric_Ic)) {
        PrCssKeyword mode = keywordOfRun(cssComputedValue(style, PrCssProperty_WritingMode));
        PrCssKeyword orientation = keywordOfRun(cssComputedValue(style, PrCssProperty_TextOrientation));
        bool vertical = mode == PrCssKeyword_VerticalRl || mode == PrCssKeyword_VerticalLr;
        upright = vertical && (orientation == PrCssKeyword_Upright
                               || (metric == CssFontMetric_Ic && orientation == PrCssKeyword_Mixed));
    }
    constexpr double kFallbacks[CssFontMetric_Count] = {0.5, 0.7, 0.5, 1};
    double ems = metric == CssFontMetric_Ch && upright ? 1 : kFallbacks[metric];
    CssFontMetrics* metrics = context.metrics;
    if (metrics != nullptr) {
        (root ? metrics->readsRoot : metrics->readsFont) = true;
        metrics->readsGroup = true;
        uint32_t side = root ? 1 : 0;
        if (style != nullptr && metrics->measure != nullptr) {
            if (metrics->sources[side] != style) {
                metrics->sources[side] = style;
                metrics->measured[side] = 0;
            }
            uint8_t bit = uint8_t(1u << metric);
            if ((metrics->measured[side] & bit) == 0) {
                double measured = 0;
                if (metrics->measure->measure(metrics->measure->data, style, metric, upright, &measured))
                    metrics->ems[side][metric] = measured;
                else
                    metrics->ems[side][metric] = ems;
                metrics->measured[side] |= bit;
            }
            ems = metrics->ems[side][metric];
        }
    }
    return ems * bases.em;
}

bool cssResolveColorComponents(CssValueRun run, uint32_t index, const CssColorContext& context, CssColor* out) {
    return resolveColorValue(run, index, context, out);
}

CssColor cssColorConvert(const CssColor& color, PrColorSpace space) {
    return convertCarrying(color, space);
}

CssColor cssColorInterpolate(const CssColor& a, const CssColor& b, double t, PrColorSpace space,
                             PrColorHueMethod hueMethod) {
    return interpolate(a, b, t, space, hueMethod);
}

PrColor cssColorToSrgb(const CssColor& color) {
    return toSrgb(color);
}

bool cssResolveColor(CssValueRun run, uint32_t index, const CssColorContext& context, PrColor* out) {
    // Most colors are a keyword or an sRGB color written whole, which need no conversion.
    if (index < run.count && run.values[index].kind == PrCssValueKind_Keyword) {
        PrCssKeyword keyword = cssValueKeyword(&run.values[index]);
        PrColor8 named{};
        if (keyword == PrCssKeyword_Currentcolor) {
            *out = context.current;
            return true;
        }
        if (keyword == PrCssKeyword_Transparent) {
            *out = PrColor{0, 0, 0, 0};
            return true;
        }
        if (cssNamedColor(keyword, &named)) {
            *out = prColorFromColor8(named);
            return true;
        }
    }
    if (index < run.count && run.values[index].kind == PrCssValueKind_Color
        && cssValueColorSpace(&run.values[index]) == PrColorSpace_Srgb) {
        CssColorChannels channels = cssColorChannelsFromValue(run, index);
        if ((channels.missing | channels.calculated) == 0) {
            float c[4];
            for (uint32_t i = 0; i < 4; ++i)
                c[i] = float(cssColorChannelValue(&run.values[channels.at[i]], *channels.meaning[i]));
            *out = PrColor{c[0], c[1], c[2], clampF32(c[3], 0, 1)};
            return true;
        }
    }
    CssColor color{};
    if (!resolveColorValue(run, index, context, &color))
        return false;
    *out = toSrgb(color);
    return true;
}

bool cssResolveLength(CssValueRun run, uint32_t index, double base, double* out) {
    if (index >= run.count)
        return false;
    const CssValueNode* value = &run.values[index];
    if (value->kind == PrCssValueKind_Dimension && value->unit == PrCssUnit_Px) {
        *out = value->number;
        return true;
    }
    if (value->kind == PrCssValueKind_Number && value->number == 0) {
        *out = 0;
        return true;
    }
    if (value->kind == PrCssValueKind_Percentage) {
        *out = value->number * base / 100;
        return true;
    }
    if (isLength(value)) {
        double scale = cssUnitScale(value->unit);
        if (scale == 0)
            return false;
        *out = value->number * scale;
        return true;
    }
    if (value->kind != PrCssValueKind_Math)
        return false;
    double px = 0;
    if (cssMathTo(run, index, PrCssUnit_Px, &px)) {
        *out = px;
        return true;
    }
    // A sum of a length and a percentage.
    CssValuePool pool{};
    PrCssUnit units[] = {PrCssUnit_Px, PrCssUnit_Percent};
    bool summed = cssMathToSum(run, index, units, 2, &pool);
    double total = 0;
    if (summed) {
        CssValueRun sum = cssValuePoolRun(&pool, CssValueSpan{0, pool.values.count});
        for (uint32_t i = 0; i < sum.count; ++i) {
            const CssValueNode& term = sum.values[i];
            if (term.kind == PrCssValueKind_Dimension)
                total += term.number;
            else if (term.kind == PrCssValueKind_Percentage)
                total += term.number * base / 100;
        }
    }
    cssValuePoolReset(&pool);
    *out = total;
    return summed;
}

} // namespace pr_impl
