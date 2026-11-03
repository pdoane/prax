// Text layouts: text set in lines in a text font, for a producer that sets text outside the inline formatting of
// layout.
//
// A text layout itemizes its text and shapes each run once. It breaks the text into lines, no wider than the width that
// it is given, at the places at which text may break. It orders the runs of each line for display, and places them. It
// returns glyph runs that a draw list records (prDrawListGlyphs), the lines that they are on, and caret positions. One
// thread uses a layout at a time.

#pragma once

#include "pr/base.h"
#include "pr/font.h"
#include "pr/raw_string.h"
#include "pr/string_view.h"
#include "pr/text.h"
#include "pr/text_font.h"
#include "pr/vec.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// A text set in lines.
typedef struct PrTextLayout PrTextLayout;

// Where the content of a line goes in the width that the lines take: against its start or end, by the direction of its
// paragraph; against its left or right; or centered.
enum {
    PrTextAlign_Start,
    PrTextAlign_End,
    PrTextAlign_Left,
    PrTextAlign_Right,
    PrTextAlign_Center,
};
typedef uint8_t PrTextAlign;

// What a layout sets, and how. A zeroed description gives a line as wide as its text and as tall as the font, aligned
// at the start, left to right.
typedef struct PrTextLayoutDesc {
    PrStringView text;                          // UTF-8; a line feed ends a line and a paragraph
    PrTextFont* font;
    PrTextShaper* shaper;
    PrTextDirection direction;                  // the base direction of each paragraph
    PrTextAlign align;

    // The largest advance of the content of a line, in px. A line breaks at the last break opportunity before this
    // width. Zero or below means no limit, so that only line feeds break lines.
    float width;

    float lineHeight;                           // from the top of one line to the next, in px; zero for the font height

    // The settings that every run is shaped with (PrTextRun).
    const PrTextFeature* PR_NULLABLE features;
    uint32_t featureCount;
    PrTextKerning kerning;
    PrTextCaps caps;
    PrTextPosition position;
    PrTextSynthesisFlags synthesis;
} PrTextLayoutDesc;

// One line: its bytes of the text, including its line feed; its position, from the top left of the layout; and its
// runs.
typedef struct PrTextLine {
    uint32_t start;
    uint32_t end;
    float top;
    float baseline;
    float height;
    float left;                                 // the x at which the content of the line starts
    float width;                                // the advance of the content, excluding the white space that ends it
    uint32_t runStart;                          // the range of the line's runs, by prTextLayoutRun
    uint32_t runCount;
} PrTextLine;

// Creates an empty layout, or frees one.
PrTextLayout* prTextLayoutCreate(void);
void prTextLayoutDestroy(PrTextLayout* PR_NULLABLE layout);

// Sets the text of `desc` in lines, replacing the contents of the layout. The layout copies the text, and holds a
// reference to the font while it holds the result.
void prTextLayoutSet(PrTextLayout* layout, const PrTextLayoutDesc* desc);

// Returns the extent of the lines: the width that they are given if it is above zero, or else the width of their widest
// content; and the sum of their heights.
PrVec2 prTextLayoutExtent(const PrTextLayout* layout);

// Returns the lines, from the first.
uint32_t prTextLayoutLineCount(const PrTextLayout* layout);
PrTextLine prTextLayoutLine(const PrTextLayout* layout, uint32_t index);

// Returns the glyph runs of every line, with the runs of each line in order from its left, placed from the top left of
// the layout. A run stays valid until the layout is set again.
uint32_t prTextLayoutRunCount(const PrTextLayout* layout);
PrFontGlyphRun prTextLayoutRun(const PrTextLayout* layout, uint32_t index);

// Returns the byte of the text at which `run` starts: its first character in the order of the text.
uint32_t prTextLayoutRunStart(const PrTextLayout* layout, uint32_t run);

// Returns the position of a caret before byte `offset` of the text, which is the start of a grapheme cluster or the end
// of the content of a line. The position is a point on the baseline of its line, at the leading edge of the cluster in
// its direction.
PrVec2 prTextLayoutCaret(const PrTextLayout* layout, uint32_t offset);

// Returns the caret stop after byte `offset`: the start of the next grapheme cluster, or the end of the text.
uint32_t prTextLayoutNextCaret(const PrTextLayout* layout, uint32_t offset);

// Returns the caret stop nearest `point`, measured from the top left of the layout, as a byte of the text. The line is
// the one at the height of the point. On that line, the stop is before or after the cluster under the point, depending
// on which half of the cluster the point is in.
uint32_t prTextLayoutHitTest(const PrTextLayout* layout, PrVec2 point);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
