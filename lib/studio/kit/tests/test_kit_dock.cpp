#include "pr/array.h"
#include "pr/kit_dock.h"
#include "pr/kit_text.h"

#include "tools/harness/kit_fixture.h"

#include "tools/harness/test.h"

using namespace harness;

namespace pr_impl {

namespace {

// The panels of the dock.
constexpr PrKitDockPanelId kScene = 1;
constexpr PrKitDockPanelId kViewport = 2;
constexpr PrKitDockPanelId kInspector = 3;
constexpr PrKitDockPanelId kProject = 4;
constexpr PrKitDockPanelId kConsole = 5;

PrKitDockTab describe(void*, PrKitDockPanelId panel) {
    static const char* const kTitles[] = {"", "Scene", "Viewport", "Inspector", "Project", "Console"};
    return PrKitDockTab{.title = kTitles[panel], .dirty = panel == kConsole};
}

// Seeds the editor's layout: Scene over Project, Viewport over Console, and Inspector, in three columns.
void seed(PrKitDock* dock) {
    PrKitDockNode scene = prKitDockPrimary(dock);
    prKitDockAddPanel(dock, scene, kScene);
    PrKitDockNode viewport = prKitDockSplit(dock, scene, PrKitDockEdge_Right, kViewport, 0.5f);
    prKitDockSplit(dock, viewport, PrKitDockEdge_Right, kInspector, 0.5f);
    prKitDockSplit(dock, scene, PrKitDockEdge_Bottom, kProject, 0.5f);
    prKitDockSplit(dock, viewport, PrKitDockEdge_Bottom, kConsole, 0.5f);
    PrKitDockNode root = prKitDockRoot(dock);
    const float columns[] = {0.25f, 0.5f, 0.25f};
    prKitDockSplitSetWeights(dock, root, columns, 3);
}

// A fixture whose frames declare a seeded dock and fill each pane with a label.
struct DockFixture : KitFixture {
    PrKitDock* dock = prKitDockCreate();
    PrLayoutNode area = kPrLayoutNodeNone;
    Array<PrKitDockPane> panes;

    DockFixture() {
        seed(dock);
        build = [this](PrLayoutNode root) {
            area = prKitDockBuild(kit, root, {.dock = dock, .describe = describe});
            panes.clear();
            for (uint32_t i = 0; i < prKitDockPaneCount(dock); ++i) {
                PrKitDockPane pane = prKitDockPane(dock, i);
                panes.push(pane);
                prKitLabel(kit, pane.content, {.text = describe(nullptr, pane.panel).title}, {});
            }
        };
    }
    ~DockFixture() { prKitDockDestroy(dock); }

    PrLayoutNode content(PrKitDockPanelId panel) {
        for (const PrKitDockPane& pane : panes) {
            if (pane.panel == panel)
                return pane.content;
        }
        return kPrLayoutNodeNone;
    }

    // Drags with the primary button from `from` to `to`, over frames, and releases.
    void drag(PrVec2 from, PrVec2 to) {
        push(PrAppEvent{.kind = PrAppEventKind_PointerMove, .pointer = {.position = from}});
        push(PrAppEvent{.kind = PrAppEventKind_PointerDown, .pointer = {.position = from}});
        frame();
        move(PrVec2{from.x + 10, from.y + 10});
        frame();
        move(to);
        frame();
        frame();
        push(PrAppEvent{.kind = PrAppEventKind_PointerUp, .pointer = {.position = to}});
        frame();
    }
};

float width(const PrRect& r) {
    return r.right - r.left;
}

float height(const PrRect& r) {
    return r.bottom - r.top;
}

PrKitDockTab describeLeaf(void*, PrKitDockPanelId) {
    return PrKitDockTab{.title = "Tab"};
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a split leaf becomes a split of two leaves, and a split along its parent's axis joins the parent") {
    PrKitDock* dock = prKitDockCreate();
    seed(dock);
    PrKitDockNode root = prKitDockRoot(dock);
    CHECK(prKitDockNodeKind(dock, root) == PrKitDockKind_Split);
    CHECK(prKitDockSplitDirection(dock, root) == PrKitDirection_Row);
    REQUIRE(prKitDockSplitChildCount(dock, root) == 3);
    CHECK(prKitDockSplitWeight(dock, root, 1) == doctest::Approx(0.5));
    PrKitDockNode left = prKitDockSplitChild(dock, root, 0);
    CHECK(prKitDockSplitDirection(dock, left) == PrKitDirection_Column);
    CHECK(prKitDockSplitChildCount(dock, left) == 2);
    CHECK(prKitDockPanelLeaf(dock, kProject) == prKitDockSplitChild(dock, left, 1));
    CHECK(prKitDockNodeParent(dock, prKitDockPanelLeaf(dock, kInspector)) == root);
    prKitDockDestroy(dock);
}

TEST_CASE("removing a leaf's last panel removes the leaf, and a split left with one child gives way to it") {
    PrKitDock* dock = prKitDockCreate();
    seed(dock);
    PrKitDockNode root = prKitDockRoot(dock);
    PrKitDockNode project = prKitDockPanelLeaf(dock, kProject);
    CHECK(prKitDockRemovePanel(dock, kProject));
    CHECK_FALSE(prKitDockNodeIsValid(dock, project));
    CHECK(prKitDockSplitChild(dock, root, 0) == prKitDockPanelLeaf(dock, kScene));
    CHECK_FALSE(prKitDockRemovePanel(dock, kProject));
    // The primary leaf stays, empty.
    CHECK(prKitDockRemovePanel(dock, kScene));
    CHECK(prKitDockNodeIsValid(dock, prKitDockPrimary(dock)));
    CHECK(prKitDockLeafTabCount(dock, prKitDockPrimary(dock)) == 0);
    prKitDockDestroy(dock);
}

TEST_CASE("a panel moved to the dock's edge along the root's axis takes an average share") {
    PrKitDock* dock = prKitDockCreate();
    seed(dock);
    prKitDockMoveToEdge(dock, kConsole, PrKitDockEdge_Right);
    PrKitDockNode root = prKitDockRoot(dock);
    REQUIRE(prKitDockSplitChildCount(dock, root) == 4);
    CHECK(prKitDockSplitChild(dock, root, 3) == prKitDockPanelLeaf(dock, kConsole));
    float total = 0;
    for (uint32_t i = 0; i < 4; ++i)
        total += prKitDockSplitWeight(dock, root, i);
    CHECK(total == doctest::Approx(1));
    // The column that Console left now holds only Viewport's leaf.
    CHECK(prKitDockNodeKind(dock, prKitDockSplitChild(dock, root, 1)) == PrKitDockKind_Leaf);
    prKitDockDestroy(dock);
}

TEST_CASE("a panel moved to another leaf's tabs opens there, and moving within a strip reorders it") {
    PrKitDock* dock = prKitDockCreate();
    seed(dock);
    PrKitDockNode inspector = prKitDockPanelLeaf(dock, kInspector);
    prKitDockMoveToTab(dock, kConsole, inspector, 0);
    CHECK(prKitDockLeafTab(dock, inspector, 0) == kConsole);
    CHECK(prKitDockLeafActiveTab(dock, inspector) == 0);
    prKitDockMoveToTab(dock, kConsole, inspector, 2);
    CHECK(prKitDockLeafTab(dock, inspector, 0) == kInspector);
    CHECK(prKitDockLeafTab(dock, inspector, 1) == kConsole);
    prKitDockDestroy(dock);
}

TEST_CASE("a seeded dock lays out its columns by their weights") {
    DockFixture f;
    f.frame();
    REQUIRE(f.panes.count() == 5);
    double free = 800 - 2 * double(kPrKitSplitBand);
    CHECK(width(f.bounds(f.content(kScene))) == doctest::Approx(free * 0.25).epsilon(0.01));
    CHECK(width(f.bounds(f.content(kViewport))) == doctest::Approx(free * 0.5).epsilon(0.01));
    CHECK(width(f.bounds(f.content(kInspector))) == doctest::Approx(free * 0.25).epsilon(0.01));
    CHECK(f.bounds(f.content(kViewport)).left > f.bounds(f.content(kScene)).right);
    CHECK(f.bounds(f.content(kProject)).top > f.bounds(f.content(kScene)).bottom);
    CHECK(height(f.bounds(f.area)) == 600.0f);
    CHECK(kitVerified(f.tree));
}

TEST_CASE("a frame of an unchanged dock makes no node and lays nothing out") {
    DockFixture f;
    f.frame();
    f.frame();
    PrLayoutTreeStats stats = prLayoutTreeStats(f.tree);
    CHECK(stats.frameNodesMade == 0);
    CHECK(stats.blocksLaidOut == 0);
    f.move(PrVec2{400, 300});
    f.frame();
    f.frame();
    stats = prLayoutTreeStats(f.tree);
    CHECK(stats.frameNodesMade == 0);
    CHECK(stats.blocksLaidOut == 0);
    CHECK(kitVerified(f.tree));
}

TEST_CASE("dragging a band moves weight between the panes beside it") {
    DockFixture f;
    f.frame();
    PrRect scene = f.bounds(f.content(kScene));
    PrRect viewport = f.bounds(f.content(kViewport));
    PrVec2 band{(scene.right + viewport.left) / 2, 300};
    f.move(band);
    f.frame();
    CHECK(prUiOutput(f.ui).cursor == PrAppCursor_ResizeX);
    f.push(PrAppEvent{.kind = PrAppEventKind_PointerDown, .pointer = {.position = band}});
    f.frame();
    f.move(PrVec2{band.x + 79, 300});
    f.frame();
    f.push(PrAppEvent{.kind = PrAppEventKind_PointerUp, .pointer = {.position = PrVec2{band.x + 79, 300}}});
    f.frame();
    f.frame();
    PrKitDockNode root = prKitDockRoot(f.dock);
    double free = 800 - 2 * double(kPrKitSplitBand);
    CHECK(prKitDockSplitWeight(f.dock, root, 0) == doctest::Approx(0.25 + 79 / free).epsilon(0.001));
    CHECK(prKitDockSplitWeight(f.dock, root, 1) == doctest::Approx(0.5 - 79 / free).epsilon(0.001));
    CHECK(prKitDockSplitWeight(f.dock, root, 2) == doctest::Approx(0.25));
    CHECK(width(f.bounds(f.content(kScene))) == doctest::Approx(double(width(scene)) + 79).epsilon(0.01));
    CHECK(kitVerified(f.tree));
}

TEST_CASE("a band dragged past its neighbour leaves it the minimum share") {
    DockFixture f;
    f.frame();
    PrRect scene = f.bounds(f.content(kScene));
    PrRect viewport = f.bounds(f.content(kViewport));
    PrVec2 band{(scene.right + viewport.left) / 2, 300};
    f.push(PrAppEvent{.kind = PrAppEventKind_PointerMove, .pointer = {.position = band}});
    f.push(PrAppEvent{.kind = PrAppEventKind_PointerDown, .pointer = {.position = band}});
    f.frame();
    f.move(PrVec2{790, 300});
    f.frame();
    f.push(PrAppEvent{.kind = PrAppEventKind_PointerUp, .pointer = {.position = PrVec2{790, 300}}});
    f.frame();
    PrKitDockNode root = prKitDockRoot(f.dock);
    CHECK(prKitDockSplitWeight(f.dock, root, 1) == doctest::Approx(0.04).epsilon(0.001));
    CHECK(prKitDockSplitWeight(f.dock, root, 0) == doctest::Approx(0.71).epsilon(0.001));
    CHECK(prKitDockSplitWeight(f.dock, root, 2) == doctest::Approx(0.25));
    CHECK(kitVerified(f.tree));
}

TEST_CASE("a dragged band lays out the panes it resizes, and a frame after it nothing") {
    DockFixture f;
    f.frame();
    PrRect scene = f.bounds(f.content(kScene));
    PrVec2 band{scene.right + kPrKitSplitBand / 2, 300};
    f.push(PrAppEvent{.kind = PrAppEventKind_PointerMove, .pointer = {.position = band}});
    f.push(PrAppEvent{.kind = PrAppEventKind_PointerDown, .pointer = {.position = band}});
    f.frame();
    f.move(PrVec2{band.x + 20, 300});
    f.frame();
    CHECK(prLayoutTreeStats(f.tree).frameNodesMade == 0);
    f.frame();
    CHECK(prLayoutTreeStats(f.tree).blocksLaidOut == 0);
    CHECK(kitVerified(f.tree));
}

TEST_CASE("clicking a tab opens its panel") {
    DockFixture f;
    PrKitDockNode inspector = prKitDockPanelLeaf(f.dock, kInspector);
    prKitDockAddPanel(f.dock, inspector, kProject + 100);
    f.build = [&](PrLayoutNode root) {
        prKitDockBuild(f.kit, root, {.dock = f.dock, .describe = describeLeaf});
    };
    // The second tab is open, and the first tab sits at the strip's start.
    f.frame();
    CHECK(prKitDockLeafActiveTab(f.dock, inspector) == 1);
    PrKitDockPane pane = prKitDockPane(f.dock, 4);
    REQUIRE(pane.leaf == inspector);
    PrRect content = f.bounds(pane.content);
    f.click(PrVec2{content.left + 10, content.top - 11});
    f.frame();
    CHECK(prKitDockLeafActiveTab(f.dock, inspector) == 0);
    CHECK(prKitDockPane(f.dock, 4).panel == kInspector);
    CHECK(kitVerified(f.tree));
}

TEST_CASE("closing a tab removes its panel at the next build") {
    DockFixture f;
    f.frame();
    // The close button ends the Inspector's lone tab, which is narrower than its strip.
    PrRect content = f.bounds(f.content(kInspector));
    PrVec2 tab{content.left + 10, content.top - 11};
    f.move(tab);
    f.frame();
    // The close button is the tab's square press target, found by hit testing along the strip.
    Array<PrLayoutNode> hits(32);
    PrVec2 close{};
    bool found = false;
    for (float x = content.left + 20; x < content.right && !found; x += 1) {
        PrLayoutVec at{prLayoutUnitFromPx(x), prLayoutUnitFromPx(tab.y)};
        uint32_t count = prLayoutTreeHitTest(f.tree, at, hits.data(), hits.count());
        for (uint32_t i = 0; i < count && !found; ++i) {
            if ((prUiFlags(f.ui, hits[i]) & PrUiFlag_Focusable) != 0 &&
                    width(f.bounds(hits[i])) == doctest::Approx(double(kPrKitHeightXs))) {
                close = f.center(hits[i]);
                found = true;
            }
        }
    }
    REQUIRE(found);
    f.click(close);
    f.frame();
    CHECK(prKitDockPanelLeaf(f.dock, kInspector) == kPrKitDockNodeNone);
    CHECK(prKitDockSplitChildCount(f.dock, prKitDockRoot(f.dock)) == 2);
    CHECK(f.panes.count() == 4);
    CHECK(kitVerified(f.tree));
}

TEST_CASE("a tab dragged onto the middle of another leaf joins its tabs, keeping its content's node") {
    DockFixture f;
    f.frame();
    PrLayoutNode console = f.content(kConsole);
    PrRect consoleBox = f.bounds(console);
    PrVec2 from{consoleBox.left + 10, consoleBox.top - 11};
    PrRect inspector = f.bounds(f.content(kInspector));
    PrVec2 to{(inspector.left + inspector.right) / 2, (inspector.top + inspector.bottom) / 2};
    f.push(PrAppEvent{.kind = PrAppEventKind_PointerMove, .pointer = {.position = from}});
    f.push(PrAppEvent{.kind = PrAppEventKind_PointerDown, .pointer = {.position = from}});
    f.frame();
    f.move(PrVec2{from.x + 10, from.y + 10});
    f.frame();
    f.move(to);
    f.frame();
    f.frame();
    REQUIRE(prUiDragging(f.ui) != nullptr);
    CHECK(prUiDragging(f.ui)->kind == kPrKitDockDragKind);
    CHECK(prUiDragging(f.ui)->value == kConsole);
    // The model holds until the drop.
    CHECK(prKitDockPanelLeaf(f.dock, kConsole) != prKitDockPanelLeaf(f.dock, kInspector));
    f.push(PrAppEvent{.kind = PrAppEventKind_PointerUp, .pointer = {.position = to}});
    f.frame();
    PrKitDockNode leaf = prKitDockPanelLeaf(f.dock, kInspector);
    CHECK(prKitDockPanelLeaf(f.dock, kConsole) == leaf);
    CHECK(prKitDockLeafTabCount(f.dock, leaf) == 2);
    CHECK(prKitDockLeafTab(f.dock, leaf, prKitDockLeafActiveTab(f.dock, leaf)) == kConsole);
    CHECK(f.content(kConsole) == console);
    CHECK(f.bounds(console).left == doctest::Approx(double(inspector.left)));
    CHECK(kitVerified(f.tree));
}

TEST_CASE("a tab in flight previews its drop in the overlay, and the preview goes with it") {
    DockFixture f;
    f.frame();
    uint32_t idle = f.paintedCommands();
    PrRect consoleBox = f.bounds(f.content(kConsole));
    PrVec2 from{consoleBox.left + 10, consoleBox.top - 11};
    PrRect inspector = f.bounds(f.content(kInspector));
    PrVec2 to{(inspector.left + inspector.right) / 2, (inspector.top + inspector.bottom) / 2};
    f.push(PrAppEvent{.kind = PrAppEventKind_PointerMove, .pointer = {.position = from}});
    f.push(PrAppEvent{.kind = PrAppEventKind_PointerDown, .pointer = {.position = from}});
    f.frame();
    f.move(to);
    f.frame();
    f.frame();
    CHECK(f.paintedCommands() > idle);
    f.key(PrAppKey_Escape);
    f.frame();
    f.frame();
    CHECK(prUiDragging(f.ui) == nullptr);
    CHECK(f.paintedCommands() == idle);
    CHECK(prKitDockPanelLeaf(f.dock, kConsole) != prKitDockPanelLeaf(f.dock, kInspector));
    CHECK(kitVerified(f.tree));
}

TEST_CASE("a tab dropped near a leaf's edge splits the leaf") {
    DockFixture f;
    f.frame();
    PrRect sceneBox = f.bounds(f.content(kScene));
    PrVec2 from{sceneBox.left + 10, sceneBox.top - 11};
    PrRect inspector = f.bounds(f.content(kInspector));
    PrVec2 to{(inspector.left + inspector.right) / 2, inspector.bottom - 70};
    f.drag(from, to);
    PrKitDockNode leaf = prKitDockPanelLeaf(f.dock, kScene);
    PrKitDockNode split = prKitDockNodeParent(f.dock, leaf);
    CHECK(prKitDockSplitDirection(f.dock, split) == PrKitDirection_Column);
    CHECK(prKitDockSplitChild(f.dock, split, 0) == prKitDockPanelLeaf(f.dock, kInspector));
    CHECK(prKitDockSplitChild(f.dock, split, 1) == leaf);
    CHECK(f.bounds(f.content(kScene)).top > f.bounds(f.content(kInspector)).bottom);
    CHECK(kitVerified(f.tree));
}

TEST_CASE("a tab dropped near the dock's own side takes that side") {
    DockFixture f;
    f.frame();
    PrRect sceneBox = f.bounds(f.content(kScene));
    PrVec2 from{sceneBox.left + 10, sceneBox.top - 11};
    f.drag(from, PrVec2{400, 590});
    PrKitDockNode root = prKitDockRoot(f.dock);
    CHECK(prKitDockSplitDirection(f.dock, root) == PrKitDirection_Column);
    REQUIRE(prKitDockSplitChildCount(f.dock, root) == 2);
    CHECK(prKitDockSplitChild(f.dock, root, 1) == prKitDockPanelLeaf(f.dock, kScene));
    CHECK(f.bounds(f.content(kScene)).top > 300.0f);
    CHECK(width(f.bounds(f.content(kScene))) == doctest::Approx(800.0));
    CHECK(kitVerified(f.tree));
}

TEST_CASE("a panel's content node keeps its handle and its subtree as the panel moves between leaves") {
    DockFixture f;
    f.frame();
    PrLayoutNode content = f.content(kProject);
    PrLayoutNode label = prLayoutNodeFirstChild(f.tree, content);
    prKitDockMoveToTab(f.dock, kProject, prKitDockPanelLeaf(f.dock, kInspector), 0);
    f.frame();
    CHECK(f.content(kProject) == content);
    CHECK(prLayoutNodeFirstChild(f.tree, content) == label);
    CHECK(f.bounds(content).left > 500.0f);
    CHECK(kitVerified(f.tree));
}

TEST_CASE("a split's panes divide its box by weight, and its band moves weight between them") {
    KitFixture f;
    float weights[2] = {1, 3};
    PrLayoutNode panes[2] = {};
    PrLayoutNode split = kPrLayoutNodeNone;
    f.build = [&](PrLayoutNode root) {
        PrKitSplitStyle style{.direction = PrKitDirection_Column, .minShare = 0.1f, .place = {.flex = PrKitFlex_Fill}};
        split = prKitSplit(f.kit, root, {.count = 2, .weights = weights}, style);
        panes[0] = prKitSplitPane(f.kit, split, 0);
        panes[1] = prKitSplitPane(f.kit, split, 1);
    };
    f.frame();
    double free = 600 - double(kPrKitSplitBand);
    CHECK(height(f.bounds(panes[0])) == doctest::Approx(free / 4).epsilon(0.01));
    CHECK(width(f.bounds(panes[1])) == 800.0f);
    PrVec2 band{400, f.bounds(panes[0]).bottom + kPrKitSplitBand / 2};
    f.move(band);
    f.frame();
    CHECK(prUiOutput(f.ui).cursor == PrAppCursor_ResizeY);
    f.push(PrAppEvent{.kind = PrAppEventKind_PointerDown, .pointer = {.position = band}});
    f.frame();
    f.move(PrVec2{400, 0});
    f.frame();
    f.push(PrAppEvent{.kind = PrAppEventKind_PointerUp, .pointer = {.position = PrVec2{400, 0}}});
    f.frame();
    CHECK(weights[0] == doctest::Approx(0.4));
    CHECK(weights[1] == doctest::Approx(3.6));
    CHECK(height(f.bounds(panes[0])) == doctest::Approx(free / 10).epsilon(0.01));
    f.frame();
    CHECK(prLayoutTreeStats(f.tree).blocksLaidOut == 0);
    CHECK(kitVerified(f.tree));
}

TEST_CASE("an empty primary leaf holds its room and takes a dropped tab") {
    DockFixture f;
    prKitDockMoveToTab(f.dock, kScene, prKitDockPanelLeaf(f.dock, kProject), 0);
    f.frame();
    PrKitDockNode primary = prKitDockPrimary(f.dock);
    CHECK(prKitDockNodeIsValid(f.dock, primary));
    CHECK(prKitDockLeafTabCount(f.dock, primary) == 0);
    CHECK(f.panes.count() == 4);
    PrRect projectBox = f.bounds(f.content(kScene));
    PrVec2 from{projectBox.left + 10, projectBox.top - 11};
    f.drag(from, PrVec2{100, 150});
    CHECK(prKitDockPanelLeaf(f.dock, kScene) == primary);
    CHECK(f.panes.count() == 5);
    CHECK(kitVerified(f.tree));
}
