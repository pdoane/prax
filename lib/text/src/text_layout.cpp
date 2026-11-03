// Text layouts: a host of a shaping (pr/text_shaping.h) that sets its text in one width, and places what it finds.

#include "pr/text_layout.h"

#include "pr/alloc.h"
#include "pr/float.h"
#include "pr/mem.h"
#include "pr/memory.h"
#include "pr/raw_array.h"
#include "pr/text_shaping.h"
#include "pr/unicode_grapheme.h"

namespace pr_impl {
namespace {

// A placed run: its font, size and glyphs in TextLayout::glyphs, with their scales if any scale is not one.
struct PlacedRun {
    const PrFont* font;
    uint32_t glyphStart;
    uint32_t glyphCount;
    uint32_t textStart;
    bool scaled;
};

struct TextLayout {
    RawArray<char> text;
    PrTextFont* PR_NULLABLE font;
    PrTextShaping* PR_NULLABLE shaping;
    PrTextShapingScratch* PR_NULLABLE scratch;
    float size;
    float width;                    // the width that the lines take
    float height;

    RawArray<PrTextLine> lines;
    RawArray<PrFontGlyph> glyphs;
    RawArray<PrAffine2> transforms;    // by glyph, for runs that are scaled
    RawArray<PlacedRun> placed;

    // By byte: the left and right edges of the placed glyphs whose cluster starts at the byte, or NaN if no cluster
    // starts there, and the bidi level of the part that holds the cluster.
    RawArray<float> clusterLeft;
    RawArray<float> clusterRight;
    RawArray<uint8_t> clusterLevel;

    RawArray<PrTextLineRun> parts;     // scratch: the parts of one line, in visual order
};

bool isLineFeed(const TextLayout& t, uint32_t byte) {
    return t.text.data[byte] == '\n';
}

// Breaks the text into lines in `width`, or only at line feeds if `width` is zero or below. A text that ends in a line
// feed has an empty line after it.
void breakLines(TextLayout& t, float width) {
    t.lines.clear();
    PrStringView text{t.text.data, t.text.count};
    for (uint32_t first = 0;;) {
        PrTextShapingLine line{.start = first, .end = t.text.count};
        uint32_t end = prTextShapingBreakLine(t.shaping, text, &line, width > 0 ? width : kPrF32Infinity, nullptr);
        t.lines.push(PrTextLine{.start = first, .end = end});
        if (end >= t.text.count) {
            if (end != 0 && isLineFeed(t, end - 1))
                t.lines.push(PrTextLine{.start = end, .end = end});
            return;
        }
        first = end;
    }
}

// Returns the end of the content of a line, which leaves out its line feed and the hanging spaces before it.
uint32_t contentEnd(const TextLayout& t, const PrTextLine& line) {
    uint32_t end = line.end;
    while (end > line.start && (isLineFeed(t, end - 1) || t.text.data[end - 1] == ' '))
        --end;
    return end;
}

// Fills `t.parts` with the parts of `line`, in visual order.
void orderLine(TextLayout& t, const PrTextLine& line) {
    uint32_t content = contentEnd(t, line);
    uint32_t count = prTextShapingLineRuns(t.shaping, line.start, line.end, content, nullptr, 0);
    t.parts.resize(count);
    prTextShapingLineRuns(t.shaping, line.start, line.end, content, t.parts.data, count);
}

// Places the parts of `line` from `left` on its baseline, as runs of glyphs, and records the edges of each cluster.
void placeLine(TextLayout& t, PrTextLine& line, const PrTextLayoutDesc& d, float left) {
    line.left = left;
    line.runStart = t.placed.count;
    float pen = left;
    PrTextShapingInfo info = prTextShapingInfo(t.shaping);
    const PrTextShapedRun* runs = info.runs;
    const PrTextShapedGlyph* glyphs = info.glyphs;
    const uint32_t* glyphAt = info.glyphAt;
    for (uint32_t p = 0; p < t.parts.count; ++p) {
        const PrTextLineRun& part = t.parts.data[p];
        const PrTextItemRun& run = runs[part.run].item;
        if (run.face == kPrTextMatchNone)
            continue;
        PlacedRun placed{prTextFontMatchedFont(d.font, run.face), t.glyphs.count, 0, part.start, false};
        // A right-to-left run's glyphs are in the order of the text, and are drawn from its last.
        uint32_t first = glyphAt[part.start];
        uint32_t count = glyphAt[part.end] - first;
        bool rightToLeft = (run.level & 1) != 0;
        for (uint32_t k = 0; k < count; ++k) {
            const PrTextShapedGlyph& glyph = glyphs[rightToLeft ? first + count - 1 - k : first + k];
            t.glyphs.push(PrFontGlyph{glyph.id, PrVec2{pen + glyph.offset.x, line.baseline + glyph.offset.y}});
            t.transforms.push(prAffine2Scale(PrVec2{glyph.scale, glyph.scale}));
            placed.scaled = placed.scaled || glyph.scale != 1;
            float next = pen + glyph.advance;
            float& l = t.clusterLeft.data[glyph.cluster];
            float& r = t.clusterRight.data[glyph.cluster];
            l = prF32IsNan(l) || pen < l ? pen : l;
            r = prF32IsNan(r) || next > r ? next : r;
            t.clusterLevel.data[glyph.cluster] = part.level;
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
    prTextShapingDestroy(layout->shaping);
    prTextShapingScratchDestroy(layout->scratch);
    layout->text.reset();
    layout->lines.reset();
    layout->glyphs.reset();
    layout->transforms.reset();
    layout->placed.reset();
    layout->clusterLeft.reset();
    layout->clusterRight.reset();
    layout->clusterLevel.reset();
    layout->parts.reset();
    prMemoryFree(layout);
}

void prTextLayoutSet(PrTextLayout* layout, const PrTextLayoutDesc* desc) {
    PR_ASSERT(desc->text.size < UINT32_MAX);
    prTextFontRetain(desc->font);
    prTextFontRelease(layout->font);
    layout->font = desc->font;
    layout->size = prTextFontStyle(desc->font).size;
    layout->text.clear();
    layout->text.append(desc->text.data, uint32_t(desc->text.size));
    layout->glyphs.clear();
    layout->transforms.clear();
    layout->placed.clear();
    uint32_t bytes = layout->text.count;
    PrStringView text{layout->text.data, bytes};
    PrTextFeatureSetDesc featureDesc{
        .caps = desc->caps,
        .position = desc->position,
        .kerning = desc->kerning,
        .synthesis = desc->synthesis,
        .features = desc->features,
        .featureCount = desc->featureCount,
    };
    PrTextFeatureSet* features = prTextFeatureSetCreate(&featureDesc);
    PrTextPiece piece{.kind = PrTextPieceKind_Text, .end = bytes, .font = desc->font, .features = features};
    PrTextShapingDesc shapingDesc{
        .text = text,
        .pieces = &piece,
        .pieceCount = 1,
        .direction = desc->direction,
        .shaper = desc->shaper,
    };
    if (layout->shaping == nullptr) {
        layout->shaping = prTextShapingCreate();
        layout->scratch = prTextShapingScratchCreate();
    }
    prTextShapingSet(layout->shaping, &shapingDesc, layout->scratch);
    prTextFeatureSetRelease(features);
    breakLines(*layout, desc->width);
    layout->clusterLeft.resize(bytes);
    layout->clusterRight.resize(bytes);
    layout->clusterLevel.resize(bytes);
    for (uint32_t b = 0; b < bytes; ++b) {
        layout->clusterLeft.data[b] = kPrF32Nan;
        layout->clusterRight.data[b] = kPrF32Nan;
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
        PrTextShapingLine content{.start = line.start, .end = contentEnd(*layout, line)};
        line.width = prTextShapingLineWidth(layout->shaping, &content);
        widest = line.width > widest ? line.width : widest;
    }
    layout->width = desc->width > 0 ? desc->width : widest;
    float top = 0;
    for (uint32_t i = 0; i < layout->lines.count; ++i) {
        PrTextLine& line = layout->lines.data[i];
        bool rightToLeft = (prTextShapingParagraphLevel(layout->shaping, line.start) & 1) != 0;
        orderLine(*layout, line);
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
    if (offset < t.text.count && !prF32IsNan(t.clusterLeft.data[offset])) {
        bool rightToLeft = (t.clusterLevel.data[offset] & 1) != 0;
        return PrVec2{rightToLeft ? t.clusterRight.data[offset] : t.clusterLeft.data[offset], line.baseline};
    }
    // The end of the content of a line lies past its last cluster in the direction of its paragraph.
    bool rightToLeft = (prTextShapingParagraphLevel(t.shaping, line.start) & 1) != 0;
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
    float nearest = kPrF32Infinity;
    for (uint32_t at = line.start; at < end; at = uint32_t(prUnicodeGraphemeNext(text, at))) {
        if (prF32IsNan(t.clusterLeft.data[at]))
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
