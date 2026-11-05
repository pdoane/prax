// The cost of printing a long document: its pagination, the paint of its pages, and the documents that lib/draw's
// writers make of them.
//
// ./dev.sh bench print runs this and prints its rows. The document is generated: sections of prose under headings, with
// links, emphasis, lists and tables, which the paint marks and the structure tags, so that a writer's text, structure
// and compression each take part. The PDF is written with a codec and without one, which separates its compression from
// the rest of its writing.
//
// Usage: print_bench [--sections N] [case]
//
// --sections sets how many sections the document holds; 40 by default, about 60 pages. If a case is named, only that
// case runs, repeatedly, for five minutes (harnessRepeat). This is for attaching a profiler.

#include "pr/assert.h"
#include "pr/compress.h"
#include "pr/count_of.h"
#include "pr/draw_list.h"
#include "pr/draw_structure.h"
#include "pr/draw_writer.h"
#include "pr/html.h"
#include "pr/html_page.h"
#include "pr/html_structure.h"
#include "pr/html_view.h"
#include "pr/layout_page.h"
#include "pr/string_ops.h"
#include "pr/text_buffer.h"
#include "pr/text_harfbuzz.h"

#include "tools/harness/harness.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

using namespace harness;

using namespace pr_impl;

namespace {

// The number of sections in the document by default.
constexpr uint32_t kDefaultSections = 40;

// The document's rules: an A4 page with a 2cm margin, and 11pt serif prose.
constexpr char kRules[] = "@page { size: A4; margin: 2cm } body { font: 11pt/1.4 serif; margin: 0 } "
    "h2 { font-size: 14pt } table { border-collapse: collapse } td, th { border: 1px solid; padding: 2px 4px }";

// The words that the prose is made of.
constexpr const char* kWords[] = {
    "the", "of", "and", "a", "to", "in", "is", "that", "for", "it", "as", "was", "with", "be", "by", "on", "not",
    "this", "are", "or", "from", "at", "which", "but", "have", "an", "they", "you", "were", "there", "been", "one",
    "all", "we", "their", "has", "would", "when", "if", "so", "no", "what", "up", "out", "more", "said", "them", "some",
    "could", "into", "other", "then", "two", "time", "these", "may", "like", "only", "over", "new", "very", "about",
    "than", "also", "any", "first", "after", "back", "most", "our", "where", "document", "printed", "paragraph",
};

// A generator of the same words on every run.
struct Words {
    uint32_t state = 12345;

    const char* next() {
        state = state * 1103515245u + 12345u;
        return kWords[(state >> 16) % countOf(kWords)];
    }
};

// Appends a paragraph of `count` words, a few of them in links and emphasis.
void paragraph(PrTextBuffer* out, Words& words, uint32_t count) {
    prTextBufferAppend(out, "<p>");
    for (uint32_t i = 0; i < count; ++i) {
        if (i != 0)
            prTextBufferAppend(out, " ");
        const char* word = words.next();
        if (i % 37 == 11) {
            prTextBufferAppend(out, "<a href=\"#s1\">");
            prTextBufferAppend(out, word);
            prTextBufferAppend(out, "</a>");
        } else if (i % 29 == 7) {
            prTextBufferAppend(out, "<em>");
            prTextBufferAppend(out, word);
            prTextBufferAppend(out, "</em>");
        } else {
            prTextBufferAppend(out, word);
        }
    }
    prTextBufferAppend(out, ".</p>");
}

// A whole document of `sections` sections.
void document(PrTextBuffer* out, uint32_t sections) {
    Words words;
    char number[16];
    prTextBufferAppend(out, "<!doctype html><html lang=en><head><title>Printed</title><style>");
    prTextBufferAppend(out, kRules);
    prTextBufferAppend(out, "</style></head><body>");
    for (uint32_t s = 0; s < sections; ++s) {
        snprintf(number, sizeof(number), "%u", s + 1);
        prTextBufferAppend(out, "<h2 id=\"s");
        prTextBufferAppend(out, number);
        prTextBufferAppend(out, "\">Section ");
        prTextBufferAppend(out, number);
        prTextBufferAppend(out, "</h2>");
        for (uint32_t p = 0; p < 8; ++p)
            paragraph(out, words, 60 + (p * 23) % 80);
        prTextBufferAppend(out, "<ul>");
        for (uint32_t i = 0; i < 4; ++i) {
            prTextBufferAppend(out, "<li>");
            for (uint32_t w = 0; w < 8; ++w) {
                prTextBufferAppend(out, words.next());
                prTextBufferAppend(out, " ");
            }
            prTextBufferAppend(out, "</li>");
        }
        prTextBufferAppend(out, "</ul><table><tr><th>Name</th><th>Value</th><th>Notes</th></tr>");
        for (uint32_t r = 0; r < 5; ++r) {
            prTextBufferAppend(out, "<tr>");
            for (uint32_t c = 0; c < 3; ++c) {
                prTextBufferAppend(out, "<td>");
                prTextBufferAppend(out, words.next());
                prTextBufferAppend(out, " ");
                prTextBufferAppend(out, words.next());
                prTextBufferAppend(out, "</td>");
            }
            prTextBufferAppend(out, "</tr>");
        }
        prTextBufferAppend(out, "</table>");
    }
    prTextBufferAppend(out, "</body></html>");
}

// Creates the set of the checkout's Tinos font, a shaper, and the default text font over the set, and returns the font.
PrTextFont* makeFont(PrFontSet** fonts, PrTextShaper** shaper) {
    *fonts = prFontSetCreate(nullptr);
    PrTextBuffer path = {};
    prTextBufferAppend(&path, PR_FONTS_DIR "/Tinos.ttf");
    const PrFont* font = nullptr;
    if (prFontCreateFromFile(prTextBufferView(&path), 0, nullptr, &font) == PrStatus_Ok) {
        PrFontFaceDescriptor descriptor = prFontFaceDescriptorDefault();
        prFontSetAdd(*fonts, &descriptor, font);
        prFontRelease(font);
        prFontSetSetGeneric(*fonts, prFontGenericFromName("serif"), "Tinos");
    }
    prTextBufferReset(&path);
    prFontSetSetDefaultFamily(*fonts, prFontFamilyGeneric(PrFontGeneric_Serif));
    *shaper = prTextHarfBuzzCreate();
    PrTextFontDesc defaultFontDesc{.set = *fonts, .style = prFontStyleDefault()};
    return prTextFontCreate(&defaultFontDesc);
}

// The printed document: its pages' lists and extents, and its structure.
struct Printed {
    PrHtmlPage* page;
    PrHtmlView* view;
    PrDrawStructure* structure;
    std::vector<PrDrawList*> lists;
    std::vector<PrDrawPage> pages;
};

// Releases the lists of the printed pages.
void release(Printed* printed) {
    for (PrDrawList* list : printed->lists)
        prDrawListRelease(list);
    printed->lists.clear();
    printed->pages.clear();
}

// Paints every page of the paged view into `printed`, marking its content, and appends the document's structure.
void paint(Printed* printed) {
    release(printed);
    prDrawStructureDestroy(printed->structure);
    printed->structure = prDrawStructureCreate(nullptr);
    uint32_t count = prHtmlViewPageCount(printed->view);
    prHtmlViewSetMarking(printed->view, PrLayoutMarking_Content);
    for (uint32_t i = 0; i < count; ++i) {
        PrDrawList* list = prDrawListCreate();
        prHtmlViewPaintPage(printed->view, i, list);
        printed->lists.push_back(list);
        PrLayoutPage page = prHtmlViewPage(printed->view, i);
        PrLayoutPageSheet sheet = prLayoutPageBoxSheet(&page.box);
        printed->pages.push_back(PrDrawPage{
            .list = list,
            .extent = prLayoutVecToPx(sheet.extent),
            .bleed = prLayoutRectToPx(sheet.bleed),
            .trim = prLayoutRectToPx(sheet.trim),
        });
    }
    prHtmlViewSetMarking(printed->view, PrLayoutMarking_None);
    prHtmlStructureAppend(printed->page, printed->structure);
}

// Writes the printed document with the writer `backend`, compressed by `codec` if it is not null, and returns the
// number of bytes that it wrote.
uint64_t write(const Printed* printed, PrDrawWriterBackend backend, PrCompressCodec* PR_NULLABLE codec) {
    PrDrawWriterOptions options = prDrawWriterOptionsDefault();
    options.compress = codec;
    PrDrawWriter* writer = nullptr;
    if (prDrawWriterCreate(backend, &options, &writer) != PrStatus_Ok)
        return 0;
    uint64_t written = 0;
    PrDrawDocumentDesc desc{
        .write =
            [](void* PR_NULLABLE context, uint32_t, PrByteView data) { *static_cast<uint64_t*>(context) += data.size; },
        .context = &written,
        .structure = printed->structure,
        .title = "Printed",
        .language = "en",
    };
    prDrawWriterWrite(writer, &desc, printed->pages.data(), uint32_t(printed->pages.size()));
    prDrawWriterDestroy(writer);
    return written;
}

} // namespace

int main(int argc, char** argv) {
    prCrashHandler();
    uint32_t sections = kDefaultSections;
    if (argc >= 3 && strcmp(argv[1], "--sections") == 0) {
        sections = uint32_t(atoi(argv[2]));
        argc -= 2;
        argv += 2;
    }
    if (argc > 2 || sections == 0) {
        fprintf(stderr, "usage: print_bench [--sections N] [case], with N at least 1\n");
        return 2;
    }
    PrFontSet* fonts = nullptr;
    PrTextShaper* shaper = nullptr;
    PrTextFont* font = makeFont(&fonts, &shaper);
    PrTextBuffer source = {};
    document(&source, sections);
    PrHtmlHost host = prHtmlHostDefault();
    host.font = font;
    host.shaper = shaper;
    Printed printed{.structure = nullptr};
    printed.page = prHtmlPageCreate(&host);
    prHtmlPageLoad(printed.page, prTextBufferView(&source), "about:blank", nullptr);
    PrHtmlViewDesc viewDesc{.page = printed.page};
    printed.view = prHtmlViewCreate(&viewDesc);
    PrLayoutPageBox initial = prLayoutPageBoxDefault();
    prHtmlViewSetPaged(printed.view, &initial, 0);
    paint(&printed);
    PrCompressCodec* codec = prCompressCodecCreate(PrCompressCodecBackend_Default);

    struct Case {
        const char* name;
        void (*run)(Printed* printed, PrCompressCodec* codec);
    };
    static const Case kCases[] = {
        {
            "print paginate",
            [](Printed* p, PrCompressCodec*) {
                PrLayoutPageBox box = prLayoutPageBoxDefault();
                prHtmlViewSetPaged(p->view, nullptr, 0);
                prHtmlViewSetPaged(p->view, &box, 0);
                prHtmlViewPageCount(p->view);
            },
        },
        {"print paint", [](Printed* p, PrCompressCodec*) { paint(p); }},
        {"pdf write", [](Printed* p, PrCompressCodec* c) { write(p, PrDrawWriterBackend_Pdf, c); }},
        {"pdf write uncompressed", [](Printed* p, PrCompressCodec*) { write(p, PrDrawWriterBackend_Pdf, nullptr); }},
        {"svg write", [](Printed* p, PrCompressCodec*) { write(p, PrDrawWriterBackend_Svg, nullptr); }},
    };

    PrStringView only = argc == 2 ? argv[1] : PrStringView{};
    bool first = true;
    printf("[");
    for (const Case& c : kCases) {
        if (only.data != nullptr) {
            if (only != c.name)
                continue;
            harnessRepeat([&] { c.run(&printed, codec); });
        }
        HarnessCounters cost = harnessMeasure([&] { c.run(&printed, codec); });
        harnessWriteCase(first, c.name, cost);
        first = false;
    }
    printf("\n]\n");
    fprintf(stderr, "print_bench: %zu pages, %llu bytes of PDF\n", printed.pages.size(),
        static_cast<unsigned long long>(write(&printed, PrDrawWriterBackend_Pdf, codec)));

    release(&printed);
    prDrawStructureDestroy(printed.structure);
    prCompressCodecDestroy(codec);
    prHtmlViewDestroy(printed.view);
    prHtmlPageDestroy(printed.page);
    prTextFontRelease(font);
    prTextShaperDestroy(shaper);
    prFontSetDestroy(fonts);
    prTextBufferReset(&source);
    return 0;
}
