// Studio Kit's node graph.

#include "pr/kit_graph.h"

#include "tools/harness/kit_fixture.h"

#include "tools/harness/test.h"

#include <vector>

using namespace harness;

namespace pr_impl {

namespace {

// The types of the scene's ports.
enum : uint32_t {
    PortType_Scalar,
    PortType_Color,
};
typedef uint32_t PortType;

// The phases that an edit sink saw.
struct Phases {
    std::vector<PrUiEditPhase> seen;

    static void record(void* context, PrLayoutNode, PrUiEditPhase phase) {
        static_cast<Phases*>(context)->seen.push_back(phase);
    }
    uint32_t count(PrUiEditPhase phase) const {
        uint32_t n = 0;
        for (PrUiEditPhase p : seen)
            n += p == phase ? 1 : 0;
        return n;
    }
};

// A graph of three nodes on a canvas 600 by 400 px: a sampler, whose color feeds a blend's base, and an output, with a
// body under its port.
struct GraphScene {
    KitFixture& f;
    Phases phases;
    PrKitGraphPort samplerOut[2] = {
        {.id = 1, .type = PortType_Color, .label = "Color"},
        {.id = 2, .type = PortType_Scalar, .label = "Alpha"},
    };
    PrKitGraphPort blendIn[2] = {
        {.id = 1, .type = PortType_Color, .label = "Base"},
        {.id = 2, .type = PortType_Scalar, .label = "Mask"},
    };
    PrKitGraphPort blendOut[1] = {{.id = 3, .type = PortType_Color, .label = "Out"}};
    PrKitGraphPort outputIn[1] = {{.id = 1, .type = PortType_Color, .label = "Color"}};
    std::vector<PrKitGraphNode> nodes = {
        {.id = 1, .position = {20, 20}, .title = "Sampler", .outputs = samplerOut, .outputCount = 2},
        {
            .id = 2,
            .position = {260, 20},
            .title = "Blend",
            .inputs = blendIn,
            .inputCount = 2,
            .outputs = blendOut,
            .outputCount = 1,
        },
        {.id = 3, .position = {260, 200}, .title = "Output", .inputs = outputIn, .inputCount = 1, .bodyHeight = 40},
    };
    std::vector<PrKitGraphWire> wires = {{.id = 1, .link = {.fromNode = 1, .fromPort = 1, .toNode = 2, .toPort = 1}}};
    PrColor colors[2] = {{0.4f, 0.6f, 0.9f, 1}, {0.9f, 0.6f, 0.3f, 1}};
    PrKitGraphView view{.zoom = 1};
    PrLayoutNode bodies[3] = {};
    PrLayoutNode canvas = kPrLayoutNodeNone;
    std::vector<PrKitGraphLink> connected;
    uint32_t removals = 0;

    explicit GraphScene(KitFixture& fixture) : f(fixture) {
        f.edit = Phases::record;
        f.editContext = &phases;
        f.build = [this](PrLayoutNode ground) {
            PrKitGraph graph{
                .nodes = nodes.data(),
                .nodeCount = uint32_t(nodes.size()),
                .wires = wires.data(),
                .wireCount = uint32_t(wires.size()),
                .typeColors = colors,
                .typeCount = 2,
                .view = &view,
            };
            canvas = prKitGraph(f.kit, ground, graph,
                {.place = {.width = prKitPx(600), .height = prKitPx(400), .flex = PrKitFlex_Fixed}});
            for (uint32_t i = 0; i < 3; ++i)
                bodies[i] = prKitGraphBody(f.kit, canvas, i);
            PrKitGraphLink link{};
            if (prKitConnected(f.kit, canvas, &link))
                connected.push_back(link);
            removals += prKitRemoved(f.kit, canvas) ? 1 : 0;
        };
    }
    // Returns where a point of the graph is on the screen.
    PrVec2 at(PrVec2 point) {
        PrRect box = f.bounds(canvas);
        return PrVec2{box.left + view.offset.x + point.x * view.zoom, box.top + view.offset.y + point.y * view.zoom};
    }
    PrVec2 port(uint32_t node, PrKitGraphSide side, uint32_t index) {
        return at(prKitGraphPortAnchor(&nodes[node], side, index));
    }

    void drag(PrVec2 from, PrVec2 to, PrAppModifierFlags modifiers = 0) {
        f.push(PrAppEvent{.kind = PrAppEventKind_PointerDown, .modifiers = modifiers, .position = from});
        f.frame();
        f.push(PrAppEvent{.kind = PrAppEventKind_PointerMove, .modifiers = modifiers, .position = to});
        f.frame();
        f.push(PrAppEvent{.kind = PrAppEventKind_PointerUp, .modifiers = modifiers, .position = to});
        f.frame();
    }
};

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a drag on a node moves it and the other selected nodes, as one gesture") {
    KitFixture f;
    GraphScene scene(f);
    f.frame();
    f.frame();
    f.click(scene.at({300, 210}));
    f.frame();
    CHECK(scene.nodes[2].selected);
    scene.drag(scene.at({60, 30}), scene.at({90, 70}), PrAppModifierFlag_Shift);
    CHECK(scene.nodes[0].selected);
    CHECK(scene.nodes[0].position.x == doctest::Approx(50.0));
    CHECK(scene.nodes[0].position.y == doctest::Approx(60.0));
    CHECK(scene.nodes[2].position.x == doctest::Approx(290.0));
    CHECK(scene.nodes[1].position.x == 260.0f);
    CHECK(scene.phases.count(PrUiEditPhase_Begin) == 1);
    CHECK(scene.phases.count(PrUiEditPhase_Commit) == 1);
    CHECK(kitVerified(f.tree));
}

TEST_CASE("a wire dropped on a port of the other side and the same type asks to join them, and any other is refused") {
    KitFixture f;
    GraphScene scene(f);
    f.frame();
    f.frame();
    // The sampler's alpha onto the blend's color input: unlike types.
    scene.drag(scene.port(0, PrKitGraphSide_Output, 1), scene.port(1, PrKitGraphSide_Input, 0));
    CHECK(scene.connected.empty());
    // The blend's mask, dragged onto the sampler's alpha, is joined from the output to the input.
    scene.drag(scene.port(1, PrKitGraphSide_Input, 1), scene.port(0, PrKitGraphSide_Output, 1));
    REQUIRE(scene.connected.size() == 1);
    CHECK(scene.connected[0].fromNode == 1);
    CHECK(scene.connected[0].fromPort == 2);
    CHECK(scene.connected[0].toNode == 2);
    CHECK(scene.connected[0].toPort == 2);
    CHECK(kitVerified(f.tree));
}

TEST_CASE("a click on a wire selects it, Delete asks to remove it, and a marquee selects the nodes it touches") {
    KitFixture f;
    GraphScene scene(f);
    f.frame();
    f.frame();
    PrVec2 from = scene.port(0, PrKitGraphSide_Output, 0);
    PrVec2 to = scene.port(1, PrKitGraphSide_Input, 0);
    f.click((from + to) / 2);
    f.frame();
    CHECK(scene.wires[0].selected);
    f.key(PrAppKey_Delete);
    f.frame();
    CHECK(scene.removals == 1);
    // From empty canvas over the blend and the output.
    scene.drag(scene.at({240, 10}), scene.at({500, 210}));
    CHECK_FALSE(scene.nodes[0].selected);
    CHECK(scene.nodes[1].selected);
    CHECK(scene.nodes[2].selected);
    f.click(scene.at({560, 380}));
    f.frame();
    CHECK_FALSE(scene.nodes[1].selected);
    CHECK_FALSE(scene.wires[0].selected);
    CHECK(kitVerified(f.tree));
}

TEST_CASE("the wheel pans a graph, and with Command zooms it about the pointer, which a node's body follows") {
    KitFixture f;
    GraphScene scene(f);
    f.frame();
    f.frame();
    CHECK(scene.bodies[0] == kPrLayoutNodeNone);
    REQUIRE(scene.bodies[2] != kPrLayoutNodeNone);
    PrVec2 over = scene.at({300, 300});
    f.move(over);
    f.push(PrAppEvent{.kind = PrAppEventKind_Scroll, .position = over, .delta = PrVec2{20, 10}});
    f.frame();
    CHECK(scene.view.offset.x == -20.0f);
    CHECK(scene.view.offset.y == -10.0f);
    f.push(PrAppEvent{
        .kind = PrAppEventKind_Scroll,
        .modifiers = PrAppModifierFlag_Super,
        .position = over,
        .delta = PrVec2{0, -70},
    });
    f.frame();
    CHECK(scene.view.zoom == doctest::Approx(2.01).epsilon(0.01));
    // The point of the graph under the pointer stays under it.
    PrVec2 still = scene.at({320, 310});
    CHECK(still.x == doctest::Approx(over.x).epsilon(0.001));
    CHECK(still.y == doctest::Approx(over.y).epsilon(0.001));
    f.frame();
    PrRect body = f.bounds(scene.bodies[2]);
    CHECK(prRectHeight(body) == doctest::Approx(40.0 * double(scene.view.zoom)).epsilon(0.01));
    CHECK(kitVerified(f.tree));
}

TEST_CASE("arranging puts each node in the column after the nodes that feed it, and ignores a cycle") {
    PrKitGraphPort out[1] = {{.id = 1}};
    PrKitGraphPort in[1] = {{.id = 1}};
    PrKitGraphNode nodes[4] = {
        {.id = 1, .position = {500, 500}, .outputs = out, .outputCount = 1},
        {.id = 2, .position = {9, 9}, .inputs = in, .inputCount = 1, .outputs = out, .outputCount = 1},
        {.id = 3, .inputs = in, .inputCount = 1, .outputs = out, .outputCount = 1},
        {.id = 4, .inputs = in, .inputCount = 1},
    };
    // 1 feeds 2 and 3, 2 feeds 3, and 3 and 4 feed each other.
    PrKitGraphWire wires[5] = {
        {.link = {1, 1, 2, 1}},
        {.link = {1, 1, 3, 1}},
        {.link = {2, 1, 3, 1}},
        {.link = {3, 1, 4, 1}},
        {.link = {4, 1, 3, 1}},
    };
    prKitGraphArrange(nodes, 4, wires, 5);
    CHECK(nodes[0].position.x == 0.0f);
    CHECK(nodes[0].position.y == 0.0f);
    CHECK(nodes[1].position.x > nodes[0].position.x);
    CHECK(nodes[2].position.x > nodes[1].position.x);
    CHECK(nodes[2].position.x != nodes[3].position.x);
    for (const PrKitGraphNode& node : nodes)
        CHECK(node.position.y == 0.0f);
}
