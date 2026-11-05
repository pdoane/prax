// Shaped text, and the algorithms that divide it into lines.

#include "pr/text_shaping.h"

#include "pr/alloc.h"
#include "pr/assert.h"
#include "pr/float.h"
#include "pr/mem.h"
#include "pr/memory.h"
#include "pr/raw_array.h"
#include "pr/unicode_bidi.h"
#include "pr/utf8.h"

namespace pr_impl {

namespace {

// The fields of a piece that breaking reads. A shaping copies them because the host owns the pieces.
struct PieceSpan {
    PrTextPieceKind kind;
    PrTextBreakStyle breakStyle;
    bool noWrap;
    bool breakSpaces;
    uint32_t start;
    uint32_t end;
};

struct TextShaping {
    PrTextDirection direction;
    uint32_t size;                      // the number of bytes of the text
    RawArray<PieceSpan> pieces;
    RawArray<PrTextShapingSegment> segments;
    RawArray<PrTextSpaceFlags> spaces;     // by byte
    float tabSize;
    float tabMinimum;
    bool wrapsWords;                       // true if lines find emergency opportunities themselves (findWordBreaks)
    RawArray<PrUnicodeLineBreak> breakAt;  // by byte and one past the last
    RawArray<PrTextEmergency> emergency;   // by byte and one past the last, or empty
    RawArray<float> advance;               // by byte
    RawArray<double> reach;                // by byte and one past the last
    RawArray<uint32_t> glyphAt;            // by byte and one past the last
    RawArray<PrTextShapedRun> runs;
    RawArray<PrTextShapedGlyph> glyphs;
    bool scaled;                           // true if a glyph's scale is not one
    RawArray<uint8_t> levels;              // by byte, or empty if every level is even
    RawArray<PrTextParagraph> paragraphs;
    RawArray<uint8_t> marks;               // by index of a piece, or empty if every level is even
};

struct TextShapingScratch {
    PrTextItems* PR_NULLABLE items;
    PrTextGlyphBuffer* PR_NULLABLE buffer;
    RawArray<uint32_t> codePoints;
    RawArray<uint32_t> codeEnds;             // the byte past each code point
    RawArray<PrUnicodeLineBreak> breaks;     // by code point
    RawArray<PrTextBreakStyle> breakStyles;  // by code point
    RawArray<PrTextEmergency> codeEmergency; // by code point
    RawArray<bool> unshaped;                 // by byte
    RawArray<uint32_t> open;                 // the Open pieces around the piece that wrapBreaks has reached
};

struct TextWordBreaks {
    RawArray<uint32_t> codePoints;
    RawArray<uint32_t> ends;               // the byte past each code point, from the first byte of the word
    RawArray<PrTextBreakStyle> styles;     // by code point
    RawArray<PrUnicodeLineBreak> breaks;   // by code point
    RawArray<PrTextEmergency> emergency;   // by code point
    RawArray<PrTextEmergency> atByte;      // by byte from the first of the word, and one past its last
    uint32_t start;                        // the bytes of the word in the text, or none if `end` is 0
    uint32_t end;
};

// Returns how the text of `piece` breaks within words. A piece that is not text breaks by the defaults.
PrTextBreakStyle breakStyleOf(const PieceSpan& piece) {
    return piece.kind == PrTextPieceKind_Text ? piece.breakStyle : PrTextBreakStyle{};
}

// Writes the style of each of `count` code points of the text, which end at `base` plus `ends`, to `out`.
void fillBreakStyles(const TextShaping& t, uint32_t base, const uint32_t* ends, PrTextBreakStyle* out, uint32_t count) {
    uint32_t at = 0;
    for (uint32_t i = 0; i < t.pieces.count && at < count; ++i) {
        const PieceSpan& piece = t.pieces.data[i];
        PrTextBreakStyle style = breakStyleOf(piece);
        while (at < count && base + ends[at] <= piece.start)
            out[at++] = PrTextBreakStyle{};
        while (at < count && base + ends[at] <= piece.end)
            out[at++] = style;
    }
    while (at < count)
        out[at++] = PrTextBreakStyle{};
}

// Returns true if a soft wrap opportunity stands between an object and the code point `codePoint` beside it. One stands
// beside every code point except those of the GL, WJ and ZWJ classes, and also beside U+00A0 NO-BREAK SPACE.
bool wrapsBesideObject(uint32_t codePoint) {
    PrUnicodeLineBreakFlags flags = 0;
    PrUnicodeLineBreakClass c = prUnicodeLineBreakClass(codePoint, &flags);
    return codePoint == 0xA0 ||
        (c != PrUnicodeLineBreakClass_GL && c != PrUnicodeLineBreakClass_WJ && c != PrUnicodeLineBreakClass_ZWJ);
}

// Returns true if the code point `codePoint`, which follows an object, takes a soft wrap opportunity before it. A space
// does not, because UAX14 puts the opportunity after the space so that no line starts with it. A hard break does not,
// because no opportunity stands before one (LB6).
bool wrapsBeforeAfterObject(uint32_t codePoint) {
    PrUnicodeLineBreakFlags flags = 0;
    PrUnicodeLineBreakClass c = prUnicodeLineBreakClass(codePoint, &flags);
    return c != PrUnicodeLineBreakClass_SP && c != PrUnicodeLineBreakClass_BK && c != PrUnicodeLineBreakClass_CR &&
        c != PrUnicodeLineBreakClass_LF && c != PrUnicodeLineBreakClass_NL && wrapsBesideObject(codePoint);
}

// Adds a soft wrap opportunity before and after each object. None is added beside a code point of its segment that
// suppresses it under UAX14, at a forced break, or before a space or a hard break.
void objectBreaks(TextShaping& t, const TextShapingScratch& s) {
    uint32_t at = 0;                    // the code point that ends past the object's start
    for (uint32_t k = 0; k < t.segments.count; ++k) {
        const PrTextShapingSegment& segment = t.segments.data[k];
        for (uint32_t i = segment.pieceStart; i < segment.pieceEnd; ++i) {
            const PieceSpan& piece = t.pieces.data[i];
            if (piece.kind != PrTextPieceKind_Object)
                continue;
            while (at < s.codeEnds.count && s.codeEnds.data[at] <= piece.start)
                ++at;
            if (piece.start != segment.byteStart && t.breakAt.data[piece.start] != PrUnicodeLineBreak_Mandatory &&
                    wrapsBesideObject(s.codePoints.data[at - 1]))
                t.breakAt.data[piece.start] = PrUnicodeLineBreak_Allowed;
            while (at < s.codeEnds.count && s.codeEnds.data[at] <= piece.end)
                ++at;
            if (piece.end != segment.byteEnd && t.breakAt.data[piece.end] != PrUnicodeLineBreak_Mandatory &&
                    wrapsBeforeAfterObject(s.codePoints.data[at]))
                t.breakAt.data[piece.end] = PrUnicodeLineBreak_Allowed;
        }
    }
}

// Removes the soft wrap opportunities at which text may not wrap, and adds one after each space of text that breaks
// spaces. An opportunity after white space exists if that white space allows wrapping. Any other opportunity follows
// the nearest Open piece that holds the text on both its sides, or `desc.noWrap` if no piece does. Text holds the
// opportunities within itself. Text that asks for none of this costs nothing.
void wrapBreaks(TextShaping& t, PrStringView text, const PrTextShapingDesc& d, TextShapingScratch& s, bool spacesStop) {
    bool any = d.noWrap || spacesStop;
    for (uint32_t i = 0; i < t.pieces.count && !any; ++i)
        any = t.pieces.data[i].noWrap || t.pieces.data[i].breakSpaces;
    if (!any)
        return;
    RawArray<uint32_t>& open = s.open;
    open.clear();
    auto noWrapAt = [&](uint32_t depth) { return depth == 0 ? d.noWrap : t.pieces.data[open.data[depth - 1]].noWrap; };
    auto settle = [&](uint32_t b, bool holderNoWrap) {
        if (t.breakAt.data[b] != PrUnicodeLineBreak_Allowed)
            return;
        PrTextSpaceFlags before = t.spaces.data[b - 1];
        bool space = (before & PrTextSpaceFlag_White) != 0 || text.data[b - 1] == ' ';
        bool noWrap = space ? (before & PrTextSpaceFlag_Wraps) == 0 : holderNoWrap;
        if (noWrap)
            t.breakAt.data[b] = PrUnicodeLineBreak_Prohibited;
    };
    bool before = false;                // true if some text of the segment precedes
    uint32_t shallowest = 0;            // the smallest number of Open pieces open since that text
    for (uint32_t i = 0; i < t.pieces.count; ++i) {
        const PieceSpan& piece = t.pieces.data[i];
        if (piece.kind == PrTextPieceKind_Break && piece.start == piece.end)
            before = false;
        if (piece.kind == PrTextPieceKind_Open) {
            open.push(i);
            continue;
        }
        if (piece.kind == PrTextPieceKind_Close) {
            open.pop();
            shallowest = shallowest < open.count ? shallowest : open.count;
            continue;
        }
        bool isText = piece.kind == PrTextPieceKind_Text;
        if ((!isText && piece.kind != PrTextPieceKind_Object) || piece.start == piece.end)
            continue;
        if (before)
            settle(piece.start, noWrapAt(shallowest));
        bool noWrap = isText ? piece.noWrap : noWrapAt(open.count);
        bool breakSpaces = isText && piece.breakSpaces;
        for (uint32_t b = piece.start + 1; b <= piece.end; ++b) {
            if (breakSpaces && (text.data[b - 1] == ' ' || text.data[b - 1] == '\t') &&
                    t.breakAt.data[b] == PrUnicodeLineBreak_Prohibited)
                t.breakAt.data[b] = PrUnicodeLineBreak_Allowed;
            if (b < piece.end)
                settle(b, noWrap);
        }
        before = true;
        shallowest = open.count;
    }
}

// Finds the break opportunities of the text: those of UAX14 in each segment, tailored by the break styles of the
// pieces, and those that `objectBreak` adds. word-break and overflow-wrap: anywhere are tailored here because they
// change the intrinsic widths. overflow-wrap: break-word alone changes neither, so a line finds its opportunities only
// when a word fits on no line. Text with that style therefore costs nothing extra while its words fit.
void findBreaks(TextShaping& t, PrStringView text, const PrTextShapingDesc& d, TextShapingScratch& s, bool spacesStop) {
    s.codePoints.clear();
    s.codeEnds.clear();
    for (size_t at = 0; at < text.size;) {
        uint32_t codePoint = 0;
        at += prUtf8Decode(text, at, &codePoint);
        s.codePoints.push(codePoint);
        s.codeEnds.push(uint32_t(at));
    }
    s.breaks.resize(s.codePoints.count);
    uint32_t first = 0;                 // the segment's first code point
    for (uint32_t k = 0; k < t.segments.count; ++k) {
        uint32_t end = first;
        while (end < s.codeEnds.count && s.codeEnds.data[end] <= t.segments.data[k].byteEnd)
            ++end;
        if (end != first)
            prUnicodeLineBreaks(s.codePoints.data + first, end - first, s.breaks.data + first);
        first = end;
    }
    bool tailored = false;
    bool wraps = false;
    for (uint32_t i = 0; i < t.pieces.count && !tailored; ++i) {
        PrTextBreakStyle style = breakStyleOf(t.pieces.data[i]);
        tailored = style.wordBreak != PrTextWordBreak_Normal || style.overflowWrap == PrTextOverflowWrap_Anywhere;
        wraps = wraps || style.overflowWrap == PrTextOverflowWrap_BreakWord;
    }
    t.emergency.clear();
    t.wrapsWords = wraps && !tailored;
    if (tailored) {
        s.breakStyles.resize(s.codePoints.count);
        fillBreakStyles(t, 0, s.codeEnds.data, s.breakStyles.data, s.codeEnds.count);
        s.codeEmergency.resize(s.codePoints.count);
        PrTextBreakDesc desc{text, s.codePoints.data, s.codeEnds.data, s.breakStyles.data, s.codePoints.count};
        prTextBreaksTailor(&desc, s.breaks.data, s.codeEmergency.data);
        t.emergency.resize(uint32_t(text.size) + 1);
        prMemZero(t.emergency.data, t.emergency.count * sizeof(PrTextEmergency));
        for (uint32_t i = 0; i < s.codePoints.count; ++i)
            t.emergency.data[s.codeEnds.data[i]] = s.codeEmergency.data[i];
    }
    t.breakAt.resize(uint32_t(text.size) + 1);
    prMemZero(t.breakAt.data, t.breakAt.count * sizeof(PrUnicodeLineBreak));
    for (uint32_t i = 0; i < s.codePoints.count; ++i)
        t.breakAt.data[s.codeEnds.data[i]] = s.breaks.data[i];
    if (d.objectBreak == PrTextObjectBreak_Around)
        objectBreaks(t, s);
    wrapBreaks(t, text, d, s, spacesStop);
}

// Finds the opportunities that overflow-wrap adds to the text from `start` to `end`. Does nothing if `s` already holds
// a word that ends at `end` and starts no later than `start`.
void findWordBreaks(const TextShaping& t, PrStringView text, uint32_t start, uint32_t end, TextWordBreaks& s) {
    if (s.end == end && s.start <= start)
        return;
    s.start = start;
    s.end = end;
    PrStringView word{text.data + start, end - start};
    s.codePoints.clear();
    s.ends.clear();
    for (size_t at = 0; at < word.size;) {
        uint32_t codePoint = 0;
        at += prUtf8Decode(word, at, &codePoint);
        s.codePoints.push(codePoint);
        s.ends.push(uint32_t(at));
    }
    uint32_t count = s.codePoints.count;
    s.styles.resize(count);
    fillBreakStyles(t, start, s.ends.data, s.styles.data, count);
    s.breaks.resize(count);
    for (uint32_t i = 0; i < count; ++i)
        s.breaks.data[i] = t.breakAt.data[start + s.ends.data[i]];
    s.emergency.resize(count);
    PrTextBreakDesc desc{word, s.codePoints.data, s.ends.data, s.styles.data, count};
    prTextBreaksTailor(&desc, s.breaks.data, s.emergency.data);
    s.atByte.resize(end - start + 1);
    prMemZero(s.atByte.data, s.atByte.count * sizeof(PrTextEmergency));
    for (uint32_t i = 0; i < count; ++i)
        s.atByte.data[s.ends.data[i]] = s.emergency.data[i];
}

void resetWordBreaks(TextWordBreaks& s) {
    s.codePoints.reset();
    s.ends.reset();
    s.styles.reset();
    s.breaks.reset();
    s.emergency.reset();
    s.atByte.reset();
}

// Shapes `run` and appends its glyphs. A run at an odd level is shaped right to left, and its glyphs are appended in
// reverse, so that the clusters of all the runs ascend.
void shapeRun(TextShaping& t, const PrTextShapingDesc& d, PrTextShapedRun& run, uint32_t index,
        const TextShapingScratch& s) {
    const PrTextItemRun& item = run.item;
    run.glyphStart = t.glyphs.count;
    run.glyphEnd = t.glyphs.count;
    if (item.face == kPrTextMatchNone)
        return;
    const PrTextPiece& piece = d.pieces[item.piece];
    float size = piece.size > 0 ? piece.size : prTextFontStyle(piece.font).size;
    PR_ASSERT(d.shaper != nullptr);
    PrTextRun shaped{};
    shaped.font = prTextFontMatchedFont(piece.font, item.face);
    shaped.text = PrStringView{d.text.data + item.start, item.end - item.start};
    shaped.before = PrStringView{d.text.data + item.contextStart, item.start - item.contextStart};
    shaped.after = PrStringView{d.text.data + item.end, item.contextEnd - item.end};
    bool rightToLeft = (item.level & 1) != 0;
    shaped.direction = rightToLeft ? PrTextDirection_Rtl : PrTextDirection_Ltr;
    shaped.axis = item.upright ? PrTextAxis_Vertical : PrTextAxis_Horizontal;
    shaped.script = item.script;
    shaped.locale = prTextFontLocale(piece.font);
    shaped.pixelsPerEm = size * (d.pixelRatio > 0 ? d.pixelRatio : 1);
    prTextFeatureSetApply(piece.features, &shaped);
    shaped.letterSpacing = size > 0 ? piece.letterSpacing / size : 0;
    shaped.wordSpacing = size > 0 ? piece.wordSpacing / size : 0;
    if (prTextShaperShape(d.shaper, &shaped, s.buffer) != PrStatus_Ok)
        return;
    const PrTextGlyph* glyphs = prTextGlyphBufferGlyphs(s.buffer);
    uint32_t count = prTextGlyphBufferGlyphCount(s.buffer);
    float upright = 0;
    if (item.upright && d.uprightAdvance != nullptr)
        upright = d.uprightAdvance(d.uprightContext, &piece, item.face);
    for (uint32_t k = 0; k < count; ++k) {
        const PrTextGlyph& glyph = glyphs[rightToLeft ? count - 1 - k : k];
        float advance = (item.upright ? glyph.advance.y : glyph.advance.x) * size;
        if (upright != 0 && advance != 0)
            advance = upright;
        uint32_t cluster = item.start + glyph.cluster;
        t.glyphs.push(PrTextShapedGlyph{
            .cluster = cluster,
            .run = index,
            .advance = advance,
            .offset = PrVec2{glyph.offset.x * size, glyph.offset.y * size},
            .id = glyph.id,
            .scale = glyph.scale,
        });
        t.scaled = t.scaled || glyph.scale != 1;
        t.advance.data[cluster] += advance;
    }
    run.glyphEnd = t.glyphs.count;
}

// Returns the advance of a preserved tab at `x`, the distance along the line from the origin of the tab stops. A tab
// advances to the next tab stop, or to the one after that if the next is nearer than the minimum advance of a tab.
float tabAdvance(const TextShaping& t, float x) {
    if (t.tabSize <= 0)
        return 0;
    float stop = (prF32Floor(x / t.tabSize) + 1) * t.tabSize;
    if (stop - x < t.tabMinimum)
        stop += t.tabSize;
    return stop - x;
}

// Measures a line from its first byte as it extends one byte at a time.
struct LinePen {
    const TextShaping& t;
    const float* PR_NULLABLE positions;
    float origin;
    uint32_t at;                        // the first byte that is not measured yet
    float x;                            // the width of the bytes before `at`
    float trailing;                     // the advance of the hanging white space at the end of those bytes

    // Extends the line to `end`.
    void to(uint32_t end) {
        // This runs for every byte of every line, so it keeps in locals what the compiler would otherwise load each
        // time.
        const float* advances = t.advance.data;
        const PrTextSpaceFlags* spaces = t.spaces.data;
        const float* steps = positions;
        float width = x;
        float hanging = trailing;
        for (uint32_t b = at; b < end; ++b) {
            float advance = advances[b];
            width += steps != nullptr ? steps[b + 1] - steps[b] : advance;
            PrTextSpaceFlags flags = spaces[b];
            if ((flags & PrTextSpaceFlag_Tab) != 0) {
                advance = tabAdvance(t, origin + width);
                width += advance;
            }
            hanging = (flags & (PrTextSpaceFlag_Collapsible | PrTextSpaceFlag_Hangs)) != 0 ? hanging + advance : 0;
        }
        x = width;
        trailing = hanging;
        at = at > end ? at : end;
    }

    // Returns the width of the line so far, without its hanging white space.
    float width() const { return x - trailing; }
};

// Returns the end of a line that does not fit in `width` at any opportunity before `end`. The end is the last
// opportunity that overflow-wrap adds before `end` and that fits. If none fits, it is the first such opportunity, and
// if there is none, it is `end`.
uint32_t breakWord(const TextShaping& t, PrStringView text, const PrTextShapingLine& line, float width, uint32_t end,
        TextWordBreaks* PR_NULLABLE words) {
    const PrTextEmergency* emergency = nullptr;
    uint32_t base = 0;
    TextWordBreaks local{};
    if (t.emergency.count != 0) {
        emergency = t.emergency.data;
    } else if (t.wrapsWords) {
        TextWordBreaks& s = words != nullptr ? *words : local;
        findWordBreaks(t, text, line.start, end, s);
        emergency = s.atByte.data;
        base = s.start;
    } else {
        return end;
    }
    uint32_t rescue = 0;
    uint32_t found = end;
    bool foundOne = false;
    LinePen pen{.t = t, .positions = line.positions, .origin = line.origin, .at = line.start};
    for (uint32_t b = line.start + 1; b < end; ++b) {
        if (emergency[b - base] == PrTextEmergency_None || t.breakAt.data[b] != PrUnicodeLineBreak_Prohibited)
            continue;
        pen.to(b);
        if (pen.width() > width + kPrTextLineFitTolerance) {
            found = rescue != 0 ? rescue : b;
            foundOne = true;
            break;
        }
        rescue = b;
    }
    resetWordBreaks(local);
    return foundOne ? found : (rescue != 0 ? rescue : end);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

// C opaque types
struct PrTextShaping : TextShaping {};
struct PrTextShapingScratch : TextShapingScratch {};
struct PrTextWordBreaks : TextWordBreaks {};

PrTextShapingScratch* prTextShapingScratchCreate(void) {
    return allocZeroed<PrTextShapingScratch>();
}

void prTextShapingScratchDestroy(PrTextShapingScratch* PR_NULLABLE scratch) {
    if (scratch == nullptr)
        return;
    prTextItemsDestroy(scratch->items);
    prTextGlyphBufferDestroy(scratch->buffer);
    scratch->codePoints.reset();
    scratch->codeEnds.reset();
    scratch->breaks.reset();
    scratch->breakStyles.reset();
    scratch->codeEmergency.reset();
    scratch->unshaped.reset();
    scratch->open.reset();
    prMemoryFree(scratch);
}

PrTextShaping* prTextShapingCreate(void) {
    return allocZeroed<PrTextShaping>();
}

void prTextShapingDestroy(PrTextShaping* PR_NULLABLE shaping) {
    if (shaping == nullptr)
        return;
    shaping->pieces.reset();
    shaping->segments.reset();
    shaping->spaces.reset();
    shaping->breakAt.reset();
    shaping->emergency.reset();
    shaping->advance.reset();
    shaping->glyphAt.reset();
    shaping->runs.reset();
    shaping->glyphs.reset();
    shaping->levels.reset();
    shaping->paragraphs.reset();
    shaping->marks.reset();
    shaping->reach.reset();
    prMemoryFree(shaping);
}

void prTextShapingSet(PrTextShaping* shaping, const PrTextShapingDesc* desc, PrTextShapingScratch* scratch) {
    PR_ASSERT(desc->text.size < UINT32_MAX);
    TextShaping& t = *shaping;
    TextShapingScratch& s = *scratch;
    uint32_t size = uint32_t(desc->text.size);
    t.direction = desc->direction;
    t.size = size;
    t.pieces.clear();
    t.segments.clear();
    PrTextShapingSegment segment{};
    for (uint32_t i = 0; i < desc->pieceCount; ++i) {
        const PrTextPiece& piece = desc->pieces[i];
        t.pieces.push(PieceSpan{piece.kind, piece.breakStyle, piece.noWrap, piece.breakSpaces, piece.start, piece.end});
        if (piece.kind == PrTextPieceKind_Break && piece.start == piece.end) {
            segment.byteEnd = piece.start;
            segment.pieceEnd = i;
            t.segments.push(segment);
            segment = PrTextShapingSegment{.byteStart = piece.start, .pieceStart = i + 1};
        }
    }
    segment.byteEnd = size;
    segment.pieceEnd = desc->pieceCount;
    t.segments.push(segment);

    t.spaces.resize(size);
    bool tabs = false;
    bool spacesStop = false;            // true if white space stops a line from wrapping after it
    for (uint32_t b = 0; b < size; ++b) {
        bool space = desc->text.data[b] == ' ';
        PrTextSpaceFlags spaces = space ? PrTextSpaceFlag_Hangs | PrTextSpaceFlag_Wraps : 0;
        if (desc->spaces != nullptr)
            spaces = desc->spaces[b];
        t.spaces.data[b] = spaces;
        tabs |= (spaces & PrTextSpaceFlag_Tab) != 0;
        spacesStop |= (space || (spaces & PrTextSpaceFlag_White) != 0) && (spaces & PrTextSpaceFlag_Wraps) == 0;
    }
    if (tabs) {
        s.unshaped.resize(size);
        for (uint32_t b = 0; b < size; ++b)
            s.unshaped.data[b] = (t.spaces.data[b] & PrTextSpaceFlag_Tab) != 0;
    }
    findBreaks(t, desc->text, *desc, s, spacesStop);

    if (s.items == nullptr)
        s.items = prTextItemsCreate();
    if (s.buffer == nullptr)
        s.buffer = prTextGlyphBufferCreate();
    PrTextItemizeDesc itemize{
        .text = desc->text,
        .pieces = desc->pieces,
        .pieceCount = desc->pieceCount,
        .direction = desc->direction,
        .bidi = desc->bidi,
        .unshaped = tabs ? s.unshaped.data : nullptr,
    };
    prTextItemsItemize(s.items, &itemize);
    t.tabSize = desc->tabSize;
    t.tabMinimum = desc->tabMinimum;

    t.advance.resize(size);
    if (size != 0)
        prMemZero(t.advance.data, size * sizeof(float));
    t.glyphs.clear();
    t.scaled = false;
    t.runs.clear();
    const PrTextItemRun* runs = prTextItemsRuns(s.items);
    for (uint32_t r = 0; r < prTextItemsRunCount(s.items); ++r) {
        t.runs.push(PrTextShapedRun{.item = runs[r]});
        shapeRun(t, *desc, t.runs.data[r], r, s);
    }
    t.glyphAt.resize(size + 1);
    t.reach.resize(size + 1);
    t.reach.data[0] = 0;
    uint32_t at = 0;
    for (uint32_t b = 0; b <= size; ++b) {
        if (b != 0)
            t.reach.data[b] = t.reach.data[b - 1] + double(t.advance.data[b - 1]);
        while (at < t.glyphs.count && t.glyphs.data[at].cluster < b)
            ++at;
        t.glyphAt.data[b] = at;
    }
    t.levels.clear();
    t.paragraphs.clear();
    t.marks.clear();
    const uint8_t* levels = prTextItemsLevels(s.items);
    if (levels != nullptr) {
        t.levels.append(levels, size);
        t.paragraphs.append(prTextItemsParagraphs(s.items), prTextItemsParagraphCount(s.items));
        t.marks.append(prTextItemsMarks(s.items), desc->pieceCount);
    }
}

PrTextShapingInfo prTextShapingInfo(const PrTextShaping* shaping) {
    return PrTextShapingInfo{
        .runs = shaping->runs.data,
        .runCount = shaping->runs.count,
        .glyphs = shaping->glyphs.data,
        .glyphCount = shaping->glyphs.count,
        .scaled = shaping->scaled,
        .segments = shaping->segments.data,
        .segmentCount = shaping->segments.count,
        .paragraphs = shaping->paragraphs.data,
        .paragraphCount = shaping->paragraphs.count,
        .advances = shaping->advance.data,
        .reach = shaping->reach.data,
        .glyphAt = shaping->glyphAt.data,
        .breaks = shaping->breakAt.data,
        .emergency = shaping->emergency.count != 0 ? shaping->emergency.data : nullptr,
        .levels = shaping->levels.count != 0 ? shaping->levels.data : nullptr,
        .marks = shaping->marks.count != 0 ? shaping->marks.data : nullptr,
    };
}

uint8_t prTextShapingParagraphLevel(const PrTextShaping* shaping, uint32_t byte) {
    const RawArray<PrTextParagraph>& paragraphs = shaping->paragraphs;
    if (paragraphs.count == 0)
        return shaping->direction == PrTextDirection_Rtl ? 1 : 0;
    uint32_t low = 0;
    uint32_t high = paragraphs.count - 1;
    while (low < high) {
        uint32_t middle = low + (high - low) / 2;
        if (byte < paragraphs.data[middle].end)
            high = middle;
        else
            low = middle + 1;
    }
    return paragraphs.data[low].level;
}

void prTextShapingPositions(const PrTextShaping* shaping, const PrTextShapingSegment* segment,
        const float* PR_NULLABLE advances, float* out) {
    const TextShaping& t = *shaping;
    uint32_t start = segment->byteStart;
    uint32_t size = segment->byteEnd - start;
    // Indexed from the start of the segment.
    float* position = out + start;
    const float* advance = t.advance.data + start;
    prMemZero(position, (size + 1) * sizeof(float));
    for (uint32_t i = segment->pieceStart; advances != nullptr && i < segment->pieceEnd && size != 0; ++i) {
        const PieceSpan& piece = t.pieces.data[i];
        uint32_t at = piece.start - start;
        // position[b + 1] holds what goes with byte b.
        if (piece.kind == PrTextPieceKind_Open || piece.kind == PrTextPieceKind_Object ||
                piece.kind == PrTextPieceKind_Boundary)
            position[at < size ? at + 1 : size] += advances[i];
        else if (piece.kind == PrTextPieceKind_Close)
            position[at > 0 ? at : 1] += advances[i];
    }
    float x = 0;
    for (uint32_t b = 0; b <= size; ++b) {
        x += position[b];
        if (b != 0)
            x += advance[b - 1];
        position[b] = x;
    }
}

float prTextShapingLineWidth(const PrTextShaping* shaping, const PrTextShapingLine* line) {
    LinePen pen{.t = *shaping, .positions = line->positions, .origin = line->origin, .at = line->start};
    pen.to(line->end);
    return pen.width();
}

float prTextShapingTabAdvance(const PrTextShaping* shaping, float x) {
    return tabAdvance(*shaping, x);
}

PrTextIntrinsic prTextShapingIntrinsic(const PrTextShaping* shaping, uint32_t start, uint32_t end,
        const float* PR_NULLABLE minPositions, const float* PR_NULLABLE maxPositions) {
    const TextShaping& t = *shaping;
    auto widest = [&](bool soft, const float* PR_NULLABLE positions) {
        float width = 0;
        uint32_t from = start;
        for (uint32_t b = start + 1; b <= end; ++b) {
            PrUnicodeLineBreak at = t.breakAt.data[b];
            if (soft && at == PrUnicodeLineBreak_Prohibited && t.emergency.count != 0 &&
                    t.emergency.data[b] == PrTextEmergency_Anywhere)
                at = PrUnicodeLineBreak_Allowed;
            if (b != end && (at == PrUnicodeLineBreak_Prohibited || (!soft && at != PrUnicodeLineBreak_Mandatory)))
                continue;
            PrTextShapingLine line{.start = from, .end = b, .positions = positions};
            width = prF32Max(width, prTextShapingLineWidth(shaping, &line));
            from = b;
        }
        return width;
    };
    PrTextIntrinsic sizes{.minContent = widest(true, minPositions)};
    sizes.maxContent = prF32Max(widest(false, maxPositions), sizes.minContent);
    return sizes;
}

PrTextWordBreaks* prTextWordBreaksCreate(void) {
    return allocZeroed<PrTextWordBreaks>();
}

void prTextWordBreaksDestroy(PrTextWordBreaks* PR_NULLABLE words) {
    if (words == nullptr)
        return;
    resetWordBreaks(*words);
    prMemoryFree(words);
}

void prTextWordBreaksClear(PrTextWordBreaks* words) {
    words->end = 0;
}

uint32_t prTextShapingBreakLine(const PrTextShaping* shaping, PrStringView text, const PrTextShapingLine* line,
        float width, PrTextWordBreaks* PR_NULLABLE words) {
    const TextShaping& t = *shaping;
    // One pass that measures each byte and then reads the opportunity after it, since every line reads every byte of
    // itself both ways.
    uint32_t fits = 0;
    uint32_t end = line->end;
    const PrUnicodeLineBreak* breakAt = t.breakAt.data;
    const float* advances = t.advance.data;
    const PrTextSpaceFlags* spaces = t.spaces.data;
    const float* positions = line->positions;
    float limit = width + kPrTextLineFitTolerance;
    float x = 0;
    float hanging = 0;
    for (uint32_t b = line->start; b < end; ++b) {
        float advance = advances[b];
        x += positions != nullptr ? positions[b + 1] - positions[b] : advance;
        PrTextSpaceFlags flags = spaces[b];
        if ((flags & PrTextSpaceFlag_Tab) != 0) {
            advance = tabAdvance(t, line->origin + x);
            x += advance;
        }
        hanging = (flags & (PrTextSpaceFlag_Collapsible | PrTextSpaceFlag_Hangs)) != 0 ? hanging + advance : 0;
        PrUnicodeLineBreak after = breakAt[b + 1];
        if (after == PrUnicodeLineBreak_Prohibited)
            continue;
        if (x - hanging > limit)
            return fits != 0 ? fits : breakWord(t, text, *line, width, b + 1, words);
        fits = b + 1;
        if (after == PrUnicodeLineBreak_Mandatory && b + 1 != end)
            return b + 1;
    }
    return fits != 0 ? fits : end;
}

uint32_t prTextShapingLineRuns(const PrTextShaping* shaping, uint32_t start, uint32_t end, uint32_t content,
        PrTextLineRun* PR_NULLABLE out, uint32_t capacity) {
    const TextShaping& t = *shaping;
    uint8_t paragraph = prTextShapingParagraphLevel(shaping, start);
    // The first run that reaches past the start of the line.
    uint32_t low = 0;
    uint32_t high = t.runs.count;
    while (low < high) {
        uint32_t middle = low + (high - low) / 2;
        if (t.runs.data[middle].item.end <= start)
            low = middle + 1;
        else
            high = middle;
    }
    RawArray<PrTextLineRun> logical;
    RawArray<uint8_t> levels;
    for (uint32_t r = low; r < t.runs.count && t.runs.data[r].item.start < end; ++r) {
        const PrTextItemRun& run = t.runs.data[r].item;
        uint32_t from = run.start > start ? run.start : start;
        uint32_t to = run.end < end ? run.end : end;
        if (from >= to)
            continue;
        if (from < content)
            logical.push(PrTextLineRun{r, from, to < content ? to : content, run.level});
        if (to > content)
            logical.push(PrTextLineRun{r, from > content ? from : content, to, paragraph});
    }
    uint32_t count = logical.count;
    if (out != nullptr && count != 0) {
        levels.resize(count);
        for (uint32_t i = 0; i < count; ++i)
            levels.data[i] = logical.data[i].level;
        RawArray<uint32_t> order;
        order.resize(count);
        prUnicodeBidiReorder(levels.data, count, order.data);
        for (uint32_t i = 0; i < count && i < capacity; ++i)
            out[i] = logical.data[order.data[i]];
        order.reset();
    }
    logical.reset();
    levels.reset();
    return count;
}
