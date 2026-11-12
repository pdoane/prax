#include "pr/float.h"

#include "dom/src/dom_control.h"
#include "dom/src/dom_element_state.h"
#include "dom/src/dom_microsyntax.h"

#include "tools/harness/test.h"

namespace pr_impl {

namespace {

PrDomElement* html(PrDomDocument* document, PrDomTag tag) {
    return domElementCreate(document, PrDomNamespace_Html, tag, PrStringView{});
}

void set(PrDomElement* element, PrStringView name, PrStringView value) {
    domSetAttribute(element, PrDomNamespace_None, name, value);
}

PrDomElement* input(PrDomDocument* document, PrStringView type) {
    PrDomElement* element = html(document, PrDomTag_Input);
    set(element, "type", type);
    return element;
}

// Returns the control's value, which lives until the next call.
PrStringView valueOf(const PrDomElement* element) {
    static PrTextBuffer value = {};
    domControlValue(element, &value);
    return prTextBufferView(&value);
}

bool parsesFloat(PrStringView text, double expected) {
    double value = 0;
    return domParseFloat(text, &value) && value == expected;
}

// A connected document with a body.
struct Page {
    PrDomDocument* document = domDocumentCreate(DomDocumentKind_Html);
    PrDomElement* body = html(document, PrDomTag_Body);

    Page() { domInsert(document, body, nullptr); }
    ~Page() { domNodeRelease(document); }
};

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("the rules for parsing floating-point numbers read a prefix, and the valid syntax is strict") {
    CHECK(parsesFloat("  1.5e2px", 150));
    CHECK(parsesFloat(".5", 0.5));
    CHECK(parsesFloat("-.5", -0.5));
    CHECK(parsesFloat("+3", 3));
    CHECK(parsesFloat("1.e5", 1));
    CHECK(parsesFloat("-0", 0));
    double value = 0;
    CHECK(!domParseFloat("e5", &value));
    CHECK(!domParseFloat("1e999", &value));
    CHECK(domIsValidFloat("-1.5e-3"));
    CHECK(domIsValidFloat(".5"));
    CHECK(!domIsValidFloat("+1"));
    CHECK(!domIsValidFloat("1."));
    CHECK(!domIsValidFloat(" 1"));
}

TEST_CASE("dates, weeks and times round-trip through their numbers") {
    PrTimeDate date{};
    CHECK(domParseDate("2024-02-29", &date));
    CHECK(!domParseDate("2023-02-29", &date));
    CHECK(!domParseDate("0000-01-01", &date));

    // A week belongs to the year of its Thursday.
    int64_t year = 0;
    uint32_t week = 0;
    domWeekOfDays(prTimeDateToDays(PrTimeDate{2010, 1, 3}), &year, &week);
    CHECK((year == 2009 && week == 53));
    domWeekOfDays(prTimeDateToDays(PrTimeDate{2008, 12, 29}), &year, &week);
    CHECK((year == 2009 && week == 1));
    CHECK(domWeeksInYear(2009) == 53);
    CHECK(domWeeksInYear(2010) == 52);
    CHECK(!domParseWeek("2010-W53", &year, &week));

    DomTime time = 0;
    uint32_t digits = 0;
    CHECK(domParseTime("23:59:59.1234", &time, &digits));
    CHECK((time == 86399123 && digits == 4));
    CHECK(!domIsValidTime("23:59:59.1234"));
    CHECK(!domParseTime("24:00", &time));
    PrTextBuffer text = {};
    domAppendTime(&text, 3600000);
    CHECK(prTextBufferView(&text) == "01:00");
    prTextBufferClear(&text);
    domAppendTime(&text, 3661500);
    CHECK(prTextBufferView(&text) == "01:01:01.500");
    prTextBufferReset(&text);
}

TEST_CASE("an input's value follows its value attribute until script sets it") {
    Page p;
    PrDomElement* text = input(p.document, "text");
    set(text, "value", "a\nb");
    CHECK(valueOf(text) == "ab");
    domControlSetValue(text, "set");
    set(text, "value", "ignored");
    CHECK(valueOf(text) == "set");

    // A change to a default mode writes the value to the attribute, and a change back reads the attribute again.
    set(text, "type", "hidden");
    CHECK(domAttributeValue(text, "value") == "set");
    CHECK(valueOf(text) == "set");
    set(text, "value", "back");
    set(text, "type", "text");
    CHECK(valueOf(text) == "back");

    PrDomElement* checkbox = input(p.document, "checkbox");
    CHECK(valueOf(checkbox) == "on");
}

TEST_CASE("a range's value rounds to its step and stays in its range") {
    Page p;
    PrDomElement* range = input(p.document, "range");
    CHECK(valueOf(range) == "50");
    set(range, "min", "0");
    set(range, "max", "100");
    set(range, "step", "20");
    set(range, "value", "50");
    // Of two nearest steps, the one toward positive infinity wins.
    CHECK(valueOf(range) == "60");
    set(range, "max", "30");
    CHECK(valueOf(range) == "20");
    set(range, "value", "nonsense");
    CHECK(valueOf(range) == "20");
}

TEST_CASE("stepping a number moves by its step within its bounds") {
    Page p;
    PrDomElement* number = input(p.document, "number");
    set(number, "min", "1");
    set(number, "max", "10");
    set(number, "step", "3");
    set(number, "value", "2");
    // A value off the steps moves to the next step in the direction of travel.
    CHECK(domInputStep(number, 1, true) == PrStatus_Ok);
    CHECK(valueOf(number) == "4");
    CHECK(domInputStep(number, 5, true) == PrStatus_Ok);
    CHECK(valueOf(number) == "10");
    set(number, "step", "any");
    CHECK(domInputStep(number, 1, true) == PrStatus_InvalidState);
}

TEST_CASE("a checked radio unchecks the rest of its group, and a group without one is indeterminate") {
    Page p;
    PrDomElement* form = html(p.document, PrDomTag_Form);
    domInsert(p.body, form, nullptr);
    PrDomElement* a = input(p.document, "radio");
    PrDomElement* b = input(p.document, "radio");
    PrDomElement* outside = input(p.document, "radio");
    PrDomElement* radios[] = {a, b, outside};
    for (PrDomElement* radio : radios)
        set(radio, "name", "g");
    domInsert(form, a, nullptr);
    domInsert(form, b, nullptr);
    domInsert(p.body, outside, nullptr);
    CHECK((domElementCheckedness(a) & DomCheckedFlag_Indeterminate) != 0);
    domInputSetChecked(a, true, true);
    domInputSetChecked(outside, true, true);
    CHECK(domInputChecked(a));
    domInputSetChecked(b, true, true);
    CHECK(!domInputChecked(a));
    // The radio outside the form is in another group.
    CHECK(domInputChecked(outside));
    CHECK(domElementCheckedness(a) == DomCheckedFlag_Unchecked);
}

TEST_CASE("a form owner follows the form attribute and the tree") {
    Page p;
    PrDomElement* first = html(p.document, PrDomTag_Form);
    PrDomElement* second = html(p.document, PrDomTag_Form);
    set(second, "id", "second");
    domInsert(p.body, first, nullptr);
    domInsert(p.body, second, nullptr);
    PrDomElement* control = input(p.document, "text");
    domInsert(first, control, nullptr);
    CHECK(domFormOwner(control) == first);

    // The attribute names a form by ID, and an ID that leaves or joins the document resets the owner.
    set(control, "form", "second");
    CHECK(domFormOwner(control) == second);
    domNodeRetain(second);
    domRemove(second);
    CHECK(domFormOwner(control) == nullptr);
    domInsert(p.body, second, nullptr);
    CHECK(domFormOwner(control) == second);
    domNodeRelease(second);

    // A control that leaves its form's tree leaves the form.
    domRemoveAttribute(control, PrDomNamespace_None, "form");
    CHECK(domFormOwner(control) == first);
    domNodeRetain(control);
    domRemove(control);
    CHECK(domFormOwner(control) == nullptr);
    domNodeRelease(control);
}

TEST_CASE("a parser-inserted control keeps a form that is not its ancestor, until the form is freed") {
    Page p;
    PrDomElement* form = html(p.document, PrDomTag_Form);
    PrDomElement* control = input(p.document, "text");
    domInsert(p.body, form, nullptr);
    domFormAssociateParserInserted(control, form);
    domInsert(p.body, control, nullptr);
    CHECK(domFormOwner(control) == form);

    // Removing the form leaves the control's owner, and freeing the form clears it.
    domNodeRetain(form);
    domRemove(form);
    CHECK(domFormOwner(control) == form);
    domNodeRelease(form);
    CHECK(domFormOwner(control) == nullptr);
}
