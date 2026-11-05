// Scribe's interface: the editor's documents and commands, Scribe's own commands, the menu bar, the palette and the
// status line.

#include "apps/scribe/scribe.h"

#include "pr/command_kit.h"
#include "pr/count_of.h"
#if defined(SCRIBE_HAS_TREESITTER)
#include "pr/syntax_treesitter_languages.h"
#endif
#include "pr/kit_menu.h"
#include "pr/kit_text.h"
#include "pr/kit_theme.h"
#include "pr/layout_query.h"
#include "pr/layout_tree.h"
#include "pr/string.h"

namespace scribe {

using namespace pr;

namespace {

// The height of the status line, in px.
constexpr float kStatusHeight = 24;

Scribe& scribeOf(void* context) {
    return *static_cast<Scribe*>(context);
}

// Adds one of Scribe's own commands, bound to `chord`.
void addCommand(Scribe& s, const char* id, const char* title, const char* chord, PrCommandRunFn run) {
    PrCommandDesc desc{.id = id, .title = title, .category = "Scribe", .run = run, .context = &s};
    prCommandRegistryAdd(s.commands, &desc);
    PrCommandChord parsed{};
    if (prCommandChordParse(chord, &parsed))
        prCommandKeymapBind(s.keymap, parsed, id, "", PrCommandBindingKind_Normal);
}

// A menu of the menu bar: its commands in order, an empty id for a separator, up to the first null.
struct Menu {
    const char* title;
    const char* items[12];
};

constexpr Menu kMenus[] = {
    {"File", {"file.save", "", "view.palette", "", "app.quit"}},
    {"Edit", {"edit.undo", "edit.redo", "", "edit.cut", "edit.copy", "edit.paste", "", "edit.selectAll"}},
    {"Selection", {"cursor.addAbove", "cursor.addBelow", "cursor.single"}},
    {
        "View",
        {
            "view.toggleWrap", "view.lineNumbers", "", "view.directionLtr", "view.directionRtl", "view.directionAuto",
            "", "view.horizontal", "view.verticalRl", "view.verticalLr",
        },
    },
    {
        "Go",
        {
            "language.definition", "language.hover", "", "language.nextProblem", "language.previousProblem", "",
            "diff.compareSaved", "diff.nextChange", "diff.previousChange", "diff.close",
        },
    },
    {
        "Debug",
        {
            "debug.continue", "debug.stop", "", "debug.stepOver", "debug.stepInto", "debug.stepOut", "",
            "debug.toggleBreakpoint",
        },
    },
};

// Declares the menu bar, whose height afterLayout keeps for the text below it. Returns true if a menu is open.
bool buildMenus(Scribe& s, PrLayoutNode root) {
    s.menuBar = prKitMenuBar(s.kit, root, {});
    bool open = false;
    for (const Menu& menu : kMenus) {
        PrLayoutNode title = prKitMenuBarItem(s.kit, s.menuBar, {.text = menu.title});
        PrLayoutNode popover = prKitPopover(s.kit, title, {}, {});
        if (popover == kPrLayoutNodeNone)
            continue;
        open = true;
        PrLayoutNode list = prKitMenu(s.kit, popover, {}, {.minWidth = 240});
        for (uint32_t i = 0; i < countOf(menu.items) && menu.items[i] != nullptr; ++i) {
            const char* id = menu.items[i];
            if (id[0] == 0)
                prKitMenuSeparator(s.kit, list, {});
            else
                prCommandMenuItem(s.kit, list, s.commands, s.keymap, id, {});
        }
    }
    return open;
}

// The status line: the document's file and state, the caret, and what the editor last had to say.
String statusLine(Scribe& s) {
    PrTextEditorDocument* document = prTextEditorShown(s.editor);
    if (document == nullptr)
        return "no document";
    PrTextEditorDocumentInfo info = prTextEditorDocumentInfo(document);
    PrTextViewInfo view = prTextViewInfo(prTextEditorDocumentView(document));
    PrTextEditorOptions options = prTextEditorOptions(s.editor);
    String state(info.edited ? "edited" : "saved");
    if (info.loading)
        state = format("loading {}%", int(info.loaded * 100));
    String language("plain text");
    if (const PrSyntaxLanguage* named = prTextEditorDocumentLanguage(document)) {
        language = named->name;
        if (info.parsing)
            language.append(", parsing");
    }
    const char* servers[] = {"", ", starting server", "", ", server failed"};
    language.append(servers[info.server]);
    if (info.errors != 0 || info.warnings != 0)
        formatTo(language, "  {} errors, {} warnings", info.errors, info.warnings);
    PrStringView path = prTextEditorDocumentPath(document);
    if (path.isEmpty())
        path = "untitled";
    PrStringView wrap = options.wrap ? "wrap" : "no wrap";
    PrStringView endings = info.crlf ? "  CRLF" : "  LF";
    String line = format("{}   {}  Ln {}, Col {}  {} lines  {}{}  {}", path, state, view.caret.line + 1,
        view.caret.column + 1, view.lineCount, wrap, endings, language);
    PrStringView said = prTextEditorStatus(s.editor);
    if (!said.isEmpty())
        formatTo(line, "   {}", said);
    return line;
}

} // namespace

void openEditor(Scribe& s) {
    s.commands = prCommandRegistryCreate();
    s.keymap = prCommandKeymapCreate();
    s.languages = prSyntaxRegistryCreate();
#if defined(SCRIBE_HAS_TREESITTER)
    prSyntaxTreeSitterAddLanguages(s.languages);
#endif
    s.launcher = prProcessLauncherCreate(PrProcessBackend_Default);
    PrTextEditorDesc desc{
        .kit = s.kit,
        .font = s.font,
        .app = s.app,
        .launcher = s.launcher,
        .languages = s.languages,
        .commands = s.commands,
        .keymap = s.keymap,
    };
    s.editor = prTextEditorCreate(&desc);
    // clangd serves C and C++, wherever the PATH has it. A program that cannot start leaves its documents unserved.
    PrStringView clangdLanguages[] = {"c", "cpp"};
    PrTextEditorServerDesc clangd{.languages = clangdLanguages, .languageCount = 2, .program = "clangd"};
    prTextEditorAddServer(s.editor, &clangd);
    addCommand(s, "view.palette", "Show All Commands", "Cmd Shift P", [](void* c, const PrCommandArgs*) {
        scribeOf(c).paletteRequested = true;
    });
    addCommand(s, "app.quit", "Quit", "Cmd Q", [](void* c, const PrCommandArgs*) {
        if (scribeOf(c).app != nullptr)
            prAppStop(scribeOf(c).app);
    });
}

bool openDocument(Scribe& s, PrStringView path) {
    PrTextEditorDocumentDesc desc{};
    if (!path.isEmpty())
        desc.path = path;
    PrTextEditorDocument* document = nullptr;
    return prTextEditorDocumentCreate(s.editor, &desc, &document) == PrStatus_Ok;
}

void setDebuggee(Scribe& s, PrStringView program) {
    // lldb-dap is part of the toolchain, which xcrun finds on Apple's platforms, and is on the PATH elsewhere.
#if defined(__APPLE__)
    PrStringView adapter = "xcrun";
    PrStringView arguments[] = {"lldb-dap"};
    uint32_t argumentCount = 1;
#else
    PrStringView adapter = "lldb-dap";
    const PrStringView* arguments = nullptr;
    uint32_t argumentCount = 0;
#endif
    String launch("{\"program\":\"");
    for (size_t i = 0; i < program.size; ++i) {
        char c = program.data[i];
        if (c == '"' || c == '\\')
            launch.append('\\');
        launch.append(c);
    }
    launch.append("\"}");
    PrTextEditorDebuggerDesc desc{
        .program = adapter,
        .arguments = arguments,
        .argumentCount = argumentCount,
        .adapterId = "lldb-dap",
        .launch = launch,
    };
    prTextEditorSetDebugger(s.editor, &desc);
}

bool openDiff(Scribe& s, PrStringView oldPath, PrStringView newPath) {
    PrTextEditorDocumentDesc oldDesc{.path = oldPath, .readOnly = true};
    PrTextEditorDocumentDesc newDesc{.path = newPath};
    PrTextEditorDocument* old = nullptr;
    PrTextEditorDocument* young = nullptr;
    if (prTextEditorDocumentCreate(s.editor, &oldDesc, &old) != PrStatus_Ok ||
            prTextEditorDocumentCreate(s.editor, &newDesc, &young) != PrStatus_Ok)
        return false;
    prTextEditorDiffCreate(s.editor, old, young);
    return true;
}

void openText(Scribe& s, PrStringView text, PrStringView name) {
    PrTextEditorDocumentDesc desc{.text = text, .name = name};
    PrTextEditorDocument* document = nullptr;
    prTextEditorDocumentCreate(s.editor, &desc, &document);
}

void closeEditor(Scribe& s) {
    prTextEditorDestroy(s.editor);
    prProcessLauncherDestroy(s.launcher);
    prSyntaxRegistryDestroy(s.languages);
    prCommandKeymapDestroy(s.keymap);
    prCommandRegistryDestroy(s.commands);
    prTextFontRelease(s.font);
}

void buildScribe(Scribe& s, PrLayoutNode root, PrVec2 extent) {
    prCommandScope(s.ui, root, s.commands, s.keymap, "");
    bool menuOpen = buildMenus(s, root);
    float height = extent.y - kStatusHeight - s.menuHeight;
    prTextEditorFrame(s.editor, root, PrVec2{extent.x, height > 0 ? height : 0});

    if (s.paletteRequested) {
        prKitSetOpen(s.kit, root, true);
        s.paletteRequested = false;
    }
    PrLayoutNode list = prCommandPalette(s.kit, root, s.commands, s.keymap, s.paletteQuery);
    PrStringView query{};
    if (list == kPrLayoutNodeNone) {
        s.paletteQuery.clear();
    } else if (prKitTextChanged(s.kit, list, &query)) {
        s.paletteQuery = query;
    }
    // A command runs on the text, so typing goes on there once what floats over it closes.
    PrLayoutNode text = prTextEditorTextNode(s.editor);
    if (text != kPrLayoutNodeNone && ((s.paletteOpen && list == kPrLayoutNodeNone) || (s.menuOpen && !menuOpen)))
        prUiSetFocus(s.ui, text);
    s.paletteOpen = list != kPrLayoutNodeNone;
    s.menuOpen = menuOpen;
    prKitLabel(s.kit, root, {.text = statusLine(s)}, {.role = PrKitTextRole_BodySmall, .tone = PrKitTone_Muted});
}

bool afterLayout(Scribe& s) {
    bool again = prTextEditorLaidOut(s.editor);
    PrRect bar = prLayoutRectToPx(prLayoutNodeBounds(prUiTree(s.ui), s.menuBar, PrLayoutCoordinates_Viewport));
    if (bar.bottom - bar.top != s.menuHeight) {
        s.menuHeight = bar.bottom - bar.top;
        again = true;
    }
    return again;
}

} // namespace scribe
