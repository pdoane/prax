#include "pr/locale_full.h"
#include "pr/string.h"
#include "pr/unicode.h"
#include "pr/utf8.h"

#include "tools/harness/test.h"

extern "C" int prTestUnicodeCApi(void);

namespace pr_impl {

namespace {

// Returns `text` mapped to `target` in `language`, one code point at a time, with the whole text as context.
String mapped(PrStringView text, PrUnicodeCase target, PrLanguage language = 0) {
    String out;
    PrStringView view = text;
    for (size_t at = 0; at < text.size;) {
        uint32_t codePoint = 0;
        uint32_t length = prUtf8Decode(view, at, &codePoint);
        uint32_t result[kPrUnicodeCaseMaxCodePoints];
        uint32_t count = prUnicodeMapCase(view, at, target, language, result);
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

TEST_CASE("the public headers compile as C") {
    CHECK(prTestUnicodeCApi() == 0);
}

TEST_CASE("titlecase maps a digraph to its capital and small form, and an unpaired letter to its capital") {
    uint32_t title[3];
    CHECK(prUnicodeTitlecase(0x01C6, title) == 1);     // dz with caron
    CHECK(title[0] == 0x01C5);
    CHECK(prUnicodeTitlecase(0x01C4, title) == 1);     // capital DZ with caron
    CHECK(title[0] == 0x01C5);
    CHECK(prUnicodeTitlecase('q', title) == 1);
    CHECK(title[0] == 'Q');
    CHECK(prUnicodeTitlecase(0xFB01, title) == 2);     // fi ligature
    CHECK((title[0] == 'F' && title[1] == 'i'));
    CHECK(prUnicodeTitlecase(0x00DF, title) == 2);     // sharp s
    CHECK((title[0] == 'S' && title[1] == 's'));
}

TEST_CASE("a capital sigma lowercases to the final form only at the end of a word") {
    CHECK(mapped("ΟΔΟΣ ΣΑΣ", PrUnicodeCase_Lower) == "οδος σας");
    CHECK(mapped("Σ", PrUnicodeCase_Lower) == "σ");
    CHECK(mapped("ΑΣ'", PrUnicodeCase_Lower) == "ας'");    // a case-ignorable apostrophe after it
    CHECK(mapped("ΑΣ'Α", PrUnicodeCase_Lower) == "ασ'α");
}

TEST_CASE("Turkish and Azerbaijani pair dotted and dotless i") {
    CHECK(mapped("ISIK İZ", PrUnicodeCase_Lower, PrLanguage_Tr) == "ısık iz");
    CHECK(mapped("İ", PrUnicodeCase_Lower, PrLanguage_Az) == "i");
    CHECK(mapped("i", PrUnicodeCase_Upper, PrLanguage_Tr) == "İ");
    CHECK(mapped("i", PrUnicodeCase_Title, PrLanguage_Tr) == "İ");
    CHECK(mapped("ISIK İZ", PrUnicodeCase_Lower) == "isik i̇z");
    CHECK(mapped("i", PrUnicodeCase_Upper, PrLanguage_En) == "I");
}

TEST_CASE("Lithuanian keeps the dot of an i under an accent, and drops it after a soft-dotted letter in capitals") {
    CHECK(mapped("Í", PrUnicodeCase_Lower, PrLanguage_Lt) == "i̇́");
    CHECK(mapped("Ì", PrUnicodeCase_Lower, PrLanguage_Lt) == "i̇̀");
    CHECK(mapped("I", PrUnicodeCase_Lower, PrLanguage_Lt) == "i");
    CHECK(mapped("i̇", PrUnicodeCase_Upper, PrLanguage_Lt) == "I");
    CHECK(mapped("i̇", PrUnicodeCase_Upper) == "İ");
}

TEST_CASE("full-width, full-size kana and mathematical italic forms") {
    CHECK(prUnicodeFullWidth('A') == 0xFF21);
    CHECK(prUnicodeFullWidth(' ') == 0x3000);
    CHECK(prUnicodeFullWidth(0xFF76) == 0x30AB);       // half-width katakana ka
    CHECK(prUnicodeFullWidth(0x00E9) == 0x00E9);       // e acute has none
    CHECK(prUnicodeFullSizeKana(0x3041) == 0x3042);    // small hiragana a
    CHECK(prUnicodeFullSizeKana(0x31F0) == 0x30AF);    // small katakana ku
    CHECK(prUnicodeFullSizeKana(0xFF67) == 0xFF71);    // small half-width katakana a stays half-width
    CHECK(prUnicodeFullSizeKana(0x3042) == 0x3042);
    CHECK(prUnicodeMathVariant('x', PrUnicodeMathVariant_Italic) == 0x1D465);
    CHECK(prUnicodeMathVariant('h', PrUnicodeMathVariant_Italic) == 0x210E);
    CHECK(prUnicodeMathVariant(0x03B1, PrUnicodeMathVariant_Italic) == 0x1D6FC);     // alpha
    CHECK(prUnicodeMathVariant('2', PrUnicodeMathVariant_Italic) == '2');
}

TEST_CASE("mathematical alphanumeric forms of every variant") {
    // The number of characters that each variant maps, as MathML Core's tables give them
    // (mathml-core/#new-text-transform-mappings).
    constexpr uint32_t kCounts[PrUnicodeMathVariant_Count] = {
        122, 112, 110, 52, 52, 52, 87, 52, 62, 120, 52, 110, 62, 20, 15, 27, 23,
    };
    for (uint32_t variant = 0; variant < PrUnicodeMathVariant_Count; ++variant) {
        uint32_t count = 0;
        for (uint32_t c = 0; c < 0x1F000; ++c)
            count += prUnicodeMathVariant(c, PrUnicodeMathVariant(variant)) != c ? 1 : 0;
        CAPTURE(variant);
        CHECK(count == kCounts[variant]);
    }
    // The holes of the alphabets are letterlike symbols.
    CHECK(prUnicodeMathVariant('B', PrUnicodeMathVariant_Script) == 0x212C);
    CHECK(prUnicodeMathVariant('C', PrUnicodeMathVariant_Fraktur) == 0x212D);
    CHECK(prUnicodeMathVariant('R', PrUnicodeMathVariant_DoubleStruck) == 0x211D);
    CHECK(prUnicodeMathVariant('l', PrUnicodeMathVariant_Script) == 0x1D4C1);
    CHECK(prUnicodeMathVariant('A', PrUnicodeMathVariant_BoldFraktur) == 0x1D56C);
    CHECK(prUnicodeMathVariant('0', PrUnicodeMathVariant_Monospace) == 0x1D7F6);
    CHECK(prUnicodeMathVariant(0x0628, PrUnicodeMathVariant_Initial) == 0x1EE21);     // beh
    CHECK(prUnicodeMathVariant(0x0131, PrUnicodeMathVariant_Italic) == 0x1D6A4);      // dotless i

    // No variant maps Greek capital pi.
    CHECK(prUnicodeMathVariant(0x03A0, PrUnicodeMathVariant_DoubleStruck) == 0x03A0);
}
