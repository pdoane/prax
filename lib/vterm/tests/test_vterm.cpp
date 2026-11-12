#include "pr/array.h"
#include "pr/string.h"
#include "pr/string_ops.h"
#include "pr/vterm.h"

#include "tools/harness/test.h"

extern "C" int prTestVtermCApi(void);

namespace pr_impl {

using namespace pr;

namespace {

// A terminal under test, with helpers that read what it shows.
struct Term {
    PrVterm* vterm;

    explicit Term(uint32_t columns, uint32_t rows, uint32_t scrollback = 100, PrVtermChecksumFlags checksum = 0) {
        PrVtermDesc desc{.extent = {columns, rows}, .scrollback = scrollback, .checksum = checksum};
        vterm = prVtermCreate(&desc);
    }
    ~Term() { prVtermDestroy(vterm); }
    Term(const Term&) = delete;
    Term& operator=(const Term&) = delete;

    void write(PrStringView text) {
        prVtermWrite(vterm, prStringBytes(text));
    }

    // The text of line `index`, without trailing spaces. Lines count from the oldest line of the scrollback.
    String line(uint32_t index) const {
        PrVtermLine line = prVtermLine(vterm, index);
        PrRawString text = prRawStringNull();
        prVtermLineAppendText(vterm, index, 0, line.cellCount, &text);
        String out(prStringTrimEndByte(prRawStringView(&text), ' '));
        prRawStringReset(&text);
        return out;
    }

    uint32_t scrollback() const { return prVtermScrollbackCount(vterm); }

    // The text of screen row `row`.
    String row(uint32_t row) const { return line(scrollback() + row); }

    // The screen's rows, each followed by a line break, up to the last row that is not empty.
    String screen() const {
        Array<String> rows;
        for (uint32_t i = 0; i < prVtermExtent(vterm).y; ++i)
            rows.push(row(i));
        while (!rows.isEmpty() && rows.last().isEmpty())
            rows.pop();
        String out;
        for (PrStringView text : rows)
            formatTo(out, "{}\n", text);
        return out;
    }

    // The scrollback and the screen, in the format of `screen`.
    String all() const {
        String out;
        for (uint32_t i = 0; i < scrollback(); ++i)
            formatTo(out, "{}\n", line(i));
        out.append(screen());
        return out;
    }

    const PrVtermCell& cell(uint32_t column, uint32_t row) const {
        return prVtermLine(vterm, scrollback() + row).cells[column];
    }

    PrUVec2 cursor() const { return prVtermCursor(vterm).position; }

    // Returns and consumes the output queued for the program.
    String output() {
        PrByteView bytes = prVtermOutput(vterm);
        String out(prStringFromBytes(bytes));
        prVtermOutputConsume(vterm, bytes.size);
        return out;
    }
};

bool cursorAt(const Term& term, uint32_t column, uint32_t row) {
    return term.cursor().x == column && term.cursor().y == row;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("vterm: the public headers compile as C") {
    CHECK(prTestVtermCApi() == 0);
}

TEST_CASE("vterm: text prints at the cursor, and a line feed and a carriage return move it") {
    Term term(10, 3);
    term.write("ab\r\ncd");
    CHECK(term.screen() == "ab\ncd\n");
    CHECK(cursorAt(term, 2, 1));
    term.write("\rX");
    CHECK(term.row(1) == "Xd");
}

TEST_CASE("vterm: text past the last column wraps, and the line that it left is marked wrapped") {
    Term term(4, 3);
    term.write("abcd");
    CHECK(cursorAt(term, 3, 0));
    CHECK(term.row(1).isEmpty());
    term.write("e");
    CHECK(term.screen() == "abcd\ne\n");
    CHECK((prVtermLine(term.vterm, 0).flags & PrVtermLineFlag_Wrapped) != 0);
    CHECK((prVtermLine(term.vterm, 1).flags & PrVtermLineFlag_Wrapped) == 0);

    // Without autowrap, the last column is written over.
    term.write("\x1b[?7l\x1b[3;1Hwxyz!");
    CHECK(term.row(2) == "wxy!");
}

TEST_CASE("vterm: lines that scroll off the top enter the scrollback, which drops its oldest past its limit") {
    Term term(4, 2, 3);
    term.write("1\r\n2\r\n3\r\n4");
    CHECK(term.scrollback() == 2);
    CHECK(term.all() == "1\n2\n3\n4\n");
    term.write("\r\n5\r\n6");
    CHECK(term.scrollback() == 3);
    CHECK(prVtermScrollbackDropped(term.vterm) == 1);
    CHECK(term.all() == "2\n3\n4\n5\n6\n");

    // The line memory holds at least the cells of every line. Its size stops changing once the scrollback is full.
    size_t size = prVtermLinesMemorySize(term.vterm);
    CHECK(size >= sizeof(PrVtermCell) * 4 * (3 + 2 + 2));
    term.write("\r\n7\r\n8");
    CHECK(prVtermLinesMemorySize(term.vterm) == size);
}

TEST_CASE("vterm: a scroll region scrolls alone, and its lines enter the scrollback only at the top") {
    Term term(4, 4);
    term.write("a\r\nb\r\nc\r\nd");
    term.write("\x1b[2;3r\x1b[3;1H\nX");
    CHECK(term.screen() == "a\nc\nX\nd\n");
    CHECK(term.scrollback() == 0);

    // A region at the top of the screen sends its lines to the scrollback. The rows below the region do not move.
    term.write("\x1b[1;2r\x1b[2;1H\nY");
    CHECK(term.scrollback() == 1);
    CHECK(term.all() == "a\nc\nY\nX\nd\n");
}

TEST_CASE("vterm: cursor movement, erasing and editing in a line") {
    Term term(8, 3);
    term.write("abcdefgh\x1b[1;3H\x1b[K");
    CHECK(term.row(0) == "ab");
    term.write("\x1b[2;1Habcdefgh\x1b[2;4H\x1b[1K");
    CHECK(term.row(1) == "    efgh");
    term.write("\x1b[3;1Habcdefgh\x1b[3;2H\x1b[2P");
    CHECK(term.row(2) == "adefgh");
    term.write("\x1b[2@");
    CHECK(term.row(2) == "a  defgh");
    term.write("\x1b[3X");
    CHECK(term.row(2) == "a   efgh");
    term.write("\x1b[2J");
    CHECK(term.screen().isEmpty());
    term.write("\x1b[5;9H");
    CHECK(cursorAt(term, 7, 2));
    term.write("\x1b[2A\x1b[3D");
    CHECK(cursorAt(term, 4, 0));
}

TEST_CASE("vterm: lines insert and delete within the scroll region") {
    Term term(4, 4);
    term.write("a\r\nb\r\nc\r\nd\x1b[2;1H\x1b[L");
    CHECK(term.screen() == "a\n\nb\nc\n");
    term.write("\x1b[2M");
    CHECK(term.screen() == "a\nc\n");
    CHECK(term.scrollback() == 0);
}

TEST_CASE("vterm: SGR sets attributes and colors in both forms of extended color") {
    Term term(10, 1);
    term.write("\x1b[1;2;3;4:3;9mA\x1b[0;38;5;196;48;2;1;2;3mB\x1b[38:2::10:20:30;58:5:4;21mC\x1b[22;24;39mD");
    const PrVtermCell& a = term.cell(0, 0);
    CHECK(a.flags == (PrVtermCellFlag_Bold | PrVtermCellFlag_Faint | PrVtermCellFlag_Italic |
        PrVtermCellFlag_Strikethrough));
    CHECK(a.underline == PrVtermUnderline_Curly);
    const PrVtermCell& b = term.cell(1, 0);
    CHECK(b.flags == 0);
    CHECK(b.foreground.kind == PrVtermColorKind_Palette);
    CHECK(prVtermColorIndex(b.foreground) == 196);
    CHECK(b.background.kind == PrVtermColorKind_Rgb);
    CHECK(b.background.blue == 3);
    const PrVtermCell& c = term.cell(2, 0);
    CHECK(c.foreground.kind == PrVtermColorKind_Rgb);
    CHECK(c.foreground.red == 10);
    CHECK(c.foreground.green == 20);
    CHECK(c.foreground.blue == 30);
    CHECK(c.underlineColor.kind == PrVtermColorKind_Palette);
    CHECK(prVtermColorIndex(c.underlineColor) == 4);
    CHECK(c.underline == PrVtermUnderline_Double);
    const PrVtermCell& d = term.cell(3, 0);
    CHECK(d.foreground.kind == PrVtermColorKind_Default);
    CHECK(d.underline == PrVtermUnderline_None);
    CHECK(d.background.kind == PrVtermColorKind_Rgb);

    // Erasing fills with the pen's background.
    term.write("\x1b[44m\x1b[K");
    CHECK(term.cell(5, 0).background.kind == PrVtermColorKind_Palette);
    CHECK(prVtermColorIndex(term.cell(5, 0).background) == 4);
}

TEST_CASE("vterm: a wide character takes two cells, and wraps whole at the end of a line") {
    Term term(5, 2);
    term.write("ab\xe4\xb8\xad");
    CHECK(term.row(0) == "ab\xe4\xb8\xad");
    CHECK((term.cell(2, 0).flags & PrVtermCellFlag_Wide) != 0);
    CHECK((term.cell(3, 0).flags & PrVtermCellFlag_WideTail) != 0);
    CHECK(cursorAt(term, 4, 0));
    term.write("\xe5\x9b\xbd");
    CHECK((term.cell(4, 0).flags & PrVtermCellFlag_WideSpacer) != 0);
    CHECK(term.row(1) == "\xe5\x9b\xbd");

    // Writing over half of a wide character empties the other half.
    term.write("\x1b[1;4Hx");
    CHECK(term.row(0) == "ab x");
}

TEST_CASE("vterm: a run of text splits the wide characters at its ends, and inserts and wraps as characters do") {
    Term term(8, 4);
    term.write("\xe4\xb8\xad" "a" "\xe5\x9b\xbd" "\x1b[1;2Hxyz");
    CHECK(term.row(0) == " xyz");
    CHECK((term.cell(0, 0).flags & PrVtermCellFlag_WidePlacement) == 0);
    CHECK((term.cell(4, 0).flags & PrVtermCellFlag_WidePlacement) == 0);

    term.write("\x1b[2;1Habcd\x1b[2;1H\x1b[4hXY\x1b[4l");
    CHECK(term.row(1) == "XYabcd");

    // With left and right margins set, a run wraps from the right margin to the left margin.
    term.write("\x1b[?69h\x1b[3;5s\x1b[3;3Hpqrstu");
    CHECK(term.row(2) == "  pqr");
    CHECK(term.row(3) == "  stu");
    CHECK(cursorAt(term, 4, 3));
}

TEST_CASE("vterm: a grapheme cluster takes one cell, and an emoji presentation makes it wide") {
    Term term(12, 1);
    // Four clusters between bars: e with a combining acute accent, a family of three joined by ZWJ, a heart with the
    // emoji presentation selector, and a flag.
    term.write("e\xcc\x81|\xf0\x9f\x91\xa8\xe2\x80\x8d\xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x91\xa7|"
        "\xe2\x9d\xa4\xef\xb8\x8f|\xf0\x9f\x87\xaf\xf0\x9f\x87\xb5|");
    CHECK(term.cell(0, 0).codePoint == 'e');
    CHECK(term.cell(0, 0).clusterCount == 1);
    CHECK(term.cell(1, 0).codePoint == '|');
    CHECK(term.cell(2, 0).codePoint == 0x1f468);
    CHECK(term.cell(2, 0).clusterCount == 4);
    CHECK((term.cell(2, 0).flags & PrVtermCellFlag_Wide) != 0);
    CHECK(term.cell(4, 0).codePoint == '|');
    CHECK(term.cell(5, 0).codePoint == 0x2764);
    CHECK((term.cell(5, 0).flags & PrVtermCellFlag_Wide) != 0);
    CHECK(term.cell(7, 0).codePoint == '|');
    CHECK(term.cell(8, 0).codePoint == 0x1f1ef);
    CHECK((term.cell(8, 0).flags & PrVtermCellFlag_Wide) != 0);
    CHECK(term.row(0) == "e\xcc\x81|\xf0\x9f\x91\xa8\xe2\x80\x8d\xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x91\xa7|"
        "\xe2\x9d\xa4\xef\xb8\x8f|\xf0\x9f\x87\xaf\xf0\x9f\x87\xb5|");
}

TEST_CASE("vterm: the DEC Special Graphics set draws lines") {
    Term term(6, 1);
    term.write("\x1b(0lqk\x1b(Bq");
    CHECK(term.row(0) == "\xe2\x94\x8c\xe2\x94\x80\xe2\x94\x90q");
}

TEST_CASE("vterm: the alternate screen keeps the primary screen and its cursor, and has no scrollback") {
    Term term(6, 2);
    term.write("one\r\ntwo");
    term.write("\x1b[?1049h");
    CHECK((prVtermModes(term.vterm) & PrVtermModeFlag_AlternateScreen) != 0);
    CHECK(term.screen().isEmpty());
    term.write("\x1b[1;1Halt\r\n\r\n\r\nx");
    CHECK(term.scrollback() == 0);
    term.write("\x1b[?1049l");
    CHECK(term.screen() == "one\ntwo\n");
    CHECK(cursorAt(term, 3, 1));
}

TEST_CASE("vterm: queries are answered in the output") {
    Term term(10, 5);
    term.write("\x1b[c");
    CHECK(term.output() == "\x1b[?62;21;22c");
    term.write("\x1b[3;4H\x1b[6n");
    CHECK(term.output() == "\x1b[3;4R");
    term.write("\x1b[?2004h\x1b[?2004$p\x1b[?2027$p\x1b[?9999$p");
    CHECK(term.output() == "\x1b[?2004;1$y\x1b[?2027;3$y\x1b[?9999;0$y");
    term.write("\x1b]11;?\x07");
    CHECK(term.output() == "\x1b]11;rgb:0000/0000/0000\x07");
    term.write("\x1b]4;1;#ff8000\x1b\\\x1b]4;1;?\x1b\\");
    CHECK(term.output() == "\x1b]4;1;rgb:ffff/8080/0000\x1b\\");
    term.write("\x1b[18t");
    CHECK(term.output() == "\x1b[8;5;10t");
}

TEST_CASE("vterm: a rectangle's checksum is DEC's by default: negated, of 8-bit codes and attributes") {
    Term term(10, 3);
    term.write("A\x1b[1mB\x1b[m");
    term.write("\x1b[1;0;1;1;1;2*y");
    CHECK(term.output() == "\x1bP1!~FEFD\x1b\\");

    // The rectangle defaults to the whole screen. Empty cells add nothing to the sum.
    term.write("\x1b[2*y\x1b[3;0;3;1;3;10*y");
    CHECK(term.output() == "\x1bP2!~FEFD\x1b\\\x1bP3!~0000\x1b\\");

    // With Unwritten, an empty cell reads as a space. Without Untrimmed, only the rectangle's first cell counts it.
    term.write("\x1b[8#y\x1b[4;0;3;1;3;10*y\x1b[9#y\x1b[5;0;3;1;3;10*y");
    CHECK(term.output() == "\x1bP4!~FFE0\x1b\\\x1bP5!~0020\x1b\\");

    // In 8-bit codes, each half of a wide character sums as ESC. A DEC line-drawing character sums as the code that
    // selects it.
    term.write("\x1b[2;1H\xe4\xb8\xad\xc3\xa9\x1b(0q\x1b(B\x1b[7#y\x1b[6;0;2;1;2;4*y");
    CHECK(term.output() == "\x1bP6!~0190\x1b\\");
}

TEST_CASE("vterm: a checksum's flags sum code points, and a full reset restores the terminal's own flags") {
    Term term(10, 3, 100, PrVtermChecksumFlag_Characters);
    term.write("A\x1b[1mB\x1b[m\x1b[1;0;1;1;1;3*y");
    CHECK(term.output() == "\x1bP1!~00A3\x1b\\");
    term.write("\x1b[2;1H\xe4\xb8\xad\xc3\xa9\x1b[2;0;2;1;2;3*y");
    CHECK(term.output() == "\x1bP2!~4F16\x1b\\");

    // In origin mode, rows count from the top of the scroll region and are clamped to the region.
    term.write("\x1b[2;3r\x1b[?6h\x1b[3;0;1;1;1;1*y\x1b[4;0;1;1;9;1*y");
    CHECK(term.output() == "\x1bP3!~4E2D\x1b\\\x1bP4!~4E4D\x1b\\");
    // Columns count from the left margin and are clamped to the margins.
    term.write("\x1b[?69h\x1b[3;4s\x1b[5;0;1;1;1;1*y\x1b[6;0;1;1;1;9*y");
    CHECK(term.output() == "\x1bP5!~00E9\x1b\\\x1bP6!~0109\x1b\\");

    term.write("\x1b[0#y\x1b" "c\x1b[7*y");
    CHECK(term.output() == "\x1bP7!~03C0\x1b\\");
}

TEST_CASE("vterm: the keyboard protocol's modes stack, and a query reports the top") {
    Term term(4, 1);
    term.write("\x1b[?u");
    CHECK(term.output() == "\x1b[?0u");
    term.write("\x1b[>1u\x1b[>63u\x1b[?u");
    CHECK(term.output() == "\x1b[?31u");
    term.write("\x1b[<u\x1b[?u");
    CHECK(term.output() == "\x1b[?1u");
}

TEST_CASE("vterm: the title, the working directory and the clipboard are events") {
    Term term(4, 1);
    term.write("\x1b]2;hello\x07\x1b]7;file://host/tmp\x07\x1b]52;c;aGVsbG8=\x07\x07");
    PrVtermEventFlags events = prVtermTakeEvents(term.vterm);
    CHECK(events == (PrVtermEventFlag_Title | PrVtermEventFlag_WorkingDirectory | PrVtermEventFlag_Clipboard |
        PrVtermEventFlag_Bell));
    CHECK(prVtermTitle(term.vterm) == "hello");
    CHECK(prVtermWorkingDirectory(term.vterm) == "file://host/tmp");
    CHECK(prVtermClipboard(term.vterm) == "hello");
    CHECK(prVtermTakeEvents(term.vterm) == 0);

    term.write("\x1b[22t\x1b]2;other\x07\x1b[23t");
    CHECK(prVtermTitle(term.vterm) == "hello");
}

TEST_CASE("vterm: a narrower screen rewraps lines, and a wider one joins them, keeping the cursor on its text") {
    Term term(10, 3);
    term.write("abcdefgh\r\nxy");
    prVtermResize(term.vterm, PrUVec2{4, 3});
    CHECK(term.all() == "abcd\nefgh\nxy\n");
    CHECK(term.scrollback() == 0);
    CHECK(cursorAt(term, 2, 2));

    prVtermResize(term.vterm, PrUVec2{3, 3});
    CHECK(term.all() == "abc\ndef\ngh\nxy\n");
    CHECK(term.scrollback() == 1);
    CHECK(cursorAt(term, 2, 2));

    prVtermResize(term.vterm, PrUVec2{10, 3});
    CHECK(term.all() == "abcdefgh\nxy\n");
    CHECK(cursorAt(term, 2, 1));
}

TEST_CASE("vterm: a resize keeps wide characters whole") {
    Term term(6, 2);
    term.write("a\xe4\xb8\xad\xe5\x9b\xbd");
    prVtermResize(term.vterm, PrUVec2{4, 2});
    CHECK(term.all() == "a\xe4\xb8\xad\n\xe5\x9b\xbd\n");
    CHECK((term.cell(3, 0).flags & PrVtermCellFlag_WideSpacer) != 0);
}

TEST_CASE("vterm: a smaller height moves rows to the scrollback, and a greater height brings them back") {
    Term term(4, 4);
    term.write("1\r\n2\r\n3\r\n4");
    prVtermResize(term.vterm, PrUVec2{4, 2});
    CHECK(term.scrollback() == 2);
    CHECK(term.screen() == "3\n4\n");
    CHECK(cursorAt(term, 1, 1));
    prVtermResize(term.vterm, PrUVec2{4, 3});
    CHECK(term.scrollback() == 1);
    CHECK(term.screen() == "2\n3\n4\n");

    // If the cursor is above the bottom row, a smaller height drops the blank rows below it first. The cursor keeps its
    // row.
    Term top(4, 4);
    top.write("1\r\n2");
    prVtermResize(top.vterm, PrUVec2{4, 2});
    CHECK(top.scrollback() == 0);
    CHECK(top.screen() == "1\n2\n");
    prVtermResize(top.vterm, PrUVec2{4, 5});
    CHECK(top.screen() == "1\n2\n");
    CHECK(cursorAt(top, 1, 1));
}

TEST_CASE("vterm: a line that a scroll reuses is blank in every column that a write reached") {
    // Each edit writes to the screen, and then line feeds scroll every row off. The scroll reuses the line of each row,
    // so clearing that line must leave every cell zeroed.
    const char* edits[] = {
        "abc",
        "abc\x1b[1;2H\x1b[5@",                                 // an insertion moves text right
        "\x1b[1;10H\xe4\xb8\xad",                              // a wide character wraps, leaving a spacer
        "e\xcc\x81",                                           // a cluster
        "\x1b#8",                                              // DECALN fills every column
        "\x1b[41m\x1b[2K\x1b[3@\x1b[m",                        // blanks with a background color
        "\x1b[?69h\x1b[1;5s\x1b[2;1Hxyz\x1b[S\x1b[s\x1b[?69l", // a scroll within margins copies cells
    };
    for (const char* edit : edits) {
        String text = edit;
        CAPTURE(text);
        Term term(10, 2, 1);
        term.write(text);
        term.write("\x1b[2;1H\n\n\n\n");
        CHECK(term.screen().isEmpty());
        constexpr PrVtermCell kZero{};
        for (uint32_t row = 0; row < 2; ++row) {
            for (uint32_t column = 0; column < 10; ++column)
                CHECK(prMemEquals(&term.cell(column, row), &kZero, sizeof(PrVtermCell)));
        }
    }

    // Lines that a rewrap produced are reused the same way.
    Term term(10, 2, 1);
    term.write("0123456789ABCDE");
    prVtermResize(term.vterm, PrUVec2{6, 2});
    prVtermResize(term.vterm, PrUVec2{12, 2});
    term.write("\x1b[2;1H\n\n\n\n");
    CHECK(term.screen().isEmpty());
}

TEST_CASE("vterm: a rewrap reads a line up to its last cell that shows something") {
    // The first row wraps into the second, and an erase then leaves blanks at its end.
    Term term(10, 2);
    term.write("0123456789ABCDE\x1b[1;6H\x1b[K\x1b[2;6H\r\n\r\n");
    REQUIRE(term.scrollback() == 2);
    prVtermResize(term.vterm, PrUVec2{20, 2});
    CHECK(term.line(0) == "01234     ABCDE");
}

TEST_CASE("vterm: a line that enters the scrollback keeps its cells up to the last that shows something") {
    Term term(20, 2);
    term.write(format("abc\r\n\x1b[41mred\x1b[K\x1b[m\r\nx  \r\n{}\xe4\xb8\xad\r\n\r\n\r\n", String().append(19, 'a')));
    REQUIRE(term.scrollback() == 6);
    CHECK(prVtermLine(term.vterm, 0).cellCount == 3);
    CHECK(prVtermLine(term.vterm, 1).cellCount == 20);   // the erase fills to the end with the background color
    CHECK(prVtermLine(term.vterm, 2).cellCount == 3);    // printed spaces are text
    CHECK(prVtermLine(term.vterm, 3).cellCount == 20);   // ends with the spacer of a wrapped wide character
    CHECK(prVtermLine(term.vterm, 4).cellCount == 2);
    CHECK(prVtermLine(term.vterm, 5).cellCount == 0);
    CHECK(prVtermLine(term.vterm, 6).cellCount == 20);   // screen rows keep every column
    CHECK(term.line(0) == "abc");
    CHECK(term.line(3) == String().append(19, 'a'));
    CHECK(term.line(4) == "\xe4\xb8\xad");
    CHECK((prVtermLine(term.vterm, 3).cells[19].flags & PrVtermCellFlag_WideSpacer) != 0);
    CHECK(prVtermLine(term.vterm, 1).cells[19].background.kind == PrVtermColorKind_Palette);

    // Columns past the end of a line's cells read as spaces, up to the screen width.
    PrRawString text = prRawStringNull();
    prVtermLineAppendText(term.vterm, 0, 1, 30, &text);
    CHECK(prRawStringView(&text) == "bc                 ");
    prRawStringReset(&text);
}

TEST_CASE("vterm: the scrollback's lines hold little more than their text") {
    Term term(120, 10, 1000);
    String lines;
    for (uint32_t i = 0; i < 1100; ++i)
        formatTo(lines, "line {}\r\n", i);
    term.write(lines);
    REQUIRE(term.scrollback() == 1000);
    CHECK(term.line(999) == "line 1090");
    // Screen rows hold full cells. Each scrollback line holds only its text and a record.
    size_t rows = 2 * 10 * 120 * sizeof(PrVtermCell) + 10 * 120 * sizeof(PrVtermCell);
    CHECK(prVtermLinesMemorySize(term.vterm) < rows + 1000 * 64 + 2 * (64u << 10));
}

TEST_CASE("vterm: rows that a resize moves between the screen and the scrollback keep their cells") {
    Term term(10, 4);
    term.write("1\r\n\x1b[1m2\x1b[m\r\n3\r\n4");
    prVtermResize(term.vterm, PrUVec2{10, 2});
    REQUIRE(term.scrollback() == 2);
    CHECK(prVtermLine(term.vterm, 0).cellCount == 1);
    CHECK(prVtermLine(term.vterm, 1).cellCount == 1);
    prVtermResize(term.vterm, PrUVec2{10, 4});
    REQUIRE(term.scrollback() == 0);
    for (uint32_t row = 0; row < 4; ++row)
        CHECK(prVtermLine(term.vterm, row).cellCount == 10);
    CHECK((term.cell(0, 1).flags & PrVtermCellFlag_Bold) != 0);
    term.write("\x1b[H\x1b[42mX");
    CHECK(term.screen() == "X\n2\n3\n4\n");
    CHECK(term.cell(0, 0).background.kind == PrVtermColorKind_Palette);
    CHECK(term.cell(9, 1).codePoint == 0);

    // After a rewrap, the lines in the scrollback stay packed, and the screen rows have every column.
    Term wrap(10, 2);
    wrap.write("ab\r\ncd\r\nef\r\ngh");
    prVtermResize(wrap.vterm, PrUVec2{6, 2});
    REQUIRE(wrap.scrollback() == 2);
    CHECK(prVtermLine(wrap.vterm, 0).cellCount == 2);
    CHECK(prVtermLine(wrap.vterm, 2).cellCount == 6);
    CHECK(prVtermLine(wrap.vterm, 3).cellCount == 6);
    CHECK(wrap.all() == "ab\ncd\nef\ngh\n");
}

TEST_CASE("vterm: cells that edits move along a row stay when the row enters the scrollback") {
    // An insertion moves text right. A scroll within margins copies text between rows. DECALN fills every column.
    Term term(10, 3);
    term.write("abc\x1b[1;2H\x1b[5@\r\n\r\n\x1b[?69h\x1b[1;5s\x1b[3;1Hxyz\x1b[2;3r\x1b[S\x1b[r\x1b[?69l");
    term.write("\x1b[3;1H\r\n\r\n\r\n");
    REQUIRE(term.scrollback() == 3);
    CHECK(term.line(0) == "a     bc");
    CHECK(term.line(1) == "xyz");
    Term aligned(4, 2);
    aligned.write("\x1b#8\x1b[2;1H\r\n");
    REQUIRE(aligned.scrollback() == 1);
    CHECK(aligned.line(0) == "EEEE");
}

TEST_CASE("vterm: a rewrap reads the columns past a wrapped line's cells as blanks") {
    // The first row wraps into the second, and an erase then leaves blanks at its end.
    Term term(10, 2);
    term.write("0123456789ABCDE\x1b[1;6H\x1b[K\x1b[2;6H\r\n\r\n");
    REQUIRE(term.scrollback() == 2);
    CHECK(prVtermLine(term.vterm, 0).cellCount == 5);
    prVtermResize(term.vterm, PrUVec2{20, 2});
    CHECK(term.line(0) == "01234     ABCDE");
}

TEST_CASE("vterm: a line of the scrollback stays valid while the lines around it as many as the screen's rows are read") {
    Term term(10, 3, 100);
    String lines;
    for (uint32_t i = 0; i < 20; ++i)
        formatTo(lines, "line{}\r\n", i);
    term.write(lines);
    REQUIRE(term.scrollback() == 18);
    PrVtermLine first = prVtermLine(term.vterm, 5);
    PrVtermLine second = prVtermLine(term.vterm, 6);
    PrVtermLine third = prVtermLine(term.vterm, 7);
    CHECK(first.cells[4].codePoint == '5');
    CHECK(second.cells[4].codePoint == '6');
    CHECK(third.cells[4].codePoint == '7');

    // Reading the same line again returns the same cells and the same revision.
    CHECK(prVtermLine(term.vterm, 5).cells == first.cells);
    CHECK(prVtermLine(term.vterm, 5).revision == first.revision);
}

TEST_CASE("vterm: a row's cells are the same once it enters the scrollback, however its text was written") {
    // Each case writes the first row and then scrolls it off. The cases cover styled runs, overwrites, gaps, links and
    // other character sets. Each of these either extends or ends the row's record of printed text.
    const char* const kCases[] = {
        "plain text",
        "\x1b[1mbold\x1b[m and \x1b[31mred\x1b[m",
        "abcdef\rxy",
        "abcdef\r\x1b[1mxy",
        "\x1b[1mabcdef\r\x1b[mxy",
        "abc\x1b[1mdef\x1b[1;2Hz",
        "abc\tdef",
        "\x1b[41mabc\tdef",
        "\x1b[3Gabc",
        "\x1b[41m\x1b[3Gabc",
        "abc\x1b]8;;https://a\x1b\\link\x1b]8;;\x1b\\ end",
        "abc\x1b(0qqq\x1b(B def",
        "abc\x1b[4hXY",
        "abcdef\x1b[1;3H\x1b[2P",
        "abc\x1b[1;2H\x1b[K",
        "\x1b[44m\x1b[2K",
        "e\xcc\x81 x",
        "\xe4\xb8\xad\xe6\x96\x87 ok",
        "12345678901234567890",
    };
    for (const char* text : kCases) {
        CAPTURE(text);
        Term term(20, 3);
        term.write(format("{}\x1b[m", String(text)));
        Array<PrVtermCell> row;
        PrVtermLine before = prVtermLine(term.vterm, 0);
        for (uint32_t i = 0; i < before.cellCount; ++i)
            row.push(before.cells[i]);
        String shown = term.row(0);
        term.write("\x1b[3;1H\r\n\r\n\r\n");
        REQUIRE(term.scrollback() == 3);
        CHECK(term.line(0) == shown);
        PrVtermLine after = prVtermLine(term.vterm, 0);
        for (uint32_t i = 0; i < row.count(); ++i) {
            PrVtermCell expected = row[i];
            PrVtermCell actual = i < after.cellCount ? after.cells[i] : PrVtermCell{};
            CAPTURE(i);
            CHECK(uint32_t(actual.codePoint) == uint32_t(expected.codePoint));
            CHECK(uint32_t(actual.clusterCount) == uint32_t(expected.clusterCount));
            CHECK(uint32_t(actual.underline) == uint32_t(expected.underline));
            CHECK(actual.flags == expected.flags);
            CHECK(actual.link == expected.link);
            CHECK(actual.foreground.kind == expected.foreground.kind);
            CHECK(actual.background.kind == expected.background.kind);
            if (expected.background.kind == PrVtermColorKind_Palette)
                CHECK(prVtermColorIndex(actual.background) == prVtermColorIndex(expected.background));
            if (expected.foreground.kind == PrVtermColorKind_Palette)
                CHECK(prVtermColorIndex(actual.foreground) == prVtermColorIndex(expected.foreground));
        }
    }
}

TEST_CASE("vterm: a rewrap of packed lines gives the lines that a rewrap of the same rows gives") {
    // The same lines rewrap in two terminals. In one, they are packed lines in the scrollback. In the other, they are
    // rows of cells on a taller screen.
    String text = format("plain\r\n\x1b[1mbold\x1b[m and \x1b[31mred\x1b[m\r\n{}"
        "\x1b[42mgreen across\x1b[m the wrap\r\n\r\nabc\x1b[41m   \x1b[m\r\n\x1b[44mx\x1b[K\x1b[m\r\n"
        "\x1b[4munder\x1b[m line\x1b[38;2;1;2;3m rgb\x1b[m\r\nend",
        String().append(23, 'w'));
    auto describe = [](const Term& term, uint32_t index) {
        String out = format("{}|", term.line(index));
        PrVtermLine line = prVtermLine(term.vterm, index);
        uint32_t end = line.cellCount;
        while (end > 0 && line.cells[end - 1].codePoint == 0 && line.cells[end - 1].background.kind == 0 &&
                line.cells[end - 1].flags == 0 && line.cells[end - 1].underline == 0)
            --end;
        for (uint32_t i = 0; i < end; ++i) {
            const PrVtermCell& cell = line.cells[i];
            formatTo(out, "{},{},{}.{},{}.{};", cell.flags, uint32_t(cell.underline), cell.foreground.kind,
                prVtermColorIndex(cell.foreground), cell.background.kind, prVtermColorIndex(cell.background));
        }
        if ((line.flags & PrVtermLineFlag_Wrapped) != 0)
            out.append('+');
        return out;
    };
    const uint32_t widths[] = {7, 13, 40};
    for (uint32_t width : widths) {
        CAPTURE(width);
        Term packed(10, 2);
        packed.write(format("{}\r\n\r\n", text));
        Term rows(10, 30);
        rows.write(text);
        REQUIRE(rows.scrollback() == 0);
        prVtermResize(packed.vterm, PrUVec2{width, 2});
        prVtermResize(rows.vterm, PrUVec2{width, 30});
        REQUIRE(rows.scrollback() == 0);
        uint32_t count = packed.scrollback();
        REQUIRE(count > 8);
        for (uint32_t i = 0; i < count; ++i) {
            CAPTURE(i);
            CHECK(describe(packed, i) == describe(rows, i));
        }
    }
}

TEST_CASE("vterm: a line's revision changes when its cells change, and stays when it scrolls") {
    Term term(4, 2);
    term.write("a");
    uint64_t revision = prVtermLine(term.vterm, 0).revision;
    uint64_t terminal = prVtermRevision(term.vterm);
    term.write("b");
    CHECK(prVtermLine(term.vterm, 0).revision != revision);
    CHECK(prVtermRevision(term.vterm) > terminal);
    revision = prVtermLine(term.vterm, 0).revision;
    term.write("\r\n\r\n");
    REQUIRE(term.scrollback() == 1);
    CHECK(prVtermLine(term.vterm, 0).revision == revision);
}

TEST_CASE("vterm: tabs, backspace and repetition") {
    Term term(20, 1);
    term.write("a\tb\x08" "c\x1b[3b");
    CHECK(term.row(0) == "a       cccc");
    term.write("\x1b[1;5H\x1bH\x1b[1;1H\tx");
    CHECK(term.row(0) == "a   x   cccc");
    term.write("\x1b[3g\x1b[1;1H\ty");
    CHECK(term.row(0) == "a   x   cccc       y");
}

TEST_CASE("vterm: origin mode places the cursor in the scroll region") {
    Term term(4, 5);
    term.write("\x1b[2;4r\x1b[?6h\x1b[1;1Hx\x1b[9;1Hy\x1b[6n");
    CHECK(term.screen() == "\nx\n\ny\n");
    CHECK(term.output() == "\x1b[3;2R");
}

TEST_CASE("vterm: a full reset empties the screen, the scrollback and the modes") {
    Term term(4, 2);
    term.write("1\r\n2\r\n3\x1b[?2004h\x1b[31m\x1b" "c");
    CHECK(term.all().isEmpty());
    CHECK(prVtermModes(term.vterm) == 0);
    CHECK(cursorAt(term, 0, 0));
    term.write("x");
    CHECK(term.cell(0, 0).foreground.kind == PrVtermColorKind_Default);
}

TEST_CASE("vterm: a line's number stays its own through a full reset and a resize to another width") {
    Term term(4, 2, 10);
    term.write("1\r\n2\r\n3\r\n4");
    REQUIRE(term.scrollback() == 2);
    // The number of lines numbered so far. No later line may reuse one of their numbers.
    uint64_t next = prVtermScrollbackDropped(term.vterm) + term.scrollback() + 2;
    term.write("\x1b" "c");
    CHECK(prVtermScrollbackDropped(term.vterm) >= next);

    term.write("5\r\n6\r\n7");
    next = prVtermScrollbackDropped(term.vterm) + term.scrollback() + 2;
    prVtermResize(term.vterm, PrUVec2{3, 2});
    CHECK(prVtermScrollbackDropped(term.vterm) >= next);
    CHECK(term.all() == "5\n6\n7\n");

    // A resize to the same width keeps the numbers.
    uint64_t dropped = prVtermScrollbackDropped(term.vterm);
    prVtermResize(term.vterm, PrUVec2{3, 3});
    CHECK(prVtermScrollbackDropped(term.vterm) == dropped);

    next = prVtermScrollbackDropped(term.vterm) + term.scrollback() + 3;
    prVtermReset(term.vterm);
    CHECK(prVtermScrollbackDropped(term.vterm) >= next);
}

TEST_CASE("vterm: the caller's reset drops the queued output, the events and the clipboard") {
    Term term(4, 2);
    term.write("\x1b[6n\x07\x1b]52;c;aGVsbG8=\x07");
    REQUIRE(prVtermOutput(term.vterm).size != 0);
    prVtermReset(term.vterm);
    CHECK(prVtermOutput(term.vterm).size == 0);
    CHECK(prVtermTakeEvents(term.vterm) == 0);
    CHECK(prVtermClipboard(term.vterm).size == 0);
}

TEST_CASE("vterm: an extended color that SGR leaves incomplete or out of range keeps the pen's color") {
    Term term(10, 1);
    term.write("\x1b[31mA\x1b[38;5mB\x1b[31m\x1b[38;2;1;2mC\x1b[31m\x1b[38;5;300mD\x1b[38;2;300;20;999mE");
    for (uint32_t column = 0; column < 4; ++column) {
        CAPTURE(column);
        CHECK(term.cell(column, 0).foreground.kind == PrVtermColorKind_Palette);
        CHECK(prVtermColorIndex(term.cell(column, 0).foreground) == 1);
    }
    // A channel above 255 clamps to 255.
    const PrVtermCell& e = term.cell(4, 0);
    CHECK(e.foreground.kind == PrVtermColorKind_Rgb);
    CHECK(e.foreground.red == 255);
    CHECK(e.foreground.green == 20);
    CHECK(e.foreground.blue == 255);
}

TEST_CASE("vterm: the color revision changes with the palette, the roles and reverse video, and only with them") {
    Term term(4, 1);
    uint64_t revision = prVtermColorRevision(term.vterm);
    auto changed = [&]() {
        uint64_t now = prVtermColorRevision(term.vterm);
        bool different = now != revision;
        revision = now;
        return different;
    };
    term.write("text\x1b[31m\x1b[?25l\x1b]4;1;nonsense\x07");
    CHECK_FALSE(changed());
    term.write("\x1b[?5h");
    CHECK(changed());
    term.write("\x1b[?5h");
    CHECK_FALSE(changed());
    term.write("\x1b[?5l");
    CHECK(changed());
    term.write("\x1b]4;1;#00ff00\x07");
    CHECK(changed());
    term.write("\x1b]4;1;?\x07");
    CHECK_FALSE(changed());
    term.write("\x1b]11;#102030\x07");
    CHECK(changed());
    term.write("\x1b]104\x07");
    CHECK(changed());
    term.write("\x1b]111\x07");
    CHECK(changed());
    prVtermPaletteSetColor(term.vterm, 3, PrColor8{1, 2, 3, 255});
    CHECK(changed());
    prVtermRoleSetColor(term.vterm, PrVtermColorRole_Cursor, PrColor8{1, 2, 3, 255});
    CHECK(changed());
}
