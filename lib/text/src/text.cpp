// Text shaping: the interface every shaper implements, and splitting text by the faces that set it.
//
// Work that does not depend on a shaper happens here, around the call to the backend. The policies of the run become
// features. Synthesis shapes the text in pieces and scales them. The result gets the size-adjust of the font, clusters
// that never decrease, and marks at the starts of grapheme clusters. Whether a font provides a feature is determined by
// shaping with and without it through the backend.

#include "pr/text.h"
#include "text/src/text.h"

#include "pr/alloc.h"
#include "pr/ascii.h"
#include "pr/assert.h"
#include "pr/atomic.h"
#include "pr/float.h"
#include "pr/locale_common.h"
#include "pr/raw_array.h"
#include "pr/sort.h"
#include "pr/text_backend.h"
#include "pr/unicode.h"
#include "pr/unicode_grapheme.h"
#include "pr/utf8.h"
#include "pr/vec.h"

#if defined(PR_TEXT_HAS_HARFBUZZ)
#include "pr/text_harfbuzz.h"
#endif
#if defined(PR_TEXT_HAS_KB)
#include "pr/text_kb.h"
#endif
#if defined(PR_TEXT_HAS_CORETEXT)
#include "pr/text_coretext.h"
#endif
#if defined(PR_TEXT_HAS_SIMPLE)
#include "pr/text_simple.h"
#endif

namespace pr_impl {

namespace {

constexpr uint32_t kWholeRun = 0xffffffffu;

constexpr PrStringView kBackendNames[PrTextShaperBackend_Count] = {"default", "harfbuzz", "kb", "coretext", "simple"};

// The backends that the build has, in the order of preference, which Default creates the first of, and then Default, so
// that a build without backends creates none.
constexpr PrTextShaperBackend kBuilt[] = {
#if defined(PR_TEXT_HAS_HARFBUZZ)
    PrTextShaperBackend_HarfBuzz,
#endif
#if defined(PR_TEXT_HAS_KB)
    PrTextShaperBackend_Kb,
#endif
#if defined(PR_TEXT_HAS_CORETEXT)
    PrTextShaperBackend_CoreText,
#endif
#if defined(PR_TEXT_HAS_SIMPLE)
    PrTextShaperBackend_Simple,
#endif
    PrTextShaperBackend_Default,
};

constexpr uint32_t tagOf(char a, char b, char c, char d) {
    return uint32_t(uint8_t(a)) << 24 | uint32_t(uint8_t(b)) << 16 | uint32_t(uint8_t(c)) << 8 | uint32_t(uint8_t(d));
}

PrTextGlyphBuffer& scratchOf(PrTextGlyphBuffer& g) {
    if (g.scratch == nullptr)
        g.scratch = allocZeroed<PrTextGlyphBuffer>();
    return *g.scratch;
}

bool isUtf8(PrStringView text) {
    return text.size == 0 || prUtf8Validate(text) == text.size;
}

bool isSpace(uint32_t codePoint) {
    return codePoint <= 0x20 || codePoint == 0xa0 || codePoint == 0x3000 || (codePoint >= 0x2000 && codePoint <= 0x200b);
}

// Writes to `out` the OpenType features that `caps` requests, and returns their number.
uint32_t capsFeatures(PrTextCaps caps, uint32_t out[2]) {
    switch (caps) {
        case PrTextCaps_Small:
            out[0] = tagOf('s', 'm', 'c', 'p');
            return 1;
        case PrTextCaps_AllSmall:
            out[0] = tagOf('c', '2', 's', 'c');
            out[1] = tagOf('s', 'm', 'c', 'p');
            return 2;
        case PrTextCaps_Petite:
            out[0] = tagOf('p', 'c', 'a', 'p');
            return 1;
        case PrTextCaps_AllPetite:
            out[0] = tagOf('c', '2', 'p', 'c');
            out[1] = tagOf('p', 'c', 'a', 'p');
            return 2;
        case PrTextCaps_Unicase:
            out[0] = tagOf('u', 'n', 'i', 'c');
            return 1;
        case PrTextCaps_Titling:
            out[0] = tagOf('t', 'i', 't', 'l');
            return 1;
        default:
            return 0;
    }
}

uint32_t positionFeature(PrTextPosition position) {
    if (position == PrTextPosition_Subscript)
        return tagOf('s', 'u', 'b', 's');
    if (position == PrTextPosition_Superscript)
        return tagOf('s', 'u', 'p', 's');
    return 0;
}

// Builds the features that a backend shapes the run with: those of the font, of the kerning policy, of the caps and of
// the position, unless those are synthesized, and then the run's own features.
void mergeFeatures(RawArray<PrTextFeature>& out, const PrTextRun& run, PrTextCaps caps, bool positionFeatures) {
    out.clear();
    for (uint32_t i = 0; i < prFontFeatureCount(run.font); ++i) {
        const PrFontFeature& f = prFontFeatures(run.font)[i];
        out.push(PrTextFeature{f.tag, f.value, 0, kWholeRun});
    }
    if (run.kerning != PrTextKerning_Auto) {
        uint32_t value = run.kerning == PrTextKerning_Normal ? 1 : 0;
        out.push(PrTextFeature{tagOf('k', 'e', 'r', 'n'), value, 0, kWholeRun});
        if (run.axis == PrTextAxis_Vertical)
            out.push(PrTextFeature{tagOf('v', 'k', 'r', 'n'), value, 0, kWholeRun});
    }
    uint32_t tags[2];
    for (uint32_t i = 0; i < capsFeatures(caps, tags); ++i)
        out.push(PrTextFeature{tags[i], 1, 0, kWholeRun});
    if (positionFeatures && positionFeature(run.position) != 0)
        out.push(PrTextFeature{positionFeature(run.position), 1, 0, kWholeRun});
    // Spaced letters take no optional ligatures, which would join them.
    if (run.letterSpacing != 0) {
        static const uint32_t kOptional[] = {
            tagOf('l', 'i', 'g', 'a'), tagOf('c', 'l', 'i', 'g'), tagOf('d', 'l', 'i', 'g'), tagOf('h', 'l', 'i', 'g'),
        };
        for (uint32_t tag : kOptional)
            out.push(PrTextFeature{tag, 0, 0, kWholeRun});
    }
    if (run.featureCount != 0)
        out.append(run.features, run.featureCount);
}

// Writes to `out`, for each of the `count` glyphs that a backend shaped from `text` in visual order, the spacing after
// it. The text divides into segments at each grapheme cluster boundary at which a cluster of glyphs starts, so that a
// mark that a backend gives a cluster of its own stays with its base, and a cluster that joins several grapheme
// clusters, as a ligature or an Indic conjunct does, is not split. Each segment takes the run's letter spacing, unless
// it starts with a letter that joins its neighbors, as in a cursive script, or with a default ignorable code point, and
// the word spacing for each word separator that starts one of its grapheme clusters. The spacing goes on the segment's
// last glyph in the order of the text.
void spacingOf(const PrTextRun& run, PrStringView text, const PrTextGlyph* glyphs, uint32_t count,
        RawArray<uint64_t>& order, float* out) {
    for (uint32_t i = 0; i < count; ++i)
        out[i] = 0;
    if (run.letterSpacing == 0 && run.wordSpacing == 0)
        return;
    // The glyphs by cluster, and by visual order within a cluster.
    order.resize(count);
    for (uint32_t i = 0; i < count; ++i)
        order.data[i] = uint64_t(glyphs[i].cluster) << 32 | i;
    sortItems(order.span(), [](uint64_t a, uint64_t b) { return a < b; });
    auto clusterAt = [&](uint32_t k) { return uint32_t(order.data[k] >> 32); };
    bool rightToLeft = run.direction == PrTextDirection_Rtl;
    uint32_t at = 0;
    uint32_t last = UINT32_MAX;
    size_t start = 0;
    float words = 0;
    for (size_t grapheme = 0; grapheme < text.size;) {
        uint32_t codePoint = 0;
        prUtf8Decode(text, grapheme, &codePoint);
        if (prTextIsWordSeparator(codePoint))
            words += run.wordSpacing;
        size_t end = prUnicodeGraphemeNext(text, grapheme);
        while (at < count && clusterAt(at) < end) {
            uint32_t past = at + 1;
            while (past < count && clusterAt(past) == clusterAt(at))
                ++past;
            // The last glyph of a cluster in the order of the text is its last placed, or its first right to left. A
            // default ignorable code point that draws nothing has its glyph removed, so it takes no spacing.
            uint32_t glyph = uint32_t(order.data[rightToLeft ? at : past - 1]);
            uint32_t drawn = 0;
            prUtf8Decode(text, clusterAt(at), &drawn);
            bool removed = (prUnicodeFlags(drawn) & PrUnicodeFlag_DefaultIgnorable) != 0 &&
                glyphs[glyph].advance.x == 0 && glyphs[glyph].advance.y == 0;
            if (!removed)
                last = glyph;
            at = past;
        }
        grapheme = end;
        if (end < text.size && (at == count || clusterAt(at) != end))
            continue;
        if (last != UINT32_MAX) {
            prUtf8Decode(text, start, &codePoint);
            PrUnicodeJoiningType joining = prUnicodeJoiningType(codePoint);
            bool joins = joining != PrUnicodeJoiningType_NonJoining && joining != PrUnicodeJoiningType_Transparent;
            bool ignorable = (prUnicodeFlags(codePoint) & PrUnicodeFlag_DefaultIgnorable) != 0;
            out[last] += words + (joins || ignorable ? 0 : run.letterSpacing);
        }
        start = end;
        words = 0;
    }
}

// Shapes the run as given, into `out`.
PrStatus shapeWith(PrTextShaper* shaper, const PrTextRun& run, PrTextGlyphBuffer& out) {
    out.glyphs.clear();
    return shaper->shapeFn(shaper, &run, &out);
}

// Returns true if the font provides the feature for the text of the run. It shapes with the feature and without it. If
// `every` is set, the glyphs of every character except spaces must differ. Otherwise, the glyphs of any one character
// must differ.
bool fontGives(PrTextShaper* shaper, const PrTextRun& run, uint32_t tag, bool every, PrTextGlyphBuffer& g) {
    PrTextGlyphBuffer& s = scratchOf(g);
    PrTextFeature on{tag, 1, 0, kWholeRun};
    PrTextRun probe = run;
    probe.features = &on;
    probe.featureCount = 1;
    if (shapeWith(shaper, probe, s) != PrStatus_Ok)
        return false;
    g.probe.clear();
    for (uint32_t i = 0; i < s.glyphs.count; ++i)
        g.probe.push(s.glyphs.data[i].id);
    on.value = 0;
    if (shapeWith(shaper, probe, s) != PrStatus_Ok)
        return false;
    if (s.glyphs.count != g.probe.count)
        return true;
    bool any = false;
    bool all = true;
    for (uint32_t i = 0; i < s.glyphs.count; ++i) {
        uint32_t codePoint = 0;
        prUtf8Decode(run.text, s.glyphs.data[i].cluster, &codePoint);
        if (isSpace(codePoint))
            continue;
        bool differs = s.glyphs.data[i].id != g.probe.data[i];
        any = any || differs;
        all = all && differs;
    }
    return every ? all && any : any;
}

// How a character is drawn if small capitals are synthesized: unchanged or as its uppercase, and at the size of the run
// or scaled.
struct CaseForm {
    bool upper;
    bool scaled;
};

CaseForm caseFormOf(PrTextCaps caps, uint32_t codePoint) {
    PrUnicodeFlags flags = prUnicodeFlags(codePoint);
    bool lower = (flags & PrUnicodeFlag_Lowercase) != 0;
    bool upper = (flags & PrUnicodeFlag_Uppercase) != 0;
    switch (caps) {
        case PrTextCaps_Small:
        case PrTextCaps_Petite:
            return CaseForm{lower, lower};
        case PrTextCaps_AllSmall:
        case PrTextCaps_AllPetite:
            return CaseForm{lower, lower || upper};
        case PrTextCaps_Unicase:
            return CaseForm{false, upper};
        default:
            return CaseForm{false, false};
    }
}

// Shapes the bytes [start, end) of the text of the run, uppercased if requested, and appends the glyphs to `out` scaled
// by `scale`, with clusters in terms of the text of the run, and the spacing after each glyph to `spacing`.
PrStatus shapePiece(PrTextShaper* shaper, const PrTextRun& run, uint32_t start, uint32_t end,
        bool upper, float scale, PrTextGlyphBuffer& g, RawArray<PrTextGlyph>& out, RawArray<float>& spacing) {
    g.piece.clear();
    g.origin.clear();
    for (size_t at = start; at < end;) {
        uint32_t codePoint = 0;
        uint32_t length = prUtf8Decode(run.text, at, &codePoint);
        uint32_t mapped[3] = {codePoint, 0, 0};
        uint32_t count = upper ? prUnicodeUppercase(codePoint, mapped) : 1;
        for (uint32_t i = 0; i < count; ++i) {
            char bytes[kPrUtf8MaxBytes];
            uint32_t written = prUtf8Encode(mapped[i], bytes);
            for (uint32_t k = 0; k < written; ++k) {
                g.piece.push(bytes[k]);
                g.origin.push(uint32_t(at));
            }
        }
        at += length;
    }
    PrTextRun piece = run;
    piece.text = PrStringView{g.piece.data, g.piece.count};
    piece.before = start == 0 ? run.before : PrStringView{run.text.data, start};
    piece.after = end == run.text.size ? run.after : PrStringView{run.text.data + end, run.text.size - end};
    // Converts features over the bytes of the run to features over the bytes of the piece.
    g.pieceFeatures.clear();
    for (uint32_t i = 0; i < run.featureCount; ++i) {
        PrTextFeature f = run.features[i];
        if (f.end <= start || f.start >= end)
            continue;
        uint32_t from = 0;
        while (from < g.origin.count && g.origin.data[from] < f.start)
            ++from;
        uint32_t to = from;
        while (to < g.origin.count && g.origin.data[to] < f.end)
            ++to;
        g.pieceFeatures.push(PrTextFeature{f.tag, f.value, from, f.end >= end ? kWholeRun : to});
    }
    piece.features = g.pieceFeatures.data;
    piece.featureCount = g.pieceFeatures.count;
    PrTextGlyphBuffer& s = scratchOf(g);
    PrStatus status = shapeWith(shaper, piece, s);
    if (status != PrStatus_Ok)
        return status;
    // The spacing follows the characters as shaped, so a character that maps to several capitals is spaced as several.
    uint32_t first = spacing.count;
    spacing.resize(first + s.glyphs.count);
    spacingOf(piece, piece.text, s.glyphs.data, s.glyphs.count, s.order, spacing.data + first);
    for (uint32_t i = 0; i < s.glyphs.count; ++i) {
        PrTextGlyph glyph = s.glyphs.data[i];
        glyph.cluster = glyph.cluster < g.origin.count ? g.origin.data[glyph.cluster] : start;
        glyph.offset = glyph.offset * scale;
        glyph.advance = glyph.advance * scale;
        glyph.scale = scale;
        out.push(glyph);
    }
    return PrStatus_Ok;
}

// Shapes the run in pieces of characters that draw alike, with small capitals synthesized, in visual order.
PrStatus shapeSynthesizedCaps(PrTextShaper* shaper, const PrTextRun& run, float scale, PrTextGlyphBuffer& g) {
    RawArray<PrTextGlyph> pieces;
    RawArray<float> spacing;                   // the spacing after each glyph of `pieces`
    RawArray<uint32_t> firsts;                 // the start of the glyphs of each piece in `pieces`
    PrStatus status = PrStatus_Ok;
    size_t at = 0;
    while (at < run.text.size && status == PrStatus_Ok) {
        uint32_t codePoint = 0;
        prUtf8Decode(run.text, at, &codePoint);
        CaseForm form = caseFormOf(run.caps, codePoint);
        size_t end = at;
        while (end < run.text.size) {
            uint32_t next = 0;
            uint32_t length = prUtf8Decode(run.text, end, &next);
            CaseForm nextForm = caseFormOf(run.caps, next);
            if (nextForm.upper != form.upper || nextForm.scaled != form.scaled)
                break;
            end += length;
        }
        firsts.push(pieces.count);
        status = shapePiece(shaper, run, uint32_t(at), uint32_t(end), form.upper, form.scaled ? scale : 1, g, pieces, spacing);
        at = end;
    }
    firsts.push(pieces.count);
    g.glyphs.clear();
    g.spacing.clear();
    if (status == PrStatus_Ok) {
        // Each piece is in visual order, and the pieces of a right-to-left run go right to left.
        bool reversed = run.direction == PrTextDirection_Rtl;
        uint32_t count = firsts.count - 1;
        for (uint32_t k = 0; k < count; ++k) {
            uint32_t p = reversed ? count - 1 - k : k;
            g.glyphs.append(pieces.data + firsts.data[p], firsts.data[p + 1] - firsts.data[p]);
            g.spacing.append(spacing.data + firsts.data[p], firsts.data[p + 1] - firsts.data[p]);
        }
    }
    pieces.reset();
    spacing.reset();
    firsts.reset();
    return status;
}

// Merges clusters until they never decrease in the order of the text. If the cluster of a glyph is below the cluster of
// an earlier glyph, the glyphs back to that earlier glyph take the cluster of the later glyph.
void mergeClusters(PrTextGlyph* glyphs, uint32_t count, bool rightToLeft) {
    for (uint32_t k = 1; k < count; ++k) {
        uint32_t i = rightToLeft ? count - 1 - k : k;
        uint32_t cluster = glyphs[i].cluster;
        for (uint32_t j = k; j > 0; --j) {
            PrTextGlyph& before = glyphs[rightToLeft ? count - j : j - 1];
            if (before.cluster <= cluster)
                break;
            before.cluster = cluster;
        }
    }
}

// Marks the first glyph, in the order of the text, of each cluster that starts a grapheme cluster.
void markGraphemes(PrTextGlyphBuffer& g, PrStringView text, bool rightToLeft) {
    g.boundaries.clear();
    for (size_t at = 0; at < text.size; at = prUnicodeGraphemeNext(text, at))
        g.boundaries.push(uint32_t(at));
    uint32_t count = g.glyphs.count;
    uint32_t next = 0;
    uint32_t previous = kWholeRun;
    for (uint32_t k = 0; k < count; ++k) {
        PrTextGlyph& glyph = g.glyphs.data[rightToLeft ? count - 1 - k : k];
        glyph.flags &= PrTextGlyphFlags(~PrTextGlyphFlag_ClusterStart);
        if (glyph.cluster == previous)
            continue;
        previous = glyph.cluster;
        while (next < g.boundaries.count && g.boundaries.data[next] < glyph.cluster)
            ++next;
        if (next < g.boundaries.count && g.boundaries.data[next] == glyph.cluster)
            glyph.flags |= PrTextGlyphFlag_ClusterStart;
    }
}

// Shapes UTF-8 text into `g`. This implements prTextShaperShape.
PrStatus shapeRun(PrTextShaper* shaper, const PrTextRun* run, PrTextGlyphBuffer& g) {
    g.glyphs.clear();
    if (run->text.size == 0)
        return PrStatus_Ok;
    const PrFont* font = run->font;

    // Synthesizes what the caps and position request if the font lacks it.
    PrTextCaps caps = run->caps;
    bool synthesizeCaps = false;
    uint32_t tags[2];
    if (capsFeatures(caps, tags) != 0 && caps != PrTextCaps_Titling) {
        PrTextRun plain = *run;
        plain.featureCount = 0;
        bool petite = caps == PrTextCaps_Petite || caps == PrTextCaps_AllPetite;
        if (petite && !fontGives(shaper, plain, tags[capsFeatures(caps, tags) - 1], false, g))
            caps = caps == PrTextCaps_Petite ? PrTextCaps_Small : PrTextCaps_AllSmall;
        uint32_t last = tags[capsFeatures(caps, tags) - 1];
        synthesizeCaps = (run->synthesis & PrTextSynthesisFlag_SmallCaps) != 0 && !fontGives(shaper, plain, last, false, g);
    }
    bool synthesizePosition = false;
    if (positionFeature(run->position) != 0 && (run->synthesis & PrTextSynthesisFlag_Position)) {
        PrTextRun plain = *run;
        plain.featureCount = 0;
        synthesizePosition = !fontGives(shaper, plain, positionFeature(run->position), true, g);
    }

    // Most text is not spaced, and skips the spacing's work: a run shaped whole has no spacing to apply.
    bool spaced = run->letterSpacing != 0 || run->wordSpacing != 0;
    PrTextRun shaped = *run;
    mergeFeatures(g.features, *run, synthesizeCaps ? PrTextCaps_Normal : caps, !synthesizePosition);
    shaped.features = g.features.data;
    shaped.featureCount = g.features.count;
    PrStatus status = PrStatus_Ok;
    PrFontMetrics metrics = prFontMetrics(font);
    float sizeAdjust = prFontSizeAdjust(font);
    if (synthesizeCaps) {
        // Small capitals as tall as the x-height, within reason.
        float scale = metrics.capHeight > 0 ? metrics.xHeight / metrics.capHeight : 0.7f;
        status = shapeSynthesizedCaps(shaper, shaped, prF32Clamp(scale, 0.6f, 0.9f), g);
    } else {
        status = shapeWith(shaper, shaped, g);
        if (status == PrStatus_Ok && spaced) {
            g.spacing.resize(g.glyphs.count);
            spacingOf(shaped, shaped.text, g.glyphs.data, g.glyphs.count, scratchOf(g).order, g.spacing.data);
        }
    }
    if (status != PrStatus_Ok) {
        g.glyphs.clear();
        return status;
    }

    // The ems of the backend are those of the design. The ems of the run include its size-adjust. A synthesized
    // position moves and scales the glyphs according to the metrics of the font.
    float positionScale = 1;
    float shift = 0;
    if (synthesizePosition) {
        bool sub = run->position == PrTextPosition_Subscript;
        positionScale = (sub ? metrics.subscriptSize : metrics.superscriptSize) / sizeAdjust;
        shift = sub ? metrics.subscriptPosition : metrics.superscriptPosition;
    }
    bool vertical = run->axis == PrTextAxis_Vertical;
    for (uint32_t i = 0; i < g.glyphs.count; ++i) {
        PrTextGlyph& glyph = g.glyphs.data[i];
        float k = sizeAdjust * positionScale;
        glyph.offset = glyph.offset * k;
        glyph.advance = glyph.advance * k;
        glyph.scale *= positionScale;
        if (vertical)
            glyph.offset.x -= shift;
        else
            glyph.offset.y += shift;
    }
    // For a font whose hinting fits advances, the design advance of each glyph is replaced with its hinted advance,
    // keeping what shaping added.
    if (!vertical && prFontHinting(font) == PrFontHinting_Full && run->pixelsPerEm > 0) {
        for (uint32_t i = 0; i < g.glyphs.count; ++i) {
            PrTextGlyph& glyph = g.glyphs.data[i];
            float s = glyph.scale;
            float hinted = prFontGlyphAdvance(font, glyph.id, run->pixelsPerEm * s);
            float design = prFontGlyphAdvance(font, glyph.id, 0);
            glyph.advance.x += (hinted - design) * s;
        }
    }
    // A default ignorable code point for which shaping left a glyph with no advance draws nothing. HarfBuzz, for
    // example, leaves the space glyph of the font. A font that marks missing characters, such as Last Resort, has a
    // visible space glyph.
    uint32_t kept = 0;
    for (uint32_t i = 0; i < g.glyphs.count; ++i) {
        const PrTextGlyph& glyph = g.glyphs.data[i];
        uint32_t codePoint = 0;
        if (glyph.cluster < run->text.size)
            prUtf8Decode(run->text, glyph.cluster, &codePoint);
        bool ignorable = (prUnicodeFlags(codePoint) & PrUnicodeFlag_DefaultIgnorable) != 0 && glyph.advance.x == 0 &&
            glyph.advance.y == 0;
        if (!ignorable) {
            if (spaced)
                g.spacing.data[kept] = g.spacing.data[i];
            g.glyphs.data[kept++] = glyph;
        }
    }
    g.glyphs.count = kept;
    // The spacing after a glyph widens its advance. Right to left, the space is on the glyph's left, so the glyph moves
    // right by it.
    for (uint32_t i = 0; spaced && i < kept; ++i) {
        PrTextGlyph& glyph = g.glyphs.data[i];
        float spacing = g.spacing.data[i];
        if (vertical) {
            glyph.advance.y += spacing;
        } else {
            glyph.advance.x += spacing;
            if (run->direction == PrTextDirection_Rtl)
                glyph.offset.x += spacing;
        }
    }
    bool rightToLeft = run->direction == PrTextDirection_Rtl;
    mergeClusters(g.glyphs.data, g.glyphs.count, rightToLeft);
    markGraphemes(g, run->text, rightToLeft);
    return PrStatus_Ok;
}

// Returns the script that the text starts in: the script of its first character whose script is neither Common nor
// Inherited, or Zzzz if there is none.
PrScript startingScript(PrStringView text) {
    for (size_t at = 0; at < text.size;) {
        uint32_t codePoint = 0;
        at += prUtf8Decode(text, at, &codePoint);
        PrScript script = prUnicodeScript(codePoint);
        if (script != PrScript_Zyyy && script != PrScript_Zinh && script != PrScript_Zzzz)
            return script;
    }
    return PrScript_Zzzz;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

struct PrTextFeatureSet {
    mutable Atomic<uint32_t> references;
    PrTextFeatureSetDesc desc;      // its features follow it in the allocation
};

bool prTextIsWordSeparator(uint32_t codePoint) {
    return codePoint == 0x20 || codePoint == 0xA0 || codePoint == 0x1361 || codePoint == 0x10100 ||
        codePoint == 0x10101 || codePoint == 0x1039F || codePoint == 0x1091F;
}

PrTextFeatureSet* prTextFeatureSetCreate(const PrTextFeatureSetDesc* desc) {
    size_t size = sizeof(PrTextFeatureSet) + sizeof(PrTextFeature) * desc->featureCount;
    PrTextFeatureSet* set = static_cast<PrTextFeatureSet*>(prMemoryAlloc(size));
    set->references.storeRelaxed(1);
    set->desc = *desc;
    PrTextFeature* features = reinterpret_cast<PrTextFeature*>(set + 1);
    if (desc->featureCount != 0)
        prMemCopy(features, desc->features, sizeof(PrTextFeature) * desc->featureCount);
    set->desc.features = desc->featureCount != 0 ? features : nullptr;
    return set;
}

void prTextFeatureSetRetain(const PrTextFeatureSet* set) {
    set->references.fetchAddRelaxed(1);
}

void prTextFeatureSetRelease(const PrTextFeatureSet* PR_NULLABLE set) {
    if (set != nullptr && set->references.fetchSub(1) == 1)
        prMemoryFree(const_cast<PrTextFeatureSet*>(set));
}

bool prTextFeatureSetEquals(const PrTextFeatureSet* PR_NULLABLE a, const PrTextFeatureSet* PR_NULLABLE b) {
    if (a == b)
        return true;
    PrTextRun x{};
    PrTextRun y{};
    prTextFeatureSetApply(a, &x);
    prTextFeatureSetApply(b, &y);
    return x.caps == y.caps && x.position == y.position && x.kerning == y.kerning && x.synthesis == y.synthesis &&
        x.featureCount == y.featureCount &&
        (x.featureCount == 0 || prMemEquals(x.features, y.features, sizeof(PrTextFeature) * x.featureCount));
}

void prTextFeatureSetApply(const PrTextFeatureSet* PR_NULLABLE set, PrTextRun* run) {
    if (set == nullptr) {
        run->caps = PrTextCaps_Normal;
        run->position = PrTextPosition_Normal;
        run->kerning = PrTextKerning_Auto;
        run->synthesis = PrTextSynthesisFlag_SmallCaps | PrTextSynthesisFlag_Position;
        run->features = nullptr;
        run->featureCount = 0;
        return;
    }
    run->caps = set->desc.caps;
    run->position = set->desc.position;
    run->kerning = set->desc.kerning;
    run->synthesis = set->desc.synthesis;
    run->features = set->desc.features;
    run->featureCount = set->desc.featureCount;
}

PrTextShaperBackend prTextShaperBackendFromName(PrStringView name) {
    for (PrTextShaperBackend backend = 0; backend < PrTextShaperBackend_Count; ++backend) {
        if (prAsciiEqualsFold(kBackendNames[backend], name))
            return backend;
    }
    return PrTextShaperBackend_Count;
}

PrStringView prTextShaperBackendName(PrTextShaperBackend backend) {
    return backend < PrTextShaperBackend_Count ? kBackendNames[backend] : PrStringView{};
}

bool prTextShaperBackendIsBuilt(PrTextShaperBackend backend) {
    for (PrTextShaperBackend built : kBuilt) {
        if (built != PrTextShaperBackend_Default && (built == backend || backend == PrTextShaperBackend_Default))
            return true;
    }
    return false;
}

PrTextShaper* PR_NULLABLE prTextShaperCreate(PrTextShaperBackend backend) {
    switch (backend == PrTextShaperBackend_Default ? kBuilt[0] : backend) {
#if defined(PR_TEXT_HAS_HARFBUZZ)
        case PrTextShaperBackend_HarfBuzz: return prTextHarfBuzzCreate();
#endif
#if defined(PR_TEXT_HAS_KB)
        case PrTextShaperBackend_Kb: return prTextKbCreate();
#endif
#if defined(PR_TEXT_HAS_CORETEXT)
        case PrTextShaperBackend_CoreText: return prTextCoreTextCreate();
#endif
#if defined(PR_TEXT_HAS_SIMPLE)
        case PrTextShaperBackend_Simple: return prTextSimpleCreate();
#endif
        default: return nullptr;
    }
}

void prTextShaperDestroy(PrTextShaper* PR_NULLABLE shaper) {
    if (shaper == nullptr)
        return;
    shaper->destroyFn(shaper);
}

PrStatus prTextShaperShape(PrTextShaper* shaper, const PrTextRun* run, PrTextGlyphBuffer* out) {
    PR_ASSERT(isUtf8(run->text) && isUtf8(run->before) && isUtf8(run->after));
    out->glyphs.clear();
    PrTextRun resolved = *run;
    if (resolved.script == PrScript_Zzzz)
        resolved.script = startingScript(run->text);
    return shapeRun(shaper, &resolved, *out);
}

PrTextGlyphBuffer* prTextGlyphBufferCreate(void) {
    return allocZeroed<PrTextGlyphBuffer>();
}

void prTextGlyphBufferDestroy(PrTextGlyphBuffer* PR_NULLABLE buffer) {
    if (buffer == nullptr)
        return;
    prTextGlyphBufferDestroy(buffer->scratch);
    buffer->glyphs.reset();
    buffer->features.reset();
    buffer->pieceFeatures.reset();
    buffer->piece.reset();
    buffer->origin.reset();
    buffer->probe.reset();
    buffer->boundaries.reset();
    buffer->spacing.reset();
    buffer->order.reset();
    prMemoryFree(buffer);
}

void prTextGlyphBufferClear(PrTextGlyphBuffer* buffer) {
    buffer->glyphs.clear();
}

uint32_t prTextGlyphBufferGlyphCount(const PrTextGlyphBuffer* buffer) {
    return buffer->glyphs.count;
}

const PrTextGlyph* PR_NULLABLE prTextGlyphBufferGlyphs(const PrTextGlyphBuffer* buffer) {
    return buffer->glyphs.data;
}

PrTextGlyph* prTextGlyphBufferAppend(PrTextGlyphBuffer* buffer, uint32_t count) {
    RawArray<PrTextGlyph>& array = buffer->glyphs;
    uint32_t start = array.count;
    array.reserve(start + count);
    array.count = start + count;
    for (uint32_t i = start; i < array.count; ++i)
        array.data[i] = PrTextGlyph{0, 0, {0, 0}, {0, 0}, 1, 0};
    return array.data + start;
}

PrVec2 prTextGlyphBufferAdvance(const PrTextGlyphBuffer* buffer) {
    const RawArray<PrTextGlyph>& array = buffer->glyphs;
    PrVec2 sum{0, 0};
    for (uint32_t i = 0; i < array.count; ++i) {
        sum.x += array.data[i].advance.x;
        sum.y += array.data[i].advance.y;
    }
    return sum;
}

PrVec2 prTextGlyphBufferPlace(const PrTextGlyphBuffer* buffer, float size, PrVec2 origin, PrFontGlyph* out,
        PrAffine2* PR_NULLABLE transforms) {
    const RawArray<PrTextGlyph>& array = buffer->glyphs;
    PrVec2 pen = origin;
    for (uint32_t i = 0; i < array.count; ++i) {
        const PrTextGlyph& g = array.data[i];
        out[i] = PrFontGlyph{g.id, pen + g.offset * size};
        if (transforms != nullptr)
            transforms[i] = prAffine2Scale(PrVec2{g.scale, g.scale});
        pen.x += g.advance.x * size;
        pen.y += g.advance.y * size;
    }
    return pen;
}
