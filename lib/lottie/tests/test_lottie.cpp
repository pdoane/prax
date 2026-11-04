#include "pr/draw_commands.h"
#include "pr/font_set.h"
#include "pr/lottie.h"
#include "pr/path.h"
#include "pr/text_kb.h"
#include "pr/vec.h"

#include "lottie/src/lottie_eval.h"
#include "lottie/src/lottie_model.h"
#include "lottie/src/lottie_parse.h"

#include "tools/harness/test.h"

#include <string>
#include <utility>
#include <vector>

extern "C" int prTestLottieCApi(void);

namespace pr_impl {

namespace {

PrLottieAnimation* read(const std::string& json) {
    PrLottieAnimation* animation = nullptr;
    REQUIRE(prLottieAnimationCreate(json, nullptr, &animation) == PrStatus_Ok);
    return animation;
}

// Returns a 100 by 100 animation with one shape layer that holds `shapes`, from frame 0 to 60.
std::string shapeLayer(const std::string& shapes, const std::string& ks = "{}") {
    return R"({"w": 100, "h": 100, "fr": 30, "ip": 0, "op": 60, "layers": [{"ty": 4, "ip": 0, "op": 60, "st": 0, "ks": )" +
        ks + R"(, "shapes": [)" + shapes + "]}]}";
}

// Returns a 100 by 100 animation with a 3D solid square, 40 on a side, centered, and a camera with `zoom` at `camera`,
// facing the composition, if `camera` is not empty.
std::string cameraScene(const std::string& camera, float zoom) {
    std::string layers =
        R"({"ty": 1, "ddd": 1, "ip": 0, "op": 60, "st": 0, "sw": 40, "sh": 40, "sc": "#ff0000",
            "ks": {"a": {"k": [20, 20, 0]}, "p": {"k": [50, 50, 0]}}})";
    if (!camera.empty())
        layers = R"({"ty": 13, "ip": 0, "op": 60, "st": 0, "pe": {"k": )" + std::to_string(zoom) +
            R"(}, "ks": {"p": {"k": )" + camera + "}}}, " + layers;
    return R"({"w": 100, "h": 100, "fr": 30, "ip": 0, "op": 60, "layers": [)" + layers + "]}";
}

// Returns the commands drawn for a frame.
std::vector<PrDrawCommand> render(PrLottieAnimation* animation, double frame, PrDrawList* list) {
    prDrawListClear(list);
    prLottieAnimationDraw(animation, frame, list);
    std::vector<PrDrawCommand> commands;
    for (uint32_t i = 0; i < prDrawListCommandCount(list); ++i)
        commands.push_back(prDrawListCommands(list)[i]);
    return commands;
}

std::vector<PrDrawCommand> painted(const std::vector<PrDrawCommand>& commands) {
    std::vector<PrDrawCommand> out;
    for (const PrDrawCommand& c : commands) {
        if (c.kind == PrDrawCommandKind_Path)
            out.push_back(c);
    }
    return out;
}

// Returns a number property's value at `frame`, read from the property's JSON.
float valueAt(const std::string& property, double frame) {
    LottieAnimation a{};
    std::string json = R"({"w": 1, "h": 1, "layers": [{"ty": 3, "ks": {"o": )" + property + "}}]}";
    REQUIRE(lottieParse(json, PrLottieOptions{}, a) == PrStatus_Ok);
    uint32_t opacity = a.transforms.data[a.layers.data[0].transform].opacity;
    float value = lottieScalar(a, opacity, frame, -1);
    lottieReset(a);
    return value;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("the C API reads, describes and draws") {
    CHECK(prTestLottieCApi() == 0);
}

TEST_CASE("text that is not JSON, or JSON that is not an animation, is refused") {
    PrLottieAnimation* animation = nullptr;
    CHECK(prLottieAnimationCreate("{", nullptr, &animation) == PrStatus_Syntax);
    CHECK(animation == nullptr);
    CHECK(prLottieAnimationCreate("{\"w\": 1}", nullptr, &animation) == PrStatus_InvalidData);
    CHECK(prLottieAnimationCreate("[]", nullptr, &animation) == PrStatus_InvalidData);
}

TEST_CASE("under a camera, a 3D layer is a projected group of its plane, and without one it draws flat") {
    PrDrawList* list = prDrawListCreate();
    PrLottieAnimation* flat = read(cameraScene("", 0));
    std::vector<PrDrawCommand> commands = render(flat, 0, list);
    for (const PrDrawCommand& c : commands)
        CHECK(!(c.kind == PrDrawCommandKind_PushGroup && c.group->projection != nullptr));
    // A camera at its default place, 200 in front of the center, leaves the plane where it was.
    PrLottieAnimation* centered = read(cameraScene("[50, 50, -200]", 200));
    commands = render(centered, 0, list);
    const PrDrawGroup* projected = nullptr;
    for (const PrDrawCommand& c : commands) {
        if (c.kind == PrDrawCommandKind_PushGroup && c.group->projection != nullptr)
            projected = c.group;
    }
    REQUIRE(projected != nullptr);
    // The group's bounds hold the square, in the layer's plane.
    CHECK(projected->bounds.left <= 0.0f);
    CHECK(projected->bounds.right >= 40.0f);
    PrVec2 corner{};
    REQUIRE(prMatrix4MapPoint(projected->projection, PrVec2{0, 0}, &corner));
    CHECK(corner.x == doctest::Approx(30).epsilon(1e-4));
    CHECK(corner.y == doctest::Approx(30).epsilon(1e-4));
    // Moved 100 toward the plane, the camera sees it twice as large about the center.
    PrLottieAnimation* near = read(cameraScene("[50, 50, -100]", 200));
    commands = render(near, 0, list);
    for (const PrDrawCommand& c : commands) {
        if (c.kind == PrDrawCommandKind_PushGroup && c.group->projection != nullptr)
            projected = c.group;
    }
    REQUIRE(prMatrix4MapPoint(projected->projection, PrVec2{0, 0}, &corner));
    CHECK(corner.x == doctest::Approx(10).epsilon(1e-4));
    CHECK(corner.y == doctest::Approx(10).epsilon(1e-4));
    prLottieAnimationDestroy(near);
    prLottieAnimationDestroy(centered);
    prLottieAnimationDestroy(flat);
    prDrawListRelease(list);
}

TEST_CASE("keyframes ease between their values and hold outside them") {
    std::string linear = R"({"a": 1, "k": [{"t": 10, "s": [0], "o": {"x": [0], "y": [0]}, "i": {"x": [1], "y": [1]}},
                                           {"t": 20, "s": [100]}]})";
    CHECK(valueAt(linear, 0) == 0.0f);
    CHECK(valueAt(linear, 15) == doctest::Approx(50));
    CHECK(valueAt(linear, 20) == 100.0f);
    CHECK(valueAt(linear, 99) == 100.0f);
    // The older form, in which a keyframe specifies its end value.
    std::string ending = R"({"a": 1, "k": [{"t": 0, "s": [10], "e": [30], "o": {"x": 0, "y": 0}, "i": {"x": 1, "y": 1}},
                                           {"t": 10}]})";
    CHECK(valueAt(ending, 5) == doctest::Approx(20));
    CHECK(valueAt(ending, 10) == 30.0f);
    std::string hold = R"({"a": 1, "k": [{"t": 0, "s": [10], "h": 1}, {"t": 10, "s": [30]}]})";
    CHECK(valueAt(hold, 9.9) == 10.0f);
    CHECK(valueAt(hold, 10) == 30.0f);
    // Ease in and out: slow at the ends, even at the middle.
    std::string eased = R"({"a": 1, "k": [{"t": 0, "s": [0], "o": {"x": [0.5], "y": [0]}, "i": {"x": [0.5], "y": [1]}},
                                          {"t": 10, "s": [100]}]})";
    CHECK(valueAt(eased, 5) == doctest::Approx(50));
    CHECK(valueAt(eased, 1) < 10.0f);
    CHECK(valueAt(eased, 9) > 90.0f);
}

TEST_CASE("easing is CSS's cubic bezier") {
    CHECK(lottieEase(0.25, 0.1, 0.25, 1, 0.5) == doctest::Approx(0.8024).epsilon(0.001));
    CHECK(lottieEase(0.42, 0, 1, 1, 0.5) == doctest::Approx(0.3153).epsilon(0.001));
    CHECK(lottieEase(0.3, 0.3, 0.7, 0.7, 0.2) == doctest::Approx(0.2));
}

TEST_CASE("a position moves along its spatial tangents") {
    LottieAnimation a{};
    std::string json = R"({"w": 1, "h": 1, "layers": [{"ty": 3, "ks": {"p": {"a": 1, "k": [
        {"t": 0, "s": [0, 0], "to": [0, -40], "ti": [0, -40], "o": {"x": 0, "y": 0}, "i": {"x": 1, "y": 1}},
        {"t": 10, "s": [100, 0]}]}}}]})";
    REQUIRE(lottieParse(json, PrLottieOptions{}, a) == PrStatus_Ok);
    const LottieTransform& t = a.transforms.data[a.layers.data[0].transform];
    PrVec2 middle = lottieTransformPosition(a, t, 5);
    CHECK(middle.x == doctest::Approx(50).epsilon(0.01));
    CHECK(middle.y == doctest::Approx(-30).epsilon(0.01));
    lottieReset(a);
}

TEST_CASE("a transform moves the anchor to the origin, scales, rotates and moves to the position") {
    LottieAnimation a{};
    std::string json = R"({"w": 1, "h": 1, "layers": [{"ty": 3, "ks": {"a": {"k": [10, 0]}, "p": {"k": [50, 50]},
        "s": {"k": [200, 200]}, "r": {"k": 90}}}]})";
    REQUIRE(lottieParse(json, PrLottieOptions{}, a) == PrStatus_Ok);
    PrAffine2 m = lottieTransformAffine(a, a.transforms.data[a.layers.data[0].transform], 0);
    PrVec2 p = prAffine2MapPoint(m, PrVec2{20, 0});
    // (20, 0) minus the anchor is (10, 0), doubled is (20, 0), rotated clockwise is (0, 20).
    CHECK(p.x == doctest::Approx(50));
    CHECK(p.y == doctest::Approx(70));
    lottieReset(a);
}

TEST_CASE("a fill paints the shapes before it, and a stroke after it paints first") {
    PrLottieAnimation* animation = read(shapeLayer(R"(
        {"ty": "rc", "p": {"k": [50, 50]}, "s": {"k": [20, 10]}, "r": {"k": 0}},
        {"ty": "st", "c": {"k": [0, 0, 1, 1]}, "o": {"k": 100}, "w": {"k": 2}},
        {"ty": "el", "p": {"k": [0, 0]}, "s": {"k": [4, 4]}},
        {"ty": "fl", "c": {"k": [1, 0, 0, 1]}, "o": {"k": 50}})"));
    PrDrawList* list = prDrawListCreate();
    std::vector<PrDrawCommand> paths = painted(render(animation, 0, list));
    REQUIRE(paths.size() == 2);
    // The fill is last in the list, so it is found first and drawn first, and it applies to both shapes.
    CHECK(paths[0].stroke == nullptr);
    CHECK(prColorFromColor16(paths[0].color).r == 1.0f);
    CHECK(prColorFromColor16(paths[0].color).a == doctest::Approx(0.5));
    PrRect filled = prPathBounds(paths[0].path);
    CHECK(filled.left == doctest::Approx(-2));
    CHECK(filled.right == doctest::Approx(60));
    // The stroke applies only to the rectangle before it.
    REQUIRE(paths[1].stroke != nullptr);
    CHECK(paths[1].stroke->shape.width == 2.0f);
    PrRect stroked = prPathBounds(paths[1].path);
    CHECK(stroked.left == doctest::Approx(40));
    CHECK(stroked.top == doctest::Approx(45));
    prDrawListRelease(list);
    prLottieAnimationDestroy(animation);
}

TEST_CASE("a group's shapes reach its parent's styles through its transform") {
    PrLottieAnimation* animation = read(shapeLayer(R"(
        {"ty": "gr", "it": [
            {"ty": "rc", "p": {"k": [0, 0]}, "s": {"k": [10, 10]}, "r": {"k": 0}},
            {"ty": "tr", "p": {"k": [30, 0]}, "o": {"k": 100}}]},
        {"ty": "fl", "c": {"k": [0, 1, 0, 1]}, "o": {"k": 100}})"));
    PrDrawList* list = prDrawListCreate();
    std::vector<PrDrawCommand> paths = painted(render(animation, 0, list));
    REQUIRE(paths.size() == 1);
    PrRect bounds = prPathBounds(paths[0].path);
    CHECK(bounds.left == doctest::Approx(25));
    CHECK(bounds.right == doctest::Approx(35));
    prDrawListRelease(list);
    prLottieAnimationDestroy(animation);
}

TEST_CASE("a layer draws only from its in point up to its out point") {
    PrLottieAnimation* animation = read(R"({"w": 10, "h": 10, "fr": 30, "ip": 0, "op": 60, "layers": [
        {"ty": 1, "ip": 10, "op": 20, "st": 0, "sw": 10, "sh": 10, "sc": "#ff0000", "ks": {}}]})");
    PrDrawList* list = prDrawListCreate();
    auto rects = [&](double frame) {
        uint32_t count = 0;
        for (const PrDrawCommand& c : render(animation, frame, list))
            count += c.kind == PrDrawCommandKind_Rect;
        return count;
    };
    CHECK(rects(5) == 0);
    CHECK(rects(10) == 1);
    CHECK(rects(19.5) == 1);
    CHECK(rects(20) == 0);
    prDrawListRelease(list);
    prLottieAnimationDestroy(animation);
}

TEST_CASE("a precomp plays its composition at its own time, clipped to its size") {
    PrLottieAnimation* animation = read(R"({"w": 100, "h": 100, "fr": 30, "ip": 0, "op": 60,
        "assets": [{"id": "inner", "w": 10, "h": 10, "layers": [
            {"ty": 1, "ip": 0, "op": 5, "st": 0, "sw": 10, "sh": 10, "sc": "#00ff00", "ks": {}}]}],
        "layers": [{"ty": 0, "refId": "inner", "w": 10, "h": 10, "ip": 0, "op": 60, "st": 20, "ks": {}}]})");
    PrDrawList* list = prDrawListCreate();
    auto solids = [&](double frame) {
        uint32_t count = 0;
        for (const PrDrawCommand& c : render(animation, frame, list))
            count += c.kind == PrDrawCommandKind_Rect;
        return count;
    };
    // The layer starts its composition at frame 20, and the composition's solid lasts five frames.
    CHECK(solids(10) == 0);
    CHECK(solids(22) == 1);
    CHECK(solids(26) == 0);
    prDrawListRelease(list);
    prLottieAnimationDestroy(animation);
}

TEST_CASE("a trim that wraps past the end of several contours joins none of them to another") {
    // Two separate squares, merged into one shape of two contours. The trim covers them whole, except for an offset
    // that moves the trim past the end and on from the start.
    PrLottieAnimation* animation = read(shapeLayer(R"(
        {"ty": "rc", "p": {"k": [15, 50]}, "s": {"k": [10, 10]}, "r": {"k": 0}},
        {"ty": "rc", "p": {"k": [75, 50]}, "s": {"k": [10, 10]}, "r": {"k": 0}},
        {"ty": "mm", "mm": 1},
        {"ty": "tm", "s": {"k": 0}, "e": {"k": 100}, "o": {"k": 36}, "m": 1},
        {"ty": "st", "c": {"k": [0, 0, 0, 1]}, "o": {"k": 100}, "w": {"k": 1}})"));
    PrDrawList* list = prDrawListCreate();
    std::vector<PrDrawCommand> paths = painted(render(animation, 0, list));
    REQUIRE(paths.size() == 1);
    // Every segment stays on its own square's side of the gap between them.
    const PrPathVerb* verbs = prPathVerbs(paths[0].path);
    const PrVec2* points = prPathPoints(paths[0].path);
    uint32_t at = 0;
    PrVec2 last{0, 0};
    uint32_t crossings = 0;
    for (uint32_t i = 0; i < prPathVerbCount(paths[0].path); ++i) {
        uint32_t count = 0;
        if (verbs[i] == PrPathVerb_Move || verbs[i] == PrPathVerb_Line)
            count = 1;
        else if (verbs[i] == PrPathVerb_Quad)
            count = 2;
        else if (verbs[i] == PrPathVerb_Cubic)
            count = 3;
        if (count > 0) {
            PrVec2 end = points[at + count - 1];
            if (verbs[i] != PrPathVerb_Move && (last.x < 45) != (end.x < 45))
                ++crossings;
            last = end;
            at += count;
        }
    }
    CHECK(crossings == 0);
    prDrawListRelease(list);
    prLottieAnimationDestroy(animation);
}

TEST_CASE("text is shaped a paragraph at a time, and each letter is its cluster's glyphs") {
    // Arabic letters join their neighbors, so in context each takes a form other than the one it has alone.
    PrFontSet* fonts = prFontSetCreate(nullptr);
    std::string path = std::string(PR_FONTS_DIR) + "/NotoSansArabic.ttf";
    PrFontFaceDescriptor descriptor = prFontFaceDescriptorDefault();
    const PrFont* font = nullptr;
    REQUIRE(prFontCreateFromFile(path, 0, nullptr, &font) == PrStatus_Ok);
    prFontSetAdd(fonts, &descriptor, font);
    prFontRelease(font);
    PrTextShaper* shaper = prTextKbCreate();
    PrLottieOptions options{};
    options.fonts = fonts;
    options.shaper = shaper;
    // Beh, yeh and teh, joined as a word, and alone as paragraphs of their own.
    auto layout = [&](const char* text, std::vector<std::vector<uint32_t>>& letters) {
        std::string json = std::string(R"({"w": 100, "h": 100, "fr": 30, "ip": 0, "op": 60,
            "fonts": {"list": [{"fName": "Arabic", "fFamily": "Noto Sans Arabic", "fStyle": "Regular"}]},
            "layers": [{"ty": 5, "ip": 0, "op": 60, "st": 0, "ks": {},
                "t": {"d": {"k": [{"s": {"s": 20, "f": "Arabic", "t": ")") + text + R"(", "j": 0}, "t": 0}]},
                      "p": {}, "m": {}, "a": []}}]})";
        LottieAnimation a{};
        REQUIRE(lottieParse(json, options, a) == PrStatus_Ok);
        for (uint32_t i = 0; i < a.letters.count; ++i) {
            const LottieLetter& letter = a.letters.data[i];
            if (letter.newline)
                continue;
            CHECK(letter.width > 0.0f);
            std::vector<uint32_t> ids;
            for (uint32_t g = 0; g < letter.glyphCount; ++g)
                ids.push_back(a.textGlyphs.data[letter.glyphs + g].id);
            letters.push_back(ids);
        }
        lottieReset(a);
    };
    std::vector<std::vector<uint32_t>> joined;
    std::vector<std::vector<uint32_t>> alone;
    layout("\\u0628\\u064a\\u062a", joined);
    layout("\\u0628\\r\\u064a\\r\\u062a", alone);
    REQUIRE(joined.size() == 3);
    REQUIRE(alone.size() == 3);
    for (size_t i = 0; i < 3; ++i) {
        CHECK(!joined[i].empty());
        CHECK(joined[i] != alone[i]);
    }
    prTextShaperDestroy(shaper);
    prFontSetDestroy(fonts);
}

TEST_CASE("a gradient's stop count is read as a count of what its values hold, whatever the file says") {
    std::string rect = R"({"ty": "rc", "p": {"a": 0, "k": [50, 50]}, "s": {"a": 0, "k": [40, 40]}, "r": {"a": 0, "k": 0}})";
    PrDrawList* list = prDrawListCreate();
    // A count that the values cannot hold paints nothing. A fraction is truncated.
    const std::pair<const char*, size_t> cases[] = {{"1073741825", 0}, {"-1", 0}, {"1e30", 0}, {"2", 1}, {"1.5", 1}};
    for (auto [count, paths] : cases) {
        std::string fill = R"({"ty": "gf", "o": {"a": 0, "k": 100}, "t": 1, "s": {"a": 0, "k": [0, 0]},
                               "e": {"a": 0, "k": [100, 0]}, "g": {"p": )"
            + std::string(count) + R"(, "k": {"a": 0, "k": [0, 1, 0, 0, 1, 0, 0, 1]}}})";
        PrLottieAnimation* animation = read(shapeLayer(rect + ", " + fill));
        std::vector<PrDrawCommand> commands = painted(render(animation, 0, list));
        CHECK(commands.size() == paths);
        prLottieAnimationDestroy(animation);
    }
    prDrawListRelease(list);
}

TEST_CASE("a zig-zag's ridges are bounded, whatever the file asks for") {
    std::string rect = R"({"ty": "rc", "p": {"a": 0, "k": [50, 50]}, "s": {"a": 0, "k": [40, 40]}, "r": {"a": 0, "k": 0}})";
    std::string fill = R"({"ty": "fl", "o": {"a": 0, "k": 100}, "c": {"a": 0, "k": [1, 0, 0, 1]}})";
    PrDrawList* list = prDrawListCreate();
    for (const char* points : {"1e30", "-1e30", "4"}) {
        std::string zigZag = R"({"ty": "zz", "s": {"a": 0, "k": 5}, "r": {"a": 0, "k": )" + std::string(points) +
            R"(}, "pt": {"a": 0, "k": 1}})";
        PrLottieAnimation* animation = read(shapeLayer(rect + ", " + zigZag + ", " + fill));
        std::vector<PrDrawCommand> commands = painted(render(animation, 0, list));
        REQUIRE(commands.size() == 1);
        CHECK(prPathPointCount(commands[0].path) < 1000000);
        prLottieAnimationDestroy(animation);
    }
    prDrawListRelease(list);
}

TEST_CASE("precomps that multiply each other's layers draw a bounded number of layers") {
    // Each composition holds ten precomps of the one before it, so the last draws a million solids.
    auto layers = [](const std::string& ref) {
        std::string out;
        for (int i = 0; i < 10; ++i)
            out += std::string(i != 0 ? ", " : "") + R"({"ty": 0, "refId": ")" + ref +
                R"(", "w": 10, "h": 10, "ip": 0, "op": 60, "st": 0, "ks": {}})";
        return out;
    };
    std::string assets = R"({"id": "c0", "w": 10, "h": 10, "layers": [
        {"ty": 1, "ip": 0, "op": 60, "st": 0, "sw": 10, "sh": 10, "sc": "#00ff00", "ks": {}}]})";
    for (int level = 1; level <= 6; ++level)
        assets += R"(, {"id": "c)" + std::to_string(level) + R"(", "w": 10, "h": 10, "layers": [)" +
            layers("c" + std::to_string(level - 1)) + "]}";
    PrLottieAnimation* animation = read(R"({"w": 100, "h": 100, "fr": 30, "ip": 0, "op": 60, "assets": [)" + assets +
        R"(], "layers": [)" + layers("c6") + "]}");
    PrDrawList* list = prDrawListCreate();
    uint32_t solids = 0;
    for (const PrDrawCommand& c : render(animation, 0, list))
        solids += c.kind == PrDrawCommandKind_Rect;
    CHECK(solids > 0);
    CHECK(solids <= 100000);
    prDrawListRelease(list);
    prLottieAnimationDestroy(animation);
}

TEST_CASE("a composition that holds precomps of itself draws its other layers once") {
    PrLottieAnimation* animation = read(R"({"w": 100, "h": 100, "fr": 30, "ip": 0, "op": 60,
        "assets": [{"id": "loop", "w": 10, "h": 10, "layers": [
            {"ty": 0, "refId": "loop", "w": 10, "h": 10, "ip": 0, "op": 60, "st": 0, "ks": {}},
            {"ty": 0, "refId": "loop", "w": 10, "h": 10, "ip": 0, "op": 60, "st": 0, "ks": {}},
            {"ty": 1, "ip": 0, "op": 60, "st": 0, "sw": 10, "sh": 10, "sc": "#00ff00", "ks": {}}]}],
        "layers": [{"ty": 0, "refId": "loop", "w": 10, "h": 10, "ip": 0, "op": 60, "st": 0, "ks": {}}]})");
    PrDrawList* list = prDrawListCreate();
    uint32_t solids = 0;
    for (const PrDrawCommand& c : render(animation, 0, list))
        solids += c.kind == PrDrawCommandKind_Rect;
    CHECK(solids == 1);
    prDrawListRelease(list);
    prLottieAnimationDestroy(animation);
}
