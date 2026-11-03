// The Markdown DOM harness, which tools/suite/markdown/markdown_dom.py drives.
//
// A case's field is the options that it parses with, `commonmark` or the GFM extensions as cmark-gfm names them,
// separated by commas, and its bytes are its Markdown. The response says whether the tree that the HTML parser gives
// for lib/markdown's HTML of the Markdown is the tree that the Markdown builds: in a document, given whole and a byte
// at a time, and in an element.

#include "pr/assert.h"
#include "pr/html_markdown.h"
#include "pr/markdown.h"
#include "pr/markdown_html.h"
#include "pr/string_ops.h"

#include "html/src/html_markdown.h"
#include "html/src/html_parser.h"
#include "tools/harness/harness.h"

using namespace harness;

using namespace pr_impl;

namespace {

// Appends the tree under `node` to `out`, with each node's boundaries, so that adjacent Text nodes differ from one.
void writeTree(PrTextBuffer* out, const PrDomNode* node) {
    for (const PrDomNode* child = node->first; child != nullptr; child = child->next) {
        if (child->type == PrDomNodeType_Element) {
            const PrDomElement* element = domElementFromNode(child);
            prTextBufferAppendChar(out, '<');
            prTextBufferAppend(out, domElementName(element));
            for (const PrDomAttr* attribute = element->firstAttribute; attribute != nullptr;
                    attribute = static_cast<const PrDomAttr*>(attribute->next)) {
                prTextBufferAppendChar(out, ' ');
                prTextBufferAppend(out, prStringView(&attribute->name));
                prTextBufferAppend(out, "=\"");
                prTextBufferAppend(out, prStringView(&attribute->value));
                prTextBufferAppendChar(out, '"');
            }
            prTextBufferAppendChar(out, '>');
            writeTree(out, child);
            prTextBufferAppend(out, "</>");
        } else if (child->type == PrDomNodeType_Text || child->type == PrDomNodeType_Comment) {
            prTextBufferAppend(out, child->type == PrDomNodeType_Text ? "[" : "<!--");
            prTextBufferAppend(out, prStringView(&static_cast<const PrDomCharacterData*>(child)->data));
            prTextBufferAppend(out, child->type == PrDomNodeType_Text ? "]" : "-->");
        } else {
            prTextBufferAppend(out, "<?>");
        }
    }
}

bool sameTree(const PrDomNode* a, const PrDomNode* b) {
    PrTextBuffer left = {};
    PrTextBuffer right = {};
    writeTree(&left, a);
    writeTree(&right, b);
    bool same = prTextBufferView(&left) == prTextBufferView(&right);
    prTextBufferReset(&left);
    prTextBufferReset(&right);
    return same;
}

bool readOptions(PrStringView field, PrMarkdownOptions* out) {
    *out = prMarkdownOptionsDefault();
    out->extensions = 0;
    if (field == "commonmark")
        return true;
    size_t start = 0;
    while (start <= field.size) {
        size_t end = start;
        while (end < field.size && field.data[end] != ',')
            ++end;
        PrStringView name = prStringSlice(field, start, end - start);
        if (name == "table")
            out->extensions |= PrMarkdownExtensionFlag_Tables;
        else if (name == "tasklist")
            out->extensions |= PrMarkdownExtensionFlag_TaskLists;
        else if (name == "strikethrough")
            out->extensions |= PrMarkdownExtensionFlag_Strikethrough;
        else if (name == "autolink")
            out->extensions |= PrMarkdownExtensionFlag_Autolinks;
        else if (name == "footnotes")
            out->extensions |= PrMarkdownExtensionFlag_Footnotes;
        else if (name == "smart")
            out->extensions |= PrMarkdownExtensionFlag_SmartPunctuation;
        else if (name == "tagfilter")
            out->rawHtml = PrMarkdownRawHtml_Filter;
        else
            return false;
        start = end + 1;
    }
    return true;
}

// Returns true if Markdown builds into a document as the expected HTML parses, given whole or a byte at a time.
bool buildsDocument(const PrMarkdownOptions& options, PrStringView markdown, PrStringView html, bool streamed) {
    PrDomDocument* expected = domDocumentCreate(DomDocumentKind_Html);
    HtmlParser parser = {};
    htmlParserInit(&parser, expected, HtmlParseFlag_DeclarativeShadowRoots);
    PrTextBuffer source = {};
    prTextBufferAppend(&source, "<!DOCTYPE html><html><head></head><body>");
    prTextBufferAppend(&source, html);
    htmlParse(&parser, prTextBufferView(&source));
    htmlParserReset(&parser);
    prTextBufferReset(&source);

    PrDomDocument* built = domDocumentCreate(DomDocumentKind_Html);
    HtmlMarkdownBuilder builder;
    htmlMarkdownBuilderInitDocument(&builder, built, HtmlParseFlag_DeclarativeShadowRoots, &options);
    if (streamed) {
        for (size_t i = 0; i < markdown.size; ++i)
            htmlMarkdownBuilderAppend(&builder, prStringSlice(markdown, i, 1));
    } else {
        htmlMarkdownBuilderAppend(&builder, markdown);
    }
    htmlMarkdownBuilderEnd(&builder);
    htmlMarkdownBuilderReset(&builder);
    bool same = sameTree(expected, built);
    domNodeRelease(expected);
    domNodeRelease(built);
    return same;
}

// Returns true if Markdown builds into a div as the expected HTML parses as the div's contents.
bool buildsElement(const PrMarkdownOptions& options, PrStringView markdown, PrStringView html) {
    PrDomDocument* document = domDocumentCreate(DomDocumentKind_Html);
    PrDomElement* element = domElementCreate(document, PrDomNamespace_Html, PrDomTag_Div, "div");
    domNodeRetain(element);
    HtmlParser parser = {};
    htmlParserInit(&parser, document, 0);
    PrDomNode* fragment = htmlParseFragment(&parser, element, html);
    htmlParserReset(&parser);
    prHtmlSetMarkdown(element, markdown, &options);
    bool same = sameTree(fragment, element);
    domNodeRelease(fragment);
    domNodeRelease(element);
    domNodeRelease(document);
    return same;
}

// Returns lib/markdown's HTML of the whole of `markdown`: its blocks, then its footnotes.
PrString htmlOf(const PrMarkdownOptions& options, PrStringView markdown) {
    PrMarkdown* document = prMarkdownCreate(&options);
    prMarkdownParse(document, markdown);
    PrString html = prStringEmpty();
    prMarkdownAppendHtml(document, 0, prMarkdownBlockCount(document), &html);
    prMarkdownAppendFootnotesHtml(document, &html);
    prMarkdownDestroy(document);
    return html;
}

} // namespace

int main() {
    prCrashHandler();
    HarnessCase test = {};
    while (harnessReadCase(&test)) {
        PrMarkdownOptions options{};
        if (test.fieldCount != 1 || !readOptions(test.fields[0], &options))
            return 1;
        PrStringView markdown = prTextBufferView(&test.input);
        PrString written = htmlOf(options, markdown);
        PrStringView html = prStringView(&written);
        PrTextBuffer out = {};
        prTextBufferAppend(&out, "{\"whole\":");
        prTextBufferAppend(&out, buildsDocument(options, markdown, html, false) ? "true" : "false");
        prTextBufferAppend(&out, ",\"streamed\":");
        prTextBufferAppend(&out, buildsDocument(options, markdown, html, true) ? "true" : "false");
        prTextBufferAppend(&out, ",\"element\":");
        prTextBufferAppend(&out, buildsElement(options, markdown, html) ? "true" : "false");
        prTextBufferAppendChar(&out, '}');
        harnessWriteLine(prTextBufferView(&out));
        prTextBufferReset(&out);
        prStringReset(&written);
    }
    harnessCaseReset(&test);
    return 0;
}
