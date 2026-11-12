// The engine's own measurements, without a shell.
//
// ./dev.sh bench html-parse runs this and prints its rows beside the rows that a browser reports. The measurements
// match the ones that tools/bench/scripts/html-parse.js makes, so both rows mean the same thing: the cost of parsing a
// page into a tree, and the size of the tree.
//
// Two measurements belong to this binary alone: the tokenizer on its own, and the speed of reading the input once. The
// second is the bound against which every pass over the source is measured.
//
// html/#parsing - https://html.spec.whatwg.org/multipage/parsing.html

#include "pr/assert.h"
#include "pr/html.h"
#include "pr/print.h"
#include "pr/string_ops.h"

#include "html/src/html_tokenizer.h"
#include "tools/harness/harness.h"

using namespace harness;

using namespace pr_impl;

namespace {

uint32_t countNodes(PrDomNode* node) {
    uint32_t count = 1;
    for (PrDomNode* child = prDomNodeFirstChild(node); child != nullptr; child = prDomNodeNextSibling(child))
        count += countNodes(child);
    return count;
}

// The value that harnessReadBound folded the source into.
volatile uint8_t sink = 0;

void readOnce(PrStringView text) {
    sink = harnessReadBound(text);
}

// The tokenizer on its own, which is the parse without building a tree.
void tokenizeOnly(PrStringView text) {
    HtmlTokenizer tokenizer = {};
    RawArray<HtmlError> errors;

    htmlTokenizerStart(&tokenizer, text, &errors);
    for (;;) {
        htmlTokenizerNext(&tokenizer);
        if (tokenizer.token.kind == HtmlTokenKind_Eof)
            break;
    }

    htmlTokenizerReset(&tokenizer);
    errors.reset();
}

// Runs the parse: the tokenizer, the tree builder, and the tree that they build.
PrDomDocument* parseOnce(PrStringView text) {
    return prHtmlDomParserParseFromString(text, PrHtmlDomParserSupportedType_TextHtml);
}

void measure(PrStringView name, PrStringView text) {
    HarnessCounters parse = harnessMeasure([&] { prDomDocumentRelease(parseOnce(text)); });
    HarnessCounters tokenize = harnessMeasure([&] { tokenizeOnly(text); });
    HarnessCounters bound = harnessMeasure([&] { readOnce(text); });

    // The tree is counted outside the measures, since only the parse is measured.
    PrDomDocument* document = parseOnce(text);
    uint32_t nodes = countNodes(prDomDocumentAsNode(document));
    prDomDocumentRelease(document);

    HarnessCounts counts{};
    harnessCountsAdd(&counts, "parse", parse);
    harnessCountsAdd(&counts, "tokenize", tokenize);
    harnessCountsAdd(&counts, "bound", bound);
    PrTextBuffer row{};
    prTextBufferAppend(&row, "{\"name\": ");
    harnessWriteString(&row, name);
    harnessAppendFormat(&row, ", \"bytes\": %zu, \"nodes\": %u, \"parse\": %.4f, \"tokenize\": %.4f, \"bound\": %.4f",
        text.size, nodes, parse.millis, tokenize.millis, bound.millis);
    harnessCountsWrite(&counts, &row);
    prTextBufferAppend(&row, "}");
    harnessWriteLine(prTextBufferView(&row));
    prTextBufferReset(&row);
}

} // namespace

int main(int argc, char** argv) {
    prCrashHandler();
    if (argc < 2) {
        printError("usage: html_bench <page>...\n");
        return 2;
    }

    for (int i = 1; i < argc; ++i) {
        PrTextBuffer page = {};
        if (!harnessReadFile(&page, argv[i])) {
            printError("cannot read {}\n", argv[i]);
            return 2;
        }
        measure(harnessFileName(argv[i]), prTextBufferView(&page));
        prTextBufferReset(&page);
    }
    return 0;
}
