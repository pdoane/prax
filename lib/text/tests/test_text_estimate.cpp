// Estimates of how text sets: lines from a table of advances, with no shaping.

#include "pr/font_set.h"
#include "pr/text_estimate.h"

#include "tools/harness/test.h"

#include <string>

namespace pr_impl {

namespace {

// An estimator of Arimo at 16 px, whose `x` advances 8 px.
struct Estimated {
    PrFontSet* fonts = prFontSetCreate(nullptr);
    PrTextFont* font = nullptr;
    PrTextEstimator* estimator = nullptr;

    Estimated() {
        std::string path = std::string(PR_FONTS_DIR) + "/Arimo-Regular.ttf";
        const PrFont* PR_NULLABLE face = nullptr;
        REQUIRE(prFontCreateFromFile(path, 0, nullptr, &face) == PrStatus_Ok);
        PrFontFaceDescriptor descriptor = prFontFaceDescriptorDefault();
        prFontSetAdd(fonts, &descriptor, face);
        prFontRelease(face);
        PrFontFamily family = prFontFamilyFromName("Arimo");
        PrFontStyle style = prFontStyleDefault();
        style.size = 16;
        PrTextFontDesc desc{.set = fonts, .families = &family, .familyCount = 1, .style = style};
        font = prTextFontCreate(&desc);
        estimator = prTextEstimatorCreate(font, PrTextAxis_Horizontal);
    }

    ~Estimated() {
        prTextEstimatorDestroy(estimator);
        prTextFontRelease(font);
        prFontSetDestroy(fonts);
    }

    uint32_t lines(const std::string& text, float width) { return prTextEstimatorLineCount(estimator, text, width); }
};

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("an estimator reads advances from the font") {
    Estimated e;
    CHECK(prTextEstimatorAdvance(e.estimator, 'x') == doctest::Approx(8));
    CHECK(prTextEstimatorAdvance(e.estimator, 'i') < prTextEstimatorAdvance(e.estimator, 'm'));
    float average = prTextEstimatorByteAdvance(e.estimator);
    CHECK(average > 4);
    CHECK(average < 12);
}

TEST_CASE("a word that fits on no line breaks anywhere, and an empty text takes a line") {
    Estimated e;
    CHECK(e.lines("", 100) == 1);
    CHECK(e.lines(std::string(10, 'x'), 80) == 1);
    CHECK(e.lines(std::string(11, 'x'), 80) == 2);
    CHECK(e.lines(std::string(4000, 'x'), 800) == 40);
    CHECK(e.lines("abc", 0) == 1);
}

TEST_CASE("a line breaks at white space, and the word after it moves down whole") {
    Estimated e;
    CHECK(e.lines("xxxxx xxxxx", 80) == 2);
    // Eight x and a space fit, and what hangs past the end takes no line.
    CHECK(e.lines("xxxxxxxxxx      ", 80) == 1);
    CHECK(e.lines("xxxxx " + std::string(30, 'x'), 80) == 4);
}

TEST_CASE("a tab advances to the next stop") {
    Estimated e;
    float space = prTextEstimatorAdvance(e.estimator, ' ');
    float stop = 8 * space;
    std::string text = "\t\t";
    CHECK(e.lines(text + "x", 2 * stop + 8) == 1);
    CHECK(e.lines(text + "xx", 2 * stop + 8) == 2);
}

TEST_CASE("wide characters break between each other") {
    Estimated e;
    float wide = prTextEstimatorAdvance(e.estimator, 0x4e00);
    std::string han;
    for (uint32_t i = 0; i < 10; ++i)
        han += "\xe4\xb8\x80";
    if (wide > 0)
        CHECK(e.lines(han, wide * 5) == 2);
}
