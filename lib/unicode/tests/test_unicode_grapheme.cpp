#include "pr/locale_common.h"
#include "pr/unicode.h"
#include "pr/unicode_grapheme.h"
#include "pr/utf8.h"

#include "tools/harness/test.h"

#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

TEST_CASE("grapheme clusters break as every case of GraphemeBreakTest.txt says") {
    std::string path = std::string(PR_UNICODE_DIR) + "/GraphemeBreakTest.txt";
    FILE* file = std::fopen(path.c_str(), "r");
    REQUIRE(file != nullptr);
    char line[4096];
    uint32_t cases = 0;
    while (std::fgets(line, sizeof(line), file) != nullptr) {
        std::string data = line;
        data = data.substr(0, data.find('#'));
        if (data.find_first_not_of(" \t\r\n") == std::string::npos)
            continue;
        // "÷ 0020 × 0308 ÷": the text, and the byte offsets of its breaks.
        std::istringstream in(data);
        std::string token;
        std::string text;
        std::vector<size_t> expected;
        while (in >> token) {
            if (token == "\xc3\xb7") {
                if (!text.empty())
                    expected.push_back(text.size());
            } else if (token != "\xc3\x97") {
                char bytes[kPrUtf8MaxBytes];
                uint32_t length = prUtf8Encode(uint32_t(std::stoul(token, nullptr, 16)), bytes);
                text.append(bytes, length);
            }
        }
        std::vector<size_t> actual;
        PrStringView view{text.data(), text.size()};
        for (size_t at = 0; at < text.size();) {
            at = prUnicodeGraphemeNext(view, at);
            actual.push_back(at);
        }
        INFO(data);
        CHECK(actual == expected);
        ++cases;
    }
    std::fclose(file);
    CHECK(cases > 700);
}

TEST_CASE("word segments break as every case of WordBreakTest.txt says") {
    std::string path = std::string(PR_UNICODE_DIR) + "/WordBreakTest.txt";
    FILE* file = std::fopen(path.c_str(), "r");
    REQUIRE(file != nullptr);
    char line[4096];
    uint32_t cases = 0;
    while (std::fgets(line, sizeof(line), file) != nullptr) {
        std::string data = line;
        data = data.substr(0, data.find('#'));
        if (data.find_first_not_of(" \t\r\n") == std::string::npos)
            continue;
        // "÷ 0020 × 0308 ÷": the text, and the byte offsets of its breaks.
        std::istringstream in(data);
        std::string token;
        std::string text;
        std::vector<size_t> expected;
        while (in >> token) {
            if (token == "\xc3\xb7") {
                if (!text.empty())
                    expected.push_back(text.size());
            } else if (token != "\xc3\x97") {
                char bytes[kPrUtf8MaxBytes];
                uint32_t length = prUtf8Encode(uint32_t(std::stoul(token, nullptr, 16)), bytes);
                text.append(bytes, length);
            }
        }
        std::vector<size_t> actual;
        PrStringView view{text.data(), text.size()};
        for (size_t at = 0; at < text.size();) {
            at = prUnicodeWordNext(view, at);
            actual.push_back(at);
        }
        INFO(data);
        CHECK(actual == expected);
        ++cases;
    }
    std::fclose(file);
    CHECK(cases > 1000);
}

TEST_CASE("joining types read from the database") {
    CHECK(prUnicodeJoiningType(0x0628) == PrUnicodeJoiningType_Dual);           // ARABIC LETTER BEH
    CHECK(prUnicodeJoiningType(0x0627) == PrUnicodeJoiningType_Right);          // ARABIC LETTER ALEF
    CHECK(prUnicodeJoiningType(0x0640) == PrUnicodeJoiningType_JoinCausing);    // ARABIC TATWEEL
    CHECK(prUnicodeJoiningType(0x064E) == PrUnicodeJoiningType_Transparent);    // ARABIC FATHA
    CHECK(prUnicodeJoiningType('a') == PrUnicodeJoiningType_NonJoining);
}

TEST_CASE("vertical orientations read from the database") {
    CHECK(prUnicodeVerticalOrientation('A') == PrUnicodeVerticalOrientation_Rotated);
    CHECK(prUnicodeVerticalOrientation(0x65E5) == PrUnicodeVerticalOrientation_Upright);              // CJK 日
    CHECK(prUnicodeVerticalOrientation(0x3001) == PrUnicodeVerticalOrientation_TransformedUpright);   // CJK 、
    CHECK(prUnicodeVerticalOrientation(0x3008) == PrUnicodeVerticalOrientation_TransformedRotated);   // CJK 〈
    CHECK(prUnicodeVerticalOrientation(0x10FFFF) == PrUnicodeVerticalOrientation_Rotated);
}

TEST_CASE("character properties read from the database") {
    CHECK(prUnicodeScript('a') == PrScript_Latn);
    CHECK(prUnicodeScript(0x0627) == PrScript_Arab);
    CHECK(prUnicodeScript(' ') == PrScript_Zyyy);
    CHECK(prUnicodeScript(0x0301) == PrScript_Zinh);
    CHECK((prUnicodeFlags(0x1F600) & PrUnicodeFlag_EmojiPresentation) != 0);
    CHECK((prUnicodeFlags(0x2764) & PrUnicodeFlag_EmojiPresentation) == 0);
    CHECK((prUnicodeFlags(0x2764) & PrUnicodeFlag_Emoji) != 0);
    CHECK((prUnicodeFlags('q') & PrUnicodeFlag_Lowercase) != 0);
    CHECK((prUnicodeFlags('Q') & PrUnicodeFlag_Lowercase) == 0);
    CHECK((prUnicodeFlags(0x200e) & PrUnicodeFlag_DefaultIgnorable) != 0);
    CHECK((prUnicodeFlags(0x00ad) & PrUnicodeFlag_DefaultIgnorable) != 0);
    CHECK((prUnicodeFlags(' ') & PrUnicodeFlag_DefaultIgnorable) == 0);
    for (uint32_t selector : {0x180Bu, 0x180Fu, 0xFE00u, 0xFE0Fu, 0xE0100u, 0xE01EFu})
        CHECK((prUnicodeFlags(selector) & PrUnicodeFlag_VariationSelector) != 0);
    for (uint32_t other : {0x180Eu, 0xFE10u, 0xE00FFu, 0xE01F0u, uint32_t('A')})
        CHECK((prUnicodeFlags(other) & PrUnicodeFlag_VariationSelector) == 0);
    uint32_t upper[3];
    CHECK(prUnicodeUppercase('q', upper) == 1);
    CHECK(upper[0] == 'Q');
    CHECK(prUnicodeUppercase(0x00DF, upper) == 2);     // sharp s to SS
    CHECK((upper[0] == 'S' && upper[1] == 'S'));
    CHECK(prUnicodeUppercase('7', upper) == 1);
    CHECK(upper[0] == '7');
}

TEST_CASE("general categories read from the database") {
    CHECK(prUnicodeGeneralCategory('A') == PrUnicodeGeneralCategory_UppercaseLetter);
    CHECK(prUnicodeGeneralCategory('a') == PrUnicodeGeneralCategory_LowercaseLetter);
    CHECK(prUnicodeGeneralCategory(0x01C5) == PrUnicodeGeneralCategory_TitlecaseLetter);      // Dž
    CHECK(prUnicodeGeneralCategory(0x65E5) == PrUnicodeGeneralCategory_OtherLetter);          // CJK 日
    CHECK(prUnicodeGeneralCategory(0x0301) == PrUnicodeGeneralCategory_NonspacingMark);
    CHECK(prUnicodeGeneralCategory('7') == PrUnicodeGeneralCategory_DecimalNumber);
    CHECK(prUnicodeGeneralCategory('_') == PrUnicodeGeneralCategory_ConnectorPunctuation);
    CHECK(prUnicodeGeneralCategory('-') == PrUnicodeGeneralCategory_DashPunctuation);
    CHECK(prUnicodeGeneralCategory('(') == PrUnicodeGeneralCategory_OpenPunctuation);
    CHECK(prUnicodeGeneralCategory(0x00AB) == PrUnicodeGeneralCategory_InitialPunctuation);   // «
    CHECK(prUnicodeGeneralCategory('!') == PrUnicodeGeneralCategory_OtherPunctuation);
    CHECK(prUnicodeGeneralCategory('+') == PrUnicodeGeneralCategory_MathSymbol);
    CHECK(prUnicodeGeneralCategory('$') == PrUnicodeGeneralCategory_CurrencySymbol);
    CHECK(prUnicodeGeneralCategory('^') == PrUnicodeGeneralCategory_ModifierSymbol);
    CHECK(prUnicodeGeneralCategory(0x1F600) == PrUnicodeGeneralCategory_OtherSymbol);
    CHECK(prUnicodeGeneralCategory(' ') == PrUnicodeGeneralCategory_SpaceSeparator);
    CHECK(prUnicodeGeneralCategory(0x00A0) == PrUnicodeGeneralCategory_SpaceSeparator);
    CHECK(prUnicodeGeneralCategory(0x2028) == PrUnicodeGeneralCategory_LineSeparator);
    CHECK(prUnicodeGeneralCategory('\n') == PrUnicodeGeneralCategory_Control);
    CHECK(prUnicodeGeneralCategory(0x200D) == PrUnicodeGeneralCategory_Format);
    CHECK(prUnicodeGeneralCategory(0xD800) == PrUnicodeGeneralCategory_Surrogate);
    CHECK(prUnicodeGeneralCategory(0xE000) == PrUnicodeGeneralCategory_PrivateUse);
    CHECK(prUnicodeGeneralCategory(0x0378) == PrUnicodeGeneralCategory_Unassigned);
    CHECK(prUnicodeGeneralCategory(0x110000) == PrUnicodeGeneralCategory_Unassigned);
}

TEST_CASE("case folding is the full folding, without the Turkic mappings") {
    uint32_t folded[3];
    CHECK(prUnicodeFold('A', folded) == 1);
    CHECK(folded[0] == 'a');
    CHECK(prUnicodeFold('a', folded) == 1);
    CHECK(folded[0] == 'a');
    CHECK(prUnicodeFold('7', folded) == 1);
    CHECK(folded[0] == '7');
    CHECK(prUnicodeFold(0x00DF, folded) == 2);     // sharp s
    CHECK((folded[0] == 's' && folded[1] == 's'));
    CHECK(prUnicodeFold(0x1E9E, folded) == 2);     // capital sharp s
    CHECK((folded[0] == 's' && folded[1] == 's'));
    CHECK(prUnicodeFold(0x0130, folded) == 2);     // capital I with dot above
    CHECK((folded[0] == 'i' && folded[1] == 0x0307));
    CHECK(prUnicodeFold('I', folded) == 1);
    CHECK(folded[0] == 'i');
    CHECK(prUnicodeFold(0x03C2, folded) == 1);     // final sigma
    CHECK(folded[0] == 0x03C3);
    CHECK(prUnicodeFold(0x03A3, folded) == 1);     // capital sigma
    CHECK(folded[0] == 0x03C3);
    CHECK(prUnicodeFold(0x0390, folded) == 3);     // iota with dialytika and tonos
    CHECK((folded[0] == 0x03B9 && folded[1] == 0x0308 && folded[2] == 0x0301));
    CHECK(prUnicodeFold(0x10400, folded) == 1);    // Deseret capital long i
    CHECK(folded[0] == 0x10428);
}

TEST_CASE("lowercase is the unconditional full mapping, which keeps the sharp s") {
    uint32_t lower[3];
    CHECK(prUnicodeLowercase('A', lower) == 1);
    CHECK(lower[0] == 'a');
    CHECK(prUnicodeLowercase('7', lower) == 1);
    CHECK(lower[0] == '7');
    CHECK(prUnicodeLowercase(0x00DF, lower) == 1);     // sharp s
    CHECK(lower[0] == 0x00DF);
    CHECK(prUnicodeLowercase(0x1E9E, lower) == 1);     // capital sharp s
    CHECK(lower[0] == 0x00DF);
    CHECK(prUnicodeLowercase(0x0130, lower) == 2);     // capital I with dot above
    CHECK((lower[0] == 'i' && lower[1] == 0x0307));
    CHECK(prUnicodeLowercase(0x03A3, lower) == 1);     // capital sigma
    CHECK(lower[0] == 0x03C3);
}
