#include "css/src/css_color_value.h"

#include "pr/color_space.h"
#include "pr/float.h"

namespace pr_impl {

namespace {

constexpr CssColorChannelSyntax kUnbounded = {1.0, 0.01, -kF64Max, kF64Max, false};
constexpr CssColorChannelSyntax kHue = {1.0, 0.0, 0.0, 0.0, true};

// The channels of rgb() and rgba(). A number is in [0, 255], and each channel is clamped to the sRGB gamut
// (css-color-4/#rgb-functions).
constexpr CssColorChannelSyntax kRgbChannel = {1.0 / 255.0, 0.01, 0.0, 1.0, false};
constexpr CssColorChannelSyntax kRgb[3] = {kRgbChannel, kRgbChannel, kRgbChannel};

// The channels of each color space, indexed by PrColorSpace (css-color-4/#predefined, css-color-4/#the-hsl-notation,
// css-color-4/#the-hwb-notation, css-color-4/#specifying-lab-lch, css-color-4/#specifying-oklab-oklch,
// css-color-hdr-1/#ictcp, css-color-hdr-1/#Jzazbz, css-color-hdr-1/#JzCzhz). A percentage of ICtCp's Ct and Cp is of
// 0.5, of Jzazbz's az and bz of 0.21, and of JzCzhz's Cz of 0.26.
constexpr CssColorChannelSyntax kChannels[PrColorSpace_Count][3] = {
    {kUnbounded, kUnbounded, kUnbounded},                                          // srgb
    {kUnbounded, kUnbounded, kUnbounded},                                          // srgb-linear
    {kUnbounded, kUnbounded, kUnbounded},                                          // display-p3
    {kUnbounded, kUnbounded, kUnbounded},                                          // display-p3-linear
    {kUnbounded, kUnbounded, kUnbounded},                                          // a98-rgb
    {kUnbounded, kUnbounded, kUnbounded},                                          // prophoto-rgb
    {kUnbounded, kUnbounded, kUnbounded},                                          // rec2020
    {kUnbounded, kUnbounded, kUnbounded},                                          // xyz-d50
    {kUnbounded, kUnbounded, kUnbounded},                                          // xyz-d65
    {                                                                              // lab
        {1.0, 1.0, 0.0, 100.0, false}, {1.0, 1.25, -kF64Max, kF64Max, false},
        {1.0, 1.25, -kF64Max, kF64Max, false},
    },
    {{1.0, 1.0, 0.0, 100.0, false}, {1.0, 1.5, 0.0, kF64Max, false}, kHue}, // lch
    {                                                                       // oklab
        {1.0, 0.01, 0.0, 1.0, false}, {1.0, 0.004, -kF64Max, kF64Max, false},
        {1.0, 0.004, -kF64Max, kF64Max, false},
    },
    {{1.0, 0.01, 0.0, 1.0, false}, {1.0, 0.004, 0.0, kF64Max, false}, kHue},            // oklch
    {kHue, {1.0, 1.0, 0.0, kF64Max, false}, {1.0, 1.0, -kF64Max, kF64Max, false}},      // hsl
    {kHue, {1.0, 1.0, -kF64Max, kF64Max, false}, {1.0, 1.0, -kF64Max, kF64Max, false}}, // hwb
    {kUnbounded, kUnbounded, kUnbounded},                                               // rec2100-pq
    {kUnbounded, kUnbounded, kUnbounded},                                               // rec2100-hlg
    {kUnbounded, kUnbounded, kUnbounded},                                               // rec2100-linear
    {                                                                                   // ictcp
        kUnbounded, {1.0, 0.005, -kF64Max, kF64Max, false},
        {1.0, 0.005, -kF64Max, kF64Max, false},
    },
    { // jzazbz
        kUnbounded, {1.0, 0.0021, -kF64Max, kF64Max, false},
        {1.0, 0.0021, -kF64Max, kF64Max, false},
    },
    {kUnbounded, {1.0, 0.0026, 0.0, kF64Max, false}, kHue},                       // jzczhz
};

} // namespace

constexpr CssColorChannelSyntax kCssColorAlpha = {1.0, 0.01, 0.0, 1.0, false};

const CssColorChannelSyntax* cssColorChannels(PrColorSpace space, bool legacy) {
    return legacy ? kRgb : kChannels[space];
}

CssColorChannels cssColorChannelsFromValue(CssValueRun run, uint32_t index) {
    CssColorChannels channels = {};
    channels.space = cssValueColorSpace(&run.values[index]);
    channels.legacy = cssValueColorLegacy(&run.values[index]);
    const CssColorChannelSyntax* meanings = cssColorChannels(channels.space, channels.legacy);
    uint32_t at = index + 1;
    for (uint32_t i = 0; i < 4; ++i) {
        const CssValueNode* channel = &run.values[at];
        channels.at[i] = at;
        channels.meaning[i] = i < 3 ? &meanings[i] : &kCssColorAlpha;
        if (channel->kind == PrCssValueKind_Keyword)
            channels.missing |= 1u << i;
        else if (!cssValueIsNumeric(channel->kind))
            channels.calculated |= 1u << i;
        at += channel->next;
    }
    return channels;
}

double cssColorChannelValue(const CssValueNode* value, const CssColorChannelSyntax& channel) {
    double number;
    if (value->kind == PrCssValueKind_Percentage)
        number = value->number * channel.percent;
    else if (value->kind == PrCssValueKind_Dimension)
        number = value->number * cssUnitScale(value->unit);
    else
        number = value->number * channel.number;
    if (channel.hue)
        return isFiniteF64(number) ? prColorHue(number) : 0.0;
    if (isNanF64(number))
        return 0.0;
    return number < channel.min ? channel.min : number > channel.max ? channel.max : number;
}

PrColorSpace cssColorSpaceFromKeyword(PrCssKeyword keyword) {
    switch (keyword) {
        case PrCssKeyword_Rgb:
        case PrCssKeyword_Rgba:
        case PrCssKeyword_Srgb:            return PrColorSpace_Srgb;
        case PrCssKeyword_SrgbLinear:      return PrColorSpace_SrgbLinear;
        case PrCssKeyword_DisplayP3:       return PrColorSpace_DisplayP3;
        case PrCssKeyword_DisplayP3Linear: return PrColorSpace_DisplayP3Linear;
        case PrCssKeyword_A98Rgb:          return PrColorSpace_A98Rgb;
        case PrCssKeyword_ProphotoRgb:     return PrColorSpace_ProPhotoRgb;
        case PrCssKeyword_Rec2020:         return PrColorSpace_Rec2020;
        case PrCssKeyword_XyzD50:          return PrColorSpace_XyzD50;
        case PrCssKeyword_Xyz:
        case PrCssKeyword_XyzD65:          return PrColorSpace_XyzD65;
        case PrCssKeyword_Lab:             return PrColorSpace_Lab;
        case PrCssKeyword_Lch:             return PrColorSpace_Lch;
        case PrCssKeyword_Oklab:           return PrColorSpace_Oklab;
        case PrCssKeyword_Oklch:           return PrColorSpace_Oklch;
        case PrCssKeyword_Hsl:
        case PrCssKeyword_Hsla:            return PrColorSpace_Hsl;
        case PrCssKeyword_Hwb:             return PrColorSpace_Hwb;
        case PrCssKeyword_Rec2100Pq:       return PrColorSpace_Rec2100Pq;
        case PrCssKeyword_Rec2100Hlg:      return PrColorSpace_Rec2100Hlg;
        case PrCssKeyword_Rec2100Linear:   return PrColorSpace_Rec2100Linear;
        case PrCssKeyword_Ictcp:           return PrColorSpace_Ictcp;
        case PrCssKeyword_Jzazbz:          return PrColorSpace_Jzazbz;
        case PrCssKeyword_Jzczhz:          return PrColorSpace_Jzczhz;
        default:                         return PrColorSpace_Count;
    }
}

bool cssIsColorFunction(PrCssKeyword keyword) {
    switch (keyword) {
        case PrCssKeyword_Alpha:
        case PrCssKeyword_Color:
        case PrCssKeyword_ColorHdr:
        case PrCssKeyword_ColorLayers:
        case PrCssKeyword_ColorMix:
        case PrCssKeyword_ContrastColor:
        case PrCssKeyword_DeviceCmyk:
        case PrCssKeyword_LightDark:
            return true;
        default:
            return false;
    }
}

bool cssColorsAreLegacy(const CssValueNode* values, uint32_t first, uint32_t end) {
    for (uint32_t i = first; i < end; i += values[i].next) {
        const CssValueNode* value = &values[i];
        if (value->kind == PrCssValueKind_Color) {
            PrColorSpace space = cssValueColorSpace(value);
            if (!cssValueColorLegacy(value) && space != PrColorSpace_Hsl && space != PrColorSpace_Hwb) {
                return false;
            }
            // A missing component is written in the modern syntax, which the legacy syntax cannot write.
            for (uint32_t c = i + 1; c < i + value->next; c += values[c].next) {
                if (values[c].kind == PrCssValueKind_Keyword)
                    return false;
            }
        } else if (value->kind == PrCssValueKind_Function &&
                (cssColorSpaceFromKeyword(cssValueKeyword(value)) != PrColorSpace_Count ||
                    cssIsColorFunction(cssValueKeyword(value)))) {
            return false;
        }
    }
    return true;
}

void cssValuePoolAddColorComponents(CssValuePool* pool, const CssColor& color) {
    CssValueNode value = {};
    value.kind = PrCssValueKind_Color;
    value.aux = CssValueAuxFlags(color.space);
    uint32_t parent = cssValuePoolAdd(pool, value);
    const CssColorChannelSyntax* syntax = cssColorChannels(color.space, false);
    for (uint32_t i = 0; i < 4; ++i) {
        if ((color.missing & (1u << i)) != 0) {
            cssValuePoolAdd(pool, cssKeywordValue(PrCssKeyword_None));
            continue;
        }
        double number = i < 3 ? color.c[i] / syntax[i].number : color.alpha / kCssColorAlpha.number;
        cssValuePoolAdd(pool, cssNumberValue(PrCssValueKind_Number, PrCssUnit_Number, number));
    }
    pool->values.data[parent].next = pool->values.count - parent;
}

void cssValuePoolAddColor(CssValuePool* pool, PrColor8 color) {
    CssValueNode value = {};
    value.kind = PrCssValueKind_Color;
    value.aux = CssValueAuxFlags(uint32_t(PrColorSpace_Srgb) | CssValueAuxFlag_ColorLegacy);
    uint32_t parent = cssValuePoolAdd(pool, value);
    const double channels[] = {double(color.r), double(color.g), double(color.b), color.a / 255.0};
    for (double channel : channels)
        cssValuePoolAdd(pool, cssNumberValue(PrCssValueKind_Number, PrCssUnit_Number, channel));
    pool->values.data[parent].next = pool->values.count - parent;
}

void cssValuePoolAddLegacyColor(CssValuePool* pool, const CssColor& color) {
    PR_ASSERT(color.space == PrColorSpace_Srgb);
    CssValueNode value = {};
    value.kind = PrCssValueKind_Color;
    value.aux = CssValueAuxFlags(uint32_t(PrColorSpace_Srgb) | CssValueAuxFlag_ColorLegacy);
    uint32_t parent = cssValuePoolAdd(pool, value);
    const CssColorChannelSyntax* syntax = cssColorChannels(PrColorSpace_Srgb, true);
    for (uint32_t i = 0; i < 4; ++i) {
        double channel;
        if ((color.missing & (1u << i)) != 0)
            channel = 0;
        else if (i < 3)
            channel = color.c[i] / syntax[i].number;
        else
            channel = color.alpha / kCssColorAlpha.number;
        cssValuePoolAdd(pool, cssNumberValue(PrCssValueKind_Number, PrCssUnit_Number, channel));
    }
    pool->values.data[parent].next = pool->values.count - parent;
}

} // namespace pr_impl
