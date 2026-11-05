// Prax Merge: a comparison of two files, or a merge of two files that each changed a base, in Studio's window.
//
// main() is the composition root: Studio's text editor (pr/text_editor.h) in Studio's window (pr/studio_window.h), with
// a menu bar of its commands and a status line.
//
// Usage: merge [--trace] old new
//        merge [--trace] base left right result
//
// A comparison shows the old file beside the new, each editable. A merge shows the left side, the result and the right
// side, and the base on request (Merge > Show Base). The result starts as the merge of the sides, each conflict between
// diff3 markers, and is written to the result's file before the window opens. The merge exits with status 0 if the
// result's file marks no conflicts once the window closes, and with status 1 otherwise, so that a tool that trusts the
// exit status keeps an unresolved merge unresolved. --trace prints the times of each frame that takes long.
//
// Git runs it as a difftool and a mergetool with:
//
//     [difftool "prax"]
//         cmd = merge "$LOCAL" "$REMOTE"
//     [mergetool "prax"]
//         cmd = merge "$BASE" "$LOCAL" "$REMOTE" "$MERGED"
//         trustExitCode = true
//
// Perforce runs it as P4DIFF, with the old file and the new, and as P4MERGE, with the base, theirs, yours and the
// result, so that the left side is theirs.

#include "pr/assert.h"
#include "pr/command.h"
#include "pr/command_keymap.h"
#include "pr/command_kit.h"
#include "pr/file.h"
#include "pr/kit_menu.h"
#include "pr/kit_text.h"
#include "pr/layout_query.h"
#include "pr/print.h"
#include "pr/string.h"
#include "pr/string_view.h"
#include "pr/studio_window.h"
#include "pr/syntax.h"
#include "pr/text_editor.h"
#include "pr/text_font.h"
#include "pr/ui.h"
#if defined(MERGE_HAS_TREESITTER)
#include "pr/syntax_treesitter_languages.h"
#endif

#include "apps/merge/merge.h"

using namespace merge;

namespace {

// The height of the status line, in px, and the times of a frame that --trace reports, in milliseconds.
constexpr float kStatusHeight = 24;
constexpr double kTraceSlowFrame = 16;
constexpr double kTraceLongGap = 100;

// A menu of the menu bar: the ids of its commands in order, with an empty id for a separator.
struct Menu {
    PrStringView title;
    Array<PrStringView> items;
};

// The merge in its window.
struct MergeApp {
    Invocation invocation;

    PrKit* kit = nullptr;
    PrUi* ui = nullptr;
    PrApp* app = nullptr;
    PrTextFont* font = nullptr;
    PrSyntaxRegistry* languages = nullptr;
    PrCommandRegistry* commands = nullptr;
    PrCommandKeymap* keymap = nullptr;
    PrTextEditor* editor = nullptr;
    PrTextEditorDocument* result = nullptr;   // of a merge

    Array<Menu> menus;
    PrLayoutNode menuBar = kPrLayoutNodeNone;
    float menuHeight = 0;                     // of the menu bar, at the last layout
    bool menuOpen = false;                    // a menu was open in the last round
};

MergeApp& appOf(void* context) {
    return *static_cast<MergeApp*>(context);
}

// A menu of `items`.
template <uint32_t N>
Menu menuOf(PrStringView title, const PrStringView (&items)[N]) {
    return Menu{title, Array<PrStringView>(items)};
}

// Adds one of the merge's own commands, bound to `chord` unless it is null.
void addCommand(MergeApp& m, const char* id, const char* title, const char* chord, PrCommandRunFn run,
        PrCommandStateFn state = nullptr) {
    PrCommandDesc desc{.id = id, .title = title, .category = "Merge", .run = run, .state = state, .context = &m};
    prCommandRegistryAdd(m.commands, &desc);
    PrCommandChord parsed{};
    if (chord != nullptr && prCommandChordParse(chord, &parsed))
        prCommandKeymapBind(m.keymap, parsed, id, "", PrCommandBindingKind_Normal);
}

// Returns true if the merge's result marks no conflicts, as of the last frame.
bool resolved(const MergeApp& m) {
    PrTextEditorDiff* diff = prTextEditorShownDiff(m.editor);
    return m.result != nullptr && diff != nullptr && prTextEditorDiffConflictCount(diff) == 0;
}

// Makes the editor with the languages and the commands of the merge, and the menus.
void openEditor(MergeApp& m) {
    m.commands = prCommandRegistryCreate();
    m.keymap = prCommandKeymapCreate();
    m.languages = prSyntaxRegistryCreate();
#if defined(MERGE_HAS_TREESITTER)
    prSyntaxTreeSitterAddLanguages(m.languages);
#endif
    PrTextEditorDesc desc{
        .kit = m.kit,
        .font = m.font,
        .app = m.app,
        .languages = m.languages,
        .commands = m.commands,
        .keymap = m.keymap,
    };
    m.editor = prTextEditorCreate(&desc);
    addCommand(m, "app.quit", "Quit", "Cmd Q", [](void* c, const PrCommandArgs*) { prAppStop(appOf(c).app); });
    addCommand(m, "merge.finish", "Save and Finish", "Cmd Enter", [](void* c, const PrCommandArgs*) {
        MergeApp& app = appOf(c);
        if (resolved(app) && prTextEditorDocumentSave(app.result) == PrStatus_Ok)
            prAppStop(app.app);
    }, [](void* c) -> PrCommandStateFlags { return resolved(appOf(c)) ? 0 : PrCommandStateFlag_Disabled; });
    m.menus.push(menuOf("File", {"file.save", "merge.finish", "", "app.quit"}));
    m.menus.push(menuOf("Edit", {
        "edit.undo", "edit.redo", "", "edit.cut", "edit.copy", "edit.paste", "", "edit.selectAll",
    }));
    m.menus.push(menuOf("Compare", {
        "diff.nextChange", "diff.previousChange", "", "diff.toggleSync", "diff.clearSyncs",
    }));
    if (m.invocation.task == Task_Merge) {
        m.menus.push(menuOf("Merge", {
            "merge.nextConflict", "merge.previousConflict", "", "merge.takeLeft", "merge.takeRight",
            "merge.takeLeftThenRight", "merge.takeRightThenLeft", "merge.takeBase", "", "merge.toggleBase",
        }));
    }
}

void closeEditor(MergeApp& m) {
    prTextEditorDestroy(m.editor);
    prSyntaxRegistryDestroy(m.languages);
    prCommandKeymapDestroy(m.keymap);
    prCommandRegistryDestroy(m.commands);
    prTextFontRelease(m.font);
    m.editor = nullptr;
}

// Opens a file as a document, which the user may edit unless `readOnly`.
PrTextEditorDocument* openFile(MergeApp& m, PrStringView path, bool readOnly) {
    PrTextEditorDocumentDesc desc{.path = path, .readOnly = readOnly};
    PrTextEditorDocument* document = nullptr;
    if (prTextEditorDocumentCreate(m.editor, &desc, &document) != PrStatus_Ok)
        printError("merge: cannot read {}\n", path);
    return document;
}

// Opens the documents of the invocation and shows their comparison. Returns false if a file cannot be read or written.
bool openTask(MergeApp& m) {
    const Invocation& task = m.invocation;
    if (task.task == Task_Compare) {
        PrTextEditorDocument* old = openFile(m, task.old, false);
        PrTextEditorDocument* young = old != nullptr ? openFile(m, task.young, false) : nullptr;
        if (young == nullptr)
            return false;
        prTextEditorDiffCreate(m.editor, old, young);
        return true;
    }
    MergedText merged;
    if (!mergeFiles(task.base, task.left, task.right, &merged)) {
        printError("merge: cannot read {}, {} or {}\n", task.base, task.left, task.right);
        return false;
    }
    PrStringView text = merged.text;
    if (!prFileWrite(task.result, PrByteView{reinterpret_cast<const uint8_t*>(text.data), text.size})) {
        printError("merge: cannot write {}\n", task.result);
        return false;
    }
    PrTextEditorDocument* base = openFile(m, task.base, true);
    PrTextEditorDocument* left = base != nullptr ? openFile(m, task.left, true) : nullptr;
    PrTextEditorDocument* right = left != nullptr ? openFile(m, task.right, true) : nullptr;
    m.result = right != nullptr ? openFile(m, task.result, false) : nullptr;
    if (m.result == nullptr)
        return false;
    PrTextEditorDiffMergeDesc desc{.base = base, .left = left, .right = right, .result = m.result};
    prTextEditorDiffCreateMerge(m.editor, &desc);
    return true;
}

// The status line: the shown document's file and state, the caret, the conflicts that remain, and what the editor last
// had to say.
String statusLine(MergeApp& m) {
    PrTextEditorDocument* document = prTextEditorShown(m.editor);
    if (document == nullptr)
        return "no document";
    PrTextEditorDocumentInfo info = prTextEditorDocumentInfo(document);
    PrTextViewInfo view = prTextViewInfo(prTextEditorDocumentView(document));
    PrStringView state = info.edited ? "edited" : "saved";
    if (info.loading)
        state = "loading";
    String line = format("{}   {}  Ln {}, Col {}", prTextEditorDocumentPath(document), state, view.caret.line + 1,
        view.caret.column + 1);
    PrTextEditorDiff* diff = prTextEditorShownDiff(m.editor);
    if (diff != nullptr && prTextEditorDiffIsMerge(diff)) {
        uint32_t conflicts = prTextEditorDiffConflictCount(diff);
        formatTo(line, "   {} {}", conflicts, conflicts == 1 ? "conflict" : "conflicts");
    }
    PrStringView said = prTextEditorStatus(m.editor);
    if (!said.isEmpty())
        formatTo(line, "   {}", said);
    return line;
}

// Declares a round: the menu bar, the editor below it, and the status line.
void build(void* context, PrStudioWindow*, PrLayoutNode root, PrVec2 extent) {
    MergeApp& m = appOf(context);
    prCommandScope(m.ui, root, m.commands, m.keymap, "");
    m.menuBar = prKitMenuBar(m.kit, root, {});
    bool menuOpen = false;
    for (const Menu& menu : m.menus) {
        PrLayoutNode title = prKitMenuBarItem(m.kit, m.menuBar, {.text = menu.title});
        PrLayoutNode popover = prKitPopover(m.kit, title, {}, {});
        if (popover == kPrLayoutNodeNone)
            continue;
        menuOpen = true;
        PrLayoutNode list = prKitMenu(m.kit, popover, {}, {.minWidth = 240});
        for (PrStringView id : menu.items) {
            if (id.isEmpty())
                prKitMenuSeparator(m.kit, list, {});
            else
                prCommandMenuItem(m.kit, list, m.commands, m.keymap, id, {});
        }
    }
    float height = extent.y - kStatusHeight - m.menuHeight;
    prTextEditorFrame(m.editor, root, PrVec2{extent.x, height > 0 ? height : 0});
    // A command runs on the text, so typing goes on there once a menu closes.
    PrLayoutNode text = prTextEditorTextNode(m.editor);
    if (text != kPrLayoutNodeNone && m.menuOpen && !menuOpen)
        prUiSetFocus(m.ui, text);
    m.menuOpen = menuOpen;
    prKitLabel(m.kit, root, {.text = statusLine(m)}, {.role = PrKitTextRole_BodySmall, .tone = PrKitTone_Muted});
}

bool laidOut(void* context, PrStudioWindow*) {
    MergeApp& m = appOf(context);
    bool again = prTextEditorLaidOut(m.editor);
    PrRect bar = prLayoutRectToPx(prLayoutNodeBounds(prUiTree(m.ui), m.menuBar, PrLayoutCoordinates_Viewport));
    if (bar.bottom - bar.top != m.menuHeight) {
        m.menuHeight = bar.bottom - bar.top;
        again = true;
    }
    return again;
}

bool open(void* context, PrStudioWindow* window) {
    MergeApp& m = appOf(context);
    PrFontFamily mono{.generic = PrFontGeneric_Monospace};
    PrFontStyle style = prFontStyleDefault();
    style.size = 13;
    PrTextFontDesc font{.set = prStudioWindowFonts(window), .families = &mono, .familyCount = 1, .style = style};
    m.font = prTextFontCreate(&font);
    m.kit = prStudioWindowKit(window);
    m.ui = prStudioWindowUi(window);
    m.app = prStudioWindowApp(window);
    openEditor(m);
    return openTask(m);
}

void trace(void*, const PrStudioWindowFrameTimes* t) {
    if (t->total <= kTraceSlowFrame && t->gap <= kTraceLongGap && t->inputWait <= kTraceLongGap)
        return;
    printError("frame {:.0f}x{:.0f}: total {:.1f} (wait {:.1f}, rounds {} {:.1f}, scene {:.1f}, play {:.1f}, "
        "render {:.1f}), gap {:.1f}, input waited {:.1f}\n", t->extent.x, t->extent.y, t->total, t->wait, t->rounds,
        t->build, t->scene, t->play, t->render, t->gap, t->inputWait);
}

} // namespace

int main(int argc, char** argv) {
    prCrashHandler();
    static MergeApp m;
    Array<PrStringView> arguments;
    for (int i = 1; i < argc; ++i)
        arguments.push(argv[i]);
    if (!parseArguments(arguments.span().asConst(), &m.invocation)) {
        printError("{}", kUsage);
        return 2;
    }
    bool merging = m.invocation.task == Task_Merge;
    PrStringView title = "Prax Merge: Compare";
    if (merging)
        title = "Prax Merge";
    PrStudioWindowDesc desc{
        .title = title,
        .extent = {1400, 900},
        .fontDirectory = PR_FONT_BUNDLE_DIR,
        .open = open,
        .build = build,
        .laidOut = laidOut,
        .close = [](void* context, PrStudioWindow*) {
            if (appOf(context).editor != nullptr)
                closeEditor(appOf(context));
        },
        .trace = m.invocation.trace ? trace : nullptr,
        .context = &m,
    };
    PrStatus status = prStudioWindowRun(&desc);
    if (status == PrStatus_NotFound)
        printError("merge: the fonts under {} could not be installed\n", PR_FONT_BUNDLE_DIR);
    else if (status == PrStatus_NotSupported)
        printError("merge: no GPU device to render with\n");
    if (status != PrStatus_Ok)
        return 1;
    // The result's file says how the merge ended, whatever the window held when it closed.
    return !merging || fileConflicts(m.invocation.result) == 0 ? 0 : 1;
}
