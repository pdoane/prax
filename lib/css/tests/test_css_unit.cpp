#include "pr/ascii.h"

#include "css/src/css_style.h"
#include "css/src/css_unit.h"

#include "tools/harness/test.h"

#include <cmath>
#include <initializer_list>

namespace pr_impl {

namespace {

// A measure that gives each metric its own fraction of the em, and counts the measurements.
struct Measured {
    uint32_t count = 0;
};

bool measureShares(void* PR_NULLABLE data, const CssComputedStyle*, CssFontMetric metric, bool, double* ems) {
    ++static_cast<Measured*>(data)->count;
    if (metric == CssFontMetric_Ic)
        return false;
    *ems = 0.1 * double(metric + 1);
    return true;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("every unit is found however its case is written") {
    // The tables hold the units lowercased, so `Q`, `Hz`, `kHz` and `dB` are the ones that show whether case folding
    // reaches the lookup.
    for (PrCssUnit unit = PrCssUnit_Em; unit < PrCssUnit_Unknown; ++unit) {
        CHECK(cssUnitCategory(unit) != CssUnitCategory_Unknown);
    }

    struct Case { const char* text; PrCssUnit unit; };
    for (const Case& test : {Case{"px", PrCssUnit_Px}, Case{"PX", PrCssUnit_Px},
        Case{"Px", PrCssUnit_Px}, Case{"q", PrCssUnit_Q}, Case{"Q", PrCssUnit_Q}, Case{"hz", PrCssUnit_Hz},
                Case{"Hz", PrCssUnit_Hz}, Case{"HZ", PrCssUnit_Hz}, Case{"khz", PrCssUnit_KHz},
                Case{"kHz", PrCssUnit_KHz}, Case{"KHZ", PrCssUnit_KHz}, Case{"db", PrCssUnit_DB},
                Case{"dB", PrCssUnit_DB}, Case{"dvmin", PrCssUnit_Dvmin},
                Case{"s", PrCssUnit_S}, Case{"st", PrCssUnit_St}}) {
        INFO(test.text);
        CHECK(cssUnitFromText(test.text) == test.unit);
    }
}

TEST_CASE("text no unit matches is unknown") {
    for (const char* text : {"", "p", "pxx", "zz", "font-size", "e", "\r", "1"}) {
        INFO(text);
        CHECK(cssUnitFromText(text) == PrCssUnit_Unknown);
    }
    CHECK(cssUnitCategory(PrCssUnit_Number) == CssUnitCategory_Number);
    CHECK(cssUnitCategory(PrCssUnit_Unknown) == CssUnitCategory_Unknown);
}

TEST_CASE("a change of math-depth past a double's range scales by a finite factor") {
    CssStyleContext context{};
    context.font = cssFontBases(16, 20);
    CHECK(cssMathDepthScale(context, 0, 2) == doctest::Approx(0.5041));
    CHECK(cssMathDepthScale(context, 2, 0) == doctest::Approx(1 / 0.5041));
    double up = cssMathDepthScale(context, 5000, 0);
    CHECK(std::isfinite(up));
    CHECK(up > 1);
    double down = cssMathDepthScale(context, 0, 5000);
    CHECK(std::isfinite(down));
    CHECK(down >= 0);
    CHECK(std::isfinite(cssMathDepthScale(context, -1000000000, 1000000000)));
    CHECK(std::isfinite(cssMathDepthScale(context, 1000000000, -1000000000)));
}

TEST_CASE("a font metric is measured once in the bases' font, and takes its fallback where it cannot be") {
    CssStyleContext context{};
    context.font = cssFontBases(20, 24);
    context.root = cssFontBases(10, 12);
    // With no measure, each metric is its fallback share of the em.
    CHECK(cssFontMetricPx(context, false, CssFontMetric_Ex) == 10);
    CHECK(cssFontMetricPx(context, false, CssFontMetric_Ch) == 10);
    CHECK(cssFontMetricPx(context, false, CssFontMetric_Ic) == 20);
    CHECK(cssFontMetricPx(context, true, CssFontMetric_Ic) == 10);

    Measured measured;
    CssFontMeasure measure{measureShares, &measured};
    CssFontMetrics metrics{};
    metrics.measure = &measure;
    context.metrics = &metrics;
    // A style stands for the font. ex and cap read nothing else of it.
    alignas(CssComputedStyle) unsigned char storage[sizeof(CssComputedStyle)] = {};
    const CssComputedStyle* style = reinterpret_cast<const CssComputedStyle*>(storage);
    context.font.style = style;
    // Bases with no style measure nothing, but record the read.
    CHECK(cssFontMetricPx(context, true, CssFontMetric_Ex) == 5);
    CHECK(metrics.readsRoot);
    CHECK_FALSE(metrics.readsFont);
    CHECK(measured.count == 0);
    CHECK(cssFontMetricPx(context, false, CssFontMetric_Ex) == doctest::Approx(2));
    CHECK(cssFontMetricPx(context, false, CssFontMetric_Ex) == doctest::Approx(2));
    CHECK(cssFontMetricPx(context, false, CssFontMetric_Cap) == doctest::Approx(4));
    CHECK(measured.count == 2);
    CHECK(metrics.readsFont);
}
