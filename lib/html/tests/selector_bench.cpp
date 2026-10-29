// The selector benchmark's cases through the C API, without a shell.
//
// ./dev.sh bench selectors runs this and prints its rows beside the rows that pr_shell reports. Each case is the one
// that tools/bench/scripts/selectors.js names. It runs through C the query that the page runs through the DOM, on the
// same document, batched and measured the same way. The difference between the two rows is therefore the cost of the
// binding and QuickJS. The document belongs to a browsing context, as the page's does, with no scripting.
//
// Usage: selector_bench [case]
//
// If a case is named, only that case runs, repeatedly, for five minutes (harnessRepeat). This is for attaching a
// profiler.

#include "pr/assert.h"
#include "pr/html.h"
#include "pr/html_page.h"
#include "pr/string_ops.h"
#include "pr/text_buffer.h"

#include "tools/harness/harness.h"

#include <stdio.h>

using namespace harness;

using namespace pr_impl;

namespace {

// The number of siblings in the list, and the number of levels in the tree and of elements per parent.
constexpr uint32_t kSiblings = 1000;
constexpr uint32_t kDepth = 3;
constexpr uint32_t kFanout = 10;

// The number of levels in the chain, and of spans in each level.
constexpr uint32_t kChain = 100;
constexpr uint32_t kChainSpans = 5;

// The query that a case runs on its element or document.
enum {
    Ask_First,
    Ask_All,
    Ask_Matches,
    Ask_Closest,
};
typedef uint8_t Ask;

struct Case {
    const char* name;
    const char* on;         // the ID of the element that it queries, or "document"
    Ask ask;
    const char* selector;
};

// bench.js's kCases.
constexpr Case kCases[] = {
    {"querySelector id", "document", Ask_First, "#target"},
    {"querySelector class", "tree", Ask_First, ".x"},
    {"querySelectorAll class", "tree", Ask_All, ".item"},
    {"querySelectorAll descendant", "tree", Ask_All, "div.group > div span.x"},
    {"querySelectorAll tag", "tree", Ask_All, "span"},
    {"querySelectorAll attribute", "list", Ask_All, "[class=a]"},
    {"querySelectorAll id descendant", "document", Ask_All, "#tree .x"},
    {"querySelectorAll list", "tree", Ask_All, "p, .x, #leaf"},
    {"querySelectorAll not", "list", Ask_All, "li:not(.a)"},
    {"querySelectorAll is", "tree", Ask_All, ":is(div, section) > .x"},
    {"querySelectorAll deep descendant", "deep", Ask_All, "section span"},
    {"querySelectorAll deep child", "deep", Ask_All, ".level > .level > span.leaf"},
    {"querySelectorAll nth-child", "list", Ask_All, ":nth-child(2n+1)"},
    {"querySelectorAll nth-last-child", "list", Ask_All, "li:nth-last-child(3n)"},
    {"querySelectorAll nth-of-type", "list", Ask_All, "li:nth-of-type(2n)"},
    {"querySelectorAll nth-child of", "list", Ask_All, ":nth-child(odd of .a)"},
    {"querySelectorAll has child", "tree", Ask_All, "div:has(> .x) span"},
    {"querySelectorAll has descendant", "tree", Ask_All, ".group:has(.x) .item"},
    {"querySelectorAll has next sibling", "list", Ask_All, "li:has(+ p)"},
    {"querySelectorAll has later sibling", "list", Ask_All, "li:has(~ .a.b)"},
    {"querySelectorAll has absent class", "list", Ask_All, "li:has(+ .absent)"},
    {"querySelectorAll has after class", "tree", Ask_All, ":has(~ * .x).item"},
    {"querySelectorAll has sibling pair", "list", Ask_All, "li:has(~ p + p)"},
    {"querySelectorAll has deep", "deep", Ask_All, "div:has(p)"},
    {"querySelectorAll has deep child", "deep", Ask_All, "div:has(> div > p)"},
    {"matches", "leaf", Ask_Matches, ".group > .group .item:nth-child(10n+1)"},
    {"closest", "leaf", Ask_Closest, "#tree"},
    {"closest deep", "bottom", Ask_Closest, "#deep"},
};

struct Work {
    const Case* current;
    PrDomNode* target;
    PrDomNodeList* list;    // the list that querySelectorAll fills, reused across runs
};

void run(Work* work) {
    PrStringView selector = work->current->selector;
    PrDomElement* element = prDomElementQuery(prDomNodeAsEventTarget(work->target));
    PrDomElement* found = nullptr;
    bool matched = false;
    switch (work->current->ask) {
        case Ask_First:
            prCssNodeQuerySelector(work->target, selector, &found);
            break;
        case Ask_All:
            prCssNodeQuerySelectorAll(work->target, selector, work->list);
            break;
        case Ask_Matches:
            prCssElementMatches(element, selector, &matched);
            break;
        case Ask_Closest:
            prCssElementClosest(element, selector, &found);
            break;
    }
}

// bench.js's listMarkup.
void listMarkup(TextBuffer* out) {
    textAppend(out, "<ul id=\"list\">");
    for (uint32_t i = 0; i < kSiblings; ++i) {
        PrStringView tag = i % 4 == 3 ? "p" : "li";
        textAppend(out, "<");
        textAppend(out, tag);
        textAppend(out, i % 3 == 0 ? " class=\"a\"></" : " class=\"b\"></");
        textAppend(out, tag);
        textAppend(out, ">");
    }
    textAppend(out, "</ul>");
}

// bench.js's treeMarkup.
void level(TextBuffer* out, uint32_t depth, uint32_t* leaves) {
    for (uint32_t i = 0; i < kFanout; ++i) {
        if (depth > 1) {
            textAppend(out, "<div class=\"group\">");
            level(out, depth - 1, leaves);
            textAppend(out, "</div>");
            continue;
        }
        uint32_t leaf = (*leaves)++;
        textAppend(out, leaf == 500 ? "<span id=\"leaf\"" : "<span");
        textAppend(out, leaf % 7 == 0 ? " class=\"item x\">text</span>" : " class=\"item\">text</span>");
    }
}

// bench.js's chainMarkup.
void chainMarkup(TextBuffer* out) {
    textAppend(out, "<div id=\"deep\">");
    for (uint32_t i = 0; i < kChain; ++i) {
        textAppend(out, "<div class=\"level\">");
        for (uint32_t j = 0; j < kChainSpans; ++j)
            textAppend(out, "<span class=\"leaf\"></span>");
    }
    textAppend(out, "<span id=\"bottom\"></span>");
    for (uint32_t i = 0; i < kChain; ++i)
        textAppend(out, "</div>");
    textAppend(out, "</div>");
}

void treeMarkup(TextBuffer* out) {
    uint32_t leaves = 0;
    textAppend(out, "<div id=\"tree\">");
    level(out, kDepth, &leaves);
    textAppend(out, "</div><p id=\"target\"></p>");
}

} // namespace

int main(int argc, char** argv) {
    prCrashHandler();
    if (argc > 2) {
        fprintf(stderr, "usage: selector_bench [case]\n");
        return 2;
    }
    TextBuffer markup = {};
    listMarkup(&markup);
    treeMarkup(&markup);
    chainMarkup(&markup);

    PrHtmlPage* page = prHtmlPageCreate(nullptr);
    prHtmlPageLoad(page, "<!doctype html><body>", "about:blank", nullptr);
    PrDomDocument* document = prHtmlWindowDocument(prHtmlPageWindow(page));
    PrDomElement* host = nullptr;
    prDomElementCreate(document, "div", nullptr, &host);
    prDomNodeAppendChild(prDomElementAsNode(prDomDocumentBody(document)), prDomElementAsNode(host));
    prDomNodeRelease(prDomElementAsNode(host));
    prHtmlSetInnerHtml(prDomElementAsNode(host), textView(&markup));

    PrStringView only = argc == 2 ? argv[1] : PrStringView{};
    Work work = {.list = prDomNodeListCreate()};
    printf("[");
    bool first = true;
    for (const Case& c : kCases) {
        if (only.data != nullptr && only != c.name)
            continue;
        work.current = &c;
        PrStringView on = c.on;
        work.target =
            on == "document"
                ? prDomDocumentAsNode(document)
                : prDomElementAsNode(prDomNodeGetElementById(prDomDocumentAsNode(document), on));
        if (only.data != nullptr) {
            harnessRepeat([&] { run(&work); });
        }
        harnessWriteCase(first, c.name, harnessMeasure([&] { run(&work); }));
        first = false;
    }
    printf("\n]\n");

    prDomNodeListDestroy(work.list);
    prHtmlPageDestroy(page);
    textReset(&markup);
    return 0;
}
