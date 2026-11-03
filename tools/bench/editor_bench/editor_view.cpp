// The editor's view: its menu bar, toolbar, panels and status bar, declared from its model every round.

#include "tools/bench/editor_bench/editor_view.h"

#include "pr/count_of.h"
#include "pr/draw_list.h"
#include "pr/kit_box.h"
#include "pr/kit_button.h"
#include "pr/kit_choice.h"
#include "pr/kit_collection.h"
#include "pr/kit_color.h"
#include "pr/kit_data.h"
#include "pr/kit_dock.h"
#include "pr/kit_icon.h"
#include "pr/kit_input.h"
#include "pr/kit_menu.h"
#include "pr/kit_text.h"
#include "pr/layout_frame.h"
#include "pr/layout_unit.h"
#include "pr/ui.h"

#include "tools/bench/editor_bench/hud.h"

#include <stdio.h>
#include <string>

namespace editor {

using namespace pr;

namespace {

// The text styles the panels share.
constexpr PrKitTextStyle kBody = {.wrap = PrKitWrap_Clip};
constexpr PrKitTextStyle kBodySmall = {.role = PrKitTextRole_BodySmall, .wrap = PrKitWrap_Clip};
constexpr PrKitTextStyle kCaption = {.role = PrKitTextRole_Caption};
constexpr PrKitTextStyle kMeta = {.role = PrKitTextRole_Meta};

constexpr const char* kRenderModes[] = {"PBR", "Unlit", "Wireframe", "Overdraw"};

// Creates a row of fixed height on a ground, with a rule on each side that `rules` names.
PrLayoutNode bar(PrKit* kit, PrLayoutNode parent, PrKitGround ground, PrKitRuleFlags rules, float height, float gap) {
    PrKitSurfaceStyle surface{.ground = ground, .rules = rules};
    surface.box.direction = PrKitDirection_Row;
    surface.box.align = PrKitAlign_Center;
    surface.box.gap = gap;
    surface.box.padding = prKitEdgesXY(0, kPrKitSpace4);
    surface.box.overflow = PrKitOverflow_Clip;
    surface.box.place.height = prKitPx(height);
    surface.box.place.flex = PrKitFlex_Fixed;
    return prKitSurface(kit, parent, {}, surface);
}

// Creates a small square button in a panel's header, and returns true if it was activated.
bool headerButton(PrKit* kit, PrLayoutNode header, PrKitIconKind icon, bool selected) {
    return prKitActivated(kit, prKitButton(kit, header, {.icon = icon, .selected = selected},
        {.variant = PrKitButtonVariant_Ghost, .size = PrKitControlSize_Xs}));
}

// Creates a segmented control of `count` items. The item at index `*selected` is selected, and activating an item
// writes its index to `selected`.
void segmented(PrKit* kit, PrLayoutNode parent, const char* const* items, uint32_t count, uint32_t* selected,
        PrKitControlSize size) {
    PrLayoutNode control = prKitSegmented(kit, parent, {}, {.size = size});
    for (uint32_t i = 0; i < count; ++i) {
        if (prKitActivated(kit, prKitSegment(kit, control, {.label = items[i], .selected = *selected == i})))
            *selected = i;
    }
}

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

struct MenuItemDef {
    const char* text;
    const char* shortcut = nullptr;
    MenuCmd cmd = MenuCmd_None;
    bool danger = false;
    bool disabled = false;
    bool separatorBefore = false;
};

struct MenuDef {
    const char* text;
    const MenuItemDef* items;
    uint32_t count;
};

constexpr MenuItemDef kFileMenu[] = {
    {"New Scene", "Cmd N", MenuCmd_NewScene},
    {"Open...", "Cmd O"},
    {"Save", "Cmd S", MenuCmd_Save},
    {"Save As...", "Shift Cmd S"},
    {"Import...", nullptr, MenuCmd_None, false, false, true},
    {"Export..."},
    {"Quit", "Cmd Q", MenuCmd_None, true, false, true},
};
constexpr MenuItemDef kEditMenu[] = {
    {"Undo", "Cmd Z", MenuCmd_None, false, true},
    {"Redo", "Shift Cmd Z", MenuCmd_None, false, true},
    {"Cut", "Cmd X", MenuCmd_None, false, false, true},
    {"Copy", "Cmd C"},
    {"Paste", "Cmd V"},
    {"Command Palette...", "Cmd K", MenuCmd_Palette, false, false, true},
};
constexpr MenuItemDef kViewMenu[] = {
    {"Grid", "Cmd '", MenuCmd_ToggleGrid},
    {"Snap", "Cmd ;", MenuCmd_ToggleSnap},
    {"Zoom In", "Cmd +", MenuCmd_None, false, false, true},
    {"Zoom Out", "Cmd -"},
};
constexpr MenuItemDef kEntityMenu[] = {
    {"Add Entity", "Shift Cmd N", MenuCmd_AddEntity},
    {"Delete Selected", "Del", MenuCmd_DeleteSelected, true, false, true},
};
constexpr MenuItemDef kBuildMenu[] = {{"Build and Run", "Cmd B", MenuCmd_BuildRun}, {"Clean Build"}};
constexpr MenuItemDef kHelpMenu[] = {{"Documentation"}, {"About Prax"}};

template <uint32_t N>
constexpr MenuDef menuDef(const char* text, const MenuItemDef (&items)[N]) {
    return MenuDef{text, items, N};
}

constexpr MenuDef kMenuDefs[] = {
    menuDef("File", kFileMenu),   menuDef("Edit", kEditMenu),   menuDef("View", kViewMenu),
    menuDef("Entity", kEntityMenu), menuDef("Build", kBuildMenu), menuDef("Help", kHelpMenu),
};

bool menuChecked(const EditorState& editor, MenuCmd cmd) {
    return (cmd == MenuCmd_ToggleGrid && editor.grid) || (cmd == MenuCmd_ToggleSnap && editor.snap);
}

void runMenuCommand(EditorState& editor, MenuCmd cmd) {
    switch (cmd) {
        case MenuCmd_NewScene:
            editor.addLog(PrKitLogLevel_Info, "New scene created", "SceneManager");
            break;
        case MenuCmd_Save:
            editor.addLog(PrKitLogLevel_Info, "Saved Level_01", "SceneManager");
            break;
        case MenuCmd_AddEntity:
            editor.addEntity();
            break;
        case MenuCmd_DeleteSelected:
            editor.removeEntity(editor.selected);
            break;
        case MenuCmd_ToggleGrid:
            editor.grid = !editor.grid;
            break;
        case MenuCmd_ToggleSnap:
            editor.snap = !editor.snap;
            break;
        case MenuCmd_BuildRun:
            editor.addLog(PrKitLogLevel_Info, "Build started, 142 targets", "Build");
            break;
        case MenuCmd_Palette:
            editor.paletteOpen = true;
            break;
        default:
            break;
    }
}

// Declares a menu's items under `menu`, and runs the item that was activated.
void menuItems(PrKit* kit, PrLayoutNode menu, const MenuDef& def, EditorState& editor) {
    for (uint32_t i = 0; i < def.count; ++i) {
        const MenuItemDef& row = def.items[i];
        if (row.separatorBefore)
            prKitMenuSeparator(kit, menu, {});
        PrKitMenuItem item{
            .text = row.text,
            .shortcut = row.shortcut != nullptr ? row.shortcut : PrStringView{},
            .checked = menuChecked(editor, row.cmd),
            .disabled = row.disabled || (row.cmd == MenuCmd_DeleteSelected && editor.selected == 0),
        };
        PrKitMenuItemStyle style{
            .variant = PrKitMenuItemVariant(row.danger ? PrKitMenuItemVariant_Danger : PrKitMenuItemVariant_Default),
        };
        PrLayoutNode node = prKitMenuItem(kit, menu, item, style);
        if (prKitActivated(kit, node))
            runMenuCommand(editor, row.cmd);
    }
}

void buildMenubar(PrKit* kit, PrLayoutNode parent, EditorState& editor) {
    PrLayoutNode menubar = prKitMenuBar(kit, parent, {});
    for (const MenuDef& def : kMenuDefs) {
        PrLayoutNode item = prKitMenuBarItem(kit, menubar, {.text = def.text});
        PrLayoutNode popover = prKitPopover(kit, item, {}, {});
        if (popover == kPrLayoutNodeNone)
            continue;
        menuItems(kit, prKitMenu(kit, popover, {}, {.minWidth = 220}), def, editor);
    }
    prKitSpacer(kit, menubar, {});
    if (editor.timing.frames != 0) {
        char label[64];
        snprintf(label, sizeof label, "%.3f ms cpu (%.3f)", double(editor.timing.cpuMs),
            double(editor.timing.smoothCpuMs));
        prKitLabel(kit, menubar, {.text = label}, kCaption);
        prKitDivider(kit, menubar, {});
    }
    PrKitButtonStyle ghost{.variant = PrKitButtonVariant_Ghost, .size = PrKitControlSize_Sm};
    PrKitButton search{.text = "Search", .icon = PrKitIconKind_Search};
    PrLayoutNode searchButton = prKitButton(kit, menubar, search, ghost);
    if (prKitActivated(kit, searchButton))
        editor.paletteOpen = true;
    prKitTooltip(kit, searchButton, {.text = "Command Palette", .shortcut = "Cmd K"});
    PrLayoutNode chip = prKitBox(kit, menubar, {}, {.direction = PrKitDirection_Row, .gap = kPrKitSpace3});
    prKitLabel(kit, chip, {.text = "shipwright"}, kBodySmall);
    prKitLabel(kit, chip, {.text = "/"}, kMeta);
    prKitLabel(kit, chip, {.text = "Level_01"}, kCaption);
}

// The palette's commands, in the order in which it shows them.
struct CommandDef {
    const char* text;
    const char* category;
    const char* shortcut;
    PrKitIconKind icon;
};

constexpr CommandDef kCommands[] = {
    {"New Scene", "File", "Cmd N", PrKitIconKind_Folder},
    {"Open Scene...", "File", "Cmd O", PrKitIconKind_Folder},
    {"Save", "File", "Cmd S", PrKitIconKind_Folder},
    {"Undo", "Edit", "Cmd Z", PrKitIconKind_Undo},
    {"Toggle Grid", "View", "Cmd '", PrKitIconKind_Grid},
    {"Focus Selected", "View", "F", PrKitIconKind_Target},
    {"Create Empty", "Entity", "Shift Cmd N", PrKitIconKind_Cube},
    {"Build and Run", "Build", "Cmd B", PrKitIconKind_Play},
};

// Declares the command palette over the screen while it is open. The palette's trigger is the root, which claims Cmd K
// to open it.
void buildPalette(PrKit* kit, PrLayoutNode root, EditorState& editor) {
    if (prUiKeyPressed(kit->ui, root, 'K', PrAppModifierFlag_Super))
        editor.paletteOpen = !editor.paletteOpen;
    prKitSetOpen(kit, root, editor.paletteOpen);
    constexpr uint32_t count = countOf(kCommands);
    PrLayoutNode list = prKitCommandPalette(kit, root, {.prompt = "Search commands", .count = count});
    if (list == kPrLayoutNodeNone) {
        editor.paletteOpen = false;
        return;
    }
    for (uint32_t i = 0; i < count; ++i) {
        PrKitCommandItem item{
            .text = kCommands[i].text,
            .category = kCommands[i].category,
            .shortcut = kCommands[i].shortcut,
            .icon = kCommands[i].icon,
        };
        if (prKitActivated(kit, prKitCommandItem(kit, list, item))) {
            editor.addLog(PrKitLogLevel_Info, std::string("Ran ") + kCommands[i].text, "Palette");
            editor.paletteOpen = false;
            prKitSetOpen(kit, root, false);
        }
    }
    prKitCommandPaletteFooter(kit, list);
}

// Creates a button that shows the render mode, and the menu of modes that it opens.
PrLayoutNode renderModeButton(PrKit* kit, PrLayoutNode parent, EditorState& editor, PrKitControlSize size) {
    PrKitButton mode{.text = kRenderModes[editor.renderMode], .icon = PrKitIconKind_Cube};
    PrLayoutNode button = prKitButton(kit, parent, mode, {.variant = PrKitButtonVariant_Ghost, .size = size});
    if (prKitActivated(kit, button))
        prKitToggleOpen(kit, button);
    PrLayoutNode popover = prKitPopover(kit, button, {}, {});
    if (popover != kPrLayoutNodeNone) {
        PrLayoutNode menu = prKitMenu(kit, popover, {}, {.minWidth = 160});
        for (uint32_t i = 0; i < countOf(kRenderModes); ++i) {
            PrKitMenuItem item{.text = kRenderModes[i], .checked = editor.renderMode == i};
            if (prKitActivated(kit, prKitMenuItem(kit, menu, item, {})))
                editor.renderMode = i;
        }
    }
    return button;
}

void buildToolbar(PrKit* kit, PrLayoutNode parent, EditorState& editor) {
    PrLayoutNode toolbar = bar(kit, parent, PrKitGround_Panel, PrKitRuleFlag_Bottom, kPrKitHeightLg, kPrKitSpace1);
    struct ToolDef {
        Tool tool;
        PrKitIconKind icon;
    };
    const ToolDef tools[] = {
        {Tool_Select, PrKitIconKind_Cursor},
        {Tool_Move, PrKitIconKind_Move},
        {Tool_Rotate, PrKitIconKind_Rotate},
        {Tool_Scale, PrKitIconKind_Scale},
    };
    for (const ToolDef& tool : tools) {
        if (prKitActivated(kit, prKitButton(kit, toolbar, {.icon = tool.icon, .selected = editor.tool == tool.tool},
                {.variant = PrKitButtonVariant_Ghost, .size = PrKitControlSize_Md})))
            editor.tool = tool.tool;
    }
    prKitDivider(kit, toolbar, {});
    const char* spaces[] = {"Local", "World"};
    segmented(kit, toolbar, spaces, 2, &editor.space, PrKitControlSize_Sm);
    prKitDivider(kit, toolbar, {});
    if (prKitActivated(kit, prKitButton(kit, toolbar, {.icon = PrKitIconKind_Magnet, .selected = editor.snap},
            {.variant = PrKitButtonVariant_Ghost, .size = PrKitControlSize_Md})))
        editor.snap = !editor.snap;
    if (prKitActivated(kit, prKitButton(kit, toolbar, {.icon = PrKitIconKind_Grid, .selected = editor.grid},
            {.variant = PrKitButtonVariant_Ghost, .size = PrKitControlSize_Md})))
        editor.grid = !editor.grid;
    prKitLabel(kit, toolbar, {.text = "0.25m"}, kCaption);
    prKitSpacer(kit, toolbar, {});
    PrKitButton play{
        .text = (editor.playing ? "Pause" : "Play"),
        .icon = PrKitIconKind(editor.playing ? PrKitIconKind_Pause : PrKitIconKind_Play),
        .selected = editor.playing,
    };
    PrKitButtonStyle primary{.variant = PrKitButtonVariant_Primary, .size = PrKitControlSize_Sm};
    if (prKitActivated(kit, prKitButton(kit, toolbar, play, primary)))
        editor.playing = !editor.playing;
    PrKitButtonStyle small{.size = PrKitControlSize_Sm};
    PrKitButton step{.icon = PrKitIconKind_Step};
    prKitButton(kit, toolbar, step, small);
    PrKitButton stop{.icon = PrKitIconKind_Stop};
    if (prKitActivated(kit, prKitButton(kit, toolbar, stop, small)))
        editor.playing = false;
    prKitDivider(kit, toolbar, {});
    renderModeButton(kit, toolbar, editor, PrKitControlSize_Sm);
}

// --- Scene ---

// Computes the entities that the scene tree shows (each entity whose ancestors are open), and how the tree shows each.
void sceneOrder(EditorState& editor) {
    editor.sceneRows.clear();
    editor.sceneItems.clear();
    bool skipping = false;
    uint32_t skipDepth = 0;
    for (const SceneEntity& node : editor.nodes) {
        if (skipping && node.depth > skipDepth)
            continue;
        skipping = node.hasChildren && !node.open;
        skipDepth = node.depth;
        editor.sceneRows.push_back(node.id);
        editor.sceneItems.push_back(PrKitTreeItem{
            .depth = node.depth,
            .hasChildren = node.hasChildren,
            .open = node.open,
        });
    }
}

// The payload kind of a scene row's drag.
constexpr uint64_t kDragSceneRow = 1;

// The payload kind of an asset's drag.
constexpr uint64_t kDragAsset = 2;

void buildScenePanel(PrKit* kit, PrLayoutNode parent, EditorState& editor) {
    char count[16];
    snprintf(count, sizeof count, "%zu", editor.nodes.size());
    PrLayoutNode header = prKitPanelHeader(kit, parent, {.title = "SCENE"});
    prKitBadge(kit, header, {.text = count}, {.variant = PrKitBadgeVariant_Accent});
    if (headerButton(kit, header, PrKitIconKind_Plus, false))
        editor.addEntity();
    headerButton(kit, header, PrKitIconKind_Filter, false);
    headerButton(kit, header, PrKitIconKind_DotsV, false);

    sceneOrder(editor);
    PrKitTreeView content{
        .count = uint32_t(editor.sceneRows.size()),
        .selection = &editor.sceneCursor,
        .items = editor.sceneItems.data(),
        .dragKind = kDragSceneRow,
    };
    PrLayoutNode view = prKitTreeView(kit, parent, content, {.place = {.flex = PrKitFlex_Fill}});
    PrUi* ui = kit->ui;
    PrKitWindow window = prKitListWindow(kit, view);
    for (uint32_t i = window.first; i < window.first + window.count; ++i) {
        EntityId id = editor.sceneRows[i];
        SceneEntity* node = editor.find(id);
        PrLayoutNode rowNode = prKitTreeRow(kit, view, {.index = i, .item = id, .selected = editor.selected == id});
        prKitRowIcon(kit, rowNode, {.icon = node->icon});
        prKitRowLabel(kit, rowNode, {.text = node->name},
            PrKitTone(node->hidden ? PrKitTone_Faint : PrKitTone_Primary));
        if (!node->typeLabel.empty())
            prKitRowMeta(kit, rowNode, {.text = node->typeLabel});
        PrKitVisibilityToggle toggle{.hidden = node->hidden, .revealed = prUiIsHovered(ui, rowNode)};
        if (prKitActivated(kit, prKitVisibilityToggle(kit, rowNode, toggle)))
            editor.toggleHidden(id);
    }
    PrKitTreeFold fold{};
    if (prKitFolded(kit, view, &fold))
        editor.toggleOpen(editor.sceneRows[fold.row]);
    PrKitSelectionEdit edit{};
    if (prKitSelectionEdited(kit, view, &edit) && editor.sceneCursor.lead < editor.sceneRows.size())
        editor.selected = editor.sceneRows[editor.sceneCursor.lead];
    PrKitTreeDrop drop{};
    if (prKitRowDropped(kit, view, &drop) && drop.row < editor.sceneRows.size()) {
        TreePlacement placement = TreePlacement_Into;
        if (drop.zone == PrKitDropZone_Before)
            placement = TreePlacement_Before;
        else if (drop.zone == PrKitDropZone_After)
            placement = TreePlacement_After;
        editor.moveEntity(EntityId(drop.item), editor.sceneRows[drop.row], placement);
    }
}

// --- Project ---

struct AssetDef {
    const char* name;
    PrKitIconKind icon;
};

constexpr AssetDef kAssets[] = {
    {"player_base", PrKitIconKind_Cube},  {"player_idle", PrKitIconKind_Script}, {"enemy_grunt", PrKitIconKind_Cube},
    {"env_diffuse", PrKitIconKind_Image}, {"env_normal", PrKitIconKind_Image},   {"water_pbr", PrKitIconKind_Sphere},
    {"footsteps", PrKitIconKind_Audio},   {"ambient_wind", PrKitIconKind_Audio}, {"controller", PrKitIconKind_Script},
    {"enemy_ai", PrKitIconKind_Script},   {"level_01", PrKitIconKind_Folder},    {"light_probes", PrKitIconKind_Light},
};

void buildAssetGrid(PrKit* kit, PrLayoutNode parent, EditorState& editor, uint32_t count) {
    PrUi* ui = kit->ui;
    PrKitBoxStyle grid{
        .direction = PrKitDirection_Row,
        .overflow = PrKitOverflow_Clip,
        .wrap = true,
        .gap = kPrKitSpace1,
        .padding = prKitEdgesAll(kPrKitSpace4),
    };
    grid.place.flex = PrKitFlex_Fill;
    PrLayoutNode cells = prKitBox(kit, parent, {}, grid);
    for (uint32_t i = 0; i < count && i < countOf(kAssets); ++i) {
        bool selected = editor.selectedAsset == i;
        PrKitSurfaceStyle cell{.ground = PrKitGround(selected ? PrKitGround_Select : PrKitGround_None)};
        cell.box.justify = PrKitJustify_Center;
        cell.box.align = PrKitAlign_Center;
        cell.box.gap = kPrKitSpace2;
        cell.box.place.width = prKitPx(84);
        cell.box.place.height = prKitPx(88);
        PrLayoutNode node = prKitSurface(kit, cells, {}, cell);
        prUiSetFlags(ui, node, PrUiFlag_Pressable);
        if (prUiIsPressed(ui, node, 0))
            prUiDragStart(ui, node, 4, kDragAsset, i);
        prKitIcon(kit, node, {.icon = kAssets[i].icon},
            {.size = 24, .tone = PrKitTone(selected ? PrKitTone_Accent : PrKitTone_Muted)});
        PrKitTextStyle name = kMeta;
        name.tone = selected ? PrKitTone_Accent : PrKitTone_Muted;
        prKitLabel(kit, node, {.text = kAssets[i].name}, name);
        if (prUiClicked(ui, node, 0))
            editor.selectedAsset = i;
    }
}

void buildProjectPanel(PrKit* kit, PrLayoutNode parent, EditorState& editor) {
    PrLayoutNode header = prKitPanelHeader(kit, parent, {.title = "PROJECT"});
    headerButton(kit, header, PrKitIconKind_Plus, false);
    headerButton(kit, header, PrKitIconKind_Filter, false);
    PrLayoutNode crumbs = bar(kit, parent, PrKitGround_Panel, PrKitRuleFlag_Bottom, kPrKitHeightSm, kPrKitSpace2);
    const char* segments[] = {"Assets", "Models", "Textures"};
    for (uint32_t i = 0; i < 3; ++i) {
        PrKitTextStyle style{
            .role = PrKitTextRole_MonoStrong,
            .tone = PrKitTone(i == 2 ? PrKitTone_Primary : PrKitTone_Muted),
        };
        prKitLabel(kit, crumbs, {.text = segments[i]}, style);
        if (i != 2)
            prKitLabel(kit, crumbs, {.text = "/"}, kMeta);
    }
    buildAssetGrid(kit, parent, editor, 8);
}

// --- Viewport ---

// Draws the viewport's grid over its content box.
void paintGrid(void* context, PrVec2 size, PrDrawList* list) {
    const EditorState& editor = *static_cast<const EditorState*>(context);
    prDrawListRect(list, PrRect{0, 0, size.x, size.y}, {.color = PrColor{5 / 255.0f, 6 / 255.0f, 7 / 255.0f, 1}});
    if (!editor.grid)
        return;
    PrColor line{0.16f, 0.18f, 0.22f, 1};
    for (float x = 24; x < size.x; x += 24)
        prDrawListRect(list, PrRect{x, 0, x + 1, size.y}, {.color = line});
    for (float y = 24; y < size.y; y += 24)
        prDrawListRect(list, PrRect{0, y, size.x, y + 1}, {.color = line});
}

void buildViewport(PrKit* kit, PrLayoutNode parent, EditorState& editor) {
    PrLayoutTree* tree = kit->tree;
    PrLayoutNode header = prKitPanelHeader(kit, parent, {.title = "VIEWPORT"});
    const char* tabs[] = {"Scene", "Game", "Animator"};
    segmented(kit, header, tabs, 3, &editor.viewportTab, PrKitControlSize_Xs);
    if (headerButton(kit, header, PrKitIconKind_Grid, editor.grid))
        editor.grid = !editor.grid;

    // The grid fills the viewport, and a layer of chips sits over it in its corners.
    PrKitBoxStyle view{.overflow = PrKitOverflow_Clip};
    view.place.flex = PrKitFlex_Fill;
    PrLayoutNode viewport = prKitStack(kit, parent, {}, view);
    PrLayoutNode grid = prLayoutTreeFrameReplaced(tree, viewport, 0, 0);
    PrKitPlace fill{.flex = PrKitFlex_Fill};
    prKitPlace(kit, grid, &fill);
    PrLayoutContent content{};
    content.kind = PrLayoutContentKind_Painter;
    content.painter.paint = paintGrid;
    content.painter.context = &editor;
    prLayoutTreeFrameSetContent(tree, grid, &content);

    // The HUD is a library of the editor's own, which shares the tree and the context with the kit. Its layer sits in a
    // kit panel, its panels hold kit widgets beside its own, and each library's floating widgets anchor to the other's
    // widgets in the one overlay.
    Hud* hud = editor.hud;
    PrLayoutNode layer = hudLayer(hud, viewport, {});
    PrLayoutNode camera = hudPanel(hud, layer, {.corner = HudCorner_TopLeft});
    hudReadout(hud, camera, {.label = "CAM", .value = "persp . 50deg"});
    hudReadout(hud, camera, {.label = "POS", .value = "12.400  3.200  -8.750"});
    PrLayoutNode modes = hudPanel(hud, layer, {.corner = HudCorner_TopRight, .row = true});
    hudReadout(hud, modes, {.label = "VIEW"});
    PrLayoutNode mode = renderModeButton(kit, modes, editor, PrKitControlSize_Xs);
    hudHint(hud, mode, "Render mode");
    PrLayoutNode stats = hudPanel(hud, layer, {.corner = HudCorner_BottomLeft, .row = true});
    hudReadout(hud, stats, {.label = "FPS", .value = "60.00"});
    hudReadout(hud, stats, {.label = "DRAW", .value = "1,412"});
    hudReadout(hud, stats, {.label = "TRIS", .value = "2.4M"});
    // The switches bind the model that the toolbar's buttons bind too, and their gestures reach the editor's sink.
    PrLayoutNode gridSwitch = hudSwitch(hud, stats, {.label = "GRID", .value = &editor.grid});
    prKitTooltip(kit, gridSwitch, {.text = "Show the grid", .shortcut = "Cmd '"});
    PrLayoutNode snapSwitch = hudSwitch(hud, stats, {.label = "SNAP", .value = &editor.snap});
    prKitTooltip(kit, snapSwitch, {.text = "Snap to the grid", .shortcut = "Cmd ;"});
}

// --- Console ---

void buildConsole(PrKit* kit, PrLayoutNode parent, EditorState& editor) {
    PrLayoutNode tabs = bar(kit, parent, PrKitGround_Header, PrKitRuleFlag_Bottom, kPrKitHeightLg, kPrKitSpace1);
    struct ConsoleTabDef {
        ConsoleTab tab;
        PrStringView label;
    };
    static const ConsoleTabDef kTabs[] = {
        {ConsoleTab_Console, "Console"},
        {ConsoleTab_AssetBrowser, "Asset Browser"},
        {ConsoleTab_Timeline, "Timeline"},
        {ConsoleTab_Profiler, "Profiler"},
    };
    PrLayoutNode strip = prKitTabStrip(kit, tabs, {}, {});
    for (const ConsoleTabDef& def : kTabs) {
        PrLayoutNode tab = prKitTab(kit, strip, {.selected = editor.consoleTab == def.tab});
        prKitTabLabel(kit, tab, {.text = def.label});
        if (prKitActivated(kit, tab))
            editor.consoleTab = def.tab;
    }
    prKitSpacer(kit, tabs, {});
    PrKitBoxStyle list{.overflow = PrKitOverflow_ScrollY, .padding = prKitEdgesXY(kPrKitSpace2, 0)};
    list.place.flex = PrKitFlex_Fill;
    PrLayoutNode body = prKitBox(kit, parent, {}, list);
    if (editor.consoleTab == ConsoleTab_AssetBrowser) {
        buildAssetGrid(kit, body, editor, 12);
        return;
    }
    for (const LogEntry* entry : editor.visibleLogs) {
        PrKitLogLine line{
            .time = entry->time,
            .message = entry->message,
            .source = entry->source,
            .level = entry->level,
        };
        prKitLogLine(kit, body, line);
    }
}

// --- Inspector ---

void buildInspector(PrKit* kit, PrLayoutNode parent, EditorState& editor) {
    SceneEntity* sel = editor.inspectorLocked ? editor.find(editor.lockedEntity) : selectedEntity(editor);
    PrLayoutNode header = prKitPanelHeader(kit, parent, {.title = "INSPECTOR"});
    if (headerButton(kit, header, PrKitIconKind(editor.inspectorLocked ? PrKitIconKind_Lock : PrKitIconKind_Unlock),
            editor.inspectorLocked)) {
        editor.inspectorLocked = !editor.inspectorLocked;
        editor.lockedEntity = editor.inspectorLocked && sel != nullptr ? sel->id : 0;
    }
    headerButton(kit, header, PrKitIconKind_DotsV, false);
    PrLayoutNode identity = bar(kit, parent, PrKitGround_None, PrKitRuleFlag_Bottom, kPrKitHeightLg, kPrKitSpace3);
    if (sel != nullptr) {
        prKitCheckbox(kit, identity, {.value = &sel->enabled}, {});
    }
    prKitIcon(kit, identity, {.icon = PrKitIconKind_Cube},
        {.size = 14, .tone = PrKitTone(sel != nullptr ? PrKitTone_Secondary : PrKitTone_Faint)});
    prKitLabel(kit, identity, {.text = sel != nullptr ? sel->name : "No selection"}, kBody);
    prKitSpacer(kit, identity, {});
    prKitBadge(kit, identity, {.text = "ENTITY"}, {.variant = PrKitBadgeVariant_Accent});

    PrKitBoxStyle scroll{.overflow = PrKitOverflow_ScrollY};
    scroll.place.flex = PrKitFlex_Fill;
    PrLayoutNode body = prKitBox(kit, parent, {}, scroll);
    prUiSetFlags(kit->ui, body, PrUiFlag_Scrollable);
    if (sel == nullptr)
        return;
    PrKitInspectorGroup transformGroup{
        .name = "Transform",
        .badge = "CORE",
        .badgeVariant = PrKitBadgeVariant_Accent,
        .open = &editor.openTransform,
    };
    PrLayoutNode transform = prKitInspectorGroup(kit, body, transformGroup);
    if (transform != kPrLayoutNodeNone) {
        PrKitNumberRange fine{.step = 0.05};
        PrLayoutNode position = prKitInspectorRow(kit, transform, {.label = "Position"}, {});
        prKitVectorField(kit, position, {.value = prKitScalarF32(&sel->position.x), .count = 3, .range = fine}, {});
        PrKitNumberRange degrees{.step = 0.5};
        PrLayoutNode rotation = prKitInspectorRow(kit, transform, {.label = "Rotation"}, {});
        PrKitVector turn{.value = prKitScalarF32(&sel->rotation.x), .count = 3, .range = degrees};
        prKitVectorField(kit, rotation, turn, {});
        PrKitNumberRange scale{.step = 0.01, .min = 0, .hasMin = true};
        PrLayoutNode scaleRow = prKitInspectorRow(kit, transform, {.label = "Scale"}, {});
        prKitVectorField(kit, scaleRow, {.value = prKitScalarF32(&sel->scale.x), .count = 3, .range = scale}, {});
    }
    PrKitInspectorGroup meshGroup{.name = "Mesh Renderer", .open = &editor.openMesh};
    PrLayoutNode mesh = prKitInspectorGroup(kit, body, meshGroup);
    if (mesh != kPrLayoutNodeNone) {
        PrKitAssetField meshField{.name = sel->meshAsset, .icon = PrKitIconKind_Cube, .kind = kDragAsset};
        PrLayoutNode meshRow = prKitInspectorRow(kit, mesh, {.label = "Mesh"}, {});
        PrLayoutNode field = prKitAssetField(kit, meshRow, meshField);
        uint64_t dropped = 0;
        if (prKitDropped(kit, field, &dropped) && dropped < countOf(kAssets))
            sel->meshAsset = std::string(kAssets[dropped].name) + ".mesh";
        prKitColorField(kit, prKitInspectorRow(kit, mesh, {.label = "Tint"}, {}), {.color = &editor.tint}, {});
        const char* lighting[] = {"realtime", "mixed", "baked"};
        segmented(kit, prKitInspectorRow(kit, mesh, {.label = "Lighting"}, {}), lighting, 3, &editor.lighting,
            PrKitControlSize_Sm);
        PrLayoutNode shadows = prKitInspectorRow(kit, mesh, {.label = "Shadows"}, {});
        prKitCheckbox(kit, shadows, {.value = &sel->castShadows, .label = "cast"}, {});
        prKitCheckbox(kit, shadows, {.value = &sel->receiveShadows, .label = "receive"}, {});
    }
    PrKitInspectorGroup bodyGroup{.name = "Rigidbody", .open = &editor.openRigidbody};
    PrLayoutNode rigidbody = prKitInspectorGroup(kit, body, bodyGroup);
    if (rigidbody != kPrLayoutNodeNone) {
        PrKitNumber mass{
            .value = prKitScalarF32(&sel->mass),
            .range = {.step = 0.05, .min = 0, .hasMin = true},
            .unit = "kg",
        };
        prKitNumberField(kit, prKitInspectorRow(kit, rigidbody, {.label = "Mass"}, {}), mass, {});
        PrLayoutNode drag = prKitInspectorRow(kit, rigidbody, {.label = "Drag"}, {});
        prKitSlider(kit, drag, {.value = prKitScalarF32(&editor.drag), .min = 0, .max = 1}, {});
        PrKitNumberRange unit{.step = 0.005, .min = 0, .max = 1, .hasMin = true, .hasMax = true};
        PrKitNumber dragNumber{.value = prKitScalarF32(&editor.drag), .range = unit};
        PrLayoutNode dragField = prKitNumberField(kit, drag, dragNumber, {});
        PrKitPlace narrow{.width = prKitPx(56), .flex = PrKitFlex_Fixed};
        prKitPlace(kit, dragField, &narrow);
        prKitToggle(kit, prKitInspectorRow(kit, rigidbody, {.label = "Gravity"}, {}), {.value = &sel->gravity}, {});
    }
    PrKitInspectorGroup scriptGroup{
        .name = "Controller Script",
        .badge = "COMPILED",
        .badgeVariant = PrKitBadgeVariant_Success,
        .open = &editor.openScript,
    };
    PrLayoutNode script = prKitInspectorGroup(kit, body, scriptGroup);
    if (script != kPrLayoutNodeNone) {
        PrKitNumberRange positive{.step = 0.05, .min = 0, .hasMin = true};
        PrLayoutNode speed = prKitInspectorRow(kit, script, {.label = "Speed"}, {});
        prKitNumberField(kit, speed, {.value = prKitScalarF32(&editor.scriptSpeed), .range = positive}, {});
        PrLayoutNode jump = prKitInspectorRow(kit, script, {.label = "Jump"}, {});
        prKitNumberField(kit, jump, {.value = prKitScalarF32(&editor.scriptJump), .range = positive}, {});
    }
}

// --- Status bar ---

void buildStatusBar(PrKit* kit, PrLayoutNode parent, EditorState& editor) {
    PrLayoutNode status = bar(kit, parent, PrKitGround_Header, PrKitRuleFlag_Top, kPrKitHeightSm, kPrKitSpace4);
    prKitIcon(kit, status, {.icon = PrKitIconKind_Check}, {.size = 10, .tone = PrKitTone_Success});
    PrKitTextStyle success{.role = PrKitTextRole_Caption, .tone = PrKitTone_Success};
    prKitLabel(kit, status, {.text = "BUILD OK"}, success);
    char entities[24];
    snprintf(entities, sizeof entities, "ENT %zu", editor.nodes.size());
    char edits[24];
    snprintf(edits, sizeof edits, "EDITS %u", editor.edits);
    const char* items[] = {
        "shipwright/Level_01", entities, edits, "DRAW 1,412", "TRIS 2.4M",
        editor.playing ? "PLAY . 60.00fps" : "EDIT . 60.00fps",
    };
    for (const char* item : items) {
        prKitDivider(kit, status, {});
        prKitLabel(kit, status, {.text = item}, kCaption);
    }
    prKitSpacer(kit, status, {});
    prKitLabel(kit, status, {.text = "LINE 42 . COL 18"}, kCaption);
    prKitDivider(kit, status, {});
    prKitLabel(kit, status, {.text = "UTF-8"}, kCaption);
}

// The panels of the dock.
enum : PrKitDockPanelId {
    Panel_Scene = 1,
    Panel_Viewport,
    Panel_Inspector,
    Panel_Project,
    Panel_Console,
};
typedef PrKitDockPanelId Panel;

PrKitDockTab describePanel(void*, PrKitDockPanelId panel) {
    static const char* const kTitles[] = {"", "Scene", "Viewport", "Inspector", "Project", "Console"};
    return PrKitDockTab{.title = (panel < countOf(kTitles) ? kTitles[panel] : "")};
}

// Makes the dock with five panels: the scene over the project, the viewport over the console, and the inspector.
PrKitDock* makeDock() {
    PrKitDock* dock = prKitDockCreate();
    PrKitDockNode scene = prKitDockPrimary(dock);
    prKitDockAddPanel(dock, scene, Panel_Scene);
    PrKitDockNode viewport = prKitDockSplit(dock, scene, PrKitDockEdge_Right, Panel_Viewport, 0.78f);
    prKitDockSplit(dock, viewport, PrKitDockEdge_Right, Panel_Inspector, 0.28f);
    prKitDockSplit(dock, scene, PrKitDockEdge_Bottom, Panel_Project, 0.38f);
    prKitDockSplit(dock, viewport, PrKitDockEdge_Bottom, Panel_Console, 0.28f);
    return dock;
}

// Counts the gestures that the editor's controls commit, whichever library made each control.
void countEdit(void* context, PrLayoutNode, PrUiEditPhase phase) {
    if (phase == PrUiEditPhase_Commit)
        ++static_cast<EditorState*>(context)->edits;
}

} // namespace

void buildEditor(PrKit* kit, PrLayoutNode ground, EditorState& editor) {
    prUiSetEditSink(kit->ui, ground, countEdit, &editor);
    buildMenubar(kit, ground, editor);
    buildToolbar(kit, ground, editor);
    if (editor.dock == nullptr)
        editor.dock = makeDock();
    PrLayoutNode dock = prKitDockBuild(kit, ground, {
        .key = prUiKey(kit->ui, ground, "dock"),
        .dock = editor.dock,
        .describe = describePanel,
    });
    PrKitPlace fill{.flex = PrKitFlex_Fill};
    prKitPlace(kit, dock, &fill);
    for (uint32_t i = 0; i < prKitDockPaneCount(editor.dock); ++i) {
        PrKitDockPane pane = prKitDockPane(editor.dock, i);
        switch (pane.panel) {
            case Panel_Scene:
                buildScenePanel(kit, pane.content, editor);
                break;
            case Panel_Viewport:
                buildViewport(kit, pane.content, editor);
                break;
            case Panel_Inspector:
                buildInspector(kit, pane.content, editor);
                break;
            case Panel_Project:
                buildProjectPanel(kit, pane.content, editor);
                break;
            case Panel_Console:
                buildConsole(kit, pane.content, editor);
                break;
            default:
                break;
        }
    }
    buildStatusBar(kit, ground, editor);
    buildPalette(kit, prLayoutTreeRoot(kit->tree), editor);
}

void editorAttach(void* context, editor_bench::Host& host) {
    static_cast<EditorState*>(context)->hud = hudCreate(host.ui, host.fonts);
}

void editorDetach(void* context, editor_bench::Host&) {
    EditorState& editor = *static_cast<EditorState*>(context);
    hudDestroy(editor.hud);
    editor.hud = nullptr;
}

} // namespace editor
