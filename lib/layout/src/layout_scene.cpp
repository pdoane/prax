// Scenes: a paint's recordings assembled into layers, each with its effects and the scroll containers it is in, and
// played into draw lists.

#include "layout/src/layout_scene.h"

#include "pr/alloc.h"
#include "pr/draw_list.h"

#include "layout/src/layout_walk.h"

namespace pr_impl {

namespace {

// ---------------------------------------------------------------------------------------------------------------------
// Assembling

// Returns the scene's container that `ref` names in the recording of the layer `layer`, whose containers start at
// `base` (LayoutPaintContainerRef), or kPaintScrollNone if it names none. A recording's own space is in the container
// that its layer is placed in, and a recording around it belongs to an ancestor layer.
uint32_t resolve(const LayoutTree* tree, const LayoutScene* scene, uint32_t layer, uint32_t base,
        LayoutPaintContainerRef ref) {
    switch (ref.kind) {
        case PaintRefKind_Own:
            return ref.value == kPaintScrollNone ? scene->layers.data[layer].placement : base + ref.value;
        case PaintRefKind_Container:
        case PaintRefKind_Space: {
            PrLayoutNode node = layoutNodeName(tree, layoutContainerBox(ref.value));
            bool sticky = ref.kind == PaintRefKind_Container && layoutContainerIsSticky(ref.value);
            for (uint32_t at = scene->layers.data[layer].parent; at != kPaintScrollNone;
                    at = scene->layers.data[at].parent) {
                const LayoutSceneLayer& l = scene->layers.data[at];
                if (ref.kind == PaintRefKind_Space) {
                    if (l.node == node)
                        return l.placement;
                    continue;
                }
                for (uint32_t i = l.scrollStart; i < l.scrollStart + l.scrollCount; ++i) {
                    const LayoutSceneContainer& c = scene->scrolls.data[i];
                    if (c.node == node && (c.kind == LayoutContainerKind_Sticky) == sticky)
                        return i;
                }
            }
            return kPaintScrollNone;
        }
        default: return kPaintScrollNone;
    }
}

// Returns true if `matrix` is the identity.
bool matrixIsIdentity(const PrMatrix4& matrix) {
    return prMemEquals(&matrix, &kLayoutTransformIdentity.matrix, sizeof(PrMatrix4));
}

// Returns the area that the box of `node` and what it holds may draw over, in the space of its layer: its overflow and
// its outline, or for an inline box, its block container's.
PrRect inkOf(const LayoutTree* tree, LayoutNodeIndex node, const LayoutFragment* fragment) {
    PrLayoutRect ink = layoutRectUnion(layoutRectAt(kLayoutVecZero, fragment->extent), fragment->overflow);
    const PrLayoutDecorationStyle& decoration = layoutDecorationStyle(tree->styles, layoutTreeNode(tree, node).style);
    PrLayoutUnit outline = decoration.outlineWidth + decoration.outlineOffset;
    if (decoration.outlineStyle != PrDrawBorderStyle_None && outline > kLayoutUnitZero)
        ink = layoutRectUnion(ink, PrLayoutRect{
            kLayoutUnitZero - outline, kLayoutUnitZero - outline,
            fragment->extent.x + outline, fragment->extent.y + outline,
        });
    return prLayoutRectToPx(ink);
}

// Returns true if a layer plays in a frame of its own, as a transformed layer does. The layer is transformed if
// `transformed` is set, and `fx` holds its resolved effects. In a frame, the effects resolve in the space of the
// layer's border box and apply within the containers that the layer is placed in. The root's layer plays in the
// viewport's content, whose fixed boxes stay in place as it scrolls. So the root's layer takes a frame only if an
// effect needs its border box's space. Its filter, blending and isolation apply as a group around its containers, as
// opacity does.
bool layerFramed(const LayoutSceneLayer& layer, const LayoutResolvedEffects* PR_NULLABLE fx, bool transformed) {
    if (transformed)
        return true;
    if (fx == nullptr)
        return false;
    bool spaced = fx->clipsRect || fx->clipPath != nullptr || fx->coverage != nullptr || fx->mask != nullptr ||
        fx->backdrop != nullptr;
    return layer.parent != kPaintScrollNone || spaced;
}

// Adds the layer of a box with a layer of its own, at `offset` in its parent layer's space and in the scene's container
// `placement`, after `segmentsBefore` of its parent's segments. Then adds the layers that its chunk names.
void addLayer(LayoutTree* tree, LayoutScene* scene, LayoutNodeIndex node, const LayoutFragment* fragment,
        LayoutPieceAt opening, PrLayoutVec offset, bool root, uint32_t parent, uint32_t placement,
        uint32_t segmentsBefore, uint32_t order) {
    uint32_t index = scene->layers.count;
    LayoutSceneLayer& made = scene->layers.emplace();
    made = LayoutSceneLayer{};
    made.node = layoutNodeName(tree, node);
    made.parent = parent;
    made.segmentsBefore = segmentsBefore;
    made.order = order;
    made.placement = placement;
    made.offset = offset;
    made.extent = fragment->extent;
    // An inline box's layer is placed at its container, and no transform applies to it.
    const PrLayoutEffectStyle& effects = layoutEffectStyle(tree->styles, layoutTreeNode(tree, node).style);
    made.transform = kPaintScrollNone;
    if (node == fragment->node && (effects.layer || layoutHasTransform(effects))) {
        LayoutResolvedTransform transform = layoutNodeTransform(tree, node, fragment);
        // The container of a scrolling parent is in the recording of a layer around this one, which is already added.
        uint32_t scroll = kPaintScrollNone;
        if (transform.scroller != kPrLayoutNodeNone)
            scroll = layoutSceneNodeScroll(scene, layoutNodeName(tree, transform.scroller));
        made.transform = scene->transforms.count;
        scene->transforms.push(LayoutSceneTransform{
            .resolved = transform,
            .perspectiveScroll = scroll,
            .transformed = !matrixIsIdentity(transform.matrix),
        });
    }
    made.opacity = effects.opacity;
    made.effects = kPaintScrollNone;
    if (layoutStyle(tree->styles, layoutTreeNode(tree, node).style).groups[PrLayoutGroup_Compositing] != 0) {
        LayoutResolvedEffects resolved{};
        if (layoutEffectsResolve(tree, node, fragment, opening, inkOf(tree, node, fragment), &resolved)) {
            made.effects = scene->effects.count;
            scene->effects.push(resolved);
            // A layer that blends is isolated by the group of the layer it is in.
            bool blends = resolved.blendMode != PrDrawBlendMode_Normal ||
                resolved.composite != PrDrawComposite_SourceOver;
            if (blends && parent != kPaintScrollNone)
                scene->layers.data[parent].isolates = true;
        }
    }
    uint32_t at = layoutPaintChunk(tree, node, fragment, opening, root);
    // Chunks are looked up again by index, because adding the layers that a chunk names can add chunks.
    const LayoutPaintChunk* chunk = &tree->paint.chunks.data[at];
    uint32_t base = scene->scrolls.count;
    for (uint32_t i = 0; i < chunk->scrolls.count; ++i) {
        const LayoutPaintScroll& s = chunk->scrolls.data[i];
        // What moves a sticky box is found anew for each scene, since it reads the layout of boxes around the recording
        // that holds the box.
        uint32_t sticky = kPaintScrollNone;
        LayoutSticky found{};
        if (s.kind == LayoutContainerKind_Sticky && layoutStickyFind(tree, layoutContainerBox(s.node), &found)) {
            sticky = scene->stickies.count;
            scene->stickies.push(found);
        }
        LayoutSceneContainer scroll{
            .node = layoutNodeName(tree, layoutContainerBox(s.node)),
            .parent = resolve(tree, scene, index, base, s.parent),
            .layer = index,
            .segment = s.segment,
            .clip = s.clip,
            .range = s.range,
            .portExtent = s.portExtent,
            .content = s.content,
            .userAxes = s.userAxes,
            .chainAxes = s.chainAxes,
            .kind = s.kind,
            .sticky = sticky,
            .curved = s.curved,
        };
        prMemCopy(scroll.corners, s.corners, sizeof(s.corners));
        scene->scrolls.push(scroll);
    }
    LayoutSceneLayer& layer = scene->layers.data[index];
    layer.scrollStart = base;
    layer.scrollCount = chunk->scrolls.count;
    layer.segmentStart = scene->segments.count;
    layer.segmentCount = chunk->segments.count;
    uint32_t fixedAt = 0;
    for (uint32_t i = 0; i < chunk->segments.count; ++i) {
        const LayoutPaintSegment& segment = chunk->segments.data[i];
        prDrawListRetain(segment.list);
        uint32_t fixed = 0;
        if (fixedAt < chunk->fixed.count && chunk->fixed.data[fixedAt].segment == i) {
            scene->fixedClips.push(chunk->fixed.data[fixedAt++].clip);
            fixed = scene->fixedClips.count;
        }
        scene->segments.push(LayoutSceneSegment{
            segment.list, resolve(tree, scene, index, base, segment.scroll),
            segment.offset, fixed,
        });
    }
    layer.slotStart = scene->slots.count;
    layer.slotCount = chunk->slots.count;
    for (uint32_t i = 0; i < chunk->slots.count; ++i) {
        const LayoutPaintSlot& s = chunk->slots.data[i];
        scene->slots.push(LayoutSceneSlot{
            s.embed, layoutNodeName(tree, s.node), index,
            resolve(tree, scene, index, base, s.scroll), s.segment, s.order, s.box, s.caret, s.color,
        });
    }
    layer.regionStart = scene->regions.count;
    layer.regionCount = chunk->regions.count;
    for (uint32_t i = 0; i < chunk->regions.count; ++i) {
        const LayoutPaintRegion& r = chunk->regions.data[i];
        scene->regions.push(LayoutSceneRegion{r.rect, resolve(tree, scene, index, base, r.scroll)});
    }
    for (uint32_t i = 0; i < tree->paint.chunks.data[at].layers.count; ++i) {
        const LayoutPaintChunk& named = tree->paint.chunks.data[at];
        LayoutPaintLayer l = named.layers.data[i];
        addLayer(tree, scene, l.node, l.fragment, l.opening, l.offset, false, index,
            resolve(tree, scene, index, base, l.scroll), l.segment, l.order);
    }
    scene->layers.data[index].end = scene->layers.count;
}

// ---------------------------------------------------------------------------------------------------------------------
// Frames

// What playing or searching a scene keeps for each layer it has reached: its frame and its origin in that frame. The
// frame is the nearest layer around it, or the layer itself, that a transform places, or kPaintScrollNone for the
// scene's space.
struct Frames {
    const LayoutScene* scene;
    Array<SceneLayerIndex> frame;   // by SceneLayerIndex
    Array<PrLayoutVec> origin;      // by SceneLayerIndex

    void reset() {
        frame.reset();
        origin.reset();
    }

    // Places the layer at `index` in its parent's frame, transformed if `transformed` is true. Returns its parent's
    // frame, and sets `at` to the top-left corner of its border box in that frame.
    uint32_t place(uint32_t index, bool transformed, PrLayoutVec* at) {
        if (frame.count < scene->layers.count) {
            frame.resize(scene->layers.count);
            origin.resize(scene->layers.count);
        }
        const LayoutSceneLayer& l = scene->layers.data[index];
        uint32_t outer = l.parent != kPaintScrollNone ? frame.data[l.parent] : kPaintScrollNone;
        *at = (l.parent != kPaintScrollNone ? origin.data[l.parent] : kLayoutVecZero) + l.offset;
        frame.data[index] = transformed ? index : outer;
        origin.data[index] = transformed ? kLayoutVecZero : *at;
        return outer;
    }

    // Returns true if the scene's container `scroll` is in the frame `in`: its layer is the frame's layer, or a layer
    // inside it.
    bool contains(uint32_t in, uint32_t scroll) const {
        uint32_t layer = scene->scrolls.data[scroll].layer;
        if (in == kPaintScrollNone)
            return true;
        return layer != kPaintScrollNone && layer >= in && layer < scene->layers.data[in].end;
    }

    // Returns the clip of the scene's container `scroll` in its frame.
    PrLayoutRect clipOf(uint32_t scroll) const {
        const LayoutSceneContainer& s = scene->scrolls.data[scroll];
        return s.layer != kPaintScrollNone ? layoutClipMoved(s.clip, origin.data[s.layer]) : s.clip;
    }

    // Returns true if the clip of the scene's container `scroll` in its frame holds `point`, within its corners'
    // curves.
    bool clipContains(uint32_t scroll, PrLayoutVec point) const {
        PrLayoutRect clip = clipOf(scroll);
        if (!layoutRectContains(clip, point))
            return false;
        const LayoutSceneContainer& s = scene->scrolls.data[scroll];
        if (!s.curved)
            return true;
        PrCornerRect shape{.rect = prLayoutRectToPx(clip)};
        prMemCopy(shape.corners, s.corners, sizeof(s.corners));
        return prCornerRectContains(shape, PrVec2{prLayoutUnitToPx(point.x), prLayoutUnitToPx(point.y)});
    }
};

// The effects that a layer plays with: its own, or those that a player gives for its node.
struct LayerEffects {
    float opacity;
    const PrMatrix4* PR_NULLABLE transform;           // null for the identity
    const LayoutSceneTransform* PR_NULLABLE resolved; // the origin and perspective, set if `transform` is
};

LayerEffects layerEffects(const LayoutScene* scene, const LayoutSceneLayer& layer,
        const PrLayoutSceneEffect* PR_NULLABLE effects, uint32_t count) {
    LayerEffects out{.opacity = layer.opacity};
    const LayoutSceneTransform* t = layer.transform != kPaintScrollNone ? &scene->transforms.data[layer.transform] : nullptr;
    if (t != nullptr && t->transformed)
        out.transform = &t->resolved.matrix;
    for (uint32_t i = 0; i < count; ++i) {
        if (effects[i].node != layer.node)
            continue;
        if ((effects[i].flags & PrLayoutSceneEffectFlag_Opacity) != 0)
            out.opacity = effects[i].opacity;
        if ((effects[i].flags & PrLayoutSceneEffectFlag_Transform) != 0)
            out.transform = matrixIsIdentity(effects[i].transform) ? nullptr : &effects[i].transform;
    }
    if (t == nullptr)
        out.transform = nullptr;
    out.resolved = out.transform != nullptr ? t : nullptr;
    return out;
}

// The map of a transformed layer's plane into the plane it paints on, both with its border box's top left at the
// origin, and whether it hides the back that it shows.
struct LayerPlane {
    PrDMatrix4 map;
    PrDMatrix3 homography;
    bool hidden;
};

LayerPlane planeOf(const LayerEffects& e, const PrLayoutVec* offsets) {
    const LayoutSceneTransform& t = *e.resolved;
    PrLayoutVec scroll = t.perspectiveScroll != kPaintScrollNone ? offsets[t.perspectiveScroll] : kLayoutVecZero;
    PrDMatrix4 map = layoutPlaneMap(t.resolved, *e.transform, scroll);
    return LayerPlane{
        map, layoutPlaneHomography(map, kLayoutVecZero),
        t.resolved.backfaceHidden && prDMatrix4ShowsBack(&map),
    };
}

// The largest extent of a projected layer's plane, in pixels: the bounds of content that is unbounded, as the content
// of an unbounded operator is, are cut to it, since a projected group's bounds must be finite.
constexpr float kPlaneMaxExtent = 1 << 24;

// Returns true if the slot plays before the child layer, both named by one layer's recording.
bool slotFirst(const LayoutSceneSlot& slot, const LayoutSceneLayer& child) {
    return slot.segmentsBefore < child.segmentsBefore ||
        (slot.segmentsBefore == child.segmentsBefore && slot.order < child.order);
}

// ---------------------------------------------------------------------------------------------------------------------
// Playing

// Plays a scene into a list.
struct Player {
    const LayoutScene* scene;
    PrDrawList* list;
    const PrLayoutScenePlayOptions& options;
    const PrLayoutVec* offsets;     // by the scene's container
    Frames frames;

    // The containers whose clips and scroll offsets are pushed, outermost first. Consecutive pieces in the same
    // containers share them. `scope` is the index at which the containers pushed inside the innermost layer's effects
    // begin, and a piece inside those effects closes only these containers.
    Array<uint32_t> open;
    uint32_t scope;

    Array<uint32_t> chain;          // scratch: a piece's containers, innermost first

    void reset() {
        frames.reset();
        open.reset();
        chain.reset();
    }

    // Pushes the clip and scroll offset of the scene's container `scroll`. A sticky box clips nothing, and pushes only
    // its offset.
    void push(uint32_t scroll) {
        const LayoutSceneContainer& s = scene->scrolls.data[scroll];
        PrLayoutVec offset = offsets[scroll];
        open.push(scroll);
        if (s.kind == LayoutContainerKind_Sticky) {
            prDrawListPushTransformDAffine2(list, layoutTranslation(kLayoutVecZero - offset));
            return;
        }
        // The clip is placed at its corner, and the content is placed from there, so that neither carries the corner's
        // offset in its coordinates.
        PrLayoutRect clip = frames.clipOf(scroll);
        PrLayoutVec corner{clip.left, clip.top};
        prDrawListPushTransformDAffine2(list, layoutTranslation(corner));
        PrRect rect = prLayoutRectToPx(layoutRectMoved(clip, kLayoutVecZero - corner));
        if (s.curved) {
            PrCornerRect shape{.rect = rect};
            prMemCopy(shape.corners, s.corners, sizeof(s.corners));
            prDrawListPushClipCornerRect(list, shape, PrDrawEdges_Snapped);
        } else {
            prDrawListPushClipRect(list, rect, PrDrawEdges_Snapped);
        }
        prDrawListPushTransformDAffine2(list, layoutTranslation(kLayoutVecZero - corner - offset));
    }

    // Pops the open containers past the first `depth`.
    void closeTo(uint32_t depth) {
        for (; open.count > depth; --open.count) {
            prDrawListPopTransform(list);
            if (scene->scrolls.data[open.data[open.count - 1]].kind == LayoutContainerKind_Sticky)
                continue;
            prDrawListPopClip(list);
            prDrawListPopTransform(list);
        }
    }

    // Leaves the scene's container `scroll` open, along with the containers it is in, as far as the frame `frame` holds
    // them. Keeps the scope's containers that are among them, and pops and pushes the rest.
    void enter(uint32_t scroll, uint32_t frame) {
        chain.clear();
        for (uint32_t at = scroll; at != kPaintScrollNone && frames.contains(frame, at);
                at = scene->scrolls.data[at].parent)
            chain.push(at);
        uint32_t kept = scope;
        while (kept < open.count && kept - scope < chain.count && open.data[kept] == chain.data[chain.count - 1 - (kept - scope)])
            ++kept;
        closeTo(kept);
        for (uint32_t i = chain.count - (kept - scope); i-- > 0;)
            push(chain.data[i]);
    }

    // Begins the scope of a layer's effects, inside which the containers open around the effects stay open. Returns the
    // enclosing scope.
    uint32_t beginScope() {
        uint32_t outer = scope;
        scope = open.count;
        return outer;
    }
    void endScope(uint32_t outer) {
        closeTo(scope);
        scope = outer;
    }

    // Returns how far the open containers' scroll offsets move what is played in them.
    PrLayoutVec scrolled() const {
        PrLayoutVec total = kLayoutVecZero;
        for (uint32_t i = 0; i < open.count; ++i)
            total = total + offsets[open.data[i]];
        return total;
    }

    // Plays a segment of the layer at `layer`, in its container. A segment fixed to the viewport is clipped where its
    // box is, and its list is moved back by the scroll offsets, so that it stays with the viewport.
    void segment(uint32_t layer, const LayoutSceneSegment& segment) {
        enter(segment.scroll, frames.frame.data[layer]);
        PrLayoutVec at = frames.origin.data[layer] + segment.offset;
        if (segment.fixed == 0) {
            prDrawListEmbed(list, segment.list, layoutOffset(at));
            return;
        }
        PrLayoutRect clip = layoutRectMoved(scene->fixedClips.data[segment.fixed - 1], at);
        prDrawListPushClipRect(list, prLayoutRectToPx(clip), PrDrawEdges_Snapped);
        prDrawListEmbed(list, segment.list, layoutOffset(scrolled()));
        prDrawListPopClip(list);
    }

    // Plays the root's background layers over the canvas, in the viewport's container.
    void canvas() {
        for (uint32_t i = 0; i < scene->canvasRuns.count; ++i) {
            const LayoutSceneCanvasRun& run = scene->canvasRuns.data[i];
            enter(0, kPaintScrollNone);
            prDrawListEmbed(list, run.list, layoutOffset(run.fixed ? scrolled() : kLayoutVecZero));
        }
    }

    // Plays the content of a slot in its container, clipped to its content box, with the box's top-left corner as the
    // origin.
    void slot(const LayoutSceneSlot& slot) {
        if (slot.caret) {
            if (scene->caretHidden)
                return;
            enter(slot.scroll, frames.frame.data[slot.layer]);
            PrLayoutRect bar = layoutRectMoved(slot.box, frames.origin.data[slot.layer]);
            prDrawListRect(list, prLayoutRectToPx(bar), {.color = slot.color});
            return;
        }
        if (options.embed == nullptr)
            return;
        enter(slot.scroll, frames.frame.data[slot.layer]);
        PrLayoutRect box = layoutRectMoved(slot.box, frames.origin.data[slot.layer]);
        PrLayoutVec extent{layoutRectWidth(box), layoutRectHeight(box)};
        prDrawListPushTransformDAffine2(list, layoutTranslation(PrLayoutVec{box.left, box.top}));
        prDrawListPushClipRect(list, prLayoutRectToPx(layoutRectAt(kLayoutVecZero, extent)), PrDrawEdges_Snapped);
        options.embed(options.embedContext, slot.embed, extent, list);
        prDrawListPopClip(list);
        prDrawListPopTransform(list);
    }

    // Plays the layer at `index` and its subtree: its effects around everything it holds, a transform placed by its own
    // container, and each segment, slot and child in its own container, at the point where its recording names it. A
    // transform that is not affine on the layer's plane plays the layer as a projected group, whose contents are
    // recorded into a list of their own first, so that the group's bounds are what they draw.
    void layer(uint32_t index) {
        const LayoutSceneLayer& l = scene->layers.data[index];
        const LayoutResolvedEffects* fx = l.effects != kPaintScrollNone ? &scene->effects.data[l.effects] : nullptr;
        // A layer that holds no recording, slot or layer plays nothing, and neither do its effects, except a filter,
        // which may draw from nothing, as a flood or an image does, and a backdrop filter, which draws what lies
        // beneath it.
        bool draws = fx != nullptr && (fx->filter != nullptr || fx->backdrop != nullptr);
        if ((l.segmentCount == 0 && l.slotCount == 0 && l.end == index + 1 && !draws) || (fx != nullptr && fx->hidden))
            return;
        LayerEffects e = layerEffects(scene, l, options.effects, options.effectCount);
        float opacity = e.opacity;
        bool transformed = e.transform != nullptr;
        LayerPlane plane{};
        PrDAffine2 affine{};
        bool projected = false;
        if (transformed) {
            plane = planeOf(e, offsets);
            if (plane.hidden)
                return;
            projected = !prDAffine2FromDMatrix3(&plane.homography, &affine);
        }
        bool framed = layerFramed(l, fx, transformed);
        PrLayoutVec at{};
        uint32_t outer = frames.place(index, framed, &at);
        bool grouped = !framed && (fx != nullptr || opacity < 1 || l.isolates) && !projected;
        // A framed layer is placed by its own containers, which stay open after it. Its pieces are in its own frame,
        // which no container open around it is in. A group holds the containers of its layer's pieces.
        PrDrawList* around = list;
        if (framed) {
            enter(l.placement, outer);
            prDrawListPushTransformDAffine2(list, layoutTranslation(at));
            if (projected)
                list = prDrawListCreate();
            else if (transformed)
                prDrawListPushTransformSnapped(list, affine, PrDrawSnapFlag_Apart);
        } else if (grouped) {
            closeTo(scope);
        }
        if (grouped && fx == nullptr) {
            PrDrawGroup group = prDrawGroupDefault();
            group.opacity = opacity;
            group.isolated = l.isolates;
            prDrawListPushGroup(list, &group);
        }
        // A projected layer's opacity applies where its plane lands, outside its effects. Effects outside a frame
        // filter all that the containers around them show.
        LayoutEffectsPushed pushed{};
        if (fx != nullptr) {
            LayoutResolvedEffects played = *fx;
            if (!framed)
                played.filterRegion = PrRect{};
            pushed = layoutEffectsPush(played, projected ? 1 : opacity, l.isolates, list);
        }
        bool scoped = framed || grouped;
        uint32_t outerScope = scoped ? beginScope() : scope;
        uint32_t played = 0;
        auto playTo = [&](uint32_t end) {
            for (; played < end && played < l.segmentCount; ++played)
                segment(index, scene->segments.data[l.segmentStart + played]);
        };
        uint32_t s = l.slotStart;
        uint32_t slotEnd = l.slotStart + l.slotCount;
        auto slotsTo = [&](const LayoutSceneLayer* PR_NULLABLE child) {
            for (; s < slotEnd && (child == nullptr || slotFirst(scene->slots.data[s], *child)); ++s) {
                playTo(scene->slots.data[s].segmentsBefore);
                slot(scene->slots.data[s]);
            }
        };
        for (uint32_t child = index + 1; child < l.end; child = scene->layers.data[child].end) {
            slotsTo(&scene->layers.data[child]);
            playTo(scene->layers.data[child].segmentsBefore);
            layer(child);
        }
        slotsTo(nullptr);
        playTo(l.segmentCount);
        if (scoped)
            endScope(outerScope);
        if (fx != nullptr)
            layoutEffectsPop(pushed, list);
        else if (grouped)
            prDrawListPopGroup(list);
        if (projected) {
            PrDrawList* contents = list;
            list = around;
            prDrawListFreeze(contents);
            PrRect largest{-kPlaneMaxExtent, -kPlaneMaxExtent, kPlaneMaxExtent, kPlaneMaxExtent};
            PrRect bounds = prRectIntersect(prDrawListBounds(contents), largest);
            if (!prRectIsEmpty(bounds)) {
                PrMatrix4 projection = prMatrix4FromDMatrix4(&plane.map);
                PrDrawGroup group = prDrawGroupDefault();
                group.bounds = bounds;
                group.opacity = opacity;
                group.projection = &projection;
                prDrawListPushGroup(list, &group);
                prDrawListEmbed(list, contents, PrDVec2{0, 0});
                prDrawListPopGroup(list);
            }
            prDrawListRelease(contents);
            prDrawListPopTransform(list);
        } else if (transformed) {
            prDrawListPopTransform(list);
            prDrawListPopTransform(list);
        } else if (framed) {
            prDrawListPopTransform(list);
        }
    }
};

// ---------------------------------------------------------------------------------------------------------------------
// Scrolling

// Returns the container around `scroll` in the scene, across its layers, or kPaintScrollNone past the viewport.
uint32_t containerAround(const LayoutScene* scene, uint32_t scroll) {
    return scene->scrolls.data[scroll].parent;
}

// Returns the scroll container that is `scroll` or encloses it, skipping boxes that only clip.
uint32_t scrollerOf(const LayoutScene* scene, uint32_t scroll) {
    while (scroll != kPaintScrollNone && scene->scrolls.data[scroll].kind != LayoutContainerKind_Scroll)
        scroll = containerAround(scene, scroll);
    return scroll;
}

// Finds the innermost container that holds a point, and whether the point is in the wheel region. Each piece of the
// scene is tested in its own containers, in painting order, so the last container found is the topmost. A piece in no
// container is in the viewport's container, so a user's scroll over the piece reaches the viewport.
struct ScrollFinder {
    const LayoutScene* scene;
    const PrLayoutSceneEffect* PR_NULLABLE effects;
    uint32_t count;
    const PrLayoutVec* offsets;     // by the scene's container
    uint32_t found;
    uint32_t slot;                  // the slot found, if the last found is one
    PrLayoutVec slotPoint;
    bool blocks;                    // a part of the wheel region holds the point
    Frames frames;

    // Maps `point`, in the frame `frame`, into the content of the scene's container `scroll`, through the containers
    // that hold it and that the frame holds. Returns false if a clip excludes the point.
    bool enterChain(uint32_t scroll, uint32_t frame, PrLayoutVec* point) {
        if (scroll == kPaintScrollNone || !frames.contains(frame, scroll))
            return true;
        if (!enterChain(scene->scrolls.data[scroll].parent, frame, point) || !frames.clipContains(scroll, *point))
            return false;
        *point += offsets[scroll];
        return true;
    }

    // Notes that the point is in the scene's container `scroll`, or in none.
    void foundIn(uint32_t scroll) {
        found = scroll != kPaintScrollNone ? scroll : 0;
        slot = kPaintScrollNone;
    }

    // Visits the layer at `index` with `point` in its parent's frame.
    void layer(uint32_t index, PrLayoutVec point) {
        const LayoutSceneLayer& l = scene->layers.data[index];
        LayerEffects e = layerEffects(scene, l, effects, count);
        bool transformed = e.transform != nullptr;
        LayerPlane plane{};
        if (transformed) {
            plane = planeOf(e, offsets);
            if (plane.hidden)
                return;
        }
        const LayoutResolvedEffects* fx = l.effects != kPaintScrollNone ? &scene->effects.data[l.effects] : nullptr;
        if (fx != nullptr && fx->hidden)
            return;
        bool framed = layerFramed(l, fx, transformed);
        PrLayoutVec at{};
        uint32_t outer = frames.place(index, framed, &at);
        uint32_t frame = frames.frame.data[index];
        if (transformed) {
            PrDMatrix3 inverse{};
            if (!enterChain(l.placement, outer, &point) || !prDMatrix3Invert(&plane.homography, &inverse) ||
                    !layoutVecProjected(inverse, point - at, &point))
                return;
        } else if (framed) {
            if (!enterChain(l.placement, outer, &point))
                return;
            point = point - at;
        }
        PrLayoutVec own = point;
        if ((framed || enterChain(l.placement, frame, &own)) &&
                layoutRectContains(layoutRectAt(frames.origin.data[index], l.extent), own))
            foundIn(l.placement);
        for (uint32_t r = l.regionStart; r < l.regionStart + l.regionCount && !blocks; ++r) {
            const LayoutSceneRegion& region = scene->regions.data[r];
            PrLayoutVec in = point;
            blocks = enterChain(region.scroll, frame, &in) &&
                layoutRectContains(layoutRectMoved(region.rect, frames.origin.data[index]), in);
        }
        // The layer's scroll containers, its slots and its children, in the order they play. A sticky box clips
        // nothing, so the point is in it wherever it is, and the boxes in it are found by what holds them.
        uint32_t next = 0;
        auto scrollsTo = [&](uint32_t segment) {
            for (; next < l.scrollCount && scene->scrolls.data[l.scrollStart + next].segment < segment; ++next) {
                if (scene->scrolls.data[l.scrollStart + next].kind == LayoutContainerKind_Sticky)
                    continue;
                PrLayoutVec in = point;
                if (enterChain(l.scrollStart + next, frame, &in))
                    foundIn(l.scrollStart + next);
            }
        };
        uint32_t s = l.slotStart;
        uint32_t slotEnd = l.slotStart + l.slotCount;
        auto slotsTo = [&](const LayoutSceneLayer* PR_NULLABLE child) {
            for (; s < slotEnd && (child == nullptr || slotFirst(scene->slots.data[s], *child)); ++s) {
                const LayoutSceneSlot& candidate = scene->slots.data[s];
                if (candidate.caret)
                    continue;
                scrollsTo(candidate.segmentsBefore);
                PrLayoutVec in = point;
                PrLayoutRect box = layoutRectMoved(candidate.box, frames.origin.data[index]);
                if (!enterChain(candidate.scroll, frame, &in) || !layoutRectContains(box, in))
                    continue;
                foundIn(candidate.scroll);
                slot = s;
                slotPoint = in - PrLayoutVec{box.left, box.top};
            }
        };
        for (uint32_t child = index + 1; child < l.end; child = scene->layers.data[child].end) {
            slotsTo(&scene->layers.data[child]);
            scrollsTo(scene->layers.data[child].segmentsBefore);
            layer(child, point);
        }
        slotsTo(nullptr);
        scrollsTo(UINT32_MAX);
    }
};

// Adds to `runs` the root's background layers drawn over `canvas`, from the bottom up, positioned in the root's boxes,
// with its border box at `border` (css-backgrounds-3/#root-background). A layer fixed to the viewport is a run of its
// own, positioned in and painting the viewport of `viewport`, in the viewport's space. Adds nothing for layers that
// draw nothing. A background whose layers blend is one run: an isolated group of the root's background color and its
// layers (compositing-1/#background-blend-mode), in which a fixed layer is placed as if it scrolled.
void canvasLayers(LayoutTree* tree, const LayoutFragment* root, PrLayoutRect border, PrLayoutRect canvas,
        PrLayoutVec viewport, Array<LayoutSceneCanvasRun>& runs) {
    PrLayoutStyleIndex style = layoutTreeNode(tree, root->node).style;
    const PrLayoutDecorationStyle& decoration = layoutDecorationStyle(tree->styles, style);
    const LayoutBackground& layers = layoutBackgroundAt(&tree->styles->backgrounds, decoration.backgroundLayers);
    LayoutBackgroundBoxes boxes = layoutBackgroundBoxes(tree->styles, root, layoutBoxStyle(tree->styles, style),
        prLayoutRectToPx(border));
    boxes.area = prLayoutRectToPx(canvas);
    if (layers.blends) {
        PrDrawList* list = prDrawListCreate();
        PrDrawGroup group = prDrawGroupDefault();
        group.isolated = true;
        group.bounds = boxes.area;
        prDrawListPushGroup(list, &group);
        prDrawListRect(list, boxes.area, {.color = decoration.background, .edges = PrDrawEdges_Snapped});
        for (uint32_t i = layers.count; i-- > 0;)
            layoutBackgroundPaintLayer(&tree->styles->compounds, layers.layers[i], boxes, list);
        prDrawListPopGroup(list);
        prDrawListFreeze(list);
        runs.push(LayoutSceneCanvasRun{list, false});
        return;
    }
    PrRect port{0, 0, prLayoutUnitToPx(viewport.x), prLayoutUnitToPx(viewport.y)};
    LayoutBackgroundBoxes fixedBoxes{.boxes = {port, port, port}, .area = port};
    // A layer fixed to the viewport needs a run of its own, because the run is played at another offset. A layer that
    // draws nothing gets no run.
    bool open = false;
    for (uint32_t i = layers.count; i-- > 0;) {
        const PrLayoutBackgroundLayer& layer = layers.layers[i];
        bool fixed = layer.attachment == PrLayoutBackgroundAttachment_Fixed;
        if (!open || fixed || runs.last().fixed) {
            runs.push(LayoutSceneCanvasRun{prDrawListCreate(), fixed});
            open = false;
        }
        bool drew =
            layoutBackgroundPaintLayer(&tree->styles->compounds, layer, fixed ? fixedBoxes : boxes, runs.last().list);
        if (!drew && !open) {
            prDrawListRelease(runs.last().list);
            runs.pop();
            continue;
        }
        open = true;
    }
    for (uint32_t i = 0; i < runs.count; ++i)
        prDrawListFreeze(runs.data[i].list);
}

// Draws a root other than the viewport's into a new list: its layers in place, with their own effects, and its scroll
// containers at the tree's offsets. Its own containing block does not scroll.
PrDrawList* drawRoot(LayoutTree* tree, const LayoutLaidRoot& root) {
    PrLayoutScene* scene = allocZeroed<PrLayoutScene>();
    scene->references.storeRelaxed(1);
    scene->caretHidden = tree->carets.hidden;
    const LayoutFragment* viewport = root.fragment;
    const LayoutChild& top = viewport->children[0];
    // What the root draws outside its containing block is the painter's to clip.
    PrLayoutUnit far = kLayoutClipUnbounded;
    PrLayoutRect unbounded{PrLayoutUnit{-far.raw}, PrLayoutUnit{-far.raw}, far, far};
    scene->scrolls.push(LayoutSceneContainer{
        .node = kPrLayoutNodeNone,
        .parent = kPaintScrollNone,
        .layer = kPaintScrollNone,
        .clip = unbounded,
        .portExtent = viewport->extent,
        .content = viewport->overflow,
        .kind = LayoutContainerKind_Clip,
        .sticky = kPaintScrollNone,
    });
    addLayer(tree, scene, top.fragment->node, top.fragment, LayoutPieceAt{kLayoutPieceNone, kLayoutPieceNone},
        top.offset, false, kPaintScrollNone, 0, 0, 0);
    Array<PrLayoutSceneScroll> scrolls;
    scrolls.resize(prLayoutSceneScrolls(static_cast<const PrLayoutTree*>(tree), scene, nullptr, 0));
    prLayoutSceneScrolls(static_cast<const PrLayoutTree*>(tree), scene, scrolls.data, scrolls.count);
    PrLayoutScenePlayOptions options = prLayoutScenePlayOptionsDefault();
    options.scrolls = scrolls.data;
    options.scrollCount = scrolls.count;
    PrDrawList* list = prDrawListCreate();
    layoutScenePlay(scene, list, options);
    prDrawListFreeze(list);
    scrolls.reset();
    prLayoutSceneRelease(scene);
    return list;
}

// Draws the roots other than the viewport's that need drawing again, last first, so that a root's drawing is ready
// before the painters that may draw it. A root needs drawing again if it has no drawing, if this paint recorded any of
// its layers again, or if a layout or a layer's effects changed since the last paint. The replaced nodes whose painters
// drew a root that is drawn again draw it again in this paint.
void drawRoots(LayoutTree* tree) {
    bool stale = (tree->paintedFor & (LayoutInputFlag_Layout | LayoutInputFlag_Effects)) != 0;
    for (uint32_t i = tree->roots.count; i-- > 0;) {
        LayoutLaidRoot& root = tree->roots.data[i];
        if (root.fragment == nullptr || root.fragment->childCount == 0)
            continue;
        uint32_t recorded = tree->stats.layersRecorded;
        PrDrawList* list = drawRoot(tree, root);
        if (!stale && root.drawing != nullptr && tree->stats.layersRecorded == recorded) {
            prDrawListRelease(list);
            continue;
        }
        prDrawListRelease(root.drawing);
        root.drawing = list;
        for (uint32_t k = 0; k < tree->rootReaders.count; ++k) {
            if (tree->rootReaders.data[k].root == root.node)
                layoutTreeRepaintContent(tree, tree->rootReaders.data[k].replaced, tree->paint.paints - 1);
        }
    }
}

// Returns `top` over `bottom`, source-over on encoded values as a target blends, both straight alpha.
PrColor colorOver(PrColor top, PrColor bottom) {
    float alpha = top.a + bottom.a * (1 - top.a);
    if (alpha <= 0)
        return PrColor{0, 0, 0, 0};
    auto over = [&](float a, float b) { return (a * top.a + b * bottom.a * (1 - top.a)) / alpha; };
    return PrColor{over(top.r, bottom.r), over(top.g, bottom.g), over(top.b, bottom.b), alpha};
}

} // namespace

bool layoutSceneIsCurrent(const LayoutTree* tree) {
    // Nothing a scene reads changed since the last one: its layout, its recordings and its effects.
    return tree->scene != nullptr && tree->pendingPaint == 0 && tree->sceneFragment == tree->fragment;
}

const PrLayoutScene* layoutScene(LayoutTree* tree) {
    if (layoutSceneIsCurrent(tree)) {
        tree->paintedFor = 0;
        layoutPaintReused(tree, tree->scene->layers.count);
        prLayoutSceneRetain(tree->scene);
        return tree->scene;
    }
    tree->paintedFor = tree->pendingPaint;
    tree->pendingPaint = 0;
    layoutPaintBegin(tree);
    drawRoots(tree);
    PrLayoutScene* scene = allocZeroed<PrLayoutScene>();
    scene->references.storeRelaxed(1);
    // The scene's boxes are where anchoring's moves since the last scene place them, so the moves go with it.
    Array<PrLayoutScrollAdjustment>& unscened = tree->anchoring.unscened;
    scene->adjustments = unscened;
    unscened = {};
    scene->pixelRatio = tree->styles->targetPixelRatio;
    scene->caretHidden = tree->carets.hidden;
    scene->canvasColor = prColorFromColor16(tree->canvasColor);
    scene->viewportWheelEvents = tree->viewportWheelEvents;
    const LayoutFragment* viewport = tree->fragment;
    if (viewport != nullptr && viewport->childCount != 0) {
        const LayoutChild& root = viewport->children[0];
        PrLayoutRect canvas = viewport->overflow;
        scene->canvas = layoutRectUnion(canvas, layoutRectAt(root.offset, root.fragment->extent));
        uint32_t rootStyle = layoutTreeNode(tree, root.fragment->node).style;
        if (layoutColorStyle(tree->styles, rootStyle).visibility == PrLayoutVisibility_Visible) {
            scene->canvasDecoration = layoutDecorationStyle(tree->styles, rootStyle);
            // A background that blends draws its color in its group (canvasLayers).
            PrColor background = prColorFromColor16(scene->canvasDecoration.background);
            if (!layoutBackgroundAt(&tree->styles->backgrounds, scene->canvasDecoration.backgroundLayers).blends)
                scene->canvasColor = colorOver(background, scene->canvasColor);
            scene->canvasLayersHash =
                layoutBackgroundAt(&tree->styles->backgrounds, scene->canvasDecoration.backgroundLayers).hash;
            scene->canvasBorder = layoutRectAt(root.offset, root.fragment->extent);
            scene->canvasViewport = viewport->extent;
            const LayoutScene* previous = tree->scene;
            if (previous != nullptr && previous->canvasBorder == scene->canvasBorder &&
                    previous->canvas == scene->canvas && previous->canvasLayersHash == scene->canvasLayersHash &&
                    previous->canvasViewport == scene->canvasViewport &&
                    prMemEquals(&previous->canvasDecoration, &scene->canvasDecoration, sizeof(PrLayoutDecorationStyle))) {
                for (uint32_t i = 0; i < previous->canvasRuns.count; ++i) {
                    prDrawListRetain(previous->canvasRuns.data[i].list);
                    scene->canvasRuns.push(previous->canvasRuns.data[i]);
                }
            } else {
                canvasLayers(tree, root.fragment, scene->canvasBorder, scene->canvas, scene->canvasViewport,
                    scene->canvasRuns);
            }
        }
        // The viewport scrolls the document as the root's overflow says. An axis in which the root's overflow is
        // visible scrolls.
        PrLayoutBoxStyle overflow = layoutBoxStyle(tree->styles, rootStyle);
        if (overflow.overflowX == PrLayoutOverflow_Visible)
            overflow.overflowX = PrLayoutOverflow_Scroll;
        if (overflow.overflowY == PrLayoutOverflow_Visible)
            overflow.overflowY = PrLayoutOverflow_Scroll;
        PrLayoutRect range = layoutScrollRange(viewport->extent, viewport->overflow);
        scene->scrolls.push(LayoutSceneContainer{
            .node = kPrLayoutNodeNone,
            .parent = kPaintScrollNone,
            .layer = kPaintScrollNone,
            .clip = layoutRectAt(kLayoutVecZero, viewport->extent),
            .range = range,
            .portExtent = viewport->extent,
            .content = viewport->overflow,
            .userAxes = layoutUserAxes(overflow),
            .chainAxes = layoutChainAxes(overflow),
            .kind = LayoutContainerKind_Scroll,
            .sticky = kPaintScrollNone,
        });
        addLayer(tree, scene, root.fragment->node, root.fragment, LayoutPieceAt{kLayoutPieceNone, kLayoutPieceNone},
            root.offset, true, kPaintScrollNone, 0, 0, 0);
    }
    layoutPaintSweep(tree);
    prLayoutSceneRelease(tree->scene);
    prLayoutSceneRetain(scene);
    tree->scene = scene;
    tree->sceneFragment = tree->fragment;
    return scene;
}

void layoutScenePlay(const LayoutScene* scene, PrDrawList* list, const PrLayoutScenePlayOptions& options) {
    if (scene->layers.count == 0)
        return;
    // A container scrolls by whole target pixels, so that a scroll moves what it shows by whole pixels.
    Array<PrLayoutVec> offsets;
    layoutSceneScrollOffsets(scene, options.scrolls, options.scrollCount, scene->pixelRatio, offsets);
    Player player{
        .scene = scene,
        .list = list,
        .options = options,
        .offsets = offsets.data,
        .frames = {.scene = scene},
    };
    if (scene->canvasColor.a > 0 && !options.canvasFilled) {
        player.enter(0, kPaintScrollNone);
        prDrawListRect(list, prLayoutRectToPx(scene->canvas),
            {.color = scene->canvasColor, .edges = PrDrawEdges_Snapped});
    }
    player.canvas();
    player.layer(0);
    player.closeTo(0);
    player.reset();
    offsets.reset();
}

void layoutSceneDestroy(LayoutScene* scene) {
    for (uint32_t i = 0; i < scene->canvasRuns.count; ++i)
        prDrawListRelease(scene->canvasRuns.data[i].list);
    scene->canvasRuns.reset();
    scene->fixedClips.reset();
    for (uint32_t i = 0; i < scene->segments.count; ++i)
        prDrawListRelease(scene->segments.data[i].list);
    for (uint32_t i = 0; i < scene->effects.count; ++i)
        layoutEffectsReset(&scene->effects.data[i]);
    scene->effects.reset();
    scene->layers.reset();
    scene->transforms.reset();
    scene->scrolls.reset();
    scene->stickies.reset();
    scene->segments.reset();
    scene->slots.reset();
    scene->regions.reset();
    scene->adjustments.reset();
    prMemoryFree(scene);
}

uint32_t layoutSceneSampleTracks(const LayoutScene* scene, const PrLayoutTrack* PR_NULLABLE tracks, uint32_t count,
        double time, Array<PrLayoutSceneEffect>& out) {
    uint32_t start = out.count;
    uint32_t sampled = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const PrLayoutTrack& track = tracks[i];
        PrVec2 reference{0, 0};
        if (track.effect == PrLayoutEffect_Transform) {
            for (uint32_t k = 0; k < scene->layers.count; ++k) {
                const LayoutSceneLayer& layer = scene->layers.data[k];
                if (layer.node == track.node && layer.transform != kPaintScrollNone) {
                    reference = scene->transforms.data[layer.transform].resolved.reference;
                    break;
                }
            }
        }
        float opacity = 1;
        PrMatrix4 matrix = prMatrix4Identity();
        if (!prLayoutTrackSample(&track, time, reference, &opacity, &matrix))
            continue;
        uint32_t at = start;
        while (at < out.count && out.data[at].node != track.node)
            ++at;
        if (at == out.count) {
            PrLayoutSceneEffect& made = out.emplace();
            made = PrLayoutSceneEffect{};
            made.node = track.node;
            made.opacity = 1;
            made.transform = prMatrix4Identity();
        }
        PrLayoutSceneEffect& effect = out.data[at];
        if (track.effect == PrLayoutEffect_Opacity) {
            effect.flags |= PrLayoutSceneEffectFlag_Opacity;
            effect.opacity = opacity;
        } else {
            effect.flags |= PrLayoutSceneEffectFlag_Transform;
            effect.transform = matrix;
        }
        ++sampled;
    }
    return sampled;
}

uint32_t layoutSceneNodeScroll(const LayoutScene* scene, PrLayoutNode node) {
    for (uint32_t i = 0; i < scene->scrolls.count; ++i) {
        if (scene->scrolls.data[i].node == node && scene->scrolls.data[i].kind == LayoutContainerKind_Scroll)
            return i;
    }
    return kPaintScrollNone;
}

void layoutSceneScrollOffsets(const LayoutScene* scene, const PrLayoutSceneScroll* PR_NULLABLE scrolls, uint32_t count,
        float pixelRatio, Array<PrLayoutVec>& out) {
    out.resize(scene->scrolls.count);
    double ratio = double(pixelRatio);
    // Offsets given in the scene's order are each found at the next position. Offsets in any other order are searched
    // for.
    uint32_t next = 0;
    for (uint32_t i = 0; i < scene->scrolls.count; ++i) {
        const LayoutSceneContainer& s = scene->scrolls.data[i];
        out.data[i] = kLayoutVecZero;
        if (s.kind != LayoutContainerKind_Scroll)
            continue;
        uint32_t at = next < count && scrolls[next].node == s.node ? next : kArrayMissing;
        for (uint32_t k = 0; k < count && at == kArrayMissing; ++k)
            at = scrolls[k].node == s.node ? k : kArrayMissing;
        if (at == kArrayMissing)
            continue;
        PrLayoutVec offset = layoutScrollClamp(scrolls[at].offset, s.range);
        if (ratio != 0) {
            offset.x = prLayoutUnitFromPxRoundDouble(prF64Floor(prLayoutUnitToPxDouble(offset.x) * ratio + 0.5) / ratio);
            offset.y = prLayoutUnitFromPxRoundDouble(prF64Floor(prLayoutUnitToPxDouble(offset.y) * ratio + 0.5) / ratio);
        }
        out.data[i] = offset;
        next = at + 1;
    }
    // A container comes after the containers around it, so the offsets of a sticky box's scroll container and of the
    // sticky boxes between them are known by the time it is reached.
    for (uint32_t i = 0; i < scene->scrolls.count; ++i) {
        const LayoutSceneContainer& s = scene->scrolls.data[i];
        if (s.sticky == kPaintScrollNone)
            continue;
        const LayoutSticky& sticky = scene->stickies.data[s.sticky];
        PrLayoutVec around = kLayoutVecZero;
        PrLayoutVec limitAround = kLayoutVecZero;
        uint32_t skipped = 0;
        uint32_t at = s.parent;
        for (; at != kPaintScrollNone && scene->scrolls.data[at].kind != LayoutContainerKind_Scroll;
                at = scene->scrolls.data[at].parent) {
            if (scene->scrolls.data[at].kind != LayoutContainerKind_Sticky)
                continue;
            around = around - out.data[at];
            if (skipped++ >= sticky.limitSkips)
                limitAround = limitAround - out.data[at];
        }
        PrLayoutVec scroll = at != kPaintScrollNone ? out.data[at] : kLayoutVecZero;
        out.data[i] = kLayoutVecZero - layoutStickyShift(sticky, scroll, around, limitAround);
    }
}

LayoutScrollHit layoutSceneScrollAt(const LayoutScene* scene, PrLayoutVec point,
        const PrLayoutSceneEffect* PR_NULLABLE effects, uint32_t count, const PrLayoutVec* offsets) {
    if (scene->layers.count == 0 || !layoutRectContains(scene->scrolls.data[0].clip, point))
        return LayoutScrollHit{kPaintScrollNone, kPaintScrollNone, kLayoutVecZero, false};
    ScrollFinder finder{
        .scene = scene,
        .effects = effects,
        .count = count,
        .offsets = offsets,
        .found = 0,
        .slot = kPaintScrollNone,
        .slotPoint = kLayoutVecZero,
        .blocks = scene->viewportWheelEvents == PrLayoutWheelEvents_Blocking,
        .frames = Frames{scene, {}, {}},
    };
    finder.layer(0, point);
    finder.frames.reset();
    return LayoutScrollHit{scrollerOf(scene, finder.found), finder.slot, finder.slotPoint, finder.blocks};
}

uint32_t layoutSceneScrollNext(const LayoutScene* scene, uint32_t scroll) {
    return scrollerOf(scene, containerAround(scene, scroll));
}

} // namespace pr_impl

using namespace pr_impl;

const PrLayoutScene* prLayoutTreeScene(PrLayoutTree* tree) {
    return layoutScene(tree);
}

bool prLayoutTreeSceneIsCurrent(const PrLayoutTree* tree) {
    return layoutSceneIsCurrent(tree);
}

void prLayoutSceneRetain(const PrLayoutScene* scene) {
    scene->references.fetchAddRelaxed(1);
}

void prLayoutSceneRelease(const PrLayoutScene* PR_NULLABLE scene) {
    if (scene != nullptr && scene->references.fetchSub(1) == 1)
        layoutSceneDestroy(const_cast<PrLayoutScene*>(scene));
}

PrLayoutScenePlayOptions prLayoutScenePlayOptionsDefault(void) {
    PrLayoutScenePlayOptions options{};
    options.effects = nullptr;
    options.effectCount = 0;
    options.scrolls = nullptr;
    options.scrollCount = 0;
    options.embed = nullptr;
    options.embedContext = nullptr;
    return options;
}

uint32_t prLayoutSceneSampleTracks(const PrLayoutScene* scene, const PrLayoutTrack* PR_NULLABLE tracks,
        uint32_t trackCount, double time, PrLayoutSceneEffect* PR_NULLABLE out, uint32_t capacity) {
    Array<PrLayoutSceneEffect> effects;
    layoutSceneSampleTracks(scene, tracks, trackCount, time, effects);
    for (uint32_t i = 0; i < effects.count && i < capacity; ++i)
        out[i] = effects.data[i];
    uint32_t total = effects.count;
    effects.reset();
    return total;
}

void prLayoutScenePlay(const PrLayoutScene* scene, PrDrawList* list, const PrLayoutScenePlayOptions* PR_NULLABLE options) {
    PrLayoutScenePlayOptions played = options != nullptr ? *options : prLayoutScenePlayOptionsDefault();
    layoutScenePlay(scene, list, played);
}

PrColor16 prLayoutSceneCanvasColor(const PrLayoutScene* scene) {
    return prColor16FromColor(scene->canvasColor);
}

uint32_t prLayoutSceneEmbeds(const PrLayoutScene* scene, PrLayoutSceneEmbed* PR_NULLABLE out, uint32_t capacity) {
    uint32_t count = 0;
    for (uint32_t i = 0; i < scene->slots.count; ++i) {
        const LayoutSceneSlot& slot = scene->slots.data[i];
        if (slot.caret)
            continue;
        if (count < capacity) {
            out[count] = PrLayoutSceneEmbed{
                slot.embed, slot.node,
                PrLayoutVec{layoutRectWidth(slot.box), layoutRectHeight(slot.box)},
            };
        }
        ++count;
    }
    return count;
}

uint32_t prLayoutSceneLayerCount(const PrLayoutScene* scene) {
    return scene->layers.count;
}

bool prLayoutSceneHasLayer(const PrLayoutScene* scene, PrLayoutNode node) {
    for (uint32_t i = 0; i < scene->layers.count; ++i) {
        if (scene->layers.data[i].node == node)
            return true;
    }
    return false;
}

bool prLayoutSceneHasScroll(const PrLayoutScene* scene, PrLayoutNode node) {
    return layoutSceneNodeScroll(scene, node) != kPaintScrollNone;
}

uint32_t prLayoutSceneScrollAdjustments(const PrLayoutScene* scene, PrLayoutScrollAdjustment* PR_NULLABLE out,
        uint32_t capacity) {
    for (uint32_t i = 0; i < scene->adjustments.count && i < capacity; ++i)
        out[i] = scene->adjustments.data[i];
    return scene->adjustments.count;
}

uint32_t prLayoutSceneScrolls(const PrLayoutTree* tree, const PrLayoutScene* scene,
        PrLayoutSceneScroll* PR_NULLABLE out, uint32_t capacity) {
    uint32_t total = 0;
    for (uint32_t i = 0; i < scene->scrolls.count; ++i) {
        const LayoutSceneContainer& s = scene->scrolls.data[i];
        if (s.kind != LayoutContainerKind_Scroll)
            continue;
        if (total < capacity) {
            PrLayoutVec offset = kLayoutVecZero;
            if (s.node == kPrLayoutNodeNone)
                offset = tree->viewportScroll;
            else if (prLayoutNodeIsValid(tree, s.node))
                offset = layoutNodeScroll(tree, layoutNodeIndex(tree, s.node));
            out[total] = PrLayoutSceneScroll{s.node, layoutScrollClamp(offset, s.range)};
        }
        ++total;
    }
    return total;
}

void prLayoutTreePaint(PrLayoutTree* tree, PrDrawList* list) {
    const PrLayoutScene* scene = layoutScene(tree);
    Array<PrLayoutSceneScroll> scrolls;
    scrolls.resize(prLayoutSceneScrolls(tree, scene, nullptr, 0));
    prLayoutSceneScrolls(tree, scene, scrolls.data, scrolls.count);
    PrLayoutScenePlayOptions options = prLayoutScenePlayOptionsDefault();
    options.scrolls = scrolls.data;
    options.scrollCount = scrolls.count;
    layoutScenePlay(scene, list, options);
    scrolls.reset();
    prLayoutSceneRelease(scene);
}

void prLayoutNodeDraw(PrLayoutTree* tree, PrLayoutNode root, PrDrawList* list) {
    LayoutNodeIndex index = layoutNodeIndex(tree, root);
    for (uint32_t i = 0; i < tree->roots.count; ++i) {
        const LayoutLaidRoot& laid = tree->roots.data[i];
        if (laid.node != index)
            continue;
        if (tree->painting != kPrLayoutNodeNone)
            tree->rootReaders.push(LayoutRootReader{tree->painting, index});
        if (laid.drawing != nullptr)
            prDrawListEmbed(list, laid.drawing, PrDVec2{0, 0});
        return;
    }
}
