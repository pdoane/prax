// Styled text in prUiTextEdit: each style run shows as an inline box, and editing, motion, the selection and the caret
// count in the whole text.

#include "pr/font_set.h"
#include "pr/layout_frame.h"
#include "pr/layout_query.h"
#include "pr/layout_style.h"
#include "pr/layout_tree.h"
#include "pr/text_edit.h"
#include "pr/text_harfbuzz.h"
#include "pr/ui.h"
#include "pr/ui_text_edit.h"

#include "tools/harness/test.h"

#include <string>
#include <vector>

namespace pr_impl {

namespace {

// An edit of a buffer, in a tree over Arimo, with two styles of runs, which each round interns.
struct Styled {
    PrFontSet* fonts = prFontSetCreate(nullptr);
    PrTextShaper* shaper = prTextHarfBuzzCreate();
    PrTextFont* font = nullptr;
    PrLayoutTree* tree = nullptr;
    PrUi* ui = nullptr;
    PrTextEditBuffer* buffer = nullptr;
    PrTextEditHistory* history = nullptr;
    static constexpr PrTextEditStyle bold = 1;
    PrLayoutNode control = kPrLayoutNodeNone;
    PrLayoutNode first = kPrLayoutNodeNone;
    double time = 0;
    uint32_t width = 400;

    PrTextDirection direction = PrTextDirection_Ltr;

    explicit Styled(const char* text, PrTextDirection paragraph = PrTextDirection_Ltr) : direction(paragraph) {
        std::string path = std::string(PR_FONTS_DIR) + "/Arimo.ttf";
        const PrFont* PR_NULLABLE face = nullptr;
        REQUIRE(prFontCreateFromFile(path, 0, nullptr, &face) == PrStatus_Ok);
        PrFontFaceDescriptor descriptor = prFontFaceDescriptorDefault();
        prFontSetAdd(fonts, &descriptor, face);
        prFontRelease(face);
        prFontSetSetDefaultFamily(fonts, prFontFamilyFromName("Arimo"));
        PrFontStyle fontStyle = prFontStyleDefault();
        fontStyle.size = 20;
        PrTextFontDesc fontDesc{.set = fonts, .style = fontStyle};
        font = prTextFontCreate(&fontDesc);
        PrLayoutTreeDesc desc{.font = font, .shaper = shaper, .targetPixelRatio = 1};
        tree = prLayoutTreeCreate(&desc);
        ui = prUiCreate(tree);
        buffer = prTextEditBufferCreate(text);
        history = prTextEditHistoryCreate(buffer);
    }

    ~Styled() {
        prUiDestroy(ui);
        prTextEditHistoryDestroy(history);
        prTextEditBufferDestroy(buffer);
        prLayoutTreeDestroy(tree);
        prTextFontRelease(font);
        prTextShaperDestroy(shaper);
        prFontSetDestroy(fonts);
    }

    // Returns a record of `display` in 20px text. `margin` makes it a record of its own, as a bold style is.
    PrLayoutStyleIndex style(PrLayoutDisplay display, float margin = 0) {
        PrLayoutBoxStyle box = prLayoutBoxStyleDefault();
        box.display = display;
        box.margin[PrRectSide_Left] = prLayoutLengthPx(margin);
        PrLayoutFontStyle fontStyle = prLayoutFontStyleDefault();
        fontStyle.font = font;
        fontStyle.lineHeight = prLayoutLineHeightPx(40);
        fontStyle.direction = direction;
        PrLayoutStyle record = prLayoutStyleDefault();
        record.groups[PrLayoutGroup_Box] = prLayoutBoxStyleIntern(tree, &box);
        record.groups[PrLayoutGroup_Font] = prLayoutFontStyleIntern(tree, &fontStyle);
        return prLayoutStyleIntern(tree, &record);
    }

    void frame() {
        time += 1.0 / 60;
        PrUiFrameDesc desc{
            .time = time,
            .delta = 1.0 / 60,
            .extent = PrVec2{800, 600},
            .maxRounds = 4,
            .display = prAppDisplayDefault(),
        };
        prUiFrame(ui, &desc, [](void* context, PrUi*, const PrUiRound*) {
            Styled* s = static_cast<Styled*>(context);
            PrLayoutStyleIndex block = s->style(PrLayoutDisplay_Block);
            const PrLayoutStyleIndex styles[] = {s->style(PrLayoutDisplay_Inline), s->style(PrLayoutDisplay_Inline, 1)};
            PrLayoutNode root = prLayoutTreeFrameElement(s->tree, kPrLayoutNodeNone, 0, block);
            s->control = prUiElement(s->ui, root, 0, block, PrUiFlag_Pressable | PrUiFlag_Focusable);
            if (prUiFocus(s->ui) != s->control)
                prUiSetFocus(s->ui, s->control);
            PrUiTextEditDesc edit{
                .control = s->control,
                .parent = s->control,
                .buffer = s->buffer,
                .styles = styles,
                .styleCount = 2,
                .flags = PrUiTextEditFlag_Multiline,
            };
            s->first = prUiTextEdit(s->ui, &edit);
        }, this);
    }

    void settle() {
        frame();
        frame();
    }

    void key(PrAppKey key, PrAppModifierFlags modifiers = 0) {
        PrAppEvent down{.kind = PrAppEventKind_KeyDown, .modifiers = modifiers, .key = key};
        down.time = time;
        prUiPushEvent(ui, &down);
        frame();
    }

    void type(const char* text) {
        PrAppEvent event{.kind = PrAppEventKind_Text, .text = text};
        event.time = time;
        prUiPushEvent(ui, &event);
        settle();
    }

    PrUiTextSelection selection() { return prUiTextEditSelection(ui, control); }

    void select(uint32_t anchor, uint32_t caret) {
        prUiTextEditSetSelection(ui, control, PrUiTextSelection{anchor, caret});
        settle();
    }

    std::string text() {
        PrStringView view = prTextEditBufferText(buffer);
        return std::string(view.data, view.size);
    }

    // Returns the text of the node after `at` in the control's next run.
    std::vector<std::string> shownRuns() {
        std::vector<std::string> shown;
        for (PrLayoutNode box = prLayoutNodeFirstChild(tree, control); box != kPrLayoutNodeNone;
                box = prLayoutNodeNextSibling(tree, box)) {
            PrLayoutNode node = prLayoutNodeFirstChild(tree, box);
            PrStringView part = prLayoutNodeText(tree, node);
            shown.push_back(std::string(part.data, part.size));
        }
        return shown;
    }
};

using Strings = std::vector<std::string>;

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("each style run of a buffer shows as an inline box of its style, with its text") {
    Styled s("hello world");
    prTextEditBufferSetStyle(s.buffer, 6, 11, s.bold);
    s.settle();
    CHECK(s.shownRuns() == Strings{"hello ", "world"});
    prTextEditBufferSetStyle(s.buffer, 0, 2, s.bold);
    s.settle();
    CHECK(s.shownRuns() == Strings{"he", "llo ", "world"});
}

TEST_CASE("the arrows count in the whole text across runs, with one press for each place") {
    Styled s("abcdef");
    prTextEditBufferSetStyle(s.buffer, 3, 6, s.bold);
    s.settle();
    s.select(0, 0);
    for (uint32_t expected = 1; expected <= 6; ++expected) {
        s.key(PrAppKey_Right);
        s.frame();
        CHECK(s.selection().caret == expected);
    }
    for (uint32_t expected = 6; expected-- > 0;) {
        s.key(PrAppKey_Left);
        s.frame();
        CHECK(s.selection().caret == expected);
    }
}

TEST_CASE("typing joins the run that it follows, and Backspace across runs removes them as they empty") {
    Styled s("abcdef");
    prTextEditBufferSetStyle(s.buffer, 3, 6, s.bold);
    s.settle();
    s.select(3, 3);
    s.type("x");
    CHECK(s.text() == "abcxdef");
    CHECK(s.shownRuns() == Strings{"abcx", "def"});
    s.select(7, 7);
    for (uint32_t i = 0; i < 3; ++i) {
        s.key(PrAppKey_Backspace);
        s.frame();
    }
    s.settle();
    CHECK(s.text() == "abcx");
    CHECK(prTextEditBufferStyleRunCount(s.buffer) == 0);
}

TEST_CASE("the selection is a highlight in each run that it reaches") {
    Styled s("abcdef");
    prTextEditBufferSetStyle(s.buffer, 3, 6, s.bold);
    s.settle();
    s.select(1, 5);
    PrLayoutNode firstBox = prLayoutNodeFirstChild(s.tree, s.control);
    PrLayoutNode secondBox = prLayoutNodeNextSibling(s.tree, firstBox);
    PrLayoutHighlight highlight{};
    REQUIRE(prLayoutNodeHighlights(s.tree, prLayoutNodeFirstChild(s.tree, firstBox), &highlight, 1) == 1);
    CHECK(highlight.start == 1);
    CHECK(highlight.end == 3);
    REQUIRE(prLayoutNodeHighlights(s.tree, prLayoutNodeFirstChild(s.tree, secondBox), &highlight, 1) == 1);
    CHECK(highlight.start == 0);
    CHECK(highlight.end == 2);
}

TEST_CASE("a style set through the view shows, undoes, and waits at a caret for the next typing") {
    Styled s("abcdef");
    s.settle();
    s.select(2, 4);
    prTextEditViewSetStyle(prUiTextEditView(s.ui, s.control), s.bold);
    s.settle();
    CHECK(s.shownRuns() == Strings{"ab", "cd", "ef"});
    CHECK(prTextEditHistoryUndo(s.history, prUiTextEditView(s.ui, s.control)));
    s.settle();
    CHECK(prTextEditBufferStyleRunCount(s.buffer) == 0);

    s.select(6, 6);
    prTextEditViewSetStyle(prUiTextEditView(s.ui, s.control), s.bold);
    s.type("x");
    CHECK(s.text() == "abcdefx");
    CHECK(s.shownRuns() == Strings{"abcdef", "x"});
}

TEST_CASE("a press lands in the run under it, and counts in the whole text") {
    Styled s("abcdef");
    prTextEditBufferSetStyle(s.buffer, 3, 6, s.bold);
    s.settle();
    PrLayoutNode secondBox = prLayoutNodeNextSibling(s.tree, prLayoutNodeFirstChild(s.tree, s.control));
    PrRect box = prLayoutRectToPx(prLayoutNodeBounds(s.tree, secondBox, PrLayoutCoordinates_Viewport));
    PrAppEvent down{.kind = PrAppEventKind_PointerDown, .position = PrVec2{box.right - 1, box.top + 5}};
    down.time = s.time;
    prUiPushEvent(s.ui, &down);
    s.frame();
    CHECK(s.selection().caret == 6);
}

TEST_CASE("in right-to-left text, Left runs forward across a run boundary, and a selection reaches both runs") {
    // Three Hebrew letters of two bytes each in each run.
    Styled s("\xd7\x90\xd7\x91\xd7\x92\xd7\x93\xd7\x94\xd7\x95", PrTextDirection_Rtl);
    prTextEditBufferSetStyle(s.buffer, 6, 12, s.bold);
    s.settle();
    s.select(0, 0);
    for (uint32_t expected = 2; expected <= 12; expected += 2) {
        s.key(PrAppKey_Left);
        s.frame();
        CHECK(s.selection().caret == expected);
    }
    s.key(PrAppKey_Home);
    s.frame();
    CHECK(s.selection().caret == 0);
    s.key(PrAppKey_End);
    s.frame();
    CHECK(s.selection().caret == 12);
}
