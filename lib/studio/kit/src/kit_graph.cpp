// Studio Kit's node graph: boxes with typed ports, joined by wires, on a canvas that pans and zooms, in a model that
// the caller owns.

#include "pr/kit_graph.h"

#include "pr/array.h"
#include "pr/assert.h"
#include "pr/draw_list.h"
#include "pr/float.h"
#include "pr/hash.h"
#include "pr/kit_text.h"
#include "pr/path.h"

#include "studio/kit/src/kit.h"

namespace pr_impl {

namespace {

// The default width of a node, the heights of its header and of a row of ports, the side of a port's square, and the
// distance from a port within which a wire drops on it, in px of the graph.
constexpr float kNodeWidth = 180;
constexpr float kHeaderHeight = 26;
constexpr float kPortRow = 20;
constexpr float kPortSize = 8;
constexpr float kPortReach = 10;

// The distance from a wire within which a click picks it, the width of a wire, and the spacing of the canvas's grid, in
// px of the canvas, except the grid's, which is in px of the graph.
constexpr float kWireReach = 5;
constexpr float kWireWidth = 2;
constexpr float kGridSpacing = 16;

// The least distance between the dots of the grid on the canvas, in px.
constexpr float kGridLeast = 8;

// The segments that a wire's curve is measured in, to find the wire under the pointer.
constexpr uint32_t kWireSegments = 24;

// The factor by which each px of wheel zooms the view, and the least and most zoom.
constexpr float kZoomPerPx = 0.01f;
constexpr float kMinZoom = 0.25f;
constexpr float kMaxZoom = 3;

// The gaps between the columns and between the nodes in a column that prKitGraphArrange leaves, in px of the graph.
constexpr float kArrangeColumnGap = 72;
constexpr float kArrangeRowGap = 24;

// The canvas: a box that clips what is drawn and placed on it, in the viewport's ground.
struct CanvasBox {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Box;
    void make(const PrKit&, PrLayoutBoxStyle& box) const {
        box.display = PrLayoutDisplay_Block;
        box.position = PrLayoutPosition_Relative;
        box.overflowX = box.overflowY = PrLayoutOverflow_Clip;
    }
};

// A layer at the canvas's origin, as large as the canvas if `fills`: the drawing of the grid and the wires under the
// nodes, and the world that the view's transform moves the nodes in.
PR_NO_PADDING_BEGIN
struct LayerBox {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Box;
    bool fills;

    void make(const PrKit&, PrLayoutBoxStyle& box) const {
        box.display = PrLayoutDisplay_Block;
        box.position = PrLayoutPosition_Absolute;
        box.inset[PrRectSide_Left] = prLayoutLengthPx(0);
        box.inset[PrRectSide_Top] = prLayoutLengthPx(0);
        if (fills) {
            box.width = prLayoutLengthPercent(100);
            box.height = prLayoutLengthPercent(100);
        }
    }
};
PR_NO_PADDING_END

// The world's transform: the view's zoom and offset, which move as the view pans and zooms.
PR_NO_PADDING_BEGIN
struct WorldEffects {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Effect;
    float zoom;
    PrVec2 offset;

    void make(const PrKit&, PrLayoutEffectStyle& effects) const {
        effects.originX = prLayoutLengthPx(0);
        effects.originY = prLayoutLengthPx(0);
        effects.transform = prLayoutTransformFromAffine2(
            prAffine2Concat(prAffine2Scale(PrVec2{zoom, zoom}), prAffine2Translate(offset)));
    }
};
PR_NO_PADDING_END

// A box placed at `rect` over its positioned parent, bordered: a node, which lays out its parts in a column inside the
// rect, or the marquee, whose border lies around the rect. The rect moves as a node is dragged and as the marquee
// grows.
PR_NO_PADDING_BEGIN
struct RectBox {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Box;
    PrRect rect;
    bool column;
    uint8_t _pad[3];

    void make(const PrKit&, PrLayoutBoxStyle& box) const {
        box.display = column ? PrLayoutDisplay_Flex : PrLayoutDisplay_Block;
        box.position = PrLayoutPosition_Absolute;
        if (column)
            box.boxSizing = PrLayoutBoxSizing_BorderBox;
        box.inset[PrRectSide_Left] = prLayoutLengthPx(rect.left);
        box.inset[PrRectSide_Top] = prLayoutLengthPx(rect.top);
        box.width = prLayoutLengthPx(prRectWidth(rect));
        box.height = prLayoutLengthPx(prRectHeight(rect));
        kitSetBorderWidth(box, 1);
    }
};
PR_NO_PADDING_END
struct NodeFlex {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Flex;
    void make(const PrKit&, PrLayoutFlexStyle& flex) const {
        flex.direction = PrLayoutFlexDirection_Column;
    }
};

// A node's header: a row of its title and meta, on a raised ground.
struct HeaderBox {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Box;
    void make(const PrKit&, PrLayoutBoxStyle& box) const {
        box.display = PrLayoutDisplay_Flex;
        box.boxSizing = PrLayoutBoxSizing_BorderBox;
        box.height = prLayoutLengthPx(kHeaderHeight);
        box.overflowX = box.overflowY = PrLayoutOverflow_Clip;
        kitSetPadding(box, prKitEdgesXY(0, kPrKitSpace4));
    }
};
struct HeaderFlex {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Flex;
    void make(const PrKit&, PrLayoutFlexStyle& flex) const {
        flex.alignItems = PrLayoutAlignment_Center;
        flex.columnGap = prLayoutLengthPx(kPrKitSpace3);
    }
};

// A row of a node's ports: an input's label at its start and an output's at its end, and the port squares on its edges.
struct PortRowBox {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Box;
    void make(const PrKit&, PrLayoutBoxStyle& box) const {
        box.display = PrLayoutDisplay_Flex;
        box.position = PrLayoutPosition_Relative;
        box.boxSizing = PrLayoutBoxSizing_BorderBox;
        box.height = prLayoutLengthPx(kPortRow);
        kitSetPadding(box, prKitEdgesXY(0, kPrKitSpace5));
    }
};
struct PortRowFlex {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Flex;
    void make(const PrKit&, PrLayoutFlexStyle& flex) const {
        flex.alignItems = PrLayoutAlignment_Center;
        flex.justifyContent = PrLayoutAlignment_SpaceBetween;
    }
};

// A node's body: the room under its ports, a column that clips what the caller puts in it.
struct BodyBox {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Box;
    void make(const PrKit&, PrLayoutBoxStyle& box) const {
        box.display = PrLayoutDisplay_Flex;
        box.boxSizing = PrLayoutBoxSizing_BorderBox;
        box.minHeight = prLayoutLengthPx(0);
        box.overflowX = box.overflowY = PrLayoutOverflow_Clip;
        kitSetPadding(box, prKitEdgesXY(kPrKitSpace2, kPrKitSpace4));
    }
};
struct BodyFlex {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Flex;
    void make(const PrKit&, PrLayoutFlexStyle& flex) const {
        flex.direction = PrLayoutFlexDirection_Column;
        flex.grow = 1;
        flex.basis = prLayoutLengthPx(0);
    }
};

// A port's square, on the edge of its row at the input's side or the output's.
PR_NO_PADDING_BEGIN
struct PortBox {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Box;
    bool input;

    void make(const PrKit&, PrLayoutBoxStyle& box) const {
        box.display = PrLayoutDisplay_Block;
        box.position = PrLayoutPosition_Absolute;
        box.inset[PrRectSide_Top] = prLayoutLengthPx((kPortRow - kPortSize) / 2);
        box.inset[input ? PrRectSide_Left : PrRectSide_Right] = prLayoutLengthPx(-kPortSize / 2);
        box.width = prLayoutLengthPx(kPortSize);
        box.height = prLayoutLengthPx(kPortSize);
        kitSetBorderWidth(box, 1);
    }
};
PR_NO_PADDING_END

// What a drag on the graph does.
enum {
    DragMode_None,
    DragMode_Move,                  // a node moves, with the other selected nodes
    DragMode_Wire,                  // a wire runs from a port to the pointer
    DragMode_Marquee,               // a rectangle over the canvas selects the nodes it touches
    DragMode_Pick,                  // a press on a wire selected it, and the drag does nothing more
};
typedef uint8_t DragMode;

// What a graph keeps between rounds: the drag in progress, and the key of the drawing of its canvas.
struct GraphState {
    uint64_t drawn;                 // the hash of what the canvas's drawing shows
    uint64_t node;                  // the node that the drag holds, or that the wire runs from
    uint64_t port;                  // the port that the wire runs from
    PrVec2 press;                   // where the drag started, in the graph's px for a move and the canvas's otherwise
    PrVec2 reach;                   // where the pointer is, in the canvas's px
    PrVec2 applied;                 // the shift that the move has applied to the selected nodes
    PrKitGraphSide side;            // the side of the port that the wire runs from
    DragMode mode;
    bool live;                   // a gesture is open
    bool extend;                 // the marquee adds to the selection
};

// Returns the color of a port's type.
PrColor typeColor(const PrKit& kit, const PrKitGraph& g, uint32_t type) {
    return g.typeColors != nullptr && type < g.typeCount ? g.typeColors[type] : kit.theme.palette.accent;
}

// Maps a point between the graph's px and the canvas's.
PrVec2 toCanvas(const PrKitGraphView& view, PrVec2 point) {
    return view.offset + point * view.zoom;
}
PrVec2 toGraph(const PrKitGraphView& view, PrVec2 point) {
    return (point - view.offset) / view.zoom;
}

// Finds the node with `id`, and the index of its port `port` on `side`. Returns false if there is none.
bool findPort(const PrKitGraph& g, uint64_t id, uint64_t port, PrKitGraphSide side, const PrKitGraphNode** node,
        uint32_t* index) {
    for (uint32_t i = 0; i < g.nodeCount; ++i) {
        const PrKitGraphNode& n = g.nodes[i];
        if (n.id != id)
            continue;
        const PrKitGraphPort* ports = side == PrKitGraphSide_Input ? n.inputs : n.outputs;
        uint32_t count = side == PrKitGraphSide_Input ? n.inputCount : n.outputCount;
        for (uint32_t j = 0; j < count; ++j) {
            if (ports[j].id == port) {
                *node = &n;
                *index = j;
                return true;
            }
        }
        return false;
    }
    return false;
}

// The ends of a wire on the canvas, and its color.
struct WireCurve {
    PrVec2 from;
    PrVec2 to;
    PrColor color;
};

// Returns the point at `t` along the curve between a wire's ends, which leaves its output and enters its input
// horizontally.
PrVec2 curvePoint(PrVec2 from, PrVec2 to, float bend, float t) {
    PrVec2 c1{from.x + bend, from.y};
    PrVec2 c2{to.x - bend, to.y};
    float u = 1 - t;
    float a = u * u * u;
    float b = 3 * u * u * t;
    float c = 3 * u * t * t;
    float d = t * t * t;
    return a * from + b * c1 + c * c2 + d * to;
}

// Returns how far a wire's curve bends out of its ends, in px of the canvas.
float bendOf(PrVec2 from, PrVec2 to, float zoom) {
    return prF32Max(prF32Abs(to.x - from.x) / 2, 40 * zoom);
}

// Finds a wire's ends on the canvas. Returns false if it names no port of the model.
bool wireCurve(const PrKit& kit, const PrKitGraph& g, const PrKitGraphWire& wire, WireCurve* out) {
    const PrKitGraphNode* from = nullptr;
    const PrKitGraphNode* to = nullptr;
    uint32_t output = 0;
    uint32_t input = 0;
    if (!findPort(g, wire.link.fromNode, wire.link.fromPort, PrKitGraphSide_Output, &from, &output) ||
            !findPort(g, wire.link.toNode, wire.link.toPort, PrKitGraphSide_Input, &to, &input))
        return false;
    *out = WireCurve{
        .from = toCanvas(*g.view, prKitGraphPortAnchor(from, PrKitGraphSide_Output, output)),
        .to = toCanvas(*g.view, prKitGraphPortAnchor(to, PrKitGraphSide_Input, input)),
        .color = typeColor(kit, g, from->outputs[output].type),
    };
    return true;
}

// Returns the distance from `point` to the curve between `from` and `to`, in px of the canvas.
float curveDistance(PrVec2 from, PrVec2 to, float zoom, PrVec2 point) {
    float bend = bendOf(from, to, zoom);
    float nearest = 1e30f;
    PrVec2 a = from;
    for (uint32_t i = 1; i <= kWireSegments; ++i) {
        PrVec2 b = curvePoint(from, to, bend, float(i) / kWireSegments);
        PrVec2 ab{b.x - a.x, b.y - a.y};
        float length = ab.x * ab.x + ab.y * ab.y;
        float t = length > 0 ? ((point.x - a.x) * ab.x + (point.y - a.y) * ab.y) / length : 0;
        t = t < 0 ? 0 : t > 1 ? 1 : t;
        float dx = a.x + ab.x * t - point.x;
        float dy = a.y + ab.y * t - point.y;
        nearest = prF32Min(nearest, dx * dx + dy * dy);
        a = b;
    }
    return prF32Sqrt(nearest);
}

// Records a wire's curve into `list`.
void drawCurve(PrDrawList* list, PrPath* path, PrVec2 from, PrVec2 to, float zoom, PrColor color, float width) {
    float bend = bendOf(from, to, zoom);
    prPathClear(path);
    prPathMoveTo(path, from);
    prPathCubicTo(path, PrVec2{from.x + bend, from.y}, PrVec2{to.x - bend, to.y}, to);
    PrDrawStroke stroke = prDrawStrokeDefault();
    stroke.shape.width = width;
    prDrawListPath(list, path, PrPathFillRule_NonZero, {.color = color, .stroke = &stroke});
}

// Finds the port nearest `point` on the canvas within reach, other than on the node `except`. Returns false if there is
// none.
bool portAt(const PrKitGraph& g, PrVec2 point, uint64_t except, const PrKitGraphNode** node, PrKitGraphSide* side,
        uint32_t* index) {
    float nearest = kPortReach * g.view->zoom;
    bool found = false;
    for (uint32_t i = 0; i < g.nodeCount; ++i) {
        const PrKitGraphNode& n = g.nodes[i];
        if (n.id == except)
            continue;
        for (PrKitGraphSide s = PrKitGraphSide_Input; s <= PrKitGraphSide_Output; ++s) {
            uint32_t count = s == PrKitGraphSide_Input ? n.inputCount : n.outputCount;
            for (uint32_t j = 0; j < count; ++j) {
                PrVec2 anchor = toCanvas(*g.view, prKitGraphPortAnchor(&n, s, j));
                float distance =
                    prF32Sqrt((anchor.x - point.x) * (anchor.x - point.x) + (anchor.y - point.y) * (anchor.y - point.y));
                if (distance <= nearest) {
                    nearest = distance;
                    *node = &n;
                    *side = s;
                    *index = j;
                    found = true;
                }
            }
        }
    }
    return found;
}

// Returns the hash of what the canvas's drawing shows: the grid, the wires, and the wire that a drag runs.
uint64_t drawingHash(const PrKitGraph& g, const GraphState& state, PrVec2 extent) {
    uint64_t h = prHashBytes(g.view, sizeof *g.view, 0x9e3779b97f4a7c15ull);
    h = prHashBytes(&extent, sizeof extent, h);
    for (uint32_t i = 0; i < g.nodeCount; ++i) {
        const PrKitGraphNode& n = g.nodes[i];
        h = prHashBytes(&n.id, sizeof n.id, h);
        h = prHashBytes(&n.position, sizeof n.position, h);
        h = prHashBytes(&n.width, sizeof n.width, h);
    }
    for (uint32_t i = 0; i < g.wireCount; ++i) {
        const PrKitGraphWire& w = g.wires[i];
        h = prHashBytes(&w.link, sizeof w.link, h);
        h = prHashBytes(&w.selected, sizeof w.selected, h);
    }
    if (state.mode == DragMode_Wire) {
        h = prHashBytes(&state.reach, sizeof state.reach, h);
        h = prHashBytes(&state.node, sizeof state.node, h);
    }
    return h;
}

// Records the canvas's grid, its wires, and the wire that a drag runs.
void drawCanvas(PrKit& kit, const PrKitGraph& g, const GraphState& state, PrVec2 extent, PrDrawList* list) {
    const PrKitPalette& p = kit.theme.palette;
    const PrKitGraphView& view = *g.view;
    float spacing = kGridSpacing * view.zoom;
    while (spacing < kGridLeast)
        spacing *= 2;
    float x0 = view.offset.x - prF32Floor(view.offset.x / spacing) * spacing;
    float y0 = view.offset.y - prF32Floor(view.offset.y / spacing) * spacing;
    for (float y = y0; y < extent.y; y += spacing) {
        for (float x = x0; x < extent.x; x += spacing)
            prDrawListRect(list, PrRect{x - 0.75f, y - 0.75f, x + 0.75f, y + 0.75f}, {.color = p.line2});
    }
    PrPath* path = prPathCreate();
    for (uint32_t i = 0; i < g.wireCount; ++i) {
        WireCurve curve{};
        if (!wireCurve(kit, g, g.wires[i], &curve))
            continue;
        bool selected = g.wires[i].selected;
        drawCurve(list, path, curve.from, curve.to, view.zoom, selected ? p.text1 : curve.color,
            selected ? kWireWidth + 1 : kWireWidth);
    }
    if (state.mode == DragMode_Wire) {
        const PrKitGraphNode* node = nullptr;
        uint32_t index = 0;
        if (findPort(g, state.node, state.port, state.side, &node, &index)) {
            PrVec2 anchor = toCanvas(view, prKitGraphPortAnchor(node, state.side, index));
            const PrKitGraphPort* ports = state.side == PrKitGraphSide_Output ? node->outputs : node->inputs;
            PrColor color = typeColor(kit, g, ports[index].type);
            color.a = 0.7f;
            bool output = state.side == PrKitGraphSide_Output;
            drawCurve(list, path, output ? anchor : state.reach, output ? state.reach : anchor, view.zoom, color,
                kWireWidth);
        }
    }
    prPathDestroy(path);
}

// Creates a node's port square at the edge of its row, in the color of its type.
PrLayoutNode portSquare(PrKit& kit, PrLayoutNode row, PrKitGraphSide side, PrColor color) {
    PrLayoutStyleIndex record = kitStyle(kit, PortBox{.input = side == PrKitGraphSide_Input},
        KitColors{.background = color, .border = kit.theme.palette.bg1});
    PrLayoutNode node = prLayoutTreeFrameElement(kit.tree, row, 0, record);
    prUiSetFlags(kit.ui, node, PrUiFlag_Pressable);
    prUiSetCursor(kit.ui, node, PrAppCursor_Crosshair);
    return node;
}

// Opens the gesture of a drag, if it is not open.
void openGesture(PrKit& kit, PrLayoutNode canvas, GraphState& state) {
    if (state.live)
        return;
    state.live = true;
    prUiEdit(kit.ui, canvas, PrUiEditPhase_Begin);
}

// Selects the nodes and wires that a press with `modifiers` on `node`, or on `wire`, selects.
void pressSelect(const PrKitGraph& g, PrKitGraphNode* PR_NULLABLE node, PrKitGraphWire* PR_NULLABLE wire,
        PrAppModifierFlags modifiers) {
    bool toggle = (modifiers & (PrAppModifierFlag_Super | PrAppModifierFlag_Control)) != 0;
    bool extend = (modifiers & PrAppModifierFlag_Shift) != 0;
    bool* selected = node != nullptr ? &node->selected : &wire->selected;
    if (toggle) {
        *selected = !*selected;
        return;
    }
    if (*selected)
        return;
    for (uint32_t i = 0; i < g.nodeCount; ++i)
        g.nodes[i].selected = extend && g.nodes[i].selected;
    for (uint32_t i = 0; i < g.wireCount; ++i)
        g.wires[i].selected = extend && g.wires[i].selected;
    *selected = true;
}

// The context that a graph's canvas provides for the current round: the connection that a dropped wire asked for, and
// the body of each node, which follow the header in the context's storage.
struct GraphRound {
    PrKitGraphLink link;
    uint32_t bodyCount;
    bool connected;

    PrLayoutNode* bodies() {
        return reinterpret_cast<PrLayoutNode*>(this + 1);
    }
    const PrLayoutNode* bodies() const {
        return reinterpret_cast<const PrLayoutNode*>(this + 1);
    }
};

// Returns the round's context of the graph whose canvas is `graph`, or null if the round created no such graph.
const GraphRound* PR_NULLABLE graphRound(const PrKit& kit, PrLayoutNode graph) {
    PrLayoutNode provider = kPrLayoutNodeNone;
    const void* round = prUiContext(kit.ui, graph, &kKitOwner, KitContext_Graph, &provider);
    return provider == graph ? static_cast<const GraphRound*>(round) : nullptr;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrLayoutNode prKitGraph(PrKit* kit, PrLayoutNode parent, PrKitGraph graph, PrKitGraphStyle style) {
    const PrKitGraph& g = graph;
    PR_ASSERT(g.view != nullptr && g.view->zoom > 0);
    PR_ASSERT(g.nodes != nullptr || g.nodeCount == 0);
    PrKit& k = *kit;
    PrUi* ui = k.ui;
    PrLayoutTree* tree = k.tree;
    const PrKitPalette& p = k.theme.palette;
    bool editable = !prUiIsDisabled(ui, parent);

    PrLayoutNode canvas = prLayoutTreeFrameElement(
        tree, parent, g.key, kitStyle(k, CanvasBox{}, KitFixed{}, KitColors{.background = p.bgViewport}));
    // The round's requests, and each node's body, which the canvas provides to the queries after it.
    uint32_t size = uint32_t(sizeof(GraphRound) + sizeof(PrLayoutNode) * g.nodeCount);
    GraphRound& round = *static_cast<GraphRound*>(prUiProvide(ui, canvas, &kKitOwner, KitContext_Graph, size));
    round.bodyCount = g.nodeCount;
    PrUiFlags flags = PrUiFlag_Wheel;
    if (editable)
        flags = flags | PrUiFlag_Pressable | PrUiFlag_Focusable;
    prUiSetFlags(ui, canvas, flags);
    prKitPlace(kit, canvas, &style.place);
    GraphState* state = static_cast<GraphState*>(prUiState(ui, canvas, &kKitOwner, KitStateKey_Graph,
        sizeof(GraphState)));
    PrKitGraphView& view = *g.view;
    PrVec2 extent = kitExtent(k, canvas);

    // The wheel pans the view, and with Command or Control zooms it about the pointer.
    PrVec2 wheel{};
    if (prUiWheel(ui, canvas, &wheel)) {
        PrUiEvents events = prUiEvents(ui, canvas);
        bool zoom = false;
        for (uint32_t i = 0; i < events.count; ++i) {
            if (events.events[i].kind == PrAppEventKind_Scroll)
                zoom = (events.events[i].modifiers & (PrAppModifierFlag_Super | PrAppModifierFlag_Control)) != 0;
        }
        PrVec2 pointer{};
        PrVec2 at{};
        if (zoom && prUiPointer(ui, &pointer) && prUiLocalPoint(ui, canvas, pointer, &at)) {
            PrVec2 pivot = toGraph(view, at);
            float scale = view.zoom * prF32Exp(-wheel.y * kZoomPerPx);
            view.zoom = scale < kMinZoom ? kMinZoom : scale > kMaxZoom ? kMaxZoom : scale;
            view.offset = at - pivot * view.zoom;
        } else if (!zoom) {
            view.offset = view.offset - wheel;
        }
    }

    // The drawing of the grid and the wires, under the nodes.
    PrLayoutNode drawing = prLayoutTreeFrameReplaced(tree, canvas, 0, kitStyle(k, LayerBox{.fills = true}));
    PrLayoutNode world = prLayoutTreeFrameElement(
        tree, canvas, 0, kitStyle(k, LayerBox{}, WorldEffects{.zoom = view.zoom, .offset = view.offset}));

    // The nodes, and the gestures that start on them and on their ports.
    PrUiDrag drag{};
    bool dragging = false;
    bool ended = false;
    // The selected nodes are drawn over the others, so that a node that a drag moves stays in front of the nodes it
    // passes. The order is fixed before a press in the round changes the selection.
    Array<uint32_t> order;
    for (uint32_t pass = 0; pass < 2; ++pass) {
        for (uint32_t i = 0; i < g.nodeCount; ++i) {
            if (g.nodes[i].selected == (pass == 1))
                order.push(i);
        }
    }
    for (uint32_t slot = 0; slot < order.count; ++slot) {
        uint32_t i = order.data[slot];
        PrKitGraphNode& n = g.nodes[i];
        PrRect rect = prKitGraphNodeRect(&n);
        PrLayoutStyleIndex record = kitStyle(k, RectBox{.rect = rect, .column = true}, NodeFlex{},
            KitColors{.background = p.bg2, .border = n.selected ? p.accent : p.line2});
        PrLayoutNode box = prLayoutTreeFrameElement(tree, world, prHashMixKeys(canvas, n.id), record);
        if (editable) {
            prUiSetFlags(ui, box, PrUiFlag_Pressable);
            prUiSetCursor(ui, box, PrAppCursor_Hand);
        }
        PrLayoutNode header = prLayoutTreeFrameElement(
            tree, box, 0, kitStyle(k, HeaderBox{}, HeaderFlex{}, KitColors{.background = p.bg3}));
        PrKitTextStyle title{.role = PrKitTextRole_Strong, .wrap = PrKitWrap_Clip};
        prKitLabel(kit, header, {.text = n.title}, title);
        if (n.meta.size != 0) {
            PrKitTextStyle meta{.role = PrKitTextRole_Meta};
            prKitLabel(kit, header, {.text = n.meta}, meta);
        }
        uint32_t rows = n.inputCount > n.outputCount ? n.inputCount : n.outputCount;
        PrKitTextStyle portStyle{
            .role = PrKitTextRole_BodySmall,
            .tone = PrKitTone_Secondary,
            .wrap = PrKitWrap_Nowrap,
        };
        for (uint32_t j = 0; j < rows; ++j) {
            PrLayoutNode row = prLayoutTreeFrameElement(tree, box, 0, kitStyle(k, PortRowBox{}, PortRowFlex{}));
            prKitLabel(kit, row, {.text = j < n.inputCount ? n.inputs[j].label : PrStringView{}}, portStyle);
            prKitLabel(kit, row, {.text = j < n.outputCount ? n.outputs[j].label : PrStringView{}}, portStyle);
            for (PrKitGraphSide side = PrKitGraphSide_Input; side <= PrKitGraphSide_Output; ++side) {
                bool input = side == PrKitGraphSide_Input;
                if (j >= (input ? n.inputCount : n.outputCount))
                    continue;
                const PrKitGraphPort& port = input ? n.inputs[j] : n.outputs[j];
                PrLayoutNode square = portSquare(k, row, side, typeColor(k, g, port.type));
                if (!editable || !prUiDrag(ui, square, 0, 0, &drag) || !(drag.active || drag.released))
                    continue;
                PrVec2 at{};
                if (!prUiLocalPoint(ui, canvas, drag.position, &at))
                    continue;
                dragging = true;
                ended = ended || drag.released;
                state->mode = DragMode_Wire;
                state->node = n.id;
                state->port = port.id;
                state->side = side;
                state->reach = at;
                if (!drag.released)
                    continue;
                // The drop joins the port under the pointer, if it is on the other side and of the same type.
                const PrKitGraphNode* other = nullptr;
                PrKitGraphSide otherSide = PrKitGraphSide_Input;
                uint32_t index = 0;
                if (!portAt(g, at, n.id, &other, &otherSide, &index) || otherSide == side)
                    continue;
                bool toInput = otherSide == PrKitGraphSide_Input;
                const PrKitGraphPort& target = toInput ? other->inputs[index] : other->outputs[index];
                if (target.type != port.type)
                    continue;
                round.connected = true;
                if (input)
                    round.link = PrKitGraphLink{other->id, target.id, n.id, port.id};
                else
                    round.link = PrKitGraphLink{n.id, port.id, other->id, target.id};
                kitEditOnce(k, canvas, [] {});
            }
        }
        if (n.bodyHeight > 0)
            round.bodies()[i] = prLayoutTreeFrameElement(tree, box, 0, kitStyle(k, BodyBox{}, BodyFlex{}));
        else
            round.bodies()[i] = kPrLayoutNodeNone;

        if (!editable || !prUiDrag(ui, box, 0, 0, &drag) || !(drag.active || drag.released))
            continue;
        PrVec2 at{};
        if (!prUiLocalPoint(ui, canvas, drag.position, &at))
            continue;
        dragging = true;
        ended = ended || drag.released;
        if (state->mode != DragMode_Move || state->node != n.id) {
            PrVec2 origin{};
            prUiLocalPoint(ui, canvas, drag.origin, &origin);
            pressSelect(g, &n, nullptr, prUiPressModifiers(ui, 0));
            state->mode = DragMode_Move;
            state->node = n.id;
            state->press = toGraph(view, origin);
            state->applied = PrVec2{};
        }
        if (!drag.moved || !n.selected)
            continue;
        openGesture(k, canvas, *state);
        PrVec2 now = toGraph(view, at);
        PrVec2 shift{now.x - state->press.x, now.y - state->press.y};
        PrVec2 by{shift.x - state->applied.x, shift.y - state->applied.y};
        if (by.x == 0 && by.y == 0)
            continue;
        for (uint32_t j = 0; j < g.nodeCount; ++j) {
            if (g.nodes[j].selected)
                g.nodes[j].position = g.nodes[j].position + by;
        }
        state->applied = shift;
        prUiEdit(k.ui, canvas, PrUiEditPhase_Change);
    }

    // A press on the canvas picks the wire under it, or starts a marquee.
    if (editable && prUiDrag(ui, canvas, 0, 0, &drag) && (drag.active || drag.released)) {
        PrVec2 at{};
        if (prUiLocalPoint(ui, canvas, drag.position, &at)) {
            dragging = true;
            ended = ended || drag.released;
            if (state->mode != DragMode_Marquee && state->mode != DragMode_Pick) {
                PrVec2 origin{};
                prUiLocalPoint(ui, canvas, drag.origin, &origin);
                PrAppModifierFlags modifiers = prUiPressModifiers(ui, 0);
                PrKitGraphWire* picked = nullptr;
                float nearest = kWireReach;
                for (uint32_t i = 0; i < g.wireCount; ++i) {
                    WireCurve curve{};
                    if (!wireCurve(k, g, g.wires[i], &curve))
                        continue;
                    float distance = curveDistance(curve.from, curve.to, view.zoom, origin);
                    if (distance <= nearest) {
                        nearest = distance;
                        picked = &g.wires[i];
                    }
                }
                if (picked != nullptr) {
                    pressSelect(g, nullptr, picked, modifiers);
                    state->mode = DragMode_Pick;
                } else {
                    state->mode = DragMode_Marquee;
                    state->press = origin;
                    state->extend = (modifiers & PrAppModifierFlag_Shift) != 0;
                }
            }
            state->reach = at;
            if (state->mode == DragMode_Marquee && drag.moved) {
                openGesture(k, canvas, *state);
                PrVec2 a = toGraph(view, state->press);
                PrVec2 b = toGraph(view, at);
                PrRect band{prF32Min(a.x, b.x), prF32Min(a.y, b.y), prF32Max(a.x, b.x), prF32Max(a.y, b.y)};
                bool changed = false;
                for (uint32_t i = 0; i < g.nodeCount; ++i) {
                    PrRect rect = prKitGraphNodeRect(&g.nodes[i]);
                    bool inside = rect.left < band.right && rect.right > band.left && rect.top < band.bottom &&
                        rect.bottom > band.top;
                    bool selected = inside || (state->extend && g.nodes[i].selected);
                    changed = changed || selected != g.nodes[i].selected;
                    g.nodes[i].selected = selected;
                }
                if (changed)
                    prUiEdit(k.ui, canvas, PrUiEditPhase_Change);
            } else if (state->mode == DragMode_Marquee && drag.released && !state->extend) {
                // A click clears the selection.
                for (uint32_t i = 0; i < g.nodeCount; ++i)
                    g.nodes[i].selected = false;
                for (uint32_t i = 0; i < g.wireCount; ++i)
                    g.wires[i].selected = false;
            }
        }
    }

    if (state->mode == DragMode_Marquee && state->live && !ended) {
        PrRect marquee{
            prF32Min(state->press.x, state->reach.x), prF32Min(state->press.y, state->reach.y),
            prF32Max(state->press.x, state->reach.x), prF32Max(state->press.y, state->reach.y),
        };
        PrColor wash = p.accent;
        wash.a = 0.12f;
        prLayoutTreeFrameElement(
            tree, canvas, 0, kitStyle(k, RectBox{.rect = marquee}, KitColors{.background = wash, .border = p.accent}));
    }

    if (!dragging || ended) {
        if (state->live) {
            state->live = false;
            prUiEdit(k.ui, canvas, PrUiEditPhase_Commit);
        }
        state->mode = DragMode_None;
    }

    // The canvas's drawing is made again only when what it shows changes.
    uint64_t hash = drawingHash(g, *state, extent);
    PrLayoutContent held = prLayoutNodeContent(tree, drawing);
    if (hash != state->drawn || held.kind != PrLayoutContentKind_DrawList) {
        PrDrawList* list = prDrawListCreate();
        drawCanvas(k, g, *state, extent, list);
        PrLayoutContent content{};
        content.kind = PrLayoutContentKind_DrawList;
        content.list = list;
        prLayoutTreeFrameSetNaturalSize(tree, drawing, prLayoutNaturalSizeNone());
        prLayoutTreeFrameSetContent(tree, drawing, &content);
        prDrawListRelease(list);
        state->drawn = hash;
    }

    // Delete and Backspace ask to remove the selection.
    if (editable) {
        bool any = false;
        for (uint32_t i = 0; i < g.nodeCount; ++i)
            any = any || g.nodes[i].selected;
        for (uint32_t i = 0; i < g.wireCount; ++i)
            any = any || g.wires[i].selected;
        bool pressed = prUiKeyPressed(ui, canvas, PrAppKey_Delete, 0);
        pressed = prUiKeyPressed(ui, canvas, PrAppKey_Backspace, 0) || pressed;
        if (pressed && any) {
            kitSignal(k, canvas, KitSignal_Removed);
            kitEditOnce(k, canvas, [] {});
        }
    }
    order.reset();
    return canvas;
}

PrLayoutNode prKitGraphBody(const PrKit* kit, PrLayoutNode graph, uint32_t index) {
    const GraphRound* round = graphRound(*kit, graph);
    return round != nullptr && index < round->bodyCount ? round->bodies()[index] : kPrLayoutNodeNone;
}

bool prKitConnected(const PrKit* kit, PrLayoutNode graph, PrKitGraphLink* link) {
    const GraphRound* round = graphRound(*kit, graph);
    if (round == nullptr || !round->connected)
        return false;
    *link = round->link;
    return true;
}

PrRect prKitGraphNodeRect(const PrKitGraphNode* node) {
    float width = node->width > 0 ? node->width : kNodeWidth;
    uint32_t rows = node->inputCount > node->outputCount ? node->inputCount : node->outputCount;
    float height = kHeaderHeight + kPortRow * float(rows) + node->bodyHeight + 2;
    return PrRect{node->position.x, node->position.y, node->position.x + width, node->position.y + height};
}

PrVec2 prKitGraphPortAnchor(const PrKitGraphNode* node, PrKitGraphSide side, uint32_t index) {
    PrRect rect = prKitGraphNodeRect(node);
    float y = rect.top + 1 + kHeaderHeight + kPortRow * (float(index) + 0.5f);
    return PrVec2{side == PrKitGraphSide_Input ? rect.left : rect.right, y};
}

void prKitGraphArrange(PrKitGraphNode* nodes, uint32_t nodeCount, const PrKitGraphWire* wires, uint32_t wireCount) {
    PR_ASSERT(nodes != nullptr || nodeCount == 0);
    PR_ASSERT(wires != nullptr || wireCount == 0);
    // A node's column is one past the furthest column that feeds it. Each pass settles at least one more node along
    // every path, so as many passes as there are nodes settle every column that a cycle does not raise without end; a
    // pass beyond that would only follow a cycle, which is ignored.
    Array<uint32_t> columns;
    for (uint32_t i = 0; i < nodeCount; ++i)
        columns.push(0);
    auto indexOf = [&](uint64_t id) {
        for (uint32_t i = 0; i < nodeCount; ++i) {
            if (nodes[i].id == id)
                return i;
        }
        return nodeCount;
    };
    for (uint32_t pass = 0; pass < nodeCount; ++pass) {
        bool moved = false;
        for (uint32_t i = 0; i < wireCount; ++i) {
            uint32_t from = indexOf(wires[i].link.fromNode);
            uint32_t to = indexOf(wires[i].link.toNode);
            if (from == nodeCount || to == nodeCount || from == to || columns.data[to] > columns.data[from])
                continue;
            if (columns.data[from] + 1 >= nodeCount)
                continue;
            columns.data[to] = columns.data[from] + 1;
            moved = true;
        }
        if (!moved)
            break;
    }
    float x = 0;
    for (uint32_t column = 0; column < nodeCount; ++column) {
        float y = 0;
        float widest = 0;
        for (uint32_t i = 0; i < nodeCount; ++i) {
            if (columns.data[i] != column)
                continue;
            nodes[i].position = PrVec2{x, y};
            PrRect rect = prKitGraphNodeRect(&nodes[i]);
            y += prRectHeight(rect) + kArrangeRowGap;
            widest = prF32Max(widest, prRectWidth(rect));
        }
        if (widest > 0)
            x += widest + kArrangeColumnGap;
    }
}
