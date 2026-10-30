// The viewer's controls in Dear ImGui, in their own context beside any that the imgui renderer creates, drawn by the
// platform's renderer of Dear ImGui (ui_renderer.h).

#include "tools/viewer/ui.h"

#include "tools/viewer/document_tree.h"
#include "tools/viewer/ui_renderer.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>

namespace viewer {

// The width of the panel of controls, in logical pixels.
constexpr float kPanel = 300;

struct Ui {
    ImGuiContext* context;
    UiRenderer* renderer = nullptr;

    // The shown collection's documents as a tree, created when the collection is first shown, the collection that the
    // tree holds, and the filter typed over it.
    std::unique_ptr<DocumentTree> tree = nullptr;
    uint32_t treeCollection = 0;
    char filter[256] = {};

    // The tree's row that the arrow keys move from: the shown document's row, or a directory's. It follows the shown
    // document when something else turns to another document.
    uint32_t cursor = 0;
    uint32_t cursorDocument = 0;
};

namespace {

void use(Ui* ui) {
    ImGui::SetCurrentContext(ui->context);
}

ImGuiKey keyOf(PrAppKey key) {
    if (key >= 'A' && key <= 'Z')
        return ImGuiKey(ImGuiKey_A + (key - 'A'));
    if (key >= '0' && key <= '9')
        return ImGuiKey(ImGuiKey_0 + (key - '0'));
    switch (key) {
        case PrAppKey_Left: return ImGuiKey_LeftArrow;
        case PrAppKey_Right: return ImGuiKey_RightArrow;
        case PrAppKey_Up: return ImGuiKey_UpArrow;
        case PrAppKey_Down: return ImGuiKey_DownArrow;
        case PrAppKey_Home: return ImGuiKey_Home;
        case PrAppKey_End: return ImGuiKey_End;
        case PrAppKey_PageUp: return ImGuiKey_PageUp;
        case PrAppKey_PageDown: return ImGuiKey_PageDown;
        case PrAppKey_Enter: return ImGuiKey_Enter;
        case PrAppKey_Escape: return ImGuiKey_Escape;
        case PrAppKey_Backspace: return ImGuiKey_Backspace;
        case PrAppKey_Delete: return ImGuiKey_Delete;
        case PrAppKey_Tab: return ImGuiKey_Tab;
        case ' ': return ImGuiKey_Space;
        default: return ImGuiKey_None;
    }
}

void modifiers(ImGuiIO& io, PrAppModifierFlags flags) {
    io.AddKeyEvent(ImGuiMod_Shift, (flags & PrAppModifierFlag_Shift) != 0);
    io.AddKeyEvent(ImGuiMod_Ctrl, (flags & PrAppModifierFlag_Control) != 0);
    io.AddKeyEvent(ImGuiMod_Alt, (flags & PrAppModifierFlag_Alt) != 0);
    io.AddKeyEvent(ImGuiMod_Super, (flags & PrAppModifierFlag_Super) != 0);
}

// Returns the row of the tree's node, or -1 if it is not listed.
int rowOf(const DocumentTree& tree, uint32_t node) {
    const std::vector<TreeRow>& rows = tree.rows();
    for (size_t i = 0; i < rows.size(); ++i) {
        if (rows[i].node == node)
            return int(i);
    }
    return -1;
}

// Moves the cursor as the arrow keys and the page keys direct, unless text is being typed. Up and down move along the
// rows, a page at a time with the page keys. Right opens a closed directory, and otherwise moves down. Left closes an
// open directory, and otherwise moves to the directory holding the row. A document the cursor lands on is shown.
// Returns true if the cursor moved, so that the tree scrolls to it.
bool moveCursor(Ui* ui, float height, Choices& choices) {
    if (ImGui::GetIO().WantTextInput)
        return false;
    int step = 0;
    int page = std::max(1, int(height / ImGui::GetFrameHeight()) - 1);
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow))
        step = 1;
    else if (ImGui::IsKeyPressed(ImGuiKey_UpArrow))
        step = -1;
    else if (ImGui::IsKeyPressed(ImGuiKey_PageDown))
        step = page;
    else if (ImGui::IsKeyPressed(ImGuiKey_PageUp))
        step = -page;
    bool right = ImGui::IsKeyPressed(ImGuiKey_RightArrow);
    bool left = ImGui::IsKeyPressed(ImGuiKey_LeftArrow);
    if (step == 0 && !right && !left)
        return false;
    DocumentTree& tree = *ui->tree;
    if (tree.rows().empty())
        return false;
    int row = rowOf(tree, ui->cursor);
    if (row < 0) {
        ui->cursor = tree.rows()[0].node;
        row = 0;
    }
    const DocumentTree::Node& node = tree.nodes()[ui->cursor];
    bool directory = node.document < 0;
    if (right && directory && !node.expanded) {
        tree.setExpanded(ui->cursor, true);
        return true;
    }
    if (left && directory && node.expanded) {
        tree.setExpanded(ui->cursor, false);
        return true;
    }
    if (left) {
        if (node.parent != 0)
            ui->cursor = node.parent;
    } else {
        int last = int(tree.rows().size()) - 1;
        ui->cursor = tree.rows()[size_t(std::clamp(row + (right ? 1 : step), 0, last))].node;
    }
    int32_t document = tree.nodes()[ui->cursor].document;
    if (document >= 0) {
        choices.document = uint32_t(document);
        ui->cursorDocument = choices.document;
    }
    return true;
}

// Draws the filter and the tree of the collection's documents. A directory opens and closes when it is clicked, and a
// clicked document is chosen. The keys move a cursor over the rows (moveCursor). A document that a plain run of its
// suite leaves out is dimmed, and shows what the run needs. The tree shows the chosen document whenever a key chose it.
void documentTree(Ui* ui, const Collection& collection, float height, Choices& choices) {
    const std::vector<Document>& documents = collection.documents();
    DocumentTree& tree = *ui->tree;
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputTextWithHint("##filter", "filter", ui->filter, sizeof(ui->filter)))
        tree.filter(ui->filter);
    bool reveal = choices.documentChanged;
    if (reveal || ui->cursorDocument != choices.document) {
        tree.reveal(choices.document);
        ui->cursor = tree.nodeOf(choices.document);
        ui->cursorDocument = choices.document;
    }
    reveal = moveCursor(ui, height, choices) || reveal;
    int toggled = -1;
    if (ImGui::BeginChild("##documents", ImVec2(-1, height), ImGuiChildFlags_Borders)) {
        const std::vector<TreeRow>& rows = tree.rows();
        int shownRow = reveal ? rowOf(tree, ui->cursor) : -1;
        const float indent = ImGui::GetStyle().IndentSpacing * 0.6f;
        ImGuiListClipper clipper;
        clipper.Begin(int(rows.size()));
        if (shownRow >= 0)
            clipper.IncludeItemByIndex(shownRow);
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                const TreeRow& row = rows[size_t(i)];
                const DocumentTree::Node& node = tree.nodes()[row.node];
                ImGui::PushID(int(row.node));
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + float(row.depth) * indent);
                ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_SpanAvailWidth;
                if (node.document < 0) {
                    if (row.node == ui->cursor)
                        flags |= ImGuiTreeNodeFlags_Selected;
                    ImGui::SetNextItemOpen(node.expanded);
                    if (ImGui::TreeNodeEx("##directory", flags, "%s  (%u)", node.name.c_str(), node.matches) !=
                            node.expanded)
                        toggled = int(row.node);
                } else {
                    flags |= ImGuiTreeNodeFlags_Leaf;
                    if (row.node == ui->cursor)
                        flags |= ImGuiTreeNodeFlags_Selected;
                    ImGui::TreeNodeEx("##document", flags, "%s", node.name.c_str());
                    if (ImGui::IsItemClicked()) {
                        choices.document = uint32_t(node.document);
                        ui->cursor = row.node;
                        ui->cursorDocument = choices.document;
                    }
                }
                if (i == shownRow)
                    ImGui::SetScrollHereY();
                ImGui::PopID();
            }
        }
    }
    ImGui::EndChild();
    if (toggled >= 0) {
        tree.setExpanded(uint32_t(toggled), !tree.nodes()[uint32_t(toggled)].expanded);
        ui->cursor = uint32_t(toggled);
    }
    if (ui->filter[0] != '\0')
        ImGui::TextDisabled("%u of %zu documents", tree.matching(), documents.size());
    else
        ImGui::TextDisabled("%zu documents", documents.size());
}

// Draws a combo that chooses the pane's family of renderers, by name, and if the family has several configurations, a
// combo that chooses among them.
void rendererCombos(const std::vector<RendererConfig>& configs, PaneChoice& pane) {
    const RendererConfig& current = configs[pane.config];
    if (ImGui::BeginCombo("Renderer", current.familyName)) {
        for (uint32_t b = 0; b < uint32_t(configs.size()); ++b) {
            if (b != 0 && configs[b - 1].family == configs[b].family)
                continue;
            if (ImGui::Selectable(configs[b].familyName, configs[b].family == current.family))
                chooseFamily(pane, configs, configs[b].family);
        }
        ImGui::EndCombo();
    }
    if (nextInFamily(configs, pane.config) == pane.config)
        return;
    ImGui::SetNextItemWidth(-90);
    if (ImGui::BeginCombo("Variant", current.name)) {
        for (uint32_t b = 0; b < uint32_t(configs.size()); ++b)
            if (configs[b].family == current.family && ImGui::Selectable(configs[b].name, b == pane.config))
                pane.config = b;
        ImGui::EndCombo();
    }
}

// Draws a combo that chooses the pane's shaper, and one that chooses how it hints fonts.
void textCombos(PaneChoice& pane) {
    ImGui::SetNextItemWidth(-90);
    if (ImGui::BeginCombo("Shaper", prTextShaperBackendName(pane.text.shaper).data)) {
        for (PrTextShaperBackend s = 0; s < PrTextShaperBackend_Count; ++s) {
            if (prTextShaperBackendIsBuilt(s) &&
                    ImGui::Selectable(prTextShaperBackendName(s).data, s == pane.text.shaper))
                pane.text.shaper = s;
        }
        ImGui::EndCombo();
    }
    ImGui::SetNextItemWidth(-90);
    if (ImGui::BeginCombo("Hinting", prFontHintingName(pane.text.hinting).data)) {
        for (PrFontHinting h = 0; h < PrFontHinting_Count; ++h)
            if (ImGui::Selectable(prFontHintingName(h).data, h == pane.text.hinting))
                pane.text.hinting = h;
        ImGui::EndCombo();
    }
}

// Draws the controls of a pane: what renders it, Prax or one of the document's references, and Prax's renderers.
void paneControls(const char* label, const std::vector<RendererConfig>& configs,
        const std::vector<Reference>& references, PaneChoice& pane) {
    ImGui::PushID(label);
    bool prax = pane.renderer == Renderer_Prax || pane.reference >= references.size();
    ImGui::SetNextItemWidth(-90);
    if (ImGui::BeginCombo(label, prax ? "Prax" : references[pane.reference].name.c_str())) {
        if (ImGui::Selectable("Prax", prax))
            pane.renderer = Renderer_Prax;
        for (uint32_t r = 0; r < uint32_t(references.size()); ++r) {
            if (ImGui::Selectable(references[r].name.c_str(), !prax && r == pane.reference)) {
                pane.renderer = Renderer_Reference;
                pane.reference = r;
            }
        }
        ImGui::EndCombo();
    }
    if (pane.renderer == Renderer_Prax || pane.reference >= references.size()) {
        ImGui::SetNextItemWidth(-90);
        rendererCombos(configs, pane);
        textCombos(pane);
    }
    ImGui::PopID();
}

void renderedText(const char* label, const Rendered& r) {
    if (r.status != PrStatus_Ok)
        ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s: failed, status %d", label, int(r.status));
    else if (r.captured && r.fps > 0)
        ImGui::Text("%s: %.1f FPS", label, r.fps);
    else if (r.captured)
        ImGui::Text("%s: %.2f ms a capture", label, r.ms);
    else if (r.fps > 0)
        ImGui::Text("%s: %.1f FPS %s, %.2f ms", label, r.fps, r.backToBack ? "back to back" : "paced", r.ms);
    else
        ImGui::Text("%s: %.2f ms %s", label, r.ms, r.backToBack ? "back to back" : "paced");
    if (r.status != PrStatus_Ok)
        return;
    if (!r.captured && r.onGpu)
        ImGui::Text("  record %.2f, renderer %.2f, GPU %.2f", r.recordMs, r.rendererMs, r.gpuMs);
    else if (!r.captured)
        ImGui::Text("  record %.2f, renderer %.2f", r.recordMs, r.rendererMs);
    if (r.time >= 0 && r.frame >= 0)
        ImGui::Text("  at %.3f s, frame %.3f", r.time, r.frame);
    else if (r.time >= 0)
        ImGui::Text("  at %.3f s", r.time);
    else if (r.frame >= 0)
        ImGui::Text("  at frame %.3f", r.frame);
}

} // namespace

void chooseFamily(PaneChoice& pane, const std::vector<RendererConfig>& configs, uint32_t family) {
    pane.shown[configs[pane.config].family] = pane.config;
    pane.config = pane.shown[family];
}

std::string paneName(const PaneChoice& pane, const std::vector<RendererConfig>& configs,
        const std::vector<Reference>& references) {
    if (pane.renderer == Renderer_Reference && pane.reference < references.size())
        return references[pane.reference].name;
    return std::string("Prax, ") + configs[pane.config].name + ", " +
        prTextShaperBackendName(pane.text.shaper).data + ", " + prFontHintingName(pane.text.hinting).data;
}

Ui* uiCreate(PrGpuDevice* device) {
    ImGuiContext* previous = ImGui::GetCurrentContext();
    Ui* ui = new Ui{ImGui::CreateContext()};
    use(ui);
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.Fonts->AddFontFromFileTTF(PR_FONTS_DIR "/NotoSans.ttf", 15);
    ImGui::StyleColorsDark();
    ui->renderer = uiRendererCreate(device);
    ImGui::SetCurrentContext(previous);
    return ui;
}

void uiDestroy(Ui* ui) {
    if (ui == nullptr)
        return;
    use(ui);
    uiRendererDestroy(ui->renderer);
    ImGui::DestroyContext(ui->context);
    delete ui;
}

bool uiEvent(Ui* ui, const PrAppEvent& event) {
    use(ui);
    ImGuiIO& io = ImGui::GetIO();
    modifiers(io, event.modifiers);
    switch (event.kind) {
        case PrAppEventKind_PointerMove:
            io.AddMousePosEvent(event.position.x, event.position.y);
            return io.WantCaptureMouse;
        case PrAppEventKind_PointerDown:
        case PrAppEventKind_PointerUp:
            io.AddMousePosEvent(event.position.x, event.position.y);
            if (event.button < ImGuiMouseButton_COUNT)
                io.AddMouseButtonEvent(event.button, event.kind == PrAppEventKind_PointerDown);
            return io.WantCaptureMouse;
        case PrAppEventKind_Scroll:
            io.AddMouseWheelEvent(-event.delta.x * 0.02f, -event.delta.y * 0.02f);
            return io.WantCaptureMouse;
        case PrAppEventKind_KeyDown:
        case PrAppEventKind_KeyUp: {
            io.AddKeyEvent(keyOf(event.key), event.kind == PrAppEventKind_KeyDown);
            // The tree's cursor moves by the arrow and page keys (moveCursor).
            bool moves = event.key == PrAppKey_Up || event.key == PrAppKey_Down || event.key == PrAppKey_Left ||
                event.key == PrAppKey_Right || event.key == PrAppKey_PageUp || event.key == PrAppKey_PageDown;
            return io.WantCaptureKeyboard || moves;
        }
        case PrAppEventKind_Text: {
            std::string text(event.text.data, event.text.size);
            io.AddInputCharactersUTF8(text.c_str());
            return io.WantTextInput;
        }
        default:
            return false;
    }
}

PrRect uiLayout(Ui* ui, float width, float height, float pixelRatio, double delta,
        const std::vector<RendererConfig>& configs, const std::vector<Reference>& references,
        const Figures& figures, Choices& choices) {
    use(ui);
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(width, height);
    io.DisplayFramebufferScale = ImVec2(pixelRatio, pixelRatio);
    io.DeltaTime = delta > 0 ? float(delta) : 1.0f / 60;
    ImGui::NewFrame();

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(kPanel, height));
    ImGui::Begin("Viewer", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus);
    const std::vector<Collection>& collections = allCollections();
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##collection", collections[choices.collection].name.c_str())) {
        for (uint32_t c = 0; c < uint32_t(collections.size()); ++c) {
            if (ImGui::Selectable(collections[c].name.c_str(), c == choices.collection)) {
                choices.collection = c;
                choices.document = 0;
                // A document is first compared with its first reference, which its suite compares Prax with.
                choices.panes[1].renderer = Renderer_Reference;
                choices.panes[1].reference = 0;
            }
        }
        ImGui::EndCombo();
    }
    // The collection as the combo left it.
    const Collection& collection = collections[choices.collection];
    if (ui->tree == nullptr || ui->treeCollection != choices.collection) {
        ui->tree = std::make_unique<DocumentTree>(collection.documents());
        ui->tree->filter(ui->filter);
        ui->treeCollection = choices.collection;
        // A new tree shows the chosen document.
        choices.documentChanged = true;
    }
    documentTree(ui, collection, height * 0.45f, choices);
    choices.documentChanged = false;
    const Document& document = collection.documents()[choices.document];
    ImGui::TextWrapped("%s", document.description.c_str());
    ImGui::Separator();

    paneControls("First", configs, references, choices.panes[0]);
    int view = choices.view == View_Single ? 0 : 1;
    ImGui::RadioButton("Single", &view, 0);
    ImGui::SameLine();
    ImGui::RadioButton("Compare", &view, 1);
    choices.view = view == 0 ? View_Single : View_Compare;
    if (choices.view == View_Compare) {
        paneControls("Second", configs, references, choices.panes[1]);
        int look = int(choices.look);
        ImGui::RadioButton("Side by side", &look, int(Look_SideBySide));
        ImGui::SameLine();
        ImGui::RadioButton("Flip", &look, int(Look_Flip));
        ImGui::SameLine();
        ImGui::RadioButton("Difference", &look, int(Look_Difference));
        choices.look = Look(look);
        if (choices.look == Look_Flip)
            ImGui::Checkbox("Showing the second", &choices.showingSecond);
    }
    // Both views zoom and pan.
    ImGui::Text("Zoom %.0f%%%s", double(figures.zoom) * 100, choices.fit ? ", fitted" : "");
    ImGui::SameLine();
    if (ImGui::SmallButton("Fit")) {
        choices.fit = true;
        choices.panX = 0;
        choices.panY = 0;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("1:1")) {
        choices.fit = false;
        choices.zoom = 1;
        choices.panX = 0;
        choices.panY = 0;
    }
    ImGui::Separator();
    ImGui::Checkbox("Render back to back", &choices.backToBack);
    // The clock pauses and steps. Rendering continues either way.
    ImGui::Separator();
    if (ImGui::SmallButton(choices.playing ? "Pause" : "Play"))
        choices.playing = !choices.playing;
    ImGui::SameLine();
    if (ImGui::SmallButton("<")) {
        choices.playing = false;
        choices.time = std::fmax(choices.time - kFrame, 0.0);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(">")) {
        choices.playing = false;
        choices.time += kFrame;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("0")) {
        choices.playing = false;
        choices.time = 0;
    }
    ImGui::SameLine();
    ImGui::Text("%.3f s", choices.time);
    ImGui::Separator();

    const Rendered& r = figures.panes[0];
    if (!r.captured)
        ImGui::Text("%u commands, %ux%u", r.commands, r.width, r.height);
    renderedText(paneName(choices.panes[0], configs, references).c_str(), r);
    if (choices.view == View_Compare) {
        renderedText(paneName(choices.panes[1], configs, references).c_str(), figures.panes[1]);
        if (figures.playing)
            ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "comparing needs the clock paused: space");
        else if (figures.comparing)
            ImGui::TextDisabled("comparing...");
        else if (figures.compared) {
            const ImageDifference& d = figures.difference;
            ImGui::Text("changed %.3f%%, of content %.3f%%", d.changedRatio * 100, d.contentChangedRatio * 100);
            ImGui::Text("mean error %.3f", d.mae);
        }
    }
    ImGui::Separator();
    ImGui::TextDisabled("arrows  tree     [ ]  document");
    ImGui::TextDisabled("1-9  renderer    V  variant     T  shaper");
    ImGui::TextDisabled("C  compare     F  flip     D  difference     X  swap");
    ImGui::TextDisabled("scroll  zoom     drag  pan     =  1:1     0  fit");
    ImGui::TextDisabled("B  back to back     H  HDR%s", choices.hdr ? " (on)" : "");
    ImGui::TextDisabled("space  play     , .  frame     Home  start");
    ImGui::TextDisabled("Q  quit");
    ImGui::End();
    return PrRect{kPanel, 0, width, height};
}

void uiImage(Ui* ui, PrGpuTexture texture, PrRect clip, PrRect destination, bool nearest) {
    use(ui);
    ImTextureID id = uiRendererTexture(ui->renderer, texture);
    if (id == ImTextureID_Invalid)
        return;
    ImDrawList* draw = ImGui::GetBackgroundDrawList();
    const ImGuiPlatformIO& platform = ImGui::GetPlatformIO();
    draw->PushClipRect(ImVec2(clip.left, clip.top), ImVec2(clip.right, clip.bottom), true);
    if (nearest && platform.DrawCallback_SetSamplerNearest != nullptr)
        draw->AddCallback(platform.DrawCallback_SetSamplerNearest, nullptr);
    draw->AddImage(ImTextureRef(id), ImVec2(destination.left, destination.top),
        ImVec2(destination.right, destination.bottom));
    if (nearest && platform.DrawCallback_SetSamplerLinear != nullptr)
        draw->AddCallback(platform.DrawCallback_SetSamplerLinear, nullptr);
    draw->PopClipRect();
}

void uiEncode(Ui* ui, PrGpuPass* pass) {
    use(ui);
    ImGui::Render();
    uiRendererRender(ui->renderer, ImGui::GetDrawData(), pass);
}

} // namespace viewer
