// The gallery's controls: buttons, inputs, toggles, tabs, and the menus and popovers.

#include "examples/studio_gallery/section_controls.h"

#include "pr/count_of.h"
#include "pr/kit_button.h"
#include "pr/kit_choice.h"
#include "pr/kit_color.h"
#include "pr/kit_input.h"
#include "pr/kit_menu.h"
#include "pr/kit_theme.h"
#include "pr/text_edit.h"
#include "pr/ui.h"
#include "pr/ui_text_edit.h"

#include "examples/studio_gallery/gallery_parts.h"

namespace gallery {

using namespace pr;

namespace {

// The width of a field's caption, in px.
constexpr float kCaptionWidth = 92;

// The styles of the styled text area, by PrTextEditStyle, and the names of their buttons.
constexpr PrKitTextAreaStyle kStyledLooks[] = {
    {PrKitTextRole_Body, PrKitTone_Auto},
    {PrKitTextRole_Strong, PrKitTone_Auto},
    {PrKitTextRole_BodyItalic, PrKitTone_Auto},
    {PrKitTextRole_Body, PrKitTone_Accent},
    {PrKitTextRole_Heading, PrKitTone_Auto},
};
constexpr const char* kStyledNames[] = {"Plain", "Strong", "Italic", "Accent", "Heading"};

// The tools of a tool row.
constexpr PrKitIconKind kToolIcons[] = {
    PrKitIconKind_Cursor, PrKitIconKind_Move, PrKitIconKind_Rotate,
    PrKitIconKind_Scale,
};

// A command of the file menu.
struct MenuRow {
    const char* label;
    const char* shortcut;
    bool separatorBefore;
    bool disabled;
};

constexpr MenuRow kFileMenu[] = {
    {"New Scene", "Cmd N", false, false},       {"Open Scene...", "Cmd O", false, false},
    {"Save", "Cmd S", false, false},            {"Save As...", "Shift Cmd S", false, true},
    {"Build and Run", "Cmd B", true, false},
};

// A verb of the context menu.
struct ContextVerb {
    const char* label;
    const char* shortcut;
};

constexpr ContextVerb kEditVerbs[] = {{"Cut", "Cmd X"}, {"Copy", "Cmd C"}, {"Paste", "Cmd V"}};
constexpr ContextVerb kRowVerbs[] = {{"Duplicate", "Cmd D"}, {"Rename", "F2"}};

// Records the bound controls' gestures as states of the undo history.
void undoEdit(void* context, PrLayoutNode, PrUiEditPhase phase) {
    UndoState& undo = *static_cast<UndoState*>(context);
    switch (phase) {
        case PrUiEditPhase_Begin:
            undoBegin(undo);
            ++undo.begins;
            break;
        case PrUiEditPhase_Change:
            undo.wrote = true;
            ++undo.changes;
            break;
        default:
            undoCommit(undo);
            ++undo.commits;
            break;
    }
}

// Creates a row with `gap` between its children, centered across, that wraps if `wrap` is true.
PrLayoutNode specimenRow(PrKit* kit, PrLayoutNode parent, float gap, bool wrap) {
    PrKitBoxStyle row{.direction = PrKitDirection_Row, .align = PrKitAlign_Center, .wrap = wrap, .gap = gap};
    return prKitBox(kit, parent, {}, row);
}

// Places `node` at a fixed width.
void fixedWidth(PrKit* kit, PrLayoutNode node, float width) {
    PrKitPlace place{.width = prKitPx(width), .flex = PrKitFlex_Fixed};
    prKitPlace(kit, node, &place);
}

void buildMenuBar(PrKit* kit, PrLayoutNode section) {
    PrLayoutNode panel = demoPanel(kit, section, "MENU BAR", "FILE OPENS", true);
    PrLayoutNode bar = prKitMenuBar(kit, panel, {});
    const char* const labels[] = {"File", "Edit", "View", "Build", "Window", "Help"};
    for (const char* label : labels) {
        PrLayoutNode item = prKitMenuBarItem(kit, bar, {.text = label});
        PrLayoutNode popover = prKitPopover(kit, item, {}, {});
        if (popover == kPrLayoutNodeNone)
            continue;
        PrKitMenuStyle wide{.minWidth = 240};
        PrLayoutNode menu = prKitMenu(kit, popover, {}, wide);
        prKitMenuLabel(kit, menu, {.text = label});
        for (const MenuRow& row : kFileMenu) {
            if (row.separatorBefore)
                prKitMenuSeparator(kit, menu, {});
            prKitMenuItem(kit, menu, {.text = row.label, .shortcut = row.shortcut, .disabled = row.disabled}, {});
        }
    }
}

// Creates a select of `count` labeled options, and returns its field. The option at index `*selected` is the chosen
// one. Choosing an option, or stepping to it with Up and Down, writes its index to `selected`.
PrLayoutNode select(PrKit* kit, PrLayoutNode parent, const PrStringView* labels, uint32_t count, uint32_t* selected,
        bool disabled) {
    PrStringView label = *selected < count ? labels[*selected] : PrStringView{};
    PrLayoutNode field = prKitSelect(kit, parent, {.label = label, .disabled = disabled}, {});
    int32_t step = 0;
    if (prKitStepped(kit, field, &step) && count != 0) {
        if (step < 0 && *selected > 0)
            --*selected;
        else if (step > 0 && *selected + 1 < count)
            ++*selected;
    }
    PrLayoutNode menu = prKitSelectMenu(kit, field);
    if (menu == kPrLayoutNodeNone)
        return field;
    for (uint32_t i = 0; i < count; ++i) {
        if (prKitActivated(kit, prKitMenuItem(kit, menu, {.text = labels[i], .checked = *selected == i}, {})))
            *selected = i;
    }
    return field;
}

void buildSelect(PrKit* kit, PrLayoutNode section, MenuState& state) {
    PrLayoutNode panel = demoPanel(kit, section, "SELECT", "DROPDOWN", false);
    uint32_t typeCount = 0;
    const PrStringView* types = galleryComponentTypes(&typeCount);
    PrLayoutNode row = specimenRow(kit, panel, kPrKitSpace4, false);
    fixedWidth(kit, select(kit, row, types, typeCount, &state.component, false), 220);
    prKitLabel(kit, row, {.text = "Up and Down step the value while the list is closed"}, kBodySecondary);
    // A list longer than the room it is given scrolls, with the chosen item in view.
    uint32_t clipCount = 0;
    const PrStringView* clips = galleryAnimationClips(&clipCount);
    PrLayoutNode longRow = specimenRow(kit, panel, kPrKitSpace4, false);
    fixedWidth(kit, select(kit, longRow, clips, clipCount, &state.clip, false), 220);
    prKitLabel(kit, longRow, {.text = "66 clips, clamped and scrolled"}, kBodySecondary);
    PrLayoutNode disabledRow = specimenRow(kit, panel, kPrKitSpace4, false);
    fixedWidth(kit, select(kit, disabledRow, types, typeCount, &state.component, true), 220);
    prKitLabel(kit, disabledRow, {.text = "a disabled select shows its value and takes no input"}, kBodySecondary);
}

// Creates the context menu's items: verbs, a row of tag colors, a check item and a destructive verb.
void buildContextMenuItems(PrKit* kit, PrLayoutNode menu, MenuState& state) {
    const PrKitPalette& palette = kit->theme.palette;
    PrUi* ui = kit->ui;
    prKitMenuLabel(kit, menu, {.text = "EDIT"});
    for (const ContextVerb& verb : kEditVerbs) {
        PrKitMenuItem item{.text = verb.label, .shortcut = verb.shortcut};
        if (prUiActivated(ui, prKitMenuItem(kit, menu, item, {})))
            state.verb = verb.label;
    }
    prKitMenuSeparator(kit, menu, {});
    prKitMenuLabel(kit, menu, {.text = "TAG"});
    PrKitBoxStyle line{
        .direction = PrKitDirection_Row,
        .gap = kPrKitSpace2,
        .padding = prKitEdgesXY(kPrKitSpace1, kPrKitSpace3),
    };
    PrLayoutNode swatches = prKitBox(kit, menu, {}, line);
    const PrColor tags[] = {palette.accent, palette.success, palette.warning, palette.danger, palette.axisW};
    for (const PrColor& tag : tags) {
        bool chosen = tag.r == state.tag.r && tag.g == state.tag.g && tag.b == state.tag.b;
        PrLayoutNode node =
            prKitSelectable(kit, swatches, {.selected = chosen}, {.padding = prKitEdgesAll(kPrKitSpace1)});
        prKitSwatch(kit, node, {.color = tag}, {.size = kPrKitSpace5});
        if (prKitActivated(kit, node))
            state.tag = tag;
    }
    prKitMenuSeparator(kit, menu, {});
    PrKitMenuItem wrap{.text = "Word Wrap", .checked = state.wrap};
    if (prUiActivated(ui, prKitMenuItem(kit, menu, wrap, {})))
        state.wrap = !state.wrap;
    prKitMenuSeparator(kit, menu, {});
    for (const ContextVerb& verb : kRowVerbs) {
        PrKitMenuItem item{.text = verb.label, .shortcut = verb.shortcut};
        if (prUiActivated(ui, prKitMenuItem(kit, menu, item, {})))
            state.verb = verb.label;
    }
    prKitMenuSeparator(kit, menu, {});
    PrKitMenuItem remove{.text = "Delete", .shortcut = "Del"};
    PrKitMenuItemStyle danger{.variant = PrKitMenuItemVariant_Danger};
    if (prUiActivated(ui, prKitMenuItem(kit, menu, remove, danger)))
        state.verb = "Delete";
}

void buildContextMenu(PrKit* kit, PrLayoutNode section, MenuState& state) {
    PrLayoutNode panel = demoPanel(kit, section, "CONTEXT MENU", "RIGHT-CLICK", false);
    PrKitBoxStyle line{.direction = PrKitDirection_Row, .align = PrKitAlign_Start, .gap = kPrKitSpace4};
    PrLayoutNode row = prKitBox(kit, panel, {}, line);
    PrKitSurfaceStyle zone{.ground = PrKitGround_Viewport};
    zone.box.direction = PrKitDirection_Row;
    zone.box.justify = PrKitJustify_Center;
    zone.box.align = PrKitAlign_Center;
    zone.box.place.width = prKitPx(220);
    zone.box.place.height = prKitPx(140);
    zone.box.place.flex = PrKitFlex_Fixed;
    PrLayoutNode host = prKitSurface(kit, row, {}, zone);
    prKitLabel(kit, host, {.text = "RIGHT-CLICK ANYWHERE"}, kMonoSecondary);
    PrKitMenuStyle wide{.minWidth = 220};
    PrLayoutNode menu = prKitContextMenu(kit, host, {}, wide);
    if (menu != kPrLayoutNodeNone)
        buildContextMenuItems(kit, menu, state);

    // A press outside the menu closes it, and reaches this button as well.
    PrKitBoxStyle column{.align = PrKitAlign_Start, .gap = kPrKitSpace2};
    PrLayoutNode readout = prKitBox(kit, row, {}, column);
    if (prKitActivated(kit, prKitButton(kit, readout, {.text = "Not A Menu Item"}, {})))
        ++state.outsideClicks;
    char clicks[32];
    prKitLabel(kit, readout, {.text = formatToBuffer(Span(clicks), "clicks: {}", state.outsideClicks)}, kMonoMuted);
    PrStringView verb = state.verb;
    if (state.verb.isEmpty())
        verb = "no verb chosen yet";
    prKitLabel(kit, readout, {.text = verb}, kMonoSecondary);
    PrLayoutNode tag = prKitBox(kit, readout, {}, {.direction = PrKitDirection_Row, .gap = kPrKitSpace2});
    prKitSwatch(kit, tag, {.color = state.tag}, {.size = kPrKitSpace5});
    prKitLabel(kit, tag, {.text = state.wrap ? "wrap: on" : "wrap: off"}, kMonoMuted);
}

void buildPalette(PrKit* kit, PrLayoutNode section, PaletteState& palette) {
    PrLayoutNode panel = demoPanel(kit, section, "COMMAND PALETTE", "UP . DOWN . ENTER", false);
    PrLayoutNode row = specimenRow(kit, panel, kPrKitSpace3, false);
    PrLayoutNode trigger = prKitButton(kit, row, {.text = "Open Palette...", .icon = PrKitIconKind_Search}, {});
    if (prKitActivated(kit, trigger))
        prKitSetOpen(kit, trigger, true);
    prKitLabel(kit, row, {.text = "a modal at the top of the frame, over a list that the keys walk"}, kBodySecondary);
    uint32_t count = 0;
    const CommandItem* commands = galleryCommands(&count);
    uint32_t shown[16];
    uint32_t matches = 0;
    for (uint32_t i = 0; i < count && matches < countOf(shown); ++i) {
        if (commandMatches(commands[i], palette.query))
            shown[matches++] = i;
    }
    PrKitCommandPalette content{
        .prompt = "Type a command",
        .count = matches,
        .query = palette.query,
        .searchable = true,
    };
    PrLayoutNode list = prKitCommandPalette(kit, trigger, content);
    if (list == kPrLayoutNodeNone)
        return;
    PrStringView text{};
    if (prKitTextChanged(kit, list, &text))
        palette.query = text;
    for (uint32_t i = 0; i < matches; ++i) {
        const CommandItem& command = commands[shown[i]];
        PrKitCommandItem item{
            .text = command.label,
            .category = command.category,
            .shortcut = command.shortcut != nullptr ? command.shortcut : PrStringView{},
            .icon = command.icon,
        };
        if (prUiActivated(kit->ui, prKitCommandItem(kit, list, item)))
            palette.active = shown[i];
    }
    prKitCommandPaletteFooter(kit, list);
}

} // namespace

void sectionButtons(PrKit* kit, PrLayoutNode parent) {
    PrLayoutNode section = sectionFrame(kit, parent, "05", "Buttons", "Four variants across four height steps.",
        "Primary for the one dominant action of a surface, secondary for the neutral options, ghost for "
            "low-weight and toolbar use, and danger for what destroys something.");
    {
        PrLayoutNode panel = demoPanel(kit, section, "VARIANTS", "4 TYPES", false);
        PrLayoutNode row = specimenRow(kit, panel, kPrKitSpace3, true);
        prKitButton(kit, row, {.text = "Import Asset"}, kPrimary);
        prKitButton(kit, row, {.text = "Cancel"}, {});
        prKitButton(kit, row, {.text = "Clear Selection"}, kGhost);
        prKitButton(kit, row, {.text = "Delete Entity"}, kDanger);
        prKitButton(kit, row, {.text = "Disabled", .disabled = true}, {});
    }
    {
        PrLayoutNode panel = demoPanel(kit, section, "SIZES", "XS . SM . MD . LG", false);
        PrLayoutNode row = specimenRow(kit, panel, kPrKitSpace3, false);
        const PrKitControlSize sizes[] = {
            PrKitControlSize_Xs, PrKitControlSize_Sm, PrKitControlSize_Md,
            PrKitControlSize_Lg,
        };
        const char* const labels[] = {"OK", "Save", "Play", "Build Project"};
        for (uint32_t i = 0; i < 4; ++i) {
            PrKitButtonStyle style{.variant = PrKitButtonVariant_Primary, .size = sizes[i]};
            prKitButton(kit, row, {.text = labels[i]}, style);
        }
    }
    {
        PrLayoutNode panel = demoPanel(kit, section, "ICON BUTTONS", "SQUARE . LABELLED", false);
        PrLayoutNode row = specimenRow(kit, panel, kPrKitSpace3, false);
        for (PrKitIconKind icon : kToolIcons)
            prKitButton(kit, row, {.icon = icon}, {});
        prKitButton(kit, row, {.text = "Add Entity", .icon = PrKitIconKind_Plus}, {});
        prKitButton(kit, row, {.text = "Search", .icon = PrKitIconKind_Search}, {});
    }
    {
        PrLayoutNode panel = demoPanel(kit, section, "TOOLTIPS", "HOVER . VERB . CHORD", false);
        PrLayoutNode row = specimenRow(kit, panel, kPrKitSpace3, false);
        PrLayoutNode undo = prKitButton(kit, row, {.text = "Undo", .icon = PrKitIconKind_Undo}, {});
        PrKitTooltip undoTip{.text = "Undo the last edit", .shortcut = "Cmd Z"};
        prKitTooltip(kit, undo, undoTip);
        PrLayoutNode redo = prKitButton(kit, row, {.icon = PrKitIconKind_Redo}, {});
        PrKitTooltip redoTip{.text = "Redo the last undone edit", .shortcut = "Cmd Shift Z"};
        prKitTooltip(kit, redo, redoTip);
        PrLayoutNode trash = prKitButton(kit, row, {.icon = PrKitIconKind_Trash},
            {.variant = PrKitButtonVariant_Ghost, .size = PrKitControlSize_Md});
        PrKitTooltip trashTip{.text = "Delete selection", .shortcut = "Del"};
        prKitTooltip(kit, trash, trashTip);
    }
    {
        PrLayoutNode panel = demoPanel(kit, section, "BUTTON GROUP", "SHARED BORDER", false);
        PrLayoutNode row = specimenRow(kit, panel, kPrKitSpace6, false);
        PrLayoutNode icons = prKitButtonGroup(kit, row, {});
        for (PrKitIconKind icon : kToolIcons)
            prKitButton(kit, icons, {.icon = icon}, {});
        PrLayoutNode spaces = prKitButtonGroup(kit, row, {});
        PrKitButtonStyle small{.size = PrKitControlSize_Sm};
        prKitButton(kit, spaces, {.text = "Local"}, small);
        PrKitButton world{.text = "World", .selected = true};
        prKitButton(kit, spaces, world, small);
    }
}

void sectionInputs(PrKit* kit, PrLayoutNode parent, GalleryState& state) {
    InputsState& inputs = state.inputs;
    PrLayoutNode section = sectionFrame(kit, parent, "06", "Inputs", "Text, numeric, vector, slider and color.",
        "Every field here is live. A numeric field scrubs under a horizontal drag, and steps under Up and Down "
            "while it has focus; a slider follows the pointer, and its track maps a ratio if its range spans decades.");
    {
        PrLayoutNode panel = demoPanel(kit, section, "TEXT", "EDITABLE . SELECT ON FOCUS . DISABLED", false);
        PrKitBoxStyle line{.direction = PrKitDirection_Row, .align = PrKitAlign_Start, .gap = kPrKitSpace4};
        PrLayoutNode row = prKitBox(kit, panel, {}, line);
        PrKitBoxStyle cell{.gap = kPrKitSpace2};
        cell.place.flex = PrKitFlex_Fill;
        PrLayoutNode name = prKitBox(kit, row, {}, cell);
        caption(kit, name, "ENTITY NAME");
        PrKitTextField nameField{.text = inputs.name, .placeholder = "Unnamed entity"};
        PrStringView text{};
        if (prKitTextChanged(kit, prKitTextField(kit, name, nameField, {}), &text))
            inputs.name = text;
        PrLayoutNode search = prKitBox(kit, row, {}, cell);
        caption(kit, search, "SELECT ON FOCUS");
        PrKitTextField searchField{
            .text = inputs.search,
            .placeholder = "Search query",
            .icon = PrKitIconKind_Search,
            .clearable = true,
            .selectOnFocus = true,
        };
        if (prKitTextChanged(kit, prKitTextField(kit, search, searchField, {}), &text))
            inputs.search = text;
        PrLayoutNode locked = prKitBox(kit, row, {.disabled = true}, cell);
        caption(kit, locked, "DISABLED");
        PrKitTextField lockedField{.text = inputs.locked};
        if (prKitTextChanged(kit, prKitTextField(kit, locked, lockedField, {}), &text))
            inputs.locked = text;
    }
    {
        PrLayoutNode panel = demoPanel(kit, section, "HISTORY", "ENTER SUBMITS . UP AND DOWN RECALL", false);
        PrLayoutNode row = captionRow(kit, panel, "COMMAND", kCaptionWidth);
        PrStringView lines[16];
        uint32_t count = 0;
        for (const String& command : inputs.commands) {
            if (count < 16)
                lines[count++] = command;
        }
        PrKitTextField command{
            .text = inputs.command,
            .placeholder = "type a line, submit it, then press Up",
            .history = lines,
            .historyCount = count,
        };
        PrKitFieldStyle mono{.mono = true};
        PrLayoutNode field = prKitTextField(kit, row, command, mono);
        PrStringView text{};
        if (prKitTextChanged(kit, field, &text))
            inputs.command = text;
        // The field leaves Enter to its caller, which submits the line.
        PrUiEvents events = prUiEvents(kit->ui, field);
        for (uint32_t i = 0; i < events.count; ++i) {
            if (events.events[i].kind == PrAppEventKind_KeyDown && events.events[i].key == PrAppKey_Enter)
                submitCommand(inputs);
        }
        char submitted[32];
        prKitLabel(kit, row, {.text = formatToBuffer(Span(submitted), "{} submitted", inputs.commands.count())}, kCaption);
    }
    {
        PrLayoutNode panel = demoPanel(kit, section, "SUGGEST", "FREE TEXT . KNOWN VALUES", false);
        uint32_t areaCount = 0;
        const PrStringView* areas = galleryAreas(&areaCount);
        PrKitFieldStyle mono{.mono = true};
        PrKitSuggestField area{
            .field = {
                .text = inputs.area,
                .placeholder = "Area path",
                .icon = PrKitIconKind_Search,
                .selectOnFocus = true,
            },
            .suggestions = areas,
            .count = areaCount,
        };
        PrStringView text{};
        if (prKitTextChanged(kit, prKitSuggestField(kit, captionRow(kit, panel, "AREA", kCaptionWidth), area, mono), &text))
            inputs.area = text;
        uint32_t revisionCount = 0;
        const PrStringView* revisions = galleryRevisions(&revisionCount);
        PrKitSuggestField revision{
            .field = {
                .text = inputs.revision,
                .placeholder = "Branch, tag or commit",
                .icon = PrKitIconKind_Branch,
                .selectOnFocus = true,
            },
            .suggestions = revisions,
            .count = revisionCount,
        };
        PrLayoutNode revisionRow = captionRow(kit, panel, "REVISION", kCaptionWidth);
        if (prKitTextChanged(kit, prKitSuggestField(kit, revisionRow, revision, mono), &text))
            inputs.revision = text;
        prKitLabel(kit, panel, {.text = "the caller filters the items, so the list shows what it hands over"},
            kBodySecondary);
    }
    {
        PrLayoutNode panel = demoPanel(kit, section, "TEXT AREA", "SOFT WRAP . SHADER SNIPPET", false);
        PrStringView text{};
        // Runs set chords in their own role within the sentence around them.
        constexpr PrKitTextRun kChord = {.role = PrKitTextRole_MonoStrong, .tone = PrKitTone_Accent};
        constexpr PrKitTextRun kStrongRun = {.role = PrKitTextRole_Strong};
        constexpr PrKitTextRun kPlainRun = {.tone = PrKitTone_Secondary};
        auto run = [](PrStringView text, PrKitTextRun look) {
            look.text = text;
            return look;
        };
        const PrKitTextRun gestures[] = {
            run("Double-click", kStrongRun),
            run(" selects a word, ", kPlainRun),
            run("triple-click", kStrongRun),
            run(" the line, and a fourth click everything; hold the drag and the selection grows by that unit. ",
                kPlainRun),
            run("Cmd Left", kChord),
            run(" and ", kPlainRun),
            run("Cmd Right", kChord),
            run(" move to the line edges, ", kPlainRun),
            run("Cmd Up", kChord),
            run(" and ", kPlainRun),
            run("Cmd Down", kChord),
            run(" to the ends of the text, each with ", kPlainRun),
            run("Shift", kChord),
            run(" to extend the selection.", kPlainRun),
        };
        prKitText(kit, panel, {.runs = gestures, .count = uint32_t(countOf(gestures))}, {});
        PrKitBoxStyle line{.direction = PrKitDirection_Row, .align = PrKitAlign_Start, .gap = kPrKitSpace4};
        PrLayoutNode row = prKitBox(kit, panel, {}, line);
        PrKitBoxStyle cell{.gap = kPrKitSpace2};
        cell.place.flex = PrKitFlex_Fill;
        PrLayoutNode notes = prKitBox(kit, row, {}, cell);
        caption(kit, notes, "NOTES");
        PrKitTextArea prose{.text = inputs.prose, .placeholder = "Notes", .rows = 5};
        if (prKitTextChanged(kit, prKitTextArea(kit, notes, prose, {}), &text))
            inputs.prose = text;
        PrLayoutNode code = prKitBox(kit, row, {}, cell);
        caption(kit, code, "SHADER SNIPPET");
        PrKitTextArea snippet{.text = inputs.snippet, .rows = 5};
        PrKitFieldStyle mono{.mono = true};
        if (prKitTextChanged(kit, prKitTextArea(kit, code, snippet, mono), &text))
            inputs.snippet = text;
    }
    {
        PrLayoutNode panel = demoPanel(kit, section, "STYLED TEXT", "STYLE RUNS IN ONE BUFFER", false);
        PrKitTextArea styled{
            .buffer = inputs.styled,
            .rows = 5,
            .styles = kStyledLooks,
            .styleCount = countOf(kStyledLooks),
        };
        PrLayoutNode tools = prKitBox(kit, panel, {}, {.direction = PrKitDirection_Row, .gap = kPrKitSpace2});
        // A button takes focus from the area, and the area takes it back, with its selection as it stood.
        bool chosen[countOf(kStyledNames)] = {};
        for (uint32_t i = 0; i < countOf(kStyledNames); ++i)
            chosen[i] = prKitActivated(kit, prKitButton(kit, tools, {.text = kStyledNames[i]}, {}));
        bool undo = prKitActivated(kit, prKitButton(kit, tools, {.text = "Undo"}, {}));
        bool redo = prKitActivated(kit, prKitButton(kit, tools, {.text = "Redo"}, {}));
        PrLayoutNode area = prKitTextArea(kit, panel, styled, {});
        PrTextEditView* view = prUiTextEditView(kit->ui, area);
        bool acted = undo || redo;
        for (uint32_t i = 0; view != nullptr && i < countOf(kStyledNames); ++i) {
            if (chosen[i])
                prTextEditViewSetStyle(view, PrTextEditStyle(i));
            acted = acted || chosen[i];
        }
        if (undo && view != nullptr)
            prTextEditHistoryUndo(inputs.styledHistory, view);
        if (redo && view != nullptr)
            prTextEditHistoryRedo(inputs.styledHistory, view);
        if (acted)
            prUiSetFocus(kit->ui, area);
    }
    {
        PrLayoutNode panel = demoPanel(kit, section, "NUMERIC . VECTORS", "DRAG TO SCRUB", false);
        PrKitVector uv{.value = prKitScalarF32(inputs.uv), .count = 2, .range = {.step = 0.01}, .precision = 3};
        prKitVectorField(kit, captionRow(kit, panel, "UV", kCaptionWidth), uv, {});
        PrKitVector position{.value = prKitScalarF32(inputs.position), .count = 3, .range = {.step = 0.05}};
        prKitVectorField(kit, captionRow(kit, panel, "POSITION", kCaptionWidth), position, {});
        PrKitVector rotation{
            .value = prKitScalarF32(inputs.rotation),
            .count = 3,
            .range = {.step = 0.5},
            .precision = 1,
        };
        prKitVectorField(kit, captionRow(kit, panel, "ROTATION", kCaptionWidth), rotation, {});
        PrKitVector orientation{
            .value = prKitScalarF32(inputs.quaternion),
            .count = 4,
            .range = {.step = 0.01},
            .precision = 3,
        };
        prKitVectorField(kit, captionRow(kit, panel, "ORIENTATION", kCaptionWidth), orientation, {});
        PrLayoutNode scalar = captionRow(kit, panel, "SCALAR", kCaptionWidth);
        PrKitNumberRange unit{.step = 0.01, .min = 0, .max = 1, .hasMin = true, .hasMax = true};
        PrKitNumber scalarNumber{.value = prKitScalarF32(&inputs.scalar), .range = unit};
        fixedWidth(kit, prKitNumberField(kit, scalar, scalarNumber, {}), 120);
        PrLayoutNode units = captionRow(kit, panel, "UNITS", kCaptionWidth);
        PrKitNumber mass{
            .value = prKitScalarF32(&inputs.mass),
            .range = {.step = 0.5, .min = 0, .hasMin = true},
            .unit = "kg",
            .precision = 1,
        };
        fixedWidth(kit, prKitNumberField(kit, units, mass, {}), 120);
        PrKitNumber shutter{
            .value = prKitScalarF32(&inputs.shutter),
            .range = {.step = 0.001, .min = 0, .hasMin = true},
            .unit = "s",
            .precision = 4,
        };
        fixedWidth(kit, prKitNumberField(kit, units, shutter, {}), 120);
        PrKitNumber seed{.value = prKitScalarU32(&inputs.seed), .range = {.step = 1}, .unit = "Hz"};
        fixedWidth(kit, prKitNumberField(kit, units, seed, {}), 120);
    }
    {
        PrLayoutNode panel = demoPanel(kit, section, "TYPED SCALARS", "I8-U64 . F32 . F64", false);
        struct TypedCell {
            const char* caption;
            PrKitScalar value;
        };
        const TypedCell cells[] = {
            {"I8", prKitScalarI8(&inputs.tiny)},          {"U8", prKitScalarU8(&inputs.level)},
            {"I16", prKitScalarI16(&inputs.offset)},      {"U16", prKitScalarU16(&inputs.stride)},
            {"F32", prKitScalarF32(&inputs.scalar)},      {"I32", prKitScalarI32(&inputs.subdivisions)},
            {"U32", prKitScalarU32(&inputs.seed)},        {"I64", prKitScalarI64(&inputs.budget)},
            {"U64", prKitScalarU64(&inputs.cycles)},      {"F64", prKitScalarF64(&inputs.density)},
        };
        Grid grid = gridBegin(kit, panel, 5, kPrKitSpace4, false);
        for (const TypedCell& cell : cells) {
            PrKitBoxStyle column{.gap = kPrKitSpace2};
            PrLayoutNode box = prKitBox(kit, gridCell(grid), {}, column);
            caption(kit, box, cell.caption);
            PrKitNumber number{.value = cell.value, .range = {.step = 0.5}};
            prKitNumberField(kit, box, number, {});
        }
        gridEnd(grid);
        prKitLabel(kit, panel, {.text = "an integer kind rounds what it writes and never leaves its own range"},
            kBodySecondary);
    }
    {
        PrLayoutNode panel = demoPanel(kit, section, "SLIDERS", "TRACK . FIELD . RATIO", false);
        struct SliderRow {
            const char* caption;
            PrKitScalar value;
            double min;
            double max;
            PrKitSliderScale scale;
            PrStringView unit;
            uint8_t precision;
        };
        const SliderRow rows[] = {
            {"STRENGTH", prKitScalarF32(&inputs.scalar), 0, 1, PrKitSliderScale_Linear, {}, 2},
            {"MASS", prKitScalarF32(&inputs.mass), 0, 200, PrKitSliderScale_Linear, "kg", 1},
            {"STEPS", prKitScalarI32(&inputs.subdivisions), 0, 8, PrKitSliderScale_Linear, {}, 0},
            {"20-20K HZ", prKitScalarF32(&inputs.frequency), 20, 20000, PrKitSliderScale_Ratio, "Hz", 1},
            {"SHUTTER", prKitScalarF32(&inputs.shutterSeconds), 1.0 / 8000, 30, PrKitSliderScale_Ratio, "s", 4},
        };
        for (const SliderRow& row : rows) {
            PrLayoutNode line = captionRow(kit, panel, row.caption, 72);
            PrKitSlider slider{.value = row.value, .min = row.min, .max = row.max, .scale = row.scale};
            prKitSlider(kit, line, slider, {});
            PrKitNumber field{
                .value = row.value,
                .range = {.min = row.min, .max = row.max, .hasMin = true, .hasMax = true},
                .unit = row.unit,
                .precision = row.precision,
            };
            fixedWidth(kit, prKitNumberField(kit, line, field, {}), 88);
        }
    }
    {
        UndoState& undo = inputs.undo;
        PrLayoutNode panel = demoPanel(kit, section, "EDIT LIFECYCLE", "SINK . UNDO . REDO", false);
        PrKitBoxStyle line{.direction = PrKitDirection_Row, .align = PrKitAlign_End, .gap = kPrKitSpace6};
        PrLayoutNode row = prKitBox(kit, panel, {}, line);
        prUiSetEditSink(kit->ui, row, undoEdit, &undo);
        PrKitBoxStyle cell{.gap = kPrKitSpace2};
        cell.place.flex = PrKitFlex_Fill;
        PrLayoutNode strength = prKitBox(kit, row, {}, cell);
        caption(kit, strength, "STRENGTH");
        prKitSlider(kit, strength, {.value = prKitScalarF32(&undo.values.strength), .min = 0, .max = 1}, {});
        PrLayoutNode steps = prKitBox(kit, row, {}, cell);
        caption(kit, steps, "STEPS");
        PrKitNumberRange range{.step = 1, .min = 0, .max = 64, .hasMin = true, .hasMax = true};
        prKitNumberField(kit, steps, {.value = prKitScalarI32(&undo.values.steps), .range = range}, {});
        PrLayoutNode enabled = prKitBox(kit, row, {}, cell);
        caption(kit, enabled, "ENABLED");
        prKitToggle(kit, enabled, {.value = &undo.values.enabled}, {});

        PrLayoutNode readout = specimenRow(kit, panel, kPrKitSpace8, false);
        const char* const labels[] = {"BEGIN", "CHANGE", "COMMIT", "STATES", "AT"};
        const uint32_t counts[] = {undo.begins, undo.changes, undo.commits, undo.history.count(), undo.cursor};
        for (uint32_t i = 0; i < 5; ++i) {
            PrKitBoxStyle column{.gap = kPrKitSpace1};
            PrLayoutNode box = prKitBox(kit, readout, {}, column);
            char value[16];
            prKitLabel(kit, box, {.text = formatToBuffer(Span(value), "{}", counts[i])}, kMonoStrong);
            caption(kit, box, labels[i]);
        }
        PrKitButton undoButton{.text = "Undo", .icon = PrKitIconKind_Undo, .disabled = !undoAvailable(undo)};
        bool undoing = prKitActivated(kit, prKitButton(kit, readout, undoButton, {}));
        PrKitButton redoButton{.text = "Redo", .icon = PrKitIconKind_Redo, .disabled = !redoAvailable(undo)};
        bool redoing = prKitActivated(kit, prKitButton(kit, readout, redoButton, {}));
        if (undoing || redoing)
            undoStep(undo, redoing);
        fillLabel(kit, readout, "a scrub across many frames is one state, and an edit after an undo forks",
            kBodySecondary);
    }
    {
        PrLayoutNode panel = demoPanel(kit, section, "COLOR", "FIELD . HEX", false);
        PrLayoutNode row = specimenRow(kit, panel, kPrKitSpace3, false);
        fixedWidth(kit, prKitColorField(kit, row, {.color = &inputs.tint}, {}), 180);
        prKitLabel(kit, row, {.text = "the field shows the color and its hex, and raises a picker when activated"},
            kBodySecondary);
    }
}

void sectionToggles(PrKit* kit, PrLayoutNode parent, GalleryState& state) {
    ToggleState& toggles = state.toggles;
    PrLayoutNode section = sectionFrame(kit, parent, "07", "Checkboxes, Radios, Toggles", "Binary state, in every flavor.", {});
    Grid columns = gridBegin(kit, section, 3, kPrKitSpace4, false);
    {
        PrLayoutNode panel = demoPanel(kit, gridCell(columns), "CHECKBOXES", "LIVE", false);
        const PrKitCheck checks[] = {
            {.value = &toggles.castShadows, .label = "Cast Shadows"},
            {.value = &toggles.occlusionCull, .label = "Occlusion Cull"},
            {.value = &toggles.children, .label = "Children"},
            {.value = &toggles.locked, .label = "Locked"},
        };
        for (const PrKitCheck& check : checks)
            prKitCheckbox(kit, panel, check, {});
    }
    {
        PrLayoutNode panel = demoPanel(kit, gridCell(columns), "RADIO GROUP", "LIVE", false);
        const PrStringView shadings[] = {"Unlit", "Lambert", "PBR Standard", "Toon"};
        PrLayoutNode group = prKitRadioGroup(kit, panel, {}, {});
        for (uint32_t i = 0; i < 4; ++i) {
            if (prKitActivated(kit, prKitRadio(kit, group, {.label = shadings[i], .selected = toggles.shading == i})))
                toggles.shading = i;
        }
    }
    {
        PrLayoutNode panel = demoPanel(kit, gridCell(columns), "TOGGLES", "LIVE", false);
        const PrKitCheck checks[] = {
            {.value = &toggles.physics, .label = "Physics Simulation"},
            {.value = &toggles.snap, .label = "Snap to Grid"},
            {.value = &toggles.gizmos, .label = "Show Gizmos"},
        };
        for (const PrKitCheck& check : checks)
            prKitToggle(kit, panel, check, {});
    }
    gridEnd(columns);
}

void sectionTabs(PrKit* kit, PrLayoutNode parent, GalleryState& state) {
    TabsState& tabs = state.tabs;
    PrLayoutNode section = sectionFrame(kit, parent, "08", "Tabs & Segmented", "Navigation, and mutually exclusive view modes.", {});
    {
        PrLayoutNode panel = demoPanel(kit, section, "TABS", "PANEL NAV . DRAG TO REORDER", true);
        // What each tab shows: a label, a badge if it has one, and a close control if it is closable.
        struct PanelTab {
            const char* label;
            const char* badge;
            bool closable;
        };
        const PanelTab all[] = {
            {"Scene", nullptr, false},
            {"Game", nullptr, false},
            {"Profiler", "3", false},
            {"Animator", nullptr, false},
            {"Shader Graph", nullptr, true},
        };
        // The strip shows the tabs in the model's order, keyed by the model's tab, and the model applies a drag.
        PrLayoutNode strip = prKitTabStrip(kit, panel, {.reorderable = true}, {});
        for (uint32_t i = 0; i < 5; ++i) {
            const PanelTab& shown = all[tabs.panelOrder[i]];
            PrLayoutNode tab = prKitTab(kit, strip, {
                .key = prUiKeyIndex(kit->ui, strip, tabs.panelOrder[i]),
                .selected = tabs.panel == i,
            });
            prKitTabLabel(kit, tab, {.text = shown.label});
            if (shown.badge != nullptr)
                prKitBadge(kit, tab, {.text = shown.badge}, {.variant = PrKitBadgeVariant_Warning});
            if (shown.closable)
                prKitTabClose(kit, tab);
            if (prKitActivated(kit, tab))
                tabs.panel = i;
        }
        uint32_t from = 0;
        uint32_t to = 0;
        if (prKitMoved(kit, strip, &from, &to))
            moveTab(tabs, from, to);
        PrKitBoxStyle body{.padding = prKitEdgesAll(kPrKitSpace5)};
        const char* const bodies[] = {
            "The scene the viewport renders.", "The game view, at the target resolution.",
            "Three frames over budget.", "The state machine of the selected rig.",
            "The material graph of the selected mesh.",
        };
        uint32_t at = tabs.panel < 5 ? tabs.panel : 0;
        prKitLabel(kit, prKitBox(kit, panel, {}, body), {.text = bodies[tabs.panelOrder[at]]}, kBodySecondary);
    }
    {
        PrLayoutNode panel = demoPanel(kit, section, "SEGMENTED", "VIEW MODE", false);
        PrLayoutNode row = specimenRow(kit, panel, kPrKitSpace3, false);
        const PrKitChoice modes[] = {
            {.label = "Wireframe"},
            {.label = "Solid"},
            {.label = "Textured"},
            {.label = "PBR"},
        };
        segmented(kit, row, modes, 4, &tabs.viewMode, {});
        const PrKitChoice shapes[] = {
            {.icon = PrKitIconKind_Cube},
            {.icon = PrKitIconKind_Sphere},
            {.icon = PrKitIconKind_Light},
        };
        segmented(kit, row, shapes, 3, &tabs.shape, {});
    }
    {
        PrLayoutNode panel = demoPanel(kit, section, "SIZES", "XS . SM . MD", false);
        PrLayoutNode row = specimenRow(kit, panel, kPrKitSpace3, false);
        const PrKitChoice spaces[] = {{.label = "Local"}, {.label = "World"}};
        const PrKitControlSize sizes[] = {PrKitControlSize_Xs, PrKitControlSize_Sm, PrKitControlSize_Md};
        for (PrKitControlSize size : sizes)
            segmented(kit, row, spaces, 2, &tabs.shape, {.size = size});
        prKitLabel(kit, row, {.text = "one binding, three steps"}, kCaption);
    }
}

void sectionMenus(PrKit* kit, PrLayoutNode parent, GalleryState& state) {
    PrLayoutNode section = sectionFrame(kit, parent, "09", "Menus & Popovers", "Application menus, context menus, a palette.",
        "Every floating surface here places itself from the box its trigger last stood in, stays inside the "
            "frame, and takes itself down on Escape or on a press that lands outside it.");
    buildMenuBar(kit, section);
    buildSelect(kit, section, state.menu);
    {
        PrLayoutNode panel = demoPanel(kit, section, "POPOVER", "TRIGGER . FRAME", false);
        PrLayoutNode row = specimenRow(kit, panel, kPrKitSpace3, false);
        PrKitButton layers{.text = "Layers", .icon = PrKitIconKind_ChevronDown};
        PrLayoutNode trigger = prKitButton(kit, row, layers, {});
        if (prKitActivated(kit, trigger))
            prKitToggleOpen(kit, trigger);
        PrLayoutNode popover = prKitPopover(kit, trigger, {}, {});
        if (popover != kPrLayoutNodeNone) {
            PrKitBoxStyle frame{.gap = kPrKitSpace2, .padding = prKitEdgesAll(kPrKitSpace5)};
            frame.place.width = prKitPx(220);
            PrLayoutNode body = prKitBox(kit, popover, {}, frame);
            caption(kit, body, "VISIBLE LAYERS");
            const PrKitCheck checks[] = {
                {.value = &state.toggles.castShadows, .label = "Shadows"},
                {.value = &state.toggles.gizmos, .label = "Gizmos"},
                {.value = &state.toggles.children, .label = "Children"},
            };
            for (const PrKitCheck& check : checks)
                prKitCheckbox(kit, body, check, {});
        }
        prKitLabel(kit, row, {.text = "a popover holds whatever the caller declares in it"}, kBodySecondary);
    }
    buildContextMenu(kit, section, state.menu);
    buildPalette(kit, section, state.palette);
}

} // namespace gallery
