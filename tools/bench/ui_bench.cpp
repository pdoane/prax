// Measures the cost of an immediate-mode frame: a tree declared whole every frame, laid out and painted into a scene.
//
// ./dev.sh bench ui runs this harness and prints its rows. The trees are generated in the shape of a tool's UI: a
// toolbar of buttons over a scroll container of rows, each row an icon, a label and a value. The sizes are a hundred
// times apart, so that a cost that grows with the tree shows. Each frame declares every node through pr/layout_frame.h
// from records interned once, as a component library declares them. The texts are made before the frames, so that no
// row measures formatting. Each row gives the cost of one frame and the work that it did.
//
// Usage: ui_bench [--verify] [case]
//
// --verify checks the tree after each case against a layout from scratch (prLayoutTreeVerify), and a failure fails the
// run. If a case is named, only that case runs, repeatedly, for five minutes (harnessRepeat). This is for attaching a
// profiler.

#include "pr/assert.h"
#include "pr/layout_frame.h"
#include "pr/layout_scene.h"
#include "pr/layout_style.h"
#include "pr/layout_tree.h"
#include "pr/string_ops.h"
#include "pr/text_buffer.h"
#include "pr/text_harfbuzz.h"

#include "tools/harness/harness.h"

#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

using namespace pr;
using namespace harness;

namespace {

// The viewport that every frame lays out in, in px.
constexpr float kViewportWidth = 1440;
constexpr float kViewportHeight = 900;

// The number of nodes that a row declares: the row, its icon, and two cells, each with a text.
constexpr uint32_t kRowNodes = 6;

// The number of buttons in the toolbar, each a box and its text.
constexpr uint32_t kButtons = 24;

// The records that the frames declare, interned once.
struct Styles {
    PrLayoutStyleIndex root;
    PrLayoutStyleIndex toolbar;
    PrLayoutStyleIndex button;
    PrLayoutStyleIndex body;
    PrLayoutStyleIndex row;
    PrLayoutStyleIndex rowHovered;
    PrLayoutStyleIndex icon;
    PrLayoutStyleIndex label;
    PrLayoutStyleIndex value;
};

// A tree and the content that its frames declare.
struct Work {
    PrLayoutTree* tree;
    Styles styles;
    std::vector<std::string> labels;    // by item
    std::vector<std::string> values;    // by item
    std::vector<std::string> buttons;
    uint32_t rows;                      // the number of rows that a frame declares
    uint32_t first;                     // the item that the first row shows
    uint32_t hovered;                   // the hovered row, or `rows` for none
    uint32_t changed;                   // the item whose label shows its alternate text, or ~0u for none
    bool keyed;                         // rows are keyed by their items
    const PrLayoutScene* PR_NULLABLE scene;
};

PrLayoutLength pxLength(float px) {
    return prLayoutLengthPx(px);
}

PrLayoutStyleIndex intern(PrLayoutTree* tree, const PrLayoutBoxStyle& box, const PrLayoutFlexStyle* PR_NULLABLE flex,
        const PrLayoutDecorationStyle* PR_NULLABLE decoration) {
    PrLayoutStyle record = prLayoutStyleDefault();
    record.groups[PrLayoutGroup_Box] = prLayoutBoxStyleIntern(tree, &box);
    if (flex != nullptr)
        record.groups[PrLayoutGroup_Flex] = prLayoutFlexStyleIntern(tree, flex);
    if (decoration != nullptr)
        record.groups[PrLayoutGroup_Decoration] = prLayoutDecorationStyleIntern(tree, decoration);
    PrLayoutStyleIndex index = prLayoutStyleIntern(tree, &record);
    prLayoutStyleRetain(tree, index);
    return index;
}

PrColor16 color(float r, float g, float b) {
    return prColor16FromColor(PrColor{r, g, b, 1});
}

Styles makeStyles(PrLayoutTree* tree) {
    Styles s{};
    PrLayoutFlexStyle column = prLayoutFlexStyleDefault();
    column.direction = PrLayoutFlexDirection_Column;
    PrLayoutFlexStyle row = prLayoutFlexStyleDefault();
    row.alignItems = PrLayoutAlignment_Center;
    row.columnGap = pxLength(8);
    PrLayoutFlexStyle grow = prLayoutFlexStyleDefault();
    grow.grow = 1;
    grow.basis = pxLength(0);

    PrLayoutBoxStyle box = prLayoutBoxStyleDefault();
    box.display = PrLayoutDisplay_Flex;
    box.width = prLayoutLengthPercent(100);
    box.height = prLayoutLengthPercent(100);
    PrLayoutDecorationStyle dark = prLayoutDecorationStyleDefault();
    dark.background = color(0.1f, 0.1f, 0.12f);
    s.root = intern(tree, box, &column, &dark);

    box = prLayoutBoxStyleDefault();
    box.display = PrLayoutDisplay_Flex;
    for (uint32_t side = 0; side < PrRectSide_Count; ++side)
        box.padding[side] = pxLength(4);
    s.toolbar = intern(tree, box, &row, nullptr);

    box = prLayoutBoxStyleDefault();
    box.display = PrLayoutDisplay_Flex;
    box.padding[PrRectSide_Left] = box.padding[PrRectSide_Right] = pxLength(8);
    box.padding[PrRectSide_Top] = box.padding[PrRectSide_Bottom] = pxLength(3);
    for (uint32_t side = 0; side < PrRectSide_Count; ++side)
        box.border[side] = prLayoutUnitFromPx(1);
    PrLayoutDecorationStyle button = prLayoutDecorationStyleDefault();
    button.background = color(0.2f, 0.2f, 0.24f);
    for (uint32_t side = 0; side < PrRectSide_Count; ++side) {
        button.borderColor[side] = color(0.3f, 0.3f, 0.36f);
        button.borderStyle[side] = PrDrawBorderStyle_Solid;
    }
    s.button = intern(tree, box, &row, &button);

    PrLayoutFlexStyle body = column;
    body.grow = 1;
    body.basis = pxLength(0);
    box = prLayoutBoxStyleDefault();
    box.display = PrLayoutDisplay_Flex;
    box.overflowX = PrLayoutOverflow_Hidden;
    box.overflowY = PrLayoutOverflow_Scroll;
    box.minHeight = pxLength(0);
    s.body = intern(tree, box, &body, nullptr);

    box = prLayoutBoxStyleDefault();
    box.display = PrLayoutDisplay_Flex;
    box.height = pxLength(22);
    box.padding[PrRectSide_Left] = box.padding[PrRectSide_Right] = pxLength(6);
    PrLayoutFlexStyle fixed = row;
    fixed.shrink = 0;
    s.row = intern(tree, box, &fixed, nullptr);
    PrLayoutDecorationStyle hovered = prLayoutDecorationStyleDefault();
    hovered.background = color(0.25f, 0.3f, 0.45f);
    s.rowHovered = intern(tree, box, &fixed, &hovered);

    box = prLayoutBoxStyleDefault();
    box.display = PrLayoutDisplay_Block;
    box.width = box.height = pxLength(16);
    PrLayoutFlexStyle still = prLayoutFlexStyleDefault();
    still.shrink = 0;
    PrLayoutDecorationStyle icon = prLayoutDecorationStyleDefault();
    icon.background = color(0.5f, 0.6f, 0.8f);
    s.icon = intern(tree, box, &still, &icon);

    box = prLayoutBoxStyleDefault();
    box.display = PrLayoutDisplay_Block;
    box.overflowX = box.overflowY = PrLayoutOverflow_Hidden;
    box.minWidth = pxLength(0);
    s.label = intern(tree, box, &grow, nullptr);
    box.width = pxLength(120);
    s.value = intern(tree, box, &still, nullptr);
    return s;
}

// Declares one frame of the tree: the toolbar, then the rows of the items from `first`.
void declare(Work* w) {
    PrLayoutTree* tree = w->tree;
    const Styles& s = w->styles;
    prLayoutTreeBeginFrame(tree);
    PrLayoutNode root = prLayoutTreeFrameElement(tree, kPrLayoutNodeNone, 0, s.root);
    PrLayoutNode toolbar = prLayoutTreeFrameElement(tree, root, 0, s.toolbar);
    for (const std::string& name : w->buttons) {
        PrLayoutNode button = prLayoutTreeFrameElement(tree, toolbar, 0, s.button);
        prLayoutTreeFrameText(tree, button, 0, name);
    }
    PrLayoutNode body = prLayoutTreeFrameElement(tree, root, 0, s.body);
    for (uint32_t i = 0; i < w->rows; ++i) {
        uint32_t item = w->first + i;
        uint64_t key = w->keyed ? uint64_t(item) + 1 : 0;
        PrLayoutNode row = prLayoutTreeFrameElement(tree, body, key, i == w->hovered ? s.rowHovered : s.row);
        prLayoutTreeFrameElement(tree, row, 0, s.icon);
        PrLayoutNode label = prLayoutTreeFrameElement(tree, row, 0, s.label);
        const std::string& text = w->labels[item];
        PrStringView view{text.data(), item == w->changed ? text.size() / 2 : text.size()};
        prLayoutTreeFrameText(tree, label, 0, view);
        PrLayoutNode value = prLayoutTreeFrameElement(tree, row, 0, s.value);
        prLayoutTreeFrameText(tree, value, 0, w->values[item]);
    }
}

// Lays out and paints the frame that the tree declared.
void finish(Work* w, bool paint) {
    PrLayoutRoot viewport{kPrLayoutNodeNone, prLayoutVecFromPx(PrVec2{kViewportWidth, kViewportHeight})};
    prLayoutTreeFrameRoots(w->tree, &viewport.node, 1);
    prLayoutTreeLayout(w->tree, &viewport, 1);
    if (paint) {
        prLayoutSceneRelease(w->scene);
        w->scene = prLayoutTreeScene(w->tree);
    }
}

// Makes a font set of the checkout's Arimo and a shaper, and returns the default text font over the set.
PrTextFont* makeFont(PrFontSet** fonts, PrTextShaper** shaper) {
    *fonts = prFontSetCreate(nullptr);
    PrTextBuffer path = {};
    prTextBufferAppend(&path, PR_FONTS_DIR "/Arimo.ttf");
    const PrFont* font = nullptr;
    if (prFontCreateFromFile(prTextBufferView(&path), 0, nullptr, &font) == PrStatus_Ok) {
        PrFontFaceDescriptor descriptor = prFontFaceDescriptorDefault();
        prFontSetAdd(*fonts, &descriptor, font);
        prFontRelease(font);
    }
    prTextBufferReset(&path);
    prFontSetSetDefaultFamily(*fonts, prFontFamilyFromName("Arimo"));
    *shaper = prTextHarfBuzzCreate();
    PrFontStyle style = prFontStyleDefault();
    style.size = 12;
    PrTextFontDesc desc{.set = *fonts, .style = style};
    return prTextFontCreate(&desc);
}

// Makes the texts for frames that declare `rows` rows, with one more item than the rows show.
void makeTexts(Work* w, uint32_t rows) {
    w->rows = rows;
    for (uint32_t i = 0; i < rows + 1; ++i) {
        w->labels.push_back("Entity " + std::to_string(i) + " of the scene");
        w->values.push_back(std::to_string(i * 37 % 1000) + "." + std::to_string(i % 10) + " ms");
    }
    for (uint32_t i = 0; i < kButtons; ++i)
        w->buttons.push_back("Tool " + std::to_string(i));
}

// Makes a tree and its first frame.
void open(Work* w, PrTextFont* font, PrTextShaper* shaper) {
    PrLayoutTreeDesc desc{.font = font, .shaper = shaper, .targetPixelRatio = 2};
    w->tree = prLayoutTreeCreate(&desc);
    w->styles = makeStyles(w->tree);
    w->first = 0;
    w->hovered = w->rows;
    w->changed = ~0u;
    w->keyed = true;
    w->scene = nullptr;
    declare(w);
    finish(w, true);
}

void close(Work* w) {
    prLayoutSceneRelease(w->scene);
    prLayoutTreeDestroy(w->tree);
    w->tree = nullptr;
}

// A case: what changes before each frame, and whether the frame is painted.
struct Case {
    const char* name;
    void (*change)(Work* w);
    bool paint;
};

void nothing(Work*) {}

void hover(Work* w) {
    w->hovered = w->hovered == w->rows / 2 ? w->rows / 2 + 1 : w->rows / 2;
}

void text(Work* w) {
    w->changed = w->changed == w->rows / 2 ? ~0u : w->rows / 2;
}

void scroll(Work* w) {
    w->first = w->first == 0 ? 1 : 0;
}

constexpr Case kCases[] = {
    {"unchanged", nothing, true},
    {"unchanged unpainted", nothing, false},
    {"hover", hover, true},
    {"text", text, true},
    {"scroll", scroll, true},
};

// The tree sizes, as the number of rows that a frame declares.
constexpr uint32_t kSizes[] = {160, 1600, 16000};

} // namespace

int main(int argc, char** argv) {
    prCrashHandler();
    bool verify = false;
    if (argc >= 2 && strcmp(argv[1], "--verify") == 0) {
        verify = true;
        --argc;
        ++argv;
    }
    if (argc > 2) {
        fprintf(stderr, "usage: ui_bench [--verify] [case]\n");
        return 2;
    }
    PrStringView only = argc == 2 ? argv[1] : PrStringView{};
    PrFontSet* fonts = nullptr;
    PrTextShaper* shaper = nullptr;
    PrTextFont* font = makeFont(&fonts, &shaper);
    bool first = true;
    bool failed = false;
    printf("[");
    for (uint32_t rows : kSizes) {
        uint32_t nodes = 3 + 2 * kButtons + rows * kRowNodes;
        Work w{};
        makeTexts(&w, rows);
        open(&w, font, shaper);
        auto report = [&](const char* label, const HarnessCounters& cost) {
            std::string name = std::to_string(nodes) + " nodes " + label;
            PrLayoutTreeStats stats = prLayoutTreeStats(w.tree);
            PrTextBuffer row{};
            harnessAppendFormat(&row, "%s\n  {\"name\": \"%s\", \"time\": %.6f, \"nodes\": %u, \"kept\": %u, \"made\": %u, "
                "\"gone\": %u, \"relinked\": %u, \"blocks\": %u, \"lines\": %u, \"layers\": %u, "
                "\"commands\": %u, \"flex laid out\": %u, \"measured\": %u, \"handoffs\": %u, "
                "\"per node\": %.1f", first ? "" : ",", name.c_str(), cost.millis, nodes, stats.frameNodesKept,
                stats.frameNodesMade, stats.frameNodesGone, stats.frameParentsRelinked,
                stats.blocksLaidOut, stats.linesLaidOut, stats.layersRecorded,
                stats.commandsRecorded, stats.flexLaidOut, stats.measuresComputed,
                stats.handoffsTaken, cost.instructions / nodes);
            HarnessCounts counts{};
            harnessCountsAdd(&counts, "time", cost);
            harnessCountsWrite(&counts, &row);
            prTextBufferAppend(&row, "}");
            fwrite(row.data, 1, row.size, stdout);
            prTextBufferReset(&row);
            first = false;
        };
        for (const Case& c : kCases) {
            std::string name = std::to_string(nodes) + " nodes " + c.name;
            if (only.data != nullptr && only != name)
                continue;
            auto run = [&] {
                c.change(&w);
                declare(&w);
                finish(&w, c.paint);
            };
            if (only.data != nullptr) {
                harnessRepeat(run);
            }
            HarnessCounters cost = harnessMeasure(run);
            PrLayoutTreeDifference difference{};
            if (verify && !prLayoutTreeVerify(w.tree, &difference)) {
                PrStringView what = prRawStringView(&difference.what);
                fprintf(stderr, "ui_bench: %s: verify: %.*s\n", name.c_str(), int(what.size), what.data);
                failed = true;
            }
            prRawStringReset(&difference.what);
            report(c.name, cost);
        }
        // A cold frame builds a tree from nothing: every node is made, laid out and recorded.
        std::string coldName = std::to_string(nodes) + " nodes cold";
        if (only.data == nullptr || only == coldName) {
            HarnessCounters cold = harnessMeasure([&] {
                close(&w);
                open(&w, font, shaper);
            });
            report("cold", cold);
        }
        close(&w);
    }
    printf("\n]\n");
    prTextFontRelease(font);
    prTextShaperDestroy(shaper);
    prFontSetDestroy(fonts);
    return failed ? 1 : 0;
}
