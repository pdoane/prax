// Studio Kit's themes: the colors, the type scale and the measures its widgets are drawn in.

#include "pr/kit_theme.h"

#include "pr/format.h"
#include "pr/text_buffer.h"

namespace pr_impl {

namespace {

constexpr PrColor rgb(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
    return PrColor{float(r) / 255.0f, float(g) / 255.0f, float(b) / 255.0f, float(a) / 255.0f};
}

// Studio's accent ramp and its semantic colors, which both variants share.
constexpr PrColor kAccent400 = rgb(40, 150, 245);
constexpr PrColor kAccent500 = rgb(0, 130, 230);
constexpr PrColor kAccent600 = rgb(0, 112, 204);
constexpr PrColor kAccent700 = rgb(0, 89, 162);
constexpr PrColor kGreen500 = rgb(51, 172, 90);
constexpr PrColor kAmber500 = rgb(226, 165, 32);
constexpr PrColor kRed500 = rgb(226, 73, 71);

constexpr PrKitPalette kDark = {
    .accent = kAccent500,
    .accentWeak = rgb(0, 57, 105, 89),
    .textSelection = rgb(38, 79, 120),
    .accentText = rgb(246, 249, 252),
    .accentHover = kAccent400,
    .accentPress = kAccent600,
    .success = kGreen500,
    .warning = kAmber500,
    .danger = kRed500,
    .bg0 = rgb(12, 13, 15),
    .bg1 = rgb(18, 20, 23),
    .bg2 = rgb(25, 27, 30),
    .bg3 = rgb(34, 36, 40),
    .bg4 = rgb(43, 46, 50),
    .bgViewport = rgb(5, 6, 7),
    .line1 = rgb(39, 41, 45),
    .line2 = rgb(58, 61, 66),
    .line3 = rgb(90, 94, 101),
    .text1 = rgb(240, 242, 245),
    .text2 = rgb(181, 183, 187),
    .text3 = rgb(126, 128, 132),
    .text4 = rgb(85, 88, 93),
    .textInverse = rgb(12, 13, 15),
    .hudFill = rgb(0, 0, 0, 102),
    .hudBorder = rgb(255, 255, 255, 18),
    .axisX = rgb(237, 117, 110),
    .axisY = rgb(104, 180, 87),
    .axisZ = rgb(75, 163, 247),
    .axisW = rgb(187, 130, 227),
};

constexpr PrKitPalette kLight = {
    .accent = kAccent600,
    .accentWeak = rgb(193, 226, 255),
    .textSelection = rgb(179, 215, 255),
    .accentText = rgb(255, 255, 255),
    .accentHover = kAccent500,
    .accentPress = kAccent700,
    .success = kGreen500,
    .warning = kAmber500,
    .danger = kRed500,
    .bg0 = rgb(248, 248, 250),
    .bg1 = rgb(255, 255, 255),
    .bg2 = rgb(240, 242, 244),
    .bg3 = rgb(230, 232, 234),
    .bg4 = rgb(212, 216, 221),
    .bgViewport = rgb(234, 235, 237),
    .line1 = rgb(220, 222, 225),
    .line2 = rgb(201, 203, 206),
    .line3 = rgb(162, 165, 169),
    .text1 = rgb(20, 22, 26),
    .text2 = rgb(64, 67, 71),
    .text3 = rgb(102, 105, 111),
    .text4 = rgb(149, 152, 158),
    .textInverse = rgb(255, 255, 255),
    .hudFill = rgb(255, 255, 255, 179),
    .hudBorder = rgb(0, 0, 0, 20),
    .axisX = rgb(185, 70, 66),
    .axisY = rgb(57, 133, 38),
    .axisZ = rgb(15, 116, 197),
    .axisW = rgb(140, 84, 178),
};

// Studio's type scale, by PrKitTextRole. Captions are muted and meta text is faint, and the mono roles, which set
// identifiers, tags and readouts, stay on one line.
constexpr PrKitType kTypes[PrKitTextRole_Count] = {
    {.size = 12, .weight = 400, .lineHeight = 16},                                     // Body
    {.size = 11, .weight = 400, .lineHeight = 14},                                     // BodySmall
    {.size = 12, .weight = 500, .lineHeight = 16},                                     // Strong
    {.italic = true, .size = 12, .weight = 400, .lineHeight = 16},                     // BodyItalic
    {.size = 16, .weight = 500, .lineHeight = 20},                                     // Heading
    {.size = 22, .weight = 500, .lineHeight = 28, .letterSpacing = -0.22f},            // Title
    {.size = 40, .weight = 600, .lineHeight = 48, .letterSpacing = -0.8f},             // Display
    {                                                                                  // Caption
        .mono = true,
        .tone = PrKitTone_Muted,
        .wrap = PrKitWrap_Nowrap,
        .size = 10,
        .weight = 500,
        .lineHeight = 12,
        .letterSpacing = 1,
    },
    { // Meta
        .mono = true,
        .tone = PrKitTone_Faint,
        .wrap = PrKitWrap_Nowrap,
        .size = 10,
        .weight = 400,
        .lineHeight = 12,
    },
    {.mono = true, .wrap = PrKitWrap_Nowrap, .size = 11, .weight = 400, .lineHeight = 14},     // Mono
    {.mono = true, .wrap = PrKitWrap_Nowrap, .size = 11, .weight = 500, .lineHeight = 14},     // MonoStrong
    {.mono = true, .wrap = PrKitWrap_Nowrap, .size = 22, .weight = 500, .lineHeight = 26},     // MonoNum
};

// The palette's colors, by the names of their custom properties.
struct NamedColor {
    const char* name;
    PrColor PrKitPalette::*color;
};

constexpr NamedColor kColors[] = {
    {"accent", &PrKitPalette::accent},
    {"accent-weak", &PrKitPalette::accentWeak},
    {"text-selection", &PrKitPalette::textSelection},
    {"accent-text", &PrKitPalette::accentText},
    {"accent-hover", &PrKitPalette::accentHover},
    {"accent-press", &PrKitPalette::accentPress},
    {"success", &PrKitPalette::success},
    {"warning", &PrKitPalette::warning},
    {"danger", &PrKitPalette::danger},
    {"bg0", &PrKitPalette::bg0},
    {"bg1", &PrKitPalette::bg1},
    {"bg2", &PrKitPalette::bg2},
    {"bg3", &PrKitPalette::bg3},
    {"bg4", &PrKitPalette::bg4},
    {"bg-viewport", &PrKitPalette::bgViewport},
    {"line1", &PrKitPalette::line1},
    {"line2", &PrKitPalette::line2},
    {"line3", &PrKitPalette::line3},
    {"text1", &PrKitPalette::text1},
    {"text2", &PrKitPalette::text2},
    {"text3", &PrKitPalette::text3},
    {"text4", &PrKitPalette::text4},
    {"text-inverse", &PrKitPalette::textInverse},
    {"hud-fill", &PrKitPalette::hudFill},
    {"hud-border", &PrKitPalette::hudBorder},
    {"axis-x", &PrKitPalette::axisX},
    {"axis-y", &PrKitPalette::axisY},
    {"axis-z", &PrKitPalette::axisZ},
    {"axis-w", &PrKitPalette::axisW},
};

// The text roles, by the names of their custom properties.
constexpr const char* kRoleNames[PrKitTextRole_Count] = {
    "body", "body-small", "strong", "body-italic", "heading", "title", "display", "caption", "meta", "mono",
    "mono-strong", "mono-num",
};

uint32_t channel(float value) {
    return uint32_t(value * 255.0f + 0.5f);
}

// Appends `family` as font-family writes it: a generic by its keyword, and any other family as a string.
void appendFamily(PrTextBuffer* out, PrFontFamily family) {
    if (family.generic != PrFontGeneric_None) {
        prTextBufferAppend(out, prFontGenericName(family.generic));
        return;
    }
    prTextBufferAppendChar(out, '"');
    for (size_t i = 0; i < family.name.size; ++i) {
        char c = family.name.data[i];
        if (c == '"' || c == '\\')
            prTextBufferAppendChar(out, '\\');
        prTextBufferAppendChar(out, c);
    }
    prTextBufferAppendChar(out, '"');
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrKitPalette prKitPaletteStudio(PrKitThemeVariant variant) {
    return variant == PrKitThemeVariant_Light ? kLight : kDark;
}

PrKitTheme prKitThemeStudio(PrKitThemeVariant variant, PrFontSet* PR_NULLABLE fonts) {
    PrKitTheme theme{};
    theme.palette = prKitPaletteStudio(variant);
    for (uint32_t role = 0; role < PrKitTextRole_Count; ++role)
        theme.types[role] = kTypes[role];
    theme.fonts = fonts;
    theme.ui = prFontFamilyGeneric(PrFontGeneric_SystemUi);
    theme.mono = prFontFamilyGeneric(PrFontGeneric_Monospace);
    return theme;
}

PrRawString prKitThemeCss(const PrKitTheme* theme) {
    const PrKitPalette& p = theme->palette;
    bool dark = 0.2126f * p.bg0.r + 0.7152f * p.bg0.g + 0.0722f * p.bg0.b < 0.5f;
    PrTextBuffer css = {};
    char line[160];
    prTextBufferAppend(&css, formatToBuffer(Span(line), ":root {{\n  color-scheme: {};\n", dark ? "dark" : "light"));
    for (const NamedColor& named : kColors) {
        PrColor c = p.*named.color;
        prTextBufferAppend(&css, formatToBuffer(Span(line), "  --kit-{}: rgb({} {} {} / {:.3f});\n", named.name,
            channel(c.r), channel(c.g), channel(c.b), c.a));
    }
    PrTextBuffer ui = {};
    PrTextBuffer mono = {};
    appendFamily(&ui, theme->ui);
    appendFamily(&mono, theme->mono);
    prTextBufferAppend(&css, "  --kit-family-ui: ");
    prTextBufferAppend(&css, prTextBufferView(&ui));
    prTextBufferAppend(&css, ";\n  --kit-family-mono: ");
    prTextBufferAppend(&css, prTextBufferView(&mono));
    prTextBufferAppend(&css, ";\n");
    for (uint32_t role = 0; role < PrKitTextRole_Count; ++role) {
        const PrKitType& type = theme->types[role];
        prTextBufferAppend(&css, formatToBuffer(Span(line), "  --kit-font-{}: {}{} {}px/{}px ", kRoleNames[role],
            type.italic ? "italic " : "", type.weight, type.size, type.lineHeight));
        prTextBufferAppend(&css, prTextBufferView(type.mono ? &mono : &ui));
        prTextBufferAppend(&css, formatToBuffer(Span(line), ";\n  --kit-tracking-{}: {}px;\n", kRoleNames[role],
            type.letterSpacing));
    }
    prTextBufferAppend(&css, "  font: var(--kit-font-body);\n  letter-spacing: var(--kit-tracking-body);\n"
        "  color: var(--kit-text1);\n}\n::selection { background: var(--kit-text-selection); }\n");
    prTextBufferReset(&ui);
    prTextBufferReset(&mono);
    return prTextBufferTake(&css);
}
