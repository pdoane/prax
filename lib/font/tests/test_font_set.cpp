#include "pr/font_set.h"

#include "doctest/doctest.h"

#include <cmath>
#include <string>
#include <vector>

namespace pr_impl {

namespace {

std::string fontPath(const char* name) {
    return std::string(PR_FONTS_DIR) + "/" + name;
}

void addFile(PrFontSet* set, const PrFontFaceDescriptor& descriptor, const char* name) {
    std::string path = fontPath(name);
    const PrFont* font = nullptr;
    REQUIRE(prFontCreateFromFile(path, 0, nullptr, &font) == PrStatus_Ok);
    prFontSetAdd(set, &descriptor, font);
    prFontRelease(font);
}

// Returns the value that the font sets the axis to, or NaN if it does not set it.
float axisValue(const PrFont* font, uint32_t tag) {
    for (uint32_t i = 0; i < prFontAxisCount(font); ++i) {
        if (prFontAxis(font, i).tag == tag)
            return prFontAxisValue(font, i);
    }
    return NAN;
}

// What a test requests of a set: a family, and a style.
struct Asked {
    PrFontFamily family;
    PrFontStyle style;
};

// Returns the faces that `asked` matches in `set`, each holding a reference to its font that the caller releases.
std::vector<PrFontFace> matchAll(PrFontSet* set, const Asked& asked) {
    PrFontMatch* match = prFontMatchCreate();
    prFontSetMatch(set, asked.family, &asked.style, match);
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

// Returns the single face that `asked` matches, holding a reference to its font that the caller releases.
PrFontFace matchOne(PrFontSet* set, const Asked& asked) {
    std::vector<PrFontFace> faces = matchAll(set, asked);
    REQUIRE(faces.size() == 1);
    return faces[0];
}

Asked request(const char* family) {
    return Asked{prFontFamilyFromName(family), prFontStyleDefault()};
}

Asked request(PrFontGeneric generic) {
    return Asked{prFontFamilyGeneric(generic), prFontStyleDefault()};
}

// Returns the family of the first face that `r` matches in `set`, or an empty string if it matches no face or a face
// that is still loading.
std::string familyMatched(PrFontSet* set, const Asked& r) {
    std::vector<PrFontFace> faces = matchAll(set, r);
    std::string answer;
    if (!faces.empty() && faces[0].font != nullptr) {
        PrString family = prFontFamilyName(faces[0].font);
        PrStringView answered = prStringView(&family);
        answer.assign(answered.data, answered.size);
        prStringReset(&family);
    }
    for (const PrFontFace& face : faces)
        prFontRelease(face.font);
    return answer;
}

const PrFont* loadFont(const char* name) {
    std::string path = fontPath(name);
    const PrFont* font = nullptr;
    REQUIRE(prFontCreateFromFile(path, 0, nullptr, &font) == PrStatus_Ok);
    return font;
}

constexpr uint32_t kWght = PR_FONT_TAG('w', 'g', 'h', 't');
constexpr uint32_t kWdth = PR_FONT_TAG('w', 'd', 't', 'h');
constexpr uint32_t kSlnt = PR_FONT_TAG('s', 'l', 'n', 't');
constexpr uint32_t kOpsz = PR_FONT_TAG('o', 'p', 's', 'z');

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a font set lists the families of its faces once each") {
    PrFontSet* set = prFontSetCreate(nullptr);
    PrFontFaceDescriptor auto_ = prFontFaceDescriptorDefault();
    addFile(set, auto_, "NotoSans.ttf");
    addFile(set, auto_, "NotoSerif.ttf");
    addFile(set, auto_, "NotoSans.ttf");
    CHECK(prFontSetFaceCount(set) == 3);
    REQUIRE(prFontSetFamilyCount(set) == 2);
    PrStringView first = prFontSetFamily(set, 0);
    PrStringView second = prFontSetFamily(set, 1);
    CHECK(std::string(first.data, first.size) == "Noto Sans");
    CHECK(std::string(second.data, second.size) == "Noto Serif");
    CHECK(matchAll(set, request("Nothing")).empty());
    prFontSetDestroy(set);
}

TEST_CASE("an alias matches its family's faces, and a family with faces matches itself") {
    PrFontSet* set = prFontSetCreate(nullptr);
    PrFontFaceDescriptor auto_ = prFontFaceDescriptorDefault();
    addFile(set, auto_, "NotoSans.ttf");
    addFile(set, auto_, "NotoSerif.ttf");
    prFontSetAlias(set, "Times", "Noto Sans");
    prFontSetAlias(set, "times", "Noto Serif");
    prFontSetAlias(set, "noto sans", "Noto Serif");
    CHECK(familyMatched(set, request("TIMES")) == "Noto Serif");
    CHECK(familyMatched(set, request("Noto Sans")) == "Noto Sans");
    CHECK(familyMatched(set, request("Helvetica")).empty());
    CHECK(prFontSetFamilyCount(set) == 2);
    prFontSetDestroy(set);
}

TEST_CASE("a generic family matches the family bound to it, and is never a name") {
    PrFontSet* set = prFontSetCreate(nullptr);
    PrFontFaceDescriptor auto_ = prFontFaceDescriptorDefault();
    addFile(set, auto_, "NotoSans.ttf");
    addFile(set, auto_, "NotoSerif.ttf");
    prFontSetSetGeneric(set, PrFontGeneric_Serif, "Noto Serif");
    CHECK(familyMatched(set, request(PrFontGeneric_Serif)) == "Noto Serif");
    // A family named "serif" is not the generic, and an unbound generic matches nothing.
    CHECK(familyMatched(set, request("serif")).empty());
    CHECK(familyMatched(set, request(PrFontGeneric_SansSerif)).empty());
    CHECK(prFontGenericFromName("Sans-Serif") == PrFontGeneric_SansSerif);
    CHECK(prFontGenericFromName("nastaliq") == PrFontGeneric_Nastaliq);
    CHECK(prFontGenericFromName("Noto Sans") == PrFontGeneric_None);
    prFontSetDestroy(set);
}

TEST_CASE("a font set's default family is serif until it is set") {
    PrFontSet* set = prFontSetCreate(nullptr);
    PrFontFamily initial = prFontSetDefaultFamily(set);
    CHECK(initial.generic == PrFontGeneric_Serif);
    prFontSetSetDefaultFamily(set, prFontFamilyFromName("Times"));
    PrFontFamily named = prFontSetDefaultFamily(set);
    CHECK(named.generic == PrFontGeneric_None);
    CHECK(std::string(named.name.data, named.name.size) == "Times");
    PrFontSet* child = prFontSetCreate(set);
    CHECK(prFontSetDefaultFamily(child).generic == PrFontGeneric_None);
    prFontSetDestroy(child);
    prFontSetDestroy(set);
}

TEST_CASE("a set over a parent matches its own family before the parent's, and the parent's generics and fallback") {
    PrFontSet* host = prFontSetCreate(nullptr);
    PrFontFaceDescriptor auto_ = prFontFaceDescriptorDefault();
    addFile(host, auto_, "NotoSans.ttf");
    addFile(host, auto_, "NotoSerif.ttf");
    prFontSetSetGeneric(host, PrFontGeneric_Serif, "Noto Serif");
    const PrFontFallbackFamily fallback[] = {{"Noto Sans", 0, PrFontGeneric_None}};
    prFontSetSetFallbackFamilies(host, fallback, 1);
    PrFontSet* document = prFontSetCreate(host);
    // The "Noto Sans" of the document is Literata, which hides the family of that name in the host.
    PrFontFaceDescriptor named = prFontFaceDescriptorDefault();
    named.automatic &= PrFontFaceAutoFlags(~PrFontFaceAutoFlag_Family);
    named.family = "Noto Sans";
    addFile(document, named, "Literata.ttf");
    CHECK(familyMatched(document, request("Noto Sans")) == "Literata");
    CHECK(familyMatched(host, request("Noto Sans")) == "Noto Sans");
    CHECK(familyMatched(document, request("Noto Serif")) == "Noto Serif");
    CHECK(familyMatched(document, request(PrFontGeneric_Serif)) == "Noto Serif");
    REQUIRE(prFontSetFallbackCount(document) == 1);
    PrStringView first = prFontSetFallbackFamily(document, 0).family;
    CHECK(std::string(first.data, first.size) == "Noto Sans");
    prFontSetDestroy(document);
    prFontSetDestroy(host);
}

TEST_CASE("a pending face is matched without a font until it loads, and a failed family hides the parent's") {
    PrFontSet* host = prFontSetCreate(nullptr);
    PrFontFaceDescriptor auto_ = prFontFaceDescriptorDefault();
    addFile(host, auto_, "NotoSans.ttf");
    PrFontSet* document = prFontSetCreate(host);
    PrFontFaceDescriptor web = prFontFaceDescriptorDefault();
    web.family = "Web";
    prFontSetAddPending(document, &web, 7);
    PrFontFaceDescriptor shadow = prFontFaceDescriptorDefault();
    shadow.family = "Noto Sans";
    prFontSetAddPending(document, &shadow, 8);

    // Pending: matched with no font, and requested once.
    PrFontFace face = matchOne(document, request("Web"));
    CHECK(face.font == nullptr);
    CHECK(face.pending == 7);
    uint64_t version = prFontSetVersion(document);
    prFontSetRequestLoad(document, 7);
    prFontSetRequestLoad(document, 7);
    uint64_t keys[4];
    REQUIRE(prFontSetTakeLoads(document, keys, 4) == 1);
    CHECK(keys[0] == 7);
    CHECK(prFontSetTakeLoads(document, keys, 4) == 0);

    // Loaded: matched with its font, at a new version.
    const PrFont* serif = loadFont("NotoSerif.ttf");
    prFontSetLoaded(document, 7, serif);
    prFontRelease(serif);
    CHECK(prFontSetVersion(document) != version);
    CHECK(familyMatched(document, request("Web")) == "Noto Serif");

    // Failed: the family is missing, and the family of that name in the host is not matched in its place.
    prFontSetFailed(document, 8);
    CHECK(matchAll(document, request("Noto Sans")).empty());
    // A change to the parent changes the version of the child.
    version = prFontSetVersion(document);
    prFontSetSetGeneric(host, PrFontGeneric_Monospace, "Noto Sans");
    CHECK(prFontSetVersion(document) != version);
    prFontSetDestroy(document);
    prFontSetDestroy(host);
}

TEST_CASE("matching narrows weights as css-fonts-4 orders them") {
    // One variable font described as three static weights, as @font-face rules would describe it.
    PrFontSet* set = prFontSetCreate(nullptr);
    for (float weight : {300.0f, 400.0f, 700.0f}) {
        PrFontFaceDescriptor d = prFontFaceDescriptorDefault();
        d.automatic = PrFontFaceAutoFlag_All & ~PrFontFaceAutoFlag_Weight;
        d.weightMin = d.weightMax = weight;
        addFile(set, d, "NotoSans.ttf");
    }
    struct WeightCase {
        float asked;
        float expected;
    };
    WeightCase cases[] = {{450, 400}, {400, 400}, {500, 400}, {600, 700}, {350, 300}, {200, 300}, {900, 700}};
    for (const WeightCase& c : cases) {
        CAPTURE(c.asked);
        Asked r = request("noto sans");
        r.style.weight = c.asked;
        PrFontFace match = matchOne(set, r);
        CHECK(axisValue(match.font, kWght) == c.expected);
        // Bold is synthesized only if a request for a bold weight matched a face that is not bold.
        CHECK((prFontEmbolden(match.font) > 0) == (c.asked >= 600 && c.expected < 600));
        prFontRelease(match.font);
    }
    prFontSetDestroy(set);
}

TEST_CASE("matching sets a variable face's axes to the request") {
    PrFontSet* set = prFontSetCreate(nullptr);
    PrFontFaceDescriptor auto_ = prFontFaceDescriptorDefault();
    addFile(set, auto_, "NotoSans.ttf");
    addFile(set, auto_, "Literata.ttf");
    addFile(set, auto_, "RobotoFlex.ttf");

    Asked r = request("Noto Sans");
    r.style.weight = 650;
    r.style.width = 80;
    PrFontFace match = matchOne(set, r);
    CHECK(axisValue(match.font, kWght) == 650.0f);
    CHECK(axisValue(match.font, kWdth) == 80.0f);
    CHECK(prFontEmbolden(match.font) == 0.0f);
    prFontRelease(match.font);

    // The same style gives the same instance.
    PrFontFace again = matchOne(set, r);
    CHECK(prFontId(again.font) == prFontId(match.font));
    prFontRelease(again.font);

    // Optical sizing follows the size, clamped to the axis, unless the request turns it off.
    r = request("Literata");
    r.style.size = 30;
    match = matchOne(set, r);
    CHECK(axisValue(match.font, kOpsz) == 30.0f);
    prFontRelease(match.font);
    r.style.size = 200;
    match = matchOne(set, r);
    CHECK(axisValue(match.font, kOpsz) == 72.0f);
    prFontRelease(match.font);
    r.style.flags &= ~PrFontStyleFlag_OpticalSizing;
    match = matchOne(set, r);
    CHECK(axisValue(match.font, kOpsz) == 12.0f);
    prFontRelease(match.font);

    // Italic from a face with no italic is synthesized at 14 degrees...
    r = request("Literata");
    r.style.slope = PrFontSlope_Italic;
    match = matchOne(set, r);
    CHECK(prFontOblique(match.font) == doctest::Approx(std::tan(14 * 3.14159265 / 180)));
    prFontRelease(match.font);
    r.style.flags &= ~PrFontStyleFlag_SynthesizeStyle;
    match = matchOne(set, r);
    CHECK(prFontOblique(match.font) == 0.0f);
    prFontRelease(match.font);

    // ...and from a slnt axis, as far as the axis goes, with nothing synthesized.
    r = request("Roboto Flex");
    r.style.slope = PrFontSlope_Italic;
    match = matchOne(set, r);
    CHECK(axisValue(match.font, kSlnt) == -10.0f);
    CHECK(prFontOblique(match.font) == 0.0f);
    prFontRelease(match.font);
    r.style.slope = PrFontSlope_Oblique;
    r.style.obliqueAngle = 5;
    match = matchOne(set, r);
    CHECK(axisValue(match.font, kSlnt) == -5.0f);
    prFontRelease(match.font);
    r.style.slope = PrFontSlope_Normal;
    match = matchOne(set, r);
    CHECK(axisValue(match.font, kSlnt) == 0.0f);
    prFontRelease(match.font);
    prFontSetDestroy(set);
}

TEST_CASE("matching narrows width before slope before weight") {
    PrFontSet* set = prFontSetCreate(nullptr);
    auto add = [&](float width, PrFontSlope slope, float weight) {
        PrFontFaceDescriptor d = prFontFaceDescriptorDefault();
        d.automatic = 0;
        d.family = "Test";
        d.widthMin = d.widthMax = width;
        d.weightMin = d.weightMax = weight;
        d.slope = slope;
        addFile(set, d, "NotoSans.ttf");
    };
    add(100, PrFontSlope_Normal, 400);
    add(75, PrFontSlope_Normal, 700);
    add(100, PrFontSlope_Italic, 700);
    Asked r = request("Test");
    r.style.width = 80;           // narrower first: 75, though it is the wrong weight
    r.style.weight = 400;
    PrFontFace match = matchOne(set, r);
    CHECK(axisValue(match.font, kWdth) == 75.0f);
    CHECK(axisValue(match.font, kWght) == 700.0f);
    prFontRelease(match.font);
    r.style.width = 100;
    r.style.slope = PrFontSlope_Italic;  // italic before weight
    match = matchOne(set, r);
    CHECK(axisValue(match.font, kWght) == 700.0f);
    CHECK(prFontOblique(match.font) == 0.0f);
    prFontRelease(match.font);
    prFontSetDestroy(set);
}

TEST_CASE("faces of one style dividing code points all match, the last added first, each serving its ranges") {
    PrFontSet* set = prFontSetCreate(nullptr);
    PrFontCodePointRange latin{0x0000, 0x024f};
    PrFontCodePointRange arabic{0x0600, 0x06ff};
    PrFontFaceDescriptor d = prFontFaceDescriptorDefault();
    d.automatic = PrFontFaceAutoFlag_All & ~PrFontFaceAutoFlag_Family;
    d.family = "Mixed";
    d.ranges = &latin;
    d.rangeCount = 1;
    addFile(set, d, "NotoSans.ttf");
    d.ranges = &arabic;
    d.automatic &= ~PrFontFaceAutoFlag_Emoji;
    d.flags = PrFontFaceFlag_Emoji;
    addFile(set, d, "NotoSansArabic.ttf");
    PrFontMatch* match = prFontMatchCreate();
    Asked mixed = request("Mixed");
    prFontSetMatch(set, mixed.family, &mixed.style, match);
    REQUIRE(prFontMatchFaceCount(match) == 2);
    PrFontFace matches[] = {prFontMatchFace(match, 0), prFontMatchFace(match, 1)};
    REQUIRE(matches[0].rangeCount == 1);
    CHECK(matches[0].ranges[0].first == 0x0600);
    CHECK(matches[1].ranges[0].first == 0x0000);
    CHECK(matches[0].flags == PrFontFaceFlag_Emoji);
    CHECK(matches[1].flags == 0);
    CHECK(prFontFaceServes(&matches[0], 0x0628));
    CHECK(!prFontFaceServes(&matches[0], 'a'));
    CHECK(prFontFaceServes(&matches[1], 'a'));
    CHECK(!prFontFaceServes(&matches[1], 0x0628));
    // The match keeps the ranges of its faces whatever the set does.
    prFontSetDestroy(set);
    CHECK(matches[1].ranges[0].last == 0x024f);
    prFontMatchDestroy(match);
}

TEST_CASE("a face with a color glyph for an emoji is an emoji face, unless its descriptor says otherwise") {
    PrFontSet* set = prFontSetCreate(nullptr);
    PrFontFaceDescriptor d = prFontFaceDescriptorDefault();
    d.automatic = PrFontFaceAutoFlag_All & ~PrFontFaceAutoFlag_Family;
    d.family = "Layers";
    addFile(set, d, "Twemoji.ttf");
    d.family = "Bitmaps";
    addFile(set, d, "TwemojiSmileySbix.ttf");
    d.family = "Text";
    addFile(set, d, "NotoSans.ttf");
    std::string path = fontPath("Twemoji.ttf");
    const PrFont* font = nullptr;
    REQUIRE(prFontCreateFromFile(path, 0, nullptr, &font) == PrStatus_Ok);
    d.family = "Added";
    prFontSetAdd(set, &d, font);
    prFontRelease(font);
    d.automatic &= ~PrFontFaceAutoFlag_Emoji;
    d.family = "Declared";
    addFile(set, d, "Twemoji.ttf");
    auto flagsOf = [&](const char* family) {
        PrFontFace face = matchOne(set, request(family));
        prFontRelease(face.font);
        return face.flags;
    };
    CHECK(flagsOf("Layers") == PrFontFaceFlag_Emoji);
    CHECK(flagsOf("Bitmaps") == PrFontFaceFlag_Emoji);
    CHECK(flagsOf("Text") == 0);
    CHECK(flagsOf("Added") == PrFontFaceFlag_Emoji);
    CHECK(flagsOf("Declared") == 0);
    prFontSetDestroy(set);
}

TEST_CASE("a descriptor's derivation applies over the axes matching sets") {
    PrFontSet* set = prFontSetCreate(nullptr);
    PrFontVariation heavy{kWght, 900};
    PrFontFaceDescriptor d = prFontFaceDescriptorDefault();
    d.derivation.variations = &heavy;
    d.derivation.variationCount = 1;
    d.derivation.sizeAdjust = 0.5f;
    addFile(set, d, "NotoSans.ttf");
    PrFontFace match = matchOne(set, request("Noto Sans"));
    CHECK(axisValue(match.font, kWght) == 900.0f);
    CHECK(prFontSizeAdjust(match.font) == 0.5f);
    prFontRelease(match.font);
    prFontSetDestroy(set);
}

TEST_CASE("a failed face leaves no face of its style rather than moving the match to another") {
    PrFontSet* set = prFontSetCreate(nullptr);
    PrFontFaceDescriptor d = prFontFaceDescriptorDefault();
    d.family = "Web";
    d.weightMin = d.weightMax = 400;
    prFontSetAddPending(set, &d, 1);
    d.weightMin = d.weightMax = 300;
    prFontSetAddPending(set, &d, 2);
    const PrFont* sans = loadFont("NotoSans.ttf");
    prFontSetLoaded(set, 2, sans);
    prFontSetFailed(set, 1);
    // The 400 face is still the best match, so no face matches, and the text moves to its next family.
    CHECK(matchAll(set, request("Web")).empty());
    Asked light = request("Web");
    light.style.weight = 300;
    CHECK(familyMatched(set, light) == "Noto Sans");

    // Faces of one style are one composite face: a failed one leaves the others.
    d.family = "Twice";
    d.weightMin = d.weightMax = 400;
    prFontSetAddPending(set, &d, 3);
    prFontSetAddPending(set, &d, 4);
    prFontSetLoaded(set, 3, sans);
    prFontSetFailed(set, 4);
    CHECK(familyMatched(set, request("Twice")) == "Noto Sans");
    prFontRelease(sans);
    prFontSetDestroy(set);
}

TEST_CASE("a family's faces rank in the order the caller gives, the last checked first") {
    PrFontSet* set = prFontSetCreate(nullptr);
    PrFontFaceDescriptor d = prFontFaceDescriptorDefault();
    d.family = "Web";
    prFontSetAddPending(set, &d, 1);
    prFontSetAddPending(set, &d, 2);
    const PrFont* sans = loadFont("NotoSans.ttf");
    const PrFont* serif = loadFont("NotoSerif.ttf");
    prFontSetLoaded(set, 1, sans);
    prFontSetLoaded(set, 2, serif);
    // Faces rank by when they were added, until they are ordered.
    CHECK(familyMatched(set, request("Web")) == "Noto Serif");
    uint64_t version = prFontSetVersion(set);
    const uint64_t added[] = {1, 2};
    prFontSetOrder(set, added, 2);
    CHECK(prFontSetVersion(set) == version);
    const uint64_t reversed[] = {2, 1};
    prFontSetOrder(set, reversed, 2);
    CHECK(prFontSetVersion(set) != version);
    CHECK(familyMatched(set, request("Web")) == "Noto Sans");
    prFontRelease(sans);
    prFontRelease(serif);
    prFontSetDestroy(set);
}

TEST_CASE("a removed face leaves the set, which a repeated removal and addition does not grow") {
    PrFontSet* set = prFontSetCreate(nullptr);
    PrFontFaceDescriptor d = prFontFaceDescriptorDefault();
    d.family = "Web";
    const PrFont* sans = loadFont("NotoSans.ttf");
    for (uint64_t key = 1; key <= 20; ++key) {
        prFontSetAddPending(set, &d, key);
        prFontSetLoaded(set, key, sans);
        CHECK(familyMatched(set, request("Web")) == "Noto Sans");
        CHECK(prFontSetFaceCount(set) == 1);
        prFontSetRequestLoad(set, key);
        prFontSetRemove(set, key);
        CHECK(prFontSetFaceCount(set) == 0);
        CHECK(matchAll(set, request("Web")).empty());
    }
    // A load requested before the removal is not taken after it.
    CHECK(prFontSetTakeLoads(set, nullptr, 0) == 0);
    prFontRelease(sans);
    prFontSetDestroy(set);
}

TEST_CASE("faces of one font and one derivation give one instance, whichever face serves") {
    PrFontSet* set = prFontSetCreate(nullptr);
    PrFontFaceDescriptor d = prFontFaceDescriptorDefault();
    d.family = "Web";
    d.derivation.sizeAdjust = 0.5f;
    const PrFont* sans = loadFont("NotoSans.ttf");
    prFontSetAddPending(set, &d, 1);
    prFontSetAddPending(set, &d, 2);
    prFontSetLoaded(set, 1, sans);
    prFontSetLoaded(set, 2, sans);
    PrFontFace second = matchAll(set, request("Web"))[0];
    prFontRelease(second.font);
    prFontSetRemove(set, 2);
    PrFontFace first = matchOne(set, request("Web"));
    CHECK(first.font == second.font);
    CHECK(prFontSizeAdjust(first.font) == 0.5f);
    // Another derivation of the same font is another font.
    d.derivation.sizeAdjust = 0.25f;
    d.family = "Smaller";
    prFontSetAddPending(set, &d, 3);
    prFontSetLoaded(set, 3, sans);
    PrFontFace smaller = matchOne(set, request("Smaller"));
    CHECK(smaller.font != first.font);
    prFontRelease(smaller.font);
    prFontRelease(first.font);
    prFontRelease(sans);
    prFontSetDestroy(set);
}

TEST_CASE("a family's stamp moves with what may change its match, and no other family's does") {
    PrFontSet* host = prFontSetCreate(nullptr);
    addFile(host, prFontFaceDescriptorDefault(), "NotoSans.ttf");
    addFile(host, prFontFaceDescriptorDefault(), "NotoSerif.ttf");
    PrFontSet* document = prFontSetCreate(host);
    PrFontFamily web = prFontFamilyFromName("Web");
    PrFontFamily other = prFontFamilyFromName("Other");
    PrFontFamily sans = prFontFamilyFromName("Noto Sans");
    PrFontFamily serif = prFontFamilyGeneric(PrFontGeneric_Serif);
    PrFontFaceDescriptor d = prFontFaceDescriptorDefault();
    auto moves = [&](PrFontFamily family, auto change) {
        uint64_t before = prFontSetFamilyStamp(document, family);
        change();
        return prFontSetFamilyStamp(document, family) > before;
    };

    SUBCASE("a family that matched nothing gains its first face") {
        d.family = "Web";
        CHECK(moves(web, [&] { prFontSetAddPending(document, &d, 1); }));
        CHECK_FALSE(moves(other, [&] { prFontSetAddPending(document, &d, 2); }));
    }
    SUBCASE("a generic's family gains a face, with its binding unchanged") {
        prFontSetSetGeneric(host, PrFontGeneric_Serif, "Noto Serif");
        d.family = "Noto Serif";
        CHECK(moves(serif, [&] { prFontSetAddPending(document, &d, 1); }));
        CHECK(moves(serif, [&] { prFontSetSetGeneric(document, PrFontGeneric_Serif, "Noto Sans"); }));
    }
    SUBCASE("an alias's target changes") {
        prFontSetAlias(host, "Web", "Noto Sans");
        CHECK(moves(web, [&] { prFontSetAlias(host, "Web", "Noto Serif"); }));
        d.family = "Noto Serif";
        CHECK(moves(web, [&] { prFontSetAddPending(document, &d, 1); }));
    }
    SUBCASE("a generic bound to an alias, whose target a child set defines") {
        prFontSetSetGeneric(host, PrFontGeneric_SansSerif, "Alias");
        prFontSetAlias(host, "Alias", "Web");
        d.family = "Web";
        PrFontFamily generic = prFontFamilyGeneric(PrFontGeneric_SansSerif);
        CHECK(moves(generic, [&] { prFontSetAddPending(document, &d, 1); }));
        const PrFont* font = loadFont("NotoSans.ttf");
        CHECK(moves(generic, [&] { prFontSetLoaded(document, 1, font); }));
        prFontRelease(font);
        CHECK(familyMatched(document, Asked{generic, prFontStyleDefault()}) == "Noto Sans");
        CHECK(moves(generic, [&] { prFontSetRemove(document, 1); }));
    }
    SUBCASE("a child set shadows a parent's family, then reveals it") {
        d.family = "Noto Sans";
        CHECK(moves(sans, [&] { prFontSetAddPending(document, &d, 1); }));
        CHECK(moves(sans, [&] { prFontSetRemove(document, 1); }));
        CHECK(familyMatched(document, Asked{sans, prFontStyleDefault()}) == "Noto Sans");
    }
    SUBCASE("a family's faces reorder") {
        d.family = "Web";
        prFontSetAddPending(document, &d, 1);
        prFontSetAddPending(document, &d, 2);
        const uint64_t reversed[] = {2, 1};
        CHECK(moves(web, [&] { prFontSetOrder(document, reversed, 2); }));
        CHECK_FALSE(moves(web, [&] { prFontSetOrder(document, reversed, 2); }));
    }
    SUBCASE("a face loads or fails") {
        d.family = "Web";
        prFontSetAddPending(document, &d, 1);
        prFontSetAddPending(document, &d, 2);
        const PrFont* font = loadFont("NotoSans.ttf");
        CHECK(moves(web, [&] { prFontSetLoaded(document, 1, font); }));
        CHECK(moves(web, [&] { prFontSetFailed(document, 2); }));
        CHECK_FALSE(moves(other, [&] { prFontSetFailed(document, 2); }));
        prFontRelease(font);
    }
    prFontSetDestroy(document);
    prFontSetDestroy(host);
}

TEST_CASE("fallback families are matched among the outermost set's faces, never a document's") {
    PrFontSet* host = prFontSetCreate(nullptr);
    addFile(host, prFontFaceDescriptorDefault(), "NotoSans.ttf");
    PrFontFallbackFamily fallback{"Noto Sans", 0, PrFontGeneric_None};
    prFontSetSetFallbackFamilies(host, &fallback, 1);
    PrFontSet* document = prFontSetCreate(host);
    PrFontFaceDescriptor d = prFontFaceDescriptorDefault();
    d.family = "Noto Sans";
    prFontSetAddPending(document, &d, 1);
    const PrFont* serif = loadFont("NotoSerif.ttf");
    prFontSetLoaded(document, 1, serif);
    prFontRelease(serif);
    // The family of that name in the document serves the text that names it, and not the text that falls back.
    CHECK(familyMatched(document, request("Noto Sans")) == "Noto Serif");
    PrFontMatch* match = prFontMatchCreate();
    PrFontStyle style = prFontStyleDefault();
    prFontSetMatchFallback(document, 0, &style, match);
    REQUIRE(prFontMatchFaceCount(match) == 1);
    PrString family = prFontFamilyName(prFontMatchFace(match, 0).font);
    CHECK(std::string(prStringView(&family).data, prStringView(&family).size) == "Noto Sans");
    prStringReset(&family);
    uint64_t stamp = prFontSetFallbackStamp(document);
    prFontSetRemove(document, 1);
    CHECK(prFontSetFallbackStamp(document) == stamp);
    prFontMatchDestroy(match);
    prFontSetDestroy(document);
    prFontSetDestroy(host);
}

TEST_CASE("a file added by its family is read when a match first asks for the family, and only then") {
    PrFontSet* set = prFontSetCreate(nullptr);
    std::string regular = fontPath("Arimo.ttf");
    std::string italic = fontPath("Arimo-Italic.ttf");
    std::string missing = fontPath("Missing.ttf");
    prFontSetAddFileFamily(set, "Arimo", PrStringView{regular.data(), regular.size()});
    prFontSetAddFileFamily(set, "Arimo", PrStringView{italic.data(), italic.size()});
    prFontSetAddFileFamily(set, "Arimo", PrStringView{italic.data(), italic.size()});
    prFontSetAddFileFamily(set, "Missing", PrStringView{missing.data(), missing.size()});
    CHECK(prFontSetFaceCount(set) == 3);
    CHECK(prFontSetFamilyCount(set) == 2);
    Asked asked = request("Arimo");
    asked.style.slope = PrFontSlope_Italic;
    PrFontFace face = matchOne(set, asked);
    REQUIRE(face.font != nullptr);
    CHECK(prFontInfo(face.font).slope == PrFontSlope_Italic);
    prFontRelease(face.font);
    // The missing file is not read until its family is asked for, and then gives no face.
    CHECK(prFontSetFaceCount(set) == 3);
    CHECK(matchAll(set, request("Missing")).empty());
    CHECK(prFontSetFaceCount(set) == 2);
    CHECK(prFontSetFamilyCount(set) == 2);
    prFontSetDestroy(set);
}

TEST_CASE("a file whose faces read as another family gives them the family it was added by, as automatic faces") {
    PrFontSet* set = prFontSetCreate(nullptr);
    std::string sans = fontPath("NotoSans.ttf");
    std::string emoji = fontPath("Twemoji.ttf");
    prFontSetAddFileFamily(set, "Platform Sans", PrStringView{sans.data(), sans.size()});
    prFontSetAddFileFamily(set, "Platform Emoji", PrStringView{emoji.data(), emoji.size()});
    CHECK(familyMatched(set, request("Platform Sans")) == "Noto Sans");
    PrFontFace face = matchOne(set, request("Platform Emoji"));
    CHECK(face.flags == PrFontFaceFlag_Emoji);
    prFontRelease(face.font);
    CHECK(familyMatched(set, request("Noto Sans")).empty());
    prFontSetDestroy(set);
}
