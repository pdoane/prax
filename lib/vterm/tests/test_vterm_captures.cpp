// Plays the output of real programs into a terminal. captures/README.md says how the captures were recorded. Each
// capture plays into a terminal of the size that it was recorded at. A test compares the screen that it leaves with the
// text that the program writes if its output is not a terminal.

#include "pr/vterm.h"

#include "tools/harness/test.h"

#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace pr_impl {

namespace {

std::string readCapture(const std::string& name) {
    std::ifstream file(std::string(PR_VTERM_CAPTURES_DIR) + "/" + name, std::ios::binary);
    REQUIRE(file.good());
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

// Splits `text` into lines. Drops trailing spaces from each line and empty lines from the end.
std::vector<std::string> linesOf(const std::string& text) {
    std::vector<std::string> lines;
    size_t at = 0;
    while (at <= text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos)
            end = text.size();
        std::string line = text.substr(at, end - at);
        while (!line.empty() && line.back() == ' ')
            line.pop_back();
        lines.push_back(line);
        at = end + 1;
    }
    while (!lines.empty() && lines.back().empty())
        lines.pop_back();
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

    void write(const std::string& bytes) {
        prVtermWrite(vterm, PrByteView{reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()});
    }

    // The text of screen row `row`, from column `start` for `count` cells, without trailing spaces.
    std::string row(uint32_t row, uint32_t start, uint32_t count) const {
        PrRawString text = prRawStringNull();
        prVtermLineAppendText(vterm, prVtermScrollbackCount(vterm) + row, start, start + count, &text);
        std::string out(prRawStringView(&text).data, prRawStringView(&text).size);
        prRawStringReset(&text);
        while (!out.empty() && out.back() == ' ')
            out.pop_back();
        return out;
    }

    const PrVtermCell& cell(uint32_t column, uint32_t row) const {
        return prVtermLine(vterm, prVtermScrollbackCount(vterm) + row).cells[column];
    }

    // Returns true if the screen shows nothing.
    bool isBlank() const {
        PrUVec2 extent = prVtermExtent(vterm);
        for (uint32_t i = 0; i < extent.y; ++i) {
            if (!row(i, 0, extent.x).empty())
                return false;
        }
        return true;
    }
};

// Returns the part of `capture` before the program leaves the alternate screen, and the rest after it.
std::pair<std::string, std::string> splitAtExit(const std::string& capture) {
    size_t exit = capture.rfind("\x1b[?1049l");
    REQUIRE(exit != std::string::npos);
    return {capture.substr(0, exit), capture.substr(exit)};
}

// Checks that the program restored the terminal. The primary screen is active and blank, the scrollback is empty, the
// cursor is visible, and lines wrap.
void checkRestored(Player& player) {
    CHECK((prVtermModes(player.vterm) & PrVtermModeFlag_AlternateScreen) == 0);
    CHECK(prVtermCursor(player.vterm).visible);
    CHECK(player.isBlank());
    CHECK(prVtermScrollbackCount(player.vterm) == 0);
    PrUVec2 extent = prVtermExtent(player.vterm);
    player.write("\r" + std::string(extent.x + 1, 'x'));
    CHECK(player.row(1, 0, extent.x) == "x");
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("vterm captures: a banner draws faint box drawing over block letters, and its last frame is its text") {
    Player player(60, 12);
    player.write(readCapture("banner.tty"));
    std::vector<std::string> expected = linesOf(readCapture("banner.pipe"));
    REQUIRE(expected.size() > 2);
    for (uint32_t i = 0; i < expected.size(); ++i) {
        CAPTURE(i);
        CHECK(player.row(i, 0, 60) == expected[i]);
    }
    // The row that held npx's spinner is erased.
    CHECK(player.row(uint32_t(expected.size()), 0, 60).empty());

    // The block letters are full weight, and the box-drawing shadow is faint.
    CHECK(player.cell(0, 0).codePoint == 0x2593);
    CHECK((player.cell(0, 0).flags & PrVtermCellFlag_Faint) == 0);
    CHECK(player.cell(6, 0).codePoint == 0x2557);
    CHECK((player.cell(6, 0).flags & PrVtermCellFlag_Faint) != 0);
    CHECK(prVtermCursor(player.vterm).visible);
}

TEST_CASE("vterm captures: a piece's frame is its text, centered, on the alternate screen") {
    std::pair<std::string, std::string> parts = splitAtExit(readCapture("donut.tty"));
    Player player(80, 24);
    player.write(parts.first);
    CHECK((prVtermModes(player.vterm) & PrVtermModeFlag_AlternateScreen) != 0);

    // The piece is 40 columns wide, from column 20, and its first line is on the screen's second row.
    std::vector<std::string> expected = linesOf(readCapture("donut.pipe"));
    REQUIRE(expected.size() > 10);
    for (uint32_t i = 0; i < expected.size(); ++i) {
        CAPTURE(i);
        CHECK(player.row(1 + i, 20, 40) == expected[i]);
    }
    CHECK(player.row(0, 0, 80).empty());
    CHECK(player.row(1 + uint32_t(expected.size()), 0, 80).empty());

    player.write(parts.second);
    checkRestored(player);
}

TEST_CASE("vterm captures: a scene in 24-bit color colors every cell") {
    std::pair<std::string, std::string> parts = splitAtExit(readCapture("ocean-sunset.tty"));
    Player player(100, 30);
    player.write(parts.first);
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
    player.write(parts.second);
    checkRestored(player);
}

TEST_CASE("vterm captures: an animation's frames end with the terminal as it was") {
    std::pair<std::string, std::string> parts = splitAtExit(readCapture("doom-fire.tty"));
    Player player(80, 24);
    player.write(parts.first);
    CHECK(!player.isBlank());
    player.write(parts.second);
    checkRestored(player);
}
