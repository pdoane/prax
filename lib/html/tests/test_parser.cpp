#include "pr/html_page.h"
#include "pr/string_ops.h"

#include "html/src/html_parser.h"

#include "tools/harness/test.h"

#include <string>
#include <vector>

namespace pr_impl {

namespace {

// A document, and the tree that `text` parses to.
struct Parse {
    std::string source;
    PrDomDocument* document = domDocumentCreate(DomDocumentKind_Html);
    HtmlParser parser = {};

    explicit Parse(std::string text, HtmlParseFlags flags = 0) : source(std::move(text)) {
        htmlParserInit(&parser, document, flags);
        htmlParse(&parser, source);
    }

    ~Parse() {
        htmlParserReset(&parser);
        domNodeRelease(document);
    }
};

// A fragment of `text` read as the contents of an element named `context`.
struct Fragment {
    std::string source;
    PrDomDocument* document = domDocumentCreate(DomDocumentKind_Html);
    HtmlParser parser = {};
    PrDomNode* node = nullptr;

    Fragment(std::string text, PrStringView context, DomNamespaceId space = PrDomNamespace_Html) :
            source(std::move(text)) {
        htmlParserInit(&parser, document, 0);
        node = htmlParseFragment(&parser, domElementCreate(document, space, domTagFromText(context), context), source);
    }

    ~Fragment() {
        htmlParserReset(&parser);
        domNodeRelease(document);
    }
};

void writeNode(std::string* out, PrDomNode* node);

void writeChildren(std::string* out, PrDomNode* node) {
    for (PrDomNode* child = node->first; child != nullptr; child = child->next)
        writeNode(out, child);
}

// Writes the tree on one line: an element as its name with its children in parentheses, and text in quotes, so that a
// case reads as input against shape.
void writeNode(std::string* out, PrDomNode* node) {
    if (!out->empty() && out->back() != '(')
        *out += ' ';

    switch (node->type) {
        case PrDomNodeType_Element: {
            const PrDomElement* element = domElementFromNode(node);
            if (element->space == PrDomNamespace_Svg)
                *out += "svg:";
            else if (element->space == PrDomNamespace_MathMl)
                *out += "math:";
            PrStringView name = domElementName(element);
            out->append(name.data, name.size);
            for (const PrDomAttr* attribute = element->firstAttribute; attribute != nullptr;
                    attribute = static_cast<const PrDomAttr*>(attribute->next)) {
                PrStringView text = prRawStringView(&attribute->name);
                *out += '[';
                out->append(text.data, text.size);
                text = prRawStringView(&attribute->value);
                *out += '=';
                out->append(text.data, text.size);
                *out += ']';
            }
            PrDomNode* contents = domElementContents(element);
            if (contents != nullptr && contents->first != nullptr) {
                *out += "(content";
                writeChildren(out, contents);
                *out += ')';
            }
            if (node->first != nullptr) {
                *out += '(';
                writeChildren(out, node);
                *out += ')';
            }
            return;
        }

        case PrDomNodeType_Text: {
            PrStringView text = prRawStringView(&static_cast<const PrDomText*>(node)->data);
            *out += '"';
            // A control character is written as an escape, so that a failure prints on one line and a carriage return
            // does not move the cursor back over it.
            for (size_t i = 0; i < text.size; ++i) {
                char c = text.data[i];
                if (c == '\r')
                    *out += "\\r";
                else if (c == '\n')
                    *out += "\\n";
                else if (c == '\t')
                    *out += "\\t";
                else
                    *out += c;
            }
            *out += '"';
            return;
        }

        case PrDomNodeType_Comment: {
            PrStringView text = prRawStringView(&static_cast<const PrDomComment*>(node)->data);
            *out += "<!--";
            out->append(text.data, text.size);
            *out += "-->";
            return;
        }

        case PrDomNodeType_DocumentType: {
            PrStringView text = prRawStringView(&static_cast<const PrDomDocumentType*>(node)->name);
            *out += "doctype:";
            out->append(text.data, text.size);
            return;
        }

        case PrDomNodeType_ProcessingInstruction: {
            const PrDomProcessingInstruction* instruction = static_cast<const PrDomProcessingInstruction*>(node);
            PrStringView target = prRawStringView(&instruction->target);
            PrStringView data = prRawStringView(&instruction->data);
            *out += "<?";
            out->append(target.data, target.size);
            *out += ' ';
            out->append(data.data, data.size);
            *out += "?>";
            return;
        }
    }
}

std::string tree(std::string text, HtmlParseFlags flags = 0) {
    Parse parse(std::move(text), flags);
    std::string out;
    writeChildren(&out, parse.document);
    return out;
}

std::string fragment(std::string text, PrStringView context, DomNamespaceId space = PrDomNamespace_Html) {
    Fragment parse(std::move(text), context, space);
    std::string out;
    writeChildren(&out, parse.node);
    return out;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

// ---------------------------------------------------------------------------------------------------------------------
// The shape every document has (html/#the-initial-insertion-mode to html/#the-after-head-insertion-mode)

TEST_CASE("a document is html, head and body however little is written") {
    CHECK(tree("") == "html(head body)");
    CHECK(tree("x") == "html(head body(\"x\"))");
    CHECK(tree("<html><head><body>") == "html(head body)");
    CHECK(tree("<title>t</title>x") == "html(head(title(\"t\")) body(\"x\"))");
}

TEST_CASE("U+FEFF in the text is a character, which decoding removes only from the start of bytes") {
    CHECK(tree("\xef\xbb\xbf<p>x") == "html(head body(\"\xef\xbb\xbf\" p(\"x\")))");
    CHECK(tree("<p>\xef\xbb\xbfx") == "html(head body(p(\"\xef\xbb\xbfx\")))");
}

TEST_CASE("a carriage return is whitespace, though the stream holds none") {
    // Preprocessing removes carriage returns from the input, but a character reference can write one, and
    // html/#parsing-main-inhtml counts it among the whitespace that every mode ignores.
    CHECK(tree("&#13;<p>x") == "html(head body(p(\"x\")))");
    CHECK(tree("<p>&#13;x") == "html(head body(p(\"\\rx\")))");
}

TEST_CASE("a doctype opens the document and names its mode") {
    CHECK(tree("<!doctype html>") == "doctype:html html(head body)");
    CHECK(tree("<!DOCTYPE HTML>x") == "doctype:html html(head body(\"x\"))");
}

TEST_CASE("head content before the body goes in the head") {
    CHECK(tree("<meta><body>x") == "html(head(meta) body(\"x\"))");
    CHECK(tree("<body><meta>") == "html(head body(meta))");
}

// ---------------------------------------------------------------------------------------------------------------------
// "in body" (html/#parsing-main-inbody)

TEST_CASE("an end tag closes an element whose name no tag names") {
    // html/#parsing-main-inbody, "any other end tag".
    CHECK(tree("<x-a></x-a><x-b>y</x-b>") == "html(head body(x-a x-b(\"y\")))");
    CHECK(tree("<foo><bar></foo>z") == "html(head body(foo(bar) \"z\"))");
}

TEST_CASE("a paragraph closes the one before it") {
    CHECK(tree("<p>a<p>b") == "html(head body(p(\"a\") p(\"b\")))");
    CHECK(tree("<p>a<div>b</div>") == "html(head body(p(\"a\") div(\"b\")))");
}

TEST_CASE("a list item closes the one before it") {
    CHECK(tree("<ul><li>a<li>b</ul>") == "html(head body(ul(li(\"a\") li(\"b\"))))");
}

TEST_CASE("an unclosed formatting element is reconstructed") {
    CHECK(tree("<b>a<p>b") == "html(head body(b(\"a\" p(\"b\"))))");
    CHECK(tree("<b>a</b><p>b") == "html(head body(b(\"a\") p(\"b\")))");
    CHECK(tree("<b>a</b>b") == "html(head body(b(\"a\") \"b\"))");
}

TEST_CASE("misnested formatting is repaired by the adoption agency") {
    CHECK(tree("<b><i>a</b>c") == "html(head body(b(i(\"a\")) i(\"c\")))");
    CHECK(tree("<p>1<b>2<i>3</b>4</i>5</p>") == "html(head body(p(\"1\" b(\"2\" i(\"3\")) i(\"4\") \"5\")))");
}

TEST_CASE("a formatting element reopened across a block keeps its nesting") {
    CHECK(tree("<p><b><div><marquee></p></b></div>X") == "html(head body(p(b) div(b(marquee(p \"X\")))))");
}

TEST_CASE("a void element takes no children") {
    CHECK(tree("<br>x") == "html(head body(br \"x\"))");
    CHECK(tree("<img src=a>x") == "html(head body(img[src=a] \"x\"))");
}

TEST_CASE("an attribute is lowercased and kept once") {
    CHECK(tree("<div ID=a Id=b>") == "html(head body(div[id=a]))");
}

TEST_CASE("a selectedcontent takes a copy of the selected option as the parser pops it") {
    // html/#maybe-clone-an-option-into-selectedcontent. The first option is selected once it is in the select, and so
    // is a later option with the selected attribute, even at the end.
    CHECK(tree("<select><button><selectedcontent></button><option><b>x</b><option>y</select>") ==
        "html(head body(select(button(selectedcontent(b(\"x\"))) option(b(\"x\")) option(\"y\"))))");
    CHECK(tree("<select><button><selectedcontent></button><option>x<option selected>y") ==
        "html(head body(select(button(selectedcontent(\"y\")) option(\"x\") option[selected=](\"y\"))))");
}

// ---------------------------------------------------------------------------------------------------------------------
// Tables (html/#parsing-main-intable to html/#parsing-main-intemplate)

TEST_CASE("a table grows the rows it was written without") {
    CHECK(tree("<table><tr><td>a") == "html(head body(table(tbody(tr(td(\"a\"))))))");
    CHECK(tree("<table><td>a") == "html(head body(table(tbody(tr(td(\"a\"))))))");
}

TEST_CASE("text a table cannot hold is fostered out of it") {
    CHECK(tree("<table>a<tr><td>b") == "html(head body(\"a\" table(tbody(tr(td(\"b\"))))))");
    CHECK(tree("<table><b>a</b><tr>") == "html(head body(b(\"a\") table(tbody(tr))))");
}

TEST_CASE("whitespace a table can hold stays in it") {
    CHECK(tree("<table> <tr></table>") == "html(head body(table(\" \" tbody(tr))))");
}

// ---------------------------------------------------------------------------------------------------------------------
// Foreign content (html/#parsing-main-inforeign)

TEST_CASE("a foreign element keeps its namespace and its spelling") {
    CHECK(tree("<svg><foreignObject>") == "html(head body(svg:svg(svg:foreignObject)))");
    CHECK(tree("<svg><FOREIGNOBJECT>") == "html(head body(svg:svg(svg:foreignObject)))");
    CHECK(tree("<math><mi>x</mi></math>") == "html(head body(math:math(math:mi(\"x\"))))");
}

TEST_CASE("a foreign attribute is respelled and namespaced") {
    CHECK(tree("<svg><g xlink:href=a viewbox=b>") == "html(head body(svg:svg(svg:g[href=a][viewBox=b])))");
}

TEST_CASE("an HTML element breaks out of foreign content") {
    CHECK(tree("<svg><p>") == "html(head body(svg:svg p))");
}

TEST_CASE("a CDATA section is text in foreign content and a comment outside it") {
    CHECK(tree("<svg><![CDATA[x]]>") == "html(head body(svg:svg(\"x\")))");
    CHECK(tree("<![CDATA[x]]>") == "<!--[CDATA[x]]--> html(head body)");
}

// ---------------------------------------------------------------------------------------------------------------------
// Templates (html/#parsing-main-inhead)

TEST_CASE("a template holds its children in its content") {
    CHECK(tree("<template><div></div></template>") == "html(head(template(content div)) body)");
    CHECK(tree("<template></template>") == "html(head(template) body)");
}

// ---------------------------------------------------------------------------------------------------------------------
// Fragments (html/#parsing-html-fragments)

TEST_CASE("a fragment answers the children its context would hold") {
    CHECK(fragment("<p>a", "div") == "p(\"a\")");
    CHECK(fragment("<td>a", "tr") == "td(\"a\")");
    CHECK(fragment("a", "title") == "\"a\"");
}

TEST_CASE("a fragment reads its context's content as text where the element does") {
    CHECK(fragment("<b>a", "title") == "\"<b>a\"");
    CHECK(fragment("<b>a", "style") == "\"<b>a\"");
    CHECK(fragment("<b>a", "div") == "b(\"a\")");
}

TEST_CASE("a fragment in foreign content stays in it") {
    CHECK(fragment("<g>", "svg", PrDomNamespace_Svg) == "svg:g");
}

TEST_CASE("an HTML destroy takes null and does nothing") {
    prHtmlPageDestroy(nullptr);
}

TEST_CASE("DOMParser parses an XML type as XML, and shows where it is not well-formed") {
    PrDomDocument* document = prHtmlDomParserParseFromString(
        "<p xmlns='urn:x'>a</p>", PrHtmlDomParserSupportedType_ImageSvgXml);
    PrStringView type = prDomDocumentContentType(document);
    CHECK(type == "image/svg+xml");
    PrStringView space = prDomElementNamespaceUri(prDomDocumentElement(document));
    CHECK(space == "urn:x");
    prDomDocumentRelease(document);

    // An XHTML doctype's entities are the HTML named character references.
    document = prHtmlDomParserParseFromString(
        "<!DOCTYPE p PUBLIC \"-//W3C//DTD XHTML 1.0 Strict//EN\" \"x\"><p>&nbsp;&hearts;</p>",
        PrHtmlDomParserSupportedType_ApplicationXhtmlXml);
    PrRawString text = prDomNodeTextContent(prDomElementAsNode(prDomDocumentElement(document)));
    CHECK(prRawStringView(&text) == "\xC2\xA0\xE2\x99\xA5");
    prRawStringReset(&text);
    prDomDocumentRelease(document);

    document = prHtmlDomParserParseFromString("<p>", PrHtmlDomParserSupportedType_TextXml);
    PrStringView name = prDomElementLocalName(prDomDocumentElement(document));
    CHECK(name == "parsererror");
    prDomDocumentRelease(document);
}

TEST_CASE("a hyperlink's URL parts read and write its href attribute") {
    PrDomDocument* document = prHtmlDomParserParseFromString(
        "<a id=a href='https://example.com:8080/one/two?q=1#top'>x</a><a id=b href='http://[bad'>y</a><area id=c>"
            "<math><a id=m href='https://example.com/math'>z</a></math>",
        PrHtmlDomParserSupportedType_TextHtml);
    PrDomNode* root = prDomDocumentAsNode(document);
    PrDomElement* a = prDomNodeGetElementById(root, "a");
    PrDomElement* b = prDomNodeGetElementById(root, "b");
    PrDomElement* c = prDomNodeGetElementById(root, "c");
    PrDomElement* m = prDomNodeGetElementById(root, "m");
    REQUIRE(prHtmlAnchorElementQuery(prDomElementAsEventTarget(a)) != nullptr);
    REQUIRE(prHtmlAreaElementQuery(prDomElementAsEventTarget(c)) != nullptr);
    auto component = [](PrDomElement* element, PrUrlComponent part) {
        PrRawString value = prHtmlHyperlinkComponent(element, part);
        std::string text(prRawStringView(&value).data, prRawStringView(&value).size);
        prRawStringReset(&value);
        return text;
    };
    CHECK(component(a, PrUrlComponent_Pathname) == "/one/two");
    CHECK(component(a, PrUrlComponent_Host) == "example.com:8080");
    CHECK(component(a, PrUrlComponent_Search) == "?q=1");
    CHECK(component(a, PrUrlComponent_Hash) == "#top");
    CHECK(prHtmlHyperlinkSetComponent(a, PrUrlComponent_Pathname, "/three") == PrStatus_Ok);
    PrStringView href = prDomElementGetAttribute(a, "href");
    CHECK(href == "https://example.com:8080/three?q=1#top");
    // An href that does not parse reads as written, with a protocol of ":".
    CHECK(component(b, PrUrlComponent_Href) == "http://[bad");
    CHECK(component(b, PrUrlComponent_Protocol) == ":");
    CHECK(component(b, PrUrlComponent_Pathname).empty());
    CHECK(component(c, PrUrlComponent_Href).empty());
    CHECK(prHtmlHyperlinkSetComponent(c, PrUrlComponent_Origin, "x") == PrStatus_ReadOnly);
    // MathML's a element is a hyperlink with the same members (mathml-core/#the-a-element).
    CHECK(component(m, PrUrlComponent_Pathname) == "/math");
    prDomDocumentRelease(document);
}

TEST_CASE("a load fires load at the Window after its scripts, and a failed one does not") {
    struct State {
        PrDomEventTarget* target = nullptr;
        int scripts = 0;
        int loads = 0;
        PrStatus status = PrStatus_Ok;
    } state;
    PrHtmlHost scripting = prHtmlHostDefault();
    scripting.context = &state;
    scripting.execute = [](void* data, PrHtmlPage*, const PrHtmlScript*) -> PrStatus {
        State* state = static_cast<State*>(data);
        ++state->scripts;
        CHECK(state->loads == 0);
        return state->status;
    };
    PrDomEventListener listener{
        [](void* data, PrDomEvent* event) {
            State* state = static_cast<State*>(data);
            CHECK(state->scripts == 2);
            CHECK(prDomEventCurrentTarget(event) == state->target);
            PrStringView type = prDomEventType(event);
            CHECK(type == "load");
            ++state->loads;
        },
        &state,
        1,
        nullptr,
        nullptr,
    };
    PrHtmlPage* page = prHtmlPageCreate(&scripting);
    state.target = prHtmlWindowAsEventTarget(prHtmlPageWindow(page));
    prDomEventTargetAddEventListener(state.target, "load", &listener, nullptr);
    PrStringView source = "<script>0</script><script>0</script>";
    CHECK(prHtmlPageLoad(page, source, "about:blank", nullptr) == PrStatus_Ok);
    CHECK(state.loads == 1);
    state.scripts = 0;
    state.loads = 0;
    state.status = PrStatus_InvalidState;
    CHECK(prHtmlPageLoad(page, source, "about:blank", nullptr) == PrStatus_InvalidState);
    CHECK(state.loads == 0);
    prHtmlPageDestroy(page);
}

TEST_CASE("a document that replaces another has a window of its own, and the closed window lets its listeners go") {
    struct State { int references = 0; int calls = 0; } state;
    PrDomEventListener callback{
        [](void* context, PrDomEvent*) { ++static_cast<State*>(context)->calls; },
        &state,
        1,
        [](void* context) { ++static_cast<State*>(context)->references; },
        [](void* context) { --static_cast<State*>(context)->references; },
    };
    PrHtmlPage* page = prHtmlPageCreate(nullptr);
    PrHtmlWindow* initial = prHtmlPageWindow(page);
    // The first document replaces the initial about:blank, whose window it keeps.
    CHECK(prHtmlPageLoad(page, "<p>first", "about:blank", nullptr) == PrStatus_Ok);
    PrHtmlWindow* window = prHtmlPageWindow(page);
    CHECK(window == initial);
    PrDomEventTarget* target = prHtmlWindowAsEventTarget(window);
    prDomEventTargetRetain(target);
    prDomEventTargetAddEventListener(target, "test", &callback, nullptr);
    prDomEventTargetAddEventListener(prDomDocumentAsEventTarget(prHtmlWindowDocument(window)), "test", &callback, nullptr);
    CHECK(state.references == 2);
    CHECK(prHtmlPageLoad(page, "<p>second", "about:blank", nullptr) == PrStatus_Ok);
    CHECK(prHtmlPageWindow(page) != window);
    CHECK(state.references == 0);
    // The closed window lives while a reference to it remains, and runs nothing.
    PrDomEvent* event = prDomEventCreate("test", nullptr);
    prDomEventTargetDispatchEvent(target, event, nullptr);
    CHECK(state.calls == 0);
    prDomEventRelease(event);
    prDomEventTargetRelease(target);
    prHtmlPageDestroy(page);
}

TEST_CASE("a Window names the first element whose id is the name, or a form whose name is") {
    PrHtmlPage* page = prHtmlPageCreate(nullptr);
    PrStringView source = "<p id=a></p><form name=f></form><div name=d></div><b id=a></b>";
    REQUIRE(prHtmlPageLoad(page, source, "about:blank", nullptr) == PrStatus_Ok);
    PrHtmlWindow* window = prHtmlPageWindow(page);

    PrDomElement* first = prHtmlWindowNamedItem(window, "a");
    REQUIRE(first != nullptr);
    PrRawString name = prDomElementTagName(first);
    CHECK(prRawStringView(&name) == "P");
    prRawStringReset(&name);

    CHECK(prHtmlWindowNamedItem(window, "f") != nullptr);
    CHECK(prHtmlWindowNamedItem(window, "d") == nullptr);   // a div's name attribute does not name it
    CHECK(prHtmlWindowNamedItem(window, "") == nullptr);

    PrDomEventTarget* target = prHtmlWindowAsEventTarget(window);
    CHECK(prHtmlWindowQuery(target) == window);
    CHECK(prDomNodeQuery(target) == nullptr);
    prHtmlPageDestroy(page);
}
