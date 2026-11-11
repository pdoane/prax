// The windowed view over documents of many lines, through real layout: the window stays small, heights settle, and
// motion and edits count in the whole document.

#include "pr/font_set.h"
#include "pr/layout_frame.h"
#include "pr/layout_query.h"
#include "pr/layout_style.h"
#include "pr/layout_tree.h"
#include "pr/text_harfbuzz.h"
#include "pr/text_view.h"

#include "tools/harness/test.h"

#include <string>
#include <utility>
#include <vector>

extern "C" int prTestTextViewCApi(void);

namespace pr_impl {

namespace {

// A view of a text in a tree over one font, Arimo unless a test names another, in a port of 800 by 400 px.
struct Shown {
    PrFontSet* fonts = prFontSetCreate(nullptr);
    PrTextShaper* shaper = prTextHarfBuzzCreate();
    PrTextFont* font = nullptr;
    PrLayoutTree* tree = nullptr;
    PrTextEditBuffer* buffer = nullptr;
    PrTextView* view = nullptr;
    PrLayoutNode port = kPrLayoutNodeNone;
    bool wrap = true;
    float width = 800;
    float height = 400;
    PrLayoutWritingMode writingMode = PrLayoutWritingMode_HorizontalTb;
    PrTextDirection direction = PrTextDirection_Ltr;
    PrLayoutStyleIndex styleRecords[2] = {};    // the records of styles zero and one, if `styled` is true
    bool styled = false;

    // Spans that the test gives each frame, in red.
    std::vector<std::pair<uint32_t, uint32_t>> spanned;
    uint32_t spanCalls = 0;

    // Spans of a second layer, underlined in blue.
    std::vector<std::pair<uint32_t, uint32_t>> underlined;

    // A gutter of this size shows each line's number, and records the lines that it was declared for.
    float gutterSize = 0;
    std::vector<uint32_t> gutterLines;

    bool colorLines = false;        // even lines are filled with green

    explicit Shown(const std::string& text, const std::string& family = "Arimo") {
        std::string path = std::string(PR_FONTS_DIR) + "/" + family + "-Regular.ttf";
        const PrFont* PR_NULLABLE face = nullptr;
        REQUIRE(prFontCreateFromFile(path, 0, nullptr, &face) == PrStatus_Ok);
        PrFontFaceDescriptor descriptor = prFontFaceDescriptorDefault();
        prFontSetAdd(fonts, &descriptor, face);
        prFontRelease(face);
        prFontSetSetDefaultFamily(fonts, prFontFamilyFromName(family));
        PrFontStyle style = prFontStyleDefault();
        style.size = 16;
        PrTextFontDesc fontDesc{.set = fonts, .style = style};
        font = prTextFontCreate(&fontDesc);
        PrLayoutTreeDesc desc{.font = font, .shaper = shaper, .targetPixelRatio = 1};
        tree = prLayoutTreeCreate(&desc);
        buffer = prTextEditBufferCreate(text);
        view = prTextViewCreate(buffer);
    }

    ~Shown() {
        prTextViewDestroy(view);
        prTextEditBufferDestroy(buffer);
        prLayoutTreeDestroy(tree);
        prTextFontRelease(font);
        prTextShaperDestroy(shaper);
        prFontSetDestroy(fonts);
    }

    PrLayoutStyleIndex style(bool row) {
        PrLayoutBoxStyle box = prLayoutBoxStyleDefault();
        box.display = PrLayoutDisplay_Block;
        if (!row) {
            box.width = prLayoutLengthPx(width);
            box.height = prLayoutLengthPx(height);
            box.overflowX = PrLayoutOverflow_Hidden;
            box.overflowY = PrLayoutOverflow_Hidden;
        }
        PrLayoutFontStyle fontStyle = prLayoutFontStyleDefault();
        fontStyle.font = font;
        fontStyle.lineHeight = prLayoutLineHeightPx(20);
        fontStyle.whiteSpaceCollapse = PrLayoutWhiteSpaceCollapse_Preserve;
        fontStyle.textWrapMode = wrap ? PrLayoutTextWrapMode_Wrap : PrLayoutTextWrapMode_Nowrap;
        fontStyle.overflowWrap = PrTextOverflowWrap_Anywhere;
        fontStyle.writingMode = writingMode;
        fontStyle.direction = direction;
        PrLayoutStyle record = prLayoutStyleDefault();
        record.groups[PrLayoutGroup_Box] = prLayoutBoxStyleIntern(tree, &box);
        record.groups[PrLayoutGroup_Font] = prLayoutFontStyleIntern(tree, &fontStyle);
        return prLayoutStyleIntern(tree, &record);
    }

    // The record of an inline box that shows a run, which differs from another only by its color.
    PrLayoutStyleIndex runStyle(PrColor color) {
        PrLayoutBoxStyle box = prLayoutBoxStyleDefault();
        box.display = PrLayoutDisplay_Inline;
        PrLayoutColorStyle look = prLayoutColorStyleDefault();
        look.color = prColor16FromColor(color);
        PrLayoutStyle record = prLayoutStyleDefault();
        record.groups[PrLayoutGroup_Box] = prLayoutBoxStyleIntern(tree, &box);
        record.groups[PrLayoutGroup_Color] = prLayoutColorStyleIntern(tree, &look);
        return prLayoutStyleIntern(tree, &record);
    }

    // Runs a frame of the view, and returns whether another would show something different.
    bool frame() {
        prLayoutTreeBeginFrame(tree);
        // The root does not scroll, so the port is under it.
        PrLayoutNode top = prLayoutTreeFrameElement(tree, kPrLayoutNodeNone, 0, style(true));
        port = prLayoutTreeFrameElement(tree, top, 0, style(false));
        if (styled) {
            styleRecords[0] = runStyle(PrColor{0, 0, 0, 1});
            styleRecords[1] = runStyle(PrColor{1, 0, 0, 1});
        }
        PrTextViewSpanLayer layers[] = {{spansOf, this}, {underlinesOf, this}};
        PrTextViewFrameDesc desc{
            .tree = tree,
            .parent = port,
            .extent = PrVec2{width, height},
            .rowStyle = style(true),
            .styles = styled ? styleRecords : nullptr,
            .styleCount = styled ? 2u : 0u,
            .spanLayers = spanned.empty() && underlined.empty() ? nullptr : layers,
            .spanLayerCount = spanned.empty() && underlined.empty() ? 0u : 2u,
            .gutter = gutterSize > 0 ? gutterOf : nullptr,
            .gutterContext = this,
            .gutterSize = gutterSize,
            .lineColor = colorLines ? lineColorOf : nullptr,
            .lineColorContext = this,
            .wrap = wrap,
            .showCaret = true,
        };
        prTextViewFrame(view, &desc);
        PrLayoutRoot root{top, prLayoutVecFromPx(PrVec2{width, height})};
        prLayoutTreeLayout(tree, &root, 1);
        // Every key of a frame names one node.
        CHECK(prLayoutTreeDuplicateKeys(tree, nullptr, 0) == 0);
        return prTextViewLaidOut(view);
    }

    static uint32_t spansOf(void* context, uint32_t start, uint32_t end, PrTextViewSpan* out, uint32_t capacity) {
        Shown* shown = static_cast<Shown*>(context);
        shown->spanCalls += 1;
        uint32_t count = 0;
        for (const auto& [from, to] : shown->spanned) {
            if (to <= start || from >= end)
                continue;
            if (count < capacity)
                out[count] = PrTextViewSpan{.start = from, .end = to, .color = prColor16FromColor(PrColor{1, 0, 0, 1})};
            ++count;
        }
        return count;
    }

    static uint32_t underlinesOf(void* context, uint32_t start, uint32_t end, PrTextViewSpan* out, uint32_t capacity) {
        Shown* shown = static_cast<Shown*>(context);
        uint32_t count = 0;
        for (const auto& [from, to] : shown->underlined) {
            if (to <= start || from >= end)
                continue;
            if (count < capacity) {
                out[count] = PrTextViewSpan{
                    .start = from,
                    .end = to,
                    .underline = prColor16FromColor(PrColor{0, 0, 1, 1}),
                    .underlineStyle = PrDrawDecorationStyle_Wavy,
                };
            }
            ++count;
        }
        return count;
    }

    static void gutterOf(void* context, PrLayoutTree* tree, PrLayoutNode cell, uint32_t line) {
        Shown* shown = static_cast<Shown*>(context);
        shown->gutterLines.push_back(line);
        std::string number = std::to_string(line + 1);
        prLayoutTreeFrameText(tree, cell, 0, number);
    }

    static PrColor16 lineColorOf(void*, uint32_t line) {
        return line % 2 == 0 ? prColor16FromColor(PrColor{0, 1, 0, 1}) : PrColor16{};
    }

    // Runs frames until the view has settled.
    void settle() {
        for (uint32_t i = 0; i < 8 && frame(); ++i) {
        }
    }

    PrTextEditPoint caret() { return prTextViewInfo(view).caret; }

    std::string text() {
        PrStringView all = prTextEditBufferText(buffer);
        return std::string(all.data, all.size);
    }
};

// A document of `count` lines of different text.
std::string document(uint32_t count) {
    std::string text;
    for (uint32_t i = 0; i < count; ++i)
        text += "line " + std::to_string(i) + (i + 1 < count ? "\n" : "");
    return text;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("the public header compiles as C") {
    CHECK(prTestTextViewCApi() == 0);
}

TEST_CASE("the window holds the lines near the viewport, however long the document") {
    Shown s(document(200000));
    s.settle();
    PrTextViewInfo info = prTextViewInfo(s.view);
    CHECK(info.lineCount == 200000);
    CHECK(info.rowCount > 20);
    CHECK(info.rowCount < 100);
    CHECK(info.documentSize == doctest::Approx(200000.0 * 20).epsilon(0.01));
}

TEST_CASE("scrolling far shows the lines there, and stops at the document's ends") {
    Shown s(document(200000));
    s.settle();
    prTextViewScrollTo(s.view, 2000000);
    s.settle();
    PrTextViewInfo info = prTextViewInfo(s.view);
    CHECK(info.offset == doctest::Approx(2000000.0).epsilon(0.001));
    CHECK(info.rowCount < 100);
    prTextViewScrollTo(s.view, 1e12);
    s.settle();
    info = prTextViewInfo(s.view);
    CHECK(info.offset == doctest::Approx(info.documentSize - 400).epsilon(0.001));
    prTextViewScrollTo(s.view, -5);
    s.settle();
    CHECK(prTextViewInfo(s.view).offset == 0);
}

TEST_CASE("the height of a wrapped line becomes exact once it is in the window, and the scroll position holds its text") {
    std::string longLine(4000, 'x');
    std::string text = document(50) + "\n" + longLine + "\n" + document(50);
    Shown s(text);
    s.settle();
    double before = prTextViewInfo(s.view).documentSize;
    CHECK(before > 100 * 20);
    prTextViewScrollTo(s.view, 40 * 20);
    s.settle();
    double measured = prTextViewInfo(s.view).documentSize;
    // 4000 of Arimo's 16px x at 8px each is 32000 px, on 40 lines of 800 px.
    CHECK(measured == doctest::Approx(101 * 20.0 + 40 * 20.0).epsilon(0.02));
}

TEST_CASE("the arrows count in the document, across lines and past the window") {
    Shown s(document(10000));
    s.settle();
    prTextViewRun(s.view, PrTextEditCommand_EdgeRight, PrTextEditSelectionMove_Collapse);
    s.settle();
    CHECK(s.caret().column == 6);
    prTextViewRun(s.view, PrTextEditCommand_Right, PrTextEditSelectionMove_Collapse);
    s.settle();
    CHECK(s.caret().line == 1);
    CHECK(s.caret().column == 0);
    prTextViewRun(s.view, PrTextEditCommand_Left, PrTextEditSelectionMove_Collapse);
    s.settle();
    CHECK(s.caret().line == 0);
    for (uint32_t i = 0; i < 200; ++i) {
        prTextViewRun(s.view, PrTextEditCommand_Down, PrTextEditSelectionMove_Collapse);
        s.frame();
    }
    s.settle();
    CHECK(s.caret().line == 200);
    PrTextViewInfo info = prTextViewInfo(s.view);
    CHECK(info.offset + 400 >= 200 * 20);
    CHECK(info.offset <= 200 * 20);
}

TEST_CASE("a caret on a line taller than the viewport, far from the window, brings the line and the caret into view") {
    std::string text = document(2000) + "\n" + std::string(20000, 'x') + "\n" + document(2000);
    Shown s(text);
    s.settle();
    uint32_t tall = prTextEditBufferLineEnd(s.buffer, 2000);
    PrTextEditSelection caret{.anchor = tall, .caret = tall};
    prTextEditViewSetSelections(prTextViewEdit(s.view), &caret, 1);
    prTextViewReveal(s.view);
    s.settle();
    PrRect rect{};
    REQUIRE(prTextViewCaretRect(s.view, &rect));
    CHECK(rect.top >= 0);
    CHECK(rect.bottom <= 400);
}

TEST_CASE("the ends of the text and a page move past the window, and the caret comes into view") {
    Shown s(document(100000));
    s.settle();
    prTextViewRun(s.view, PrTextEditCommand_TextEnd, PrTextEditSelectionMove_Collapse);
    s.settle();
    CHECK(s.caret().line == 99999);
    PrTextViewInfo info = prTextViewInfo(s.view);
    CHECK(info.offset == doctest::Approx(info.documentSize - 400).epsilon(0.001));
    prTextViewRunPage(s.view, PrTextViewPage_Up, PrTextEditSelectionMove_Collapse);
    s.settle();
    CHECK(s.caret().line == 99999 - 19);
    prTextViewRun(s.view, PrTextEditCommand_TextStart, PrTextEditSelectionMove_Collapse);
    s.settle();
    CHECK(prTextViewInfo(s.view).offset == 0);
}

TEST_CASE("edits count in the height index, and line breaks and deletions keep the window right") {
    Shown s(document(1000));
    s.settle();
    prTextViewRun(s.view, PrTextEditCommand_EdgeRight, PrTextEditSelectionMove_Collapse);
    prTextViewInsert(s.view, "x", PrTextEditMerge_Typing);
    s.settle();
    CHECK(s.text().substr(0, 7) == "line 0x");
    prTextViewRun(s.view, PrTextEditCommand_InsertLineBreak, PrTextEditSelectionMove_Collapse);
    s.settle();
    CHECK(prTextViewInfo(s.view).lineCount == 1001);
    CHECK(prTextViewInfo(s.view).documentSize == doctest::Approx(1001 * 20.0).epsilon(0.001));
    prTextViewRun(s.view, PrTextEditCommand_DeleteBackward, PrTextEditSelectionMove_Collapse);
    s.settle();
    CHECK(prTextViewInfo(s.view).lineCount == 1000);
    CHECK(s.caret().line == 0);
    CHECK(prTextViewInfo(s.view).documentSize == doctest::Approx(1000 * 20.0).epsilon(0.001));
}

TEST_CASE("an edit that spans many lines keeps the index whole") {
    Shown s(document(30000));
    s.settle();
    PrTextEditChange all{.start = 10, .end = prTextEditBufferSize(s.buffer) - 10, .text = "\n"};
    prTextEditViewApply(prTextViewEdit(s.view), &all, 1, PrTextEditOrigin_Local, PrTextEditMerge_None);
    s.settle();
    PrTextViewInfo info = prTextViewInfo(s.view);
    CHECK(info.lineCount == 3);
    CHECK(info.documentSize == doctest::Approx(3 * 20.0).epsilon(0.01));
    PrTextEditChange grown{.start = 0, .end = 0, .text = "a\nb\nc\nd\n"};
    prTextEditViewApply(prTextViewEdit(s.view), &grown, 1, PrTextEditOrigin_Local, PrTextEditMerge_None);
    s.settle();
    CHECK(prTextViewInfo(s.view).lineCount == 7);
}

TEST_CASE("a press lands on the line under it, and a selection shows as a highlight in each row") {
    Shown s(document(100));
    s.settle();
    uint32_t offset = 0;
    PrTextEditAffinity side = 0;
    REQUIRE(prTextViewOffsetFromPoint(s.view, PrVec2{1, 45}, &offset, &side));
    CHECK(prTextEditBufferPointFromOffset(s.buffer, offset).line == 2);
    prTextViewRun(s.view, PrTextEditCommand_SelectAll, PrTextEditSelectionMove_Collapse);
    PrRawString selected = prTextViewSelectedText(s.view);
    CHECK(prRawStringView(&selected).size == prTextEditBufferSize(s.buffer));
    prRawStringReset(&selected);
}

TEST_CASE("without wrap, a long line is one row, and the caret brings it across") {
    std::string text = std::string(3000, 'x') + "\nend";
    Shown s(text);
    s.wrap = false;
    s.settle();
    CHECK(prTextViewInfo(s.view).documentSize == doctest::Approx(2 * 20.0).epsilon(0.01));
    prTextViewRun(s.view, PrTextEditCommand_EdgeRight, PrTextEditSelectionMove_Collapse);
    s.settle();
    PrRect caret{};
    REQUIRE(prTextViewCaretRect(s.view, &caret));
    CHECK(caret.left >= 0);
    CHECK(caret.left <= 800);
}

TEST_CASE("style runs show as inline boxes in their rows, and offsets count in the line") {
    Shown s("alpha beta\nplain\ngamma");
    s.styled = true;
    prTextEditBufferSetStyle(s.buffer, 2, 8, 1);
    s.settle();
    // Runs do not change where text sets.
    CHECK(prTextViewInfo(s.view).documentSize == doctest::Approx(3 * 20.0));
    // The arrows pass the place where two runs meet once.
    for (uint32_t i = 0; i < 3; ++i) {
        prTextViewRun(s.view, PrTextEditCommand_Right, PrTextEditSelectionMove_Collapse);
        s.settle();
    }
    CHECK(s.caret().column == 3);
    PrRect first{};
    REQUIRE(prTextViewCaretRect(s.view, &first));
    CHECK(first.left > 0);
    prTextViewRun(s.view, PrTextEditCommand_EdgeRight, PrTextEditSelectionMove_Collapse);
    s.settle();
    CHECK(s.caret().column == 10);

    // A press past the run's start lands in the line, and a selection reaches through the runs.
    uint32_t offset = 0;
    PrTextEditAffinity side = 0;
    REQUIRE(prTextViewOffsetFromPoint(s.view, PrVec2{first.left + 1, 5}, &offset, &side));
    CHECK(offset == 3);
    PrTextEditSelection across{.anchor = 1, .caret = 9};
    prTextEditViewSetSelections(prTextViewEdit(s.view), &across, 1);
    s.settle();
    PrLayoutCaret caret{};
    CHECK(prLayoutTreeCarets(s.tree, &caret, 1) == 0);

    // Typing inside a run keeps its style, and the row follows.
    PrTextEditSelection inside{.anchor = 4, .caret = 4};
    prTextEditViewSetSelections(prTextViewEdit(s.view), &inside, 1);
    prTextViewInsert(s.view, "x", PrTextEditMerge_Typing);
    s.settle();
    CHECK(s.text().substr(0, 11) == "alphx" + std::string("a beta"));
    CHECK(prTextEditBufferStyleAt(s.buffer, 4) == 1);
    PrRect after{};
    REQUIRE(prTextViewCaretRect(s.view, &after));
    CHECK(after.left > first.left);
}

TEST_CASE("every selection moves, types and deletes, and carets that meet join") {
    // In a monospaced font, the goal of a caret keeps its column on each line.
    Shown s("one\ntwo\nthree\nfour", "Cousine");
    s.settle();
    prTextViewRun(s.view, PrTextEditCommand_EdgeRight, PrTextEditSelectionMove_Collapse);
    s.settle();
    prTextViewAddCaretOnLine(s.view, PrTextEditLineStep_Next);
    s.settle();
    prTextViewAddCaretOnLine(s.view, PrTextEditLineStep_Next);
    s.settle();
    CHECK(prTextEditViewSelectionCount(prTextViewEdit(s.view)) == 3);
    CHECK(s.caret().line == 2);
    // Each caret paints.
    CHECK(prLayoutTreeCarets(s.tree, nullptr, 0) == 3);

    prTextViewInsert(s.view, "!", PrTextEditMerge_Typing);
    s.settle();
    CHECK(s.text().substr(0, 9) == "one!\ntwo!");
    prTextViewRun(s.view, PrTextEditCommand_DeleteBackward, PrTextEditSelectionMove_Collapse);
    s.settle();
    CHECK(s.text() == "one\ntwo\nthree\nfour");

    prTextViewRun(s.view, PrTextEditCommand_Left, PrTextEditSelectionMove_Extend);
    s.settle();
    PrRawString selected = prTextViewSelectedText(s.view);
    CHECK(std::string(prRawStringView(&selected).data, prRawStringView(&selected).size) == "e\no\nr");
    prRawStringReset(&selected);
    prTextViewEraseSelection(s.view);
    s.settle();
    CHECK(s.text() == "on\ntw\nthee\nfour");

    // Motions to one place leave one caret.
    prTextViewRun(s.view, PrTextEditCommand_TextStart, PrTextEditSelectionMove_Collapse);
    s.settle();
    CHECK(prTextEditViewSelectionCount(prTextViewEdit(s.view)) == 1);

    prTextViewAddCaret(s.view, 3, PrTextEditAffinity_Downstream);
    s.settle();
    CHECK(prTextEditViewSelectionCount(prTextViewEdit(s.view)) == 2);
    CHECK(s.caret().line == 1);
    prTextViewKeepPrimary(s.view);
    CHECK(prTextEditViewSelectionCount(prTextViewEdit(s.view)) == 1);
    CHECK(s.caret().line == 1);
}

TEST_CASE("deletions at carets side by side remove one range") {
    Shown s("abcdef");
    s.settle();
    PrTextEditSelection carets[2] = {{.anchor = 2, .caret = 2}, {.anchor = 3, .caret = 3}};
    prTextEditViewSetSelections(prTextViewEdit(s.view), carets, 2);
    s.settle();
    prTextViewRun(s.view, PrTextEditCommand_DeleteBackward, PrTextEditSelectionMove_Collapse);
    s.settle();
    CHECK(s.text() == "adef");
}

TEST_CASE("refinement reads the lines outside the window, and the scroll position holds its text") {
    // Two thousand lines of a narrow letter, which take fewer lines of the port than their lengths suggest.
    std::string line(1000, 'x');
    std::string narrow(1000, 'i');
    std::string text;
    for (uint32_t i = 0; i < 2000; ++i)
        text += narrow + "\n";
    text += line;
    Shown s(text);
    s.width = 2000;
    s.settle();
    prTextViewScrollTo(s.view, 20000);
    s.settle();
    PrTextViewInfo before = prTextViewInfo(s.view);
    uint32_t atPoint = 0;
    PrTextEditAffinity side = 0;
    REQUIRE(prTextViewOffsetFromPoint(s.view, PrVec2{1, 5}, &atPoint, &side));
    uint32_t shownBefore = prTextEditBufferPointFromOffset(s.buffer, atPoint).line;

    uint32_t passes = 0;
    while (prTextViewRefine(s.view, 1) && passes < 100000)
        ++passes;
    s.settle();
    PrTextViewInfo after = prTextViewInfo(s.view);
    // 1000 i at about 3.5 px each take two lines of 2000 px, and 1000 x at 8 px each take four.
    CHECK(after.documentSize == doctest::Approx(2000 * 40.0 + 4 * 20.0).epsilon(0.01));
    CHECK(after.documentSize != doctest::Approx(before.documentSize).epsilon(0.01));
    REQUIRE(prTextViewOffsetFromPoint(s.view, PrVec2{1, 5}, &atPoint, &side));
    CHECK(prTextEditBufferPointFromOffset(s.buffer, atPoint).line == shownBefore);
    // There is nothing left to read until the width changes.
    CHECK_FALSE(prTextViewRefine(s.view, 1));
    s.width = 1000;
    s.settle();
    CHECK(prTextViewRefine(s.view, 0));
}

TEST_CASE("a right-to-left view without wrap scrolls toward its line's start and keeps the caret in the port") {
    // A Hebrew line, whose first byte is at the right.
    std::string text;
    for (uint32_t i = 0; i < 1500; ++i)
        text += "\xd7\x90";
    Shown s(text + "\nend");
    s.wrap = false;
    s.direction = PrTextDirection_Rtl;
    s.settle();
    prTextViewRun(s.view, PrTextEditCommand_LineEnd, PrTextEditSelectionMove_Collapse);
    s.settle();
    CHECK(s.caret().column == 3000);
    PrRect caret{};
    REQUIRE(prTextViewCaretRect(s.view, &caret));
    CHECK(caret.left >= 0);
    CHECK(caret.left <= 800);
    CHECK(prLayoutVecToPx(prLayoutNodeScroll(s.tree, s.port)).x < 0);
    prTextViewRun(s.view, PrTextEditCommand_LineStart, PrTextEditSelectionMove_Collapse);
    s.settle();
    REQUIRE(prTextViewCaretRect(s.view, &caret));
    CHECK(caret.left >= 0);
    CHECK(caret.left <= 800);
    CHECK(double(prLayoutVecToPx(prLayoutNodeScroll(s.tree, s.port)).x) == doctest::Approx(0));
}

TEST_CASE("bidi text keeps its offsets through the view") {
    Shown s("abc \xd7\x90\xd7\x91\xd7\x92 def\nnext");
    s.settle();
    for (uint32_t i = 0; i < 5; ++i) {
        prTextViewRun(s.view, PrTextEditCommand_Right, PrTextEditSelectionMove_Collapse);
        s.settle();
    }
    // Logical motion: past the space, then one Hebrew letter of two bytes.
    CHECK(s.caret().column == 6);
    prTextViewRun(s.view, PrTextEditCommand_DeleteBackward, PrTextEditSelectionMove_Collapse);
    s.settle();
    CHECK(s.text().substr(0, 6) == "abc \xd7\x91");
    CHECK(s.caret().column == 4);
}

TEST_CASE("a vertical view stacks its rows along the block axis, and the arrows turn with it") {
    for (PrLayoutWritingMode mode : {PrLayoutWritingMode_VerticalRl, PrLayoutWritingMode_VerticalLr}) {
        bool reversed = mode == PrLayoutWritingMode_VerticalRl;
        Shown s(document(10000));
        s.writingMode = mode;
        s.settle();
        PrTextViewInfo info = prTextViewInfo(s.view);
        CHECK(double(info.portSize) == doctest::Approx(800));
        CHECK(info.documentSize == doctest::Approx(10000.0 * 20).epsilon(0.01));
        CHECK(info.rowCount > 40);
        CHECK(info.rowCount < 200);

        // Scrolling far shows the lines there, as the tree's offset says.
        prTextViewScrollTo(s.view, 40000);
        s.settle();
        CHECK(prTextViewInfo(s.view).offset == doctest::Approx(40000.0).epsilon(0.001));
        float scrolled = prLayoutVecToPx(prLayoutNodeScroll(s.tree, s.port)).x;
        CHECK((reversed ? scrolled < 0 : scrolled > 0));
        uint32_t offset = 0;
        PrTextEditAffinity side = 0;
        REQUIRE(prTextViewOffsetFromPoint(s.view, PrVec2{reversed ? 795.0f : 5.0f, 5}, &offset, &side));
        CHECK(prTextEditBufferPointFromOffset(s.buffer, offset).line == 2000);

        // Along a line is down the screen, and the next line is across it.
        uint32_t lineStart = prTextEditBufferLineStart(s.buffer, 2000);
        PrTextEditSelection start{.anchor = lineStart, .caret = lineStart};
        prTextEditViewSetSelections(prTextViewEdit(s.view), &start, 1);
        s.settle();
        prTextViewRun(s.view, PrTextEditCommand_Down, PrTextEditSelectionMove_Collapse);
        s.settle();
        CHECK(s.caret().line == 2000);
        CHECK(s.caret().column == 1);
        prTextViewRun(s.view, reversed ? PrTextEditCommand_Left : PrTextEditCommand_Right,
            PrTextEditSelectionMove_Collapse);
        s.settle();
        CHECK(s.caret().line == 2001);

        // A caret far off comes into the port.
        prTextViewRun(s.view, PrTextEditCommand_TextEnd, PrTextEditSelectionMove_Collapse);
        s.settle();
        CHECK(s.caret().line == 9999);
        PrRect caret{};
        REQUIRE(prTextViewCaretRect(s.view, &caret));
        CHECK(caret.left >= 0);
        CHECK(caret.right <= 800);
        info = prTextViewInfo(s.view);
        CHECK(info.offset == doctest::Approx(info.documentSize - 800).epsilon(0.001));
    }
}

TEST_CASE("a vertical view wraps at the port's height") {
    Shown s(std::string(4000, 'x') + "\nend");
    s.writingMode = PrLayoutWritingMode_VerticalRl;
    s.settle();
    prTextViewRun(s.view, PrTextEditCommand_LineStart, PrTextEditSelectionMove_Collapse);
    s.settle();
    // 32000 px of text on lines of 400 px.
    CHECK(prTextViewInfo(s.view).documentSize == doctest::Approx(81 * 20.0).epsilon(0.03));
}

TEST_CASE("spans color their text as highlights under the selection's, across runs and lines") {
    Shown s("int value = 42;\nplain line\nreturn value;");
    s.styled = true;
    // A span that crosses the run's end, and one that crosses a line.
    s.spanned = {{0, 3}, {6, 14}, {20, 30}};
    prTextEditBufferSetStyle(s.buffer, 2, 8, 1);
    PrTextEditSelection selection{.anchor = 12, .caret = 13};
    prTextEditViewSetSelections(prTextViewEdit(s.view), &selection, 1);
    s.settle();
    // The view asks once for the whole window in each frame.
    uint32_t calls = s.spanCalls;
    s.frame();
    CHECK(s.spanCalls == calls + 1);
    // The first line has three runs: before the style, the style, and after it. The second run holds bytes 2 to 8.
    PrLayoutNode run = kPrLayoutNodeNone;
    for (PrLayoutNode row =
            prLayoutNodeFirstChild(s.tree, s.port); row != kPrLayoutNodeNone && run == kPrLayoutNodeNone;
            row = prLayoutNodeNextSibling(s.tree, row)) {
        PrLayoutNode second = prLayoutNodeNextSibling(s.tree, prLayoutNodeFirstChild(s.tree, row));
        if (second != kPrLayoutNodeNone)
            run = prLayoutNodeFirstChild(s.tree, second);
    }
    REQUIRE(run != kPrLayoutNodeNone);
    // The run holds bytes 2 to 8: the first span colors its first byte, and the second its last two.
    PrLayoutHighlight marks[4] = {};
    REQUIRE(prLayoutNodeHighlights(s.tree, run, marks, 4) == 2);
    CHECK(marks[0].start == 0);
    CHECK(marks[0].end == 1);
    CHECK(marks[1].start == 4);
    CHECK(marks[1].end == 6);
    CHECK(marks[1].color.a != 0);
    // Spans change no line, so the text sets as it did.
    CHECK(prTextViewInfo(s.view).documentSize == doctest::Approx(3 * 20.0));
    prTextViewRun(s.view, PrTextEditCommand_TextStart, PrTextEditSelectionMove_Collapse);
    s.settle();
    for (uint32_t i = 0; i < 15; ++i) {
        prTextViewRun(s.view, PrTextEditCommand_Right, PrTextEditSelectionMove_Collapse);
        s.settle();
    }
    CHECK(s.caret().column == 15);
}

TEST_CASE("a jump far through a document whose rows hold spans lays out the rows that it reaches") {
    Shown s(document(5000));
    s.styled = true;
    // A span over each word of every line.
    PrStringView all = prTextEditBufferText(s.buffer);
    for (uint32_t i = 0; i < all.size;) {
        while (i < all.size && (all.data[i] == ' ' || all.data[i] == '\n'))
            ++i;
        uint32_t from = i;
        while (i < all.size && all.data[i] != ' ' && all.data[i] != '\n')
            ++i;
        if (from < i)
            s.spanned.push_back({from, i});
    }
    s.settle();
    for (uint32_t jump = 0; jump < 6; ++jump) {
        prTextViewScrollTo(s.view, double(jump % 3) * 30000 + double(jump) * 700);
        s.settle();
        uint32_t offset = 0;
        PrTextEditAffinity side = 0;
        REQUIRE(prTextViewOffsetFromPoint(s.view, PrVec2{1, 5}, &offset, &side));
        prTextViewInsert(s.view, "x", PrTextEditMerge_None);
        s.settle();
    }
}

TEST_CASE("layers of spans paint over each other, and a layer's span may underline its text") {
    Shown s("int value = 42;");
    s.spanned = {{0, 3}};
    s.underlined = {{0, 9}};
    s.settle();
    PrLayoutNode text = prLayoutNodeFirstChild(s.tree, prLayoutNodeFirstChild(s.tree, s.port));
    PrLayoutHighlight marks[4] = {};
    REQUIRE(prLayoutNodeHighlights(s.tree, text, marks, 4) == 2);
    CHECK(marks[0].end == 3);
    CHECK(marks[0].color.a != 0);
    CHECK(marks[0].underline.a == 0);
    CHECK(marks[1].end == 9);
    CHECK(marks[1].underline.a != 0);
    CHECK(marks[1].underlineStyle == PrDrawDecorationStyle_Wavy);
}

TEST_CASE("a gutter holds a cell for each row's line, and the text wraps in the room that it leaves") {
    // A line that fits the port, but not beside the gutter.
    std::string text = document(3) + "\n" + std::string(95, 'x');
    Shown plain(text);
    plain.settle();
    CHECK(prTextViewInfo(plain.view).documentSize == doctest::Approx(4 * 20.0));

    Shown s(text);
    s.gutterSize = 100;
    s.settle();
    CHECK(prTextViewInfo(s.view).documentSize == doctest::Approx(5 * 20.0));
    CHECK(s.gutterLines.size() >= 4);
    // The row holds the cell, at the port's left, and then the text's block, past the gutter.
    PrLayoutNode row = prLayoutNodeFirstChild(s.tree, s.port);
    PrLayoutNode cell = prLayoutNodeFirstChild(s.tree, row);
    PrLayoutNode block = prLayoutNodeNextSibling(s.tree, cell);
    PrRect cellBox = prLayoutRectToPx(prLayoutNodeBounds(s.tree, cell, PrLayoutCoordinates_Layout));
    PrRect blockBox = prLayoutRectToPx(prLayoutNodeBounds(s.tree, block, PrLayoutCoordinates_Layout));
    CHECK(cellBox.left == doctest::Approx(0.0));
    CHECK(cellBox.right == doctest::Approx(100.0));
    CHECK(blockBox.left == doctest::Approx(100.0));
    // A press in the text still finds its line, past the gutter.
    uint32_t offset = 0;
    PrTextEditAffinity side = 0;
    REQUIRE(prTextViewOffsetFromPoint(s.view, PrVec2{101, 25}, &offset, &side));
    CHECK(offset == prTextEditBufferLineStart(s.buffer, 1));
    // Motion between lines keeps its place along them, past the gutter.
    prTextViewRun(s.view, PrTextEditCommand_TextStart, PrTextEditSelectionMove_Collapse);
    prTextViewRun(s.view, PrTextEditCommand_Right, PrTextEditSelectionMove_Collapse);
    s.settle();
    prTextViewRun(s.view, PrTextEditCommand_Down, PrTextEditSelectionMove_Collapse);
    s.settle();
    CHECK(s.caret().line == 1);
    CHECK(s.caret().column == 1);
    prTextViewAddCaretOnLine(s.view, PrTextEditLineStep_Next);
    s.settle();
    CHECK(s.caret().line == 2);
    CHECK(s.caret().column == 1);
    // A press over the gutter finds the start of its line, not the number there.
    REQUIRE(prTextViewOffsetFromPoint(s.view, PrVec2{20, 45}, &offset, &side));
    CHECK(offset == prTextEditBufferLineStart(s.buffer, 2));
}

TEST_CASE("a gutter stays at the port's edge while lines that do not wrap scroll, however far the wheel goes") {
    // The longest line passes the port by 300 px or so, beside the gutter.
    Shown s(document(3) + "\n" + std::string(120, 'x'));
    s.wrap = false;
    s.gutterSize = 100;
    s.colorLines = true;
    s.settle();
    // The rows lie in a sheet, which the port scrolls.
    PrLayoutNode sheet = prLayoutNodeFirstChild(s.tree, s.port);
    PrLayoutNode row = prLayoutNodeFirstChild(s.tree, sheet);
    PrLayoutNode cell = prLayoutNodeFirstChild(s.tree, row);
    PrLayoutNode block = prLayoutNodeNextSibling(s.tree, cell);
    PrRect before = prLayoutRectToPx(prLayoutNodeBounds(s.tree, block, PrLayoutCoordinates_Viewport));
    // A wheel far past the longest line scrolls the text only to its end.
    prTextViewScrollBy(s.view, PrVec2{5000, 0});
    s.settle();
    PrLayoutScrollInfo info{};
    REQUIRE(prLayoutNodeScrollInfo(s.tree, s.port, &info));
    float end = prLayoutRectToPx(info.range).right;
    CHECK(end > 0);
    CHECK(prLayoutVecToPx(info.offset).x == doctest::Approx(end));
    sheet = prLayoutNodeFirstChild(s.tree, s.port);
    row = prLayoutNodeFirstChild(s.tree, sheet);
    cell = prLayoutNodeFirstChild(s.tree, row);
    block = prLayoutNodeNextSibling(s.tree, cell);
    PrRect cellBox = prLayoutRectToPx(prLayoutNodeBounds(s.tree, cell, PrLayoutCoordinates_Viewport));
    PrRect blockBox = prLayoutRectToPx(prLayoutNodeBounds(s.tree, block, PrLayoutCoordinates_Viewport));
    PrRect rowBox = prLayoutRectToPx(prLayoutNodeBounds(s.tree, row, PrLayoutCoordinates_Viewport));
    // The cell stays at the port's left, and the text moves under it by the offset.
    CHECK(cellBox.left == doctest::Approx(0.0));
    CHECK(cellBox.right == doctest::Approx(100.0));
    CHECK(blockBox.left == doctest::Approx(before.left - end));
    // The row's fill reaches the port's right edge wherever the lines scroll.
    CHECK(rowBox.right >= s.width - 0.5f);
    // Scrolling back by the offset returns to the start, with nothing past the end to take back first.
    prTextViewScrollBy(s.view, PrVec2{-end, 0});
    s.settle();
    REQUIRE(prLayoutNodeScrollInfo(s.tree, s.port, &info));
    CHECK(prLayoutVecToPx(info.offset).x == doctest::Approx(0.0));
}

TEST_CASE("a line's color fills its row") {
    Shown s(document(4));
    s.colorLines = true;
    s.settle();
    PrLayoutNode first = prLayoutNodeFirstChild(s.tree, s.port);
    PrLayoutNode second = prLayoutNodeNextSibling(s.tree, first);
    PrLayoutStyle filled = prLayoutStyleRecord(s.tree, prLayoutNodeStyle(s.tree, first));
    PrLayoutStyle empty = prLayoutStyleRecord(s.tree, prLayoutNodeStyle(s.tree, second));
    CHECK(prLayoutDecorationStyleRecord(s.tree, filled.groups[PrLayoutGroup_Decoration]).background.a != 0);
    CHECK(prLayoutDecorationStyleRecord(s.tree, empty.groups[PrLayoutGroup_Decoration]).background.a == 0);
}

TEST_CASE("gaps add space before their lines, and after the last, and the reader's place holds") {
    Shown s(document(5000));
    s.settle();
    double size = prTextViewInfo(s.view).documentSize;
    PrTextViewGap gaps[] = {{0, 30}, {2, 50}, {5000, 10}};
    prTextViewSetGaps(s.view, gaps, 3);
    s.settle();
    CHECK(prTextViewInfo(s.view).documentSize == doctest::Approx(size + 90));
    // Line 0 starts below its gap, and line 2 below both gaps before it.
    PrRect caret{};
    REQUIRE(prTextViewCaretRect(s.view, &caret));
    CHECK(caret.top >= 30);
    prTextViewRun(s.view, PrTextEditCommand_Down, PrTextEditSelectionMove_Collapse);
    prTextViewRun(s.view, PrTextEditCommand_Down, PrTextEditSelectionMove_Collapse);
    s.settle();
    CHECK(s.caret().line == 2);
    REQUIRE(prTextViewCaretRect(s.view, &caret));
    CHECK(caret.top >= 30 + 20 + 20 + 50);

    // A gap above the reader's place moves nothing that they see.
    prTextViewScrollTo(s.view, 50000);
    s.settle();
    double offset = prTextViewInfo(s.view).offset;
    uint32_t at = 0;
    PrTextEditAffinity side = 0;
    REQUIRE(prTextViewOffsetFromPoint(s.view, PrVec2{1, 5}, &at, &side));
    PrTextViewGap more[] = {{0, 30}, {2, 50}, {10, 400}, {5000, 10}};
    prTextViewSetGaps(s.view, more, 4);
    s.settle();
    CHECK(prTextViewInfo(s.view).offset == doctest::Approx(offset + 400));
    uint32_t still = 0;
    REQUIRE(prTextViewOffsetFromPoint(s.view, PrVec2{1, 5}, &still, &side));
    CHECK(still == at);
    prTextViewSetGaps(s.view, nullptr, 0);
    s.settle();
    CHECK(prTextViewInfo(s.view).documentSize == doctest::Approx(size));
}
