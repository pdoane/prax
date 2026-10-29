// The trees of the flex benchmark, and the engines that lay them out. Each engine's adapter builds its own tree from
// the description, changes it, lays it out, and reports where it placed each node and the work that its last layout
// did.

#pragma once

#include "tools/bench/flex_bench/flex_style.h"

#include <cstdint>
#include <memory>
#include <vector>

PR_ASSUME_NONNULL_BEGIN

namespace flex_bench {

// A tree that every engine builds. Nodes are indexed, and each has a style and an ordered list of children. The root is
// sized by the viewport that it is laid out in.
struct Tree {
    std::vector<FlexStyle> styles;
    std::vector<std::vector<uint32_t>> children;
    uint32_t root = 0;

    uint32_t add(const FlexStyle& style) {
        styles.push_back(style);
        children.emplace_back();
        return uint32_t(styles.size() - 1);
    }
};

// A node's border box in the viewport's coordinates, in pixels.
struct Box {
    float x, y, width, height;
};

// The work of a layout, as each engine counts it. A count that an engine does not keep is -1.
//
// `laidOut` is the number of boxes that the layout laid out instead of reusing. `reused` is the number of boxes whose
// earlier result, including their subtree, the layout reused. `measured` is the number of sizes that the layout
// computed for a box before laying it out at a final size: Yoga's measure-function calls, the leaves that Taffy laid
// out, and the intrinsic sizes that lib/layout computed.
struct Shape {
    int64_t laidOut = -1;
    int64_t reused = -1;
    int64_t measured = -1;

    // Counts that only lib/layout keeps (PrLayoutTreeStats): measurements made, taken from the cache and replaced;
    // measurements that final layouts used; flex containers solved on their own and laid out; items that the freeze
    // loop visited; lines laid out and reused; box fragments made; and placements published.
    int64_t measures = -1;
    int64_t measureHits = -1;
    int64_t measuresReplaced = -1;
    int64_t handoffs = -1;
    int64_t solved = -1;
    int64_t flexLaidOut = -1;
    int64_t flexed = -1;
    int64_t lines = -1;
    int64_t linesReused = -1;
    int64_t fragments = -1;
    int64_t published = -1;
};

// An engine's adapter: it builds its engine's tree from a Tree, changes it, lays it out, and reports the boxes and the
// work of its last layout.
class Engine {
public:
    virtual ~Engine() = default;

    virtual const char* name() const = 0;

    // Converts the style values of `tree` to the engine's own types, if they are not already in them. This runs outside
    // every measure, because a client holds its values in the engine's types. Build makes each node's style from the
    // converted values.
    virtual void prepare(const Tree& tree) = 0;

    // Builds the engine's tree for `tree`, which must be the tree last given to prepare, and replaces any tree that the
    // engine holds. Destroy frees the engine's tree. Each engine's adapter walks the tree recursively, in the order in
    // which the engine's interface takes its nodes.
    virtual void build(const Tree& tree) = 0;
    virtual void destroy() = 0;

    // Sets the width of the node at `index` in the last tree built to `px`.
    virtual void setWidth(uint32_t index, float px) = 0;

    // Lays the tree out in a viewport of `width` by `height`.
    virtual void layout(float width, float height) = 0;

    // Starts counting the work of the layouts that follow, or stops counting and returns the work of the last layout.
    virtual void startCounting() = 0;
    virtual Shape stopCounting() = 0;

    // Returns the border box of the node at `index` after the last layout.
    virtual Box box(uint32_t index) = 0;
};

} // namespace flex_bench

PR_ASSUME_NONNULL_END
