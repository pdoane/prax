// The cost of a frame after each kind of change, and the cost of loading a page in pieces, to its first frame and to
// its end.
//
// ./dev.sh bench frame runs this and prints its rows. A frame is the work between a change to a document and the scene
// that a compositor plays: the styles that the change reaches, the layout, and the recordings, on the caller's thread.
// Each row gives the cost of a frame and what that frame did. A change should cost in proportion to what it reaches,
// whatever the page holds, so the runner measures a page of the default size and one many times larger. A cost that
// grows with the page is a defect. The page belongs to a browsing context that renders in the checkout's fonts, under
// the large stylesheet that the style benchmark reads, with no scripting.
//
// Usage: frame_bench [--cards N] [--verify] <large.css> [case]
//
// --cards sets how many cards the page holds; 100 by default. --verify checks the presentation after each case against
// a full recomputation (prCssPresentationVerify), and a failure fails the run. A benchmark or a profile runs without
// it, so that neither counts the check. If a case is named, only that case runs, repeatedly, for five minutes
// (harnessRepeat). This is for attaching a profiler.

#include "pr/assert.h"
#include "pr/count_of.h"
#include "pr/css.h"
#include "pr/css_presentation.h"
#include "pr/css_view.h"
#include "pr/html.h"
#include "pr/html_page.h"
#include "pr/layout_scene.h"
#include "pr/layout_style.h"
#include "pr/layout_tree.h"
#include "pr/string_ops.h"
#include "pr/text_buffer.h"
#include "pr/text_harfbuzz.h"
#include "pr/time.h"

#include "tools/harness/harness.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

using namespace harness;

using namespace pr_impl;

namespace {

// The number of cards in the page by default.
constexpr uint32_t kDefaultCards = 100;

// The viewport that the page is laid out in, and the narrower viewport that the resize case alternates with it.
constexpr PrLayoutVec kViewport{{1024 * kPrLayoutUnitsPerPx}, {768 * kPrLayoutUnitsPerPx}};
constexpr PrLayoutVec kNarrowViewport{{1000 * kPrLayoutUnitsPerPx}, {768 * kPrLayoutUnitsPerPx}};

// The size in bytes of each piece that a chunked load delivers.
constexpr uint32_t kChunk = 4096;

// The page's own rules, beside the large stylesheet.
constexpr char kRules[] =
    ".card { display: block; margin: 4px; padding: 4px 8px; border: 1px solid rgb(200, 200, 200); max-width: 600px }"
    ".card h2 { font-size: 15px; margin: 0 0 4px 0 }.card p { margin: 2px 0; line-height: 1.4 }"
    ".card span { color: rgb(0, 0, 200); padding: 0 2px }.card.active { background-color: rgb(255, 255, 200) }"
    ".card.tall p { font-size: 20px }.card.faded { opacity: 0.5 }.card.raised { position: relative; margin-top: -8px }"
    ".scroller { height: 120px; overflow: auto }";

// The markup of the page's content: `cards` cards, the last few in a scroll container.
void markup(PrTextBuffer* out, uint32_t cards) {
    for (uint32_t i = 0; i < cards; ++i) {
        if (i == cards - 10)
            prTextBufferAppend(out, "<div class=\"scroller\">");
        prTextBufferAppend(out, "<div class=\"card\"><h2>Card ");
        prTextBufferAppendU64(out, i, 10);
        prTextBufferAppend(out, "</h2><p>Some text with <span>an inline span</span> in a paragraph of a card, long "
            "enough to break across a line or two at the width a card has.</p></div>");
    }
    prTextBufferAppend(out, "</div>");
}

// A whole document of the page: its sheets in its head, then its content.
void page(PrTextBuffer* out, PrStringView large, uint32_t cards) {
    prTextBufferAppend(out, "<!doctype html><html><head><style>");
    prTextBufferAppend(out, large);
    prTextBufferAppend(out, "</style><style>");
    prTextBufferAppend(out, kRules);
    prTextBufferAppend(out, "</style></head><body>");
    markup(out, cards);
    prTextBufferAppend(out, "</body></html>");
}

// The state that the cases work on.
struct Work {
    PrHtmlPage* page;
    PrCssPresentation* presentation;
    PrDomDocument* document;
    std::vector<PrDomElement*> cards;
    PrDomElement* middle;           // the card in the middle of the page
    PrDomElement* faded;            // a card with a layer of its own, for its opacity
    PrDomElement* raised;           // a relative card over the one before it, for its z-index
    PrDomElement* middleText;
    PrDomElement* scroller;
    PrDomElement* added;
    PrLayoutVec viewport;
    bool flip;
    bool narrowed;                  // true while the viewport is narrow; only the resize case sets it
    PrCssPresentationStats styled;  // the statistics of the last frame's update
    PrLayoutTreeStats laid;
};

// Runs a frame: brings the presentation up to date in the viewport, and records its scene.
void frame(Work* w) {
    bool changed = prHtmlPresentationUpdate(w->presentation, w->viewport);
    PrLayoutTree* tree = prCssPresentationTree(w->presentation);
    w->styled = prCssPresentationStats(w->presentation);
    const PrLayoutScene* scene = prLayoutTreeScene(tree);
    w->laid = changed ? prLayoutTreeStats(tree) : PrLayoutTreeStats{};
    prLayoutSceneRelease(scene);
}

void toggle(PrDomElement* element, PrStringView token) {
    bool on = false;
    PrDomTokenList* classes = prDomTokenListCreate(element, "class");
    prDomTokenListToggle(classes, token, nullptr, &on);
    prDomTokenListDestroy(classes);
}

PrDomElement* firstNamed(PrDomElement* parent, PrStringView name) {
    PrDomHtmlCollection* found = prDomHtmlCollectionCreateByTagName(prDomElementAsNode(parent), name);
    PrDomElement* first = prDomHtmlCollectionElementAt(found, 0);
    prDomHtmlCollectionDestroy(found);
    return first;
}

typedef void (*BodyFn)(Work* work);

struct Case {
    const char* name;
    BodyFn change;                  // the change made before the frame
};

constexpr Case kCases[] = {
    {"frame unchanged", [](Work*) {}},
    {"frame class on leaf", [](Work* w) { toggle(w->middle, "active"); }},
    {
        "frame background",
        [](Work* w) {
            w->flip = !w->flip;
            prDomElementSetAttribute(w->middle, "style", w->flip ? "background-color: rgb(200, 230, 255)" : "");
        },
    },
    {"frame font size", [](Work* w) { toggle(w->middle, "tall"); }},
    {
        "frame width",
        [](Work* w) {
            w->flip = !w->flip;
            prDomElementSetAttribute(w->middle, "style", w->flip ? "width: 300px" : "width: 320px");
        },
    },
    {
        "frame text",
        [](Work* w) {
            w->flip = !w->flip;
            prDomNodeSetTextContent(prDomElementAsNode(w->middleText),
                w->flip ? "Other text, of about the same length as the paragraph had." :
                    "Some text of about the same length as the paragraph had, too.");
        },
    },
    {
        "frame opacity on layer",
        [](Work* w) {
            w->flip = !w->flip;
            prDomElementSetAttribute(w->faded, "style", w->flip ? "opacity: 0.6" : "");
        },
    },
    {
        "frame transform",
        [](Work* w) {
            w->flip = !w->flip;
            prDomElementSetAttribute(w->middle, "style", w->flip ? "transform: translate(4px, 0)" : "transform: none");
        },
    },
    {
        "frame z-index",
        [](Work* w) {
            w->flip = !w->flip;
            prDomElementSetAttribute(w->raised, "style", w->flip ? "z-index: 2" : "z-index: 1");
        },
    },
    {
        "frame z-index auto",
        [](Work* w) {
            w->flip = !w->flip;
            prDomElementSetAttribute(w->raised, "style", w->flip ? "z-index: 1" : "");
        },
    },
    {
        "frame scroll",
        [](Work* w) {
            w->flip = !w->flip;
            prCssElementSetScrollTop(w->scroller, w->flip ? 40 : 0);
        },
    },
    {
        "frame append",
        [](Work* w) {
            PrDomNode* body = prDomElementAsNode(prDomDocumentBody(w->document));
            if (prDomNodeParentNode(prDomElementAsNode(w->added)) == nullptr)
                prDomNodeAppendChild(body, prDomElementAsNode(w->added));
            else
                prDomNodeRemove(prDomElementAsNode(w->added));
        },
    },
    // This case runs last, because it may leave the narrower viewport in place for the code that follows. Every box is
    // laid out again in another width.
    {
        "frame resize",
        [](Work* w) {
            w->narrowed = !w->narrowed;
            w->viewport = w->narrowed ? kNarrowViewport : kViewport;
        },
    },
};

// Creates the set of the checkout's Tinos and Arimo fonts, a shaper, and the default text font over the set, and
// returns the font.
PrTextFont* makeFont(PrFontSet** fonts, PrTextShaper** shaper) {
    *fonts = prFontSetCreate(nullptr);
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
            prFontSetAdd(*fonts, &descriptor, font);
            prFontRelease(font);
            if (kFonts[i][1] != nullptr)
                prFontSetSetGeneric(*fonts, prFontGenericFromName(kFonts[i][1]), kFamilies[i]);
        }
        prTextBufferReset(&path);
    }
    prFontSetSetDefaultFamily(*fonts, prFontFamilyGeneric(PrFontGeneric_Serif));
    *shaper = prTextHarfBuzzCreate();
    PrTextFontDesc defaultFontDesc{.set = *fonts, .style = prFontStyleDefault()};
    return prTextFontCreate(&defaultFontDesc);
}

// Loads the page in pieces, with a frame between pieces as a host's event loop runs them, until the page first renders
// and until its load ends. Writes the milliseconds that each took.
void load(PrTextFont* font, PrTextShaper* shaper, PrStringView document, HarnessCounters* firstFrame, HarnessCounters* end) {
    PrHtmlHost host = prHtmlHostDefault();
    host.font = font;
    host.shaper = shaper;
    PrHtmlPage* page = prHtmlPageCreate(&host);
    HarnessCounters started = harnessCounters();
    bool framed = false;
    prHtmlPageBeginLoad(page, "about:blank", nullptr);
    PrCssPresentation* presentation = prHtmlPagePresentation(page);
    for (size_t at = 0;;) {
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
            if (!framed)
                *firstFrame = harnessSpent(started, harnessCounters());
            framed = true;
        }
        if (at == document.size)
            break;
    }
    *end = harnessSpent(started, harnessCounters());
    prHtmlPageDestroy(page);
}

} // namespace

int main(int argc, char** argv) {
    prCrashHandler();
    uint32_t cardCount = kDefaultCards;
    bool verify = false;
    for (;;) {
        if (argc >= 3 && strcmp(argv[1], "--cards") == 0) {
            cardCount = uint32_t(atoi(argv[2]));
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
    if ((argc != 2 && argc != 3) || cardCount < 20) {
        fprintf(stderr, "usage: frame_bench [--cards N] [--verify] <large.css> [case], with N at least 20\n");
        return 2;
    }
    PrTextBuffer large = {};
    if (!harnessReadFile(&large, argv[1])) {
        fprintf(stderr, "frame_bench: cannot read the stylesheet\n");
        return 1;
    }
    PrFontSet* fonts = nullptr;
    PrTextShaper* shaper = nullptr;
    PrTextFont* font = makeFont(&fonts, &shaper);
    PrTextBuffer document = {};
    page(&document, prTextBufferView(&large), cardCount);

    PrHtmlHost host = prHtmlHostDefault();
    host.font = font;
    host.shaper = shaper;
    Work work = {};
    work.viewport = kViewport;
    work.page = prHtmlPageCreate(&host);
    prHtmlPageLoad(work.page, prTextBufferView(&document), "about:blank", nullptr);
    work.presentation = prHtmlPagePresentation(work.page);
    work.document = prHtmlWindowDocument(prHtmlPageWindow(work.page));
    PrDomHtmlCollection* cards = prDomHtmlCollectionCreateByClassName(prDomDocumentAsNode(work.document), "card");
    for (uint32_t i = 0; i < cardCount; ++i)
        work.cards.push_back(prDomHtmlCollectionElementAt(cards, i));
    prDomHtmlCollectionDestroy(cards);
    work.middle = work.cards[cardCount >> 1];
    work.faded = work.cards[(cardCount >> 1) + 2];
    toggle(work.faded, "faded");
    work.raised = work.cards[(cardCount >> 1) + 4];
    toggle(work.raised, "raised");
    prDomElementSetAttribute(work.raised, "style", "z-index: 1");
    work.middleText = firstNamed(work.middle, "p");
    PrDomHtmlCollection* scrollers = prDomHtmlCollectionCreateByClassName(prDomDocumentAsNode(work.document), "scroller");
    work.scroller = prDomHtmlCollectionElementAt(scrollers, 0);
    prDomHtmlCollectionDestroy(scrollers);
    prDomElementCreate(work.document, "div", nullptr, &work.added);
    prDomElementSetAttribute(work.added, "class", "card");
    prDomNodeSetTextContent(prDomElementAsNode(work.added), "An added card, at the end of the page.");
    frame(&work);

    PrStringView only = argc == 3 ? argv[2] : PrStringView{};
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
            fprintf(stderr, "frame_bench: %s: presentation verify: %.*s\n", c.name, int(what.size), what.data);
            failed = true;
        }
        prStringReset(&report);
        PrTextBuffer row{};
        harnessAppendFormat(&row, "%s\n  {\"name\": \"%s\", \"time\": %.6f, \"visited\": %u, \"styled\": %u, "
            "\"cached\": %u, \"unchanged\": %u, \"blocks\": %u, \"lines\": %u, \"shaped\": %u, "
            "\"layers\": %u, \"commands\": %u, \"repainted\": %u",
            first ? "" : ",", c.name, cost.millis, work.styled.elementsVisited,
            work.styled.elementsStyled, work.styled.elementsStyledCached,
            work.styled.elementsStyledUnchanged, work.laid.blocksLaidOut,
            work.laid.linesLaidOut, work.laid.inlinesShaped, work.laid.layersRecorded,
            work.laid.commandsRecorded, work.laid.boxesRepaintedUnchanged);
        HarnessCounts counts{};
        harnessCountsAdd(&counts, "time", cost);
        harnessCountsWrite(&counts, &row);
        prTextBufferAppend(&row, "}");
        fwrite(row.data, 1, row.size, stdout);
        prTextBufferReset(&row);
        first = false;
    }
    if (only.data == nullptr) {
        // A load is measured whole, after a first load that fills the caches, so that the measured load finds them
        // full.
        HarnessCounters firstFrame{};
        HarnessCounters end{};
        load(font, shaper, prTextBufferView(&document), &firstFrame, &end);
        load(font, shaper, prTextBufferView(&document), &firstFrame, &end);
        harnessWriteCase(false, "load to first frame", firstFrame);
        harnessWriteCase(false, "load to end", end);
    }
    printf("\n]\n");

    prDomNodeRelease(prDomElementAsNode(work.added));
    prHtmlPageDestroy(work.page);
    prTextFontRelease(font);
    prTextShaperDestroy(shaper);
    prFontSetDestroy(fonts);
    prTextBufferReset(&document);
    prTextBufferReset(&large);
    return failed ? 1 : 0;
}
