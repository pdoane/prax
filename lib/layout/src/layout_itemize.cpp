// Itemization: an inline formatting context's text divided into shaping runs, before it is shaped (pr/text_itemize.h).

#include "layout/src/layout_itemize.h"

#include "pr/mem.h"

#include "layout/src/layout_bidi.h"
#include "layout/src/layout_logical.h"

namespace pr_impl {

namespace {

// Returns the orientation of text styled by `font` on a line of a container styled by `container`.
PrTextOrientation orientationOf(const PrLayoutFontStyle& container, const PrLayoutFontStyle& font) {
    if (!layoutIsVerticalTypographic(LayoutMode{container.writingMode, container.direction}))
        return PrTextOrientation_Horizontal;
    switch (font.textOrientation) {
        case PrLayoutTextOrientation_Upright: return PrTextOrientation_Upright;
        case PrLayoutTextOrientation_Sideways: return PrTextOrientation_Sideways;
        default: return PrTextOrientation_Mixed;
    }
}

// Returns the used direction of the item.
PrTextDirection directionOf(const LayoutStyles* styles, const LayoutInlineItem& item) {
    return layoutUsedMode(layoutTableAt(styles->font, item.font)).direction;
}

} // namespace

bool layoutIsUpright(const PrLayoutFontStyle& container, const PrLayoutFontStyle& font, uint32_t codePoint, PrScript script) {
    return prTextIsUpright(orientationOf(container, font), codePoint, script);
}

bool layoutSameShapingStyle(const LayoutStyles* styles, uint32_t a, uint32_t b) {
    if (a == b)
        return true;
    return layoutTextFont(styles, layoutTableAt(styles->font, a)) == layoutTextFont(styles, layoutTableAt(styles->font, b));
}

bool layoutItemize(LayoutStyles* styles, LayoutInlineShaping& t, LayoutShapingScratch& s) {
    const PrLayoutFontStyle& container = layoutTableAt(styles->font, t.containerFont);
    // Converts each item to one piece, so that bidi marks are recorded per item. A float becomes a boundary. A text
    // item takes the font group of the text before it if the two groups have the same shaping style, so that the two
    // shape as one run if nothing between them breaks shaping.
    s.pieces.clear();
    s.unshaped.resize(t.text.count);
    for (uint32_t b = 0; b < t.text.count; ++b)
        s.unshaped.data[b] = (t.spaces.data[b] & LayoutSpaceFlag_Tab) != 0;
    PrLayoutStyleIndex style = UINT32_MAX;
    for (uint32_t i = 0; i < t.items.count; ++i) {
        const LayoutInlineItem& item = t.items.data[i];
        PrTextPiece piece{};
        piece.start = item.start;
        piece.end = item.end;
        piece.kind = PrTextPieceKind_Boundary;
        switch (item.kind) {
            case LayoutInlineItemKind_Text:
                piece.kind = PrTextPieceKind_Text;
                piece.orientation = orientationOf(container, layoutTableAt(styles->font, item.font));
                if (style == UINT32_MAX || !layoutSameShapingStyle(styles, style, item.font))
                    style = item.font;
                piece.style = style;
                piece.font = layoutFontFacesOfGroup(styles, style).font;
                break;
            case LayoutInlineItemKind_Open:
            case LayoutInlineItemKind_Close:
                piece.kind = item.kind == LayoutInlineItemKind_Open ? PrTextPieceKind_Open : PrTextPieceKind_Close;
                piece.bidi = item.unicodeBidi;
                piece.direction = directionOf(styles, item);
                piece.breaksShaping = item.breaksShaping;
                break;
            case LayoutInlineItemKind_Atomic:
                piece.kind = PrTextPieceKind_Object;
                piece.bidi = item.unicodeBidi;
                piece.direction = directionOf(styles, item);
                break;
            case LayoutInlineItemKind_Break: piece.kind = PrTextPieceKind_Break; break;
            case LayoutInlineItemKind_Block:
                piece.kind = PrTextPieceKind_Break;
                piece.end = piece.start;
                break;
            case LayoutInlineItemKind_OutOfFlow: piece.kind = PrTextPieceKind_Anchor; break;
            default: break;
        }
        s.pieces.push(piece);
    }
    PrTextItemizeDesc desc{};
    desc.text = PrStringView{t.text.data, t.text.count};
    desc.pieces = s.pieces.data;
    desc.pieceCount = s.pieces.count;
    desc.direction = layoutUsedMode(container).direction;
    desc.bidi = t.containerBidi;
    desc.unshaped = s.unshaped.data;
    if (s.items == nullptr)
        s.items = prTextItemsCreate();
    prTextItemsItemize(s.items, &desc);

    // Measures the faces that the runs use, then copies the runs, the levels, the paragraphs and the marks.
    t.runs.clear();
    const PrTextItemRun* runs = prTextItemsRuns(s.items);
    for (uint32_t r = 0; r < prTextItemsRunCount(s.items); ++r) {
        const PrTextItemRun& run = runs[r];
        PrLayoutGroupIndex font = s.pieces.data[run.piece].style;
        layoutFontFacesMeasureNew(styles, font);
        t.runs.push(LayoutShapingRun{
            .start = run.start,
            .end = run.end,
            .contextStart = run.contextStart,
            .contextEnd = run.contextEnd,
            .font = font,
            .face = run.face != kPrTextMatchNone ? LayoutFaceIndex(run.face) : kLayoutFaceNone,
            .script = run.script,
            .level = run.level,
            .upright = run.upright,
        });
    }
    t.bidi = false;
    t.levels.clear();
    t.paragraphs.clear();
    t.marks.clear();
    t.splits.clear();
    const uint8_t* levels = prTextItemsLevels(s.items);
    if (levels == nullptr)
        return false;
    t.bidi = true;
    t.levels.append(levels, t.text.count);
    t.paragraphs.append(prTextItemsParagraphs(s.items), prTextItemsParagraphCount(s.items));
    t.marks.append(prTextItemsMarks(s.items), t.items.count);
    layoutBidiFindSplits(t, s);
    return true;
}

} // namespace pr_impl
