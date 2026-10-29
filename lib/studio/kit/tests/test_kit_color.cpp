// Studio Kit's colors.

#include "pr/color_space.h"
#include "pr/kit_box.h"
#include "pr/kit_color.h"

#include "tools/harness/kit_fixture.h"

#include "doctest/doctest.h"

#include <string>
#include <vector>

using namespace harness;

namespace pr_impl {

namespace {

// The phases that an edit sink saw, in order.
struct Edits {
    std::vector<PrUiEditPhase> phases;

    static void record(void* context, PrLayoutNode, PrUiEditPhase phase) {
        static_cast<Edits*>(context)->phases.push_back(phase);
    }
    uint32_t count(PrUiEditPhase phase) const {
        uint32_t n = 0;
        for (PrUiEditPhase p : phases)
            n += p == phase ? 1 : 0;
        return n;
    }
};

// A color picker over a color, which reports its gestures to `edits`.
struct PickerScene {
    KitFixture& f;
    Edits edits;
    PrColor color{0.5f, 0.5f, 0.5f, 1};
    PrKitColorMode mode = PrKitColorMode_Hsv;
    bool opaque = false;
    PrLayoutNode picker = kPrLayoutNodeNone;

    explicit PickerScene(KitFixture& fixture) : f(fixture) {
        f.edit = Edits::record;
        f.editContext = &edits;
        f.build = [this](PrLayoutNode ground) {
            PrKitColorPicker content{
                .color = &color, .mode = mode, .gamut = prUiDisplay(f.ui).gamut, .opaque = opaque};
            picker = prKitColorPicker(f.kit, ground, content);
        };
    }
    // Returns child `index` of `node`.
    PrLayoutNode child(PrLayoutNode node, uint32_t index) const {
        PrLayoutNode at = prLayoutNodeFirstChild(f.tree, node);
        for (uint32_t i = 0; i < index; ++i)
            at = prLayoutNodeNextSibling(f.tree, at);
        return at;
    }

    // The picker's rows: its tabs, then its plane and strips, its coordinates or the ramp's caption, its hex value, and
    // its channels.
    PrLayoutNode tabs() const { return child(picker, 0); }
    PrLayoutNode plane() const { return child(child(picker, 1), 0); }
    PrLayoutNode hues() const { return child(child(picker, 1), 1); }
    PrLayoutNode alpha() const { return child(child(picker, 1), 2); }

    // Returns the point at fractions `x` and `y` of a node's box.
    PrVec2 at(PrLayoutNode node, float x, float y) {
        PrRect box = f.bounds(node);
        return PrVec2{box.left + (box.right - box.left) * x, box.top + (box.bottom - box.top) * y};
    }

    void drag(PrVec2 from, PrVec2 to) {
        f.push(PrAppEvent{.kind = PrAppEventKind_PointerDown, .position = from});
        f.frame();
        f.push(PrAppEvent{.kind = PrAppEventKind_PointerMove, .position = PrVec2{(from.x + to.x) / 2, from.y}});
        f.frame();
        f.push(PrAppEvent{.kind = PrAppEventKind_PointerMove, .position = to});
        f.frame();
        f.push(PrAppEvent{.kind = PrAppEventKind_PointerUp, .position = to});
        f.frame();
    }

    // Clicks the tab of `m`.
    void choose(PrKitColorMode m) {
        f.click(at(tabs(), (float(m) + 0.5f) / 3, 0.5f));
        f.frame();
    }
};

// Returns the OKLCH lightness of `color`.
float lightnessOf(PrColor color) {
    double in[3] = {double(color.r), double(color.g), double(color.b)};
    double out[3] = {};
    prColorConvert(PrColorSpace_Srgb, PrColorSpace_Oklch, in, out);
    return float(out[0]);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a color field shows its color's hex, and a click opens its picker and closes it") {
    KitFixture f;
    PrColor color{1, 0.5f, 0, 1};
    PrLayoutNode field = kPrLayoutNodeNone;
    f.build = [&](PrLayoutNode ground) {
        PrLayoutNode row = prKitBox(f.kit, ground, {}, {.direction = PrKitDirection_Row, .gap = 0});
        field = prKitColorField(f.kit, row, {.color = &color}, {});
    };
    f.frame();
    CHECK(f.bounds(field).right - f.bounds(field).left > 40.0f);
    uint32_t closed = f.paintedCommands();
    f.click(f.center(field));
    f.frame();
    CHECK(f.paintedCommands() > closed + 10);
    f.click(f.center(field));
    f.frame();
    CHECK(f.paintedCommands() == closed);
    CHECK(kitVerified(f.tree));
}

TEST_CASE("a drag on a color picker's HSV plane sets saturation across and value up, as one gesture") {
    KitFixture f;
    PickerScene scene(f);
    scene.color = PrColor{1, 0, 0, 1};
    f.frame();
    scene.drag(scene.at(scene.plane(), 0.3f, 0.5f), scene.at(scene.plane(), 0.5f, 0.5f));
    CHECK(scene.edits.count(PrUiEditPhase_Begin) == 1);
    CHECK(scene.edits.count(PrUiEditPhase_Commit) == 1);
    CHECK(scene.edits.count(PrUiEditPhase_Change) >= 2);
    // Value is a half and saturation a half, at red's hue.
    CHECK(scene.color.r == doctest::Approx(0.5).epsilon(0.03));
    CHECK(scene.color.g == doctest::Approx(scene.color.b));
    CHECK(scene.color.g == doctest::Approx(0.25).epsilon(0.05));
    CHECK(scene.color.a == 1.0f);
    CHECK(kitVerified(f.tree));
}

TEST_CASE("a color picker keeps its hue through a gray, and its alpha strip fades the color from the top down") {
    KitFixture f;
    PickerScene scene(f);
    f.frame();
    // A gray has no hue of its own, so choosing one leaves the color, and the plane then saturates at that hue.
    f.click(scene.at(scene.hues(), 0.5f, 1.0f / 3));
    f.frame();
    CHECK(scene.color.r == 0.5f);
    CHECK(scene.color.g == 0.5f);
    f.click(scene.at(scene.plane(), 0.99f, 0.01f));
    f.frame();
    CHECK(scene.color.g == doctest::Approx(1.0).epsilon(0.02));
    CHECK(scene.color.r < 0.1f);
    CHECK(scene.color.b < 0.1f);
    f.click(scene.at(scene.alpha(), 0.5f, 0.75f));
    f.frame();
    CHECK(scene.color.a == doctest::Approx(0.25).epsilon(0.03));
    CHECK(scene.color.g == doctest::Approx(1.0).epsilon(0.02));
    CHECK(kitVerified(f.tree));
}

TEST_CASE("the OKLCH plane sets perceptual lightness across, and keeps a color that sRGB cannot hold in its gamut") {
    KitFixture f;
    PickerScene scene(f);
    scene.color = PrColor{0.2f, 0.4f, 0.8f, 1};
    f.frame();
    scene.choose(PrKitColorMode_Oklch);
    // Lightness 0.7 at the most chroma, past sRGB: the color is mapped into sRGB, the display's gamut.
    f.click(scene.at(scene.plane(), 0.7f, 0.01f));
    f.frame();
    CHECK(lightnessOf(scene.color) == doctest::Approx(0.7).epsilon(0.02));
    for (float channel : {scene.color.r, scene.color.g, scene.color.b}) {
        CHECK(channel >= -1e-3f);
        CHECK(channel <= 1 + 1e-3f);
    }
    CHECK(scene.color.b > scene.color.r);
    CHECK(kitVerified(f.tree));
}

TEST_CASE("on a P3 display, the OKLCH plane picks a color past sRGB that P3 holds") {
    KitFixture f;
    f.display.gamut = PrColorSpace_DisplayP3;
    PickerScene scene(f);
    scene.color = PrColor{0.2f, 0.4f, 0.8f, 1};
    f.frame();
    scene.choose(PrKitColorMode_Oklch);
    f.click(scene.at(scene.plane(), 0.7f, 0.01f));
    f.frame();
    CHECK(lightnessOf(scene.color) == doctest::Approx(0.7).epsilon(0.02));
    bool pastSrgb = false;
    for (float channel : {scene.color.r, scene.color.g, scene.color.b})
        pastSrgb = pastSrgb || channel < -1e-3f || channel > 1 + 1e-3f;
    CHECK(pastSrgb);
    const double srgb[3] = {double(scene.color.r), double(scene.color.g), double(scene.color.b)};
    double p3[3];
    prColorConvert(PrColorSpace_Srgb, PrColorSpace_DisplayP3, srgb, p3);
    for (double channel : p3) {
        CHECK(channel >= -1e-3);
        CHECK(channel <= 1 + 1e-3);
    }
    CHECK(kitVerified(f.tree));
}

TEST_CASE("the ramp's plane sets its tones without the color, and a click on a tone sets the color, keeping alpha") {
    KitFixture f;
    PickerScene scene(f);
    scene.color = PrColor{0.8f, 0.3f, 0.2f, 0.5f};
    scene.mode = PrKitColorMode_Ramp;
    f.frame();
    PrColor before = scene.color;
    scene.drag(scene.at(scene.plane(), 0.2f, 0.5f), scene.at(scene.plane(), 0.6f, 0.4f));
    CHECK(scene.color.r == before.r);
    CHECK(scene.color.g == before.g);
    // The tones are the children of the row under the ramp's caption.
    PrLayoutNode tones = scene.child(scene.picker, 3);
    PrLayoutNode light = scene.child(tones, 0);
    PrLayoutNode dark = scene.child(tones, 10);
    f.click(f.center(light));
    f.frame();
    CHECK(scene.color.a == 0.5f);
    float lightL = lightnessOf(scene.color);
    f.click(f.center(dark));
    f.frame();
    CHECK(lightnessOf(scene.color) < lightL - 0.5f);
    CHECK(scene.edits.count(PrUiEditPhase_Commit) == 2);
    CHECK(kitVerified(f.tree));
}

TEST_CASE("a color picker reads a color that its caller changes, and an opaque picker has no alpha") {
    KitFixture f;
    PickerScene scene(f);
    scene.opaque = true;
    f.frame();
    scene.color = PrColor{0, 0, 1, 1};
    f.frame();
    // The click lands on the plane near blue's own saturation and value, which keeps its hue.
    f.click(scene.at(scene.plane(), 0.99f, 0.01f));
    f.frame();
    CHECK(scene.color.b == doctest::Approx(1.0).epsilon(0.02));
    CHECK(scene.color.r < 0.02f);
    uint32_t strips = 0;
    for (PrLayoutNode n = prLayoutNodeFirstChild(f.tree, scene.child(scene.picker, 1)); n != kPrLayoutNodeNone;
         n = prLayoutNodeNextSibling(f.tree, n))
        ++strips;
    CHECK(strips == 2);
    CHECK(kitVerified(f.tree));
}

TEST_CASE("hex text round-trips a color through its channels") {
    char hex[9];
    PrStringView opaque = prKitColorHex(PrColor{1, 0.5f, 0, 1}, hex);
    CHECK(std::string(opaque.data, opaque.size) == "#FF8000");
    PrStringView clear = prKitColorHex(PrColor{0, 0, 1, 0.5f}, hex);
    CHECK(std::string(clear.data, clear.size) == "#0000FF80");
    PrColor parsed{};
    CHECK(prKitColorParseHex("ff8000", &parsed));
    CHECK(parsed.g == doctest::Approx(128.0 / 255));
    CHECK(parsed.a == 1.0f);
    PrColor kept{0.25f, 0.25f, 0.25f, 1};
    CHECK_FALSE(prKitColorParseHex("#12345", &kept));
    CHECK_FALSE(prKitColorParseHex("#12345g", &kept));
    CHECK(kept.r == 0.25f);
}
