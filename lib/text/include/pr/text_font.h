// Text fonts: the fonts that text is set in, and the face that sets each grapheme cluster.
//
// A text font is a family list and a style in a font set, at a size and for a locale. Its faces are those of the
// families that the list names, then those of the default family of the set, matched at the style, and then the
// fallback families of the set. It answers which face sets each grapheme cluster, and keeps the answer, so the same
// text in the same text font uses the same faces wherever it is set. An answer is an index into a table of the fonts
// that the text font has answered with. The table keeps its indices until a change of the set changes an answer
// (prTextFontUpdate).
//
// A text font is reference counted. One thread uses it at a time. Its references may be taken and released on any
// thread.

#pragma once

#include "pr/base.h"
#include "pr/font.h"
#include "pr/font_set.h"
#include "pr/locale.h"
#include "pr/raw_string.h"
#include "pr/string_view.h"
#include "pr/text.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// A family list and a style in a font set, and the faces that set its text.
typedef struct PrTextFont PrTextFont;

// The description of a text font.
typedef struct PrTextFontDesc {
    PrFontSet* set;                             // must outlive the text font
    const PrFontFamily* PR_NULLABLE families;   // the first is preferred; the default family of the set follows them
    uint32_t familyCount;
    PrFontStyle style;
    PrLocale locale;                            // the language of the text, which fallback reads; zero for none
} PrTextFontDesc;

// The match index that means no font, for a text font that has no face with a font.
static const uint32_t kPrTextMatchNone = 0xffffffffu;

// Creates a text font with one reference. The text font copies the description.
PrTextFont* prTextFontCreate(const PrTextFontDesc* desc);

// Adds or releases a reference. The last release frees the text font.
void prTextFontRetain(PrTextFont* font);
void prTextFontRelease(PrTextFont* PR_NULLABLE font);

// Returns the set, style and locale of the text font.
PrFontSet* prTextFontSet(const PrTextFont* font);
PrFontStyle prTextFontStyle(const PrTextFont* font);
PrLocale prTextFontLocale(const PrTextFont* font);

// Matches the grapheme cluster `cluster`, whose script is `script` as itemization resolved it, to the face that sets
// it, and returns the index of that face's font among the text font's matched fonts. The face is chosen as follows:
// - The first face that serves the whole cluster, from the families that the list names and then the default family.
//   Faces are tried in the order of the presentation of the cluster: faces flagged Emoji come first for a cluster with
//   emoji presentation, and last for any other cluster. A face whose font has not loaded serves nothing. It is
//   requested (prFontSetRequestLoad) if the search passes it and its ranges hold the first code point of the cluster.
// - Otherwise, the first face of the fallback families of the set that serves the cluster and whose font declares its
//   script, and then the first face that serves it at all. In each search, color faces come first for an emoji, and
//   then the families that suit the text font best: those made for its locale, the closest first, and then those drawn
//   in its generic family. That generic family is the first that its list names, or else that of the default family.
//   A face from the fallback families joins the faces of the text font. A cluster with a private-use code point or a
//   control character uses no fallback family.
// - Otherwise, the face that serves its first code point, or the first available face (prTextFontFirst).
//
// A cluster that holds a variation sequence first goes through the first two steps with only the faces whose character
// maps list the sequence (prFontVariationSequenceGlyph). If none does, it goes through them again with every face. So a
// face that lists the sequence is preferred to a face that has only its base character. The emoji presentation
// selectors do not form such sequences, because they choose between color and text faces.
//
// The choice is a function of the cluster, the script and the text font, and the text font remembers it. A font that
// has not matched before is appended to the matched fonts (prTextFontMatchedFont). Returns kPrTextMatchNone if the text
// font has no face with a font.
uint32_t prTextFontMatch(PrTextFont* font, PrStringView cluster, PrScript script);

// Returns the number of fonts that the text font has matched so far, and the matched font at `index`.
uint32_t prTextFontMatchCount(const PrTextFont* font);
const PrFont* prTextFontMatchedFont(const PrTextFont* font, uint32_t index);

// Returns the first available face, as a match index. This is the first face of its families whose font has loaded and
// whose ranges hold U+0020. The strut of a line and font-relative lengths read its metrics. Returns kPrTextMatchNone if
// there is no such face.
uint32_t prTextFontFirst(PrTextFont* font);

// Writes to `advance` the advance, in ems, of the glyph that the text font sets `codePoint` of `script` in
// (prTextFontMatch), along `axis`. Returns false if no face that the text font matches has a glyph for the code point.
bool prTextFontAdvance(PrTextFont* font, uint32_t codePoint, PrScript script, PrTextAxis axis, float* advance);

// Brings the text font up to date with its set. If a change of the set may have changed what its families or the
// fallback families match (prFontSetFamilyStamp), the text font matches them again and asks again every question that
// it has answered. If every answer is the same font, its table stays. Otherwise, it empties its table and raises its
// revision, which invalidates every index that it has given. Returns true if it raised the revision.
//
// The text font records a bounded number of questions. A question past the bound is answered but not recorded. The next
// update that matches again then raises the revision, because the text font cannot vouch for answers that it did not
// record.
//
// A caller that keeps answers calls this function only when no answer is in use. A caller that shares the text font
// with others compares the revision at which it read the table with prTextFontRevision, instead of relying on the
// result of its own call.
bool prTextFontUpdate(PrTextFont* font);

// Returns a number, starting from 1, that rises each time the answers of the text font stop being valid.
uint64_t prTextFontRevision(const PrTextFont* font);

// Returns true if the face serves every code point of the grapheme cluster `cluster` other than the default ignorable
// ones, which shaping consumes or draws as nothing. A variation selector is default ignorable, so a face with the base
// character of a variation sequence serves the sequence.
bool prTextFaceServesCluster(const PrFontFace* face, PrStringView cluster);

// Returns true if the grapheme cluster `cluster` has emoji presentation. It does if its first code point has emoji
// presentation by default or it has an emoji variation selector, and it has no text variation selector.
bool prTextClusterIsEmoji(PrStringView cluster);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
