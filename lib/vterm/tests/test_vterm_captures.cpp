// Plays the output of real programs into a terminal. captures/README.md says how the captures were recorded. Each
// capture plays into a terminal of the size that it was recorded at. A test compares the screen that it leaves with the
// text that the program writes if its output is not a terminal.

#include "pr/array.h"
#include "pr/string.h"
#include "pr/string_ops.h"
#include "pr/vterm.h"

#include "tools/harness/test.h"

namespace pr_impl {

using namespace pr;

namespace {

String readCapture(PrStringView name) {
    return harness::readText(format("{}/{}", PR_VTERM_CAPTURES_DIR, name));
}

// Splits `text` into lines. Drops trailing spaces from each line and empty lines from the end.
Array<String> linesOf(PrStringView text) {
    Array<String> lines;
    size_t at = 0;
    while (at <= text.size) {
        PrStringView rest = prStringSlice(text, at, text.size - at);
        size_t end = at + prStringFindByte(rest, '\n');
        lines.push(String(prStringTrimEndByte(prStringSlice(text, at, end - at), ' ')));
        at = end + 1;
    }
    while (!lines.isEmpty() && lines.last().isEmpty())
        lines.pop();
    return lines;
}

// A terminal that a capture plays into.
struct Player {
    PrVterm* vterm;

    Player(uint32_t columns, uint32_t rows) {
        PrVtermDesc desc{.extent = {columns, rows}, .scrollback = 100};
        vterm = prVtermCreate(&desc);
    }
    ~Player() { prVtermDestroy(vterm); }
    Player(const Player&) = delete;
    Player& operator=(const Player&) = delete;

    void write(PrStringView bytes) {
        prVtermWrite(vterm, prStringBytes(bytes));
    }

    // The text of screen row `row`, from column `start` for `count` cells, without trailing spaces.
    String row(uint32_t row, uint32_t start, uint32_t count) const {
        PrRawString text = prRawStringNull();
        prVtermLineAppendText(vterm, prVtermScrollbackCount(vterm) + row, start, start + count, &text);
        String out(prStringTrimEndByte(prRawStringView(&text), ' '));
        prRawStringReset(&text);
        return out;
    }

    const PrVtermCell& cell(uint32_t column, uint32_t row) const {
        return prVtermLine(vterm, prVtermScrollbackCount(vterm) + row).cells[column];
    }

    // Returns true if the screen shows nothing.
    bool isBlank() const {
        PrUVec2 extent = prVtermExtent(vterm);
        for (uint32_t i = 0; i < extent.y; ++i) {
            if (!row(i, 0, extent.x).isEmpty())
                return false;
        }
        return true;
    }
};

// Returns the part of `capture` before the program leaves the alternate screen, and the rest after it.
struct Parts {
    String before;
    String after;
};
Parts splitAtExit(PrStringView capture) {
    PrStringView leave = "\x1b[?1049l";
    size_t exit = capture.size;
    for (size_t from = 0;;) {
        PrStringView rest = prStringSlice(capture, from, capture.size - from);
        size_t at = prStringFind(rest, leave);
        if (at == rest.size)
            break;
        exit = from + at;
        from = exit + 1;
    }
    REQUIRE(exit != capture.size);
    return {String(prStringSlice(capture, 0, exit)), String(prStringSlice(capture, exit, capture.size - exit))};
}

// Checks that the program restored the terminal. The primary screen is active and blank, the scrollback is empty, the
// cursor is visible, and lines wrap.
void checkRestored(Player& player) {
    CHECK((prVtermModes(player.vterm) & PrVtermModeFlag_AlternateScreen) == 0);
    CHECK(prVtermCursor(player.vterm).visible);
    CHECK(player.isBlank());
    CHECK(prVtermScrollbackCount(player.vterm) == 0);
    PrUVec2 extent = prVtermExtent(player.vterm);
    String line = "\r";
    for (uint32_t i = 0; i < extent.x + 1; ++i)
        line.append('x');
    player.write(line);
    CHECK(player.row(1, 0, extent.x) == "x");
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("vterm captures: a banner draws faint box drawing over block letters, and its last frame is its text") {
    Player player(60, 12);
    player.write(readCapture("banner.tty"));
    Array<String> expected = linesOf(readCapture("banner.pipe"));
    REQUIRE(expected.count() > 2);
    for (uint32_t i = 0; i < expected.count(); ++i) {
        CAPTURE(i);
        CHECK(player.row(i, 0, 60) == expected[i]);
    }
    // The row that held npx's spinner is erased.
    CHECK(player.row(expected.count(), 0, 60).isEmpty());

    // The block letters are full weight, and the box-drawing shadow is faint.
    CHECK(player.cell(0, 0).codePoint == 0x2593);
    CHECK((player.cell(0, 0).flags & PrVtermCellFlag_Faint) == 0);
    CHECK(player.cell(6, 0).codePoint == 0x2557);
    CHECK((player.cell(6, 0).flags & PrVtermCellFlag_Faint) != 0);
    CHECK(prVtermCursor(player.vterm).visible);
}

TEST_CASE("vterm captures: a piece's frame is its text, centered, on the alternate screen") {
    Parts parts = splitAtExit(readCapture("donut.tty"));
    Player player(80, 24);
    player.write(parts.before);
    CHECK((prVtermModes(player.vterm) & PrVtermModeFlag_AlternateScreen) != 0);

    // The piece is 40 columns wide, from column 20, and its first line is on the screen's second row.
    Array<String> expected = linesOf(readCapture("donut.pipe"));
    REQUIRE(expected.count() > 10);
    for (uint32_t i = 0; i < expected.count(); ++i) {
        CAPTURE(i);
        CHECK(player.row(1 + i, 20, 40) == expected[i]);
    }
    CHECK(player.row(0, 0, 80).isEmpty());
    CHECK(player.row(1 + expected.count(), 0, 80).isEmpty());

    player.write(parts.after);
    checkRestored(player);
}

TEST_CASE("vterm captures: a scene in 24-bit color colors every cell") {
    Parts parts = splitAtExit(readCapture("ocean-sunset.tty"));
    Player player(100, 30);
    player.write(parts.before);
    uint32_t colored = 0;
    for (uint32_t row = 0; row < 30; ++row) {
        for (uint32_t column = 0; column < 100; ++column) {
            const PrVtermCell& cell = player.cell(column, row);
            if (cell.foreground.kind == PrVtermColorKind_Rgb || cell.background.kind == PrVtermColorKind_Rgb)
                ++colored;
        }
    }
    // The scene fills every row except a margin at the top and the bottom.
    CHECK(colored >= 100 * 25);
    player.write(parts.after);
    checkRestored(player);
}

TEST_CASE("vterm captures: an animation's frames end with the terminal as it was") {
    Parts parts = splitAtExit(readCapture("doom-fire.tty"));
    Player player(80, 24);
    player.write(parts.before);
    CHECK(!player.isBlank());
    player.write(parts.after);
    checkRestored(player);
}
