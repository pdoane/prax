// Text transforms: case, width, size and math forms, and the words that capitalize finds.

#include "pr/locale_common.h"
#include "pr/string.h"
#include "pr/text_transform.h"
#include "pr/utf8.h"

#include "tools/harness/test.h"

namespace pr_impl {

using namespace pr;

namespace {

// Returns the bytes [start, end) of `text` transformed by `transform`, with the rest of `text` as context. By default
// the run is all of `text`.
String transformed(PrStringView text, PrTextTransform transform, PrLocale locale = 0, size_t start = 0,
        size_t end = SIZE_MAX) {
    PrTextTransformRun run{
        .text = text,
        .start = start,
        .end = end == SIZE_MAX ? text.size : end,
        .transform = transform,
        .locale = locale,
    };
    String out;
    for (size_t at = run.start; at < run.end;) {
        uint32_t codePoint = 0;
        uint32_t length = prUtf8Decode(run.text, at, &codePoint);
        uint32_t result[kPrTextTransformMaxCodePoints];
        uint32_t count = prTextTransformAt(&run, at, result);
        for (uint32_t i = 0; i < count; ++i) {
            char bytes[kPrUtf8MaxBytes];
            out.append(PrStringView(bytes, prUtf8Encode(result[i], bytes)));
        }
        at += length;
    }
    return out;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("capitalize titlecases the first letter of each word, if it is lowercase") {
    CHECK(transformed("the miXed text", PrTextTransform_Capitalize) == "The MiXed Text");
    CHECK(transformed("ǆemal", PrTextTransform_Capitalize) == "ǅemal");
    CHECK(transformed("Ǆemal", PrTextTransform_Capitalize) == "Ǆemal");
    CHECK(transformed("hello world", PrTextTransform_Capitalize) == "Hello World");
}

TEST_CASE("capitalize's words: hyphens and leading punctuation divide them, apostrophes and low lines do not") {
    CHECK(transformed("foo-bar", PrTextTransform_Capitalize) == "Foo-Bar");
    CHECK(transformed("john's apple", PrTextTransform_Capitalize) == "John's Apple");
    CHECK(transformed("foo_bar", PrTextTransform_Capitalize) == "Foo_bar");
    CHECK(transformed("({[-–\"«'.<?!transform", PrTextTransform_Capitalize) == "({[-–\"«'.<?!Transform");
    CHECK(transformed("ag·rouée", PrTextTransform_Capitalize) == "Ag·rouée");
    CHECK(transformed("3rd", PrTextTransform_Capitalize) == "3rd");
    CHECK(transformed("ⓐⓐⓐ ⓑ", PrTextTransform_Capitalize) == "ⓐⓐⓐ ⓑ");    // lowercase symbols, not letters
}

TEST_CASE("capitalize reads the text before the run, so a word continues across a change of style") {
    CHECK(transformed("abc", PrTextTransform_Capitalize, 0, 1, 2) == "b");
    CHECK(transformed("a bc", PrTextTransform_Capitalize, 0, 2, 3) == "B");
    CHECK(transformed("mixed", PrTextTransform_Capitalize, 0, 0, 2) == "Mi");
}

TEST_CASE("uppercase and lowercase use the full and conditional mappings, tailored by language") {
    CHECK(transformed("straße", PrTextTransform_Uppercase) == "STRASSE");
    CHECK(transformed("ΟΔΟΣ ΣΑΣ", PrTextTransform_Lowercase) == "οδος σας");
    PrLocale turkish = prLocaleMake(PrLanguage_Tr, 0, 0);
    CHECK(transformed("i", PrTextTransform_Uppercase, turkish) == "İ");
    CHECK(transformed("ISIK", PrTextTransform_Lowercase, turkish) == "ısık");
    CHECK(transformed("i", PrTextTransform_Capitalize, turkish) == "İ");
}

TEST_CASE("full-width and full-size-kana apply after the case transform") {
    CHECK(transformed("abc", PrTextTransform(PrTextTransform_Uppercase | PrTextTransformFlag_FullWidth)) == "ＡＢＣ");
    CHECK(transformed("ｧｨｩ", PrTextTransform(PrTextTransformFlag_FullWidth | PrTextTransformFlag_FullSizeKana)) == "アイウ");
    CHECK(transformed("A B", PrTextTransformFlag_FullWidth) == "Ａ　Ｂ");
    CHECK(transformed("AéB", PrTextTransformFlag_FullWidth) == "ＡéＢ");
    CHECK(transformed("ぁゃっ", PrTextTransformFlag_FullSizeKana) == "あやつ");
}

TEST_CASE("math-auto sets a lone character in its italic form and leaves longer text") {
    CHECK(transformed("x", PrTextTransform_MathAuto) == "𝑥");
    CHECK(transformed(" x ", PrTextTransform_MathAuto) == " 𝑥 ");
    CHECK(transformed("sin", PrTextTransform_MathAuto) == "sin");
    CHECK(transformed("2", PrTextTransform_MathAuto) == "2");
    CHECK(transformed("sin x", PrTextTransform_MathAuto, 0, 4, 5) == "𝑥");
    CHECK(transformed("sin", PrTextTransform_MathBold) == "𝐬𝐢𝐧");
    CHECK(transformed("Rx", prTextTransformFromMathVariant(PrUnicodeMathVariant_DoubleStruck)) == "ℝ𝕩");
    CHECK(transformed("h2", PrTextTransform_MathItalic) == "ℎ2");
}
