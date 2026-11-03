#include "dom/src/dom_node.h"

#include "doctest/doctest.h"

using namespace pr_impl;

TEST_CASE("a tag collection follows the tree as it changes, and leaves out what is detached or in a template") {
    PrDomDocument* document = domDocumentCreate(DomDocumentKind_Html);
    PrDomElement* root = domElementCreate(document, PrDomNamespace_Html, PrDomTag_Div, "");
    PrDomElement* nested = domElementCreate(document, PrDomNamespace_Html, PrDomTag_Div, "");
    PrDomElement* sibling = domElementCreate(document, PrDomNamespace_Html, PrDomTag_Div, "");
    PrDomElement* detached = domElementCreate(document, PrDomNamespace_Html, PrDomTag_Div, "");
    PrDomElement* templ = domElementCreate(document, PrDomNamespace_Html, PrDomTag_Template, "");
    domAttach(document, root, nullptr);
    domAttach(root, nested, nullptr);
    domAttach(document, sibling, nullptr);
    domAttach(document, templ, nullptr);
    domAttach(domElementContents(templ), detached, nullptr);
    PrDomDocument* handle = document;
    PrDomHtmlCollection* collection = prDomHtmlCollectionCreateByTagName(prDomDocumentAsNode(handle), "DIV");
    CHECK(prDomHtmlCollectionLength(collection) == 3);
    CHECK(prDomHtmlCollectionElementAt(collection, 0) == root);
    CHECK(prDomHtmlCollectionElementAt(collection, 1) == nested);
    CHECK(prDomHtmlCollectionElementAt(collection, 2) == sibling);
    CHECK(prDomHtmlCollectionElementAt(collection, 3) == nullptr);
    domDetach(nested);
    CHECK(prDomHtmlCollectionLength(collection) == 2);
    CHECK(prDomHtmlCollectionElementAt(collection, 1) == sibling);
    domAttach(document, nested, root);
    CHECK(prDomHtmlCollectionElementAt(collection, 0) == nested);
    PrDomDocument* destination = domDocumentCreate(DomDocumentKind_Html);
    domDetach(root);
    domAdopt(destination, root);
    CHECK(prDomHtmlCollectionLength(collection) == 2);
    prDomHtmlCollectionDestroy(collection);
    prDomHtmlCollectionDestroy(nullptr);
    domNodeRelease(document);
    domNodeRelease(destination);
}

TEST_CASE("a foreign element matches its tag name as written, and a named item is the first in tree order") {
    PrDomDocument* document = domDocumentCreate(DomDocumentKind_Html);
    PrDomDocument* handle = document;
    PrDomElement* html = domElementCreate(document, PrDomNamespace_Html, PrDomTag_Div, "");
    PrDomElement* foreign = domElementCreate(document, PrDomNamespace_Svg, PrDomTag_Unknown, "DIV");
    domAttach(document, html, nullptr);
    domAttach(document, foreign, nullptr);
    domSetAttribute(html, PrDomNamespace_None, "name", "match");
    domSetAttribute(foreign, PrDomNamespace_None, "id", "match");
    domSetAttribute(foreign, PrDomNamespace_None, "name", "foreign-name");
    char requested[] = "DIV";
    PrDomHtmlCollection* upper = prDomHtmlCollectionCreateByTagName(prDomDocumentAsNode(handle), PrStringView{requested, 3});
    requested[0] = 'x';
    PrDomHtmlCollection* lower = prDomHtmlCollectionCreateByTagName(prDomDocumentAsNode(handle), "div");
    PrDomHtmlCollection* all = prDomHtmlCollectionCreateByTagName(prDomDocumentAsNode(handle), "*");
    PrDomHtmlCollection* empty = prDomHtmlCollectionCreateByTagName(prDomDocumentAsNode(handle), "");
    CHECK(prDomHtmlCollectionLength(upper) == 2);
    CHECK(prDomHtmlCollectionLength(lower) == 1);
    CHECK(prDomHtmlCollectionLength(all) == 2);
    CHECK(prDomHtmlCollectionLength(empty) == 0);
    CHECK(prDomHtmlCollectionNamedItem(all, "match") == html);
    CHECK(prDomHtmlCollectionNamedItem(all, "foreign-name") == nullptr);
    CHECK(prDomHtmlCollectionNamedItem(all, "") == nullptr);
    domSetAttribute(html, PrDomNamespace_None, "name", "changed");
    CHECK(prDomHtmlCollectionNamedItem(all, "match") == foreign);
    CHECK(prDomHtmlCollectionNamedItem(lower, "match") == nullptr);
    prDomHtmlCollectionDestroy(upper);
    prDomHtmlCollectionDestroy(lower);
    prDomHtmlCollectionDestroy(all);
    prDomHtmlCollectionDestroy(empty);
    domNodeRelease(document);
}

TEST_CASE("a token list reads a value short enough to be held inline, and toggles by it") {
    PrDomDocument* document = domDocumentCreate(DomDocumentKind_Html);
    PrDomElement* element = domElementCreate(document, PrDomNamespace_Html, PrDomTag_Div, "");
    domSetAttribute(element, PrDomNamespace_None, "class", "a b");
    PrDomTokenList* list = prDomTokenListCreate(element, "class");
    bool on = false;
    const bool yes = true;
    const bool no = false;
    CHECK(prDomTokenListToggle(list, "a", &yes, &on) == PrStatus_Ok);
    CHECK(on);
    CHECK(domAttributeValue(element, "class") == "a b");
    CHECK(prDomTokenListToggle(list, "c", nullptr, &on) == PrStatus_Ok);
    CHECK(on);
    CHECK(domAttributeValue(element, "class") == "a b c");
    CHECK(prDomTokenListToggle(list, "a", &no, &on) == PrStatus_Ok);
    CHECK(!on);
    CHECK(domAttributeValue(element, "class") == "b c");
    prDomTokenListDestroy(list);
    domNodeRelease(document);
}
