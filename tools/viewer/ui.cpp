// The viewer's controls in Studio Kit, in a layout tree of their own, which renders over the documents' renders.

#include "tools/viewer/ui.h"

#include "pr/app_display.h"
#include "pr/draw_list.h"
#include "pr/font_bundle.h"
#include "pr/font_set.h"
#include "pr/kit.h"
#include "pr/kit_box.h"
#include "pr/kit_button.h"
#include "pr/kit_choice.h"
#include "pr/kit_collection.h"
#include "pr/kit_input.h"
#include "pr/kit_menu.h"
#include "pr/kit_surface.h"
#include "pr/kit_text.h"
#include "pr/kit_theme.h"
#include "pr/layout_query.h"
#include "pr/layout_scene.h"
#include "pr/layout_tree.h"
#include "pr/text.h"
#include "pr/ui.h"

#include "tools/viewer/document_tree.h"
#include "tools/viewer/zoom.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace viewer {

using namespace harness;

// The width of the panel of controls, in logical pixels.
constexpr float kPanel = 300;

// The height of the list of documents, as a share of the window's, and of its rows, in logical pixels.
constexpr float kTreeShare = 0.45f;
constexpr float kTreeRow = 22;

// A render of a document that the frame shows under the controls.
struct ShownImage {
    PrGpuTexture texture;
    PrRect clip;
    PrRect destination;
    bool nearest;
};

struct Ui {
    PrGpuDevice* device = nullptr;
    PrFontSet* fonts = nullptr;
    PrTextShaper* shaper = nullptr;
    PrLayoutTree* layout = nullptr;
    PrUi* ui = nullptr;
    PrKit* kit = nullptr;
    PrDrawRenderer* renderer = nullptr;
    PrDrawFrame* frame = nullptr;
    PrDrawList* list = nullptr;
    const PrLayoutScene* scene = nullptr;
    std::vector<PrLayoutSceneScroll> scrolls;
    std::vector<ShownImage> images;
    float pixelRatio = 1;
    PrAppColorScheme scheme = PrAppColorScheme_Light;

    // The nodes of the last round: the box left for documents, and the list of documents, which holds the keyboard
    // unless something else does.
    PrLayoutNode slot = kPrLayoutNodeNone;
    PrLayoutNode treeView = kPrLayoutNodeNone;
    PrVec2 pointer{};
    bool pressed = false;           // a press that landed on the controls is held

    // The shown collection's documents as a tree, created when the collection is first shown, the collection that the
    // tree holds, and the filter typed over it.
    std::unique_ptr<DocumentTree> tree = nullptr;
    uint32_t treeCollection = 0;
    std::string filter;
    std::vector<PrKitTreeItem> items;   // the tree's rows, as the view takes them

    // The row that the keys move from, and the document that the row last followed.
    PrKitSelection selection{};
    uint32_t cursorDocument = 0;
};

namespace {

// What a round of the controls reads and changes.
struct Round {
    Ui* ui;
    const std::vector<RendererConfig>* configs;
    const std::vector<Reference>* references;
    const Figures* figures;
    Choices* choices;
    float height;
    bool pageKeys;
    bool prints;                // the shown document is one that the engine prints (prints)
};

// Returns the row of the tree's node, or the first row if it is not listed.
uint32_t rowOf(const DocumentTree& tree, uint32_t node) {
    const std::vector<TreeRow>& rows = tree.rows();
    for (size_t i = 0; i < rows.size(); ++i) {
        if (rows[i].node == node)
            return uint32_t(i);
    }
    return 0;
}

// Returns text that printf formats.
std::string format(const char* text, ...) PR_PRINTF_FORMAT(1, 2);

std::string format(const char* text, ...) {
    char buffer[256];
    va_list args;
    va_start(args, text);
    std::vsnprintf(buffer, sizeof buffer, text, args);
    va_end(args);
    return buffer;
}

void label(PrKit* kit, PrLayoutNode parent, const std::string& text, PrKitTone tone = PrKitTone_Auto) {
    prKitLabel(kit, parent, {.text = text}, {.role = PrKitTextRole_BodySmall, .tone = tone, .wrap = PrKitWrap_Wrap});
}

// Creates a select of `count` options under `parent`, which `name` names and `chosen` marks. Calls `choose` with the
// index of an option that is activated.
template <typename Name, typename Choose>
void select(PrKit* kit, PrLayoutNode parent, const std::string& shown, uint32_t count, uint32_t chosen, Name name,
        Choose choose) {
    PrLayoutNode node = prKitSelect(kit, parent, {.label = shown}, {.size = PrKitControlSize_Sm});
    PrLayoutNode menu = prKitSelectMenu(kit, node);
    if (menu == kPrLayoutNodeNone)
        return;
    for (uint32_t i = 0; i < count; ++i) {
        std::string text = name(i);
        if (text.empty())
            continue;
        PrKitMenuItem item{.text = text, .checked = i == chosen};
        if (prKitActivated(kit, prKitMenuItem(kit, menu, item, {})))
            choose(i);
    }
}

// Creates a labeled row of the panel, and returns the node that its control goes under.
PrLayoutNode row(PrKit* kit, PrLayoutNode parent, PrStringView name) {
    return prKitInspectorRow(kit, parent, {.label = name}, {});
}

// Creates a row of small buttons, and returns the row.
PrLayoutNode buttonRow(PrKit* kit, PrLayoutNode parent) {
    PrKitBoxStyle style{.direction = PrKitDirection_Row, .align = PrKitAlign_Center, .gap = kPrKitSpace2};
    return prKitBox(kit, parent, {}, style);
}

bool button(PrKit* kit, PrLayoutNode parent, PrStringView text) {
    return prKitActivated(kit, prKitButton(kit, parent, {.text = text}, {.size = PrKitControlSize_Sm}));
}

// Creates the filter and the tree of the collection's documents. A document whose row the keys or a click select is
// shown, and a row's disclosure opens and closes it. The tree shows the chosen document whenever something else chose
// it.
void documentTree(const Round& r, PrLayoutNode parent, const Collection& collection) {
    Ui& ui = *r.ui;
    PrKit* kit = ui.kit;
    Choices& choices = *r.choices;
    DocumentTree& tree = *ui.tree;
    PrLayoutNode field = prKitTextField(kit, parent, {.text = ui.filter, .placeholder = "filter"},
        {.size = PrKitControlSize_Sm});
    PrStringView typed{};
    if (prKitTextChanged(kit, field, &typed)) {
        ui.filter.assign(typed.data, typed.size);
        tree.filter(ui.filter);
    }
    bool reveal = choices.documentChanged || ui.cursorDocument != choices.document;
    if (reveal) {
        tree.reveal(choices.document);
        ui.cursorDocument = choices.document;
    }
    uint32_t lead = reveal ? rowOf(tree, tree.nodeOf(choices.document)) : ui.selection.lead;
    const std::vector<TreeRow>& rows = tree.rows();
    if (reveal)
        ui.selection = PrKitSelection{lead, lead};

    ui.items.resize(rows.size());
    for (size_t i = 0; i < rows.size(); ++i) {
        const DocumentTree::Node& node = tree.nodes()[rows[i].node];
        ui.items[i] = PrKitTreeItem{.depth = rows[i].depth, .hasChildren = node.document < 0, .open = node.expanded};
    }
    PrKitTreeView content{.count = uint32_t(rows.size()), .selection = &ui.selection, .items = ui.items.data()};
    float treeHeight = r.height * kTreeShare;
    PrKitListViewStyle style{.rowHeight = kTreeRow, .place = {.height = prKitPx(treeHeight), .flex = PrKitFlex_Fixed}};
    PrLayoutNode view = prKitTreeView(kit, parent, content, style);
    ui.treeView = view;
    if (r.pageKeys && prUiIsFocusWithin(ui.ui, view))
        prUiSetFocus(ui.ui, kPrLayoutNodeNone);
    else if (!r.pageKeys && prUiFocus(ui.ui) == kPrLayoutNodeNone)
        prUiSetFocus(ui.ui, view);

    auto rowAt = [&](uint32_t i) {
        const DocumentTree::Node& node = tree.nodes()[rows[i].node];
        PrLayoutNode shown =
            prKitTreeRow(kit, view, {.index = i, .item = rows[i].node + 1, .selected = i == ui.selection.lead});
        if (node.document < 0) {
            std::string text = format("%s  (%u)", node.name.c_str(), node.matches);
            prKitRowLabel(kit, shown, {.text = text}, PrKitTone_Auto);
        } else {
            prKitRowLabel(kit, shown, {.text = node.name}, PrKitTone_Auto);
        }
        return shown;
    };
    PrKitWindow window = prKitListWindow(kit, view);
    uint32_t end = std::min(window.first + window.count, uint32_t(rows.size()));
    for (uint32_t i = window.first; i < end; ++i)
        rowAt(i);
    // A row that something else chose may lie outside the window, so the view scrolls by its place in the list, with
    // the row in the middle of the view. The next round's window holds it.
    if (reveal) {
        float offset = std::max(float(lead) * kTreeRow - treeHeight / 2, 0.0f);
        prLayoutNodeSetScroll(ui.layout, view, PrLayoutVec{prLayoutUnitFromPx(0), prLayoutUnitFromPx(offset)});
    }

    PrKitSelectionEdit edit{};
    if (prKitSelectionEdited(kit, view, &edit) && edit.first < rows.size()) {
        int32_t document = tree.nodes()[rows[edit.first].node].document;
        ui.selection = PrKitSelection{edit.first, edit.first};
        if (document >= 0) {
            choices.document = uint32_t(document);
            ui.cursorDocument = choices.document;
        }
    }
    PrKitTreeFold fold{};
    if (prKitFolded(kit, view, &fold) && fold.row < rows.size())
        tree.setExpanded(rows[fold.row].node, fold.open);

    if (!ui.filter.empty())
        label(kit, parent, format("%u of %zu documents", tree.matching(), collection.documents().size()),
            PrKitTone_Faint);
    else
        label(kit, parent, format("%zu documents", collection.documents().size()), PrKitTone_Faint);
}

// Creates a segment of a segmented control, and returns true if it is activated.
bool segment(PrKit* kit, PrLayoutNode control, PrStringView name, bool selected) {
    return prKitActivated(kit, prKitSegment(kit, control, {.label = name, .selected = selected}));
}

// Creates the control of the media that a pane renders a page for.
void mediaControl(const Round& r, PrLayoutNode parent, PaneChoice& pane) {
    PrKit* kit = r.ui->kit;
    PrLayoutNode media = prKitSegmented(kit, row(kit, parent, "Media"), {}, {.size = PrKitControlSize_Sm});
    if (segment(kit, media, "Screen", pane.media == page_host::Media_Screen))
        pane.media = page_host::Media_Screen;
    if (segment(kit, media, "Print", pane.media == page_host::Media_Print))
        pane.media = page_host::Media_Print;
}

// Creates the controls of Prax's renderers in a pane: the family of renderers, its variants if it has several, the
// shaper, the hinting, and the media of a document that the engine prints.
void praxControls(const Round& r, PrLayoutNode parent, PaneChoice& pane) {
    PrKit* kit = r.ui->kit;
    const std::vector<RendererConfig>& configs = *r.configs;
    const RendererConfig& current = configs[pane.config];
    select(kit, row(kit, parent, "Renderer"), current.familyName, uint32_t(configs.size()), pane.config, [&](uint32_t b) {
        bool first = b == 0 || configs[b - 1].family != configs[b].family;
        return first ? std::string(configs[b].familyName) : std::string();
    }, [&](uint32_t b) { chooseFamily(pane, configs, configs[b].family); });
    if (nextInFamily(configs, pane.config) != pane.config) {
        select(kit, row(kit, parent, "Variant"), configs[pane.config].name, uint32_t(configs.size()),
            pane.config, [&](uint32_t b) {
                return configs[b].family == configs[pane.config].family ? std::string(configs[b].name) : std::string();
            }, [&](uint32_t b) { pane.config = b; });
    }
    select(kit, row(kit, parent, "Shaper"), std::string(prTextShaperBackendName(pane.text.shaper).data),
        PrTextShaperBackend_Count, pane.text.shaper, [](uint32_t s) {
            bool built = prTextShaperBackendIsBuilt(PrTextShaperBackend(s));
            return built ? std::string(prTextShaperBackendName(PrTextShaperBackend(s)).data) : std::string();
        }, [&](uint32_t s) { pane.text.shaper = PrTextShaperBackend(s); });
    select(kit, row(kit, parent, "Hinting"), std::string(prFontHintingName(pane.text.hinting).data),
        PrFontHinting_Count, pane.text.hinting,
        [](uint32_t h) { return std::string(prFontHintingName(PrFontHinting(h)).data); },
        [&](uint32_t h) { pane.text.hinting = PrFontHinting(h); });
    if (r.prints)
        mediaControl(r, parent, pane);
}

// Creates the controls of a pane: what renders it, Prax or one of the document's references, and Prax's renderers.
void paneControls(const Round& r, PrLayoutNode parent, PrStringView name, PaneChoice& pane) {
    PrKit* kit = r.ui->kit;
    const std::vector<Reference>& references = *r.references;
    PrKitBoxStyle style{.gap = kPrKitSpace2};
    PrLayoutNode box = prKitBox(kit, parent, {}, style);
    bool prax = pane.renderer == Renderer_Prax || pane.reference >= references.size();
    select(kit, row(kit, box, name), prax ? "Prax" : references[pane.reference].name,
        uint32_t(references.size()) + 1, prax ? 0 : pane.reference + 1,
        [&](uint32_t i) { return i == 0 ? std::string("Prax") : references[i - 1].name; }, [&](uint32_t i) {
            pane.renderer = i == 0 ? Renderer_Prax : Renderer_Reference;
            if (i != 0)
                pane.reference = i - 1;
        });
    if (pane.renderer == Renderer_Prax || pane.reference >= references.size())
        praxControls(r, box, pane);
    else if (r.prints && references[pane.reference].browsed)
        mediaControl(r, box, pane);
}

// Creates the figures of a render.
void renderedText(PrKit* kit, PrLayoutNode parent, const std::string& name, const Rendered& r) {
    const char* paced = r.backToBack ? "back to back" : "paced";
    if (r.status != PrStatus_Ok) {
        label(kit, parent, format("%s: failed, status %d", name.c_str(), int(r.status)), PrKitTone_Danger);
        return;
    }
    if (r.captured && r.fps > 0)
        label(kit, parent, format("%s: %.1f FPS", name.c_str(), r.fps));
    else if (r.captured)
        label(kit, parent, format("%s: %.2f ms a capture", name.c_str(), r.ms));
    else if (r.fps > 0)
        label(kit, parent, format("%s: %.1f FPS %s, %.2f ms", name.c_str(), r.fps, paced, r.ms));
    else
        label(kit, parent, format("%s: %.2f ms %s", name.c_str(), r.ms, paced));
    if (!r.captured && r.onGpu)
        label(kit, parent, format("record %.2f, renderer %.2f, GPU %.2f", r.recordMs, r.rendererMs, r.gpuMs),
            PrKitTone_Muted);
    else if (!r.captured)
        label(kit, parent, format("record %.2f, renderer %.2f", r.recordMs, r.rendererMs), PrKitTone_Muted);
    if (r.time >= 0 && r.frame >= 0)
        label(kit, parent, format("at %.3f s, frame %.3f", r.time, r.frame), PrKitTone_Muted);
    else if (r.time >= 0)
        label(kit, parent, format("at %.3f s", r.time), PrKitTone_Muted);
    else if (r.frame >= 0)
        label(kit, parent, format("at frame %.3f", r.frame), PrKitTone_Muted);
}

// Creates the panel's view controls: one render or two, how two compare, the zoom, and the clock.
void viewControls(const Round& r, PrLayoutNode parent) {
    PrKit* kit = r.ui->kit;
    Choices& c = *r.choices;
    PrKitChoiceStyle small{.size = PrKitControlSize_Sm};
    PrLayoutNode view = prKitSegmented(kit, parent, {}, small);
    if (segment(kit, view, "Single", c.view == View_Single))
        c.view = View_Single;
    if (segment(kit, view, "Compare", c.view == View_Compare))
        c.view = View_Compare;
    if (c.view == View_Compare) {
        paneControls(r, parent, "Second", c.panes[1]);
        PrLayoutNode look = prKitSegmented(kit, parent, {}, small);
        if (segment(kit, look, "Side by side", c.look == Look_SideBySide))
            c.look = Look_SideBySide;
        if (segment(kit, look, "Flip", c.look == Look_Flip))
            c.look = Look_Flip;
        if (segment(kit, look, "Difference", c.look == Look_Difference))
            c.look = Look_Difference;
        if (c.look == Look_Flip)
            prKitCheckbox(kit, parent, {.value = &c.showingSecond, .label = "Showing the second"}, small);
    }
    // The zoom commands, as the keys give them.
    PrLayoutNode zoom = buttonRow(kit, parent);
    if (button(kit, zoom, "-"))
        c.zoom = zoomOut(c.zoom);
    label(kit, zoom, format("%.0f%%", double(c.zoom) * 100));
    if (button(kit, zoom, "+"))
        c.zoom = zoomIn(c.zoom);
    if (button(kit, zoom, "Actual size"))
        c.zoom = 1;
    if (button(kit, zoom, "Fit width"))
        c.fitWidth = true;
    prKitCheckbox(kit, parent, {.value = &c.backToBack, .label = "Render back to back"}, small);
    // The clock pauses and steps. Rendering continues either way.
    PrLayoutNode clock = buttonRow(kit, parent);
    if (button(kit, clock, c.playing ? "Pause" : "Play"))
        c.playing = !c.playing;
    if (button(kit, clock, "<")) {
        c.playing = false;
        c.time = std::fmax(c.time - kFrame, 0.0);
    }
    if (button(kit, clock, ">")) {
        c.playing = false;
        c.time += kFrame;
    }
    if (button(kit, clock, "0")) {
        c.playing = false;
        c.time = 0;
    }
    label(kit, clock, format("%.3f s", c.time));
}

// Creates the panel's figures, and what the keys do.
void figureControls(const Round& r, PrLayoutNode parent) {
    PrKit* kit = r.ui->kit;
    const Choices& c = *r.choices;
    const Figures& figures = *r.figures;
    const Rendered& first = figures.panes[0];
    if (!first.captured)
        label(kit, parent, format("%u commands, %ux%u", first.commands, first.width, first.height));
    const Source& source = allCollections()[c.collection].documents()[c.document].source;
    renderedText(kit, parent, paneName(c.panes[0], *r.configs, *r.references, paneMedia(c.panes[0], source)), first);
    if (c.view == View_Compare) {
        renderedText(kit, parent, paneName(c.panes[1], *r.configs, *r.references, paneMedia(c.panes[1], source)),
            figures.panes[1]);
        if (figures.playing) {
            label(kit, parent, "comparing needs the clock paused: space", PrKitTone_Warning);
        } else if (figures.comparing) {
            label(kit, parent, "comparing...", PrKitTone_Faint);
        } else if (figures.compared) {
            const ImageDifference& d = figures.difference;
            label(kit, parent, format("changed %.3f%%, of content %.3f%%", d.changedRatio * 100, d.contentChangedRatio * 100));
            label(kit, parent, format("mean error %.3f", d.mae));
        }
    }
    prKitDivider(kit, parent, {});
    for (const char* line : {"arrows  tree     [ ]  document", "1-9  renderer    V  variant     T  shaper     P  print",
        "C  compare     F  flip     D  difference     X  swap", "Cmd + -  zoom     Cmd 0  actual size",
                "space  play     , .  frame     Home  start", "Q  quit"})
        label(kit, parent, line, PrKitTone_Faint);
    label(kit, parent, format("B  back to back     H  HDR%s", c.hdr ? " (on)" : ""), PrKitTone_Faint);
}

// Builds a round: the panel at the window's left, and the box left for documents, which draws nothing.
void build(void* context, PrUi*, const PrUiRound*) {
    const Round& r = *static_cast<const Round*>(context);
    Ui& ui = *r.ui;
    PrKit* kit = ui.kit;
    Choices& choices = *r.choices;
    PrKitBoxStyle shell{.direction = PrKitDirection_Row, .place = {.width = prKitFill(), .height = prKitFill()}};
    PrLayoutNode root = prKitBox(kit, kPrLayoutNodeNone, {}, shell);
    PrKitSurfaceStyle panelStyle{
        .box = {
            .overflow = PrKitOverflow_ScrollY,
            .gap = kPrKitSpace3,
            .padding = prKitEdgesXY(kPrKitSpace4, kPrKitSpace4),
            .place = {.width = prKitPx(kPanel), .height = prKitFill(), .flex = PrKitFlex_Fixed},
        },
        .background = PrKitBackground_Panel,
        .rules = PrKitRuleFlag_Right,
    };
    PrLayoutNode panel = prKitSurface(kit, root, {}, panelStyle);
    ui.slot = prKitBox(kit, root, {}, {.place = {.height = prKitFill(), .flex = PrKitFlex_Fill}});

    const std::vector<Collection>& collections = allCollections();
    select(kit, panel, collections[choices.collection].name, uint32_t(collections.size()), choices.collection,
        [&](uint32_t c) { return collections[c].name; }, [&](uint32_t c) {
            choices.collection = c;
            choices.document = 0;
            // The engine renders the collection's pages for its media until a pane chooses another.
            for (PaneChoice& pane : choices.panes)
                pane.media = collections[c].media;
            // A document is first compared with its first reference, which its suite compares Prax with.
            choices.panes[1].renderer = Renderer_Reference;
            choices.panes[1].reference = 0;
        });
    const Collection& collection = collections[choices.collection];
    if (ui.tree == nullptr || ui.treeCollection != choices.collection) {
        ui.tree = std::make_unique<DocumentTree>(collection.documents());
        ui.tree->filter(ui.filter);
        ui.treeCollection = choices.collection;
        // A new tree shows the chosen document.
        choices.documentChanged = true;
    }
    documentTree(r, panel, collection);
    choices.documentChanged = false;
    label(ui.kit, panel, collection.documents()[choices.document].description, PrKitTone_Muted);
    prKitDivider(kit, panel, {});
    paneControls(r, panel, "First", choices.panes[0]);
    viewControls(r, panel);
    prKitDivider(kit, panel, {});
    figureControls(r, panel);
}

// Returns true if something of the controls lies at `point`, rather than the box left for documents.
bool overControls(const Ui& ui, PrVec2 point) {
    PrLayoutNode top = kPrLayoutNodeNone;
    prLayoutTreeHitTest(ui.layout, prLayoutVecFromPx(point), &top, 1);
    return top != kPrLayoutNodeNone && top != ui.slot;
}

// Returns true if the keyboard is with something that takes every key: a text field or an open menu.
bool holdsKeys(const Ui& ui) {
    PrLayoutNode focus = prUiFocus(ui.ui);
    return focus != kPrLayoutNodeNone && focus != ui.treeView;
}

} // namespace

void chooseFamily(PaneChoice& pane, const std::vector<RendererConfig>& configs, uint32_t family) {
    pane.shown[configs[pane.config].family] = pane.config;
    pane.config = pane.shown[family];
}

std::string paneName(const PaneChoice& pane, const std::vector<RendererConfig>& configs,
        const std::vector<Reference>& references, page_host::Media media) {
    bool print = media == page_host::Media_Print;
    if (pane.renderer == Renderer_Reference && pane.reference < references.size()) {
        const Reference& reference = references[pane.reference];
        return reference.browsed && print ? reference.name + " (print)" : reference.name;
    }
    std::string prax = print ? "Prax (print), " : "Prax, ";
    return prax + configs[pane.config].name + ", " + prTextShaperBackendName(pane.text.shaper).data + ", " +
        prFontHintingName(pane.text.hinting).data;
}

page_host::Media paneMedia(const PaneChoice& pane, const Source& source) {
    return prints(source) ? pane.media : page_host::Media_Screen;
}

Ui* uiCreate(PrGpuDevice* device, PrAppWindow* window) {
    Ui* ui = new Ui{};
    ui->device = device;
    ui->fonts = prFontSetCreate(nullptr);
    PrFontBundle bundle = PrFontBundle_Complete;
    prFontBundleInstall(ui->fonts, &bundle, 1, PR_FONT_BUNDLE_DIR);
    ui->shaper = prTextShaperCreate(PrTextShaperBackend_Default);
    PrLayoutTreeDesc tree{.shaper = ui->shaper};
    ui->layout = prLayoutTreeCreate(&tree);
    ui->ui = prUiCreate(ui->layout);
    prUiSetWindow(ui->ui, window);
    PrKitTheme theme = prKitThemeStudio(PrKitThemeVariant_Light, ui->fonts);
    ui->kit = prKitCreate(ui->ui, &theme);
    prDrawRendererCreate(PrDrawRendererBackend_Default, device, nullptr, &ui->renderer);
    ui->frame = prDrawFrameCreate(ui->renderer);
    ui->list = prDrawListCreate();
    return ui;
}

void uiDestroy(Ui* ui) {
    if (ui == nullptr)
        return;
    prLayoutSceneRelease(ui->scene);
    prDrawListRelease(ui->list);
    prDrawFrameDestroy(ui->frame);
    prDrawRendererDestroy(ui->renderer);
    prKitDestroy(ui->kit);
    prUiDestroy(ui->ui);
    prLayoutTreeDestroy(ui->layout);
    prTextShaperDestroy(ui->shaper);
    prFontSetDestroy(ui->fonts);
    delete ui;
}

bool uiEvent(Ui* ui, const PrAppEvent& event) {
    switch (event.kind) {
        case PrAppEventKind_PointerMove:
        case PrAppEventKind_PointerDown:
        case PrAppEventKind_PointerUp: {
            ui->pointer = event.pointer.position;
            bool over = overControls(*ui, event.pointer.position);
            bool taken = over || ui->pressed;
            if (event.kind == PrAppEventKind_PointerDown)
                ui->pressed = over;
            else if (event.kind == PrAppEventKind_PointerUp)
                ui->pressed = false;
            // The controls see every move, so that their hover follows the pointer out of them, and every press, so
            // that a press elsewhere closes their menus.
            prUiPushEvent(ui->ui, &event);
            return taken;
        }
        case PrAppEventKind_Scroll:
            if (!overControls(*ui, ui->pointer))
                return false;
            prUiPushEvent(ui->ui, &event);
            return true;
        case PrAppEventKind_KeyDown:
        case PrAppEventKind_KeyUp: {
            if (holdsKeys(*ui)) {
                prUiPushEvent(ui->ui, &event);
                return true;
            }
            // The tree takes the arrow and page keys. The rest are the viewer's.
            bool moves = event.keyboard.key == PrAppKey_Up || event.keyboard.key == PrAppKey_Down ||
                event.keyboard.key == PrAppKey_Left ||
                event.keyboard.key == PrAppKey_Right || event.keyboard.key == PrAppKey_PageUp ||
                event.keyboard.key == PrAppKey_PageDown;
            if (moves)
                prUiPushEvent(ui->ui, &event);
            return moves;
        }
        case PrAppEventKind_Text:
        case PrAppEventKind_CompositionUpdate:
        case PrAppEventKind_CompositionEnd:
            if (!holdsKeys(*ui))
                return false;
            prUiPushEvent(ui->ui, &event);
            return true;
        case PrAppEventKind_Paste:
            prUiPushEvent(ui->ui, &event);
            return true;
        default:
            return false;
    }
}

PrRect uiLayout(Ui* ui, const PrAppFrame* frame, bool pageKeys, const std::vector<RendererConfig>& configs,
        const std::vector<Reference>& references, const Figures& figures, Choices& choices) {
    if (frame->preferences.colorScheme != ui->scheme) {
        ui->scheme = frame->preferences.colorScheme;
        PrKitThemeVariant variant =
            ui->scheme == PrAppColorScheme_Dark ? PrKitThemeVariant_Dark : PrKitThemeVariant_Light;
        PrKitTheme theme = prKitThemeStudio(variant, ui->fonts);
        prKitSetTheme(ui->kit, &theme);
    }
    const Source& source = allCollections()[choices.collection].documents()[choices.document].source;
    Round round{ui, &configs, &references, &figures, &choices, frame->extent.y, pageKeys, prints(source)};
    PrUiFrameDesc desc{
        .time = frame->time,
        .delta = frame->delta,
        .extent = frame->extent,
        .maxRounds = 4,
        .display = frame->display,
        .preferences = frame->preferences,
    };
    prUiFrame(ui->ui, &desc, build, &round);
    ui->pixelRatio = frame->display.pixelRatio;
    ui->images.clear();
    return prLayoutRectToPx(prLayoutNodeBounds(ui->layout, ui->slot, PrLayoutCoordinates_Viewport));
}

PrAppOutput uiOutput(const Ui* ui) {
    return prUiOutput(ui->ui);
}

void uiImage(Ui* ui, PrGpuTexture texture, PrRect clip, PrRect destination, bool nearest) {
    ui->images.push_back(ShownImage{texture, clip, destination, nearest});
}

void uiPrepare(Ui* ui, PrGpuCommands* commands, PrUVec2 extent, PrPixelFormat format) {
    const PrLayoutScene* scene = prLayoutTreeScene(ui->layout);
    prLayoutSceneRelease(ui->scene);
    ui->scene = scene;
    ui->scrolls.resize(prLayoutSceneScrolls(ui->layout, scene, nullptr, 0));
    prLayoutSceneScrolls(ui->layout, scene, ui->scrolls.data(), uint32_t(ui->scrolls.size()));
    PrLayoutScenePlayOptions options = prLayoutScenePlayOptionsDefault();
    options.scrolls = ui->scrolls.data();
    options.scrollCount = uint32_t(ui->scrolls.size());
    options.canvasFilled = true;

    PrDrawList* list = ui->list;
    prDrawListClear(list);
    prDrawListPushTransformAffine2(list, prAffine2Scale(PrVec2{ui->pixelRatio, ui->pixelRatio}));
    // The renders go first, so that a menu that opens over them covers them.
    for (const ShownImage& image : ui->images) {
        PrUVec2 size = prGpuTextureExtent(ui->device, image.texture);
        PrRect all{0, 0, float(size.x), float(size.y)};
        PrDrawSource source =
            prDrawSourceTexture(prDrawGpuTexture(image.texture), size, all, PrDrawAlpha_Premultiplied);
        PrDrawSampling sampling = image.nearest ? PrDrawSampling_Nearest : PrDrawSampling_Linear;
        prDrawListPushClipRect(list, image.clip, PrDrawEdges_Aliased);
        PrDrawBrush brush = prDrawBrushImage(source, image.destination, sampling);
        PrDrawPaint paint{};
        paint.brush = &brush;
        prDrawListRect(list, image.destination, paint);
        prDrawListPopClip(list);
    }
    prLayoutScenePlay(scene, list, &options);
    prDrawListPopTransform(list);
    PrDrawTarget target{.extent = extent, .format = format, .commands = commands};
    prDrawFramePrepare(ui->frame, list, &target);
}

void uiEncode(Ui* ui, PrGpuPass* pass) {
    prDrawFrameRender(ui->frame, pass, PrIVec2{0, 0});
}

} // namespace viewer
