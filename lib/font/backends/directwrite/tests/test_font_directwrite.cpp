// The DirectWrite backend: the files in DirectWrite's set of the system's fonts become faces of a set that match by
// family and style.

#include "pr/font.h"
#include "pr/font_directwrite.h"

#include "doctest/doctest.h"

#include <vector>

extern "C" int prTestFontDirectWriteCApi(void);

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
    CHECK(prTestFontDirectWriteCApi() == 0);
}

TEST_CASE("the system's fonts are a set") {
    PrFontSet* set = prFontSetCreate(nullptr);
    CHECK(prFontDirectWriteInstall(set) > 0);
    CHECK(prFontSetFamilyCount(set) > 0);
    std::vector<PrFontFace> faces = matchAll(set, "Segoe UI", 700);
    REQUIRE(!faces.empty());
    CHECK(prFontEmbolden(faces[0].font) == 0.0f);
    release(faces);
    faces = matchAll(set, "Segoe UI Emoji", 400);
    REQUIRE(!faces.empty());
    CHECK(faces[0].flags == PrFontFaceFlag_Emoji);
    release(faces);
    prFontSetDestroy(set);
}
