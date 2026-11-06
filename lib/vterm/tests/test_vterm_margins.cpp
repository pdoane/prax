#include "pr/vterm.h"

#include "tools/harness/test.h"

#include <string>

namespace pr_impl {

namespace {

// A terminal under test, with helpers that read what it shows.
struct Term {
    PrVterm* vterm;

    explicit Term(uint32_t columns, uint32_t rows, uint32_t scrollback = 100) {
        PrVtermDesc desc{.extent = {columns, rows}, .scrollback = scrollback};
        vterm = prVtermCreate(&desc);
    }
    ~Term() { prVtermDestroy(vterm); }
    Term(const Term&) = delete;
    Term& operator=(const Term&) = delete;

    void write(const std::string& text) {
        prVtermWrite(vterm, PrByteView{reinterpret_cast<const uint8_t*>(text.data()), text.size()});
    }

    uint32_t scrollback() const { return prVtermScrollbackCount(vterm); }

    PrVtermLine line(uint32_t row) const { return prVtermLine(vterm, scrollback() + row); }

    // The text of screen row `row`, without trailing spaces.
    std::string row(uint32_t row) const {
        PrVtermLine line = this->line(row);
        PrRawString text = prRawStringNull();
        prVtermLineAppendText(vterm, scrollback() + row, 0, line.cellCount, &text);
        std::string out(prRawStringView(&text).data, prRawStringView(&text).size);
        prRawStringReset(&text);
        while (!out.empty() && out.back() == ' ')
            out.pop_back();
        return out;
    }

    const PrVtermCell& cell(uint32_t column, uint32_t row) const { return line(row).cells[column]; }

    bool wrapped(uint32_t row) const { return (line(row).flags & PrVtermLineFlag_Wrapped) != 0; }

    PrUVec2 cursor() const { return prVtermCursor(vterm).position; }

    std::string output() {
        PrByteView bytes = prVtermOutput(vterm);
        std::string out(reinterpret_cast<const char*>(bytes.data), bytes.size);
        prVtermOutputConsume(vterm, bytes.size);
        return out;
    }

    // Returns true if, in the scrollback and on the screen, every wide cluster is followed by its tail and every tail
    // follows a wide cluster.
    bool widesWhole() const {
        for (uint32_t index = 0; index < scrollback() + prVtermExtent(vterm).y; ++index) {
            PrVtermLine line = prVtermLine(vterm, index);
            for (uint32_t column = 0; column < line.cellCount; ++column) {
                bool wide = (line.cells[column].flags & PrVtermCellFlag_Wide) != 0;
                bool tail = (line.cells[column].flags & PrVtermCellFlag_WideTail) != 0;
                bool nextTail = column + 1 < line.cellCount && (line.cells[column + 1].flags & PrVtermCellFlag_WideTail) != 0;
                bool previousWide = column > 0 && (line.cells[column - 1].flags & PrVtermCellFlag_Wide) != 0;
                if (wide != nextTail || (tail && !previousWide))
                    return false;
            }
        }
        return true;
    }
};

bool cursorAt(const Term& term, uint32_t column, uint32_t row) {
    return term.cursor().x == column && term.cursor().y == row;
}

// Fills the first `rows` rows of a six-column terminal with consecutive letters, starting at `a`.
void fillLetters(Term* term, uint32_t rows) {
    std::string text;
    for (uint32_t row = 0; row < rows; ++row) {
        text += "\x1b[" + std::to_string(row + 1) + ";1H";
        for (uint32_t column = 0; column < 6; ++column)
            text += char('a' + (row * 6 + column) % 26);
    }
    term->write(text);
}

// Returns the sequence that sets DECLRMM and then sets the margins from column `left` to column `right`. Columns count
// from one, as in DECSLRM.
std::string margins(uint32_t left, uint32_t right) {
    return "\x1b[?69h\x1b[" + std::to_string(left) + ";" + std::to_string(right) + "s";
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("vterm margins: DECSLRM sets the margins while DECLRMM is set, and CSI s saves the cursor otherwise") {
    Term term(10, 4);
    term.write("\x1b[1;5H\x1b[s\x1b[2;2H\x1b[u");
    CHECK(cursorAt(term, 4, 0));
    term.write("\x1b[?69$p");
    CHECK(term.output() == "\x1b[?69;2$y");

    // DECSLRM homes the cursor, and text then wraps from the right margin to the left.
    term.write("\x1b[2;2H" + margins(3, 6));
    CHECK(cursorAt(term, 0, 0));
    term.write("\x1b[?69$p");
    CHECK(term.output() == "\x1b[?69;1$y");
    term.write("\x1b[1;3Habcdefg");
    CHECK(term.row(0) == "  abcd");
    CHECK(term.row(1) == "  efg");
    CHECK(cursorAt(term, 5, 1));

    // Margins that are narrower than two columns or reversed are ignored, and do not move the cursor.
    term.write("\x1b[5;5s\x1b[7;3s");
    CHECK(cursorAt(term, 5, 1));

    // Without parameters, DECSLRM sets the margins to the screen's edges.
    term.write("\x1b[s\x1b[3;1H0123456789ab");
    CHECK(term.row(2) == "0123456789");
    CHECK(term.row(3) == "ab");
    CHECK(cursorAt(term, 2, 3));

    // Resetting DECLRMM resets the margins, and CSI s saves the cursor again.
    term.write(margins(3, 6) + "\x1b[?69l\x1b[3;1Hxxxxxxxx\x1b[s\x1b[1;1H\x1b[u");
    CHECK(cursorAt(term, 8, 2));
    term.write("\x1b[?69h\x1b[1;1Habcdefghij");
    CHECK(term.row(0) == "abcdefghij");
}

TEST_CASE("vterm margins: text wraps at the right margin, and only a wrap across the whole width marks the line") {
    Term term(10, 4);
    term.write(margins(1, 5) + "abcdefg");
    CHECK(term.row(0) == "abcde");
    CHECK(term.row(1) == "fg");
    CHECK_FALSE(term.wrapped(0));

    term.write("\x1b[s\x1b[3;1H0123456789xy");
    CHECK(term.row(2) == "0123456789");
    CHECK(term.row(3) == "xy");
    CHECK(term.wrapped(2));

    // Without autowrap, the column at the right margin is overwritten.
    term.write(margins(3, 6) + "\x1b[?7l\x1b[1;3Hvwxyz");
    CHECK(term.row(0) == "abvwxz");
}

TEST_CASE("vterm margins: text right of the right margin runs to the screen's edge, and wraps to the left margin") {
    Term term(10, 4);
    term.write(margins(3, 6) + "\x1b[1;8Hxyz");
    CHECK(term.row(0) == "       xyz");
    CHECK(cursorAt(term, 9, 0));
    term.write("w");
    CHECK(term.row(1) == "  w");
    CHECK(cursorAt(term, 3, 1));

    // Text that begins left of the left margin wraps at the right margin.
    term.write("\x1b[3;1Habcdefgh");
    CHECK(term.row(2) == "abcdef");
    CHECK(term.row(3) == "  gh");
}

TEST_CASE("vterm margins: a wide character that does not fit before the right margin wraps whole") {
    Term term(10, 3);
    term.write(margins(3, 6) + "\x1b[1;3Habc\xe4\xb8\xad");
    CHECK(term.row(0) == "  abc");
    CHECK(term.cell(5, 0).flags == 0);
    CHECK(term.row(1) == "  \xe4\xb8\xad");
    CHECK((term.cell(2, 1).flags & PrVtermCellFlag_Wide) != 0);
    CHECK(cursorAt(term, 4, 1));

    // A wide character that fits ends at the right margin, and the next character wraps.
    term.write("\xe5\x9b\xbdx");
    CHECK(term.row(1) == "  \xe4\xb8\xad\xe5\x9b\xbd");
    CHECK(term.row(2) == "  x");

    // An emoji presentation selector at the right margin does not widen the cluster past it.
    term.write("\x1b[3;6H\xe2\x9d\xa4\xef\xb8\x8f");
    CHECK((term.cell(5, 2).flags & PrVtermCellFlag_Wide) == 0);
    CHECK(term.widesWhole());
}

TEST_CASE("vterm margins: a carriage return goes to the left margin, or to the first column from left of it") {
    Term term(10, 2);
    term.write(margins(3, 6) + "\x1b[1;5H\r");
    CHECK(cursorAt(term, 2, 0));
    term.write("\x1b[1;9H\r");
    CHECK(cursorAt(term, 2, 0));
    term.write("\x1b[1;2H\r");
    CHECK(cursorAt(term, 0, 0));

    // A line feed in newline mode returns the cursor in the same way, and so do NEL, CNL and CPL.
    term.write("\x1b[20h\x1b[1;5H\n");
    CHECK(cursorAt(term, 2, 1));
    term.write("\x1b[20l\x1b[1;5H\x1b" "E");
    CHECK(cursorAt(term, 2, 1));
    term.write("\x1b[1;5H\x1b[E");
    CHECK(cursorAt(term, 2, 1));
    term.write("\x1b[2;2H\x1b[F");
    CHECK(cursorAt(term, 0, 0));
}

TEST_CASE("vterm margins: horizontal movements stop at the margins from inside them, and at the edges from outside") {
    Term term(10, 2);
    term.write(margins(3, 6));
    term.write("\x1b[1;5H\x1b[9D");
    CHECK(cursorAt(term, 2, 0));
    term.write("\x1b[1;2H\x1b[9D");
    CHECK(cursorAt(term, 0, 0));
    term.write("\x1b[1;3H\x08");
    CHECK(cursorAt(term, 2, 0));
    term.write("\x1b[1;4H\x1b[9C");
    CHECK(cursorAt(term, 5, 0));
    term.write("\x1b[1;1H\x1b[9C");
    CHECK(cursorAt(term, 5, 0));
    term.write("\x1b[1;8H\x1b[9C");
    CHECK(cursorAt(term, 9, 0));
    term.write("\x1b[1;4H\x1b[9a");
    CHECK(cursorAt(term, 5, 0));

    // A tab stops at the right margin from inside the margins.
    term.write("\x1b[1;3H\t");
    CHECK(cursorAt(term, 5, 0));
    term.write("\x1b[1;7H\t");
    CHECK(cursorAt(term, 8, 0));

    // Without origin mode, CHA and CUP place the cursor on the screen, outside the margins too.
    term.write("\x1b[9G");
    CHECK(cursorAt(term, 8, 0));
    term.write("\x1b[2;1H");
    CHECK(cursorAt(term, 0, 1));
}

TEST_CASE("vterm margins: origin mode counts columns from the left margin, and keeps the cursor in the margins") {
    Term term(10, 5);
    term.write(margins(3, 6) + "\x1b[2;4r\x1b[?6h");
    CHECK(cursorAt(term, 2, 1));
    term.write("x");
    CHECK(term.row(1) == "  x");

    term.write("\x1b[2;2H");
    CHECK(cursorAt(term, 3, 2));
    term.write("\x1b[6n");
    CHECK(term.output() == "\x1b[2;2R");
    term.write("\x1b[?6n");
    CHECK(term.output() == "\x1b[?2;2;1R");
    term.write("\x1b[9;9H");
    CHECK(cursorAt(term, 5, 3));
    term.write("\x1b[2G");
    CHECK(cursorAt(term, 3, 3));
    term.write("\x1b[9`");
    CHECK(cursorAt(term, 5, 3));

    // VPA keeps the column.
    term.write("\x1b[2G\x1b[1d");
    CHECK(cursorAt(term, 3, 1));

    // A back tab stops at the left margin.
    term.write("\x1b[Z");
    CHECK(cursorAt(term, 2, 1));

    // DECSLRM moves the cursor to the top-left corner of the new margins.
    term.write("\x1b[5;8s");
    CHECK(cursorAt(term, 4, 1));
}

TEST_CASE("vterm margins: IND, RI and LF scroll the rectangle of the margins, and only from inside it") {
    Term term(6, 4);
    fillLetters(&term, 4);
    uint64_t first = term.line(0).revision;
    term.write("\x1b[2;4r" + margins(2, 4) + "\x1b[4;3H\n");
    CHECK(term.row(0) == "abcdef");
    CHECK(term.row(1) == "gnopkl");
    CHECK(term.row(2) == "mtuvqr");
    CHECK(term.row(3) == "s   wx");
    CHECK(cursorAt(term, 2, 3));
    CHECK(term.line(0).revision == first);
    CHECK(term.scrollback() == 0);

    // Outside the margins, the cursor stays at the bottom margin and nothing scrolls.
    term.write("\x1b[4;6H\x1b" "D\x1b[4;1H\n");
    CHECK(term.row(3) == "s   wx");
    CHECK(cursorAt(term, 0, 3));

    term.write("\x1b[2;2H\x1bM");
    CHECK(term.row(1) == "g   kl");
    CHECK(term.row(2) == "mnopqr");
    CHECK(term.row(3) == "stuvwx");
    CHECK(cursorAt(term, 1, 1));
    term.write("\x1b[2;5H\x1bM");
    CHECK(term.row(1) == "g   kl");
    CHECK(cursorAt(term, 4, 1));

    // NEL performs an index at the cursor's column, and then returns the cursor.
    term.write("\x1b[4;6H\x1b" "E");
    CHECK(term.row(3) == "stuvwx");
    CHECK(cursorAt(term, 1, 3));
    term.write("\x1b[4;3H\x1b" "E");
    CHECK(term.row(1) == "gnopkl");
    CHECK(term.row(3) == "s   wx");
    CHECK(cursorAt(term, 1, 3));
}

TEST_CASE("vterm margins: SU and SD scroll the rectangle of the margins, wherever the cursor is") {
    Term term(6, 3);
    fillLetters(&term, 3);
    term.write(margins(2, 3) + "\x1b[1;6H\x1b[S");
    CHECK(term.row(0) == "ahidef");
    CHECK(term.row(1) == "gnojkl");
    CHECK(term.row(2) == "m  pqr");
    term.write("\x1b[2T");
    CHECK(term.row(0) == "a  def");
    CHECK(term.row(1) == "g  jkl");
    CHECK(term.row(2) == "mhipqr");
    CHECK(term.scrollback() == 0);
}

TEST_CASE("vterm margins: lines enter the scrollback only from a region that spans the width from the top") {
    Term term(6, 2);
    term.write("abcdef" + margins(1, 3) + "\x1b[2;1H\n");
    CHECK(term.scrollback() == 0);
    CHECK(term.row(0) == "   def");

    term.write("\x1b[s\x1b[2;1Hstuvwx\n");
    CHECK(term.scrollback() == 1);
    CHECK(term.row(0) == "stuvwx");
}

TEST_CASE("vterm margins: IL and DL move the rectangle below the cursor and return to the left margin") {
    Term term(6, 4);
    fillLetters(&term, 4);
    term.write(margins(2, 4) + "\x1b[2;3H\x1b[L");
    CHECK(term.row(0) == "abcdef");
    CHECK(term.row(1) == "g   kl");
    CHECK(term.row(2) == "mhijqr");
    CHECK(term.row(3) == "snopwx");
    CHECK(cursorAt(term, 1, 1));

    term.write("\x1b[2;3H\x1b[2M");
    CHECK(term.row(1) == "gnopkl");
    CHECK(term.row(2) == "m   qr");
    CHECK(term.row(3) == "s   wx");
    CHECK(cursorAt(term, 1, 1));

    // Outside the margins, IL and DL do nothing and do not move the cursor.
    term.write("\x1b[2;6H\x1b[L\x1b[2;1H\x1b[M");
    CHECK(term.row(1) == "gnopkl");
    CHECK(cursorAt(term, 0, 1));
    CHECK(term.scrollback() == 0);
}

TEST_CASE("vterm margins: ICH and DCH move the cells up to the right margin, and do nothing outside the margins") {
    Term term(8, 1);
    term.write("abcdefgh" + margins(3, 6) + "\x1b[1;4H\x1b[2@");
    CHECK(term.row(0) == "abc  dgh");
    term.write("\x1b[1;4H\x1b[P");
    CHECK(term.row(0) == "abc d gh");
    term.write("\x1b[1;8H\x1b[@\x1b[1;2H\x1b[P");
    CHECK(term.row(0) == "abc d gh");

    // Insert mode pushes the cells to the right margin.
    term.write("\x1b[4h\x1b[1;3HXY");
    CHECK(term.row(0) == "abXYc gh");
}

TEST_CASE("vterm margins: ECH, EL and ED ignore the margins") {
    Term term(8, 2);
    term.write("abcdefgh\r\nabcdefgh" + margins(3, 6) + "\x1b[1;4H\x1b[K");
    CHECK(term.row(0) == "abc");
    term.write("\x1b[2;4H\x1b[4X");
    CHECK(term.row(1) == "abc    h");
    term.write("\x1b[2;2H\x1b[1K");
    CHECK(term.row(1) == "  c    h");
    term.write("\x1b[1;1H\x1b[J");
    CHECK(term.row(1).empty());
}

TEST_CASE("vterm margins: DECIC and DECDC insert and delete columns in the scroll region") {
    Term term(6, 3);
    fillLetters(&term, 3);
    term.write("\x1b[1;2r" + margins(2, 5) + "\x1b[2;3H\x1b[2'}");
    CHECK(term.row(0) == "ab  cf");
    CHECK(term.row(1) == "gh  il");
    CHECK(term.row(2) == "mnopqr");
    CHECK(cursorAt(term, 2, 1));

    term.write("\x1b[1;2H\x1b['~");
    CHECK(term.row(0) == "a  c f");
    CHECK(term.row(1) == "g  i l");

    // Outside the margins or the scroll region, they do nothing.
    term.write("\x1b[1;6H\x1b['}\x1b[3;3H\x1b['~");
    CHECK(term.row(0) == "a  c f");
    CHECK(term.row(2) == "mnopqr");
}

TEST_CASE("vterm margins: DECBI and DECFI move the cursor, and scroll the columns at a margin") {
    Term term(6, 2);
    fillLetters(&term, 2);
    term.write(margins(2, 5) + "\x1b[1;4H\x1b" "6");
    CHECK(cursorAt(term, 2, 0));
    term.write("\x1b" "6\x1b" "6");
    CHECK(cursorAt(term, 1, 0));
    CHECK(term.row(0) == "a bcdf");
    CHECK(term.row(1) == "g hijl");

    term.write("\x1b[1;4H\x1b" "9");
    CHECK(cursorAt(term, 4, 0));
    term.write("\x1b" "9");
    CHECK(cursorAt(term, 4, 0));
    CHECK(term.row(0) == "abcd f");
    CHECK(term.row(1) == "ghij l");

    // Outside the margins, DECBI and DECFI only move the cursor.
    term.write("\x1b[1;1H\x1b" "6\x1b[1;6H\x1b" "9");
    CHECK(cursorAt(term, 5, 0));
    CHECK(term.row(0) == "abcd f");
}

TEST_CASE("vterm margins: SL and SR scroll the columns of the margins") {
    Term term(6, 2);
    fillLetters(&term, 2);
    term.write(margins(2, 5) + "\x1b[1;3H\x1b[2 @");
    CHECK(term.row(0) == "ade  f");
    CHECK(term.row(1) == "gjk  l");
    term.write("\x1b[ A");
    CHECK(term.row(0) == "a de f");
    CHECK(cursorAt(term, 2, 0));
}

TEST_CASE("vterm margins: a scroll of the rectangle keeps clusters and wide characters whole") {
    Term term(6, 2);
    // e with a combining acute accent, and a wide character across the left margin.
    term.write("ae\xcc\x81" "bcd\r\n\xe4\xb8\xad" "wxyz" + margins(2, 4) + "\x1b[1;1H\x1b[S");
    CHECK(term.row(0) == "a wxd");
    CHECK(term.row(1) == "    yz");
    CHECK(term.widesWhole());
    term.write("\x1b[T");
    CHECK(term.row(0) == "a   d");
    CHECK(term.row(1) == "  wxyz");

    // A cluster that the scroll moves to another line keeps its code points, however often it moves.
    Term clusters(6, 2);
    clusters.write("ae\xcc\x81" "cdef\r\nghijkl" + margins(2, 4) + "\x1b[T");
    CHECK(clusters.row(0) == "a   ef");
    CHECK(clusters.row(1) == "ge\xcc\x81" "cdkl");
    clusters.write("\x1b[S");
    CHECK(clusters.row(0) == "ae\xcc\x81" "cdef");
    CHECK(clusters.row(1) == "g   kl");
    for (int i = 0; i < 100; ++i)
        clusters.write("\x1b[T\x1b[S");
    CHECK(clusters.row(0) == "ae\xcc\x81" "cdef");
    CHECK(clusters.line(0).cells[1].clusterCount == 1);
}

TEST_CASE("vterm margins: ICH, DCH and DECIC keep wide characters whole at the margins") {
    Term term(8, 1);
    // The right margin splits a wide character, so ICH and DCH empty it.
    term.write("a\xe4\xb8\xad" "b\xe5\x9b\xbd" "cd" + margins(1, 2) + "\x1b[@");
    CHECK(term.widesWhole());
    CHECK(term.row(0) == " a b\xe5\x9b\xbd" "cd");
    term.write("\x1b[P");
    CHECK(term.widesWhole());
    CHECK(term.row(0) == "a  b\xe5\x9b\xbd" "cd");

    // An insertion pushes half of a wide character past the right margin.
    term.write("\x1b[s\x1b[1;1Ha\xe4\xb8\xad" "b\xe5\x9b\xbd" "cd" + margins(2, 3) + "\x1b[1;2H\x1b[@");
    CHECK(term.widesWhole());
    CHECK(term.row(0) == "a  b\xe5\x9b\xbd" "cd");

    // Both margins split wide characters.
    term.write("\x1b[s\x1b[1;1Ha\xe4\xb8\xad" "b\xe5\x9b\xbd" "cd" + margins(3, 5) + "\x1b[1;3H\x1b['}");
    CHECK(term.widesWhole());
    CHECK(term.row(0) == "a   b cd");
}

TEST_CASE("vterm margins: the lines that a scroll of the rectangle changes get new revisions and are not wrapped") {
    Term term(6, 3);
    term.write("abcdefghijklmnop");
    REQUIRE(term.wrapped(0));
    uint64_t revisions[3] = {term.line(0).revision, term.line(1).revision, term.line(2).revision};
    term.write("\x1b[2;3r" + margins(2, 4) + "\x1b[S");
    CHECK(term.line(0).revision == revisions[0]);
    CHECK(term.line(1).revision != revisions[1]);
    CHECK(term.line(2).revision != revisions[2]);
    CHECK(term.wrapped(0));
    CHECK_FALSE(term.wrapped(1));
}

TEST_CASE("vterm margins: DECSC and DECRC leave the margins, and DECSTR, RIS and a resize reset them") {
    Term term(10, 3);
    term.write(margins(3, 6) + "\x1b" "7" + margins(2, 4) + "\x1b" "8\x1b[1;2Habcd");
    CHECK(term.row(0) == " abc");
    CHECK(term.row(1) == " d");

    term.write("\x1b[!p\x1b[?69$p");
    CHECK(term.output() == "\x1b[?69;2$y");
    term.write("\x1b[3;1H0123456789");
    CHECK(term.row(2) == "0123456789");

    term.write(margins(3, 6) + "\x1b" "c\x1b[?69$p");
    CHECK(term.output() == "\x1b[?69;2$y");
    term.write("0123456789");
    CHECK(term.row(0) == "0123456789");

    // A resize keeps DECLRMM set.
    term.write(margins(3, 6));
    prVtermResize(term.vterm, PrUVec2{8, 3});
    term.write("\x1b[?69$p");
    CHECK(term.output() == "\x1b[?69;1$y");
    term.write("\x1b[2;1Habcdefghij");
    CHECK(term.row(1) == "abcdefgh");
    CHECK(term.row(2) == "ij");
}

TEST_CASE("vterm margins: a resize rewraps only the lines that wrapped across the whole width") {
    Term term(6, 4);
    term.write(margins(1, 3) + "abcdef\x1b[s\x1b[3;1Hghijklmn");
    prVtermResize(term.vterm, PrUVec2{12, 4});
    CHECK(term.row(0) == "abc");
    CHECK(term.row(1) == "def");
    CHECK(term.row(2) == "ghijklmn");
}

TEST_CASE("vterm margins: the primary device attributes report horizontal scrolling") {
    Term term(4, 1);
    term.write("\x1b[c");
    CHECK(term.output() == "\x1b[?62;21;22c");
}
