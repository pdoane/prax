// The parts that the gallery's sections share: section and panel frames, cards, captions and grids.

#include "examples/studio_gallery/gallery_parts.h"

#include "pr/kit_button.h"

namespace gallery {

namespace {

constexpr PrKitTextStyle kMonoStrongFaint = {.role = PrKitTextRole_MonoStrong, .tone = PrKitTone_Faint};

// The width of a section's description, in px.
constexpr float kMeasure = 600;

} // namespace

PrLayoutNode card(PrKit* kit, PrLayoutNode parent, const PrKitBoxStyle& box) {
    PrKitPanelStyle look{.box = box, .background = PrKitBackground_Panel, .rules = PrKitRuleFlag_All};
    return prKitPanel(kit, parent, {}, look);
}

PrLayoutNode fillLabel(PrKit* kit, PrLayoutNode parent, PrStringView text, const PrKitTextStyle& style) {
    PrLayoutNode label = prKitLabel(kit, parent, {.text = text}, style);
    PrKitPlace fill{.flex = PrKitFlex_Fill};
    prKitPlace(kit, label, &fill);
    return label;
}

PrLayoutNode caption(PrKit* kit, PrLayoutNode parent, PrStringView text) {
    return prKitLabel(kit, parent, {.text = text}, kCaption);
}

PrLayoutNode captionRow(PrKit* kit, PrLayoutNode parent, PrStringView text, float labelWidth) {
    PrKitBoxStyle row{.direction = PrKitDirection_Row, .align = PrKitAlign_Center, .gap = kPrKitSpace4};
    PrLayoutNode line = prKitBox(kit, parent, {}, row);
    PrKitBoxStyle label{.direction = PrKitDirection_Row};
    label.place.width = prKitPx(labelWidth);
    label.place.flex = PrKitFlex_Fixed;
    caption(kit, prKitBox(kit, line, {}, label), text);
    return line;
}

PrLayoutNode sectionFrame(PrKit* kit, PrLayoutNode parent, PrStringView number, PrStringView title, PrStringView lede,
        PrStringView description) {
    PrLayoutNode section = prKitBox(kit, parent, {}, {.gap = kPrKitSpace3});
    PrKitPanelStyle header{.rules = PrKitRuleFlag_Bottom};
    header.box.direction = PrKitDirection_Row;
    header.box.align = PrKitAlign_Baseline;
    header.box.gap = kPrKitSpace4;
    header.box.padding = PrKitEdges{0, 0, kPrKitSpace6, 0};
    header.box.place.flex = PrKitFlex_Fixed;
    PrLayoutNode head = prKitPanel(kit, section, {}, header);
    prKitLabel(kit, head, {.text = number}, kMonoStrongFaint);
    prKitLabel(kit, head, {.text = title}, kTitle);
    if (lede.size != 0)
        fillLabel(kit, head, lede, kLede);
    if (description.size != 0) {
        // A column of a fixed measure keeps the paragraph from setting the page's width.
        PrKitBoxStyle line{.padding = PrKitEdges{kPrKitSpace3, 0, kPrKitSpace5, 0}};
        line.place.maxWidth = prKitPx(kMeasure);
        prKitLabel(kit, prKitBox(kit, section, {}, line), {.text = description}, kBodySecondary);
    }
    return section;
}

PrLayoutNode demoPanel(PrKit* kit, PrLayoutNode parent, PrStringView title, PrStringView tags, bool bare) {
    PrLayoutNode frame = card(kit, parent, PrKitBoxStyle{});
    PrKitPanelStyle header{.background = PrKitBackground_Header, .rules = PrKitRuleFlag_Bottom};
    header.box.direction = PrKitDirection_Row;
    header.box.align = PrKitAlign_Center;
    header.box.gap = kPrKitSpace3;
    header.box.padding = prKitEdgesXY(0, kPrKitSpace5);
    header.box.place.height = prKitPx(kPrKitHeightSm);
    header.box.place.flex = PrKitFlex_Fixed;
    PrLayoutNode head = prKitPanel(kit, frame, {}, header);
    prKitLabel(kit, head, {.text = title}, kCaption);
    if (tags.size != 0) {
        prKitSpacer(kit, head, {});
        prKitBadge(kit, head, {.text = tags}, {.variant = PrKitBadgeVariant_Neutral});
    }
    PrKitBoxStyle body{.gap = bare ? 0 : kPrKitSpace3, .padding = prKitEdgesAll(bare ? 0 : kPrKitSpace5)};
    return prKitBox(kit, frame, {}, body);
}

PrLayoutNode segmented(PrKit* kit, PrLayoutNode parent, const PrKitChoice* items, uint32_t count, uint32_t* selected,
        PrKitChoiceStyle style) {
    PrLayoutNode control = prKitSegmented(kit, parent, {}, style);
    for (uint32_t i = 0; i < count; ++i) {
        PrKitChoice item = items[i];
        item.selected = *selected == i;
        if (prKitActivated(kit, prKitSegment(kit, control, item)))
            *selected = i;
    }
    return control;
}

Grid gridBegin(PrKit* kit, PrLayoutNode parent, uint32_t columns, float gap, bool ruled) {
    return Grid{kit, parent, kPrLayoutNodeNone, columns > 0 ? columns : 1, 0, ruled ? 0 : gap, ruled};
}

PrLayoutNode gridCell(Grid& grid) {
    if (grid.count % grid.columns == 0) {
        PrKitBoxStyle row{.direction = PrKitDirection_Row, .align = PrKitAlign_Stretch, .gap = grid.gap};
        grid.row = prKitBox(grid.kit, grid.parent, {}, row);
    }
    ++grid.count;
    PrKitPanelStyle cell{.rules = PrKitRuleFlags(grid.ruled ? PrKitRuleFlag_Right | PrKitRuleFlag_Bottom : 0)};
    cell.box.place.flex = PrKitFlex_Fill;
    return prKitPanel(grid.kit, grid.row, {}, cell);
}

void gridEnd(Grid& grid) {
    while (grid.count % grid.columns != 0)
        gridCell(grid);
}

} // namespace gallery
