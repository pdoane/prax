// The style benchmark's cases through the C API, without a shell.
//
// ./dev.sh bench restyle runs this and prints its rows beside the rows that pr_shell reports. Each case is the one that
// tools/bench/scripts/restyle.js names, done through C: the same change, then the same read of a resolved value through
// the element's computed style, which brings the presentation up to date. The page belongs to a browsing context that
// renders in the checkout's fonts, as the shell's pages do, with no scripting.
//
// Usage: style_bench <large.css> [case]
//
// If a case is named, only that case runs, repeatedly, for five minutes (harnessRepeat). This is for attaching a
// profiler.

#include "pr/assert.h"
#include "pr/count_of.h"
#include "pr/css.h"
#include "pr/html.h"
#include "pr/html_page.h"
#include "pr/layout_style.h"
#include "pr/string_ops.h"
#include "pr/text_buffer.h"
#include "pr/text_harfbuzz.h"

#include "tools/harness/harness.h"

#include <stdio.h>

using namespace harness;

using namespace pr_impl;

namespace {

// The number of cards in the page and of items in its list, as in bench.js.
constexpr uint32_t kCards = 100;
constexpr uint32_t kItems = 50;

// The rules whose matches the cases change, as in bench.js.
constexpr char kRules[] =
    ".card { display: block; margin: 4px; padding: 4px 8px; border: 1px solid rgb(200, 200, 200); max-width: 600px }"
    ".card h2 { font-size: 15px; margin: 0 0 4px 0 }.card p { margin: 2px 0; color: var(--text); line-height: 1.4 }"
    ".card span { color: rgb(0, 0, 200); padding: 0 2px }"
    ".theme-dark .card { background-color: rgb(20, 20, 20); color: rgb(240, 240, 240) }"
    ".card.active { background-color: rgb(255, 255, 200) }.card[data-state=open] p { font-weight: 700 }"
    ".card:has(.flag) { border-color: rgb(200, 0, 0) }.list li { display: block; height: 12px }"
    ".list li:nth-child(odd) { color: rgb(120, 120, 120) }:root { --text: rgb(51, 51, 51) }";

// The number of :has() rules in the sheet of anchors, and how many of them have an argument that reaches a later
// sibling, as in bench.js.
constexpr uint32_t kAnchors = 400;
constexpr uint32_t kSiblingAnchors = 8;

// The sheet of anchors, as in bench.js. No rule matches the page.
void anchorRules(PrTextBuffer* out) {
    for (uint32_t i = 0; i < kAnchors - kSiblingAnchors; ++i) {
        prTextBufferAppend(out, ".anchor");
        prTextBufferAppendU64(out, i, 10);
        prTextBufferAppend(out, ":has(.flag");
        prTextBufferAppendU64(out, i, 10);
        prTextBufferAppend(out, ") { outline: 1px solid rgb(200, 0, 0) }\n");
    }
    for (uint32_t i = 0; i < kSiblingAnchors; ++i) {
        prTextBufferAppend(out, ".sibling");
        prTextBufferAppendU64(out, i, 10);
        prTextBufferAppend(out, ":has(~ .flag");
        prTextBufferAppendU64(out, i, 10);
        prTextBufferAppend(out, ") { outline: 1px solid rgb(0, 0, 200) }\n");
    }
}

// The state that the cases work on.
struct Work {
    PrDomDocument* document;
    PrDomElement* root;
    PrDomNode* host;
    PrStringView html;
    PrCssStyleSheet* own;
    PrDomElement* cards[kCards];
    PrDomElement* lastText;
    PrDomElement* middleText;
    PrDomElement* middleSpan;
    PrDomNode* list;
    PrDomNode* extra;
    PrDomNode* inserted;            // the element that the :has anchors insert case inserts and removes
    bool flip;
    int64_t height;                 // the value that the geometry loop read, so that no read is dropped
};

typedef void (*BodyFn)(Work* work);

// A case, and what it sets up before it is measured, if anything.
struct Case {
    const char* name;
    BodyFn run;
    BodyFn setup = nullptr;
};

// The markup of the page's content.
void markup(PrTextBuffer* out) {
    for (uint32_t i = 0; i < kCards; ++i) {
        prTextBufferAppend(out, "<div class=\"card\" id=\"card");
        prTextBufferAppendU64(out, i, 10);
        prTextBufferAppend(out, "\"><h2>Card ");
        prTextBufferAppendU64(out, i, 10);
        prTextBufferAppend(out, "</h2><p>Some text with <span>an inline span</span> in a paragraph of a card.</p></div>");
    }
    prTextBufferAppend(out, "<ul class=\"list\">");
    for (uint32_t i = 0; i < kItems; ++i) {
        prTextBufferAppend(out, "<li>item ");
        prTextBufferAppendU64(out, i, 10);
        prTextBufferAppend(out, "</li>");
    }
    prTextBufferAppend(out, "</ul>");
}

// The value that a read of the resolved value gives, stored where the optimizer cannot remove the read.
volatile size_t readSize;

void read(PrDomElement* element, PrCssProperty property) {
    PrCssDeclarationName name = prCssDeclarationNameFromProperty(property);
    PrString value = prCssStyleDeclarationGetPropertyValue(
        prCssStylePropertiesAsCssStyleDeclaration(prCssElementComputedStyle(element, PrStringView{})), &name);
    readSize = prStringView(&value).size;
    prStringReset(&value);
}

PrDomElement* element(Work* work, PrStringView name) {
    PrDomElement* made = nullptr;
    prDomElementCreate(work->document, name, nullptr, &made);
    return made;
}

// Returns the first element child of `parent` named `name`.
PrDomElement* firstNamed(PrDomElement* parent, PrStringView name) {
    PrDomHtmlCollection* found = prDomHtmlCollectionCreateByTagName(prDomElementAsNode(parent), name);
    PrDomElement* first = prDomHtmlCollectionElementAt(found, 0);
    prDomHtmlCollectionDestroy(found);
    return first;
}

// Fills the host with the page's content, and finds the elements that the cases change and read.
void build(Work* work) {
    prHtmlSetInnerHtml(work->host, work->html);
    PrDomElement* host = prDomElementQuery(prDomNodeAsEventTarget(work->host));
    PrDomHtmlCollection* cards = prDomHtmlCollectionCreateByClassName(prDomElementAsNode(host), "card");
    for (uint32_t i = 0; i < kCards; ++i)
        work->cards[i] = prDomHtmlCollectionElementAt(cards, i);
    prDomHtmlCollectionDestroy(cards);
    work->lastText = firstNamed(work->cards[kCards - 1], "p");
    work->middleText = firstNamed(work->cards[kCards >> 1], "p");
    work->middleSpan = firstNamed(work->cards[kCards >> 1], "span");
    work->list = prDomNodeLastChild(work->host);
}

// Appends a style element that holds `text` to the document's head. Returns its sheet.
PrCssStyleSheet* addStyle(Work* work, PrStringView text);

void toggle(PrDomElement* element, PrStringView token) {
    bool on = false;
    PrDomTokenList* classes = prDomTokenListCreate(element, "class");
    prDomTokenListToggle(classes, token, nullptr, &on);
    prDomTokenListDestroy(classes);
}

constexpr Case kCases[] = {
    {
        "style page build",
        [](Work* w) {
            build(w);
            read(prDomElementQuery(prDomNodeAsEventTarget(prDomNodeLastChild(w->list))), PrCssProperty_Color);
        },
    },
    {
        "style class on root",
        [](Work* w) {
            toggle(w->root, "theme-dark");
            read(w->lastText, PrCssProperty_Color);
        },
    },
    {
        "style class on leaf",
        [](Work* w) {
            toggle(w->cards[kCards >> 1], "active");
            read(w->cards[kCards >> 1], PrCssProperty_BackgroundColor);
        },
    },
    {
        "style attribute",
        [](Work* w) {
            w->flip = !w->flip;
            prDomElementSetAttribute(w->cards[kCards >> 1], "data-state", w->flip ? "open" : "closed");
            read(w->middleText, PrCssProperty_FontWeight);
        },
    },
    {
        "style custom property",
        [](Work* w) {
            w->flip = !w->flip;
            PrCssDeclarationName name = prCssDeclarationNameFromProperty(PrCssProperty_Count);
            name.author = prStringExternal("--text");
            PrCssStyleDeclaration* style = prCssStylePropertiesAsCssStyleDeclaration(prCssElementStyle(w->root));
            prCssStyleDeclarationSetProperty(style, &name, w->flip ? "rgb(200, 0, 0)" : "rgb(51, 51, 51)", false);
            read(w->lastText, PrCssProperty_Color);
        },
    },
    {
        "style nth-child insert",
        [](Work* w) {
            if (prDomNodeParentNode(w->extra) == nullptr)
                prDomNodeInsertBefore(w->list, w->extra, prDomNodeFirstChild(w->list));
            else
                prDomNodeRemove(w->extra);
            read(prDomElementQuery(prDomNodeAsEventTarget(prDomNodeLastChild(w->list))), PrCssProperty_Color);
        },
    },
    {
        "style :has",
        [](Work* w) {
            toggle(w->middleSpan, "flag");
            read(w->cards[kCards >> 1], PrCssProperty_BorderTopColor);
        },
    },
    {
        "style insertRule",
        [](Work* w) {
            uint32_t count = prCssRuleListLength(prCssStyleSheetCssRules(w->own));
            prCssStyleSheetInsertRule(w->own, ".card span { color: rgb(0, 120, 0) }", count, nullptr);
            read(w->middleSpan, PrCssProperty_Color);
            prCssStyleSheetDeleteRule(w->own, count);
        },
    },
    {
        "style write read loop",
        [](Work* w) {
            PrCssDeclarationName width = prCssDeclarationNameFromProperty(PrCssProperty_Width);
            PrCssStyleDeclaration* style = prCssStylePropertiesAsCssStyleDeclaration(prCssElementStyle(w->cards[kCards - 1]));
            char text[16];
            for (uint32_t i = 0; i < 10; ++i) {
                int size = snprintf(text, sizeof(text), "%upx", 300 + i);
                prCssStyleDeclarationSetProperty(style, &width, PrStringView{text, size_t(size)}, false);
                read(w->cards[kCards - 1], PrCssProperty_Width);
            }
        },
    },
    {
        "style geometry loop",
        [](Work* w) {
            PrCssDeclarationName width = prCssDeclarationNameFromProperty(PrCssProperty_Width);
            PrCssStyleDeclaration* style = prCssStylePropertiesAsCssStyleDeclaration(prCssElementStyle(w->cards[kCards - 2]));
            PrHtmlElement* text = prHtmlElementQuery(
                prDomNodeAsEventTarget(prDomElementAsNode(firstNamed(w->cards[kCards - 2], "p"))));
            char value[16];
            for (uint32_t i = 0; i < 10; ++i) {
                int size = snprintf(value, sizeof(value), "%upx", 200 + 10 * i);
                prCssStyleDeclarationSetProperty(style, &width, PrStringView{value, size_t(size)}, false);
                w->height += prHtmlElementOffsetHeight(text);
            }
        },
    },
    {
        "style :has anchors page build",
        [](Work* w) {
            build(w);
            read(prDomElementQuery(prDomNodeAsEventTarget(prDomNodeLastChild(w->list))), PrCssProperty_Color);
        },
        [](Work* w) {
            PrTextBuffer rules = {};
            anchorRules(&rules);
            addStyle(w, prTextBufferView(&rules));
            prTextBufferReset(&rules);
            read(prDomElementQuery(prDomNodeAsEventTarget(prDomNodeLastChild(w->list))), PrCssProperty_Color);
        },
    },
    {
        "style :has anchors insert",
        [](Work* w) {
            if (prDomNodeParentNode(w->inserted) == nullptr)
                prDomNodeAppendChild(prDomElementAsNode(w->middleText), w->inserted);
            else
                prDomNodeRemove(w->inserted);
            read(w->lastText, PrCssProperty_Color);
        },
    },
    {
        "style :has anchors text",
        [](Work* w) {
            w->flip = !w->flip;
            PrDomNode* text = prDomNodeFirstChild(prDomElementAsNode(w->middleSpan));
            prDomCharacterDataSetData(prDomCharacterDataQuery(prDomNodeAsEventTarget(text)),
                w->flip ? "a changed span" : "an inline span");
            read(w->lastText, PrCssProperty_Color);
        },
    },
};

PrCssStyleSheet* addStyle(Work* work, PrStringView text) {
    PrDomElement* style = element(work, "style");
    prDomNodeSetTextContent(prDomElementAsNode(style), text);
    prDomNodeAppendChild(prDomElementAsNode(prDomDocumentHead(work->document)), prDomElementAsNode(style));
    prDomNodeRelease(prDomElementAsNode(style));
    return prHtmlStyleElementSheet(prHtmlStyleElementQuery(prDomNodeAsEventTarget(prDomElementAsNode(style))));
}

} // namespace

int main(int argc, char** argv) {
    prCrashHandler();
    if (argc != 2 && argc != 3) {
        fprintf(stderr, "usage: style_bench <large.css> [case]\n");
        return 2;
    }
    PrTextBuffer large = {};
    if (!harnessReadFile(&large, argv[1])) {
        fprintf(stderr, "style_bench: cannot read the stylesheet\n");
        return 1;
    }
    // The checkout's fonts, which the shell renders in.
    PrFontSet* fonts = prFontSetCreate(nullptr);
    const char* const kFonts[][2] = {{"Tinos.ttf", "serif"}, {"Tinos-Bold.ttf", nullptr}, {"Arimo.ttf", "sans-serif"}};
    const char* const kFamilies[] = {"Tinos", "Tinos", "Arimo"};
    for (uint32_t i = 0; i < countOf(kFonts); ++i) {
        PrTextBuffer path = {};
        prTextBufferAppend(&path, PR_FONTS_DIR);
        prTextBufferAppend(&path, "/");
        prTextBufferAppend(&path, kFonts[i][0]);
        const PrFont* font = nullptr;
        if (prFontCreateFromFile(prTextBufferView(&path), 0, nullptr, &font) == PrStatus_Ok) {
            PrFontFaceDescriptor descriptor = prFontFaceDescriptorDefault();
            prFontSetAdd(fonts, &descriptor, font);
            prFontRelease(font);
            if (kFonts[i][1] != nullptr)
                prFontSetSetGeneric(fonts, prFontGenericFromName(kFonts[i][1]), kFamilies[i]);
        }
        prTextBufferReset(&path);
    }
    prFontSetSetDefaultFamily(fonts, prFontFamilyGeneric(PrFontGeneric_Serif));
    PrTextShaper* shaper = prTextHarfBuzzCreate();
    PrTextFontDesc defaultFontDesc{.set = fonts, .style = prFontStyleDefault()};
    PrTextFont* defaultFont = prTextFontCreate(&defaultFontDesc);
    PrHtmlHost host = prHtmlHostDefault();
    host.font = defaultFont;
    host.shaper = shaper;
    PrHtmlPage* page = prHtmlPageCreate(&host);
    prHtmlPageLoad(page, "<!doctype html><body>", "about:blank", nullptr);

    PrTextBuffer html = {};
    markup(&html);
    Work work = {};
    work.document = prHtmlWindowDocument(prHtmlPageWindow(page));
    work.root = prDomDocumentElement(work.document);
    addStyle(&work, prTextBufferView(&large));
    work.own = addStyle(&work, kRules);
    work.host = prDomElementAsNode(element(&work, "div"));
    prDomNodeAppendChild(prDomElementAsNode(prDomDocumentBody(work.document)), work.host);
    work.html = prTextBufferView(&html);
    work.extra = prDomElementAsNode(element(&work, "li"));
    work.inserted = prDomElementAsNode(element(&work, "em"));
    build(&work);

    PrStringView only = argc == 3 ? argv[2] : PrStringView{};
    printf("[");
    for (uint32_t i = 0; i < countOf(kCases); ++i) {
        const Case& c = kCases[i];
        if (c.setup != nullptr)
            c.setup(&work);
        if (only.data != nullptr) {
            if (only != c.name)
                continue;
            harnessRepeat([&] { c.run(&work); });
        }
        // The page is rebuilt after the case that builds it, in the same way as the setup of bench.js's next case.
        HarnessCounters cost = harnessMeasure([&] { c.run(&work); });
        if (i == 0)
            build(&work);
        harnessWriteCase(i == 0, c.name, cost);
    }
    printf("\n]\n");

    prDomNodeRelease(work.inserted);
    prDomNodeRelease(work.extra);
    prDomNodeRelease(work.host);
    prHtmlPageDestroy(page);
    prTextFontRelease(defaultFont);
    prTextShaperDestroy(shaper);
    prFontSetDestroy(fonts);
    prTextBufferReset(&html);
    prTextBufferReset(&large);
    return 0;
}
