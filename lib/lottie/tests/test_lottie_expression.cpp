#include "pr/lottie.h"
#include "pr/string.h"

#include "lottie/src/lottie_eval.h"
#include "lottie/src/lottie_model.h"

#include "tools/harness/test.h"

namespace pr_impl {

namespace {

// Returns an animation at 30 frames per second, with a null layer "Target" that moves, and a layer "Moved" whose
// opacity is given by the expression.
String animationWith(PrStringView expression) {
    String json(R"({"w": 100, "h": 100, "fr": 30, "ip": 0, "op": 60, "layers": [
        {"ty": 3, "nm": "Target", "ind": 1, "ip": 0, "op": 60, "st": 0,
         "ks": {"p": {"a": 1, "k": [{"t": 0, "s": [0, 0], "o": {"x": 0, "y": 0}, "i": {"x": 1, "y": 1}},
                                    {"t": 30, "s": [30, 60]}]}},
         "ef": [{"ty": 5, "nm": "Slider Control", "ef": [{"ty": 0, "nm": "Slider", "v": {"k": 42}}]}]},
        {"ty": 3, "nm": "Moved", "ind": 2, "ip": 0, "op": 60, "st": 0,
         "ks": {"o": {"a": 1, "k": [{"t": 0, "s": [0], "o": {"x": [0], "y": [0]}, "i": {"x": [1], "y": [1]}},
                                    {"t": 10, "s": [100]}], "x": ")");
    json.append(expression);
    json.append(R"("}}}]})");
    return json;
}

// Returns the opacity of the layer "Moved" at `frame`, with its expression evaluated.
float opacityAt(PrStringView expression, double frame) {
    String json = animationWith(expression);
    PrLottieAnimation* animation = nullptr;
    REQUIRE(prLottieAnimationCreate(json, nullptr, &animation) == PrStatus_Ok);
    const LottieAnimation& a = *animation;
    const LottieLayer& moved = a.layers.data[a.comps.data[0].layers + 1];
    float value = -1;
    lottieEvaluate(a, a.transforms.data[moved.transform].opacity, frame, &value);
    prLottieAnimationDestroy(animation);
    return value;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("an expression's result replaces its property's value") {
    CHECK(opacityAt("var $bm_rt; $bm_rt = time * 10;", 15) == doctest::Approx(5));
    CHECK(opacityAt("var $bm_rt; $bm_rt = $bm_div(value, 2);", 5) == doctest::Approx(25));
    // An expression that does not evaluate leaves the value unchanged.
    CHECK(opacityAt("var $bm_rt; $bm_rt = nothing.here;", 5) == doctest::Approx(50));
}

TEST_CASE("an expression reaches other layers, their transforms and their effects") {
    CHECK(opacityAt("var $bm_rt; $bm_rt = thisComp.layer('Target').transform.position[1];", 15) == doctest::Approx(30));
    CHECK(opacityAt("var $bm_rt; $bm_rt = thisComp.layer(1).effect('Slider Control')('Slider');", 0) ==
        doctest::Approx(42));
    CHECK(opacityAt("var $bm_rt; $bm_rt = thisLayer.name.length;", 0) == doctest::Approx(5));
}

TEST_CASE("loopOut cycles a property's keyframes past its last") {
    CHECK(opacityAt("var $bm_rt; $bm_rt = loopOut('cycle');", 15) == doctest::Approx(50));
    CHECK(opacityAt("var $bm_rt; $bm_rt = loopOut('pingpong');", 15) == doctest::Approx(50));
    CHECK(opacityAt("var $bm_rt; $bm_rt = linear(time, 0, 1, 0, 200);", 15) == doctest::Approx(100));
}

TEST_CASE("wiggle and random draw the same at a frame however often it is drawn, and wiggle within its amplitude") {
    String json = animationWith("var $bm_rt; $bm_rt = wiggle(3, 20) + random(0, 0.001);");
    PrLottieAnimation* animation = nullptr;
    REQUIRE(prLottieAnimationCreate(json, nullptr, &animation) == PrStatus_Ok);
    const LottieAnimation& a = *animation;
    uint32_t opacity = a.transforms.data[a.layers.data[a.comps.data[0].layers + 1].transform].opacity;
    auto at = [&](double frame) {
        float value = -1;
        lottieEvaluate(a, opacity, frame, &value);
        return value;
    };
    float first = at(40);
    float values[30];
    for (uint32_t f = 0; f < 30; ++f)
        values[f] = at(30 + f);
    CHECK(at(40) == first);
    bool moves = false;
    for (uint32_t f = 0; f < 30; ++f) {
        // The keyframed value holds at 100 past frame 10.
        CHECK(values[f] >= 80.0f);
        CHECK(values[f] <= 120.001f);
        moves = moves || values[f] != values[0];
    }
    CHECK(moves);
    prLottieAnimationDestroy(animation);
}
