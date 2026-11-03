// SVG fonts: the glyphs of a font element, and the pairs that its hkern elements kern. SVG 1.1 defines SVG fonts, SVG
// Tiny 1.2 keeps them, and SVG 2 drops them.
//
// SVG11 - https://www.w3.org/TR/SVG11/fonts.html

#pragma once

#include "pr/array.h"
#include "pr/dom.h"
#include "pr/font_set.h"
#include "pr/path.h"
#include "pr/string.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

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
uint32_t svgFontMatch(const SvgFont& font, PrStringView text, size_t pos, PrStringView language, SvgArabicForm form,
    size_t* length);

// Returns how far glyph `second` moves toward glyph `first` when it follows `first`, in font units. The distance is
// that of the first hkern whose `u1` or `g1` names `first` and whose `u2` or `g2` names `second`
// (SVG11/fonts.html#KernElements).
float svgFontKerning(const SvgFont& font, uint32_t first, uint32_t second);

// A font-face element of a document's tree, which names an SVG font for a family (SVG11/fonts.html#FontFaceElement).
struct SvgFontFace {
    PrString family;
    uint32_t familyHash;                // svgFontFamilyHash of `family`
    const PrDomElement* element;
    Array<PrFontCodePointRange> ranges; // its unicode-range; empty means every code point
    const SvgFont* PR_NULLABLE font;    // its font, once found, if the face's document holds it
    bool resolved;                      // true once `font` is found
};

// Called with the hash of a family (svgFontFamilyHash) whose faces or fonts may have changed.
typedef void (*SvgFontFamilyChangedFn)(void* PR_NULLABLE context, uint32_t familyHash);

// The SVG fonts of a document. The document's changes keep the faces current, and a font element is read once, when
// text first needs it, until a change under it. No font outlives its document.
struct SvgDocumentFonts {
    PrDomDocument* document;
    Array<SvgFontFace> faces;           // in tree order
    Array<SvgFont*> fonts;
    SvgFontFamilyChangedFn PR_NULLABLE changed;
    void* PR_NULLABLE changedContext;
};

// Makes the fonts of `document` from the font-face elements that it holds, and frees them.
SvgDocumentFonts* svgDocumentFontsCreate(PrDomDocument* document);
void svgDocumentFontsDestroy(SvgDocumentFonts* fonts);

// Keep the fonts current with the document's changes (dom_hooks.h).
void svgDocumentFontsInserted(SvgDocumentFonts* fonts, PrDomNode* node);
void svgDocumentFontsRemoving(SvgDocumentFonts* fonts, PrDomNode* node);
void svgDocumentFontsAttributeChanged(SvgDocumentFonts* fonts, const PrDomElement* element, PrStringView name);
void svgDocumentFontsResourceLoaded(SvgDocumentFonts* fonts);

// Sets who is told of a family's changes. A document's presentation is the one that keeps trees built from its fonts.
void svgDocumentFontsSetChanged(SvgDocumentFonts* fonts, SvgFontFamilyChangedFn PR_NULLABLE changed, void* PR_NULLABLE context);

// Returns the font of the font element `element` of the document, read the first time that it is asked for.
const SvgFont* svgDocumentFontsLoad(SvgDocumentFonts* fonts, const PrDomElement* element);

// Returns what identifies a family, whose names match ignoring ASCII case.
uint32_t svgFontFamilyHash(PrStringView family);

// Replaces `out` with the ranges of a unicode-range. The text is a comma-separated list of ranges of code points, each
// starting with `U+`. A `?` stands for every hex digit. Returns false if `text` does not parse.
bool svgParseUnicodeRange(PrStringView text, Array<PrFontCodePointRange>* out);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
