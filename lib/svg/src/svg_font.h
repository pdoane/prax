// SVG fonts: the glyphs of a font element, and the pairs that its hkern elements kern. SVG 1.1 defines SVG fonts, SVG
// Tiny 1.2 keeps them, and SVG 2 drops them.
//
// SVG11 - https://www.w3.org/TR/SVG11/fonts.html

#pragma once

#include "pr/array.h"
#include "pr/dom.h"
#include "pr/font_set.h"
#include "pr/path.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// An element of a document (dom/src/dom_node.h).

// The form of an Arabic character that a glyph draws (SVG11/fonts.html#GlyphElementArabicFormAttribute). A character's
// form depends on which sides it joins on, and that depends on its joining type and the joining types of its neighbors.
// Any is the form of a glyph that names no form.
enum {
    SvgArabicForm_Any,
    SvgArabicForm_Isolated,
    SvgArabicForm_Initial,
    SvgArabicForm_Medial,
    SvgArabicForm_Terminal,
};
typedef uint8_t SvgArabicForm;

// A glyph of an SVG font. It is drawn from its path data and its child elements, in font units with y up.
struct SvgGlyph {
    PrStringView unicode;       // the characters it draws, UTF-8; empty for missing-glyph
    PrStringView names;         // glyph-name, a comma-separated list
    PrStringView languages;     // lang, a comma-separated list; the null view for any
    SvgArabicForm arabicForm;
    float advance;              // font units
    PrPath* PR_NULLABLE path;   // null for none

    // The glyph element, if its child elements draw it.
    const PrDomElement* PR_NULLABLE content;
};

// An hkern element. `u1` and `g1` name the first glyphs of the pairs by character and by name, and `u2` and `g2` the
// second.
struct SvgKern {
    PrStringView u1;
    PrStringView g1;
    PrStringView u2;
    PrStringView g2;
    float k;                    // the distance, in font units, that the second glyph moves toward the first
};

// A font element, with the metrics that its font-face gives.
struct SvgFont {
    const PrDomElement* element;
    float unitsPerEm;
    float ascent;               // font units above the baseline
    float descent;              // font units below the baseline, positive
    float ideographic;          // the baselines, in font units above the alphabetic one
    float hanging;
    float mathematical;
    float xHeight;
    float underlinePosition;    // the line's center, in font units above the baseline
    float underlineThickness;
    float overlinePosition;
    float overlineThickness;
    float strikethroughPosition;
    float strikethroughThickness;
    Array<SvgGlyph> glyphs;     // missing-glyph first, then the glyphs in document order
    Array<SvgKern> kerns;
};

// The index of the glyph that a font's missing-glyph element defines.
constexpr uint32_t kSvgMissingGlyph = 0;

// Reads the glyphs, kerning and metrics of the font element `font`.
void svgFontLoad(const PrDomElement* font, SvgFont* out);

// Frees the glyphs and kerning.
void svgFontReset(SvgFont* font);

// Returns the glyph for the text at `pos` (SVG11/fonts.html#GlyphElement), and writes the number of bytes it covers to
// `*length`. The glyph is the first glyph in document order whose characters start the text at `pos`. If the glyph has
// languages, `language` must be one of them or a subtag of one. If the glyph has an Arabic form, that form must be
// `form`, unless `form` is Any. If no glyph matches, the result is missing-glyph, covering one character.
uint32_t svgFontMatch(const SvgFont& font, PrStringView text, size_t pos, PrStringView language,
                      SvgArabicForm form, size_t* length);

// Returns how far glyph `second` moves toward glyph `first` when it follows `first`, in font units. The distance is
// that of the first hkern whose `u1` or `g1` names `first` and whose `u2` or `g2` names `second`
// (SVG11/fonts.html#KernElements).
float svgFontKerning(const SvgFont& font, uint32_t first, uint32_t second);

// Replaces `out` with the ranges of a unicode-range. The text is a comma-separated list of ranges of code points, each
// starting with `U+`. A `?` stands for every hex digit. Returns false if `text` does not parse.
bool svgParseUnicodeRange(PrStringView text, Array<PrFontCodePointRange>* out);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
