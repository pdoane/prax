// The gallery's components: the tree, the inspector, the toolbar, tables, and tags and badges.

#include "examples/studio_gallery/section_components.h"

#include "pr/assert.h"
#include "pr/count_of.h"
#include "pr/kit_button.h"
#include "pr/kit_choice.h"
#include "pr/kit_collection.h"
#include "pr/kit_color.h"
#include "pr/kit_icon.h"
#include "pr/kit_input.h"
#include "pr/kit_menu.h"
#include "pr/ui.h"

#include "examples/studio_gallery/gallery_parts.h"

namespace gallery {

using namespace pr;

namespace {

// The kinds of the payloads that the tree's rows and the asset chips carry.
constexpr uint64_t kDragTreeRow = 1;
constexpr uint64_t kDragAssetChip = 2;

// The distance in px that a press moves before it picks up a chip.
constexpr float kDragThreshold = 4;

// An asset that the inspector's mesh field takes.
struct AssetChip {
    const char* name;
    PrKitIconKind icon;
};

constexpr AssetChip kAssetChips[] = {
    {"player_base.mesh", PrKitIconKind_Cube},
    {"enemy_grunt.mesh", PrKitIconKind_Cube},
    {"sky_dome.mesh", PrKitIconKind_Sphere},
    {"terrain_patch.mesh", PrKitIconKind_Terrain},
};

// The labels that the chip list adds, in turn.
constexpr const char* kChipPool[] = {"lod0", "static", "baked", "nav", "trigger", "decal"};

// Creates a row of `gap`, centered across.
PrLayoutNode specimenRow(PrKit* kit, PrLayoutNode parent, float gap, bool wrap) {
    PrKitBoxStyle row{.direction = PrKitDirection_Row, .align = PrKitAlign_Center, .wrap = wrap, .gap = gap};
    return prKitBox(kit, parent, {}, row);
}

// Creates the cells of an asset's row: its name by its icon, its type, its size and when it was modified.
void assetCells(PrKit* kit, PrLayoutNode row, const AssetRow& asset) {
    prKitTableCell(kit, row, {.column = 0, .text = asset.name, .icon = asset.icon, .role = PrKitTextRole_BodySmall});
    prKitTableCell(kit, row, {
        .column = 1,
        .text = asset.type,
        .role = PrKitTextRole_Mono,
        .tone = PrKitTone_Secondary,
    });
    prKitTableCell(kit, row, {
        .column = 2,
        .text = asset.sizeText,
        .role = PrKitTextRole_Mono,
        .tone = PrKitTone_Secondary,
    });
    prKitTableCell(kit, row, {
        .column = 3,
        .text = asset.modified,
        .role = PrKitTextRole_Mono,
        .tone = PrKitTone_Muted,
    });
}

} // namespace

void sectionTree(PrKit* kit, PrLayoutNode parent, GalleryState& state) {
    TreeState& tree = state.tree;
    PrLayoutNode section = sectionFrame(kit, parent, "10", "Tree View", "Expand, select, hide, drag to reparent.",
        "Click a row and Up and Down walk the tree, Left and Right fold and unfold it. A row carries an "
            "optional status in place of its glyph, and a drag lands before, after or into the row under the pointer.");
    PrLayoutNode panel = demoPanel(kit, section, "SCENE HIERARCHY", "LIVE . DRAG TO REPARENT", true);
    treeOrder(tree);
    PrKitTreeView content{
        .count = tree.rows.count(),
        .selection = &tree.cursor,
        .items = tree.rows.data(),
        .dragKind = kDragTreeRow,
    };
    PrLayoutNode view =
        prKitTreeView(kit, panel, content, {.place = {.height = prKitPx(300), .flex = PrKitFlex_Fixed}});
    PrUi* ui = kit->ui;
    PrKitWindow window = prKitListWindow(kit, view);
    for (uint32_t i = window.first; i < window.first + window.count; ++i) {
        uint32_t key = tree.keys[i];
        TreeNode* node = findTreeNode(tree, key);
        PrLayoutNode rowNode = prKitTreeRow(kit, view, {.index = i, .item = key, .selected = tree.selected == key});
        // A node with a status shows it in place of its icon.
        if (!node->badge.isEmpty())
            prKitStatus(kit, rowNode, {.text = node->badge}, {.tone = node->badgeTone});
        else
            prKitRowIcon(kit, rowNode, {.icon = node->icon});
        prKitRowLabel(kit, rowNode, {.text = node->name},
            PrKitTone(node->hidden ? PrKitTone_Faint : PrKitTone_Primary));
        if (!node->meta.isEmpty())
            prKitRowMeta(kit, rowNode, {.text = node->meta});
        PrKitVisibilityToggle toggle{.hidden = node->hidden, .revealed = prUiIsHovered(ui, rowNode)};
        if (prKitActivated(kit, prKitVisibilityToggle(kit, rowNode, toggle)))
            node->hidden = !node->hidden;
    }
    PrKitTreeFold fold{};
    if (prKitFolded(kit, view, &fold)) {
        if (TreeNode* node = findTreeNode(tree, tree.keys[fold.row]))
            node->open = fold.open;
    }
    PrKitSelectionEdit edit{};
    if (prKitSelectionEdited(kit, view, &edit) && tree.cursor.lead < tree.keys.count())
        tree.selected = tree.keys[tree.cursor.lead];
    PrKitTreeDrop drop{};
    if (prKitRowDropped(kit, view, &drop) && drop.row < tree.keys.count()) {
        TreePlacement placement = TreePlacement_Into;
        if (drop.zone == PrKitDropZone_Before)
            placement = TreePlacement_Before;
        else if (drop.zone == PrKitDropZone_After)
            placement = TreePlacement_After;
        moveTreeNode(tree, uint32_t(drop.item), tree.keys[drop.row], placement);
    }
    const PrUiPayload* carried = prUiDragging(ui);
    if (carried != nullptr && carried->kind == kDragTreeRow) {
        if (TreeNode* node = findTreeNode(tree, uint32_t(carried->value))) {
            PrKitDragGhost ghost{.text = node->name, .icon = node->icon};
            prKitDragGhost(kit, ghost);
        }
    }
}

void sectionInspector(PrKit* kit, PrLayoutNode parent, GalleryState& state) {
    InspectorState& inspector = state.inspector;
    PrLayoutNode section = sectionFrame(kit, parent, "11", "Inspector Rows", "The workhorse of the property editor.",
        "An inspector row is a two-column grid: a label column and the controls that take the width it leaves. "
            "A group header folds its rows away, and a numeric field scrubs under a drag.");
    PrLayoutNode panel = demoPanel(kit, section, "ENTITY PROPERTIES", "ENTITY . PLAYER", true);
    {
        PrKitPanelStyle bar{.rules = PrKitRuleFlag_Bottom};
        bar.box.direction = PrKitDirection_Row;
        bar.box.align = PrKitAlign_Center;
        bar.box.gap = kPrKitSpace3;
        bar.box.padding = prKitEdgesXY(kPrKitSpace4, kPrKitSpace5);
        bar.box.place.flex = PrKitFlex_Fixed;
        PrLayoutNode identity = prKitPanel(kit, panel, {}, bar);
        prKitIcon(kit, identity, {.icon = PrKitIconKind_Cube}, {.size = 14, .tone = PrKitTone_Secondary});
        PrKitTextField name{.text = inspector.name};
        PrKitFieldStyle small{.size = PrKitControlSize_Sm};
        PrStringView text{};
        if (prKitTextChanged(kit, prKitTextField(kit, identity, name, small), &text))
            inspector.name = text;
        prKitBadge(kit, identity, {.text = "ENTITY"}, {.variant = PrKitBadgeVariant_Accent});
        prKitBadge(kit, identity, {.text = "0x7FF8_A2B0"}, {.variant = PrKitBadgeVariant_Neutral});
    }
    PrKitInspectorGroup transformGroup{
        .name = "Transform",
        .badge = "CORE",
        .badgeVariant = PrKitBadgeVariant_Accent,
        .open = &inspector.openTransform,
    };
    PrLayoutNode transform = prKitInspectorGroup(kit, panel, transformGroup);
    if (transform != kPrLayoutNodeNone) {
        PrKitNumberRange fine{.step = 0.05};
        PrLayoutNode position = prKitInspectorRow(kit, transform, {.label = "Position"}, {});
        prKitVectorField(kit, position, {.value = prKitScalarF32(inspector.position), .count = 3, .range = fine}, {});
        PrKitVector rotation{
            .value = prKitScalarF32(inspector.rotation),
            .count = 3,
            .range = {.step = 0.5},
            .precision = 1,
        };
        prKitVectorField(kit, prKitInspectorRow(kit, transform, {.label = "Rotation"}, {}), rotation, {});
        PrLayoutNode scale = prKitInspectorRow(kit, transform, {.label = "Scale"}, {});
        PrKitNumberRange positive{.step = 0.01, .min = 0, .hasMin = true};
        prKitVectorField(kit, scale, {.value = prKitScalarF32(inspector.scale), .count = 3, .range = positive}, {});
        prKitIcon(kit, scale, {.icon = PrKitIconKind_Link}, {.size = 11, .tone = PrKitTone_Muted});
    }
    PrKitInspectorGroup meshGroup{.name = "Mesh Renderer", .open = &inspector.openMesh};
    PrLayoutNode mesh = prKitInspectorGroup(kit, panel, meshGroup);
    if (mesh != kPrLayoutNodeNone) {
        PrKitAssetField meshField{.name = inspector.meshRef, .icon = inspector.meshIcon, .kind = kDragAssetChip};
        PrLayoutNode slot = prKitAssetField(kit, prKitInspectorRow(kit, mesh, {.label = "Mesh"}, {}), meshField);
        uint64_t dropped = 0;
        if (prKitDropped(kit, slot, &dropped) && dropped < countOf(kAssetChips)) {
            inspector.meshRef = kAssetChips[dropped].name;
            inspector.meshIcon = kAssetChips[dropped].icon;
        }
        PrKitAssetField material{.name = "player_pbr", .tint = inspector.tint};
        prKitAssetField(kit, prKitInspectorRow(kit, mesh, {.label = "Material"}, {}), material);
        prKitColorField(kit, prKitInspectorRow(kit, mesh, {.label = "Tint"}, {}), {.color = &inspector.tint}, {});
        prKitColorField(kit, prKitInspectorRow(kit, mesh, {.label = "Emissive"}, {}), {.color = &inspector.emissive}, {});
        prKitCheckbox(kit, prKitInspectorRow(kit, mesh, {.label = "Cast Shadows"}, {}),
            {.value = &inspector.castShadows}, {});
        prKitCheckbox(kit, prKitInspectorRow(kit, mesh, {.label = "Receive Shadows"}, {}),
            {.value = &inspector.receiveShadows}, {});
        const PrKitChoice lighting[] = {{.label = "realtime"}, {.label = "mixed"}, {.label = "baked"}};
        segmented(kit, prKitInspectorRow(kit, mesh, {.label = "Lighting"}, {}), lighting, 3, &inspector.lighting,
            {.size = PrKitControlSize_Sm});
    }
    PrKitInspectorGroup bodyGroup{.name = "Rigidbody", .open = &inspector.openRigidbody};
    PrLayoutNode rigidbody = prKitInspectorGroup(kit, panel, bodyGroup);
    if (rigidbody != kPrLayoutNodeNone) {
        PrKitNumber mass{
            .value = prKitScalarF32(&inspector.mass),
            .range = {.step = 0.1, .min = 0, .hasMin = true},
            .unit = "kg",
            .precision = 1,
        };
        prKitNumberField(kit, prKitInspectorRow(kit, rigidbody, {.label = "Mass"}, {}), mass, {});
        PrLayoutNode drag = prKitInspectorRow(kit, rigidbody, {.label = "Drag"}, {});
        prKitSlider(kit, drag, {.value = prKitScalarF32(&inspector.drag), .min = 0, .max = 1}, {});
        PrKitNumberRange unit{.step = 0.005, .min = 0, .max = 1, .hasMin = true, .hasMax = true};
        PrLayoutNode field = prKitNumberField(kit, drag, {.value = prKitScalarF32(&inspector.drag), .range = unit}, {});
        PrKitPlace fixed{.width = prKitPx(72), .flex = PrKitFlex_Fixed};
        prKitPlace(kit, field, &fixed);
        prKitToggle(kit, prKitInspectorRow(kit, rigidbody, {.label = "Use Gravity"}, {}),
            {.value = &inspector.useGravity}, {});
    }
    {
        PrKitPanelStyle foot{.rules = PrKitRuleFlag_Top};
        foot.box.direction = PrKitDirection_Row;
        foot.box.padding = prKitEdgesAll(kPrKitSpace4);
        foot.box.place.flex = PrKitFlex_Fixed;
        PrLayoutNode add = prKitPanel(kit, panel, {}, foot);
        PrKitButton content{.text = "Add Component", .icon = PrKitIconKind_Plus};
        PrKitButtonStyle small{.size = PrKitControlSize_Sm};
        PrLayoutNode button = prKitButton(kit, add, content, small);
        PrKitPlace fill{.flex = PrKitFlex_Fill};
        prKitPlace(kit, button, &fill);
    }

    // Monospaced labels set telemetry keys apart from prose.
    PrLayoutNode telemetry = demoPanel(kit, section, "TELEMETRY ROWS", "MONO LABELS", true);
    prKitPanelHeader(kit, telemetry, {.title = "INPUT DEVICE", .meta = "POLL 125 HZ"});
    const char* const keys[] = {"seq", "delta", "buttons", "axis_l"};
    const char* const values[] = {"48213", "0.0021 s", "0x0004", "+0.512 / -0.087"};
    PrKitInspectorRowStyle mono{.mono = true};
    for (uint32_t i = 0; i < 4; ++i)
        prKitLabel(kit, prKitInspectorRow(kit, telemetry, {.label = keys[i]}, mono), {.text = values[i]}, kMono);

    PrLayoutNode references = demoPanel(kit, section, "ASSET REFERENCE", "PICK . LOCATE . CLEAR", false);
    PrKitBoxStyle column{.gap = kPrKitSpace2};
    column.place.maxWidth = prKitPx(460);
    PrLayoutNode list = prKitBox(kit, references, {}, column);
    const PrKitAssetField fields[] = {
        {.name = "player_base.mesh", .icon = PrKitIconKind_Cube},
        {.name = "player_pbr", .tint = inspector.tint},
        {.name = "footsteps_grass_01.ogg", .icon = PrKitIconKind_Audio},
        {.name = "enemy_grunt.mesh", .icon = PrKitIconKind_Alert, .missing = true},
        {.name = "None", .icon = PrKitIconKind_Plus, .empty = true},
    };
    for (const PrKitAssetField& field : fields)
        prKitAssetField(kit, list, field);

    PrLayoutNode chips = demoPanel(kit, section, "ASSET DRAG", "DROP ON THE MESH SLOT", false);
    PrLayoutNode row = specimenRow(kit, chips, kPrKitSpace2, true);
    for (uint32_t i = 0; i < countOf(kAssetChips); ++i) {
        PrKitButton content{.text = kAssetChips[i].name, .icon = kAssetChips[i].icon};
        PrKitButtonStyle small{.size = PrKitControlSize_Sm};
        PrLayoutNode chip = prKitButton(kit, row, content, small);
        prUiDragStart(kit->ui, chip, kDragThreshold, kDragAssetChip, i, 0);
    }
    const PrUiPayload* carried = prUiDragging(kit->ui);
    if (carried != nullptr && carried->kind == kDragAssetChip && carried->value < countOf(kAssetChips)) {
        PrKitDragGhost ghost{.text = kAssetChips[carried->value].name, .icon = kAssetChips[carried->value].icon};
        prKitDragGhost(kit, ghost);
    }
}

void sectionToolbar(PrKit* kit, PrLayoutNode parent, GalleryState& state) {
    ToolbarState& toolbar = state.toolbar;
    PrLayoutNode section = sectionFrame(kit, parent, "12", "Toolbar", "The tool row and the transport beside it.", {});
    PrLayoutNode panel = demoPanel(kit, section, "VIEWPORT TOOLBAR", "MAIN", true);
    PrKitPanelStyle strip{.background = PrKitBackground_Panel};
    strip.box.direction = PrKitDirection_Row;
    strip.box.align = PrKitAlign_Center;
    strip.box.gap = kPrKitSpace1;
    strip.box.padding = prKitEdgesXY(0, kPrKitSpace3);
    strip.box.place.height = prKitPx(kPrKitHeightLg);
    strip.box.place.flex = PrKitFlex_Fixed;
    PrLayoutNode bar = prKitPanel(kit, panel, {}, strip);
    const char* const tips[] = {"Select", "Move", "Rotate", "Scale"};
    const PrKitIconKind tools[] = {PrKitIconKind_Cursor, PrKitIconKind_Move, PrKitIconKind_Rotate, PrKitIconKind_Scale};
    for (uint32_t i = 0; i < 4; ++i) {
        PrLayoutNode tool = prKitButton(kit, bar, {.icon = tools[i], .selected = i == toolbar.tool},
            {.variant = PrKitButtonVariant_Ghost, .size = PrKitControlSize_Md});
        PrKitTooltip tip{.text = tips[i]};
        prKitTooltip(kit, tool, tip);
        if (prKitActivated(kit, tool))
            toolbar.tool = i;
    }
    prKitDivider(kit, bar, {});
    const PrKitChoice spaces[] = {{.label = "Local"}, {.label = "World"}};
    segmented(kit, bar, spaces, 2, &toolbar.space, {.size = PrKitControlSize_Sm});
    prKitDivider(kit, bar, {});
    if (prKitActivated(kit, prKitButton(kit, bar, {.icon = PrKitIconKind_Magnet, .selected = toolbar.snap},
            {.variant = PrKitButtonVariant_Ghost, .size = PrKitControlSize_Md})))
        toolbar.snap = !toolbar.snap;
    if (prKitActivated(kit, prKitButton(kit, bar, {.icon = PrKitIconKind_Grid, .selected = toolbar.grid},
            {.variant = PrKitButtonVariant_Ghost, .size = PrKitControlSize_Md})))
        toolbar.grid = !toolbar.grid;
    prKitLabel(kit, bar, {.text = "0.25m"}, kCaption);
    prKitSpacer(kit, bar, {});
    PrKitIconKind play = toolbar.playing ? PrKitIconKind_Pause : PrKitIconKind_Play;
    if (prKitActivated(kit, prKitButton(kit, bar, {.icon = play, .selected = toolbar.playing},
            {.variant = PrKitButtonVariant_Ghost, .size = PrKitControlSize_Md})))
        toolbar.playing = !toolbar.playing;
    prKitButton(kit, bar, {.icon = PrKitIconKind_Step},
        {.variant = PrKitButtonVariant_Ghost, .size = PrKitControlSize_Md});
    if (prKitActivated(kit, prKitButton(kit, bar, {.icon = PrKitIconKind_Stop},
            {.variant = PrKitButtonVariant_Ghost, .size = PrKitControlSize_Md})))
        toolbar.playing = false;

    PrLayoutNode sizes = demoPanel(kit, section, "CELL SIZES", "XS . SM . MD . LG", false);
    PrKitBoxStyle line{.direction = PrKitDirection_Row, .align = PrKitAlign_End, .gap = kPrKitSpace8};
    PrLayoutNode row = prKitBox(kit, sizes, {}, line);
    // Each cell is shown selected, so that its background shows its size.
    const PrKitControlSize steps[] = {
        PrKitControlSize_Xs, PrKitControlSize_Sm, PrKitControlSize_Md,
        PrKitControlSize_Lg,
    };
    const char* const names[] = {"XS 18", "SM 22", "MD 26", "LG 30"};
    for (uint32_t i = 0; i < 4; ++i) {
        PrKitBoxStyle cell{.align = PrKitAlign_Center, .gap = kPrKitSpace2};
        PrLayoutNode column = prKitBox(kit, row, {}, cell);
        prKitButton(kit, column, {.icon = PrKitIconKind_Magnet, .selected = true},
            {.variant = PrKitButtonVariant_Ghost, .size = steps[i]});
        prKitLabel(kit, column, {.text = names[i]}, kCaption);
    }
}

void sectionTables(PrKit* kit, PrLayoutNode parent, GalleryState& state) {
    TableState& table = state.table;
    PrLayoutNode section = sectionFrame(kit, parent, "13", "Tables", "Dense rows: a header that sorts, rows that select.",
        "Click a heading to sort by its column and again to reverse it; click, Shift-click and Cmd-click rows "
            "to select them. The name column fills the room that the fixed columns leave.");
    uint32_t count = 0;
    const AssetRow* assets = galleryAssets(&count);
    const PrKitTableColumn columns[] = {
        {.label = "NAME", .width = 140, .flex = PrKitFlex_Fill, .sortable = true},
        {.label = "TYPE", .width = 110, .sortable = true},
        {.label = "SIZE", .width = 90, .align = PrKitTextAlign_End, .sortable = true},
        {.label = "MODIFIED", .width = 110, .sortable = true},
    };
    {
        char tags[16];
        PrLayoutNode panel = demoPanel(kit, section, "ASSET BROWSER", formatToBuffer(Span(tags), "{} ROWS", count), true);
        PrKitTable content{
            .count = count,
            .selection = &table.cursor,
            .columns = columns,
            .columnCount = countOf(columns),
            .sort = &table.sort,
        };
        PrLayoutNode view =
            prKitTable(kit, panel, content, {.place = {.maxHeight = prKitPx(260), .flex = PrKitFlex_Fixed}});
        uint32_t order[16];
        PR_ASSERT(count <= countOf(order));
        sortAssetOrder(assets, count, AssetSort(table.sort.column), !table.sort.descending, order);
        PrKitWindow window = prKitListWindow(kit, view);
        for (uint32_t i = window.first; i < window.first + window.count; ++i) {
            const AssetRow& asset = assets[order[i]];
            PrKitTableRow row{.index = i, .item = order[i], .selected = selectionHolds(table.selection, order[i])};
            assetCells(kit, prKitTableRow(kit, view, row), asset);
        }
        PrKitSelectionEdit edit{};
        if (prKitSelectionEdited(kit, view, &edit))
            selectionApply(table.selection, edit, order);
    }
    {
        // Fixed columns wider than the table: it scrolls in both axes, and the header follows the columns sideways
        // while it stays over the rows.
        const PrKitTableColumn wide[] = {
            {.label = "NAME", .width = 220},
            {.label = "TYPE", .width = 140},
            {.label = "SIZE", .width = 120, .align = PrKitTextAlign_End},
            {.label = "MODIFIED", .width = 160},
        };
        // The panel is as wide as the table, which is narrower than its columns.
        PrKitBoxStyle fit{.direction = PrKitDirection_Row, .align = PrKitAlign_Start};
        PrLayoutNode panel = demoPanel(kit, prKitBox(kit, section, {}, fit), "OVERFLOWING COLUMNS", "SCROLLS BOTH AXES", true);
        PrKitListViewStyle style{.place = {.height = prKitPx(110), .maxWidth = prKitPx(380), .flex = PrKitFlex_Fixed}};
        PrLayoutNode view = prKitTable(kit, panel, {.count = count, .columns = wide, .columnCount = countOf(wide)}, style);
        PrKitWindow window = prKitListWindow(kit, view);
        for (uint32_t i = window.first; i < window.first + window.count; ++i)
            assetCells(kit, prKitTableRow(kit, view, {.index = i, .item = i}), assets[i]);
    }
}

void sectionTags(PrKit* kit, PrLayoutNode parent, GalleryState& state) {
    PrLayoutNode section = sectionFrame(kit, parent, "14", "Tags & Badges", "Small metadata, clearly typed.", {});
    {
        PrLayoutNode panel = demoPanel(kit, section, "BADGES", "STATIC LABEL", false);
        PrLayoutNode row = specimenRow(kit, panel, kPrKitSpace3, true);
        prKitBadge(kit, row, {.text = "DEFAULT"}, {.variant = PrKitBadgeVariant_Neutral});
        prKitBadge(kit, row, {.text = "ACCENT"}, {.variant = PrKitBadgeVariant_Accent});
        prKitBadge(kit, row, {.text = "BUILT"}, {.variant = PrKitBadgeVariant_Success});
        prKitBadge(kit, row, {.text = "PENDING"}, {.variant = PrKitBadgeVariant_Warning});
        prKitBadge(kit, row, {.text = "ERROR"}, {.variant = PrKitBadgeVariant_Danger});
        prKitBadge(kit, row, {.text = "v0.1.0"}, {.variant = PrKitBadgeVariant_Neutral});
        prKitBadge(kit, row, {.text = "0x7FF8_A2B0"}, {.variant = PrKitBadgeVariant_Neutral});
        PrLayoutNode statuses = specimenRow(kit, panel, kPrKitSpace3, false);
        prKitStatus(kit, statuses, {.text = "M"}, {.tone = PrKitTone_Warning});
        prKitStatus(kit, statuses, {.text = "A"}, {.tone = PrKitTone_Success});
        prKitStatus(kit, statuses, {.text = "D"}, {.tone = PrKitTone_Danger});
        prKitStatus(kit, statuses, {.text = "R"}, {.tone = PrKitTone_Accent});
        prKitLabel(kit, statuses, {.text = "the one or two character status a row wears"}, kBodySecondary);
    }
    {
        PrLayoutNode panel = demoPanel(kit, section, "KBD", "KEY CHIPS", false);
        PrLayoutNode row = specimenRow(kit, panel, kPrKitSpace3, false);
        const char* const chords[] = {"Cmd", "Shift", "K", "Ret", "Esc", "F2"};
        for (const char* chord : chords)
            prKitKbd(kit, row, {.text = chord});
        prKitLabel(kit, row, {.text = "the key caps that menus and tooltips show"}, kBodySecondary);
    }
    PrLayoutNode panel = demoPanel(kit, section, "CHIPS", "REMOVABLE . READ-ONLY", false);
    PrLayoutNode row = specimenRow(kit, panel, kPrKitSpace3, true);
    uint32_t removeAt = state.chips.count();
    for (uint32_t i = 0; i < state.chips.count(); ++i) {
        if (prKitRemoved(kit, prKitChip(kit, row, {.label = state.chips[i], .removable = true})))
            removeAt = i;
    }
    PrKitButton add{.text = "Tag", .icon = PrKitIconKind_Plus};
    PrKitButtonStyle ghost{.variant = PrKitButtonVariant_Ghost, .size = PrKitControlSize_Xs};
    if (prKitActivated(kit, prKitButton(kit, row, add, ghost)))
        state.chips.push(kChipPool[state.chips.count() % countOf(kChipPool)]);
    if (removeAt < state.chips.count())
        state.chips.erase(removeAt);
    PrLayoutNode readOnly = specimenRow(kit, panel, kPrKitSpace3, true);
    for (const char* label : kChipPool) {
        prKitChip(kit, readOnly, {.label = label});
    }
}

} // namespace gallery
