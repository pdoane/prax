// Canvas text: the fonts of the text styles, and text set as the text preparation algorithm sets it.
//
// html - https://html.spec.whatwg.org/

#pragma once

#include "pr/array.h"
#include "pr/canvas.h"
#include "pr/css.h"
#include "pr/font.h"
#include "pr/font_set.h"
#include "pr/locale.h"
#include "pr/string_view.h"
#include "pr/text.h"
#include "pr/text_buffer.h"
#include "pr/text_font.h"
#include "pr/text_layout.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// A font of the text styles (html/#dom-context-2d-font): the font that CSS computed, and the text font that sets text
// in it, which is made the first time that text needs it. A font is reference counted, so that saved states share it.
struct CanvasFont {
    uint32_t references;
    PrCssFont* css;
    PrTextFont* PR_NULLABLE text;   // for the width and locale below
    float textWidth;
    PrLocale textLocale;
};

// Creates a font of `css`, which it takes, with one reference.
CanvasFont* canvasFontCreate(PrCssFont* css);

// Adds or releases a reference. The last release frees the font.
void canvasFontRetain(CanvasFont* font);
void canvasFontRelease(CanvasFont* PR_NULLABLE font);

// The text styles that the text preparation algorithm reads (html/#text-preparation-algorithm), resolved: the direction
// and language that inherit are the canvas's, spacing is in px, and a width or capitals of the font that an attribute
// overrides is the attribute's.
struct CanvasTextStyle {
    CanvasFont* font;
    PrCanvasTextAlign align;
    PrCanvasTextBaseline baseline;
    PrTextDirection direction;
    PrLocale locale;
    double letterSpacing;
    double wordSpacing;
    PrTextKerning kerning;
    PrTextCaps caps;
    float width;                    // percent of the normal width
};

// Text set on one line (html/#text-preparation-algorithm), placed with the anchor point at the origin, y down. A set
// keeps its storage from one text to the next.
struct CanvasText {
    PrTextLayout* layout;
    TextBuffer prepared;            // the text with its whitespace made spaces
    Array<PrFontGlyph> glyphs;      // every run's glyphs, spaced and placed from the anchor point
    Array<PrFontGlyphRun> runs;     // each a range of `glyphs`

    double width;                   // the advance of the inline box
    PrRect ink;                     // the bounds of the glyphs' outlines, or empty for none

    // The positions of the first available font's lines below the baseline that the text baseline names: its ascent and
    // descent, its em box's top and bottom, and its hanging, alphabetic and ideographic-under baselines. A line above
    // the baseline is negative.
    double ascent;
    double descent;
    double emTop;
    double emBottom;
    double hanging;
    double alphabetic;
    double ideographic;
};

// Makes an empty text, or frees one.
void canvasTextInit(CanvasText* text);
void canvasTextReset(CanvasText* text);

// Sets `source` in `style`, in the families of `set` and shaped with `shaper`. Returns false, and leaves the text
// empty, if the font has no face.
bool canvasTextSet(CanvasText* text, PrStringView source, const CanvasTextStyle& style, PrFontSet* set, PrTextShaper* shaper);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
