#include "dom/src/dom_element_state.h"
#include "dom/src/dom_select.h"

#include "tools/harness/test.h"

namespace pr_impl {

namespace {

PrDomElement* html(PrDomDocument* document, PrDomTag tag) {
    return domElementCreate(document, PrDomNamespace_Html, tag, PrStringView{});
}

PrDomElement* option(PrDomDocument* document, PrStringView text, bool selected = false) {
    PrDomElement* element = html(document, PrDomTag_Option);
    if (selected)
        domSetAttribute(element, PrDomNamespace_None, "selected", "");
    domInsert(element, domTextCreate(document, text), nullptr);
    return element;
}

// A document whose root is a select, connected.
struct Select {
    PrDomDocument* document = domDocumentCreate(DomDocumentKind_Html);
    PrDomElement* select = html(document, PrDomTag_Select);

    Select() { domInsert(document, select, nullptr); }
    ~Select() { domNodeRelease(document); }
};

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a select of display size 1 selects its first enabled option where none is") {
    Select s;
    PrDomElement* disabled = option(s.document, "a");
    domSetAttribute(disabled, PrDomNamespace_None, "disabled", "");
    PrDomElement* first = option(s.document, "b");
    domInsert(s.select, disabled, nullptr);
    domInsert(s.select, first, nullptr);
    CHECK(!domOptionSelected(disabled));
    CHECK(domOptionSelected(first));
    CHECK(domElementCheckedness(first) == DomCheckedFlag_Checked);

    // An option with the selected attribute takes the selection, and a removed one gives it back.
    PrDomElement* chosen = option(s.document, "c", true);
    domInsert(s.select, chosen, nullptr);
    CHECK(!domOptionSelected(first));
    CHECK(domOptionSelected(chosen));
    // A removed node that nothing references is freed, so the test holds this one.
    domNodeRetain(chosen);
    domRemove(chosen);
    CHECK(domOptionSelected(first));
    // Outside a select, an option's selectedness follows its attribute.
    CHECK(domOptionSelected(chosen));
    domNodeRelease(chosen);
}

TEST_CASE("the selected attribute moves the selection of a select without multiple") {
    Select s;
    PrDomElement* a = option(s.document, "a");
    PrDomElement* b = option(s.document, "b");
    domInsert(s.select, a, nullptr);
    domInsert(s.select, b, nullptr);
    domSetAttribute(b, PrDomNamespace_None, "selected", "");
    CHECK(!domOptionSelected(a));
    CHECK(domOptionSelected(b));

    // With multiple, each option keeps its own selectedness, and none is chosen for it.
    Select multiple;
    domSetAttribute(multiple.select, PrDomNamespace_None, "multiple", "");
    PrDomElement* c = option(multiple.document, "c", true);
    PrDomElement* d = option(multiple.document, "d", true);
    PrDomElement* e = option(multiple.document, "e");
    domInsert(multiple.select, c, nullptr);
    domInsert(multiple.select, d, nullptr);
    domInsert(multiple.select, e, nullptr);
    CHECK(domOptionSelected(c));
    CHECK(domOptionSelected(d));
    CHECK(!domOptionSelected(e));
}

TEST_CASE("an option in a nested optgroup, a datalist or another option is in no select") {
    Select s;
    PrDomElement* outer = html(s.document, PrDomTag_Optgroup);
    PrDomElement* inner = html(s.document, PrDomTag_Optgroup);
    PrDomElement* nested = option(s.document, "a");
    domInsert(s.select, outer, nullptr);
    domInsert(outer, inner, nullptr);
    domInsert(inner, nested, nullptr);
    CHECK(domNearestAncestorSelect(nested) == nullptr);
    CHECK(!domOptionSelected(nested));

    PrDomElement* grouped = option(s.document, "b");
    domInsert(outer, grouped, nullptr);
    CHECK(domNearestAncestorSelect(grouped) == s.select);
    CHECK(domOptionSelected(grouped));
}

TEST_CASE("a selectedcontent reflects its select's selected option") {
    Select s;
    PrDomElement* button = html(s.document, PrDomTag_Button);
    PrDomElement* content = html(s.document, PrDomTag_Selectedcontent);
    domInsert(button, content, nullptr);
    PrDomElement* a = option(s.document, "a");
    domInsert(s.select, a, nullptr);
    domInsert(s.select, button, s.select->first);
    REQUIRE(content->first != nullptr);
    CHECK(domIsText(content->first));

    // A second selectedcontent is not the select's, and is emptied. Removing the first leaves the select's update to
    // the one that is now first.
    PrDomElement* second = html(s.document, PrDomTag_Selectedcontent);
    domInsert(second, domTextCreate(s.document, "stale"), nullptr);
    domInsert(button, second, nullptr);
    CHECK(second->first == nullptr);
    domRemove(content);
    REQUIRE(second->first != nullptr);
    CHECK(domIsText(second->first));
}
