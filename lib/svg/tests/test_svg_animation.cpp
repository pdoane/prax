// SVG animation (svg-animations): the timing of the animation elements, which a document seeked to a time samples at
// once, read through the values that they give their targets.

#include "pr/css.h"
#include "pr/dom_xml.h"
#include "pr/svg.h"
#include "pr/svg_element.h"
#include "pr/svg_types.h"

#include "dom/src/dom_hooks.h"
#include "dom/src/dom_node.h"

#include "tools/harness/test.h"

namespace pr_impl {

namespace {

// A standalone SVG document whose outermost svg element holds `body`, and whose time a test seeks.
struct Animated {
    PrDomDocument* document;

    explicit Animated(const char* body) {
        String source = format("<svg xmlns='http://www.w3.org/2000/svg' "
            "xmlns:xlink='http://www.w3.org/1999/xlink' id='root' width='200' height='100'>{}</svg>", body);
        document = prDomParseXml(source, "image/svg+xml", nullptr);
        REQUIRE(document != nullptr);
        prCssEnable(document);
        prSvgEnable(document);
    }

    ~Animated() { prDomDocumentRelease(document); }

    PrSvgElement* byId(const char* id) {
        PrDomElement* element = prDomNodeGetElementById(prDomDocumentAsNode(document), id);
        REQUIRE(element != nullptr);
        return prSvgElementQuery(prDomElementAsEventTarget(element));
    }

    // Seeks the document to `seconds`, which samples its animations there.
    void seek(float seconds) { prSvgSvgElementSetCurrentTime(byId("root"), seconds); }

    // Returns the x that the animations give the element with `id`, or its own, at `seconds`.
    float xAt(const char* id, float seconds) {
        seek(seconds);
        PrSvgLength x{};
        REQUIRE(prSvgElementGetLength(byId(id), PrSvgAttribute_X, PrSvgLayer_Animated, &x));
        return x.value;
    }
};

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("an indefinite repeatDur repeats until the end, and one that is absent leaves the simple duration") {
    Animated d("<rect id='repeats' width='1' height='1'>"
        "<set attributeName='x' to='74' dur='1s' end='5s' repeatDur='indefinite'/></rect>"
        "<rect id='once' width='1' height='1'><set attributeName='x' to='74' dur='1s' end='5s'/></rect>");
    CHECK(d.xAt("repeats", 0.5f) == 74);
    CHECK(d.xAt("repeats", 4.5f) == 74);
    CHECK(d.xAt("repeats", 5.5f) == 0);
    CHECK(d.xAt("once", 0.5f) == 74);
    CHECK(d.xAt("once", 2) == 0);
}

TEST_CASE("min and max bound the active duration, and a min past the max ignores both") {
    Animated d("<rect id='bounded' width='1' height='1'><set attributeName='x' to='74' max='2s' end='5s'/></rect>"
        "<rect id='ignored' width='1' height='1'><set attributeName='x' to='74' min='8s' max='2s' end='5s'/></rect>");
    CHECK(d.xAt("bounded", 1) == 74);
    CHECK(d.xAt("bounded", 3) == 0);
    CHECK(d.xAt("ignored", 4) == 74);
    CHECK(d.xAt("ignored", 6) == 0);
}

TEST_CASE("the fill decides what a min that extends the active duration past its repeats shows") {
    Animated d("<rect id='removed' width='1' height='1'>"
        "<set attributeName='x' to='74' min='8s' end='2s' dur='1s' repeatCount='5' fill='remove'/></rect>"
        "<rect id='frozen' width='1' height='1'>"
        "<set attributeName='x' to='74' min='8s' end='2s' dur='1s' repeatCount='5' fill='freeze'/></rect>");
    CHECK(d.xAt("removed", 4) == 74);
    CHECK(d.xAt("removed", 6) == 0);
    CHECK(d.xAt("frozen", 6) == 74);
    CHECK(d.xAt("frozen", 9) == 74);
}

TEST_CASE("an svg's time is zero until its document is complete, which begins it where a seek put it") {
    Animated d("<rect id='r' width='1' height='1'><set attributeName='x' to='74' begin='5s'/></rect>");
    // A parser loads the document, as a page's does, until its readiness is complete before its load event.
    d.document->readiness = DomReadiness_Loading;
    PrSvgElement* root = d.byId("root");
    prSvgSvgElementSetCurrentTime(root, 6);
    CHECK(prSvgSvgElementGetCurrentTime(root) == 0);
    PrSvgLength x{};
    REQUIRE(prSvgElementGetLength(d.byId("r"), PrSvgAttribute_X, PrSvgLayer_Animated, &x));
    CHECK(x.value == 0);

    d.document->readiness = DomReadiness_Complete;
    domNotifyReadinessChanged(d.document);
    CHECK(prSvgSvgElementGetCurrentTime(root) == doctest::Approx(6));
    REQUIRE(prSvgElementGetLength(d.byId("r"), PrSvgAttribute_X, PrSvgLayer_Animated, &x));
    CHECK(x.value == 74);
}
