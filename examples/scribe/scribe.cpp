// Scribe's document, its commands and what a round shows.

#include "examples/scribe/scribe.h"

#include "pr/command_kit.h"
#include "pr/count_of.h"
#include "pr/file.h"
#include "pr/float.h"
#if defined(SCRIBE_HAS_TREESITTER)
#include "pr/syntax_treesitter_c.h"
#include "pr/syntax_treesitter_cpp.h"
#include "pr/syntax_treesitter_css.h"
#include "pr/syntax_treesitter_html.h"
#include "pr/syntax_treesitter_javascript.h"
#include "pr/syntax_treesitter_json.h"
#include "pr/syntax_treesitter_markdown.h"
#include "pr/syntax_treesitter_markdown_inline.h"
#include "pr/syntax_treesitter_python.h"
#include "pr/syntax_treesitter_xml.h"
#endif
#include "pr/kit_box.h"
#include "pr/kit_menu.h"
#include "pr/kit_text.h"
#include "pr/kit_theme.h"
#include "pr/layout_frame.h"
#include "pr/layout_query.h"
#include "pr/layout_style.h"
#include "pr/layout_tree.h"
#include "pr/string.h"
#include "pr/string_ops.h"
#include "pr/text_edit_command.h"
#include "pr/time.h"
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

// The time that a frame gives to the document's syntax once its text has changed, and to loading the text, in
// milliseconds. A parse that runs out of time resumes in the next frame. A piece of loading text parses whole, so that
// no text shows before its syntax does.
constexpr double kParseMilliseconds = 4;
constexpr double kLoadMilliseconds = 8;

// The most bytes of a piece of loading text.
constexpr size_t kLoadPiece = 64 * 1024;

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

// A style class: what one theme colors, whatever the language and whatever its grammar's queries call it.
enum {
    StyleClass_None,
    StyleClass_Keyword,
    StyleClass_Type,
    StyleClass_Function,
    StyleClass_Member,
    StyleClass_Variable,
    StyleClass_String,
    StyleClass_Escape,
    StyleClass_Number,
    StyleClass_Constant,
    StyleClass_Comment,
    StyleClass_Attribute,
    StyleClass_Operator,
    StyleClass_Punctuation,
    StyleClass_Heading,
    StyleClass_Strong,
    StyleClass_Emphasis,
    StyleClass_Link,                // the text that a link reads as
    StyleClass_LinkTarget,          // where a link points
    StyleClass_Count,
};
typedef uint8_t StyleClass;

// A word of a category's name, and the class that it gives.
struct ClassWord {
    const char* word;
    StyleClass style;
};

// The classes by the first word of a category's name, as both Neovim's queries and Helix's name them. The name of a
// markup element, such as a tag, reads as its structure, as a keyword does.
constexpr ClassWord kClassOfFirstWord[] = {
    {"attribute", StyleClass_Attribute},
    {"boolean", StyleClass_Constant},
    {"character", StyleClass_Constant},
    {"comment", StyleClass_Comment},
    {"constant", StyleClass_Constant},
    {"constructor", StyleClass_Type},
    {"delimiter", StyleClass_Punctuation},
    {"escape", StyleClass_Escape},
    {"field", StyleClass_Member},
    {"float", StyleClass_Number},
    {"function", StyleClass_Function},
    {"keyword", StyleClass_Keyword},
    {"label", StyleClass_Variable},
    {"member", StyleClass_Member},
    {"method", StyleClass_Function},
    {"module", StyleClass_Type},
    {"namespace", StyleClass_Type},
    {"number", StyleClass_Number},
    {"operator", StyleClass_Operator},
    {"parameter", StyleClass_Variable},
    {"preproc", StyleClass_Keyword},
    {"property", StyleClass_Member},
    {"punctuation", StyleClass_Punctuation},
    {"string", StyleClass_String},
    {"tag", StyleClass_Keyword},
    {"type", StyleClass_Type},
    {"variable", StyleClass_Variable},
};

// The classes of markup, by the second word of a name whose first is `text`, as Neovim names markup, or `markup`, as
// Helix does.
constexpr ClassWord kClassOfMarkupWord[] = {
    {"bold", StyleClass_Strong},
    {"emphasis", StyleClass_Emphasis},
    {"heading", StyleClass_Heading},
    {"italic", StyleClass_Emphasis},
    {"link", StyleClass_LinkTarget},
    {"literal", StyleClass_String},
    {"raw", StyleClass_String},
    {"reference", StyleClass_Link},
    {"strong", StyleClass_Strong},
    {"title", StyleClass_Heading},
    {"uri", StyleClass_LinkTarget},
};

// The color of a class's text in a dark theme and in a light one, or zero alpha for the text's own.
struct ClassColor {
    PrColor8 dark;
    PrColor8 light;
};

constexpr ClassColor kClassColors[StyleClass_Count] = {
    {},                                                 // None
    {{197, 134, 192, 255}, {175, 0, 219, 255}},         // Keyword
    {{78, 201, 176, 255}, {38, 127, 153, 255}},         // Type
    {{220, 220, 170, 255}, {121, 94, 38, 255}},         // Function
    {{156, 220, 254, 255}, {0, 16, 128, 255}},          // Member
    {},                                                 // Variable
    {{206, 145, 120, 255}, {163, 21, 21, 255}},         // String
    {{215, 186, 125, 255}, {238, 0, 0, 255}},           // Escape
    {{181, 206, 168, 255}, {9, 134, 88, 255}},          // Number
    {{79, 193, 255, 255}, {0, 112, 193, 255}},          // Constant
    {{106, 153, 85, 255}, {0, 128, 0, 255}},            // Comment
    {{156, 220, 254, 255}, {229, 0, 0, 255}},           // Attribute
    {},                                                 // Operator
    {{150, 150, 150, 255}, {110, 110, 110, 255}},       // Punctuation
    {{86, 156, 214, 255}, {128, 0, 0, 255}},            // Heading
    {{197, 134, 192, 255}, {175, 0, 219, 255}},         // Strong
    {{206, 145, 120, 255}, {163, 21, 21, 255}},         // Emphasis
    {{86, 156, 214, 255}, {0, 0, 255, 255}},            // Link
    {{156, 220, 254, 255}, {0, 16, 128, 255}},          // LinkTarget
};

// Returns the word of `name` that starts at `from`, up to the next dot.
PrStringView wordAt(PrStringView name, size_t from) {
    size_t end = from;
    while (end < name.size && name.data[end] != '.')
        ++end;
    return PrStringView{name.data + from, end - from};
}

StyleClass classOfWord(const ClassWord* words, size_t count, PrStringView word) {
    for (size_t i = 0; i < count; ++i) {
        if (word == words[i].word)
            return words[i].style;
    }
    return StyleClass_None;
}

// Returns the style class of a category's name.
StyleClass classOfCategory(PrStringView name) {
    PrStringView first = wordAt(name, 0);
    if (first == "text" || first == "markup") {
        if (first.size >= name.size)
            return StyleClass_None;
        return classOfWord(kClassOfMarkupWord, countOf(kClassOfMarkupWord), wordAt(name, first.size + 1));
    }
    return classOfWord(kClassOfFirstWord, countOf(kClassOfFirstWord), first);
}

PrColor16 colorOfCategory(PrStringView name, bool dark) {
    const ClassColor& color = kClassColors[classOfCategory(name)];
    PrColor8 shown = dark ? color.dark : color.light;
    return shown.a != 0 ? prColor16FromColor(prColorFromColor8(shown)) : PrColor16{};
}

// Writes the colored spans of the window from the syntax tree, which the view asks for once in each frame.
uint32_t spansOf(void* context, uint32_t start, uint32_t end, PrTextViewSpan* PR_NULLABLE out, uint32_t capacity) {
    Scribe& s = scribeOf(context);
    uint64_t version = prTextEditBufferVersion(s.buffer);
    if (version != s.spansVersion || s.syntaxChanges != s.spansChanges || start != s.spansStart || end != s.spansEnd) {
        s.spans.resize(prSyntaxTreeSpans(s.syntax, start, end, nullptr, 0));
        prSyntaxTreeSpans(s.syntax, start, end, s.spans.data(), uint32_t(s.spans.size()));
        s.spansVersion = version;
        s.spansChanges = s.syntaxChanges;
        s.spansStart = start;
        s.spansEnd = end;
    }
    uint32_t count = 0;
    for (const PrSyntaxSpan& span : s.spans) {
        if (span.category >= s.categoryColors.size() || s.categoryColors[span.category].a == 0)
            continue;
        if (count < capacity)
            out[count] = PrTextViewSpan{span.start, span.end, s.categoryColors[span.category]};
        ++count;
    }
    return count;
}

// Returns the text that streams into the document.
PrStringView sourceOf(const Scribe& s) {
    if (s.mapping.data != nullptr)
        return PrStringView{reinterpret_cast<const char*>(s.mapping.data), s.mapping.size};
    return PrStringView(s.source);
}

// Ends the load: the file holds what the buffer does, and the source is no longer needed.
void finishLoad(Scribe& s) {
    s.loading = false;
    s.savedVersion = prTextEditBufferVersion(s.buffer);
    if (s.mapping.data != nullptr)
        prFileUnmap(&s.mapping);
    s.mapping = PrFileMapping{};
    s.source.clear();
    s.source.shrink_to_fit();
}

// Appends pieces of the loading text to the end of the buffer for about `milliseconds`, and parses each before the
// next, so that the text and its syntax arrive together.
void loadPieces(Scribe& s, double milliseconds) {
    if (!s.loading)
        return;
    PrStringView source = sourceOf(s);
    double end = prTimeMonotonicMilliseconds() + milliseconds;
    do {
        size_t at = s.loaded;
        size_t stop = source.size - at > kLoadPiece ? at + kLoadPiece : source.size;
        if (stop < source.size) {
            // A piece ends after a line feed where it can, so that lines arrive whole, and otherwise between code
            // points and never between a CR and its LF.
            PrStringView window{source.data + at, stop - at};
            size_t lineFeed = stringFindLast(window, '\n');
            if (lineFeed != window.size) {
                stop = at + lineFeed + 1;
            } else {
                size_t start = prTextEditCodePointStart(source, uint32_t(stop));
                stop = start > at ? start : stop;
                if (source.data[stop - 1] == '\r' && source.data[stop] == '\n')
                    ++stop;
            }
        }
        PrTextEditInputFlags keep =
            PrTextEditInputFlag_LineFeeds | PrTextEditInputFlag_Tabs | PrTextEditInputFlag_Controls;
        s.piece.resize(stop - at);
        s.piece.resize(prTextEditInputNormalize(PrStringView{source.data + at, stop - at}, keep, s.piece.data()));
        uint32_t size = prTextEditBufferSize(s.buffer);
        PrTextEditChange change{.start = size, .end = size, .text = PrStringView(s.piece)};
        prTextEditBufferApply(s.buffer, &change, 1, PrTextEditOrigin_Load);
        s.loaded = stop;
        if (s.syntax != nullptr && prSyntaxTreeUpdate(s.syntax, kInfinityF64))
            ++s.syntaxChanges;
    } while (s.loaded < source.size && prTimeMonotonicMilliseconds() < end);
    if (s.loaded == source.size)
        finishLoad(s);
}

// Loads more of the document, brings its syntax and the colors of its categories up to date, and asks for another frame
// while there is more to load or to parse.
void updateSyntax(Scribe& s, PrLayoutNode port, bool dark) {
    loadPieces(s, kLoadMilliseconds);
    if (s.syntax != nullptr) {
        if (prSyntaxTreeUpdate(s.syntax, kParseMilliseconds))
            ++s.syntaxChanges;
        uint32_t categories = prSyntaxRegistryCategoryCount(s.languages);
        if (dark != s.darkColors)
            s.categoryColors.clear();
        s.darkColors = dark;
        for (uint32_t i = uint32_t(s.categoryColors.size()); i < categories; ++i)
            s.categoryColors.push_back(colorOfCategory(prSyntaxRegistryCategoryName(s.languages, i), dark));
    }
    if (s.loading || (s.syntax != nullptr && prSyntaxTreeIsPending(s.syntax)))
        prUiRequestMotionAt(s.ui, port, prUiCurrentRound(s.ui)->time);
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

// Starts an empty document whose text streams in, in the language that `name` chooses.
void startDocument(Scribe& s, const std::string& name) {
    s.buffer = prTextEditBufferCreate("");
    s.history = prTextEditHistoryCreate(s.buffer);
    s.view = prTextViewCreate(s.buffer);
    s.savedVersion = prTextEditBufferVersion(s.buffer);
    s.languages = prSyntaxRegistryCreate();
#if defined(SCRIBE_HAS_TREESITTER)
    for (const PrSyntaxLanguage* language : {
        prSyntaxTreeSitterC(), prSyntaxTreeSitterCpp(), prSyntaxTreeSitterCss(), prSyntaxTreeSitterHtml(),
            prSyntaxTreeSitterJavaScript(), prSyntaxTreeSitterJson(), prSyntaxTreeSitterMarkdown(),
            prSyntaxTreeSitterMarkdownInline(), prSyntaxTreeSitterPython(), prSyntaxTreeSitterXml(),
    })
        prSyntaxRegistryAdd(s.languages, language);
#endif
    s.language = name.empty() ? nullptr : prSyntaxRegistryFromPath(s.languages, name);
    if (s.language != nullptr)
        s.syntax = prSyntaxTreeCreate(s.language, s.languages, s.buffer);
}

} // namespace

bool openDocument(Scribe& s, const std::string& path) {
    s.path = path;
    PrFileMapping mapping{};
    if (!path.empty()) {
        if (!prFileMap(path, &mapping))
            return false;
        if (mapping.size > size_t(kPrTextEditBufferMaxBytes)) {
            prFileUnmap(&mapping);
            return false;
        }
        // A document records its line ending, and its buffer ends lines with a line feed.
        const char* bytes = reinterpret_cast<const char*>(mapping.data);
        s.crlf = false;
        for (size_t i = 0; i + 1 < mapping.size; ++i) {
            if (bytes[i] == '\n')
                break;
            if (bytes[i] == '\r' && bytes[i + 1] == '\n') {
                s.crlf = true;
                break;
            }
        }
    }
    startDocument(s, path);
    s.mapping = mapping;
    s.loading = mapping.size != 0;
    return true;
}

void openText(Scribe& s, PrStringView text, const std::string& name) {
    startDocument(s, name);
    s.source.assign(text.data, text.size);
    s.loading = text.size != 0;
}

void closeDocument(Scribe& s) {
    prCommandKeymapDestroy(s.keymap);
    prCommandRegistryDestroy(s.commands);
    if (s.mapping.data != nullptr)
        prFileUnmap(&s.mapping);
    prSyntaxTreeDestroy(s.syntax);
    prSyntaxRegistryDestroy(s.languages);
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
        [](void* c) -> PrCommandStateFlags {
            const Scribe& editor = scribeOf(c);
            // A file that is still loading would be saved cut short.
            return editor.path.empty() || editor.loading ? PrCommandStateFlag_Disabled : 0;
        });
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
    // The theme is dark if its text is lighter than its ground.
    bool dark = palette.text1.r + palette.text1.g + palette.text1.b > palette.bg0.r + palette.bg0.g + palette.bg0.b;
    updateSyntax(s, port, dark);
    PrUiTextViewDesc view{
        .view = s.view,
        .control = port,
        .extent = PrVec2{width, portHeight},
        .rowStyle = rowStyle,
        .spans = s.syntax != nullptr ? spansOf : nullptr,
        .spansContext = &s,
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
    std::string state = dirty ? "edited" : "saved";
    if (s.loading) {
        size_t total = sourceOf(s).size;
        state = "loading " + std::to_string(total != 0 ? s.loaded * 100 / total : 0) + "%";
    }
    char where[128];
    std::string language = "plain text";
    if (s.language != nullptr) {
        language.assign(s.language->name.data, s.language->name.size);
        if (prSyntaxTreeIsPending(s.syntax))
            language += ", parsing";
    }
    snprintf(where, sizeof(where), "%s  Ln %u, Col %u  %u lines  %s%s  %s", state.c_str(),
        info.caret.line + 1, info.caret.column + 1, info.lineCount, s.wrap ? "wrap" : "no wrap", s.crlf ? "  CRLF" : "  LF",
        language.c_str());
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
    return prTextViewRefine(s.view, kRefineMilliseconds) || again || s.loading;
}

} // namespace scribe
