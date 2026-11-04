// Grid layout: a grid container's items, placed in its grid and sized with its tracks, and what each layout of them
// costs. Words are inline-blocks 20px tall of the widths given, in a font of size 0, with no white space between them,
// so that lines break between any two and are only as tall as their words.

#include "pr/layout_query.h"
#include "pr/layout_style.h"
#include "pr/layout_tree.h"

#include "layout/tests/layout_fixture.h"

#include "tools/harness/test.h"

#include <vector>

namespace pr_impl {

namespace {

// A track of a fixed size, a flex factor, an intrinsic size, a range, or fit-content.
PrLayoutGridTrack fixed(float px) {
    return PrLayoutGridTrack{prLayoutTrackBreadthPx(px), prLayoutTrackBreadthPx(px), prLayoutLengthPx(0)};
}
PrLayoutGridTrack percent(float value) {
    return PrLayoutGridTrack{
        prLayoutTrackBreadthPercent(value), prLayoutTrackBreadthPercent(value),
        prLayoutLengthPx(0),
    };
}
PrLayoutGridTrack fr(float factor) {
    return PrLayoutGridTrack{prLayoutTrackBreadthAuto(), prLayoutTrackBreadthFlex(factor), prLayoutLengthPx(0)};
}
PrLayoutGridTrack autoTrack() {
    return PrLayoutGridTrack{prLayoutTrackBreadthAuto(), prLayoutTrackBreadthAuto(), prLayoutLengthPx(0)};
}
PrLayoutGridTrack minContent() {
    return PrLayoutGridTrack{prLayoutTrackBreadthMinContent(), prLayoutTrackBreadthMinContent(), prLayoutLengthPx(0)};
}
PrLayoutGridTrack maxContent() {
    return PrLayoutGridTrack{prLayoutTrackBreadthMaxContent(), prLayoutTrackBreadthMaxContent(), prLayoutLengthPx(0)};
}
PrLayoutGridTrack minmax(PrLayoutTrackBreadth min, PrLayoutTrackBreadth max) {
    return PrLayoutGridTrack{min, max, prLayoutLengthPx(0)};
}
PrLayoutGridTrack fitContent(float px) {
    return PrLayoutGridTrack{prLayoutTrackBreadthAuto(), prLayoutTrackBreadthFitContent(), prLayoutLengthPx(px)};
}

// A grid-placement value: a line, a span, or auto.
PrLayoutGridLine line(int16_t integer, PrLayoutGridName name = kPrLayoutGridNameNone) {
    return PrLayoutGridLine{name, integer, PrLayoutGridLineKind_Line, 0};
}
PrLayoutGridLine span(int16_t integer, PrLayoutGridName name = kPrLayoutGridNameNone) {
    return PrLayoutGridLine{name, integer, PrLayoutGridLineKind_Span, 0};
}

// A tree whose root is in a font of size 0, with helpers for the boxes, words and grid groups that grid cases are made
// of.
struct Stage {
    Fixture f;
    PrLayoutFontStyle font = prLayoutFontStyleDefault();
    PrLayoutNode root = kPrLayoutNodeNone;

    Stage() {
        font.font = textFontOf(0);
        root = add(kPrLayoutNodeNone, block());
    }

    // Returns the record of a box with its grid and flex groups.
    uint32_t style(const PrLayoutBoxStyle& box, const PrLayoutGridStyle& grid = prLayoutGridStyleDefault(),
            const PrLayoutFlexStyle& flex = prLayoutFlexStyleDefault()) {
        PrLayoutStyle record = prLayoutStyleDefault();
        record.groups[PrLayoutGroup_Box] = prLayoutBoxStyleIntern(f.tree, &box);
        record.groups[PrLayoutGroup_Font] = prLayoutFontStyleIntern(f.tree, &font);
        record.groups[PrLayoutGroup_Flex] = prLayoutFlexStyleIntern(f.tree, &flex);
        record.groups[PrLayoutGroup_Grid] = prLayoutGridStyleIntern(f.tree, &grid);
        return prLayoutStyleIntern(f.tree, &record);
    }

    // Creates an element under `parent`.
    PrLayoutNode add(PrLayoutNode parent, const PrLayoutBoxStyle& box,
            const PrLayoutGridStyle& grid = prLayoutGridStyleDefault(),
            const PrLayoutFlexStyle& flex = prLayoutFlexStyleDefault()) {
        PrLayoutNode node = prLayoutNodeCreateElement(f.tree, parent, style(box, grid, flex));
        if (parent == kPrLayoutNodeNone)
            f.root = node;
        return node;
    }

    // Interns a track list of `tracks`, with the names given, by line.
    PrLayoutGridTracksIndex tracks(std::vector<PrLayoutGridTrack> list, std::vector<PrLayoutGridLineName> names = {}) {
        PrLayoutGridTrackList desc{
            .tracks = list.data(),
            .trackCount = uint32_t(list.size()),
            .names = names.data(),
            .nameCount = uint32_t(names.size()),
        };
        return prLayoutGridTracksIntern(f.tree, &desc);
    }

    // Interns a track list whose tracks from `start`, `count` of them, repeat automatically by `repeat`.
    PrLayoutGridTracksIndex repeating(std::vector<PrLayoutGridTrack> list, uint32_t start, uint32_t count,
            PrLayoutGridRepeat repeat) {
        PrLayoutGridTrackList desc{
            .tracks = list.data(),
            .trackCount = uint32_t(list.size()),
            .repeat = repeat,
            .repeatStart = start,
            .repeatCount = count,
        };
        return prLayoutGridTracksIntern(f.tree, &desc);
    }

    PrLayoutGridName name(const char* text) { return prLayoutGridNameIntern(f.tree, text); }

    // Creates a grid container under `parent`, `width` wide if given.
    PrLayoutNode grid(PrLayoutNode parent, const PrLayoutGridStyle& grid, float width = -1,
            const PrLayoutFlexStyle& flex = prLayoutFlexStyleDefault()) {
        PrLayoutBoxStyle box = block();
        box.display = PrLayoutDisplay_Grid;
        if (width >= 0)
            box.width = prLayoutLengthPx(width);
        return add(parent, box, grid, flex);
    }

    // Creates an item under `parent`, placed by `placement`, `width` by `height` if given.
    PrLayoutNode item(PrLayoutNode parent, const PrLayoutGridStyle& placement = prLayoutGridStyleDefault(),
            float width = -1, float height = -1, const PrLayoutFlexStyle& flex = prLayoutFlexStyleDefault()) {
        PrLayoutBoxStyle box = block();
        if (width >= 0)
            box.width = prLayoutLengthPx(width);
        if (height >= 0)
            box.height = prLayoutLengthPx(height);
        return add(parent, box, placement, flex);
    }

    // Creates words of the widths given, each 20px tall.
    void words(PrLayoutNode parent, std::vector<float> widths) {
        for (float width : widths) {
            PrLayoutBoxStyle box = block(20);
            box.display = PrLayoutDisplay_InlineBlock;
            box.width = prLayoutLengthPx(width);
            add(parent, box);
        }
    }

    void layout(float width = 600, float height = 400) {
        f.layout(px(width, height));
        CHECK(verified(f.tree));
    }

    PrLayoutTreeStats stats() { return prLayoutTreeStats(f.tree); }

    PrRect at(PrLayoutNode node) { return f.bounds(node); }
    float left(PrLayoutNode node) { return at(node).left; }
    float top(PrLayoutNode node) { return at(node).top; }
    float width(PrLayoutNode node) { return at(node).right - at(node).left; }
    float height(PrLayoutNode node) { return at(node).bottom - at(node).top; }
};

// Returns a placement in both axes: rows `rowStart` to `rowEnd`, and columns `columnStart` to `columnEnd`.
PrLayoutGridStyle area(PrLayoutGridLine rowStart, PrLayoutGridLine columnStart, PrLayoutGridLine rowEnd,
        PrLayoutGridLine columnEnd) {
    PrLayoutGridStyle grid = prLayoutGridStyleDefault();
    grid.rowStart = rowStart;
    grid.columnStart = columnStart;
    grid.rowEnd = rowEnd;
    grid.columnEnd = columnEnd;
    return grid;
}

// Returns a placement in the columns alone.
PrLayoutGridStyle columns(PrLayoutGridLine start, PrLayoutGridLine end = PrLayoutGridLine{}) {
    PrLayoutGridStyle grid = prLayoutGridStyleDefault();
    grid.columnStart = start;
    grid.columnEnd = end;
    return grid;
}

// Returns a placement in the rows alone.
PrLayoutGridStyle rows(PrLayoutGridLine start, PrLayoutGridLine end = PrLayoutGridLine{}) {
    PrLayoutGridStyle grid = prLayoutGridStyleDefault();
    grid.rowStart = start;
    grid.rowEnd = end;
    return grid;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("grid items fill a fixed grid in rows, in document order") {
    Stage s;
    PrLayoutGridStyle style = prLayoutGridStyleDefault();
    style.templateColumns = s.tracks({fixed(100), fixed(200)});
    style.templateRows = s.tracks({fixed(50), fixed(60)});
    PrLayoutNode g = s.grid(s.root, style);
    PrLayoutNode a = s.item(g);
    PrLayoutNode b = s.item(g);
    PrLayoutNode c = s.item(g);
    PrLayoutNode d = s.item(g);
    s.layout();
    checkRect(s.at(a), 0, 0, 100, 50);
    checkRect(s.at(b), 100, 0, 300, 50);
    checkRect(s.at(c), 0, 50, 100, 110);
    checkRect(s.at(d), 100, 50, 300, 110);
    // The container's auto height is its rows', and its width fills its containing block.
    checkRect(s.at(g), 0, 0, 600, 110);
    CHECK(s.stats().gridsLaidOut == 1);
}

TEST_CASE("flexible columns share the free space by their factors, and gaps come out first") {
    Stage s;
    PrLayoutGridStyle style = prLayoutGridStyleDefault();
    style.templateColumns = s.tracks({fr(1), fr(2), fixed(40)});
    PrLayoutFlexStyle flex = prLayoutFlexStyleDefault();
    flex.columnGap = prLayoutLengthPx(10);
    PrLayoutNode g = s.grid(s.root, style, 360, flex);
    PrLayoutNode a = s.item(g, prLayoutGridStyleDefault(), -1, 20);
    PrLayoutNode b = s.item(g, prLayoutGridStyleDefault(), -1, 20);
    PrLayoutNode c = s.item(g, prLayoutGridStyleDefault(), -1, 20);
    s.layout();
    // 360 less the 40px track and two 10px gaps leaves 300, split 1:2.
    checkRect(s.at(a), 0, 0, 100, 20);
    checkRect(s.at(b), 110, 0, 310, 20);
    checkRect(s.at(c), 320, 0, 360, 20);
}

TEST_CASE("auto tracks fit their items' content and stretch over the free space") {
    Stage s;
    PrLayoutGridStyle style = prLayoutGridStyleDefault();
    style.templateColumns = s.tracks({autoTrack(), autoTrack()});
    PrLayoutNode g = s.grid(s.root, style, 300);
    PrLayoutNode a = s.item(g);
    s.words(a, {30, 20});
    PrLayoutNode b = s.item(g);
    s.words(b, {50});
    s.layout();
    // The tracks take their items' max-content sizes, 50 each, and share the 200px left equally.
    CHECK(s.width(a) == doctest::Approx(150));
    CHECK(s.left(b) == doctest::Approx(150));
    CHECK(s.width(b) == doctest::Approx(150));
    CHECK(s.height(a) == doctest::Approx(20));
}

TEST_CASE("content-sized tracks that do not stretch keep their content's sizes") {
    Stage s;
    PrLayoutGridStyle style = prLayoutGridStyleDefault();
    style.templateColumns = s.tracks({minContent(), maxContent(), fitContent(40)});
    PrLayoutNode g = s.grid(s.root, style, 400);
    PrLayoutNode a = s.item(g);
    s.words(a, {30, 20});
    PrLayoutNode b = s.item(g);
    s.words(b, {30, 20});
    PrLayoutNode c = s.item(g);
    s.words(c, {30, 20});
    s.layout();
    CHECK(s.width(a) == doctest::Approx(30));
    CHECK(s.height(a) == doctest::Approx(40));
    CHECK(s.width(b) == doctest::Approx(50));
    CHECK(s.width(c) == doctest::Approx(40));
    CHECK(s.left(c) == doctest::Approx(80));
}

TEST_CASE("an item spanning auto tracks grows the tracks that its content needs") {
    Stage s;
    PrLayoutGridStyle style = prLayoutGridStyleDefault();
    style.templateColumns = s.tracks({
        minmax(prLayoutTrackBreadthMinContent(), prLayoutTrackBreadthMaxContent()),
        minmax(prLayoutTrackBreadthMinContent(), prLayoutTrackBreadthMaxContent()),
    });
    PrLayoutFlexStyle flex = prLayoutFlexStyleDefault();
    flex.justifyContent = PrLayoutAlignment_Start;
    PrLayoutNode g = s.grid(s.root, style, 400, flex);
    // The note's example (css-grid-1/#algo-spanning-items): an item of 10 in the first track, and one spanning both
    // whose min-content is 30 and max-content 100.
    PrLayoutNode first = s.item(g);
    s.words(first, {10});
    PrLayoutNode spanning = s.item(g, area(line(2), line(1), PrLayoutGridLine{}, span(2)));
    s.words(spanning, {30, 30, 40});
    s.layout();
    // The first track stays at its item's size, and the second takes the rest of the spanning item's needs.
    CHECK(s.width(first) == doctest::Approx(10));
    CHECK(s.width(spanning) == doctest::Approx(100));
}

TEST_CASE("line placement counts from the end with negative lines and creates implicit tracks") {
    Stage s;
    PrLayoutGridStyle style = prLayoutGridStyleDefault();
    style.templateColumns = s.tracks({fixed(100), fixed(100), fixed(100)});
    style.autoColumns = s.tracks({fixed(30)});
    style.autoRows = s.tracks({fixed(25)});
    PrLayoutNode g = s.grid(s.root, style);
    PrLayoutNode last = s.item(g, columns(line(-2), line(-1)));
    PrLayoutNode before = s.item(g, columns(line(-6), span(1)));
    PrLayoutNode after = s.item(g, columns(line(4)));
    s.layout();
    // Line -6 is two implicit lines before the explicit grid, which then starts 60px in.
    CHECK(s.left(before) == doctest::Approx(0));
    CHECK(s.width(before) == doctest::Approx(30));
    CHECK(s.left(last) == doctest::Approx(260));
    CHECK(s.width(last) == doctest::Approx(100));
    // Line 4 is the explicit grid's last line, so an item that starts there is in an implicit column past it.
    CHECK(s.left(after) == doctest::Approx(360));
    CHECK(s.width(after) == doctest::Approx(30));
    CHECK(s.height(after) == doctest::Approx(25));
}

TEST_CASE("conflicting placements swap, collapse to one track, or drop the end's span") {
    Stage s;
    PrLayoutGridStyle style = prLayoutGridStyleDefault();
    style.templateColumns = s.tracks({fixed(10), fixed(20), fixed(30), fixed(40)});
    style.templateRows = s.tracks({fixed(10), fixed(10), fixed(10)});
    PrLayoutNode g = s.grid(s.root, style);
    PrLayoutNode swapped = s.item(g, area(line(1), line(4), PrLayoutGridLine{}, line(2)));
    PrLayoutNode same = s.item(g, area(line(2), line(3), PrLayoutGridLine{}, line(3)));
    PrLayoutNode spans = s.item(g, area(line(3), span(2), PrLayoutGridLine{}, span(3)));
    s.layout();
    CHECK(s.left(swapped) == doctest::Approx(10));
    CHECK(s.width(swapped) == doctest::Approx(50));
    CHECK(s.left(same) == doctest::Approx(30));
    CHECK(s.width(same) == doctest::Approx(30));
    // Two spans keep the start's, and the item is placed automatically in its row.
    CHECK(s.left(spans) == doctest::Approx(0));
    CHECK(s.width(spans) == doctest::Approx(30));
}

TEST_CASE("named lines and areas place items, and a name alone finds an area's edges") {
    Stage s;
    PrLayoutGridName header = s.name("header");
    PrLayoutGridName main = s.name("main");
    PrLayoutGridName side = s.name("side");
    PrLayoutGridName content = s.name("content");
    std::vector<PrLayoutGridArea> areas = {{header, 0, 1, 0, 2}, {side, 1, 2, 0, 1}, {main, 1, 2, 1, 2}};
    PrLayoutGridAreas template_{.rowCount = 2, .columnCount = 2, .areas = areas.data(), .areaCount = 3};
    PrLayoutGridStyle style = prLayoutGridStyleDefault();
    style.templateAreas = prLayoutGridAreasIntern(s.f.tree, &template_);
    style.templateColumns = s.tracks({fixed(100), fixed(300)}, {{1, content}, {2, content}});
    style.templateRows = s.tracks({fixed(40), fixed(200)});
    PrLayoutNode g = s.grid(s.root, style);
    PrLayoutNode m = s.item(g, area(line(0, main), line(0, main), line(0, main), line(0, main)));
    PrLayoutNode h = s.item(g, area(line(0, header), line(0, header), line(0, header), line(0, header)));
    PrLayoutNode c = s.item(g, area(line(2), line(1, content), PrLayoutGridLine{}, line(2, content)));
    s.layout();
    checkRect(s.at(h), 0, 0, 400, 40);
    checkRect(s.at(m), 100, 40, 400, 240);
    checkRect(s.at(c), 100, 40, 400, 240);
}

TEST_CASE("auto-placement fills rows past placed items, or holes too if dense") {
    for (bool dense : {false, true}) {
        Stage s;
        PrLayoutGridStyle style = prLayoutGridStyleDefault();
        style.templateColumns = s.tracks({fixed(10), fixed(10), fixed(10)});
        style.autoRows = s.tracks({fixed(10)});
        style.autoFlow = dense ? PrLayoutGridAutoFlowFlag_Dense : 0;
        PrLayoutNode g = s.grid(s.root, style);
        PrLayoutNode a = s.item(g);
        PrLayoutNode wide = s.item(g, columns(span(3)));
        PrLayoutNode b = s.item(g);
        s.layout();
        CHECK(s.top(a) == doctest::Approx(0));
        CHECK(s.top(wide) == doctest::Approx(10));
        // A sparse flow goes on past the wide item, and a dense one fills the first row's hole.
        CHECK(s.top(b) == doctest::Approx(dense ? 0 : 20));
        CHECK(s.left(b) == doctest::Approx(dense ? 10 : 0));
    }
}

TEST_CASE("a column flow fills each column in turn") {
    Stage s;
    PrLayoutGridStyle style = prLayoutGridStyleDefault();
    style.templateRows = s.tracks({fixed(10), fixed(10)});
    style.autoColumns = s.tracks({fixed(15)});
    style.autoFlow = PrLayoutGridAutoFlowFlag_Column;
    PrLayoutNode g = s.grid(s.root, style);
    PrLayoutNode a = s.item(g);
    PrLayoutNode b = s.item(g);
    PrLayoutNode c = s.item(g);
    s.layout();
    checkRect(s.at(a), 0, 0, 15, 10);
    checkRect(s.at(b), 0, 10, 15, 20);
    checkRect(s.at(c), 15, 0, 30, 10);
}

TEST_CASE("order changes where items are placed automatically and the order they paint in") {
    Stage s;
    PrLayoutGridStyle style = prLayoutGridStyleDefault();
    style.templateColumns = s.tracks({fixed(10), fixed(10)});
    PrLayoutNode g = s.grid(s.root, style);
    PrLayoutFlexStyle later = prLayoutFlexStyleDefault();
    later.order = 1;
    PrLayoutNode a = s.item(g, prLayoutGridStyleDefault(), -1, 10, later);
    PrLayoutNode b = s.item(g, prLayoutGridStyleDefault(), -1, 10);
    s.layout();
    CHECK(s.left(b) == doctest::Approx(0));
    CHECK(s.left(a) == doctest::Approx(10));
}

TEST_CASE("auto-fill repeats as many tracks as fit, and auto-fit collapses the empty ones") {
    for (PrLayoutGridRepeat repeat : {PrLayoutGridRepeat_AutoFill, PrLayoutGridRepeat_AutoFit}) {
        Stage s;
        PrLayoutGridStyle style = prLayoutGridStyleDefault();
        style.templateColumns = s.repeating({minmax(prLayoutTrackBreadthPx(100), prLayoutTrackBreadthFlex(1))}, 0, 1, repeat);
        PrLayoutFlexStyle flex = prLayoutFlexStyleDefault();
        flex.columnGap = prLayoutLengthPx(10);
        PrLayoutNode g = s.grid(s.root, style, 450, flex);
        PrLayoutNode a = s.item(g, prLayoutGridStyleDefault(), -1, 10);
        PrLayoutNode b = s.item(g, prLayoutGridStyleDefault(), -1, 10);
        s.layout();
        if (repeat == PrLayoutGridRepeat_AutoFill) {
            // Four 100px tracks and three gaps fit in 450: the tracks share the 20px left.
            CHECK(s.width(a) == doctest::Approx(105));
            CHECK(s.left(b) == doctest::Approx(115));
        } else {
            // The two empty tracks collapse with their gaps, and the two left share what remains.
            CHECK(s.width(a) == doctest::Approx(220));
            CHECK(s.left(b) == doctest::Approx(230));
        }
    }
}

TEST_CASE("items align in their areas, and auto margins take the free space first") {
    Stage s;
    PrLayoutGridStyle style = prLayoutGridStyleDefault();
    style.templateColumns = s.tracks({fixed(100), fixed(100)});
    style.templateRows = s.tracks({fixed(100)});
    PrLayoutNode g = s.grid(s.root, style);
    PrLayoutFlexStyle centered = prLayoutFlexStyleDefault();
    centered.justifySelf = PrLayoutAlignment_Center;
    centered.alignSelf = PrLayoutAlignment_End;
    PrLayoutNode a = s.item(g, prLayoutGridStyleDefault(), 20, 10, centered);
    PrLayoutBoxStyle box = block(10);
    box.width = prLayoutLengthPx(20);
    box.margin[PrRectSide_Left] = prLayoutLengthAuto();
    PrLayoutNode b = s.add(g, box);
    PrLayoutNode stretched = s.add(s.root, block());
    (void)stretched;
    s.layout();
    checkRect(s.at(a), 40, 90, 60, 100);
    checkRect(s.at(b), 180, 0, 200, 10);
}

TEST_CASE("items stretch over their areas unless they have a size, and fit their content otherwise") {
    Stage s;
    PrLayoutGridStyle style = prLayoutGridStyleDefault();
    style.templateColumns = s.tracks({fixed(100)});
    style.templateRows = s.tracks({fixed(60), fixed(60)});
    PrLayoutNode g = s.grid(s.root, style);
    PrLayoutNode stretched = s.item(g);
    s.words(stretched, {30});
    PrLayoutFlexStyle start = prLayoutFlexStyleDefault();
    start.justifySelf = PrLayoutAlignment_Start;
    start.alignSelf = PrLayoutAlignment_Start;
    PrLayoutNode fitted = s.item(g, prLayoutGridStyleDefault(), -1, -1, start);
    s.words(fitted, {30});
    s.layout();
    checkRect(s.at(stretched), 0, 0, 100, 60);
    checkRect(s.at(fitted), 0, 60, 30, 80);
}

TEST_CASE("justify-content and align-content place the tracks in the content box") {
    Stage s;
    PrLayoutGridStyle style = prLayoutGridStyleDefault();
    style.templateColumns = s.tracks({fixed(100), fixed(100)});
    style.templateRows = s.tracks({fixed(50)});
    PrLayoutFlexStyle flex = prLayoutFlexStyleDefault();
    flex.justifyContent = PrLayoutAlignment_SpaceBetween;
    flex.alignContent = PrLayoutAlignment_Center;
    PrLayoutBoxStyle box = block(150);
    box.display = PrLayoutDisplay_Grid;
    box.width = prLayoutLengthPx(400);
    PrLayoutNode g = s.add(s.root, box, style, flex);
    PrLayoutNode a = s.item(g);
    PrLayoutNode b = s.item(g);
    s.layout();
    checkRect(s.at(a), 0, 50, 100, 100);
    checkRect(s.at(b), 300, 50, 400, 100);
}

TEST_CASE("percentage rows of an auto height resolve against the height that their content gives") {
    Stage s;
    PrLayoutGridStyle style = prLayoutGridStyleDefault();
    style.templateRows = s.tracks({autoTrack(), percent(50)});
    PrLayoutNode g = s.grid(s.root, style, 100);
    PrLayoutNode a = s.item(g, prLayoutGridStyleDefault(), -1, 80);
    PrLayoutNode b = s.item(g);
    s.layout();
    // The rows are 80 and auto while the height is found, and then the second is half of that height.
    CHECK(s.height(g) == doctest::Approx(80));
    CHECK(s.height(a) == doctest::Approx(80));
    CHECK(s.top(b) == doctest::Approx(80));
    CHECK(s.height(b) == doctest::Approx(40));
}

TEST_CASE("an inline grid shrinks to its columns' content") {
    Stage s;
    PrLayoutGridStyle style = prLayoutGridStyleDefault();
    style.templateColumns = s.tracks({autoTrack(), fixed(40)});
    PrLayoutBoxStyle box = block();
    box.display = PrLayoutDisplay_InlineGrid;
    PrLayoutNode g = s.add(s.root, box, style);
    PrLayoutNode a = s.item(g);
    s.words(a, {30, 20});
    s.item(g, prLayoutGridStyleDefault(), -1, 10);
    s.layout();
    CHECK(s.width(g) == doctest::Approx(90));
    CHECK(s.width(a) == doctest::Approx(50));
}

TEST_CASE("a positioned grid places a box out of flow in the area its placement names") {
    Stage s;
    PrLayoutGridStyle style = prLayoutGridStyleDefault();
    style.templateColumns = s.tracks({fixed(100), fixed(100), fixed(100)});
    style.templateRows = s.tracks({fixed(50), fixed(50)});
    PrLayoutBoxStyle box = block();
    box.display = PrLayoutDisplay_Grid;
    box.position = PrLayoutPosition_Relative;
    box.padding[PrRectSide_Left] = prLayoutLengthPx(10);
    box.width = prLayoutLengthPx(400);
    PrLayoutNode g = s.add(s.root, box, style);
    PrLayoutBoxStyle abs = block();
    abs.position = PrLayoutPosition_Absolute;
    for (uint32_t side = 0; side < PrRectSide_Count; ++side)
        abs.inset[side] = prLayoutLengthPx(0);
    PrLayoutNode placed = s.add(g, abs, area(line(2), line(2), PrLayoutGridLine{}, span(2)));
    // An auto line is the padding edge: this box runs from the padding box's left to the end of the first column, and
    // from the first row line to the padding box's bottom.
    PrLayoutNode padded = s.add(g, abs, area(line(1), PrLayoutGridLine{}, PrLayoutGridLine{}, line(2)));
    s.layout();
    checkRect(s.at(placed), 110, 50, 310, 100);
    checkRect(s.at(padded), 0, 0, 110, 100);
}

TEST_CASE("a child out of flow has its static position as the sole item of the content box") {
    Stage s;
    PrLayoutGridStyle style = prLayoutGridStyleDefault();
    style.templateColumns = s.tracks({fixed(100)});
    PrLayoutBoxStyle box = block(100);
    box.display = PrLayoutDisplay_Grid;
    box.width = prLayoutLengthPx(200);
    PrLayoutNode g = s.add(s.root, box, style);
    PrLayoutBoxStyle abs = block(10);
    abs.width = prLayoutLengthPx(20);
    abs.position = PrLayoutPosition_Absolute;
    PrLayoutFlexStyle flex = prLayoutFlexStyleDefault();
    flex.justifySelf = PrLayoutAlignment_End;
    flex.alignSelf = PrLayoutAlignment_Center;
    PrLayoutNode child = s.add(g, abs, prLayoutGridStyleDefault(), flex);
    s.layout();
    checkRect(s.at(child), 180, 45, 200, 55);
}

TEST_CASE("items aligned by baseline share their row's baseline, which the row grows to fit") {
    Stage s;
    PrLayoutGridStyle style = prLayoutGridStyleDefault();
    style.templateColumns = s.tracks({fixed(100), fixed(100)});
    PrLayoutFlexStyle flex = prLayoutFlexStyleDefault();
    flex.alignItems = PrLayoutAlignment_Baseline;
    PrLayoutNode g = s.grid(s.root, style, -1, flex);
    // Two items whose single lines of words end 20px down, one pushed down by 30px of padding.
    PrLayoutNode a = s.item(g);
    s.words(a, {20});
    PrLayoutBoxStyle padded = block();
    padded.padding[PrRectSide_Top] = prLayoutLengthPx(30);
    PrLayoutNode b = s.add(g, padded);
    s.words(b, {20});
    s.layout();
    CHECK(s.top(a) == doctest::Approx(30));
    CHECK(s.top(b) == doctest::Approx(0));
    CHECK(s.height(g) == doctest::Approx(50));
}

TEST_CASE("a change to an item's placement lays out its container, and the item only if its area changes") {
    Stage s;
    PrLayoutGridStyle style = prLayoutGridStyleDefault();
    style.templateColumns = s.tracks({fixed(100), fixed(100)});
    PrLayoutNode g = s.grid(s.root, style);
    PrLayoutNode a = s.item(g, prLayoutGridStyleDefault(), -1, 10);
    PrLayoutNode b = s.item(g, prLayoutGridStyleDefault(), -1, 10);
    s.layout();
    CHECK(s.left(b) == doctest::Approx(100));
    PrLayoutBoxStyle box = block(10);
    prLayoutNodeSetStyle(s.f.tree, b, s.style(box, rows(line(2))));
    s.layout();
    CHECK(s.left(b) == doctest::Approx(0));
    CHECK(s.top(b) == doctest::Approx(10));
    CHECK(s.left(a) == doctest::Approx(0));
    CHECK(s.stats().gridsLaidOut == 1);
    // The items' areas are the same size, so neither is laid out again.
    CHECK(s.stats().blocksLaidOut <= 2);
}

TEST_CASE("a grid lays out nothing again when nothing changed") {
    Stage s;
    PrLayoutGridStyle style = prLayoutGridStyleDefault();
    style.templateColumns = s.tracks({autoTrack(), fr(1)});
    PrLayoutNode g = s.grid(s.root, style);
    PrLayoutNode a = s.item(g);
    s.words(a, {30, 20});
    s.item(g);
    s.layout();
    s.layout();
    CHECK(s.stats().gridsLaidOut == 0);
    CHECK(s.stats().blocksLaidOut == 0);
}

TEST_CASE("text in a grid container is an anonymous item, which contributes its content's sizes") {
    Stage s;
    PrLayoutGridStyle style = prLayoutGridStyleDefault();
    style.templateColumns = s.tracks({maxContent(), fixed(100)});
    PrLayoutNode g = s.grid(s.root, style);
    s.f.text(g, "x");
    PrLayoutNode b = s.item(g, prLayoutGridStyleDefault(), -1, 10);
    s.layout();
    // The text is set in a font of size 0, so its column is empty.
    CHECK(s.left(b) == doctest::Approx(0));
    CHECK(s.width(b) == doctest::Approx(100));
}

TEST_CASE("a grid that is measured is solved alone, without its items' final layouts") {
    Stage s;
    PrLayoutBoxStyle row = block();
    row.display = PrLayoutDisplay_Flex;
    row.width = prLayoutLengthPx(300);
    PrLayoutNode r = s.add(s.root, row);
    PrLayoutGridStyle style = prLayoutGridStyleDefault();
    style.templateColumns = s.tracks({fixed(50), fixed(50)});
    PrLayoutBoxStyle box = block();
    box.display = PrLayoutDisplay_Grid;
    PrLayoutNode g = s.add(r, box, style);
    PrLayoutNode a = s.item(g, prLayoutGridStyleDefault(), -1, 30);
    s.item(g, prLayoutGridStyleDefault(), -1, 40);
    s.layout();
    CHECK(s.height(g) == doctest::Approx(40));
    CHECK(s.width(g) == doctest::Approx(100));
    CHECK(s.height(a) == doctest::Approx(30));
}

TEST_CASE("flexible rows of an indefinite height take their share of the height that their content gives the grid") {
    Stage s;
    PrLayoutGridStyle style = prLayoutGridStyleDefault();
    style.templateColumns = s.tracks({fixed(100)});
    style.templateRows = s.tracks({minmax(prLayoutTrackBreadthPx(0), prLayoutTrackBreadthFlex(0.5f))});
    PrLayoutNode g = s.grid(s.root, style, 100);
    s.item(g, prLayoutGridStyleDefault(), 200, 200);
    s.layout();
    // Under the max-content constraint the row is half of the item's 200px, which makes the grid 100px tall, and the
    // row is half of that once the grid's height is known.
    CHECK(s.height(g) == doctest::Approx(100));
    PrLayoutUnit sizes[1];
    REQUIRE(prLayoutNodeGridTrackSizes(s.f.tree, g, PrLayoutGridAxis_Rows, sizes, 1) == 1);
    CHECK(prLayoutUnitToPx(sizes[0]) == doctest::Approx(50));
}

TEST_CASE("a spanning item's max-content contribution grows only the tracks whose minimum is max-content") {
    Stage s;
    PrLayoutGridStyle style = prLayoutGridStyleDefault();
    style.templateColumns = s.tracks({fitContent(40), maxContent()});
    PrLayoutFlexStyle flex = prLayoutFlexStyleDefault();
    flex.columnGap = prLayoutLengthPx(5);
    flex.justifyContent = PrLayoutAlignment_Start;
    PrLayoutNode g = s.grid(s.root, style, 100, flex);
    PrLayoutNode a = s.item(g, area(line(1), line(2), PrLayoutGridLine{}, PrLayoutGridLine{}));
    s.words(a, {50});
    PrLayoutNode b = s.item(g, area(line(2), line(1), PrLayoutGridLine{}, line(-1)));
    s.words(b, {30, 20, 30});
    s.layout();
    // The second column fits the first item's 50px, and then takes all of the 25px more that the spanning item's 80px
    // needs past the gap. The fit-content column takes none of it.
    PrLayoutUnit sizes[2];
    REQUIRE(prLayoutNodeGridTrackSizes(s.f.tree, g, PrLayoutGridAxis_Columns, sizes, 2) == 2);
    CHECK(prLayoutUnitToPx(sizes[0]) == doctest::Approx(0));
    CHECK(prLayoutUnitToPx(sizes[1]) == doctest::Approx(75));
}

TEST_CASE("a replaced item's percentage size does not floor its track at its natural size") {
    Stage s;
    PrLayoutGridStyle style = prLayoutGridStyleDefault();
    PrLayoutNode g = s.grid(s.root, style, 50);
    PrLayoutBoxStyle box = block();
    box.width = prLayoutLengthPercent(200);
    PrLayoutNode image = prLayoutNodeCreateReplaced(s.f.tree, g, s.style(box));
    prLayoutNodeSetNaturalSize(s.f.tree, image, natural(200, 200));
    s.layout();
    // Its automatic minimum is capped by 200% of zero, so the auto column is the grid's 50px, and the image is 200% of
    // that, square.
    CHECK(s.width(image) == doctest::Approx(100));
    CHECK(s.height(image) == doctest::Approx(100));
}

TEST_CASE("tracks that overflow with space-evenly sit at the start, as safe center places them") {
    Stage s;
    PrLayoutGridStyle style = prLayoutGridStyleDefault();
    style.templateColumns = s.tracks({fixed(100)});
    PrLayoutFlexStyle flex = prLayoutFlexStyleDefault();
    flex.justifyContent = PrLayoutAlignment_SpaceEvenly;
    PrLayoutNode g = s.grid(s.root, style, 50, flex);
    PrLayoutNode a = s.item(g, prLayoutGridStyleDefault(), -1, 20);
    s.layout();
    CHECK(s.left(a) == doctest::Approx(0));
    CHECK(s.width(a) == doctest::Approx(100));
}

TEST_CASE("a grid of a fixed width has the intrinsic sizes of one automatic repetition") {
    Stage s;
    PrLayoutBoxStyle row = block(20);
    row.display = PrLayoutDisplay_Flex;
    row.width = prLayoutLengthPx(100);
    PrLayoutNode container = s.add(s.root, row);
    PrLayoutFlexStyle rigid = prLayoutFlexStyleDefault();
    rigid.shrink = 0;
    s.item(container, prLayoutGridStyleDefault(), 50, -1, rigid);
    PrLayoutGridStyle style = prLayoutGridStyleDefault();
    style.templateColumns = s.repeating({fixed(50)}, 0, 1, PrLayoutGridRepeat_AutoFill);
    PrLayoutNode g = s.grid(container, style, 200);
    s.layout();
    // Its automatic minimum as a flex item is its min-content size, one 50px column, so it shrinks to the 50px left.
    CHECK(s.width(g) == doctest::Approx(50));
}
