#include "pr/dom.h"
#include "pr/draw_commands.h"
#include "pr/draw_list.h"
#include "pr/draw_structure.h"
#include "pr/html_page.h"
#include "pr/html_structure.h"
#include "pr/html_view.h"
#include "pr/set.h"
#include "pr/string.h"
#include "pr/text_harfbuzz.h"

#include "tools/harness/test.h"

namespace pr_impl {

namespace {

// A page presented by a view, over Tinos, painted with its content marked.
struct Structured {
    PrFontSet* fonts = prFontSetCreate(nullptr);
    PrTextShaper* shaper = prTextHarfBuzzCreate();
    PrTextFont* defaultFont = nullptr;
    PrHtmlPage* page = nullptr;
    PrHtmlView* view = nullptr;
    PrDrawList* list = prDrawListCreate();
    PrDrawStructure* structure = prDrawStructureCreate(nullptr);

    explicit Structured(const char* html) {
        const PrFont* font = nullptr;
        REQUIRE(prFontCreateFromFile(PR_FONTS_DIR "/Tinos-Regular.ttf", 0, nullptr, &font) == PrStatus_Ok);
        PrFontFaceDescriptor descriptor = prFontFaceDescriptorDefault();
        prFontSetAdd(fonts, &descriptor, font);
        prFontRelease(font);
        prFontSetSetDefaultFamily(fonts, prFontFamilyFromName("Tinos"));
        PrTextFontDesc defaultFontDesc{.set = fonts, .style = prFontStyleDefault()};
        defaultFont = prTextFontCreate(&defaultFontDesc);
        PrHtmlHost host = prHtmlHostDefault();
        host.font = defaultFont;
        host.shaper = shaper;
        page = prHtmlPageCreate(&host);
        REQUIRE(prHtmlPageLoad(page, html, "https://example.org/page.html", nullptr) == PrStatus_Ok);
        PrHtmlViewDesc viewDesc{.page = page};
        view = prHtmlViewCreate(&viewDesc);
        prHtmlViewSetViewport(view, PrVec2{400, 400});
        prHtmlViewSetMarking(view, PrLayoutMarking_Content);
        prHtmlViewPaint(view, list);
        prHtmlStructureAppend(page, structure);
    }

    ~Structured() {
        prDrawStructureDestroy(structure);
        prDrawListRelease(list);
        prHtmlViewDestroy(view);
        prHtmlPageDestroy(page);
        prTextFontRelease(defaultFont);
        prTextShaperDestroy(shaper);
        prFontSetDestroy(fonts);
    }

    // Returns the structure as text, each key as the text of the node that it is, quoted.
    String dump() const { return dumpNode(kPrDrawStructureRoot, 0); }

    String dumpNode(PrDrawStructureNode node, uint32_t depth) const {
        String out = format("{:{}}", "", depth * 2);
        PrDrawStructureNodeDesc desc = prDrawStructureNodeDesc(structure, node);
        out.append(prDrawStructureTypeName(desc.type));
        auto attribute = [&](const char* label, PrStringView value) {
            if (value.size != 0)
                formatTo(out, " {}=\"{}\"", label, value);
        };
        attribute("alt", desc.alt);
        attribute("lang", desc.language);
        attribute("title", desc.title);
        attribute("uri", desc.uri);
        attribute("expansion", desc.expansion);
        if (desc.destination != 0)
            formatTo(out, " destination={}", textOf(desc.destination));
        out.append("\n");
        const PrDrawStructureItem* items = prDrawStructureItems(structure, node);
        for (uint32_t i = 0; i < prDrawStructureItemCount(structure, node); ++i) {
            if (items[i].node != kPrDrawStructureNodeNone)
                out.append(dumpNode(items[i].node, depth + 1));
            else
                formatTo(out, "{:{}}{}\n", "", (depth + 1) * 2, textOf(items[i].key));
        }
        return out;
    }

    // Returns the text of the node that `key` is, quoted, or the element's name in angle brackets.
    static String textOf(uint64_t key) {
        PrDomNode* node = reinterpret_cast<PrDomNode*>(uintptr_t(key));
        if (prDomNodeType(node) != PrDomNodeType_Text)
            return format("<{}>", prDomElementLocalName(prDomElementQuery(prDomNodeAsEventTarget(node))));
        String text = String::adopt(prDomNodeTextContent(node));
        return format("\"{}\"", text);
    }

    // Returns the keys of the marks that the paint recorded, through the lists that it embeds.
    static void markKeys(const PrDrawList* list, Set<uint64_t>& out) {
        const PrDrawCommand* commands = prDrawListCommands(list);
        for (uint32_t i = 0; i < prDrawListCommandCount(list); ++i) {
            if (commands[i].kind == PrDrawCommandKind_Embed)
                markKeys(commands[i].embed.list, out);
            else if (commands[i].kind == PrDrawCommandKind_PushMark)
                out.add(commands[i].key);
        }
    }

    // Returns the keys that the structure names.
    void structureKeys(PrDrawStructureNode node, Set<uint64_t>& out) const {
        const PrDrawStructureItem* items = prDrawStructureItems(structure, node);
        for (uint32_t i = 0; i < prDrawStructureItemCount(structure, node); ++i) {
            if (items[i].node != kPrDrawStructureNodeNone)
                structureKeys(items[i].node, out);
            else
                out.add(items[i].key);
        }
    }
};

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a document's structure follows its roles in reading order, and leaves out what does not render") {
    Structured s("<!DOCTYPE html><html lang=en><body><h1>Title</h1>"
        "<p>Hello <a href='https://example.com/'>link</a> <em>world</em></p><ul><li>One</li><li lang=fr>Deux</li></ul>"
        "<div aria-hidden=true>secret</div><div style='display: none'>gone</div>"
        "<div>loose <span>text</span><p>block</p>after</div><p><abbr title='HyperText Markup Language'>HTML</abbr></p>"
        "</body></html>");
    CHECK(s.dump() == "Document lang=\"en\"\n"
        "  H1 title=\"Title\"\n"
        "    \"Title\"\n"
        "  P\n"
        "    \"Hello \"\n"
        "    Link uri=\"https://example.com/\"\n"
        "      \"link\"\n"
        "    \" \"\n"
        "    Em\n"
        "      \"world\"\n"
        "  L\n"
        "    LI\n"
        "      LBody\n"
        "        \"One\"\n"
        "    LI lang=\"fr\"\n"
        "      LBody\n"
        "        \"Deux\"\n"
        "  P\n"
        "    \"loose \"\n"
        "    \"text\"\n"
        "  P\n"
        "    \"block\"\n"
        "  P\n"
        "    \"after\"\n"
        "  P\n"
        "    Span expansion=\"HyperText Markup Language\"\n"
        "      \"HTML\"\n");
    // Every key that the structure names was painted with a mark.
    Set<uint64_t> painted;
    Structured::markKeys(s.list, painted);
    Set<uint64_t> named;
    s.structureKeys(kPrDrawStructureRoot, named);
    for (uint64_t key : named)
        CHECK(painted.contains(key));
}

TEST_CASE("a link to a fragment leads to the first text of its target, and aria-owns moves what it owns") {
    Structured s("<!DOCTYPE html><body><p><a href='#end'>jump</a></p><div id=owner aria-owns=moved role=list></div>"
        "<p id=end>the <b>end</b></p><div role=listitem id=moved>moved</div><img alt='' src=x></body>");
    CHECK(s.dump() == "Document\n"
        "  P\n"
        "    Link destination=\"the \"\n"
        "      \"jump\"\n"
        "  L\n"
        "    LI\n"
        "      LBody\n"
        "        \"moved\"\n"
        "  P\n"
        "    \"the \"\n"
        "    \"end\"\n");
}
