#include "pr/html.h"
#include "pr/html_markdown.h"
#include "pr/html_page.h"
#include "pr/markdown.h"
#include "pr/markdown_html.h"
#include "pr/string_ops.h"

#include "html/src/html_markdown.h"
#include "html/src/html_parser.h"

#include "tools/harness/test.h"

#include <string>

namespace pr_impl {

namespace {

// Writes the tree on one line, with each node's boundaries, so that adjacent Text nodes differ from one Text node.
void writeTree(std::string* out, const PrDomNode* node) {
    for (const PrDomNode* child = node->first; child != nullptr; child = child->next) {
        switch (child->type) {
            case PrDomNodeType_Element: {
                const PrDomElement* element = domElementFromNode(child);
                PrStringView name = domElementName(element);
                *out += '<';
                out->append(name.data, name.size);
                for (const PrDomAttr* attribute = element->firstAttribute; attribute != nullptr;
                        attribute = static_cast<const PrDomAttr*>(attribute->next)) {
                    PrStringView text = prRawStringView(&attribute->name);
                    *out += ' ';
                    out->append(text.data, text.size);
                    text = prRawStringView(&attribute->value);
                    *out += "=\"";
                    out->append(text.data, text.size);
                    *out += '"';
                }
                *out += '>';
                writeTree(out, child);
                *out += "</";
                out->append(name.data, name.size);
                *out += '>';
                break;
            }
            case PrDomNodeType_Text: {
                PrStringView text = prRawStringView(&static_cast<const PrDomText*>(child)->data);
                *out += "[";
                out->append(text.data, text.size);
                *out += "]";
                break;
            }
            case PrDomNodeType_Comment: {
                PrStringView text = prRawStringView(&static_cast<const PrDomComment*>(child)->data);
                *out += "<!--";
                out->append(text.data, text.size);
                *out += "-->";
                break;
            }
            case PrDomNodeType_DocumentType:
                *out += "<!doctype>";
                break;
            default:
                *out += "?";
                break;
        }
    }
}

std::string treeOf(const PrDomNode* node) {
    std::string out;
    writeTree(&out, node);
    return out;
}

// Returns the HTML that lib/markdown writes for `markdown`, parsed whole: its blocks, then its footnotes.
std::string htmlOf(PrStringView markdown, const PrMarkdownOptions& options) {
    PrMarkdown* document = prMarkdownCreate(&options);
    prMarkdownParse(document, markdown);
    PrRawString html = prRawStringEmpty();
    prMarkdownAppendHtml(document, 0, prMarkdownBlockCount(document), &html);
    prMarkdownAppendFootnotesHtml(document, &html);
    std::string out(prRawStringView(&html).data, prRawStringView(&html).size);
    prRawStringReset(&html);
    prMarkdownDestroy(document);
    return out;
}

// The tree of the document that parsing lib/markdown's HTML gives, as a Markdown document starts it.
std::string expectedDocument(PrStringView markdown, const PrMarkdownOptions& options) {
    std::string source = "<!DOCTYPE html><html><head></head><body>" + htmlOf(markdown, options);
    PrDomDocument* document = domDocumentCreate(DomDocumentKind_Html);
    HtmlParser parser = {};
    htmlParserInit(&parser, document, HtmlParseFlag_DeclarativeShadowRoots);
    htmlParse(&parser, source);
    htmlParserReset(&parser);
    std::string tree = treeOf(document);
    domNodeRelease(document);
    return tree;
}

// The tree of the document that the builder builds from `markdown`, given whole, or a byte at a time if `streamed`.
std::string builtDocument(PrStringView markdown, const PrMarkdownOptions& options, bool streamed) {
    PrDomDocument* document = domDocumentCreate(DomDocumentKind_Html);
    HtmlMarkdownBuilder builder;
    htmlMarkdownBuilderInitDocument(&builder, document, HtmlParseFlag_DeclarativeShadowRoots, &options);
    if (streamed) {
        for (size_t i = 0; i < markdown.size; ++i)
            htmlMarkdownBuilderAppend(&builder, prStringSlice(markdown, i, 1));
    } else {
        htmlMarkdownBuilderAppend(&builder, markdown);
    }
    htmlMarkdownBuilderEnd(&builder);
    htmlMarkdownBuilderReset(&builder);
    std::string tree = treeOf(document);
    domNodeRelease(document);
    return tree;
}

// The tree of a div's contents that parsing lib/markdown's HTML as them gives.
std::string expectedElement(PrStringView markdown, const PrMarkdownOptions& options) {
    std::string source = htmlOf(markdown, options);
    PrDomDocument* document = domDocumentCreate(DomDocumentKind_Html);
    HtmlParser parser = {};
    htmlParserInit(&parser, document, 0);
    PrDomElement* context = domElementCreate(document, PrDomNamespace_Html, PrDomTag_Div, "div");
    domNodeRetain(context);
    PrDomNode* fragment = htmlParseFragment(&parser, context, source);
    htmlParserReset(&parser);
    std::string tree = treeOf(fragment);
    domNodeRelease(fragment);
    domNodeRelease(context);
    domNodeRelease(document);
    return tree;
}

// The tree of a div's contents that a stream builds from `markdown`, given whole, or a byte at a time if `streamed`.
std::string builtElement(PrStringView markdown, const PrMarkdownOptions& options, bool streamed) {
    PrDomDocument* document = domDocumentCreate(DomDocumentKind_Html);
    PrDomElement* element = domElementCreate(document, PrDomNamespace_Html, PrDomTag_Div, "div");
    domNodeRetain(element);
    if (streamed) {
        PrHtmlMarkdownStream* stream = prHtmlMarkdownStreamCreate(element, &options);
        for (size_t i = 0; i < markdown.size; ++i)
            prHtmlMarkdownStreamAppend(stream, prStringSlice(markdown, i, 1));
        prHtmlMarkdownStreamEnd(stream);
        prHtmlMarkdownStreamDestroy(stream);
    } else {
        prHtmlSetMarkdown(element, markdown, &options);
    }
    std::string tree = treeOf(element);
    domNodeRelease(element);
    domNodeRelease(document);
    return tree;
}

PrMarkdownOptions everything() {
    PrMarkdownOptions options = prMarkdownOptionsDefault();
    options.extensions |= PrMarkdownExtensionFlag_Footnotes;
    return options;
}

// Checks that every way of building `markdown` gives the tree that parsing its HTML gives.
void checkBuilds(PrStringView markdown, const PrMarkdownOptions& options = everything()) {
    INFO(std::string(markdown.data, markdown.size));
    std::string document = expectedDocument(markdown, options);
    CHECK(builtDocument(markdown, options, false) == document);
    CHECK(builtDocument(markdown, options, true) == document);
    std::string element = expectedElement(markdown, options);
    CHECK(builtElement(markdown, options, false) == element);
    CHECK(builtElement(markdown, options, true) == element);
}

// Counts the elements in the MathML namespace below `node`, and the elements in another namespace below a <math>.
void countMathMl(const PrDomNode* node, bool inMath, uint32_t* mathml, uint32_t* other) {
    for (const PrDomNode* child = node->first; child != nullptr; child = child->next) {
        if (child->type != PrDomNodeType_Element)
            continue;
        const PrDomElement* element = domElementFromNode(child);
        bool isMathMl = element->space == PrDomNamespace_MathMl;
        if (isMathMl)
            ++*mathml;
        else if (inMath)
            ++*other;
        countMathMl(child, inMath || isMathMl, mathml, other);
    }
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("Markdown builds the DOM that parsing its HTML gives") {
    checkBuilds("# Title\n\nSome *emphasis*, **strong** and `code`.\n");
    checkBuilds("Setext\n======\n\nA paragraph\nwith a soft break  \nand a hard one.\n");
    checkBuilds("- tight\n- list\n  - nested\n\n1. loose\n\n2. ordered\n\n3) other\n");
    checkBuilds("7. starts at seven\n8. next\n");
    checkBuilds("> quoted\n> > twice\n\n***\n");
    checkBuilds("```js extra\nlet x = 1 < 2 && 3;\n```\n\n    indented\n    code\n");
    checkBuilds("[link](/a?b=1&c='d' \"title\") and ![alt *em*](i.png \"t\") and <https://x.org>\n");
    checkBuilds("&amp; &copy; &#35; \\* escaped\n");
    checkBuilds("| a | b |\n|:--|--:|\n| 1 | 2 |\n\n- [ ] task\n- [x] done\n");
    checkBuilds("Footnote[^1] and again[^1].\n\n[^1]: The note.\n\n    More of it.\n");
    checkBuilds("~~struck~~ www.example.com\n");
}

TEST_CASE("math builds MathML elements, as parsing its HTML does") {
    const char* markdown = "Inline $x^2 < \\frac{a}{b}$ and display $$\\begin{cases} 1 & x \\\\ 0 \\end{cases}$$ math\n";
    checkBuilds(markdown);
    checkBuilds("```math\n\\sqrt{x}\n```\n");
    PrDomDocument* document = domDocumentCreate(DomDocumentKind_Html);
    PrDomElement* element = domElementCreate(document, PrDomNamespace_Html, PrDomTag_Div, "div");
    domNodeRetain(element);
    PrMarkdownOptions options = everything();
    prHtmlSetMarkdown(element, markdown, &options);
    uint32_t mathml = 0;
    uint32_t other = 0;
    countMathMl(element, false, &mathml, &other);
    CHECK(mathml > 20);
    CHECK(other == 0);
    CHECK(treeOf(element).find("<math display=\"block\"><semantics><mrow><mo>[{]</mo><mtable ") != std::string::npos);
    domNodeRelease(element);
    domNodeRelease(document);
}

TEST_CASE("raw HTML nests with Markdown as the HTML that holds both does") {
    checkBuilds("<span>*a*</span> and <b>open\n\nnext paragraph\n");
    checkBuilds("<div>\n\n*inside*\n\n</div>\n\nafter\n");
    checkBuilds("<div>\nunclosed\n\n# heading\n\ntext\n");
    checkBuilds("</div>\n\npara\n\n</div>\n\nmore\n");
    checkBuilds("<table><tr><td>\n\n*cell*\n\n</td></tr></table>\n");
    checkBuilds("a <textarea> *b* </textarea> c\n\n*d*\n");
    checkBuilds("<!-- open comment\n\n*not emphasis*\n\n-->\n\nafter\n");
    checkBuilds("<script>\nlet a = '*';\n</script>\n\n*x*\n");
    checkBuilds("<pre>\n\nline\n</pre>\n");
    checkBuilds("text <a href=\"x\n");
}

TEST_CASE("a late definition and changed footnotes rebuild what they change") {
    checkBuilds("[ref] first\n\nsecond\n\n[ref]: /url\n");
    checkBuilds("[^n] first\n\nsecond\n\n[^n]: note\n\nthird[^m]\n\n[^m]: other\n");
    checkBuilds("- a\n- b\n\n  c\n- d\n");
}

TEST_CASE("a stream keeps the nodes of final blocks as more arrives") {
    PrDomDocument* document = domDocumentCreate(DomDocumentKind_Html);
    PrDomElement* element = domElementCreate(document, PrDomNamespace_Html, PrDomTag_Div, "div");
    domNodeRetain(element);
    PrMarkdownOptions options = prMarkdownOptionsDefault();
    PrHtmlMarkdownStream* stream = prHtmlMarkdownStreamCreate(element, &options);
    prHtmlMarkdownStreamAppend(stream, "first paragraph\n\nsecond");
    PrDomNode* first = element->first;
    REQUIRE(first != nullptr);
    prHtmlMarkdownStreamAppend(stream, " grows\n\nthird");
    CHECK(element->first == first);
    prHtmlMarkdownStreamEnd(stream);
    prHtmlMarkdownStreamDestroy(stream);
    CHECK(treeOf(element) == "<p>[first paragraph]</p>[\n]<p>[second grows]</p>[\n]<p>[third]</p>[\n]");
    domNodeRelease(element);
    domNodeRelease(document);
}

TEST_CASE("a text/markdown load parses the variant that its content type names") {
    struct Variant {
        const char* contentType;
        const char* body;
    };
    const Variant variants[] = {
        {"text/markdown", "<h1 id=\"hi\">Hi</h1>\n<p><del>x</del></p>\n"},
        {"text/markdown; variant=GFM", "<h1>Hi</h1>\n<p><del>x</del></p>\n"},
        {"text/markdown; charset=utf-8; variant=\"commonmark\"", "<h1>Hi</h1>\n<p>~~x~~</p>\n"},
    };
    for (const Variant& variant : variants) {
        PrHtmlPage* page = prHtmlPageCreate(nullptr);
        PrHtmlResponseHeaders headers{};
        headers.contentType = variant.contentType;
        REQUIRE(prHtmlPageLoad(page, "# Hi\n\n~~x~~\n", "https://example.com/a.md", &headers) == PrStatus_Ok);
        PrDomDocument* document = prHtmlWindowDocument(prHtmlPageWindow(page));
        PrRawString body = prHtmlInnerHtml(prDomElementAsNode(prDomDocumentBody(document)));
        CHECK(prRawStringView(&body) == variant.body);
        prRawStringReset(&body);
        prHtmlPageDestroy(page);
    }
}

TEST_CASE("a browsing context loads text/markdown as a document that runs no script") {
    PrHtmlPage* page = prHtmlPageCreate(nullptr);
    PrHtmlResponseHeaders headers{};
    headers.contentType = "text/markdown; charset=utf-8";
    REQUIRE(prHtmlPageLoad(page, "# Hi\n\n<script>x()</script>\n", "https://example.com/a.md", &headers) ==
        PrStatus_Ok);
    PrDomDocument* document = prHtmlWindowDocument(prHtmlPageWindow(page));
    PrRawString body = prHtmlInnerHtml(prDomElementAsNode(prDomDocumentBody(document)));
    CHECK(prRawStringView(&body) == "<h1 id=\"hi\">Hi</h1>\n<script>x()</script>\n");
    prRawStringReset(&body);
    CHECK(prHtmlDocumentReadyState(document) == PrHtmlDocumentReadyState_Complete);

    // A streamed load builds the blocks as they arrive.
    REQUIRE(prHtmlPageBeginLoad(page, "https://example.com/b.md", &headers) == PrStatus_Ok);
    prHtmlPageAppendLoad(page, "*one*\n\ntw");
    while (prDomAgentRunTask(prHtmlPageAgent(page))) {
    }
    document = prHtmlWindowDocument(prHtmlPageWindow(page));
    body = prHtmlInnerHtml(prDomElementAsNode(prDomDocumentBody(document)));
    CHECK(prRawStringView(&body) == "<p><em>one</em></p>\n<p>tw</p>\n");
    prRawStringReset(&body);
    prHtmlPageAppendLoad(page, "o\n");
    prHtmlPageEndLoad(page);
    while (prDomAgentRunTask(prHtmlPageAgent(page))) {
    }
    body = prHtmlInnerHtml(prDomElementAsNode(prDomDocumentBody(document)));
    CHECK(prRawStringView(&body) == "<p><em>one</em></p>\n<p>two</p>\n");
    prRawStringReset(&body);
    CHECK(prHtmlDocumentReadyState(document) == PrHtmlDocumentReadyState_Complete);
    prHtmlPageDestroy(page);
}
