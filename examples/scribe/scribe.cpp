// Scribe's document, its commands and what a round shows.

#include "examples/scribe/scribe.h"

#include "pr/command_kit.h"
#include "pr/count_of.h"
#include "pr/file.h"
#include "pr/kit_box.h"
#include "pr/kit_menu.h"
#include "pr/kit_text.h"
#include "pr/kit_theme.h"
#include "pr/layout_frame.h"
#include "pr/layout_query.h"
#include "pr/layout_style.h"
#include "pr/layout_tree.h"
#include "pr/string.h"
#include "pr/text_edit_command.h"
#include "pr/ui_text_view.h"

#include <stdio.h>
#include <string>

namespace scribe {

using namespace pr;

namespace {

// The height of the status line, and the width of the scrollbar, in px.
constexpr float kStatusHeight = 24;
constexpr float kScrollbarWidth = 12;

// The smallest length of the scrollbar's thumb, in px.
constexpr float kThumbMinLength = 24;

// The time that a frame gives to better estimates of the heights of lines outside the window.
constexpr double kRefineMilliseconds = 2;

// A command that carries out a key's text command through the view.
struct KeyCommand {
    const char* id;
    const char* title;
    const char* chord;
    PrTextEditCommand command;
    PrTextEditSelectionMove kind;
};

constexpr KeyCommand kKeyCommands[] = {
    {"cursor.left", "Move Left", "Left", PrTextEditCommand_Left, PrTextEditSelectionMove_Collapse},
    {"cursor.right", "Move Right", "Right", PrTextEditCommand_Right, PrTextEditSelectionMove_Collapse},
    {"cursor.up", "Move Up", "Up", PrTextEditCommand_Up, PrTextEditSelectionMove_Collapse},
    {"cursor.down", "Move Down", "Down", PrTextEditCommand_Down, PrTextEditSelectionMove_Collapse},
    {"select.left", "Select Left", "Shift Left", PrTextEditCommand_Left, PrTextEditSelectionMove_Extend},
    {"select.right", "Select Right", "Shift Right", PrTextEditCommand_Right, PrTextEditSelectionMove_Extend},
    {"select.up", "Select Up", "Shift Up", PrTextEditCommand_Up, PrTextEditSelectionMove_Extend},
    {"select.down", "Select Down", "Shift Down", PrTextEditCommand_Down, PrTextEditSelectionMove_Extend},
#if defined(__APPLE__)
        {"cursor.wordLeft", "Move Word Left", "Alt Left", PrTextEditCommand_WordLeft, PrTextEditSelectionMove_Collapse},
    {"cursor.wordRight", "Move Word Right", "Alt Right", PrTextEditCommand_WordRight, PrTextEditSelectionMove_Collapse},
    {
        "select.wordLeft", "Select Word Left", "Alt Shift Left", PrTextEditCommand_WordLeft,
        PrTextEditSelectionMove_Extend,
    },
    {
        "select.wordRight", "Select Word Right", "Alt Shift Right", PrTextEditCommand_WordRight,
        PrTextEditSelectionMove_Extend,
    },
    {"cursor.lineLeft", "Move to Line Start", "Cmd Left", PrTextEditCommand_EdgeLeft, PrTextEditSelectionMove_Collapse},
    {
        "cursor.lineRight", "Move to Line End", "Cmd Right", PrTextEditCommand_EdgeRight,
        PrTextEditSelectionMove_Collapse,
    },
    {
        "select.lineLeft", "Select to Line Start", "Cmd Shift Left", PrTextEditCommand_EdgeLeft,
        PrTextEditSelectionMove_Extend,
    },
    {
        "select.lineRight", "Select to Line End", "Cmd Shift Right", PrTextEditCommand_EdgeRight,
        PrTextEditSelectionMove_Extend,
    },
    {"cursor.textStart", "Move to Text Start", "Cmd Up", PrTextEditCommand_TextStart, PrTextEditSelectionMove_Collapse},
    {"cursor.textEnd", "Move to Text End", "Cmd Down", PrTextEditCommand_TextEnd, PrTextEditSelectionMove_Collapse},
    {
        "select.textStart", "Select to Text Start", "Cmd Shift Up", PrTextEditCommand_TextStart,
        PrTextEditSelectionMove_Extend,
    },
    {
        "select.textEnd", "Select to Text End", "Cmd Shift Down", PrTextEditCommand_TextEnd,
        PrTextEditSelectionMove_Extend,
    },
    {
        "edit.deleteWordBefore", "Delete Word Before", "Alt Backspace", PrTextEditCommand_DeleteWordBefore,
        PrTextEditSelectionMove_Collapse,
    },
    {
        "edit.deleteWordAfter", "Delete Word After", "Alt Delete", PrTextEditCommand_DeleteWordAfter,
        PrTextEditSelectionMove_Collapse,
    },
#else
        {
            "cursor.wordLeft", "Move Word Left", "Ctrl Left", PrTextEditCommand_WordLeft,
            PrTextEditSelectionMove_Collapse,
        },
    {
        "cursor.wordRight", "Move Word Right", "Ctrl Right", PrTextEditCommand_WordRight,
        PrTextEditSelectionMove_Collapse,
    },
    {
        "select.wordLeft", "Select Word Left", "Ctrl Shift Left", PrTextEditCommand_WordLeft,
        PrTextEditSelectionMove_Extend,
    },
    {
        "select.wordRight", "Select Word Right", "Ctrl Shift Right", PrTextEditCommand_WordRight,
        PrTextEditSelectionMove_Extend,
    },
    {
        "cursor.textStart", "Move to Text Start", "Ctrl Home", PrTextEditCommand_TextStart,
        PrTextEditSelectionMove_Collapse,
    },
    {"cursor.textEnd", "Move to Text End", "Ctrl End", PrTextEditCommand_TextEnd, PrTextEditSelectionMove_Collapse},
    {
        "select.textStart", "Select to Text Start", "Ctrl Shift Home", PrTextEditCommand_TextStart,
        PrTextEditSelectionMove_Extend,
    },
    {
        "select.textEnd", "Select to Text End", "Ctrl Shift End", PrTextEditCommand_TextEnd,
        PrTextEditSelectionMove_Extend,
    },
    {
        "edit.deleteWordBefore", "Delete Word Before", "Ctrl Backspace", PrTextEditCommand_DeleteWordBefore,
        PrTextEditSelectionMove_Collapse,
    },
    {
        "edit.deleteWordAfter", "Delete Word After", "Ctrl Delete", PrTextEditCommand_DeleteWordAfter,
        PrTextEditSelectionMove_Collapse,
    },
#endif
        {"cursor.home", "Move to Line Start", "Home", PrTextEditCommand_LineStart, PrTextEditSelectionMove_Collapse},
    {"cursor.end", "Move to Line End", "End", PrTextEditCommand_LineEnd, PrTextEditSelectionMove_Collapse},
    {"select.home", "Select to Line Start", "Shift Home", PrTextEditCommand_LineStart, PrTextEditSelectionMove_Extend},
    {"select.end", "Select to Line End", "Shift End", PrTextEditCommand_LineEnd, PrTextEditSelectionMove_Extend},
    {"edit.deleteLeft", "Delete Left", "Backspace", PrTextEditCommand_DeleteBackward, PrTextEditSelectionMove_Collapse},
    {"edit.deleteRight", "Delete Right", "Delete", PrTextEditCommand_DeleteForward, PrTextEditSelectionMove_Collapse},
    {"edit.newline", "Insert Line Break", "Enter", PrTextEditCommand_InsertLineBreak, PrTextEditSelectionMove_Collapse},
    {"edit.selectAll", "Select All", "Cmd A", PrTextEditCommand_SelectAll, PrTextEditSelectionMove_Collapse},
};

// A key command's context: the editor, and which command.
struct KeyContext {
    Scribe* scribe;
    const KeyCommand* command;
};

KeyContext keyContexts[countOf(kKeyCommands)];

void runKey(void* context, const PrCommandArgs*) {
    KeyContext& key = *static_cast<KeyContext*>(context);
    prTextViewRun(key.scribe->view, key.command->command, key.command->kind);
}

// Adds a command with no arguments, bound to `chord`, or to nothing if it is null.
void addCommand(Scribe& s, const char* id, const char* title, const char* chord, PrCommandRunFn run, void* context,
        PrCommandStateFn PR_NULLABLE state = nullptr) {
    PrCommandDesc desc{.id = id, .title = title, .category = "Scribe", .run = run, .state = state, .context = context};
    prCommandRegistryAdd(s.commands, &desc);
    PrCommandChord parsed{};
    if (chord != nullptr && prCommandChordParse(chord, &parsed))
        prCommandKeymapBind(s.keymap, parsed, id, "", PrCommandBindingKind_Normal);
}

// Writes the document to its file, with the line endings that the file had.
void save(Scribe& s) {
    if (s.path.empty()) {
        s.status = "no file to save to";
        return;
    }
    PrStringView text = prTextEditBufferText(s.buffer);
    bool ok = false;
    if (s.crlf) {
        std::string restored;
        restored.reserve(text.size + prTextEditBufferLineCount(s.buffer));
        for (size_t i = 0; i < text.size; ++i) {
            if (text.data[i] == '\n')
                restored += '\r';
            restored += text.data[i];
        }
        ok = prFileWrite(s.path, PrByteView{reinterpret_cast<const uint8_t*>(restored.data()), restored.size()});
    } else {
        ok = prFileWrite(s.path, PrByteView{reinterpret_cast<const uint8_t*>(text.data), text.size});
    }
    s.status = ok ? "saved" : "could not save";
    if (ok)
        s.savedVersion = prTextEditBufferVersion(s.buffer);
}

Scribe& scribeOf(void* context) {
    return *static_cast<Scribe*>(context);
}

// A command that chooses one of a set of values of the view.
struct DirectionCommand {
    const char* id;
    const char* title;
    Direction direction;
};

struct WritingModeCommand {
    const char* id;
    const char* title;
    PrLayoutWritingMode mode;
};

constexpr DirectionCommand kDirections[] = {
    {"view.directionLtr", "Left to Right", Direction_Ltr},
    {"view.directionRtl", "Right to Left", Direction_Rtl},
    {"view.directionAuto", "Direction from Each Line", Direction_Auto},
};

constexpr WritingModeCommand kWritingModes[] = {
    {"view.horizontal", "Horizontal Lines", PrLayoutWritingMode_HorizontalTb},
    {"view.verticalRl", "Vertical Lines, Right to Left", PrLayoutWritingMode_VerticalRl},
    {"view.verticalLr", "Vertical Lines, Left to Right", PrLayoutWritingMode_VerticalLr},
};

// A choosing command's context: the editor, and which value of its table.
struct ChoiceContext {
    Scribe* scribe;
    uint32_t index;
};

ChoiceContext directionContexts[countOf(kDirections)];
ChoiceContext writingModeContexts[countOf(kWritingModes)];

// A menu of the menu bar: its commands in order, an empty id for a separator, up to the first null.
struct Menu {
    const char* title;
    const char* items[10];
};

constexpr Menu kMenus[] = {
    {"File", {"file.save", "", "view.palette", "", "app.quit"}},
    {"Edit", {"edit.undo", "edit.redo", "", "edit.cut", "edit.copy", "edit.paste", "", "edit.selectAll"}},
    {"Selection", {"cursor.addAbove", "cursor.addBelow", "cursor.single"}},
    {
        "View",
        {
            "view.toggleWrap", "", "view.directionLtr", "view.directionRtl", "view.directionAuto", "",
            "view.horizontal", "view.verticalRl", "view.verticalLr",
        },
    },
};

// Declares the menu bar, whose height afterLayout keeps for the text below it. Returns true if a menu is open.
bool buildMenus(Scribe& s, PrLayoutNode root) {
    s.menuBar = prKitMenuBar(s.kit, root, {});
    bool open = false;
    for (const Menu& menu : kMenus) {
        std::string name = std::string("scribe.menu.") + menu.title;
        PrUiKey key = prUiKey(s.ui, kPrLayoutNodeNone, name);
        PrLayoutNode title = prKitMenuBarItem(s.kit, s.menuBar, {.key = key, .text = menu.title});
        PrLayoutNode popover = prKitPopover(s.kit, title, {}, {});
        if (popover == kPrLayoutNodeNone)
            continue;
        open = true;
        PrUiKey listKey = prUiKey(s.ui, kPrLayoutNodeNone, name + ".list");
        PrLayoutNode list = prKitMenu(s.kit, popover, {.key = listKey}, {.minWidth = 240});
        for (uint32_t i = 0; i < countOf(menu.items) && menu.items[i] != nullptr; ++i) {
            const char* id = menu.items[i];
            if (id[0] == 0)
                prKitMenuSeparator(s.kit, list, {});
            else
                prCommandMenuItem(s.kit, list, s.commands, s.keymap, id);
        }
    }
    return open;
}

// Returns a record of the given box and look.
PrLayoutStyleIndex record(PrLayoutTree* tree, const PrLayoutBoxStyle& box,
        const PrLayoutDecorationStyle* PR_NULLABLE decoration = nullptr) {
    PrLayoutStyle style = prLayoutStyleDefault();
    style.groups[PrLayoutGroup_Box] = prLayoutBoxStyleIntern(tree, &box);
    if (decoration != nullptr)
        style.groups[PrLayoutGroup_Decoration] = prLayoutDecorationStyleIntern(tree, decoration);
    return prLayoutStyleIntern(tree, &style);
}

// Declares the scrollbar along the block axis of the text: at the right of horizontal lines, and below vertical ones,
// where the start of the document is at the side that the lines stack from. A drag moves the thumb. The document's size
// is an estimate outside the window, so the thumb can jump as estimates settle, while the reader's place holds.
void buildScrollbar(Scribe& s, PrLayoutNode parent, PrVec2 extent) {
    PrLayoutTree* tree = prUiTree(s.ui);
    const PrKitPalette& palette = s.kit->theme.palette;
    PrTextViewInfo info = prTextViewInfo(s.view);
    bool vertical = s.writingMode != PrLayoutWritingMode_HorizontalTb;
    bool reversed = s.writingMode == PrLayoutWritingMode_VerticalRl;
    float size = vertical ? extent.x : extent.y;
    PrLayoutBoxStyle trackBox = prLayoutBoxStyleDefault();
    trackBox.display = PrLayoutDisplay_Block;
    trackBox.position = PrLayoutPosition_Absolute;
    trackBox.inset[PrRectSide_Right] = prLayoutLengthPx(0);
    trackBox.inset[vertical ? PrRectSide_Bottom : PrRectSide_Top] = prLayoutLengthPx(0);
    trackBox.width = prLayoutLengthPx(vertical ? size : kScrollbarWidth);
    trackBox.height = prLayoutLengthPx(vertical ? kScrollbarWidth : size);
    PrUiKey trackKey = prUiKey(s.ui, kPrLayoutNodeNone, "scribe.scrollbar");
    PrLayoutNode track = prUiElement(s.ui, parent, trackKey, record(tree, trackBox), PrUiFlag_Pressable);
    if (info.documentSize <= double(size))
        return;
    float length = float(double(size) * double(size) / info.documentSize);
    length = length > kThumbMinLength ? length : kThumbMinLength;
    double range = info.documentSize - double(size);
    float room = size - length;
    float start = float(info.offset / range) * room;
    start = reversed ? room - start : start;
    PrUiDrag drag{};
    if (prUiDrag(s.ui, track, 0, 0, &drag) && drag.active) {
        double at = double(vertical ? drag.position.x : drag.position.y) - double(length) / 2;
        at = reversed ? double(room) - at : at;
        prTextViewScrollTo(s.view, at / double(room) * range);
    }
    PrLayoutBoxStyle thumbBox = prLayoutBoxStyleDefault();
    thumbBox.display = PrLayoutDisplay_Block;
    thumbBox.position = PrLayoutPosition_Absolute;
    thumbBox.inset[PrRectSide_Left] = prLayoutLengthPx(vertical ? start : 2);
    thumbBox.inset[PrRectSide_Top] = prLayoutLengthPx(vertical ? 2 : start);
    thumbBox.width = prLayoutLengthPx(vertical ? length : kScrollbarWidth - 4);
    thumbBox.height = prLayoutLengthPx(vertical ? kScrollbarWidth - 4 : length);
    PrLayoutDecorationStyle thumbLook = prLayoutDecorationStyleDefault();
    PrColor thumbColor = palette.text3;
    thumbColor.a *= 0.5f;
    thumbLook.background = prColor16FromColor(thumbColor);
    prLayoutTreeFrameElement(tree, track, 0, record(tree, thumbBox, &thumbLook));
}

} // namespace

bool openDocument(Scribe& s, const std::string& path) {
    s.path = path;
    std::string text;
    if (!path.empty()) {
        PrFileMapping mapping{};
        if (!prFileMap(path, &mapping))
            return false;
        if (mapping.size > size_t(kPrTextEditBufferMaxBytes)) {
            prFileUnmap(&mapping);
            return false;
        }
        const char* bytes = reinterpret_cast<const char*>(mapping.data);
        size_t size = mapping.size;
        // A document records its line ending, and its buffer ends lines with a line feed.
        s.crlf = false;
        for (size_t i = 0; i + 1 < size; ++i) {
            if (bytes[i] == '\n')
                break;
            if (bytes[i] == '\r' && bytes[i + 1] == '\n') {
                s.crlf = true;
                break;
            }
        }
        text.reserve(size);
        for (size_t i = 0; i < size; ++i) {
            if (bytes[i] == '\r' && i + 1 < size && bytes[i + 1] == '\n')
                continue;
            text += bytes[i];
        }
        prFileUnmap(&mapping);
    }
    openText(s, text);
    return true;
}

void openText(Scribe& s, PrStringView text) {
    s.buffer = prTextEditBufferCreate(text);
    s.history = prTextEditHistoryCreate(s.buffer);
    s.view = prTextViewCreate(s.buffer);
    s.savedVersion = prTextEditBufferVersion(s.buffer);
}

void closeDocument(Scribe& s) {
    prCommandKeymapDestroy(s.keymap);
    prCommandRegistryDestroy(s.commands);
    prTextViewDestroy(s.view);
    prTextEditHistoryDestroy(s.history);
    prTextEditBufferDestroy(s.buffer);
    prTextFontRelease(s.font);
}

void registerCommands(Scribe& s) {
    s.commands = prCommandRegistryCreate();
    s.keymap = prCommandKeymapCreate();
    for (uint32_t i = 0; i < countOf(kKeyCommands); ++i) {
        keyContexts[i] = KeyContext{&s, &kKeyCommands[i]};
        addCommand(s, kKeyCommands[i].id, kKeyCommands[i].title, kKeyCommands[i].chord, runKey, &keyContexts[i]);
    }
    addCommand(s, "cursor.pageUp", "Page Up", "PageUp", [](void* c, const PrCommandArgs*) {
        prTextViewRunPage(scribeOf(c).view, PrTextViewPage_Up, PrTextEditSelectionMove_Collapse);
    }, &s);
    addCommand(s, "cursor.pageDown", "Page Down", "PageDown", [](void* c, const PrCommandArgs*) {
        prTextViewRunPage(scribeOf(c).view, PrTextViewPage_Down, PrTextEditSelectionMove_Collapse);
    }, &s);
    addCommand(s, "select.pageUp", "Select Page Up", "Shift PageUp", [](void* c, const PrCommandArgs*) {
        prTextViewRunPage(scribeOf(c).view, PrTextViewPage_Up, PrTextEditSelectionMove_Extend);
    }, &s);
    addCommand(s, "select.pageDown", "Select Page Down", "Shift PageDown", [](void* c, const PrCommandArgs*) {
        prTextViewRunPage(scribeOf(c).view, PrTextViewPage_Down, PrTextEditSelectionMove_Extend);
    }, &s);
    addCommand(s, "cursor.addBelow", "Add Cursor Below", "Cmd Alt Down", [](void* c, const PrCommandArgs*) {
        prTextViewAddCaretOnLine(scribeOf(c).view, PrTextEditLineStep_Next);
    }, &s);
    addCommand(s, "cursor.addAbove", "Add Cursor Above", "Cmd Alt Up", [](void* c, const PrCommandArgs*) {
        prTextViewAddCaretOnLine(scribeOf(c).view, PrTextEditLineStep_Previous);
    }, &s);
    addCommand(s, "cursor.single", "Keep Primary Cursor", "Escape", [](void* c, const PrCommandArgs*) {
        prTextViewKeepPrimary(scribeOf(c).view);
    }, &s);
    addCommand(s, "edit.tab", "Insert Tab", "Tab", [](void* c, const PrCommandArgs*) {
        prTextViewInsert(scribeOf(c).view, "\t", PrTextEditMerge_Typing);
    }, &s);
    addCommand(s, "edit.undo", "Undo", "Cmd Z", [](void* c, const PrCommandArgs*) {
        Scribe& editor = scribeOf(c);
        if (prTextEditHistoryUndo(editor.history, prTextViewEdit(editor.view)))
            prTextViewReveal(editor.view);
    }, &s, [](void* c) -> PrCommandStateFlags {
        return prTextEditHistoryCanUndo(scribeOf(c).history) ? 0 : PrCommandStateFlag_Disabled;
    });
    addCommand(s, "edit.redo", "Redo", "Cmd Shift Z", [](void* c, const PrCommandArgs*) {
        Scribe& editor = scribeOf(c);
        if (prTextEditHistoryRedo(editor.history, prTextViewEdit(editor.view)))
            prTextViewReveal(editor.view);
    }, &s, [](void* c) -> PrCommandStateFlags {
        return prTextEditHistoryCanRedo(scribeOf(c).history) ? 0 : PrCommandStateFlag_Disabled;
    });
    addCommand(s, "edit.copy", "Copy", "Cmd C", [](void* c, const PrCommandArgs*) {
        Scribe& editor = scribeOf(c);
        PrString text = prTextViewSelectedText(editor.view);
        if (editor.app != nullptr && prStringView(&text).size != 0)
            prAppSetClipboardText(editor.app, prStringView(&text));
        prStringReset(&text);
    }, &s);
    addCommand(s, "edit.cut", "Cut", "Cmd X", [](void* c, const PrCommandArgs*) {
        Scribe& editor = scribeOf(c);
        PrString text = prTextViewSelectedText(editor.view);
        if (editor.app != nullptr && prStringView(&text).size != 0)
            prAppSetClipboardText(editor.app, prStringView(&text));
        prStringReset(&text);
        prTextViewEraseSelection(editor.view);
    }, &s);
    addCommand(s, "edit.paste", "Paste", "Cmd V", [](void* c, const PrCommandArgs*) {
        Scribe& editor = scribeOf(c);
        prUiTextViewPaste(editor.ui, editor.editor);
    }, &s);
    addCommand(s, "file.save", "Save", "Cmd S", [](void* c, const PrCommandArgs*) { save(scribeOf(c)); }, &s,
        [](void* c) -> PrCommandStateFlags { return scribeOf(c).path.empty() ? PrCommandStateFlag_Disabled : 0; });
    addCommand(s, "view.toggleWrap", "Word Wrap", "Alt Z", [](void* c, const PrCommandArgs*) {
        Scribe& editor = scribeOf(c);
        editor.wrap = !editor.wrap;
        prTextViewReveal(editor.view);
    }, &s, [](void* c) -> PrCommandStateFlags { return scribeOf(c).wrap ? PrCommandStateFlag_Checked : 0; });
    for (uint32_t i = 0; i < countOf(kDirections); ++i) {
        const DirectionCommand& command = kDirections[i];
        addCommand(s, command.id, command.title, nullptr, [](void* c, const PrCommandArgs*) {
            ChoiceContext& choice = *static_cast<ChoiceContext*>(c);
            choice.scribe->direction = kDirections[choice.index].direction;
            prTextViewReveal(choice.scribe->view);
        }, &directionContexts[i], [](void* c) -> PrCommandStateFlags {
            const ChoiceContext& choice = *static_cast<const ChoiceContext*>(c);
            return choice.scribe->direction == kDirections[choice.index].direction ? PrCommandStateFlag_Checked : 0;
        });
        directionContexts[i] = ChoiceContext{&s, i};
    }
    for (uint32_t i = 0; i < countOf(kWritingModes); ++i) {
        const WritingModeCommand& command = kWritingModes[i];
        addCommand(s, command.id, command.title, nullptr, [](void* c, const PrCommandArgs*) {
            ChoiceContext& choice = *static_cast<ChoiceContext*>(c);
            choice.scribe->writingMode = kWritingModes[choice.index].mode;
            prTextViewReveal(choice.scribe->view);
        }, &writingModeContexts[i], [](void* c) -> PrCommandStateFlags {
            const ChoiceContext& choice = *static_cast<const ChoiceContext*>(c);
            return choice.scribe->writingMode == kWritingModes[choice.index].mode ? PrCommandStateFlag_Checked : 0;
        });
        writingModeContexts[i] = ChoiceContext{&s, i};
    }
    addCommand(s, "view.palette", "Show All Commands", "Cmd Shift P", [](void* c, const PrCommandArgs*) {
        scribeOf(c).paletteRequested = true;
    }, &s);
    addCommand(s, "app.quit", "Quit", "Cmd Q", [](void* c, const PrCommandArgs*) {
        if (scribeOf(c).app != nullptr)
            prAppStop(scribeOf(c).app);
    }, &s);
}

void buildEditor(Scribe& s, PrLayoutNode root, PrVec2 extent) {
    PrLayoutTree* tree = prUiTree(s.ui);
    const PrKitPalette& palette = s.kit->theme.palette;
    prCommandScope(s.ui, root, s.commands, s.keymap, "");
    bool menuOpen = buildMenus(s, root);
    bool vertical = s.writingMode != PrLayoutWritingMode_HorizontalTb;
    float height = extent.y - kStatusHeight - s.menuHeight;
    height = height > 0 ? height : 0;
    float width = extent.x - (vertical ? 0 : kScrollbarWidth);
    width = width > 0 ? width : 0;
    float portHeight = height - (vertical ? kScrollbarWidth : 0);
    portHeight = portHeight > 0 ? portHeight : 0;

    PrLayoutBoxStyle holderBox = prLayoutBoxStyleDefault();
    holderBox.display = PrLayoutDisplay_Block;
    holderBox.position = PrLayoutPosition_Relative;
    holderBox.width = prLayoutLengthPx(extent.x);
    holderBox.height = prLayoutLengthPx(height);
    PrLayoutNode holder = prLayoutTreeFrameElement(tree, root, 0, record(tree, holderBox));

    // The port and the rows share a writing mode and a direction, which decide how the rows stack and scroll.
    PrLayoutFontStyle rowFont = prLayoutFontStyleDefault();
    rowFont.font = s.font;
    rowFont.lineHeight = prLayoutLineHeightPx(19);
    rowFont.whiteSpaceCollapse = PrLayoutWhiteSpaceCollapse_Preserve;
    rowFont.textWrapMode = s.wrap ? PrLayoutTextWrapMode_Wrap : PrLayoutTextWrapMode_Nowrap;
    rowFont.overflowWrap = PrTextOverflowWrap_Anywhere;
    rowFont.writingMode = s.writingMode;
    rowFont.direction = s.direction == Direction_Rtl ? PrTextDirection_Rtl : PrTextDirection_Ltr;
    PrLayoutGroupIndex fontGroup = prLayoutFontStyleIntern(tree, &rowFont);

    PrLayoutBoxStyle portBox = prLayoutBoxStyleDefault();
    portBox.display = PrLayoutDisplay_Block;
    portBox.width = prLayoutLengthPx(width);
    portBox.height = prLayoutLengthPx(portHeight);
    portBox.overflowX = PrLayoutOverflow_Hidden;
    portBox.overflowY = PrLayoutOverflow_Hidden;
    PrLayoutStyle portStyle = prLayoutStyleDefault();
    portStyle.groups[PrLayoutGroup_Box] = prLayoutBoxStyleIntern(tree, &portBox);
    portStyle.groups[PrLayoutGroup_Font] = fontGroup;
    PrLayoutNode port = prUiElement(s.ui, holder, 2, prLayoutStyleIntern(tree, &portStyle),
        PrUiFlag_Pressable | PrUiFlag_Focusable | PrUiFlag_Wheel);
    prUiSetCursor(s.ui, port, PrAppCursor_Text);
    if (s.editor == kPrLayoutNodeNone)
        prUiSetFocus(s.ui, port);
    s.editor = port;

    PrLayoutBoxStyle rowBox = prLayoutBoxStyleDefault();
    rowBox.display = PrLayoutDisplay_Block;
    // Each row is a paragraph of its own, so that plaintext gives each line the direction of its script.
    rowBox.unicodeBidi = s.direction == Direction_Auto ? PrTextBidi_Plaintext : PrTextBidi_Normal;
    PrLayoutColorStyle rowColor = prLayoutColorStyleDefault();
    rowColor.color = prColor16FromColor(palette.text1);
    PrLayoutStyle row = prLayoutStyleDefault();
    row.groups[PrLayoutGroup_Box] = prLayoutBoxStyleIntern(tree, &rowBox);
    row.groups[PrLayoutGroup_Font] = fontGroup;
    row.groups[PrLayoutGroup_Color] = prLayoutColorStyleIntern(tree, &rowColor);
    PrLayoutStyleIndex rowStyle = prLayoutStyleIntern(tree, &row);
    PrUiTextViewDesc view{
        .view = s.view,
        .control = port,
        .extent = PrVec2{width, portHeight},
        .rowStyle = rowStyle,
        .wrap = s.wrap,
        .selection = prColor16FromColor(palette.textSelection),
        .caret = prColor16FromColor(palette.text1),
    };
    prUiTextView(s.ui, &view);
    buildScrollbar(s, holder, PrVec2{extent.x, height});

    if (s.paletteRequested) {
        prKitSetOpen(s.kit, root, true);
        s.paletteRequested = false;
    }
    PrLayoutNode list = prCommandPalette(s.kit, root, s.commands, s.keymap, s.paletteQuery);
    PrStringView query{};
    if (list == kPrLayoutNodeNone) {
        s.paletteQuery.clear();
    } else if (prKitTextChanged(s.kit, list, &query)) {
        s.paletteQuery.assign(query.data, query.size);
    }
    // A command runs on the text, so typing goes on there once what floats over it closes.
    if ((s.paletteOpen && list == kPrLayoutNodeNone) || (s.menuOpen && !menuOpen))
        prUiSetFocus(s.ui, port);
    s.paletteOpen = list != kPrLayoutNodeNone;
    s.menuOpen = menuOpen;

    PrTextViewInfo info = prTextViewInfo(s.view);
    bool dirty = prTextEditBufferVersion(s.buffer) != s.savedVersion;
    char where[128];
    snprintf(where, sizeof(where), "%s  Ln %u, Col %u  %u lines  %s%s", dirty ? "edited" : "saved", info.caret.line + 1,
        info.caret.column + 1, info.lineCount, s.wrap ? "wrap" : "no wrap", s.crlf ? "  CRLF" : "  LF");
    std::string line = (s.path.empty() ? std::string("untitled") : s.path) + "   " + where;
    if (!s.status.empty())
        line += "   " + s.status;
    prKitLabel(s.kit, root, {.text = line}, {.role = PrKitTextRole_BodySmall, .tone = PrKitTone_Muted});
}

bool afterLayout(Scribe& s) {
    bool again = prTextViewLaidOut(s.view);
    PrRect bar = prLayoutRectToPx(prLayoutNodeBounds(prUiTree(s.ui), s.menuBar, PrLayoutCoordinates_Viewport));
    if (bar.bottom - bar.top != s.menuHeight) {
        s.menuHeight = bar.bottom - bar.top;
        again = true;
    }
    return prTextViewRefine(s.view, kRefineMilliseconds) || again;
}

} // namespace scribe
