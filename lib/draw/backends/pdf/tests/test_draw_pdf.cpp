#include "pr/compress.h"
#include "pr/draw_pdf.h"
#include "pr/float.h"
#include "pr/font.h"
#include "pr/task.h"

#include "tools/harness/test.h"

#include <cstdlib>
#include <string>
#include <vector>

extern "C" int prTestDrawPdfCApi(void);

namespace pr_impl {

namespace {

void collect(void* PR_NULLABLE context, uint32_t output, PrByteView bytes) {
    CHECK(output == 0);
    static_cast<std::string*>(context)->append(reinterpret_cast<const char*>(bytes.data), bytes.size);
}

// A document written by the PDF writer, read back by its cross-reference table or stream.
struct Pdf {
    std::string bytes;
    std::vector<std::string> bodies;    // by object: its body, or empty for a free object
    std::string trailer;                // the trailer's entries, in its dictionary or the cross-reference stream's
    PrCompressCodec* codec = nullptr;

    Pdf(const PrDrawPage* pages, uint32_t count, const PrDrawStructure* PR_NULLABLE structure = nullptr,
            bool compress = false, PrStringView title = {}, PrStringView language = {}, PrTaskPool* PR_NULLABLE tasks = nullptr) {
        codec = prCompressCodecCreate(PrCompressCodecBackend_Default);
        PrDrawWriterOptions options = prDrawWriterOptionsDefault();
        options.compress = compress ? codec : nullptr;
        options.tasks = tasks;
        PrDrawWriter* writer = prDrawPdfCreate(&options);
        PrDrawDocumentDesc desc{
            .write = collect,
            .context = &bytes,
            .structure = structure,
            .title = title,
            .language = language,
        };
        prDrawWriterWrite(writer, &desc, pages, count);
        prDrawWriterDestroy(writer);
        readXref();
    }

    ~Pdf() { prCompressCodecDestroy(codec); }

    Pdf(const Pdf&) = delete;
    Pdf& operator=(const Pdf&) = delete;

    // Returns the body of object `object`, which starts at `offset`, from after its header to its endobj, and checks
    // that its header names it.
    std::string direct(size_t object, size_t offset) const {
        std::string head = std::to_string(object) + " 0 obj\n";
        CHECK(bytes.compare(offset, head.size(), head) == 0);
        size_t start = offset + head.size();
        size_t end = bytes.find("\nendobj\n", start);
        REQUIRE(end != std::string::npos);
        return bytes.substr(start, end - start);
    }

    // Returns the integer after `entry` in `text`.
    static size_t integerAfter(const std::string& text, const std::string& entry) {
        size_t at = text.find(entry);
        REQUIRE(at != std::string::npos);
        return size_t(std::strtoull(text.c_str() + at + entry.size(), nullptr, 10));
    }

    // Reads the cross-reference table or stream that startxref names, and the objects that it finds.
    void readXref() {
        CHECK(bytes.compare(0, 9, "%PDF-2.0\n") == 0);
        CHECK(bytes.compare(bytes.size() - 6, 6, "%%EOF\n") == 0);
        size_t start = bytes.rfind("startxref\n");
        REQUIRE(start != std::string::npos);
        size_t xref = size_t(std::strtoull(bytes.c_str() + start + 10, nullptr, 10));
        if (bytes.compare(xref, 5, "xref\n") == 0) {
            size_t at = xref + 5;
            REQUIRE(bytes.compare(at, 2, "0 ") == 0);
            size_t count = size_t(std::strtoull(bytes.c_str() + at + 2, nullptr, 10));
            at = bytes.find('\n', at) + 1;
            bodies.assign(count, {});
            for (size_t i = 0; i < count; ++i) {
                std::string entry = bytes.substr(at + i * 20, 20);
                REQUIRE(entry.size() == 20);
                CHECK(entry.substr(18) == "\r\n");
                if (i != 0 && entry[17] == 'n')
                    bodies[i] = direct(i, size_t(std::strtoull(entry.c_str(), nullptr, 10)));
            }
            size_t dictionary = bytes.find("trailer\n", at + count * 20);
            REQUIRE(dictionary != std::string::npos);
            trailer = bytes.substr(dictionary, start - dictionary);
            return;
        }
        // A cross-reference stream: entries of a type, a field of /W's width, and two bytes.
        size_t number = size_t(std::strtoull(bytes.c_str() + xref, nullptr, 10));
        std::string body = direct(number, xref);
        trailer = body.substr(0, body.find(">>\nstream\n"));
        CHECK(trailer.find("/Type /XRef") != std::string::npos);
        size_t count = integerAfter(trailer, "/Size ");
        size_t width = integerAfter(trailer, "/W [1 ");
        std::string entries = data(body);
        REQUIRE(entries.size() == count * (width + 3));
        auto field = [&](size_t at, size_t size) {
            size_t value = 0;
            for (size_t k = 0; k < size; ++k)
                value = value << 8 | uint8_t(entries[at + k]);
            return value;
        };
        bodies.assign(count, {});
        for (size_t i = 1; i < count; ++i) {
            size_t at = i * (width + 3);
            if (entries[at] == 1)
                bodies[i] = direct(i, field(at + 1, width));
        }
        // Each object in an object stream follows the stream's pairs of number and offset, from /First.
        for (size_t i = 1; i < count; ++i) {
            size_t at = i * (width + 3);
            if (entries[at] != 2)
                continue;
            size_t container = field(at + 1, width);
            size_t index = field(at + 1 + width, 2);
            const std::string& head = bodies[container];
            REQUIRE(head.find("/Type /ObjStm") != std::string::npos);
            std::string packed = data(head);
            size_t first = integerAfter(head, "/First ");
            size_t objects = integerAfter(head, "/N ");
            REQUIRE(index < objects);
            std::vector<size_t> numbers(objects);
            std::vector<size_t> offsets(objects + 1, packed.size() - first);
            const char* cursor = packed.c_str();
            for (size_t k = 0; k < objects; ++k) {
                char* end = nullptr;
                numbers[k] = size_t(std::strtoull(cursor, &end, 10));
                offsets[k] = size_t(std::strtoull(end, &end, 10));
                cursor = end;
            }
            CHECK(numbers[index] == i);
            std::string object = packed.substr(first + offsets[index], offsets[index + 1] - offsets[index]);
            while (!object.empty() && object.back() == '\n')
                object.pop_back();
            bodies[i] = object;
        }
    }

    // Returns the body of object `object`, from after its header to its endobj.
    std::string object(size_t object) const {
        REQUIRE(object < bodies.size());
        return bodies[object];
    }

    // Returns the object that the reference after `entry` in `text` names, as "/Key N 0 R".
    size_t referenced(const std::string& text, const std::string& entry) const {
        size_t at = text.find(entry);
        REQUIRE(at != std::string::npos);
        at += entry.size();
        while (at < text.size() && (text[at] == ' ' || text[at] == '['))
            ++at;
        return size_t(std::strtoull(text.c_str() + at, nullptr, 10));
    }

    // Returns the number of the first object whose body contains `part`, or zero.
    size_t find(const std::string& part) const {
        for (size_t i = 1; i < bodies.size(); ++i) {
            if (bodies[i].find(part) != std::string::npos)
                return i;
        }
        return 0;
    }

    // Returns the data of the stream `object`, decompressed if it is compressed.
    std::string stream(size_t object) const {
        return data(this->object(object));
    }

    // Returns the data of a stream whose body is `body`, decompressed if it is compressed.
    std::string data(const std::string& body) const {
        size_t start = body.find(">>\nstream\n");
        REQUIRE(start != std::string::npos);
        start += 10;
        size_t end = body.rfind("\nendstream");
        std::string data = body.substr(start, end - start);
        if (body.find("/FlateDecode") == std::string::npos)
            return data;
        std::vector<uint8_t> out(1 << 22);
        PrCompressRead read{};
        PrByteView input{reinterpret_cast<const uint8_t*>(data.data()), data.size()};
        REQUIRE(prCompressCodecDecode(codec, PrCompressFormat_Zlib, input, out.data(), out.size(), &read) ==
            PrStatus_Ok);
        return std::string(reinterpret_cast<const char*>(out.data()), read.written);
    }

    // Returns the content stream of the first page.
    std::string pageContent() const {
        size_t page = find("/Type /Page ");
        REQUIRE(page != 0);
        return stream(referenced(object(page), "/Contents"));
    }
};

bool contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

const PrFont* notoSans() {
    const PrFont* font = nullptr;
    REQUIRE(prFontCreateFromFile(PR_FONTS_DIR "/NotoSans[wdth,wght].ttf", 0, nullptr, &font) == PrStatus_Ok);
    return font;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("the PDF writer's header compiles as C") {
    CHECK(prTestDrawPdfCApi() == 0);
}

TEST_CASE("a page's content is in px with y down, scaled to points") {
    PrDrawList* list = prDrawListCreate();
    prDrawListRect(list, PrRect{10, 10, 30, 20}, {.color = PrColor{1, 0, 0, 1}});
    PrDrawPage page{.list = list, .extent = PrVec2{200, 100}};
    Pdf pdf(&page, 1);
    std::string pageObject = pdf.object(pdf.find("/Type /Page "));
    CHECK(contains(pageObject, "/MediaBox [0 0 150 75]"));
    std::string content = pdf.pageContent();
    CHECK(content.find("0.75 0 0 -0.75 0 75 cm\n") == 0);
    CHECK(contains(content, "1 0 0 rg\n10 10 20 10 re\nf\n"));
    std::string catalog = pdf.object(1);
    CHECK(contains(catalog, "/Type /Catalog /Pages 2 0 R"));
    CHECK(!contains(catalog, "/StructTreeRoot"));
    prDrawListRelease(list);
}

TEST_CASE("a page writes its bleed and trim boxes in points with y up, if they differ from its media box") {
    PrDrawList* list = prDrawListCreate();
    PrDrawPage pages[3] = {
        {.list = list, .extent = PrVec2{200, 100}, .bleed = PrRect{8, 8, 192, 92}, .trim = PrRect{16, 12, 184, 92}},
        {.list = list, .extent = PrVec2{200, 100}, .bleed = PrRect{0, 0, 200, 100}, .trim = PrRect{8, 8, 192, 92}},
        {.list = list, .extent = PrVec2{200, 100}},
    };
    Pdf pdf(pages, 3);
    // The pages' objects, in the order of the pages.
    std::vector<std::string> objects;
    for (const std::string& body : pdf.bodies) {
        if (contains(body, "/Type /Page /Parent"))
            objects.push_back(body);
    }
    REQUIRE(objects.size() == 3);
    CHECK(contains(objects[0], "/MediaBox [0 0 150 75] /BleedBox [6 6 144 69] /TrimBox [12 6 138 66]"));
    // A box that is the media box is left out, since a missing box defaults to it, and so is an empty box.
    CHECK(!contains(objects[1], "/BleedBox"));
    CHECK(contains(objects[1], "/TrimBox [6 6 144 69]"));
    CHECK(!contains(objects[2], "/BleedBox"));
    CHECK(!contains(objects[2], "/TrimBox"));
    prDrawListRelease(list);
}

TEST_CASE("streams are compressed with the writer's codec") {
    PrDrawList* list = prDrawListCreate();
    for (int i = 0; i < 20; ++i)
        prDrawListRect(list, PrRect{float(i), 0, float(i) + 1, 10}, {.color = PrColor{0, 0, 0, 1}});
    PrDrawPage page{.list = list, .extent = PrVec2{100, 100}};
    Pdf pdf(&page, 1, nullptr, true);
    size_t pageObject = pdf.find("/Type /Page ");
    std::string raw = pdf.object(pdf.referenced(pdf.object(pageObject), "/Contents"));
    CHECK(contains(raw, "/Filter /FlateDecode"));
    CHECK(contains(pdf.pageContent(), "19 0 1 10 re\nf\n"));
    // The objects that are not streams are compressed in object streams, which a cross-reference stream finds.
    CHECK(contains(pdf.object(1), "/Type /Catalog"));
    CHECK(!contains(pdf.bytes, "/Type /Catalog"));
    CHECK(contains(pdf.trailer, "/Root 1 0 R"));
    CHECK(contains(pdf.trailer, "/ID [<"));
    prDrawListRelease(list);
}

TEST_CASE("a writer with a pool compresses streams on its threads, and writes the same objects") {
    std::vector<PrDrawList*> lists;
    std::vector<PrDrawPage> pages;
    for (int i = 0; i < 150; ++i) {
        PrDrawList* list = prDrawListCreate();
        for (int k = 0; k < 20; ++k)
            prDrawListRect(list, PrRect{float(k), float(i), float(k) + 1, 10}, {.color = PrColor{0, 0, 0, 1}});
        lists.push_back(list);
        pages.push_back(PrDrawPage{.list = list, .extent = PrVec2{100, 100}});
    }
    PrTaskPool* tasks = prTaskPoolCreate(PrTaskPoolBackend_Default, 3);
    Pdf alone(pages.data(), uint32_t(pages.size()), nullptr, true);
    Pdf pooled(pages.data(), uint32_t(pages.size()), nullptr, true, {}, {}, tasks);
    // The streams are written in another order, so only the cross-reference stream, the last object, and the identifier
    // that it holds differ.
    REQUIRE(pooled.bodies.size() == alone.bodies.size());
    for (size_t i = 0; i + 1 < alone.bodies.size(); ++i)
        CHECK(pooled.bodies[i] == alone.bodies[i]);
    prTaskPoolDestroy(tasks);
    for (PrDrawList* list : lists)
        prDrawListRelease(list);
}

TEST_CASE("a document has an identifier made from its content") {
    PrDrawList* list = prDrawListCreate();
    prDrawListRect(list, PrRect{0, 0, 10, 10}, {.color = PrColor{0, 0, 0, 1}});
    PrDrawPage page{.list = list, .extent = PrVec2{100, 100}};
    Pdf first(&page, 1);
    Pdf second(&page, 1);
    size_t at = first.trailer.find("/ID [<");
    REQUIRE(at != std::string::npos);
    std::string id = first.trailer.substr(at + 6, 32);
    CHECK(id.find_first_not_of("0123456789ABCDEF") == std::string::npos);
    CHECK(contains(first.trailer, "/ID [<" + id + "> <" + id + ">]"));
    CHECK(first.bytes == second.bytes);
    prDrawListRect(list, PrRect{0, 0, 20, 20}, {.color = PrColor{0, 0, 0, 1}});
    Pdf third(&page, 1);
    CHECK(!contains(third.trailer, id));
    prDrawListRelease(list);
}

TEST_CASE("groups are transparency groups and alpha is a graphics state") {
    PrDrawList* list = prDrawListCreate();
    PrDrawGroup group = prDrawGroupDefault();
    group.opacity = 0.5f;
    group.blendMode = PrDrawBlendMode_Multiply;
    prDrawListPushGroup(list, &group);
    prDrawListRect(list, PrRect{0, 0, 10, 10}, {.color = PrColor{0, 0, 1, 0.25f}});
    prDrawListPopGroup(list);
    PrDrawPage page{.list = list, .extent = PrVec2{20, 20}};
    Pdf pdf(&page, 1);
    size_t form = pdf.find("/Subtype /Form");
    REQUIRE(form != 0);
    CHECK(contains(pdf.object(form), "/Group << /S /Transparency >>"));
    CHECK(contains(pdf.stream(form), "/G0 gs"));
    CHECK(contains(pdf.object(pdf.find("/Type /ExtGState /ca 0.25")), "/CA 1"));
    CHECK(pdf.find("/Type /ExtGState /ca 0.5 /CA 0.5 /BM /Multiply") != 0);
    CHECK(contains(pdf.pageContent(), "/X0 Do"));
    prDrawListRelease(list);
}

TEST_CASE("gradients are shading patterns") {
    PrDrawList* list = prDrawListCreate();
    PrDrawColorStop stops[2] = {
        {.offset = 0, .color = PrColor{1, 0, 0, 1}},
        {.offset = 1, .color = PrColor{0, 0, 1, 1}},
    };
    PrDrawBrush brush = prDrawBrushRadial(PrVec2{5, 5}, 0, PrVec2{10, 10}, 10, stops, 2);
    prDrawListRect(list, PrRect{0, 0, 20, 20}, {.brush = &brush});
    PrDrawPage page{.list = list, .extent = PrVec2{20, 20}};
    Pdf pdf(&page, 1);
    size_t pattern = pdf.find("/PatternType 2");
    REQUIRE(pattern != 0);
    std::string body = pdf.object(pattern);
    CHECK(contains(body, "/ShadingType 3 /Coords [5 5 0 10 10 10 ]"));
    CHECK(contains(body, "/C0 [1 0 0] /C1 [0 0 1]"));
    CHECK(contains(body, "/Extend [true true]"));
    CHECK(contains(pdf.pageContent(), "/Pattern cs /P0 scn\n"));
    prDrawListRelease(list);
}

TEST_CASE("what PDF cannot draw is rasterized into an image with a soft mask") {
    PrDrawList* list = prDrawListCreate();
    PrDrawShadow shadow{.shape = {.rect = PrRect{4, 4, 12, 12}}, .sigma = 2};
    prDrawListDropShadow(list, &shadow, {.color = PrColor{0, 0, 0, 1}});
    PrDrawPage page{.list = list, .extent = PrVec2{16, 16}};
    Pdf pdf(&page, 1);
    size_t image = pdf.find("/Subtype /Image /ColorSpace /DeviceRGB");
    REQUIRE(image != 0);
    size_t mask = pdf.referenced(pdf.object(image), "/SMask");
    CHECK(contains(pdf.object(mask), "/ColorSpace /DeviceGray"));
    CHECK(contains(pdf.pageContent(), "/I0 Do"));
    prDrawListRelease(list);
}

TEST_CASE("text is a subset font that maps back to the run's characters") {
    const PrFont* font = notoSans();
    uint16_t h = prFontCodePointGlyph(font, 'H');
    uint16_t i = prFontCodePointGlyph(font, 'i');
    PrFontGlyph glyphs[2] = {{h, PrVec2{0, 0}}, {i, PrVec2{12, 0}}};
    uint32_t clusters[2] = {0, 1};
    PrFontGlyphRun run{.font = font, .size = 20, .glyphs = glyphs, .glyphCount = 2, .text = "Hi", .clusters = clusters};
    PrDrawList* list = prDrawListCreate();
    prDrawListGlyphs(list, &run, PrVec2{4, 30}, {.color = PrColor{0, 0, 0, 1}});
    PrDrawPage page{.list = list, .extent = PrVec2{60, 40}};
    Pdf pdf(&page, 1, nullptr, true);
    size_t type0 = pdf.find("/Subtype /Type0");
    REQUIRE(type0 != 0);
    std::string fontObject = pdf.object(type0);
    CHECK(contains(fontObject, "/Encoding /Identity-H"));
    std::string cid = pdf.object(pdf.referenced(fontObject, "/DescendantFonts"));
    CHECK(contains(cid, "/Subtype /CIDFontType2"));
    CHECK(contains(cid, "/CIDToGIDMap /Identity"));
    // The font is named by its PostScript name, after the subset's tag.
    CHECK(contains(cid, "/BaseFont /AAAAAA+NotoSans-Regular"));
    std::string descriptor = pdf.object(pdf.referenced(cid, "/FontDescriptor"));
    size_t file = pdf.referenced(descriptor, "/FontFile2");
    std::string subset = pdf.stream(file);
    CHECK(subset.size() < prFontData(font).size / 4);
    const PrFont* reopened = nullptr;
    CHECK(prFontCreate(PrByteView{reinterpret_cast<const uint8_t*>(subset.data()), subset.size()}, 0, nullptr,
        &reopened) == PrStatus_Ok);
    prFontRelease(reopened);
    std::string toUnicode = pdf.stream(pdf.referenced(fontObject, "/ToUnicode"));
    char hex[16];
    std::snprintf(hex, sizeof(hex), "<%04X> <0048>", h);
    CHECK(contains(toUnicode, hex));
    std::snprintf(hex, sizeof(hex), "<%04X> <0069>", i);
    CHECK(contains(toUnicode, hex));
    // The run is one text object at the run's size, whose TJ array moves `i` back from where the width of `h` leaves
    // it, in thousandths of an em.
    std::string content = pdf.pageContent();
    CHECK(contains(content, "BT\n/F0 20 Tf\n1 0 0 -1 4 30 Tm\n"));
    long back = long(prF32Round(prFontGlyphAdvance(font, h, 0) * 1000 - 12.0f / 20 * 1000));
    char shown[64];
    std::snprintf(shown, sizeof(shown), "[<%04X>%ld<%04X>]TJ\nET\n", h, back, i);
    CHECK(contains(content, shown));
    CHECK(!contains(content, "/ActualText"));
    prDrawListRelease(list);
    prFontRelease(font);
}

TEST_CASE("runs of text share a text object, which starts each line by Td") {
    const PrFont* font = notoSans();
    uint16_t a = prFontCodePointGlyph(font, 'a');
    float width = prFontGlyphAdvance(font, a, 0) * 10;
    // Two glyphs where the font's width places them, and a third raised off the line, as a superscript is.
    PrFontGlyph glyphs[3] = {{a, PrVec2{0, 0}}, {a, PrVec2{width, 0}}, {a, PrVec2{2 * width, -4}}};
    PrFontGlyphRun run{.font = font, .size = 10, .glyphs = glyphs, .glyphCount = 3};
    PrDrawList* list = prDrawListCreate();
    prDrawListGlyphs(list, &run, PrVec2{5, 20}, {.color = PrColor{0, 0, 0, 1}});
    run.glyphCount = 1;
    prDrawListGlyphs(list, &run, PrVec2{5, 34}, {.color = PrColor{0, 0, 0, 1}});
    PrDrawPage page{.list = list, .extent = PrVec2{60, 60}};
    Pdf pdf(&page, 1);
    std::string content = pdf.pageContent();
    char expected[160];
    std::snprintf(expected, sizeof(expected), "0 0 0 rg\nBT\n/F0 10 Tf\n1 0 0 -1 5 20 Tm\n[<%04X%04X>]TJ\n", a, a);
    CHECK(contains(content, expected));
    // The raised glyph and the next run each start a line, and the font and the color are not selected again.
    std::string after = content.substr(content.find("]TJ\n") + 4);
    CHECK(contains(after, " 4 Td\n["));
    CHECK(contains(after, " -18 Td\n["));
    CHECK(!contains(after, "Tf"));
    CHECK(!contains(after, "rg"));
    CHECK(content.find("ET\n") == content.rfind("ET\n"));
    prDrawListRelease(list);
    prFontRelease(font);
}

TEST_CASE("a run whose glyphs do not map back to its text carries it as replacement text") {
    const PrFont* font = notoSans();
    uint16_t f = prFontCodePointGlyph(font, 'f');
    // One glyph shaped from two characters, as a ligature is.
    PrFontGlyph glyphs[1] = {{f, PrVec2{0, 0}}};
    uint32_t clusters[1] = {0};
    PrFontGlyphRun run{.font = font, .size = 20, .glyphs = glyphs, .glyphCount = 1, .text = "fi", .clusters = clusters};
    PrDrawList* list = prDrawListCreate();
    prDrawListGlyphs(list, &run, PrVec2{4, 30}, {.color = PrColor{0, 0, 0, 1}});
    // A second run shows the same glyph as the one character it is.
    run.text = "f";
    prDrawListGlyphs(list, &run, PrVec2{30, 30}, {.color = PrColor{0, 0, 0, 1}});
    PrDrawPage page{.list = list, .extent = PrVec2{60, 40}};
    Pdf pdf(&page, 1);
    std::string content = pdf.pageContent();
    CHECK(!contains(content, "/Span << /ActualText (fi) >> BDC"));
    CHECK(contains(content, "/Span << /ActualText (f) >> BDC"));
    prDrawListRelease(list);
    prFontRelease(font);
}

TEST_CASE("a tagged document joins its marks to its structure") {
    PrDrawList* list = prDrawListCreate();
    prDrawListRect(list, PrRect{0, 0, 100, 100}, {.color = PrColor{0.5f, 0.5f, 0.5f, 1}});
    prDrawListPushMark(list, 101);
    prDrawListRect(list, PrRect{10, 10, 90, 20}, {.color = PrColor{0, 0, 0, 1}});
    prDrawListPopMark(list);
    prDrawListPushMark(list, 102);
    prDrawListRect(list, PrRect{10, 30, 50, 40}, {.color = PrColor{0, 0, 0, 1}});
    prDrawListPopMark(list);
    prDrawListPushMark(list, 999);
    prDrawListRect(list, PrRect{10, 50, 50, 60}, {.color = PrColor{0, 0, 0, 1}});
    prDrawListPopMark(list);
    PrDrawPage page{.list = list, .extent = PrVec2{100, 100}};
    PrDrawStructure* structure = prDrawStructureCreate(nullptr);
    PrDrawStructureNodeDesc heading = prDrawStructureNodeDescDefault(PrDrawStructureType_H1);
    heading.title = "Title";
    PrDrawStructureNode h1 = prDrawStructureAppendNode(structure, kPrDrawStructureRoot, &heading);
    prDrawStructureAppendKey(structure, h1, 101);
    PrDrawStructureNodeDesc paragraph = prDrawStructureNodeDescDefault(PrDrawStructureType_P);
    PrDrawStructureNode p = prDrawStructureAppendNode(structure, kPrDrawStructureRoot, &paragraph);
    PrDrawStructureNodeDesc link = prDrawStructureNodeDescDefault(PrDrawStructureType_Link);
    link.uri = "https://example.com/";
    PrDrawStructureNode a = prDrawStructureAppendNode(structure, p, &link);
    prDrawStructureAppendKey(structure, a, 102);
    Pdf pdf(&page, 1, structure, false, "Doc", "en");
    std::string content = pdf.pageContent();
    // The background and the mark that no node names are artifacts.
    CHECK(contains(content, "/Artifact BMC\n0.5 0.5 0.5 rg"));
    CHECK(contains(content, "/H1 << /MCID 0 >> BDC\n"));
    CHECK(contains(content, "/Link << /MCID 1 >> BDC\n"));
    CHECK(!contains(content, "/MCID 2"));
    std::string catalog = pdf.object(1);
    CHECK(contains(catalog, "/Lang (en)"));
    CHECK(contains(catalog, "/MarkInfo << /Marked true >>"));
    CHECK(contains(catalog, "/ViewerPreferences << /DisplayDocTitle true >>"));
    // The metadata gives the title and claims PDF/UA-2, as an XMP packet that is not compressed.
    std::string metadata = pdf.object(pdf.referenced(catalog, "/Metadata"));
    CHECK(contains(metadata, "/Type /Metadata /Subtype /XML"));
    CHECK(contains(metadata, "<dc:title><rdf:Alt><rdf:li xml:lang=\"x-default\">Doc</rdf:li></rdf:Alt></dc:title>"));
    CHECK(contains(metadata, "<pdfuaid:part>2</pdfuaid:part>"));
    std::string root = pdf.object(pdf.referenced(catalog, "/StructTreeRoot"));
    CHECK(contains(root, "/ParentTree << /Nums [0 ["));
    std::string document = pdf.object(pdf.referenced(root, "/K"));
    CHECK(contains(document, "/S /Document"));
    std::string h1Element = pdf.object(pdf.find("/S /H1"));
    CHECK(contains(h1Element, "/K [0 ]"));
    CHECK(contains(h1Element, "/T (Title)"));
    std::string linkElement = pdf.object(pdf.find("/S /Link"));
    CHECK(contains(linkElement, "/Type /OBJR"));
    size_t annotation = pdf.find("/Subtype /Link");
    REQUIRE(annotation != 0);
    std::string annotationObject = pdf.object(annotation);
    CHECK(contains(annotationObject, "/URI (https://example.com/)"));
    CHECK(contains(annotationObject, "/Rect ["));
    CHECK(contains(annotationObject, "/StructParent 1"));
    std::string pageObject = pdf.object(pdf.find("/Type /Page "));
    CHECK(contains(pageObject, "/Annots ["));
    CHECK(contains(pageObject, "/StructParents 0"));
    CHECK(contains(pageObject, "/Tabs /S"));
    std::string outline = pdf.object(pdf.referenced(catalog, "/Outlines"));
    CHECK(contains(outline, "/Count 1"));
    CHECK(pdf.find("/Title (Title) /Parent") != 0);
    prDrawStructureDestroy(structure);
    prDrawListRelease(list);
}

TEST_CASE("a node's content may span pages") {
    PrDrawList* first = prDrawListCreate();
    prDrawListPushMark(first, 7);
    prDrawListRect(first, PrRect{0, 0, 10, 10}, {.color = PrColor{0, 0, 0, 1}});
    prDrawListPopMark(first);
    PrDrawList* second = prDrawListCreate();
    prDrawListPushMark(second, 7);
    prDrawListRect(second, PrRect{0, 0, 10, 10}, {.color = PrColor{0, 0, 0, 1}});
    prDrawListPopMark(second);
    PrDrawPage pages[2] = {{.list = first, .extent = PrVec2{20, 20}}, {.list = second, .extent = PrVec2{20, 20}}};
    PrDrawStructure* structure = prDrawStructureCreate(nullptr);
    PrDrawStructureNodeDesc paragraph = prDrawStructureNodeDescDefault(PrDrawStructureType_P);
    prDrawStructureAppendKey(structure, prDrawStructureAppendNode(structure, kPrDrawStructureRoot, &paragraph), 7);
    Pdf pdf(pages, 2, structure);
    std::string element = pdf.object(pdf.find("/S /P"));
    CHECK(contains(element, "/K [0 << /Type /MCR /Pg "));
    prDrawStructureDestroy(structure);
    prDrawListRelease(first);
    prDrawListRelease(second);
}

TEST_CASE("a rasterized group's image takes the place of its first mark's content") {
    PrDrawList* list = prDrawListCreate();
    PrDrawGroup group = prDrawGroupDefault();
    group.blendMode = PrDrawBlendMode_HardMix;
    prDrawListPushGroup(list, &group);
    prDrawListPushMark(list, 5);
    prDrawListRect(list, PrRect{2, 2, 10, 10}, {.color = PrColor{0, 0, 0, 1}});
    prDrawListPopMark(list);
    prDrawListPopGroup(list);
    PrDrawPage page{.list = list, .extent = PrVec2{12, 12}};
    PrDrawStructure* structure = prDrawStructureCreate(nullptr);
    PrDrawStructureNodeDesc figure = prDrawStructureNodeDescDefault(PrDrawStructureType_Figure);
    figure.alt = "A square";
    prDrawStructureAppendKey(structure, prDrawStructureAppendNode(structure, kPrDrawStructureRoot, &figure), 5);
    Pdf pdf(&page, 1, structure);
    std::string content = pdf.pageContent();
    size_t mark = content.find("/Figure << /MCID 0 >> BDC\n");
    size_t image = content.find("/I0 Do");
    REQUIRE(mark != std::string::npos);
    REQUIRE(image != std::string::npos);
    CHECK(mark < image);
    CHECK(content.find("EMC", image) != std::string::npos);
    CHECK(contains(pdf.object(pdf.find("/S /Figure")), "/Alt (A square)"));
    prDrawStructureDestroy(structure);
    prDrawListRelease(list);
}

TEST_CASE("glyphs that letter spacing places past the font's widths show with character spacing") {
    const PrFont* font = notoSans();
    uint16_t a = prFontCodePointGlyph(font, 'a');
    float width = prFontGlyphAdvance(font, a, 0) * 10;
    // Each glyph is 2 px past where the width of the one before it leaves the pen, which is 200 thousandths of the
    // size.
    PrFontGlyph glyphs[4] = {
        {a, PrVec2{0, 0}}, {a, PrVec2{width + 2, 0}}, {a, PrVec2{2 * width + 4, 0}}, {a, PrVec2{3 * width + 6, 0}},
    };
    PrFontGlyphRun run{.font = font, .size = 10, .glyphs = glyphs, .glyphCount = 4};
    PrDrawList* list = prDrawListCreate();
    prDrawListGlyphs(list, &run, PrVec2{5, 20}, {.color = PrColor{0, 0, 0, 1}});
    // A run whose glyphs the widths place resets the spacing.
    glyphs[1].position.x = width;
    run.glyphCount = 2;
    prDrawListGlyphs(list, &run, PrVec2{5, 40}, {.color = PrColor{0, 0, 0, 1}});
    PrDrawPage page{.list = list, .extent = PrVec2{60, 60}};
    Pdf pdf(&page, 1);
    std::string content = pdf.pageContent();
    char expected[160];
    std::snprintf(expected, sizeof(expected), "2 Tc\n[<%04X%04X%04X%04X>]TJ\n", a, a, a, a);
    CHECK(contains(content, expected));
    std::snprintf(expected, sizeof(expected), "0 Tc\n[<%04X%04X>]TJ\n", a, a);
    CHECK(contains(content, expected));
    prDrawListRelease(list);
    prFontRelease(font);
}

TEST_CASE("text in a font with synthetic bold is text of a CFF font that holds the emboldened outlines") {
    const PrFont* font = notoSans();
    PrFontDerivation bold = prFontDerivationDefault();
    bold.embolden = 0.02f;
    const PrFont* derived = prFontCreateDerived(font, &bold);
    uint16_t h = prFontCodePointGlyph(font, 'H');
    PrFontGlyph glyphs[1] = {{h, PrVec2{0, 0}}};
    uint32_t clusters[1] = {0};
    PrFontGlyphRun run{
        .font = derived,
        .size = 20,
        .glyphs = glyphs,
        .glyphCount = 1,
        .text = "H",
        .clusters = clusters,
    };
    PrDrawList* list = prDrawListCreate();
    prDrawListGlyphs(list, &run, PrVec2{4, 30}, {.color = PrColor{0, 0, 0, 1}});
    PrDrawPage page{.list = list, .extent = PrVec2{60, 40}};
    Pdf pdf(&page, 1);
    std::string fontObject = pdf.object(pdf.find("/Subtype /Type0"));
    std::string cid = pdf.object(pdf.referenced(fontObject, "/DescendantFonts"));
    CHECK(contains(cid, "/Subtype /CIDFontType0"));
    std::string descriptor = pdf.object(pdf.referenced(cid, "/FontDescriptor"));
    CHECK(contains(pdf.object(pdf.referenced(descriptor, "/FontFile3")), "/Subtype /CIDFontType0C"));
    char hex[16];
    std::snprintf(hex, sizeof(hex), "<%04X> <0048>", h);
    CHECK(contains(pdf.stream(pdf.referenced(fontObject, "/ToUnicode")), hex));
    std::string content = pdf.pageContent();
    std::snprintf(hex, sizeof(hex), "[<%04X>]TJ", h);
    CHECK(contains(content, hex));
    CHECK(!contains(content, "/ActualText"));
    prDrawListRelease(list);
    prFontRelease(derived);
    prFontRelease(font);
}
