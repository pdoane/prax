// Tests the hyperlinks that a program puts on cells (OSC 8). They cover which cells take a link, which cells share one
// link, and how links last through scrolling, rewrapping, editing and erasing.

#include "pr/string.h"
#include "pr/string_ops.h"
#include "pr/vterm.h"

#include "tools/harness/test.h"

namespace pr_impl {

using namespace pr;

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

    void write(PrStringView text) {
        prVtermWrite(vterm, prStringBytes(text));
    }

    String output() {
        PrByteView bytes = prVtermOutput(vterm);
        String out(prStringFromBytes(bytes));
        prVtermOutputConsume(vterm, bytes.size);
        return out;
    }

    uint32_t scrollback() const { return prVtermScrollbackCount(vterm); }

    // The cell at `column` of screen row `row`, or of line `index`. Lines count from the oldest line of the scrollback.
    const PrVtermCell& cell(uint32_t column, uint32_t row) const { return at(column, scrollback() + row); }
    const PrVtermCell& at(uint32_t column, uint32_t index) const { return prVtermLine(vterm, index).cells[column]; }

    // The link of a screen cell, and the URI and id of a link. The URI and id are empty for kPrVtermLinkNone.
    PrVtermLinkIndex link(uint32_t column, uint32_t row) const { return cell(column, row).link; }
    String uri(PrVtermLinkIndex link) const {
        if (link == kPrVtermLinkNone)
            return "";
        PrStringView uri = prVtermLink(vterm, link).uri;
        return String(uri);
    }
    String id(PrVtermLinkIndex link) const {
        if (link == kPrVtermLinkNone)
            return "";
        PrStringView id = prVtermLink(vterm, link).id;
        return String(id);
    }
    String uriAt(uint32_t column, uint32_t row) const { return uri(link(column, row)); }
};

// The sequence that opens a link to `uri` with `params`, and the one that closes it.
String open(PrStringView uri, PrStringView params = "") {
    return format("\x1b]8;{};{}\x1b\\", params, uri);
}
constexpr PrStringView kClose = "\x1b]8;;\x1b\\";

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("vterm link: printed cells take the open link, and closing it ends it") {
    Term t(20, 4);
    t.write(format("a{}bc{}d", open("https://example.com/", "id=x"), kClose));
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
    t.write(format("{}one{} gap {}two{}", open("https://a/", "id=1"), kClose, open("https://a/", "id=1"), kClose));
    CHECK(t.link(0, 0) == t.link(9, 0));
    // A redraw of the same text at the same place keeps the link.
    t.write(format("\x1b[H{}one{}", open("https://a/", "id=1"), kClose));
    CHECK(t.link(0, 0) == t.link(9, 0));
    // Another URI with the same id, the same URI with another id, and an open without an id are each a separate link.
    t.write(format("\r\n{}b{}{}c{}", open("https://b/", "id=1"), kClose, open("https://a/", "id=2"), kClose));
    t.write(format("{}d{}{}e{}", open("https://a/"), kClose, open("https://a/"), kClose));
    PrVtermLinkIndex links[] = {t.link(0, 0), t.link(0, 1), t.link(1, 1), t.link(2, 1), t.link(3, 1)};
    for (uint32_t i = 0; i < 5; ++i) {
        for (uint32_t j = i + 1; j < 5; ++j)
            CHECK(links[i] != links[j]);
    }
    CHECK(t.uri(links[3]) == "https://a/");
}

TEST_CASE("vterm link: an open without an id is one link across the line that it wraps to") {
    Term t(4, 4);
    t.write(format("{}abcdef{}", open("https://wrap/"), kClose));
    PrVtermLinkIndex link = t.link(0, 0);
    CHECK(link != kPrVtermLinkNone);
    CHECK(t.link(3, 0) == link);
    CHECK(t.link(1, 1) == link);
    CHECK(t.link(2, 1) == kPrVtermLinkNone);
}

TEST_CASE("vterm link: parameters other than the id are ignored, and a malformed command changes nothing") {
    Term t(20, 4);
    t.write(format("{}a", open("https://p/", "foo=bar:id=k:x=y")));
    CHECK(t.id(t.link(0, 0)) == "k");
    // A command without the separator before the URI leaves the current link open.
    t.write(format("\x1b]8;id=z\x1b\\b{}", kClose));
    CHECK(t.link(1, 0) == t.link(0, 0));
}

TEST_CASE("vterm link: a URI that is too long or has bytes outside printable ASCII opens no link") {
    Term t(20, 4);
    String longUri = format("https://x/{}", String().append(kPrVtermLinkMaxUriBytes, 'a'));
    t.write(format("{}a{}b{}", open("https://ok/"), open(longUri), kClose));
    CHECK(t.uriAt(0, 0) == "https://ok/");
    CHECK(t.link(1, 0) == kPrVtermLinkNone);
    t.write(format("{}c{}", open("https://a b\x7f/"), kClose));
    CHECK(t.link(2, 0) == kPrVtermLinkNone);
    String exact = format("https://x/{}", String().append(kPrVtermLinkMaxUriBytes - 10, 'a'));
    t.write(format("{}d{}", open(exact), kClose));
    CHECK(t.uriAt(3, 0) == exact);
    String longId = String().append(kPrVtermLinkMaxIdBytes + 1, 'i');
    t.write(format("{}e{}", open("https://id/", format("id={}", longId)), kClose));
    CHECK(t.link(4, 0) == kPrVtermLinkNone);
}

TEST_CASE("vterm link: a wide cluster's halves both take the link") {
    Term t(10, 2);
    t.write(format("{}\xe4\xb8\x80{}", open("https://wide/"), kClose));
    CHECK(t.link(0, 0) != kPrVtermLinkNone);
    CHECK(t.link(1, 0) == t.link(0, 0));
}

TEST_CASE("vterm link: links stay on lines that scroll into the scrollback") {
    Term t(10, 2);
    t.write(format("{}old{}\r\n\n\n", open("https://old/"), kClose));
    REQUIRE(t.scrollback() == 2);
    CHECK(t.uri(t.at(0, 0).link) == "https://old/");
}

TEST_CASE("vterm link: a rewrap keeps the cells' links") {
    Term t(10, 4);
    t.write(format("ab{}cdefghij{}kl", open("https://r/", "id=r"), kClose));
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
    t.write(format("{}ab{}cd", open("https://m/"), kClose));
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
    t.write(format("{}abcdef\r\nghij{}", open("https://e/"), kClose));
    PrVtermLinkIndex link = t.link(0, 0);
    // An erase while a link is open leaves blank cells without the link.
    t.write(format("\x1b[H{}\x1b[2X", open("https://e2/")));
    CHECK(t.link(0, 0) == kPrVtermLinkNone);
    CHECK(t.link(1, 0) == kPrVtermLinkNone);
    t.write(format("{}\x1b[1;4HZ\x1b[2;1H\x1b[K", kClose));
    CHECK(t.link(2, 0) == link);
    CHECK(t.link(3, 0) == kPrVtermLinkNone);
    CHECK(t.link(0, 1) == kPrVtermLinkNone);
}

TEST_CASE("vterm link: scrolling a region between left and right margins copies the links with the cells") {
    Term t(10, 4);
    t.write(format("\x1b[2;1H{}0123456789{}", open("https://s/"), kClose));
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
    t.write(format("{}a\x1b[0m\x1b[1mb\x1b" "7\x1b[5;5H\x1b" "8c\x1b[?1049hd\x1b[?1049l", open("https://g/")));
    PrVtermLinkIndex link = t.link(0, 0);
    CHECK(t.link(1, 0) == link);
    CHECK(t.link(2, 0) == link);
    t.write("\x1b[!pe");
    CHECK(t.link(3, 0) == kPrVtermLinkNone);
    t.write(format("{}\x1b" "cf", open("https://g/")));
    CHECK(t.link(0, 0) == kPrVtermLinkNone);
}

TEST_CASE("vterm link: an open link leaves DECRQSS's report of the pen as it is") {
    Term t(20, 2);
    t.write(format("\x1b[1m{}\x1bP$qm\x1b\\", open("https://q/", "id=q")));
    CHECK(t.output() == "\x1bP1$r0;1m\x1b\\");
}

TEST_CASE("vterm link: the alternate screen's links are its own, and the primary screen's stay while it shows") {
    Term t(10, 2);
    t.write(format("{}p{}\x1b[?1049h\x1b[H{}q{}", open("https://primary/"), kClose, open("https://alt/"), kClose));
    CHECK(t.uriAt(0, 0) == "https://alt/");
    // Links that no cell holds are freed. The links on the hidden primary screen stay.
    for (uint32_t i = 0; i < 2000; ++i)
        t.write(format("\x1b[2;1H{}x{}", open(format("https://churn/{}", i)), kClose));
    t.write("\x1b[?1049l");
    CHECK(t.uriAt(0, 0) == "https://primary/");
}

TEST_CASE("vterm link: links that no cell holds any more are freed, and those that cells hold stay") {
    Term t(10, 3, 1000);
    t.write(format("{}k{}", open("https://kept/", "id=k"), kClose));
    // Each link replaces the previous one in the same cell, so the table never holds more than a few links.
    for (uint32_t i = 0; i < 3 * uint32_t(kPrVtermMaxLinks); ++i)
        t.write(format("\x1b[2;1H{}x{}", open(format("https://u/{}", i)), kClose));
    CHECK(t.uriAt(0, 0) == "https://kept/");
    CHECK(t.uriAt(0, 1) == format("https://u/{}", 3 * uint32_t(kPrVtermMaxLinks) - 1));
    // Opening the same id and URI again gives the same link.
    t.write(format("\x1b[1;2H{}j{}", open("https://kept/", "id=k"), kClose));
    CHECK(t.link(1, 0) == t.link(0, 0));
}

TEST_CASE("vterm link: a link past the most that the terminal holds opens no link until cells free some") {
    // A cell for each link that the terminal can hold, and one more.
    Term t(256, 256, 0);
    String text;
    for (uint32_t i = 0; i < uint32_t(kPrVtermMaxLinks) + 1; ++i)
        formatTo(text, "{}x", open(format("https://n/{}", i)));
    text.append(kClose);
    t.write(text);
    CHECK(t.uriAt(254, 255) == format("https://n/{}", kPrVtermMaxLinks - 1));
    CHECK(t.link(255, 255) == kPrVtermLinkNone);
    t.write(format("\x1b[2J\x1b[H{}y{}", open("https://after/"), kClose));
    CHECK(t.uriAt(0, 0) == "https://after/");
}
