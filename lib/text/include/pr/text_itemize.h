// Itemization: dividing text into the runs that shaping takes, with their bidirectional levels and scripts.
//
// UAX9 - https://www.unicode.org/reports/tr9/
// UAX24 - https://www.unicode.org/reports/tr24/
// UAX50 - https://www.unicode.org/reports/tr50/
//
// A producer gives its text as pieces in logical order: runs of styled text, the starts and ends of elements, objects
// and paragraph breaks. Itemization resolves the levels of each paragraph, including the controls that the unicode-bidi
// of each element adds. It resolves the script and orientation of each cluster, and the face that each cluster is set
// in. It then divides the text into runs of one style, face, script, orientation and level. A run continues across
// pieces if nothing that affects shaping changes. What belongs to the producer -- lines, positions, its own fonts --
// stays with the producer.

#pragma once

#include "pr/base.h"
#include "pr/locale.h"
#include "pr/raw_string.h"
#include "pr/string_view.h"
#include "pr/text.h"
#include "pr/text_break.h"
#include "pr/text_font.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// How the content of an element takes part in bidirectional reordering: like the text around it, embedded or isolated
// in its direction, overriding the directions of its characters, both, or isolated in the direction of its own text.
enum {
    PrTextBidi_Normal,
    PrTextBidi_Embed,
    PrTextBidi_Isolate,
    PrTextBidi_BidiOverride,
    PrTextBidi_IsolateOverride,
    PrTextBidi_Plaintext,
};
typedef uint8_t PrTextBidi;

// How text is set on a line: across a horizontal line, or in one of three vertical typographic modes. In Mixed, each
// typographic character unit is upright or sideways by its Vertical_Orientation (UAX50). In Upright, every unit is
// upright except those of the vertical-only scripts, Mongolian and Phags-pa. In Sideways, every unit is sideways.
enum {
    PrTextOrientation_Horizontal,
    PrTextOrientation_Mixed,
    PrTextOrientation_Upright,
    PrTextOrientation_Sideways,
};
typedef uint8_t PrTextOrientation;

// Returns true if a typographic character unit whose first code point is `codePoint`, of the resolved script `script`,
// is set upright under `orientation`.
bool prTextIsUpright(PrTextOrientation orientation, uint32_t codePoint, PrScript script);

// The kinds of piece.
enum {
    PrTextPieceKind_Text,       // text in a style; a line feed in it is a paragraph separator and a forced break
    PrTextPieceKind_Open,       // the start of an element whose content may add bidi controls
    PrTextPieceKind_Close,      // the end of such an element
    PrTextPieceKind_Object,     // an object in the text, U+FFFC at `start`: neutral, or strong if it embeds
    PrTextPieceKind_Break,      // the end of a paragraph: a separator at [start, end), or none if they are equal
    PrTextPieceKind_Boundary,   // a place that ends shaping and takes no part in bidi, such as a float
    PrTextPieceKind_Anchor,     // a place that takes no part in shaping or bidi, such as a box out of flow
};
typedef uint8_t PrTextPieceKind;

// A piece of the text, in logical order. Text pieces cover the bytes of the text that they style, in order.
typedef struct PrTextPiece {
    PrTextPieceKind kind;
    PrTextBidi bidi;                    // Open, Close, Object: the unicode-bidi of the element
    PrTextDirection direction;          // Open, Close, Object: the direction of the element
    bool breaksShaping;                 // Open, Close: true if shaping does not join across it, as at a margin
    PrTextOrientation orientation;      // Text
    uint32_t start;
    uint32_t end;
    PrTextFont* PR_NULLABLE font;       // Text: the text font that its clusters are set in, or null for none
    uint32_t style;                     // Text: the producer's style; a run has one style, and its pieces one font

    // Text: the features that shaping applies, or null for the defaults, and how the text breaks within words. A
    // shaping reads these (pr/text_shaping.h), and itemization does not.
    const PrTextFeatureSet* PR_NULLABLE features;
    PrTextBreakStyle breakStyle;

    // Text: the size that the text is set at, in px, or zero for the size of its text font.
    float size;

    // Text: the space that shaping adds after each grapheme cluster, and after each word separator
    // (prTextIsWordSeparator), in px. A shaping reads these.
    float letterSpacing;
    float wordSpacing;

    bool noWrap;                        // Text, Open: true if lines do not wrap inside it
    bool breakSpaces;                   // Text: true if a line may break after any space or tab

    // Text: true if it is set as mathematical notation. Such text is shaped in the math script (ISO 15924 Zmth,
    // OpenType 'math') whatever the scripts of its characters, because a math font holds its script and dotless forms
    // there.
    bool math;
} PrTextPiece;

// The input of an itemization: the text, its pieces, and the direction and unicode-bidi of its container, which set the
// level of each paragraph (UAX9 P2, P3) and override it.
typedef struct PrTextItemizeDesc {
    PrStringView text;
    const PrTextPiece* pieces;
    uint32_t pieceCount;
    PrTextDirection direction;
    PrTextBidi bidi;

    // By byte, or null: true if the byte is shaped into nothing, as a preserved tab is. Such a byte ends a run.
    const bool* PR_NULLABLE unshaped;
} PrTextItemizeDesc;

// A paragraph: its bytes, and its level.
typedef struct PrTextParagraph {
    uint32_t start;
    uint32_t end;
    uint8_t level;
} PrTextParagraph;

// A run: text of one style, face, script, orientation and level, which shapes as one unit, with the text that it joins
// across as context. A run is shaped right to left at an odd level, and top to bottom if it is upright.
typedef struct PrTextItemRun {
    uint32_t start;
    uint32_t end;
    uint32_t contextStart;
    uint32_t contextEnd;
    uint32_t piece;                     // the first Text piece of the run's style, whose font sets the run
    uint32_t face;                      // an index among the faces of that piece's font, or kPrTextMatchNone
    PrScript script;
    uint8_t level;
    bool upright;
} PrTextItemRun;

// No level: for an isolating element whose controls X9 removed, or for a piece that adds no controls
// (prTextItemsMarks).
enum { kPrTextLevelNone = 0xff };

// The results of an itemization, and the scratch memory that later itemizations reuse. One thread uses it at a time.
typedef struct PrTextItems PrTextItems;

// Creates empty results, or frees them.
PrTextItems* prTextItemsCreate(void);
void prTextItemsDestroy(PrTextItems* PR_NULLABLE items);

// Itemizes `desc` into `items`, replacing their contents. Sets the clusters of each Text piece in its text font
// (prTextFontMatch), whose faces may grow.
void prTextItemsItemize(PrTextItems* items, const PrTextItemizeDesc* desc);

// Returns the level of each byte, or null if every level is even. That is the case for text in left-to-right scripts
// alone in a left-to-right container, which resolves nothing.
const uint8_t* PR_NULLABLE prTextItemsLevels(const PrTextItems* items);

// Returns the paragraphs. There are none if the levels are null.
uint32_t prTextItemsParagraphCount(const PrTextItems* items);
const PrTextParagraph* PR_NULLABLE prTextItemsParagraphs(const PrTextItems* items);

// Returns, by piece, the level that UAX9 gives the control that an isolating Open or Close adds, which places the piece
// on a line, or kPrTextLevelNone. Returns null if the levels are null.
const uint8_t* PR_NULLABLE prTextItemsMarks(const PrTextItems* items);

// Returns the resolved script of each byte (UAX24). The script of a cluster is the script of its first code point that
// is neither Common nor Inherited. A cluster of only Common and Inherited code points takes the script of the text
// before it in its paragraph, or of the text after it at the start of the paragraph. A closing bracket takes the script
// of the bracket that it closes. The script is Zzzz if a paragraph has no script.
const PrScript* PR_NULLABLE prTextItemsScripts(const PrTextItems* items);

// Returns, for each byte, true if it is set upright.
const bool* PR_NULLABLE prTextItemsUpright(const PrTextItems* items);

// Returns the runs, in the order of the text.
uint32_t prTextItemsRunCount(const PrTextItems* items);
const PrTextItemRun* PR_NULLABLE prTextItemsRuns(const PrTextItems* items);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
