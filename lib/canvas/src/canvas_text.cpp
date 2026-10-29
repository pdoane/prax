// Canvas text: the fonts of the text styles, and text set as the text preparation algorithm sets it.
//
// html - https://html.spec.whatwg.org/

#include "canvas/src/canvas_text.h"

#include "pr/alloc.h"
#include "pr/ascii.h"
#include "pr/float.h"

namespace pr_impl {

namespace {

// Returns the text font of `font` at `width` and `locale`, made again if either differs from the last text font's.
PrTextFont* textFontOf(CanvasFont* font, PrFontSet* set, float width, PrLocale locale) {
    if (font->text != nullptr && font->textWidth == width && font->textLocale == locale)
        return font->text;
    prTextFontRelease(font->text);
    PrFontStyle style = prCssFontStyle(font->css);
    style.width = width;
    PrTextFontDesc desc{.set = set,
                        .families = prCssFontFamilies(font->css),
                        .familyCount = prCssFontFamilyCount(font->css),
                        .style = style,
                        .locale = locale};
    font->text = prTextFontCreate(&desc);
    font->textWidth = width;
    font->textLocale = locale;
    return font->text;
}

// Returns a metric of a font in ems, rounded to the font's design units, so that a metric that the font gives as a
// whole number of units measures exactly that many units at every size.
double emsOf(float metric, uint32_t unitsPerEm) {
    return unitsPerEm != 0 ? roundF64(double(metric) * unitsPerEm) / unitsPerEm : double(metric);
}

} // namespace

CanvasFont* canvasFontCreate(PrCssFont* css) {
    CanvasFont* font = allocZeroed<CanvasFont>();
    font->references = 1;
    font->css = css;
    return font;
}

void canvasFontRetain(CanvasFont* font) {
    ++font->references;
}

void canvasFontRelease(CanvasFont* PR_NULLABLE font) {
    if (font == nullptr || --font->references != 0)
        return;
    prTextFontRelease(font->text);
    prCssFontDestroy(font->css);
    prMemoryFree(font);
}

void canvasTextInit(CanvasText* text) {
    *text = CanvasText{.layout = prTextLayoutCreate()};
}

void canvasTextReset(CanvasText* text) {
    prTextLayoutDestroy(text->layout);
    textReset(&text->prepared);
    text->glyphs.reset();
    text->runs.reset();
    *text = CanvasText{};
}

bool canvasTextSet(CanvasText* text, PrStringView source, const CanvasTextStyle& style, PrFontSet* set,
                   PrTextShaper* shaper) {
    text->glyphs.clear();
    text->runs.clear();
    text->width = 0;
    text->ink = PrRect{};
    text->ascent = text->descent = text->emTop = text->emBottom = 0;
    text->hanging = text->alphabetic = text->ideographic = 0;
    PrTextFont* font = textFontOf(style.font, set, style.width, style.locale);
    prTextFontUpdate(font);
    uint32_t first = prTextFontFirst(font);
    if (first == kPrTextMatchNone)
        return false;
    float size = prCssFontStyle(style.font->css).size;

    // The text's ASCII whitespace is spaces.
    textReset(&text->prepared);
    textAppend(&text->prepared, source);
    for (uint32_t i = 0; i < text->prepared.text.count; ++i) {
        if (asciiIsSpace(text->prepared.text.data[i]))
            text->prepared.text.data[i] = ' ';
    }
    PrTextLayoutDesc desc{.text = textView(&text->prepared),
                          .font = font,
                          .shaper = shaper,
                          .direction = style.direction,
                          .align = PrTextAlign_Start,
                          .kerning = style.kerning,
                          .caps = style.caps,
                          .synthesis = PrTextSynthesisFlag_SmallCaps | PrTextSynthesisFlag_Position};
    prTextLayoutSet(text->layout, &desc);

    // The glyphs, spaced: letter spacing follows each character, and word spacing each space. The width is the advance
    // of the glyphs as shaped, and then the spacing.
    float baseline = prTextLayoutLineCount(text->layout) != 0 ? prTextLayoutLine(text->layout, 0).baseline : 0;
    double left = kInfinityF64;
    double right = -kInfinityF64;
    double spacing = 0;
    uint32_t runCount = prTextLayoutRunCount(text->layout);
    for (uint32_t r = 0; r < runCount; ++r) {
        PrFontGlyphRun run = prTextLayoutRun(text->layout, r);
        uint16_t space = prFontCodePointGlyph(run.font, ' ');
        for (uint32_t g = 0; g < run.glyphCount; ++g) {
            PrFontGlyph glyph = run.glyphs[g];
            double advance = double(prFontGlyphAdvance(run.font, glyph.id, 0)) * double(run.size);
            left = minF64(left, double(glyph.position.x));
            right = maxF64(right, double(glyph.position.x) + advance);
            double x = double(glyph.position.x) + spacing;
            spacing += style.letterSpacing + (glyph.id == space ? style.wordSpacing : 0);
            text->glyphs.push(PrFontGlyph{glyph.id, PrVec2{float(x), glyph.position.y - baseline}});
        }
    }
    if (!(left < kInfinityF64)) {
        left = 0;
        right = 0;
    }
    text->width = maxF64(right - left + spacing, 0);

    // The anchor point: across, by the alignment and the direction, and down, by the baseline of the first available
    // font that the text baseline names. The em box is the font's ascent and descent scaled to an em.
    PrFontMetrics metrics = prFontMetrics(prTextFontMatchedFont(font, first));
    uint32_t units = metrics.unitsPerEm;
    double ascent = emsOf(metrics.ascent, units) * double(size);
    double descent = emsOf(metrics.descent, units) * double(size);
    double hanging = emsOf(metrics.hangingBaseline, units) * double(size);
    double ideographic = emsOf(metrics.ideographicUnder, units) * double(size);
    bool rtl = style.direction == PrTextDirection_Rtl;
    double anchorX = left;
    if (style.align == PrCanvasTextAlign_Right || (style.align == PrCanvasTextAlign_Start && rtl)
        || (style.align == PrCanvasTextAlign_End && !rtl))
        anchorX = left + text->width;
    else if (style.align == PrCanvasTextAlign_Center)
        anchorX = left + text->width / 2;
    double emOver = ascent + descent > 0 ? ascent / (ascent + descent) * double(size) : double(size);
    double emUnder = double(size) - emOver;
    double anchorY = 0;
    switch (style.baseline) {
        case PrCanvasTextBaseline_Top: anchorY = -emOver; break;
        case PrCanvasTextBaseline_Hanging: anchorY = hanging; break;
        case PrCanvasTextBaseline_Middle: anchorY = (emUnder - emOver) / 2; break;
        case PrCanvasTextBaseline_Ideographic: anchorY = ideographic; break;
        case PrCanvasTextBaseline_Bottom: anchorY = emUnder; break;
        default: break;
    }
    // Adding zero makes a negative zero positive, as a metric that is zero reads.
    text->ascent = -ascent - anchorY + 0.0;
    text->descent = descent - anchorY + 0.0;
    text->emTop = -emOver - anchorY + 0.0;
    text->emBottom = emUnder - anchorY + 0.0;
    text->hanging = hanging - anchorY + 0.0;
    text->alphabetic = -anchorY + 0.0;
    text->ideographic = ideographic - anchorY + 0.0;

    // The runs, over the placed glyphs, and the bounds of their outlines.
    uint32_t at = 0;
    for (uint32_t r = 0; r < runCount; ++r) {
        PrFontGlyphRun run = prTextLayoutRun(text->layout, r);
        for (uint32_t g = 0; g < run.glyphCount; ++g) {
            PrFontGlyph& glyph = text->glyphs.data[at + g];
            glyph.position = PrVec2{float(double(glyph.position.x) - anchorX),
                                    float(double(glyph.position.y) - anchorY)};
            PrRect bounds = prFontGlyphBounds(run.font, glyph.id);
            if (prRectIsEmpty(bounds))
                continue;
            PrRect placed{glyph.position.x + bounds.left * run.size, glyph.position.y + bounds.top * run.size,
                          glyph.position.x + bounds.right * run.size, glyph.position.y + bounds.bottom * run.size};
            text->ink = prRectIsEmpty(text->ink) ? placed : prRectUnion(text->ink, placed);
        }
        run.glyphs = text->glyphs.data + at;
        text->runs.push(run);
        at += run.glyphCount;
    }
    return true;
}

} // namespace pr_impl
