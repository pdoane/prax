// Studio Kit's readouts: stat tiles, and the lines of a console's log.

#include "pr/kit_data.h"

#include "pr/assert.h"

#include "studio/kit/src/kit.h"

namespace pr_impl {

namespace {

// The width in px of a log line's time and of its tag.
constexpr float kTimeWidth = 60;
constexpr float kTagWidth = 32;

// Returns the tone that a log line's tag is set in.
PrKitTone levelTone(PrKitLogLevel level) {
    if (level == PrKitLogLevel_Error)
        return PrKitTone_Danger;
    return level == PrKitLogLevel_Warning ? PrKitTone_Warning : PrKitTone_Accent;
}

// Returns the font of a readout's text: `role`, on one line.
KitFont lineFont(PrKitTextRole role) {
    return KitFont{.role = role, .wrap = PrKitWrap_Nowrap};
}

// A stat tile: a padded, bordered column of its label, value and note.
struct TileBox {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Box;
    void make(const PrKit&, PrLayoutBoxStyle& box) const {
        box.display = PrLayoutDisplay_Flex;
        kitSetPadding(box, prKitEdgesXY(kPrKitSpace4, kPrKitSpace5));
        kitSetBorderWidth(box, 1);
    }
};
struct TileFlex {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Flex;
    void make(const PrKit&, PrLayoutFlexStyle& flex) const {
        flex.direction = PrLayoutFlexDirection_Column;
        flex.rowGap = prLayoutLengthPx(kPrKitSpace2);
        flex.shrink = 0;
    }
};

// A log line: a row at least an extra small control tall, with a rule below it.
struct LineBox {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Box;
    void make(const PrKit&, PrLayoutBoxStyle& box) const {
        box.display = PrLayoutDisplay_Flex;
        box.boxSizing = PrLayoutBoxSizing_BorderBox;
        box.minHeight = prLayoutLengthPx(kPrKitHeightXs);
        kitSetPadding(box, prKitEdgesXY(kPrKitSpace1, kPrKitSpace4));
        box.border[PrRectSide_Bottom] = prLayoutUnitFromPx(1);
    }
};
struct LineFlex {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Flex;
    void make(const PrKit&, PrLayoutFlexStyle& flex) const {
        flex.alignItems = PrLayoutAlignment_Center;
        flex.columnGap = prLayoutLengthPx(kPrKitSpace3);
        flex.shrink = 0;
    }
};

// A column of a log line of `width` px, which clips its text.
PR_NO_PADDING_BEGIN
struct ColumnBox {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Box;
    float width;

    void make(const PrKit&, PrLayoutBoxStyle& box) const {
        box.display = PrLayoutDisplay_Block;
        box.width = prLayoutLengthPx(width);
        box.overflowX = box.overflowY = PrLayoutOverflow_Clip;
    }
};
PR_NO_PADDING_END

// A log line's message, which takes the room the line leaves and clips its text.
struct MessageBox {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Box;
    void make(const PrKit&, PrLayoutBoxStyle& box) const {
        box.display = PrLayoutDisplay_Block;
        box.minWidth = prLayoutLengthPx(0);
        box.overflowX = box.overflowY = PrLayoutOverflow_Clip;
    }
};

// Creates a node of `record` under `parent`, with `text` in it if it is not empty.
PrLayoutNode part(PrKit& kit, PrLayoutNode parent, uint64_t key, PrLayoutStyleIndex record, PrStringView text) {
    PrLayoutNode node = prLayoutTreeFrameElement(kit.tree, parent, key, record);
    if (text.size != 0)
        prLayoutTreeFrameText(kit.tree, node, 0, text);
    return node;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrLayoutNode prKitStatTile(PrKit* kit, PrLayoutNode parent, PrKitStatTile tile) {
    PrKit& k = *kit;
    const PrKitPalette& p = k.theme.palette;
    PrLayoutStyleIndex tileRecord = kitStyle(k, TileBox{}, TileFlex{}, KitColors{.background = p.bg1, .border = p.line1});
    PrLayoutNode card = part(k, parent, tile.key, tileRecord, PrStringView{});
    prUiSetFlags(k.ui, card, PrUiFlag_SubtreeTransparent);
    part(k, card, 0, kitStyle(k, KitBlock{}, lineFont(PrKitTextRole_Caption), KitToneInk{.tone = PrKitTone_Muted}),
        tile.label);
    part(k, card, 0, kitStyle(k, KitBlock{}, lineFont(PrKitTextRole_MonoNum), KitToneInk{.tone = PrKitTone_Primary}),
        tile.value);
    if (tile.note.size != 0) {
        PrKitTone tone = tile.noteTone == PrKitTone_Auto ? PrKitTone(PrKitTone_Muted) : tile.noteTone;
        part(k, card, 0, kitStyle(k, KitBlock{}, lineFont(PrKitTextRole_Meta), KitToneInk{.tone = tone}), tile.note);
    }
    return card;
}

PrLayoutNode prKitLogLine(PrKit* kit, PrLayoutNode parent, PrKitLogLine line) {
    PR_ASSERT(line.level <= PrKitLogLevel_Error);
    static const PrStringView kTags[] = {"INFO", "WARN", "ERROR"};
    PrKit& k = *kit;
    const PrKitPalette& p = k.theme.palette;
    // A warning or an error washes its line in its tone.
    PrColor wash{0, 0, 0, 0};
    if (line.level != PrKitLogLevel_Info) {
        wash = line.level == PrKitLogLevel_Error ? p.danger : p.warning;
        wash.a = line.level == PrKitLogLevel_Error ? 0.14f : 0.10f;
    }
    PrLayoutStyleIndex lineRecord = kitStyle(k, LineBox{}, LineFlex{}, KitColors{.background = wash, .border = p.line1});
    PrLayoutNode row = part(k, parent, line.key, lineRecord, PrStringView{});
    if (line.time.size != 0) {
        part(k, row, 0, kitStyle(k, ColumnBox{.width = kTimeWidth}, KitFixed{}, lineFont(PrKitTextRole_Meta),
            KitToneInk{.tone = PrKitTone_Faint}), line.time);
    }
    part(k, row, 0, kitStyle(k, ColumnBox{.width = kTagWidth}, KitFixed{}, lineFont(PrKitTextRole_Caption),
        KitToneInk{.tone = levelTone(line.level)}), kTags[line.level]);
    part(k, row, 0,
        kitStyle(k, MessageBox{}, KitGrow{}, lineFont(PrKitTextRole_Mono), KitToneInk{.tone = PrKitTone_Primary}),
        line.message);
    if (line.source.size != 0) {
        part(k, row, 0,
            kitStyle(k, KitBlock{}, KitFixed{}, lineFont(PrKitTextRole_Meta), KitToneInk{.tone = PrKitTone_Faint}),
            line.source);
    }
    return row;
}
