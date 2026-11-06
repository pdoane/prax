// Tests the hyperlinks that a program puts on cells (OSC 8). They cover which cells take a link, which cells share one
// link, and how links last through scrolling, rewrapping, editing and erasing.

#include "pr/vterm.h"

#include "tools/harness/test.h"

#include <string>

namespace pr_impl {

namespace {

// A terminal under test.
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

    std::string output() {
        PrByteView bytes = prVtermOutput(vterm);
        std::string out(reinterpret_cast<const char*>(bytes.data), bytes.size);
        prVtermOutputConsume(vterm, bytes.size);
        return out;
    }

    uint32_t scrollback() const { return prVtermScrollbackCount(vterm); }

    // The cell at `column` of screen row `row`, or of line `index`. Lines count from the oldest line of the scrollback.
    const PrVtermCell& cell(uint32_t column, uint32_t row) const { return at(column, scrollback() + row); }
    const PrVtermCell& at(uint32_t column, uint32_t index) const { return prVtermLine(vterm, index).cells[column]; }

    // The link of a screen cell, and the URI and id of a link. The URI and id are empty for kPrVtermLinkNone.
    PrVtermLinkIndex link(uint32_t column, uint32_t row) const { return cell(column, row).link; }
    std::string uri(PrVtermLinkIndex link) const {
        if (link == kPrVtermLinkNone)
            return "";
        PrStringView uri = prVtermLink(vterm, link).uri;
        return std::string(uri.data, uri.size);
    }
    std::string id(PrVtermLinkIndex link) const {
        if (link == kPrVtermLinkNone)
            return "";
        PrStringView id = prVtermLink(vterm, link).id;
        return std::string(id.data, id.size);
    }
    std::string uriAt(uint32_t column, uint32_t row) const { return uri(link(column, row)); }
};

// The sequence that opens a link to `uri` with `params`, and the one that closes it.
std::string open(const std::string& uri, const std::string& params = "") {
    return "\x1b]8;" + params + ";" + uri + "\x1b\\";
}
const std::string kClose = "\x1b]8;;\x1b\\";

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("vterm link: printed cells take the open link, and closing it ends it") {
    Term t(20, 4);
    t.write("a" + open("https://example.com/", "id=x") + "bc" + kClose + "d");
    CHECK(t.link(0, 0) == kPrVtermLinkNone);
    PrVtermLinkIndex link = t.link(1, 0);
    REQUIRE(link != kPrVtermLinkNone);
    CHECK(t.link(2, 0) == link);
    CHECK(t.link(3, 0) == kPrVtermLinkNone);
    CHECK(t.uri(link) == "https://example.com/");
    CHECK(t.id(link) == "x");
}

TEST_CASE("vterm link: the BEL terminator opens a link too, and a link without an id has the empty id") {
    Term t(20, 4);
    t.write("\x1b]8;;file:///tmp/a.txt\x07z\x1b]8;;\x07");
    CHECK(t.uriAt(0, 0) == "file:///tmp/a.txt");
    CHECK(t.id(t.link(0, 0)) == "");
}

TEST_CASE("vterm link: cells of the same id and URI are one link, apart, and after a redraw") {
    Term t(20, 4);
    t.write(open("https://a/", "id=1") + "one" + kClose + " gap " + open("https://a/", "id=1") + "two" + kClose);
    CHECK(t.link(0, 0) == t.link(9, 0));
    // A redraw of the same text at the same place keeps the link.
    t.write("\x1b[H" + open("https://a/", "id=1") + "one" + kClose);
    CHECK(t.link(0, 0) == t.link(9, 0));
    // Another URI with the same id, the same URI with another id, and an open without an id are each a separate link.
    t.write("\r\n" + open("https://b/", "id=1") + "b" + kClose + open("https://a/", "id=2") + "c" + kClose);
    t.write(open("https://a/") + "d" + kClose + open("https://a/") + "e" + kClose);
    PrVtermLinkIndex links[] = {t.link(0, 0), t.link(0, 1), t.link(1, 1), t.link(2, 1), t.link(3, 1)};
    for (uint32_t i = 0; i < 5; ++i) {
        for (uint32_t j = i + 1; j < 5; ++j)
            CHECK(links[i] != links[j]);
    }
    CHECK(t.uri(links[3]) == "https://a/");
}

TEST_CASE("vterm link: an open without an id is one link across the line that it wraps to") {
    Term t(4, 4);
    t.write(open("https://wrap/") + "abcdef" + kClose);
    PrVtermLinkIndex link = t.link(0, 0);
    CHECK(link != kPrVtermLinkNone);
    CHECK(t.link(3, 0) == link);
    CHECK(t.link(1, 1) == link);
    CHECK(t.link(2, 1) == kPrVtermLinkNone);
}

TEST_CASE("vterm link: parameters other than the id are ignored, and a malformed command changes nothing") {
    Term t(20, 4);
    t.write(open("https://p/", "foo=bar:id=k:x=y") + "a");
    CHECK(t.id(t.link(0, 0)) == "k");
    // A command without the separator before the URI leaves the current link open.
    t.write("\x1b]8;id=z\x1b\\b" + kClose);
    CHECK(t.link(1, 0) == t.link(0, 0));
}

TEST_CASE("vterm link: a URI that is too long or has bytes outside printable ASCII opens no link") {
    Term t(20, 4);
    std::string longUri = "https://x/" + std::string(kPrVtermLinkMaxUriBytes, 'a');
    t.write(open("https://ok/") + "a" + open(longUri) + "b" + kClose);
    CHECK(t.uriAt(0, 0) == "https://ok/");
    CHECK(t.link(1, 0) == kPrVtermLinkNone);
    t.write(open("https://a b\x7f/") + "c" + kClose);
    CHECK(t.link(2, 0) == kPrVtermLinkNone);
    std::string exact = "https://x/" + std::string(kPrVtermLinkMaxUriBytes - 10, 'a');
    t.write(open(exact) + "d" + kClose);
    CHECK(t.uriAt(3, 0) == exact);
    std::string longId(kPrVtermLinkMaxIdBytes + 1, 'i');
    t.write(open("https://id/", "id=" + longId) + "e" + kClose);
    CHECK(t.link(4, 0) == kPrVtermLinkNone);
}

TEST_CASE("vterm link: a wide cluster's halves both take the link") {
    Term t(10, 2);
    t.write(open("https://wide/") + "\xe4\xb8\x80" + kClose);
    CHECK(t.link(0, 0) != kPrVtermLinkNone);
    CHECK(t.link(1, 0) == t.link(0, 0));
}

TEST_CASE("vterm link: links stay on lines that scroll into the scrollback") {
    Term t(10, 2);
    t.write(open("https://old/") + "old" + kClose + "\r\n\n\n");
    REQUIRE(t.scrollback() == 2);
    CHECK(t.uri(t.at(0, 0).link) == "https://old/");
}

TEST_CASE("vterm link: a rewrap keeps the cells' links") {
    Term t(10, 4);
    t.write("ab" + open("https://r/", "id=r") + "cdefghij" + kClose + "kl");
    prVtermResize(t.vterm, PrUVec2{4, 4});
    uint32_t base = t.scrollback();
    // The line "abcdefghijkl" now spans three rows of four.
    PrVtermLinkIndex link = t.at(2, base + 0).link;
    CHECK(t.uri(link) == "https://r/");
    CHECK(t.at(1, base + 0).link == kPrVtermLinkNone);
    CHECK(t.at(0, base + 1).link == link);
    CHECK(t.at(1, base + 2).link == link);
    CHECK(t.at(2, base + 2).link == kPrVtermLinkNone);
    prVtermResize(t.vterm, PrUVec2{20, 4});
    base = t.scrollback();
    CHECK(t.at(9, base).link == link);
    CHECK(t.at(10, base).link == kPrVtermLinkNone);
}

TEST_CASE("vterm link: inserting and deleting characters moves the links with their cells") {
    Term t(10, 2);
    t.write(open("https://m/") + "ab" + kClose + "cd");
    PrVtermLinkIndex link = t.link(0, 0);
    t.write("\x1b[H\x1b[2@");
    CHECK(t.link(0, 0) == kPrVtermLinkNone);
    CHECK(t.link(2, 0) == link);
    CHECK(t.link(3, 0) == link);
    t.write("\x1b[3P");
    CHECK(t.link(0, 0) == link);
    CHECK(t.link(1, 0) == kPrVtermLinkNone);
}

TEST_CASE("vterm link: erasing and overwriting drop the links, and erasing cells gives them no link") {
    Term t(10, 3);
    t.write(open("https://e/") + "abcdef\r\nghij" + kClose);
    PrVtermLinkIndex link = t.link(0, 0);
    // An erase while a link is open leaves blank cells without the link.
    t.write("\x1b[H" + open("https://e2/") + "\x1b[2X");
    CHECK(t.link(0, 0) == kPrVtermLinkNone);
    CHECK(t.link(1, 0) == kPrVtermLinkNone);
    t.write(kClose + "\x1b[1;4HZ\x1b[2;1H\x1b[K");
    CHECK(t.link(2, 0) == link);
    CHECK(t.link(3, 0) == kPrVtermLinkNone);
    CHECK(t.link(0, 1) == kPrVtermLinkNone);
}

TEST_CASE("vterm link: scrolling a region between left and right margins copies the links with the cells") {
    Term t(10, 4);
    t.write("\x1b[2;1H" + open("https://s/") + "0123456789" + kClose);
    PrVtermLinkIndex link = t.link(0, 1);
    t.write("\x1b[?69h\x1b[3;6s\x1b[1;4r\x1b[1;3H\x1b[S");
    // Columns 3 to 6 of the second row moved up. The rest of that row did not move.
    CHECK(t.link(2, 0) == link);
    CHECK(t.link(5, 0) == link);
    CHECK(t.link(6, 0) == kPrVtermLinkNone);
    CHECK(t.link(0, 1) == link);
    CHECK(t.link(2, 1) == kPrVtermLinkNone);
    CHECK(t.uri(t.link(0, 1)) == "https://s/");
}

TEST_CASE("vterm link: SGR and saving and restoring the cursor keep the link, and a reset ends it") {
    Term t(20, 2);
    t.write(open("https://g/") + "a\x1b[0m\x1b[1mb\x1b" "7\x1b[5;5H\x1b" "8c\x1b[?1049hd\x1b[?1049l");
    PrVtermLinkIndex link = t.link(0, 0);
    CHECK(t.link(1, 0) == link);
    CHECK(t.link(2, 0) == link);
    t.write("\x1b[!pe");
    CHECK(t.link(3, 0) == kPrVtermLinkNone);
    t.write(open("https://g/") + "\x1b" "cf");
    CHECK(t.link(0, 0) == kPrVtermLinkNone);
}

TEST_CASE("vterm link: an open link leaves DECRQSS's report of the pen as it is") {
    Term t(20, 2);
    t.write("\x1b[1m" + open("https://q/", "id=q") + "\x1bP$qm\x1b\\");
    CHECK(t.output() == "\x1bP1$r0;1m\x1b\\");
}

TEST_CASE("vterm link: the alternate screen's links are its own, and the primary screen's stay while it shows") {
    Term t(10, 2);
    t.write(open("https://primary/") + "p" + kClose + "\x1b[?1049h\x1b[H" + open("https://alt/") + "q" + kClose);
    CHECK(t.uriAt(0, 0) == "https://alt/");
    // Links that no cell holds are freed. The links on the hidden primary screen stay.
    for (uint32_t i = 0; i < 2000; ++i)
        t.write("\x1b[2;1H" + open("https://churn/" + std::to_string(i)) + "x" + kClose);
    t.write("\x1b[?1049l");
    CHECK(t.uriAt(0, 0) == "https://primary/");
}

TEST_CASE("vterm link: links that no cell holds any more are freed, and those that cells hold stay") {
    Term t(10, 3, 1000);
    t.write(open("https://kept/", "id=k") + "k" + kClose);
    // Each link replaces the previous one in the same cell, so the table never holds more than a few links.
    for (uint32_t i = 0; i < 3 * uint32_t(kPrVtermMaxLinks); ++i)
        t.write("\x1b[2;1H" + open("https://u/" + std::to_string(i)) + "x" + kClose);
    CHECK(t.uriAt(0, 0) == "https://kept/");
    CHECK(t.uriAt(0, 1) == "https://u/" + std::to_string(3 * uint32_t(kPrVtermMaxLinks) - 1));
    // Opening the same id and URI again gives the same link.
    t.write("\x1b[1;2H" + open("https://kept/", "id=k") + "j" + kClose);
    CHECK(t.link(1, 0) == t.link(0, 0));
}

TEST_CASE("vterm link: a link past the most that the terminal holds opens no link until cells free some") {
    // A cell for each link that the terminal can hold, and one more.
    Term t(256, 256, 0);
    std::string text;
    for (uint32_t i = 0; i < uint32_t(kPrVtermMaxLinks) + 1; ++i)
        text += open("https://n/" + std::to_string(i)) + "x";
    t.write(text + kClose);
    CHECK(t.uriAt(254, 255) == "https://n/" + std::to_string(kPrVtermMaxLinks - 1));
    CHECK(t.link(255, 255) == kPrVtermLinkNone);
    t.write("\x1b[2J\x1b[H" + open("https://after/") + "y" + kClose);
    CHECK(t.uriAt(0, 0) == "https://after/");
}
