// The CommonMark and GFM harness, which tools/suite/markdown/markdown_spec.py drives.
//
// The first field of a case names its options: `commonmark` for CommonMark alone, or the extensions that it parses, as
// cmark-gfm names them. A second field of `whole` leaves out the streamed parse. The response is the case's HTML parsed
// whole and the milliseconds that the parse took, and, unless the case is whole, whether a parse that takes the source
// a byte at a time gives the same HTML, both at its end and as the blocks that its changes report.

#include "pr/array.h"
#include "pr/assert.h"
#include "pr/markdown.h"
#include "pr/markdown_html.h"
#include "pr/string_ops.h"

#include "tools/harness/harness.h"

using namespace harness;

using namespace pr_impl;

namespace {

PrString htmlOf(const PrMarkdown* document, uint32_t first, uint32_t count) {
    PrString html = prStringEmpty();
    prMarkdownAppendHtml(document, first, count, &html);
    return html;
}

// Returns the HTML of the whole document: its blocks', then its footnotes'.
PrString documentHtml(const PrMarkdown* document) {
    PrString html = htmlOf(document, 0, prMarkdownBlockCount(document));
    prMarkdownAppendFootnotesHtml(document, &html);
    return html;
}

// Applies the document's changes to `blocks`, the HTML of each top-level block as the client last read it.
void applyChanges(PrMarkdown* document, Array<PrString>* blocks) {
    PrMarkdownChange changes[4];
    uint32_t count = 0;
    while ((count = prMarkdownTakeChanges(document, changes, 4)) != 0) {
        for (uint32_t c = 0; c < count; ++c) {
            const PrMarkdownChange& change = changes[c];
            for (uint32_t i = 0; i < change.removedCount; ++i)
                prStringReset(&blocks->data[change.first + i]);
            blocks->erase(change.first, change.removedCount);
            for (uint32_t i = 0; i < change.insertedCount; ++i)
                blocks->insert(change.first + i, htmlOf(document, change.first + i, 1));
        }
    }
}

// Returns true if parsing `source` a byte at a time gives `whole`, both at the end and as the client's blocks.
bool streams(const PrMarkdownOptions& options, PrStringView source, PrStringView whole) {
    PrMarkdown* document = prMarkdownCreate(&options);
    Array<PrString> blocks;
    bool agrees = true;
    for (size_t i = 0; i < source.size; ++i) {
        prMarkdownAppend(document, prStringSlice(source, i, 1));
        applyChanges(document, &blocks);
        agrees = agrees && blocks.count == prMarkdownBlockCount(document);
    }
    prMarkdownEnd(document);
    applyChanges(document, &blocks);
    PrString html = documentHtml(document);
    agrees = agrees && prStringView(&html) == whole;
    PrTextBuffer joined = {};
    for (uint32_t i = 0; i < blocks.count; ++i) {
        prTextBufferAppend(&joined, prStringView(&blocks.data[i]));
        prStringReset(&blocks.data[i]);
    }
    PrString footnotes = prStringEmpty();
    prMarkdownAppendFootnotesHtml(document, &footnotes);
    prTextBufferAppend(&joined, prStringView(&footnotes));
    prStringReset(&footnotes);
    agrees = agrees && prTextBufferView(&joined) == whole;
    prTextBufferReset(&joined);
    prStringReset(&html);
    blocks.reset();
    prMarkdownDestroy(document);
    return agrees;
}

// Reads the options that a case's field names: `commonmark`, or extensions as cmark-gfm names them, separated by
// commas. Returns false if the field names an extension that the document has no option for.
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

} // namespace

int main() {
    prCrashHandler();
    HarnessCase test = {};
    while (harnessReadCase(&test)) {
        if (test.fieldCount < 1 || test.fieldCount > 2 || (test.fieldCount == 2 && test.fields[1] != "whole"))
            return 1;
        PrMarkdownOptions options{};
        if (!readOptions(test.fields[0], &options))
            return 1;
        PrStringView source = prTextBufferView(&test.input);
        HarnessCounters before = harnessCounters();
        PrMarkdown* document = prMarkdownCreate(&options);
        prMarkdownParse(document, source);
        PrString html = documentHtml(document);
        prMarkdownDestroy(document);
        HarnessCounters spent = harnessSpent(before, harnessCounters());

        PrTextBuffer out = {};
        prTextBufferAppend(&out, "{\"html\":");
        harnessWriteString(&out, prStringView(&html));
        prTextBufferAppend(&out, ",\"millis\":");
        harnessWriteNumber(&out, uint64_t(spent.millis));
        if (test.fieldCount == 1) {
            prTextBufferAppend(&out, ",\"streamed\":");
            prTextBufferAppend(&out, streams(options, source, prStringView(&html)) ? "true" : "false");
        }
        prTextBufferAppendChar(&out, '}');
        harnessWriteLine(prTextBufferView(&out));
        prTextBufferReset(&out);
        prStringReset(&html);
    }
    harnessCaseReset(&test);
    return 0;
}
