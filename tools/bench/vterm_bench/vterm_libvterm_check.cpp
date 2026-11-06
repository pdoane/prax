// Checks that the vterm benchmark's adapters for lib/vterm and libvterm hold the same text after the same input. The
// cases cover text with colors, wide and combining characters, the scrollback and its limit, the alternate screen, and
// resizes. The program exits with a nonzero status if a result is wrong.

#include "tools/bench/vterm_bench/libvterm_engine.h"
#include "tools/bench/vterm_bench/prax_engine.h"

#include <memory>
#include <stdio.h>
#include <string>

using namespace vterm_bench;

namespace {

int failures = 0;

void check(bool passed, const char* what) {
    if (passed)
        return;
    fprintf(stderr, "vterm_libvterm_check: %s\n", what);
    ++failures;
}

// Returns the text of line `index` in the terminal of `engine`, without trailing spaces.
std::string lineText(const Engine& engine, uint32_t index, uint32_t columns) {
    PrRawString text = prRawStringEmpty();
    engine.lineAppendText(index, columns, &text);
    std::string out(prRawStringView(&text).data, prRawStringView(&text).size);
    prRawStringReset(&text);
    while (!out.empty() && out.back() == ' ')
        out.pop_back();
    return out;
}

// A terminal of libvterm and one of lib/vterm, with the same extent and scrollback, that receive the same bytes.
struct Pair {
    std::unique_ptr<Engine> libvterm = makeLibvterm();
    std::unique_ptr<Engine> prax = makePrax();
    PrUVec2 extent;
    const char* name;

    Pair(const char* name, uint32_t columns, uint32_t rows, uint32_t scrollback) : extent{columns, rows}, name(name) {
        libvterm->create(extent, scrollback);
        prax->create(extent, scrollback);
    }

    void write(const std::string& bytes) {
        PrByteView view{reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()};
        libvterm->write(view);
        prax->write(view);
    }

    void resize(uint32_t columns, uint32_t rows) {
        extent = PrUVec2{columns, rows};
        libvterm->resize(extent);
        prax->resize(extent);
    }

    // The text of libvterm's line `index`, and of row `row` of its screen, which follows the scrollback.
    std::string line(uint32_t index) const { return lineText(*libvterm, index, extent.x); }
    std::string row(uint32_t row) const { return line(libvterm->scrollbackCount() + row); }

    // Checks that both terminals hold the same scrollback and show the same text.
    void checkSame() const {
        uint32_t scrollback = prax->scrollbackCount();
        if (libvterm->scrollbackCount() != scrollback) {
            fprintf(stderr, "vterm_libvterm_check: %s: libvterm holds %u lines of scrollback, lib/vterm %u\n", name,
                libvterm->scrollbackCount(), scrollback);
            ++failures;
            return;
        }
        for (uint32_t i = 0; i < scrollback + extent.y; ++i) {
            std::string theirs = line(i);
            std::string ours = lineText(*prax, i, extent.x);
            if (theirs != ours) {
                fprintf(stderr, "vterm_libvterm_check: %s: line %u is \"%s\" in libvterm and \"%s\" in lib/vterm\n",
                    name, i, theirs.c_str(), ours.c_str());
                ++failures;
            }
        }
    }
};

// Text in UTF-8 shows on the screen. Its colors are parsed and do not show as text.
void checkText() {
    Pair pair("text", 20, 4, 10);
    pair.write("hello\r\n\x1b[1;38;2;10;20;30mcaf\xc3\xa9\x1b[0m \x1b[38;5;200mworld");
    check(pair.row(0) == "hello", "text: the first row");
    check(pair.row(1) == "caf\xc3\xa9 world", "text: the colored row");
    pair.checkSame();
}

// Lines that scroll off the top enter the scrollback, which drops its oldest lines past its limit.
void checkScrollback() {
    Pair pair("scrollback", 10, 2, 3);
    for (int i = 0; i < 9; ++i)
        pair.write("line " + std::to_string(i) + "\r\n");
    check(pair.libvterm->scrollbackCount() == 3, "scrollback: the limit");
    check(pair.line(0) == "line 5", "scrollback: the oldest line");
    check(pair.line(2) == "line 7", "scrollback: the newest line");
    check(pair.row(0) == "line 8", "scrollback: the screen's first row");
    check(pair.row(1).empty(), "scrollback: the screen's last row");
    pair.checkSame();
}

// Without a scrollback, every line that scrolls off is dropped.
void checkNoScrollback() {
    Pair pair("no scrollback", 10, 2, 0);
    pair.write("a\r\nb\r\nc");
    check(pair.libvterm->scrollbackCount() == 0, "no scrollback: a line kept");
    check(pair.row(0) == "b", "no scrollback: the first row");
    check(pair.row(1) == "c", "no scrollback: the last row");
    pair.checkSame();
}

// A wide character takes two cells, and a combining mark joins its base.
void checkWide() {
    Pair pair("wide", 10, 2, 10);
    pair.write("\xe4\xb8\xad\xe6\x96\x87x e\xcc\x81");
    check(pair.row(0) == "\xe4\xb8\xad\xe6\x96\x87x e\xcc\x81", "wide: the row");
    pair.write("\x1b[1;6Hy");
    check(pair.row(0) == "\xe4\xb8\xad\xe6\x96\x87xye\xcc\x81", "wide: the column after the wide characters");
    pair.checkSame();
}

// The alternate screen keeps the primary screen and its scrollback.
void checkAlternateScreen() {
    Pair pair("alternate screen", 10, 2, 10);
    pair.write("a\r\nb\r\nc");
    pair.write("\x1b[?1049h\x1b[Hfull\r\nscreen\r\n");
    check(pair.libvterm->scrollbackCount() == 1, "alternate screen: the scrollback");
    check(pair.row(0) == "screen", "alternate screen: the row");
    pair.checkSame();
    pair.write("\x1b[?1049l");
    check(pair.row(0) == "b", "alternate screen: the primary screen's first row");
    check(pair.row(1) == "c", "alternate screen: the primary screen's last row");
    pair.checkSame();
}

// A taller screen takes its lines back from the scrollback.
void checkTaller() {
    Pair pair("taller", 10, 2, 10);
    pair.write("a\r\nb\r\nc\r\nd");
    check(pair.libvterm->scrollbackCount() == 2, "taller: the scrollback before");
    pair.resize(10, 4);
    check(pair.libvterm->scrollbackCount() == 0, "taller: the scrollback after");
    check(pair.row(0) == "a", "taller: the first row");
    check(pair.row(3) == "d", "taller: the last row");
    pair.checkSame();
}

// A narrower screen rewraps the lines that it shows.
void checkNarrower() {
    Pair pair("narrower", 10, 3, 10);
    pair.write("old\r\nabcdefghij");
    pair.resize(5, 3);
    check(pair.row(0) == "old", "narrower: the first row");
    check(pair.row(1) == "abcde", "narrower: the wrapped line's first row");
    check(pair.row(2) == "fghij", "narrower: the wrapped line's last row");
    pair.checkSame();
}

} // namespace

int main() {
    checkText();
    checkScrollback();
    checkNoScrollback();
    checkWide();
    checkAlternateScreen();
    checkTaller();
    checkNarrower();
    return failures == 0 ? 0 : 1;
}
