// Outlines: the built-in readers of glyf and CFF load the points and advances that FreeType loads, at the default
// instance and at the corners of each variable font's design space.

#include "pr/font.h"

#include "font/src/cff.h"
#include "font/src/glyf.h"
#include "font/src/glyph_metrics.h"
#include "font/src/outline.h"
#include "font/src/sfnt.h"

#include "doctest/doctest.h"
#include "freetype/freetype.h"
#include "freetype/ftmm.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace pr_impl {

namespace {

constexpr uint32_t kMaxp = 0x6d617870;           // 'maxp'

// How far a point may lie from FreeType's, in font units. FreeType keeps a variation's points to a 64th of a unit.
constexpr float kTolerance = 0.05f;

// The most glyphs of one font that a comparison loads at each instance. A larger font is sampled evenly.
constexpr uint32_t kMaxGlyphs = 6000;

bool readFile(const std::string& path, std::vector<uint8_t>& bytes) {
    FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr)
        return false;
    uint8_t buffer[65536];
    size_t read = 0;
    while ((read = std::fread(buffer, 1, sizeof(buffer), file)) != 0)
        bytes.insert(bytes.end(), buffer, buffer + read);
    std::fclose(file);
    return true;
}

// A face as both readers open it.
struct Face {
    std::vector<uint8_t> bytes;
    Sfnt sfnt;
    FontGlyphMetricTables metrics;
    GlyfFace glyf;
    CffFace cff;
    bool truetype;
    uint32_t glyphCount;
    FT_Library library;
    FT_Face face;
};

// Opens the font at `path`, or returns false if there is no such file.
bool open(const std::string& path, Face& f) {
    if (!readFile(path, f.bytes))
        return false;
    REQUIRE(sfntOpen(f.bytes.data(), f.bytes.size(), 0, &f.sfnt));
    fontGlyphMetricsOpen(f.sfnt, &f.metrics);
    f.glyphCount = sfntTable(f.sfnt, kMaxp).uint16At(4);
    f.truetype = glyfOpen(f.sfnt, &f.metrics, f.glyphCount, &f.glyf);
    REQUIRE((f.truetype || cffOpen(f.sfnt, &f.cff)));
    if (!f.truetype && cffGlyphCount(f.cff) < f.glyphCount)
        f.glyphCount = cffGlyphCount(f.cff);
    REQUIRE(FT_Init_FreeType(&f.library) == 0);
    REQUIRE(FT_New_Memory_Face(f.library, f.bytes.data(), FT_Long(f.bytes.size()), 0, &f.face) == 0);
    // One pixel per unit, so that FreeType's 26.6 coordinates are 64ths of a unit.
    REQUIRE(FT_Set_Char_Size(f.face, 0, FT_F26Dot6(f.face->units_per_EM) << 6, 72, 72) == 0);
    return true;
}

void close(Face& f) {
    cffReset(f.cff);
    FT_Done_Face(f.face);
    FT_Done_FreeType(f.library);
}

// Loads the glyph with the built-in reader, and gets its advance. Returns false if the reader loads nothing.
bool load(Face& f, SfntCoords at, uint32_t glyph, FontGlyphOutline& outline, float* advance) {
    if (f.truetype) {
        GlyfPhantom phantom{};
        bool loaded = glyfLoad(f.glyf, at, glyph, &outline, &phantom);
        *advance = phantom.horizontalAdvance.x - phantom.horizontalOrigin.x;
        return loaded;
    }
    int32_t units = 0;
    int32_t lsb = 0;
    fontGlyphMetricsHorizontal(f.metrics, at, glyph, &units, &lsb);
    *advance = float(units);
    return cffLoad(f.cff, at, glyph, &outline);
}

// Returns the number of glyphs whose outline or advance differs from FreeType's at the instance, and reports the first.
uint32_t compare(Face& f, const std::vector<float>& coords, const std::string& name) {
    SfntCoords at{coords.empty() ? nullptr : coords.data(), uint32_t(coords.size())};
    FontGlyphOutline outline{};
    uint32_t differing = 0;
    uint32_t step = f.glyphCount > kMaxGlyphs ? f.glyphCount / kMaxGlyphs + 1 : 1;
    for (uint32_t glyph = 0; glyph < f.glyphCount; glyph += step) {
        float advance = 0;
        bool loaded = load(f, at, glyph, outline, &advance);
        if (FT_Load_Glyph(f.face, glyph, FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP) != 0) {
            differing += loaded ? 1 : 0;
            continue;
        }
        const FT_Outline& ft = f.face->glyph->outline;
        bool same = loaded && uint32_t(ft.n_points) == outline.points.count
                    && uint32_t(ft.n_contours) == outline.ends.count;
        for (uint32_t c = 0; same && c < outline.ends.count; ++c)
            same = uint32_t(ft.contours[c]) + 1 == outline.ends.data[c];
        uint32_t point = 0;
        for (; same && point < outline.points.count; ++point) {
            PrVec2 p = outline.points.data[point];
            bool on = (ft.tags[point] & 1) != 0;
            same = std::fabs(float(ft.points[point].x) / 64 - p.x) <= kTolerance
                   && std::fabs(float(ft.points[point].y) / 64 - p.y) <= kTolerance
                   && on == (outline.kinds.data[point] == OutlinePoint_On);
        }
        float ftAdvance = float(f.face->glyph->metrics.horiAdvance) / 64;
        same = same && std::fabs(advance - ftAdvance) <= kTolerance;
        if (!same) {
            if (differing == 0) {
                MESSAGE(name << " glyph " << glyph << ": " << outline.points.count << " points against "
                              << ft.n_points << ", advance " << advance << " against " << ftAdvance
                              << (point > 0 && point <= outline.points.count
                                      ? ", first differing point " + std::to_string(point - 1)
                                      : std::string()));
            }
            ++differing;
        }
    }
    fontOutlineReset(outline);
    return differing;
}

// The instances to compare at: the default, and each axis at its minimum and its maximum with the others at default.
std::vector<std::vector<float>> instances(Face& f) {
    std::vector<std::vector<float>> out{{}};
    uint32_t axes = sfntAxisCount(f.sfnt);
    for (uint32_t i = 0; i < axes; ++i) {
        PrFontAxis axis = sfntAxis(f.sfnt, i);
        for (float value : {axis.min, axis.max}) {
            if (value == axis.defaultValue)
                continue;
            std::vector<float> design;
            for (uint32_t k = 0; k < axes; ++k)
                design.push_back(k == i ? value : sfntAxis(f.sfnt, k).defaultValue);
            out.push_back(design);
        }
    }
    return out;
}

// Compares the font at `file`, relative to the fonts directory. A font that the checkout lacks is skipped.
void compareFont(const std::string& file) {
    Face f{};
    if (!open(std::string(PR_FONTS_DIR) + "/" + file, f))
        return;
    for (const std::vector<float>& design : instances(f)) {
        std::vector<float> coords(design.size());
        std::string name = file;
        if (!design.empty()) {
            sfntNormalize(f.sfnt, design.data(), uint32_t(design.size()), coords.data());
            std::vector<FT_Fixed> fixed;
            for (float value : design) {
                fixed.push_back(FT_Fixed(std::lround(value * 65536)));
                name += " " + std::to_string(value);
            }
            REQUIRE(FT_Set_Var_Design_Coordinates(f.face, FT_UInt(fixed.size()), fixed.data()) == 0);
        }
        CHECK_MESSAGE(compare(f, coords, name) == 0, name);
    }
    close(f);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("TrueType outlines load as FreeType loads them") {
    for (const char* file : {"Ahem.ttf", "Tinos.ttf", "Tinos-Bold.ttf", "Cousine.ttf", "NotoSansMath.ttf",
                             "host/LastResort-Regular.ttf", "NotoSansSymbols2.ttf", "BungeeSpice.ttf", "Twemoji.ttf"})
        compareFont(file);
}

TEST_CASE("TrueType outlines vary as FreeType varies them") {
    for (const char* file : {"NotoSans.ttf", "NotoSans-Italic.ttf", "NotoSerif.ttf", "NotoSansMono.ttf", "Arimo.ttf",
                             "Literata.ttf", "RobotoFlex.ttf", "Nabla.ttf", "KalniaGlaze.ttf", "NotoSansArabic.ttf",
                             "NotoSansDevanagari.ttf", "NotoSansTaiTham.ttf"})
        compareFont(file);
}

TEST_CASE("CFF outlines load as FreeType loads them") {
    // The fonts of the web platform tests, if the checkout has them, hold CID-keyed CFF.
    for (const char* file : {"woff2/SFNT-CFF-Reference.otf", "../wpt/fonts/adobe-fonts/CSSHWOrientationTest.otf",
                             "../wpt/fonts/adobe-fonts/CSSFWOrientationTest.otf",
                             "../wpt/fonts/noto/cjk-kr/NotoSansCJKkr-Regular-subset.otf"})
        compareFont(file);
}

TEST_CASE("CFF2 outlines blend as FreeType blends them") {
    for (const char* file : {"HandwritingCff2.otf", "host/NotoSansJP-VF.otf", "host/NotoSerifKR-VF.otf"})
        compareFont(file);
}
