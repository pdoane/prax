// The flex benchmark, which ./dev.sh bench flex runs. It lays out the same trees in lib/layout, Yoga and Taffy.
//
// The trees are those of Taffy's own flexbox benchmarks (benches/benches/flexbox.rs in Taffy), generated here from this
// harness's own seed, so they have the shapes and styles of Taffy's trees but not their exact sizes. Every engine lays
// them out in the same viewport, and the root is sized to the viewport.
//
// Each tree is measured in each engine for the operations that a client performs on it:
//
//   build      making the engine's tree from style values in the engine's own types, and freeing it
//   cold       making the tree, laying it out and freeing it: the cost of a build plus a first layout
//   resize     a layout after the viewport's width changes, back and forth between two widths
//   change     a layout after the width of the tree's deepest leaf changes, back and forth between two widths
//   unchanged  a layout after nothing changed
//
// A row is an engine's measure of one tree: {"engine", "name": "<tree> <measure>", "time", "counts", "boxes", "shape"}.
// The shape is the work of one layout as the engine counts it (flex_engine.h). A cold row also has "agrees", the number
// of boxes that the engine places where Taffy places them, to within a pixel. Rows are written as a JSON array, one row
// per line.
//
// Usage: flex_bench [--differences] [--repeat MEASURE] [--html NODE] [TREE]...
//
// Measures the trees whose names start with one of the TREE arguments, or every tree. --differences prints, on stderr,
// the first boxes that each engine places differently from Taffy, with the node's style. --repeat runs one measure of
// the first chosen tree in lib/layout over and over for five minutes (kHarnessRepeatSeconds), for a profiler to sample,
// and prints nothing. --html prints the subtree at NODE of the first chosen tree as a probe page (./dev.sh probe) that
// measures each box by its id, and measures nothing itself. The page shows how the browsers lay out a subtree in which
// the engines differ.

#include "pr/assert.h"

#include "tools/bench/flex_bench/flex_engine.h"
#include "tools/bench/flex_bench/prax_engine.h"
#include "tools/bench/flex_bench/taffy_engine.h"
#include "tools/bench/flex_bench/yoga_engine.h"
#include "tools/harness/harness.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>

using namespace pr;
using namespace harness;

namespace flex_bench {
namespace {

constexpr float kViewportWidth = 1024;
constexpr float kViewportHeight = 768;
constexpr float kNarrowWidth = 900;

// A deterministic stream of numbers (splitmix64), so that every run builds the same trees.
struct Random {
    uint64_t state;

    uint64_t next() {
        uint64_t z = (state += 0x9e3779b97f4a7c15ull);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
        return z ^ (z >> 31);
    }

    // Returns a number in [0, 1).
    float unit() { return float(next() >> 40) / float(1ull << 24); }

    // Returns a whole number in [low, high].
    uint32_t between(uint32_t low, uint32_t high) { return low + uint32_t(next() % (high - low + 1)); }
};

FlexLength px(float value) {
    return FlexLength{.kind = FlexLengthKind_Px, .value = value};
}

// Taffy's random_dimension: auto a fifth of the time, a length under 500 px three fifths of the time, and otherwise a
// percentage.
FlexLength randomLength(Random& random) {
    float choice = random.unit();
    if (choice < 0.2f)
        return FlexLength{};
    if (choice < 0.8f)
        return px(random.unit() * 500);
    return FlexLength{.kind = FlexLengthKind_Percent, .value = random.unit() * 100};
}

// How a generator styles the leaves and the containers that it makes.
struct Styler {
    std::function<FlexStyle(Random&)> leaf;
    std::function<FlexStyle(Random&)> container;
};

Styler fixed(const FlexStyle& style) {
    return Styler{[style](Random&) { return style; }, [style](Random&) { return style; }};
}

Styler randomSizes() {
    auto make = [](Random& random) {
        FlexStyle style = flexStyleDefault();
        style.width = randomLength(random);
        style.height = randomLength(random);
        return style;
    };
    return Styler{make, make};
}

// Returns a tree whose root has the default style, as Taffy's builders make it.
Tree rooted() {
    Tree tree;
    tree.root = tree.add(flexStyleDefault());
    tree.styles[tree.root].width = px(kViewportWidth);
    tree.styles[tree.root].height = px(kViewportHeight);
    return tree;
}

// Taffy's build_deep_tree: `branching` containers, each holding an equal share of the remaining nodes, recursively down
// to leaves.
std::vector<uint32_t> deepTree(Tree& tree, Random& random, const Styler& styler, uint32_t nodes, uint32_t branching) {
    std::vector<uint32_t> made;
    if (nodes <= branching) {
        for (uint32_t i = 0; i < nodes; ++i)
            made.push_back(tree.add(styler.leaf(random)));
        return made;
    }
    for (uint32_t i = 0; i < branching; ++i) {
        std::vector<uint32_t> children = deepTree(tree, random, styler, (nodes - branching) / branching, branching);
        uint32_t container = tree.add(styler.container(random));
        tree.children[container] = std::move(children);
        made.push_back(container);
    }
    return made;
}

Tree deepHierarchy(const Styler& styler, uint32_t nodes, uint32_t branching) {
    Random random{12345};
    Tree tree = rooted();
    tree.children[tree.root] = deepTree(tree, random, styler, nodes, branching);
    return tree;
}

// Taffy's build_flat_hierarchy: containers of one to four leaves under the root, until the tree holds `nodes`.
Tree flatHierarchy(const Styler& styler, uint32_t nodes) {
    Random random{12345};
    Tree tree = rooted();
    std::vector<uint32_t> containers;
    while (tree.styles.size() < nodes) {
        uint32_t count = random.between(1, 4);
        std::vector<uint32_t> leaves;
        for (uint32_t i = 0; i < count; ++i)
            leaves.push_back(tree.add(styler.leaf(random)));
        uint32_t container = tree.add(styler.container(random));
        tree.children[container] = std::move(leaves);
        containers.push_back(container);
    }
    tree.children[tree.root] = std::move(containers);
    return tree;
}

// Taffy's build_super_deep_hierarchy: `depth` levels, each holding a container and `perLevel` - 1 leaves beside it.
Tree superDeepHierarchy(const Styler& styler, uint32_t depth, uint32_t perLevel) {
    Random random{12345};
    Tree tree = rooted();
    std::vector<uint32_t> children;
    for (uint32_t level = 0; level < depth; ++level) {
        uint32_t container = tree.add(styler.container(random));
        tree.children[container] = children;
        children.clear();
        children.push_back(container);
        for (uint32_t i = 0; i + 1 < perLevel; ++i)
            children.push_back(tree.add(styler.leaf(random)));
    }
    tree.children[tree.root] = std::move(children);
    return tree;
}

// Containers in any direction, with a random justify-content, align-items and gap. Their leaves have fixed sizes, and
// each leaf may or may not grow, set its own alignment, or have an auto margin. Measures the alignment and gaps that
// place a line's items.
Styler aligned() {
    auto container = [](Random& random) {
        FlexStyle style = flexStyleDefault();
        const FlexDirection directions[] = {FlexDirection_Row, FlexDirection_Column, FlexDirection_RowReverse,
                                            FlexDirection_ColumnReverse};
        const FlexAlign justifies[] = {FlexAlign_Normal, FlexAlign_FlexEnd, FlexAlign_Center, FlexAlign_SpaceBetween,
                                       FlexAlign_SpaceAround, FlexAlign_SpaceEvenly};
        const FlexAlign aligns[] = {FlexAlign_Normal, FlexAlign_FlexStart, FlexAlign_FlexEnd, FlexAlign_Center,
                                    FlexAlign_Baseline};
        const float gaps[] = {0, 0, 4, 10};
        style.direction = directions[random.between(0, 3)];
        style.justifyContent = justifies[random.between(0, 5)];
        style.alignItems = aligns[random.between(0, 4)];
        style.gap = gaps[random.between(0, 3)];
        style.grow = float(random.between(0, 1));
        return style;
    };
    auto leaf = [](Random& random) {
        FlexStyle style = flexStyleDefault();
        style.width = px(float(random.between(10, 60)));
        style.height = px(float(random.between(10, 60)));
        style.grow = float(random.between(0, 2));
        if (random.unit() < 0.25f) {
            const FlexAlign selves[] = {FlexAlign_FlexStart, FlexAlign_FlexEnd, FlexAlign_Center, FlexAlign_Stretch,
                                        FlexAlign_Baseline};
            style.alignSelf = selves[random.between(0, 4)];
        }
        if (random.unit() < 0.15f)
            style.marginAuto = uint8_t(1u << random.between(0, 3));
        return style;
    };
    return Styler{leaf, container};
}

// Leaves of fixed sizes from 10 to 60 px with grow factors of up to two, under containers with a grow factor of 0 or 1.
Styler sizedLeaves() {
    auto leaf = [](Random& random) {
        FlexStyle style = flexStyleDefault();
        style.width = px(float(random.between(10, 60)));
        style.height = px(float(random.between(10, 60)));
        style.grow = float(random.between(0, 2));
        return style;
    };
    auto container = [](Random& random) {
        FlexStyle style = flexStyleDefault();
        style.grow = float(random.between(0, 1));
        return style;
    };
    return Styler{leaf, container};
}

// Sets the direction of each container below `node` by its depth: a row at even depths and a column at odd ones.
void alternate(Tree& tree, uint32_t node, uint32_t depth) {
    for (uint32_t child : tree.children[node]) {
        if (!tree.children[child].empty())
            tree.styles[child].direction = depth % 2 == 0 ? FlexDirection_Column : FlexDirection_Row;
        alternate(tree, child, depth + 1);
    }
}

// Containers of 8 to 30 leaves under the root, until the tree holds `nodes`. Most containers are rows as wide as the
// viewport that wrap or wrap in reverse, and a quarter are wrapping columns 300 px tall. Each has a random
// align-content and gap. The leaves are 20 to 120 px wide and 10 to 60 px tall, and may or may not grow.
Tree wrappingHierarchy(uint32_t nodes) {
    Random random{12345};
    Tree tree = rooted();
    tree.styles[tree.root].direction = FlexDirection_Column;
    tree.styles[tree.root].alignItems = FlexAlign_FlexStart;
    std::vector<uint32_t> containers;
    const FlexAlign contents[] = {FlexAlign_Normal, FlexAlign_FlexStart, FlexAlign_FlexEnd, FlexAlign_Center,
                                  FlexAlign_SpaceBetween, FlexAlign_SpaceAround, FlexAlign_SpaceEvenly};
    const float gaps[] = {0, 0, 4, 10};
    while (tree.styles.size() < nodes) {
        uint32_t count = random.between(8, 30);
        std::vector<uint32_t> leaves;
        for (uint32_t i = 0; i < count; ++i) {
            FlexStyle leaf = flexStyleDefault();
            leaf.width = px(float(random.between(20, 120)));
            leaf.height = px(float(random.between(10, 60)));
            leaf.grow = float(random.between(0, 1));
            leaves.push_back(tree.add(leaf));
        }
        FlexStyle style = flexStyleDefault();
        style.wrap = random.unit() < 0.25f ? FlexWrap_WrapReverse : FlexWrap_Wrap;
        style.alignContent = contents[random.between(0, 6)];
        style.gap = gaps[random.between(0, 3)];
        style.shrink = 0;
        if (random.unit() < 0.25f) {
            style.direction = FlexDirection_Column;
            style.height = px(300);
        } else {
            style.width = px(kViewportWidth);
            style.height = px(float(random.between(100, 300)));
        }
        uint32_t container = tree.add(style);
        tree.children[container] = std::move(leaves);
        containers.push_back(container);
    }
    tree.children[tree.root] = std::move(containers);
    return tree;
}

// The containers of the aligned trees, each the containing block of its children. A fifth of the leaves are absolutely
// positioned: by a top and a left inset, by a right and a bottom inset, or by no insets, at the static position that
// the container's alignment gives.
Styler positioned() {
    Styler base = aligned();
    auto container = [base](Random& random) {
        FlexStyle style = base.container(random);
        style.position = FlexPosition_Relative;
        return style;
    };
    auto leaf = [base](Random& random) {
        FlexStyle style = base.leaf(random);
        if (random.unit() < 0.2f) {
            style.position = FlexPosition_Absolute;
            style.marginAuto = 0;
            uint32_t placed = random.between(0, 2);
            if (placed == 0) {
                style.inset[0] = px(float(random.between(0, 40)));
                style.inset[3] = px(float(random.between(0, 40)));
            } else if (placed == 1) {
                style.inset[1] = px(float(random.between(0, 40)));
                style.inset[2] = px(float(random.between(0, 40)));
            }
        }
        return style;
    };
    return Styler{leaf, container};
}

// Containers in either direction that align their items without stretching them. Their leaves have a preferred aspect
// ratio and are sized by their width or their height. The other size follows from the ratio, after flexing may have
// changed the given size. Containers do not stretch, because stretching an item whose cross size determines its main
// size compounds through the levels, which no real page does.
Styler ratios() {
    auto container = [](Random& random) {
        FlexStyle style = flexStyleDefault();
        style.direction = random.unit() < 0.5f ? FlexDirection_Row : FlexDirection_Column;
        const FlexAlign aligns[] = {FlexAlign_FlexStart, FlexAlign_Center, FlexAlign_FlexEnd};
        style.alignItems = aligns[random.between(0, 2)];
        style.grow = float(random.between(0, 1));
        return style;
    };
    auto leaf = [](Random& random) {
        FlexStyle style = flexStyleDefault();
        const float ratios[] = {0.5f, 1, 4.0f / 3, 2};
        style.aspectRatio = ratios[random.between(0, 3)];
        if (random.unit() < 0.5f)
            style.width = px(float(random.between(10, 60)));
        else
            style.height = px(float(random.between(10, 60)));
        style.grow = float(random.between(0, 1));
        return style;
    };
    return Styler{leaf, container};
}

// Mostly columns that stretch their items. Every box's height is auto or a percentage of its container's height,
// including containers. Percentages resolve at every level against a size that stretching or flexing made definite.
Styler percentages() {
    auto percent = [](Random& random) {
        const float percents[] = {0, 50, 100};
        float value = percents[random.between(0, 2)];
        return value == 0 ? FlexLength{} : FlexLength{.kind = FlexLengthKind_Percent, .value = value};
    };
    auto container = [percent](Random& random) {
        FlexStyle style = flexStyleDefault();
        style.direction = random.unit() < 0.75f ? FlexDirection_Column : FlexDirection_Row;
        style.height = percent(random);
        style.grow = float(random.between(0, 1));
        return style;
    };
    auto leaf = [percent](Random& random) {
        FlexStyle style = flexStyleDefault();
        style.width = px(float(random.between(10, 60)));
        style.height = percent(random);
        style.minHeight = px(float(random.between(0, 20)));
        style.grow = float(random.between(0, 1));
        return style;
    };
    return Styler{leaf, container};
}

// Columns at their automatic minimum size, holding leaves of fixed heights. A third of the columns are scroll
// containers, whose automatic minimum is zero. Measures the content-based minimum, read through every level and cut off
// at a scroll container.
Styler scrollingColumns() {
    auto container = [](Random& random) {
        FlexStyle style = flexStyleDefault();
        style.direction = FlexDirection_Column;
        style.grow = 1;
        style.basis = px(0);
        if (random.unit() < 0.33f)
            style.overflow = FlexOverflow_Scroll;
        return style;
    };
    auto leaf = [](Random& random) {
        FlexStyle style = flexStyleDefault();
        style.height = px(float(random.between(10, 60)));
        style.grow = float(random.between(0, 1));
        return style;
    };
    return Styler{leaf, container};
}

// Rows as wide as the viewport, each of `perLine` items at a zero basis. Each item's grow factor is three times the
// previous item's. Each item's maximum is set so that it is the only item to exceed its maximum in its own round of
// resolving flexible lengths (css-flexbox-2/#resolve-flexible-lengths), largest first. Each line therefore takes one
// round per item, which is the most rounds the loop can take.
Tree staircaseHierarchy(uint32_t nodes, uint32_t perLine) {
    Tree tree = rooted();
    tree.styles[tree.root].direction = FlexDirection_Column;
    std::vector<uint32_t> rows;
    while (tree.styles.size() < nodes) {
        std::vector<double> grows(perLine);
        double total = 0;
        for (uint32_t i = 0; i < perLine; ++i)
            total += grows[i] = std::pow(3.0, double(i));
        // In each round, the largest unfrozen item holds two thirds of the remaining grow factors and is frozen at nine
        // tenths of its target. That raises every other item's target by a fifth. Each item's maximum is nine tenths of
        // its target in its own round, so the item exceeds its maximum only in that round.
        std::vector<float> maximums(perLine);
        double free = double(kViewportWidth);
        for (uint32_t i = perLine; i-- > 0;) {
            double target = free * grows[i] / total;
            maximums[i] = float(target * 0.9);
            free -= double(maximums[i]);
            total -= grows[i];
        }
        std::vector<uint32_t> items;
        for (uint32_t i = 0; i < perLine; ++i) {
            FlexStyle item = flexStyleDefault();
            item.basis = px(0);
            item.grow = float(grows[i]);
            item.minWidth = px(0);
            item.maxWidth = px(maximums[i]);
            item.height = px(10);
            items.push_back(tree.add(item));
        }
        FlexStyle row = flexStyleDefault();
        row.shrink = 0;
        row.height = px(10);
        uint32_t container = tree.add(row);
        tree.children[container] = std::move(items);
        rows.push_back(container);
    }
    tree.children[tree.root] = std::move(rows);
    return tree;
}

// Taffy's flat trees, with every item at an order from one to three that is stable from frame to frame, so that each
// container sorts its items. Only lib/layout runs this tree, because neither Yoga nor Taffy supports order.
Tree orderedHierarchy(uint32_t nodes) {
    Tree tree = flatHierarchy(randomSizes(), nodes);
    Random random{54321};
    for (uint32_t i = 0; i < tree.styles.size(); ++i)
        if (i != tree.root)
            tree.styles[i].order = int32_t(random.between(1, 3));
    return tree;
}

// Rows and columns that stretch their items, holding leaves of words in lines. A quarter of the leaves hold a word
// sized by a percentage of the leaf's height. If a stretched leaf is laid out again at another height, its lines are
// reused, unless a word reads that height, in which case they are laid out again. Only lib/layout runs this tree,
// because neither Yoga nor Taffy lays out inline content.
Styler wordLeaves() {
    auto container = [](Random& random) {
        FlexStyle style = flexStyleDefault();
        style.direction = random.unit() < 0.5f ? FlexDirection_Row : FlexDirection_Column;
        style.grow = float(random.between(0, 1));
        return style;
    };
    auto leaf = [](Random& random) {
        FlexStyle style = flexStyleDefault();
        style.width = px(float(random.between(40, 160)));
        style.words = uint16_t(random.between(3, 16));
        style.percentWord = random.unit() < 0.25f;
        style.grow = float(random.between(0, 1));
        return style;
    };
    return Styler{leaf, container};
}

struct Case {
    const char* name;
    Tree tree;
    bool local = false;             // only lib/layout runs it, because it sets what Yoga and Taffy do not lay out
};

std::vector<Case> cases() {
    FlexStyle nested = flexStyleDefault();
    nested.width = px(10);
    nested.height = px(10);
    nested.grow = 1;
    // Margins that add up to more than any viewport, so a box below the first few levels is as wide as its content at
    // its automatic minimum size, whatever the viewport's width. The "deep auto" tree measures caching of that size.
    FlexStyle grown = flexStyleDefault();
    grown.grow = 1;
    for (float& side : grown.margin)
        side = 10;
    // Boxes that fill the viewport. Content fits at every level, so each box's size follows the viewport's size.
    FlexStyle filling = flexStyleDefault();
    filling.grow = 1;
    std::vector<Case> out;
    out.push_back({"huge nested", deepHierarchy(fixed(nested), 10000, 10)});
    out.push_back({"wide", flatHierarchy(randomSizes(), 10000)});
    out.push_back({"deep auto", deepHierarchy(fixed(grown), 4000, 2)});
    out.push_back({"deep filling", deepHierarchy(fixed(filling), 4000, 2)});
    out.push_back({"deep random", deepHierarchy(randomSizes(), 4000, 2)});
    out.push_back({"super deep", superDeepHierarchy(fixed(filling), 100, 3)});
    out.push_back({"aligned", deepHierarchy(aligned(), 4000, 5)});
    // Every container is a column, so every item is sized in its block axis.
    Styler columnar = sizedLeaves();
    std::function<FlexStyle(Random&)> rows = columnar.container;
    columnar.container = [rows](Random& random) {
        FlexStyle style = rows(random);
        style.direction = FlexDirection_Column;
        return style;
    };
    out.push_back({"columns", deepHierarchy(columnar, 4000, 4)});
    Tree alternating = deepHierarchy(sizedLeaves(), 4000, 4);
    alternate(alternating, alternating.root, 0);
    out.push_back({"alternating", std::move(alternating)});
    out.push_back({"wrapping", wrappingHierarchy(10000)});
    out.push_back({"positioned", deepHierarchy(positioned(), 4000, 5)});
    out.push_back({"ratio", deepHierarchy(ratios(), 4000, 4)});
    out.push_back({"percent", deepHierarchy(percentages(), 4000, 4)});
    out.push_back({"scrolling", deepHierarchy(scrollingColumns(), 4000, 3)});
    out.push_back({"staircase", staircaseHierarchy(10000, 8)});
    out.push_back({"ordered", orderedHierarchy(10000), true});
    out.push_back({"words", deepHierarchy(wordLeaves(), 4000, 4), true});
    return out;
}

// Returns the leaf furthest from the root, choosing the first in tree order. A change to it reaches every level.
uint32_t deepestLeaf(const Tree& tree) {
    uint32_t best = tree.root;
    uint32_t bestDepth = 0;
    std::function<void(uint32_t, uint32_t)> walk = [&](uint32_t node, uint32_t depth) {
        if (tree.children[node].empty() && depth > bestDepth) {
            best = node;
            bestDepth = depth;
        }
        for (uint32_t child : tree.children[node])
            walk(child, depth + 1);
    };
    walk(tree.root, 0);
    return best;
}

std::vector<Box> boxesOf(Engine& engine, const Tree& tree) {
    std::vector<Box> out(tree.styles.size());
    for (uint32_t i = 0; i < out.size(); ++i)
        out[i] = engine.box(i);
    return out;
}

bool same(const Box& a, const Box& b) {
    return std::fabs(a.x - b.x) <= 1 && std::fabs(a.y - b.y) <= 1 && std::fabs(a.width - b.width) <= 1
           && std::fabs(a.height - b.height) <= 1;
}

uint32_t agreeing(const std::vector<Box>& a, const std::vector<Box>& b) {
    uint32_t count = 0;
    for (size_t i = 0; i < a.size(); ++i)
        count += same(a[i], b[i]);
    return count;
}

bool showDifferences = false;

const char* lengthText(FlexLength length, char* buffer, size_t size) {
    if (length.kind == FlexLengthKind_Auto)
        return "auto";
    snprintf(buffer, size, length.kind == FlexLengthKind_Px ? "%gpx" : "%g%%", double(length.value));
    return buffer;
}

// Writes the properties that a node's style sets to `buffer`: its sizes and flexibility, and any alignment, direction,
// gap and auto margins that differ from the initial values.
const char* styleText(const FlexStyle& style, size_t children, char* buffer, size_t size) {
    char width[32];
    char height[32];
    int at = snprintf(buffer, size, "%zu children, width %s, height %s, grow %g", children,
                      lengthText(style.width, width, sizeof width), lengthText(style.height, height, sizeof height),
                      double(style.grow));
    auto add = [&](const char* name, int value) {
        if (value != 0 && at >= 0 && size_t(at) < size)
            at += snprintf(buffer + at, size - size_t(at), ", %s %d", name, value);
    };
    add("direction", style.direction);
    add("wrap", style.wrap);
    add("justify", style.justifyContent);
    add("align-items", style.alignItems);
    add("align-self", style.alignSelf);
    add("gap", int(style.gap));
    add("auto margins", style.marginAuto);
    add("align-content", style.alignContent);
    add("position", style.position);
    add("aspect ratio x100", int(style.aspectRatio * 100));
    return buffer;
}

// Writes the subtree at `node` as HTML. Each node is a div whose id is the node's index, styled by the node's style.
void writeHtml(const Tree& tree, uint32_t node, std::string& out) {
    const FlexStyle& style = tree.styles[node];
    char length[32];
    std::string css = "display:flex;box-sizing:border-box;min-width:auto;min-height:auto";
    auto add = [&](const char* name, const std::string& value) { css += std::string(";") + name + ":" + value; };
    auto lengthOf = [&](FlexLength l) { return std::string(lengthText(l, length, sizeof length)); };
    const char* directions[] = {"row", "column", "row-reverse", "column-reverse"};
    const char* wraps[] = {"nowrap", "wrap", "wrap-reverse"};
    const char* aligns[] = {"normal", "flex-start", "flex-end", "center", "stretch", "space-between", "space-around",
                            "space-evenly", "baseline"};
    add("flex-direction", directions[style.direction]);
    add("flex-wrap", wraps[style.wrap]);
    add("align-items", aligns[style.alignItems]);
    if (style.alignSelf != FlexAlign_Normal)
        add("align-self", aligns[style.alignSelf]);
    add("align-content", aligns[style.alignContent]);
    add("justify-content", aligns[style.justifyContent]);
    char number[32];
    snprintf(number, sizeof number, "%g %g ", double(style.grow), double(style.shrink));
    add("flex", number + lengthOf(style.basis));
    add("width", lengthOf(style.width));
    add("height", lengthOf(style.height));
    if (style.maxWidth.kind != FlexLengthKind_Auto)
        add("max-width", lengthOf(style.maxWidth));
    if (style.maxHeight.kind != FlexLengthKind_Auto)
        add("max-height", lengthOf(style.maxHeight));
    if (style.minWidth.kind != FlexLengthKind_Auto)
        add("min-width", lengthOf(style.minWidth));
    if (style.minHeight.kind != FlexLengthKind_Auto)
        add("min-height", lengthOf(style.minHeight));
    std::string margin;
    for (int side = 0; side < 4; ++side) {
        snprintf(number, sizeof number, "%gpx ", double(style.margin[side]));
        margin += (style.marginAuto & (1u << side)) != 0 ? std::string("auto ") : std::string(number);
    }
    add("margin", margin);
    snprintf(number, sizeof number, "%gpx", double(style.gap));
    add("gap", number);
    if (style.aspectRatio > 0) {
        snprintf(number, sizeof number, "%g", double(style.aspectRatio));
        add("aspect-ratio", number);
    }
    const char* positions[] = {"static", "relative", "absolute"};
    add("position", positions[style.position]);
    const char* insets[] = {"top", "right", "bottom", "left"};
    for (int side = 0; side < 4; ++side) {
        if (style.inset[side].kind != FlexLengthKind_Auto)
            add(insets[side], lengthOf(style.inset[side]));
    }
    out += "<div id=n" + std::to_string(node) + " style=\"" + css + "\">";
    for (uint32_t child : tree.children[node])
        writeHtml(tree, child, out);
    out += "</div>";
}

// Prints the subtree at `node` as a probe page with one case. The page measures every box in the subtree relative to
// the subtree's root.
void printHtml(const Case& c, uint32_t node) {
    std::string boxes;
    std::string measures;
    writeHtml(c.tree, node, boxes);
    std::function<void(uint32_t)> each = [&](uint32_t at) {
        std::string id = "n" + std::to_string(at);
        measures += id + ":left " + id + ":top " + id + ":width " + id + ":height ";
        for (uint32_t child : c.tree.children[at])
            each(child);
    };
    each(node);
    measures.pop_back();
    printf("<!doctype html>\n<meta charset=utf-8>\n<style>body { margin: 0; } div { background: #9dc4f933; "
           "outline: 1px solid #2c5a8f; outline-offset: -1px; }</style>\n");
    printf("<figure style=\"margin:0\" data-case=N%u data-what=\"%s node %u\" data-measure=\"%s\">\n<div "
           "style=\"display:flow-root;position:relative;width:%gpx\">%s</div></figure>\n",
           node, c.name, node, measures.c_str(), double(kViewportWidth), boxes.c_str());
    // The probe page's script. It collects each case's measures and posts them to the URL that the page's query names.
    printf("%s", R"(<pre id=out></pre>
<script>
function run() {
  const results = {viewport: [innerWidth, innerHeight], ua: navigator.userAgent, dpr: self.devicePixelRatio, cases: {}};
  const round = v => Math.round(v * 1000) / 1000;
  for (const f of document.querySelectorAll('figure[data-case]')) {
    const stage = f.firstElementChild.getBoundingClientRect();
    results.cases[f.dataset.case] = f.dataset.measure.split(' ').map(m => {
      const [id, what] = m.split(':');
      const r = document.getElementById(id).getBoundingClientRect();
      return round(what === 'top' ? r.top - stage.top : what === 'left' ? r.left - stage.left : r[what]);
    });
  }
  const text = JSON.stringify(results);
  document.getElementById('out').textContent = text;
  if (typeof console !== 'undefined')
    console.log(text);
  const post = new URLSearchParams(location.search).get('post');
  if (post) fetch(post, {method: 'POST', body: text});
}
addEventListener('load', () => requestAnimationFrame(() => requestAnimationFrame(run)));
</script>
)");
}

// Prints the boxes of `engine` where each difference starts: the first boxes, in the order that the tree made them,
// that are not where Taffy places them although their parents are. Each is printed with its parent's style and box.
void printDifferences(const Engine& engine, const char* tree, const Tree& nodes, const std::vector<Box>& boxes,
                      const std::vector<Box>& reference) {
    std::vector<uint32_t> parents(boxes.size(), UINT32_MAX);
    for (uint32_t parent = 0; parent < nodes.children.size(); ++parent) {
        for (uint32_t child : nodes.children[parent])
            parents[child] = parent;
    }
    uint32_t shown = 0;
    for (uint32_t i = 0; i < boxes.size() && shown < 5; ++i) {
        if (same(boxes[i], reference[i]))
            continue;
        if (parents[i] != UINT32_MAX && !same(boxes[parents[i]], reference[parents[i]]))
            continue;
        char own[256];
        fprintf(stderr, "%s %s node %u (%s): %g,%g %gx%g; taffy %g,%g %gx%g\n", engine.name(), tree, i,
                styleText(nodes.styles[i], nodes.children[i].size(), own, sizeof own), double(boxes[i].x),
                double(boxes[i].y), double(boxes[i].width), double(boxes[i].height), double(reference[i].x),
                double(reference[i].y), double(reference[i].width), double(reference[i].height));
        if (parents[i] != UINT32_MAX) {
            uint32_t parent = parents[i];
            const std::vector<uint32_t>& children = nodes.children[parent];
            char style[256];
            fprintf(stderr, "    in node %u (%s): %g,%g %gx%g; taffy %g,%g %gx%g\n", parent,
                    styleText(nodes.styles[parent], children.size(), style, sizeof style), double(boxes[parent].x),
                    double(boxes[parent].y), double(boxes[parent].width), double(boxes[parent].height),
                    double(reference[parent].x), double(reference[parent].y), double(reference[parent].width),
                    double(reference[parent].height));
        }
        ++shown;
    }
}

void appendShape(TextBuffer* row, const Shape& shape) {
    textAppend(row, ", \"shape\": {");
    const char* separator = "";
    const std::pair<const char*, int64_t> fields[] = {{"laidOut", shape.laidOut},
                                                      {"reused", shape.reused},
                                                      {"measured", shape.measured},
                                                      {"measures", shape.measures},
                                                      {"measureHits", shape.measureHits},
                                                      {"measuresReplaced", shape.measuresReplaced},
                                                      {"handoffs", shape.handoffs},
                                                      {"solved", shape.solved},
                                                      {"flexLaidOut", shape.flexLaidOut},
                                                      {"flexed", shape.flexed},
                                                      {"lines", shape.lines},
                                                      {"linesReused", shape.linesReused},
                                                      {"fragments", shape.fragments},
                                                      {"published", shape.published}};
    for (const auto& [name, value] : fields) {
        if (value < 0)
            continue;
        harnessAppendFormat(row, "%s\"%s\": %lld", separator, name, static_cast<long long>(value));
        separator = ", ";
    }
    textAppend(row, "}");
}

bool firstRow = true;

void writeRow(const Engine& engine, const Case& c, const char* measure, const HarnessCounters& cost,
              const Shape* shape, int64_t agrees) {
    TextBuffer row{};
    textAppend(&row, firstRow ? "\n  {\"engine\": " : ",\n  {\"engine\": ");
    firstRow = false;
    harnessWriteString(&row, engine.name());
    std::string name = std::string(c.name) + " " + measure;
    textAppend(&row, ", \"name\": ");
    harnessWriteString(&row, name);
    harnessAppendFormat(&row, ", \"time\": %.6f, \"boxes\": %zu", cost.millis, c.tree.styles.size());
    if (shape != nullptr)
        appendShape(&row, *shape);
    if (agrees >= 0)
        harnessAppendFormat(&row, ", \"agrees\": %lld", static_cast<long long>(agrees));
    HarnessCounts counts{};
    harnessCountsAdd(&counts, "time", cost);
    harnessCountsWrite(&counts, &row);
    textAppend(&row, "}");
    fwrite(row.text.data, 1, row.text.count, stdout);
    textReset(&row);
}

// Returns the work of one call of `step`, which lays the tree out once.
template <typename Step>
Shape counted(Engine& engine, Step step) {
    engine.startCounting();
    step();
    return engine.stopCounting();
}

void measure(Engine& engine, const Case& c, const std::vector<Box>& reference) {
    const Tree& tree = c.tree;
    engine.prepare(tree);
    HarnessCounters build = harnessMeasure([&] {
        engine.build(tree);
        engine.destroy();
    });
    writeRow(engine, c, "build", build, nullptr, -1);

    HarnessCounters cold = harnessMeasure([&] {
        engine.build(tree);
        engine.layout(kViewportWidth, kViewportHeight);
        engine.destroy();
    });
    engine.build(tree);
    Shape coldShape = counted(engine, [&] { engine.layout(kViewportWidth, kViewportHeight); });
    std::vector<Box> boxes = boxesOf(engine, tree);
    if (showDifferences && !c.local)
        printDifferences(engine, c.name, tree, boxes, reference);
    writeRow(engine, c, "cold", cold, &coldShape, c.local ? int64_t(-1) : int64_t(agreeing(boxes, reference)));

    // Each step toggles the width between two values, so that every call lays out after a change.
    bool narrow = false;
    auto resize = [&] {
        narrow = !narrow;
        float width = narrow ? kNarrowWidth : kViewportWidth;
        engine.setWidth(tree.root, width);
        engine.layout(width, kViewportHeight);
    };
    HarnessCounters resized = harnessMeasure(resize);
    Shape resizeShape = counted(engine, resize);
    writeRow(engine, c, "resize", resized, &resizeShape, -1);
    if (narrow)
        resize();

    uint32_t leaf = deepestLeaf(tree);
    bool wider = false;
    auto change = [&] {
        wider = !wider;
        engine.setWidth(leaf, wider ? 20 : 10);
        engine.layout(kViewportWidth, kViewportHeight);
    };
    HarnessCounters changed = harnessMeasure(change);
    Shape changeShape = counted(engine, change);
    writeRow(engine, c, "change", changed, &changeShape, -1);

    HarnessCounters unchanged = harnessMeasure([&] { engine.layout(kViewportWidth, kViewportHeight); });
    Shape unchangedShape = counted(engine, [&] { engine.layout(kViewportWidth, kViewportHeight); });
    writeRow(engine, c, "unchanged", unchanged, &unchangedShape, -1);
    engine.destroy();
}

// Runs the step of `measure` on the tree of `c` in `engine` over and over, for kHarnessRepeatSeconds.
void repeat(Engine& engine, const Case& c, const char* measure) {
    const Tree& tree = c.tree;
    engine.prepare(tree);
    if (strcmp(measure, "build") == 0) {
        harnessRepeat([&] {
            engine.build(tree);
            engine.destroy();
        });
        return;
    }
    if (strcmp(measure, "cold") == 0) {
        harnessRepeat([&] {
            engine.build(tree);
            engine.layout(kViewportWidth, kViewportHeight);
            engine.destroy();
        });
        return;
    }
    engine.build(tree);
    engine.layout(kViewportWidth, kViewportHeight);
    uint32_t leaf = deepestLeaf(tree);
    bool flip = false;
    harnessRepeat([&] {
        flip = !flip;
        if (strcmp(measure, "resize") == 0) {
            float width = flip ? kNarrowWidth : kViewportWidth;
            engine.setWidth(tree.root, width);
            engine.layout(width, kViewportHeight);
        } else if (strcmp(measure, "change") == 0) {
            engine.setWidth(leaf, flip ? 20 : 10);
            engine.layout(kViewportWidth, kViewportHeight);
        } else {
            engine.layout(kViewportWidth, kViewportHeight);
        }
    });
    engine.destroy();
}

} // namespace
} // namespace flex_bench

int main(int argc, char** argv) {
    prCrashHandler();
    using namespace flex_bench;
    std::vector<const char*> names;
    const char* repeated = nullptr;
    int64_t html = -1;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--differences") == 0)
            showDifferences = true;
        else if (strcmp(argv[i], "--html") == 0 && i + 1 < argc)
            html = atoll(argv[++i]);
        else if (strcmp(argv[i], "--repeat") == 0 && i + 1 < argc)
            repeated = argv[++i];
        else
            names.push_back(argv[i]);
    }
    std::vector<std::unique_ptr<Engine>> engines;
    engines.push_back(makePrax());
    engines.push_back(makeYoga());
    engines.push_back(makeTaffy());
    Engine& taffy = *engines[2];

    if (html < 0)
        printf("[");
    for (const Case& c : cases()) {
        bool chosen = names.empty();
        for (const char* name : names)
            chosen = chosen || strncmp(c.name, name, strlen(name)) == 0;
        if (!chosen)
            continue;
        if (html >= 0) {
            printHtml(c, uint32_t(html));
            return 0;
        }
        if (repeated != nullptr) {
            repeat(*engines[0], c, repeated);
            return 0;
        }
        if (c.local) {
            measure(*engines[0], c, {});
            continue;
        }
        taffy.prepare(c.tree);
        taffy.build(c.tree);
        taffy.layout(kViewportWidth, kViewportHeight);
        std::vector<Box> reference = boxesOf(taffy, c.tree);
        taffy.destroy();
        for (const std::unique_ptr<Engine>& engine : engines)
            measure(*engine, c, reference);
    }
    printf("\n]\n");
    return 0;
}
