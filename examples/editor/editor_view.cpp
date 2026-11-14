// The editor's view (editor_view.h).

#include "examples/editor/editor_view.h"

#include "pr/ascii.h"
#include "pr/count_of.h"
#include "pr/draw_list.h"
#include "pr/float.h"
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
#include "pr/kit_panel.h"
#include "pr/kit_text.h"
#include "pr/layout_frame.h"
#include "pr/layout_query.h"
#include "pr/layout_unit.h"
#include "pr/print.h"
#include "pr/ui.h"

#include "examples/editor/hud.h"

namespace editor {

using namespace pr;

namespace {

constexpr PrKitTextStyle kBody = {.wrap = PrKitWrap_Clip};
constexpr PrKitTextStyle kCaption = {.role = PrKitTextRole_Caption};
constexpr PrKitTextStyle kMeta = {.role = PrKitTextRole_Meta};

constexpr PrKitButtonStyle kGhostXs = {.variant = PrKitButtonVariant_Ghost, .size = PrKitControlSize_Xs};
constexpr PrKitButtonStyle kGhostSm = {.variant = PrKitButtonVariant_Ghost, .size = PrKitControlSize_Sm};
constexpr PrKitButtonStyle kGhostMd = {.variant = PrKitButtonVariant_Ghost, .size = PrKitControlSize_Md};

constexpr const char* kRenderModes[RenderMode_Count] = {"Shaded", "Flat", "Wireframe"};

// The payload kinds of the drags: a row of the scene tree, and an asset of the project.
constexpr uint64_t kDragEntity = 1;
constexpr uint64_t kDragAsset = 2;

// Creates a row of fixed `height` on `background`, with the rules that `rules` names. Its items are `gap` px apart and
// centered across the row.
PrLayoutNode bar(PrKit* kit, PrLayoutNode parent, PrKitBackground background, PrKitRuleFlags rules, float height, float gap) {
    PrKitPanelStyle look{.background = background, .rules = rules};
    look.box.direction = PrKitDirection_Row;
    look.box.align = PrKitAlign_Center;
    look.box.gap = gap;
    look.box.padding = prKitEdgesXY(0, kPrKitSpace3);
    look.box.overflow = PrKitOverflow_Clip;
    look.box.place.height = prKitPx(height);
    look.box.place.flex = PrKitFlex_Fixed;
    return prKitPanel(kit, parent, {}, look);
}

// Creates the bar of a panel's actions under the dock's tab, which names the panel.
PrLayoutNode actionBar(PrKit* kit, PrLayoutNode panel) {
    return bar(kit, panel, PrKitBackground_Panel, PrKitRuleFlag_Bottom, kPrKitHeightLg, kPrKitSpace2);
}

// Creates a box that fills the rest of its panel and scrolls down.
PrLayoutNode scrolling(PrKit* kit, PrLayoutNode panel, PrKitEdges padding = {}) {
    PrKitBoxStyle style{.overflow = PrKitOverflow_ScrollY, .padding = padding};
    style.place.flex = PrKitFlex_Fill;
    return prKitBox(kit, panel, {}, style);
}

// Creates a ghost button with `icon` that shows `on` as selected, and returns true if it was activated.
bool iconButton(PrKit* kit, PrLayoutNode parent, PrKitIconKind icon, bool on, PrKitButtonStyle style) {
    return prKitActivated(kit, prKitButton(kit, parent, {.icon = icon, .selected = on}, style));
}

// Creates a segmented control of `count` items, the one at `*selected` selected. Activating an item selects it.
void segmented(PrKit* kit, PrLayoutNode parent, const char* const* items, uint32_t count, uint32_t* selected) {
    PrLayoutNode control = prKitSegmented(kit, parent, {}, {.size = PrKitControlSize_Sm});
    for (uint32_t i = 0; i < count; ++i) {
        if (prKitActivated(kit, prKitSegment(kit, control, {.label = items[i], .selected = *selected == i})))
            *selected = i;
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Commands

// What the editor can be asked to do, from its menus and its command palette.
enum {
    Command_None,
    Command_AddEntity,
    Command_DeleteSelected,
    Command_ToggleGrid,
    Command_ToggleSnap,
    Command_Play,
    Command_ClearConsole,
    Command_Palette,
};
typedef uint8_t Command;

struct CommandDef {
    const char* text;
    const char* shortcut;
    Command command;
    PrKitIconKind icon;
};

void run(EditorState& editor, Command command) {
    switch (command) {
        case Command_AddEntity: editor.addEntity(); break;
        case Command_DeleteSelected: editor.removeEntity(editor.selected); break;
        case Command_ToggleGrid: editor.grid = !editor.grid; break;
        case Command_ToggleSnap: editor.snap = !editor.snap; break;
        case Command_Play: editor.playing = !editor.playing; break;
        case Command_ClearConsole: editor.clearLogs(); break;
        case Command_Palette: editor.paletteOpen = true; break;
        default: break;
    }
}

// A menu of the menu bar, and its items.
struct MenuDef {
    const char* text;
    const CommandDef* items;
    uint32_t count;
};

constexpr CommandDef kEditMenu[] = {{"Command Palette...", "Cmd K", Command_Palette, PrKitIconKind_Search}};
constexpr CommandDef kViewMenu[] = {
    {"Grid", "Cmd '", Command_ToggleGrid, PrKitIconKind_Grid},
    {"Snap", "Cmd ;", Command_ToggleSnap, PrKitIconKind_Magnet},
};
constexpr CommandDef kEntityMenu[] = {
    {"Add Entity", "Shift Cmd N", Command_AddEntity, PrKitIconKind_Cube},
    {"Delete Selected", "Del", Command_DeleteSelected, PrKitIconKind_Close},
};
constexpr CommandDef kRunMenu[] = {
    {"Play", "Cmd P", Command_Play, PrKitIconKind_Play},
    {"Clear Console", "Cmd L", Command_ClearConsole, PrKitIconKind_Close},
};

template <uint32_t N>
constexpr MenuDef menu(const char* text, const CommandDef (&items)[N]) {
    return MenuDef{text, items, N};
}

constexpr MenuDef kMenus[] = {
    menu("Edit", kEditMenu), menu("View", kViewMenu), menu("Entity", kEntityMenu),
    menu("Run", kRunMenu),
};

// Returns true if the command's menu item shows a check.
bool checked(const EditorState& editor, Command command) {
    return (command == Command_ToggleGrid && editor.grid) || (command == Command_ToggleSnap && editor.snap) ||
        (command == Command_Play && editor.playing);
}

void buildMenuBar(PrKit* kit, PrLayoutNode parent, EditorState& editor) {
    PrLayoutNode menubar = prKitMenuBar(kit, parent, {});
    for (const MenuDef& def : kMenus) {
        PrLayoutNode title = prKitMenuBarItem(kit, menubar, {.text = def.text});
        PrLayoutNode popover = prKitPopover(kit, title, {}, {});
        if (popover == kPrLayoutNodeNone)
            continue;
        PrLayoutNode items = prKitMenu(kit, popover, {}, {.minWidth = 220});
        for (uint32_t i = 0; i < def.count; ++i) {
            const CommandDef& command = def.items[i];
            PrKitMenuItem item{
                .text = command.text,
                .shortcut = command.shortcut,
                .checked = checked(editor, command.command),
                .disabled = command.command == Command_DeleteSelected && editor.selected == 0,
            };
            if (prKitActivated(kit, prKitMenuItem(kit, items, item, {})))
                run(editor, command.command);
        }
    }
    prKitSpacer(kit, menubar, {});
    PrLayoutNode search = prKitButton(kit, menubar, {.text = "Search", .icon = PrKitIconKind_Search}, kGhostSm);
    if (prKitActivated(kit, search))
        editor.paletteOpen = true;
    prKitTooltip(kit, search, {.text = "Command Palette", .shortcut = "Cmd K"});
}

// Declares the command palette over the window while it is open. Its trigger is the root, which claims Cmd K.
void buildPalette(PrKit* kit, PrLayoutNode root, EditorState& editor) {
    if (prUiKeyPressed(kit->ui, root, 'K', PrAppModifierFlag_Super))
        editor.paletteOpen = !editor.paletteOpen;
    prKitSetOpen(kit, root, editor.paletteOpen);
    uint32_t count = 0;
    for (const MenuDef& def : kMenus)
        count += def.count;
    PrLayoutNode list = prKitCommandPalette(kit, root, {.prompt = "Search commands", .count = count});
    if (list == kPrLayoutNodeNone) {
        editor.paletteOpen = false;
        return;
    }
    for (const MenuDef& def : kMenus) {
        for (uint32_t i = 0; i < def.count; ++i) {
            const CommandDef& command = def.items[i];
            PrKitCommandItem item{
                .text = command.text,
                .category = def.text,
                .shortcut = command.shortcut,
                .icon = command.icon,
            };
            if (prKitActivated(kit, prKitCommandItem(kit, list, item))) {
                editor.paletteOpen = false;
                prKitSetOpen(kit, root, false);
                run(editor, command.command);
            }
        }
    }
    prKitCommandPaletteFooter(kit, list);
}

// ---------------------------------------------------------------------------------------------------------------------
// Toolbar and status bar

void buildToolbar(PrKit* kit, PrLayoutNode parent, EditorState& editor) {
    PrLayoutNode toolbar = bar(kit, parent, PrKitBackground_Panel, PrKitRuleFlag_Bottom, kPrKitHeightLg, kPrKitSpace1);
    struct ToolDef {
        Tool tool;
        PrKitIconKind icon;
        const char* name;
    };
    constexpr ToolDef kTools[] = {
        {Tool_Select, PrKitIconKind_Cursor, "Select"},
        {Tool_Move, PrKitIconKind_Move, "Move"},
        {Tool_Rotate, PrKitIconKind_Rotate, "Rotate"},
        {Tool_Scale, PrKitIconKind_Scale, "Scale"},
    };
    for (const ToolDef& tool : kTools) {
        PrLayoutNode button = prKitButton(kit, toolbar, {.icon = tool.icon, .selected = editor.tool == tool.tool}, kGhostMd);
        if (prKitActivated(kit, button))
            editor.tool = tool.tool;
        prKitTooltip(kit, button, {.text = tool.name});
    }
    prKitDivider(kit, toolbar, {});
    const char* spaces[] = {"Local", "World"};
    segmented(kit, toolbar, spaces, 2, &editor.space);
    prKitDivider(kit, toolbar, {});
    if (iconButton(kit, toolbar, PrKitIconKind_Magnet, editor.snap, kGhostMd))
        editor.snap = !editor.snap;
    if (iconButton(kit, toolbar, PrKitIconKind_Grid, editor.grid, kGhostMd))
        editor.grid = !editor.grid;
    prKitSpacer(kit, toolbar, {});
    PrKitButton play{
        .text = editor.playing ? "Pause" : "Play",
        .icon = PrKitIconKind(editor.playing ? PrKitIconKind_Pause : PrKitIconKind_Play),
        .selected = editor.playing,
    };
    if (prKitActivated(kit, prKitButton(kit, toolbar, play, {
            .variant = PrKitButtonVariant_Primary,
            .size = PrKitControlSize_Sm,
    })))
        editor.playing = !editor.playing;
    if (prKitActivated(kit, prKitButton(kit, toolbar, {.icon = PrKitIconKind_Stop}, {.size = PrKitControlSize_Sm})))
        editor.playing = false;
}

void buildStatusBar(PrKit* kit, PrLayoutNode parent, const EditorState& editor) {
    PrLayoutNode status = bar(kit, parent, PrKitBackground_Header, PrKitRuleFlag_Top, kPrKitHeightSm, kPrKitSpace3);
    prKitLabel(kit, status, {.text = editor.levelName}, kCaption);
    char entities[32];
    char edits[32];
    PrStringView items[] = {
        formatToBuffer(Span(entities), "{} entities", editor.entities.count()),
        formatToBuffer(Span(edits), "{} edits", editor.edits),
    };
    for (PrStringView item : items) {
        prKitDivider(kit, status, {});
        prKitLabel(kit, status, {.text = item}, kCaption);
    }
    prKitSpacer(kit, status, {});
    PrKitTextStyle mode{
        .role = PrKitTextRole_Caption,
        .tone = PrKitTone(editor.playing ? PrKitTone_Success : PrKitTone_Muted),
    };
    prKitLabel(kit, status, {.text = editor.playing ? "PLAYING" : "EDITING"}, mode);
}

// ---------------------------------------------------------------------------------------------------------------------
// Scene

// Lists the entities that the scene tree shows: those whose ancestors are open, or, while the filter has text, every
// entity whose name holds it, flat.
void sceneRows(EditorState& editor) {
    editor.sceneRows.clear();
    editor.sceneItems.clear();
    bool filtered = !editor.sceneFilter.isEmpty();
    uint32_t closedDepth = UINT32_MAX;
    for (const Entity& entity : editor.entities) {
        if (filtered) {
            if (prAsciiContainsFold(entity.name, editor.sceneFilter)) {
                editor.sceneRows.push(entity.id);
                editor.sceneItems.push(PrKitTreeItem{});
            }
            continue;
        }
        if (entity.depth > closedDepth)
            continue;
        closedDepth = entity.hasChildren && !entity.open ? entity.depth : UINT32_MAX;
        editor.sceneRows.push(entity.id);
        editor.sceneItems.push(PrKitTreeItem{
            .depth = entity.depth,
            .hasChildren = entity.hasChildren,
            .open = entity.open,
        });
    }
}

void buildScenePanel(PrKit* kit, PrLayoutNode panel, EditorState& editor) {
    PrLayoutNode actions = actionBar(kit, panel);
    PrKitTextField filter{
        .text = editor.sceneFilter,
        .placeholder = "Filter",
        .icon = PrKitIconKind_Search,
        .clearable = true,
    };
    PrLayoutNode field = prKitTextField(kit, actions, filter, {.size = PrKitControlSize_Sm});
    PrStringView changed{};
    if (prKitTextChanged(kit, field, &changed))
        editor.sceneFilter = changed;
    prKitPlace(kit, field, {.flex = PrKitFlex_Fill});
    PrLayoutNode add = prKitButton(kit, actions, {.icon = PrKitIconKind_Plus}, kGhostXs);
    if (prKitActivated(kit, add))
        editor.addEntity();
    prKitTooltip(kit, add, {.text = "Add Entity", .shortcut = "Shift Cmd N"});

    sceneRows(editor);
    PrKitTreeView content{
        .count = editor.sceneRows.count(),
        .selection = &editor.sceneCursor,
        .items = editor.sceneItems.data(),
        .dragKind = kDragEntity,
    };
    PrLayoutNode view = prKitTreeView(kit, panel, content, {.place = {.flex = PrKitFlex_Fill}});
    PrUi* ui = kit->ui;
    PrKitWindow window = prKitListWindow(kit, view);
    for (uint32_t i = window.first; i < window.first + window.count; ++i) {
        Entity& entity = *editor.find(editor.sceneRows[i]);
        PrLayoutNode node =
            prKitTreeRow(kit, view, {.index = i, .item = entity.id, .selected = editor.selected == entity.id});
        prKitRowIcon(kit, node, {.icon = entity.icon});
        prKitRowLabel(kit, node, {.text = entity.name}, PrKitTone(entity.hidden ? PrKitTone_Faint : PrKitTone_Primary));
        if (!entity.type.isEmpty())
            prKitRowMeta(kit, node, {.text = entity.type});
        PrKitVisibilityToggle toggle{.hidden = entity.hidden, .revealed = prUiIsHovered(ui, node)};
        if (prKitActivated(kit, prKitVisibilityToggle(kit, node, toggle)))
            entity.hidden = !entity.hidden;
    }
    PrKitTreeFold fold{};
    if (prKitFolded(kit, view, &fold)) {
        Entity& entity = *editor.find(editor.sceneRows[fold.row]);
        entity.open = !entity.open;
    }
    PrKitSelectionEdit edit{};
    if (prKitSelectionEdited(kit, view, &edit) && editor.sceneCursor.lead < editor.sceneRows.count())
        editor.selected = editor.sceneRows[editor.sceneCursor.lead];
    PrKitTreeDrop drop{};
    if (prKitRowDropped(kit, view, &drop) && drop.row < editor.sceneRows.count()) {
        TreePlacement placement = TreePlacement_Into;
        if (drop.zone == PrKitDropZone_Before)
            placement = TreePlacement_Before;
        else if (drop.zone == PrKitDropZone_After)
            placement = TreePlacement_After;
        editor.moveEntity(EntityId(drop.item), editor.sceneRows[drop.row], placement);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Project

struct AssetDef {
    const char* name;
    PrKitIconKind icon;
};

constexpr AssetDef kAssets[] = {
    {"player_base", PrKitIconKind_Cube},   {"enemy_grunt", PrKitIconKind_Cube},   {"crate", PrKitIconKind_Cube},
    {"barrel", PrKitIconKind_Cube},        {"env_diffuse", PrKitIconKind_Image},  {"env_normal", PrKitIconKind_Image},
    {"water_pbr", PrKitIconKind_Sphere},   {"footsteps", PrKitIconKind_Audio},    {"controller", PrKitIconKind_Script},
    {"enemy_ai", PrKitIconKind_Script},    {"light_probes", PrKitIconKind_Light}, {"level_01", PrKitIconKind_Folder},
};

void buildProjectPanel(PrKit* kit, PrLayoutNode panel, EditorState& editor) {
    PrLayoutNode crumbs = actionBar(kit, panel);
    const char* path[] = {"Assets", "Models"};
    for (uint32_t i = 0; i < countOf(path); ++i) {
        if (i != 0)
            prKitLabel(kit, crumbs, {.text = "/"}, kMeta);
        bool last = i + 1 == countOf(path);
        prKitLabel(kit, crumbs, {.text = path[i]},
            {.role = PrKitTextRole_MonoStrong, .tone = PrKitTone(last ? PrKitTone_Primary : PrKitTone_Muted)});
    }
    PrLayoutNode body = scrolling(kit, panel);
    PrKitBoxStyle grid{
        .direction = PrKitDirection_Row,
        .wrap = true,
        .gap = kPrKitSpace1,
        .padding = prKitEdgesAll(kPrKitSpace3),
    };
    PrLayoutNode cells = prKitBox(kit, body, {}, grid);
    PrUi* ui = kit->ui;
    for (uint32_t i = 0; i < countOf(kAssets); ++i) {
        bool selected = editor.selectedAsset == i;
        PrKitPanelStyle cell{.background = PrKitBackground(selected ? PrKitBackground_Select : PrKitBackground_None)};
        cell.box.justify = PrKitJustify_Center;
        cell.box.align = PrKitAlign_Center;
        cell.box.gap = kPrKitSpace2;
        cell.box.place.width = prKitPx(84);
        cell.box.place.height = prKitPx(76);
        PrLayoutNode node = prKitPanel(kit, cells, {}, cell);
        // An asset drags onto the inspector's mesh field.
        prUiSetFlags(ui, node, PrUiFlag_Pressable);
        if (prUiIsPressed(ui, node, 0))
            prUiDragStart(ui, node, 4, kDragAsset, i, 0);
        if (prUiClicked(ui, node, 0))
            editor.selectedAsset = i;
        PrKitTone tone = PrKitTone(selected ? PrKitTone_Accent : PrKitTone_Muted);
        prKitIcon(kit, node, {.icon = kAssets[i].icon}, {.size = 22, .tone = tone});
        prKitLabel(kit, node, {.text = kAssets[i].name}, {.role = PrKitTextRole_Meta, .tone = tone});
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Viewport

// The ground plane, seen from above: a meter of it is this fraction of the viewport's shorter side, centered.
constexpr float kMetersAcross = 30;

// Returns how many px a meter of ground takes in a viewport of `size`.
float pixelsPerMeter(PrVec2 size) {
    return prF32Min(size.x, size.y) / kMetersAcross;
}

// Returns where an entity stands in a viewport of `size`, in px.
PrVec2 placeOf(const Entity& entity, PrVec2 size) {
    float scale = pixelsPerMeter(size);
    return PrVec2{size.x / 2 + entity.position.x * scale, size.y / 2 + entity.position.z * scale};
}

// Returns true if the viewport draws the entity: one with a place of its own, which is not hidden.
bool drawn(const Entity& entity) {
    return !entity.hidden && entity.type != "Group" && entity.type != "Script";
}

// The colors of the viewport.
constexpr PrColor kVoid = {0.02f, 0.024f, 0.03f, 1};
constexpr PrColor kGround = {0.10f, 0.14f, 0.11f, 1};
constexpr PrColor kGridLine = {1, 1, 1, 0.05f};
constexpr PrColor kWater = {0.16f, 0.36f, 0.58f, 1};
constexpr PrColor kLight = {0.98f, 0.78f, 0.34f, 1};
constexpr PrColor kCamera = {0.52f, 0.66f, 0.98f, 1};
constexpr PrColor kSolid = {0.78f, 0.80f, 0.84f, 1};
constexpr PrColor kSelected = {0.20f, 0.60f, 1.00f, 1};

// Fills `shape` as the render mode draws a solid: a fill with a darker rim, a flat fill, or an outline alone.
template <typename Shape>
void solid(RenderMode mode, PrColor color, Shape shape) {
    PrDrawStroke rim = prDrawStrokeDefault();
    rim.shape.width = 1.5f;
    if (mode == RenderMode_Wireframe) {
        shape(PrDrawPaint{.color = color, .stroke = &rim});
        return;
    }
    shape(PrDrawPaint{.color = color});
    if (mode == RenderMode_Shaded)
        shape(PrDrawPaint{.color = PrColor{0, 0, 0, 0.45f}, .stroke = &rim});
}

// Draws the level from above: the ground with its grid, and each entity as a mark of its kind.
void paintViewport(void* context, PrVec2 size, PrDrawList* list) {
    const EditorState& editor = *static_cast<const EditorState*>(context);
    float scale = pixelsPerMeter(size);
    PrVec2 center{size.x / 2, size.y / 2};
    prDrawListRect(list, PrRect{0, 0, size.x, size.y}, {.color = kVoid});
    PrRect ground{center.x - 13 * scale, center.y - 10 * scale, center.x + 13 * scale, center.y + 10 * scale};
    solid(editor.renderMode, kGround,
        [&](PrDrawPaint paint) { prDrawListCornerRect(list, prCornerRectRound(ground, scale), paint); });
    if (editor.grid) {
        for (float x = prF32Fmod(center.x, scale); x < size.x; x += scale)
            prDrawListRect(list, PrRect{x, 0, x + 1, size.y}, {.color = kGridLine});
        for (float y = prF32Fmod(center.y, scale); y < size.y; y += scale)
            prDrawListRect(list, PrRect{0, y, size.x, y + 1}, {.color = kGridLine});
    }
    for (const Entity& entity : editor.entities) {
        if (!drawn(entity) || entity.icon == PrKitIconKind_Terrain)
            continue;
        PrVec2 at = placeOf(entity, size);
        float r = scale * 0.6f;
        if (entity.icon == PrKitIconKind_Sphere) {
            solid(editor.renderMode, kWater,
                [&](PrDrawPaint paint) { prDrawListEllipse(list, at, PrVec2{3 * scale, 2.2f * scale}, 0, paint); });
        } else if (entity.type == "Light") {
            solid(editor.renderMode, kLight,
                [&](PrDrawPaint paint) { prDrawListEllipse(list, at, PrVec2{r, r}, 0, paint); });
        } else if (entity.type == "Camera") {
            PrVec2 points[] = {{at.x, at.y - r}, {at.x + r, at.y + r}, {at.x - r, at.y + r}};
            solid(editor.renderMode, kCamera, [&](PrDrawPaint paint) {
                prDrawListPolylineContour(list, points, 3, PrDrawContour_Closed, PrPathFillRule_NonZero, paint);
            });
        } else {
            PrRect box{at.x - r, at.y - r, at.x + r, at.y + r};
            solid(editor.renderMode, kSolid,
                [&](PrDrawPaint paint) { prDrawListCornerRect(list, prCornerRectRound(box, 2), paint); });
        }
        if (entity.id == editor.selected) {
            PrDrawStroke ring = prDrawStrokeDefault();
            ring.shape.width = 2;
            prDrawListEllipse(list, at, PrVec2{r + 6, r + 6}, 0, {.color = kSelected, .stroke = &ring});
        }
    }
}

// Selects the entity that a click on the viewport lands nearest, if one is within reach.
void pickEntity(PrKit* kit, PrLayoutNode viewport, EditorState& editor) {
    PrUi* ui = kit->ui;
    PrVec2 pointer{};
    if (!prUiClicked(ui, viewport, 0) || !prUiPointer(ui, nullptr, &pointer))
        return;
    PrRect bounds = prLayoutRectToPx(prLayoutNodeBounds(kit->tree, viewport, PrLayoutCoordinates_Viewport));
    PrVec2 size{bounds.right - bounds.left, bounds.bottom - bounds.top};
    PrVec2 local{pointer.x - bounds.left, pointer.y - bounds.top};
    float reach = pixelsPerMeter(size) * 1.5f;
    EntityId nearest = 0;
    for (const Entity& entity : editor.entities) {
        if (!drawn(entity))
            continue;
        PrVec2 at = placeOf(entity, size);
        float distance = prF32Hypot(at.x - local.x, at.y - local.y);
        if (distance < reach) {
            reach = distance;
            nearest = entity.id;
        }
    }
    editor.selected = nearest;
}

// Creates a button that shows the render mode, and the kit menu of the modes that it opens.
PrLayoutNode renderModeButton(PrKit* kit, PrLayoutNode parent, EditorState& editor) {
    PrLayoutNode button = prKitButton(kit, parent, {.text = kRenderModes[editor.renderMode]}, kGhostXs);
    if (prKitActivated(kit, button))
        prKitToggleOpen(kit, button);
    PrLayoutNode popover = prKitPopover(kit, button, {}, {});
    if (popover != kPrLayoutNodeNone) {
        PrLayoutNode modes = prKitMenu(kit, popover, {}, {.minWidth = 140});
        for (RenderMode mode = 0; mode < RenderMode_Count; ++mode) {
            PrKitMenuItem item{.text = kRenderModes[mode], .checked = editor.renderMode == mode};
            if (prKitActivated(kit, prKitMenuItem(kit, modes, item, {})))
                editor.renderMode = mode;
        }
    }
    return button;
}

void buildViewport(PrKit* kit, PrLayoutNode panel, EditorState& editor) {
    // The level fills the viewport, drawn by a painter, and the HUD's layer stands over it.
    PrLayoutTree* tree = kit->tree;
    PrKitBoxStyle stack{.overflow = PrKitOverflow_Clip};
    stack.place.flex = PrKitFlex_Fill;
    PrLayoutNode viewport = prKitStack(kit, panel, {}, stack);
    PrLayoutNode picture = prLayoutTreeFrameReplaced(tree, viewport, 0, 0);
    prKitPlace(kit, picture, {.flex = PrKitFlex_Fill});
    PrLayoutContent content{};
    content.kind = PrLayoutContentKind_Painter;
    content.painter.paint = paintViewport;
    content.painter.context = &editor;
    prLayoutTreeFrameSetContent(tree, picture, &content);
    prUiSetFlags(kit->ui, picture, PrUiFlag_Pressable);
    pickEntity(kit, picture, editor);

    // The HUD is a library of the editor's own that shares the tree and the context with the kit. Its panels hold kit
    // widgets beside its own, a kit menu opens from one of them, and kit tooltips anchor to its switches.
    Hud* hud = editor.hud;
    PrLayoutNode layer = hudLayer(hud, viewport, {});
    PrLayoutNode view = hudPanel(hud, layer, {.corner = HudCorner_TopLeft});
    hudReadout(hud, view, {.label = "VIEW", .value = "TOP"});
    hudReadout(hud, view, {.label = "CELL", .value = "1 m"});
    PrLayoutNode modes = hudPanel(hud, layer, {.corner = HudCorner_TopRight, .row = true});
    hudReadout(hud, modes, {.label = "MODE"});
    hudHint(hud, renderModeButton(kit, modes, editor), "How the viewport draws the level");
    PrLayoutNode status = hudPanel(hud, layer, {.corner = HudCorner_BottomLeft, .row = true});
    const Entity* selected = editor.find(editor.selected);
    hudReadout(hud, status, {.label = "SEL", .value = selected != nullptr ? PrStringView(selected->name) : "none"});
    char at[48];
    PrStringView position = "-";
    if (selected != nullptr)
        position = formatToBuffer(Span(at), "{:.1f}, {:.1f}", selected->position.x, selected->position.z);
    hudReadout(hud, status, {.label = "AT", .value = position});
    // The switches bind what the toolbar's buttons bind, and their gestures reach the editor's edit sink.
    prKitTooltip(kit, hudSwitch(hud, status, {.label = "GRID", .value = &editor.grid}),
        {.text = "Show the grid", .shortcut = "Cmd '"});
    prKitTooltip(kit, hudSwitch(hud, status, {.label = "SNAP", .value = &editor.snap}),
        {.text = "Snap to the grid", .shortcut = "Cmd ;"});
}

// ---------------------------------------------------------------------------------------------------------------------
// Console

void buildConsole(PrKit* kit, PrLayoutNode panel, EditorState& editor) {
    PrLayoutNode actions = actionBar(kit, panel);
    uint32_t counts[3] = {};
    for (const LogEntry& log : editor.logs)
        ++counts[log.level == PrKitLogLevel_Error ? 2 : log.level == PrKitLogLevel_Warning ? 1 : 0];
    struct LevelDef {
        const char* name;
        bool* shown;
    };
    LevelDef levels[] = {
        {"Info", &editor.showInfo}, {"Warnings", &editor.showWarnings}, {"Errors", &editor.showErrors},
    };
    for (uint32_t i = 0; i < 3; ++i) {
        char label[32];
        PrStringView text = formatToBuffer(Span(label), "{} {}", levels[i].name, counts[i]);
        if (prKitActivated(kit, prKitButton(kit, actions, {.text = text, .selected = *levels[i].shown}, kGhostXs)))
            *levels[i].shown = !*levels[i].shown;
    }
    prKitSpacer(kit, actions, {});
    if (prKitActivated(kit, prKitButton(kit, actions, {.text = "Clear"}, kGhostXs)))
        editor.clearLogs();
    PrLayoutNode lines = scrolling(kit, panel, prKitEdgesXY(kPrKitSpace2, 0));
    for (const LogEntry& log : editor.logs) {
        if (editor.shows(log.level))
            prKitLogLine(kit, lines,
                {.time = log.time, .message = log.message, .source = log.source, .level = log.level});
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Inspector

void buildInspector(PrKit* kit, PrLayoutNode panel, EditorState& editor) {
    Entity* entity = editor.find(editor.inspectorLocked ? editor.lockedEntity : editor.selected);
    PrLayoutNode identity = actionBar(kit, panel);
    if (entity != nullptr)
        prKitCheckbox(kit, identity, {.value = &entity->enabled}, {});
    prKitIcon(kit, identity, {.icon = PrKitIconKind(entity != nullptr ? entity->icon : PrKitIconKind_Cube)},
        {.size = 14, .tone = PrKitTone(entity != nullptr ? PrKitTone_Secondary : PrKitTone_Faint)});
    prKitLabel(kit, identity, {.text = entity != nullptr ? PrStringView(entity->name) : "No selection"}, kBody);
    prKitSpacer(kit, identity, {});
    if (entity != nullptr)
        prKitBadge(kit, identity, {.text = entity->type}, {.variant = PrKitBadgeVariant_Accent});
    PrKitIconKind lock = PrKitIconKind(editor.inspectorLocked ? PrKitIconKind_Lock : PrKitIconKind_Unlock);
    if (iconButton(kit, identity, lock, editor.inspectorLocked, kGhostXs)) {
        editor.inspectorLocked = !editor.inspectorLocked;
        editor.lockedEntity = editor.inspectorLocked && entity != nullptr ? entity->id : 0;
    }

    PrLayoutNode body = scrolling(kit, panel);
    if (entity == nullptr)
        return;
    PrLayoutNode transform = prKitInspectorGroup(kit, body, {.name = "Transform", .open = &editor.openTransform});
    if (transform != kPrLayoutNodeNone) {
        PrKitNumberRange meters{.step = editor.snap ? 0.5 : 0.05};
        prKitVectorField(kit, prKitInspectorRow(kit, transform, {.label = "Position"}, {}),
            {.value = prKitScalarF32(&entity->position.x), .count = 3, .range = meters}, {});
        prKitVectorField(kit, prKitInspectorRow(kit, transform, {.label = "Rotation"}, {}),
            {.value = prKitScalarF32(&entity->rotation.x), .count = 3, .range = {.step = 0.5}}, {});
        PrKitNumberRange scale{.step = 0.01, .min = 0, .hasMin = true};
        prKitVectorField(kit, prKitInspectorRow(kit, transform, {.label = "Scale"}, {}),
            {.value = prKitScalarF32(&entity->scale.x), .count = 3, .range = scale}, {});
    }
    PrLayoutNode mesh = prKitInspectorGroup(kit, body, {.name = "Mesh Renderer", .open = &editor.openMesh});
    if (mesh != kPrLayoutNodeNone) {
        PrKitAssetField field{.name = entity->mesh, .icon = PrKitIconKind_Cube, .kind = kDragAsset};
        PrLayoutNode asset = prKitAssetField(kit, prKitInspectorRow(kit, mesh, {.label = "Mesh"}, {}), field);
        uint64_t dropped = 0;
        if (prKitDropped(kit, asset, &dropped) && dropped < countOf(kAssets))
            entity->mesh = format("{}.mesh", kAssets[dropped].name);
        prKitColorField(kit, prKitInspectorRow(kit, mesh, {.label = "Tint"}, {}), {.color = &editor.tint}, {});
        const char* lighting[] = {"realtime", "mixed", "baked"};
        segmented(kit, prKitInspectorRow(kit, mesh, {.label = "Lighting"}, {}), lighting, 3, &editor.lighting);
        PrLayoutNode shadows = prKitInspectorRow(kit, mesh, {.label = "Shadows"}, {});
        prKitCheckbox(kit, shadows, {.value = &entity->castShadows, .label = "cast"}, {});
        prKitCheckbox(kit, shadows, {.value = &entity->receiveShadows, .label = "receive"}, {});
    }
    PrLayoutNode rigidbody = prKitInspectorGroup(kit, body, {.name = "Rigidbody", .open = &editor.openRigidbody});
    if (rigidbody != kPrLayoutNodeNone) {
        PrKitNumber mass{
            .value = prKitScalarF32(&entity->mass),
            .range = {.step = 0.05, .min = 0, .hasMin = true},
            .unit = "kg",
        };
        prKitNumberField(kit, prKitInspectorRow(kit, rigidbody, {.label = "Mass"}, {}), mass, {});
        PrLayoutNode drag = prKitInspectorRow(kit, rigidbody, {.label = "Drag"}, {});
        prKitSlider(kit, drag, {.value = prKitScalarF32(&editor.drag), .min = 0, .max = 1}, {});
        PrKitNumberRange unit{.step = 0.005, .min = 0, .max = 1, .hasMin = true, .hasMax = true};
        PrLayoutNode number = prKitNumberField(kit, drag, {.value = prKitScalarF32(&editor.drag), .range = unit}, {});
        prKitPlace(kit, number, {.width = prKitPx(56), .flex = PrKitFlex_Fixed});
        prKitToggle(kit, prKitInspectorRow(kit, rigidbody, {.label = "Gravity"}, {}), {.value = &entity->gravity}, {});
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
        prKitNumberField(kit, prKitInspectorRow(kit, script, {.label = "Speed"}, {}),
            {.value = prKitScalarF32(&editor.scriptSpeed), .range = positive}, {});
        prKitNumberField(kit, prKitInspectorRow(kit, script, {.label = "Jump"}, {}),
            {.value = prKitScalarF32(&editor.scriptJump), .range = positive}, {});
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// The dock

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
    return PrKitDockTab{.title = panel < countOf(kTitles) ? kTitles[panel] : ""};
}

// Makes the dock's layout: the scene over the project, the viewport over the console, and the inspector.
PrKitDock* makeDock() {
    PrKitDock* dock = prKitDockCreate();
    PrKitDockNode scene = prKitDockPrimary(dock);
    prKitDockAddPanel(dock, scene, Panel_Scene);
    PrKitDockNode viewport = prKitDockSplit(dock, scene, PrKitDockEdge_Right, Panel_Viewport, 0.80f);
    prKitDockSplit(dock, viewport, PrKitDockEdge_Right, Panel_Inspector, 0.30f);
    prKitDockSplit(dock, scene, PrKitDockEdge_Bottom, Panel_Project, 0.36f);
    prKitDockSplit(dock, viewport, PrKitDockEdge_Bottom, Panel_Console, 0.28f);
    return dock;
}

// Counts the edit gestures that the editor's controls commit, whichever library made each control.
void countEdit(void* context, PrLayoutNode, PrUiEditPhase phase) {
    if (phase == PrUiEditPhase_Commit)
        ++static_cast<EditorState*>(context)->edits;
}

} // namespace

void buildEditor(PrKit* kit, PrLayoutNode root, EditorState& editor) {
    prUiSetEditSink(kit->ui, root, countEdit, &editor);
    buildMenuBar(kit, root, editor);
    buildToolbar(kit, root, editor);
    if (editor.dock == nullptr)
        editor.dock = makeDock();
    PrLayoutNode dock = prKitDockBuild(kit, root, {
        .key = prUiKey(kit->ui, root, "dock"),
        .dock = editor.dock,
        .describe = describePanel,
    });
    prKitPlace(kit, dock, {.flex = PrKitFlex_Fill});
    for (uint32_t i = 0; i < prKitDockPaneCount(editor.dock); ++i) {
        PrKitDockPane pane = prKitDockPane(editor.dock, i);
        switch (pane.panel) {
            case Panel_Scene: buildScenePanel(kit, pane.content, editor); break;
            case Panel_Viewport: buildViewport(kit, pane.content, editor); break;
            case Panel_Inspector: buildInspector(kit, pane.content, editor); break;
            case Panel_Project: buildProjectPanel(kit, pane.content, editor); break;
            case Panel_Console: buildConsole(kit, pane.content, editor); break;
            default: break;
        }
    }
    buildStatusBar(kit, root, editor);
    buildPalette(kit, prLayoutTreeRoot(kit->tree), editor);
}

} // namespace editor
