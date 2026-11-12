// Scribe's whole interface, driven through events as a window would drive it: its menus, palette, cursors, scrollbar
// and the view's direction and writing mode.

#include "pr/array.h"
#include "pr/file.h"
#include "pr/font_set.h"
#include "pr/layout_query.h"
#include "pr/layout_tree.h"
#include "pr/string.h"
#include "pr/string_ops.h"
#include "pr/text_font.h"
#include "pr/thread.h"
#include "pr/ui.h"
#include "pr/url_file.h"

#include "apps/scribe/scribe.h"
#include "tools/harness/kit_fixture.h"

#include "tools/harness/test.h"

using namespace harness;
using namespace scribe;

namespace {

// Scribe over `text` in a kit of its own.
struct Editor {
    KitFixture f;
    Scribe s;

    explicit Editor(PrStringView text, PrStringView name = "") {
        PrFontFamily mono{.generic = PrFontGeneric_Monospace};
        PrFontStyle style = prFontStyleDefault();
        style.size = 13;
        PrTextFontDesc desc{.set = f.fonts, .families = &mono, .familyCount = 1, .style = style};
        s.font = prTextFontCreate(&desc);
        s.fonts = f.fonts;
        s.kit = f.kit;
        s.ui = f.ui;
        openEditor(s);
        if (!text.isEmpty() || !name.isEmpty())
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
    void run(PrStringView id) {
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

    String text() {
        return prTextEditBufferText(prTextEditorDocumentBuffer(document()));
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

    Array<PrLayoutHighlight> firstRowMarks() {
        PrLayoutNode text = firstRowText();
        Array<PrLayoutHighlight> marks(prLayoutNodeHighlights(f.tree, text, nullptr, 0));
        prLayoutNodeHighlights(f.tree, text, marks.data(), marks.count());
        return marks;
    }
};

// A document of `count` lines of different text.
String document(uint32_t count) {
    String text;
    for (uint32_t i = 0; i < count; ++i)
        formatTo(text, "line {}{}", i, i + 1 < count ? "\n" : "");
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

TEST_CASE("the terminal panel opens below the text with focus, and its toggle hides it and gives the text focus") {
    Editor e("hello");
    float textBottom = e.portBox().bottom;
    e.run("terminal.toggle");
    // The terminal receives focus in the frame after it asks for it.
    e.settle();
    REQUIRE(e.s.terminals.count() == 1);
    CHECK(e.s.terminalShown);
    PrLayoutNode screen = prTerminalNode(e.s.terminals[0]);
    REQUIRE(screen != kPrLayoutNodeNone);
    CHECK(prUiFocus(e.f.ui) == screen);
    CHECK(prTerminalHasFocus(e.s.terminals[0]));
    CHECK(e.portBox().bottom < textBottom);
    CHECK(e.f.bounds(screen).top >= e.portBox().bottom);
    e.run("terminal.toggle");
    CHECK(!e.s.terminalShown);
    CHECK(e.portBox().bottom == doctest::Approx(double(textBottom)));
    e.type("x");
    CHECK(e.text() == "xhello");
    e.run("terminal.new");
    e.settle();
    e.run("terminal.close");
    CHECK(e.s.terminals.count() == 1);
}

TEST_CASE("the terminal panel paints the terminal's background beneath its view, and follows the program's") {
    Editor e("hello");
    e.run("terminal.toggle");
    e.settle();
    REQUIRE(e.s.terminals.count() == 1);
    PrTerminal* terminal = e.s.terminals[0];
    PrStringView magenta = "\x1b]11;#ff00ff\x07";
    prVtermWrite(prTerminalVtermLock(terminal), PrByteView{prStringBytes(magenta), magenta.size});
    prTerminalVtermUnlock(terminal);
    e.settle();
    PrColor background = prTerminalBackground(terminal);
    CHECK(background.r == 1.0f);
    CHECK(background.g == 0.0f);
    std::vector<uint8_t> rgba;
    REQUIRE(e.f.render(&rgba));
    auto isMagenta = [&](PrVec2 at) {
        size_t i = (size_t(at.y) * KitFixture::kRenderWidth + size_t(at.x)) * 4;
        return rgba[i] > 250 && rgba[i + 1] < 5 && rgba[i + 2] > 250;
    };
    // A blank cell, the gutter, and the leftover space past the last whole cell, at the view's bottom right.
    PrRect view = e.f.bounds(prTerminalNode(terminal));
    PrVec2 cell = prTerminalCellExtent(terminal);
    PrVec2 origin = prTerminalScreenOrigin(terminal);
    CHECK(isMagenta(PrVec2{view.left + origin.x + cell.x * 20.5f, view.top + cell.y * 2.5f}));
    CHECK(isMagenta(PrVec2{view.left + origin.x / 2, view.top + cell.y * 2.5f}));
    CHECK(isMagenta(PrVec2{view.right - 1, view.bottom - 1}));
    // The text above the panel keeps its own background.
    PrRect text = e.portBox();
    CHECK_FALSE(isMagenta(PrVec2{text.right - 20, text.bottom - 4}));
}

TEST_CASE("the first terminal starts in the document's workspace, and a new one where the shown one is") {
    String folder = format("{}/workspace", PrStringView(harness::testFolder()));
    REQUIRE(prFileCreateDirectory(format("{}/.git", folder)));
    REQUIRE(prFileCreateDirectory(format("{}/sub", folder)));
    String path = format("{}/sub/notes.txt", folder);
    REQUIRE(prFileWrite(path, PrByteView{prStringBytes("notes\n"), 6}));
    Editor e("");
    REQUIRE(openDocument(e.s, path));
    prTextEditorShow(e.s.editor, prTextEditorDocumentFromPath(e.s.editor, path));
    e.settle();
    // Returns the working directory of the terminal at `index`.
    auto directory = [&](uint32_t index) {
        PrRawString text = prTerminalWorkingDirectory(e.s.terminals[index]);
        String out(prRawStringView(&text));
        prRawStringReset(&text);
        return out;
    };
    // Runs frames until `done` returns true, or at most 5000 times. Returns the last result of `done`.
    auto settleUntil = [&](auto done) {
        for (uint32_t i = 0; i < 5000 && !done(); ++i) {
            prThreadSleep(2);
            e.settle();
        }
        return done();
    };

    e.run("terminal.new");
    REQUIRE(e.s.terminals.count() == 1);
    CHECK(settleUntil([&] { return prStringEndsWith(directory(0), "/workspace"); }));

    // The shell reports the workspace's sub directory.
    String sub = String::adopt(prFilePathAbsolute(format("{}/sub", folder), ""));
    PrRawString url = prUrlFromFilePath(sub);
    String report = format("\x1b]7;{}\x07", prRawStringView(&url));
    prRawStringReset(&url);
    prVtermWrite(prTerminalVtermLock(e.s.terminals[0]), PrByteView{prStringBytes(report), report.count()});
    prTerminalVtermUnlock(e.s.terminals[0]);
    e.run("terminal.new");
    REQUIRE(e.s.terminals.count() == 2);
    CHECK(settleUntil([&] { return directory(1) == sub; }));
}

TEST_CASE("a click on a terminal's notification shows the panel with that terminal, and gives it focus") {
    Editor e("hello");
    e.run("terminal.new");
    e.settle();
    e.run("terminal.new");
    e.settle();
    REQUIRE(e.s.terminals.count() == 2);
    CHECK(prTerminalNotificationTag(e.s.terminals[0]) != prTerminalNotificationTag(e.s.terminals[1]));
    e.run("terminal.toggle");
    REQUIRE(!e.s.terminalShown);
    PrAppEvent click{.kind = PrAppEventKind_Notification, .tag = prTerminalNotificationTag(e.s.terminals[0])};
    CHECK(handleEvent(e.s, click));
    e.settle();
    e.settle();
    CHECK(e.s.terminalShown);
    CHECK(e.s.terminal == 0);
    CHECK(prTerminalHasFocus(e.s.terminals[0]));
    PrAppEvent key{.kind = PrAppEventKind_KeyDown, .key = 'A'};
    CHECK_FALSE(handleEvent(e.s, key));
}

TEST_CASE("the commands of the shell's integration run on the terminal that shows, and do nothing without one") {
    Editor e("hello");
    const char* commands[] = {
        "terminal.copyCommandOutput", "terminal.selectCommandOutput", "terminal.previousPrompt", "terminal.nextPrompt",
    };
    for (const char* command : commands)
        e.run(command);
    CHECK(e.s.terminals.isEmpty());
    e.run("terminal.toggle");
    REQUIRE(e.s.terminals.count() == 1);
    for (const char* command : commands)
        e.run(command);
    CHECK(e.s.terminalShown);
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
    CHECK(prStringSlice(e.text(), 0, 7) == "xline 0");
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
    PrStringView ids[] = {"view.verticalRl", "view.verticalLr"};
    for (PrStringView id : ids) {
        bool reversed = id == "view.verticalRl";
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
    // Hebrew letters, and a line of Latin ones.
    Editor e("\xd7\x90\xd7\x91\xd7\x92\nabc");
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
    Array<PrLayoutHighlight> marks = e.firstRowMarks();
    REQUIRE(!marks.isEmpty());
    CHECK(marks[0].start == 0);
    CHECK(marks[0].end == 3);
    CHECK(marks[0].color.a != 0);
    // A comment opened at the start colors the whole line as one.
    e.type("/");
    e.type("/");
    marks = e.firstRowMarks();
    REQUIRE(marks.count() == 1);
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
    CHECK(e.firstRowMarks().isEmpty());
}

TEST_CASE("a large file streams in over frames, each piece colored as it shows, and an edit meanwhile keeps its undo") {
    String program;
    while (program.count() < (1u << 20))
        program.append("int value = 1; // a line of the program\n");
    Editor e(program, "big.c");
    // The first frame shows text, already colored, and the rest is still to come.
    CHECK(prTextEditBufferSize(prTextEditorDocumentBuffer(e.document())) != 0);
    CHECK(!e.firstRowMarks().isEmpty());
    // Typing goes in while the file loads, and the load keeps its undo.
    e.f.type("x");
    e.f.frame();
    for (uint32_t i = 0; i < 2000 && prTextEditorDocumentInfo(e.document()).loading; ++i) {
        e.f.frame();
        afterLayout(e.s);
    }
    REQUIRE_FALSE(prTextEditorDocumentInfo(e.document()).loading);
    CHECK(e.text() == format("x{}", program));
    e.key('Z', kCommand);
    CHECK(e.text() == program);
}

#endif

TEST_CASE("Save waits for the load to finish") {
    String program;
    while (program.count() < (1u << 20))
        program.append("int value = 1;\n");
    String folder = String(PrStringView(harness::testFolder()));
    String path = format("{}/big.c", folder);
    REQUIRE(prFileWrite(path, PrByteView{prStringBytes(program), program.count()}));
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

TEST_CASE("a click with the primary modifier on a terminal's reference to a file shows the file at its line") {
    String folder = String(PrStringView(harness::testFolder()));
    String path = format("{}/linked.txt", folder);
    String text = document(5);
    REQUIRE(prFileWrite(path, PrByteView{prStringBytes(text), text.count()}));
    Editor e("hello");
    e.run("terminal.toggle");
    e.settle();
    REQUIRE(e.s.terminals.count() == 1);
    PrTerminal* terminal = e.s.terminals[0];
    // The reference is printed on row 9, below the shell's prompt, in the form a compiler uses.
    String printed = format("\x1b[9;1H{}:3:4: note\r\n", path);
    prVtermWrite(prTerminalVtermLock(terminal), PrByteView{prStringBytes(printed), printed.count()});
    prTerminalVtermUnlock(terminal);
    e.settle();
    PrRect box = e.f.bounds(prTerminalNode(terminal));
    PrVec2 cell = prTerminalCellExtent(terminal);
    e.click(PrVec2{box.left + 2.5f * cell.x, box.top + 8.5f * cell.y}, kCommand);
    REQUIRE(e.document() != nullptr);
    // The editor normalizes the document's path.
    CHECK(prTextEditorDocumentPath(e.document()) == String::adopt(prFilePathAbsolute(path, "")));
    for (uint32_t i = 0; i < 100 && prTextEditorDocumentInfo(e.document()).loading; ++i)
        e.settle();
    CHECK(e.info().caret.line == 2);
    CHECK(e.info().caret.column == 3);
}
