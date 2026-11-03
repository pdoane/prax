// SVG text: a text element's characters laid out when the tree is built, and drawn from what the layout recorded.
//
// SVG2 - https://svgwg.org/svg2-draft/text.html
// SVG11 - https://www.w3.org/TR/SVG11/text.html

#pragma once

#include "svg/src/svg_draw.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// What a build keeps between its text elements: the SVG fonts that their families name, and the host's text fonts.
struct SvgTextFonts;

// Lays out the text element or SVG Tiny 1.2 textArea `element`, whose style is `style`, adds a node for it under
// `parent`, and returns the node. `instance` says whether the element is in a use element's shadow tree, whose elements
// inherit from their parents there.
SvgNodeIndex svgBuildText(SvgBuilder& b, const PrDomElement* element, const CssComputedStyle* style,
    SvgNodeIndex parent, bool instance);

// Draws a text node's glyphs and decorations. Its children, which the glyphs of SVG fonts draw, are drawn after them.
void svgDrawText(SvgDrawer& d, const SvgNode& node);

// Returns the bounds of the text's glyph cells, which a bounding-box paint server maps onto.
PrRect svgTextBounds(const SvgText* text);

// An addressable character of a text as laid out (SVG2/text.html#TermAddressableCharacter), which the methods of
// SVGTextContentElement measure. The characters of a typographic character share its glyphs' advance equally, each
// placed after the one before it along the glyphs' direction.
PR_NO_PADDING_BEGIN
struct SvgTextCharacter {
    PrVec2 position;            // the glyph's origin on its baseline, in the text's user space
    float advance;              // along the direction that `rotate` turns the x-axis to, spacing included
    float rotate;               // in degrees: the rotate attribute's rotation plus the text path's
    float direction;            // in degrees: the direction of the advance, down the line for upright vertical text

    // The extent of the cell of the glyph's font above and below its baseline.
    float ascent;
    float descent;

    uint8_t units;              // UTF-16 code units, which the methods index by; none if not addressable
    bool hidden;                // off the end of a text path, or a line break's character
    uint8_t _pad[2];
};
PR_NO_PADDING_END

// The characters of one element of a text: the text element itself, or a tspan, textPath or a within it, and what
// decides whether a pointer over their cells hits the element (SVG2/interact.html#PointerEventsProp).
struct SvgTextElement {
    const PrDomElement* element;

    // The element's range of svgTextCharacters.
    uint32_t start;
    uint32_t end;

    SvgPointerEvents pointerEvents;
    bool visible;
    bool painted;               // the element's fill or stroke is not none
};

// Returns the characters of the text in logical order, which are valid until the text is destroyed.
uint32_t svgTextCharacterCount(const SvgText* text);
const SvgTextCharacter* PR_NULLABLE svgTextCharacters(const SvgText* text);

// Returns the elements of the text, the text element first and then its descendants in tree order, which are valid
// until the text is destroyed.
uint32_t svgTextElementCount(const SvgText* text);
const SvgTextElement* PR_NULLABLE svgTextElements(const SvgText* text);

// Returns the range of the characters of `element`, or null if the text does not hold the element.
const SvgTextElement* PR_NULLABLE svgTextElementFromElement(const SvgText* text, const PrDomElement* element);

// Returns the paints of the text's elements, which the text's pieces paint with, and whose strokes the text owns. The
// paints are valid until the text is destroyed.
uint32_t svgTextPaintsCount(const SvgText* text);
const SvgPaints* PR_NULLABLE svgTextPaints(const SvgText* text);

// Returns true if two texts draw the same, each with the strokes and servers of its own tree. Otherwise writes what
// differs to `difference`.
bool svgTextSame(const SvgTree* tree, const SvgText* text, const SvgTree* expectedTree, const SvgText* expected,
    PrTextBuffer* difference);

// Frees what a text node holds, with the strokes that it owns in `tree`.
void svgTextDestroy(SvgTree* tree, SvgText* PR_NULLABLE text);

// Frees what a build kept for its text.
void svgTextFontsDestroy(SvgTextFonts* PR_NULLABLE fonts);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
