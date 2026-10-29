// Font bundles: every file of each bundle installs, and the names each bundle gives reach its fonts.

#include "pr/font_bundle.h"

#include "doctest/doctest.h"

#include <string>

namespace pr_impl {

namespace {

// Returns the family of the face that `name` matches in `set`, or empty if it matches none. If `name` is the keyword of
// a generic family, it is matched as that generic family.
std::string familyOf(PrFontSet* set, const char* name) {
    PrFontGeneric generic = prFontGenericFromName(name);
    PrFontFamily asked = generic != PrFontGeneric_None ? prFontFamilyGeneric(generic) : prFontFamilyFromName(name);
    PrFontStyle style = prFontStyleDefault();
    PrFontMatch* match = prFontMatchCreate();
    prFontSetMatch(set, asked, &style, match);
    std::string answer;
    if (prFontMatchFaceCount(match) != 0) {
        PrString family = prFontFamilyName(prFontMatchFace(match, 0).font);
        PrStringView answered = prStringView(&family);
        answer.assign(answered.data, answered.size);
        prStringReset(&family);
    }
    prFontMatchDestroy(match);
    return answer;
}

// Returns the family of fallback family `index` of `set`.
std::string fallbackFamily(PrFontSet* set, uint32_t index) {
    PrStringView family = prFontSetFallbackFamily(set, index).family;
    return std::string(family.data, family.size);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("each bundle installs every one of its files, and falls back to Last Resort last") {
    for (PrFontBundle bundle : {PrFontBundle_Noto, PrFontBundle_Standard14}) {
        CAPTURE(int(bundle));
        PrFontSet* set = prFontSetCreate(nullptr);
        CHECK(prFontBundleInstall(set, bundle, PR_FONT_BUNDLE_DIR) == prFontBundleFileCount(bundle));
        uint32_t count = prFontSetFallbackCount(set);
        REQUIRE(count > 1);
        CHECK(fallbackFamily(set, 0) == "Noto Sans");
        CHECK(fallbackFamily(set, count - 1) == "Last Resort");
        prFontSetDestroy(set);
    }
}

TEST_CASE("Noto names the generic families") {
    PrFontSet* set = prFontSetCreate(nullptr);
    prFontBundleInstall(set, PrFontBundle_Noto, PR_FONT_BUNDLE_DIR);
    CHECK(familyOf(set, "serif") == "Noto Serif");
    CHECK(familyOf(set, "sans-serif") == "Noto Sans");
    CHECK(familyOf(set, "monospace") == "Noto Sans Mono");
    CHECK(familyOf(set, "math") == "NewComputerModernMath");
    CHECK(familyOf(set, "Helvetica").empty());
    prFontSetDestroy(set);
}

TEST_CASE("Standard14 names the generic families and PDF's standard fonts") {
    PrFontSet* set = prFontSetCreate(nullptr);
    prFontBundleInstall(set, PrFontBundle_Standard14, PR_FONT_BUNDLE_DIR);
    CHECK(familyOf(set, "serif") == "Tinos");
    CHECK(familyOf(set, "Times") == "Tinos");
    CHECK(familyOf(set, "sans-serif") == "Arimo");
    CHECK(familyOf(set, "Helvetica") == "Arimo");
    CHECK(familyOf(set, "monospace") == "Cousine");
    CHECK(familyOf(set, "Courier") == "Cousine");
    CHECK(familyOf(set, "Symbol") == "LeviSymbol");
    CHECK(familyOf(set, "ZapfDingbats") == "Levibats-Regular");
    prFontSetDestroy(set);
}

TEST_CASE("a bundle whose directory holds none of its files installs none, and falls back to nothing") {
    PrFontSet* set = prFontSetCreate(nullptr);
    CHECK(prFontBundleInstall(set, PrFontBundle_Noto, "/nonexistent") == 0);
    CHECK(familyOf(set, "serif").empty());
    CHECK(prFontSetFallbackCount(set) == 0);
    prFontSetDestroy(set);
}
