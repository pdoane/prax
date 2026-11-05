#include "pr/compress.h"
#include "pr/draw_filter.h"
#include "pr/draw_svg.h"
#include "pr/font.h"
#include "pr/image.h"

#include "tools/harness/test.h"

#include <string>
#include <vector>

extern "C" int prTestDrawSvgCApi(void);

namespace pr_impl {

namespace {

// Appends the bytes to the string of their output, by page.
void collect(void* PR_NULLABLE context, uint32_t output, PrByteView bytes) {
    std::vector<std::string>& outputs = *static_cast<std::vector<std::string>*>(context);
    if (output >= outputs.size())
        outputs.resize(output + 1);
    outputs[output].append(reinterpret_cast<const char*>(bytes.data), bytes.size);
}

// Writes the pages as SVG, with a PNG encoder for images, and returns each page's document.
std::vector<std::string> writePages(const PrDrawPage* pages, uint32_t count, PrStringView title = {}) {
    PrCompressCodec* compress = prCompressCodecCreate(PrCompressCodecBackend_Default);
    PrImageCodec* images = prImageCodecCreate(PrImageCodecBackend_Default, compress);
    PrDrawWriterOptions options = prDrawWriterOptionsDefault();
    options.images = images;
    PrDrawWriter* writer = prDrawSvgCreate(&options);
    std::vector<std::string> outputs;
    PrDrawDocumentDesc desc{.write = collect, .context = &outputs, .title = title};
    prDrawWriterWrite(writer, &desc, pages, count);
    prDrawWriterDestroy(writer);
    prImageCodecDestroy(images);
    prCompressCodecDestroy(compress);
    return outputs;
}

// Writes the list as a page of `extent` and returns its document.
std::string write(const PrDrawList* list, PrVec2 extent) {
    PrDrawPage page{.list = list, .extent = extent};
    std::vector<std::string> outputs = writePages(&page, 1);
    REQUIRE(outputs.size() == 1);
    return outputs[0];
}

bool contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("the SVG writer's header compiles as C") {
    CHECK(prTestDrawSvgCApi() == 0);
}

TEST_CASE("shapes and strokes are SVG paths with their paint") {
    PrDrawList* list = prDrawListCreate();
    prDrawListRect(list, PrRect{10, 10, 30, 20}, {.color = PrColor{1, 0, 0, 1}});
    PrDrawStroke stroke = prDrawStrokeDefault();
    stroke.shape.width = 2;
    stroke.shape.join = PrPathJoin_Round;
    prDrawListPushTransformAffine2(list, prAffine2Translate(PrVec2{5, 0}));
    prDrawListLine(list, PrVec2{0, 0}, PrVec2{10, 10}, {.color = PrColor{0, 0, 1, 0.5f}, .stroke = &stroke});
    prDrawListPopTransform(list);
    CHECK(write(list, PrVec2{40, 30}) == "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"40\" height=\"30\" viewBox=\"0 0 40 30\">\n"
        "<path d=\"M10 10L30 10L30 20L10 20Z\" fill=\"#FF0000\"/>\n"
        "<path d=\"M0 0L10 10\" transform=\"matrix(1 0 0 1 5 0)\" fill=\"none\" stroke=\"#0000FF\" "
        "stroke-opacity=\"0.5\" stroke-width=\"2\" stroke-linejoin=\"round\"/>\n"
        "</svg>\n");
    prDrawListRelease(list);
}

TEST_CASE("clips and plain groups are SVG's own") {
    PrDrawList* list = prDrawListCreate();
    prDrawListPushClipRect(list, PrRect{0, 0, 8, 8}, PrDrawEdges_Antialiased);
    PrDrawGroup group = prDrawGroupDefault();
    group.opacity = 0.25f;
    group.blendMode = PrDrawBlendMode_Multiply;
    prDrawListPushGroup(list, &group);
    prDrawListRect(list, PrRect{0, 0, 16, 16}, {.color = PrColor{0, 1, 0, 1}});
    prDrawListPopGroup(list);
    prDrawListPopClip(list);
    std::string svg = write(list, PrVec2{16, 16});
    CHECK(contains(svg, "<clipPath id=\"c1\"><path d=\"M0 0L8 0L8 8L0 8Z\"/></clipPath>"));
    CHECK(contains(svg, "<g clip-path=\"url(#c1)\">\n<g opacity=\"0.25\" style=\"mix-blend-mode:multiply;\">\n"));
    CHECK(!contains(svg, "<image"));
    prDrawListRelease(list);
}

TEST_CASE("gradients are SVG gradients in the command's space") {
    PrDrawList* list = prDrawListCreate();
    PrDrawColorStop stops[2] = {{.offset = 0, .color = PrColor{1, 0, 0, 1}}, {.offset = 1, .color = PrColor{0, 0, 1, 1}}};
    PrDrawBrush brush = prDrawBrushLinear(PrVec2{0, 0}, PrVec2{20, 0}, stops, 2);
    prDrawListRect(list, PrRect{0, 0, 20, 10}, {.brush = &brush});
    std::string svg = write(list, PrVec2{20, 10});
    CHECK(contains(svg, "<linearGradient id=\"g1\" x1=\"0\" y1=\"0\" x2=\"20\" y2=\"0\" gradientUnits=\"userSpaceOnUse\">"
        "<stop offset=\"0\" stop-color=\"#FF0000\"/><stop offset=\"1\" stop-color=\"#0000FF\"/></linearGradient>"));
    CHECK(contains(svg, "fill=\"url(#g1)\""));
    prDrawListRelease(list);
}

TEST_CASE("what SVG cannot draw is rasterized into an image where it lies") {
    PrDrawList* list = prDrawListCreate();
    PrDrawShadow shadow{.shape = {.rect = PrRect{4, 4, 12, 12}}, .sigma = 2};
    prDrawListDropShadow(list, &shadow, {.color = PrColor{0, 0, 0, 1}});
    std::string svg = write(list, PrVec2{16, 16});
    CHECK(contains(svg, "<image width=\""));
    CHECK(contains(svg, "href=\"data:image/png;base64,"));
    CHECK(!contains(svg, "<path"));
    prDrawListRelease(list);
}

TEST_CASE("each page is a document of its own, with its own definitions") {
    PrDrawList* first = prDrawListCreate();
    PrDrawColorStop stops[2] = {{.offset = 0, .color = PrColor{1, 0, 0, 1}}, {.offset = 1, .color = PrColor{0, 0, 1, 1}}};
    PrDrawBrush brush = prDrawBrushLinear(PrVec2{0, 0}, PrVec2{10, 0}, stops, 2);
    prDrawListRect(first, PrRect{0, 0, 10, 10}, {.brush = &brush});
    PrDrawList* second = prDrawListCreate();
    prDrawListRect(second, PrRect{0, 0, 5, 5}, {.brush = &brush});
    PrDrawPage pages[2] = {{.list = first, .extent = PrVec2{10, 20}}, {.list = second, .extent = PrVec2{30, 15}}};
    std::vector<std::string> svgs = writePages(pages, 2, "Two & more");
    REQUIRE(svgs.size() == 2);
    CHECK(contains(svgs[0], "width=\"10\" height=\"20\" viewBox=\"0 0 10 20\""));
    CHECK(contains(svgs[1], "width=\"30\" height=\"15\" viewBox=\"0 0 30 15\""));
    for (const std::string& svg : svgs) {
        CHECK(svg.starts_with("<?xml"));
        CHECK(contains(svg, "<title>Two &amp; more</title>"));
        CHECK(contains(svg, "<linearGradient id=\"g1\""));
        CHECK(contains(svg, "fill=\"url(#g1)\""));
    }
    prDrawListRelease(first);
    prDrawListRelease(second);
}

TEST_CASE("glyphs are outlines defined once, and the run's text labels them") {
    const PrFont* font = nullptr;
    REQUIRE(prFontCreateFromFile(PR_FONTS_DIR "/NotoSans.ttf", 0, nullptr, &font) == PrStatus_Ok);
    uint16_t a = prFontCodePointGlyph(font, 'a');
    PrFontGlyph glyphs[3] = {{a, PrVec2{0, 0}}, {a, PrVec2{10, 0}}, {a, PrVec2{20, 0}}};
    uint32_t clusters[3] = {0, 1, 2};
    PrFontGlyphRun run{.font = font, .size = 16, .glyphs = glyphs, .glyphCount = 3, .text = "aaa", .clusters = clusters};
    PrDrawList* list = prDrawListCreate();
    prDrawListGlyphs(list, &run, PrVec2{0, 20}, {.color = PrColor{0, 0, 0, 1}});
    std::string svg = write(list, PrVec2{40, 30});
    CHECK(contains(svg, "<path id=\"f0\" d=\"M"));
    CHECK(svg.find("<path id=") == svg.rfind("<path id="));
    CHECK(contains(svg, "<g fill=\"#000000\" aria-label=\"aaa\"><use href=\"#f0\" transform=\"matrix(16 0 0 16 0 20)\"/>"));
    prDrawListRelease(list);
    prFontRelease(font);
}
