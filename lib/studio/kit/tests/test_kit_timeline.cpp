// Studio Kit's timeline.

#include "pr/kit_box.h"
#include "pr/kit_timeline.h"

#include "tools/harness/kit_fixture.h"

#include "tools/harness/test.h"

#include <vector>

using namespace harness;

namespace pr_impl {

namespace {

// The px of the gutter and the ruler, and of a track, as the timeline draws them by default.
constexpr float kGutter = 96;
constexpr float kRuler = 20;
constexpr float kTrack = 26;

// The phases that an edit sink saw.
struct Phases {
    std::vector<PrUiEditPhase> seen;

    static void record(void* context, PrLayoutNode, PrUiEditPhase phase) {
        static_cast<Phases*>(context)->seen.push_back(phase);
    }
};

// A timeline of two tracks over eight seconds, whose lanes are 400 px wide, so that a second is 50 px: block 1 from 1 s
// to 3 s and block 2 from 4 s to 5 s on the first track, and block 3 from 2 s to 4 s on the second.
struct TimelineScene {
    KitFixture& f;
    Phases phases;
    PrKitTimelineTrack tracks[2] = {{"Position"}, {"Rotation"}};
    std::vector<PrKitTimelineBlock> blocks = {
        {.id = 1, .track = 0, .start = 1, .duration = 2, .color = {0.3f, 0.5f, 0.8f, 1}, .label = "Walk"},
        {.id = 2, .track = 0, .start = 4, .duration = 1, .color = {0.3f, 0.5f, 0.8f, 1}, .label = "Run"},
        {.id = 3, .track = 1, .start = 2, .duration = 2, .color = {0.8f, 0.5f, 0.3f, 1}, .label = "Turn"},
    };
    std::vector<PrKitTimelineMarker> markers;
    PrKitTimeSpan view{0, 8};
    float playhead = 0;
    PrKitTimelineSnap snap{.step = 0.25f};
    PrLayoutNode frame = kPrLayoutNodeNone;
    bool added = false;
    float above = 0;                // px of content above the timeline, in a scroll container, or zero for none
    PrLayoutNode scroller = kPrLayoutNodeNone;
    uint32_t removals = 0;
    uint32_t additions = 0;

    explicit TimelineScene(KitFixture& fixture) : f(fixture) {
        f.edit = Phases::record;
        f.editContext = &phases;
        f.build = [this](PrLayoutNode root) {
            PrKitTimeline timeline{
                .tracks = tracks,
                .trackCount = 2,
                .blocks = blocks.data(),
                .blockCount = uint32_t(blocks.size()),
                .markers = markers.data(),
                .markerCount = uint32_t(markers.size()),
                .view = &view,
                .playhead = &playhead,
                .snap = snap,
            };
            PrLayoutNode parent = root;
            if (above > 0) {
                PrKitBoxStyle box{.overflow = PrKitOverflow_ScrollY};
                box.place = {.height = prKitPx(300), .flex = PrKitFlex_Fixed};
                scroller = prKitBox(f.kit, root, {}, box);
                PrKitBoxStyle spacer{};
                spacer.place = {.height = prKitPx(above), .flex = PrKitFlex_Fixed};
                prKitBox(f.kit, scroller, {}, spacer);
                parent = scroller;
            }
            frame = prKitTimeline(f.kit, parent, timeline,
                {.place = {.width = prKitPx(kGutter + 400), .flex = PrKitFlex_Fixed}});
            removals += prKitRemoved(f.kit, frame) ? 1 : 0;
            uint32_t track = 0;
            float time = 0;
            added = prKitAdded(f.kit, frame, &track, &time);
            additions += added ? 1 : 0;
        };
    }
    // Returns the point at `time` seconds in the middle of `track`, or of the ruler for track -1.
    PrVec2 at(float time, int32_t track) {
        PrRect box = f.bounds(frame);
        float y = track < 0 ? kRuler / 2 : kRuler + kTrack * float(track) + kTrack / 2;
        return PrVec2{box.left + kGutter + time * 50, box.top + y};
    }

    void drag(PrVec2 from, PrVec2 to, PrAppModifierFlags modifiers = 0) {
        f.push(PrAppEvent{.kind = PrAppEventKind_PointerDown, .modifiers = modifiers, .position = from});
        f.frame();
        f.push(PrAppEvent{.kind = PrAppEventKind_PointerMove, .modifiers = modifiers, .position = to});
        f.frame();
        f.push(PrAppEvent{.kind = PrAppEventKind_PointerUp, .modifiers = modifiers, .position = to});
        f.frame();
    }

    // Returns the number of nodes under `node`, counting it.
    uint32_t nodes(PrLayoutNode node) const {
        uint32_t total = 1;
        for (PrLayoutNode c = prLayoutNodeFirstChild(f.tree, node); c != kPrLayoutNodeNone;
                c = prLayoutNodeNextSibling(f.tree, c))
            total += nodes(c);
        return total;
    }

    uint32_t count(PrUiEditPhase phase) const {
        uint32_t n = 0;
        for (PrUiEditPhase p : phases.seen)
            n += p == phase ? 1 : 0;
        return n;
    }
};

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a drag on a block moves it and the other selected blocks, snapped to the step, as one gesture") {
    KitFixture f;
    TimelineScene scene(f);
    f.frame();
    f.frame();
    f.click(scene.at(4.5f, 0), PrAppModifierFlag_Shift);
    f.frame();
    scene.drag(scene.at(2, 0), scene.at(2 + 1.1f, 0), PrAppModifierFlag_Shift);
    // The press joined block 1 to the selection, and the drag moved both by 1.1 s, which the step rounds to 1 s.
    CHECK(scene.blocks[0].selected);
    CHECK(scene.blocks[1].selected);
    CHECK(scene.blocks[0].start == doctest::Approx(2.0));
    CHECK(scene.blocks[1].start == doctest::Approx(5.0));
    CHECK(scene.blocks[2].start == 2.0f);
    CHECK(scene.count(PrUiEditPhase_Begin) == 1);
    CHECK(scene.count(PrUiEditPhase_Commit) == 1);
    CHECK(kitVerified(f.tree));
}

TEST_CASE("a drag on a block's end resizes it, and a press on another block selects it alone") {
    KitFixture f;
    TimelineScene scene(f);
    f.frame();
    f.frame();
    scene.drag(scene.at(3 - 0.04f, 0), scene.at(3.5f, 0));
    CHECK(scene.blocks[0].start == 1.0f);
    CHECK(scene.blocks[0].duration == doctest::Approx(2.5));
    scene.drag(scene.at(1 + 0.04f, 0), scene.at(0.5f, 0));
    CHECK(scene.blocks[0].start == doctest::Approx(0.5));
    CHECK(scene.blocks[0].duration == doctest::Approx(3.0));
    f.click(scene.at(3, 1));
    f.frame();
    CHECK(scene.blocks[2].selected);
    CHECK_FALSE(scene.blocks[0].selected);
    CHECK(kitVerified(f.tree));
}

TEST_CASE("a drag on the ruler scrubs the playhead, and a marquee over the lanes selects what it touches") {
    KitFixture f;
    TimelineScene scene(f);
    f.frame();
    f.frame();
    scene.drag(scene.at(1, -1), scene.at(6.1f, -1));
    CHECK(scene.playhead == doctest::Approx(6.0));
    // From 3.5 s on the first track to 6.5 s on the second touches blocks 2 and 3.
    scene.drag(scene.at(3.5f, 0), scene.at(6.5f, 1));
    CHECK_FALSE(scene.blocks[0].selected);
    CHECK(scene.blocks[1].selected);
    CHECK(scene.blocks[2].selected);
    // A click on an empty part of a lane clears the selection.
    f.click(scene.at(7.5f, 1));
    f.frame();
    CHECK_FALSE(scene.blocks[1].selected);
    CHECK_FALSE(scene.blocks[2].selected);
    CHECK(kitVerified(f.tree));
}

TEST_CASE("Delete asks to remove the selection, and a double press on an empty lane asks for a block there") {
    KitFixture f;
    TimelineScene scene(f);
    f.frame();
    f.frame();
    f.click(scene.at(2, 0));
    f.frame();
    f.key(PrAppKey_Delete);
    f.frame();
    CHECK(scene.removals == 1);
    f.click(scene.at(6, 1));
    f.click(scene.at(6, 1), 0, 2);
    f.frame();
    CHECK(scene.additions == 1);
    CHECK(scene.added == false);
    CHECK(kitVerified(f.tree));
}

TEST_CASE("the wheel pans a timeline's view, and with Command zooms it about the pointer") {
    KitFixture f;
    TimelineScene scene(f);
    f.frame();
    f.frame();
    PrVec2 over = scene.at(4, 0);
    f.move(over);
    f.push(PrAppEvent{.kind = PrAppEventKind_Scroll, .position = over, .delta = PrVec2{50, 0}});
    f.frame();
    CHECK(scene.view.start == doctest::Approx(1.0));
    CHECK(scene.view.end == doctest::Approx(9.0));
    f.push(PrAppEvent{
        .kind = PrAppEventKind_Scroll,
        .modifiers = PrAppModifierFlag_Super,
        .position = over,
        .delta = PrVec2{0, -70},
    });
    f.frame();
    // The time under the pointer stays where it was, 5 s, and the view spans half as much.
    float span = scene.view.end - scene.view.start;
    CHECK(span == doctest::Approx(8.0 * 0.4966).epsilon(0.01));
    CHECK(scene.view.start + span / 2 == doctest::Approx(5.0).epsilon(0.01));
    CHECK(kitVerified(f.tree));
}

TEST_CASE("a timeline scrolled into view in its container takes drags where it is drawn") {
    KitFixture f;
    TimelineScene scene(f);
    scene.above = 1000;
    f.frame();
    f.frame();
    prLayoutNodeSetScroll(f.tree, scene.scroller, PrLayoutVec{prLayoutUnitFromPx(0), prLayoutUnitFromPx(950)});
    f.frame();
    f.frame();
    scene.drag(scene.at(2, 0), scene.at(3, 0));
    CHECK(scene.blocks[0].selected);
    CHECK(scene.blocks[0].start == doctest::Approx(2.0));
    CHECK(kitVerified(f.tree));
}

TEST_CASE("a drag snaps to the edges of other blocks, to markers and to the playhead, each only while it is enabled") {
    struct Case {
        PrKitTimelineSnap snap;
        float expected;
    };
    // Block 2's start dragged from 4 s to 3.08 s, near block 1's end at 3 s.
    for (Case c : {Case{{.edges = true}, 3.0f}, Case{{}, 3.08f}}) {
        KitFixture f;
        TimelineScene scene(f);
        scene.snap = c.snap;
        f.frame();
        f.frame();
        scene.drag(scene.at(4.5f, 0), scene.at(4.5f - 0.92f, 0));
        CHECK(scene.blocks[1].start == doctest::Approx(c.expected).epsilon(0.002));
    }
    // Block 2's end dragged from 5 s to 6.44 s, near a marker at 6.5 s.
    for (Case c : {Case{{.markers = true}, 6.5f}, Case{{}, 6.44f}}) {
        KitFixture f;
        TimelineScene scene(f);
        scene.snap = c.snap;
        scene.markers = {{.track = 1, .time = 6.5f, .color = {1, 0, 0, 1}}};
        f.frame();
        f.frame();
        scene.drag(scene.at(5 - 0.04f, 0), scene.at(6.44f, 0));
        CHECK(scene.blocks[1].start + scene.blocks[1].duration == doctest::Approx(c.expected).epsilon(0.002));
    }
    // Block 1's end dragged from 3 s to 2.5 s, near the playhead at 2.55 s.
    for (Case c : {Case{{.playhead = true}, 2.55f}, Case{{}, 2.5f}}) {
        KitFixture f;
        TimelineScene scene(f);
        scene.snap = c.snap;
        scene.playhead = 2.55f;
        f.frame();
        f.frame();
        scene.drag(scene.at(3 - 0.04f, 0), scene.at(2.5f, 0));
        CHECK(scene.blocks[0].start + scene.blocks[0].duration == doctest::Approx(c.expected).epsilon(0.002));
    }
}

TEST_CASE("a drag that snaps to a source shows a guide at its time, until the drag ends") {
    KitFixture f;
    TimelineScene scene(f);
    scene.snap = PrKitTimelineSnap{.edges = true};
    f.frame();
    f.frame();
    PrVec2 from = scene.at(4.5f, 0);
    PrVec2 to = scene.at(4.5f - 0.92f, 0);
    f.push(PrAppEvent{.kind = PrAppEventKind_PointerDown, .position = from});
    f.frame();
    uint32_t still = scene.nodes(scene.frame);
    f.push(PrAppEvent{.kind = PrAppEventKind_PointerMove, .position = to});
    f.frame();
    // The guide is a line in each of the two lanes and in the ruler.
    CHECK(scene.blocks[1].start == doctest::Approx(3.0));
    CHECK(scene.nodes(scene.frame) == still + 3);
    f.push(PrAppEvent{.kind = PrAppEventKind_PointerUp, .position = to});
    f.frame();
    CHECK(scene.nodes(scene.frame) == still);
    CHECK(kitVerified(f.tree));
}
