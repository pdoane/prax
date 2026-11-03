// The editor through its frames, as an application drives it: its documents, gutter and commands, and a language
// server's diagnostics, hover and definitions, from a server that the tests run.

#include "pr/command.h"
#include "pr/command_keymap.h"
#include "pr/command_kit.h"
#include "pr/file.h"
#include "pr/layout_query.h"
#include "pr/layout_tree.h"
#include "pr/text_editor.h"
#include "pr/text_font.h"
#include "pr/thread.h"
#include "pr/time.h"
#include "pr/ui.h"
#if defined(TEXT_EDITOR_TESTS_HAVE_TREESITTER)
#include "pr/syntax_treesitter_c.h"
#endif

#include "tools/harness/kit_fixture.h"

#include "doctest/doctest.h"

#include <string>
#include <vector>

extern "C" int prTestTextEditorCApi(void);

using namespace harness;

namespace pr_impl {

namespace {

// How long a test waits for a language server, in milliseconds.
constexpr double kPatience = 10000;

// An editor in a kit of its own, at 800 by 600 px.
struct Editor {
    KitFixture f;
    PrCommandRegistry* commands = prCommandRegistryCreate();
    PrCommandKeymap* keymap = prCommandKeymapCreate();
    PrSyntaxRegistry* languages = prSyntaxRegistryCreate();
    PrProcessLauncher* launcher = prProcessLauncherCreate(PrProcessBackend_Default);
    PrTextFont* font = nullptr;
    PrTextEditor* editor = nullptr;

    Editor() {
#if defined(TEXT_EDITOR_TESTS_HAVE_TREESITTER)
        prSyntaxRegistryAdd(languages, prSyntaxTreeSitterC());
#endif
        PrFontFamily mono{.generic = PrFontGeneric_Monospace};
        PrFontStyle style = prFontStyleDefault();
        style.size = 13;
        PrTextFontDesc fontDesc{.set = f.fonts, .families = &mono, .familyCount = 1, .style = style};
        font = prTextFontCreate(&fontDesc);
        PrTextEditorDesc desc{
            .kit = f.kit,
            .font = font,
            .launcher = launcher,
            .languages = languages,
            .commands = commands,
            .keymap = keymap,
        };
        editor = prTextEditorCreate(&desc);
        f.build = [this](PrLayoutNode root) {
            prCommandScope(f.ui, root, commands, keymap, "");
            prTextEditorFrame(editor, root, PrVec2{800, 600});
        };
    }

    ~Editor() {
        prTextEditorDestroy(editor);
        prProcessLauncherDestroy(launcher);
        prTextFontRelease(font);
        prSyntaxRegistryDestroy(languages);
        prCommandKeymapDestroy(keymap);
        prCommandRegistryDestroy(commands);
    }

    // Runs frames until the editor has nothing more to show.
    void settle() {
        f.frame();
        for (uint32_t i = 0; i < 32 && prTextEditorLaidOut(editor); ++i)
            f.frame();
    }

    // Runs frames until `done` holds, or fails once the patience runs out.
    template <typename Done>
    void waitFor(Done done) {
        double end = prTimeMonotonicMilliseconds() + kPatience;
        while (!done()) {
            REQUIRE(prTimeMonotonicMilliseconds() < end);
            prThreadSleep(2);
            settle();
        }
    }

    void run(const char* id) {
        REQUIRE(prCommandRegistryRun(commands, id, nullptr, nullptr) == PrStatus_Ok);
        settle();
    }

    PrTextEditorDocument* shown() { return prTextEditorShown(editor); }

    std::string text() {
        PrStringView all = prTextEditBufferText(prTextEditorDocumentBuffer(shown()));
        return std::string(all.data, all.size);
    }

    PrTextEditPoint caret() { return prTextViewInfo(prTextEditorDocumentView(shown())).caret; }

    std::string status() {
        PrStringView status = prTextEditorStatus(editor);
        return std::string(status.data, status.size);
    }

    // Returns the text of every text node under `node`, in order.
    std::string textUnder(PrLayoutNode node) {
        std::string all;
        for (PrLayoutNode child = prLayoutNodeFirstChild(f.tree, node); child != kPrLayoutNodeNone;
                child = prLayoutNodeNextSibling(f.tree, child)) {
            if (prLayoutNodeKind(f.tree, child) == PrLayoutNodeKind_Text) {
                PrStringView text = prLayoutNodeText(f.tree, child);
                all.append(text.data, text.size);
            } else {
                all += textUnder(child);
            }
        }
        return all;
    }
};

// Writes `text` to a file of `name` in a folder of the tests' own, and returns its path.
std::string writeFile(const std::string& name, const std::string& text) {
    PrRawString cache = prFileCacheDirectory();
    std::string folder = std::string(prRawStringView(&cache).data, prRawStringView(&cache).size) + "/prax-tests/text_editor";
    prRawStringReset(&cache);
    REQUIRE(prFileCreateDirectory(folder));
    std::string path = folder + "/" + name;
    REQUIRE(prFileWrite(path, PrByteView{reinterpret_cast<const uint8_t*>(text.data()), text.size()}));
    return path;
}

PrTextEditorDocument* open(Editor& e, const std::string& path) {
    PrTextEditorDocumentDesc desc{.path = path};
    PrTextEditorDocument* document = nullptr;
    REQUIRE(prTextEditorDocumentCreate(e.editor, &desc, &document) == PrStatus_Ok);
    e.settle();
    return document;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("text editor: the public headers compile as C") {
    CHECK(prTestTextEditorCApi() == 0);
}

TEST_CASE("text editor: a document of text shows, takes focus, and numbers its lines in the gutter") {
    Editor e;
    PrTextEditorDocumentDesc desc{.text = "one\ntwo\nthree"};
    PrTextEditorDocument* document = nullptr;
    REQUIRE(prTextEditorDocumentCreate(e.editor, &desc, &document) == PrStatus_Ok);
    e.settle();
    CHECK(e.shown() == document);
    e.f.type("x");
    e.settle();
    CHECK(e.text() == "xone\ntwo\nthree");
    std::string shown = e.textUnder(prTextEditorTextNode(e.editor));
    CHECK(shown.find("1xone2two3three") != std::string::npos);
    CHECK(prTextEditorDocumentInfo(document).edited);

    PrTextEditorOptions options = prTextEditorOptions(e.editor);
    options.lineNumbers = false;
    prTextEditorSetOptions(e.editor, &options);
    e.settle();
    CHECK(e.textUnder(prTextEditorTextNode(e.editor)).find("xonetwothree") != std::string::npos);
}

TEST_CASE("text editor: a file opens, saves with its line endings, and a missing file is NotFound") {
    Editor e;
    std::string path = writeFile("crlf.txt", "a\r\nb\r\n");
    PrTextEditorDocument* document = open(e, path);
    CHECK(e.text() == "a\nb\n");
    CHECK(prTextEditorDocumentInfo(document).crlf);
    e.f.type("z");
    e.settle();
    e.run("file.save");
    CHECK(e.status() == "saved");
    PrFileMapping mapping{};
    REQUIRE(prFileMap(path, &mapping));
    CHECK(std::string(reinterpret_cast<const char*>(mapping.data), mapping.size) == "za\r\nb\r\n");
    prFileUnmap(&mapping);
    CHECK_FALSE(prTextEditorDocumentInfo(document).edited);

    PrTextEditorDocumentDesc missing{.path = "/prax/no/such/file.txt"};
    PrTextEditorDocument* none = document;
    CHECK(prTextEditorDocumentCreate(e.editor, &missing, &none) == PrStatus_NotFound);
    CHECK(none == nullptr);
}

TEST_CASE("text editor: a comparison aligns its sides, tints what changed, and scrolls them together") {
    Editor e;
    std::string old;
    std::string young;
    // Line 6 is long on both sides, so that the sides scroll along their lines.
    std::string wide(400, 'x');
    for (uint32_t i = 0; i < 400; ++i) {
        std::string tail = i == 6 ? wide : std::string();
        old += "line " + std::to_string(i) + tail + "\n";
        young += (i == 1 ? std::string("LINE 1") : "line " + std::to_string(i) + tail) + "\n";
        if (i == 3)
            young += "added a\nadded b\n";
    }
    PrTextEditorDocumentDesc oldDesc{.text = old, .readOnly = true};
    PrTextEditorDocumentDesc youngDesc{.text = young};
    PrTextEditorDocument* before = nullptr;
    PrTextEditorDocument* after = nullptr;
    REQUIRE(prTextEditorDocumentCreate(e.editor, &oldDesc, &before) == PrStatus_Ok);
    REQUIRE(prTextEditorDocumentCreate(e.editor, &youngDesc, &after) == PrStatus_Ok);
    PrTextEditorDiff* diff = prTextEditorDiffCreate(e.editor, before, after);
    e.settle();
    CHECK(prTextEditorShownDiff(e.editor) == diff);
    CHECK(e.shown() == after);
    REQUIRE(prTextEditorDiffChangeCount(diff) == 2);
    PrTextDiffChange changed = prTextEditorDiffChange(diff, 0);
    CHECK(changed.oldStart == 1);
    CHECK(changed.oldCount == 1);
    CHECK(changed.newCount == 1);
    PrTextDiffChange added = prTextEditorDiffChange(diff, 1);
    CHECK(added.oldCount == 0);
    CHECK(added.newStart == 4);
    CHECK(added.newCount == 2);
    // The old side leaves room for the added lines, so the sides are as long.
    PrTextViewInfo oldInfo = prTextViewInfo(prTextEditorDocumentView(before));
    PrTextViewInfo newInfo = prTextViewInfo(prTextEditorDocumentView(after));
    CHECK(oldInfo.documentSize == doctest::Approx(newInfo.documentSize));

    // The old side refuses its user's edits.
    PrTextEditView* oldEdit = prTextViewEdit(prTextEditorDocumentView(before));
    CHECK_FALSE(prTextEditViewInsert(oldEdit, "x", PrTextEditOrigin_Local, PrTextEditMerge_None));

    // The changed line's row is tinted, and the bytes that changed within it are marked.
    PrLayoutNode port = prTextEditorTextNode(e.editor);
    // The sides do not wrap, so their rows lie in a sheet.
    PrLayoutNode sheet = prLayoutNodeFirstChild(e.f.tree, port);
    PrLayoutNode second = prLayoutNodeNextSibling(e.f.tree, prLayoutNodeFirstChild(e.f.tree, sheet));
    PrLayoutStyle filled = prLayoutStyleRecord(e.f.tree, prLayoutNodeStyle(e.f.tree, second));
    CHECK(prLayoutDecorationStyleRecord(e.f.tree, filled.groups[PrLayoutGroup_Decoration]).background.a != 0);
    PrLayoutNode block = prLayoutNodeNextSibling(e.f.tree, prLayoutNodeFirstChild(e.f.tree, second));
    PrLayoutHighlight marks[4] = {};
    REQUIRE(prLayoutNodeHighlights(e.f.tree, prLayoutNodeFirstChild(e.f.tree, block), marks, 4) >= 1);
    CHECK(marks[0].start == 0);
    CHECK(marks[0].end == 4);
    CHECK(marks[0].background.a != 0);

    // Scrolling the new side scrolls the old with it.
    prTextViewScrollTo(prTextEditorDocumentView(after), 1000);
    e.settle();
    CHECK(prTextViewInfo(prTextEditorDocumentView(before)).offset == doctest::Approx(1000.0));
    // And along the lines.
    prTextViewScrollTo(prTextEditorDocumentView(after), 0);
    e.settle();
    prTextViewScrollBy(prTextEditorDocumentView(after), PrVec2{120, 0});
    e.settle();
    double along = prTextViewInfo(prTextEditorDocumentView(after)).inlineOffset;
    CHECK(along == doctest::Approx(120.0));
    CHECK(prTextViewInfo(prTextEditorDocumentView(before)).inlineOffset == doctest::Approx(along));

    e.run("diff.nextChange");
    CHECK(e.caret().line == 1);
    e.run("diff.nextChange");
    CHECK(e.caret().line == 4);

    prTextEditorShow(e.editor, after);
    CHECK(prTextEditorShownDiff(e.editor) == nullptr);
    prTextEditorDiffDestroy(diff);
}

TEST_CASE("text editor: breakpoints follow their lines, and go with a removed line") {
    Editor e;
    PrTextEditorDocumentDesc desc{.text = "a\nb\nc\nd\n"};
    PrTextEditorDocument* document = nullptr;
    REQUIRE(prTextEditorDocumentCreate(e.editor, &desc, &document) == PrStatus_Ok);
    e.settle();
    prTextEditorDocumentToggleBreakpoint(document, 2);
    prTextEditorDocumentToggleBreakpoint(document, 1);
    uint32_t lines[4] = {};
    REQUIRE(prTextEditorDocumentBreakpoints(document, lines, 4) == 2);
    CHECK(lines[0] == 1);
    CHECK(lines[1] == 2);
    // A line inserted above moves both, and removing a breakpoint's line removes it.
    e.f.type("new\n");
    e.settle();
    REQUIRE(prTextEditorDocumentBreakpoints(document, lines, 4) == 2);
    CHECK(lines[0] == 2);
    CHECK(lines[1] == 3);
    PrTextEditChange removal{
        .start = prTextEditBufferLineStart(prTextEditorDocumentBuffer(document), 1),
        .end = prTextEditBufferLineStart(prTextEditorDocumentBuffer(document), 3),
    };
    REQUIRE(prTextEditBufferApply(prTextEditorDocumentBuffer(document), &removal, 1, PrTextEditOrigin_Local));
    REQUIRE(prTextEditorDocumentBreakpoints(document, lines, 4) == 1);
    CHECK(lines[0] == 1);
    prTextEditorDocumentToggleBreakpoint(document, 1);
    CHECK(prTextEditorDocumentBreakpoints(document, nullptr, 0) == 0);
}

TEST_CASE("text editor: a debugging session stops at a breakpoint, steps, shows its stack and variables, and ends") {
    Editor e;
    PrStringView arguments[] = {TEXT_EDITOR_TESTS_DIR "/fake_debug_adapter.py"};
    PrTextEditorDebuggerDesc debugger{
        .program = TEXT_EDITOR_TESTS_PYTHON,
        .arguments = arguments,
        .argumentCount = 1,
        .adapterId = "fake",
        .launch = "{\"program\":\"/fake/program\"}",
    };
    prTextEditorSetDebugger(e.editor, &debugger);
    std::string path = writeFile("debugged.c", "int main(void) {\n    int x = 1;\n    x += 1;\n    return x;\n}\n");
    PrTextEditorDocument* document = open(e, path);
    // A breakpoint on the caret's line, line 1.
    prTextViewRun(prTextEditorDocumentView(document), PrTextEditCommand_Down, PrTextEditSelectionMove_Collapse);
    e.run("debug.toggleBreakpoint");
    CHECK(prTextEditorDocumentBreakpoints(document, nullptr, 0) == 1);
    CHECK(e.textUnder(prTextEditorTextNode(e.editor)).find("\xE2\x97\x8F 2") != std::string::npos);
    // The caret moves away, so that the stop brings it back.
    prTextViewRun(prTextEditorDocumentView(document), PrTextEditCommand_TextEnd, PrTextEditSelectionMove_Collapse);
    e.settle();

    e.run("debug.continue");
    e.waitFor([&] { return prTextEditorDebugInfo(e.editor).state == PrTextEditorDebugState_Stopped; });
    e.waitFor([&] { return e.textUnder(e.f.root).find("steps = 0") != std::string::npos; });
    PrTextEditorDebugInfo info = prTextEditorDebugInfo(e.editor);
    CHECK(info.path == PrStringView(path));
    CHECK(info.line == 1);
    CHECK(e.caret().line == 1);
    std::string panel = e.textUnder(e.f.root);
    CHECK(panel.find("stopped: breakpoint") != std::string::npos);
    CHECK(panel.find("main  debugged.c:2") != std::string::npos);
    CHECK(panel.find("hello") != std::string::npos);
    // The stopped line's row is tinted.
    PrLayoutNode port = prTextEditorTextNode(e.editor);
    PrLayoutNode second = prLayoutNodeNextSibling(e.f.tree, prLayoutNodeFirstChild(e.f.tree, port));
    PrLayoutStyle filled = prLayoutStyleRecord(e.f.tree, prLayoutNodeStyle(e.f.tree, second));
    CHECK(prLayoutDecorationStyleRecord(e.f.tree, filled.groups[PrLayoutGroup_Decoration]).background.a != 0);

    e.run("debug.stepOver");
    e.waitFor([&] { return e.textUnder(e.f.root).find("steps = 1") != std::string::npos; });
    CHECK(prTextEditorDebugInfo(e.editor).line == 2);
    CHECK(e.caret().line == 2);

    e.run("debug.continue");
    e.waitFor([&] { return prTextEditorDebugInfo(e.editor).state == PrTextEditorDebugState_Ended; });
    e.waitFor([&] { return e.textUnder(e.f.root).find("exit code 0") != std::string::npos; });
    CHECK(e.textUnder(e.f.root).find("bye") != std::string::npos);
    e.run("debug.stop");
    CHECK(prTextEditorDebugInfo(e.editor).state == PrTextEditorDebugState_None);
}

TEST_CASE("text editor: Compare with Saved compares a document with its file, and closing it shows the document") {
    Editor e;
    std::string path = writeFile("compared.txt", "one\ntwo\nthree\n");
    PrTextEditorDocument* document = open(e, path);
    e.f.type("zero\n");
    e.settle();
    e.run("diff.compareSaved");
    e.settle();
    PrTextEditorDiff* diff = prTextEditorShownDiff(e.editor);
    REQUIRE(diff != nullptr);
    CHECK(prTextEditorDocumentCount(e.editor) == 1);
    REQUIRE(prTextEditorDiffChangeCount(diff) == 1);
    CHECK(prTextEditorDiffChange(diff, 0).oldCount == 0);
    CHECK(prTextEditorDiffChange(diff, 0).newCount == 1);
    // Edits to the document reach the comparison: the caret was at the start of "one", which now changes too.
    e.f.type("and ");
    e.settle();
    REQUIRE(prTextEditorDiffChangeCount(diff) == 1);
    CHECK(prTextEditorDiffChange(diff, 0).oldCount == 1);
    CHECK(prTextEditorDiffChange(diff, 0).newCount == 2);
    CHECK(e.text() == "zero\nand one\ntwo\nthree\n");
    e.run("diff.close");
    CHECK(prTextEditorShownDiff(e.editor) == nullptr);
    CHECK(e.shown() == document);
    CHECK(prTextEditorDocumentCount(e.editor) == 1);
}

#if defined(TEXT_EDITOR_TESTS_HAVE_TREESITTER)

TEST_CASE("text editor: a language server's diagnostics, hover and definition reach the document") {
    Editor e;
    PrStringView languages[] = {"c"};
    PrStringView arguments[] = {TEXT_EDITOR_TESTS_DIR "/fake_language_server.py"};
    PrTextEditorServerDesc server{
        .languages = languages,
        .languageCount = 1,
        .program = TEXT_EDITOR_TESTS_PYTHON,
        .arguments = arguments,
        .argumentCount = 1,
    };
    prTextEditorAddServer(e.editor, &server);
    // The emoji counts two units in UTF-16, which the server counts in.
    std::string path = writeFile("served.c", "int x = y;\n\xF0\x9F\x98\x80  z;\n");
    PrTextEditorDocument* document = open(e, path);
    e.waitFor([&] { return prTextEditorDocumentInfo(document).errors == 1; });
    CHECK(prTextEditorDocumentInfo(document).server == PrTextEditorServerState_Running);
    CHECK(e.status() == "error: bad start");

    // The diagnostic underlines its text in the first row.
    PrLayoutNode row = prLayoutNodeFirstChild(e.f.tree, prTextEditorTextNode(e.editor));
    PrLayoutNode block = prLayoutNodeNextSibling(e.f.tree, prLayoutNodeFirstChild(e.f.tree, row));
    PrLayoutNode text = prLayoutNodeFirstChild(e.f.tree, block);
    std::vector<PrLayoutHighlight> marks(prLayoutNodeHighlights(e.f.tree, text, nullptr, 0));
    prLayoutNodeHighlights(e.f.tree, text, marks.data(), uint32_t(marks.size()));
    bool underlined = false;
    for (const PrLayoutHighlight& mark : marks)
        underlined = underlined || (mark.start == 0 && mark.end == 3 && mark.underline.a != 0);
    CHECK(underlined);

    e.run("language.hover");
    e.waitFor([&] { return e.textUnder(e.f.root).find("hover at 0:0") != std::string::npos; });

    // The definition is on line 1 at UTF-16 unit 4, which is past the emoji's four bytes and two spaces.
    e.run("language.definition");
    e.waitFor([&] { return e.caret().line == 1; });
    CHECK(e.caret().column == 6);

    e.run("language.nextProblem");
    CHECK(e.caret().line == 0);
    CHECK(e.caret().column == 0);
}

#endif
