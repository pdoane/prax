// Text layouts.
//
// UAX9 - https://www.unicode.org/reports/tr9/
// UAX14 - https://www.unicode.org/reports/tr14/
// UAX29 - https://www.unicode.org/reports/tr29/
//
// A layout shapes the whole text once, run by run as itemization divides it, so a line takes its glyphs from runs
// shaped with their whole context. It breaks lines greedily at the opportunities of UAX14, and the white space that a
// line ends in hangs past its width. It then orders the pieces of each line by the rules of UAX9 for a line (L1, L2):
// the white space at the end of the line takes the level of its paragraph, and the pieces are reversed by level.

#include "pr/text_layout.h"

#include "pr/alloc.h"
#include "pr/array.h"
#include "pr/float.h"
#include "pr/mem.h"
#include "pr/text_itemize.h"
#include "pr/unicode_bidi.h"
#include "pr/unicode_grapheme.h"
#include "pr/unicode_line_break.h"
#include "pr/utf8.h"

namespace pr_impl {
namespace {

// A shaped run from itemization: its bytes, the face that sets it, its level, and its glyphs in TextLayout::shaped. The
// glyphs are in visual order, and their clusters are bytes of the text.
struct ShapedRun {
    uint32_t start;
    uint32_t end;
    uint32_t face;
    uint8_t level;
    uint32_t glyphStart;
    uint32_t glyphEnd;
};

// A code point of the text: its bytes, its advance, whether it is a space that a line may end in, and the break
// opportunity after it.
struct CodePoint {
    uint32_t start;
    uint32_t end;
    float advance;
    bool space;
    PrUnicodeLineBreak after;
};

// A piece of a line: the part of one run in the line, at a level. Placing visits pieces in visual order.
struct Piece {
    uint32_t run;
    uint32_t start;
    uint32_t end;
    uint8_t level;
};

// A placed run: its font, size and glyphs in TextLayout::glyphs, with their scales if any scale is not one.
struct PlacedRun {
    const PrFont* font;
    uint32_t glyphStart;
    uint32_t glyphCount;
    uint32_t textStart;
    bool scaled;
};

struct TextLayout {
    Array<char> text;
    PrTextFont* PR_NULLABLE font;
    PrTextDirection direction;      // the base direction of each paragraph
    float size;
    float width;                    // the width that the lines take
    float height;

    PrTextItems* PR_NULLABLE items;
    PrTextGlyphBuffer* PR_NULLABLE buffer;
    Array<PrTextGlyph> shaped;
    Array<ShapedRun> runs;
    Array<CodePoint> codePoints;
    Array<uint32_t> codePointValues;    // scratch: the code points, for UAX14
    Array<PrUnicodeLineBreak> breaks;   // scratch

    Array<PrTextLine> lines;
    Array<PrFontGlyph> glyphs;
    Array<PrAffine2> transforms;    // by glyph, for runs that are scaled
    Array<PlacedRun> placed;

    // By byte: the left and right edges of the placed glyphs whose cluster starts at the byte, or NaN if no cluster
    // starts there, and the bidi level of the piece that holds the cluster.
    Array<float> clusterLeft;
    Array<float> clusterRight;
    Array<uint8_t> clusterLevel;

    Array<Piece> pieces;            // scratch: the pieces of one line
    Array<uint8_t> pieceLevels;     // scratch
    Array<uint32_t> order;          // scratch
};

bool isLineFeed(const TextLayout& t, uint32_t byte) {
    return t.text.data[byte] == '\n';
}

// Shapes each run that itemization gives into `t.shaped`, and writes the advance of each byte to its code point.
void shapeRuns(TextLayout& t, const PrTextLayoutDesc& d, Array<float>& advance) {
    PrStringView text{t.text.data, t.text.count};
    PrTextPiece piece{};
    piece.kind = PrTextPieceKind_Text;
    piece.end = t.text.count;
    piece.font = d.font;
    PrTextItemizeDesc itemize{};
    itemize.text = text;
    itemize.pieces = &piece;
    itemize.pieceCount = 1;
    itemize.direction = d.direction;
    if (t.items == nullptr)
        t.items = prTextItemsCreate();
    prTextItemsItemize(t.items, &itemize);
    if (t.buffer == nullptr)
        t.buffer = prTextGlyphBufferCreate();
    const PrTextItemRun* runs = prTextItemsRuns(t.items);
    for (uint32_t r = 0; r < prTextItemsRunCount(t.items); ++r) {
        const PrTextItemRun& item = runs[r];
        ShapedRun run{item.start, item.end, item.face, item.level, t.shaped.count, t.shaped.count};
        if (item.face != kPrTextMatchNone) {
            PrTextRun shape{};
            shape.font = prTextFontMatchedFont(d.font, item.face);
            shape.text = PrStringView{text.data + item.start, item.end - item.start};
            shape.before = PrStringView{text.data + item.contextStart, item.start - item.contextStart};
            shape.after = PrStringView{text.data + item.end, item.contextEnd - item.end};
            shape.direction = (item.level & 1) != 0 ? PrTextDirection_Rtl : PrTextDirection_Ltr;
            shape.script = item.script;
            shape.locale = prTextFontLocale(d.font);
            shape.features = d.features;
            shape.featureCount = d.featureCount;
            shape.kerning = d.kerning;
            shape.caps = d.caps;
            shape.position = d.position;
            shape.synthesis = d.synthesis;
            shape.pixelsPerEm = t.size;
            if (prTextShaperShape(d.shaper, &shape, t.buffer) == PrStatus_Ok) {
                const PrTextGlyph* glyphs = prTextGlyphBufferGlyphs(t.buffer);
                for (uint32_t i = 0; i < prTextGlyphBufferGlyphCount(t.buffer); ++i) {
                    PrTextGlyph glyph = glyphs[i];
                    glyph.cluster += item.start;
                    advance.data[glyph.cluster] += glyph.advance.x * t.size;
                    t.shaped.push(glyph);
                }
            }
        }
        run.glyphEnd = t.shaped.count;
        t.runs.push(run);
    }
}

// Divides the text into code points, each with its advance and the opportunity after it (UAX14). A line feed forces a
// break.
void findCodePoints(TextLayout& t, const Array<float>& advance) {
    PrStringView text{t.text.data, t.text.count};
    t.codePoints.clear();
    t.codePointValues.clear();
    for (uint32_t at = 0; at < text.size;) {
        uint32_t value = 0;
        uint32_t size = prUtf8Decode(text, at, &value);
        CodePoint c{at, at + size, 0, value == ' ', PrUnicodeLineBreak_Prohibited};
        for (uint32_t b = c.start; b < c.end; ++b)
            c.advance += advance.data[b];
        t.codePoints.push(c);
        t.codePointValues.push(value);
        at += size;
    }
    t.breaks.resize(t.codePoints.count);
    if (t.codePoints.count != 0)
        prUnicodeLineBreaks(t.codePointValues.data, t.codePoints.count, t.breaks.data);
    for (uint32_t i = 0; i < t.codePoints.count; ++i)
        t.codePoints.data[i].after = t.breaks.data[i];
}

// Breaks the text into lines at the last opportunity before `width`, so that no line is wider than `width` if it is
// above zero.
void breakLines(TextLayout& t, float width) {
    t.lines.clear();
    uint32_t count = t.codePoints.count;
    uint32_t first = 0;
    while (first < count || t.lines.count == 0) {
        float advanced = 0;
        float content = 0;              // the advance up to the last code point that is not a space
        uint32_t candidate = UINT32_MAX;
        uint32_t end = count;
        for (uint32_t i = first; i < count; ++i) {
            const CodePoint& c = t.codePoints.data[i];
            advanced += c.advance;
            if (!c.space)
                content = advanced;
            if (width > 0 && content > width && candidate != UINT32_MAX) {
                end = candidate;
                break;
            }
            if (c.after == PrUnicodeLineBreak_Mandatory && (i + 1 < count || isLineFeed(t, c.start))) {
                end = i + 1;
                break;
            }
            if (c.after == PrUnicodeLineBreak_Allowed)
                candidate = i + 1;
        }
        PrTextLine line{};
        line.start = first < count ? t.codePoints.data[first].start : t.text.count;
        line.end = end < count ? t.codePoints.data[end].start : t.text.count;
        t.lines.push(line);
        if (end >= count) {
            // A text that ends in a line feed has an empty line after it.
            if (count != 0 && isLineFeed(t, t.codePoints.data[count - 1].start) && end == count)
                t.lines.push(PrTextLine{.start = t.text.count, .end = t.text.count});
            break;
        }
        first = end;
    }
}

// Returns the level of the paragraph that holds byte `byte`, as itemization resolved it.
uint8_t paragraphLevel(const TextLayout& t, uint32_t byte, PrTextDirection direction) {
    const PrTextParagraph* paragraphs = prTextItemsParagraphs(t.items);
    for (uint32_t i = 0; i < prTextItemsParagraphCount(t.items); ++i) {
        if (byte < paragraphs[i].end || i + 1 == prTextItemsParagraphCount(t.items))
            return paragraphs[i].level;
    }
    return direction == PrTextDirection_Rtl ? 1 : 0;
}

// Returns the end of the content of a line, which leaves out its line feed and the hanging spaces before it.
uint32_t contentEnd(const TextLayout& t, const PrTextLine& line) {
    uint32_t end = line.end;
    while (end > line.start && (isLineFeed(t, end - 1) || t.text.data[end - 1] == ' '))
        --end;
    return end;
}

// Collects the pieces of `line` in `t.pieces`, in visual order: the part of each run in the line, with the white space
// that the line ends in at the level of its paragraph (L1), reversed by level (L2).
void orderLine(TextLayout& t, const PrTextLine& line, uint32_t content, uint8_t paragraph) {
    Array<Piece> logical;
    for (uint32_t r = 0; r < t.runs.count; ++r) {
        const ShapedRun& run = t.runs.data[r];
        uint32_t start = run.start > line.start ? run.start : line.start;
        uint32_t end = run.end < line.end ? run.end : line.end;
        if (start >= end)
            continue;
        if (start < content)
            logical.push(Piece{r, start, end < content ? end : content, run.level});
        if (end > content)
            logical.push(Piece{r, start > content ? start : content, end, paragraph});
    }
    t.pieceLevels.clear();
    for (uint32_t i = 0; i < logical.count; ++i)
        t.pieceLevels.push(logical.data[i].level);
    t.order.resize(logical.count);
    if (logical.count != 0)
        prUnicodeBidiReorder(t.pieceLevels.data, logical.count, t.order.data);
    t.pieces.clear();
    for (uint32_t i = 0; i < logical.count; ++i)
        t.pieces.push(logical.data[t.order.data[i]]);
    logical.reset();
}

// Returns the advance of the glyphs of `piece`.
float pieceAdvance(const TextLayout& t, const Piece& piece) {
    const ShapedRun& run = t.runs.data[piece.run];
    float advance = 0;
    for (uint32_t g = run.glyphStart; g < run.glyphEnd; ++g) {
        const PrTextGlyph& glyph = t.shaped.data[g];
        if (glyph.cluster >= piece.start && glyph.cluster < piece.end)
            advance += glyph.advance.x * t.size;
    }
    return advance;
}

// Places the pieces of `line` from `left` on its baseline, as runs of glyphs, and records the edges of each cluster.
void placeLine(TextLayout& t, PrTextLine& line, const PrTextLayoutDesc& d, float left) {
    line.left = left;
    line.runStart = t.placed.count;
    float pen = left;
    for (uint32_t p = 0; p < t.pieces.count; ++p) {
        const Piece& piece = t.pieces.data[p];
        const ShapedRun& run = t.runs.data[piece.run];
        if (run.face == kPrTextMatchNone)
            continue;
        PlacedRun placed{prTextFontMatchedFont(d.font, run.face), t.glyphs.count, 0, piece.start, false};
        for (uint32_t g = run.glyphStart; g < run.glyphEnd; ++g) {
            const PrTextGlyph& glyph = t.shaped.data[g];
            if (glyph.cluster < piece.start || glyph.cluster >= piece.end)
                continue;
            t.glyphs.push(PrFontGlyph{
                glyph.id, PrVec2{pen + glyph.offset.x * t.size, line.baseline + glyph.offset.y * t.size},
            });
            t.transforms.push(prAffine2Scale(PrVec2{glyph.scale, glyph.scale}));
            placed.scaled = placed.scaled || glyph.scale != 1;
            float next = pen + glyph.advance.x * t.size;
            float& l = t.clusterLeft.data[glyph.cluster];
            float& r = t.clusterRight.data[glyph.cluster];
            l = isNanF32(l) || pen < l ? pen : l;
            r = isNanF32(r) || next > r ? next : r;
            t.clusterLevel.data[glyph.cluster] = piece.level;
            pen = next;
        }
        placed.glyphCount = t.glyphs.count - placed.glyphStart;
        if (placed.glyphCount != 0)
            t.placed.push(placed);
    }
    line.runCount = t.placed.count - line.runStart;
}

// Returns the line that holds byte `offset`: the last line that starts at or before it.
uint32_t lineOf(const TextLayout& t, uint32_t offset) {
    uint32_t index = 0;
    while (index + 1 < t.lines.count && t.lines.data[index + 1].start <= offset)
        ++index;
    return index;
}

} // namespace
} // namespace pr_impl

using namespace pr_impl;

// C opaque types
struct PrTextLayout : TextLayout {};

PrTextLayout* prTextLayoutCreate(void) {
    return allocZeroed<PrTextLayout>();
}

void prTextLayoutDestroy(PrTextLayout* PR_NULLABLE layout) {
    if (layout == nullptr)
        return;
    prTextFontRelease(layout->font);
    prTextItemsDestroy(layout->items);
    prTextGlyphBufferDestroy(layout->buffer);
    layout->text.reset();
    layout->shaped.reset();
    layout->runs.reset();
    layout->codePoints.reset();
    layout->codePointValues.reset();
    layout->breaks.reset();
    layout->lines.reset();
    layout->glyphs.reset();
    layout->transforms.reset();
    layout->placed.reset();
    layout->clusterLeft.reset();
    layout->clusterRight.reset();
    layout->clusterLevel.reset();
    layout->pieces.reset();
    layout->pieceLevels.reset();
    layout->order.reset();
    prMemoryFree(layout);
}

void prTextLayoutSet(PrTextLayout* layout, const PrTextLayoutDesc* desc) {
    PR_ASSERT(desc->text.size < UINT32_MAX);
    prTextFontRetain(desc->font);
    prTextFontRelease(layout->font);
    layout->font = desc->font;
    layout->direction = desc->direction;
    layout->size = prTextFontStyle(desc->font).size;
    layout->text.clear();
    layout->text.append(desc->text.data, uint32_t(desc->text.size));
    layout->shaped.clear();
    layout->runs.clear();
    layout->glyphs.clear();
    layout->transforms.clear();
    layout->placed.clear();
    uint32_t bytes = layout->text.count;
    Array<float> advance;
    advance.resize(bytes);
    memZero(advance.data, bytes * sizeof(float));
    if (bytes != 0)
        shapeRuns(*layout, *desc, advance);
    findCodePoints(*layout, advance);
    advance.reset();
    breakLines(*layout, desc->width);
    layout->clusterLeft.resize(bytes);
    layout->clusterRight.resize(bytes);
    layout->clusterLevel.resize(bytes);
    for (uint32_t b = 0; b < bytes; ++b) {
        layout->clusterLeft.data[b] = kNanF32;
        layout->clusterRight.data[b] = kNanF32;
        layout->clusterLevel.data[b] = 0;
    }

    // The height of the lines: that of the first available face, or the height given.
    float ascent = layout->size * 0.8f;
    float descent = layout->size * 0.2f;
    float lineGap = 0;
    uint32_t first = prTextFontFirst(desc->font);
    if (first != kPrTextMatchNone) {
        PrFontMetrics metrics = prFontMetrics(prTextFontMatchedFont(desc->font, first));
        ascent = metrics.ascent * layout->size;
        descent = metrics.descent * layout->size;
        lineGap = metrics.lineGap * layout->size;
    }
    float lineHeight = desc->lineHeight > 0 ? desc->lineHeight : ascent + descent + lineGap;

    // Finds the content of each line and its width, then the width that the lines take, and then places each line in
    // that width.
    float widest = 0;
    for (uint32_t i = 0; i < layout->lines.count; ++i) {
        PrTextLine& line = layout->lines.data[i];
        uint8_t paragraph = paragraphLevel(*layout, line.start, desc->direction);
        orderLine(*layout, line, contentEnd(*layout, line), paragraph);
        line.width = 0;
        for (uint32_t p = 0; p < layout->pieces.count; ++p) {
            if (layout->pieces.data[p].end <= contentEnd(*layout, line))
                line.width += pieceAdvance(*layout, layout->pieces.data[p]);
        }
        widest = line.width > widest ? line.width : widest;
    }
    layout->width = desc->width > 0 ? desc->width : widest;
    float top = 0;
    for (uint32_t i = 0; i < layout->lines.count; ++i) {
        PrTextLine& line = layout->lines.data[i];
        uint8_t paragraph = paragraphLevel(*layout, line.start, desc->direction);
        bool rightToLeft = (paragraph & 1) != 0;
        orderLine(*layout, line, contentEnd(*layout, line), paragraph);
        float room = layout->width - line.width;
        float left = 0;
        switch (desc->align) {
            case PrTextAlign_Start: left = rightToLeft ? room : 0; break;
            case PrTextAlign_End: left = rightToLeft ? 0 : room; break;
            case PrTextAlign_Right: left = room; break;
            case PrTextAlign_Center: left = room / 2; break;
            default: break;
        }
        line.top = top;
        line.height = lineHeight;
        line.baseline = top + (lineHeight - ascent - descent) / 2 + ascent;
        placeLine(*layout, line, *desc, left);
        top += lineHeight;
    }
    layout->height = top;
}

PrVec2 prTextLayoutExtent(const PrTextLayout* layout) {
    return PrVec2{layout->width, layout->height};
}

uint32_t prTextLayoutLineCount(const PrTextLayout* layout) {
    return layout->lines.count;
}

PrTextLine prTextLayoutLine(const PrTextLayout* layout, uint32_t index) {
    PR_ASSERT(index < layout->lines.count);
    return layout->lines.data[index];
}

uint32_t prTextLayoutRunCount(const PrTextLayout* layout) {
    return layout->placed.count;
}

PrFontGlyphRun prTextLayoutRun(const PrTextLayout* layout, uint32_t index) {
    PR_ASSERT(index < layout->placed.count);
    const PlacedRun& placed = layout->placed.data[index];
    PrFontGlyphRun run{};
    run.font = placed.font;
    run.size = layout->size;
    run.glyphs = layout->glyphs.data + placed.glyphStart;
    run.glyphCount = placed.glyphCount;
    run.transforms = placed.scaled ? layout->transforms.data + placed.glyphStart : nullptr;
    return run;
}

uint32_t prTextLayoutRunStart(const PrTextLayout* layout, uint32_t run) {
    PR_ASSERT(run < layout->placed.count);
    return layout->placed.data[run].textStart;
}

PrVec2 prTextLayoutCaret(const PrTextLayout* layout, uint32_t offset) {
    const TextLayout& t = *layout;
    if (t.lines.count == 0)
        return PrVec2{0, 0};
    const PrTextLine& line = t.lines.data[lineOf(t, offset)];
    if (offset < t.text.count && !isNanF32(t.clusterLeft.data[offset])) {
        bool rightToLeft = (t.clusterLevel.data[offset] & 1) != 0;
        return PrVec2{rightToLeft ? t.clusterRight.data[offset] : t.clusterLeft.data[offset], line.baseline};
    }
    // The end of the content of a line lies past its last cluster in the direction of its paragraph.
    bool rightToLeft = (paragraphLevel(t, line.start, t.direction) & 1) != 0;
    return PrVec2{rightToLeft ? line.left : line.left + line.width, line.baseline};
}

uint32_t prTextLayoutNextCaret(const PrTextLayout* layout, uint32_t offset) {
    PrStringView text{layout->text.data, layout->text.count};
    return offset >= text.size ? uint32_t(text.size) : uint32_t(prUnicodeGraphemeNext(text, offset));
}

uint32_t prTextLayoutHitTest(const PrTextLayout* layout, PrVec2 point) {
    const TextLayout& t = *layout;
    if (t.lines.count == 0)
        return 0;
    uint32_t index = 0;
    while (index + 1 < t.lines.count && point.y >= t.lines.data[index + 1].top)
        ++index;
    const PrTextLine& line = t.lines.data[index];
    PrStringView text{t.text.data, t.text.count};
    uint32_t end = contentEnd(t, line);
    uint32_t best = end;
    float nearest = kInfinityF32;
    for (uint32_t at = line.start; at < end; at = uint32_t(prUnicodeGraphemeNext(text, at))) {
        if (isNanF32(t.clusterLeft.data[at]))
            continue;
        float left = t.clusterLeft.data[at];
        float right = t.clusterRight.data[at];
        bool rightToLeft = (t.clusterLevel.data[at] & 1) != 0;
        uint32_t next = uint32_t(prUnicodeGraphemeNext(text, at));
        if (point.x >= left && point.x < right) {
            bool leading = rightToLeft ? point.x >= (left + right) / 2 : point.x < (left + right) / 2;
            return leading ? at : next;
        }
        float distance = point.x < left ? left - point.x : point.x - right;
        if (distance < nearest) {
            nearest = distance;
            bool before = (point.x < left) != rightToLeft;
            best = before ? at : next;
        }
    }
    return best;
}
