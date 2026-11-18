// The editor's view in elements: its menu bar, toolbar, panels and status bar, declared from its model every round,
// with the widgets of element_widgets.h, as editor_view.cpp declares them with Studio Kit's. Each part is a function
// that walks what it shows of the model, and memoizes what is worth it with a struct of its inputs (pr/ui_element.h,
// Memos).

#include "tools/bench/editor_bench/element_view.h"

#include "pr/count_of.h"
#include "pr/draw_list.h"
#include "pr/layout_frame.h"

#include "tools/bench/editor_bench/element_widgets.h"

#include <stdio.h>

namespace editor {

using namespace widgets;

namespace {

constexpr PrStringView kRenderModes[] = {"PBR", "Unlit", "Wireframe", "Overdraw"};

// --- Menus ---

// What a menu's item does.
enum {
    MenuCmd_None,
    MenuCmd_NewScene,
    MenuCmd_Save,
    MenuCmd_AddEntity,
    MenuCmd_DeleteSelected,
    MenuCmd_ToggleGrid,
    MenuCmd_ToggleSnap,
    MenuCmd_BuildRun,
    MenuCmd_Palette,
};
typedef uint8_t MenuCmd;

struct MenuDef {
    PrStringView text;
    const MenuEntry* entries;
    uint32_t count;
};

constexpr MenuEntry kFileMenu[] = {
    {"New Scene", "Cmd N", MenuCmd_NewScene},
    {"Open...", "Cmd O"},
    {"Save", "Cmd S", MenuCmd_Save},
    {"Save As...", "Shift Cmd S"},
    {.text = "Import...", .separatorBefore = true},
    {"Export..."},
    {.text = "Quit", .shortcut = "Cmd Q", .danger = true, .separatorBefore = true},
};
constexpr MenuEntry kEditMenu[] = {
    {.text = "Undo", .shortcut = "Cmd Z", .disabled = true},
    {.text = "Redo", .shortcut = "Shift Cmd Z", .disabled = true},
    {.text = "Cut", .shortcut = "Cmd X", .separatorBefore = true},
    {"Copy", "Cmd C"},
    {"Paste", "Cmd V"},
    {.text = "Command Palette...", .shortcut = "Cmd K", .command = MenuCmd_Palette, .separatorBefore = true},
};
constexpr MenuEntry kViewMenu[] = {
    {"Grid", "Cmd '", MenuCmd_ToggleGrid},
    {"Snap", "Cmd ;", MenuCmd_ToggleSnap},
    {.text = "Zoom In", .shortcut = "Cmd +", .separatorBefore = true},
    {"Zoom Out", "Cmd -"},
};
constexpr MenuEntry kEntityMenu[] = {
    {"Add Entity", "Shift Cmd N", MenuCmd_AddEntity},
    {
        .text = "Delete Selected",
        .shortcut = "Del",
        .command = MenuCmd_DeleteSelected,
        .danger = true,
        .separatorBefore = true,
    },
};
constexpr MenuEntry kBuildMenu[] = {{"Build and Run", "Cmd B", MenuCmd_BuildRun}, {"Clean Build"}};
constexpr MenuEntry kHelpMenu[] = {{"Documentation"}, {"About Prax"}};

template <uint32_t N>
constexpr MenuDef menuDef(PrStringView text, const MenuEntry (&entries)[N]) {
    return MenuDef{text, entries, N};
}

constexpr MenuDef kMenuDefs[] = {
    menuDef("File", kFileMenu),     menuDef("Edit", kEditMenu),   menuDef("View", kViewMenu),
    menuDef("Entity", kEntityMenu), menuDef("Build", kBuildMenu), menuDef("Help", kHelpMenu),
};

// Returns the bits of a menu's entries that `test` holds for.
template <typename Test>
uint64_t entriesWhere(const MenuDef& def, Test test) {
    uint64_t bits = 0;
    for (uint32_t i = 0; i < def.count; ++i) {
        if (test(MenuCmd(def.entries[i].command)))
            bits |= uint64_t(1) << i;
    }
    return bits;
}

void runMenuCommand(EditorState& state, MenuCmd cmd) {
    switch (cmd) {
        case MenuCmd_NewScene: state.addLog(PrKitLogLevel_Info, "New scene created", "SceneManager"); break;
        case MenuCmd_Save: state.addLog(PrKitLogLevel_Info, "Saved Level_01", "SceneManager"); break;
        case MenuCmd_AddEntity: state.addEntity(); break;
        case MenuCmd_DeleteSelected: state.removeEntity(state.selected); break;
        case MenuCmd_ToggleGrid: state.grid = !state.grid; break;
        case MenuCmd_ToggleSnap: state.snap = !state.snap; break;
        case MenuCmd_BuildRun: state.addLog(PrKitLogLevel_Info, "Build started, 142 targets", "Build"); break;
        case MenuCmd_Palette: state.paletteOpen = true; break;
        default: break;
    }
}

// Declares the menu bar: the menus, the search button and its tooltip, and the project's name. Runs the command that a
// menu ran.
void buildMenuBar(ElementEditor& editor, PrLayoutNode parent) {
    EditorState& state = editor.state;
    struct Memo {
        bool grid;
        bool snap;
        bool canDelete;
        bool operator==(const Memo&) const = default;

        Ran build(UiBuilder& ui, UiElement root) const {
            root.flexRow().itemsCenter().h(kMenuBar).shrink0().px(6).gap(2).bg(kHeader).borderB(1).borderColor(kLine)
                .textColor(kText);
            Ran out;
            for (const MenuDef& def : kMenuDefs) {
                Menu entries{
                    .title = def.text,
                    .entries = def.entries,
                    .count = def.count,
                    .checked = entriesWhere(def, [&](MenuCmd cmd) {
                        return (cmd == MenuCmd_ToggleGrid && grid) || (cmd == MenuCmd_ToggleSnap && snap);
                    }),
                    .disabled = entriesWhere(def, [&](MenuCmd cmd) { return cmd == MenuCmd_DeleteSelected && !canDelete; }),
                };
                UiResult<Ran> ran = menu(ui, root, entries);
                if (ran.ran)
                    out = ran;
            }
            spacer(ui, root);
            UiResult<Clicked> search = button(ui, root, {.text = "Search", .which = PrStudioIconKind_Search});
            if (search.clicked)
                out = Ran{.ran = true, .command = MenuCmd_Palette};
            tooltip(ui, search.root, "Command Palette", "Cmd K");
            UiElement chip = ui.div(root).flexRow().itemsCenter().gap(6).px(8);
            label(ui, chip, "shipwright", kSecondary);
            label(ui, chip, "/", kFaint);
            label(ui, chip, "Level_01", kText);
            return out;
        }
    };
    Memo inputs{.grid = state.grid, .snap = state.snap, .canDelete = state.selected != 0};
    UiResult<Ran> ran = editor.ui->memo(parent, inputs);
    if (ran.ran)
        runMenuCommand(state, MenuCmd(ran.command));
}

// The palette's commands, in the order in which it shows them.
struct CommandDef {
    PrStringView text;
    PrStringView category;
    PrStringView shortcut;
    PrStudioIconKind which;
};

constexpr CommandDef kCommands[] = {
    {"New Scene", "File", "Cmd N", PrStudioIconKind_Folder},
    {"Open Scene...", "File", "Cmd O", PrStudioIconKind_Folder},
    {"Save", "File", "Cmd S", PrStudioIconKind_Folder},
    {"Undo", "Edit", "Cmd Z", PrStudioIconKind_Undo},
    {"Toggle Grid", "View", "Cmd '", PrStudioIconKind_Grid},
    {"Focus Selected", "View", "F", PrStudioIconKind_Target},
    {"Create Empty", "Entity", "Shift Cmd N", PrStudioIconKind_Cube},
    {"Build and Run", "Build", "Cmd B", PrStudioIconKind_Play},
};

// Declares the command palette over the screen while it is open, and logs the command that it ran.
void buildPalette(ElementEditor& editor) {
    EditorState& state = editor.state;
    if (!state.paletteOpen)
        return;
    UiBuilder& ui = *editor.ui;
    UiElement scrim = ui.div(prUiOverlay(ui.ui())).absolute().inset(0).bg(PrColor{0, 0, 0, 0.4f}).flexCol()
        .itemsCenter().pt(120);
    UiElement card = ui.div(scrim).w(520).flexCol().rounded(8).bg(kHeader).border(1).borderColor(kLine).shadowXl()
        .textColor(kText).overflowHidden();
    UiElement prompt = ui.div(card).flexRow().itemsCenter().gap(8).h(40).px(12).borderB(1).borderColor(kLine);
    icon(ui, prompt, PrStudioIconKind_Search, 14, kMuted);
    body(ui.span(prompt)).textColor(kMuted).text("Search commands");
    int32_t ran = -1;
    UiElement list = ui.div(card).flexCol().p(4);
    for (uint32_t i = 0; i < countOf(kCommands); ++i) {
        UiElement row = ui.div(list).flexRow().itemsCenter().gap(10).h(30).px(8).rounded(4).cursorPointer()
            .hoverBg(kHover);
        icon(ui, row, kCommands[i].which, 14, kSecondary);
        body(ui.span(row).flex1()).text(kCommands[i].text);
        caption(ui.span(row)).textColor(kFaint).text(kCommands[i].category);
        caption(ui.span(row)).textColor(kMuted).mono().text(kCommands[i].shortcut);
        if (row.clicked())
            ran = int32_t(i);
    }
    UiElement footer = ui.div(card).flexRow().gap(12).h(28).px(12).itemsCenter().borderT(1).borderColor(kLine);
    label(ui, footer, "Enter to run", kFaint);
    label(ui, footer, "Esc to close", kFaint);
    if (ran >= 0) {
        PrStringView text = kCommands[ran].text;
        state.addLog(PrKitLogLevel_Info, "Ran " + std::string(text.data, text.size), "Palette");
        state.paletteOpen = false;
    }
}

// --- Toolbar ---

// Declares the toolbar, and applies what its controls did to the model.
void buildToolbar(ElementEditor& editor, PrLayoutNode parent) {
    EditorState& state = editor.state;
    // What the toolbar's controls did: each field holds its control's new value, or is unchanged.
    struct Changed {
        int32_t tool = -1;
        int32_t space = -1;
        int32_t renderMode = -1;
        bool snap = false;          // toggled
        bool grid = false;
        bool play = false;
        bool stop = false;
    };
    struct Memo {
        Tool tool;
        uint32_t space;
        bool snap;
        bool grid;
        bool playing;
        uint32_t renderMode;
        bool operator==(const Memo&) const = default;

        Changed build(UiBuilder& ui, UiElement root) const {
            root.flexRow().itemsCenter().h(kToolbar).shrink0().px(8).gap(2).bg(kPanel).borderB(1).borderColor(kLine)
                .textColor(kText);
            Changed out;
            constexpr PrStudioIconKind kTools[] =
                {PrStudioIconKind_Cursor, PrStudioIconKind_Move, PrStudioIconKind_Rotate, PrStudioIconKind_Scale};
            for (uint32_t i = 0; i < countOf(kTools); ++i) {
                if (iconButton(ui, root, {.which = kTools[i], .selected = tool == i}).clicked)
                    out.tool = int32_t(i);
            }
            divider(ui, root);
            static constexpr PrStringView kSpaces[] = {"Local", "World"};
            out.space = segmented(ui, root, {.items = kSpaces, .count = 2, .selected = space}).index;
            divider(ui, root);
            out.snap = iconButton(ui, root, {.which = PrStudioIconKind_Magnet, .selected = snap}).clicked;
            out.grid = iconButton(ui, root, {.which = PrStudioIconKind_Grid, .selected = grid}).clicked;
            label(ui, root, "0.25m", kMuted);
            spacer(ui, root);
            Button play{
                .text = playing ? "Pause" : "Play",
                .which = PrStudioIconKind(playing ? PrStudioIconKind_Pause : PrStudioIconKind_Play),
                .look = ButtonLook_Primary,
                .selected = playing,
            };
            out.play = button(ui, root, play).clicked;
            button(ui, root, {.which = PrStudioIconKind_Step, .look = ButtonLook_Solid});
            out.stop = button(ui, root, {.which = PrStudioIconKind_Stop, .look = ButtonLook_Solid}).clicked;
            divider(ui, root);
            Dropdown mode{
                .items = kRenderModes,
                .count = countOf(kRenderModes),
                .selected = renderMode,
                .which = PrStudioIconKind_Cube,
            };
            out.renderMode = dropdown(ui, root, mode).index;
            return out;
        }
    };
    Memo inputs{
        .tool = state.tool,
        .space = state.space,
        .snap = state.snap,
        .grid = state.grid,
        .playing = state.playing,
        .renderMode = state.renderMode,
    };
    UiResult<Changed> changed = editor.ui->memo(parent, inputs);
    if (changed.tool >= 0)
        state.tool = Tool(changed.tool);
    if (changed.space >= 0)
        state.space = uint32_t(changed.space);
    if (changed.renderMode >= 0)
        state.renderMode = uint32_t(changed.renderMode);
    state.snap = state.snap != changed.snap;
    state.grid = state.grid != changed.grid;
    if (changed.play)
        state.playing = !state.playing;
    if (changed.stop)
        state.playing = false;
}

// --- Scene ---

// Computes the entities that the scene tree shows, each entity whose ancestors are open, as indices of the model's
// entities.
void sceneRows(ElementEditor& editor) {
    editor.rows.clear();
    bool skipping = false;
    uint32_t skipDepth = 0;
    const std::vector<SceneEntity>& nodes = editor.state.nodes;
    for (uint32_t i = 0; i < nodes.size(); ++i) {
        const SceneEntity& node = nodes[i];
        if (skipping && node.depth > skipDepth)
            continue;
        skipping = node.hasChildren && !node.open;
        skipDepth = node.depth;
        editor.rows.push_back(i);
    }
}

// Declares the scene panel: its header, and the rows of the scene tree that its scroll shows (listWindow). Applies the
// round's input to the model once the rows are declared.
void buildScenePanel(ElementEditor& editor, PrLayoutNode parent) {
    UiBuilder& ui = *editor.ui;
    EditorState& state = editor.state;
    UiElement panel = ui.div(parent).flexCol().flex1().minH(0).bg(kPanel);
    struct Header {
        uint32_t count;
        bool operator==(const Header&) const = default;

        Clicked build(UiBuilder& ui, UiElement root) const {
            root.flexCol().shrink0();
            UiElement header = panelHeader(ui, root, "SCENE");
            char text[32];
            badge(ui, header, widgets::count(text, count), kAccent);
            bool add = iconButton(ui, header, {.which = PrStudioIconKind_Plus, .size = 18}).clicked;
            iconButton(ui, header, {.which = PrStudioIconKind_Filter, .size = 18});
            iconButton(ui, header, {.which = PrStudioIconKind_DotsV, .size = 18});
            return Clicked{add};
        }
    };
    bool add = ui.memo(panel, Header{.count = uint32_t(state.nodes.size())}).clicked;

    sceneRows(editor);
    uint32_t count = uint32_t(editor.rows.size());
    UiElement tree = ui.div(panel);
    ListWindow window = listWindow(ui, tree, count, kRow);
    EntityId select = 0;
    EntityId fold = 0;
    EntityId hide = 0;
    for (uint32_t i = window.first; i < window.end; ++i) {
        const SceneEntity& node = state.nodes[editor.rows[i]];
        TreeRow row{
            .name = node.name.c_str(),
            .type = node.typeLabel.c_str(),
            .which = node.icon,
            .depth = node.depth,
            .hasChildren = node.hasChildren,
            .open = node.open,
            .hidden = node.hidden,
            .selected = node.id == state.selected,
        };
        UiResult<RowEvents> e = treeRow(ui, tree, row, ui.key(tree, node.id));
        if (e.folded)
            fold = node.id;
        else if (e.hid)
            hide = node.id;
        else if (e.clicked)
            select = node.id;
    }
    listRest(ui, tree, window, count, kRow);

    if (add)
        state.addEntity();
    if (select != 0)
        state.selected = select;
    if (fold != 0)
        state.toggleOpen(fold);
    if (hide != 0)
        state.toggleHidden(hide);
}

// --- Project ---

struct AssetDef {
    PrStringView name;
    PrStudioIconKind which;
};

constexpr AssetDef kAssets[] = {
    {"player_base", PrStudioIconKind_Cube},  {"player_idle", PrStudioIconKind_Script},
    {"enemy_grunt", PrStudioIconKind_Cube},
    {"env_diffuse", PrStudioIconKind_Image}, {"env_normal", PrStudioIconKind_Image},
    {"water_pbr", PrStudioIconKind_Sphere},
    {"footsteps", PrStudioIconKind_Audio},   {"ambient_wind", PrStudioIconKind_Audio},
    {"controller", PrStudioIconKind_Script},
    {"enemy_ai", PrStudioIconKind_Script},   {"level_01", PrStudioIconKind_Folder},
    {"light_probes", PrStudioIconKind_Light},
};

// Declares a grid of the first `count` assets, and selects the asset that a click chose.
void buildAssetGrid(ElementEditor& editor, PrLayoutNode parent, uint32_t count) {
    UiBuilder& ui = *editor.ui;
    EditorState& state = editor.state;
    UiElement grid = ui.div(parent).flexRow().flexWrap().gap(2).p(8).flex1().minH(0).overflowHidden();
    int32_t chosen = -1;
    for (uint32_t i = 0; i < count && i < countOf(kAssets); ++i) {
        Tile asset{.which = kAssets[i].which, .name = kAssets[i].name, .selected = state.selectedAsset == i};
        if (tile(ui, grid, asset).clicked)
            chosen = int32_t(i);
    }
    if (chosen >= 0)
        state.selectedAsset = uint32_t(chosen);
}

// Declares the project panel: its header, the path of the folder that it shows, and the folder's assets.
void buildProjectPanel(ElementEditor& editor, PrLayoutNode parent) {
    UiElement panel = editor.ui->div(parent).flexCol().flex1().minH(0).bg(kPanel);
    struct Header {
        bool operator==(const Header&) const = default;

        void build(UiBuilder& ui, UiElement root) const {
            root.flexCol().shrink0();
            UiElement header = panelHeader(ui, root, "PROJECT");
            iconButton(ui, header, {.which = PrStudioIconKind_Plus, .size = 18});
            iconButton(ui, header, {.which = PrStudioIconKind_Filter, .size = 18});
            UiElement crumbs = bar(ui, root, kPanel, kRow, 4).borderB(1).borderColor(kLine).mono();
            constexpr PrStringView kSegments[] = {"Assets", "Models", "Textures"};
            for (uint32_t i = 0; i < 3; ++i) {
                label(ui, crumbs, kSegments[i], i == 2 ? kText : kMuted);
                if (i != 2)
                    label(ui, crumbs, "/", kFaint);
            }
        }
    };
    editor.ui->memo(panel, Header{});
    buildAssetGrid(editor, panel, 8);
}

// --- Viewport ---

// Draws the viewport's grid over its content box. Its context is the grid's state, so that a change of it changes the
// content.
void paintGrid(void* context, PrVec2 size, PrDrawList* list) {
    bool grid = *static_cast<const bool*>(context);
    prDrawListRect(list, PrRect{0, 0, size.x, size.y}, {.color = PrColor{5 / 255.0f, 6 / 255.0f, 7 / 255.0f, 1}});
    if (!grid)
        return;
    PrColor line{0.16f, 0.18f, 0.22f, 1};
    for (float x = 24; x < size.x; x += 24)
        prDrawListRect(list, PrRect{x, 0, x + 1, size.y}, {.color = line});
    for (float y = 24; y < size.y; y += 24)
        prDrawListRect(list, PrRect{0, y, size.x, y + 1}, {.color = line});
}

constexpr bool kGridOn = true;
constexpr bool kGridOff = false;

// Declares the viewport: its header, the grid, and the HUD's panels over it in its corners. Applies what its controls
// did to the model.
void buildViewport(ElementEditor& editor, PrLayoutNode parent) {
    EditorState& state = editor.state;
    struct Changed {
        int32_t tab = -1;
        int32_t renderMode = -1;
        bool grid = false;
        bool snap = false;
    };
    struct Memo {
        bool grid;
        bool snap;
        uint32_t tab;
        uint32_t renderMode;
        bool operator==(const Memo&) const = default;

        Changed build(UiBuilder& ui, UiElement root) const {
            root.flexCol().flex1().minH(0).bg(kPanel);
            UiElement header = panelHeader(ui, root, "VIEWPORT");
            Changed out;
            static constexpr PrStringView kTabs[] = {"Scene", "Game", "Animator"};
            out.tab = segmented(ui, header, {.items = kTabs, .count = 3, .selected = tab}).index;
            out.grid = iconButton(ui, header, {.which = PrStudioIconKind_Grid, .selected = grid, .size = 18}).clicked;

            UiElement view = ui.div(root).flex1().minH(0).relative().overflowHidden().flexCol();
            UiElement canvas = ui.replaced(view).flex1().minH(0);
            PrLayoutContent content{};
            content.kind = PrLayoutContentKind_Painter;
            content.painter.paint = paintGrid;
            content.painter.context = const_cast<bool*>(grid ? &kGridOn : &kGridOff);
            prLayoutTreeFrameSetContent(ui.tree(), canvas, &content);

            UiElement camera = hudPanel(ui, view, false, false, false);
            readout(ui, camera, "CAM", "persp . 50deg");
            readout(ui, camera, "POS", "12.400  3.200  -8.750");
            UiElement modes = hudPanel(ui, view, true, false, true);
            readout(ui, modes, "VIEW", {});
            Dropdown mode{
                .items = kRenderModes,
                .count = countOf(kRenderModes),
                .selected = renderMode,
                .which = PrStudioIconKind_Cube,
            };
            out.renderMode = dropdown(ui, modes, mode).index;
            UiElement stats = hudPanel(ui, view, false, true, true);
            readout(ui, stats, "FPS", "60.00");
            readout(ui, stats, "DRAW", "1,412");
            readout(ui, stats, "TRIS", "2.4M");
            readout(ui, stats, "GRID", {});
            out.grid = toggle(ui, stats, {.on = grid}).clicked || out.grid;
            readout(ui, stats, "SNAP", {});
            out.snap = toggle(ui, stats, {.on = snap}).clicked;
            return out;
        }
    };
    Memo inputs{.grid = state.grid, .snap = state.snap, .tab = state.viewportTab, .renderMode = state.renderMode};
    UiResult<Changed> changed = editor.ui->memo(parent, inputs);
    if (changed.tab >= 0)
        state.viewportTab = uint32_t(changed.tab);
    if (changed.renderMode >= 0)
        state.renderMode = uint32_t(changed.renderMode);
    state.grid = state.grid != changed.grid;
    state.snap = state.snap != changed.snap;
}

// --- Console ---

// Declares the console: its tabs, and the visible log's lines or the asset browser.
void buildConsole(ElementEditor& editor, PrLayoutNode parent) {
    UiBuilder& ui = *editor.ui;
    EditorState& state = editor.state;
    UiElement panel = ui.div(parent).flexCol().flex1().minH(0).bg(kPanel);
    UiElement strip = bar(ui, panel, kHeader, kToolbar, 2).borderB(1).borderColor(kLine);
    static constexpr PrStringView kTabs[] = {"Console", "Asset Browser", "Timeline", "Profiler"};
    UiResult<Chosen> tab = tabs(ui, strip, {.items = kTabs, .count = countOf(kTabs), .selected = editor.consoleTab});
    UiElement list = ui.div(panel).flexCol().flex1().minH(0).overflowYScroll().px(4);
    if (editor.consoleTab == 1) {
        buildAssetGrid(editor, list, 12);
    } else {
        for (const LogEntry* entry : state.visibleLogs) {
            LogLine line{
                .time = entry->time.c_str(),
                .message = entry->message.c_str(),
                .source = entry->source.c_str(),
                .level = entry->level,
            };
            logLine(ui, list, line);
        }
    }
    if (tab.index >= 0)
        editor.consoleTab = uint32_t(tab.index);
}

// --- Inspector ---

// Returns the row that shows a property of `kind` with `hint` (PropertyInfo).
FieldKind fieldKind(PropertyKind kind, PropertyHint hint) {
    switch (kind) {
        case PropertyKind_Float: return hint == PropertyHint_Slider ? FieldKind_Slider : FieldKind_Number;
        case PropertyKind_Vector3: return FieldKind_Vector;
        case PropertyKind_Bool: return hint == PropertyHint_Check ? FieldKind_Check : FieldKind_Switch;
        case PropertyKind_Enum: return FieldKind_Choice;
        case PropertyKind_Color: return FieldKind_Color;
        default: return FieldKind_Asset;
    }
}

// Reads a property's value from where it is stored, as its kind says, into a row's value.
FieldValue readProperty(const PropertyInfo& property, void* at) {
    FieldValue value{};
    switch (property.kind) {
        case PropertyKind_Float: value.numbers[0] = *static_cast<float*>(at); break;
        case PropertyKind_Vector3: {
            const Vector3& v = *static_cast<Vector3*>(at);
            value.numbers[0] = v.x;
            value.numbers[1] = v.y;
            value.numbers[2] = v.z;
            break;
        }
        case PropertyKind_Bool: value.flag = *static_cast<bool*>(at); break;
        case PropertyKind_Enum: value.choice = *static_cast<uint32_t*>(at); break;
        case PropertyKind_Color: value.color = *static_cast<PrColor*>(at); break;
        case PropertyKind_Asset: value.text = static_cast<std::string*>(at)->c_str(); break;
        default: break;
    }
    return value;
}

// Writes a row's value to where a property is stored.
void writeProperty(const PropertyInfo& property, void* at, const FieldValue& value) {
    switch (property.kind) {
        case PropertyKind_Float: *static_cast<float*>(at) = value.numbers[0]; break;
        case PropertyKind_Vector3: *static_cast<Vector3*>(at) = {value.numbers[0], value.numbers[1], value.numbers[2]};
            break;
        case PropertyKind_Bool: *static_cast<bool*>(at) = value.flag; break;
        case PropertyKind_Enum: *static_cast<uint32_t*>(at) = value.choice; break;
        case PropertyKind_Color: *static_cast<PrColor*>(at) = value.color; break;
        case PropertyKind_Asset: *static_cast<std::string*>(at) = std::string(value.text.view().data,
            value.text.view().size); break;
        default: break;
    }
}

// The color of a component's badge.
PrColor badgeColor(PrKitBadgeVariant variant) {
    switch (variant) {
        case PrKitBadgeVariant_Accent: return kAccent;
        case PrKitBadgeVariant_Success: return kSuccess;
        default: return kMuted;
    }
}

// Declares the inspector under `parent`: its header, and a group for each of the entity's components with a row for
// each of its properties, from the model's descriptions of them (entityComponents), and applies the round's edits to
// the model. Each row memoizes one property's value, so a round builds only the rows whose values changed, however many
// properties the entity has.
void buildInspector(ElementEditor& editor, PrLayoutNode parent) {
    UiBuilder& ui = *editor.ui;
    EditorState& state = editor.state;
    SceneEntity* entity = state.inspectorLocked ? state.find(state.lockedEntity) : selectedEntity(state);

    UiElement panel = ui.div(parent).flexCol().flex1().minH(0).bg(kPanel).textColor(kText);
    // The header and the identity of the entity shown.
    struct Events {
        bool lock = false;
        bool enable = false;
    };
    struct Header {
        bool any;                   // whether an entity is shown
        String name;
        bool enabled;
        bool locked;
        bool operator==(const Header&) const = default;

        Events build(UiBuilder& ui, UiElement root) const {
            root.flexCol().shrink0();
            Events out;
            UiElement header = panelHeader(ui, root, "INSPECTOR");
            IconButton lock{
                .which = PrStudioIconKind(locked ? PrStudioIconKind_Lock : PrStudioIconKind_Unlock),
                .selected = locked,
                .size = 18,
            };
            out.lock = iconButton(ui, header, lock).clicked;
            iconButton(ui, header, {.which = PrStudioIconKind_DotsV, .size = 18});
            UiElement identity = bar(ui, root, kPanel, kToolbar, 6).borderB(1).borderColor(kLine);
            if (any)
                out.enable = checkbox(ui, identity, {.checked = enabled}).clicked;
            icon(ui, identity, PrStudioIconKind_Cube, 14, any ? kSecondary : kFaint);
            body(ui.span(identity).flex1().minW(0).whitespaceNowrap()).text(any ? name.view() : "No selection");
            badge(ui, identity, "ENTITY", kAccent);
            return out;
        }
    };
    Header header{
        .any = entity != nullptr,
        .name = entity != nullptr ? entity->name.c_str() : "",
        .enabled = entity != nullptr && entity->enabled,
        .locked = state.inspectorLocked,
    };
    UiResult<Events> events = ui.memo(panel, header);
    if (events.lock) {
        state.inspectorLocked = !state.inspectorLocked;
        state.lockedEntity = state.inspectorLocked && entity != nullptr ? entity->id : 0;
        ++state.edits;
    }
    if (events.enable && entity != nullptr) {
        entity->enabled = !entity->enabled;
        ++state.edits;
    }

    UiElement list = ui.div(panel).flexCol().flex1().minH(0).overflowYScroll();
    if (entity == nullptr)
        return;
    uint32_t count = 0;
    const ComponentInfo* components = entityComponents(&count);
    for (uint32_t c = 0; c < count; ++c) {
        const ComponentInfo& component = components[c];
        bool& open = state.*component.open;
        GroupHeader group{
            .name = component.name,
            .badge = component.badge,
            .badgeColor = badgeColor(component.badgeVariant),
            .open = open,
        };
        // A group's header and rows are keyed by the component, so that a group that closes leaves the others as they
        // were.
        if (groupHeader(ui, list, group, ui.key(list, uint64_t(c) * 2)).clicked) {
            open = !open;
            ++state.edits;
        }
        if (!open)
            continue;
        UiElement rows = ui.div(list, ui.key(list, uint64_t(c) * 2 + 1)).flexCol().py(4).borderB(1).borderColor(kLine)
            .shrink0();
        for (uint32_t i = 0; i < component.count; ++i) {
            const PropertyInfo& property = component.properties[i];
            void* at = property.at(state, *entity);
            PropertyRow row{
                .name = property.name,
                .kind = fieldKind(property.kind, property.hint),
                .value = readProperty(property, at),
                .step = property.step,
                .min = property.min,
                .max = property.max,
                .unit = property.unit,
                .choices = property.choices,
                .choiceCount = property.choiceCount,
            };
            UiResult<FieldEdited> edited = propertyRow(ui, rows, row);
            if (!edited.changed)
                continue;
            writeProperty(property, at, edited.value);
            if (edited.committed)
                ++state.edits;
        }
    }
}

// --- Status bar ---

// Declares the status bar.
void buildStatusBar(ElementEditor& editor, PrLayoutNode parent) {
    EditorState& state = editor.state;
    struct Memo {
        uint32_t entities;
        uint32_t edits;
        bool playing;
        bool operator==(const Memo&) const = default;

        void build(UiBuilder& ui, UiElement root) const {
            root.flexRow().itemsCenter().h(kRow).shrink0().px(8).gap(8).bg(kHeader).borderT(1).borderColor(kLine);
            icon(ui, root, PrStudioIconKind_Check, 10, kSuccess);
            label(ui, root, "BUILD OK", kSuccess);
            char entityText[24];
            int entitySize = snprintf(entityText, sizeof entityText, "ENT %u", entities);
            char editText[24];
            int editSize = snprintf(editText, sizeof editText, "EDITS %u", edits);
            PrStringView items[] = {
                "shipwright/Level_01",
                PrStringView{entityText, size_t(entitySize)},
                PrStringView{editText, size_t(editSize)},
                "DRAW 1,412",
                "TRIS 2.4M",
                playing ? "PLAY . 60.00fps" : "EDIT . 60.00fps",
            };
            for (PrStringView item : items) {
                divider(ui, root);
                label(ui, root, item, kMuted);
            }
            spacer(ui, root);
            label(ui, root, "LINE 42 . COL 18", kMuted);
            divider(ui, root);
            label(ui, root, "UTF-8", kMuted);
        }
    };
    Memo inputs{.entities = uint32_t(state.nodes.size()), .edits = state.edits, .playing = state.playing};
    editor.ui->memo(parent, inputs);
}

} // namespace

void buildElementEditor(ElementEditor& editor, PrLayoutNode root) {
    UiBuilder& ui = *editor.ui;
    EditorState& state = editor.state;

    UiElement app = ui.div(root).flexCol().wFull().hFull().bg(kApp).textColor(kText).overflowHidden();
    if (prUiKeyPressed(ui.ui(), app, 'K', PrAppModifierFlag_Super))
        state.paletteOpen = !state.paletteOpen;
    buildMenuBar(editor, app);
    buildToolbar(editor, app);

    // The dock: the scene over the project, the viewport over the console, and the inspector.
    UiElement dock = ui.div(app).flexRow().flex1().minH(0);
    UiElement left = ui.div(dock).flexCol().w(uiPct(editor.left * 100)).shrink0().minH(0);
    buildScenePanel(editor, pane(ui, left, "Scene", editor.scene));
    splitter(ui, left, left, false, &editor.scene);
    buildProjectPanel(editor, pane(ui, left, "Project"));
    splitter(ui, dock, dock, true, &editor.left);

    UiElement center = ui.div(dock).flexCol().flex1().minW(0).minH(0);
    buildViewport(editor, pane(ui, center, "Viewport", editor.viewport));
    splitter(ui, center, center, false, &editor.viewport);
    buildConsole(editor, pane(ui, center, "Console"));
    splitter(ui, dock, dock, true, &editor.right, true);
    UiElement right = ui.div(dock).flexCol().w(uiPct(editor.right * 100)).shrink0().minH(0);
    buildInspector(editor, pane(ui, right, "Inspector"));

    buildStatusBar(editor, app);
    buildPalette(editor);
}

ElementEditor::~ElementEditor() {
    destroy(resources);
}

void elementAttach(void* context, editor_bench::Host& host) {
    ElementEditor& editor = *static_cast<ElementEditor*>(context);
    // The host keeps its font set across its contexts, and the editor its resources.
    if (editor.resources == nullptr)
        editor.resources = create<UiResources>(host.fonts);
    UiBuilderDesc desc{.ui = host.ui, .resources = editor.resources, .textSize = 12, .textColor = kText};
    editor.ui = create<UiBuilder>(desc);
}

void elementDetach(void* context, editor_bench::Host&) {
    ElementEditor& editor = *static_cast<ElementEditor*>(context);
    destroy(editor.ui);
    editor.ui = nullptr;
}

} // namespace editor
