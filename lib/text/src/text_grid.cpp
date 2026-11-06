// Text grids: a grid shapes its text with a text shaping (pr/text_shaping.h), and places each cluster's glyphs in the
// columns of its cell.

#include "pr/text_grid.h"

#include "pr/alloc.h"
#include "pr/assert.h"
#include "pr/float.h"
#include "pr/locale_common.h"
#include "pr/memory.h"
#include "pr/raw_array.h"
#include "pr/text_shaping.h"

namespace pr_impl {

namespace {

// A placed run: its font, its size, and its glyphs in TextGrid::glyphs. `scaled` is true if any glyph's scale is not
// one, and only then does the run give its transforms.
struct GridRun {
    const PrFont* font;
    float size;
    uint32_t glyphStart;
    uint32_t glyphCount;
    uint32_t group;
    bool scaled;
};

struct TextGrid {
    PrTextShaping* PR_NULLABLE shaping;
    PrTextShapingScratch* PR_NULLABLE scratch;
    RawArray<PrTextPiece> pieces;
    RawArray<uint32_t> cellOf;          // by byte: the cell whose cluster holds it
    RawArray<PrFontGlyph> glyphs;
    RawArray<PrAffine2> transforms;     // by glyph
    RawArray<GridRun> runs;
};

// Returns `value` in px, rounded to a whole number of target pixels, and at least one target pixel if `atLeastOne`.
float snap(float value, float ratio, bool atLeastOne) {
    float pixels = prF32Round(value * ratio);
    if (atLeastOne && pixels < 1)
        pixels = 1;
    return pixels / ratio;
}

// The height of the middle of the font's line above its baseline, in px at the em size `size`.
float middleHeight(const PrFont* font, float size) {
    PrFontMetrics metrics = prFontMetrics(font);
    return (metrics.ascent - metrics.descent) / 2 * size;
}

// Writes the left edge and the width, in px, of the columns that cells `first` up to `last`, exclusive, span.
void cellSpan(const PrTextGridDesc& d, uint32_t first, uint32_t last, float* left, float* width) {
    const PrTextGridCell& start = d.cells[first];
    const PrTextGridCell& end = d.cells[last - 1];
    *left = float(start.column) * d.cellWidth;
    *width = float(end.column + end.columns - start.column) * d.cellWidth;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

// C opaque types
struct PrTextGrid : TextGrid {};

PrTextGridMetrics prTextGridMetrics(PrTextFont* font, float pixelRatio, float lineHeight) {
    PR_ASSERT(pixelRatio > 0);
    float size = prTextFontStyle(font).size;
    float ascent = 0.8f;
    float descent = 0.2f;
    float lineGap = 0;
    PrFontMetrics metrics{};
    metrics.underlinePosition = 0.1f;
    metrics.underlineThickness = 0.05f;
    metrics.strikeoutPosition = -0.3f;
    metrics.strikeoutThickness = 0.05f;
    uint32_t first = prTextFontFirst(font);
    if (first != kPrTextMatchNone) {
        metrics = prFontMetrics(prTextFontMatchedFont(font, first));
        ascent = metrics.ascent;
        descent = metrics.descent;
        lineGap = metrics.lineGap;
    }
    float advance = 0.6f;
    if (!prTextFontAdvance(font, '0', PrScript_Latn, PrTextAxis_Horizontal, &advance) || advance <= 0)
        advance = 0.6f;

    PrTextGridMetrics out{};
    out.cell.x = snap(advance * size, pixelRatio, true);
    out.cell.y = snap((ascent + descent + lineGap) * size * (lineHeight > 0 ? lineHeight : 1), pixelRatio, true);
    // Center the glyphs' box, from the ascent to the descent, in the cell, and put its baseline on a target pixel.
    float glyphs = (ascent + descent) * size;
    out.baseline = snap((out.cell.y - glyphs) / 2 + ascent * size, pixelRatio, false);
    out.underlineThickness = snap(metrics.underlineThickness * size, pixelRatio, true);
    out.underlineTop = snap(out.baseline + metrics.underlinePosition * size - out.underlineThickness / 2, pixelRatio, false);
    float lowest = out.cell.y - out.underlineThickness;
    out.underlineTop = out.underlineTop < lowest ? out.underlineTop : lowest;
    out.strikeoutThickness = snap(metrics.strikeoutThickness * size, pixelRatio, true);
    out.strikeoutTop = snap(out.baseline + metrics.strikeoutPosition * size - out.strikeoutThickness / 2, pixelRatio, false);
    return out;
}

PrTextGrid* prTextGridCreate(void) {
    return allocZeroed<PrTextGrid>();
}

void prTextGridDestroy(PrTextGrid* PR_NULLABLE grid) {
    if (grid == nullptr)
        return;
    prTextShapingDestroy(grid->shaping);
    prTextShapingScratchDestroy(grid->scratch);
    grid->pieces.reset();
    grid->cellOf.reset();
    grid->glyphs.reset();
    grid->transforms.reset();
    grid->runs.reset();
    prMemoryFree(grid);
}

void prTextGridSet(PrTextGrid* grid, const PrTextGridDesc* desc) {
    PR_ASSERT(desc->text.size < UINT32_MAX);
    PR_ASSERT(desc->pixelRatio > 0);
    const PrTextGridDesc& d = *desc;
    grid->pieces.clear();
    grid->glyphs.clear();
    grid->transforms.clear();
    grid->runs.clear();
    uint32_t bytes = uint32_t(d.text.size);
    if (d.cellCount == 0 || bytes == 0)
        return;
    PR_ASSERT(d.cells[0].start == 0);

    // A piece for each run of cells of one font and group, and the cell of each byte.
    grid->cellOf.resize(bytes);
    for (uint32_t i = 0; i < d.cellCount; ++i) {
        const PrTextGridCell& cell = d.cells[i];
        PR_ASSERT(cell.font < d.fontCount && cell.columns > 0);
        uint32_t end = i + 1 < d.cellCount ? d.cells[i + 1].start : bytes;
        PR_ASSERT(cell.start <= end && end <= bytes);
        for (uint32_t b = cell.start; b < end; ++b)
            grid->cellOf.data[b] = i;
        bool joins = grid->pieces.count != 0 && i > 0 && d.cells[i - 1].font == cell.font &&
            d.cells[i - 1].group == cell.group;
        if (joins) {
            grid->pieces.last().end = end;
            continue;
        }
        grid->pieces.push(PrTextPiece{
            .kind = PrTextPieceKind_Text,
            .start = cell.start,
            .end = end,
            .font = d.fonts[cell.font],
            .style = grid->pieces.count,
        });
    }

    // Shape the cells left to right as one bidirectional override, so that no cell moves.
    PrTextShapingDesc shapingDesc{
        .text = d.text,
        .pieces = grid->pieces.data,
        .pieceCount = grid->pieces.count,
        .direction = PrTextDirection_Ltr,
        .bidi = PrTextBidi_BidiOverride,
        .shaper = d.shaper,
        .pixelRatio = d.pixelRatio,
    };
    if (grid->shaping == nullptr) {
        grid->shaping = prTextShapingCreate();
        grid->scratch = prTextShapingScratchCreate();
    }
    prTextShapingSet(grid->shaping, &shapingDesc, grid->scratch);

    PrTextShapingInfo info = prTextShapingInfo(grid->shaping);
    for (uint32_t r = 0; r < info.runCount; ++r) {
        const PrTextShapedRun& shaped = info.runs[r];
        const PrTextItemRun& item = shaped.item;
        if (item.face == kPrTextMatchNone || shaped.glyphStart == shaped.glyphEnd)
            continue;
        const PrTextPiece& piece = grid->pieces.data[item.piece];
        GridRun run{
            .font = prTextFontMatchedFont(piece.font, item.face),
            .size = prTextFontStyle(piece.font).size,
            .glyphStart = grid->glyphs.count,
            .group = d.cells[grid->cellOf.data[item.start]].group,
        };
        // The glyphs of a cluster, with those of any clusters that a ligature joins to it, are centered in the cells
        // from this cluster's cell to the next cluster's. Glyphs wider than those cells scale down to fit. A scaled
        // glyph moves so that the middle of its face's line sits at the middle of the line of the text font's first
        // available face, which the cells center (prTextGridMetrics). The square icons of a symbol font therefore fit
        // and center in cells of one column.
        uint32_t first = prTextFontFirst(piece.font);
        const PrFont* lineFont = first != kPrTextMatchNone ? prTextFontMatchedFont(piece.font, first) : run.font;
        float lineMiddle = middleHeight(lineFont, run.size);
        float faceMiddle = middleHeight(run.font, run.size);
        uint32_t g = shaped.glyphStart;
        while (g < shaped.glyphEnd) {
            uint32_t cluster = info.glyphs[g].cluster;
            uint32_t next = g;
            float advance = 0;
            while (next < shaped.glyphEnd && info.glyphs[next].cluster == cluster)
                advance += info.glyphs[next++].advance;
            uint32_t firstCell = grid->cellOf.data[cluster];
            uint32_t lastCell = firstCell + 1;
            if (next < shaped.glyphEnd)
                lastCell = grid->cellOf.data[info.glyphs[next].cluster];
            else if (item.end < bytes)
                lastCell = grid->cellOf.data[item.end];
            else
                lastCell = d.cellCount;
            if (lastCell <= firstCell)
                lastCell = firstCell + 1;
            float left = 0;
            float width = 0;
            cellSpan(d, firstCell, lastCell, &left, &width);
            float fit = advance > width ? width / advance : 1;
            float pen = left + (width - advance * fit) / 2;
            float baseline = fit < 1 ? d.baseline - lineMiddle + faceMiddle * fit : d.baseline;
            for (; g < next; ++g) {
                const PrTextShapedGlyph& glyph = info.glyphs[g];
                PrVec2 origin{pen + glyph.offset.x * fit, baseline + glyph.offset.y * fit};
                float scale = glyph.scale * fit;
                grid->glyphs.push(PrFontGlyph{glyph.id, origin});
                grid->transforms.push(prAffine2Scale(PrVec2{scale, scale}));
                run.scaled = run.scaled || scale != 1;
                pen += glyph.advance * fit;
            }
        }
        run.glyphCount = grid->glyphs.count - run.glyphStart;
        grid->runs.push(run);
    }
}

uint32_t prTextGridRunCount(const PrTextGrid* grid) {
    return grid->runs.count;
}

PrFontGlyphRun prTextGridRun(const PrTextGrid* grid, uint32_t index) {
    PR_ASSERT(index < grid->runs.count);
    const GridRun& run = grid->runs.data[index];
    PrFontGlyphRun out{};
    out.font = run.font;
    out.size = run.size;
    out.glyphs = grid->glyphs.data + run.glyphStart;
    out.glyphCount = run.glyphCount;
    out.transforms = run.scaled ? grid->transforms.data + run.glyphStart : nullptr;
    return out;
}

uint32_t prTextGridRunGroup(const PrTextGrid* grid, uint32_t index) {
    PR_ASSERT(index < grid->runs.count);
    return grid->runs.data[index].group;
}
