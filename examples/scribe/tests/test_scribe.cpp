// Scribe's whole interface, driven through events as a window would drive it: its menus, palette, cursors, scrollbar
// and the view's direction and writing mode.

#include "pr/file.h"
#include "pr/font_set.h"
#include "pr/layout_query.h"
#include "pr/layout_tree.h"
#include "pr/text_font.h"
#include "pr/ui.h"

#include "examples/scribe/scribe.h"
#include "tools/harness/kit_fixture.h"

#include "doctest/doctest.h"

#include <string>
#include <vector>

using namespace harness;
using namespace scribe;

namespace {

// Scribe over `text` in a kit of its own.
struct Editor {
    KitFixture f;
    Scribe s;

    explicit Editor(const std::string& text, const std::string& name = "") {
        PrFontFamily mono{.generic = PrFontGeneric_Monospace};
        PrFontStyle style = prFontStyleDefault();
        style.size = 13;
        PrTextFontDesc desc{.set = f.fonts, .families = &mono, .familyCount = 1, .style = style};
        s.font = prTextFontCreate(&desc);
        s.kit = f.kit;
        s.ui = f.ui;
        openEditor(s);
        if (!text.empty() || !name.empty())
            openText(s, text, name);
        else
            openDocument(s, "");
        f.build = [this](PrLayoutNode root) { buildScribe(s, root, PrVec2{800, 600}); };
        settle();
    }

    ~Editor() { closeEditor(s); }

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

    // Runs the command `id`, as the menu bar, the palette and the keys do.
    void run(const std::string& id) {
        REQUIRE(prCommandRegistryRun(s.commands, id, nullptr, nullptr) == PrStatus_Ok);
        settle();
    }

    // Returns the middle of the scrollbar's track, which runs along the port's right edge, or below it for vertical
    // lines, 12 px across.
    PrVec2 trackMiddle() {
        PrRect box = portBox();
        if (options().writingMode != PrLayoutWritingMode_HorizontalTb)
            return PrVec2{(box.left + box.right) / 2, box.bottom + 6};
        return PrVec2{box.right + 6, (box.top + box.bottom) / 2};
    }

    PrTextEditorDocument* document() { return prTextEditorShown(s.editor); }
    PrTextView* view() { return prTextEditorDocumentView(document()); }
    PrTextEditorOptions options() { return prTextEditorOptions(s.editor); }

    std::string text() {
        PrStringView all = prTextEditBufferText(prTextEditorDocumentBuffer(document()));
        return std::string(all.data, all.size);
    }

    uint32_t selections() { return prTextEditViewSelectionCount(prTextViewEdit(view())); }
    PrTextViewInfo info() { return prTextViewInfo(view()); }
    PrLayoutNode port() { return prTextEditorTextNode(s.editor); }
    PrRect portBox() { return f.bounds(port()); }

    // Returns the text node of the first row: the first node of its text's block, past the gutter's cell.
    PrLayoutNode firstRowText() {
        PrLayoutNode row = prLayoutNodeFirstChild(f.tree, port());
        PrLayoutNode cell = prLayoutNodeFirstChild(f.tree, row);
        PrLayoutNode block = prLayoutNodeNextSibling(f.tree, cell);
        return prLayoutNodeFirstChild(f.tree, block != kPrLayoutNodeNone ? block : row);
    }

    std::vector<PrLayoutHighlight> firstRowMarks() {
        PrLayoutNode text = firstRowText();
        std::vector<PrLayoutHighlight> marks(prLayoutNodeHighlights(f.tree, text, nullptr, 0));
        prLayoutNodeHighlights(f.tree, text, marks.data(), uint32_t(marks.size()));
        return marks;
    }
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
    CHECK(e.portBox().top == doctest::Approx(double(e.s.menuHeight)));
    CHECK(e.portBox().bottom < 600);
    e.type("x");
    CHECK(e.text() == "xhello");
}

TEST_CASE("a command runs on the text, and typing goes on in it") {
    Editor e("hello");
    e.type("x");
    e.run("edit.undo");
    CHECK(e.text() == "hello");
    e.type("y");
    CHECK(e.text() == "yhello");
}

TEST_CASE("the palette finds a command, runs it, and gives focus back to the text") {
    Editor e("hello");
    CHECK(e.options().wrap);
    e.key('P', kCommand | PrAppModifierFlag_Shift);
    CHECK(e.s.paletteOpen);
    e.type("word wrap");
    CHECK(e.s.paletteQuery == "word wrap");
    CHECK(e.text() == "hello");
    e.key(PrAppKey_Enter);
    e.settle();
    CHECK_FALSE(e.s.paletteOpen);
    CHECK_FALSE(e.options().wrap);
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

TEST_CASE("cursors from the keys and from the commands all type") {
    Editor e("one\ntwo\nthree");
    e.key(PrAppKey_Down, kCommand | PrAppModifierFlag_Alt);
    e.run("cursor.addBelow");
    CHECK(e.selections() == 3);
    CHECK(prLayoutTreeCarets(e.f.tree, nullptr, 0) == 3);
    e.type("x");
    CHECK(e.text() == "xone\nxtwo\nxthree");
    e.key(PrAppKey_Up, kCommand | PrAppModifierFlag_Alt);
    CHECK(e.selections() == 3);
}

TEST_CASE("a press with Alt adds a cursor where it lands, and a press without it leaves one") {
    Editor e(document(10));
    PrRect port = e.portBox();
    // A press over the gutter lands at the start of its line.
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
    PrRect port = e.portBox();
    PrVec2 inside{(port.left + port.right) / 2, (port.top + port.bottom) / 2};
    e.f.move(inside);
    e.f.frame();
    e.f.push(PrAppEvent{.kind = PrAppEventKind_Scroll, .position = inside, .delta = PrVec2{0, 200}});
    e.settle();
    CHECK(e.info().offset == doctest::Approx(200.0));

    PrVec2 middle = e.trackMiddle();
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
        e.run(id);
        CHECK(e.options().writingMode == (reversed ? PrLayoutWritingMode_VerticalRl : PrLayoutWritingMode_VerticalLr));

        // The rows stack across the port, and lines run down it.
        e.key(PrAppKey_Down);
        CHECK(e.info().caret.line == 0);
        CHECK(e.info().caret.column == 1);
        e.key(reversed ? PrAppKey_Left : PrAppKey_Right);
        CHECK(e.info().caret.line == 1);
        e.key(reversed ? PrAppKey_Right : PrAppKey_Left);
        CHECK(e.info().caret.line == 0);

        // A wheel that turns one way moves through the document, since wrapped lines have nowhere to scroll along.
        PrRect port = e.portBox();
        PrVec2 inside{(port.left + port.right) / 2, (port.top + port.bottom) / 2};
        e.f.move(inside);
        e.f.frame();
        e.f.push(PrAppEvent{.kind = PrAppEventKind_Scroll, .position = inside, .delta = PrVec2{0, 200}});
        e.settle();
        CHECK(e.info().offset == doctest::Approx(200.0));

        // The scrollbar lies under the text, and its middle is the middle of the document.
        PrVec2 middle = e.trackMiddle();
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
        e.run("view.horizontal");
        CHECK(e.options().writingMode == PrLayoutWritingMode_HorizontalTb);
    }
}

TEST_CASE("the View menu sets each line's direction, from the choice or from its script") {
    const std::string hebrew = "\xd7\x90\xd7\x91\xd7\x92";
    Editor e(hebrew + "\nabc");
    // Without the gutter, the port's edges are the lines' edges.
    e.run("view.lineNumbers");
    PrRect caret{};
    REQUIRE(prTextViewCaretRect(e.view(), &caret));
    float middle = (e.portBox().left + e.portBox().right) / 2;
    // In a left-to-right paragraph, Hebrew text still reads from its right, so the first caret is past its letters, and
    // near the left.
    CHECK(caret.left < middle);

    e.run("view.directionRtl");
    REQUIRE(prTextViewCaretRect(e.view(), &caret));
    CHECK(caret.left > middle);
    e.key(PrAppKey_Down);
    CHECK(e.info().caret.line == 1);
    REQUIRE(prTextViewCaretRect(e.view(), &caret));
    CHECK(caret.left > middle);

    // Each line takes the direction of its own script.
    e.run("view.directionAuto");
    e.key(PrAppKey_Home);
    REQUIRE(prTextViewCaretRect(e.view(), &caret));
    CHECK(caret.left < middle);
    e.key(PrAppKey_Up);
    e.key(PrAppKey_Home);
    REQUIRE(prTextViewCaretRect(e.view(), &caret));
    CHECK(caret.left > middle);
}

#if defined(SCRIBE_HAS_TREESITTER)

TEST_CASE("a file's extension chooses its language, whose captures color the text, and edits recolor it") {
    Editor e("int main(void) {\n    return 0; // done\n}\n", "main.c");
    const PrSyntaxLanguage* language = prTextEditorDocumentLanguage(e.document());
    REQUIRE(language != nullptr);
    CHECK(language->name == "c");
    // The first line's text node colors `int` as a type.
    std::vector<PrLayoutHighlight> marks = e.firstRowMarks();
    REQUIRE(!marks.empty());
    CHECK(marks[0].start == 0);
    CHECK(marks[0].end == 3);
    CHECK(marks[0].color.a != 0);
    // A comment opened at the start colors the whole line as one.
    e.type("/");
    e.type("/");
    marks = e.firstRowMarks();
    REQUIRE(marks.size() == 1);
    CHECK(marks[0].start == 0);
    CHECK(marks[0].end == 18);
}

TEST_CASE("a heading of Markdown takes the color of its class, whichever convention its grammar names it by") {
    Editor e("# Title\n\nbody text\n", "notes.md");
    bool titled = false;
    for (const PrLayoutHighlight& mark : e.firstRowMarks())
        titled = titled || (mark.start <= 2 && mark.end >= 7 && mark.color.a != 0);
    CHECK(titled);
}

TEST_CASE("a file of no language the build has shows plain text") {
    Editor e("plain words\n", "notes.txt");
    CHECK(prTextEditorDocumentLanguage(e.document()) == nullptr);
    CHECK(e.firstRowMarks().empty());
}

TEST_CASE("a large file streams in over frames, each piece colored as it shows, and an edit meanwhile keeps its undo") {
    std::string program;
    while (program.size() < (1u << 20))
        program += "int value = 1; // a line of the program\n";
    Editor e(program, "big.c");
    // The first frame shows text, already colored, and the rest is still to come.
    CHECK(prTextEditBufferSize(prTextEditorDocumentBuffer(e.document())) != 0);
    CHECK(!e.firstRowMarks().empty());
    // Typing goes in while the file loads, and the load keeps its undo.
    e.f.type("x");
    e.f.frame();
    for (uint32_t i = 0; i < 2000 && prTextEditorDocumentInfo(e.document()).loading; ++i) {
        e.f.frame();
        afterLayout(e.s);
    }
    REQUIRE_FALSE(prTextEditorDocumentInfo(e.document()).loading);
    CHECK(e.text() == "x" + program);
    e.key('Z', kCommand);
    CHECK(e.text() == program);
}

#endif

TEST_CASE("Save waits for the load to finish") {
    std::string program;
    while (program.size() < (1u << 20))
        program += "int value = 1;\n";
    PrRawString cache = prFileCacheDirectory();
    std::string folder = std::string(prRawStringView(&cache).data, prRawStringView(&cache).size) + "/prax-tests/scribe";
    prRawStringReset(&cache);
    REQUIRE(prFileCreateDirectory(folder));
    std::string path = folder + "/big.c";
    REQUIRE(prFileWrite(path, PrByteView{reinterpret_cast<const uint8_t*>(program.data()), program.size()}));
    Editor e("");
    REQUIRE(openDocument(e.s, path));
    prTextEditorShow(e.s.editor, prTextEditorDocumentFromPath(e.s.editor, path));
    e.f.frame();
    if (prTextEditorDocumentInfo(e.document()).loading) {
        uint32_t index = prCommandRegistryFind(e.s.commands, "file.save");
        CHECK((prCommandRegistryState(e.s.commands, index) & PrCommandStateFlag_Disabled) != 0);
    }
    for (uint32_t i = 0; i < 2000 && prTextEditorDocumentInfo(e.document()).loading; ++i)
        e.settle();
    uint32_t index = prCommandRegistryFind(e.s.commands, "file.save");
    CHECK((prCommandRegistryState(e.s.commands, index) & PrCommandStateFlag_Disabled) == 0);
}
