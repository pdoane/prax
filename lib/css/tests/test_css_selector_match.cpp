#include "pr/array.h"
#include "pr/span.h"
#include "pr/string.h"
#include "pr/string_ops.h"

#include "css/src/css_parser.h"
#include "css/src/css_selector_match.h"
#include "css/tests/css_tests.h"
#include "dom/src/dom_custom.h"

#include "tools/harness/test.h"

namespace pr_impl {

namespace {

// An attribute of a node to build, by its qualified name.
struct Attribute {
    String name;
    String value;

    Attribute(PrStringView attributeName, PrStringView attributeValue) : name(attributeName), value(attributeValue) {}
};

// A node to build: an element with its attributes and children, or text if `name` is null. An element is built up a
// call at a time, as `E("p").attr("id", "a").add(T("text"))`.
struct Spec {
    const char* PR_NULLABLE name;
    Array<Attribute> attributes;
    Array<Spec> children;
    DomNamespaceId space = PrDomNamespace_Html;
    const char* text = "";

    // Adds the attribute `attributeName` with `value`, after those added before it.
    Spec& attr(PrStringView attributeName, PrStringView value) {
        attributes.push(Attribute(attributeName, value));
        return *this;
    }

    // Adds `child`, or each of `list`, after the children added before.
    Spec& add(const Spec& child) {
        children.push(child);
        return *this;
    }
    Spec& add(Span<const Spec> list) {
        children.append(list);
        return *this;
    }
};

// Returns an HTML element named `name`, with no attributes or children.
Spec E(const char* name) {
    return Spec{name, {}, {}};
}

// Returns an SVG element, as E returns an HTML one.
Spec Svg(const char* name) {
    Spec spec = E(name);
    spec.space = PrDomNamespace_Svg;
    return spec;
}

Spec T(const char* text) {
    Spec spec = {};
    spec.text = text;
    return spec;
}

// Builds the node that `spec` describes and appends it to `parent`.
PrDomNode* build(PrDomDocument* document, PrDomNode* parent, const Spec& spec) {
    if (spec.name == nullptr) {
        PrDomNode* text = domTextCreate(document, spec.text);
        domAttach(parent, text, nullptr);
        return text;
    }
    PrStringView name = spec.name;
    bool held = false;
    PrDomElement* element = nullptr;
    if (spec.space == PrDomNamespace_Html)
        element = domElementCreateHtml(document, domTagFromText(name), name, {}, false, &held);
    else
        element = domElementCreate(document, spec.space, domTagFromText(name), name);
    domAttach(parent, element, nullptr);
    for (const Attribute& attribute : spec.attributes) {
        // An `xml:` name is the attribute in the XML namespace.
        PrStringView qualified = attribute.name;
        bool xml = prStringStartsWith(qualified, "xml:");
        PrStringView local = xml ? prStringSlice(qualified, 4, qualified.size - 4) : qualified;
        domSetAttribute(element, xml ? PrDomNamespace_Xml : PrDomNamespace_None, local, attribute.value);
    }
    for (const Spec& child : spec.children)
        build(document, element, child);
    return element;
}

// A document holding one element tree, destroyed with the test.
struct Doc {
    PrDomDocument* document;
    PrDomElement* root;

    explicit Doc(const Spec& spec, DomQuirks quirks = DomQuirks_No) : document(domDocumentCreate(DomDocumentKind_Html)) {
        document->quirks = quirks;
        root = static_cast<PrDomElement*>(build(document, document, spec));
    }

    ~Doc() { domNodeRelease(document); }

    Doc(const Doc&) = delete;
    Doc& operator=(const Doc&) = delete;

    PrDomElement* byId(const char* id) const { return domElementById(document, id); }
};

// Returns the IDs of the descendants of `root` that the selectors match, in tree order, joined by spaces. "-" stands
// for an element without an ID, and "!" means the text is not a selector list.
String select(PrDomNode* root, const char* text) {
    PrCssSelectors* selectors = cssParseDomSelectors(text);
    if (selectors == nullptr)
        return "!";
    String out;
    CssSelectorMatcher matcher;
    cssSelectorMatcherInit(&matcher, selectors, domElementFromNode(root), root->document, root);
    for (PrDomElement* element = cssSelectorMatcherNext(&matcher, root, root); element != nullptr;
            element = cssSelectorMatcherNext(&matcher, root, element)) {
        PrStringView id = domElementId(element);
        if (!out.isEmpty())
            out.append(' ');
        out.append(id.size != 0 ? id : PrStringView("-"));
    }
    cssSelectorMatcherReset(&matcher);
    cssSelectorsDestroy(selectors);
    return out;
}

String select(const Doc& doc, const char* text) {
    return select(doc.document, text);
}

bool matches(const PrDomElement* element, const char* text, const PrDomElement* PR_NULLABLE scope = nullptr) {
    PrCssSelectors* selectors = cssParseDomSelectors(text);
    REQUIRE(selectors != nullptr);
    bool result = cssSelectorsMatch(selectors, element, scope);
    cssSelectorsDestroy(selectors);
    return result;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a DOM selectors string declares no namespace prefix and fails as a whole") {
    CHECK(cssParseDomSelectors("") == nullptr);
    CHECK(cssParseDomSelectors("a,") == nullptr);
    CHECK(cssParseDomSelectors("ns|a") == nullptr);
    CHECK(cssParseDomSelectors(":dir()") == nullptr);
    PrCssSelectors* selectors = cssParseDomSelectors("  *|a, |b  ");
    REQUIRE(selectors != nullptr);
    CHECK(selectors->list.count == 2);
    cssSelectorsDestroy(selectors);
}

TEST_CASE("type selectors compare an HTML element's name in lowercase and a foreign one's as written") {
    const Spec inSvg[] = {Svg("foreignObject").attr("id", "fo"), Svg("div").attr("id", "svgdiv")};
    const Spec body[] = {
        E("div").attr("id", "div"),
        E("my-el").attr("id", "custom"),
        Svg("svg").attr("id", "svg").add(inSvg),
    };
    Doc doc(E("html").attr("id", "html").add(body));
    CHECK(select(doc, "div") == "div svgdiv");
    CHECK(select(doc, "DIV") == "div");
    CHECK(select(doc, "MY-EL") == "custom");
    CHECK(select(doc, "foreignObject") == "fo");
    CHECK(select(doc, "foreignobject") == "");
    CHECK(select(doc, "*|div") == "div svgdiv");
    CHECK(select(doc, "|div") == "");
    CHECK(select(doc, "svg *") == "fo svgdiv");
}

TEST_CASE("a sheet's namespaces decide the elements and attributes its prefixes take") {
    Doc doc(E("html").add(E("a").attr("id", "html").attr("xml:lang", "en")).add(Svg("a").attr("id", "svg")));
    CssTestSheet sheet("@namespace url(http://www.w3.org/2000/svg); @namespace x url(http://www.w3.org/XML/1998/namespace);");
    CssParser parser;
    cssParserInit(&parser);
    CssSelectorContext context = {};
    context.sheet = sheet.sheet->data;
    PrCssSelectors* svg = cssParseSelectorText(&parser, "a", &context);
    PrCssSelectors* any = cssParseSelectorText(&parser, "*|a", &context);
    PrCssSelectors* xml = cssParseSelectorText(&parser, "*|*[x|lang=en]", &context);
    REQUIRE(svg != nullptr);
    REQUIRE(any != nullptr);
    REQUIRE(xml != nullptr);
    CHECK(!cssSelectorsMatch(svg, doc.byId("html"), nullptr));
    CHECK(cssSelectorsMatch(svg, doc.byId("svg"), nullptr));
    CHECK(cssSelectorsMatch(any, doc.byId("html"), nullptr));
    CHECK(cssSelectorsMatch(xml, doc.byId("html"), nullptr));
    cssSelectorsDestroy(svg);
    cssSelectorsDestroy(any);
    cssSelectorsDestroy(xml);
    cssParserReset(&parser);
}

TEST_CASE("a compound without a type selector matches only the default namespace, but not as the subject in :is()") {
    const Spec body[] = {
        E("a").attr("id", "html").attr("class", "c").add(E("b").attr("id", "inner")),
        Svg("a").attr("id", "svg").attr("class", "c").add(Svg("b").attr("id", "svgInner")),
    };
    Doc doc(E("html").add(body));
    CssTestSheet sheet("@namespace url(http://www.w3.org/1999/xhtml);");
    CssParser parser;
    cssParserInit(&parser);
    CssSelectorContext context = {};
    context.sheet = sheet.sheet->data;
    // Returns the IDs of the elements, of those with one, that `text` matches.
    auto matched = [&](const char* text) {
        PrCssSelectors* selectors = cssParseSelectorText(&parser, text, &context);
        REQUIRE(selectors != nullptr);
        String ids;
        const char* const named[] = {"html", "inner", "svg", "svgInner"};
        for (const char* id : named) {
            if (!cssSelectorsMatch(selectors, doc.byId(id), nullptr))
                continue;
            if (!ids.isEmpty())
                ids.append(' ');
            ids.append(id);
        }
        cssSelectorsDestroy(selectors);
        return ids;
    };
    CHECK(matched(".c") == "html");
    CHECK(matched("[id]") == "html inner");
    CHECK(matched(".c > *|*") == "inner");
    CHECK(matched("*|*:is(.c)") == "html svg");
    CHECK(matched("*|*:not(.c)") == "inner svgInner");
    CHECK(matched("*|*:is(.c *)") == "inner");
    CHECK(matched("*|*:is(*.c)") == "html");
    // The implied universal selector is not written back.
    PrCssSelectors* written = cssParseSelectorText(&parser, ".c::before, [id] b", &context);
    REQUIRE(written != nullptr);
    PrTextBuffer buffer = {};
    cssSerializeSelectors(&buffer, written);
    CHECK(take(&buffer) == ".c::before, [id] b");
    cssSelectorsDestroy(written);
    cssParserReset(&parser);
}

TEST_CASE("IDs and classes compare exactly, and ASCII case-insensitively in quirks mode") {
    Spec tree = E("html").add(E("p").attr("id", "Foo").attr("class", " a\tB\nc "));
    Doc standards(tree);
    CHECK(select(standards, "#Foo") == "Foo");
    CHECK(select(standards, "#foo") == "");
    CHECK(select(standards, ".a.B.c") == "Foo");
    CHECK(select(standards, ".b") == "");
    CHECK(select(standards, "[class=b i]") == "");

    Doc quirks(tree, DomQuirks_Yes);
    CHECK(select(quirks, "#foo") == "Foo");
    CHECK(select(quirks, ".b.C") == "Foo");
    CHECK(select(quirks, "[id=foo]") == "");

    Doc limited(tree, DomQuirks_Limited);
    CHECK(select(limited, "#foo") == "");
}

TEST_CASE("attribute selectors test presence and each operator on the value") {
    const Spec body[] = {
        E("p").attr("id", "a").attr("title", "one two").attr("lang", "en-US").attr("data-x", ""),
        E("p").attr("id", "b").attr("title", "onetwo").attr("lang", "en"),
        E("p").attr("id", "c").attr("title", "\x0bone").attr("lang", "-x"),
    };
    Doc doc(E("html").add(body));
    CHECK(select(doc, "[title]") == "a b c");
    CHECK(select(doc, "[TITLE]") == "a b c");
    CHECK(select(doc, "[data-x]") == "a");
    CHECK(select(doc, "[data-x='']") == "a");
    CHECK(select(doc, "[title=onetwo]") == "b");
    CHECK(select(doc, "[title~=two]") == "a");
    CHECK(select(doc, "[title~='one two']") == "");
    CHECK(select(doc, "[title~='']") == "");
    CHECK(select(doc, "[title~=one]") == "a");
    CHECK(select(doc, "[lang|=en]") == "a b");
    CHECK(select(doc, "[lang|='']") == "c");
    CHECK(select(doc, "[title^=one]") == "a b");
    CHECK(select(doc, "[title$=two]") == "a b");
    CHECK(select(doc, "[title*=etw]") == "b");
    CHECK(select(doc, "[title^=''], [title$=''], [title*='']") == "");
}

TEST_CASE("the i and s flags and HTML's list of attributes decide the case of a value") {
    const Spec body[] = {
        E("p").attr("id", "html").attr("align", "LEFT").attr("foo", "BAR").attr("xml:lang", "EN"),
        Svg("g").attr("id", "svg").attr("align", "LEFT"),
    };
    Doc doc(E("html").add(body));
    CHECK(select(doc, "[foo=bar]") == "");
    CHECK(select(doc, "[foo=bar i]") == "html");
    CHECK(select(doc, "[foo=BAR s]") == "html");
    CHECK(select(doc, "[align=left]") == "html");
    CHECK(select(doc, "[align=left s]") == "");
    CHECK(select(doc, "[align=left i]") == "html svg");
    CHECK(select(doc, "[align^=l]") == "html");
    CHECK(select(doc, "[*|lang=en]") == "");
    CHECK(select(doc, "[*|lang=en i]") == "html");
}

TEST_CASE("combinators walk ancestors and earlier siblings") {
    const Spec inD1[] = {
        E("p").attr("id", "p1"),
        T("text"),
        E("span").attr("id", "s1"),
        E("p").attr("id", "p2").add(E("em").attr("id", "e1")),
    };
    const Spec body[] = {
        E("div").attr("id", "d1").attr("class", "x").add(inD1),
        E("div").attr("id", "d2").add(E("div").attr("id", "d3").add(E("em").attr("id", "e2"))),
    };
    Doc doc(E("html").attr("id", "root").add(body));
    CHECK(select(doc, ".x em") == "e1");
    CHECK(select(doc, "div > em") == "e2");
    CHECK(select(doc, "p + span") == "s1");
    CHECK(select(doc, "p + p") == "");
    CHECK(select(doc, "p ~ p") == "p2");
    CHECK(select(doc, "#p1 ~ * > em") == "e1");
    CHECK(select(doc, "div div em, html > div + div") == "d2 e2");
    CHECK(select(doc, "html div div em") == "e2");
    CHECK(select(doc, "span div em") == "");
    CHECK(select(doc, "* > html") == "");
    CHECK(select(doc, "a || em, a /for/ em") == "");
}

TEST_CASE("the reference combinator follows an attribute that holds an ID") {
    const Spec body[] = {
        E("label").attr("id", "l1").attr("for", "i2"),
        E("input").attr("id", "i1"),
        E("input").attr("id", "i2"),
    };
    Doc doc(E("html").add(body));
    CHECK(select(doc, "label /for/ input") == "i2");
    CHECK(select(doc, "#l1 /FOR/ #i1") == "");
}

TEST_CASE(":is, :where and :not take a list, and :has searches from its element") {
    const Spec body[] = {
        E("div").attr("id", "a").add(E("p").attr("id", "b").add(E("em").attr("id", "c"))),
        E("div").attr("id", "d").add(E("span").attr("id", "e")),
        E("section").attr("id", "f"),
    };
    Doc doc(E("html").add(body));
    CHECK(select(doc, ":is(p, span)") == "b e");
    CHECK(select(doc, "div :where(em, span)") == "c e");
    CHECK(select(doc, "div:not(:has(em))") == "d");
    CHECK(select(doc, ":is(div > p) em") == "c");
    CHECK(select(doc, ":is(::before, p)") == "b");
    CHECK(select(doc, "div:has(em)") == "a");
    CHECK(select(doc, "div:has(> em)") == "");
    CHECK(select(doc, "div:has(> p > em)") == "a");
    CHECK(select(doc, "div:has(> p em)") == "a");
    CHECK(select(doc, "div:has(+ div)") == "a");
    CHECK(select(doc, "div:has(~ section)") == "a d");
    CHECK(select(doc, "div:has(+ div > span)") == "a");
    CHECK(select(doc, "div:has(+ div span)") == "a");
    CHECK(select(doc, "*:has(em):not(html)") == "a b");
    CHECK(select(doc, "div:has(:scope em)") == "");
}

TEST_CASE(":has searches only where its argument's combinators reach") {
    const Spec inA[] = {E("p").attr("id", "b").add(E("em").attr("id", "c")), E("span").attr("id", "d")};
    const Spec body[] = {
        E("div").attr("id", "a").add(inA),
        E("div").attr("id", "e").add(E("em").attr("id", "f").attr("class", "x")),
        E("section").attr("id", "g").add(E("p").attr("id", "h")),
        E("p").attr("id", "i").attr("class", "x"),
    };
    Doc doc(E("html").add(body));
    // A child combinator puts the subject at one depth, and sibling combinators after it keep it there.
    CHECK(select(doc, ":has(> em)") == "b e");
    CHECK(select(doc, "div:has(> * > em)") == "a");
    CHECK(select(doc, ":has(> p ~ span)") == "a");
    CHECK(select(doc, "div:has(> * em)") == "a");
    // Sibling combinators before any other reach one later sibling, or any.
    CHECK(select(doc, ":has(+ div)") == "a");
    CHECK(select(doc, ":has(+ * + section)") == "a");
    CHECK(select(doc, ":has(+ * + * > p)") == "a");
    CHECK(select(doc, ":has(~ p)") == "a e g");
    CHECK(select(doc, ":has(~ .x)") == "a e g");
    CHECK(select(doc, ":has(~ * > .x)") == "a");
    CHECK(select(doc, ":has(~ * em)") == "a");
    // A class that no element has, and a class that only an element out of reach has.
    CHECK(select(doc, ":has(.absent), :has(+ .absent), :has(~ p.absent)") == "");
    CHECK(select(doc, "section:has(.x)") == "");
    // Cheaper tests run first, which does not change what the compound matches.
    CHECK(select(doc, ":has(em).x, :has(> em)#e, :not(:has(*)):is(p)") == "e h i");
}

TEST_CASE("an ID left of the subject limits the search to its element's descendants") {
    const Spec inA[] = {
        E("p").attr("id", "b"),
        E("section").attr("id", "c").add(E("p").attr("id", "d")),
        E("p").attr("id", "e"),
    };
    const Spec body[] = {
        E("div").attr("id", "a").add(inA),
        E("div").attr("id", "f").add(E("p").attr("id", "g")),
        E("div").attr("class", "dup").attr("id", "dup").add(E("p").attr("id", "h")),
        E("div").attr("class", "dup").attr("id", "dup").add(E("p").attr("id", "i")),
    };
    Doc doc(E("html").add(body));
    CHECK(select(doc, "#a p") == "b d e");
    CHECK(select(doc, "#a > p") == "b e");
    CHECK(select(doc, "#a section > p, #f p") == "d g");
    CHECK(select(doc, "#dup p") == "h i");
    CHECK(select(doc, "#absent p") == "");
    CHECK(select(doc, "#a ~ div p") == "g h i");
    // From an element inside the one with the ID, and from one beside it.
    CHECK(select(doc.byId("c"), "#a p") == "d");
    CHECK(select(doc.byId("a"), "#a p") == "b d e");
    CHECK(select(doc.byId("f"), "#a p") == "");
}

TEST_CASE("the structural pseudo-classes count element siblings") {
    const Spec body[] = {
        E("p").attr("id", "p1"),
        E("span").attr("id", "s1"),
        E("p").attr("id", "p2").attr("class", "x"),
        E("p").attr("id", "p3").add(T(" \n\t")),
        E("div").attr("id", "d1").add(T("x")),
        E("div").attr("id", "d2").add(E("b").attr("id", "only")),
    };
    Doc doc(E("html").attr("id", "root").add(body));
    CHECK(select(doc, ":root") == "root");
    CHECK(select(doc, ":empty") == "p1 s1 p2 p3 only");
    CHECK(select(doc, ":first-child") == "root p1 only");
    CHECK(select(doc, ":last-child") == "root d2 only");
    CHECK(select(doc, ":only-child") == "root only");
    CHECK(select(doc, ":first-of-type") == "root p1 s1 d1 only");
    CHECK(select(doc, ":last-of-type") == "root s1 p3 d2 only");
    CHECK(select(doc, ":only-of-type") == "root s1 only");
    CHECK(select(doc, "html > :nth-child(2n+1)") == "p1 p2 d1");
    CHECK(select(doc, "html > :nth-child(-n+2)") == "p1 s1");
    CHECK(select(doc, "html > :nth-last-child(2)") == "d1");
    CHECK(select(doc, "p:nth-of-type(2)") == "p2");
    CHECK(select(doc, "p:nth-last-of-type(odd)") == "p1 p3");
    CHECK(select(doc, ":nth-child(2 of p)") == "p2");
    CHECK(select(doc, ":nth-child(1 of .x)") == "p2");
    CHECK(select(doc, ":nth-last-child(1 of p, span)") == "p3");
    CHECK(select(doc, ":nth-child(2147483647n+2147483647)") == "");

    // A detached element has no siblings, whatever the links that list the detached roots say.
    PrDomElement* first = domElementCreate(doc.document, PrDomNamespace_Html, PrDomTag_P, "");
    PrDomElement* second = domElementCreate(doc.document, PrDomNamespace_Html, PrDomTag_P, "");
    CHECK(matches(first, ":only-child:only-of-type"));
    CHECK(matches(second, ":first-child:last-child"));
    CHECK(!matches(second, ":root"));
}

TEST_CASE("a matcher answers each element as a matcher of its own would") {
    // Siblings of mixed types and classes, each queried under several counts of the same parent, and a :has() queried
    // again for each ancestor that a descendant combinator walks to.
    Array<Spec> items;
    for (int i = 0; i < 24; ++i) {
        String id = format("i{}", i);
        Spec item = E(i % 3 == 2 ? "p" : "li").attr("id", id).attr("class", i % 4 == 0 ? "a" : "b");
        items.push(item.add(E("em").attr("id", format("{}e", id))));
    }
    // A list longer than the distance a count walks back, queried out of order by a :has() and a sibling combinator.
    Array<Spec> longer;
    for (int i = 0; i < 80; ++i) {
        String id = format("o{}", i);
        longer.push(E(i % 5 == 1 ? "dt" : "dd").attr("id", id).attr("class", i % 3 == 0 ? "c" : "d"));
    }
    const Spec body[] = {
        E("ul").attr("id", "list").add(items.span()),
        E("div").attr("id", "d").add(E("p").attr("id", "x")),
        E("dl").attr("id", "long").add(longer.span()),
    };
    Doc doc(E("html").attr("id", "root").add(body));

    const char* lists[] = {
        ":nth-child(3n+1), :nth-last-child(2n), li:nth-of-type(odd)",
        ":nth-child(odd of .a), p:nth-last-of-type(-n+3)",
        ":nth-child(2 of :nth-child(odd of li)), :nth-last-child(1 of .b)",
        "ul:has(> .a) li > em, :is(li, p):has(em) + p",
        "li:has(~ .a), :has(+ li + p > em), :has(~ p em)",
        "*:has(em), :has(> * > em), .b:has(~ p.a)",
        "li:has(~ .a + p), li:has(~ p + li + .b), :has(~ li + p) em",
        "ul li em, html .a em, :not(p) em, :is(.a, .b) em, html ul :nth-child(odd) em",
        "dd:nth-last-child(3n+1), dt:nth-of-type(odd), :nth-last-child(-n+40 of .c)",
        "dl:has(> :nth-child(79)), :nth-child(40) ~ :nth-last-child(2n of dd), dd:has(~ :nth-last-of-type(1))",
    };
    for (const char* text : lists) {
        PrCssSelectors* selectors = cssParseDomSelectors(text);
        REQUIRE(selectors != nullptr);
        String expected;
        for (PrDomNode* node = domNodeNext(doc.document, doc.document); node != nullptr;
                node = domNodeNext(doc.document, node)) {
            PrDomElement* element = domElementFromNode(node);
            if (element != nullptr && cssSelectorsMatch(selectors, element, nullptr))
                formatTo(expected, "{}{}", expected.isEmpty() ? "" : " ", domElementId(element));
        }
        CHECK(select(doc, text) == expected);
        cssSelectorsDestroy(selectors);
    }
    CHECK(select(doc, ":nth-child(odd of .a)") == "i0 i8 i16");
}

TEST_CASE("a selector list parsed once answers every query that takes one") {
    const Spec inA[] = {E("p").attr("id", "b"), E("p").attr("id", "c"), E("p").attr("id", "d")};
    Doc doc(E("html").attr("id", "root").add(E("div").attr("id", "a").add(inA)));
    PrCssSelectors* bad = nullptr;
    CHECK(prCssSelectorsCreate("p,", &bad) == PrStatus_Syntax);
    CHECK(bad == nullptr);

    PrCssSelectors* odd = nullptr;
    REQUIRE(prCssSelectorsCreate("p:nth-child(odd)", &odd) == PrStatus_Ok);
    PrDomNode* document = doc.document;
    PrDomNodeList* found = prDomNodeListCreate();
    prCssNodeQuerySelectorAllCompiled(document, odd, found);
    CHECK(prDomNodeListLength(found) == 2);
    prDomNodeListDestroy(found);
    CHECK(prCssNodeQuerySelectorCompiled(document, odd) == (doc.byId("b")));
    CHECK(prCssElementMatchesCompiled((doc.byId("d")), odd));
    CHECK(!prCssElementMatchesCompiled((doc.byId("c")), odd));
    prCssSelectorsDestroy(odd);

    // :scope is the element that the query is made on.
    PrCssSelectors* scoped = nullptr;
    REQUIRE(prCssSelectorsCreate(":scope > p, div:not(:scope)", &scoped) == PrStatus_Ok);
    CHECK(prCssNodeQuerySelectorCompiled((static_cast<PrDomNode*>(doc.byId("a"))), scoped) == (doc.byId("b")));
    CHECK(prCssElementClosestCompiled((doc.byId("c")), scoped) == (doc.byId("a")));
    prCssSelectorsDestroy(scoped);
}

TEST_CASE("a query whose subject names an ID tries the elements its tree keeps by that ID") {
    const Spec body[] = {
        E("div").attr("id", "a").attr("class", "c").add(E("p").attr("id", "x")).add(E("span").attr("id", "y")),
        E("div").attr("id", "b").add(E("em").attr("id", "y")),
    };
    Doc doc(E("html").attr("id", "root").add(body));
    CHECK(select(doc, "#x") == "x");
    CHECK(select(doc, "div#x") == "");
    CHECK(select(doc, ".c #x") == "x");
    CHECK(select(doc.byId("b"), "#x") == "");
    CHECK(select(doc.byId("a"), "#a") == "");
    CHECK(select(doc, "#nonesuch") == "");
    // Two elements with one ID both match, in tree order.
    CHECK(select(doc, "#y") == "y y");
    CHECK(select(doc.byId("b"), "#y") == "y");

    // In quirks mode an ID compares ASCII case-insensitively.
    Doc quirks(E("html").add(E("p").attr("id", "x")), DomQuirks_Yes);
    CHECK(select(quirks, "#X") == "x");
}

TEST_CASE("a document keeps the selector lists its queries parse") {
    Doc doc(E("html").attr("id", "root").add(E("p").attr("id", "x").attr("class", "c")));
    PrDomElement* x = doc.byId("x");
    bool matched = false;
    // More lists than the document keeps, each queried twice, whatever was released in between.
    for (uint32_t round = 0; round < 2; ++round) {
        for (uint32_t i = 0; i < kCssSelectorCacheSize + 8; ++i) {
            String text = format("p:nth-child({}n+1)", i);
            REQUIRE(prCssElementMatches(x, text, &matched) == PrStatus_Ok);
            CHECK(matched);
        }
    }
    CHECK(doc.document->selectors->entries.count == kCssSelectorCacheSize);
    CHECK(prCssElementMatches(x, "p,", &matched) == PrStatus_Syntax);
    CHECK(prCssElementMatches(x, "p,", &matched) == PrStatus_Syntax);
}

TEST_CASE(":scope is the scoping root, and :root where there is none") {
    Doc doc(E("html").attr("id", "root")
        .add(E("div").attr("id", "a").add(E("div").attr("id", "b").add(E("p").attr("id", "c")))));
    PrDomElement* a = doc.byId("a");
    CHECK(select(a, ":scope") == "");
    CHECK(select(a, ":scope > div") == "b");
    CHECK(select(a, "& p") == "c");
    CHECK(select(a, "div div") == "b");
    CHECK(select(a, ":scope div div") == "");
    CHECK(select(doc, ":scope") == "root");
    CHECK(select(doc, ":scope > div") == "a");
    CHECK(matches(a, ":scope", a));
    CHECK(!matches(a, ":scope"));
    CHECK(matches(doc.root, ":scope"));

    PrDomNode* fragment = domDocumentFragmentCreate(doc.document);
    PrDomElement* orphan = domElementCreate(doc.document, PrDomNamespace_Html, PrDomTag_Div, "");
    domAttach(fragment, orphan, nullptr);
    CHECK(select(fragment, ":scope > div") == "");
    CHECK(select(fragment, "div") == "-");
}

TEST_CASE(":lang matches the inherited language by extended filtering") {
    const Spec body[] = {
        E("p").attr("id", "en"),
        E("p").attr("id", "frx").attr("lang", "fr-x-standard"),
        E("p").attr("id", "frstd").attr("lang", "fr-standard"),
        E("p").attr("id", "long").attr("lang", "fr-ninechars"),
        E("p").attr("id", "dech").attr("lang", "de-Latn-CH"),
        E("p").attr("id", "empty").attr("lang", ""),
        E("p").attr("id", "xml").attr("xml:lang", "ja").attr("lang", "ko"),
        Svg("svg").attr("id", "svg").attr("lang", "it"),
    };
    Doc doc(E("html").attr("lang", "en").add(body));
    CHECK(select(doc, "p:lang(en)") == "en");
    CHECK(select(doc, "p:lang(EN)") == "en");
    CHECK(select(doc, ":lang(fr)") == "frx frstd");
    CHECK(select(doc, ":lang(fr-x)") == "frx");
    CHECK(select(doc, ":lang(fr-standard)") == "frstd");
    CHECK(select(doc, ":lang(de-CH)") == "dech");
    CHECK(select(doc, ":lang('*-CH')") == "dech");
    CHECK(select(doc, ":lang(\\*-Latn)") == "dech");
    CHECK(select(doc, "p:lang('')") == "empty");
    CHECK(select(doc, "p:lang('*')") == "en frx frstd dech xml");
    CHECK(select(doc, ":lang(ja)") == "xml");
    CHECK(select(doc, ":lang(it)") == "svg");
    CHECK(select(doc, ":lang(zh, de)") == "dech");

    PrDomElement* detached = domElementCreate(doc.document, PrDomNamespace_Html, PrDomTag_P, "");
    CHECK(matches(detached, ":lang('')"));
}

TEST_CASE(":lang reaches a shadow tree's host") {
    Doc doc(E("html").attr("lang", "en").add(E("div").attr("id", "host").attr("lang", "fr")));
    PrDomShadowRootInit init = prDomShadowRootInitDefault(PrDomShadowRootMode_Open);
    PrDomShadowRoot* shadow = nullptr;
    REQUIRE(domAttachShadow(doc.byId("host"), &init, &shadow) == PrStatus_Ok);
    PrDomElement* inside = static_cast<PrDomElement*>(build(doc.document, shadow, E("p")));
    CHECK(matches(inside, ":lang(fr)"));
}

TEST_CASE(":dir follows the dir attribute, the parent and the first strong character") {
    const Spec inRtl[] = {
        E("p").attr("id", "inherits"),
        E("input").attr("id", "tel").attr("type", "tel"),
        Svg("svg").attr("id", "svg"),
    };
    const Spec inAuto[] = {E("script").add(T("abc")), E("span").attr("dir", "ltr").add(T("abc")), T("12 ת abc")};
    const Spec body[] = {
        E("div").attr("id", "rtl").attr("dir", "RTL").add(inRtl),
        E("div").attr("id", "auto").attr("dir", "auto").add(inAuto),
        E("bdi").attr("id", "bdi").add(T("ا")),
        E("div").attr("id", "lol").attr("dir", "lol"),
        E("input").attr("id", "value").attr("dir", "auto").attr("value", "א"),
        E("textarea").attr("id", "area").attr("dir", "auto").add(T("x א")),
    };
    Doc doc(E("html").add(body));
    CHECK(select(doc, ":dir(rtl)") == "rtl inherits svg auto - bdi value");
    CHECK(select(doc, "div:dir(ltr)") == "lol");
    CHECK(select(doc, ":dir(auto)") == "");
    CHECK(select(doc, "#area:dir(ltr)") == "area");
}

TEST_CASE(":defined leaves out an HTML element whose name is a valid custom element name") {
    const Spec body[] = {
        E("my-el").attr("id", "custom"),
        E("font-face").attr("id", "reserved"),
        E("unknown").attr("id", "unknown"),
        Svg("my-el").attr("id", "svg"),
    };
    Doc doc(E("html").attr("id", "html").add(body));
    CHECK(select(doc, ":not(:defined)") == "custom");
}

TEST_CASE("a form attribute names a form of the element's own tree") {
    // html/#form-owner: a shadow tree's form attribute looks in the shadow tree.
    Doc doc(E("html").add(E("div").attr("id", "host")).add(E("form").attr("id", "outer")));
    PrDomShadowRootInit init = prDomShadowRootInitDefault(PrDomShadowRootMode_Open);
    PrDomShadowRoot* shadow = nullptr;
    REQUIRE(domAttachShadow(doc.byId("host"), &init, &shadow) == PrStatus_Ok);
    build(doc.document, shadow, E("form").attr("id", "f"));
    PrDomElement* inside = static_cast<PrDomElement*>(
        build(doc.document, shadow, E("input").attr("type", "submit").attr("form", "f")));
    PrDomElement* outside = static_cast<PrDomElement*>(
        build(doc.document, shadow, E("input").attr("type", "submit").attr("form", "outer")));
    CHECK(matches(inside, ":default"));
    CHECK(!matches(outside, ":default"));
}

TEST_CASE("HTML defines the form and link pseudo-classes by attributes") {
    const Spec inFieldset[] = {
        E("legend").attr("id", "legend").add(E("input").attr("id", "inlegend").attr("type", "button")),
        E("select").attr("id", "select").add(E("option").attr("id", "option").attr("selected", "")),
    };
    const Spec group = E("optgroup").attr("id", "group").attr("disabled", "").add(E("option").attr("id", "grouped"));
    const Spec inEditable = E("p").attr("id", "p").attr("contenteditable", "false");
    const Spec inForm[] = {
        E("input").attr("id", "checkbox").attr("type", "CHECKBOX").attr("checked", ""),
        E("input").attr("id", "radio1").attr("type", "radio").attr("name", "r"),
        E("input").attr("id", "radio2").attr("type", "radio").attr("name", "r").attr("checked", ""),
        E("input").attr("id", "radio3").attr("type", "radio").attr("name", "s"),
        E("input").attr("id", "required").attr("required", "").attr("placeholder", "p"),
        E("input").attr("id", "hidden").attr("type", "hidden").attr("required", ""),
        E("input").attr("id", "readonly").attr("readonly", "").attr("value", "v").attr("placeholder", "p"),
        E("button").attr("id", "reset").attr("type", "reset"),
        E("button").attr("id", "submit"),
        E("input").attr("id", "submit2").attr("type", "submit"),
        E("fieldset").attr("id", "fieldset").attr("disabled", "").add(inFieldset),
        E("select").attr("id", "select2").add(group),
        E("textarea").attr("id", "area").attr("placeholder", ""),
    };
    const Spec body[] = {
        E("a").attr("id", "link").attr("href", ""),
        E("a").attr("id", "anchor"),
        E("form").attr("id", "form").add(inForm),
        E("progress").attr("id", "progress"),
        E("details").attr("id", "details").attr("open", ""),
        E("div").attr("id", "editable").attr("contenteditable", "").add(inEditable),
        E("h1").attr("id", "h1"),
        E("h6").attr("id", "h6"),
        E("dialog").attr("id", "dialog"),
    };
    Doc doc(E("html").add(body));
    CHECK(select(doc, ":link") == "link");
    CHECK(select(doc, ":any-link") == "link");
    CHECK(select(doc, ":visited, :hover, :focus, :active, :target, :playing") == "");
    CHECK(select(doc, ":checked") == "checkbox radio2 option");
    CHECK(select(doc, ":unchecked") == "radio1 radio3 grouped");
    CHECK(select(doc, ":indeterminate") == "radio3 progress");
    CHECK(select(doc, ":default") == "checkbox radio2 submit option");
    CHECK(select(doc, ":disabled") == "fieldset select option group grouped");
    CHECK(select(doc, "form :enabled") ==
        "checkbox radio1 radio2 radio3 required hidden readonly reset submit submit2 inlegend select2 area");
    CHECK(select(doc, ":required") == "required");
    CHECK(select(doc, "input:optional") == "checkbox radio1 radio2 radio3 readonly");
    CHECK(select(doc, ":read-write") == "required area editable");
    CHECK(select(doc, "input:read-only") == "checkbox radio1 radio2 radio3 hidden readonly submit2 inlegend");
    CHECK(select(doc, ":placeholder-shown") == "required area");
    CHECK(select(doc, ":open") == "details");
    CHECK(select(doc, ":heading") == "h1 h6");
    CHECK(select(doc, ":heading(6, 2)") == "h6");
}

TEST_CASE("a link the host knows is visited is visited to the style, and unvisited to a script") {
    Doc doc(E("div").add(E("a").attr("id", "seen").attr("href", "x")).add(E("a").attr("id", "new").attr("href", "y")));
    DomDocumentHost host = {};
    host.context = doc.byId("seen");
    host.elementState = [](void* context, const PrDomElement* element, PrDomHostState wanted) -> PrDomHostState {
        return element == context ? PrDomHostState(wanted & PrDomHostState_Visited) : PrDomHostState(0);
    };
    doc.document->host = &host;
    PrCssSelectors* selectors = cssParseDomSelectors(":visited");
    REQUIRE(selectors != nullptr);
    CssSelectorMatcher matcher;
    cssSelectorMatcherInit(&matcher, selectors, nullptr, doc.document, nullptr);
    CHECK_FALSE(cssSelectorMatcherMatches(&matcher, doc.byId("seen")));
    matcher.purpose = CssMatchPurpose_Style;
    CHECK(cssSelectorMatcherMatches(&matcher, doc.byId("seen")));
    CHECK_FALSE(cssSelectorMatcherMatches(&matcher, doc.byId("new")));
    cssSelectorMatcherReset(&matcher);
    cssSelectorsDestroy(selectors);
    CHECK(select(doc, ":link") == "seen new");
    doc.document->host = nullptr;
}

TEST_CASE("a pseudo-element never matches an element") {
    Doc doc(E("html").add(E("p").attr("id", "p")));
    CHECK(select(doc, "p::before") == "");
    CHECK(select(doc, "p::-webkit-anything") == "");
}
