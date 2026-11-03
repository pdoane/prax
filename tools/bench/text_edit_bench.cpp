// The cost of editing text at each scale that lib/text_edit supports: edits at the caret, edits far apart, and what a
// transaction carries through a change.
//
// ./dev.sh bench text-edit runs this and prints its rows. The cost of an edit near the last one should not depend on
// the size of the document, so a row of those cases that grows with the document is a defect.
//
// Usage: text_edit_bench [case]
//
// If a case is named, only that case runs, repeatedly, for five minutes (harnessRepeat). This is for attaching a
// profiler.

#include "pr/assert.h"
#include "pr/font_set.h"
#include "pr/layout_style.h"
#include "pr/layout_tree.h"
#include "pr/string_view.h"
#include "pr/text_edit.h"
#include "pr/text_edit_layout.h"
#include "pr/text_edit_motion.h"
#include "pr/text_harfbuzz.h"

#include "tools/harness/harness.h"

#include <stdio.h>
#include <string>

using namespace harness;

namespace {

// The numbers of lines of the documents.
constexpr uint32_t kLines[] = {100, 10000, 1000000};

// How many lines a far edit moves, as a fraction of the document.
constexpr uint32_t kFarDivisor = 3;

// The number of anchors in the case that carries them.
constexpr uint32_t kCarried = 1000;

// The number of style runs in the case that carries them.
constexpr uint32_t kStyled = 1000;

// Returns a document of `lines` lines of different text.
std::string makeDocument(uint32_t lines) {
    std::string text;
    for (uint32_t i = 0; i < lines; ++i) {
        text += "    const value";
        text += std::to_string(i);
        text += " = compute(alpha, beta) + table[index] * 42; // note\n";
    }
    return text;
}

// The state that the cases work on: a buffer with a view whose caret is in the middle of the text.
struct Work {
    PrTextEditBuffer* buffer;
    PrTextEditView* view;
    uint32_t middle;
    uint32_t size;
    bool toggled;
};

void placeCaret(Work* w, uint32_t at) {
    PrTextEditSelection caret{.anchor = at, .caret = at};
    prTextEditViewSetSelections(w->view, &caret, 1);
}

// Types a character and removes it, at the caret.
void typeAtCaret(Work* w) {
    prTextEditViewInsert(w->view, "x", PrTextEditOrigin_Local, PrTextEditMerge_None);
    uint32_t caret = prTextEditViewSelections(w->view)[0].caret;
    PrTextEditChange remove{.start = caret - 1, .end = caret};
    prTextEditViewApply(w->view, &remove, 1, PrTextEditOrigin_Local, PrTextEditMerge_None);
}

// Inserts a line feed and removes it, at the caret, which changes the line index.
void splitLine(Work* w) {
    prTextEditViewInsert(w->view, "\n", PrTextEditOrigin_Local, PrTextEditMerge_None);
    uint32_t caret = prTextEditViewSelections(w->view)[0].caret;
    PrTextEditChange remove{.start = caret - 1, .end = caret};
    prTextEditViewApply(w->view, &remove, 1, PrTextEditOrigin_Local, PrTextEditMerge_None);
}

// Edits a place a third of the document away from the last edit.
void editFar(Work* w) {
    uint32_t at = w->toggled ? w->middle : w->middle - w->middle / kFarDivisor;
    w->toggled = !w->toggled;
    PrTextEditChange insert{.start = at, .end = at, .text = "x"};
    prTextEditBufferApply(w->buffer, &insert, 1, PrTextEditOrigin_Local);
    PrTextEditChange remove{.start = at, .end = at + 1};
    prTextEditBufferApply(w->buffer, &remove, 1, PrTextEditOrigin_Local);
}

// Answers where an offset is, as a click or a diagnostic does.
void findPoint(Work* w) {
    w->toggled = !w->toggled;
    PrTextEditPoint point = prTextEditBufferPointFromOffset(w->buffer, w->toggled ? w->middle : w->middle / 2);
    volatile uint32_t start = prTextEditBufferOffsetFromPoint(w->buffer, point);
    (void)start;
}

struct Case {
    const char* name;
    void (*run)(Work*);
};

// The numbers of lines of the documents that a tree shows whole, for the cost of a motion through layout. A motion's
// cost follows what the tree shows, so a window of rows holds it down at larger scales.
constexpr uint32_t kShownLines[] = {100, 10000};

// Measures the motions of a caret in the middle of `document`, shown in one text node, and writes their rows.
void measureMotion(const std::string& document, uint32_t lines, bool& first) {
    PrFontSet* fonts = prFontSetCreate(nullptr);
    PrTextShaper* shaper = prTextHarfBuzzCreate();
    const PrFont* PR_NULLABLE face = nullptr;
    std::string path = std::string(PR_FONTS_DIR) + "/Arimo.ttf";
    if (prFontCreateFromFile(path, 0, nullptr, &face) != PrStatus_Ok)
        PR_FATAL("cannot open Arimo");
    PrFontFaceDescriptor descriptor = prFontFaceDescriptorDefault();
    prFontSetAdd(fonts, &descriptor, face);
    prFontRelease(face);
    prFontSetSetDefaultFamily(fonts, prFontFamilyFromName("Arimo"));
    PrTextFontDesc fontDesc{.set = fonts, .style = prFontStyleDefault()};
    PrTextFont* font = prTextFontCreate(&fontDesc);
    PrLayoutTreeDesc treeDesc{.font = font, .shaper = shaper, .targetPixelRatio = 1};
    PrLayoutTree* tree = prLayoutTreeCreate(&treeDesc);
    PrLayoutFontStyle fontStyle = prLayoutFontStyleDefault();
    fontStyle.font = font;
    fontStyle.whiteSpaceCollapse = PrLayoutWhiteSpaceCollapse_Preserve;
    PrLayoutBoxStyle box = prLayoutBoxStyleDefault();
    box.display = PrLayoutDisplay_Block;
    PrLayoutStyle record = prLayoutStyleDefault();
    record.groups[PrLayoutGroup_Box] = prLayoutBoxStyleIntern(tree, &box);
    record.groups[PrLayoutGroup_Font] = prLayoutFontStyleIntern(tree, &fontStyle);
    PrLayoutNode root = prLayoutNodeCreateElement(tree, kPrLayoutNodeNone, prLayoutStyleIntern(tree, &record));
    PrLayoutNode text = prLayoutNodeCreateText(tree, root, document);
    PrLayoutRoot viewport{root, prLayoutVecFromPx(PrVec2{800, 600})};
    prLayoutTreeLayout(tree, &viewport, 1);
    PrTextEditSelection caret{.anchor = uint32_t(document.size() / 2), .caret = uint32_t(document.size() / 2)};
    // A press makes its own geometry, whose answers last only as long as it does.
    auto report = [&](const char* label, PrTextEditMotion motion) {
        std::string name = std::to_string(lines) + " lines " + label;
        HarnessCounters cost = harnessMeasure([&] {
            PrTextEditGeometry geometry = prTextEditGeometryFromLayoutText(tree, text, document);
            prTextEditSelectionMove(&geometry, text, caret, motion, PrTextEditSelectionMove_Collapse);
            prTextEditGeometryReset(&geometry);
        });
        harnessWriteCase(first, name.c_str(), cost);
        first = false;
    };
    report("motion logical", PrTextEditMotion_ClusterAfter);
    report("motion visual", PrTextEditMotion_Right);
    report("motion next line", PrTextEditMotion_LineNext);
    prLayoutTreeDestroy(tree);
    prTextFontRelease(font);
    prTextShaperDestroy(shaper);
    prFontSetDestroy(fonts);
}

constexpr Case kCases[] = {
    {"type at caret", typeAtCaret},
    {"split line", splitLine},
    {"edit far", editFar},
    {"find point", findPoint},
};

} // namespace

int main(int argc, char** argv) {
    prCrashHandler();
    if (argc > 2) {
        fprintf(stderr, "usage: text_edit_bench [case]\n");
        return 2;
    }
    PrStringView only = argc == 2 ? argv[1] : PrStringView{};
    bool first = true;
    if (only.size == 0)
        printf("[");
    for (uint32_t lines : kLines) {
        std::string document = makeDocument(lines);
        Work w{};
        w.buffer = prTextEditBufferCreate(document);
        w.view = prTextEditViewCreate(w.buffer);
        w.size = uint32_t(document.size());
        w.middle = prTextEditBufferLineStart(w.buffer, lines / 2);
        placeCaret(&w, w.middle);
        auto report = [&](const char* label, const HarnessCounters& cost) {
            std::string name = std::to_string(lines) + " lines " + label;
            harnessWriteCase(first, name.c_str(), cost);
            first = false;
        };
        for (const Case& c : kCases) {
            std::string name = std::to_string(lines) + " lines " + c.name;
            if (only.size != 0) {
                if (only == PrStringView{name})
                    harnessRepeat([&] { c.run(&w); });
                continue;
            }
            report(c.name, harnessMeasure([&] { c.run(&w); }));
        }
        if (only.size == 0) {
            // What a transaction carries is proportional to what is attached, so this row has a fixed amount of it.
            PrTextEditAnchor anchors[kCarried];
            for (uint32_t i = 0; i < kCarried; ++i)
                anchors[i] = prTextEditAnchorCreate(w.buffer, w.size * i / kCarried, PrTextEditBias_Left);
            report("type with anchors", harnessMeasure([&] { typeAtCaret(&w); }));
            for (PrTextEditAnchor anchor : anchors)
                prTextEditAnchorRemove(w.buffer, anchor);

            // What a keystroke costs follows the number of runs, so this row has a fixed number of them.
            for (uint32_t i = 0; i < kStyled; ++i) {
                uint32_t at = w.size / kStyled * i;
                prTextEditBufferSetStyle(w.buffer, at, at + 1, 1);
            }
            report("type with style runs", harnessMeasure([&] { typeAtCaret(&w); }));
            prTextEditBufferSetStyle(w.buffer, 0, w.size, 0);

            PrTextEditHistory* history = prTextEditHistoryCreate(w.buffer);
            report("type and undo", harnessMeasure([&] {
                prTextEditViewInsert(w.view, "x", PrTextEditOrigin_Local, PrTextEditMerge_None);
                prTextEditHistoryUndo(history, w.view);
                prTextEditHistoryClear(history);
            }));
            prTextEditHistoryDestroy(history);

            report("open", harnessMeasure([&] { prTextEditBufferDestroy(prTextEditBufferCreate(document)); }));
        }
        prTextEditViewDestroy(w.view);
        prTextEditBufferDestroy(w.buffer);
    }
    for (uint32_t lines : kShownLines) {
        if (only.size == 0)
            measureMotion(makeDocument(lines), lines, first);
    }
    if (only.size == 0)
        printf("\n]\n");
    return 0;
}
