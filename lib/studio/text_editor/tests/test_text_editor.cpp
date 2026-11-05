// The editor through its frames, as an application drives it: its documents, gutter and commands, and a language
// server's diagnostics, hover and definitions, from a server that the tests run.

#include "pr/command.h"
#include "pr/command_keymap.h"
#include "pr/command_kit.h"
#include "pr/file.h"
#include "pr/layout_query.h"
#include "pr/layout_tree.h"
#include "pr/process.h"
#include "pr/text_editor.h"
#include "pr/text_font.h"
#include "pr/thread.h"
#include "pr/time.h"
#include "pr/ui.h"
#if defined(TEXT_EDITOR_TESTS_HAVE_TREESITTER)
#include "pr/syntax_treesitter_c.h"
#endif

#include "studio/text_editor/src/text_editor_diff.h"
#include "tools/harness/kit_fixture.h"

#include "tools/harness/test.h"

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

// Creates a document of `text`, which the user may edit.
PrTextEditorDocument* textDocument(Editor& e, const std::string& text) {
    PrTextEditorDocumentDesc desc{.text = text};
    PrTextEditorDocument* document = nullptr;
    REQUIRE(prTextEditorDocumentCreate(e.editor, &desc, &document) == PrStatus_Ok);
    return document;
}

std::string textOf(PrTextEditorDocument* document) {
    PrStringView all = prTextEditBufferText(prTextEditorDocumentBuffer(document));
    return std::string(all.data, all.size);
}

double documentSize(PrTextEditorDocument* document) {
    return prTextViewInfo(prTextEditorDocumentView(document)).documentSize;
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
    PrByteView mapping{};
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

TEST_CASE("text editor: each pane of a comparison takes edits, and the pane that is clicked shows") {
    Editor e;
    PrTextEditorDocument* before = textDocument(e, "a\nb\nc\n");
    PrTextEditorDocument* after = textDocument(e, "a\nB\nc\n");
    PrTextEditorDiff* diff = prTextEditorDiffCreate(e.editor, before, after);
    e.settle();
    CHECK(e.shown() == after);
    // The old pane is the left half of the editor.
    e.f.click(PrVec2{200, 8});
    e.settle();
    CHECK(prTextEditorShownDiff(e.editor) == diff);
    CHECK(e.shown() == before);
    e.run("cursor.textStart");
    e.f.type("x");
    e.settle();
    CHECK(textOf(before) == "xa\nb\nc\n");
    // The changed lines meet, so they are one change.
    REQUIRE(prTextEditorDiffChangeCount(diff) == 1);
    CHECK(prTextEditorDiffChange(diff, 0).oldStart == 0);
    CHECK(prTextEditorDiffChange(diff, 0).oldCount == 2);
    // Back to the new pane.
    e.f.click(PrVec2{600, 8});
    e.settle();
    CHECK(e.shown() == after);
    prTextEditorDiffDestroy(diff);
}

TEST_CASE("text editor: a sync point aligns its lines, follows edits, and goes with a removed line") {
    Editor e;
    PrTextEditorDocument* before = textDocument(e, "a\nb\nc\n");
    PrTextEditorDocument* after = textDocument(e, "a\nx\ny\nb2\nc\n");
    PrTextEditorDiff* diff = prTextEditorDiffCreate(e.editor, before, after);
    e.settle();
    REQUIRE(prTextEditorDiffChangeCount(diff) == 1);
    uint32_t lines[] = {1, 3};
    REQUIRE(prTextEditorDiffAddSync(diff, lines));
    e.settle();
    REQUIRE(prTextEditorDiffChangeCount(diff) == 2);
    CHECK(prTextEditorDiffChange(diff, 1).oldStart == 1);
    CHECK(prTextEditorDiffChange(diff, 1).newStart == 3);
    CHECK(documentSize(before) == doctest::Approx(documentSize(after)));
    // A point that crosses another, or shares a line with it, is refused.
    uint32_t crossing[] = {2, 0};
    CHECK_FALSE(prTextEditorDiffAddSync(diff, crossing));
    uint32_t sharing[] = {1, 4};
    CHECK_FALSE(prTextEditorDiffAddSync(diff, sharing));
    uint32_t past[] = {9, 9};
    CHECK_FALSE(prTextEditorDiffAddSync(diff, past));

    // A line added above moves the point with its line.
    PrTextEditChange top{.start = 0, .end = 0, .text = "top\n"};
    REQUIRE(prTextEditBufferApply(prTextEditorDocumentBuffer(after), &top, 1, PrTextEditOrigin_Local));
    e.settle();
    uint32_t synced[2] = {};
    REQUIRE(prTextEditorDiffSyncs(diff, synced, 1) == 1);
    CHECK(synced[0] == 1);
    CHECK(synced[1] == 4);
    // Removing the old side's line removes the point.
    PrTextEditBuffer* old = prTextEditorDocumentBuffer(before);
    PrTextEditChange removal{.start = prTextEditBufferLineStart(old, 1), .end = prTextEditBufferLineStart(old, 2)};
    REQUIRE(prTextEditBufferApply(old, &removal, 1, PrTextEditOrigin_Local));
    e.settle();
    CHECK(prTextEditorDiffSyncs(diff, nullptr, 0) == 0);

    // The command syncs the carets' lines, and again removes the point.
    PrTextViewInfo info = prTextViewInfo(prTextEditorDocumentView(before));
    CHECK(info.caret.line == 0);
    e.run("diff.toggleSync");
    CHECK(prTextEditorDiffSyncs(diff, nullptr, 0) == 1);
    CHECK(e.textUnder(prTextEditorTextNode(e.editor)).find("\xE2\x87\x94 1") != std::string::npos);
    e.run("diff.toggleSync");
    CHECK(prTextEditorDiffSyncs(diff, nullptr, 0) == 0);
    prTextEditorDiffDestroy(diff);
}

TEST_CASE("text editor: a merge shows its sides beside the result, and resolves the result's conflicts") {
    Editor e;
    PrTextEditorDocument* base = textDocument(e, "a\nb\nc\nd\n");
    PrTextEditorDocument* left = textDocument(e, "A\nb\nc\nleft\n");
    PrTextEditorDocument* right = textDocument(e, "a\nb\nc\nright\n");
    PrTextEditorDocument* result = textDocument(e, "A\nb\nc\n<<<<<<<\nleft\n|||||||\nd\n=======\nright\n>>>>>>>\n");
    PrTextEditorDiffMergeDesc desc{.base = base, .left = left, .right = right, .result = result};
    PrTextEditorDiff* diff = prTextEditorDiffCreateMerge(e.editor, &desc);
    e.settle();
    CHECK(prTextEditorDiffIsMerge(diff));
    CHECK(e.shown() == result);
    CHECK(prTextEditorDiffConflictCount(diff) == 1);
    uint32_t conflicts = 0;
    uint32_t lefts = 0;
    for (uint32_t i = 0; i < prTextEditorDiffMergeChunkCount(diff); ++i) {
        PrTextDiffMergeKind kind = prTextEditorDiffMergeChunk(diff, i).kind;
        conflicts += kind == PrTextDiffMergeKind_Conflict ? 1 : 0;
        lefts += kind == PrTextDiffMergeKind_Left ? 1 : 0;
    }
    CHECK(conflicts == 1);
    CHECK(lefts == 1);
    // The panes are as long as each other: the sides leave room for the result's markers.
    CHECK(documentSize(left) == doctest::Approx(documentSize(result)));
    CHECK(documentSize(right) == doctest::Approx(documentSize(result)));

    e.run("merge.takeRight");
    CHECK(textOf(result) == "A\nb\nc\nright\n");
    CHECK(prTextEditorDiffConflictCount(diff) == 0);
    e.run("edit.undo");
    CHECK(prTextEditorDiffConflictCount(diff) == 1);
    e.run("cursor.textStart");
    e.run("merge.nextConflict");
    CHECK(e.caret().line == 3);
    e.run("merge.takeLeftThenRight");
    CHECK(textOf(result) == "A\nb\nc\nleft\nright\n");
    e.run("edit.undo");
    e.run("merge.takeBase");
    CHECK(textOf(result) == "A\nb\nc\nd\n");

    // Showing the base adds its pane, aligned with the others.
    e.run("merge.toggleBase");
    CHECK(prTextEditorDiffBaseShown(diff));
    CHECK(documentSize(base) == doctest::Approx(documentSize(result)));
    prTextEditorDiffDestroy(diff);
}

TEST_CASE("text editor: a merge tints each chunk in every side, and the result by where its lines came from") {
    // The left side adds a line at the top, and the right side changes f. Both change e alike, and conflict over V and
    // T, which meet, and over g and m. The result took the right side's V, T and g, and the left side's m.
    PrTextEditBuffer* base = prTextEditBufferCreate("a\nV1\nT30\nx\ng\ny\nf\nz\nm\nw\ne\n");
    PrTextEditBuffer* left = prTextEditBufferCreate("OS\na\nV11\nT30\nx\ngL\ny\nf\nz\nmL\nw\nE\n");
    PrTextEditBuffer* right = prTextEditBufferCreate("a\nV1\nT60\nx\ngR\ny\nfR\nz\nmR\nw\nE\n");
    PrTextEditBuffer* result = prTextEditBufferCreate("OS\na\nV1\nT60\nx\ngR\ny\nfR\nz\nmL\nw\nE\n");
    TextEditorDiffColors colors{};
    for (uint32_t role = 0; role < TextEditorDiffRole_Count; ++role)
        colors.lines[role] = prColor16FromColor(PrColor{.a = float(role + 1) / 16});
    colors.conflict = prColor16FromColor(PrColor{.a = 0.75f});
    TextEditorDiffModel model{};
    textEditorDiffModelInit(&model, TextEditorDiffKind_Merge);
    const PrTextEditBuffer* buffers[] = {base, left, right, result};
    REQUIRE(textEditorDiffModelUpdate(&model, buffers, nullptr, 0, colors, 19));
    auto tint = [&](TextEditorDiffSlot slot, uint32_t line) {
        return float(textEditorDiffLineColor(&model, slot, line).a);
    };
    float leftColor = float(colors.lines[TextEditorDiffRole_Left].a);
    float rightColor = float(colors.lines[TextEditorDiffRole_Right].a);
    float bothColor = float(colors.lines[TextEditorDiffRole_Base].a);
    float conflictColor = float(colors.conflict.a);
    // The right side's change of f shows in the left side, and the conflicts in both.
    CHECK(tint(TextEditorDiffSlot_Left, 7) == rightColor);
    CHECK(tint(TextEditorDiffSlot_Right, 6) == rightColor);
    CHECK(tint(TextEditorDiffSlot_Left, 0) == leftColor);
    CHECK(tint(TextEditorDiffSlot_Right, 1) == conflictColor);
    CHECK(tint(TextEditorDiffSlot_Left, 2) == conflictColor);
    CHECK(tint(TextEditorDiffSlot_Left, 11) == bothColor);
    CHECK(tint(TextEditorDiffSlot_Left, 1) == 0);
    // The result shows where each run came from, the line that equals the base included.
    CHECK(tint(TextEditorDiffSlot_Result, 0) == leftColor);
    CHECK(tint(TextEditorDiffSlot_Result, 2) == rightColor);
    CHECK(tint(TextEditorDiffSlot_Result, 3) == rightColor);
    CHECK(tint(TextEditorDiffSlot_Result, 5) == rightColor);
    CHECK(tint(TextEditorDiffSlot_Result, 7) == rightColor);
    CHECK(tint(TextEditorDiffSlot_Result, 9) == leftColor);
    CHECK(tint(TextEditorDiffSlot_Result, 11) == bothColor);
    CHECK(tint(TextEditorDiffSlot_Result, 1) == 0);
    textEditorDiffModelReset(&model);
    prTextEditBufferDestroy(result);
    prTextEditBufferDestroy(right);
    prTextEditBufferDestroy(left);
    prTextEditBufferDestroy(base);
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

TEST_CASE("text editor: a debugging session stops at a breakpoint, steps, shows its stack and variables, and ends" *
        doctest::skip(!prProcessBackendIsBuilt(PrProcessBackend_Default))) {
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

TEST_CASE("text editor: a language server's diagnostics, hover and definition reach the document" *
        doctest::skip(!prProcessBackendIsBuilt(PrProcessBackend_Default))) {
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
