#include "pr/draw_image.h"
#include "pr/kit_box.h"
#include "pr/kit_button.h"
#include "pr/kit_collection.h"
#include "pr/layout_unit.h"

#include "tools/harness/kit_fixture.h"

#include "tools/harness/test.h"

#include <vector>

using namespace harness;

namespace pr_impl {

namespace {

// The row height, and the height of the views that the tests show, which is ten rows.
constexpr float kRowHeight = 22;
constexpr float kViewHeight = 220;

// The payload kind of the tests' tree rows.
constexpr uint64_t kRowKind = 7;

// What a view showed in its last round: the view, the node its rows go under, its window, and its edit of the
// selection.
struct Rows {
    PrLayoutNode view;
    PrLayoutNode content;
    uint32_t first;
    uint32_t count;
    PrKitSelectionEdit edit;
};

// Returns what the list or tree view `view` showed in the round. Its rows go under the first child of its scroll
// container, which is the view, at the last commit.
Rows rowsOf(KitFixture& f, PrLayoutNode view) {
    PrKitWindow window = prKitListWindow(f.kit, view);
    return Rows{
        .view = view,
        .content = prLayoutNodeFirstChild(f.tree, view),
        .first = window.first,
        .count = window.count,
    };
}

// A list over a large set. It holds its membership by row, and its cursor in the selection that the view edits.
struct ListScene {
    static constexpr uint32_t kMaxRows = 10000;
    KitFixture& f;
    uint32_t count = kMaxRows;
    bool bound = true;
    bool focus = false;
    bool selected[kMaxRows] = {};
    PrKitSelection selection{};
    Rows rows{};
    PrLayoutNode cells[kMaxRows] = {};

    explicit ListScene(KitFixture& fixture) : f(fixture) {
        f.build = [this](PrLayoutNode root) { build(root); };
    }

    void build(PrLayoutNode root) {
        PrKitListView list{.count = count, .selection = bound ? &selection : nullptr};
        PrLayoutNode view = prKitListView(f.kit, root, list,
            {.rowHeight = kRowHeight, .place = {.height = prKitPx(kViewHeight), .flex = PrKitFlex_Fixed}});
        rows = rowsOf(f, view);
        if (focus) {
            prUiSetFocus(f.ui, rows.view);
            focus = false;
        }
        for (uint32_t i = 0; i < rows.count; ++i) {
            uint32_t index = rows.first + i;
            cells[index] = prKitListRow(f.kit, rows.view, {.index = index, .item = index, .selected = selected[index]});
            prKitRowIcon(f.kit, cells[index], {.icon = PrKitIconKind_Cube});
            prKitRowLabel(f.kit, cells[index], {.text = "Entity"}, PrKitTone_Primary);
            prKitRowMeta(f.kit, cells[index], {.text = "mesh"});
        }
        if (prKitSelectionEdited(f.kit, rows.view, &rows.edit))
            prKitSelectionApply(&rows.edit, selected, count);
    }

    uint32_t selectedCount() const {
        uint32_t total = 0;
        for (uint32_t i = 0; i < count; ++i)
            total += selected[i] ? 1 : 0;
        return total;
    }

    float scrollY() const {
        return prLayoutUnitToPx(prLayoutNodeScroll(f.tree, rows.view).y);
    }

    void clickRow(uint32_t index, PrAppModifierFlags modifiers = 0) {
        PrVec2 at = f.center(cells[index]);
        f.push(PrAppEvent{.kind = PrAppEventKind_PointerDown, .modifiers = modifiers, .pointer = {.position = at}});
        f.push(PrAppEvent{.kind = PrAppEventKind_PointerUp, .modifiers = modifiers, .pointer = {.position = at}});
        f.frame();
    }
};

// A tree of three groups, each with two leaves while it is open, flattened in display order each round.
struct TreeScene {
    KitFixture& f;
    bool open[3] = {};
    bool hidden[3] = {};
    std::vector<PrKitTreeItem> items;
    std::vector<uint64_t> ids;
    PrKitSelection selection{};
    Rows rows{};
    std::vector<PrLayoutNode> nodes;
    std::vector<PrLayoutNode> toggles;
    uint64_t selectedId = 0;
    PrKitTreeFold fold{.row = kPrKitRowNone};
    PrKitTreeDrop drop{};
    uint32_t toggled = 0;

    explicit TreeScene(KitFixture& fixture) : f(fixture) {
        f.build = [this](PrLayoutNode root) { build(root); };
    }

    void order() {
        items.clear();
        ids.clear();
        for (uint32_t group = 0; group < 3; ++group) {
            items.push_back(PrKitTreeItem{.depth = 0, .hasChildren = true, .open = open[group]});
            ids.push_back(group * 10 + 10);
            if (!open[group])
                continue;
            for (uint32_t child = 0; child < 2; ++child) {
                items.push_back(PrKitTreeItem{.depth = 1});
                ids.push_back(group * 10 + 10 + child + 1);
            }
        }
    }

    void build(PrLayoutNode root) {
        order();
        PrKitTreeView tree{
            .count = uint32_t(items.size()),
            .selection = &selection,
            .items = items.data(),
            .dragKind = kRowKind,
        };
        PrLayoutNode view = prKitTreeView(f.kit, root, tree,
            {.rowHeight = kRowHeight, .place = {.height = prKitPx(kViewHeight), .flex = PrKitFlex_Fixed}});
        rows = rowsOf(f, view);
        nodes.assign(items.size(), kPrLayoutNodeNone);
        toggles.assign(items.size(), kPrLayoutNodeNone);
        for (uint32_t i = 0; i < rows.count; ++i) {
            uint32_t index = rows.first + i;
            uint64_t id = ids[index];
            nodes[index] = prKitTreeRow(f.kit, view, {.index = index, .item = id, .selected = id == selectedId});
            prKitRowIcon(f.kit, nodes[index], {.icon = PrKitIconKind_Cube});
            prKitRowLabel(f.kit, nodes[index], {.text = "Entity"}, PrKitTone_Primary);
            if (items[index].depth == 0) {
                uint32_t group = uint32_t(id / 10 - 1);
                toggles[index] = prKitVisibilityToggle(
                    f.kit, nodes[index], {.hidden = hidden[group], .revealed = prUiIsHovered(f.ui, nodes[index])});
                if (prKitActivated(f.kit, toggles[index])) {
                    hidden[group] = !hidden[group];
                    ++toggled;
                }
            }
        }
        if (prKitSelectionEdited(f.kit, view, &rows.edit))
            selectedId = ids[selection.lead];
        PrKitTreeFold folded{};
        if (prKitFolded(f.kit, view, &folded)) {
            fold = folded;
            open[ids[folded.row] / 10 - 1] = folded.open;
        }
        PrKitTreeDrop dropped{};
        if (prKitRowDropped(f.kit, view, &dropped))
            drop = dropped;
    }

    // Returns the index of the row of `id` in the last order.
    uint32_t indexOf(uint64_t id) const {
        for (uint32_t i = 0; i < ids.size(); ++i) {
            if (ids[i] == id)
                return i;
        }
        return kPrKitRowNone;
    }
};

// A table of a hundred rows in three columns, sorted by the application: a name that fills, a fixed type, and a fixed
// size at the end.
struct TableScene {
    static constexpr uint32_t kRows = 100;
    KitFixture& f;
    float width = 400;
    bool limited = false;           // the width is the table's most, and the table fills its parent's width up to it
    float nameWidth = 120;
    PrKitTableSort sort{.column = kPrKitColumnNone};
    PrKitSelection selection{};
    bool selected[kRows] = {};
    uint32_t order[kRows] = {};
    uint32_t sorts = 0;
    Rows rows{};
    PrLayoutNode cells[kRows][3] = {};

    explicit TableScene(KitFixture& fixture) : f(fixture) {
        f.build = [this](PrLayoutNode root) { build(root); };
    }

    void build(PrLayoutNode root) {
        const PrKitTableColumn columns[] = {
            {.label = "NAME", .width = nameWidth, .flex = PrKitFlex_Fill, .sortable = true},
            {.label = "TYPE", .width = 80, .sortable = true},
            {.label = "SIZE", .width = 60, .align = PrKitTextAlign_End},
        };
        PrKitTable table{.count = kRows, .selection = &selection, .columns = columns, .columnCount = 3, .sort = &sort};
        PrKitListViewStyle style{
            .rowHeight = kRowHeight,
            .place = {.width = prKitPx(width), .height = prKitPx(kViewHeight), .flex = PrKitFlex_Fixed},
        };
        if (limited)
            style.place = {.height = prKitPx(kViewHeight), .maxWidth = prKitPx(width), .flex = PrKitFlex_Fixed};
        PrLayoutNode view = prKitTable(f.kit, prKitBox(f.kit, root, {}, {.gap = 0}), table, style);
        rows = rowsOf(f, view);
        if (prKitSorted(f.kit, view))
            ++sorts;
        for (uint32_t i = 0; i < kRows; ++i)
            order[i] = sort.column != kPrKitColumnNone && sort.descending ? kRows - 1 - i : i;
        for (uint32_t i = rows.first; i < rows.first + rows.count; ++i) {
            PrLayoutNode row = prKitTableRow(f.kit, view, {.index = i, .item = order[i], .selected = selected[i]});
            cells[i][0] = prKitTableCell(f.kit, row, {.column = 0, .text = "Entity", .icon = PrKitIconKind_Cube});
            cells[i][1] = prKitTableCell(f.kit, row, {.column = 1, .text = "mesh", .role = PrKitTextRole_Mono});
            cells[i][2] = prKitTableCell(f.kit, row, {.column = 2, .text = "12 KB", .role = PrKitTextRole_Mono});
        }
        if (prKitSelectionEdited(f.kit, view, &rows.edit))
            prKitSelectionApply(&rows.edit, selected, kRows);
    }

    // Returns the container that the table's rows scroll in, which follows its header.
    PrLayoutNode scroller() const {
        return prLayoutNodeLastChild(f.tree, rows.view);
    }

    // Returns the header's heading of a column, which is the column's child of the header's row.
    PrLayoutNode heading(uint32_t column) const {
        PrLayoutNode header = prLayoutNodeFirstChild(f.tree, rows.view);
        PrLayoutNode node = prLayoutNodeFirstChild(f.tree, prLayoutNodeFirstChild(f.tree, header));
        for (uint32_t i = 0; i < column; ++i)
            node = prLayoutNodeNextSibling(f.tree, node);
        return node;
    }
};

// A grid of a thousand tiles, a hundred px wide at least, in a view that is four hundred and ten px wide.
struct GridScene {
    static constexpr uint32_t kTiles = 1000;
    KitFixture& f;
    bool selected[kTiles] = {};
    PrKitSelection selection{};
    Rows rows{};
    PrLayoutNode tiles[kTiles] = {};

    explicit GridScene(KitFixture& fixture) : f(fixture) {
        f.build = [this](PrLayoutNode root) { build(root); };
    }

    void build(PrLayoutNode root) {
        PrKitListViewStyle style{
            .rowHeight = 80,
            .tileWidth = 100,
            .place = {.width = prKitPx(410), .height = prKitPx(kViewHeight), .flex = PrKitFlex_Fixed},
        };
        PrLayoutNode view = prKitListView(f.kit, root, {.count = kTiles, .selection = &selection}, style);
        rows = rowsOf(f, view);
        for (uint32_t i = rows.first; i < rows.first + rows.count; ++i) {
            tiles[i] = prKitListTile(f.kit, view, {.index = i, .item = i, .selected = selected[i]});
            prKitTileIcon(f.kit, tiles[i], {.icon = PrKitIconKind_Cube}, PrColor{0, 0, 0, 0});
            prKitTileLabel(f.kit, tiles[i], {.text = "Entity"});
        }
        if (prKitSelectionEdited(f.kit, view, &rows.edit))
            prKitSelectionApply(&rows.edit, selected, kTiles);
    }
};

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a list of ten thousand items declares only the rows its view shows") {
    KitFixture f;
    ListScene scene(f);
    f.frame();
    f.frame();
    CHECK(scene.rows.first == 0);
    CHECK(scene.rows.count >= uint32_t(kViewHeight / kRowHeight));
    CHECK(scene.rows.count < 20);
    // The content covers every row, and so does the scroll range.
    PrRect content = f.bounds(scene.rows.content);
    CHECK(content.bottom - content.top == doctest::Approx(double(kRowHeight) * ListScene::kMaxRows));
    PrRect row = f.bounds(scene.cells[3]);
    CHECK(row.top - f.bounds(scene.rows.view).top == doctest::Approx(3 * double(kRowHeight)));
    CHECK(row.bottom - row.top == doctest::Approx(double(kRowHeight)));
    CHECK(kitVerified(f.tree));
}

TEST_CASE("a list frame that changes nothing makes no node and lays nothing out") {
    KitFixture f;
    ListScene scene(f);
    f.frame();
    f.frame();
    f.frame();
    PrLayoutTreeStats stats = prLayoutTreeStats(f.tree);
    CHECK(stats.frameNodesMade == 0);
    CHECK(stats.blocksLaidOut == 0);
    CHECK_FALSE(prUiOutput(f.ui).wantsFrame);
    CHECK(kitVerified(f.tree));
}

TEST_CASE("the wheel moves a list's window in its frame, laying out the list's content alone") {
    KitFixture f;
    ListScene scene(f);
    f.frame();
    f.frame();
    PrVec2 at = f.center(scene.rows.view);
    f.move(at);
    f.frame();
    f.frame();
    f.push(PrAppEvent{.kind = PrAppEventKind_Scroll, .scroll = {.position = at, .delta = PrVec2{0, 50 * kRowHeight}}});
    f.frame();
    CHECK(scene.scrollY() == doctest::Approx(50 * double(kRowHeight)));
    CHECK(scene.rows.first > 40);
    CHECK(scene.rows.first <= 50);
    CHECK(scene.rows.count < 20);
    PrLayoutTreeStats stats = prLayoutTreeStats(f.tree);
    // Only the rows that the window gained are laid out, each in a few blocks, along with the view and its content.
    CHECK(stats.blocksLaidOut <= scene.rows.count * 5 + 2);
    // The rows that the window kept keep their nodes.
    CHECK(stats.frameNodesMade > 0);
    CHECK(kitVerified(f.tree));
    PrRect row = f.bounds(scene.cells[50]);
    CHECK(row.top == doctest::Approx(double(f.bounds(scene.rows.view).top)));
    f.frame();
    CHECK(prLayoutTreeStats(f.tree).blocksLaidOut == 0);
}

TEST_CASE("a click selects a row, Shift extends from the anchor, and Command toggles") {
    KitFixture f;
    ListScene scene(f);
    f.frame();
    f.frame();
    scene.clickRow(2);
    CHECK(scene.selectedCount() == 1);
    CHECK(scene.selected[2]);
    CHECK(scene.selection.anchor == 2);
    scene.clickRow(5, PrAppModifierFlag_Shift);
    CHECK(scene.selectedCount() == 4);
    CHECK(scene.selection.anchor == 2);
    CHECK(scene.selection.lead == 5);
    scene.clickRow(3, PrAppModifierFlag_Super);
    CHECK(scene.selectedCount() == 3);
    CHECK_FALSE(scene.selected[3]);
    scene.clickRow(8, PrAppModifierFlag_Control);
    CHECK(scene.selectedCount() == 4);
    CHECK(scene.selected[8]);
    scene.clickRow(0);
    CHECK(scene.selectedCount() == 1);
    CHECK(scene.selected[0]);
    // A click focuses the view, which takes the keys.
    CHECK(prUiFocus(f.ui) == scene.rows.view);
    CHECK(kitVerified(f.tree));
}

TEST_CASE("a selected row is painted, not laid out, again") {
    KitFixture f;
    ListScene scene(f);
    f.frame();
    f.frame();
    PrLayoutStyleIndex rest = prLayoutNodeStyle(f.tree, scene.cells[4]);
    scene.clickRow(4);
    f.frame();
    CHECK(prLayoutNodeStyle(f.tree, scene.cells[4]) != rest);
    // Moving away ends the hover. The fill change affects only painting.
    f.move(PrVec2{790, 590});
    f.frame();
    CHECK(prLayoutTreeStats(f.tree).blocksLaidOut == 0);
    CHECK(kitVerified(f.tree));
}

TEST_CASE("Up and Down move the lead, and Shift extends the range") {
    KitFixture f;
    ListScene scene(f);
    f.frame();
    f.frame();
    scene.clickRow(3);
    f.key(PrAppKey_Down);
    f.frame();
    CHECK(scene.selection.lead == 4);
    CHECK(scene.selected[4]);
    CHECK(scene.selectedCount() == 1);
    f.key(PrAppKey_Down, PrAppModifierFlag_Shift);
    f.frame();
    f.key(PrAppKey_Down, PrAppModifierFlag_Shift);
    f.frame();
    CHECK(scene.selection.lead == 6);
    CHECK(scene.selection.anchor == 4);
    CHECK(scene.selectedCount() == 3);
    f.key(PrAppKey_Up);
    f.frame();
    CHECK(scene.selection.lead == 5);
    CHECK(scene.selectedCount() == 1);
    CHECK(kitVerified(f.tree));
}

TEST_CASE("a key that moves the lead past the view scrolls it into view and declares its row") {
    KitFixture f;
    ListScene scene(f);
    scene.focus = true;
    f.frame();
    f.frame();
    REQUIRE(prUiFocus(f.ui) == scene.rows.view);
    for (uint32_t i = 0; i < 12; ++i) {
        f.key(PrAppKey_Down);
        f.frame();
    }
    CHECK(scene.selection.lead == 12);
    // The lead's row is the last row that the scrollport shows.
    CHECK(scene.scrollY() == doctest::Approx(13 * double(kRowHeight) - double(kViewHeight)));
    PrRect view = f.bounds(scene.rows.view);
    CHECK(f.bounds(scene.cells[12]).bottom == doctest::Approx(double(view.bottom)));
    f.key(PrAppKey_End);
    f.frame();
    CHECK(scene.selection.lead == ListScene::kMaxRows - 1);
    CHECK(scene.rows.first + scene.rows.count == ListScene::kMaxRows);
    CHECK(f.bounds(scene.cells[ListScene::kMaxRows - 1]).bottom == doctest::Approx(double(view.bottom)));
    f.key(PrAppKey_Home);
    f.frame();
    CHECK(scene.selection.lead == 0);
    CHECK(scene.scrollY() == 0.0f);
    CHECK(scene.rows.first == 0);
    CHECK(kitVerified(f.tree));
}

TEST_CASE("the page keys move the lead by the rows the view shows") {
    KitFixture f;
    ListScene scene(f);
    f.frame();
    f.frame();
    scene.clickRow(0);
    f.key(PrAppKey_PageDown);
    f.frame();
    CHECK(scene.selection.lead == 10);
    f.key(PrAppKey_PageDown, PrAppModifierFlag_Shift);
    f.frame();
    CHECK(scene.selection.lead == 20);
    CHECK(scene.selectedCount() == 11);
    f.key(PrAppKey_PageUp);
    f.frame();
    CHECK(scene.selection.lead == 10);
    CHECK(scene.selectedCount() == 1);
}

TEST_CASE("Space toggles the lead, and the accelerator's A selects every row") {
    KitFixture f;
    ListScene scene(f);
    scene.count = 50;
    f.frame();
    f.frame();
    scene.clickRow(1);
    f.key(' ');
    f.frame();
    CHECK(scene.selectedCount() == 0);
    f.key('A', PrAppModifierFlag_Super);
    f.frame();
    CHECK(scene.selectedCount() == 50);
    CHECK(scene.selection.anchor == 0);
    CHECK(scene.selection.lead == 49);
}

TEST_CASE("a list bound to no selection takes no focus and no keys") {
    KitFixture f;
    ListScene scene(f);
    scene.bound = false;
    f.frame();
    f.frame();
    scene.clickRow(2);
    CHECK(prUiFocus(f.ui) == kPrLayoutNodeNone);
    CHECK(scene.selectedCount() == 0);
    f.key(PrAppKey_Down);
    f.frame();
    CHECK(scene.selectedCount() == 0);
}

TEST_CASE("an edit replaces, adds or toggles its rows, clipped to the set") {
    bool selected[6] = {true, false, true, false, false, false};
    PrKitSelectionEdit toggle{.op = PrKitSelectionOp_Toggle, .first = 1, .count = 2};
    prKitSelectionApply(&toggle, selected, 6);
    CHECK(selected[0]);
    CHECK(selected[1]);
    CHECK_FALSE(selected[2]);
    PrKitSelectionEdit add{.op = PrKitSelectionOp_Add, .first = 4, .count = 10};
    prKitSelectionApply(&add, selected, 6);
    CHECK(selected[0]);
    CHECK(selected[5]);
    PrKitSelectionEdit replace{.op = PrKitSelectionOp_Replace, .first = 3, .count = 1};
    prKitSelectionApply(&replace, selected, 6);
    uint32_t total = 0;
    for (bool held : selected)
        total += held ? 1 : 0;
    CHECK(total == 1);
    CHECK(selected[3]);
}

TEST_CASE("a click of a disclosure opens its row and selects nothing") {
    KitFixture f;
    TreeScene scene(f);
    f.frame();
    f.frame();
    REQUIRE(scene.items.size() == 3);
    f.click(f.center(prLayoutNodeFirstChild(f.tree, scene.nodes[1])));
    f.frame();
    CHECK(scene.fold.row == 1);
    CHECK(scene.fold.open);
    CHECK(scene.open[1]);
    CHECK(scene.items.size() == 5);
    CHECK(scene.selectedId == 0);
    f.click(f.center(prLayoutNodeFirstChild(f.tree, scene.nodes[1])));
    f.frame();
    CHECK_FALSE(scene.open[1]);
    CHECK(scene.items.size() == 3);
    CHECK(kitVerified(f.tree));
}

TEST_CASE("a tree indents its rows by depth, and lines up leaves with their parents' cells") {
    KitFixture f;
    TreeScene scene(f);
    scene.open[0] = true;
    f.frame();
    f.frame();
    PrRect parent = f.bounds(prLayoutNodeFirstChild(f.tree, scene.nodes[0]));
    PrRect leaf = f.bounds(prLayoutNodeFirstChild(f.tree, scene.nodes[1]));
    CHECK(leaf.left - parent.left == doctest::Approx(double(kPrKitSpace6)));
    CHECK(f.bounds(scene.nodes[1]).right == doctest::Approx(double(f.bounds(scene.nodes[0]).right)));
}

TEST_CASE("Right opens a row and steps into it, Left climbs to its parent and closes it") {
    KitFixture f;
    TreeScene scene(f);
    f.frame();
    f.frame();
    f.click(f.center(scene.nodes[0]));
    f.frame();
    REQUIRE(scene.selectedId == 10);
    f.key(PrAppKey_Right);
    f.frame();
    CHECK(scene.open[0]);
    CHECK(scene.items.size() == 5);
    f.key(PrAppKey_Right);
    f.frame();
    CHECK(scene.selection.lead == 1);
    CHECK(scene.selectedId == 11);
    f.key(PrAppKey_Down);
    f.frame();
    CHECK(scene.selectedId == 12);
    f.key(PrAppKey_Left);
    f.frame();
    CHECK(scene.selectedId == 10);
    f.key(PrAppKey_Left);
    f.frame();
    CHECK_FALSE(scene.open[0]);
    CHECK(scene.items.size() == 3);
    CHECK(kitVerified(f.tree));
}

TEST_CASE("a row dragged onto another reports where it lands, by the third the pointer is over") {
    struct Case {
        float at;
        PrKitDropZone zone;
    };
    const Case cases[] = {{0.15f, PrKitDropZone_Before}, {0.5f, PrKitDropZone_Inside}, {0.85f, PrKitDropZone_After}};
    for (const Case& c : cases) {
        KitFixture f;
        TreeScene scene(f);
        scene.open[0] = true;
        f.frame();
        f.frame();
        uint32_t source = scene.indexOf(11);
        uint32_t target = scene.indexOf(20);
        PrRect box = f.bounds(scene.nodes[target]);
        PrVec2 over{f.center(scene.nodes[target]).x, box.top + (box.bottom - box.top) * c.at};
        f.push(PrAppEvent{.kind = PrAppEventKind_PointerDown, .pointer = {.position = f.center(scene.nodes[source])}});
        f.frame();
        f.move(over);
        f.frame();
        REQUIRE(prUiDragging(f.ui) != nullptr);
        f.frame();
        // The target is marked where the drop would land. The mark affects only painting.
        CHECK(prLayoutTreeStats(f.tree).blocksLaidOut == 0);
        f.push(PrAppEvent{.kind = PrAppEventKind_PointerUp, .pointer = {.position = over}});
        f.frame();
        CHECK(scene.drop.zone == c.zone);
        CHECK(scene.drop.item == 11);
        CHECK(scene.drop.row == target);
        // The release that drops selects nothing.
        CHECK(scene.selectedId == 0);
        CHECK(kitVerified(f.tree));
    }
}

TEST_CASE("a drop target's row is marked while a row is dragged over it") {
    KitFixture f;
    TreeScene scene(f);
    f.frame();
    f.frame();
    PrLayoutStyleIndex rest = prLayoutNodeStyle(f.tree, scene.nodes[2]);
    f.push(PrAppEvent{.kind = PrAppEventKind_PointerDown, .pointer = {.position = f.center(scene.nodes[0])}});
    f.frame();
    f.move(f.center(scene.nodes[2]));
    f.frame();
    f.frame();
    CHECK(prLayoutNodeStyle(f.tree, scene.nodes[2]) != rest);
    f.key(PrAppKey_Escape);
    f.frame();
    f.frame();
    CHECK(prUiDragging(f.ui) == nullptr);
    CHECK(scene.drop.zone == PrKitDropZone_None);
}

TEST_CASE("a visibility toggle shows while its row is hovered or hidden, and a click toggles it") {
    KitFixture f;
    TreeScene scene(f);
    f.frame();
    f.frame();
    uint32_t bare = f.paintedCommands();
    f.move(f.center(scene.nodes[0]));
    f.frame();
    f.frame();
    CHECK(f.paintedCommands() > bare);
    f.click(f.center(scene.toggles[0]));
    f.frame();
    CHECK(scene.toggled == 1);
    CHECK(scene.hidden[0]);
    CHECK(scene.selectedId == 0);
    f.move(PrVec2{790, 590});
    f.frame();
    f.frame();
    CHECK(f.paintedCommands() > bare);
    CHECK(kitVerified(f.tree));
}

TEST_CASE("an empty state shows its icon over its title and hint") {
    KitFixture f;
    PrLayoutNode column = kPrLayoutNodeNone;
    f.build = [&](PrLayoutNode root) {
        column = prKitEmptyState(f.kit, root,
            {.icon = PrKitIconKind_Search, .title = "NO ASSETS", .hint = "Clear the filter to see them all"});
    };
    f.frame();
    PrRect box = f.bounds(column);
    CHECK(box.bottom - box.top == doctest::Approx(600.0));
    CHECK(f.paintedCommands() > 3);
    CHECK(kitVerified(f.tree));
}

TEST_CASE("a table's columns line up under their headings, and a fill column takes the room that the others leave") {
    KitFixture f;
    TableScene scene(f);
    f.frame();
    for (uint32_t c = 0; c < 3; ++c) {
        PrRect heading = f.bounds(scene.heading(c));
        PrRect cell = f.bounds(scene.cells[0][c]);
        CHECK(cell.left == doctest::Approx(heading.left));
        CHECK(cell.right == doctest::Approx(heading.right));
    }
    CHECK(prRectWidth(f.bounds(scene.cells[0][1])) == doctest::Approx(80.0));
    CHECK(prRectWidth(f.bounds(scene.cells[0][2])) == doctest::Approx(60.0));
    PrRect row = f.bounds(prLayoutNodeParent(f.tree, scene.cells[0][0]));
    CHECK(f.bounds(scene.cells[0][2]).right < row.right);
    CHECK(prRectWidth(f.bounds(scene.cells[0][0])) > 120.0f);
    // The first row starts under the header.
    CHECK(row.top >= f.bounds(scene.heading(0)).bottom);
    CHECK(kitVerified(f.tree));
}

TEST_CASE("a click on a sortable heading sorts by its column, and a second click reverses the order") {
    KitFixture f;
    TableScene scene(f);
    f.frame();
    f.click(f.center(scene.heading(2)));
    f.frame();
    CHECK(scene.sort.column == kPrKitColumnNone);
    f.click(f.center(scene.heading(0)));
    f.frame();
    CHECK(scene.sort.column == 0);
    CHECK_FALSE(scene.sort.descending);
    CHECK(scene.sorts == 1);
    f.click(f.center(scene.heading(0)));
    f.frame();
    CHECK(scene.sort.descending);
    f.click(f.center(scene.heading(1)));
    f.frame();
    CHECK(scene.sort.column == 1);
    CHECK_FALSE(scene.sort.descending);
    CHECK(scene.sorts == 3);
    CHECK(kitVerified(f.tree));
}

TEST_CASE("columns wider than a table scroll it sideways, and the header scrolls with them") {
    KitFixture f;
    TableScene scene(f);
    scene.width = 200;
    scene.limited = true;
    f.frame();
    PrLayoutNode view = scene.scroller();
    float before = f.bounds(scene.cells[0][0]).left;
    f.move(f.center(view));
    f.push(PrAppEvent{.kind = PrAppEventKind_Scroll, .scroll = {.position = f.center(view), .delta = PrVec2{40, 0}}});
    f.frame();
    float scrolled = prLayoutUnitToPx(prLayoutNodeScroll(f.tree, view).x);
    CHECK(scrolled > 0.0f);
    CHECK(f.bounds(scene.cells[0][0]).left == doctest::Approx(before - scrolled));
    uint32_t moved = f.paintedCommands();
    (void)moved;
    for (uint32_t c = 0; c < 3; ++c) {
        CHECK(f.bounds(scene.cells[scene.rows.first][c]).left == doctest::Approx(f.bounds(scene.heading(c)).left));
    }
    CHECK(kitVerified(f.tree));
}

TEST_CASE("a table's window holds the rows that its scrollport shows under its header") {
    KitFixture f;
    TableScene scene(f);
    f.frame();
    f.frame();
    // The header takes a row's height of the view, so nine rows show, and the window adds its overscan past them.
    float port = kViewHeight - kRowHeight;
    uint32_t shown = uint32_t(port / kRowHeight) + 1;
    CHECK(scene.rows.first == 0);
    CHECK(scene.rows.count >= shown);
    CHECK(scene.rows.count <= shown + 2);
    CHECK(kitVerified(f.tree));
}

TEST_CASE("a grid's tiles fill rows of as many as fit, and its window holds whole rows") {
    KitFixture f;
    GridScene scene(f);
    f.frame();
    f.frame();
    CHECK(scene.rows.first == 0);
    CHECK(scene.rows.count % 4 == 0);
    PrRect first = f.bounds(scene.tiles[0]);
    PrRect fourth = f.bounds(scene.tiles[3]);
    PrRect fifth = f.bounds(scene.tiles[4]);
    CHECK(prRectWidth(first) == doctest::Approx(102.5));
    CHECK(fourth.top == doctest::Approx(first.top));
    CHECK(fifth.top == doctest::Approx(first.top + 80));
    CHECK(fifth.left == doctest::Approx(first.left));
    CHECK(kitVerified(f.tree));
}

TEST_CASE("a tile's image fills its thumbnail in place of its icon") {
    KitFixture f;
    std::vector<uint8_t> pixels(16 * 8 * 4, 255);
    PrDrawImageDesc desc{
        PrPixelView{pixels.data(), 16 * 4, {16, 8}}, PrPixelFormat_Rgba8, PrDrawAlpha_Straight,
        PrColorSpace_Srgb,
    };
    const PrDrawImage* image = nullptr;
    REQUIRE(prDrawImageCreate(&desc, &image) == PrStatus_Ok);
    PrLayoutNode tile = kPrLayoutNodeNone;
    f.build = [&](PrLayoutNode root) {
        PrKitListViewStyle style{
            .rowHeight = 80,
            .tileWidth = 100,
            .place = {.width = prKitPx(200), .height = prKitPx(200), .flex = PrKitFlex_Fixed},
        };
        PrLayoutNode view = prKitListView(f.kit, root, {.count = 1}, style);
        tile = prKitListTile(f.kit, view, {.index = 0, .item = 7});
        prKitTileImage(f.kit, tile, {.image = image}, {});
        prKitTileLabel(f.kit, tile, {.text = "fox.png"});
    };
    f.frame();
    f.frame();
    PrLayoutNode thumb = prLayoutNodeFirstChild(f.tree, tile);
    PrLayoutNode shown = prLayoutNodeFirstChild(f.tree, thumb);
    REQUIRE(shown != kPrLayoutNodeNone);
    CHECK(prLayoutNodeContent(f.tree, shown).image == image);
    CHECK(prLayoutNodeNextSibling(f.tree, shown) == kPrLayoutNodeNone);
    PrRect thumbBounds = f.bounds(thumb);
    PrRect imageBounds = f.bounds(shown);
    CHECK(prRectWidth(imageBounds) == doctest::Approx(prRectWidth(thumbBounds)));
    CHECK(prRectHeight(imageBounds) == doctest::Approx(prRectHeight(thumbBounds)));
    CHECK(kitVerified(f.tree));
    f.build = nullptr;
    f.frame();
    prDrawImageRelease(image);
}

TEST_CASE("a tile's image grows inside its thumbnail under the pointer, and a tile answers a secondary click") {
    KitFixture f;
    uint8_t pixels[4] = {255, 0, 0, 255};
    PrDrawImageDesc desc{
        .pixels = {pixels, 4, {1, 1}},
        .format = PrPixelFormat_Rgba8,
        .alpha = PrDrawAlpha_Straight,
        .space = PrColorSpace_Srgb,
    };
    const PrDrawImage* image = nullptr;
    REQUIRE(prDrawImageCreate(&desc, &image) == PrStatus_Ok);
    PrLayoutNode tile = kPrLayoutNodeNone;
    bool secondary = false;
    f.build = [&](PrLayoutNode root) {
        PrKitListViewStyle style{
            .rowHeight = 80,
            .tileWidth = 100,
            .place = {.width = prKitPx(200), .height = prKitPx(200), .flex = PrKitFlex_Fixed},
        };
        PrLayoutNode view = prKitListView(f.kit, root, {.key = 1, .count = 1}, style);
        tile = prKitListTile(f.kit, view, {.index = 0, .item = 7});
        prKitTileImage(f.kit, tile, {.image = image}, {.hoverZoom = 0.5f});
        secondary = prUiClicked(f.ui, tile, 1) || secondary;
    };
    f.frame();
    f.frame();
    PrLayoutNode thumb = prLayoutNodeFirstChild(f.tree, tile);
    PrRect before = f.bounds(thumb);
    CHECK(prRectWidth(f.bounds(prLayoutNodeFirstChild(f.tree, thumb))) == doctest::Approx(prRectWidth(before)));

    PrVec2 at = f.center(tile);
    f.move(at);
    for (int i = 0; i < 30; ++i)
        f.frame();
    thumb = prLayoutNodeFirstChild(f.tree, tile);
    PrRect grown = f.bounds(prLayoutNodeFirstChild(f.tree, thumb));
    CHECK(prRectWidth(f.bounds(thumb)) == doctest::Approx(prRectWidth(before)));
    CHECK(prRectWidth(grown) == doctest::Approx(prRectWidth(before) * 1.5f));
    // It grows evenly past the thumbnail's edges.
    CHECK((grown.left + grown.right) / 2 == doctest::Approx((before.left + before.right) / 2));

    f.push(PrAppEvent{.kind = PrAppEventKind_PointerDown, .pointer = {.position = at, .button = 1}});
    f.push(PrAppEvent{.kind = PrAppEventKind_PointerUp, .pointer = {.position = at, .button = 1}});
    f.frame();
    CHECK(secondary);

    f.build = nullptr;
    f.frame();
    prDrawImageRelease(image);
}

TEST_CASE("in a grid, Up and Down move the lead by a row of tiles, and Left and Right by a tile") {
    KitFixture f;
    GridScene scene(f);
    f.frame();
    f.frame();
    f.click(f.center(scene.tiles[1]));
    f.frame();
    CHECK(scene.selected[1]);
    f.key(PrAppKey_Down);
    f.frame();
    CHECK(scene.selection.lead == 5);
    CHECK(scene.selected[5]);
    CHECK_FALSE(scene.selected[1]);
    f.key(PrAppKey_Right);
    f.frame();
    CHECK(scene.selection.lead == 6);
    f.key(PrAppKey_Up, PrAppModifierFlag_Shift);
    f.frame();
    CHECK(scene.selection.lead == 2);
    CHECK(scene.selected[2]);
    CHECK(scene.selected[6]);
    CHECK(kitVerified(f.tree));
}
