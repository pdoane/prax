// The flex benchmark's adapter for lib/layout. Each node of the tree becomes an element of a layout tree: a flex
// container, styled through the C API as any client styles one, with every property that the node's style sets, whether
// or not lib/layout lays that property out yet. Prepare converts the styles to lib/layout's style groups, and build
// interns them.

#include "tools/bench/flex_bench/prax_engine.h"

#include "pr/layout_query.h"
#include "pr/layout_style.h"
#include "pr/layout_tree.h"

#include <vector>

namespace flex_bench {
namespace {

PrLayoutLength lengthOf(FlexLength length) {
    switch (length.kind) {
        case FlexLengthKind_Px: return prLayoutLengthPx(length.value);
        case FlexLengthKind_Percent: return prLayoutLengthPercent(length.value);
        default: return prLayoutLengthAuto();
    }
}

// Returns a maximum size, mapping auto to none.
PrLayoutLength maximumOf(FlexLength length) {
    return length.kind == FlexLengthKind_Auto ? prLayoutLengthNone() : lengthOf(length);
}

// Returns the alignment value of `align`. `normal` is the value of FlexAlign_Normal for the property.
PrLayoutAlignment alignmentOf(FlexAlign align, PrLayoutAlignment normal) {
    switch (align) {
        case FlexAlign_FlexStart: return PrLayoutAlignment_FlexStart;
        case FlexAlign_FlexEnd: return PrLayoutAlignment_FlexEnd;
        case FlexAlign_Center: return PrLayoutAlignment_Center;
        case FlexAlign_Stretch: return PrLayoutAlignment_Stretch;
        case FlexAlign_SpaceBetween: return PrLayoutAlignment_SpaceBetween;
        case FlexAlign_SpaceAround: return PrLayoutAlignment_SpaceAround;
        case FlexAlign_SpaceEvenly: return PrLayoutAlignment_SpaceEvenly;
        case FlexAlign_Baseline: return PrLayoutAlignment_Baseline;
        default: return normal;
    }
}

PrLayoutFlexStyle flexOf(const FlexStyle& style) {
    PrLayoutFlexStyle flex = prLayoutFlexStyleDefault();
    switch (style.direction) {
        case FlexDirection_Column: flex.direction = PrLayoutFlexDirection_Column; break;
        case FlexDirection_RowReverse: flex.direction = PrLayoutFlexDirection_RowReverse; break;
        case FlexDirection_ColumnReverse: flex.direction = PrLayoutFlexDirection_ColumnReverse; break;
        default: flex.direction = PrLayoutFlexDirection_Row; break;
    }
    if (style.wrap == FlexWrap_Wrap)
        flex.wrap = PrLayoutFlexWrap_Wrap;
    else if (style.wrap == FlexWrap_WrapReverse)
        flex.wrap = PrLayoutFlexWrap_WrapReverse;
    else
        flex.wrap = PrLayoutFlexWrap_NoWrap;
    flex.alignItems = alignmentOf(style.alignItems, PrLayoutAlignment_Normal);
    flex.alignSelf = alignmentOf(style.alignSelf, PrLayoutAlignment_Auto);
    flex.alignContent = alignmentOf(style.alignContent, PrLayoutAlignment_Normal);
    flex.justifyContent = alignmentOf(style.justifyContent, PrLayoutAlignment_Normal);
    flex.grow = style.grow;
    flex.shrink = style.shrink;
    flex.basis = lengthOf(style.basis);
    flex.rowGap = prLayoutLengthPx(style.gap);
    flex.columnGap = prLayoutLengthPx(style.gap);
    flex.order = style.order;
    return flex;
}

PrLayoutBoxStyle boxOf(const FlexStyle& style) {
    PrLayoutBoxStyle box = prLayoutBoxStyleDefault();
    // A leaf with words is a block container that holds the lines of words.
    if (style.display == FlexDisplay_None)
        box.display = PrLayoutDisplay_None;
    else if (style.words != 0)
        box.display = PrLayoutDisplay_FlowRoot;
    else
        box.display = PrLayoutDisplay_Flex;
    if (style.overflow == FlexOverflow_Scroll) {
        box.overflowX = PrLayoutOverflow_Scroll;
        box.overflowY = PrLayoutOverflow_Scroll;
    }
    box.width = lengthOf(style.width);
    box.height = lengthOf(style.height);
    box.minWidth = lengthOf(style.minWidth);
    box.minHeight = lengthOf(style.minHeight);
    box.maxWidth = maximumOf(style.maxWidth);
    box.maxHeight = maximumOf(style.maxHeight);
    box.aspectRatio = style.aspectRatio;
    if (style.position == FlexPosition_Absolute)
        box.position = PrLayoutPosition_Absolute;
    else if (style.position == FlexPosition_Relative)
        box.position = PrLayoutPosition_Relative;
    else
        box.position = PrLayoutPosition_Static;
    for (int side = 0; side < PrRectSide_Count; ++side) {
        if ((style.marginAuto & (1u << side)) != 0)
            box.margin[side] = prLayoutLengthAuto();
        else
            box.margin[side] = prLayoutLengthPx(style.margin[side]);
        box.padding[side] = prLayoutLengthPx(style.padding[side]);
        box.border[side] = prLayoutUnitFromPx(style.border[side]);
        box.inset[side] = lengthOf(style.inset[side]);
    }
    return box;
}

class Prax final : public Engine {
public:
    ~Prax() override { destroy(); }

    const char* name() const override { return "prax"; }

    void prepare(const Tree& tree) override {
        boxes_.resize(tree.styles.size());
        flexes_.resize(tree.styles.size());
        for (size_t i = 0; i < tree.styles.size(); ++i) {
            boxes_[i] = boxOf(tree.styles[i]);
            flexes_[i] = flexOf(tree.styles[i]);
        }
        for (uint32_t i = 0; i < kWordKinds; ++i) {
            PrLayoutBoxStyle word = prLayoutBoxStyleDefault();
            word.display = PrLayoutDisplay_InlineBlock;
            word.width = prLayoutLengthPx(float(10 + 10 * (i % 4)));
            word.height = i < 4 ? prLayoutLengthPx(20) : prLayoutLengthPercent(50);
            words_[i] = word;
        }
    }

    void build(const Tree& tree) override {
        destroy();
        // The trees hold no text, so the tree has no font and no shaper.
        PrLayoutTreeDesc desc{.font = nullptr, .shaper = nullptr, .targetPixelRatio = 1};
        tree_ = prLayoutTreeCreate(&desc);
        nodes_.resize(tree.styles.size());
        make(tree, tree.root, kPrLayoutNodeNone);
        root_ = nodes_[tree.root];
    }

    void destroy() override {
        prLayoutTreeDestroy(tree_);
        tree_ = nullptr;
    }

    void setWidth(uint32_t index, float px) override {
        PrLayoutStyle record = prLayoutStyleRecord(tree_, prLayoutNodeStyle(tree_, nodes_[index]));
        PrLayoutBoxStyle box = prLayoutBoxStyleRecord(tree_, record.groups[PrLayoutGroup_Box]);
        box.width = prLayoutLengthPx(px);
        prLayoutNodeSetStyle(tree_, nodes_[index], styleOf(box, record.groups[PrLayoutGroup_Flex]));
    }

    void layout(float width, float height) override {
        PrLayoutRoot viewport{root_, PrLayoutVec{prLayoutUnitFromPx(width), prLayoutUnitFromPx(height)}};
        prLayoutTreeLayout(tree_, &viewport, 1);
    }

    void startCounting() override {}

    Shape stopCounting() override {
        PrLayoutTreeStats stats = prLayoutTreeStats(tree_);
        return Shape{
            .laidOut = stats.blocksLaidOut,
            .reused = stats.blocksReused,
            .measured = stats.intrinsicsComputed,
            .measures = stats.measuresComputed,
            .measureHits = stats.measuresReused,
            .measuresReplaced = stats.measuresReplaced,
            .handoffs = stats.handoffsTaken,
            .solved = stats.flexSolved,
            .flexLaidOut = stats.flexLaidOut,
            .flexed = stats.flexItemsFlexed,
            .lines = stats.linesLaidOut,
            .linesReused = stats.linesReused,
            .fragments = stats.boxFragmentsMade,
            .published = stats.childrenPublished,
        };
    }

private:
    // Makes the node at `index` under `parent`, then its children under the node, in order.
    void make(const Tree& tree, uint32_t index, PrLayoutNode parent) {
        PrLayoutGroupIndex flex = prLayoutFlexStyleIntern(tree_, &flexes_[index]);
        nodes_[index] = prLayoutNodeCreateElement(tree_, parent, styleOf(boxes_[index], flex));
        for (uint32_t child : tree.children[index])
            make(tree, child, nodes_[index]);
        const FlexStyle& style = tree.styles[index];
        for (uint32_t i = 0; i < style.words; ++i) {
            uint32_t kind = (index + i) % 4 + (i == 0 && style.percentWord ? 4 : 0);
            prLayoutNodeCreateElement(tree_, nodes_[index], styleOf(words_[kind], 0));
        }
    }

    PrLayoutStyleIndex styleOf(const PrLayoutBoxStyle& box, PrLayoutGroupIndex flex) {
        PrLayoutStyle record = prLayoutStyleDefault();
        record.groups[PrLayoutGroup_Box] = prLayoutBoxStyleIntern(tree_, &box);
        record.groups[PrLayoutGroup_Flex] = flex;
        return prLayoutStyleIntern(tree_, &record);
    }

    PrLayoutTree* tree_ = nullptr;
    std::vector<PrLayoutNode> nodes_;
    PrLayoutNode root_ = kPrLayoutNodeNone;
    std::vector<PrLayoutBoxStyle> boxes_;   // by node: the style groups that prepare converted
    std::vector<PrLayoutFlexStyle> flexes_;

    // The styles of a leaf's words, by kind: four widths at a fixed height, then the same widths at a height that is a
    // percentage of the leaf's height.
    static constexpr uint32_t kWordKinds = 8;
    PrLayoutBoxStyle words_[kWordKinds];
};

} // namespace

std::unique_ptr<Engine> makePrax() {
    return std::make_unique<Prax>();
}

} // namespace flex_bench
