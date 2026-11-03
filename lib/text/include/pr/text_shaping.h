// Shaped text: everything about a text that does not depend on the width it is set in, and the algorithms that divide
// it into lines.
//
// UAX9 - https://www.unicode.org/reports/tr9/
// UAX14 - https://www.unicode.org/reports/tr14/
//
// A shaping serves any host that sets text in lines, such as an inline formatting context, a flexbox's measure function
// or a caption. It itemizes the text, shapes its runs and finds its break opportunities once. The host can then measure
// and break the text at any number of widths without shaping it again. A shaping holds only what it derives from the
// text, so the host passes the text to each call that reads it.
//
// An object in the text, such as an image or an inline block, is a piece whose advance the host provides. The host also
// owns what surrounds the lines. It handles alignment, and it handles floats by giving each line a narrower width.

#pragma once

#include "pr/base.h"
#include "pr/string_view.h"
#include "pr/text.h"
#include "pr/text_break.h"
#include "pr/text_font.h"
#include "pr/text_itemize.h"
#include "pr/unicode_line_break.h"
#include "pr/vec.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// A text's runs, glyphs and break opportunities.
typedef struct PrTextShaping PrTextShaping;

// What a byte of the text is as white space.
enum {
    PrTextSpaceFlag_Collapsible = 1 << 0,   // a space that is removed if a line ends after it
    PrTextSpaceFlag_Hangs = 1 << 1,         // white space that stays past the width if a line ends after it
    PrTextSpaceFlag_Tab = 1 << 2,           // a preserved tab, which has no glyph and advances to the next tab stop
    PrTextSpaceFlag_Wraps = 1 << 3,         // white space after which a line may wrap
    PrTextSpaceFlag_White = PrTextSpaceFlag_Collapsible | PrTextSpaceFlag_Hangs | PrTextSpaceFlag_Tab,
};
typedef uint8_t PrTextSpaceFlags;

// Where a line may break beside an object in the text.
enum {
    PrTextObjectBreak_Unicode,      // where UAX14 allows a break beside U+FFFC

    // On each side of the object, unless the code point there is of class GL, WJ or ZWJ. U+00A0 NO-BREAK SPACE allows a
    // break, although it is of class GL. No break stands at a forced break, or before a space or a hard break.
    PrTextObjectBreak_Around,
};
typedef uint8_t PrTextObjectBreak;

// Returns the advance of each glyph that is set upright in face `face` of the text font of `piece`, or zero for the
// advances that shaping gives. A host uses it for a face that has no vertical metrics.
typedef float (*PrTextUprightAdvanceFn)(void* PR_NULLABLE context, const PrTextPiece* piece, uint32_t face);

// The input of a shaping. The shaping copies what it keeps, so none of this needs to outlive the call.
typedef struct PrTextShapingDesc {
    PrStringView text;                  // UTF-8
    const PrTextPiece* pieces;
    uint32_t pieceCount;
    PrTextDirection direction;          // the base direction of each paragraph
    PrTextBidi bidi;                    // the unicode-bidi of what holds the text
    PrTextShaper* PR_NULLABLE shaper;   // may be null if the text has no run to shape
    float pixelRatio;                   // the target pixels per px that the text is drawn at, or zero for one

    // By byte: what each byte is as white space. A space without PrTextSpaceFlag_Wraps stops a line from wrapping after
    // it. Null means that each space hangs and wraps, and that no byte is a tab.
    const PrTextSpaceFlags* PR_NULLABLE spaces;

    // The distance between tab stops and the least advance of a tab, in px, if `spaces` has a tab. A tab with no
    // distance has no advance.
    float tabSize;
    float tabMinimum;

    PrTextObjectBreak objectBreak;

    // True if lines do not wrap at an opportunity that no Open piece around it holds on both sides.
    bool noWrap;

    PrTextUprightAdvanceFn PR_NULLABLE uprightAdvance;
    void* PR_NULLABLE uprightContext;
} PrTextShapingDesc;

// The scratch memory that shaping uses. One thread uses it at a time.
typedef struct PrTextShapingScratch PrTextShapingScratch;

// Creates the scratch memory, or frees it.
PrTextShapingScratch* prTextShapingScratchCreate(void);
void prTextShapingScratchDestroy(PrTextShapingScratch* PR_NULLABLE scratch);

// Creates an empty shaping, or frees one.
PrTextShaping* prTextShapingCreate(void);
void prTextShapingDestroy(PrTextShaping* PR_NULLABLE shaping);

// Shapes the text of `desc`. Replaces the contents of the shaping and keeps its storage. Sets the clusters of each Text
// piece in its text font (prTextFontMatch), whose faces may grow.
void prTextShapingSet(PrTextShaping* shaping, const PrTextShapingDesc* desc, PrTextShapingScratch* scratch);

// ---------------------------------------------------------------------------------------------------------------------
// What it holds

// A run from itemization and the range of its glyphs.
typedef struct PrTextShapedRun {
    PrTextItemRun item;
    uint32_t glyphStart;
    uint32_t glyphEnd;
} PrTextShapedRun;

// A shaped glyph, in px. Its advance is along the line, which is downward for an upright glyph. A shaping keeps its
// glyphs in the order of the text, so their clusters ascend. The glyphs of a right-to-left run are therefore drawn from
// the last to the first.
typedef struct PrTextShapedGlyph {
    uint32_t cluster;                   // the first byte that it was shaped from
    uint32_t run;
    float advance;
    PrVec2 offset;
    uint16_t id;
    float scale;                        // the em of the glyph relative to the em of the run; one unless synthesized
} PrTextShapedGlyph;

// A range of the text that is broken and measured separately, as its bytes and its pieces. A Break piece with no bytes
// ends a segment. A mandatory break follows the last byte of a segment.
typedef struct PrTextShapingSegment {
    uint32_t byteStart;
    uint32_t byteEnd;
    uint32_t pieceStart;
    uint32_t pieceEnd;
} PrTextShapingSegment;

// What a shaping holds. The lists stay valid until the shaping is set again.
typedef struct PrTextShapingInfo {
    const PrTextShapedRun* PR_NULLABLE runs;                // in the order of the text
    uint32_t runCount;
    const PrTextShapedGlyph* PR_NULLABLE glyphs;
    uint32_t glyphCount;
    const PrTextShapingSegment* segments;                   // at least one
    uint32_t segmentCount;
    const PrTextParagraph* PR_NULLABLE paragraphs;          // the bidi paragraphs, or none if `levels` is null
    uint32_t paragraphCount;

    const float* PR_NULLABLE advances;                      // by byte: the advance of the glyphs shaped from it

    // By byte and one past the last: the sum of the advances of the bytes before it. Sums of doubles do not gather the
    // rounding error of a long text.
    const double* reach;

    // By byte and one past the last: the first glyph that was shaped from that byte or a later one.
    const uint32_t* glyphAt;

    // By byte and one past the last: the break opportunity at that offset, after the byte before it. The first is
    // Prohibited.
    const PrUnicodeLineBreak* breaks;

    // By byte and one past the last: the emergency opportunity at that offset. Null if the text has none, or if it has
    // only those of overflow-wrap: break-word, which prTextShapingBreakLine finds when a word fits on no line.
    const PrTextEmergency* PR_NULLABLE emergency;

    // By byte: the bidi level as UAX9 resolves it before the rules of a line. Null if every level is even.
    const uint8_t* PR_NULLABLE levels;

    // By index of a piece: the level that UAX9 gives the control that an isolating Open or Close adds, or
    // kPrTextLevelNone. Null if `levels` is null.
    const uint8_t* PR_NULLABLE marks;
} PrTextShapingInfo;

// Returns what the shaping holds.
PrTextShapingInfo prTextShapingInfo(const PrTextShaping* shaping);

// Returns the level of the bidi paragraph that holds byte `byte`.
uint8_t prTextShapingParagraphLevel(const PrTextShaping* shaping, uint32_t byte);

// ---------------------------------------------------------------------------------------------------------------------
// Measuring

// Writes the position of each byte offset of `segment` along one unbroken line to `out`. `out` is indexed by byte of
// the whole text, and has room for the offset at the end of the segment. A position is the sum of the advances of the
// bytes before it and of the pieces around those bytes.
//
// `advances` holds, by piece, the advance that each Open, Close, Object or Boundary piece adds, or is null for none. An
// advance can be the width of an object, or the margin, border and padding of an inline box. A Close piece counts with
// the byte before it, and the others count with the byte after them. A piece at either end of the segment counts with
// the nearest byte of the segment.
void prTextShapingPositions(const PrTextShaping* shaping, const PrTextShapingSegment* segment,
    const float* PR_NULLABLE advances, float* out);

// How far the width of a line may exceed its space and still fit, in px, which allows for the error of summing
// advances.
static const float kPrTextLineFitTolerance = 1.0f / 64;

// A range of the text that a call measures or breaks as one line.
typedef struct PrTextShapingLine {
    uint32_t start;
    uint32_t end;                       // the end of the line, or the furthest that a break may reach

    // The distance from the origin of the tab stops to the start of the line.
    float origin;

    // The positions of the segment that holds the line (prTextShapingPositions), or null if no piece has an advance.
    const float* PR_NULLABLE positions;
} PrTextShapingLine;

// Returns the width of the line. The width includes each tab up to its stop, and leaves out the hanging white space at
// the end of the line.
float prTextShapingLineWidth(const PrTextShaping* shaping, const PrTextShapingLine* line);

// Returns the advance of a preserved tab at `x`, the distance along the line from the origin of the tab stops. A tab
// advances to the next tab stop, or to the one after that if the next is nearer than the least advance of a tab.
float prTextShapingTabAdvance(const PrTextShaping* shaping, float x);

// The intrinsic widths of a text. The min-content width breaks at every opportunity, and the max-content width breaks
// only at mandatory breaks.
typedef struct PrTextIntrinsic {
    float minContent;
    float maxContent;
} PrTextIntrinsic;

// Returns the intrinsic widths of the bytes from `start` to `end`, where `start` begins a line. `minPositions` and
// `maxPositions` are the positions (prTextShapingPositions) that each width uses, or null if no piece has an advance.
// Of the emergency opportunities, only PrTextEmergency_Anywhere counts for the min-content width.
PrTextIntrinsic prTextShapingIntrinsic(const PrTextShaping* shaping, uint32_t start, uint32_t end,
    const float* PR_NULLABLE minPositions, const float* PR_NULLABLE maxPositions);

// ---------------------------------------------------------------------------------------------------------------------
// Breaking

// Scratch memory that holds the opportunities inside one long word, so that the lines that break the word find them
// once. One thread uses it at a time.
typedef struct PrTextWordBreaks PrTextWordBreaks;

// Creates the scratch memory, or frees it.
PrTextWordBreaks* prTextWordBreaksCreate(void);
void prTextWordBreaksDestroy(PrTextWordBreaks* PR_NULLABLE words);

// Forgets the word that the memory holds. Call it before breaking another text.
void prTextWordBreaksClear(PrTextWordBreaks* words);

// Returns where a greedy break in `width` ends the line that starts at `line->start` and may reach `line->end`. The end
// is the first mandatory break that fits, or else the last opportunity that fits. If no opportunity fits, the end is
// the last emergency opportunity that fits, or else the first emergency opportunity, or else the first opportunity.
//
// `text` is the text of the shaping. `words` keeps the opportunities inside a long word. If it is null, each line finds
// them again. A host beside floats passes each line its own width.
uint32_t prTextShapingBreakLine(const PrTextShaping* shaping, PrStringView text, const PrTextShapingLine* line,
    float width, PrTextWordBreaks* PR_NULLABLE words);

// A part of one run on a line, with its bidi level.
typedef struct PrTextLineRun {
    uint32_t run;
    uint32_t start;
    uint32_t end;
    uint8_t level;
} PrTextLineRun;

// Writes the parts of the runs on the line from `start` to `end` to `out`, up to `capacity`, in visual order. Returns
// the total number of parts. The bytes from `content` to `end` are the white space at the end of the line, which takes
// the level of its paragraph (UAX9 L1). The parts are then reversed by level (L2). This call reorders whole runs, so a
// host whose inline boxes split runs must reorder those parts itself.
uint32_t prTextShapingLineRuns(const PrTextShaping* shaping, uint32_t start, uint32_t end, uint32_t content,
    PrTextLineRun* PR_NULLABLE out, uint32_t capacity);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
