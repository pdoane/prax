// Filters: the items of a computed <filter-value-list>, as layout takes them, and blend modes, as lib/draw takes them.
//
// filter-effects-1 - https://drafts.fxtf.org/filter-effects-1/
// compositing-1 - https://drafts.fxtf.org/compositing-1/

#include "css/src/css_filter.h"

#include "pr/float.h"

#include "css/src/css_math.h"

namespace pr_impl {

namespace {

// Returns the amount of a filter function's argument at `index`: a number, or a percentage as a fraction. Returns
// `fallback` if the value is neither.
float amountAt(CssValueRun run, uint32_t index, float fallback) {
    const CssValueNode* value = &run.values[index];
    if (value->kind == PrCssValueKind_Percentage)
        return float(value->number / 100);
    double number = 0;
    if (cssMathTo(run, index, PrCssUnit_Number, &number))
        return float(number);
    if (cssMathTo(run, index, PrCssUnit_Percent, &number))
        return float(number / 100);
    return fallback;
}

} // namespace

void cssFilterFunctions(CssValueRun run, const CssColorContext& colors, Array<CssFilterFunction>* out) {
    for (uint32_t i = 0; i < run.count; i += run.values[i].next) {
        const CssValueNode* value = &run.values[i];
        CssFilterFunction item{.filter = {.amount = 1, .color = prColor16FromColor(colors.current)}};
        PrLayoutFilter& f = item.filter;
        if (value->kind == PrCssValueKind_Url) {
            f.kind = PrLayoutFilterKind_Source;
            item.url = cssValueText(run, value);
            out->push(item);
            continue;
        }
        if (value->kind != PrCssValueKind_Function)
            continue;
        PrCssKeyword name = cssValueKeyword(value);
        uint32_t first = i + 1;
        uint32_t end = i + value->next;
        bool given = first < end;
        if (name == PrCssKeyword_Url) {
            f.kind = PrLayoutFilterKind_Source;
            item.url = given && run.values[first].kind == PrCssValueKind_String ? cssValueText(run, &run.values[first])
                                                                                : PrStringView{};
            out->push(item);
            continue;
        }
        switch (name) {
            case PrCssKeyword_Blur: {
                f.kind = PrLayoutFilterKind_Blur;
                double sigma = 0;
                f.amount = given && cssResolveLength(run, first, 0, &sigma) ? maxF32(float(sigma), 0) : 0;
                break;
            }
            case PrCssKeyword_DropShadow: {
                // The color may come before or after two or three lengths: the offsets, then the deviation.
                f.kind = PrLayoutFilterKind_DropShadow;
                f.amount = 0;
                float lengths[3] = {0, 0, 0};
                uint32_t count = 0;
                for (uint32_t k = first; k < end; k += run.values[k].next) {
                    double px = 0;
                    PrColor color{};
                    if (cssResolveLength(run, k, 0, &px)) {
                        if (count < 3)
                            lengths[count++] = float(px);
                    } else if (cssResolveColor(run, k, colors, &color)) {
                        f.color = prColor16FromColor(color);
                    }
                }
                f.offset = PrVec2{lengths[0], lengths[1]};
                f.sigma = maxF32(lengths[2], 0);
                break;
            }
            case PrCssKeyword_HueRotate: {
                f.kind = PrLayoutFilterKind_HueRotate;
                double degrees = 0;
                f.amount = given && cssMathTo(run, first, PrCssUnit_Deg, &degrees) ? float(degrees * kPiF64 / 180) : 0;
                break;
            }
            case PrCssKeyword_Brightness: f.kind = PrLayoutFilterKind_Brightness; break;
            case PrCssKeyword_Contrast: f.kind = PrLayoutFilterKind_Contrast; break;
            case PrCssKeyword_Grayscale: f.kind = PrLayoutFilterKind_Grayscale; break;
            case PrCssKeyword_Invert: f.kind = PrLayoutFilterKind_Invert; break;
            case PrCssKeyword_Opacity: f.kind = PrLayoutFilterKind_Opacity; break;
            case PrCssKeyword_Saturate: f.kind = PrLayoutFilterKind_Saturate; break;
            case PrCssKeyword_Sepia: f.kind = PrLayoutFilterKind_Sepia; break;
            default: continue;
        }
        if (f.kind != PrLayoutFilterKind_Blur && f.kind != PrLayoutFilterKind_DropShadow
            && f.kind != PrLayoutFilterKind_HueRotate) {
            // An omitted amount is 1. An amount over 1 is clamped for a function whose effect is complete at 1
            // (filter-effects-1/#supported-filter-functions).
            f.amount = maxF32(given ? amountAt(run, first, 1) : 1, 0);
            bool clamps = f.kind == PrLayoutFilterKind_Grayscale || f.kind == PrLayoutFilterKind_Invert
                          || f.kind == PrLayoutFilterKind_Opacity || f.kind == PrLayoutFilterKind_Sepia;
            if (clamps)
                f.amount = minF32(f.amount, 1);
        }
        out->push(item);
    }
}

bool cssBlendModeFromKeyword(PrCssKeyword keyword, PrDrawBlendMode* out) {
    switch (keyword) {
        case PrCssKeyword_Normal: *out = PrDrawBlendMode_Normal; return true;
        case PrCssKeyword_Multiply: *out = PrDrawBlendMode_Multiply; return true;
        case PrCssKeyword_Screen: *out = PrDrawBlendMode_Screen; return true;
        case PrCssKeyword_Overlay: *out = PrDrawBlendMode_Overlay; return true;
        case PrCssKeyword_Darken: *out = PrDrawBlendMode_Darken; return true;
        case PrCssKeyword_Lighten: *out = PrDrawBlendMode_Lighten; return true;
        case PrCssKeyword_ColorDodge: *out = PrDrawBlendMode_ColorDodge; return true;
        case PrCssKeyword_ColorBurn: *out = PrDrawBlendMode_ColorBurn; return true;
        case PrCssKeyword_HardLight: *out = PrDrawBlendMode_HardLight; return true;
        case PrCssKeyword_SoftLight: *out = PrDrawBlendMode_SoftLight; return true;
        case PrCssKeyword_Difference: *out = PrDrawBlendMode_Difference; return true;
        case PrCssKeyword_Exclusion: *out = PrDrawBlendMode_Exclusion; return true;
        case PrCssKeyword_Hue: *out = PrDrawBlendMode_Hue; return true;
        case PrCssKeyword_Saturation: *out = PrDrawBlendMode_Saturation; return true;
        case PrCssKeyword_Color: *out = PrDrawBlendMode_Color; return true;
        case PrCssKeyword_Luminosity: *out = PrDrawBlendMode_Luminosity; return true;
        default: return false;
    }
}

} // namespace pr_impl
