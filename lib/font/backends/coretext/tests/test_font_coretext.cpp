// The CoreText backend: the files that CoreText's font manager lists become faces of a set that match by family and
// style.

#include "pr/font.h"
#include "pr/font_coretext.h"

#include "doctest/doctest.h"

#include <vector>

extern "C" int prTestFontCoreTextCApi(void);

namespace pr_impl {

namespace {

// Returns the faces that `family` at `weight` matches in `set`, each holding a reference to its font that the caller
// releases.
std::vector<PrFontFace> matchAll(PrFontSet* set, const char* family, float weight) {
    PrFontStyle style = prFontStyleDefault();
    style.weight = weight;
    PrFontMatch* match = prFontMatchCreate();
    prFontSetMatch(set, prFontFamilyFromName(family), &style, match);
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

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("the public headers compile as C") {
    CHECK(prTestFontCoreTextCApi() == 0);
}

TEST_CASE("the system's fonts are a set") {
    PrFontSet* set = prFontSetCreate(nullptr);
    CHECK(prFontCoreTextInstall(set) > 0);
    CHECK(prFontSetFamilyCount(set) > 0);
    std::vector<PrFontFace> faces = matchAll(set, "Helvetica", 700);
    REQUIRE(!faces.empty());
    CHECK(prFontEmbolden(faces[0].font) == 0.0f);
    CHECK(faces[0].flags == 0);
    release(faces);
    faces = matchAll(set, "Apple Color Emoji", 400);
    REQUIRE(!faces.empty());
    CHECK(faces[0].flags == PrFontFaceFlag_Emoji);
    release(faces);
    prFontSetDestroy(set);
}
