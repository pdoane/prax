// An inline formatting context's items, described to lib/text, which itemizes and shapes them (pr/text_shaping.h).

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

// Returns how the text of `item` breaks within words: as its font group says, but without the opportunities that
// overflow-wrap and word-break's break-word add if the text does not wrap. Anything else is not text.
PrTextBreakStyle breakStyleOf(const LayoutStyles* styles, const LayoutInlineItem& item) {
    if (item.kind != LayoutInlineItemKind_Text)
        return PrTextBreakStyle{};
    const PrLayoutFontStyle& font = layoutTableAt(styles->font, item.font);
    if (font.textWrapMode == PrLayoutTextWrapMode_Nowrap)
        return PrTextBreakStyle{
            font.wordBreak == PrTextWordBreak_BreakWord ? PrTextWordBreak(PrTextWordBreak_Normal) : font.wordBreak,
            PrTextOverflowWrap_Normal,
        };
    return PrTextBreakStyle{font.wordBreak, font.overflowWrap};
}

// Returns the advance of an upright glyph in `face` of the font group of `piece`. A face with no vertical metrics
// advances each typographic character unit by its ascent and descent as layout rounds them, rather than by the
// unrounded sum that shaping synthesizes (css-writing-modes-4/#vertical-font-features).
float uprightAdvance(void* context, const PrTextPiece* piece, uint32_t face) {
    LayoutStyles* styles = static_cast<LayoutStyles*>(context);
    layoutFontFacesMeasureNew(styles, piece->style);
    const LayoutFaceMetrics& metrics = layoutFontFacesOfGroup(styles, piece->style).metrics.data[face];
    return metrics.vertical ? 0 : prLayoutUnitToPx(metrics.ascent + metrics.descent);
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
    const PrLayoutFontStyle& first = layoutTableAt(styles->font, a);
    const PrLayoutFontStyle& second = layoutTableAt(styles->font, b);
    return layoutTextFont(styles, first) == layoutTextFont(styles, second) &&
        prTextFeatureSetEquals(first.features, second.features) && first.letterSpacing == second.letterSpacing &&
        first.wordSpacing == second.wordSpacing;
}

bool layoutItemize(LayoutStyles* styles, PrTextShaper* PR_NULLABLE shaper, LayoutInlineShaping& t, LayoutShapingScratch& s) {
    const PrLayoutFontStyle& container = layoutTableAt(styles->font, t.containerFont);
    // Converts each item to one piece, so that bidi marks are recorded per item. A float becomes a boundary. A text
    // item takes the font group of the text before it if the two groups have the same shaping style, so that the two
    // shape as one run if nothing between them breaks shaping.
    s.pieces.clear();
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
                piece.size = prLayoutUnitToPx(layoutFontFacesOfGroup(styles, style).size);
                piece.features = layoutTableAt(styles->font, style).features;
                piece.letterSpacing = layoutTableAt(styles->font, style).letterSpacing;
                piece.wordSpacing = layoutTableAt(styles->font, style).wordSpacing;
                piece.breakStyle = breakStyleOf(styles, item);
                piece.noWrap = layoutTableAt(styles->font, item.font).textWrapMode == PrLayoutTextWrapMode_Nowrap;
                piece.breakSpaces = layoutTableAt(styles->font, item.font).whiteSpaceCollapse ==
                    PrLayoutWhiteSpaceCollapse_BreakSpaces;
                break;
            case LayoutInlineItemKind_Open:
            case LayoutInlineItemKind_Close:
                piece.kind = item.kind == LayoutInlineItemKind_Open ? PrTextPieceKind_Open : PrTextPieceKind_Close;
                piece.bidi = item.unicodeBidi;
                piece.direction = directionOf(styles, item);
                piece.breaksShaping = item.breaksShaping;
                piece.noWrap = layoutTableAt(styles->font, item.font).textWrapMode == PrLayoutTextWrapMode_Nowrap;
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
    PrTextShapingDesc desc{
        .text = PrStringView{t.text.data, t.text.count},
        .pieces = s.pieces.data,
        .pieceCount = s.pieces.count,
        .direction = layoutUsedMode(container).direction,
        .bidi = t.containerBidi,
        .shaper = shaper,
        .pixelRatio = styles->targetPixelRatio,
        .spaces = t.spaces.data,
        .tabSize = t.tabSize,
        .tabMinimum = t.tabMinimum,
        .objectBreak = PrTextObjectBreak_Around,
        .noWrap = container.textWrapMode == PrLayoutTextWrapMode_Nowrap,
        .uprightAdvance = uprightAdvance,
        .uprightContext = styles,
    };
    if (t.shaped == nullptr)
        t.shaped = prTextShapingCreate();
    if (s.text == nullptr)
        s.text = prTextShapingScratchCreate();
    prTextShapingSet(t.shaped, &desc, s.text);
    t.info = prTextShapingInfo(t.shaped);

    // Measures the faces that the runs use, and copies the runs in the groups and faces of layout.
    t.runs.clear();
    const PrTextShapedRun* runs = t.info.runs;
    for (uint32_t r = 0; r < t.info.runCount; ++r) {
        const PrTextItemRun& run = runs[r].item;
        PrLayoutGroupIndex font = s.pieces.data[run.piece].style;
        layoutFontFacesMeasureNew(styles, font);
        t.runs.push(LayoutShapingRun{
            .start = run.start,
            .end = run.end,
            .contextStart = run.contextStart,
            .contextEnd = run.contextEnd,
            .glyphStart = runs[r].glyphStart,
            .glyphEnd = runs[r].glyphEnd,
            .font = font,
            .face = run.face != kPrTextMatchNone ? LayoutFaceIndex(run.face) : kLayoutFaceNone,
            .script = run.script,
            .level = run.level,
            .upright = run.upright,
        });
    }
    t.splits.clear();
    t.bidi = t.info.levels != nullptr;
    if (!t.bidi)
        return false;
    layoutBidiFindSplits(t, s);
    return true;
}

} // namespace pr_impl
