#include "pr/array.h"
#include "pr/float.h"
#include "pr/kit_box.h"
#include "pr/kit_button.h"
#include "pr/kit_color.h"
#include "pr/kit_input.h"
#include "pr/kit_menu.h"
#include "pr/span.h"
#include "pr/string.h"

#include "tools/harness/kit_fixture.h"

#include "tools/harness/test.h"

using namespace harness;

namespace pr_impl {

namespace {

void down(KitFixture& f, PrVec2 at) {
    f.push(PrAppEvent{.kind = PrAppEventKind_PointerDown, .pointer = {.position = at}});
}
void moveTo(KitFixture& f, PrVec2 at, PrAppModifierFlags modifiers = 0) {
    f.push(PrAppEvent{.kind = PrAppEventKind_PointerMove, .modifiers = modifiers, .pointer = {.position = at}});
}
void up(KitFixture& f, PrVec2 at) {
    f.push(PrAppEvent{.kind = PrAppEventKind_PointerUp, .pointer = {.position = at}});
}

// Drags from `from` by each of `steps` px horizontally, a frame each, and releases.
void dragBy(KitFixture& f, PrVec2 from, Span<const float> steps, PrAppModifierFlags modifiers = 0) {
    down(f, from);
    f.frame();
    PrVec2 at = from;
    for (float step : steps) {
        at.x += step;
        moveTo(f, at, modifiers);
        f.frame();
    }
    up(f, at);
    f.frame();
}

// Drags from `from` by `step` px horizontally in one frame, and releases.
void dragBy(KitFixture& f, PrVec2 from, float step, PrAppModifierFlags modifiers = 0) {
    dragBy(f, from, Span<const float>(&step, 1), modifiers);
}

// The phases that an edit sink saw, in order.
struct Edits {
    Array<PrUiEditPhase> phases;
    Array<PrLayoutNode> controls;

    static void record(void* context, PrLayoutNode control, PrUiEditPhase phase) {
        Edits* edits = static_cast<Edits*>(context);
        edits->phases.push(phase);
        edits->controls.push(control);
    }
    uint32_t count(PrUiEditPhase phase) const {
        uint32_t n = 0;
        for (PrUiEditPhase p : phases)
            n += p == phase ? 1 : 0;
        return n;
    }
};

// A fixture whose controls report their gestures to `edits`.
struct EditFixture : KitFixture {
    Edits edits;
    EditFixture() {
        edit = Edits::record;
        editContext = &edits;
    }
};

// Returns the center of a rectangle.
PrVec2 middle(PrRect r) {
    return PrVec2{(r.left + r.right) / 2, (r.top + r.bottom) / 2};
}

// Returns the border box of a node as laid out, before any transform.
PrRect laidOut(KitFixture& f, PrLayoutNode node) {
    return prLayoutRectToPx(prLayoutNodeBounds(f.tree, node, PrLayoutCoordinates_Layout));
}

// Returns where the point `p` of a panel's plane, in layout px, lands in the viewport: turned by `turn` about the
// panel's center `panel`, and projected by a perspective of `distance` about the center `stage` of the panel's parent.
PrVec2 landed(PrVec2 p, PrVec2 panel, PrVec2 stage, const PrMatrix4& turn, float distance) {
    PrMatrix4 toPanel = prMatrix4Translate(PrVec3{-panel.x, -panel.y, 0});
    PrMatrix4 fromPanel = prMatrix4Translate(PrVec3{panel.x, panel.y, 0});
    PrMatrix4 toStage = prMatrix4Translate(PrVec3{-stage.x, -stage.y, 0});
    PrMatrix4 fromStage = prMatrix4Translate(PrVec3{stage.x, stage.y, 0});
    PrMatrix4 perspective = prMatrix4Perspective(distance);
    PrMatrix4 m = prMatrix4Concat(&toPanel, &turn);
    m = prMatrix4Concat(&m, &fromPanel);
    m = prMatrix4Concat(&m, &toStage);
    m = prMatrix4Concat(&m, &perspective);
    m = prMatrix4Concat(&m, &fromStage);
    PrVec2 out{};
    REQUIRE(prMatrix4MapPoint(&m, p, &out));
    return out;
}

// Writes the text of the control `field` to `owned`, if it changed in the round, and returns the control.
PrLayoutNode taken(PrKit* kit, PrLayoutNode field, String& owned) {
    PrStringView text{};
    if (prKitTextChanged(kit, field, &text))
        owned.assign(text);
    return field;
}

// Returns the asset field's gutter buttons, in order: browse, then locate and clear unless the field is empty.
Array<PrLayoutNode> gutterButtons(KitFixture& f, PrLayoutNode field) {
    Array<PrLayoutNode> buttons;
    PrLayoutNode gutter = prLayoutNodeLastChild(f.tree, field);
    for (PrLayoutNode n = prLayoutNodeFirstChild(f.tree, gutter); n != kPrLayoutNodeNone;
            n = prLayoutNodeNextSibling(f.tree, n))
        buttons.push(n);
    return buttons;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("dragging a number field changes its value by the step for each px, and clamps to its range") {
    KitFixture f;
    float value = 1;
    PrLayoutNode field = kPrLayoutNodeNone;
    f.build = [&](PrLayoutNode root) {
        PrLayoutNode row = prKitBox(f.kit, root, {}, {.direction = PrKitDirection_Row, .gap = 0});
        PrKitNumber number{.value = prKitScalarF32(&value), .range = {.step = 0.1, .max = 4, .hasMax = true}};
        field = prKitNumberField(f.kit, row, number, {});
    };
    f.frame();
    PrVec2 at = f.center(field);
    const float steps[] = {10, 10};
    dragBy(f, at, steps);
    CHECK(value == doctest::Approx(3.0));
    dragBy(f, at, 30);
    CHECK(value == doctest::Approx(4.0));
    CHECK(kitVerified(f.tree));
}

TEST_CASE("a scrub past a bound turns back as soon as the pointer does") {
    KitFixture f;
    double value = 0;
    PrLayoutNode field = kPrLayoutNodeNone;
    f.build = [&](PrLayoutNode root) {
        PrKitNumber number{
            .value = prKitScalarF64(&value),
            .range = {.step = 1, .min = 0, .max = 5, .hasMin = true, .hasMax = true},
        };
        field = prKitNumberField(f.kit, root, number, {});
    };
    f.frame();
    const float steps[] = {20, -3};
    dragBy(f, f.center(field), steps);
    CHECK(value == 2.0);
}

TEST_CASE("Shift scrubs a tenth of a step for each px") {
    KitFixture f;
    float value = 0;
    PrLayoutNode field = kPrLayoutNodeNone;
    f.build = [&](PrLayoutNode root) {
        PrKitNumber number{.value = prKitScalarF32(&value), .range = {.step = 0.1}};
        field = prKitNumberField(f.kit, root, number, {});
    };
    f.frame();
    PrVec2 at = f.center(field);
    down(f, at);
    f.frame();
    moveTo(f, PrVec2{at.x + 10, at.y});
    f.frame();
    CHECK(value == doctest::Approx(1.0));
    moveTo(f, PrVec2{at.x + 30, at.y}, PrAppModifierFlag_Shift);
    f.frame();
    up(f, PrVec2{at.x + 30, at.y});
    f.frame();
    CHECK(value == doctest::Approx(1.2));
}

TEST_CASE("an integer field scrubs in whole values, and stops at its kind's bounds") {
    KitFixture f;
    uint8_t value = 250;
    PrLayoutNode field = kPrLayoutNodeNone;
    f.build = [&](PrLayoutNode root) {
        PrKitNumber number{.value = prKitScalarU8(&value), .range = {.step = 0.5}};
        field = prKitNumberField(f.kit, root, number, {});
    };
    f.frame();
    PrVec2 at = f.center(field);
    const float steps[] = {-4, -4};
    dragBy(f, at, steps);
    CHECK(value == 246);
    dragBy(f, at, 40);
    CHECK(value == 255);
}

TEST_CASE("a focused number field steps by arrow keys, by ten steps and by one with Shift") {
    KitFixture f;
    float value = 0;
    int32_t count = 0;
    PrLayoutNode field = kPrLayoutNodeNone;
    PrLayoutNode intField = kPrLayoutNodeNone;
    f.build = [&](PrLayoutNode root) {
        PrKitNumberRange range{.step = 0.1};
        field = prKitNumberField(f.kit, root, {.value = prKitScalarF32(&value), .range = range}, {});
        intField = prKitNumberField(f.kit, root, {.value = prKitScalarI32(&count)}, {});
    };
    f.frame();
    f.key(PrAppKey_Up);
    f.frame();
    CHECK(value == 0.0f);
    f.click(f.center(field));
    f.frame();
    REQUIRE(prUiFocus(f.ui) == field);
    f.key(PrAppKey_Up);
    f.frame();
    CHECK(value == doctest::Approx(1.0));
    f.key(PrAppKey_Down, PrAppModifierFlag_Shift);
    f.frame();
    CHECK(value == doctest::Approx(0.9));
    // An integer field moves by one if its steps add up to less.
    f.click(f.center(intField));
    f.frame();
    f.key(PrAppKey_Up);
    f.frame();
    CHECK(count == 1);
    CHECK(kitVerified(f.tree));
}

TEST_CASE("a disabled number field takes no scrub and no focus") {
    KitFixture f;
    float value = 1;
    PrLayoutNode field = kPrLayoutNodeNone;
    f.build = [&](PrLayoutNode root) {
        field = prKitNumberField(f.kit, prKitBox(f.kit, root, {.disabled = true}, {}),
            {.value = prKitScalarF32(&value)}, {});
    };
    f.frame();
    dragBy(f, f.center(field), 20);
    CHECK(value == 1.0f);
    CHECK(prUiFocus(f.ui) == kPrLayoutNodeNone);
}

TEST_CASE("a vector edits each component through its own field, tagged in the axes' tones") {
    KitFixture f;
    float xyz[3] = {1, 2, 3};
    PrLayoutNode vector = kPrLayoutNodeNone;
    f.build = [&](PrLayoutNode root) {
        PrKitNumberRange range{.step = 0.5};
        vector = prKitVectorField(f.kit, root, {.value = prKitScalarF32(xyz), .count = 3, .range = range}, {});
    };
    f.frame();
    PrRect row = f.bounds(vector);
    CHECK(row.right - row.left == 800.0f);
    float third = (row.right - row.left) / 3;
    PrVec2 middle{row.left + third * 1.5f, (row.top + row.bottom) / 2};
    const float steps[] = {4, 4};
    dragBy(f, middle, steps);
    CHECK(xyz[0] == 1.0f);
    CHECK(xyz[1] == 6.0f);
    CHECK(xyz[2] == 3.0f);
    PrVec2 last{row.left + third * 2.5f, middle.y};
    dragBy(f, last, -4);
    CHECK(xyz[2] == 1.0f);
    CHECK(kitVerified(f.tree));
}

TEST_CASE("a slider takes the fraction a press stands at, and follows the drag") {
    KitFixture f;
    float value = 0;
    PrLayoutNode slider = kPrLayoutNodeNone;
    f.build = [&](PrLayoutNode root) {
        PrLayoutNode row = prKitBox(f.kit, root, {}, {.direction = PrKitDirection_Row, .gap = 0});
        slider = prKitSlider(f.kit, row, {.value = prKitScalarF32(&value), .min = 0, .max = 10}, {});
    };
    f.frame();
    PrRect box = f.bounds(slider);
    float pad = kPrKitSpace3 / 2;
    float width = box.right - box.left - 2 * pad;
    float y = (box.top + box.bottom) / 2;
    f.click(PrVec2{box.left + pad + width / 4, y});
    f.frame();
    CHECK(value == doctest::Approx(2.5).epsilon(0.001));
    dragBy(f, PrVec2{box.left + pad + width / 2, y}, width / 4);
    CHECK(value == doctest::Approx(7.5).epsilon(0.001));
    dragBy(f, PrVec2{box.left + pad + width / 2, y}, width);
    CHECK(value == 10.0f);
    CHECK(kitVerified(f.tree));
}

TEST_CASE("controls on a plane turned in perspective take input where they are drawn, measured in the plane") {
    KitFixture f;
    float value = 0;
    bool activated = false;
    PrLayoutNode stage = kPrLayoutNodeNone;
    PrLayoutNode panel = kPrLayoutNodeNone;
    PrLayoutNode slider = kPrLayoutNodeNone;
    PrLayoutNode button = kPrLayoutNodeNone;
    const float distance = 300;
    PrMatrix4 turn = prMatrix4Rotate(PrVec3{0, 1, 0}, 1.0f);
    f.build = [&](PrLayoutNode root) {
        // A stage that is the eye for the panel under it, and the panel turned in it.
        PrKitPlaneStyle outer{
            .box = {
                .padding = PrKitEdges{40, 0, 0, 80},
                .place = {.width = prKitPx(400), .height = prKitPx(300), .flex = PrKitFlex_Fixed},
            },
            .perspective = distance,
        };
        stage = prKitPlane(f.kit, root, {}, outer);
        PrKitPlaneStyle inner{
            .box = {
                .align = PrKitAlign_Start,
                .gap = 8,
                .padding = prKitEdgesAll(12),
                .place = {.width = prKitPx(240), .align = PrKitAlign_Start},
            },
        };
        panel = prKitPlane(f.kit, stage, {.transform = &turn}, inner);
        slider = prKitSlider(f.kit, panel, {.value = prKitScalarF32(&value), .min = 0, .max = 10}, {});
        PrKitPlace fill{.align = PrKitAlign_Stretch};
        prKitPlace(f.kit, slider, &fill);
        button = prKitButton(f.kit, panel, {.text = "Press"}, {});
        activated = activated || prKitActivated(f.kit, button);
    };
    f.frame();
    PrVec2 panelCenter = middle(laidOut(f, panel));
    PrVec2 stageCenter = middle(laidOut(f, stage));
    auto at = [&](PrVec2 p) { return landed(p, panelCenter, stageCenter, turn, distance); };

    // The button sits at the panel's left, which the turn moves: a click where it is drawn activates it.
    PrVec2 flat = middle(laidOut(f, button));
    PrVec2 drawn = at(flat);
    CHECK(prF32Abs(drawn.x - flat.x) > 10.0f);
    f.click(drawn);
    f.frame();
    CHECK(activated);

    // A press on the track takes the fraction of the track under the pointer in the panel's plane, and a drag follows.
    PrRect box = laidOut(f, slider);
    float pad = kPrKitSpace3 / 2;
    float width = box.right - box.left - 2 * pad;
    float y = (box.top + box.bottom) / 2;
    down(f, at(PrVec2{box.left + pad + width / 4, y}));
    f.frame();
    CHECK(value == doctest::Approx(2.5).epsilon(0.01));
    moveTo(f, at(PrVec2{box.left + pad + width * 3 / 4, y}));
    f.frame();
    CHECK(value == doctest::Approx(7.5).epsilon(0.01));
    up(f, at(PrVec2{box.left + pad + width * 3 / 4, y}));
    f.frame();
    CHECK(kitVerified(f.tree));
}

TEST_CASE("a plane that hides its back takes no input while it shows it, and a plane's level orders it") {
    KitFixture f;
    PrLayoutNode stage = kPrLayoutNodeNone;
    PrLayoutNode clicked = kPrLayoutNodeNone;
    PrLayoutNode back = kPrLayoutNodeNone;
    PrLayoutNode front = kPrLayoutNodeNone;
    PrMatrix4 flip = prMatrix4Rotate(PrVec3{0, 1, 0}, 3.1415927f);
    bool hidden = false;
    int32_t level = 0;
    f.build = [&](PrLayoutNode root) {
        PrKitPlaneStyle outer{
            .box = {.place = {.width = prKitPx(200), .height = prKitPx(200), .flex = PrKitFlex_Fixed,}},
            .perspective = 300,
        };
        stage = prKitPlane(f.kit, root, {}, outer);
        PrKitBoxStyle cover{.place = {.width = prKitPercent(100), .height = prKitPercent(100)}};
        back = prKitPlane(f.kit, prKitStack(f.kit, stage, {}, cover), {}, {.box = cover, .zIndex = level});
        // The layer that holds the front plane lets the pointer through to what is under it.
        PrLayoutNode layer = prKitLayer(f.kit, stage, {}, cover);
        prUiSetFlags(f.ui, layer, PrUiFlag_Transparent);
        front = prKitPlane(f.kit, layer, {.transform = hidden ? &flip : nullptr}, {.box = cover, .backfaceHidden = true});
        const PrLayoutNode planes[] = {back, front};
        for (PrLayoutNode node : planes) {
            prUiSetFlags(f.ui, node, PrUiFlag_Pressable);
            if (prUiClicked(f.ui, node, 0))
                clicked = node;
        }
    };
    f.frame();
    PrVec2 middle = f.center(stage);
    f.click(middle);
    f.frame();
    CHECK(clicked == front);
    hidden = true;
    f.frame();
    f.click(middle);
    f.frame();
    CHECK(clicked == back);
    // Shown again, the front plane is on top until the plane under it takes a higher level.
    hidden = false;
    level = 1;
    f.frame();
    f.click(middle);
    f.frame();
    CHECK(clicked == back);
}

TEST_CASE("a focused slider moves by its step with Left and Right, and to its ends with Home and End") {
    KitFixture f;
    double value = 0.5;
    PrLayoutNode slider = kPrLayoutNodeNone;
    f.build = [&](PrLayoutNode root) {
        PrKitSlider content{.value = prKitScalarF64(&value), .step = 0.1f};
        slider = prKitSlider(f.kit, root, content, {});
    };
    f.frame();
    f.click(f.center(slider));
    f.frame();
    REQUIRE(prUiFocus(f.ui) == slider);
    f.key(PrAppKey_Right);
    f.frame();
    CHECK(value == doctest::Approx(0.6));
    f.key(PrAppKey_Left, PrAppModifierFlag_Shift);
    f.frame();
    CHECK(value == 0.0);
    f.key(PrAppKey_End);
    f.frame();
    CHECK(value == 1.0);
}

TEST_CASE("a ratio slider stands its midpoint at the geometric mean of its range") {
    KitFixture f;
    float value = 0;
    PrLayoutNode slider = kPrLayoutNodeNone;
    f.build = [&](PrLayoutNode root) {
        PrKitSlider content{.value = prKitScalarF32(&value), .min = 1, .max = 100, .scale = PrKitSliderScale_Ratio};
        slider = prKitSlider(f.kit, root, content, {});
    };
    f.frame();
    f.click(f.center(slider));
    f.frame();
    CHECK(value == doctest::Approx(10.0).epsilon(0.01));
}

TEST_CASE("a drag reports one begin and one commit, with its changes between") {
    EditFixture f;
    float value = 0;
    float level = 0;
    PrLayoutNode field = kPrLayoutNodeNone;
    PrLayoutNode slider = kPrLayoutNodeNone;
    f.build = [&](PrLayoutNode root) {
        field = prKitNumberField(f.kit, root, {.value = prKitScalarF32(&value)}, {});
        slider = prKitSlider(f.kit, root, {.value = prKitScalarF32(&level), .min = 0, .max = 1}, {});
    };
    f.frame();
    const float fieldSteps[] = {10, 10, 10};
    dragBy(f, f.center(field), fieldSteps);
    REQUIRE(f.edits.phases.count() == 5);
    CHECK(f.edits.phases[0] == PrUiEditPhase_Begin);
    CHECK(f.edits.phases.last() == PrUiEditPhase_Commit);
    CHECK(f.edits.count(PrUiEditPhase_Change) == 3);
    for (PrLayoutNode control : f.edits.controls)
        CHECK(control == field);

    f.edits = Edits{};
    PrVec2 at = f.center(slider);
    const float sliderSteps[] = {50, 50};
    dragBy(f, PrVec2{at.x - 100, at.y}, sliderSteps);
    CHECK(f.edits.count(PrUiEditPhase_Begin) == 1);
    CHECK(f.edits.count(PrUiEditPhase_Commit) == 1);
    CHECK(f.edits.count(PrUiEditPhase_Change) == 3);
    CHECK(f.edits.phases.last() == PrUiEditPhase_Commit);
}

TEST_CASE("a press that does not scrub opens no gesture, and a key press is a gesture of its own") {
    EditFixture f;
    float value = 0;
    PrLayoutNode field = kPrLayoutNodeNone;
    f.build = [&](PrLayoutNode root) {
        field = prKitNumberField(f.kit, root, {.value = prKitScalarF32(&value)}, {});
    };
    f.frame();
    f.click(f.center(field));
    f.frame();
    CHECK(f.edits.phases.isEmpty());
    f.key(PrAppKey_Up);
    f.frame();
    Array<PrUiEditPhase> expected({PrUiEditPhase_Begin, PrUiEditPhase_Change, PrUiEditPhase_Commit});
    CHECK(f.edits.phases == expected);
}

TEST_CASE("an inspector group's header discloses and collapses its body") {
    KitFixture f;
    bool open = true;
    PrLayoutNode body = kPrLayoutNodeNone;
    PrLayoutNode row = kPrLayoutNodeNone;
    PrLayoutNode kept = kPrLayoutNodeNone;
    f.build = [&](PrLayoutNode root) {
        PrKitInspectorGroup group{
            .name = "Transform",
            .badge = "CORE",
            .badgeVariant = PrKitBadgeVariant_Accent,
            .open = &open,
        };
        body = prKitInspectorGroup(f.kit, root, group);
        row = kPrLayoutNodeNone;
        if (body != kPrLayoutNodeNone)
            row = prKitInspectorRow(f.kit, body, {.label = "Position"}, {});
        PrKitInspectorGroup own{.name = "Mesh"};
        kept = prKitInspectorGroup(f.kit, root, own);
    };
    f.frame();
    REQUIRE(body != kPrLayoutNodeNone);
    CHECK(kept != kPrLayoutNodeNone);
    PrRect header = f.bounds(body);
    CHECK(header.top == doctest::Approx(double(kPrKitHeightSm)).epsilon(0.05));
    f.click(PrVec2{100, kPrKitHeightSm / 2});
    f.frame();
    CHECK_FALSE(open);
    CHECK(body == kPrLayoutNodeNone);
    CHECK(row == kPrLayoutNodeNone);
    f.click(PrVec2{100, kPrKitHeightSm / 2});
    f.frame();
    CHECK(open);
    CHECK(body != kPrLayoutNodeNone);
    CHECK(kitVerified(f.tree));
}

TEST_CASE("a group the caller keeps no flag for is open at first, and keeps its own") {
    KitFixture f;
    PrLayoutNode body = kPrLayoutNodeNone;
    f.build = [&](PrLayoutNode root) {
        PrKitInspectorGroup group{.name = "Rigidbody"};
        body = prKitInspectorGroup(f.kit, root, group);
    };
    f.frame();
    CHECK(body != kPrLayoutNodeNone);
    f.click(PrVec2{100, kPrKitHeightSm / 2});
    f.frame();
    CHECK(body == kPrLayoutNodeNone);
    f.frame();
    CHECK(body == kPrLayoutNodeNone);
}

TEST_CASE("an inspector row lays its control out after its label column") {
    KitFixture f;
    PrLayoutNode controls = kPrLayoutNodeNone;
    PrLayoutNode field = kPrLayoutNodeNone;
    float value = 0;
    f.build = [&](PrLayoutNode root) {
        controls = prKitInspectorRow(f.kit, root, {.label = "Mass"}, {});
        PrKitNumber number{.value = prKitScalarF32(&value), .unit = "kg"};
        field = prKitNumberField(f.kit, controls, number, {});
    };
    f.frame();
    PrRect region = f.bounds(controls);
    CHECK(region.left == doctest::Approx(kPrKitSpace6 + 88 + kPrKitSpace3));
    CHECK(region.right == doctest::Approx(800 - kPrKitSpace3));
    // A field in a row fills it.
    CHECK(f.bounds(field).right == doctest::Approx(region.right));
    CHECK(kitVerified(f.tree));
}

TEST_CASE("an asset field takes a dropped payload of its kind, and passes others by") {
    KitFixture f;
    uint64_t kind = 7;
    PrLayoutNode source = kPrLayoutNodeNone;
    PrLayoutNode field = kPrLayoutNodeNone;
    uint32_t drops = 0;
    uint64_t dropped = 0;
    f.build = [&](PrLayoutNode root) {
        PrKitButton content{.text = "cube.mesh"};
        source = prKitButton(f.kit, root, content, {});
        if (prUiIsPressed(f.ui, source, 0))
            prUiDragStart(f.ui, source, 4, kind, 42);
        PrKitAssetField ref{.name = "player.mesh", .icon = PrKitIconKind_Cube, .kind = 7};
        field = prKitAssetField(f.kit, root, ref);
        uint64_t value = 0;
        if (prKitDropped(f.kit, field, &value)) {
            ++drops;
            dropped = value;
        }
    };
    f.frame();
    CHECK(gutterButtons(f, field).count() == 3);
    auto carry = [&] {
        PrVec2 from = f.center(source);
        PrVec2 to = f.center(field);
        down(f, from);
        f.frame();
        moveTo(f, to);
        f.frame();
        f.frame();
        up(f, to);
        f.frame();
    };
    carry();
    CHECK(drops == 1);
    CHECK(dropped == 42);
    kind = 8;
    carry();
    CHECK(drops == 1);
    CHECK(kitVerified(f.tree));
}

TEST_CASE("an empty asset field offers browsing alone, and reports its gutter's button on the field") {
    KitFixture f;
    PrLayoutNode field = kPrLayoutNodeNone;
    uint32_t browsed = 0;
    f.build = [&](PrLayoutNode root) {
        PrKitAssetField ref{.name = "None", .empty = true};
        field = prKitAssetField(f.kit, root, ref);
        browsed += prKitBrowsed(f.kit, field) ? 1 : 0;
        CHECK_FALSE(prKitLocated(f.kit, field));
        CHECK_FALSE(prKitCleared(f.kit, field));
    };
    f.frame();
    Array<PrLayoutNode> buttons = gutterButtons(f, field);
    REQUIRE(buttons.count() == 1);
    f.click(f.center(buttons[0]));
    f.frame();
    CHECK(browsed == 1);
}

TEST_CASE("a frame of the same inspector makes no node and lays nothing out") {
    KitFixture f;
    bool open = true;
    float position[3] = {1, 2, 3};
    float mass = 1;
    float drag = 0.25f;
    PrColor tint{0.2f, 0.4f, 0.8f, 1};
    f.build = [&](PrLayoutNode root) {
        PrKitInspectorGroup transform{.name = "Transform", .badge = "CORE", .open = &open};
        PrLayoutNode body = prKitInspectorGroup(f.kit, root, transform);
        PrLayoutNode positionRow = prKitInspectorRow(f.kit, body, {.label = "Position"}, {});
        prKitVectorField(f.kit, positionRow, {.value = prKitScalarF32(position), .count = 3}, {});
        PrKitNumber number{.value = prKitScalarF32(&mass), .unit = "kg"};
        prKitNumberField(f.kit, prKitInspectorRow(f.kit, body, {.label = "Mass"}, {}), number, {});
        PrLayoutNode dragRow = prKitInspectorRow(f.kit, body, {.label = "Drag"}, {});
        prKitSlider(f.kit, dragRow, {.value = prKitScalarF32(&drag), .min = 0, .max = 1}, {});
        prKitNumberField(f.kit, dragRow, {.value = prKitScalarF32(&drag)}, {});
        prKitColorField(f.kit, prKitInspectorRow(f.kit, body, {.label = "Tint"}, {}), {.color = &tint}, {});
        PrKitAssetField ref{.name = "player_pbr", .icon = PrKitIconKind_Sphere, .tint = tint, .kind = 1};
        prKitAssetField(f.kit, prKitInspectorRow(f.kit, body, {.label = "Material"}, {}), ref);
    };
    f.frame();
    f.frame();
    PrLayoutTreeStats stats = prLayoutTreeStats(f.tree);
    CHECK(stats.frameNodesMade == 0);
    CHECK(stats.blocksLaidOut == 0);
    CHECK(kitVerified(f.tree));
}

TEST_CASE("a scrub changes the field's text alone, and leaves its siblings' layout") {
    KitFixture f;
    float value = 0;
    PrLayoutNode field = kPrLayoutNodeNone;
    f.build = [&](PrLayoutNode root) {
        for (uint32_t i = 0; i < 4; ++i)
            prKitInspectorRow(f.kit, root, {.label = "Other"}, {});
        PrLayoutNode speed = prKitInspectorRow(f.kit, root, {.label = "Speed"}, {});
        field = prKitNumberField(f.kit, speed, {.value = prKitScalarF32(&value)}, {});
    };
    f.frame();
    f.frame();
    PrVec2 at = f.center(field);
    dragBy(f, at, 10);
    CHECK(value != 0.0f);
    f.frame();
    CHECK(prLayoutTreeStats(f.tree).frameNodesMade == 0);
    CHECK(kitVerified(f.tree));
}

TEST_CASE("a text field's clear control empties its text as one gesture, and shows only while there is text") {
    KitFixture f;
    String text = "level_01";
    uint32_t commits = 0;
    f.edit = [](void* context, PrLayoutNode, PrUiEditPhase phase) {
        *static_cast<uint32_t*>(context) += phase == PrUiEditPhase_Commit ? 1 : 0;
    };
    f.editContext = &commits;
    PrLayoutNode field = kPrLayoutNodeNone;
    f.build = [&](PrLayoutNode root) {
        PrLayoutNode row = prKitBox(f.kit, root, {}, {.direction = PrKitDirection_Row, .gap = 0});
        PrKitTextField content{.text = text, .placeholder = "Search", .clearable = true};
        field = taken(f.kit, prKitTextField(f.kit, row, content, {}), text);
    };
    f.frame();
    PrRect box = f.bounds(field);
    f.click(PrVec2{box.right - 10, (box.top + box.bottom) / 2});
    f.frame();
    CHECK(text.isEmpty());
    CHECK(commits == 1);
    CHECK(prUiFocus(f.ui) == field);
    CHECK(kitVerified(f.tree));
}

TEST_CASE("Up and Down recall a text field's history while it has focus, and past the latest line is empty") {
    KitFixture f;
    String text = "";
    const PrStringView history[] = {"build", "bench"};
    PrLayoutNode field = kPrLayoutNodeNone;
    f.build = [&](PrLayoutNode root) {
        PrKitTextField content{.text = text, .history = history, .historyCount = 2};
        field = taken(f.kit, prKitTextField(f.kit, root, content, {}), text);
    };
    f.frame();
    f.click(f.center(field));
    f.frame();
    f.key(PrAppKey_Up);
    f.frame();
    CHECK(text == "bench");
    f.key(PrAppKey_Up);
    f.frame();
    CHECK(text == "build");
    f.key(PrAppKey_Down);
    f.frame();
    f.key(PrAppKey_Down);
    f.frame();
    CHECK(text.isEmpty());
    CHECK(kitVerified(f.tree));
}

TEST_CASE("a suggest field opens its list as it takes focus, and a suggestion replaces the text") {
    KitFixture f;
    String text = "lib/g";
    const PrStringView suggestions[] = {"lib/gpu", "lib/geom"};
    PrLayoutNode field = kPrLayoutNodeNone;
    f.build = [&](PrLayoutNode root) {
        PrKitSuggestField content{.field = {.text = text}, .suggestions = suggestions, .count = 2};
        field = taken(f.kit, prKitSuggestField(f.kit, root, content, {}), text);
    };
    f.frame();
    f.click(f.center(field));
    f.frame();
    f.frame();
    REQUIRE(prKitIsOpen(f.kit, field));
    // Focus stays in the field while its list is open.
    CHECK(prUiFocus(f.ui) == field);
    PrRect box = f.bounds(field);
    f.click(PrVec2{(box.left + box.right) / 2, box.bottom + 2 + kPrKitHeightMd * 1.5f});
    f.frame();
    CHECK(text == "lib/geom");
    CHECK_FALSE(prKitIsOpen(f.kit, field));
    CHECK(kitVerified(f.tree));
}
