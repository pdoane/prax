// The DirectWrite backend: a set asks DirectWrite for families by name and for fallback.

#include "pr/array.h"
#include "pr/font.h"
#include "pr/font_directwrite.h"

#include "tools/harness/test.h"

extern "C" int prTestFontDirectWriteCApi(void);

namespace pr_impl {

using namespace pr;

namespace {

// Returns the faces that `family` at `weight` matches in `set`, each holding a reference to its font that the caller
// releases.
Array<PrFontFace> matchAll(PrFontSet* set, const char* family, float weight) {
    PrFontStyle style = prFontStyleDefault();
    style.weight = weight;
    PrFontMatch* match = prFontMatchCreate();
    prFontSetMatch(set, prFontFamilyFromName(family), &style, match);
    Array<PrFontFace> faces;
    for (uint32_t i = 0; i < prFontMatchFaceCount(match); ++i) {
        PrFontFace face = prFontMatchFace(match, i);
        if (face.font != nullptr)
            prFontRetain(face.font);
        faces.push(face);
    }
    prFontMatchDestroy(match);
    return faces;
}

// Releases the fonts of `faces`.
void release(const Array<PrFontFace>& faces) {
    for (const PrFontFace& face : faces)
        prFontRelease(face.font);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("the public headers compile as C") {
    CHECK(prTestFontDirectWriteCApi() == 0);
}

TEST_CASE("a family that no set defines is the system's, read only when text matches it") {
    PrFontSystem* system = prFontDirectWriteCreate();
    REQUIRE(system != nullptr);
    PrFontSet* set = prFontSetCreate(nullptr);
    prFontSetSetSystem(set, system);
    CHECK(prFontSetFaceCount(set) == 0);
    Array<PrFontFace> faces = matchAll(set, "Segoe UI", 700);
    REQUIRE(!faces.isEmpty());
    CHECK(prFontEmbolden(faces[0].font) == 0.0f);
    release(faces);
    faces = matchAll(set, "Segoe UI Emoji", 400);
    REQUIRE(!faces.isEmpty());
    CHECK(faces[0].flags == PrFontFaceFlag_Emoji);
    release(faces);
    // U+6F22, a Han ideograph, in Japanese, falls back to the family that the system gives. The set asks only once.
    PrLocale japanese = prLocaleFromTag("ja");
    CHECK(prFontSetRequestFallback(set, "\xE6\xBC\xA2", japanese, PrFontGeneric_SansSerif));
    CHECK(prFontSetFallbackCount(set) == 1);
    CHECK_FALSE(prFontSetRequestFallback(set, "\xE6\xBC\xA2", japanese, PrFontGeneric_SansSerif));
    prFontSetDestroy(set);
    prFontSystemDestroy(system);
}
