// The paint walk: a tree's fragments visited in painting order, which recording, hit testing and measuring share.

#include "layout/src/layout_walk.h"

#include "layout/src/layout_cache.h"

namespace pr_impl {

namespace {

// Returns the reference box of the box `fragment`, whose style is `style`, relative to the top left of its border box
// (css-transforms-1/#transform-box).
PrLayoutRect referenceBoxOf(const LayoutStyles* styles, PrLayoutStyleIndex style, const LayoutFragment* fragment) {
    const PrLayoutEffectStyle& effects = layoutEffectStyle(styles, style);
    PrLayoutRect reference = layoutRectAt(kLayoutVecZero, fragment->extent);
    bool content = effects.transformBox == PrLayoutTransformBox_ContentBox
                   || effects.transformBox == PrLayoutTransformBox_FillBox;
    if (content && fragment->kind == LayoutFragmentKind_Replaced) {
        reference = fragment->replaced.content;
    } else if (content) {
        const PrLayoutBoxStyle& box = layoutBoxStyle(styles, style);
        PrLayoutUnit basis =
            fragment->kind == LayoutFragmentKind_Box ? layoutUnitMax(fragment->box.containingInline, kLayoutUnitZero)
                                                     : kLayoutUnitZero;
        PrLayoutUnit inset[PrRectSide_Count];
        for (uint32_t side = 0; side < PrRectSide_Count; ++side)
            inset[side] = box.border[side] + layoutResolve(styles, box.padding[side], basis);
        reference.left += inset[PrRectSide_Left];
        reference.top += inset[PrRectSide_Top];
        reference.right = layoutUnitMax(reference.left, reference.right - inset[PrRectSide_Right]);
        reference.bottom = layoutUnitMax(reference.top, reference.bottom - inset[PrRectSide_Bottom]);
    }
    return reference;
}

} // namespace

LayoutResolvedTransform layoutNodeTransform(const LayoutTree* tree, LayoutNodeIndex node,
                                            const LayoutFragment* fragment) {
    const LayoutStyles* styles = tree->styles;
    PrLayoutStyleIndex style = layoutTreeNode(tree, node).style;
    const PrLayoutEffectStyle& effects = layoutEffectStyle(styles, style);
    PrLayoutRect reference = referenceBoxOf(styles, style, fragment);
    PrLayoutVec extent{layoutRectWidth(reference), layoutRectHeight(reference)};
    PrVec2 px{prLayoutUnitToPx(extent.x), prLayoutUnitToPx(extent.y)};
    LayoutResolvedTransform out{
        .matrix = prLayoutTransformResolve(&effects.transform, px),
        .origin = PrLayoutVec{reference.left + layoutResolve(styles, effects.originX, extent.x),
                              reference.top + layoutResolve(styles, effects.originY, extent.y)},
        .originZ = effects.originZ,
        .reference = px,
        .perspective = 0,
        .backfaceHidden = effects.backfaceHidden,
        .perspectiveOrigin = kLayoutVecZero,
        .scroller = kPrLayoutNodeNone,
    };
    // A perspective applies to the transforms of the box's children, and only a box that a transform applies to has
    // one: not an inline box. Such a box holds every box in its subtree, so a child is placed in it.
    LayoutNodeIndex parent = layoutTreeNode(tree, node).parent;
    if (parent == kPrLayoutNodeNone)
        return out;
    PrLayoutStyleIndex parentStyle = layoutTreeNode(tree, parent).style;
    float perspective = layoutEffectStyle(styles, parentStyle).perspective;
    if (perspective <= 0 || layoutIsInlineBox(tree, parent))
        return out;
    LayoutPlacement placement = layoutCachePlacement(tree->cache, node);
    const LayoutFragment* parentFragment = layoutCachePlaced(tree->cache, parent);
    if (placement.parent != parent || parentFragment == nullptr || parentFragment->kind == LayoutFragmentKind_Around)
        return out;
    const PrLayoutEffectStyle& parentEffects = layoutEffectStyle(styles, parentStyle);
    PrLayoutRect parentReference = referenceBoxOf(styles, parentStyle, parentFragment);
    PrLayoutVec point{
        parentReference.left
            + layoutResolve(styles, parentEffects.perspectiveOriginX, layoutRectWidth(parentReference)),
        parentReference.top
            + layoutResolve(styles, parentEffects.perspectiveOriginY, layoutRectHeight(parentReference))};
    out.perspective = perspective;
    out.perspectiveOrigin = point - placement.offset;
    if (layoutFragmentScrolls(tree, parentFragment))
        out.scroller = parent;
    return out;
}

PrDMatrix4 layoutPlaneMap(const LayoutResolvedTransform& transform, const PrMatrix4& matrix, PrLayoutVec scroll) {
    double o[3] = {prLayoutUnitToPxDouble(transform.origin.x), prLayoutUnitToPxDouble(transform.origin.y),
                   double(transform.originZ)};
    // T(o) * matrix * T(-o): each row of T(o) * matrix adds o times the matrix's last row, and T(-o) moves each row's
    // last column by its first three columns times -o.
    PrDMatrix4 out{};
    for (uint32_t row = 0; row < 4; ++row) {
        for (uint32_t column = 0; column < 4; ++column) {
            double value = double(matrix.m[row][column]);
            if (row < 3)
                value += o[row] * double(matrix.m[3][column]);
            out.m[row][column] = value;
        }
        out.m[row][3] -= out.m[row][0] * o[0] + out.m[row][1] * o[1] + out.m[row][2] * o[2];
    }
    if (transform.perspective > 0) {
        // T(p) * perspective(d) * T(-p) on the left: x and y lose p times z / d, and w loses z / d.
        double d = double(transform.perspective);
        double p[2] = {prLayoutUnitToPxDouble(transform.perspectiveOrigin.x + scroll.x) / d,
                       prLayoutUnitToPxDouble(transform.perspectiveOrigin.y + scroll.y) / d};
        for (uint32_t column = 0; column < 4; ++column) {
            double z = out.m[2][column];
            out.m[0][column] -= p[0] * z;
            out.m[1][column] -= p[1] * z;
            out.m[3][column] -= z / d;
        }
    }
    return out;
}

PrDMatrix3 layoutPlaneHomography(const PrDMatrix4& map, PrLayoutVec offset) {
    PrDMatrix3 plane = prDMatrix3FromDMatrix4(&map);
    if (offset == kLayoutVecZero)
        return plane;
    PrDMatrix3 to = prDMatrix3FromDAffine2(layoutTranslation(kLayoutVecZero - offset));
    PrDMatrix3 from = prDMatrix3FromDAffine2(layoutTranslation(offset));
    PrDMatrix3 inner = prDMatrix3Concat(&to, &plane);
    return prDMatrix3Concat(&inner, &from);
}

PrDMatrix3 layoutBoxPlane(const LayoutTree* tree, const LayoutFragment* fragment, PrLayoutVec offset,
                          bool* PR_NULLABLE hidden) {
    LayoutResolvedTransform transform = layoutNodeTransform(tree, fragment->node, fragment);
    PrLayoutVec scroll = kLayoutVecZero;
    if (transform.scroller != kPrLayoutNodeNone)
        scroll = layoutFragmentScroll(tree, layoutCachePlaced(tree->cache, transform.scroller));
    PrDMatrix4 map = layoutPlaneMap(transform, transform.matrix, scroll);
    if (hidden != nullptr)
        *hidden = transform.backfaceHidden && prDMatrix4ShowsBack(&map);
    return layoutPlaneHomography(map, offset);
}

bool layoutFragmentPaintsWhole(const LayoutTree* tree, const LayoutFragment* fragment) {
    return fragment->node != kPrLayoutNodeNone && fragment->kind != LayoutFragmentKind_Around
           && layoutPaintsWholeAs(tree, fragment->node, layoutTreeNode(tree, fragment->node).style);
}

const PrLayoutBoxStyle* PR_NULLABLE layoutOverflowBox(const LayoutTree* tree, const LayoutFragment* fragment) {
    if (fragment->node == kPrLayoutNodeNone || layoutTreeNode(tree, fragment->node).parent == kPrLayoutNodeNone
        || fragment->kind != LayoutFragmentKind_Box)
        return nullptr;
    return &layoutBoxStyle(tree->styles, layoutTreeNode(tree, fragment->node).style);
}

LayoutClips layoutFragmentClips(const LayoutTree* tree, const LayoutFragment* fragment) {
    const PrLayoutBoxStyle* box = layoutOverflowBox(tree, fragment);
    return box != nullptr ? layoutClips(*box) : LayoutClips{false, false};
}

PrLayoutRect layoutPaddingBox(const LayoutTree* tree, const LayoutFragment* fragment, PrLayoutVec offset) {
    const PrLayoutBoxStyle& box = layoutBoxStyle(tree->styles, layoutTreeNode(tree, fragment->node).style);
    PrLayoutRect rect = layoutRectAt(offset, fragment->extent);
    rect.left += box.border[PrRectSide_Left];
    rect.top += box.border[PrRectSide_Top];
    rect.right = layoutUnitMax(rect.left, rect.right - box.border[PrRectSide_Right]);
    rect.bottom = layoutUnitMax(rect.top, rect.bottom - box.border[PrRectSide_Bottom]);
    return rect;
}

PrLayoutVec layoutViewportScroll(const LayoutTree* tree, const LayoutFragment* viewport) {
    return layoutScrollClamp(tree->viewportScroll, layoutScrollRange(viewport->extent, viewport->overflow));
}

bool layoutFragmentClipped(const LayoutTree* tree, const LayoutFragment* fragment) {
    const PrLayoutBoxStyle* box = layoutOverflowBox(tree, fragment);
    return box != nullptr && box->clipped;
}

bool layoutFragmentScrolls(const LayoutTree* tree, const LayoutFragment* fragment) {
    const PrLayoutBoxStyle* box = layoutOverflowBox(tree, fragment);
    if (box == nullptr)
        return false;
    LayoutClips clips = layoutClips(*box);
    return clips.x && clips.y && layoutIsScrollContainer(*box);
}

PrLayoutVec layoutFragmentScroll(const LayoutTree* tree, const LayoutFragment* fragment) {
    const PrLayoutBoxStyle& box = layoutBoxStyle(tree->styles, layoutTreeNode(tree, fragment->node).style);
    PrLayoutRect padding = layoutPaddingBox(tree, fragment, kLayoutVecZero);
    PrLayoutVec port{layoutRectWidth(padding), layoutRectHeight(padding)};
    return layoutScrollClamp(layoutTreeNode(tree, fragment->node).scroll,
                             layoutScrollRange(port, layoutScrollContent(fragment, box)));
}

bool layoutInOwningInline(const LayoutTree* tree, LayoutNodeIndex node) {
    for (LayoutNodeIndex at = layoutTreeNode(tree, node).parent; at != kPrLayoutNodeNone && layoutIsInlineBox(tree, at);
         at = layoutTreeNode(tree, at).parent) {
        if (layoutNodeInlinePaintsWhole(tree, at))
            return true;
    }
    return false;
}

void layoutContainerChain(const LayoutTree* tree, LayoutNodeIndex container, Array<LayoutNodeIndex>& out) {
    if (container == kLayoutContainerOutside)
        return;
    if (container == kLayoutContainerViewport) {
        out.push(kLayoutContainerViewport);
        return;
    }
    layoutContainerChain(tree, layoutBoxPositioned(tree, container).container, out);
    out.push(container);
}

} // namespace pr_impl
