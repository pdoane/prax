// Text shaping: runs of text turned into positioned glyphs.
//
// A producer divides its text into runs (pr/text_itemize.h), lays runs into lines and places their glyphs. One thread
// uses a shaper at a time.

#pragma once

#include "pr/base.h"
#include "pr/font.h"
#include "pr/font_set.h"
#include "pr/geom.h"
#include "pr/locale.h"
#include "pr/status.h"
#include "pr/string.h"
#include "pr/string_view.h"
#include "pr/vec.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// A shaper. Each backend provides its own constructor. A shaper caches data that it derives from fonts.
typedef struct PrTextShaper PrTextShaper;

// The glyphs that a shaping produces, and scratch memory that later shapings reuse.
typedef struct PrTextGlyphBuffer PrTextGlyphBuffer;

// The direction in which text runs along a line: left to right, or right to left.
enum {
    PrTextDirection_Ltr,
    PrTextDirection_Rtl,
};
typedef uint8_t PrTextDirection;

// The axis along which the glyphs of a run advance: across a horizontal line, or down a vertical line with its glyphs
// upright.
enum {
    PrTextAxis_Horizontal,
    PrTextAxis_Vertical,
};
typedef uint8_t PrTextAxis;

// Pair kerning: the default of the font, or on or off regardless of that default.
enum {
    PrTextKerning_Auto,
    PrTextKerning_Normal,
    PrTextKerning_None,
};
typedef uint8_t PrTextKerning;

// Capital forms, as CSS font-variant-caps names them.
enum {
    PrTextCaps_Normal,
    PrTextCaps_Small,               // lowercase as small capitals (smcp)
    PrTextCaps_AllSmall,            // both cases as small capitals (c2sc, smcp)
    PrTextCaps_Petite,              // pcap, falling back to Small
    PrTextCaps_AllPetite,           // c2pc and pcap, falling back to AllSmall
    PrTextCaps_Unicase,             // uppercase as small capitals beside lowercase (unic)
    PrTextCaps_Titling,             // titl
};
typedef uint8_t PrTextCaps;

// Subscript and superscript forms, as CSS font-variant-position names them.
enum {
    PrTextPosition_Normal,
    PrTextPosition_Subscript,       // subs
    PrTextPosition_Superscript,     // sups
};
typedef uint8_t PrTextPosition;

// The forms that are synthesized when the font lacks the features that provide them. Small capitals are uppercase
// glyphs scaled to the x-height. Subscripts and superscripts are the glyphs of the run, scaled and moved by the metrics
// of the font.
enum {
    PrTextSynthesisFlag_SmallCaps = 1u << 0,
    PrTextSynthesisFlag_Position = 1u << 1,
};
typedef uint8_t PrTextSynthesisFlags;

// The value of a font feature over the bytes [start, end) of the text of the run.
typedef struct PrTextFeature {
    uint32_t tag;               // a tag such as PR_FONT_TAG('l', 'i', 'g', 'a')
    uint32_t value;             // zero is off, one is on, and a larger value selects an alternate
    uint32_t start;
    uint32_t end;
} PrTextFeature;

// A run: text in one font, direction, script and language, which the caller split from the text that it lays out. The
// text around the run is context. Shaping joins across the context but produces no glyphs for it, so an Arabic word
// split by a change of color stays joined. Backends differ in how much context they read.
typedef struct PrTextRun {
    const PrFont* font;
    PrStringView text;                          // UTF-8
    PrStringView before;                        // the text before the run, UTF-8
    PrStringView after;                         // the text after the run, UTF-8
    PrTextDirection direction;                  // shaped right to left if Rtl; a vertical run is shaped down
    PrTextAxis axis;

    PrScript script;                            // Zzzz for the script that the text starts in
    PrLocale locale;                            // the language the text is in, or zero for none

    // Features that follow the font's own features. A later feature wins over an earlier one.
    const PrTextFeature* PR_NULLABLE features;
    uint32_t featureCount;

    PrTextKerning kerning;
    PrTextCaps caps;
    PrTextPosition position;
    PrTextSynthesisFlags synthesis;

    // The pixels per em at which the run is drawn. A horizontal run of a hinted font uses its advances at this size
    // (prFontGlyphAdvance). Zero gives the design advances.
    float pixelsPerEm;
} PrTextRun;

// What a style asks of the shaping of its text: its capital and position forms, its kerning, the forms that may be
// synthesized, and font features. The features apply in order, after the font's own. A feature set is immutable and
// reference counted. A producer without a set shapes with the defaults: kerning as the font gives it, no capital or
// position forms, and synthesis of both small capitals and positions.
typedef struct PrTextFeatureSet PrTextFeatureSet;

// What a feature set is made from.
typedef struct PrTextFeatureSetDesc {
    PrTextCaps caps;
    PrTextPosition position;
    PrTextKerning kerning;
    PrTextSynthesisFlags synthesis;
    const PrTextFeature* PR_NULLABLE features; // each over the whole run
    uint32_t featureCount;
} PrTextFeatureSetDesc;

// Creates a feature set, with one reference for the caller, or adds or releases a reference. The last release frees the
// set.
PrTextFeatureSet* prTextFeatureSetCreate(const PrTextFeatureSetDesc* desc);
void prTextFeatureSetRetain(const PrTextFeatureSet* set);
void prTextFeatureSetRelease(const PrTextFeatureSet* PR_NULLABLE set);

// Returns true if two feature sets ask the same of shaping. Null stands for the defaults.
bool prTextFeatureSetEquals(const PrTextFeatureSet* PR_NULLABLE a, const PrTextFeatureSet* PR_NULLABLE b);

// Sets the forms, kerning, synthesis and features of `run` to what `set` asks, or to the defaults for null. The run
// reads the set's features until the set is released.
void prTextFeatureSetApply(const PrTextFeatureSet* PR_NULLABLE set, PrTextRun* run);

// Marks on a shaped glyph.
enum {
    // The first glyph of a grapheme cluster, in the order of the text. A caret stops before it.
    PrTextGlyphFlag_ClusterStart = 1u << 0,
};
typedef uint8_t PrTextGlyphFlags;

// A glyph and its position, in ems of the font of the run, with y down. The origin of the glyph is the pen position
// plus `offset`, and the pen then moves by `advance`. Shapers agree only on the position of each glyph. They differ in
// how they divide it between the offset and the preceding advances. The glyphs of a vertical run are placed by their
// horizontal origins, which is how every glyph is drawn.
//
// `cluster` is the first byte of the characters that the glyph was shaped from. If shaping reorders characters, the
// glyphs involved all take the first byte of those characters. Clusters therefore never decrease in reading order,
// which is right to left for a right-to-left run.
typedef struct PrTextGlyph {
    uint16_t id;
    uint32_t cluster;
    PrVec2 offset;
    PrVec2 advance;
    float scale;                // the em of the glyph relative to the em of the run; one unless synthesized
    PrTextGlyphFlags flags;
} PrTextGlyph;

// A shaper, placed at the start of the backend's state. It holds the functions that the backend implements, each named
// for what it does, and the sizes of the state that the backend keeps for a face and for a font.
//
// A face is a face of a font file as the backend reads it, and every font made from those bytes shares it. A variable
// instance or a derived font differs from its base in its design point and features, not in its tables, and reading the
// tables is the costly part. The state of a font therefore holds only what the font adds to its face. The shaper keeps
// the states of the fonts that it shaped with most recently, and of the faces that they read. Each state starts zeroed
// and stays at its address while it is kept. The shaper keeps alive the bytes that a face reads.
//
// prTextShaperShape passes `shape` the states of the font and face of the run. The run that it passes has its kerning,
// caps and position already expressed as features, with the font's own features before the run's. Its script is the one
// that the text starts in, which is Zzzz only for text with no script other than Common and Inherited.
// prTextShaperShape then scales and marks the glyphs that `shape` appends. One thread uses a shaper at a time. Threads
// that shape at the same time each use a shaper of their own.
struct PrTextShaper {
    void (*PR_NULLABLE destroy)(PrTextShaper* shaper);
    PrStatus (*shape)(PrTextShaper* shaper, void* face, void* font, const PrTextRun* run, PrTextGlyphBuffer* out);

    uint32_t faceSize;
    uint32_t fontSize;

    // Reads the face for the bytes and index of the font. Returns false if the backend cannot read it.
    bool (*createFace)(PrTextShaper* shaper, const PrFont* font, void* face);
    void (*destroyFace)(PrTextShaper* shaper, void* face);

    // Creates the state of the font over the face that it reads.
    void (*createFont)(PrTextShaper* shaper, void* face, const PrFont* font, void* state);
    void (*destroyFont)(PrTextShaper* shaper, void* state);

    struct PrTextFontCache* PR_NULLABLE fonts;      // the kept states; the backend leaves this field zeroed
};

// The library's backends of shapers, which the build turns on (README.md), and Default, the first that the build has of
// HarfBuzz, kb, CoreText and the simple shaper.
enum {
    PrTextShaperBackend_Default,
    PrTextShaperBackend_HarfBuzz,
    PrTextShaperBackend_Kb,
    PrTextShaperBackend_CoreText,
    PrTextShaperBackend_Simple,
    PrTextShaperBackend_Count,
};
typedef uint8_t PrTextShaperBackend;

// Returns the backend that `name` names, as prTextShaperBackendName spells it, ignoring case, or Count if it names
// none.
PrTextShaperBackend prTextShaperBackendFromName(PrStringView name);

// Returns the name of a backend in lowercase, as a command line spells it.
PrStringView prTextShaperBackendName(PrTextShaperBackend backend);

// Returns true if the build has the backend. The build has Default if it has any backend.
bool prTextShaperBackendIsBuilt(PrTextShaperBackend backend);

// Creates a shaper by the backend. A backend's own header documents how it shapes. Returns null if the build does not
// have the backend.
PrTextShaper* PR_NULLABLE prTextShaperCreate(PrTextShaperBackend backend);

// Frees the shaper.
void prTextShaperDestroy(PrTextShaper* PR_NULLABLE shaper);

// Replaces the contents of `out` with the glyphs of the run in visual order: left to right, or top to bottom. The
// clusters of a right-to-left run therefore descend. Returns NotSupported for a font or direction that the shaper
// cannot shape. On failure, `out` is empty.
PrStatus prTextShaperShape(PrTextShaper* shaper, const PrTextRun* run, PrTextGlyphBuffer* out);

// ---------------------------------------------------------------------------------------------------------------------
// Glyph buffers

// Creates an empty buffer, frees one, or empties one and keeps its storage.
PrTextGlyphBuffer* prTextGlyphBufferCreate(void);
void prTextGlyphBufferDestroy(PrTextGlyphBuffer* PR_NULLABLE buffer);
void prTextGlyphBufferClear(PrTextGlyphBuffer* buffer);

// Returns the glyphs, which stay valid until they change.
uint32_t prTextGlyphBufferGlyphCount(const PrTextGlyphBuffer* buffer);
const PrTextGlyph* PR_NULLABLE prTextGlyphBufferGlyphs(const PrTextGlyphBuffer* buffer);

// Appends `count` glyphs for a backend to fill in, and returns the first. Each has a scale of one and every other field
// zero. Growth invalidates the pointer that prTextGlyphBufferGlyphs returned.
PrTextGlyph* prTextGlyphBufferAppend(PrTextGlyphBuffer* buffer, uint32_t count);

// Returns the sum of the advances of the glyphs.
PrVec2 prTextGlyphBufferAdvance(const PrTextGlyphBuffer* buffer);

// Places the glyphs as the glyphs of a run at `size` px per em, with the pen at `origin` before the first glyph. Writes
// the id and position of each glyph to `out`, and, if `transforms` is not null, the scale of each glyph about its
// origin. Returns the pen position after the last glyph. `out` and `transforms` hold prTextGlyphBufferGlyphCount
// entries.
PrVec2 prTextGlyphBufferPlace(const PrTextGlyphBuffer* buffer, float size, PrVec2 origin, PrFontGlyph* out,
                              PrAffine2* PR_NULLABLE transforms);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
