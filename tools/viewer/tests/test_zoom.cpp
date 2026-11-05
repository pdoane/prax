// Tests of the viewer's zoom (zoom.h).

#include "tools/viewer/zoom.h"

#include "tools/harness/test.h"

using namespace viewer;

TEST_CASE("zooming in and out steps through the standard zooms") {
    CHECK(zoomIn(1) == 1.1f);
    CHECK(zoomOut(1) == 0.9f);
    CHECK(zoomIn(0.25f) == 0.33f);
    CHECK(zoomOut(0.33f) == 0.25f);
    float zoom = kZoomSteps[0];
    for (float step : kZoomSteps) {
        CHECK(zoom == step);
        zoom = zoomIn(zoom);
    }
    zoom = 5;
    for (size_t i = sizeof(kZoomSteps) / sizeof(float); i-- > 0;) {
        CHECK(zoom == kZoomSteps[i]);
        zoom = zoomOut(zoom);
    }
}

TEST_CASE("zooming stops at the smallest and largest steps") {
    CHECK(zoomIn(5) == 5);
    CHECK(zoomIn(9) == 5);
    CHECK(zoomOut(0.25f) == 0.25f);
    CHECK(zoomOut(0.1f) == 0.25f);
}

TEST_CASE("a zoom between steps moves to the step beside it") {
    CHECK(zoomIn(1.3f) == 1.5f);
    CHECK(zoomOut(1.3f) == 1.25f);
}

TEST_CASE("zooming keeps the point at the pane's center fixed") {
    // A pane 800 px high at 100% shows CSS pixels 1000 to 1800, centered on 1400.
    float scroll = anchoredScroll(1000, 800, 1, 2);
    CHECK(scroll == doctest::Approx(1200));
    CHECK(scroll + 800 / (2 * 2.0f) == doctest::Approx(1400));
    CHECK(anchoredScroll(1200, 800, 2, 1) == doctest::Approx(1000));
}

TEST_CASE("zooming out near the top keeps the offset at the top") {
    CHECK(anchoredScroll(100, 800, 1, 0.5f) == 0);
}

TEST_CASE("a scroll offset is clamped to the content") {
    CHECK(clampScroll(-10, 2000, 800) == 0);
    CHECK(clampScroll(500, 2000, 800) == 500);
    CHECK(clampScroll(1500, 2000, 800) == 1200);
    CHECK(clampScroll(300, 600, 800) == 0);
}
