// The gallery's data views: the collection of five thousand assets, the timeline and the node graph.

#include "examples/studio_gallery/section_data.h"

#include "pr/kit_button.h"
#include "pr/kit_choice.h"
#include "pr/kit_collection.h"
#include "pr/kit_data.h"
#include "pr/kit_graph.h"
#include "pr/kit_menu.h"
#include "pr/kit_surface.h"
#include "pr/kit_timeline.h"
#include "pr/ui.h"

#include "examples/studio_gallery/gallery_parts.h"

namespace gallery {

namespace {

constexpr PrKitTextStyle kBodySecondaryLine = {.tone = PrKitTone_Secondary, .wrap = PrKitWrap_Nowrap};

// The collection's row height and viewport height, and the least width and the height of a grid's tile, in px.
constexpr float kRowHeight = 32;
constexpr float kViewHeight = 360;
constexpr float kTileWidth = 112;
constexpr float kTileHeight = 104;

// The views that the segmented control chooses between.
enum {
    CollectionView_List,
    CollectionView_Grid,
    CollectionView_Tree,
};
typedef uint32_t CollectionView;

// Creates the list view's rows, and returns its scroll container.
PrLayoutNode buildList(PrKit* kit, PrLayoutNode parent, CollectionsState& collections) {
    PrKitListViewStyle style{
        .rowHeight = kRowHeight,
        .place = {.height = prKitPx(kViewHeight), .flex = PrKitFlex_Fixed},
    };
    PrLayoutNode view = prKitListView(
        kit, parent, {.count = collections.assets.count(), .selection = &collections.cursor}, style);
    PrKitWindow window = prKitListWindow(kit, view);
    for (uint32_t i = window.first; i < window.first + window.count; ++i) {
        const CollectionAsset& asset = collections.assets[i];
        bool selected = selectionHolds(collections.selection, i);
        PrLayoutNode node = prKitListRow(kit, view, {.index = i, .item = i, .selected = selected});
        prKitRowIcon(kit, node, {.icon = asset.icon});
        prKitRowLabel(kit, node, {.text = asset.name}, PrKitTone_Primary);
        prKitRowMeta(kit, node, {.text = asset.sizeText});
        PrKitBoxStyle type{.direction = PrKitDirection_Row, .justify = PrKitJustify_End};
        type.place.width = prKitPx(60);
        type.place.flex = PrKitFlex_Fixed;
        prKitLabel(kit, prKitBox(kit, node, {}, type), {.text = asset.type}, kMonoFaint);
    }
    PrKitSelectionEdit edit{};
    if (prKitSelectionEdited(kit, view, &edit))
        selectionApply(collections.selection, edit, nullptr);
    return view;
}

// Creates the grid's tiles, which share the list's selection, and returns its scroll container.
PrLayoutNode buildGrid(PrKit* kit, PrLayoutNode parent, CollectionsState& collections) {
    PrKitListViewStyle style{
        .rowHeight = kTileHeight,
        .tileWidth = kTileWidth,
        .place = {.height = prKitPx(kViewHeight), .flex = PrKitFlex_Fixed},
    };
    PrLayoutNode view = prKitListView(
        kit, parent, {.count = collections.assets.count(), .selection = &collections.cursor}, style);
    PrKitWindow window = prKitListWindow(kit, view);
    for (uint32_t i = window.first; i < window.first + window.count; ++i) {
        const CollectionAsset& asset = collections.assets[i];
        PrKitTile item{.index = i, .item = i, .selected = selectionHolds(collections.selection, i)};
        PrLayoutNode tile = prKitListTile(kit, view, item);
        prKitTileIcon(kit, tile, {.icon = asset.icon}, asset.tint);
        prKitTileLabel(kit, tile, {.text = asset.name});
    }
    PrKitSelectionEdit edit{};
    if (prKitSelectionEdited(kit, view, &edit))
        selectionApply(collections.selection, edit, nullptr);
    return view;
}

// Creates the tree view's rows over the assets' groups, and returns its scroll container.
PrLayoutNode buildTree(PrKit* kit, PrLayoutNode parent, CollectionsState& collections) {
    collectionsTreeOrder(collections);
    PrKitTreeView tree{
        .count = collections.treeRows.count(),
        .selection = &collections.treeCursor,
        .items = collections.treeRows.data(),
    };
    PrLayoutNode view =
        prKitTreeView(kit, parent, tree, {.place = {.height = prKitPx(kViewHeight), .flex = PrKitFlex_Fixed}});
    PrKitWindow window = prKitListWindow(kit, view);
    for (uint32_t i = window.first; i < window.first + window.count; ++i) {
        uint32_t key = collections.treeKeys[i];
        bool selected = selectionHolds(collections.treeSelection, key);
        uint32_t index = 0;
        char label[32];
        char meta[16];
        PrLayoutNode row = prKitTreeRow(kit, view, {.index = i, .item = key, .selected = selected});
        if (collectionAssetOf(key, &index)) {
            const CollectionAsset& asset = collections.assets[index];
            prKitRowIcon(kit, row, {.icon = asset.icon});
            prKitRowLabel(kit, row, {.text = asset.name}, PrKitTone_Primary);
            prKitRowMeta(kit, row, {.text = asset.type});
        } else {
            PrKitIconKind folder = collections.treeRows[i].open ? PrKitIconKind_FolderOpen : PrKitIconKind_Folder;
            prKitRowIcon(kit, row, {.icon = folder});
            prKitRowLabel(kit, row, {.text = formatToBuffer(Span(label), "Group {}", collectionGroupOf(key))},
                PrKitTone_Primary);
            prKitRowMeta(kit, row, {.text = formatToBuffer(Span(meta), "{}", kCollectionGroupSize)});
        }
    }
    PrKitTreeFold fold{};
    if (prKitFolded(kit, view, &fold))
        collections.groupOpen[collectionGroupOf(collections.treeKeys[fold.row])] = fold.open ? 1 : 0;
    PrKitSelectionEdit edit{};
    if (prKitSelectionEdited(kit, view, &edit))
        selectionApply(collections.treeSelection, edit, collections.treeKeys.data());
    return view;
}

// Records the timeline's gestures for its Undo: a gesture copies the blocks that it opens on, and one that changed them
// becomes the state that Undo restores.
void timelineEdit(void* context, PrLayoutNode, PrUiEditPhase phase) {
    TimelineState& timeline = *static_cast<TimelineState*>(context);
    switch (phase) {
        case PrUiEditPhase_Begin:
            timeline.opened = timeline.blocks;
            timeline.wrote = false;
            ++timeline.begins;
            break;
        case PrUiEditPhase_Change:
            timeline.wrote = true;
            ++timeline.changes;
            break;
        default:
            if (timeline.wrote) {
                timeline.undo = timeline.opened;
                timeline.hasUndo = true;
            }
            ++timeline.commits;
            break;
    }
}

} // namespace

void sectionCollections(PrKit* kit, PrLayoutNode parent, GalleryState& state) {
    CollectionsState& collections = state.collections;
    PrLayoutNode section = sectionFrame(kit, parent, "19", "Collections", "Virtualized multi-select over five thousand assets.",
        "The set is five thousand generated assets, and only the rows the viewport shows are built, a dozen or "
            "so of them at a time. Click to select, Shift-click for a range, Cmd or Ctrl-click to add or remove "
            "one; the arrow keys move the lead and Shift extends from the anchor. The grid shows the same assets "
            "as tiles, as many to a row as fit, and shares the list's selection. The tree reaches them through "
            "their groups, where Right opens a group and steps into it and Left closes it.");
    PrLayoutNode panel = demoPanel(kit, section, "ASSET BROWSER", "LIST . GRID . TREE", true);
    PrKitSurfaceStyle strip{.rules = PrKitRuleFlag_Bottom};
    strip.box.direction = PrKitDirection_Row;
    strip.box.align = PrKitAlign_Center;
    strip.box.gap = kPrKitSpace3;
    strip.box.padding = prKitEdgesXY(0, kPrKitSpace4);
    strip.box.place.height = prKitPx(kPrKitHeightMd);
    strip.box.place.flex = PrKitFlex_Fixed;
    PrLayoutNode bar = prKitSurface(kit, panel, {}, strip);
    const PrKitChoice modes[] = {{.label = "List"}, {.label = "Grid"}, {.label = "Tree"}};
    segmented(kit, bar, modes, 3, &collections.viewMode, {.size = PrKitControlSize_Sm});
    prKitSpacer(kit, bar, {});
    bool tree = collections.viewMode == CollectionView_Tree;
    char summary[64];
    size_t held = tree ? collections.treeSelection.count() : collections.selection.count();
    prKitLabel(kit, bar,
        {.text = formatToBuffer(Span(summary), "{} of {} selected", held, collections.assets.count())}, kMonoMuted);
    if (!collections.verb.isEmpty())
        prKitLabel(kit, bar, {.text = collections.verb}, kBodySecondaryLine);

    PrLayoutNode view;
    if (tree)
        view = buildTree(kit, panel, collections);
    else if (collections.viewMode == CollectionView_Grid)
        view = buildGrid(kit, panel, collections);
    else
        view = buildList(kit, panel, collections);
    PrKitMenuStyle wide{.minWidth = 200};
    PrLayoutNode menu = prKitContextMenu(kit, view, {}, wide);
    if (menu != kPrLayoutNodeNone) {
        const char* const verbs[] = {"Open", "Reveal in Finder"};
        for (const char* verb : verbs) {
            PrKitMenuItem item{.text = verb};
            if (prUiActivated(kit->ui, prKitMenuItem(kit, menu, item, {})))
                collections.verb = verb;
        }
        prKitMenuSeparator(kit, menu, {});
        PrKitMenuItem remove{.text = "Delete"};
        PrKitMenuItemStyle danger{.variant = PrKitMenuItemVariant_Danger};
        if (prUiActivated(kit->ui, prKitMenuItem(kit, menu, remove, danger)))
            collections.verb = "Delete";
    }

    PrLayoutNode empty = demoPanel(kit, section, "EMPTY STATE", "NO RESULTS", true);
    PrKitSurfaceStyle region{.ground = PrKitGround_Panel};
    region.box.place.height = prKitPx(140);
    region.box.place.flex = PrKitFlex_Fixed;
    prKitEmptyState(kit, prKitSurface(kit, empty, {}, region), {
        .icon = PrKitIconKind_Search,
        .title = "No assets match the filter",
        .hint = "Clear the filter to see every asset again",
    });
}

void sectionTimeline(PrKit* kit, PrLayoutNode parent, GalleryState& state) {
    TimelineState& timeline = state.timeline;
    PrLayoutNode section = sectionFrame(kit, parent, "20", "Timeline", "An editable clip over caller-owned tracks.",
        "The kit's timeline draws the ruler, the lanes, the blocks, the markers, the loop band and the playhead, "
            "and writes the model that this example owns. Drag the ruler to scrub the playhead, a block to move it, "
            "a block's end to resize it, and an empty part of a track to select with a rectangle; double-click a "
            "track to add a block, and Delete removes what is selected. A drag snaps to the grid, and to the edges "
            "of other blocks, the markers and the playhead as the toolbar enables them, and a line marks a snap to "
            "any of those. The wheel pans the view and Cmd with the wheel zooms it. Every gesture reports to the "
            "edit sink, which is what Undo steps back.");

    // The playhead runs through the loop while the clip plays.
    PrUi* ui = kit->ui;
    if (timeline.playing) {
        timeline.playhead += float(prUiCurrentRound(ui)->delta);
        if (timeline.playhead >= timeline.loop.end)
            timeline.playhead = timeline.loop.start;
        prUiRequestFrame(ui);
    }

    PrKitSurfaceStyle strip{.rules = PrKitRuleFlag_Bottom};
    strip.box.direction = PrKitDirection_Row;
    strip.box.align = PrKitAlign_Center;
    strip.box.gap = kPrKitSpace5;
    strip.box.padding = prKitEdgesXY(0, kPrKitSpace4);
    strip.box.place.height = prKitPx(kPrKitHeightMd);
    strip.box.place.flex = PrKitFlex_Fixed;
    PrLayoutNode bar = prKitSurface(kit, section, {}, strip);
    prKitToggle(kit, bar, {.value = &timeline.playing, .label = "Play"}, {});
    PrKitButtonStyle small{.size = PrKitControlSize_Sm};
    PrKitButton reset{.text = "Reset cursor"};
    if (prUiActivated(ui, prKitButton(kit, bar, reset, small)))
        timeline.playhead = timeline.loop.start;
    PrKitButton fit{.text = "Fit view"};
    if (prUiActivated(ui, prKitButton(kit, bar, fit, small)))
        timeline.view = PrKitTimeSpan{0, 8};
    const PrKitCheck snaps[] = {
        {.value = &timeline.snapping, .label = "Grid 0.25s"},
        {.value = &timeline.snapEdges, .label = "Edges"},
        {.value = &timeline.snapMarkers, .label = "Markers"},
        {.value = &timeline.snapPlayhead, .label = "Playhead"},
    };
    for (const PrKitCheck& snap : snaps)
        prKitCheckbox(kit, bar, snap, {});
    prKitSpacer(kit, bar, {});
    PrKitButton undo{.text = "Undo", .icon = PrKitIconKind_Undo, .disabled = !timeline.hasUndo};
    bool undone = prUiActivated(ui, prKitButton(kit, bar, undo, small));
    if (undone) {
        timeline.blocks = timeline.undo;
        timeline.hasUndo = false;
    }
    uint32_t selected = 0;
    for (const PrKitTimelineBlock& block : timeline.blocks)
        selected += block.selected ? 1 : 0;
    char count[32];
    prKitLabel(kit, bar, {.text = formatToBuffer(Span(count), "{} selected", selected)}, kMonoMuted);

    PrLayoutNode panel = demoPanel(kit, section, "ANIMATION CLIP", "EDITABLE . UNDO SINK", true);
    PrKitTimeline view{
        .tracks = timeline.tracks.data(),
        .trackCount = timeline.tracks.count(),
        .blocks = timeline.blocks.data(),
        .blockCount = timeline.blocks.count(),
        .markers = timeline.markers.data(),
        .markerCount = timeline.markers.count(),
        .view = &timeline.view,
        .playhead = &timeline.playhead,
        .loop = &timeline.loop,
        .snap = {
            .step = timeline.snapping ? 0.25f : 0,
            .edges = timeline.snapEdges,
            .markers = timeline.snapMarkers,
            .playhead = timeline.snapPlayhead,
        },
    };
    prUiSetEditSink(kit->ui, panel, timelineEdit, &timeline);
    PrLayoutNode clip = prKitTimeline(kit, panel, view, {.trackHeight = 30});
    if (prKitRemoved(kit, clip)) {
        Array<PrKitTimelineBlock> kept;
        for (const PrKitTimelineBlock& block : timeline.blocks) {
            if (!block.selected)
                kept.push(block);
        }
        timeline.blocks = kept;
    }
    uint32_t addTrack = 0;
    float addTime = 0;
    if (prKitAdded(kit, clip, &addTrack, &addTime)) {
        timeline.blocks.push(PrKitTimelineBlock{
            .id = timeline.nextId++,
            .track = addTrack,
            .start = addTime,
            .duration = 1,
            .color = {.47f, .70f, .47f, 1},
            .label = "New",
        });
    }
    PrKitSurfaceStyle tally{.rules = PrKitRuleFlag_Top};
    tally.box.direction = PrKitDirection_Row;
    tally.box.gap = kPrKitSpace4;
    tally.box.padding = prKitEdgesAll(kPrKitSpace4);
    PrLayoutNode counts = prKitSurface(kit, panel, {}, tally);
    const char* const labels[] = {"BEGIN", "CHANGE", "COMMIT"};
    const uint32_t values[] = {timeline.begins, timeline.changes, timeline.commits};
    for (uint32_t i = 0; i < 3; ++i) {
        char value[16];
        PrKitStatTile tile{.label = labels[i], .value = formatToBuffer(Span(value), "{}", values[i])};
        prKitStatTile(kit, counts, tile);
    }

    // The same model again, shown and not edited, with its own view.
    PrLayoutNode live = demoPanel(kit, section, "STATE INSPECTOR", "READ-ONLY", true);
    PrKitTimeline shown = view;
    shown.view = &timeline.inspectorView;
    shown.readOnly = true;
    prKitTimeline(kit, live, shown, {.trackHeight = 22});
}

void sectionGraph(PrKit* kit, PrLayoutNode parent, GalleryState& state) {
    GraphState& graph = state.graph;
    PrLayoutNode section = sectionFrame(kit, parent, "22", "Node Graph", "Typed nodes joined by wires.",
        "The kit's graph draws the canvas, the node boxes and the wires, and hands back each node's body, where "
            "the blend node's swatch is declared with the same kit calls as the rest of the page. Drag a node to "
            "move it and every node selected with it; drag from a port to a port of the same type to wire them, and "
            "an unlike pair refuses. Drag on empty canvas to select, click a wire to pick it, and Delete removes "
            "what is selected. The wheel pans, and Cmd with the wheel zooms.");
    PrUi* ui = kit->ui;

    PrKitSurfaceStyle strip{.rules = PrKitRuleFlag_Bottom};
    strip.box.direction = PrKitDirection_Row;
    strip.box.align = PrKitAlign_Center;
    strip.box.gap = kPrKitSpace3;
    strip.box.padding = prKitEdgesXY(0, kPrKitSpace4);
    strip.box.place.height = prKitPx(kPrKitHeightMd);
    strip.box.place.flex = PrKitFlex_Fixed;
    PrLayoutNode bar = prKitSurface(kit, section, {}, strip);
    PrKitButtonStyle small{.size = PrKitControlSize_Sm};
    PrKitButton arrange{.text = "Arrange"};
    if (prUiActivated(ui, prKitButton(kit, bar, arrange, small)))
        prKitGraphArrange(graph.nodes.data(), graph.nodes.count(), graph.wires.data(), graph.wires.count());
    uint32_t selected = 0;
    for (const PrKitGraphNode& node : graph.nodes)
        selected += node.selected ? 1 : 0;
    uint32_t picked = 0;
    for (const PrKitGraphWire& wire : graph.wires)
        picked += wire.selected ? 1 : 0;
    PrKitButton remove{.text = "Delete", .disabled = selected + picked == 0};
    bool removing = prUiActivated(ui, prKitButton(kit, bar, remove, small));
    prKitSpacer(kit, bar, {});
    char readout[64];
    prKitLabel(kit, bar, {
        .text = formatToBuffer(Span(readout), "{} nodes . {} wires . {} selected", graph.nodes.count(),
            graph.wires.count(), selected + picked),
    }, kMonoMuted);

    PrLayoutNode panel = demoPanel(kit, section, "MATERIAL GRAPH", "EDITABLE", true);
    static const PrColor kTypeColors[PortType_Count] = {
        {.41f, .62f, .87f, 1}, // scalar
        {.46f, .76f, .54f, 1}, // vector
        {.89f, .64f, .35f, 1}, // color
    };
    PrKitGraph view{
        .nodes = graph.nodes.data(),
        .nodeCount = graph.nodes.count(),
        .wires = graph.wires.data(),
        .wireCount = graph.wires.count(),
        .typeColors = kTypeColors,
        .typeCount = PortType_Count,
        .view = &graph.view,
    };
    PrLayoutNode canvas = prKitGraph(kit, panel, view, {.place = {.height = prKitPx(340), .flex = PrKitFlex_Fixed}});

    // The blend node's body shows its mix from its first input's color to its second's.
    for (uint32_t i = 0; i < graph.nodes.count(); ++i) {
        PrLayoutNode body = prKitGraphBody(kit, canvas, i);
        if (body == kPrLayoutNodeNone)
            continue;
        PrLayoutNode row = prKitBox(kit, body, {}, {.direction = PrKitDirection_Row, .gap = 0});
        constexpr uint32_t kCells = 12;
        for (uint32_t c = 0; c < kCells; ++c) {
            float t = float(c) / float(kCells - 1);
            PrColor from = kTypeColors[PortType_Color];
            PrColor to = kTypeColors[PortType_Vector];
            PrColor mixed{from.r + (to.r - from.r) * t, from.g + (to.g - from.g) * t, from.b + (to.b - from.b) * t, 1};
            prKitSwatch(kit, row, {.color = mixed}, {.size = 12});
        }
    }

    PrKitGraphLink link{};
    if (prKitConnected(kit, canvas, &link)) {
        // A wire into an input replaces the wire that fed it.
        Array<PrKitGraphWire> kept;
        for (const PrKitGraphWire& wire : graph.wires) {
            if (wire.link.toNode != link.toNode || wire.link.toPort != link.toPort)
                kept.push(wire);
        }
        kept.push(PrKitGraphWire{.id = graph.nextWireId++, .link = link});
        graph.wires = kept;
    }
    if (prKitRemoved(kit, canvas) || removing) {
        Array<PrKitGraphNode> nodes;
        for (const PrKitGraphNode& node : graph.nodes) {
            if (!node.selected)
                nodes.push(node);
        }
        auto held = [&](uint64_t id) {
            for (const PrKitGraphNode& node : nodes) {
                if (node.id == id)
                    return true;
            }
            return false;
        };
        Array<PrKitGraphWire> wires;
        for (const PrKitGraphWire& wire : graph.wires) {
            if (!wire.selected && held(wire.link.fromNode) && held(wire.link.toNode))
                wires.push(wire);
        }
        graph.nodes = nodes;
        graph.wires = wires;
    }

    PrKitBoxStyle legend{
        .direction = PrKitDirection_Row,
        .align = PrKitAlign_Center,
        .gap = kPrKitSpace6,
        .padding = prKitEdgesAll(kPrKitSpace4),
    };
    PrLayoutNode types = prKitBox(kit, panel, {}, legend);
    const char* const names[] = {"scalar", "vector", "color"};
    for (uint32_t i = 0; i < PortType_Count; ++i) {
        PrLayoutNode cell = prKitBox(kit, types, {}, {.direction = PrKitDirection_Row, .gap = kPrKitSpace2});
        prKitSwatch(kit, cell, {.color = kTypeColors[i]}, {.size = 8});
        prKitLabel(kit, cell, {.text = names[i]}, kBodySecondaryLine);
    }
}

} // namespace gallery
