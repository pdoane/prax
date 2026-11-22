// The cost of the documents of suites/prax-limits, whose content multiplies or runs long: what the engine bounds, and
// what must cost in proportion to what a document holds.
//
// ./dev.sh bench limits runs this over every document of the corpus and prints its rows, a row for each document. An
// SVG document is parsed and drawn, a Lottie animation read and its first frame drawn, and an HTML document loaded,
// paginated and its pages painted. What each drew, in commands and pages, goes to standard error, where a blow-up of a
// bound shows beside its time.
//
// Usage: limits_bench <path>... [--only <path>]
//
// With --only, that document alone is drawn, repeatedly, for five minutes (harnessRepeat). This is for attaching a
// profiler.

#include "pr/blob_ref.h"
#include "pr/dom.h"
#include "pr/dom_xml.h"
#include "pr/draw_list.h"
#include "pr/file.h"
#include "pr/font.h"
#include "pr/font_set.h"
#include "pr/html.h"
#include "pr/html_page.h"
#include "pr/html_view.h"
#include "pr/layout_page.h"
#include "pr/lottie.h"
#include "pr/print.h"
#include "pr/string_ops.h"
#include "pr/svg.h"
#include "pr/svg_render.h"
#include "pr/text.h"
#include "pr/text_buffer.h"
#include "pr/text_harfbuzz.h"

#include "tools/harness/harness.h"

using namespace harness;
using namespace pr;

namespace {

// The extent that an SVG document and a Lottie animation are drawn at.
constexpr PrVec2 kViewport{100, 100};

// What drawing a document made.
struct Drawn {
    uint32_t commands = 0;
    uint32_t pages = 0;
};

// The text that an HTML document is laid out with: the set of the checkout's Tinos, a shaper, and the default font.
struct Text {
    PrFontSet* fonts = prFontSetCreate(nullptr);
    PrTextShaper* shaper = prTextHarfBuzzCreate();
    PrTextFont* font = nullptr;

    Text() {
        const PrFont* tinos = nullptr;
        BlobRef opened = BlobRef::adopt(prFileOpen(PR_FONTS_DIR "/Tinos.ttf"));
        if (opened && prFontCreate(opened, 0, nullptr, &tinos) == PrStatus_Ok) {
            PrFontFaceDescriptor descriptor = prFontFaceDescriptorDefault();
            prFontSetAdd(fonts, &descriptor, tinos);
            prFontRelease(tinos);
            prFontSetSetGeneric(fonts, prFontGenericFromName("serif"), "Tinos");
        }
        prFontSetSetDefaultFamily(fonts, prFontFamilyGeneric(PrFontGeneric_Serif));
        PrTextFontDesc desc{.set = fonts, .style = prFontStyleDefault()};
        font = prTextFontCreate(&desc);
    }

    ~Text() {
        prTextFontRelease(font);
        prTextShaperDestroy(shaper);
        prFontSetDestroy(fonts);
    }
};

Drawn drawSvg(PrStringView source) {
    Drawn drawn;
    PrDomDocument* document = prDomParseXml(source, "image/svg+xml", nullptr);
    PrSvgElement* root = prSvgElementQuery(prDomElementAsEventTarget(prDomDocumentElement(document)));
    PrDrawList* list = prDrawListCreate();
    if (root != nullptr)
        prSvgElementDraw(root, kViewport, nullptr, list);
    drawn.commands = prDrawListCommandCount(list);
    prDrawListRelease(list);
    prDomDocumentRelease(document);
    return drawn;
}

Drawn drawLottie(PrStringView source) {
    Drawn drawn;
    PrLottieAnimation* animation = nullptr;
    if (prLottieAnimationCreate(source, nullptr, &animation) != PrStatus_Ok)
        return drawn;
    PrDrawList* list = prDrawListCreate();
    prLottieAnimationDraw(animation, 0, list);
    drawn.commands = prDrawListCommandCount(list);
    prDrawListRelease(list);
    prLottieAnimationDestroy(animation);
    return drawn;
}

Drawn printHtml(PrStringView source, const Text& text) {
    Drawn drawn;
    PrHtmlHost host = prHtmlHostDefault();
    host.font = text.font;
    host.shaper = text.shaper;
    PrHtmlPage* page = prHtmlPageCreate(&host);
    prHtmlPageLoad(page, source, "about:blank", nullptr);
    PrHtmlViewDesc viewDesc{.page = page};
    PrHtmlView* view = prHtmlViewCreate(&viewDesc);
    PrLayoutPageBox initial = prLayoutPageBoxDefault();
    prHtmlViewSetPaged(view, &initial, 0);
    drawn.pages = prHtmlViewPageCount(view);
    PrDrawList* list = prDrawListCreate();
    for (uint32_t i = 0; i < drawn.pages; ++i) {
        prDrawListClear(list);
        prHtmlViewPaintPage(view, i, list);
        drawn.commands += prDrawListCommandCount(list);
    }
    prDrawListRelease(list);
    prHtmlViewDestroy(view);
    prHtmlPageDestroy(page);
    return drawn;
}

Drawn draw(PrStringView path, PrStringView source, const Text& text) {
    if (prStringEndsWith(path, ".svg"))
        return drawSvg(source);
    if (prStringEndsWith(path, ".json"))
        return drawLottie(source);
    return printHtml(source, text);
}

} // namespace

int main(int argc, char** argv) {
    prCrashHandler();
    PrStringView only{};
    Text text;
    print("[");
    bool first = true;
    for (int i = 1; i < argc; ++i) {
        PrStringView path = argv[i];
        if (path == "--only" && i + 1 < argc) {
            only = argv[++i];
            continue;
        }
        if (only.size != 0 && path != only)
            continue;
        PrTextBuffer source{};
        if (!harnessReadFile(&source, argv[i])) {
            printError("limits_bench: cannot read {}\n", path);
            return 1;
        }
        PrStringView document = prTextBufferView(&source);
        if (only.size != 0)
            harnessRepeat([&] { draw(path, document, text); });
        Drawn drawn;
        HarnessCounters cost = harnessMeasure([&] { drawn = draw(path, document, text); });
        // The name is the end of the path, so it ends where the argument does.
        PrStringView name = harnessFileName(argv[i]);
        harnessWriteCase(first, name.data, cost);
        first = false;
        printError("limits_bench: {}: {} commands, {} pages\n", name, drawn.commands, drawn.pages);
        prTextBufferReset(&source);
    }
    harnessWriteLine("\n]");
    return 0;
}
