// Scribe's whole interface, driven through events as a window would drive it: its menus, palette, cursors, scrollbar
// and the view's direction and writing mode.

#include "pr/font_set.h"
#include "pr/layout_query.h"
#include "pr/layout_tree.h"
#include "pr/text_font.h"
#include "pr/ui.h"

#include "examples/scribe/scribe.h"
#include "tools/harness/kit_fixture.h"

#include "doctest/doctest.h"

#include <string>

using namespace harness;
using namespace scribe;

namespace {

// The editor over `text` in a kit of its own.
struct Editor {
    KitFixture f;
    Scribe s;

    explicit Editor(const std::string& text) {
        openText(s, text);
        PrFontFamily mono{.generic = PrFontGeneric_Monospace};
        PrFontStyle style = prFontStyleDefault();
        style.size = 13;
        PrTextFontDesc desc{.set = f.fonts, .families = &mono, .familyCount = 1, .style = style};
        s.font = prTextFontCreate(&desc);
        s.kit = f.kit;
        s.ui = f.ui;
        registerCommands(s);
        f.build = [this](PrLayoutNode root) { buildEditor(s, root, PrVec2{800, 600}); };
        settle();
    }

    ~Editor() { closeDocument(s); }

    // Runs frames until the editor has nothing more to show.
    void settle() {
        f.frame();
        for (uint32_t i = 0; i < 32 && afterLayout(s); ++i)
            f.frame();
        afterLayout(s);
    }

    void key(PrAppKey key, PrAppModifierFlags modifiers = 0) {
        f.key(key, modifiers);
        settle();
    }

    void type(const char* text) {
        f.type(text);
        settle();
    }

    void click(PrVec2 at, PrAppModifierFlags modifiers = 0) {
        f.move(at);
        f.frame();
        f.click(at, modifiers);
        settle();
    }

    // Returns the node of a whole-tree key, or kPrLayoutNodeNone.
    PrLayoutNode find(const std::string& name) {
        return prLayoutNodeFromKey(f.tree, prUiKey(f.ui, kPrLayoutNodeNone, name));
    }

    // Returns the item of the menu bar titled `menu`.
    PrLayoutNode menuTitle(const std::string& menu) { return find("scribe.menu." + menu); }

    // Returns the item of the command `id` in the open menu titled `menu`, or kPrLayoutNodeNone.
    PrLayoutNode menuItem(const std::string& menu, const std::string& id) {
        PrLayoutNode list = find("scribe.menu." + menu + ".list");
        if (list == kPrLayoutNodeNone)
            return kPrLayoutNodeNone;
        return prLayoutNodeFromKey(f.tree, prUiKey(f.ui, list, id));
    }

    // Opens the menu titled `menu`, and presses the item of the command `id` in it.
    void choose(const std::string& menu, const std::string& id) {
        click(f.center(menuTitle(menu)));
        PrLayoutNode item = menuItem(menu, id);
        REQUIRE(item != kPrLayoutNodeNone);
        click(f.center(item));
    }

    std::string text() {
        PrStringView all = prTextEditBufferText(s.buffer);
        return std::string(all.data, all.size);
    }

    uint32_t selections() { return prTextEditViewSelectionCount(prTextViewEdit(s.view)); }
    PrTextViewInfo info() { return prTextViewInfo(s.view); }
    PrRect port() { return f.bounds(s.editor); }
};

// A document of `count` lines of different text.
std::string document(uint32_t count) {
    std::string text;
    for (uint32_t i = 0; i < count; ++i)
        text += "line " + std::to_string(i) + (i + 1 < count ? "\n" : "");
    return text;
}

constexpr PrAppModifierFlags kCommand = kPrCommandPrimaryModifier;

} // namespace

TEST_CASE("the text sits below the menu bar and above the status line, and takes focus") {
    Editor e("hello");
    CHECK(e.s.menuHeight > 0);
    CHECK(e.port().top == doctest::Approx(double(e.s.menuHeight)));
    CHECK(e.port().bottom < 600);
    e.type("x");
    CHECK(e.text() == "xhello");
}

TEST_CASE("a menu's item runs its command, and typing goes on in the text") {
    Editor e("hello");
    e.type("x");
    e.choose("Edit", "edit.undo");
    CHECK(e.text() == "hello");
    e.type("y");
    CHECK(e.text() == "yhello");
}

TEST_CASE("the palette finds a command, runs it, and gives focus back to the text") {
    Editor e("hello");
    CHECK(e.s.wrap);
    e.key('P', kCommand | PrAppModifierFlag_Shift);
    CHECK(e.s.paletteOpen);
    e.type("word wrap");
    CHECK(e.s.paletteQuery == "word wrap");
    CHECK(e.text() == "hello");
    e.key(PrAppKey_Enter);
    e.settle();
    CHECK_FALSE(e.s.paletteOpen);
    CHECK_FALSE(e.s.wrap);
    e.type("x");
    CHECK(e.text() == "xhello");
}

TEST_CASE("Escape closes the palette and leaves the cursors, and then keeps one cursor") {
    Editor e("one\ntwo\nthree");
    e.key(PrAppKey_Down, kCommand | PrAppModifierFlag_Alt);
    CHECK(e.selections() == 2);
    e.key('P', kCommand | PrAppModifierFlag_Shift);
    CHECK(e.s.paletteOpen);
    e.key(PrAppKey_Escape);
    CHECK_FALSE(e.s.paletteOpen);
    CHECK(e.selections() == 2);
    e.key(PrAppKey_Escape);
    CHECK(e.selections() == 1);
}

TEST_CASE("Escape closes an open menu and leaves the cursors") {
    Editor e("one\ntwo");
    e.key(PrAppKey_Down, kCommand | PrAppModifierFlag_Alt);
    e.click(e.f.center(e.menuTitle("Selection")));
    CHECK(e.menuItem("Selection", "cursor.single") != kPrLayoutNodeNone);
    e.key(PrAppKey_Escape);
    CHECK(e.menuItem("Selection", "cursor.single") == kPrLayoutNodeNone);
    CHECK(e.selections() == 2);
}

TEST_CASE("cursors from the keys and from the menu all type") {
    Editor e("one\ntwo\nthree");
    e.key(PrAppKey_Down, kCommand | PrAppModifierFlag_Alt);
    e.choose("Selection", "cursor.addBelow");
    CHECK(e.selections() == 3);
    CHECK(prLayoutTreeCarets(e.f.tree, nullptr, 0) == 3);
    e.type("x");
    CHECK(e.text() == "xone\nxtwo\nxthree");
    e.key(PrAppKey_Up, kCommand | PrAppModifierFlag_Alt);
    CHECK(e.selections() == 3);
}

TEST_CASE("a press with Alt adds a cursor where it lands, and a press without it leaves one") {
    Editor e(document(10));
    PrRect port = e.port();
    PrVec2 third{port.left + 1, port.top + 2 * 19 + 5};
    e.click(third, PrAppModifierFlag_Alt);
    CHECK(e.selections() == 2);
    CHECK(e.info().caret.line == 2);
    e.type("x");
    CHECK(e.text().substr(0, 7) == "xline 0");
    e.click(PrVec2{port.left + 1, port.top + 5});
    CHECK(e.selections() == 1);
    CHECK(e.info().caret.line == 0);
}

TEST_CASE("the wheel scrolls the text, and the scrollbar's drag moves it") {
    Editor e(document(5000));
    PrRect port = e.port();
    PrVec2 inside{(port.left + port.right) / 2, (port.top + port.bottom) / 2};
    e.f.move(inside);
    e.f.frame();
    e.f.push(PrAppEvent{.kind = PrAppEventKind_Scroll, .position = inside, .delta = PrVec2{0, 200}});
    e.settle();
    CHECK(e.info().offset == doctest::Approx(200.0));

    PrRect track = e.f.bounds(e.find("scribe.scrollbar"));
    PrVec2 middle{(track.left + track.right) / 2, (track.top + track.bottom) / 2};
    e.f.move(middle);
    e.f.frame();
    e.f.push(PrAppEvent{.kind = PrAppEventKind_PointerDown, .position = middle});
    e.f.frame();
    e.f.push(PrAppEvent{.kind = PrAppEventKind_PointerMove, .position = PrVec2{middle.x, middle.y + 2}});
    e.settle();
    e.f.push(PrAppEvent{.kind = PrAppEventKind_PointerUp, .position = middle});
    e.settle();
    CHECK(e.info().offset > e.info().documentSize / 4);
    CHECK(e.info().offset < e.info().documentSize * 3 / 4);
}

TEST_CASE("the View menu sets vertical lines, whose arrows, wheel and scrollbar turn with them") {
    for (const char* id : {"view.verticalRl", "view.verticalLr"}) {
        bool reversed = std::string(id) == "view.verticalRl";
        Editor e(document(5000));
        e.choose("View", id);
        CHECK(e.s.writingMode == (reversed ? PrLayoutWritingMode_VerticalRl : PrLayoutWritingMode_VerticalLr));

        // The rows stack across the port, and lines run down it.
        e.key(PrAppKey_Down);
        CHECK(e.info().caret.line == 0);
        CHECK(e.info().caret.column == 1);
        e.key(reversed ? PrAppKey_Left : PrAppKey_Right);
        CHECK(e.info().caret.line == 1);
        e.key(reversed ? PrAppKey_Right : PrAppKey_Left);
        CHECK(e.info().caret.line == 0);

        // A wheel that turns one way moves through the document, since wrapped lines have nowhere to scroll along.
        PrRect port = e.port();
        PrVec2 inside{(port.left + port.right) / 2, (port.top + port.bottom) / 2};
        e.f.move(inside);
        e.f.frame();
        e.f.push(PrAppEvent{.kind = PrAppEventKind_Scroll, .position = inside, .delta = PrVec2{0, 200}});
        e.settle();
        CHECK(e.info().offset == doctest::Approx(200.0));

        // The scrollbar lies under the text, and its middle is the middle of the document.
        PrRect track = e.f.bounds(e.find("scribe.scrollbar"));
        CHECK(track.top >= port.bottom - 1);
        PrVec2 middle{(track.left + track.right) / 2, (track.top + track.bottom) / 2};
        e.f.move(middle);
        e.f.frame();
        e.f.push(PrAppEvent{.kind = PrAppEventKind_PointerDown, .position = middle});
        e.f.frame();
        e.f.push(PrAppEvent{.kind = PrAppEventKind_PointerMove, .position = PrVec2{middle.x + 2, middle.y}});
        e.settle();
        e.f.push(PrAppEvent{.kind = PrAppEventKind_PointerUp, .position = middle});
        e.settle();
        CHECK(e.info().offset > e.info().documentSize / 4);
        CHECK(e.info().offset < e.info().documentSize * 3 / 4);

        // The caret comes back into the port from far off.
        e.key(PrAppKey_End, kCommand);
        e.choose("View", "view.horizontal");
        CHECK(e.s.writingMode == PrLayoutWritingMode_HorizontalTb);
    }
}

TEST_CASE("the View menu sets each line's direction, from the choice or from its script") {
    const std::string hebrew = "\xd7\x90\xd7\x91\xd7\x92";
    Editor e(hebrew + "\nabc");
    PrRect caret{};
    REQUIRE(prTextViewCaretRect(e.s.view, &caret));
    float middle = (e.port().left + e.port().right) / 2;
    // In a left-to-right paragraph, Hebrew text still reads from its right, so the first caret is past its letters, and
    // near the left.
    CHECK(caret.left < middle);

    e.choose("View", "view.directionRtl");
    REQUIRE(prTextViewCaretRect(e.s.view, &caret));
    CHECK(caret.left > middle);
    e.key(PrAppKey_Down);
    CHECK(e.info().caret.line == 1);
    REQUIRE(prTextViewCaretRect(e.s.view, &caret));
    CHECK(caret.left > middle);

    // Each line takes the direction of its own script.
    e.choose("View", "view.directionAuto");
    e.key(PrAppKey_Home);
    REQUIRE(prTextViewCaretRect(e.s.view, &caret));
    CHECK(caret.left < middle);
    e.key(PrAppKey_Up);
    e.key(PrAppKey_Home);
    REQUIRE(prTextViewCaretRect(e.s.view, &caret));
    CHECK(caret.left > middle);
}
