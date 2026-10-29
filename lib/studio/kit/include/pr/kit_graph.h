// Studio Kit's node graph: boxes with typed ports, joined by wires, on a canvas that pans and zooms, in a model that
// the caller owns.

#pragma once

#include "pr/color.h"
#include "pr/kit.h"
#include "pr/string.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// A port of a node. Only ports of one type join. `id` names the port among the node's ports on its side.
typedef struct PrKitGraphPort {
    uint64_t id;
    uint32_t type;
    PrStringView label;
} PrKitGraphPort;

// A node: a box at `position`, in the graph's px, `width` px wide or the default width for zero, headed by its title
// and `meta` beside it, with a row for each of its inputs, on its left, and its outputs, on its right. A body
// `bodyHeight` px tall under the rows holds what the caller declares there. `id` names the node from round to round,
// and no other node has it. `selected` marks the node as selected, and the graph writes it and `position`.
typedef struct PrKitGraphNode {
    uint64_t id;
    PrVec2 position;
    float width;
    PrStringView title;
    PrStringView meta;
    const PrKitGraphPort* PR_NULLABLE inputs;
    uint32_t inputCount;
    const PrKitGraphPort* PR_NULLABLE outputs;
    uint32_t outputCount;
    float bodyHeight;
    bool selected;
} PrKitGraphNode;

// The ports that a wire joins: an output of one node and an input of another.
typedef struct PrKitGraphLink {
    uint64_t fromNode;
    uint64_t fromPort;
    uint64_t toNode;
    uint64_t toPort;
} PrKitGraphLink;

// A wire, drawn as a curve in the color of its output's type. A wire that names no port of the model is not drawn. `id`
// names the wire from round to round. `selected` marks the wire as selected, and the graph writes it.
typedef struct PrKitGraphWire {
    uint64_t id;
    PrKitGraphLink link;
    bool selected;
} PrKitGraphWire;

// Where the graph's origin sits on the canvas, in px from its top left corner, and the canvas px for each of the
// graph's px.
typedef struct PrKitGraphView {
    PrVec2 offset;
    float zoom;                     // above zero
} PrKitGraphView;

// The content of a graph: the caller's model, drawn on a canvas. A port's color is `typeColors` at its type, or the
// accent for a type past them. `view` is the part of the graph that the canvas shows, which the wheel pans and, with
// Command or Control, zooms about the pointer.
//
// A drag on a node moves it and the other selected nodes. A press on a node selects it alone; with Shift it joins the
// selection, and with Command or Control it joins or leaves it. A drag from a port to a port of the other side and the
// same type asks to wire them. A click on a wire selects it, a drag on empty canvas selects the nodes that its
// rectangle touches, and a click there clears the selection. Each drag is one gesture.
typedef struct PrKitGraph {
    uint64_t key;
    PrKitGraphNode* PR_NULLABLE nodes;
    uint32_t nodeCount;
    PrKitGraphWire* PR_NULLABLE wires;
    uint32_t wireCount;
    const PrColor* PR_NULLABLE typeColors;
    uint32_t typeCount;
    PrKitGraphView* view;
} PrKitGraph;

// The style of a graph: the canvas's placement in its parent.
typedef struct PrKitGraphStyle {
    PrKitPlace place;
} PrKitGraphStyle;

// Creates a graph under `parent`, applies the round's gestures to its model, and returns its canvas.
PrLayoutNode prKitGraph(PrKit* kit, PrLayoutNode parent, PrKitGraph graph, PrKitGraphStyle style);

// Returns the body of the graph's node at `index`, where the caller creates the node's content, or none for a node with
// no body.
PrLayoutNode prKitGraphBody(const PrKit* kit, PrLayoutNode graph, uint32_t index);

// Returns true if a wire was dropped on a port in the current round, which asks the caller to connect `link`. The
// request is one gesture.
bool prKitConnected(const PrKit* kit, PrLayoutNode graph, PrKitGraphLink* link);

// The side of a node that a port is on.
enum {
    PrKitGraphSide_Input,
    PrKitGraphSide_Output,
};
typedef uint8_t PrKitGraphSide;

// Returns a node's box, or the point where a wire joins its port `index` on `side`, in the graph's px.
PrRect prKitGraphNodeRect(const PrKitGraphNode* node);
PrVec2 prKitGraphPortAnchor(const PrKitGraphNode* node, PrKitGraphSide side, uint32_t index);

// Arranges the nodes in columns by their wires, from left to right: a node stands in the column after the furthest
// column of the nodes that feed it. A column keeps the order of its nodes in the array. A wire that closes a cycle is
// ignored.
void prKitGraphArrange(PrKitGraphNode* PR_NULLABLE nodes, uint32_t nodeCount, const PrKitGraphWire* PR_NULLABLE wires,
                       uint32_t wireCount);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
