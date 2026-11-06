// Studio Kit's readouts, chips and button groups.

#include "pr/kit_box.h"
#include "pr/kit_button.h"
#include "pr/kit_data.h"
#include "pr/layout_style.h"

#include "tools/harness/kit_fixture.h"

#include "tools/harness/test.h"

using namespace harness;

namespace pr_impl {

namespace {

// Returns the node's box group in the tree's last commit.
PrLayoutBoxStyle boxOf(KitFixture& f, PrLayoutNode node) {
    PrLayoutStyle record = prLayoutStyleRecord(f.tree, prLayoutNodeStyle(f.tree, node));
    return prLayoutBoxStyleRecord(f.tree, record.groups[PrLayoutGroup_Box]);
}

float widthOf(KitFixture& f, PrLayoutNode node) {
    PrRect r = f.bounds(node);
    return r.right - r.left;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a log line's message takes the room that its row leaves, and the same lines make nothing again") {
    KitFixture f;
    PrLayoutNode lines[2] = {};
    f.build = [&](PrLayoutNode root) {
        PrKitBoxStyle column{};
        column.place.width = prKitPx(400);
        column.place.flex = PrKitFlex_Fixed;
        PrLayoutNode log = prKitBox(f.kit, root, {}, column);
        PrKitLogLine info{.time = "12:04", .message = "Loaded", .source = "Scene"};
        lines[0] = prKitLogLine(f.kit, log, info);
        PrKitLogLine error{.message = "Failed", .level = PrKitLogLevel_Error};
        lines[1] = prKitLogLine(f.kit, log, error);
    };
    f.frame();
    CHECK(widthOf(f, lines[0]) == 400.0f);
    CHECK(widthOf(f, lines[1]) == 400.0f);
    // A line ends in a rule, and its height is at least a compact control's.
    CHECK(boxOf(f, lines[0]).border[PrRectSide_Bottom].raw == prLayoutUnitFromPx(1).raw);
    PrRect line = f.bounds(lines[0]);
    CHECK(line.bottom - line.top >= kPrKitHeightXs);
    f.frame();
    CHECK(prLayoutTreeStats(f.tree).frameNodesMade == 0);
    CHECK(prLayoutTreeStats(f.tree).blocksLaidOut == 0);
    CHECK(kitVerified(f.tree));
}

TEST_CASE("a stat tile stacks its caption, reading and note, and a tile without a note has two lines") {
    KitFixture f;
    PrLayoutNode tiles[2] = {};
    f.build = [&](PrLayoutNode root) {
        PrLayoutNode row = prKitBox(f.kit, root, {}, {.direction = PrKitDirection_Row, .gap = kPrKitSpace3});
        PrKitStatTile frame{.label = "FRAME", .value = "16.6 ms", .note = "60 fps target"};
        tiles[0] = prKitStatTile(f.kit, row, frame);
        PrKitStatTile bare{.label = "ERRORS", .value = "1"};
        tiles[1] = prKitStatTile(f.kit, row, bare);
    };
    f.frame();
    PrRect a = f.bounds(tiles[0]);
    PrRect b = f.bounds(tiles[1]);
    CHECK(a.bottom - a.top > 0.0f);
    CHECK(b.left > a.right);
    CHECK(kitVerified(f.tree));
}

TEST_CASE("a chip's remove control reports the removal, and a read-only chip has none") {
    KitFixture f;
    PrLayoutNode removable = kPrLayoutNodeNone;
    PrLayoutNode fixed = kPrLayoutNodeNone;
    uint32_t removals = 0;
    f.build = [&](PrLayoutNode root) {
        PrLayoutNode row = prKitBox(f.kit, root, {}, {.direction = PrKitDirection_Row, .gap = kPrKitSpace3});
        removable = prKitChip(f.kit, row, {.label = "player", .removable = true});
        removals += prKitRemoved(f.kit, removable) ? 1 : 0;
        fixed = prKitChip(f.kit, row, {.label = "static"});
    };
    f.frame();
    CHECK(widthOf(f, removable) > widthOf(f, fixed) - 20);
    // The remove control is the chip's last child, at its end.
    PrRect chip = f.bounds(removable);
    f.click(PrVec2{chip.right - 8, (chip.top + chip.bottom) / 2});
    f.frame();
    CHECK(removals == 1);
    CHECK(kitVerified(f.tree));
}

TEST_CASE("the buttons of a group draw no border of their own, and a button outside it does") {
    KitFixture f;
    PrLayoutNode grouped = kPrLayoutNodeNone;
    PrLayoutNode alone = kPrLayoutNodeNone;
    PrLayoutNode group = kPrLayoutNodeNone;
    f.build = [&](PrLayoutNode root) {
        PrLayoutNode row = prKitBox(f.kit, root, {}, {.direction = PrKitDirection_Row, .gap = kPrKitSpace3});
        group = prKitButtonGroup(f.kit, row, {});
        PrKitButton local{.text = "Local"};
        grouped = prKitButton(f.kit, group, local, {});
        PrKitButton world{.text = "World"};
        prKitButton(f.kit, group, world, {});
        alone = prKitButton(f.kit, row, local, {});
    };
    f.frame();
    CHECK(boxOf(f, grouped).border[PrRectSide_Left].raw == 0);
    CHECK(boxOf(f, alone).border[PrRectSide_Left].raw == prLayoutUnitFromPx(1).raw);
    CHECK(boxOf(f, group).border[PrRectSide_Left].raw == prLayoutUnitFromPx(1).raw);
    CHECK(kitVerified(f.tree));
}
