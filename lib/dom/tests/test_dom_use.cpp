// The shadow trees of SVG's use elements: their instances, kept up to date with what the uses reference.

#include "pr/dom.h"
#include "pr/string_ops.h"

#include "dom/src/dom_node.h"
#include "dom/src/dom_node_leaves.h"
#include "dom/src/dom_use.h"

#include "doctest/doctest.h"

namespace pr_impl {

namespace {

constexpr const char* kSvg = "http://www.w3.org/2000/svg";

// A document whose root is an svg element.
struct UseDocument {
    PrDomDocument* document = prDomDocumentCreate();
    PrDomElement* svg = make("svg");

    UseDocument() {
        REQUIRE(prDomNodeAppendChild(prDomDocumentAsNode(document), prDomElementAsNode(svg)) == PrStatus_Ok);
    }

    ~UseDocument() { prDomDocumentRelease(document); }

    // Creates an element of SVG named `name`, with the ID `id` if it is given, and appends it to `parent`.
    PrDomElement* add(PrDomElement* parent, const char* name, const char* id = nullptr) {
        PrDomElement* element = make(name);
        if (id != nullptr)
            prDomElementSetAttribute(element, "id", id);
        REQUIRE(prDomNodeAppendChild(prDomElementAsNode(parent), prDomElementAsNode(element)) == PrStatus_Ok);
        return element;
    }

    PrDomElement* make(const char* name) {
        PrDomElement* element = nullptr;
        REQUIRE(prDomElementCreateNs(document, kSvg, name, nullptr, &element) == PrStatus_Ok);
        return element;
    }

    // Returns the instance root of `use`, brought up to date.
    PrDomElement* instanceOf(PrDomElement* use) {
        domUsesSync(document);
        const PrDomShadowRoot* shadow = domShadowRootOfHost(use);
        return shadow != nullptr ? domElementFromNode(shadow->first) : nullptr;
    }
};

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a use element hosts a closed shadow root holding instances of what it references") {
    UseDocument d;
    PrDomElement* g = d.add(d.svg, "g", "g");
    PrDomElement* rect = d.add(g, "rect");
    prDomElementSetAttribute(rect, "width", "10");
    PrDomElement* use = d.add(d.svg, "use");
    prDomElementSetAttribute(use, "href", "#g");
    PrDomElement* root = d.instanceOf(use);
    REQUIRE(root != nullptr);
    CHECK(domElementName(root) == "g");
    CHECK(domShadowRootOfHost(use)->mode == PrDomShadowRootMode_Closed);
    CHECK(domInUseShadowTree(root));
    PrDomElement* instance = domElementFromNode(root->first);
    REQUIRE(instance != nullptr);
    CHECK(domAttributeValue(instance, "width") == "10");
}

TEST_CASE("an instance keeps its identity as its original's attributes change and its siblings leave") {
    UseDocument d;
    PrDomElement* g = d.add(d.svg, "g", "g");
    d.add(g, "rect");
    PrDomElement* text = d.add(g, "text");
    PrDomElement* use = d.add(d.svg, "use");
    prDomElementSetAttribute(use, "href", "#g");
    PrDomElement* root = d.instanceOf(use);
    REQUIRE(root != nullptr);
    PrDomNode* rect = root->first;
    prDomElementSetAttribute(g, "class", "on");
    CHECK(d.instanceOf(use) == root);
    CHECK(domAttributeValue(root, "class") == "on");
    CHECK(root->first == rect);
    prDomNodeRemove(prDomElementAsNode(text));
    CHECK(d.instanceOf(use) == root);
    CHECK(root->first == rect);
    CHECK(rect->next == nullptr);
    // A sibling inserted before the rect leaves the rect's instance in place too.
    PrDomElement* circle = d.make("circle");
    REQUIRE(prDomNodeInsertBefore(prDomElementAsNode(g), prDomElementAsNode(circle), g->first) == PrStatus_Ok);
    CHECK(d.instanceOf(use) == root);
    CHECK(rect->previous != nullptr);
    CHECK(root->first->next == rect);
}

TEST_CASE("a use's instances follow its href, and a reference that makes a cycle instances nothing") {
    UseDocument d;
    d.add(d.svg, "rect", "a");
    d.add(d.svg, "circle", "b");
    PrDomElement* use = d.add(d.svg, "use");
    prDomElementSetAttribute(use, "href", "#a");
    REQUIRE(d.instanceOf(use) != nullptr);
    CHECK(domElementName(d.instanceOf(use)) == "rect");
    prDomElementSetAttribute(use, "href", "#b");
    CHECK(domElementName(d.instanceOf(use)) == "circle");
    // A group that holds a use of itself.
    PrDomElement* g = d.add(d.svg, "g", "self");
    PrDomElement* inner = d.add(g, "use");
    prDomElementSetAttribute(inner, "href", "#self");
    CHECK(d.instanceOf(inner) == nullptr);
    // Two uses that reference each other's groups: the outer instances the inner, whose instance would repeat.
    PrDomElement* x = d.add(d.svg, "g", "x");
    PrDomElement* y = d.add(d.svg, "g", "y");
    PrDomElement* toY = d.add(x, "use");
    prDomElementSetAttribute(toY, "href", "#y");
    PrDomElement* toX = d.add(y, "use");
    prDomElementSetAttribute(toX, "href", "#x");
    PrDomElement* instance = d.instanceOf(toY);
    REQUIRE(instance != nullptr);
    PrDomElement* nested = domElementFromNode(instance->first);
    REQUIRE(nested != nullptr);
    const PrDomShadowRoot* shadow = domShadowRootOfHost(nested);
    REQUIRE(shadow != nullptr);
    PrDomElement* x2 = domElementFromNode(shadow->first);
    REQUIRE(x2 != nullptr);
    const PrDomShadowRoot* inner2 = domShadowRootOfHost(domElementFromNode(x2->first));
    CHECK((inner2 == nullptr || inner2->first == nullptr));
}

TEST_CASE("a use's instances follow the first element with its ID as elements gain, lose, and move with it") {
    UseDocument d;
    PrDomElement* rect = d.add(d.svg, "rect", "a");
    PrDomElement* circle = d.add(d.svg, "circle", "a");
    PrDomElement* use = d.add(d.svg, "use");
    prDomElementSetAttribute(use, "href", "#a");
    REQUIRE(d.instanceOf(use) != nullptr);
    CHECK(domElementName(d.instanceOf(use)) == "rect");
    // The circle moves before the rectangle, within the tree.
    REQUIRE(prDomNodeInsertBefore(prDomElementAsNode(d.svg), prDomElementAsNode(circle), prDomElementAsNode(rect)) ==
        PrStatus_Ok);
    CHECK(domElementName(d.instanceOf(use)) == "circle");
    // An ellipse that is inserted first.
    PrDomElement* ellipse = d.make("ellipse");
    prDomElementSetAttribute(ellipse, "id", "a");
    REQUIRE(prDomNodeInsertBefore(prDomElementAsNode(d.svg), prDomElementAsNode(ellipse), prDomElementAsNode(circle)) ==
        PrStatus_Ok);
    CHECK(domElementName(d.instanceOf(use)) == "ellipse");
    REQUIRE(prDomNodeRemoveChild(prDomElementAsNode(d.svg), prDomElementAsNode(ellipse)) == PrStatus_Ok);
    CHECK(domElementName(d.instanceOf(use)) == "circle");
    prDomElementSetAttribute(circle, "id", "b");
    CHECK(domElementName(d.instanceOf(use)) == "rect");
    // An element that has no ID that a use names leaves the use as it was.
    PrDomElement* before = d.instanceOf(use);
    d.add(d.svg, "path", "c");
    CHECK(d.instanceOf(use) == before);
}

TEST_CASE("a use of an element that is not in SVG instances nothing, and a script is not instanced") {
    UseDocument d;
    PrDomElement* html = nullptr;
    REQUIRE(prDomElementCreateNs(d.document, "http://www.w3.org/1999/xhtml", "div", nullptr, &html) == PrStatus_Ok);
    prDomElementSetAttribute(html, "id", "h");
    REQUIRE(prDomNodeAppendChild(prDomElementAsNode(d.svg), prDomElementAsNode(html)) == PrStatus_Ok);
    PrDomElement* use = d.add(d.svg, "use");
    prDomElementSetAttribute(use, "href", "#h");
    CHECK(d.instanceOf(use) == nullptr);
    PrDomElement* g = d.add(d.svg, "g", "g");
    d.add(g, "script");
    d.add(g, "rect");
    prDomElementSetAttribute(use, "href", "#g");
    PrDomElement* root = d.instanceOf(use);
    REQUIRE(root != nullptr);
    REQUIRE(root->first != nullptr);
    CHECK(domElementName(domElementFromNode(root->first)) == "rect");
    CHECK(root->first->next == nullptr);
}
