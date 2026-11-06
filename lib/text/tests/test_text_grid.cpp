// Text grids: text set in cells, as a terminal sets it.

#include "pr/float.h"
#include "pr/font_set.h"
#include "pr/text_grid.h"
#include "pr/text_harfbuzz.h"

#include "tools/harness/test.h"

#include <string>
#include <vector>

namespace pr_impl {

namespace {

// A grid in Cousine, a monospaced face, with Noto Sans JP and Noto Sans Hebrew to fall back on.
struct Grid {
    PrFontSet* fonts = prFontSetCreate(nullptr);
    PrTextShaper* shaper = prTextHarfBuzzCreate();
    PrTextFont* font = nullptr;
    PrTextGrid* grid = prTextGridCreate();
    PrTextGridMetrics metrics{};
    std::string text;

    Grid() {
        for (const char* name : {"Cousine.ttf", "NotoSansJP.ttf", "NotoSansHebrew.ttf"}) {
            std::string path = std::string(PR_FONTS_DIR) + "/" + name;
            const PrFont* PR_NULLABLE face = nullptr;
            REQUIRE(prFontCreateFromFile(path, 0, nullptr, &face) == PrStatus_Ok);
            PrFontFaceDescriptor descriptor = prFontFaceDescriptorDefault();
            prFontSetAdd(fonts, &descriptor, face);
            prFontRelease(face);
        }
        PrFontFamily families[] = {
            prFontFamilyFromName("Cousine"),
            prFontFamilyFromName("Noto Sans JP"),
            prFontFamilyFromName("Noto Sans Hebrew"),
        };
        PrFontStyle style = prFontStyleDefault();
        style.size = 14;
        PrTextFontDesc desc{.set = fonts, .families = families, .familyCount = 3, .style = style};
        font = prTextFontCreate(&desc);
        metrics = prTextGridMetrics(font, 2, 1);
    }

    ~Grid() {
        prTextGridDestroy(grid);
        prTextFontRelease(font);
        prTextShaperDestroy(shaper);
        prFontSetDestroy(fonts);
    }

    // Sets `clusters` from column zero, one cell each. `wide` gives the columns of each cell and `groups` its group. A
    // cell past the end of either list spans one column and has group zero.
    void set(const std::vector<std::string>& clusters, const std::vector<uint32_t>& wide = {},
            const std::vector<uint32_t>& groups = {}) {
        text.clear();
        std::vector<PrTextGridCell> cells;
        uint32_t column = 0;
        for (size_t i = 0; i < clusters.size(); ++i) {
            uint32_t columns = i < wide.size() ? wide[i] : 1;
            uint32_t group = i < groups.size() ? groups[i] : 0;
            cells.push_back(PrTextGridCell{
                .start = uint32_t(text.size()),
                .column = column,
                .columns = columns,
                .group = group,
            });
            text += clusters[i];
            column += columns;
        }
        PrTextFont* fontList[] = {font};
        PrTextGridDesc desc{
            .text = text,
            .cells = cells.data(),
            .cellCount = uint32_t(cells.size()),
            .fonts = fontList,
            .fontCount = 1,
            .shaper = shaper,
            .pixelRatio = 2,
            .cellWidth = metrics.cell.x,
            .baseline = metrics.baseline,
        };
        prTextGridSet(grid, &desc);
    }

    // The x of each glyph, in the order of the runs.
    std::vector<float> positions() const {
        std::vector<float> out;
        for (uint32_t r = 0; r < prTextGridRunCount(grid); ++r) {
            PrFontGlyphRun run = prTextGridRun(grid, r);
            for (uint32_t g = 0; g < run.glyphCount; ++g)
                out.push_back(run.glyphs[g].position.x);
        }
        return out;
    }
};

bool isWholePixels(float value, float ratio) {
    return prF32Abs(value * ratio - prF32Round(value * ratio)) < 1e-4f;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("text grid: cells are as wide as a digit and as tall as a line, in whole target pixels") {
    Grid g;
    CHECK(g.metrics.cell.x > 7);
    CHECK(g.metrics.cell.x < 10);
    CHECK(g.metrics.cell.y > 14);
    CHECK(isWholePixels(g.metrics.cell.x, 2));
    CHECK(isWholePixels(g.metrics.cell.y, 2));
    CHECK(isWholePixels(g.metrics.baseline, 2));
    CHECK(g.metrics.baseline > 0);
    CHECK(g.metrics.baseline < g.metrics.cell.y);
    CHECK(g.metrics.underlineTop > g.metrics.baseline);
    CHECK(g.metrics.underlineTop + g.metrics.underlineThickness <= g.metrics.cell.y);
    CHECK(g.metrics.strikeoutTop < g.metrics.baseline);
    CHECK(g.metrics.underlineThickness >= 0.5f);
}

TEST_CASE("text grid: each cluster sits in its column, on the baseline") {
    Grid g;
    g.set({"a", "b", "c", "W"});
    REQUIRE(prTextGridRunCount(g.grid) == 1);
    std::vector<float> x = g.positions();
    REQUIRE(x.size() == 4);
    PrFontGlyphRun run = prTextGridRun(g.grid, 0);
    for (uint32_t i = 0; i < 4; ++i) {
        CAPTURE(i);
        // A monospaced face's advance is nearly the cell's, so each glyph starts near its column's left.
        CHECK(prF32Abs(x[i] - float(i) * g.metrics.cell.x) < 0.5f);
        CHECK(run.glyphs[i].position.y == g.metrics.baseline);
    }
}

TEST_CASE("text grid: a wide cluster centers in its two columns, in the face that it falls back to") {
    Grid g;
    g.set({"a", "\xe4\xb8\xad", "b"}, {1, 2, 1});
    // Cousine sets the Latin letters and Noto Sans JP the ideograph, which splits the runs.
    REQUIRE(prTextGridRunCount(g.grid) == 3);
    CHECK(prTextGridRun(g.grid, 0).font != prTextGridRun(g.grid, 1).font);
    std::vector<float> x = g.positions();
    REQUIRE(x.size() == 3);
    float cell = g.metrics.cell.x;
    // The ideograph is an em wide, which is narrower than two cells of a face whose advance is about 0.6 em.
    CHECK(x[1] > cell);
    CHECK(x[1] < 2 * cell);
    CHECK(prF32Abs(x[2] - 3 * cell) < 0.5f);
}

TEST_CASE("text grid: a cluster wider than its cell scales down to fit it, about the middle of its face's line") {
    Grid g;
    g.set({"a", "\xe4\xb8\xad", "b"});
    REQUIRE(prTextGridRunCount(g.grid) == 3);
    // The letters keep their size.
    CHECK(prTextGridRun(g.grid, 0).transforms == nullptr);
    PrFontGlyphRun run = prTextGridRun(g.grid, 1);
    REQUIRE(run.transforms != nullptr);
    float cell = g.metrics.cell.x;
    float scale = run.transforms[0].a;
    CHECK(run.transforms[0].d == scale);
    // The ideograph is an em wide, so it scales down to fill its one cell, starting at the cell's left.
    CHECK(prF32Abs(scale * run.size - cell) < 0.05f);
    CHECK(prF32Abs(run.glyphs[0].position.x - cell) < 1e-3f);
    // The middle of the face's line moves to the middle of the line of Cousine, the first available face. The cells
    // center Cousine's line.
    auto middle = [](const PrFont* font, float size) {
        PrFontMetrics metrics = prFontMetrics(font);
        return (metrics.ascent - metrics.descent) / 2 * size;
    };
    float line = middle(prTextGridRun(g.grid, 0).font, run.size);
    CHECK(prF32Abs(run.glyphs[0].position.y - middle(run.font, run.size) * scale - (g.metrics.baseline - line)) < 1e-3f);
    std::vector<float> x = g.positions();
    REQUIRE(x.size() == 3);
    CHECK(prF32Abs(x[2] - 2 * cell) < 0.5f);
}

TEST_CASE("text grid: a right-to-left script keeps the order of its cells") {
    Grid g;
    g.set({"\xd7\xa9", "\xd7\x9c", "\xd7\x95", "\xd7\x9d"});
    std::vector<float> x = g.positions();
    REQUIRE(x.size() == 4);
    for (uint32_t i = 1; i < 4; ++i)
        CHECK(x[i] > x[i - 1]);
    CHECK(x[0] < g.metrics.cell.x);
}

TEST_CASE("text grid: a run holds cells of one group, which the caller reads back") {
    Grid g;
    g.set({"a", "b", "c", "d"}, {}, {7, 7, 9, 7});
    REQUIRE(prTextGridRunCount(g.grid) == 3);
    CHECK(prTextGridRunGroup(g.grid, 0) == 7);
    CHECK(prTextGridRun(g.grid, 0).glyphCount == 2);
    CHECK(prTextGridRunGroup(g.grid, 1) == 9);
    CHECK(prTextGridRunGroup(g.grid, 2) == 7);
    std::vector<float> x = g.positions();
    REQUIRE(x.size() == 4);
    CHECK(prF32Abs(x[3] - 3 * g.metrics.cell.x) < 0.5f);
}

TEST_CASE("text grid: an empty row has no runs") {
    Grid g;
    g.set({});
    CHECK(prTextGridRunCount(g.grid) == 0);
}
