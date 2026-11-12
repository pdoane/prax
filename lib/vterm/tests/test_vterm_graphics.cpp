// Tests the graphics protocol. They cover transmission (formats, media, chunks and compression), the answers,
// placements and their geometry, how placements move with the text, deletion, the image budget, and the placeholders of
// virtual placements.

#include "pr/array.h"
#include "pr/base64.h"
#include "pr/compress.h"
#include "pr/compress_builtin.h"
#include "pr/file.h"
#include "pr/image.h"
#include "pr/image_builtin.h"
#include "pr/string.h"
#include "pr/string_ops.h"
#include "pr/vterm.h"
#include "pr/vterm_graphics.h"

#include "tools/harness/test.h"

namespace pr_impl {

using namespace pr;

namespace {

// The pixel extent of a cell in every test terminal.
constexpr PrUVec2 kCell = {10, 20};

// The codecs that the test terminals decode images with.
struct Codecs {
    PrCompressCodec* compress = prCompressBuiltinCreate();
    PrImageCodec* image = prImageBuiltinCreate(compress);

    ~Codecs() {
        prImageCodecDestroy(image);
        prCompressCodecDestroy(compress);
    }
};

Codecs& codecs() {
    static Codecs shared;
    return shared;
}

// A terminal of 20 columns by 10 rows, with 10 lines of scrollback. It keeps images within `budget` bytes.
struct Term {
    PrVterm* vterm;

    explicit Term(uint64_t budget = kPrVtermImageBytesDefault, PrVtermImageFlags flags = 0) {
        PrVtermDesc desc{
            .extent = {20, 10},
            .scrollback = 10,
            .imageBytes = budget,
            .imageFlags = flags,
            .compressCodec = codecs().compress,
            .imageCodec = codecs().image,
        };
        vterm = prVtermCreate(&desc);
        prVtermSetCellPixelExtent(vterm, kCell);
    }
    ~Term() { prVtermDestroy(vterm); }
    Term(const Term&) = delete;
    Term& operator=(const Term&) = delete;

    void write(PrStringView text) {
        prVtermWrite(vterm, prStringBytes(text));
    }

    String output() {
        PrByteView bytes = prVtermOutput(vterm);
        String out(prStringFromBytes(bytes));
        prVtermOutputConsume(vterm, bytes.size);
        return out;
    }

    uint32_t lineCount() const { return prVtermScrollbackCount(vterm) + prVtermExtent(vterm).y; }

    // The placements on every line.
    Array<PrVtermPlacement> placements() const {
        Array<PrVtermPlacement> out(prVtermPlacements(vterm, 0, lineCount(), nullptr, 0));
        CHECK(prVtermPlacements(vterm, 0, lineCount(), out.data(), out.count()) == out.count());
        return out;
    }

    // The placements on the screen's rows.
    Array<PrVtermPlacement> onScreen() const {
        uint32_t first = prVtermScrollbackCount(vterm);
        uint32_t rows = prVtermExtent(vterm).y;
        Array<PrVtermPlacement> out(prVtermPlacements(vterm, first, rows, nullptr, 0));
        prVtermPlacements(vterm, first, rows, out.data(), out.count());
        return out;
    }

    // The screen row of a placement's anchor. It is negative if the anchor is in the scrollback.
    int64_t row(const PrVtermPlacement& placement) const {
        return placement.line - int64_t(prVtermScrollbackCount(vterm));
    }

    PrUVec2 cursor() const { return prVtermCursor(vterm).position; }

    String text(uint32_t index) const {
        PrRawString text = prRawStringNull();
        prVtermLineAppendText(vterm, index, 0, prVtermExtent(vterm).x, &text);
        PrStringView view = prRawStringView(&text);
        while (!view.isEmpty() && view.data[view.size - 1] == ' ')
            --view.size;
        String line(view);
        prRawStringReset(&text);
        return line;
    }
};

String base64(PrStringView bytes) {
    Array<char> out(prBase64EncodedSize(bytes.size));
    prBase64Encode(bytes, out.data(), out.count());
    return String(PrStringView(out.data(), out.count()));
}

// A graphics command with the `control` keys, and with `payload` encoded in base64.
String command(PrStringView control, PrStringView payload = "") {
    String out = format("\x1b_G{}", control);
    if (!payload.isEmpty())
        formatTo(out, ";{}", base64(payload));
    return format("{}\x1b\\", out);
}

// The answer that the terminal sends to a command.
String answer(PrStringView keys, PrStringView message) {
    return format("\x1b_G{};{}\x1b\\", keys, message);
}

// Pixels of one color, `channels` bytes each. A fourth channel is alpha 0x80.
String solid(uint32_t width, uint32_t height, uint32_t channels, uint8_t red, uint8_t green, uint8_t blue) {
    String out;
    for (uint32_t i = 0; i < width * height; ++i) {
        out.append(char(red));
        out.append(char(green));
        out.append(char(blue));
        if (channels == 4)
            out.append(char(0x80));
    }
    return out;
}

String zlib(PrStringView bytes) {
    PrByteView input = prStringBytes(bytes);
    size_t bound = prCompressCodecEncodeBound(codecs().compress, PrCompressFormat_Zlib, bytes.size);
    Array<uint8_t> out(bound);
    size_t written = 0;
    REQUIRE(prCompressCodecEncode(codecs().compress, PrCompressFormat_Zlib, PrCompressLevel_Default, input, out.data(),
        &written) == PrStatus_Ok);
    return String(PrStringView(reinterpret_cast<const char*>(out.data()), written));
}

// A PNG of `width` by `height` pixels of one opaque color.
String png(uint32_t width, uint32_t height, uint8_t red, uint8_t green, uint8_t blue) {
    String rgba;
    for (uint32_t i = 0; i < width * height; ++i) {
        rgba.append(char(red));
        rgba.append(char(green));
        rgba.append(char(blue));
        rgba.append(char(0xff));
    }
    PrPixelView pixels{prStringBytes(rgba).data, size_t(width) * 4, {width, height}};
    size_t size = 0;
    prImageCodecEncode(codecs().image, "image/png", pixels, nullptr, 0, &size);
    Array<uint8_t> out(size);
    REQUIRE(prImageCodecEncode(codecs().image, "image/png", pixels, out.data(), size, &size) == PrStatus_Ok);
    return String(PrStringView(reinterpret_cast<const char*>(out.data()), size));
}

// The first pixel of the image of `placement`.
String firstPixel(const Term& t, const PrVtermPlacement& placement) {
    PrVtermImage image{};
    REQUIRE(prVtermImage(t.vterm, placement.image, &image));
    return String(PrStringView(reinterpret_cast<const char*>(image.pixels), 4));
}

String rgba(uint8_t red, uint8_t green, uint8_t blue, uint8_t alpha) {
    const char bytes[] = {char(red), char(green), char(blue), char(alpha)};
    return String(PrStringView(bytes, 4));
}

// The user's temporary directory, where the protocol allows temporary files.
String temporaryFolder() {
    PrRawString temporary = prFileUserDirectory(PrFileUserDirectory_Temporary);
    return String::adopt(temporary);
}

void writeFile(PrStringView path, PrStringView bytes) {
    REQUIRE(prFileWrite(path, prStringBytes(bytes)));
}

bool fileExists(PrStringView path) {
    PrFileInfo info{};
    return prFileInfo(path, &info) && info.kind == PrFileKind_Regular;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("vterm graphics: a terminal without a budget ignores the protocol, and one with it answers a query") {
    Term off(0);
    off.write(command("i=31,s=1,v=1,a=q,t=d,f=24", "\x01\x02\x03"));
    CHECK(off.output().isEmpty());

    Term t;
    t.write(format("{}\x1b[c", command("i=31,s=1,v=1,a=q,t=d,f=24", "\x01\x02\x03")));
    CHECK(t.output() == format("{}\x1b[?62;21;22c", answer("i=31", "OK")));
    // A query stores nothing, so the id names no image.
    t.write(command("a=p,i=31"));
    CHECK(prStringStartsWith(t.output(), "\x1b_Gi=31;ENOENT:"));
    CHECK(t.placements().isEmpty());
}

TEST_CASE("vterm graphics: a feature query answers the keys that the terminal supports for an action") {
    Term t;
    t.write(command("a=q,i=31,e=T,U=1,C=1"));
    CHECK(t.output() == answer("i=31,e=T,U=1,C=1", "OK"));
    t.write(command("a=q,i=32,e=t,f=100,o=z,t=s"));
    CHECK(t.output() == answer("i=32,e=t,f=100,o=z", "OK"));
    // Animation is not supported, so its answer has no `e`.
    t.write(command("a=q,i=33,e=f,r=1"));
    CHECK(t.output() == answer("i=33", "OK"));
}

TEST_CASE("vterm graphics: transmit and put places the image at the cursor and moves the cursor past it") {
    Term t;
    t.write("ab");
    // With cells of 10 by 20 pixels, 25 by 30 pixels take three columns and two rows.
    t.write(command("a=T,f=24,s=25,v=30,i=7", solid(25, 30, 3, 0xff, 0, 0)));
    CHECK(t.output() == answer("i=7", "OK"));
    Array<PrVtermPlacement> placed = t.placements();
    REQUIRE(placed.count() == 1);
    const PrVtermPlacement& p = placed[0];
    CHECK(t.row(p) == 0);
    CHECK(p.column == 2);
    CHECK(p.columns == 3);
    CHECK(p.firstRow == 0);
    CHECK(p.endRow == 2);
    CHECK(p.offset.x == 0);
    CHECK(p.offset.y == 0);
    CHECK(p.extent.x == doctest::Approx(2.5));
    CHECK(p.extent.y == doctest::Approx(1.5));
    CHECK(p.sourceOrigin.x == 0);
    CHECK(p.sourceExtent.x == 25);
    CHECK(p.sourceExtent.y == 30);
    CHECK(p.z == 0);
    CHECK(firstPixel(t, p) == rgba(0xff, 0, 0, 0xff));
    // The cursor moves right by the image's columns, and down by its rows minus one.
    CHECK(t.cursor().x == 5);
    CHECK(t.cursor().y == 1);

    // With C=1, the cursor does not move.
    t.write(command("a=p,i=7,C=1"));
    CHECK(t.output() == answer("i=7", "OK"));
    CHECK(t.cursor().x == 5);
    CHECK(t.cursor().y == 1);
}

TEST_CASE("vterm graphics: chunks join into one image, which shows only once the last arrives, at any split") {
    Term t;
    String data = base64(solid(4, 4, 4, 0, 0xff, 0));
    REQUIRE(data.count() > 40);
    t.write(format("\x1b_Ga=T,f=32,s=4,v=4,i=3,m=1;{}\x1b\\", prStringSlice(data, 0, 18)));
    t.write("text");
    t.write(format("\x1b_Gm=1;{}\x1b\\", prStringSlice(data, 18, 23)));
    CHECK(t.placements().isEmpty());
    CHECK(t.output().isEmpty());
    t.write(format("\x1b_Gm=0;{}\x1b\\", prStringSlice(data, 41, data.count() - 41)));
    CHECK(t.output() == answer("i=3", "OK"));
    Array<PrVtermPlacement> placed = t.placements();
    REQUIRE(placed.count() == 1);
    // The image is placed at the cursor position when the last chunk arrives.
    CHECK(placed[0].column == 4);
    CHECK(firstPixel(t, placed[0]) == rgba(0, 0xff, 0, 0x80));
}

TEST_CASE("vterm graphics: zlib compression and PNG decode, alone and together") {
    Term t;
    t.write(command("a=T,f=24,s=8,v=8,o=z,i=1", zlib(solid(8, 8, 3, 1, 2, 3))));
    CHECK(t.output() == answer("i=1", "OK"));
    t.write(command("a=T,f=100,i=2", png(12, 6, 9, 8, 7)));
    CHECK(t.output() == answer("i=2", "OK"));
    String encoded = png(5, 5, 4, 5, 6);
    t.write(command(format("a=T,f=100,o=z,i=3,S={}", encoded.count()), zlib(encoded)));
    CHECK(t.output() == answer("i=3", "OK"));
    Array<PrVtermPlacement> placed = t.placements();
    REQUIRE(placed.count() == 3);
    CHECK(firstPixel(t, placed[0]) == rgba(1, 2, 3, 0xff));
    CHECK(firstPixel(t, placed[1]) == rgba(9, 8, 7, 0xff));
    CHECK(placed[1].sourceExtent.x == 12);
    CHECK(placed[1].sourceExtent.y == 6);
    CHECK(firstPixel(t, placed[2]) == rgba(4, 5, 6, 0xff));
}

TEST_CASE("vterm graphics: errors answer with their codes, and q quiets the answers") {
    Term t;
    t.write(command("a=p,i=99"));
    CHECK(prStringStartsWith(t.output(), "\x1b_Gi=99;ENOENT:"));
    t.write(command("a=t,i=1,I=2,f=24,s=1,v=1", "abc"));
    CHECK(prStringStartsWith(t.output(), "\x1b_Gi=1,I=2;EINVAL:"));
    t.write(command("a=t,i=1,f=24,s=2,v=2", "abc"));
    CHECK(prStringStartsWith(t.output(), "\x1b_Gi=1;ENODATA:"));
    t.write(command("a=t,i=1,f=100", "not a png"));
    CHECK(prStringStartsWith(t.output(), "\x1b_Gi=1;EBADPNG:"));
    t.write(command("a=t,i=1,f=24", "abc"));
    CHECK(prStringStartsWith(t.output(), "\x1b_Gi=1;EINVAL:"));
    t.write(command("a=t,i=1,f=24,s=1,v=1,o=z", "not zlib"));
    CHECK(prStringStartsWith(t.output(), "\x1b_Gi=1;EINVAL:"));
    t.write(command("a=t,i=1,t=s,f=24,s=1,v=1", "/name"));
    CHECK(prStringStartsWith(t.output(), "\x1b_Gi=1;EINVAL:"));
    t.write(command("a=f,i=1,f=24,s=1,v=1", "abc"));
    CHECK(prStringStartsWith(t.output(), "\x1b_Gi=1;EINVAL:"));
    t.write(command("a=t,i=1,f=7,s=1,v=1", "abc"));
    CHECK(prStringStartsWith(t.output(), "\x1b_Gi=1;EINVAL:"));
    t.write(command("a=t,i=1,s=x,f=24", "abc"));
    CHECK(prStringStartsWith(t.output(), "\x1b_Gi=1;EINVAL:"));

    // q=1 suppresses OK answers, and q=2 suppresses errors too. A command with neither an id nor a number gets no
    // answer.
    t.write(command("a=t,i=5,q=1,f=24,s=1,v=1", "abc"));
    CHECK(t.output().isEmpty());
    t.write(command("a=p,i=99,q=1"));
    CHECK(prStringStartsWith(t.output(), "\x1b_Gi=99;ENOENT:"));
    t.write(command("a=p,i=99,q=2"));
    CHECK(t.output().isEmpty());
    t.write(command("a=T,f=24,s=1,v=1", "abc"));
    CHECK(t.output().isEmpty());
    CHECK(t.placements().count() == 1);
}

TEST_CASE("vterm graphics: an image number is answered with the id that the terminal gave, and names the newest") {
    Term t;
    t.write(command("a=t,I=13,f=24,s=1,v=1", "abc"));
    String first = t.output();
    REQUIRE(prStringStartsWith(first, "\x1b_Gi="));
    REQUIRE(prStringContains(first, ",I=13;OK\x1b\\"));
    t.write(command("a=t,I=13,f=24,s=1,v=1", "def"));
    String second = t.output();
    CHECK(second != first);
    t.write(command("a=p,I=13,p=4"));
    String put = t.output();
    CHECK(prStringContains(put, ",I=13,p=4;OK\x1b\\"));
    Array<PrVtermPlacement> placed = t.placements();
    REQUIRE(placed.count() == 1);
    CHECK(firstPixel(t, placed[0]) == rgba('d', 'e', 'f', 0xff));
}

TEST_CASE("vterm graphics: a placement id names one placement, which a put of the same id moves") {
    Term t;
    t.write(command("a=t,i=4,f=24,s=1,v=1", "abc"));
    t.output();
    t.write(command("a=p,i=4,p=1"));
    CHECK(t.output() == answer("i=4,p=1", "OK"));
    t.write("\x1b[5;6H");
    t.write(command("a=p,i=4,p=1"));
    t.write(command("a=p,i=4,p=2"));
    t.output();
    Array<PrVtermPlacement> placed = t.placements();
    REQUIRE(placed.count() == 2);
    CHECK(t.row(placed[0]) == 4);
    CHECK(placed[0].column == 5);
    // A retransmission of the id deletes the image and its placements.
    t.write(command("a=t,i=4,f=24,s=1,v=1", "abc"));
    CHECK(t.placements().isEmpty());
}

TEST_CASE("vterm graphics: columns and rows scale the image, keeping its aspect, and offsets and a source rectangle") {
    Term t;
    t.write(command("a=t,i=1,f=24,s=40,v=20", solid(40, 20, 3, 0, 0, 0xff)));
    t.output();
    // Columns and rows: the image is letterboxed into 4 columns by 3 rows (40 by 60 pixels). It shows at 40 by 20,
    // centered vertically.
    t.write(command("a=p,i=1,c=4,r=3,C=1"));
    // Columns alone: 6 columns are 60 pixels, so the image is 30 pixels high and takes two rows.
    t.write(command("a=p,i=1,c=6,C=1"));
    // Rows alone: 2 rows are 40 pixels, so the image is 80 pixels wide and takes 8 columns.
    t.write(command("a=p,i=1,r=2,C=1"));
    // An offset within the cell, and a source rectangle clipped to the image.
    t.write(command("a=p,i=1,X=5,Y=10,x=30,y=5,w=50,h=10,C=1"));
    t.output();
    Array<PrVtermPlacement> placed = t.placements();
    REQUIRE(placed.count() == 4);
    CHECK(placed[0].columns == 4);
    CHECK(placed[0].endRow == 3);
    CHECK(placed[0].extent.x == doctest::Approx(4));
    CHECK(placed[0].extent.y == doctest::Approx(1));
    CHECK(placed[0].offset.y == doctest::Approx(1));
    CHECK(placed[1].columns == 6);
    CHECK(placed[1].endRow == 2);
    CHECK(placed[1].extent.y == doctest::Approx(1.5));
    CHECK(placed[2].columns == 8);
    CHECK(placed[2].endRow == 2);
    CHECK(placed[2].extent.x == doctest::Approx(8));
    CHECK(placed[3].offset.x == doctest::Approx(0.5));
    CHECK(placed[3].offset.y == doctest::Approx(0.5));
    CHECK(placed[3].sourceOrigin.x == 30);
    CHECK(placed[3].sourceOrigin.y == 5);
    CHECK(placed[3].sourceExtent.x == 10);
    CHECK(placed[3].sourceExtent.y == 10);
    // A 5-pixel offset plus 10 pixels of width takes two columns. A 10-pixel offset plus 10 pixels of height fits in
    // one row.
    CHECK(placed[3].columns == 2);
    CHECK(placed[3].endRow == 1);
}

TEST_CASE("vterm graphics: placements draw by z-index, then by image") {
    Term t;
    t.write(format("{}{}", command("a=t,i=2,f=24,s=1,v=1", "abc"), command("a=t,i=1,f=24,s=1,v=1", "abc")));
    t.write(format("{}{}{}{}", command("a=p,i=2,z=5"), command("a=p,i=1,z=-3"), command("a=p,i=2,z=-3"),
        command("a=p,i=1,z=-1073741825")));
    t.output();
    Array<PrVtermPlacement> placed = t.placements();
    REQUIRE(placed.count() == 4);
    CHECK(placed[0].z == -1073741825);
    CHECK(placed[1].z == -3);
    CHECK(placed[2].z == -3);
    CHECK(placed[3].z == 5);
    PrVtermImage one{};
    PrVtermImage two{};
    REQUIRE(prVtermImage(t.vterm, placed[0].image, &one));
    CHECK(placed[1].image == placed[0].image);
    CHECK(placed[2].image != placed[1].image);
    REQUIRE(prVtermImage(t.vterm, placed[2].image, &two));
}

TEST_CASE("vterm graphics: a placement at the bottom scrolls the screen, and scrolls with the text into the scrollback") {
    Term t;
    t.write("\x1b[10;1Hbottom");
    // An image three rows tall, placed on the last row, scrolls the screen up by two rows.
    t.write(command("a=T,f=24,s=10,v=60", solid(10, 60, 3, 1, 1, 1)));
    Array<PrVtermPlacement> placed = t.onScreen();
    REQUIRE(placed.count() == 1);
    CHECK(t.row(placed[0]) == 7);
    CHECK(t.text(prVtermScrollbackCount(t.vterm) + 7) == "bottom");
    CHECK(t.cursor().y == 9);
    // As the program writes lines, the placement moves up with its line into the scrollback.
    for (uint32_t i = 0; i < 11; ++i)
        t.write("\r\n");
    Array<PrVtermPlacement> all = t.placements();
    REQUIRE(all.count() == 1);
    CHECK(t.text(uint32_t(all[0].line)) == "bottom");
    CHECK(t.row(all[0]) < 0);
    CHECK(t.onScreen().isEmpty());
    // The placement is removed when the scrollback drops the last of its lines.
    t.write("\n\n\n\n\n\n\n\n\n\n");
    CHECK(t.placements().isEmpty());
}

TEST_CASE("vterm graphics: a scroll region moves the placements in it, cuts them at its edge, and leaves the rest") {
    Term t;
    t.write("\x1b[3;7r");
    // Inside the region, on rows 4 and 5.
    t.write(format("\x1b[5;1H{}", command("a=T,f=24,s=10,v=40,C=1", solid(10, 40, 3, 1, 1, 1))));
    // Across the bottom of the region, on rows 6 to 8.
    t.write(format("\x1b[7;5H{}", command("a=T,f=24,s=10,v=60,C=1", solid(10, 60, 3, 1, 1, 1))));
    // Below the region.
    t.write(format("\x1b[9;9H{}", command("a=T,f=24,s=10,v=20,C=1", solid(10, 20, 3, 1, 1, 1))));
    t.write("\x1b[2S");
    Array<PrVtermPlacement> placed = t.onScreen();
    REQUIRE(placed.count() == 3);
    CHECK(placed[0].column == 0);
    CHECK(t.row(placed[0]) == 2);
    CHECK(placed[0].firstRow == 0);
    CHECK(placed[0].endRow == 2);
    CHECK(t.row(placed[1]) == 6);
    CHECK(t.row(placed[2]) == 8);
    // Scrolling one more line cuts off the top row of the first placement. Scrolling another removes it.
    t.write("\x1b[S");
    placed = t.onScreen();
    REQUIRE(placed.count() == 3);
    CHECK(t.row(placed[0]) == 1);
    CHECK(placed[0].firstRow == 1);
    t.write("\x1b[S");
    CHECK(t.onScreen().count() == 2);
    // If lines are inserted, the region scrolls down and cuts placements at its bottom. Nothing enters the scrollback.
    t.write(format("\x1b[r\x1b[1;1H{}", command("a=T,f=24,s=10,v=40,C=1", solid(10, 40, 3, 1, 1, 1))));
    t.write("\x1b[3;7r\x1b[3;1H\x1b[5L");
    CHECK(prVtermScrollbackCount(t.vterm) == 0);
}

TEST_CASE("vterm graphics: left and right margins move only the placements that lie in their columns") {
    Term t;
    t.write("\x1b[?69h\x1b[3;7r\x1b[2;10s");
    t.write(format("\x1b[5;3H{}", command("a=T,f=24,s=10,v=20,C=1", solid(10, 20, 3, 1, 1, 1))));
    t.write(format("\x1b[5;15H{}", command("a=T,f=24,s=10,v=20,C=1", solid(10, 20, 3, 1, 1, 1))));
    t.write("\x1b[S");
    Array<PrVtermPlacement> placed = t.onScreen();
    REQUIRE(placed.count() == 2);
    const PrVtermPlacement& inside = placed[0].column == 2 ? placed[0] : placed[1];
    const PrVtermPlacement& outside = placed[0].column == 2 ? placed[1] : placed[0];
    CHECK(t.row(inside) == 3);
    CHECK(t.row(outside) == 4);
    CHECK(outside.column == 14);
}

TEST_CASE("vterm graphics: the erase of the screen removes placements, and other erases leave them") {
    Term t;
    t.write(format("\x1b[2;2H{}", command("a=T,i=1,f=24,s=10,v=20", solid(10, 20, 3, 1, 1, 1))));
    t.write("\x1b[2K\x1b[J\x1b[1J\x1b[X");
    CHECK(t.placements().count() == 1);
    t.write("\x1b[2J");
    CHECK(t.placements().isEmpty());
    // The image itself remains, and a put can place it again.
    t.output();
    t.write(command("a=p,i=1"));
    CHECK(t.output() == answer("i=1", "OK"));
    // ED 3 removes the placements in the scrollback.
    t.write("\n\n\n\n\n\n\n\n\n\n\n\n");
    CHECK(t.placements().count() == 1);
    t.write("\x1b[3J");
    CHECK(t.placements().isEmpty());
}

TEST_CASE("vterm graphics: the alternate screen has images of its own, which showing it with a clear empties") {
    Term t;
    t.write(command("a=T,i=1,f=24,s=10,v=20", solid(10, 20, 3, 1, 1, 1)));
    t.output();
    t.write("\x1b[?1049h");
    CHECK(t.onScreen().isEmpty());
    t.write(command("a=p,i=1"));
    CHECK(prStringStartsWith(t.output(), "\x1b_Gi=1;ENOENT:"));
    t.write(command("a=T,i=1,f=24,s=10,v=20", solid(10, 20, 3, 2, 2, 2)));
    Array<PrVtermPlacement> alternate = t.onScreen();
    REQUIRE(alternate.count() == 1);
    CHECK(firstPixel(t, alternate[0]) == rgba(2, 2, 2, 0xff));
    t.write("\x1b[?1049l");
    Array<PrVtermPlacement> primary = t.onScreen();
    REQUIRE(primary.count() == 1);
    CHECK(firstPixel(t, primary[0]) == rgba(1, 1, 1, 0xff));
    t.write("\x1b[?1049h");
    CHECK(t.onScreen().isEmpty());
    t.output();
    t.write(command("a=p,i=1"));
    CHECK(prStringStartsWith(t.output(), "\x1b_Gi=1;ENOENT:"));
}

TEST_CASE("vterm graphics: a resize keeps a placement on the line that its line's first cell rewraps to") {
    Term t;
    t.write("0123456789abcdefghijKLM\r\nnext\r\n");
    t.write(command("a=T,f=24,s=10,v=20,C=1", solid(10, 20, 3, 1, 1, 1)));
    t.write(format("\x1b[1;1H\x1b[3C{}", command("a=T,f=24,s=10,v=20,C=1", solid(10, 20, 3, 1, 1, 1))));
    // At width 10, the first line rewraps to three lines. The placement on the line after "next" moves down with it.
    auto check = [&](PrStringView first) {
        Array<PrVtermPlacement> placed = t.placements();
        REQUIRE(placed.count() == 2);
        const PrVtermPlacement& top = placed[0].column == 3 ? placed[0] : placed[1];
        const PrVtermPlacement& below = placed[0].column == 3 ? placed[1] : placed[0];
        CHECK(top.column == 3);
        CHECK(t.text(uint32_t(top.line)) == first);
        CHECK(t.text(uint32_t(below.line) - 1) == "next");
    };
    prVtermResize(t.vterm, PrUVec2{10, 10});
    check("0123456789");
    prVtermResize(t.vterm, PrUVec2{20, 5});
    check("0123456789abcdefghij");
}

TEST_CASE("vterm graphics: a full reset removes every image") {
    Term t;
    t.write(format("{}\x1b" "c", command("a=T,i=1,f=24,s=1,v=1", "abc")));
    CHECK(t.placements().isEmpty());
    t.output();
    t.write(command("a=p,i=1"));
    CHECK(prStringStartsWith(t.output(), "\x1b_Gi=1;ENOENT:"));
}

TEST_CASE("vterm graphics: deletion by id, number, cell, cursor, column, row, z-index and range") {
    Term t;
    auto put = [&](PrStringView keys) { t.write(command(format("a=p,C=1,{}", keys))); };
    auto count = [&] { return t.placements().count(); };
    t.write(command("a=t,i=1,f=24,s=10,v=20,q=1", solid(10, 20, 3, 1, 1, 1)));
    t.write(command("a=t,i=2,f=24,s=10,v=20,q=1", solid(10, 20, 3, 2, 2, 2)));

    // By id: one placement, then every placement of the image. Lowercase keeps the image, and uppercase frees it.
    put("i=1,p=1");
    put("i=1,p=2");
    t.write(command("a=d,d=i,i=1,p=1"));
    CHECK(count() == 1);
    t.write(command("a=d,d=i,i=1"));
    CHECK(count() == 0);
    t.output();
    put("i=1");
    CHECK(t.output() == answer("i=1", "OK"));
    t.write(command("a=d,d=I,i=1"));
    put("i=1");
    CHECK(prStringStartsWith(t.output(), "\x1b_Gi=1;ENOENT:"));

    // By cell, by cell and z-index, at the cursor, by column, by row and by z-index. Cells, columns and rows count from
    // 1.
    t.write("\x1b[3;4H");
    put("i=2,z=1");
    put("i=2,z=2");
    t.write("\x1b[1;1H");
    t.write(command("a=d,d=q,x=4,y=3,z=2"));
    CHECK(count() == 1);
    t.write(command("a=d,d=p,x=4,y=3"));
    CHECK(count() == 0);
    t.write("\x1b[3;4H");
    put("i=2");
    t.write(command("a=d,d=c"));
    CHECK(count() == 0);
    put("i=2");
    t.write(command("a=d,d=x,x=5"));
    CHECK(count() == 1);
    t.write(command("a=d,d=x,x=4"));
    CHECK(count() == 0);
    put("i=2");
    t.write(command("a=d,d=y,y=3"));
    CHECK(count() == 0);
    put("i=2,z=7");
    t.write(command("a=d,d=z,z=7"));
    CHECK(count() == 0);

    // With no target, a delete removes every placement on the screen.
    put("i=2");
    put("i=2");
    t.write(command("a=d"));
    CHECK(count() == 0);

    // By number, which names the newest image with that number, and by a range of ids.
    t.write(command("a=t,I=9,f=24,s=1,v=1,q=2", "abc"));
    put("I=9");
    t.write(command("a=d,d=N,I=9"));
    CHECK(count() == 0);
    t.output();
    put("I=9");
    CHECK(prStringContains(t.output(), "ENOENT"));
    put("i=2");
    t.write(command("a=d,d=R,x=1,y=2"));
    CHECK(count() == 0);
    t.output();
    put("i=2");
    CHECK(prStringStartsWith(t.output(), "\x1b_Gi=2;ENOENT:"));
}

TEST_CASE("vterm graphics: a delete aborts a transmission in chunks") {
    Term t;
    String data = base64(solid(2, 2, 3, 1, 1, 1));
    t.write(format("\x1b_Ga=T,f=24,s=2,v=2,i=1,m=1;{}\x1b\\", prStringSlice(data, 0, 8)));
    t.write(command("a=d"));
    t.write(format("\x1b_Gm=0;{}\x1b\\", prStringSlice(data, 8, data.count() - 8)));
    CHECK(t.placements().isEmpty());
}

TEST_CASE("vterm graphics: images past the budget free those without placements first, then the oldest") {
    // Room for three 10 by 10 images in RGBA.
    Term t(3 * 400);
    auto send = [&](uint32_t id, PrStringView action) {
        t.write(command(format("a={},i={},f=24,s=10,v=10,C=1,q=1", action, id), solid(10, 10, 3, uint8_t(id), 0, 0)));
    };
    auto exists = [&](uint32_t id) {
        t.output();
        t.write(command(format("a=p,C=1,q=1,i={}", id)));
        return t.output().isEmpty();
    };
    send(1, "T");
    send(2, "t");
    send(3, "T");
    send(4, "T");
    // The image without a placement is freed first.
    CHECK(t.placements().count() == 3);
    CHECK_FALSE(exists(2));
    send(5, "T");
    // Then the oldest image is freed.
    CHECK_FALSE(exists(1));
    CHECK(exists(5));
    // An image larger than the budget is refused.
    t.write(command("a=t,i=9,f=24,s=40,v=40", solid(40, 40, 3, 0, 0, 0)));
    CHECK(prStringStartsWith(t.output(), "\x1b_Gi=9;EFBIG:"));
}

TEST_CASE("vterm graphics: the placeholders of a virtual placement show its image in their cells") {
    Term t;
    // The 20 by 20 image goes into 2 columns by 2 rows, which are 20 by 40 pixels. It fills the width and is centered
    // in the height.
    t.write(command("a=T,U=1,i=42,c=2,r=2,f=24,s=20,v=20,q=2", solid(20, 20, 3, 5, 5, 5)));
    CHECK(t.placements().isEmpty());
    CHECK(t.cursor().x == 0);
    // Diacritics give each placeholder's row and column. Row 0 gives both in every cell. Row 1 gives only the row, and
    // its columns are inherited.
    t.write("\x1b[38;5;42m\xf4\x8e\xbb\xae\xcc\x85\xcc\x85\xf4\x8e\xbb\xae\xcc\x85\xcc\x8d\x1b[39m\r\n");
    t.write("\x1b[38;5;42m\xf4\x8e\xbb\xae\xcc\x8d\xf4\x8e\xbb\xae\x1b[39m\r\n");
    Array<PrVtermPlacement> placed = t.placements();
    REQUIRE(placed.count() == 2);
    for (uint32_t i = 0; i < 2; ++i) {
        CHECK(t.row(placed[i]) == int64_t(i));
        CHECK(placed[i].column == 0);
        CHECK(placed[i].columns == 2);
        CHECK(placed[i].firstRow == 0);
        CHECK(placed[i].endRow == 1);
        CHECK(placed[i].extent.x == doctest::Approx(2));
        CHECK(placed[i].extent.y == doctest::Approx(1));
        CHECK(placed[i].offset.x == doctest::Approx(0));
        CHECK(placed[i].offset.y == doctest::Approx(0.5 - double(i)));
    }
    // The placeholders are text, so an erase removes them as it removes text.
    t.write("\x1b[2J");
    CHECK(t.placements().isEmpty());
    // An id in 24-bit color, with its most significant byte in a third diacritic.
    t.write(command("a=T,U=1,i=33554474,c=1,r=1,f=24,s=10,v=20,q=2", solid(10, 20, 3, 6, 6, 6)));
    t.write("\x1b[1;1H\x1b[38;2;0;0;42m\xf4\x8e\xbb\xae\xcc\x85\xcc\x85\xcc\x8e");
    placed = t.placements();
    REQUIRE(placed.count() == 1);
    CHECK(firstPixel(t, placed[0]) == rgba(6, 6, 6, 0xff));
    // Deleting the image's virtual placements leaves its placeholders with nothing to show.
    t.write(command("a=d,d=i,i=33554474"));
    CHECK(t.placements().isEmpty());
}

TEST_CASE("vterm graphics: XTWINOPS reports the cell's pixel extent and the screen's") {
    Term t;
    t.write("\x1b[16t\x1b[14t");
    CHECK(t.output() == "\x1b[6;20;10t\x1b[4;200;200t");
    prVtermSetCellPixelExtent(t.vterm, PrUVec2{7, 15});
    t.write("\x1b[16t");
    CHECK(t.output() == "\x1b[6;15;7t");
}

TEST_CASE("vterm graphics: files are read only where the caller allows, and temporary files are deleted after") {
    String folder = temporaryFolder();
    // The protocol takes temporary files only from the system's temporary folder, where others' are too.
    String path = format("{}/prax-vterm-tty-graphics-protocol-test-{}.rgb", folder, harness::runTag());
    String pixels = solid(2, 2, 3, 7, 8, 9);
    writeFile(path, format("xx{}", pixels));

    Term remote;
    remote.write(command("a=T,t=f,i=1,f=24,s=2,v=2", path));
    CHECK(remote.output() == answer("i=1", "EBADF:Failed to read image file"));

    Term t(kPrVtermImageBytesDefault, PrVtermImageFlag_Files);
    t.write(command("a=T,t=f,i=1,f=24,s=2,v=2,O=2,S=12", path));
    CHECK(t.output() == answer("i=1", "OK"));
    REQUIRE(t.placements().count() == 1);
    CHECK(firstPixel(t, t.placements()[0]) == rgba(7, 8, 9, 0xff));
    CHECK(fileExists(path));
    // A missing file, a range past the end of the file, a relative path, a directory, a device and a path on another
    // host all answer the same error.
    String missing = format("{}/prax-missing-file", folder);
    const PrStringView bads[] = {missing, path, "relative/file", folder, "/dev/zero", "//localhost/prax/missing-file"};
    for (PrStringView bad : bads) {
        PrStringView range = bad == path.view() ? ",O=8,S=12" : "";
        t.write(command(format("a=t,t=f,i=2,f=24,s=2,v=2{}", range), bad));
        CHECK(t.output() == answer("i=2", "EBADF:Failed to read image file"));
    }
    // A temporary file is deleted after it is read.
    t.write(command("a=t,t=t,i=3,f=24,s=2,v=2,O=2", path));
    CHECK(t.output() == answer("i=3", "OK"));
    CHECK_FALSE(fileExists(path));
    // A temporary file is neither read nor deleted if its path lacks the protocol's name or it is outside a temporary
    // directory.
    String unnamed = format("{}/prax-vterm-unnamed.rgb", folder);
    writeFile(unnamed, pixels);
    t.write(command("a=t,t=t,i=4,f=24,s=2,v=2", unnamed));
    CHECK(t.output() == answer("i=4", "EBADF:Failed to read image file"));
    CHECK(fileExists(unnamed));
    prFileRemove(unnamed);
}
