// Studio Kit's virtualized lists, their selection, and trees.

#include "pr/kit_collection.h"

#include "pr/assert.h"
#include "pr/kit_box.h"
#include "pr/kit_image.h"
#include "pr/layout_query.h"
#include "pr/layout_unit.h"

#include "studio/kit/src/kit.h"

namespace pr_impl {

namespace {

// How long a tile's image takes to grow or settle as the pointer enters or leaves the tile.
constexpr double kTileZoomSeconds = 0.12;

// The number of rows that a window declares past each edge of the scrollport, so that a scroll shows declared rows
// while the next round declares the rows that it reaches.
constexpr uint32_t kOverscan = 2;

// The number of rows that a window declares before its view's first layout, if the view's height is not given in px.
constexpr uint32_t kUnmeasuredRows = 32;

// The distance in px that a row's press moves before the row drags.
constexpr float kDragThreshold = 4;

// The px by which a tree indents each level, and the inset of a row's content from each side.
constexpr float kIndent = 12;              // the sixth step of the spacing scale
constexpr float kRowInset = 6;             // the third step of the spacing scale

// The size of a tile's icon in its thumbnail.
constexpr float kTileIconSize = 24;

// The sizes of a row's icon, and of a disclosure's chevron and a visibility toggle's eye in their cells.
constexpr float kRowIconSize = 12;
constexpr float kChevronSize = 10;
constexpr float kCellIconSize = 12;

// The rules that a row reserves on its sides, which a selection and a drop mark with color: the selection's bar at the
// row's start, and a drop's line above or below the row.
constexpr float kSelectionBar = 2;
constexpr float kDropLine = 1;

// The height of a table's header, which is a small control's height.
constexpr float kHeaderHeight = 22;

// The gap between the cells of a row, and between a table's columns, which keeps a column whose cells end at its end
// apart from the next.
constexpr float kRowGap = 4;               // the second step of the spacing scale
constexpr float kColumnGap = 12;           // the sixth step of the spacing scale

// What a row is.
enum {
    RowKind_List,                   // a list's or a tree's row
    RowKind_Table,                  // a table's row of cells
    RowKind_Header,                 // a table's header, which heads its columns
};
typedef uint8_t RowKind;

// A row's box: as tall as its view's rows, padded by its indent at its start, and at least `minWidth` px wide if that
// is nonzero. Every rule is reserved regardless of the row's state, so that selecting or marking a row changes only its
// painting: the selection's bar at its start, and a drop's line above and below.
PR_NO_PADDING_BEGIN
struct RowBox {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Box;
    float height;
    float indent;
    float minWidth;

    void make(const PrKit&, PrLayoutBoxStyle& box) const {
        box.display = PrLayoutDisplay_Flex;
        box.boxSizing = PrLayoutBoxSizing_BorderBox;
        box.height = prLayoutLengthPx(height);
        if (minWidth > 0)
            box.minWidth = prLayoutLengthPx(minWidth);
        kitSetPadding(box, PrKitEdges{0, kRowInset, 0, indent});
        box.border[PrRectSide_Top] = prLayoutUnitFromPx(kDropLine);
        box.border[PrRectSide_Bottom] = prLayoutUnitFromPx(kDropLine);
        box.border[PrRectSide_Left] = prLayoutUnitFromPx(kSelectionBar);
    }
};

// A row's flex: its cells centered across it, apart by the gap of a list's row or of a table's columns.
struct RowFlex {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Flex;
    RowKind kind;

    void make(const PrKit&, PrLayoutFlexStyle& flex) const {
        flex.shrink = 0;
        flex.alignItems = PrLayoutAlignment_Center;
        flex.columnGap = prLayoutLengthPx(kind == RowKind_List ? kRowGap : kColumnGap);
    }
};

// A row's decoration: its fill by its state, and the rules that mark its selection and a drop on it. A table's header
// is filled as a panel, with a rule below it.
struct RowFill {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Decoration;
    bool selected;
    bool hover;
    PrKitDropZone drop;
    RowKind kind;

    void make(const PrKit& kit, PrLayoutDecorationStyle& decoration) const {
        const PrKitPalette& p = kit.theme.palette;
        PrColor clear{0, 0, 0, 0};
        PrColor colors[PrRectSide_Count] = {clear, clear, clear, clear};
        if (selected)
            colors[PrRectSide_Left] = p.accent;
        if (drop == PrKitDropZone_Before)
            colors[PrRectSide_Top] = p.accent;
        if (drop == PrKitDropZone_After)
            colors[PrRectSide_Bottom] = p.accent;
        if (kind == RowKind_Header)
            colors[PrRectSide_Bottom] = p.line1;
        // The rules of RowBox: every side but the end.
        for (uint32_t side = 0; side < PrRectSide_Count; ++side) {
            decoration.borderColor[side] = kitColor16(colors[side]);
            if (side != PrRectSide_Right)
                decoration.borderStyle[side] = PrDrawBorderStyle_Solid;
        }
        PrColor fill = selected ? p.accentWeak : hover ? p.bg2 : clear;
        if (kind == RowKind_Header)
            fill = p.bg1;
        if (drop == PrKitDropZone_Inside) {
            fill = p.accentWeak;
            decoration.outlineColor = kitColor16(p.accent);
            decoration.outlineStyle = PrDrawBorderStyle_Solid;
            decoration.outlineWidth = prLayoutUnitFromPx(1);
            decoration.outlineOffset = prLayoutUnitFromPx(-1);
        }
        decoration.background = kitColor16(fill);
    }
};
PR_NO_PADDING_END

// A grid's tile: a share of its row's width, a padded column of its thumbnail and label, with a frame reserved on every
// tile, so that selecting one changes only its painting.
PR_NO_PADDING_BEGIN
struct TileBox {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Box;
    float width;                    // a percentage of the row's width
    float height;

    void make(const PrKit&, PrLayoutBoxStyle& box) const {
        box.display = PrLayoutDisplay_Flex;
        box.boxSizing = PrLayoutBoxSizing_BorderBox;
        box.width = prLayoutLengthPercent(width);
        box.height = prLayoutLengthPx(height);
        kitSetPadding(box, prKitEdgesAll(kPrKitSpace2));
        kitSetBorderWidth(box, 1);
    }
};
PR_NO_PADDING_END
struct TileFlex {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Flex;
    void make(const PrKit&, PrLayoutFlexStyle& flex) const {
        flex.direction = PrLayoutFlexDirection_Column;
        flex.rowGap = prLayoutLengthPx(kPrKitSpace2);
        flex.shrink = 0;
    }
};

// A tile's thumbnail, which fills the room over the label and centers its icon.
struct ThumbBox {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Box;
    void make(const PrKit&, PrLayoutBoxStyle& box) const {
        box.display = PrLayoutDisplay_Flex;
        box.minHeight = prLayoutLengthPx(0);
        box.overflowX = PrLayoutOverflow_Clip;
        box.overflowY = PrLayoutOverflow_Clip;
    }
};
struct ThumbFlex {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Flex;
    void make(const PrKit&, PrLayoutFlexStyle& flex) const {
        flex.grow = 1;
        flex.basis = prLayoutLengthPx(0);
        flex.alignItems = PrLayoutAlignment_Center;
        flex.justifyContent = PrLayoutAlignment_Center;
    }
};

// A square cell in a row that centers its icon: a disclosure or a visibility toggle.
struct CellBox {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Box;
    void make(const PrKit&, PrLayoutBoxStyle& box) const {
        box.display = PrLayoutDisplay_Flex;
        box.width = box.height = prLayoutLengthPx(kPrKitHeightXxs);
    }
};
struct CellFlex {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Flex;
    void make(const PrKit&, PrLayoutFlexStyle& flex) const {
        flex.shrink = 0;
        flex.alignItems = PrLayoutAlignment_Center;
        flex.justifyContent = PrLayoutAlignment_Center;
    }
};

// Creates a cell under `parent`, filled while it is hovered if it is pressable.
PrLayoutNode cell(PrKit* kit, PrLayoutNode parent, uint64_t key, bool pressable) {
    PrLayoutNode node = prLayoutTreeFrameElement(kit->tree, parent, key, 0);
    bool disabled = pressable ? kitSetFlags(*kit, node, PrUiFlag_Pressable) : prUiIsDisabled(kit->ui, node);
    bool hover = pressable && prUiIsHovered(kit->ui, node) && !disabled;
    KitColors fill{.background = hover ? kit->theme.palette.bg3 : PrColor{0, 0, 0, 0}};
    prLayoutTreeFrameSetStyle(kit->tree, node, kitStyle(*kit, CellBox{}, CellFlex{}, fill));
    if (pressable && !disabled)
        prUiSetCursor(kit->ui, node, PrAppCursor_Hand);
    return node;
}

// A table's column, a heading or a cell: a row that clips, of its column's width, or filling a share of the room at
// least that wide.
PR_NO_PADDING_BEGIN
struct ColumnBox {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Box;
    float width;
    bool fills;
    uint8_t _pad[3];

    void make(const PrKit&, PrLayoutBoxStyle& box) const {
        box.display = PrLayoutDisplay_Flex;
        box.overflowX = box.overflowY = PrLayoutOverflow_Clip;
        if (fills)
            box.minWidth = prLayoutLengthPx(width);
        else
            box.width = prLayoutLengthPx(width);
    }
};
struct ColumnFlex {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Flex;
    float weight;                   // the share of a column that fills, or zero for one that does not
    PrKitTextAlign align;
    uint8_t _pad[3];

    void make(const PrKit&, PrLayoutFlexStyle& flex) const {
        flex.alignItems = PrLayoutAlignment_Center;
        if (align == PrKitTextAlign_End)
            flex.justifyContent = PrLayoutAlignment_FlexEnd;
        else if (align == PrKitTextAlign_Center)
            flex.justifyContent = PrLayoutAlignment_Center;
        else
            flex.justifyContent = PrLayoutAlignment_FlexStart;
        flex.columnGap = prLayoutLengthPx(kRowGap);
        flex.shrink = 0;
        if (weight > 0) {
            flex.grow = weight;
            flex.basis = prLayoutLengthPx(0);
        }
    }
};
PR_NO_PADDING_END

// The port of a table's header, which follows the rows' sideways scroll.
struct HeaderPortBox {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Box;
    void make(const PrKit&, PrLayoutBoxStyle& box) const {
        box.display = PrLayoutDisplay_Block;
        box.overflowX = box.overflowY = PrLayoutOverflow_Hidden;
    }
};

// A view's content, which stands in for the rows outside its window with its padding above and below, and lays out a
// grid's tiles in rows that wrap.
PR_NO_PADDING_BEGIN
struct ContentBox {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Box;
    float minWidth;
    float above;
    float below;
    bool tiles;
    uint8_t _pad[3];

    void make(const PrKit&, PrLayoutBoxStyle& box) const {
        box.display = tiles ? PrLayoutDisplay_Flex : PrLayoutDisplay_Block;
        if (minWidth > 0)
            box.minWidth = prLayoutLengthPx(minWidth);
        box.padding[PrRectSide_Top] = prLayoutLengthPx(above);
        box.padding[PrRectSide_Bottom] = prLayoutLengthPx(below);
    }
};
struct ContentFlex {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Flex;
    bool tiles;

    void make(const PrKit&, PrLayoutFlexStyle& flex) const {
        flex.shrink = 0;
        if (tiles)
            flex.wrap = PrLayoutFlexWrap_Wrap;
    }
};
PR_NO_PADDING_END

// Returns the width of a table's rows with every column at its least width.
float tableMinWidth(const PrKitTableColumn* columns, uint32_t count) {
    float width = kSelectionBar + kRowInset * 2;
    for (uint32_t i = 0; i < count; ++i)
        width += columns[i].width + (i != 0 ? kColumnGap : 0);
    return width;
}

// Creates a column's heading or cell under `parent`.
PrLayoutNode columnElement(PrKit* kit, PrLayoutNode parent, uint64_t key, const PrKitTableColumn& c) {
    bool fills = c.flex == PrKitFlex_Fill;
    ColumnFlex flex{.weight = fills ? (c.weight > 0 ? c.weight : 1) : 0, .align = c.align};
    ColumnBox box{.width = c.width, .fills = fills};
    return prLayoutTreeFrameElement(kit->tree, parent, key, kitStyle(*kit, box, flex));
}

// ---------------------------------------------------------------------------------------------------------------------
// Selection

// Moves the lead to `to`, clamped to the rows, and returns the edit: the lead's row, or the range from the anchor if
// `extend` is true.
PrKitSelectionEdit moveLead(PrKitSelection& selection, uint32_t count, uint32_t to, bool extend) {
    to = to < count ? to : count - 1;
    selection.lead = to;
    if (!extend) {
        selection.anchor = to;
        return PrKitSelectionEdit{.op = PrKitSelectionOp_Replace, .first = to, .count = 1};
    }
    uint32_t anchor = selection.anchor < count ? selection.anchor : count - 1;
    uint32_t first = anchor < to ? anchor : to;
    uint32_t last = anchor < to ? to : anchor;
    return PrKitSelectionEdit{.op = PrKitSelectionOp_Replace, .first = first, .count = last - first + 1};
}

// Returns the edit that a click on row `index` makes with `modifiers`, and moves the cursor to the row.
PrKitSelectionEdit clickEdit(PrKitSelection& selection, uint32_t index, PrAppModifierFlags modifiers) {
    bool extend = (modifiers & PrAppModifierFlag_Shift) != 0;
    bool toggle = (modifiers & (PrAppModifierFlag_Super | PrAppModifierFlag_Control)) != 0;
    if (extend) {
        uint32_t first = selection.anchor < index ? selection.anchor : index;
        uint32_t last = selection.anchor < index ? index : selection.anchor;
        selection.lead = index;
        PrKitSelectionOp op = toggle ? PrKitSelectionOp_Add : PrKitSelectionOp_Replace;
        return PrKitSelectionEdit{.op = op, .first = first, .count = last - first + 1};
    }
    selection.anchor = index;
    selection.lead = index;
    PrKitSelectionOp op = toggle ? PrKitSelectionOp_Toggle : PrKitSelectionOp_Replace;
    return PrKitSelectionEdit{.op = op, .first = index, .count = 1};
}

// Returns the modifiers of the last pointer event that the round routed to `node`.
PrAppModifierFlags pointerModifiers(const PrUi* ui, PrLayoutNode node) {
    PrUiEvents events = prUiEvents(ui, node);
    PrAppModifierFlags modifiers = 0;
    for (uint32_t i = 0; i < events.count; ++i) {
        PrAppEventKind kind = events.events[i].kind;
        if (kind == PrAppEventKind_PointerDown || kind == PrAppEventKind_PointerUp)
            modifiers = events.events[i].modifiers;
    }
    return modifiers;
}

// ---------------------------------------------------------------------------------------------------------------------
// Keys

// A key that a view claims. If the key moves the lead, the view claims it both with and without Shift.
struct ViewKey {
    PrAppKey key;
    bool extends;
};

constexpr ViewKey kListKeys[] = {
    {PrAppKey_Up, true},       {PrAppKey_Down, true},     {PrAppKey_Home, true}, {PrAppKey_End, true},
    {PrAppKey_PageUp, true},   {PrAppKey_PageDown, true}, {' ', false},
};
constexpr ViewKey kTreeKeys[] = {{PrAppKey_Left, true}, {PrAppKey_Right, true}};

// Finds the key, among those that the view claims, that the round routed to the view, and whether Shift was held.
// Claims every key for the next round. A tree and a grid claim Left and Right too. Returns false if the round routed
// none.
bool viewKey(PrUi* ui, PrLayoutNode view, bool sideways, PrAppKey* key, bool* shift) {
    bool found = false;
    auto claim = [&](PrAppKey k, PrAppModifierFlags modifiers) {
        if (prUiKeyPressed(ui, view, k, modifiers) && !found) {
            found = true;
            *key = k;
            *shift = (modifiers & PrAppModifierFlag_Shift) != 0;
        }
    };
    for (const ViewKey& k : kListKeys) {
        claim(k.key, 0);
        if (k.extends)
            claim(k.key, PrAppModifierFlag_Shift);
    }
    if (sideways) {
        for (const ViewKey& k : kTreeKeys) {
            claim(k.key, 0);
            claim(k.key, PrAppModifierFlag_Shift);
        }
    }
    // Select all is A with the accelerator modifier, which is Command or Control.
    if (prUiKeyPressed(ui, view, 'A', PrAppModifierFlag_Super) ||
            prUiKeyPressed(ui, view, 'A', PrAppModifierFlag_Control)) {
        if (!found) {
            found = true;
            *key = 'A';
            *shift = false;
        }
    }
    return found;
}

// Returns the nearest row before `index` with a lesser depth, or `index` at the top level.
uint32_t parentRow(const PrKitTreeItem* items, uint32_t index) {
    for (uint32_t i = index; i > 0; --i) {
        if (items[i - 1].depth < items[index].depth)
            return i - 1;
    }
    return index;
}

// Applies a key to the view's selection, or asks for a fold of a tree's row. `columns` is the number of tiles in a row
// of a grid, or 1, and `page` the number of rows or tiles that the scrollport shows. Returns the edit, which moves the
// lead if its op is not None.
PrKitSelectionEdit applyKey(PrKitSelection& selection, uint32_t count, uint32_t columns, uint32_t page, PrAppKey key,
        bool shift, const PrKitTreeItem* PR_NULLABLE items, PrKitTreeFold* fold) {
    uint32_t lead = selection.lead < count ? selection.lead : count - 1;
    if (items != nullptr && (key == PrAppKey_Left || key == PrAppKey_Right)) {
        const PrKitTreeItem& item = items[lead];
        bool right = key == PrAppKey_Right;
        if (item.hasChildren && item.open != right) {
            *fold = PrKitTreeFold{.row = lead, .open = right};
            return PrKitSelectionEdit{};
        }
        if (right)
            return item.hasChildren ? moveLead(selection, count, lead + 1, shift) : PrKitSelectionEdit{};
        return moveLead(selection, count, parentRow(items, lead), shift);
    }
    switch (key) {
        case PrAppKey_Up:
            return moveLead(selection, count, lead >= columns ? lead - columns : lead % columns, shift);
        case PrAppKey_Down:
            return moveLead(selection, count, lead + columns, shift);
        case PrAppKey_Left:
            return moveLead(selection, count, lead > 0 ? lead - 1 : 0, shift);
        case PrAppKey_Right:
            return moveLead(selection, count, lead + 1, shift);
        case PrAppKey_PageUp:
            return moveLead(selection, count, lead > page ? lead - page : 0, shift);
        case PrAppKey_PageDown:
            return moveLead(selection, count, lead + page < count ? lead + page : count - 1, shift);
        case PrAppKey_Home:
            return moveLead(selection, count, 0, shift);
        case PrAppKey_End:
            return moveLead(selection, count, count - 1, shift);
        case ' ':
            return PrKitSelectionEdit{.op = PrKitSelectionOp_Toggle, .first = lead, .count = 1};
        case 'A':
            selection.anchor = 0;
            selection.lead = count - 1;
            return PrKitSelectionEdit{.op = PrKitSelectionOp_Replace, .first = 0, .count = count};
        default:
            return PrKitSelectionEdit{};
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Views

// Returns where a drop on `node` lands, from which third of its box the pointer was over at the last layout
// (prKitDropZone).
PrKitDropZone dropZoneOf(PrKit& kit, PrLayoutNode node) {
    // The pointer is measured in the row's own plane, so that the zones hold under a transform.
    PrVec2 pointer{};
    PrVec2 at{};
    if (!prUiPointer(kit.ui, nullptr, &pointer) || !prUiLocalPoint(kit.ui, node, pointer, &at))
        return PrKitDropZone_None;
    PrVec2 extent = kitExtent(kit, node);
    if (extent.y <= 0 || at.x < 0 || at.x >= extent.x || at.y < 0 || at.y >= extent.y)
        return PrKitDropZone_None;
    if (at.y < extent.y / 3)
        return PrKitDropZone_Before;
    return at.y >= extent.y * 2 / 3 ? PrKitDropZone_After : PrKitDropZone_Inside;
}

// The context that a list, tree or table view provides to its rows for the current round: the node the rows go under,
// the window, the rows' height, the tiles in a row of a grid, and what the round's input asked of the caller. A tree
// view also gives its items and drag kind, and a table its columns.
struct View {
    PrLayoutNode content;
    PrKitSelection* PR_NULLABLE selection;
    const PrKitTreeItem* PR_NULLABLE items;
    const PrKitTableColumn* PR_NULLABLE columns;
    uint64_t dragKind;
    uint32_t columnCount;
    uint32_t first;
    uint32_t count;
    uint32_t tiles;                 // the tiles in a row of a grid, or 1
    float rowHeight;
    float minWidth;                 // a table's least row width, or 0
    PrKitSelectionEdit edit;
    PrKitTreeFold fold;
    PrKitTreeDrop drop;
    bool folded;
    bool sorted;
};

// Returns the context of the view that `view` names. Rows are created under the view itself.
View& viewOf(PrKit& kit, PrLayoutNode view) {
    PrLayoutNode provider = kPrLayoutNodeNone;
    View* v = kitContext<View>(kit, view, KitContext_View, &provider);
    PR_ASSERT(v != nullptr && provider == view);
    return *v;
}

const View* PR_NULLABLE findView(const PrKit& kit, PrLayoutNode view) {
    PrLayoutNode provider = kPrLayoutNodeNone;
    const View* v = kitContext<View>(kit, view, KitContext_View, &provider);
    return provider == view ? v : nullptr;
}

// How a table's view differs from a list's: its rows are at least `minWidth` px wide, and scroll sideways if the view
// is narrower. `port` is the height of its scrollport before its first layout, or zero if the view's place gives it.
struct ViewShape {
    PrKitPlace place;
    float minWidth;
    float port;
};

// Creates the scroll container of a view of `count` rows in `style`, under `parent`, and fills `v` with its window,
// applying the round's keys to `v.selection`. If `v.items` is set, the view is a tree view and writes its fold. Returns
// the scroll container.
PrLayoutNode listView(PrKit& kit, PrLayoutNode parent, PrUiKey key, uint32_t count, const PrKitListViewStyle& style,
        const ViewShape& shape, View& v) {
    float rowHeight = style.rowHeight > 0 ? style.rowHeight : kPrKitHeightSm;
    PrUi* ui = kit.ui;
    PrLayoutTree* tree = kit.tree;
    PrKitBoxStyle box{};
    box.overflow = shape.minWidth > 0 ? PrKitOverflow_Scroll : PrKitOverflow_ScrollY;
    box.place = shape.place;
    PrLayoutNode view = kitBoxNode(kit, parent, PrKitBox{.key = key}, box.overflow,
        kitStyle(kit, KitContainerBox{.style = box}, kitContainerFlex(box)));
    PrUiFlags flags = PrUiFlag_Scrollable;
    if (v.selection != nullptr)
        flags = flags | PrUiFlag_Focusable;
    prUiSetFlags(ui, view, flags);

    // The scrollport at the last layout, or a guess before the view's first layout.
    PrLayoutScrollInfo info{};
    bool measured = prLayoutNodeScrollInfo(tree, view, &info) && info.scrollContainer;
    float port = measured ? prLayoutUnitToPx(info.portExtent.y) : 0;
    if (!measured) {
        if (shape.port > 0)
            port = shape.port;
        else if (shape.place.height.kind == PrKitSizeKind_Px)
            port = shape.place.height.value;
        else
            port = rowHeight * kUnmeasuredRows;
        prUiRequestFrame(ui);
    }
    // A grid's tiles fill rows of as many as fit at their width, and the view's rows are rows of tiles.
    uint32_t tiles = 1;
    if (style.tileWidth > 0 && measured) {
        float width = prLayoutUnitToPx(info.portExtent.x);
        tiles = width >= style.tileWidth * 2 ? uint32_t(width / style.tileWidth) : 1;
    }
    uint32_t rowCount = (count + tiles - 1) / tiles;
    float total = rowHeight * float(rowCount);
    float maxOffset = total > port ? total - port : 0;
    PrLayoutVec scroll = prLayoutNodeScroll(tree, view);
    float offset = prLayoutUnitToPx(scroll.y);
    offset = offset < 0 ? 0 : offset > maxOffset ? maxOffset : offset;

    v.tiles = tiles;
    v.rowHeight = rowHeight;
    v.minWidth = shape.minWidth;
    PrAppKey pressed = PrAppKey_Unknown;
    bool shift = false;
    bool sideways = v.items != nullptr || style.tileWidth > 0;
    if (v.selection != nullptr && viewKey(ui, view, sideways, &pressed, &shift) && count != 0) {
        uint32_t page = uint32_t(port / rowHeight);
        page = page > 1 ? page : 1;
        PrKitTreeFold fold{.row = kPrKitRowNone};
        v.edit = applyKey(*v.selection, count, tiles, page * tiles, pressed, shift, v.items, &fold);
        if (fold.row != kPrKitRowNone) {
            v.fold = fold;
            v.folded = true;
        }
        // Scrolls by the smallest amount that shows the lead's row, which the window then holds.
        if (v.edit.op != PrKitSelectionOp_None && measured) {
            float top = rowHeight * float(v.selection->lead / tiles);
            float to = top < offset ? top : top + rowHeight > offset + port ? top + rowHeight - port : offset;
            if (to != offset) {
                offset = to;
                prLayoutNodeSetScroll(tree, view, PrLayoutVec{scroll.x, prLayoutUnitFromPx(offset)});
            }
        }
    }

    uint32_t top = uint32_t(offset / rowHeight);
    uint32_t bottom = uint32_t((offset + port) / rowHeight) + 1;
    uint32_t first = top > kOverscan ? top - kOverscan : 0;
    uint32_t end = bottom + kOverscan < rowCount ? bottom + kOverscan : rowCount;
    first = first < end ? first : end;
    v.first = first * tiles;
    v.count = (end * tiles < count ? end * tiles : count) - v.first;

    // The content's padding moves with the scroll, and only its box group with it.
    bool grid = style.tileWidth > 0;
    ContentBox content{
        .minWidth = shape.minWidth,
        .above = rowHeight * float(first),
        .below = rowHeight * float(rowCount - end),
        .tiles = grid,
    };
    v.content = prLayoutTreeFrameElement(tree, view, 0, kitStyle(kit, content, ContentFlex{.tiles = grid}));
    return view;
}

// What a row or a tile tells the cells that its caller creates under it (KitContext_Item).
struct ItemCell {
    bool selected;
};

// Returns true if the row or tile that `node` was created under is selected.
bool itemSelected(const PrKit& kit, PrLayoutNode node) {
    const ItemCell* item = kitContext<ItemCell>(kit, node, KitContext_Item);
    return item != nullptr && item->selected;
}

// Creates a tile's thumbnail box on a background of `tint`, or on the theme's raised background if `tint` is fully
// transparent.
PrLayoutNode tileThumbnail(PrKit& kit, PrLayoutNode tile, PrColor tint) {
    KitColors background{.background = tint.a > 0 ? tint : kit.theme.palette.bg3};
    return prLayoutTreeFrameElement(kit.tree, tile, 0, kitStyle(kit, ThumbBox{}, ThumbFlex{}, background));
}

// Applies a click on the node of row or tile `index` to the view's selection.
void applyClick(PrKit& kit, View& v, uint32_t index, PrLayoutNode node) {
    if (v.selection != nullptr && prUiClicked(kit.ui, node, 0))
        v.edit = clickEdit(*v.selection, index, pointerModifiers(kit.ui, node));
}

// Creates the element of a view's row, keyed by `item` under the view, and indented by `indent` px, and applies a click
// on it to the view's selection. A row of a tree view that drags marks where a payload of its kind would land on it.
PrLayoutNode rowElement(PrKit& kit, PrLayoutNode view, View& v, uint32_t index, uint64_t item, bool selected, float indent) {
    RowKind kind = v.minWidth > 0 ? RowKind_Table : RowKind_List;
    PrUi* ui = kit.ui;
    PrLayoutTree* tree = kit.tree;
    PrLayoutNode node = prLayoutTreeFrameElement(tree, v.content, prUiKeyIndex(kit.ui, view, item), 0);
    bool disabled = kitSetFlags(kit, node, PrUiFlag_Pressable);
    bool hover = prUiIsHovered(ui, node) && !disabled;
    // A row is a target for payloads of the view's kind, and shows its zone under the pointer at the last layout.
    PrKitDropZone drop = PrKitDropZone_None;
    if (v.dragKind != 0 && prUiIsDropTarget(ui, node, v.dragKind))
        drop = dropZoneOf(kit, node);
    PrLayoutStyleIndex record =
        kitStyle(kit, RowBox{.height = v.rowHeight, .indent = indent, .minWidth = v.minWidth}, RowFlex{.kind = kind},
            RowFill{.selected = selected, .hover = hover, .drop = drop, .kind = kind});
    prLayoutTreeFrameSetStyle(tree, node, record);
    kitProvide<ItemCell>(kit, node, KitContext_Item).selected = selected;
    applyClick(kit, v, index, node);
    return node;
}

// Creates a row of a list or tree view, which its caller fills. A tree's row is indented by its depth and led by its
// disclosure, which this writes to `disclosure`.
PrLayoutNode rowOf(PrKit* kit, PrLayoutNode view, View& v, const PrKitRow& row, PrLayoutNode* PR_NULLABLE disclosure) {
    const PrKitTreeItem* place = v.items != nullptr ? &v.items[row.index] : nullptr;
    float depth = place != nullptr ? float(place->depth) : 0;
    PrLayoutNode node = rowElement(*kit, view, v, row.index, row.item, row.selected, kRowInset + depth * kIndent);

    if (place != nullptr) {
        // The disclosure's cell is reserved on leaves too, so that the cells after it line up down the tree.
        PrLayoutNode caret = cell(kit, node, 0, place->hasChildren);
        if (place->hasChildren) {
            PrKitIconKind chevron = place->open ? PrKitIconKind_ChevronDown : PrKitIconKind_ChevronRight;
            prKitIcon(kit, caret, {.icon = chevron}, {.size = kChevronSize, .tone = PrKitTone_Muted});
        }
        *disclosure = caret;
    }
    return node;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

void prKitSelectionApply(const PrKitSelectionEdit* edit, bool* selected, uint32_t count) {
    if (edit->op == PrKitSelectionOp_None)
        return;
    if (edit->op == PrKitSelectionOp_Replace) {
        for (uint32_t i = 0; i < count; ++i)
            selected[i] = false;
    }
    uint32_t end = edit->first + edit->count < count ? edit->first + edit->count : count;
    for (uint32_t i = edit->first; i < end; ++i)
        selected[i] = edit->op == PrKitSelectionOp_Toggle ? !selected[i] : true;
}

PrLayoutNode prKitListView(PrKit* kit, PrLayoutNode parent, PrKitListView list, PrKitListViewStyle style) {
    PrKit& k = *kit;
    View v{.selection = list.selection};
    PrLayoutNode view = listView(k, parent, list.key, list.count, style, ViewShape{.place = style.place}, v);
    kitProvide<View>(k, view, KitContext_View) = v;
    return view;
}

PrKitWindow prKitListWindow(const PrKit* kit, PrLayoutNode view) {
    const View* v = findView(*kit, view);
    PR_ASSERT(v != nullptr);
    return PrKitWindow{.first = v->first, .count = v->count};
}

bool prKitSelectionEdited(const PrKit* kit, PrLayoutNode view, PrKitSelectionEdit* edit) {
    const View* v = findView(*kit, view);
    if (v == nullptr || v->edit.op == PrKitSelectionOp_None)
        return false;
    *edit = v->edit;
    return true;
}

PrLayoutNode prKitListRow(PrKit* kit, PrLayoutNode view, PrKitRow row) {
    View& v = viewOf(*kit, view);
    PR_ASSERT(v.items == nullptr && v.columns == nullptr);
    return rowOf(kit, view, v, row, nullptr);
}

PrLayoutNode prKitRowIcon(PrKit* kit, PrLayoutNode row, PrKitIcon icon) {
    PrKitTone tone = itemSelected(*kit, row) ? PrKitTone_Accent : PrKitTone_Muted;
    return prKitIcon(kit, row, icon, {.size = kRowIconSize, .tone = tone});
}

PrLayoutNode prKitRowLabel(PrKit* kit, PrLayoutNode row, PrKitLabel label, PrKitTone tone) {
    PrKitTextStyle style{.role = PrKitTextRole_BodySmall, .tone = tone, .wrap = PrKitWrap_Clip};
    PrLayoutNode node = prKitLabel(kit, row, label, style);
    PrKitPlace fill{.flex = PrKitFlex_Fill};
    prKitPlace(kit, node, &fill);
    return node;
}

PrLayoutNode prKitRowMeta(PrKit* kit, PrLayoutNode row, PrKitLabel label) {
    return prKitLabel(kit, row, label, {.role = PrKitTextRole_Meta});
}

PrLayoutNode prKitListTile(PrKit* kit, PrLayoutNode view, PrKitTile tile) {
    PrKit& k = *kit;
    View& v = viewOf(k, view);
    PR_ASSERT(tile.index >= v.first && tile.index < v.first + v.count);
    PrUi* ui = k.ui;
    PrLayoutTree* tree = k.tree;
    PrLayoutNode node = prLayoutTreeFrameElement(tree, v.content, prUiKeyIndex(k.ui, view, tile.item), 0);
    bool disabled = kitSetFlags(k, node, PrUiFlag_Pressable);
    bool hover = prUiIsHovered(ui, node) && !disabled;
    const PrKitPalette& p = k.theme.palette;
    PrColor clear{0, 0, 0, 0};
    KitColors frame{
        .background = tile.selected ? p.accentWeak : hover ? p.bg2 : clear,
        .border = tile.selected ? p.accent : clear,
    };
    PrLayoutStyleIndex record =
        kitStyle(k, TileBox{.width = 100.0f / float(v.tiles), .height = v.rowHeight}, TileFlex{}, frame);
    prLayoutTreeFrameSetStyle(tree, node, record);
    kitProvide<ItemCell>(k, node, KitContext_Item).selected = tile.selected;
    applyClick(k, v, tile.index, node);
    return node;
}

PrLayoutNode prKitTileImage(PrKit* kit, PrLayoutNode tile, PrKitImage image, PrKitTileImageStyle style) {
    PrLayoutNode thumbnail = tileThumbnail(*kit, tile, PrColor{0, 0, 0, 0});
    float zoom = 0;
    if (style.hoverZoom > 0) {
        static const char kOwner = 0;
        float target = prUiIsHovered(kit->ui, tile) ? 1.0f : 0.0f;
        zoom = prUiTween(kit->ui, thumbnail, &kOwner, 0, target, kTileZoomSeconds, nullptr) * style.hoverZoom;
    }
    // The thumbnail centers the image, so that it grows evenly past every edge.
    float percent = 100.0f * (1.0f + zoom);
    PrKitImageStyle cover{
        .fit = PrKitImageFit_Cover,
        .place = {.width = prKitPercent(percent), .height = prKitPercent(percent), .flex = PrKitFlex_Fixed},
    };
    prKitImage(kit, thumbnail, image, cover);
    return thumbnail;
}

PrLayoutNode prKitTileIcon(PrKit* kit, PrLayoutNode tile, PrKitIcon icon, PrColor tint) {
    PrLayoutNode thumbnail = tileThumbnail(*kit, tile, tint);
    PrKitTone tone = tint.a > 0 ? PrKitTone_Primary : PrKitTone_Muted;
    prKitIcon(kit, thumbnail, icon, {.size = kTileIconSize, .tone = tone});
    return thumbnail;
}

PrLayoutNode prKitTileLabel(PrKit* kit, PrLayoutNode tile, PrKitLabel label) {
    PrKitTextStyle style{
        .role = PrKitTextRole_BodySmall,
        .tone = PrKitTone(itemSelected(*kit, tile) ? PrKitTone_Primary : PrKitTone_Secondary),
        .wrap = PrKitWrap_Clip,
        .align = PrKitTextAlign_Center,
    };
    return prKitLabel(kit, tile, label, style);
}

PrLayoutNode prKitEmptyState(PrKit* kit, PrLayoutNode parent, PrKitEmptyState empty) {
    PrKitBoxStyle box{};
    box.justify = PrKitJustify_Center;
    box.align = PrKitAlign_Center;
    box.gap = kPrKitSpace3;
    box.padding = prKitEdgesAll(kPrKitSpace8);
    box.place.flex = PrKitFlex_Fill;
    PrLayoutNode column = prKitBox(kit, parent, {.key = empty.key}, box);
    if (empty.icon != PrKitIconKind_None)
        prKitIcon(kit, column, {.icon = empty.icon}, {.size = 28, .tone = PrKitTone_Faint});
    PrKitTextStyle titleStyle{.role = PrKitTextRole_Caption};
    prKitLabel(kit, column, {.text = empty.title}, titleStyle);
    if (empty.hint.size != 0) {
        PrKitTextStyle hintStyle{
            .role = PrKitTextRole_BodySmall,
            .tone = PrKitTone_Faint,
            .align = PrKitTextAlign_Center,
        };
        prKitLabel(kit, column, {.text = empty.hint}, hintStyle);
    }
    return column;
}

PrLayoutNode prKitTreeView(PrKit* kit, PrLayoutNode parent, PrKitTreeView tree, PrKitListViewStyle style) {
    PR_ASSERT(tree.items != nullptr || tree.count == 0);
    PrKit& k = *kit;
    View v{.selection = tree.selection, .items = tree.items, .dragKind = tree.dragKind};
    PrLayoutNode view = listView(k, parent, tree.key, tree.count, style, ViewShape{.place = style.place}, v);
    kitProvide<View>(k, view, KitContext_View) = v;
    return view;
}

PrLayoutNode prKitTreeRow(PrKit* kit, PrLayoutNode view, PrKitRow row) {
    PrKit& k = *kit;
    View& v = viewOf(k, view);
    PR_ASSERT(v.items != nullptr && row.index < v.first + v.count);
    PrUi* ui = k.ui;
    const PrKitTreeItem& place = v.items[row.index];
    PrLayoutNode disclosure = kPrLayoutNodeNone;
    PrLayoutNode node = rowOf(kit, view, v, row, &disclosure);
    if (place.hasChildren && prUiActivated(ui, disclosure)) {
        v.fold = PrKitTreeFold{.row = row.index, .open = !place.open};
        v.folded = true;
    }
    if (v.dragKind != 0) {
        if (prUiIsPressed(ui, node, 0))
            prUiDragStart(ui, node, kDragThreshold, v.dragKind, row.item, 0);
        const PrUiPayload* dropped = prUiDropped(ui, node);
        if (dropped != nullptr && dropped->kind == v.dragKind && dropped->source != node) {
            PrKitDropZone zone = dropZoneOf(k, node);
            v.drop = PrKitTreeDrop{
                .item = dropped->value,
                .row = row.index,
                .zone = zone != PrKitDropZone_None ? zone : PrKitDropZone(PrKitDropZone_Inside),
            };
        }
    }
    return node;
}

bool prKitFolded(const PrKit* kit, PrLayoutNode view, PrKitTreeFold* fold) {
    const View* v = findView(*kit, view);
    if (v == nullptr || !v->folded)
        return false;
    *fold = v->fold;
    return true;
}

bool prKitRowDropped(const PrKit* kit, PrLayoutNode view, PrKitTreeDrop* drop) {
    const View* v = findView(*kit, view);
    if (v == nullptr || v->drop.zone == PrKitDropZone_None)
        return false;
    *drop = v->drop;
    return true;
}

PrKitDropZone prKitDropZone(PrKit* kit, PrLayoutNode node) {
    return dropZoneOf(*kit, node);
}

PrLayoutNode prKitVisibilityToggle(PrKit* kit, PrLayoutNode parent, PrKitVisibilityToggle toggle) {
    PrLayoutNode node = cell(kit, parent, toggle.key, true);
    if (toggle.hidden || toggle.revealed) {
        PrKitIconKind eye = toggle.hidden ? PrKitIconKind_EyeOff : PrKitIconKind_Eye;
        prKitIcon(kit, node, {.icon = eye},
            {.size = kCellIconSize, .tone = PrKitTone(toggle.hidden ? PrKitTone_Secondary : PrKitTone_Faint)});
    }
    return node;
}

PrLayoutNode prKitTable(PrKit* kit, PrLayoutNode parent, PrKitTable table, PrKitListViewStyle style) {
    PR_ASSERT(table.columns != nullptr || table.columnCount == 0);
    PrKit& k = *kit;
    PrUi* ui = k.ui;
    PrLayoutTree* tree = k.tree;
    PrKitBoxStyle box{};
    box.overflow = PrKitOverflow_Clip;
    box.place = style.place;
    PrLayoutNode frame = prKitBox(kit, parent, {.key = table.key}, box);
    float minWidth = tableMinWidth(table.columns, table.columnCount);

    // The header sits above the rows' scroll container, and takes its sideways scroll from it below.
    PrLayoutNode header = prLayoutTreeFrameElement(tree, frame, 0, kitStyle(k, HeaderPortBox{}, KitFixed{}));
    PrLayoutStyleIndex headings = kitStyle(k, RowBox{.height = kHeaderHeight, .indent = kRowInset, .minWidth = minWidth},
        RowFlex{.kind = RowKind_Header}, RowFill{.kind = RowKind_Header});
    PrLayoutNode row = prLayoutTreeFrameElement(tree, header, 0, headings);
    View v{.selection = table.selection, .columns = table.columns, .columnCount = table.columnCount};
    PrKitTableSort* sort = table.sort;
    for (uint32_t i = 0; i < table.columnCount; ++i) {
        const PrKitTableColumn& c = table.columns[i];
        PrLayoutNode heading = columnElement(kit, row, 0, c);
        bool sorted = sort != nullptr && sort->column == i;
        PrKitTone tone = sorted ? PrKitTone_Primary : PrKitTone_Muted;
        PrKitTextStyle labelStyle{.role = PrKitTextRole_Caption, .tone = tone};
        prKitLabel(kit, heading, {.text = c.label}, labelStyle);
        if (sorted) {
            PrKitIconKind chevron = sort->descending ? PrKitIconKind_ChevronDown : PrKitIconKind_ChevronUp;
            prKitIcon(kit, heading, {.icon = chevron}, {.size = kChevronSize, .tone = PrKitTone_Muted});
        }
        if (!c.sortable || sort == nullptr)
            continue;
        if (!kitSetFlags(k, heading, PrUiFlag_Pressable))
            prUiSetCursor(ui, heading, PrAppCursor_Hand);
        if (prUiClicked(ui, heading, 0)) {
            *sort = PrKitTableSort{.column = i, .descending = sorted && !sort->descending};
            v.sorted = true;
        }
    }

    float port = style.place.height.kind == PrKitSizeKind_Px ? style.place.height.value - kHeaderHeight : 0;
    // The rows take their content's height, and shrink to the room that the table leaves under its header.
    ViewShape shape{.minWidth = minWidth, .port = port > 0 ? port : 0};
    shape.place.flex = PrKitFlex_Auto;
    PrLayoutNode view = listView(k, frame, 0, table.count, style, shape, v);
    prLayoutNodeSetScroll(tree, header, PrLayoutVec{prLayoutNodeScroll(tree, view).x, prLayoutUnitFromPx(0)});
    kitProvide<View>(k, frame, KitContext_View) = v;
    return frame;
}

bool prKitSorted(const PrKit* kit, PrLayoutNode table) {
    const View* v = findView(*kit, table);
    return v != nullptr && v->sorted;
}

PrLayoutNode prKitTableRow(PrKit* kit, PrLayoutNode table, PrKitTableRow row) {
    View& v = viewOf(*kit, table);
    PR_ASSERT(v.columns != nullptr && row.index < v.first + v.count);
    return rowElement(*kit, table, v, row.index, row.item, row.selected, kRowInset);
}

PrLayoutNode prKitTableCell(PrKit* kit, PrLayoutNode row, PrKitTableCell cell) {
    const View* v = kitContext<View>(*kit, row, KitContext_View);
    PR_ASSERT(v != nullptr && v->columns != nullptr && cell.column < v->columnCount);
    PrLayoutNode node = columnElement(kit, row, 0, v->columns[cell.column]);
    if (cell.icon != PrKitIconKind_None)
        prKitIcon(kit, node, {.icon = cell.icon}, {.size = kRowIconSize, .tone = PrKitTone_Muted});
    if (cell.text.size != 0) {
        PrKitTextStyle textStyle{.role = cell.role, .tone = cell.tone, .wrap = PrKitWrap_Nowrap};
        prKitLabel(kit, node, {.text = cell.text}, textStyle);
    }
    return node;
}
