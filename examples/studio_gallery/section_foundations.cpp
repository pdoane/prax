// The gallery's foundations: the principles, color, type, space and icons that every widget is drawn from.

#include "examples/studio_gallery/section_foundations.h"

#include "pr/kit_button.h"
#include "pr/kit_color.h"
#include "pr/kit_dock.h"
#include "pr/kit_icon.h"
#include "pr/kit_input.h"
#include "pr/kit_theme.h"

#include "examples/studio_gallery/gallery_parts.h"

namespace gallery {

namespace {

// A principle of the system.
struct Principle {
    const char* number;
    const char* title;
    const char* body;
};

constexpr Principle kPrinciples[] = {
    {"01", "Age well",
     "Neutral surfaces, zero radius, restrained color. What ships today still reads the same in year five."},
    {"02", "Density without noise", "A four-pixel grid and 22 to 26 pixel controls. Every pixel earns its place."},
    {"03", "Paths of discovery",
     "Progressive disclosure, a chord on every action, and search wherever a list grows long."},
    {"04", "Legible structure",
     "Monospaced ids and addresses, predictable node structure. A reader reasons about it."},
    {"05", "Indie to studio", "The same surfaces carry a solo project and a two hundred person team."},
    {"06", "Full parity in light", "Dark is the default. Light is never a downgrade."},
};

// A step of the surface ramp.
struct SurfaceStep {
    const char* label;
    const char* key;
    PrKitGround ground;
};

constexpr SurfaceStep kSurfaces[] = {
    {"App", "bg-0", PrKitGround_App},          {"Panel", "bg-1", PrKitGround_Panel},
    {"Header", "bg-2", PrKitGround_Header},    {"Raised", "bg-3", PrKitGround_Raised},
    {"Select", "accent-weak", PrKitGround_Select}, {"Viewport", "bg-viewport", PrKitGround_Viewport},
};

// A role of the type scale.
struct ScaleRow {
    const char* key;
    const char* meta;
    const char* sample;
    PrKitTextRole role;
    PrKitTone tone;
};

constexpr ScaleRow kScale[] = {
    {"Display", "40 / 600", "Hold it in your head", PrKitTextRole_Display, PrKitTone_Primary},
    {"Title", "22 / 500", "Section Title", PrKitTextRole_Title, PrKitTone_Primary},
    {"Heading", "16 / 500", "Panel Heading", PrKitTextRole_Heading, PrKitTone_Primary},
    {"Body", "12 / 400", "The quick brown fox jumps over the lazy dog. 0123456789", PrKitTextRole_Body,
     PrKitTone_Primary},
    {"BodySmall", "11 / 400", "Inspector row labels at eleven pixels.", PrKitTextRole_BodySmall, PrKitTone_Primary},
    {"Strong", "12 / 500", "Emphasis inside body text", PrKitTextRole_Strong, PrKitTone_Primary},
    {"BodyItalic", "12 / 400 . ITALIC", "A preview tab, a derived value", PrKitTextRole_BodyItalic,
     PrKitTone_Primary},
    {"Caption", "10 / 500 . MONO", "UPPERCASE . LABELS . META", PrKitTextRole_Caption, PrKitTone_Muted},
    {"Meta", "10 / 400 . MONO", "42 items . view . 2026-08-02", PrKitTextRole_Meta, PrKitTone_Faint},
    {"Mono", "11 / 400 . MONO", "entity_id: 0x7ff8_a2b0 . pos = (1.234, 0.000, -4.500)", PrKitTextRole_Mono,
     PrKitTone_Muted},
    {"MonoStrong", "11 / 500 . MONO", "player_base.mesh", PrKitTextRole_MonoStrong, PrKitTone_Primary},
    {"MonoNum", "22 / 500 . MONO", "128 . 2048 . 60.00fps", PrKitTextRole_MonoNum, PrKitTone_Primary},
};

// A tone of the emphasis and state ramp.
struct ToneRow {
    const char* name;
    PrKitTone tone;
};

constexpr ToneRow kTones[] = {
    {"Primary", PrKitTone_Primary}, {"Secondary", PrKitTone_Secondary}, {"Muted", PrKitTone_Muted},
    {"Faint", PrKitTone_Faint},     {"Accent", PrKitTone_Accent},       {"Success", PrKitTone_Success},
    {"Warning", PrKitTone_Warning}, {"Danger", PrKitTone_Danger},
};

// A step of the spacing scale.
struct SpaceStep {
    const char* key;
    const char* value;
    float px;
};

constexpr SpaceStep kSpaces[] = {
    {"--s-1", "2px", 2},   {"--s-2", "4px", 4},    {"--s-3", "6px", 6},    {"--s-4", "8px", 8},
    {"--s-5", "10px", 10}, {"--s-6", "12px", 12},  {"--s-8", "16px", 16},  {"--s-10", "20px", 20},
    {"--s-12", "24px", 24}, {"--s-16", "32px", 32},
};

// A control height, and the size of a button at that height.
struct HeightStep {
    const char* key;
    const char* value;
    PrKitControlSize size;
};

constexpr HeightStep kHeights[] = {
    {"--h-xs", "18px", PrKitControlSize_Xs},
    {"--h-sm", "22px", PrKitControlSize_Sm},
    {"--h-md", "26px", PrKitControlSize_Md},
    {"--h-lg", "30px", PrKitControlSize_Lg},
};

// The height of a band of color, in px.
constexpr float kBandHeight = 56;

// Returns `color` mixed toward white (a positive amount) or black (a negative one).
PrColor shade(PrColor color, float amount) {
    float target = amount > 0 ? 1.0f : 0.0f;
    float t = amount > 0 ? amount : -amount;
    return PrColor{color.r + (target - color.r) * t, color.g + (target - color.g) * t,
                   color.b + (target - color.b) * t, color.a};
}

// Creates a band of `band`'s color over its name and caption.
void rampCell(PrKit* kit, PrLayoutNode parent, PrStringView name, PrColor band, PrStringView note) {
    PrLayoutNode cell = card(kit, parent, 0, PrKitBoxStyle{});
    PrLayoutNode swatch = prKitSwatch(kit, cell, {.color = band}, {.size = kBandHeight});
    PrKitPlace wide{.width = prKitFill(), .flex = PrKitFlex_Fixed};
    prKitPlace(kit, swatch, &wide);
    PrKitBoxStyle text{.gap = kPrKitSpace1, .padding = prKitEdgesAll(kPrKitSpace4)};
    PrLayoutNode words = prKitBox(kit, cell, {}, text);
    prKitLabel(kit, words, {.text = name}, kCaption);
    if (note.size != 0)
        prKitLabel(kit, words, {.text = note}, kMetaMuted);
}

// Creates a row ruled below, laid out across with `gap` between its children, and returns it.
PrLayoutNode ruledRow(PrKit* kit, PrLayoutNode parent, float gap, PrKitAlign align) {
    PrKitSurfaceStyle row{.rules = PrKitRuleFlag_Bottom};
    row.box.direction = PrKitDirection_Row;
    row.box.align = align;
    row.box.gap = gap;
    row.box.padding = prKitEdgesXY(kPrKitSpace2, 0);
    row.box.place.flex = PrKitFlex_Fixed;
    return prKitSurface(kit, parent, {}, row);
}

// Creates a column `width` px wide in a row, and returns it.
PrLayoutNode fixedColumn(PrKit* kit, PrLayoutNode row, float width) {
    PrKitBoxStyle box{.direction = PrKitDirection_Row};
    box.place.width = prKitPx(width);
    box.place.flex = PrKitFlex_Fixed;
    return prKitBox(kit, row, {}, box);
}

// Creates a pane of a splitter's specimen, on `ground`, with `label` in it.
void pane(PrKit* kit, PrLayoutNode parent, PrKitGround ground, PrStringView label) {
    PrKitSurfaceStyle surface{.ground = ground};
    surface.box.padding = prKitEdgesAll(kPrKitSpace2);
    surface.box.place.flex = PrKitFlex_Fill;
    prKitLabel(kit, prKitSurface(kit, parent, {}, surface), {.text = label}, kCaption);
}

} // namespace

void sectionPrinciples(PrKit* kit, PrLayoutNode parent) {
    PrLayoutNode section = sectionFrame(kit, parent, "00", "Principles",
                                        "Six commitments the system answers to.", {});
    PrLayoutNode frame = card(kit, section, 0, PrKitBoxStyle{});
    Grid grid = gridBegin(kit, frame, 3, 0, true);
    for (const Principle& principle : kPrinciples) {
        PrKitBoxStyle body{.gap = kPrKitSpace3, .padding = prKitEdgesAll(kPrKitSpace5)};
        PrLayoutNode cell = prKitBox(kit, gridCell(grid), {}, body);
        prKitLabel(kit, cell, {.text = principle.number}, kCaptionAccent);
        prKitLabel(kit, cell, {.text = principle.title}, kStrong);
        prKitLabel(kit, cell, {.text = principle.body}, kBodySecondary);
    }
    gridEnd(grid);
}

void sectionColor(PrKit* kit, PrLayoutNode parent, GalleryState& state) {
    const PrKitPalette& palette = prKitTheme(kit)->palette;
    PrLayoutNode section = sectionFrame(
        kit, parent, "01", "Color", "Low-saturation neutrals. One signature accent.",
        "The neutral ramp reads as a tool rather than a toy. The accent carries selection, focus and the primary "
              "action, and nothing else; the semantic colors are reserved for state.");
    {
        PrLayoutNode panel = demoPanel(kit, section, "ACCENT RAMP", "10 STOPS", false);
        Grid grid = gridBegin(kit, panel, 5, kPrKitSpace2, false);
        const char* const names[] = {"accent-50",  "accent-100", "accent-200", "accent-300", "accent-400",
                                     "accent-500", "accent-600", "accent-700", "accent-800", "accent-900"};
        const float amounts[] = {0.9f, 0.75f, 0.6f, 0.4f, 0.2f, 0, -0.2f, -0.4f, -0.6f, -0.75f};
        for (uint32_t i = 0; i < 10; ++i)
            rampCell(kit, gridCell(grid), names[i], shade(palette.accent, amounts[i]),
                     i == 5 ? "primary" : PrStringView{});
        gridEnd(grid);
    }
    {
        PrLayoutNode panel = demoPanel(kit, section, "COLOR PICKER", "HSV . OKLCH . RAMP", false);
        PrKitBoxStyle line{.direction = PrKitDirection_Row, .align = PrKitAlign_Start, .gap = kPrKitSpace8};
        PrLayoutNode row = prKitBox(kit, panel, {}, line);
        PrKitColorPicker picker{.color = &state.color, .gamut = prUiDisplay(prKitUi(kit)).gamut};
        prKitColorPicker(kit, row, picker);
        PrLayoutNode fields = prKitBox(kit, row, {}, {.gap = kPrKitSpace3});
        PrKitPlace fixed{.width = prKitPx(220), .flex = PrKitFlex_Fixed};
        prKitLabel(kit, fields, {.text = "The same color in a field, which opens the picker in a popover:"},
                   kBodySecondary);
        prKitPlace(kit, prKitColorField(kit, fields, {.color = &state.color}, {}), &fixed);
    }
    {
        PrLayoutNode panel = demoPanel(kit, section, "GAMUTS", "SRGB . DISPLAY P3 . REC. 2020", false);
        prKitLabel(kit, panel,
                   {.text = "The same green in a picker of each gamut. A plane shows a color past its gamut mapped into "
                            "it, a line marks the gamut's edge, and a dashed line where sRGB ends inside a wider one. A "
                            "display shows the colors of a gamut wider than its own mapped into its own."},
                   kBodySecondary);
        PrKitBoxStyle line{.direction = PrKitDirection_Row, .align = PrKitAlign_Start, .gap = kPrKitSpace8};
        PrLayoutNode row = prKitBox(kit, panel, {}, line);
        const PrColorSpace gamuts[3] = {PrColorSpace_Srgb, PrColorSpace_DisplayP3, PrColorSpace_Rec2020};
        const char* const names[3] = {"sRGB", "Display P3", "Rec. 2020"};
        for (uint32_t i = 0; i < 3; ++i) {
            PrLayoutNode column = prKitBox(kit, row, {}, {.gap = kPrKitSpace2});
            prKitLabel(kit, column, {.text = names[i]}, kStrong);
            PrKitColorPicker picker{.key = i + 1,
                                    .color = &state.gamutColors[i],
                                    .mode = PrKitColorMode_Oklch,
                                    .gamut = gamuts[i],
                                    .opaque = true};
            prKitColorPicker(kit, column, picker);
        }
    }
    {
        PrLayoutNode panel = demoPanel(kit, section, "SURFACE RAMP", "6 STEPS", true);
        PrKitBoxStyle line{.direction = PrKitDirection_Row};
        PrLayoutNode row = prKitBox(kit, panel, {}, line);
        for (const SurfaceStep& step : kSurfaces) {
            PrKitSurfaceStyle surface{.ground = step.ground, .rules = PrKitRuleFlag_Right};
            surface.box.gap = kPrKitSpace1;
            surface.box.padding = prKitEdgesXY(kPrKitSpace3, kPrKitSpace4);
            surface.box.place.flex = PrKitFlex_Fill;
            PrLayoutNode cell = prKitSurface(kit, row, {}, surface);
            prKitLabel(kit, cell, {.text = step.label}, kStrong);
            prKitLabel(kit, cell, {.text = step.key}, kCaption);
        }
    }
    Grid pair = gridBegin(kit, section, 2, kPrKitSpace4, false);
    {
        PrLayoutNode panel = demoPanel(kit, gridCell(pair), "TEXT", "4 LEVELS", false);
        const ToneRow levels[] = {{"Primary - The quick brown fox", PrKitTone_Primary},
                                  {"Secondary - The quick brown fox", PrKitTone_Secondary},
                                  {"Tertiary - The quick brown fox", PrKitTone_Muted},
                                  {"Disabled - The quick brown fox", PrKitTone_Faint}};
        const char* const keys[] = {"text-1", "text-2", "text-3", "text-4"};
        for (uint32_t i = 0; i < 4; ++i) {
            PrLayoutNode row = ruledRow(kit, panel, kPrKitSpace2, PrKitAlign_Center);
            fillLabel(kit, row, levels[i].name, PrKitTextStyle{.tone = levels[i].tone});
            prKitLabel(kit, row, {.text = keys[i]}, kMetaMuted);
        }
    }
    {
        PrLayoutNode panel = demoPanel(kit, gridCell(pair), "SEMANTIC", "STATE", false);
        const PrColor colors[] = {palette.success, palette.warning, palette.danger, palette.accent};
        const char* const names[] = {"Success", "Warning", "Error", "Accent"};
        const char* const keys[] = {"success", "warning", "danger", "accent"};
        for (uint32_t i = 0; i < 4; ++i) {
            PrLayoutNode row = ruledRow(kit, panel, kPrKitSpace3, PrKitAlign_Center);
            prKitSwatch(kit, row, {.color = colors[i]}, {.size = kPrKitSpace6});
            fillLabel(kit, row, names[i], PrKitTextStyle{});
            prKitLabel(kit, row, {.text = keys[i]}, kMetaMuted);
        }
    }
    gridEnd(pair);
}

void sectionType(PrKit* kit, PrLayoutNode parent) {
    PrLayoutNode section = sectionFrame(
        kit, parent, "02", "Type", "One family, two voices.",
        "The sans face carries everything a person wrote; the monospaced face carries everything a machine "
              "produced -- coordinates, hashes, timestamps, chords and log lines. A label names a role and a tone, "
              "never a face, a size or a color.");
    PrLayoutNode panel = demoPanel(kit, section, "TYPE SCALE", "12 ROLES", false);
    for (const ScaleRow& scale : kScale) {
        PrLayoutNode row = ruledRow(kit, panel, kPrKitSpace5, PrKitAlign_Baseline);
        prKitLabel(kit, fixedColumn(kit, row, 100), {.text = scale.key}, kCaption);
        prKitLabel(kit, fixedColumn(kit, row, 110), {.text = scale.meta}, kMeta);
        fillLabel(kit, row, scale.sample,
                  PrKitTextStyle{.role = scale.role, .tone = scale.tone, .wrap = PrKitWrap_Clip});
    }
    PrLayoutNode tones = demoPanel(kit, section, "TONE", "EMPHASIS . STATE", false);
    for (const ToneRow& tone : kTones) {
        PrLayoutNode row = captionRow(kit, tones, tone.name, 100);
        fillLabel(kit, row, "The quick brown fox jumps over the lazy dog.",
                  PrKitTextStyle{.tone = tone.tone, .wrap = PrKitWrap_Clip});
    }
}

void sectionSpace(PrKit* kit, PrLayoutNode parent, GalleryState& state) {
    const PrKitPalette& palette = prKitTheme(kit)->palette;
    PrLayoutNode section = sectionFrame(kit, parent, "03", "Space & Grid",
                                        "A two-pixel base: dense, consistent, predictable.", {});
    Grid columns = gridBegin(kit, section, 2, kPrKitSpace4, false);
    {
        PrLayoutNode panel = demoPanel(kit, gridCell(columns), "SPACING SCALE", "2PX BASE", false);
        for (const SpaceStep& step : kSpaces) {
            PrKitBoxStyle line{.direction = PrKitDirection_Row, .align = PrKitAlign_Center, .gap = kPrKitSpace3};
            line.place.height = prKitPx(kPrKitHeightSm);
            line.place.flex = PrKitFlex_Fixed;
            PrLayoutNode row = prKitBox(kit, panel, {}, line);
            prKitLabel(kit, fixedColumn(kit, row, 60), {.text = step.key}, kCaption);
            prKitLabel(kit, fixedColumn(kit, row, 50), {.text = step.value}, kMeta);
            PrLayoutNode bar = prKitSwatch(kit, row, {.color = palette.accent}, {.size = kPrKitSpace6});
            PrKitPlace width{.width = prKitPx(step.px), .flex = PrKitFlex_Fixed};
            prKitPlace(kit, bar, &width);
        }
    }
    {
        PrLayoutNode panel = demoPanel(kit, gridCell(columns), "CONTROL HEIGHTS", "4 STEPS", false);
        for (const HeightStep& step : kHeights) {
            PrKitBoxStyle line{.direction = PrKitDirection_Row, .align = PrKitAlign_Center, .gap = kPrKitSpace4};
            line.place.height = prKitPx(kPrKitSpace16);
            line.place.flex = PrKitFlex_Fixed;
            PrLayoutNode row = prKitBox(kit, panel, {}, line);
            prKitLabel(kit, fixedColumn(kit, row, 60), {.text = step.key}, kCaption);
            prKitLabel(kit, fixedColumn(kit, row, 50), {.text = step.value}, kMeta);
            PrKitButton button{.text = "Button"};
            PrKitButtonStyle style{.size = step.size};
            prKitButton(kit, row, button, style);
        }
    }
    gridEnd(columns);
    // Two shares, and three, of a box that the splits divide by weight.
    PrLayoutNode splitters = demoPanel(kit, section, "SPLITTERS", "DRAG THE DIVIDERS", false);
    PrKitSplitStyle row{.direction = PrKitDirection_Row, .place = {.height = prKitPx(90), .flex = PrKitFlex_Fixed}};
    PrLayoutNode two = prKitSplit(kit, splitters, {.count = 2, .weights = state.splitWeights}, row);
    pane(kit, prKitSplitPane(kit, two, 0), PrKitGround_Header, "SHARE A");
    pane(kit, prKitSplitPane(kit, two, 1), PrKitGround_Panel, "SHARE B");
    PrLayoutNode three = prKitSplit(kit, splitters, {.count = 3, .weights = state.splitThirds}, row);
    pane(kit, prKitSplitPane(kit, three, 0), PrKitGround_Header, "1");
    pane(kit, prKitSplitPane(kit, three, 1), PrKitGround_Panel, "2");
    pane(kit, prKitSplitPane(kit, three, 2), PrKitGround_Header, "1");
}

void sectionIcons(PrKit* kit, PrLayoutNode parent) {
    PrLayoutNode section = sectionFrame(
        kit, parent, "04", "Icons", "One set, on a 24 by 24 grid, tinted by tone.",
        "Every glyph the kit draws, in the order the set declares them. Each is a path, drawn at any size, and "
              "the draw atlas keeps its coverage, so a grid of them draws from the atlas after the first frame.");
    PrLayoutNode panel = demoPanel(kit, section, "ICON SET", "EVERY GLYPH", true);
    Grid grid = gridBegin(kit, panel, 8, 0, true);
    for (PrKitIconKind icon = PrKitIconKind_None + 1; icon < PrKitIconKind_Count; ++icon) {
        PrKitBoxStyle cell{.justify = PrKitJustify_Center, .align = PrKitAlign_Center, .overflow = PrKitOverflow_Clip,
                      .gap = kPrKitSpace3};
        cell.place.height = prKitPx(64);
        cell.place.flex = PrKitFlex_Fixed;
        PrLayoutNode box = prKitBox(kit, gridCell(grid), {}, cell);
        prKitIcon(kit, box, {.icon = icon}, {.size = 18, .tone = PrKitTone_Secondary});
        PrKitTextStyle name{.role = PrKitTextRole_Meta, .wrap = PrKitWrap_Clip};
        prKitLabel(kit, box, {.text = prKitIconName(icon)}, name);
    }
    gridEnd(grid);
    PrLayoutNode sizes = demoPanel(kit, section, "SIZES", "ANY SIZE", false);
    PrKitBoxStyle line{.direction = PrKitDirection_Row, .align = PrKitAlign_Center, .gap = kPrKitSpace6};
    PrLayoutNode row = prKitBox(kit, sizes, {}, line);
    const float steps[] = {10, 12, 14, 18, 24, 32};
    for (float size : steps) {
        PrKitBoxStyle column{.align = PrKitAlign_Center, .gap = kPrKitSpace2};
        PrLayoutNode cell = prKitBox(kit, row, {}, column);
        prKitIcon(kit, cell, {.icon = PrKitIconKind_Atlas}, {.size = size, .tone = PrKitTone_Accent});
        char label[8];
        prKitLabel(kit, cell, {.text = format(label, sizeof label, "%dpx", int(size))}, kMeta);
    }
    PrLayoutNode legend = prKitBox(kit, sizes, {}, {.direction = PrKitDirection_Row, .gap = kPrKitSpace3});
    prKitLabel(kit, legend, {.text = "prKitIconName"}, kMonoStrong);
    prKitLabel(kit, legend, {.text = "names every glyph the grid above loops over"}, kBodySecondary);
}

} // namespace gallery
