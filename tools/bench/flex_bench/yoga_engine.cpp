// The flex benchmark's adapter for Yoga. Each node of the tree becomes a Yoga node, styled from the description with
// every property set, so that Yoga's defaults, which differ from CSS's, decide nothing. Prepare converts the styles to
// Yoga's types, and build sets them.

#include "tools/bench/flex_bench/yoga_engine.h"

#include <yoga/Yoga.h>
#include <yoga/event/event.h>

namespace flex_bench {
namespace {

using facebook::yoga::Event;
using facebook::yoga::LayoutData;

YGFlexDirection directionOf(FlexDirection direction) {
    switch (direction) {
        case FlexDirection_Column: return YGFlexDirectionColumn;
        case FlexDirection_RowReverse: return YGFlexDirectionRowReverse;
        case FlexDirection_ColumnReverse: return YGFlexDirectionColumnReverse;
        default: return YGFlexDirectionRow;
    }
}

YGWrap wrapOf(FlexWrap wrap) {
    switch (wrap) {
        case FlexWrap_Wrap: return YGWrapWrap;
        case FlexWrap_WrapReverse: return YGWrapWrapReverse;
        default: return YGWrapNoWrap;
    }
}

// Returns the Yoga alignment for `align`. `normal` is the value of FlexAlign_Normal for the property.
YGAlign alignOf(FlexAlign align, YGAlign normal) {
    switch (align) {
        case FlexAlign_FlexStart: return YGAlignFlexStart;
        case FlexAlign_FlexEnd: return YGAlignFlexEnd;
        case FlexAlign_Center: return YGAlignCenter;
        case FlexAlign_Stretch: return YGAlignStretch;
        case FlexAlign_SpaceBetween: return YGAlignSpaceBetween;
        case FlexAlign_SpaceAround: return YGAlignSpaceAround;
        case FlexAlign_SpaceEvenly: return YGAlignSpaceEvenly;
        case FlexAlign_Baseline: return YGAlignBaseline;
        default: return normal;
    }
}

YGJustify justifyOf(FlexAlign align) {
    switch (align) {
        case FlexAlign_FlexEnd: return YGJustifyFlexEnd;
        case FlexAlign_Center: return YGJustifyCenter;
        case FlexAlign_SpaceBetween: return YGJustifySpaceBetween;
        case FlexAlign_SpaceAround: return YGJustifySpaceAround;
        case FlexAlign_SpaceEvenly: return YGJustifySpaceEvenly;
        default: return YGJustifyFlexStart;
    }
}

// A style's values in Yoga's types. Prepare converts them, and build sets them.
struct YogaStyle {
    YGDisplay display;
    YGFlexDirection direction;
    YGWrap wrap;
    YGAlign alignItems;
    YGAlign alignSelf;
    YGAlign alignContent;
    YGJustify justifyContent;
    float grow;
    float shrink;
    YGValue basis;
    YGValue width;
    YGValue height;
    YGValue minWidth;           // a limit: undefined if it is none
    YGValue minHeight;
    YGValue maxWidth;
    YGValue maxHeight;
    float margin[4];            // by YGEdge of top, right, bottom and left
    uint8_t marginAuto;         // one bit per side whose margin is auto
    float padding[4];
    float border[4];
    float gap;
    float aspectRatio;          // undefined if there is none
    YGPositionType position;
    YGOverflow overflow;
    YGValue inset[4];
};

YGValue valueOf(FlexLength length) {
    switch (length.kind) {
        case FlexLengthKind_Px: return YGValue{length.value, YGUnitPoint};
        case FlexLengthKind_Percent: return YGValue{length.value, YGUnitPercent};
        default: return YGValueAuto;
    }
}

// Returns a limit. Yoga has no auto limit, so auto maps to none.
YGValue limitOf(FlexLength length) {
    return length.kind == FlexLengthKind_Auto ? YGValueUndefined : valueOf(length);
}

YogaStyle projectStyle(const FlexStyle& style) {
    YogaStyle out{
        .display = style.display == FlexDisplay_None ? YGDisplayNone : YGDisplayFlex,
        .direction = directionOf(style.direction),
        .wrap = wrapOf(style.wrap),
        .alignItems = alignOf(style.alignItems, YGAlignStretch),
        .alignSelf = alignOf(style.alignSelf, YGAlignAuto),
        .alignContent = alignOf(style.alignContent, YGAlignStretch),
        .justifyContent = justifyOf(style.justifyContent),
        .grow = style.grow,
        .shrink = style.shrink,
        .basis = valueOf(style.basis),
        .width = valueOf(style.width),
        .height = valueOf(style.height),
        .minWidth = limitOf(style.minWidth),
        .minHeight = limitOf(style.minHeight),
        .maxWidth = limitOf(style.maxWidth),
        .maxHeight = limitOf(style.maxHeight),
        .marginAuto = style.marginAuto,
        .gap = style.gap,
        .aspectRatio = style.aspectRatio > 0 ? style.aspectRatio : YGUndefined,
        .position = style.position == FlexPosition_Absolute   ? YGPositionTypeAbsolute
                    : style.position == FlexPosition_Relative ? YGPositionTypeRelative
                                                              : YGPositionTypeStatic,
        .overflow = style.overflow == FlexOverflow_Scroll ? YGOverflowScroll : YGOverflowVisible,
    };
    for (int side = 0; side < 4; ++side) {
        out.margin[side] = style.margin[side];
        out.padding[side] = style.padding[side];
        out.border[side] = style.border[side];
        out.inset[side] = valueOf(style.inset[side]);
    }
    return out;
}

// Sets a length through the setter for its unit. An undefined length, such as a limit that is none, is set through the
// setter for points.
template <typename Px, typename Percent, typename Auto>
void setLength(YGNodeRef node, YGValue value, Px px, Percent percent, Auto automatic) {
    switch (value.unit) {
        case YGUnitPercent: percent(node, value.value); break;
        case YGUnitAuto: automatic(node); break;
        default: px(node, value.value); break;
    }
}
template <typename Px, typename Percent>
void setLimit(YGNodeRef node, YGValue value, Px px, Percent percent) {
    if (value.unit == YGUnitPercent)
        percent(node, value.value);
    else
        px(node, value.value);
}

void applyStyle(YGNodeRef node, const YogaStyle& style) {
    YGNodeStyleSetDisplay(node, style.display);
    YGNodeStyleSetPositionType(node, style.position);
    YGNodeStyleSetOverflow(node, style.overflow);
    YGNodeStyleSetAspectRatio(node, style.aspectRatio);
    YGNodeStyleSetFlexDirection(node, style.direction);
    YGNodeStyleSetFlexWrap(node, style.wrap);
    YGNodeStyleSetAlignItems(node, style.alignItems);
    YGNodeStyleSetAlignSelf(node, style.alignSelf);
    YGNodeStyleSetAlignContent(node, style.alignContent);
    YGNodeStyleSetJustifyContent(node, style.justifyContent);
    YGNodeStyleSetFlexGrow(node, style.grow);
    YGNodeStyleSetFlexShrink(node, style.shrink);
    setLength(node, style.basis, YGNodeStyleSetFlexBasis, YGNodeStyleSetFlexBasisPercent, YGNodeStyleSetFlexBasisAuto);
    setLength(node, style.width, YGNodeStyleSetWidth, YGNodeStyleSetWidthPercent, YGNodeStyleSetWidthAuto);
    setLength(node, style.height, YGNodeStyleSetHeight, YGNodeStyleSetHeightPercent, YGNodeStyleSetHeightAuto);
    setLimit(node, style.minWidth, YGNodeStyleSetMinWidth, YGNodeStyleSetMinWidthPercent);
    setLimit(node, style.minHeight, YGNodeStyleSetMinHeight, YGNodeStyleSetMinHeightPercent);
    setLimit(node, style.maxWidth, YGNodeStyleSetMaxWidth, YGNodeStyleSetMaxWidthPercent);
    setLimit(node, style.maxHeight, YGNodeStyleSetMaxHeight, YGNodeStyleSetMaxHeightPercent);
    const YGEdge edges[4] = {YGEdgeTop, YGEdgeRight, YGEdgeBottom, YGEdgeLeft};
    for (int side = 0; side < 4; ++side) {
        if ((style.marginAuto & (1u << side)) != 0)
            YGNodeStyleSetMarginAuto(node, edges[side]);
        else
            YGNodeStyleSetMargin(node, edges[side], style.margin[side]);
        YGNodeStyleSetPadding(node, edges[side], style.padding[side]);
        YGNodeStyleSetBorder(node, edges[side], style.border[side]);
        setLength(
            node, style.inset[side], [&](YGNodeRef n, float v) { YGNodeStyleSetPosition(n, edges[side], v); },
            [&](YGNodeRef n, float v) { YGNodeStyleSetPositionPercent(n, edges[side], v); },
            [&](YGNodeRef n) { YGNodeStyleSetPositionAuto(n, edges[side]); });
    }
    YGNodeStyleSetGap(node, YGGutterAll, style.gap);
}

class Yoga final : public Engine {
public:
    Yoga() : config_(YGConfigNew()) {
        // A scale of 0 disables rounding. Yoga's default of 1 rounds to whole pixels.
        YGConfigSetPointScaleFactor(config_, 0);
    }
    ~Yoga() override {
        destroy();
        YGConfigFree(config_);
    }

    const char* name() const override { return "yoga"; }

    void prepare(const Tree& tree) override {
        styles_.resize(tree.styles.size());
        for (size_t i = 0; i < tree.styles.size(); ++i)
            styles_[i] = projectStyle(tree.styles[i]);
    }

    void build(const Tree& tree) override {
        destroy();
        nodes_.resize(tree.styles.size());
        parents_.assign(tree.styles.size(), UINT32_MAX);
        root_ = make(tree, tree.root);
    }

    void destroy() override {
        if (root_ != nullptr)
            YGNodeFreeRecursive(root_);
        root_ = nullptr;
        nodes_.clear();
    }

    void setWidth(uint32_t index, float px) override { YGNodeStyleSetWidth(nodes_[index], px); }

    void layout(float width, float height) override { YGNodeCalculateLayout(root_, width, height, YGDirectionLTR); }

    void startCounting() override {
        Event::subscribe([this](YGNodeConstRef, Event::Type type, Event::Data data) {
            if (type == Event::LayoutPassEnd)
                last_ = *data.get<Event::LayoutPassEnd>().layoutData;
        });
    }

    Shape stopCounting() override {
        Event::reset();
        return Shape{.laidOut = last_.layouts,
                     .reused = last_.cachedLayouts,
                     .measured = last_.measures};
    }

    Box box(uint32_t index) override {
        Box out{0, 0, YGNodeLayoutGetWidth(nodes_[index]), YGNodeLayoutGetHeight(nodes_[index])};
        for (uint32_t at = index; at != UINT32_MAX; at = parents_[at]) {
            out.x += YGNodeLayoutGetLeft(nodes_[at]);
            out.y += YGNodeLayoutGetTop(nodes_[at]);
        }
        return out;
    }

private:
    // Makes the node at `index`, then its children, inserting each child as it is made.
    YGNodeRef make(const Tree& tree, uint32_t index) {
        YGNodeRef node = YGNodeNewWithConfig(config_);
        applyStyle(node, styles_[index]);
        nodes_[index] = node;
        const std::vector<uint32_t>& children = tree.children[index];
        for (size_t at = 0; at < children.size(); ++at) {
            YGNodeInsertChild(node, make(tree, children[at]), at);
            parents_[children[at]] = index;
        }
        return node;
    }

    YGConfigRef config_;
    YGNodeRef root_ = nullptr;
    std::vector<YGNodeRef> nodes_;
    std::vector<YogaStyle> styles_;     // by node: the style that prepare converted
    std::vector<uint32_t> parents_;
    LayoutData last_{};
};

} // namespace

std::unique_ptr<Engine> makeYoga() {
    return std::make_unique<Yoga>();
}

} // namespace flex_bench
