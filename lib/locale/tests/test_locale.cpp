#include "pr/locale.h"
#include "pr/locale_common.h"
#include "pr/locale_full.h"

#include "doctest/doctest.h"

#include <string>
#include <vector>

extern "C" int prTestLocaleCApi(void);

namespace pr_impl {

namespace {

std::string tag(PrLocale locale) {
    char out[kPrLocaleMaxTag];
    return std::string(out, prLocaleTag(locale, out));
}

std::string resolved(const char* text) {
    return tag(prLocaleFromTag(text));
}

uint32_t ot(const char* text) {
    std::string padded = std::string(text) + "    ";
    return uint32_t(uint8_t(padded[0])) << 24 | uint32_t(uint8_t(padded[1])) << 16 | uint32_t(uint8_t(padded[2])) << 8 |
        uint32_t(uint8_t(padded[3]));
}

std::vector<uint32_t> systems(const char* text) {
    PrLocale locale = prLocaleFromTag(text);
    std::vector<uint32_t> out(prLocaleOpenTypeLanguages(locale, nullptr, 0));
    prLocaleOpenTypeLanguages(locale, out.data(), uint32_t(out.size()));
    return out;
}

std::string quote(const char* text, uint32_t depth, bool open) {
    PrStringView mark = prLocaleQuote(prLocaleFromTag(text), depth, open);
    return std::string(mark.data, mark.size);
}

std::string view(PrStringView text) {
    return std::string(text.data, text.size);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("the public headers compile as C") {
    CHECK(prTestLocaleCApi() == 0);
}

TEST_CASE("a subtag names its language, script or region whatever its case, a deprecated one as replaced") {
    CHECK(prLanguageFromSubtag("EN") == PrLanguage_En);
    CHECK(prLanguageFromSubtag("yue") == PrLanguage_Yue);
    CHECK(prLanguageFromSubtag("iw") == PrLanguage_He);
    CHECK(prLanguageFromSubtag("tlh") == PrLanguage_Tlh);
    CHECK(prLanguageFromSubtag("english") == PrLanguage_None);
    CHECK(prScriptFromCode("latn") == PrScript_Latn);
    CHECK(prScriptFromCode("HANT") == PrScript_Hant);
    CHECK(prScriptFromCode("Xyzw") == PrScript_Zzzz);
    CHECK(prRegionFromSubtag("us") == PrRegion_US);
    CHECK(prRegionFromSubtag("419") == PrRegion_419);
    CHECK(prRegionFromSubtag("BU") == PrRegion_MM);
    CHECK(view(prLanguageSubtag(PrLanguage_Yue)) == "yue");
    CHECK(view(prScriptCode(PrScript_Hant)) == "Hant");
    CHECK(view(prRegionSubtag(PrRegion_419)) == "419");
    CHECK(view(prLanguageSubtag(PrLanguage_None)).empty());
}

TEST_CASE("a tag resolves to its canonical locale, with its likely script and region") {
    CHECK(resolved("en") == "en-Latn-US");
    CHECK(resolved("EN-gb") == "en-Latn-GB");
    CHECK(resolved("zh") == "zh-Hans-CN");
    CHECK(resolved("zh-TW") == "zh-Hant-TW");
    CHECK(resolved("zh-Hant") == "zh-Hant-TW");
    CHECK(resolved("zh-Hant-HK") == "zh-Hant-HK");
    CHECK(resolved("sr") == "sr-Cyrl-RS");
    CHECK(resolved("ja") == "ja-Jpan-JP");
    CHECK(resolved("ko") == "ko-Kore-KR");
    // The registry's preferred values, and CLDR's aliases.
    CHECK(resolved("iw") == "he-Hebr-IL");
    CHECK(resolved("cmn-Hant") == "zh-Hant-TW");
    CHECK(resolved("sh") == "sr-Latn-RS");
    // An extlang is the language that it names. A tag that the registry lists whole is replaced by the registry's
    // replacement.
    CHECK(resolved("zh-yue") == "yue-Hant-HK");
    CHECK(resolved("i-klingon") == "tlh");
    CHECK(resolved("i-default").empty());
    // Variants, extensions and private use are dropped, and so are a private-use script and region.
    CHECK(resolved("de-DE-1996") == "de-Latn-DE");
    CHECK(resolved("en-US-u-ca-gregory-x-private") == "en-Latn-US");
    CHECK(resolved("en-Qaaa-QM") == "en-Latn-US");
}

TEST_CASE("a tag whose language does not resolve resolves to no locale") {
    for (const char* text : {"", "e", "english", "qaa", "-en", "zz-US", "not a tag"})
        CHECK_MESSAGE(prLocaleFromTag(text) == 0, text);
}

TEST_CASE("a tag ends at its first subtag that is ill-formed or unlisted, keeping the locale of those before it") {
    CHECK(prLocaleFromTag("en-EN") == prLocaleFromTag("en"));
    CHECK(prLocaleFromTag("en-UU") == prLocaleFromTag("en"));
    CHECK(prLocaleFromTag("en-Xyzw-GB") == prLocaleFromTag("en"));
    CHECK(prLocaleFromTag("zh-Hant-UU") == prLocaleFromTag("zh-Hant"));
    CHECK(prLocaleFromTag("en-") == prLocaleFromTag("en"));
    CHECK(prLocaleFromTag("en--US") == prLocaleFromTag("en"));
    CHECK(prLocaleFromTag("en-a") == prLocaleFromTag("en"));
    CHECK(prLocaleFromTag("en-US-a-b") == prLocaleFromTag("en-US"));
    // An extlang must follow its own prefix, and only one may.
    CHECK(prLocaleFromTag("en-yue") == prLocaleFromTag("en"));
    CHECK(prLocaleFromTag("zh-yue-yue") == prLocaleFromTag("yue"));
}

TEST_CASE("a locale packs its language, script and region, and writes its tag") {
    PrLocale locale = prLocaleMake(PrLanguage_Pt, PrScript_Latn, PrRegion_PT);
    CHECK(prLocaleLanguage(locale) == PrLanguage_Pt);
    CHECK(prLocaleScript(locale) == PrScript_Latn);
    CHECK(prLocaleRegion(locale) == PrRegion_PT);
    CHECK(tag(locale) == "pt-Latn-PT");
    CHECK(tag(prLocaleMake(PrLanguage_Pt, PrScript_Zzzz, PrRegion_None)) == "pt");
    CHECK(tag(0).empty());
}

TEST_CASE("a script has its OpenType tags, the newest first") {
    uint32_t tags[kPrScriptMaxOpenTypeTags];
    REQUIRE(prScriptOpenTypeTags(PrScript_Deva, tags) == 2);
    CHECK(tags[0] == ot("dev2"));
    CHECK(tags[1] == ot("deva"));
    REQUIRE(prScriptOpenTypeTags(PrScript_Latn, tags) == 1);
    CHECK(tags[0] == ot("latn"));
    REQUIRE(prScriptOpenTypeTags(PrScript_Laoo, tags) == 1);
    CHECK(tags[0] == ot("lao"));
    CHECK(prScriptOpenTypeTags(PrScript_Zzzz, tags) == 0);
}

TEST_CASE("a locale's OpenType language systems go from its script and region to its language") {
    CHECK(systems("en") == std::vector<uint32_t>{ot("ENG")});
    CHECK(systems("zh") == std::vector<uint32_t>{ot("ZHS")});
    CHECK(systems("zh-TW") == std::vector<uint32_t>{ot("ZHT")});
    CHECK(systems("zh-HK") == std::vector<uint32_t>{ot("ZHH"), ot("ZHT")});
    CHECK(systems("yue") == std::vector<uint32_t>{ot("ZHH"), ot("ZHT")});
    // The system of a macrolanguage follows the language's own system. A macrolanguage that no system lists takes the
    // systems of its languages.
    CHECK(systems("id") == std::vector<uint32_t>{ot("IND"), ot("MLY")});
    CHECK(systems("no") == std::vector<uint32_t>{ot("NOR"), ot("NYN")});
    // The system for Polytonic Greek belongs to a variant, which the engine does not distinguish.
    CHECK(systems("el") == std::vector<uint32_t>{ot("ELL")});
    CHECK(systems("ka") == std::vector<uint32_t>{ot("KAT")});
    CHECK(prLocaleOpenTypeLanguages(0, nullptr, 0) == 0);
}

TEST_CASE("a locale's quotation marks are CLDR's, through its fallbacks to the root") {
    CHECK(quote("en", 0, true) == "“");
    CHECK(quote("en", 1, false) == "’");
    CHECK(quote("de", 0, true) == "„");
    CHECK(quote("fr", 0, true) == "«");
    CHECK(quote("ja", 0, true) == "「");
    CHECK(quote("ja", 2, true) == "『");
    CHECK(quote("pt", 0, true) == "“");
    CHECK(quote("pt-PT", 0, true) == "«");
    CHECK(quote("zh-Hant", 0, true) == "「");
    CHECK(quote("zh", 0, true) == "“");
    CHECK(quote("tlh", 0, true) == "“");
    CHECK(prLocaleQuote(0, 0, true).size != 0);
}
