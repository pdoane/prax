// The CoreText backend: a set asks CoreText for families by name, for generic families, and for fallback.

#include "pr/font.h"
#include "pr/font_coretext.h"

#include "tools/harness/test.h"

#include <string>
#include <vector>

extern "C" int prTestFontCoreTextCApi(void);

namespace pr_impl {

namespace {

// Returns the faces that `family` at `weight` matches in `set`, each holding a reference to its font that the caller
// releases.
std::vector<PrFontFace> matchAll(PrFontSet* set, PrFontFamily family, float weight) {
    PrFontStyle style = prFontStyleDefault();
    style.weight = weight;
    PrFontMatch* match = prFontMatchCreate();
    prFontSetMatch(set, family, &style, match);
    std::vector<PrFontFace> faces;
    for (uint32_t i = 0; i < prFontMatchFaceCount(match); ++i) {
        PrFontFace face = prFontMatchFace(match, i);
        if (face.font != nullptr)
            prFontRetain(face.font);
        faces.push_back(face);
    }
    prFontMatchDestroy(match);
    return faces;
}

// Releases the fonts of `faces`.
void release(const std::vector<PrFontFace>& faces) {
    for (const PrFontFace& face : faces)
        prFontRelease(face.font);
}

// A set with no fonts of its own, which asks the CoreText system for every family.
struct SystemSet {
    PrFontSystem* system = prFontCoreTextCreate();
    PrFontSet* set = prFontSetCreate(nullptr);

    SystemSet() { prFontSetSetSystem(set, system); }
    ~SystemSet() {
        prFontSetDestroy(set);
        prFontSystemDestroy(system);
    }
};

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("the public headers compile as C") {
    CHECK(prTestFontCoreTextCApi() == 0);
}

TEST_CASE("a family that no set defines is the system's, read only when text matches it") {
    SystemSet fonts;
    CHECK(prFontSetFaceCount(fonts.set) == 0);
    std::vector<PrFontFace> faces = matchAll(fonts.set, prFontFamilyFromName("Helvetica"), 700);
    REQUIRE(!faces.empty());
    CHECK(prFontEmbolden(faces[0].font) == 0.0f);
    CHECK(faces[0].flags == 0);
    release(faces);
    faces = matchAll(fonts.set, prFontFamilyFromName("Apple Color Emoji"), 400);
    REQUIRE(!faces.empty());
    CHECK(faces[0].flags == PrFontFaceFlag_Emoji);
    release(faces);
    // A family that the system lacks matches nothing.
    CHECK(matchAll(fonts.set, prFontFamilyFromName("No Such Family Anywhere"), 400).empty());
}

TEST_CASE("the generic families are the system's, and its own text's where the system sets it") {
    SystemSet fonts;
    for (PrFontGeneric generic : {PrFontGeneric_Monospace, PrFontGeneric_SystemUi, PrFontGeneric_SansSerif}) {
        CAPTURE(generic);
        CHECK(prFontSetGenericFamily(fonts.set, generic).size != 0);
        std::vector<PrFontFace> faces = matchAll(fonts.set, prFontFamilyGeneric(generic), 400);
        CHECK(!faces.empty());
        release(faces);
    }
    // A binding of the set's own comes before the system's.
    prFontSetSetGeneric(fonts.set, PrFontGeneric_Serif, "Helvetica");
    CHECK(prFontSetGenericFamily(fonts.set, PrFontGeneric_Serif) == "Helvetica");
}

TEST_CASE("text that no fallback family serves falls back to the family that the system gives, asked once") {
    SystemSet fonts;
    CHECK(prFontSetFallbackCount(fonts.set) == 0);
    // U+6F22, a Han ideograph, in Japanese.
    PrLocale japanese = prLocaleFromTag("ja");
    REQUIRE(prFontSetRequestFallback(fonts.set, "\xE6\xBC\xA2", japanese, PrFontGeneric_SansSerif));
    REQUIRE(prFontSetFallbackCount(fonts.set) == 1);
    CHECK(prFontSetFallbackFamily(fonts.set, 0).family.size != 0);
    CHECK(prFontSetFallbackFamily(fonts.set, 0).generic == PrFontGeneric_SansSerif);
    PrFontStyle style = prFontStyleDefault();
    PrFontMatch* match = prFontMatchCreate();
    prFontSetMatchFallback(fonts.set, 0, &style, match);
    bool served = false;
    for (uint32_t i = 0; i < prFontMatchFaceCount(match); ++i) {
        PrFontFace face = prFontMatchFace(match, i);
        served = served || prFontFaceServes(&face, 0x6f22);
    }
    CHECK(served);
    prFontMatchDestroy(match);
    // The question is not asked again.
    CHECK_FALSE(prFontSetRequestFallback(fonts.set, "\xE6\xBC\xA2", japanese, PrFontGeneric_SansSerif));
    CHECK(prFontSetFallbackCount(fonts.set) == 1);
}

TEST_CASE("a set over another asks the outermost set's system") {
    SystemSet fonts;
    PrFontSet* document = prFontSetCreate(fonts.set);
    std::vector<PrFontFace> faces = matchAll(document, prFontFamilyFromName("Helvetica"), 400);
    CHECK(!faces.empty());
    release(faces);
    CHECK(prFontSetFaceCount(document) == 0);
    CHECK(prFontSetFaceCount(fonts.set) != 0);
    CHECK(prFontSetRequestFallback(document, "\xE6\xBC\xA2", 0, PrFontGeneric_None));
    CHECK(prFontSetFallbackCount(document) == 1);
    prFontSetDestroy(document);
}
