// Outlines: the built-in readers of glyf and CFF load the points and advances that FreeType loads, at the default
// instance and at the corners of each variable font's design space.

#include "pr/array.h"
#include "pr/float.h"
#include "pr/font.h"
#include "pr/string.h"

#include "font/src/cff.h"
#include "font/src/glyf.h"
#include "font/src/glyph_metrics.h"
#include "font/src/outline.h"
#include "font/src/sfnt.h"

#include "freetype/freetype.h"
#include "freetype/ftmm.h"
#include "tools/harness/test.h"

namespace pr_impl {

using namespace pr;

namespace {

constexpr uint32_t kMaxp = 0x6d617870;           // 'maxp'

// How far a point may lie from FreeType's, in font units. FreeType keeps a variation's points to a 64th of a unit.
constexpr float kTolerance = 0.05f;

// The most glyphs of one font that a comparison loads at each instance. A larger font is sampled evenly.
constexpr uint32_t kMaxGlyphs = 6000;

// A face as both readers open it.
struct Face {
    Array<uint8_t> bytes;
    Sfnt sfnt;
    FontGlyphMetricTables metrics;
    GlyfFace glyf;
    CffFace cff;
    bool truetype;
    uint32_t glyphCount;
    FT_Library library;
    FT_Face face;
};

// Opens the font at `path`.
void open(PrStringView path, Face& f) {
    f.bytes = harness::readFile(path);
    REQUIRE_MESSAGE(!f.bytes.isEmpty(), String(path));
    REQUIRE(sfntOpen(f.bytes.data(), f.bytes.count(), 0, &f.sfnt));
    fontGlyphMetricsOpen(f.sfnt, &f.metrics);
    f.glyphCount = sfntTable(f.sfnt, kMaxp).uint16At(4);
    f.truetype = glyfOpen(f.sfnt, &f.metrics, f.glyphCount, &f.glyf);
    REQUIRE((f.truetype || cffOpen(f.sfnt, &f.cff)));
    if (!f.truetype && cffGlyphCount(f.cff) < f.glyphCount)
        f.glyphCount = cffGlyphCount(f.cff);
    REQUIRE(FT_Init_FreeType(&f.library) == 0);
    REQUIRE(FT_New_Memory_Face(f.library, f.bytes.data(), FT_Long(f.bytes.count()), 0, &f.face) == 0);
    // One pixel per unit, so that FreeType's 26.6 coordinates are 64ths of a unit.
    REQUIRE(FT_Set_Char_Size(f.face, 0, FT_F26Dot6(f.face->units_per_EM) << 6, 72, 72) == 0);
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
uint32_t compare(Face& f, const Array<float>& coords, const String& name) {
    SfntCoords at{coords.isEmpty() ? nullptr : coords.data(), coords.count()};
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
        bool same = loaded && uint32_t(ft.n_points) == outline.points.count &&
            uint32_t(ft.n_contours) == outline.ends.count;
        for (uint32_t c = 0; same && c < outline.ends.count; ++c)
            same = uint32_t(ft.contours[c]) + 1 == outline.ends.data[c];
        uint32_t point = 0;
        for (; same && point < outline.points.count; ++point) {
            PrVec2 p = outline.points.data[point];
            bool on = (ft.tags[point] & 1) != 0;
            same = prF32Abs(float(ft.points[point].x) / 64 - p.x) <= kTolerance &&
                prF32Abs(float(ft.points[point].y) / 64 - p.y) <= kTolerance &&
                on == (outline.kinds.data[point] == OutlinePoint_On);
        }
        float ftAdvance = float(f.face->glyph->metrics.horiAdvance) / 64;
        same = same && prF32Abs(advance - ftAdvance) <= kTolerance;
        if (!same) {
            if (differing == 0) {
                String first;
                if (point > 0 && point <= outline.points.count)
                    first = format(", first differing point {}", point - 1);
                MESSAGE(name << " glyph " << glyph << ": " << outline.points.count << " points against " <<
                    ft.n_points << ", advance " << advance << " against " << ftAdvance << first);
            }
            ++differing;
        }
    }
    fontOutlineReset(outline);
    return differing;
}

// The instances to compare at: the default, and each axis at its minimum and its maximum with the others at default.
Array<Array<float>> instances(Face& f) {
    Array<Array<float>> out;
    out.emplace();
    uint32_t axes = sfntAxisCount(f.sfnt);
    for (uint32_t i = 0; i < axes; ++i) {
        PrFontAxis axis = sfntAxis(f.sfnt, i);
        const float ends[] = {axis.min, axis.max};
        for (float value : ends) {
            if (value == axis.defaultValue)
                continue;
            Array<float> design;
            for (uint32_t k = 0; k < axes; ++k)
                design.push(k == i ? value : sfntAxis(f.sfnt, k).defaultValue);
            out.push(move(design));
        }
    }
    return out;
}

// Compares the font at `file`, relative to the fonts directory.
void compareFont(PrStringView file) {
    Face f{};
    open(format("{}/{}", PR_FONTS_DIR, file), f);
    for (const Array<float>& design : instances(f)) {
        Array<float> coords(design.count());
        String name = file;
        if (!design.isEmpty()) {
            sfntNormalize(f.sfnt, design.data(), design.count(), coords.data());
            Array<FT_Fixed> fixed;
            for (float value : design) {
                fixed.push(FT_Fixed(prF32Round(value * 65536)));
                formatTo(name, " {:.6f}", value);
            }
            REQUIRE(FT_Set_Var_Design_Coordinates(f.face, FT_UInt(fixed.count()), fixed.data()) == 0);
        }
        CHECK_MESSAGE(compare(f, coords, name) == 0, name);
    }
    close(f);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("TrueType outlines load as FreeType loads them") {
    const char* const files[] = {
        "Ahem.ttf", "Arimo-Regular.ttf", "Tinos-Regular.ttf", "Tinos-Bold.ttf", "Cousine-Regular.ttf",
        "NotoSansMath-Regular.ttf", "LastResort-Regular.ttf", "NotoSansSymbols2-Regular.ttf", "BungeeSpice.ttf",
        "Twemoji.ttf",
    };
    for (const char* file : files)
        compareFont(file);
}

TEST_CASE("TrueType outlines vary as FreeType varies them") {
    const char* const files[] = {
        "NotoSans[wdth,wght].ttf", "NotoSans-Italic[wdth,wght].ttf", "NotoSerif[wdth,wght].ttf",
        "NotoSansMono[wdth,wght].ttf", "Literata.ttf", "RobotoFlex.ttf", "Nabla.ttf", "KalniaGlaze.ttf",
        "NotoSansArabic[wdth,wght].ttf", "NotoSansDevanagari[wdth,wght].ttf", "NotoSansTaiTham.ttf",
    };
    for (const char* file : files)
        compareFont(file);
}

TEST_CASE("CFF outlines load as FreeType loads them") {
    // The fonts of the web platform tests hold CID-keyed CFF.
    const char* const files[] = {
        "SFNT-CFF-Reference.otf", "CSSHWOrientationTest.otf", "CSSFWOrientationTest.otf",
        "NotoSansCJKkr-Regular-subset.otf",
    };
    for (const char* file : files)
        compareFont(file);
}

TEST_CASE("CFF2 outlines blend as FreeType blends them") {
    const char* const files[] = {"noto_handwriting-cff2_colr_1.otf", "TestCFF2VF.otf", "NotoSansJP-VF.otf"};
    for (const char* file : files)
        compareFont(file);
}
