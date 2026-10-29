// The cost of a long list's markers and counters: a list of many items, loaded in pieces, and frames after changes to
// it.
//
// ./dev.sh bench lists runs this and prints its rows. Every item of the list takes part in the counter pass, which
// numbers the items again after a change that adds, removes or restyles one in a way that reaches the counters. The
// cases separate the costs: a change that renumbers every item, one that renumbers none but adds an item, and a restyle
// of an item that reaches no counter, which should cost no pass at all.
//
// Usage: list_bench [--items N] [--verify] [case]
//
// --items sets how many items the list holds; 10000 by default. --verify checks the presentation after each case
// against a full recomputation (prCssPresentationVerify), and a failure fails the run. If a case is named, only that
// case runs, repeatedly, for five minutes (harnessRepeat). This is for attaching a profiler.

#include "pr/assert.h"
#include "pr/count_of.h"
#include "pr/css_presentation.h"
#include "pr/html.h"
#include "pr/html_page.h"
#include "pr/layout_scene.h"
#include "pr/layout_tree.h"
#include "pr/string_ops.h"
#include "pr/text_buffer.h"
#include "pr/text_harfbuzz.h"

#include "tools/harness/harness.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

using namespace harness;

using namespace pr_impl;

namespace {

// The number of items in the list by default.
constexpr uint32_t kDefaultItems = 10000;

// The viewport that the page is laid out in.
constexpr PrLayoutVec kViewport{{1024 * kPrLayoutUnitsPerPx}, {768 * kPrLayoutUnitsPerPx}};

// The size in bytes of each piece that a load delivers.
constexpr uint32_t kChunk = 4096;

// The page's rules. The items' markers are inside, so that their boxes hold the numbers that the counters give them.
constexpr char kRules[] = "ol { list-style-position: inside } li.marked { color: rgb(0, 0, 200) }";

// A whole document of the page: an ordered list of `items` items.
void page(TextBuffer* out, uint32_t items) {
    textAppend(out, "<!doctype html><html><head><style>");
    textAppend(out, kRules);
    textAppend(out, "</style></head><body><ol>");
    for (uint32_t i = 0; i < items; ++i)
        textAppend(out, "<li>An item of the list</li>");
    textAppend(out, "</ol></body></html>");
}

// The state that the cases work on.
struct Work {
    PrCssPresentation* presentation;
    PrDomDocument* document;
    PrDomNode* list;
    PrDomElement* added;            // an item that the insert and append cases add and remove
    PrDomElement* middle;           // the item in the middle of the list
};

// Runs a frame: brings the presentation up to date in the viewport, and records its scene.
void frame(Work* w) {
    prHtmlPresentationUpdate(w->presentation, kViewport);
    const PrLayoutScene* scene = prLayoutTreeScene(prCssPresentationTree(w->presentation));
    prLayoutSceneRelease(scene);
}

typedef void (*ChangeFn)(Work* work);

struct Case {
    const char* name;
    ChangeFn change;
};

// Each case changes the list on one run, and undoes the change on the next, so that the list does not grow.
const Case kCases[] = {
    {"list unchanged", [](Work*) {}},
    {"list insert at start",
     [](Work* w) {
         if (prDomNodeParentNode(prDomElementAsNode(w->added)) == nullptr)
             prDomNodeInsertBefore(w->list, prDomElementAsNode(w->added), prDomNodeFirstChild(w->list));
         else
             prDomNodeRemove(prDomElementAsNode(w->added));
     }},
    {"list append",
     [](Work* w) {
         if (prDomNodeParentNode(prDomElementAsNode(w->added)) == nullptr)
             prDomNodeAppendChild(w->list, prDomElementAsNode(w->added));
         else
             prDomNodeRemove(prDomElementAsNode(w->added));
     }},
    {"list restyle without counters",
     [](Work* w) {
         bool on = false;
         PrDomTokenList* classes = prDomTokenListCreate(w->middle, "class");
         prDomTokenListToggle(classes, "marked", nullptr, &on);
         prDomTokenListDestroy(classes);
     }},
};

// Creates the set of the checkout's Tinos font, a shaper, and the default text font over the set, and returns the font.
PrTextFont* makeFont(PrFontSet** fonts, PrTextShaper** shaper) {
    *fonts = prFontSetCreate(nullptr);
    TextBuffer path = {};
    textAppend(&path, PR_FONTS_DIR "/Tinos.ttf");
    const PrFont* font = nullptr;
    if (prFontCreateFromFile(textView(&path), 0, nullptr, &font) == PrStatus_Ok) {
        PrFontFaceDescriptor descriptor = prFontFaceDescriptorDefault();
        prFontSetAdd(*fonts, &descriptor, font);
        prFontRelease(font);
        prFontSetSetGeneric(*fonts, prFontGenericFromName("serif"), "Tinos");
    }
    textReset(&path);
    prFontSetSetDefaultFamily(*fonts, prFontFamilyGeneric(PrFontGeneric_Serif));
    *shaper = prTextHarfBuzzCreate();
    PrTextFontDesc defaultFontDesc{.set = *fonts, .style = prFontStyleDefault()};
    return prTextFontCreate(&defaultFontDesc);
}

// Loads the page in pieces, with a frame between pieces as a host's event loop runs them, and returns what the whole
// load cost.
HarnessCounters load(PrTextFont* font, PrTextShaper* shaper, PrStringView document) {
    PrHtmlHost host = prHtmlHostDefault();
    host.font = font;
    host.shaper = shaper;
    PrHtmlPage* page = prHtmlPageCreate(&host);
    HarnessCounters started = harnessCounters();
    prHtmlPageBeginLoad(page, "about:blank", nullptr);
    PrCssPresentation* presentation = prHtmlPagePresentation(page);
    for (size_t at = 0; at < document.size;) {
        size_t size = document.size - at < kChunk ? document.size - at : kChunk;
        prHtmlPageAppendLoad(page, PrStringView{document.data + at, size});
        at += size;
        if (at == document.size)
            prHtmlPageEndLoad(page);
        PrDomAgent* agent = prHtmlPageAgent(page);
        while (prDomAgentNextTaskDelay(agent) == 0 && prDomAgentRunTask(agent)) {
        }
        if (!prHtmlPageIsRenderBlocked(page)) {
            prHtmlPresentationUpdate(presentation, kViewport);
            const PrLayoutScene* scene = prLayoutTreeScene(prCssPresentationTree(presentation));
            prLayoutSceneRelease(scene);
        }
    }
    HarnessCounters spent = harnessSpent(started, harnessCounters());
    prHtmlPageDestroy(page);
    return spent;
}

} // namespace

int main(int argc, char** argv) {
    prCrashHandler();
    uint32_t itemCount = kDefaultItems;
    bool verify = false;
    for (;;) {
        if (argc >= 3 && strcmp(argv[1], "--items") == 0) {
            itemCount = uint32_t(atoi(argv[2]));
            argc -= 2;
            argv += 2;
        } else if (argc >= 2 && strcmp(argv[1], "--verify") == 0) {
            verify = true;
            argc -= 1;
            argv += 1;
        } else {
            break;
        }
    }
    if (argc > 2 || itemCount < 2) {
        fprintf(stderr, "usage: list_bench [--items N] [--verify] [case], with N at least 2\n");
        return 2;
    }
    PrFontSet* fonts = nullptr;
    PrTextShaper* shaper = nullptr;
    PrTextFont* font = makeFont(&fonts, &shaper);
    TextBuffer document = {};
    page(&document, itemCount);

    PrHtmlHost host = prHtmlHostDefault();
    host.font = font;
    host.shaper = shaper;
    PrHtmlPage* page = prHtmlPageCreate(&host);
    prHtmlPageLoad(page, textView(&document), "about:blank", nullptr);
    Work work = {};
    work.presentation = prHtmlPagePresentation(page);
    work.document = prHtmlWindowDocument(prHtmlPageWindow(page));
    PrDomHtmlCollection* lists = prDomHtmlCollectionCreateByTagName(prDomDocumentAsNode(work.document), "ol");
    work.list = prDomElementAsNode(prDomHtmlCollectionElementAt(lists, 0));
    prDomHtmlCollectionDestroy(lists);
    PrDomHtmlCollection* items = prDomHtmlCollectionCreateByTagName(prDomDocumentAsNode(work.document), "li");
    work.middle = prDomHtmlCollectionElementAt(items, itemCount / 2);
    prDomHtmlCollectionDestroy(items);
    prDomElementCreate(work.document, "li", nullptr, &work.added);
    prDomNodeSetTextContent(prDomElementAsNode(work.added), "An added item");
    frame(&work);

    PrStringView only = argc == 2 ? argv[1] : PrStringView{};
    bool first = true;
    bool failed = false;
    printf("[");
    for (const Case& c : kCases) {
        if (only.data != nullptr) {
            if (only != c.name)
                continue;
            harnessRepeat([&] {
                c.change(&work);
                frame(&work);
            });
        }
        HarnessCounters cost = harnessMeasure([&] {
            c.change(&work);
            frame(&work);
        });
        PrString report{};
        if (verify && !prCssPresentationVerify(work.presentation, &report)) {
            PrStringView what = prStringView(&report);
            fprintf(stderr, "list_bench: %s: presentation verify: %.*s\n", c.name, int(what.size), what.data);
            failed = true;
        }
        prStringReset(&report);
        harnessWriteCase(first, c.name, cost);
        first = false;
    }
    if (only.data == nullptr) {
        // A load is measured whole, after a first load that fills the caches.
        load(font, shaper, textView(&document));
        harnessWriteCase(false, "list load in pieces", load(font, shaper, textView(&document)));
    }
    printf("\n]\n");

    prDomNodeRelease(prDomElementAsNode(work.added));
    prHtmlPageDestroy(page);
    prTextFontRelease(font);
    prTextShaperDestroy(shaper);
    prFontSetDestroy(fonts);
    textReset(&document);
    return failed ? 1 : 0;
}
