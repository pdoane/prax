// The Studio Kit gallery's model: the values its live specimens are bound to, the data its tables, trees and
// collections show, and the helpers that edit them.

#pragma once

#include "pr/color.h"
#include "pr/draw_image.h"
#include "pr/kit_collection.h"
#include "pr/kit_graph.h"
#include "pr/kit_icon.h"
#include "pr/kit_text.h"
#include "pr/kit_timeline.h"
#include "pr/layout_tree.h"
#include "pr/string_view.h"

#include <stdint.h>
#include <string>
#include <vector>

PR_ASSUME_NONNULL_BEGIN

namespace gallery {

// The sections of the gallery, in page order. The sidebar lists them, and the content records the node of each.
enum {
    Section_Principles,
    Section_Color,
    Section_Type,
    Section_Space,
    Section_Icons,
    Section_Buttons,
    Section_Inputs,
    Section_Toggles,
    Section_Tabs,
    Section_Menus,
    Section_Tree,
    Section_Inspector,
    Section_Toolbar,
    Section_Tables,
    Section_Tags,
    Section_Progress,
    Section_Tooltips,
    Section_Dialogs,
    Section_Status,
    Section_Collections,
    Section_Timeline,
    Section_Images,
    Section_Graph,
    Section_Keys,
    Section_Count,
};
typedef uint8_t Section;

// The column that the asset table is ordered by.
enum {
    AssetSort_Name,
    AssetSort_Type,
    AssetSort_Size,
    AssetSort_Modified,
};
typedef uint8_t AssetSort;

// A row of the asset table: what it shows, and the keys it sorts by.
struct AssetRow {
    const char* name;
    const char* type;
    const char* sizeText;
    const char* modified;
    uint64_t sizeBytes;
    uint64_t modifiedKey;           // larger is more recent
    PrKitIconKind icon;
};

// A command of the palette.
struct CommandItem {
    const char* label;
    const char* category;
    const char* PR_NULLABLE shortcut;
    PrKitIconKind icon;
};

// A node of the scene tree, flattened in preorder with its depth.
struct TreeNode {
    std::string name;
    std::string meta;
    std::string badge;              // a status shown in place of the icon, or empty
    uint32_t depth = 0;
    uint32_t key = 0;               // the node's identity, which a move keeps
    PrKitIconKind icon = PrKitIconKind_None;
    PrKitTone badgeTone = PrKitTone_Muted;
    bool hasChildren = false;
    bool open = false;
    bool hidden = false;
};

// Where a moved subtree goes relative to the node it is dropped on.
enum {
    TreePlacement_Before,
    TreePlacement_Into,
    TreePlacement_After,
};
typedef uint8_t TreePlacement;

// The flags and the choice that the checkboxes, radios and toggles are bound to.
struct ToggleState {
    uint32_t shading = 2;
    bool castShadows = true;
    bool occlusionCull = false;
    bool children = false;
    bool locked = false;
    bool physics = true;
    bool snap = false;
    bool gizmos = true;
};

// The values that the edit lifecycle panel binds. One undo state is a copy of them.
struct UndoSnapshot {
    float strength = 0.4f;
    int32_t steps = 8;
    bool enabled = true;
};

// The undo history that the edit sink records. A gesture copies the values that it opens on and commits one state if it
// changed them. An edit after an undo drops the states after it.
struct UndoState {
    UndoSnapshot values;
    UndoSnapshot opened;
    std::vector<UndoSnapshot> history;  // oldest first
    uint32_t cursor = 0;                // the state that `values` is
    uint32_t begins = 0;
    uint32_t changes = 0;
    uint32_t commits = 0;
    bool wrote = false;                 // the open gesture changed `values`
};

// The text, numeric, vector and color values of the inputs section.
struct InputsState {
    std::string name = "MainCamera";
    std::string search = "level_01";
    std::string locked = "Locked";
    std::string command;
    std::string area = "lib/gpu";
    std::string revision = "main";
    std::string prose;
    std::string snippet;
    std::vector<std::string> commands;      // submitted lines, oldest first
    float uv[2] = {0.5f, 0.5f};
    float position[3] = {1.234f, 0.0f, -4.5f};
    float rotation[3] = {0.0f, 90.0f, 0.0f};
    float quaternion[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    float scalar = 0.75f;
    float mass = 80.0f;
    float shutter = 0.016f;
    float frequency = 440.0f;
    float shutterSeconds = 1.0f / 60.0f;
    int8_t tiny = -12;
    uint8_t level = 200;
    int16_t offset = -1200;
    uint16_t stride = 40000;
    int32_t subdivisions = 3;
    int64_t budget = -5000000;
    uint32_t seed = 1024;
    uint64_t cycles = 9000000;
    double density = 80.0;
    UndoState undo;
    PrColor tint = {.42f, .55f, .78f, 1.0f};
};

// The property panel that the inspector section edits.
struct InspectorState {
    std::string name = "Player";
    std::string meshRef = "player_base.mesh";
    float position[3] = {1.234f, 0.0f, -4.5f};
    float rotation[3] = {0.0f, 90.0f, 0.0f};
    float scale[3] = {1.0f, 1.0f, 1.0f};
    float mass = 80.0f;
    float drag = 0.08f;
    uint32_t lighting = 0;
    PrColor tint = {.77f, .38f, .24f, 1.0f};
    PrColor emissive = {.24f, .38f, .77f, 1.0f};
    PrKitIconKind meshIcon = PrKitIconKind_Cube;
    bool openTransform = true;
    bool openMesh = true;
    bool openRigidbody = false;
    bool castShadows = true;
    bool receiveShadows = true;
    bool useGravity = true;
};

// The asset table's order and selection.
struct TableState {
    PrKitTableSort sort{.column = AssetSort_Name};
    PrKitSelection cursor{.anchor = 1, .lead = 1};
    std::vector<uint32_t> selection = {1};      // assets, sorted
};

// The command palette's query and selected command.
struct PaletteState {
    std::string query;
    uint32_t active = 0;
};

// What the menu specimens chose.
struct MenuState {
    std::string verb;
    uint32_t component = 0;
    uint32_t clip = 34;
    uint32_t outsideClicks = 0;
    PrColor tag = {.35f, .55f, .82f, 1.0f};
    bool wrap = false;
};

// The tab strip and the segmented controls. `panelOrder` is the order in which the strip shows its tabs, which a drag
// permutes.
struct TabsState {
    uint32_t panelOrder[5] = {0, 1, 2, 3, 4};
    uint32_t panel = 0;
    uint32_t viewMode = 1;
    uint32_t shape = 0;
};

// The viewport toolbar's tool, space and transport.
struct ToolbarState {
    uint32_t tool = 1;
    uint32_t space = 1;
    bool snap = false;
    bool grid = true;
    bool playing = false;
};

// The scene tree, its visible rows in display order, and its selection.
struct TreeState {
    std::vector<TreeNode> nodes;
    std::vector<PrKitTreeItem> rows;    // the visible rows
    std::vector<uint32_t> keys;         // the node of each visible row
    PrKitSelection cursor = {};
    uint32_t selected = 6;
};

// Whether the keys section gives two of its rows one key.
struct KeysState {
    bool duplicate = false;
};

// The confirm dialog's option.
struct DialogState {
    bool deleteOrphans = false;
};

// The values that the progress specimens show.
struct ProgressState {
    float bake = 0.32f;
    float memory = 0.62f;
    float budget = 11.2f;
    bool running = true;
};

// The collection's groups, and the assets in each.
constexpr uint32_t kCollectionGroups = 50;
constexpr uint32_t kCollectionGroupSize = 100;

// A generated asset of the collection.
struct CollectionAsset {
    std::string name;
    std::string sizeText;
    const char* type = "";
    uint64_t sizeBytes = 0;
    PrColor tint = {};
    PrKitIconKind icon = PrKitIconKind_None;
};

// The collection's assets, each view's selection and cursor, and what its context menu did.
struct CollectionsState {
    std::vector<CollectionAsset> assets;
    std::vector<uint32_t> selection;        // assets, sorted
    std::vector<uint32_t> treeSelection;    // tree keys (collectionTreeKey)
    std::vector<PrKitTreeItem> treeRows;
    std::vector<uint32_t> treeKeys;
    std::vector<uint8_t> groupOpen;
    std::string verb;
    PrKitSelection cursor = {};
    PrKitSelection treeCursor = {};
    uint32_t contextIndex = 0;
    uint32_t viewMode = 0;                  // list, grid or tree
};

// The timeline section's clip: its tracks, blocks and markers, the view, the playhead and the loop, the snapping that
// the toolbar turns on, and the undo state that the edit sink records. A gesture copies the blocks that it opens on,
// and one that changed them becomes the state that Undo restores.
struct TimelineState {
    std::vector<PrKitTimelineTrack> tracks;
    std::vector<PrKitTimelineBlock> blocks;
    std::vector<PrKitTimelineMarker> markers;
    std::vector<PrKitTimelineBlock> opened;
    std::vector<PrKitTimelineBlock> undo;
    PrKitTimeSpan view = {0, 8};
    PrKitTimeSpan inspectorView = {0, 8};   // the read-only timeline's own view
    PrKitTimeSpan loop = {2, 6};
    float playhead = 1.5f;
    uint64_t nextId = 1;
    uint32_t begins = 0;
    uint32_t changes = 0;
    uint32_t commits = 0;
    bool playing = false;
    bool snapping = true;
    bool snapEdges = true;
    bool snapMarkers = true;
    bool snapPlayhead = true;
    bool hasUndo = false;
    bool wrote = false;                 // the open gesture changed the blocks
};

// The types of the graph section's ports, which index its colors.
enum : uint32_t {
    PortType_Scalar,
    PortType_Vector,
    PortType_Color,
    PortType_Count,
};
typedef uint32_t PortType;

// The graph section's material: its nodes and wires, and the view.
struct GraphState {
    std::vector<PrKitGraphNode> nodes;
    std::vector<PrKitGraphWire> wires;
    PrKitGraphView view = {{16, 8}, 1};
    uint64_t nextWireId = 1;
};

// The themes that the gallery offers.
enum {
    GalleryTheme_System,
    GalleryTheme_Dark,
    GalleryTheme_Light,
};
typedef uint32_t GalleryTheme;

// The gallery's model and its place on the page. It outlives every frame.
struct GalleryState {
    ToggleState toggles;
    InputsState inputs;
    InspectorState inspector;
    TableState table;
    PaletteState palette;
    MenuState menu;
    TabsState tabs;
    ToolbarState toolbar;
    TreeState tree;
    DialogState dialog;
    KeysState keys;
    ProgressState progress;
    CollectionsState collections;
    std::vector<std::string> chips;
    std::string filter;
    PrColor color = {.42f, .55f, .78f, 1.0f};

    // The colors of the pickers in sRGB, Display P3 and Rec. 2020, each starting at P3's green primary in extended
    // sRGB, which lies past sRGB.
    PrColor gamutColors[3] = {{-0.511f, 1.018f, -0.311f, 1.0f},
                              {-0.511f, 1.018f, -0.311f, 1.0f},
                              {-0.511f, 1.018f, -0.311f, 1.0f}};
    float splitWeights[2] = {1.0f, 1.0f};
    float splitThirds[3] = {1.0f, 2.0f, 1.0f};

    // The theme that the gallery shows the kit in: the user's preferred scheme's, or the dark or light one, in the
    // order of GalleryTheme.
    uint32_t theme = GalleryTheme_System;

    // The node of each section and of the content's scroll container, from the last round, which the sidebar reads the
    // geometry of.
    PrLayoutNode sectionAnchor[Section_Count] = {};
    PrLayoutNode contentView = kPrLayoutNodeNone;
    uint32_t activeSection = 0;
    Section reveal = Section_Count;     // the section that the next round scrolls into view, or Section_Count

    TimelineState timeline;
    GraphState graph;
    const PrDrawImage* PR_NULLABLE picture = nullptr;   // the images section's picture, made on first use

    GalleryState();
    ~GalleryState();
    GalleryState(const GalleryState&) = delete;
    GalleryState& operator=(const GalleryState&) = delete;
};

// Returns the picture that the images section fits into its frames: a landscape twice as wide as it is tall, with a sun
// toward its right, so that each crop shows a different part of it.
const PrDrawImage* galleryPicture(GalleryState& state);

// Returns the asset table's rows, and writes their count.
const AssetRow* galleryAssets(uint32_t* count);

// Writes the indices of `count` assets to `order`, sorted by `column`. Equal keys keep their order.
void sortAssetOrder(const AssetRow* assets, uint32_t count, AssetSort column, bool ascending,
                    uint32_t* order);

// Return the component types, the animation clips and the palette's commands, and write their counts.
const PrStringView* galleryComponentTypes(uint32_t* count);
const PrStringView* galleryAnimationClips(uint32_t* count);
const CommandItem* galleryCommands(uint32_t* count);

// Return the area paths and the revisions that the suggest fields offer, and write their counts.
const PrStringView* galleryAreas(uint32_t* count);
const PrStringView* galleryRevisions(uint32_t* count);

// Appends the command line in flight to the history and empties it. An empty line adds nothing.
void submitCommand(InputsState& inputs);

// Returns true if the command's label or category holds `query`, ignoring case. An empty query matches every command.
bool commandMatches(const CommandItem& item, const std::string& query);

// Begins an edit gesture, copying the values it opens on.
void undoBegin(UndoState& undo);

// Commits a gesture that changed the values, dropping the states after an undo.
void undoCommit(UndoState& undo);

// Return true if there is a state to undo to, or to redo to.
bool undoAvailable(const UndoState& undo);
bool redoAvailable(const UndoState& undo);

// Steps the history one state back, or forward if `forward` is true, and writes that state to the values. Returns false
// if there is no state to step to.
bool undoStep(UndoState& undo, bool forward);

// Moves a tab in the strip's order, keeping the chosen tab chosen.
void moveTab(TabsState& tabs, uint32_t from, uint32_t to);

// Returns the node of `key`, or null.
TreeNode* PR_NULLABLE findTreeNode(TreeState& tree, uint32_t key);

// Moves the node of `key`, with its subtree, relative to the node of `targetKey`. A move onto itself or into its own
// subtree does nothing.
void moveTreeNode(TreeState& tree, uint32_t key, uint32_t targetKey, TreePlacement placement);

// Finds the scene tree's visible rows: every node whose ancestors are open.
void treeOrder(TreeState& tree);

// Returns true if `selection` holds `value`.
bool selectionHolds(const std::vector<uint32_t>& selection, uint32_t value);

// Applies a view's edit to a selection of values. `keys` gives the value of each row, or is null if a row's index is
// its value.
void selectionApply(std::vector<uint32_t>& selection, const PrKitSelectionEdit& edit,
                    const uint32_t* PR_NULLABLE keys);

// Finds the collection tree's visible rows: each group, and the assets of each open group.
void collectionsTreeOrder(CollectionsState& collections);

// Returns the group of a collection tree's key, or writes the asset it names to `index` and returns true if it names
// one rather than its group's own row.
uint32_t collectionGroupOf(uint32_t key);
bool collectionAssetOf(uint32_t key, uint32_t* index);

} // namespace gallery

PR_ASSUME_NONNULL_END
