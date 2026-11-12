// Font bundles: each bundle's files are the files that the build fetches for it, and the names that each bundle gives
// match its fonts. Bundles installed together fall back to each bundle's families in turn. The tests install the
// bundles' files that the test data holds (cmake/test_data.cmake), which a bundle installs as it would all of its
// files. The font-coverage suite reads the bundles whole.

#include "pr/font_bundle.h"
#include "pr/locale.h"
#include "pr/locale_common.h"
#include "pr/span.h"
#include "pr/string.h"
#include "pr/string_ops.h"
#include "pr/text_font.h"

#include "tools/harness/test.h"

namespace pr_impl {

namespace {

using namespace pr;

// Returns the family of the face that `name` matches in `set`, or empty if it matches none. If `name` is the keyword of
// a generic family, it is matched as that generic family.
String familyOf(PrFontSet* set, const char* name) {
    PrFontGeneric generic = prFontGenericFromName(name);
    PrFontFamily asked = generic != PrFontGeneric_None ? prFontFamilyGeneric(generic) : prFontFamilyFromName(name);
    PrFontStyle style = prFontStyleDefault();
    PrFontMatch* match = prFontMatchCreate();
    prFontSetMatch(set, asked, &style, match);
    String answer;
    if (prFontMatchFaceCount(match) != 0)
        answer = String::adopt(prFontFamilyName(prFontMatchFace(match, 0).font));
    prFontMatchDestroy(match);
    return answer;
}

// Installs `bundles` into `set`, in order, and returns the number of files added.
uint32_t install(PrFontSet* set, Span<const PrFontBundle> bundles) {
    return prFontBundleInstall(set, bundles.data(), bundles.count(), PR_FONT_BUNDLE_DIR);
}

// Installs the one bundle `bundle` into `set`, and returns the number of files added.
uint32_t install(PrFontSet* set, PrFontBundle bundle) {
    return install(set, Span<const PrFontBundle>(&bundle, 1));
}

// Returns the family of fallback family `index` of `set`.
String fallbackFamily(PrFontSet* set, uint32_t index) {
    return prFontSetFallbackFamily(set, index).family;
}

// Returns the family of the face that sets `text` in a text font of `generic` in `set`, as the fallback finds it.
String fallbackOf(PrFontSet* set, PrFontGeneric generic, const char* text, PrScript script, PrLocale locale = 0) {
    PrFontFamily family = prFontFamilyGeneric(generic);
    PrTextFontDesc desc{
        .set = set,
        .families = &family,
        .familyCount = 1,
        .style = prFontStyleDefault(),
        .locale = locale,
    };
    PrTextFont* font = prTextFontCreate(&desc);
    uint32_t match = prTextFontMatch(font, text, script);
    String answer;
    if (match != kPrTextMatchNone)
        answer = String::adopt(prFontFamilyName(prTextFontMatchedFont(font, match)));
    prTextFontRelease(font);
    return answer;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("each bundle's files are those that the build fetches for it") {
    struct Expected {
        PrFontBundle bundle;
        uint32_t files;         // the files of the bundle's list in font_bundle.cmake
    };
    const Expected expected[] = {
        {PrFontBundle_Complete, PR_FONT_BUNDLE_FILES_Complete},
        {PrFontBundle_NotoMono, PR_FONT_BUNDLE_FILES_NotoMono},
        {PrFontBundle_Standard14, PR_FONT_BUNDLE_FILES_Standard14},
        {PrFontBundle_Plex, PR_FONT_BUNDLE_FILES_Plex},
        {PrFontBundle_NerdFontSymbols, PR_FONT_BUNDLE_FILES_NerdFontSymbols},
    };
    for (const Expected& e : expected) {
        CAPTURE(int(e.bundle));
        CHECK(prFontBundleFileCount(&e.bundle, 1) == e.files);
    }
}

TEST_CASE("each bundle falls back only to its own families") {
    struct Expected {
        PrFontBundle bundle;
        const char* first;      // the first fallback family
        const char* last;       // the last fallback family
        const char* prefix;     // what every fallback family's name begins with, or null
    };
    const Expected expected[] = {
        {PrFontBundle_Complete, "Noto Sans", "Last Resort", nullptr},
        {PrFontBundle_Standard14, "Arimo", "NewComputerModernMath", nullptr},
        {PrFontBundle_NerdFontSymbols, "Symbols Nerd Font Mono", "Symbols Nerd Font Mono", nullptr},
    };
    for (const Expected& e : expected) {
        CAPTURE(int(e.bundle));
        PrFontSet* set = prFontSetCreate(nullptr);
        CHECK(install(set, e.bundle) != 0);
        uint32_t count = prFontSetFallbackCount(set);
        REQUIRE(count != 0);
        CHECK(fallbackFamily(set, 0) == e.first);
        CHECK(fallbackFamily(set, count - 1) == e.last);
        for (uint32_t i = 0; i < count && e.prefix != nullptr; ++i)
            CHECK(prStringStartsWith(fallbackFamily(set, i), e.prefix));
        prFontSetDestroy(set);
    }
}

TEST_CASE("a stack of bundles installs each file once, and falls back to each bundle's families in turn") {
    PrFontSet* set = prFontSetCreate(nullptr);
    const PrFontBundle bundles[] = {PrFontBundle_Standard14, PrFontBundle_Complete};
    // New Computer Modern Math is in both.
    CHECK(prFontBundleFileCount(bundles, 2) ==
        prFontBundleFileCount(&bundles[0], 1) + prFontBundleFileCount(&bundles[1], 1) - 1);
    PrFontSet* both = prFontSetCreate(nullptr);
    PrFontSet* each = prFontSetCreate(nullptr);
    CHECK(install(both, bundles) == install(each, PrFontBundle_Standard14) + install(each, PrFontBundle_Complete) - 1);
    prFontSetDestroy(both);
    prFontSetDestroy(each);
    install(set, bundles);
    // The first bundle that gives a name binds it, and a later bundle binds what the earlier ones do not.
    CHECK(familyOf(set, "serif") == "Tinos");
    CHECK(familyOf(set, "Helvetica") == "Arimo");
    CHECK(familyOf(set, "system-ui") == "Noto Sans");
    uint32_t count = prFontSetFallbackCount(set);
    REQUIRE(count > 1);
    CHECK(fallbackFamily(set, 0) == "Arimo");
    CHECK(fallbackFamily(set, count - 1) == "Last Resort");
    // Each family is listed once, where the first bundle lists it.
    uint32_t math = 0;
    for (uint32_t i = 0; i < count; ++i)
        math += fallbackFamily(set, i) == "NewComputerModernMath" ? 1 : 0;
    CHECK(math == 1);
    prFontSetDestroy(set);
}

TEST_CASE("Noto names the generic families") {
    PrFontSet* set = prFontSetCreate(nullptr);
    install(set, PrFontBundle_Complete);
    CHECK(familyOf(set, "serif") == "Noto Serif");
    CHECK(familyOf(set, "sans-serif") == "Noto Sans");
    CHECK(familyOf(set, "system-ui") == "Noto Sans");
    CHECK(familyOf(set, "monospace") == "Noto Sans Mono");
    CHECK(familyOf(set, "math") == "NewComputerModernMath");
    CHECK(familyOf(set, "Helvetica").isEmpty());
    prFontSetDestroy(set);
}

TEST_CASE("Standard14 names the generic families and PDF's standard fonts") {
    PrFontSet* set = prFontSetCreate(nullptr);
    install(set, PrFontBundle_Standard14);
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
    PrFontBundle bundle = PrFontBundle_Complete;
    CHECK(prFontBundleInstall(set, &bundle, 1, "/nonexistent") == 0);
    CHECK(familyOf(set, "serif").isEmpty());
    CHECK(prFontSetFallbackCount(set) == 0);
    prFontSetDestroy(set);
}

TEST_CASE("Noto sets the symbols of no script that its text faces lack in a face of the text's style, not in a CJK face") {
    PrFontSet* set = prFontSetCreate(nullptr);
    install(set, PrFontBundle_Complete);
    // The math and symbol families come before the CJK families. An arrow beside Latin text therefore sits on the math
    // axis, as a math face draws it, rather than as wide and as high as an ideograph. Itemization gives an arrow in
    // Latin text the Latin script, and an arrow alone no script.
    const char* const arrows[] = {"\xe2\x86\x90", "\xe2\x87\x92"};
    const PrScript scripts[] = {PrScript_Latn, PrScript_Zyyy};
    for (const char* arrow : arrows) {
        for (PrScript script : scripts) {
            CAPTURE(arrow);
            CAPTURE(int(script));
            CHECK(fallbackOf(set, PrFontGeneric_SansSerif, arrow, script) == "Noto Sans Math");
            CHECK(fallbackOf(set, PrFontGeneric_Serif, arrow, script) == "NewComputerModernMath");
        }
    }
    // Text in a CJK language takes these symbols from the family made for that language.
    PrLocale japanese = prLocaleFromTag("ja");
    CHECK(fallbackOf(set, PrFontGeneric_SansSerif, "\xe2\x86\x90", PrScript_Jpan, japanese) == "Noto Sans JP");
    CHECK(fallbackOf(set, PrFontGeneric_SansSerif, "\xe2\x86\x90", PrScript_Zyyy, japanese) == "Noto Sans JP");
    // A character used by several scripts is set in the face of the surrounding script, even if a face of another of
    // its scripts comes first. The danda in Bengali text is set in the Bengali face.
    CHECK(fallbackOf(set, PrFontGeneric_SansSerif, "\xe0\xa5\xa4", PrScript_Beng) == "Noto Sans Bengali");
    CHECK(fallbackOf(set, PrFontGeneric_SansSerif, "\xe0\xa5\xa4", PrScript_Deva) == "Noto Sans Devanagari");
    prFontSetDestroy(set);
}

TEST_CASE("Nerd Fonts' symbols serve the private-use code points after the text faces, and no other text") {
    const char* file = "\xef\x85\x9b";              // U+F15B, a file
    const char* square = "\xf3\xb0\x80\x81";        // U+F0001, a Material Design vector square
    const char* heart = "\xe2\x99\xa5";             // U+2665, a heart, which Nerd Fonts' symbols also map
    PrFontSet* set = prFontSetCreate(nullptr);
    const PrFontBundle bundles[] = {PrFontBundle_Complete, PrFontBundle_NerdFontSymbols};
    install(set, bundles);
    // NerdFontSymbols names no family, and its family is the only one that serves private-use code points.
    CHECK(familyOf(set, "monospace") == "Noto Sans Mono");
    uint32_t count = prFontSetFallbackCount(set);
    REQUIRE(count > 1);
    CHECK(fallbackFamily(set, count - 1) == "Symbols Nerd Font Mono");
    for (uint32_t i = 0; i < count; ++i)
        CHECK(prFontSetFallbackFamily(set, i).privateUse == (i == count - 1));
    // Last Resort comes before it and has a glyph for every code point, but a private-use code point skips it.
    const PrFontGeneric generics[] = {PrFontGeneric_Monospace, PrFontGeneric_SansSerif, PrFontGeneric_Serif};
    for (PrFontGeneric generic : generics) {
        CAPTURE(int(generic));
        CHECK(fallbackOf(set, generic, file, PrScript_Zyyy) == "Symbols Nerd Font Mono");
        CHECK(fallbackOf(set, generic, square, PrScript_Zyyy) == "Symbols Nerd Font Mono");
        CHECK(fallbackOf(set, generic, heart, PrScript_Zyyy) != "Symbols Nerd Font Mono");
    }
    prFontSetDestroy(set);
}
