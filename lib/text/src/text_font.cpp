// Text fonts.
//
// A text font chooses faces from a match order. The order holds the faces of its families, which the set matches when a
// cluster first needs them, and then the faces of the fallback families of the set that clusters need. Matching opens
// no file that the font of a face was not already opened from, and loads no pending face. A pending face is requested
// only if a cluster reaches it. The order is matched again if a change may have changed it (prFontSetFamilyStamp), so a
// question always reads the current state of the set.
//
// An answer is an index into a table of the fonts that the text font has answered with. The table keeps its indices
// while the answers hold. The text font records each question that it answered. When the order is matched again, the
// text font asks every recorded question again of the new order. If every answer is the same font, the table stays.
// Otherwise, the table is emptied and the revision raised.

#include "pr/text_font.h"

#include "pr/alloc.h"
#include "pr/atomic.h"
#include "pr/hash.h"
#include "pr/hash_index.h"
#include "pr/locale_common.h"
#include "pr/mem.h"
#include "pr/raw_array.h"
#include "pr/string_array.h"
#include "pr/unicode.h"
#include "pr/utf8.h"

namespace pr_impl {
namespace {

// The highest rank of a fallback face (FallbackFace::rank). The rank is twice the locale rank, which counts the
// language, script and region, plus one if the family is drawn in the generic family of the text font.
constexpr uint32_t kMaxFallbackRank = 3 * 2 + 1;

// The largest number of questions that a text font records, and the largest number of bytes in the recorded clusters of
// more than one code point. A question past either limit is answered but not recorded.
constexpr uint32_t kMaxRecorded = 16384;
constexpr uint32_t kMaxRecordedBytes = 65536;

// A face of a match order. Its ranges start at `rangeStart` in the ranges of the order.
struct OrderFace {
    PrFontFace face;                    // holds a reference that the order owns; its ranges are null
    uint32_t rangeStart;
};

// A face of a fallback family, matched at the style of the text font. It records the OpenType scripts that its font
// declares, how well its family suits the text font, and its place among the faces of the order once a cluster has
// needed it.
struct FallbackFace {
    OrderFace face;
    uint32_t scriptStart;               // the start of its script tags in Order::fallbackScripts
    uint32_t scriptCount;
    uint32_t rank;
    uint32_t index;                     // its index among the faces of the order, or kPrTextMatchNone
};

// The faces that a text font chooses among, as its set matched them.
struct Order {
    RawArray<PrFontFamily> families;       // the list's families, then the set's default family; names are in `names`
    RawArray<PrRawString> names;
    PrFontGeneric generic;              // the first generic family of the families

    // The faces matched so far: the faces of each family in turn, then those of the fallback families that clusters
    // needed.
    RawArray<OrderFace> faces;

    RawArray<PrFontCodePointRange> ranges; // the ranges of the faces and of the fallback faces
    uint32_t familiesMatched;              // the number of families whose faces are among `faces`
    uint32_t requested;                    // the number of faces of those families, which come first

    RawArray<FallbackFace> fallback;
    RawArray<uint32_t> fallbackScripts;
    bool fallbackMatched;
};

// A question about one code point that the text font answered, keyed by its code point and script, and the answer: an
// index into the table, or kPrTextMatchNone.
struct Choice {
    uint64_t key;
    uint32_t answer;
};

// A question about a cluster of more than one code point. Its bytes are in TextFont::clusterText.
struct ClusterChoice {
    uint32_t start;
    uint32_t size;
    PrScript script;
    uint32_t answer;
};

struct TextFont {
    Atomic<uint32_t> references;
    PrFontSet* set;
    PrFontStyle style;
    PrLocale locale;
    RawArray<PrFontFamily> listed;         // the families of the description; their names are copies in `listedNames`
    RawArray<PrRawString> listedNames;

    Order order;
    uint64_t version;                   // the version of the set when the text font last checked its order
    uint64_t stamp;                     // the latest change that the order may depend on, as of its match

    RawArray<const PrFont*> table;         // the fonts answered with, by answer; each holds a reference
    uint64_t revision;

    // The questions answered since the table was last emptied.
    KeyIndex choices;
    RawArray<Choice> chosen;
    HashIndex clusterIndex;
    RawArray<ClusterChoice> clusters;
    RawArray<char> clusterText;
    uint32_t firstAnswer;
    bool firstAsked;
    bool incomplete;                    // true if a question went unrecorded, so a replay cannot vouch for the table

    PrFontMatch* match;                 // scratch
};

// Returns how closely the locale `locale` suits a fallback family made for `madeFor`. Returns zero if they differ in a
// part that `madeFor` sets, or if `madeFor` is zero. Otherwise, returns how many of the language, script and region
// `madeFor` sets.
uint32_t localeRank(PrLocale madeFor, PrLocale locale) {
    PrLanguage language = prLocaleLanguage(madeFor);
    PrScript script = prLocaleScript(madeFor);
    PrRegion region = prLocaleRegion(madeFor);
    if ((language != PrLanguage_None && language != prLocaleLanguage(locale)) ||
            (script != PrScript_Zzzz && script != prLocaleScript(locale)) ||
            (region != PrRegion_None && region != prLocaleRegion(locale)))
        return 0;
    return (language != PrLanguage_None ? 1u : 0u) + (script != PrScript_Zzzz ? 1u : 0u) + (region != PrRegion_None ? 1u : 0u);
}

// Starts an order of the families of the text font, followed by the default family of the set as it is now.
void orderInit(const TextFont& t, Order& o) {
    PrFontFamily fallback = prFontSetDefaultFamily(t.set);
    o.generic = PrFontGeneric_None;
    for (uint32_t i = 0; i <= t.listed.count; ++i) {
        PrFontFamily family = i < t.listed.count ? t.listed.data[i] : fallback;
        o.names.push(prRawStringCopy(family.name));
        if (o.generic == PrFontGeneric_None)
            o.generic = family.generic;
    }
    for (uint32_t i = 0; i <= t.listed.count; ++i) {
        PrFontGeneric generic = i < t.listed.count ? t.listed.data[i].generic : fallback.generic;
        o.families.push(PrFontFamily{prRawStringView(&o.names.data[i]), generic});
    }
}

// Releases what the order holds.
void orderReset(Order& o) {
    for (uint32_t i = 0; i < o.faces.count; ++i)
        prFontRelease(o.faces.data[i].face.font);
    for (uint32_t i = 0; i < o.fallback.count; ++i)
        prFontRelease(o.fallback.data[i].face.face.font);
    stringsReset(&o.names);
    o.families.reset();
    o.faces.reset();
    o.ranges.reset();
    o.fallback.reset();
    o.fallbackScripts.reset();
    o = Order{};
}

// Returns the face of the order, with its ranges in the order.
PrFontFace faceAt(const Order& o, const OrderFace& face) {
    PrFontFace out = face.face;
    out.ranges = out.rangeCount != 0 ? o.ranges.data + face.rangeStart : nullptr;
    return out;
}

// Returns the face, with a reference to its font and a copy of its ranges in the order.
OrderFace keep(Order& o, const PrFontFace& face) {
    if (face.font != nullptr)
        prFontRetain(face.font);
    OrderFace kept{face, o.ranges.count};
    kept.face.ranges = nullptr;
    if (face.rangeCount != 0)
        o.ranges.append(face.ranges, face.rangeCount);
    return kept;
}

// Matches the faces of the next family of the order, and returns false if every family is matched.
bool matchNextFamily(TextFont& t, Order& o) {
    if (o.familiesMatched == o.families.count)
        return false;
    prFontMatchClear(t.match);
    prFontSetMatch(t.set, o.families.data[o.familiesMatched++], &t.style, t.match);
    for (uint32_t k = 0; k < prFontMatchFaceCount(t.match); ++k)
        o.faces.push(keep(o, prFontMatchFace(t.match, k)));
    o.requested = o.faces.count;
    prFontMatchClear(t.match);
    return true;
}

// Matches the faces of every family of the order that has not been matched.
void matchFamilies(TextFont& t, Order& o) {
    while (matchNextFamily(t, o)) {
    }
}

// Matches the fallback families of the set at the style of the text font, recording the scripts that each face declares
// and how well each family suits the text font.
void matchFallback(TextFont& t, Order& o) {
    o.fallbackMatched = true;
    uint32_t tags[64];
    for (uint32_t i = 0; i < prFontSetFallbackCount(t.set); ++i) {
        PrFontFallbackFamily family = prFontSetFallbackFamily(t.set, i);
        bool drawnIn = family.generic != PrFontGeneric_None && family.generic == o.generic;
        uint32_t rank = localeRank(family.locale, t.locale) * 2 + (drawnIn ? 1 : 0);
        prFontMatchClear(t.match);
        prFontSetMatchFallback(t.set, i, &t.style, t.match);
        for (uint32_t k = 0; k < prFontMatchFaceCount(t.match); ++k) {
            PrFontFace matched = prFontMatchFace(t.match, k);
            if (matched.font == nullptr)
                continue;
            FallbackFace face{
                .face = keep(o, matched),
                .scriptStart = o.fallbackScripts.count,
                .rank = rank,
                .index = kPrTextMatchNone,
            };
            uint32_t scripts = prFontScriptTags(matched.font, tags, 64);
            o.fallbackScripts.append(tags, scripts < 64 ? scripts : 64);
            face.scriptCount = o.fallbackScripts.count - face.scriptStart;
            o.fallback.push(face);
        }
    }
    prFontMatchClear(t.match);
}

// Returns true if the fallback face declares the script `script`, or if `script` is not a script of its own (Zzzz,
// Common or Inherited).
bool declares(const Order& o, const FallbackFace& face, PrScript script) {
    if (script == PrScript_Zzzz || script == PrScript_Zyyy || script == PrScript_Zinh)
        return true;
    uint32_t tags[kPrScriptMaxOpenTypeTags];
    uint32_t count = prScriptOpenTypeTags(script, tags);
    for (uint32_t i = 0; i < face.scriptCount; ++i) {
        for (uint32_t k = 0; k < count; ++k) {
            if (o.fallbackScripts.data[face.scriptStart + i] == tags[k])
                return true;
        }
    }
    return false;
}

// Returns true if the cluster holds a code point that falls back to no installed family. That is a private-use code
// point, which only a face that a list names may serve, or a control character, whose visible glyph is the missing
// glyph of the first available face, a visible mark of the character.
bool fallsBackToNothing(PrStringView cluster) {
    for (size_t at = 0; at < cluster.size;) {
        uint32_t codePoint = 0;
        at += prUtf8Decode(cluster, at, &codePoint);
        bool privateUse = (codePoint >= 0xE000 && codePoint <= 0xF8FF) || codePoint >= 0xF0000;
        bool control = codePoint < 0x20 || (codePoint >= 0x7F && codePoint <= 0x9F);
        if (privateUse || control)
            return true;
    }
    return false;
}

// Returns true for a code point that a face does not need a glyph for to serve a cluster. These are the default
// ignorable code points -- among them joiners, variation selectors, zero-width spaces and directional marks -- which
// shaping either consumes or shows nothing of. A cluster of only these code points stays in the first face.
bool isIgnorable(uint32_t codePoint) {
    return (prUnicodeFlags(codePoint) & PrUnicodeFlag_DefaultIgnorable) != 0;
}

// Returns the first requested face that serves the cluster, or kPrTextMatchNone. Faces that match the presentation of
// the cluster come first, then the others, each group in the order of the list. A face whose font has not loaded and
// that the search passes is requested if its ranges hold the first code point of the cluster.
uint32_t requestedFace(TextFont& t, Order& o, PrStringView cluster, uint32_t first, bool emoji) {
    matchFamilies(t, o);
    for (uint32_t pass = 0; pass < 2; ++pass) {
        for (uint32_t i = 0; i < o.requested; ++i) {
            PrFontFace face = faceAt(o, o.faces.data[i]);
            if (face.font == nullptr) {
                if (pass == 0 && face.pending != 0 && prFontFaceInRanges(&face, first))
                    prFontSetRequestLoad(t.set, face.pending);
                continue;
            }
            bool emojiFace = (face.flags & PrFontFaceFlag_Emoji) != 0;
            if ((emojiFace == emoji) == (pass == 0) && prTextFaceServesCluster(&face, cluster))
                return i;
        }
    }
    return kPrTextMatchNone;
}

// Returns the fallback face that serves the cluster, after joining it to the faces of the order, or kPrTextMatchNone.
uint32_t fallbackFace(TextFont& t, Order& o, PrStringView cluster, PrScript script, bool emoji) {
    if (fallsBackToNothing(cluster))
        return kPrTextMatchNone;
    if (!o.fallbackMatched)
        matchFallback(t, o);
    // The first face that declares the script, or else the first that serves the cluster. In each pass, color faces
    // come first for an emoji, then the families by how well they suit the text font, then in the order of the set.
    uint32_t found = kPrTextMatchNone;
    for (uint32_t pass = 0; pass < 2 && found == kPrTextMatchNone; ++pass) {
        for (uint32_t colors = 0; colors < (emoji ? 2u : 1u) && found == kPrTextMatchNone; ++colors) {
            for (uint32_t rank = kMaxFallbackRank + 1; rank-- > 0 && found == kPrTextMatchNone;) {
                for (uint32_t i = 0; i < o.fallback.count; ++i) {
                    const FallbackFace& candidate = o.fallback.data[i];
                    PrFontFace face = faceAt(o, candidate.face);
                    bool color = (face.flags & PrFontFaceFlag_Emoji) != 0;
                    if ((emoji && color != (colors == 0)) || candidate.rank != rank)
                        continue;
                    if ((pass == 0 && !declares(o, candidate, script)) || !prTextFaceServesCluster(&face, cluster))
                        continue;
                    found = i;
                    break;
                }
            }
        }
    }
    if (found == kPrTextMatchNone)
        return kPrTextMatchNone;
    if (o.fallback.data[found].index == kPrTextMatchNone) {
        matchFamilies(t, o);
        FallbackFace& candidate = o.fallback.data[found];
        candidate.index = o.faces.count;
        OrderFace joined = candidate.face;
        prFontRetain(joined.face.font);
        o.faces.push(joined);
    }
    return o.fallback.data[found].index;
}

// Returns the first available face of the order: the first face of its families whose font has loaded and whose ranges
// hold U+0020, or kPrTextMatchNone. Requests each pending face that it passes whose ranges hold U+0020.
uint32_t firstFace(TextFont& t, Order& o) {
    matchFamilies(t, o);
    for (uint32_t i = 0; i < o.requested; ++i) {
        PrFontFace face = faceAt(o, o.faces.data[i]);
        if (face.font != nullptr && prFontFaceInRanges(&face, ' '))
            return i;
        if (face.font == nullptr && face.pending != 0 && prFontFaceInRanges(&face, ' '))
            prFontSetRequestLoad(t.set, face.pending);
    }
    return kPrTextMatchNone;
}

// Returns the face of the order that sets the cluster, or kPrTextMatchNone (prTextFontMatch).
uint32_t chooseFace(TextFont& t, Order& o, PrStringView cluster, PrScript script) {
    uint32_t first = 0;
    prUtf8Decode(cluster, 0, &first);
    bool emoji = prTextClusterIsEmoji(cluster);
    uint32_t face = requestedFace(t, o, cluster, first, emoji);
    if (face == kPrTextMatchNone)
        face = fallbackFace(t, o, cluster, script, emoji);
    if (face == kPrTextMatchNone) {
        // No face serves the whole cluster: use the face that serves its first code point, or the first available face.
        for (uint32_t i = 0; i < o.requested && face == kPrTextMatchNone; ++i) {
            PrFontFace at = faceAt(o, o.faces.data[i]);
            if (prFontFaceServes(&at, first))
                face = i;
        }
        if (face == kPrTextMatchNone)
            face = firstFace(t, o);
    }
    return face;
}

// Returns the font of the face of the order at `at`, or null for kPrTextMatchNone.
const PrFont* PR_NULLABLE fontAt(const Order& o, uint32_t at) {
    return at != kPrTextMatchNone ? o.faces.data[at].face.font : nullptr;
}

// Returns the index in the table of the font of the face of the order at `at`, adding the font to the table if it is
// new. Returns kPrTextMatchNone for kPrTextMatchNone.
uint32_t answerOf(TextFont& t, uint32_t at) {
    const PrFont* font = fontAt(t.order, at);
    if (font == nullptr)
        return kPrTextMatchNone;
    for (uint32_t i = 0; i < t.table.count; ++i) {
        if (t.table.data[i] == font)
            return i;
    }
    prFontRetain(font);
    t.table.push(font);
    return t.table.count - 1;
}

// Returns the font of the answer in the table, or null for kPrTextMatchNone.
const PrFont* PR_NULLABLE tableFont(const TextFont& t, uint32_t answer) {
    return answer != kPrTextMatchNone ? t.table.data[answer] : nullptr;
}

uint64_t keyOf(uint32_t codePoint, PrScript script) {
    return uint64_t(codePoint) | uint64_t(script) << 21;
}

uint32_t clusterHash(PrStringView cluster, PrScript script) {
    return uint32_t(prHashBytes(cluster.data, cluster.size, 0x51ed270b27353d0dull + uint64_t(script)));
}

// Returns the recorded question for a cluster of more than one code point, or kHashMissing.
uint32_t findCluster(const TextFont& t, PrStringView cluster, PrScript script) {
    return t.clusterIndex.find(clusterHash(cluster, script), [&](uint32_t i) {
        const ClusterChoice& c = t.clusters.data[i];
        return c.script == script && c.size == cluster.size &&
            prMemEquals(t.clusterText.data + c.start, cluster.data, cluster.size);
    });
}

// Returns true if one more question, of `bytes` more bytes, fits the record. Otherwise, marks the record incomplete.
bool recordFits(TextFont& t, size_t bytes) {
    bool fits = t.chosen.count + t.clusters.count < kMaxRecorded && t.clusterText.count + bytes <= kMaxRecordedBytes;
    t.incomplete = t.incomplete || !fits;
    return fits;
}

// Releases the fonts of the table and forgets every question, because the answers change.
void forgetAnswers(TextFont& t) {
    for (uint32_t i = 0; i < t.table.count; ++i)
        prFontRelease(t.table.data[i]);
    t.table.clear();
    t.choices.clear();
    t.chosen.clear();
    t.clusterIndex.clear();
    t.clusters.clear();
    t.clusterText.clear();
    t.firstAsked = false;
    t.incomplete = false;
}

// Returns the latest change of the set that may change what the order matched: a change of its families, through their
// bindings, or of the fallback families.
uint64_t dependencyStamp(const TextFont& t) {
    uint64_t stamp = prFontSetFallbackStamp(t.set);
    for (uint32_t i = 0; i < t.order.families.count; ++i) {
        uint64_t family = prFontSetFamilyStamp(t.set, t.order.families.data[i]);
        stamp = family > stamp ? family : stamp;
    }
    return stamp;
}

// Returns true if `fresh` gives every recorded question the same answer as the table.
bool replays(TextFont& t, Order& fresh) {
    if (t.incomplete)
        return false;
    for (uint32_t i = 0; i < t.chosen.count; ++i) {
        const Choice& choice = t.chosen.data[i];
        char bytes[kPrUtf8MaxBytes];
        uint32_t size = prUtf8Encode(uint32_t(choice.key & 0x1fffff), bytes);
        uint32_t at = chooseFace(t, fresh, PrStringView{bytes, size}, PrScript(choice.key >> 21));
        if (fontAt(fresh, at) != tableFont(t, choice.answer))
            return false;
    }
    for (uint32_t i = 0; i < t.clusters.count; ++i) {
        const ClusterChoice& choice = t.clusters.data[i];
        PrStringView cluster{t.clusterText.data + choice.start, choice.size};
        if (fontAt(fresh, chooseFace(t, fresh, cluster, choice.script)) != tableFont(t, choice.answer))
            return false;
    }
    return !t.firstAsked || fontAt(fresh, firstFace(t, fresh)) == tableFont(t, t.firstAnswer);
}

} // namespace
} // namespace pr_impl

using namespace pr_impl;

// C opaque types
struct PrTextFont : TextFont {};

PrTextFont* prTextFontCreate(const PrTextFontDesc* desc) {
    PrTextFont* t = allocZeroed<PrTextFont>();
    t->references.storeRelaxed(1);
    t->set = desc->set;
    t->style = desc->style;
    t->locale = desc->locale;
    t->revision = 1;
    t->match = prFontMatchCreate();
    for (uint32_t i = 0; i < desc->familyCount; ++i)
        t->listedNames.push(prRawStringCopy(desc->families[i].name));
    for (uint32_t i = 0; i < desc->familyCount; ++i)
        t->listed.push(PrFontFamily{prRawStringView(&t->listedNames.data[i]), desc->families[i].generic});
    orderInit(*t, t->order);
    t->version = prFontSetVersion(desc->set);
    t->stamp = dependencyStamp(*t);
    return t;
}

void prTextFontRetain(PrTextFont* font) {
    font->references.fetchAdd(1);
}

void prTextFontRelease(PrTextFont* PR_NULLABLE font) {
    if (font == nullptr || font->references.fetchSub(1) != 1)
        return;
    forgetAnswers(*font);
    orderReset(font->order);
    stringsReset(&font->listedNames);
    font->listed.reset();
    font->table.reset();
    font->choices.reset();
    font->chosen.reset();
    font->clusterIndex.reset();
    font->clusters.reset();
    font->clusterText.reset();
    prFontMatchDestroy(font->match);
    prMemoryFree(font);
}

PrFontSet* prTextFontSet(const PrTextFont* font) {
    return font->set;
}

PrFontStyle prTextFontStyle(const PrTextFont* font) {
    return font->style;
}

PrLocale prTextFontLocale(const PrTextFont* font) {
    return font->locale;
}

uint32_t prTextFontMatch(PrTextFont* font, PrStringView cluster, PrScript script) {
    uint32_t first = 0;
    size_t firstSize = prUtf8Decode(cluster, 0, &first);
    if (firstSize == cluster.size) {
        uint64_t key = keyOf(first, script);
        uint32_t at = font->choices.find(key, [&](uint32_t i) { return font->chosen.data[i].key; });
        if (at != kHashMissing)
            return font->chosen.data[at].answer;
        uint32_t answer = answerOf(*font, chooseFace(*font, font->order, cluster, script));
        if (recordFits(*font, 0)) {
            font->choices.insert(key, font->chosen.count);
            font->chosen.push(Choice{key, answer});
        }
        return answer;
    }
    uint32_t at = findCluster(*font, cluster, script);
    if (at != kHashMissing)
        return font->clusters.data[at].answer;
    uint32_t answer = answerOf(*font, chooseFace(*font, font->order, cluster, script));
    if (recordFits(*font, cluster.size)) {
        font->clusterIndex.insert(clusterHash(cluster, script), font->clusters.count);
        font->clusters.push(ClusterChoice{font->clusterText.count, uint32_t(cluster.size), script, answer});
        font->clusterText.append(cluster.data, uint32_t(cluster.size));
    }
    return answer;
}

uint32_t prTextFontMatchCount(const PrTextFont* font) {
    return font->table.count;
}

const PrFont* prTextFontMatchedFont(const PrTextFont* font, uint32_t index) {
    PR_ASSERT(index < font->table.count);
    return font->table.data[index];
}

uint32_t prTextFontFirst(PrTextFont* font) {
    if (!font->firstAsked) {
        font->firstAnswer = answerOf(*font, firstFace(*font, font->order));
        font->firstAsked = true;
    }
    return font->firstAnswer;
}

bool prTextFontAdvance(PrTextFont* font, uint32_t codePoint, PrScript script, PrTextAxis axis, float* advance) {
    char bytes[4];
    size_t size = prUtf8Encode(codePoint, bytes);
    uint32_t match = prTextFontMatch(font, PrStringView{bytes, size}, script);
    if (match == kPrTextMatchNone)
        return false;
    const PrFont* face = prTextFontMatchedFont(font, match);
    uint16_t glyph = prFontCodePointGlyph(face, codePoint);
    if (glyph == 0)
        return false;
    *advance = axis == PrTextAxis_Vertical ? prFontGlyphVerticalAdvance(face, glyph) : prFontGlyphAdvance(face, glyph, 0);
    return true;
}

bool prTextFontUpdate(PrTextFont* font) {
    uint64_t version = prFontSetVersion(font->set);
    if (version == font->version)
        return false;
    font->version = version;
    uint64_t stamp = dependencyStamp(*font);
    if (stamp <= font->stamp)
        return false;
    font->stamp = stamp;
    // Matches the order afresh, and asks it again every question answered so far. If the default family changed, the
    // fresh order uses the new one.
    Order fresh{};
    orderInit(*font, fresh);
    bool same = replays(*font, fresh);
    orderReset(font->order);
    font->order = fresh;
    if (same)
        return false;
    forgetAnswers(*font);
    ++font->revision;
    return true;
}

uint64_t prTextFontRevision(const PrTextFont* font) {
    return font->revision;
}

bool prTextFaceServesCluster(const PrFontFace* face, PrStringView cluster) {
    for (size_t at = 0; at < cluster.size;) {
        uint32_t codePoint = 0;
        at += prUtf8Decode(cluster, at, &codePoint);
        if (!isIgnorable(codePoint) && !prFontFaceServes(face, codePoint))
            return false;
    }
    return true;
}

bool prTextClusterIsEmoji(PrStringView cluster) {
    uint32_t first = 0;
    prUtf8Decode(cluster, 0, &first);
    bool emoji = (prUnicodeFlags(first) & PrUnicodeFlag_EmojiPresentation) != 0;
    for (size_t at = 0; at < cluster.size;) {
        uint32_t codePoint = 0;
        at += prUtf8Decode(cluster, at, &codePoint);
        if (codePoint == 0xfe0f)
            emoji = true;
        else if (codePoint == 0xfe0e)
            emoji = false;
    }
    return emoji;
}
