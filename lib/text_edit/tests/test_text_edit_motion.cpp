// Motion: tables of cases per script and writing mode, against text's own lines and against real layout, in flat text
// and in a tree of nodes.

#include "pr/array.h"
#include "pr/font_set.h"
#include "pr/layout_style.h"
#include "pr/layout_tree.h"
#include "pr/span.h"
#include "pr/string.h"
#include "pr/text_edit_layout.h"
#include "pr/text_edit_motion.h"
#include "pr/text_harfbuzz.h"

#include "tools/harness/test.h"

namespace pr_impl {

namespace {

// Returns the offsets as text, each followed by a space.
String shownOf(Span<const uint32_t> offsets) {
    String shown;
    for (uint32_t offset : offsets)
        formatTo(shown, "{} ", offset);
    return shown;
}

// Each key of a walk until the caret stops, as the offsets that it stands at.
Array<uint32_t> walk(const PrTextEditGeometry& g, PrTextEditPosition at, PrTextEditMotion motion) {
    Array<uint32_t> offsets;
    for (uint32_t i = 0; i < 32; ++i) {
        float goal = 0;
        bool hasGoal = false;
        PrTextEditPosition next = prTextEditMotionMove(&g, at, motion, &goal, &hasGoal);
        if (next.node == at.node && next.offset == at.offset && next.affinity == at.affinity)
            break;
        at = next;
        offsets.push(at.offset);
    }
    return offsets;
}

// Returns true if a walk stands at the offsets `expected`, in order.
bool walks(const PrTextEditGeometry& g, PrTextEditPosition at, PrTextEditMotion motion, Span<const uint32_t> expected) {
    return walk(g, at, motion).span() == expected;
}

// A tree over Noto Sans Arabic and Arimo in an 800 by 600 px viewport.
struct Fixture {
    PrFontSet* fonts = prFontSetCreate(nullptr);
    PrTextShaper* shaper = prTextHarfBuzzCreate();
    PrTextFont* font = nullptr;
    PrLayoutTree* tree = nullptr;
    PrLayoutNode root = kPrLayoutNodeNone;

    Fixture() {
        String path = format("{}/Arimo-Regular.ttf", PR_FONTS_DIR);
        const PrFont* PR_NULLABLE face = nullptr;
        REQUIRE(prFontCreateFromFile(path, 0, nullptr, &face) == PrStatus_Ok);
        PrFontFaceDescriptor descriptor = prFontFaceDescriptorDefault();
        prFontSetAdd(fonts, &descriptor, face);
        prFontRelease(face);
        String arabicPath = format("{}/NotoSansArabic[wdth,wght].ttf", PR_FONTS_DIR);
        REQUIRE(prFontCreateFromFile(arabicPath, 0, nullptr, &face) == PrStatus_Ok);
        prFontSetAdd(fonts, &descriptor, face);
        prFontRelease(face);
        prFontSetSetDefaultFamily(fonts, prFontFamilyFromName("Arimo"));
        PrFontStyle style = prFontStyleDefault();
        style.size = 20;
        PrFontFamily arabic = prFontFamilyFromName("Noto Sans Arabic");
        PrTextFontDesc fontDesc{.set = fonts, .families = &arabic, .familyCount = 1, .style = style};
        font = prTextFontCreate(&fontDesc);
        PrLayoutTreeDesc desc{.font = font, .shaper = shaper, .targetPixelRatio = 1};
        tree = prLayoutTreeCreate(&desc);
    }

    ~Fixture() {
        prLayoutTreeDestroy(tree);
        prTextFontRelease(font);
        prTextShaperDestroy(shaper);
        prFontSetDestroy(fonts);
    }

    PrLayoutNode element(PrLayoutNode parent, const PrLayoutBoxStyle& box,
            const PrLayoutFontStyle* PR_NULLABLE fontStyle = nullptr) {
        PrLayoutStyle record = prLayoutStyleDefault();
        record.groups[PrLayoutGroup_Box] = prLayoutBoxStyleIntern(tree, &box);
        if (fontStyle != nullptr)
            record.groups[PrLayoutGroup_Font] = prLayoutFontStyleIntern(tree, fontStyle);
        PrLayoutNode node = prLayoutNodeCreateElement(tree, parent, prLayoutStyleIntern(tree, &record));
        if (parent == kPrLayoutNodeNone)
            root = node;
        return node;
    }

    PrLayoutNode text(PrLayoutNode parent, const char* value) { return prLayoutNodeCreateText(tree, parent, value); }

    void layout() {
        PrLayoutRoot viewport{root, prLayoutVecFromPx(PrVec2{800, 600})};
        prLayoutTreeLayout(tree, &viewport, 1);
    }

    // Returns the font style of 20px text on 40px lines.
    PrLayoutFontStyle fontStyle() const {
        PrLayoutFontStyle style = prLayoutFontStyleDefault();
        style.font = font;
        style.lineHeight = prLayoutLineHeightPx(40);
        return style;
    }
};

PrLayoutBoxStyle blockBox() {
    PrLayoutBoxStyle box = prLayoutBoxStyleDefault();
    box.display = PrLayoutDisplay_Block;
    return box;
}

// A block of 20px text on 40px lines, with its text in one node, which keeps white space.
struct Shown {
    Fixture f;
    PrLayoutNode root = kPrLayoutNodeNone;
    PrLayoutNode text = kPrLayoutNodeNone;
    PrTextEditGeometry geometry{};

    Shown(const char* value, PrTextDirection direction = PrTextDirection_Ltr,
            PrLayoutWritingMode mode = PrLayoutWritingMode_HorizontalTb, float width = 400) {
        PrLayoutFontStyle font = f.fontStyle();
        font.whiteSpaceCollapse = PrLayoutWhiteSpaceCollapse_Preserve;
        font.direction = direction;
        font.writingMode = mode;
        PrLayoutBoxStyle box = blockBox();
        box.width = prLayoutLengthPx(width);
        root = f.element(kPrLayoutNodeNone, box, &font);
        text = f.text(root, value);
        f.layout();
        geometry = prTextEditGeometryFromLayoutText(f.tree, text, value);
    }

    ~Shown() { prTextEditGeometryReset(&geometry); }

    PrTextEditPosition at(uint32_t offset, PrTextEditAffinity affinity = PrTextEditAffinity_Downstream) const {
        return PrTextEditPosition{text, offset, affinity};
    }
};

// Latin with a Hebrew word at its end: a space, then three letters of two bytes each.
const char* const kMixed = "abc \xd7\x90\xd7\x91\xd7\x92";

// A Hebrew word of four letters.
const char* const kHebrew = "\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d";

// An Arabic word of four letters, whose second and third join as a ligature in fonts that have one.
const char* const kArabic = "\xd8\xb3\xd9\x84\xd8\xa7\xd9\x85";

// Two Thai clusters, each a consonant, a vowel and a tone mark, of three bytes each.
const char* const kThai = "\xe0\xb8\x97\xe0\xb8\xb5\xe0\xb9\x88\xe0\xb8\x99\xe0\xb8\xb5\xe0\xb9\x88";

// Three Han characters.
const char* const kHan = "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e";

// A tree of a paragraph of two nodes, the second in an inline box, and a block after it.
struct Tree {
    Fixture f;
    PrLayoutNode root = kPrLayoutNodeNone;
    PrLayoutNode first = kPrLayoutNodeNone;
    PrLayoutNode second = kPrLayoutNodeNone;
    PrLayoutNode third = kPrLayoutNodeNone;
    PrTextEditGeometry geometry{};

    Tree(const char* a, const char* b, const char* c, PrTextDirection direction = PrTextDirection_Ltr,
            PrLayoutWritingMode mode = PrLayoutWritingMode_HorizontalTb) {
        PrLayoutFontStyle font = f.fontStyle();
        font.direction = direction;
        font.writingMode = mode;
        PrLayoutBoxStyle box = blockBox();
        box.width = prLayoutLengthPx(400);
        root = f.element(kPrLayoutNodeNone, box, &font);
        PrLayoutNode paragraph = f.element(root, blockBox(), &font);
        first = f.text(paragraph, a);
        PrLayoutBoxStyle span = prLayoutBoxStyleDefault();
        span.display = PrLayoutDisplay_Inline;
        second = f.text(f.element(paragraph, span, &font), b);
        third = f.text(f.element(root, blockBox(), &font), c);
        f.layout();
        geometry = prTextEditGeometryFromLayout(f.tree, root);
    }

    ~Tree() { prTextEditGeometryReset(&geometry); }
};

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a logical motion runs through the text in order, and a stopping motion stops at a change of direction") {
    Shown s(kMixed);
    const uint32_t clusters[] = {1, 2, 3, 4, 6, 8, 10};
    CHECK(walks(s.geometry, s.at(0), PrTextEditMotion_ClusterAfter, clusters));
    const uint32_t clustersBack[] = {8, 6, 4, 3, 2, 1, 0};
    CHECK(walks(s.geometry, s.at(10), PrTextEditMotion_ClusterBefore, clustersBack));
    const uint32_t stops[] = {1, 2, 3, 4, 4, 6, 8, 10};
    CHECK(walks(s.geometry, s.at(0), PrTextEditMotion_StopAfter, stops));
    const uint32_t stopsBack[] = {8, 6, 4, 4, 3, 2, 1, 0};
    CHECK(walks(s.geometry, s.at(10), PrTextEditMotion_StopBefore, stopsBack));

    // At the boundary, a logical motion stands with the Latin, and a stop reaches each side in turn.
    float goal = 0;
    bool hasGoal = false;
    PrTextEditPosition at = s.at(3);
    at = prTextEditMotionMove(&s.geometry, at, PrTextEditMotion_ClusterAfter, &goal, &hasGoal);
    CHECK(at.affinity == PrTextEditAffinity_Upstream);
    at = prTextEditMotionMove(&s.geometry, s.at(3), PrTextEditMotion_StopAfter, &goal, &hasGoal);
    CHECK(at.affinity == PrTextEditAffinity_Upstream);
    at = prTextEditMotionMove(&s.geometry, at, PrTextEditMotion_StopAfter, &goal, &hasGoal);
    CHECK(at.offset == 4);
    CHECK(at.affinity == PrTextEditAffinity_Downstream);
}

TEST_CASE("an arrow along a line moves as its option says, by the way the paragraph runs") {
    struct Case {
        PrTextDirection direction;
        PrTextEditCommand command;
        PrTextEditArrowMotion arrows;
        PrTextEditMotion motion;
    };
    const Case cases[] = {
        {PrTextDirection_Ltr, PrTextEditCommand_Right, PrTextEditArrowMotion_Logical, PrTextEditMotion_ClusterAfter},
        {PrTextDirection_Ltr, PrTextEditCommand_Left, PrTextEditArrowMotion_Logical, PrTextEditMotion_ClusterBefore},
        {PrTextDirection_Rtl, PrTextEditCommand_Left, PrTextEditArrowMotion_Logical, PrTextEditMotion_ClusterAfter},
        {PrTextDirection_Rtl, PrTextEditCommand_Right, PrTextEditArrowMotion_Logical, PrTextEditMotion_ClusterBefore},
        {PrTextDirection_Rtl, PrTextEditCommand_Left, PrTextEditArrowMotion_LogicalStops, PrTextEditMotion_StopAfter},
        {PrTextDirection_Ltr, PrTextEditCommand_Left, PrTextEditArrowMotion_LogicalStops, PrTextEditMotion_StopBefore},
        {PrTextDirection_Rtl, PrTextEditCommand_Left, PrTextEditArrowMotion_Visual, PrTextEditMotion_Left},
        {PrTextDirection_Ltr, PrTextEditCommand_Right, PrTextEditArrowMotion_Visual, PrTextEditMotion_Right},
    };
    for (const Case& c : cases) {
        Shown s(kHebrew, c.direction);
        CHECK(prTextEditMotionFromCommand(&s.geometry, s.at(0), c.command, c.arrows) == c.motion);
    }
}

TEST_CASE("the ends of a line in mixed text are its ends in the order of the text") {
    Shown s("abc \xd7\x90\xd7\x91\xd7\x92");
    float goal = 0;
    bool hasGoal = false;
    CHECK(prTextEditMotionMove(&s.geometry, s.at(1), PrTextEditMotion_LineRight, &goal, &hasGoal).offset == 10);
    CHECK(prTextEditMotionMove(&s.geometry, s.at(6), PrTextEditMotion_LineLeft, &goal, &hasGoal).offset == 0);
    CHECK(prTextEditMotionMove(&s.geometry, s.at(6), PrTextEditMotion_LineEnd, &goal, &hasGoal).offset == 10);
}

TEST_CASE("a visual motion walks a line in the order that it shows, in each script") {
    struct Case {
        const char* name;
        const char* text;
        PrTextDirection direction;
        uint32_t start;
        PrTextEditMotion motion;
        Span<const uint32_t> expected;
    };
    static constexpr uint32_t kLatin[] = {1, 2, 3};
    static constexpr uint32_t kLatinBack[] = {2, 1, 0};
    static constexpr uint32_t kForward[] = {2, 4, 6, 8};
    static constexpr uint32_t kBack[] = {6, 4, 2, 0};
    static constexpr uint32_t kThaiClusters[] = {9, 18};
    static constexpr uint32_t kHanCharacters[] = {3, 6, 9};
    static constexpr uint32_t kMixedForward[] = {1, 2, 3, 4, 8, 6, 4};
    static constexpr uint32_t kMixedEnd[] = {6, 8, 10, 3, 2, 1, 0};
    const Case cases[] = {
        {"latin", "abc", PrTextDirection_Ltr, 0, PrTextEditMotion_Right, kLatin},
        {"latin back", "abc", PrTextDirection_Ltr, 3, PrTextEditMotion_Left, kLatinBack},
        {"hebrew runs leftward", kHebrew, PrTextDirection_Rtl, 0, PrTextEditMotion_Left, kForward},
        {"hebrew back", kHebrew, PrTextDirection_Rtl, 8, PrTextEditMotion_Right, kBack},
        {"arabic stops inside a ligature", kArabic, PrTextDirection_Rtl, 0, PrTextEditMotion_Left, kForward},
        {"thai moves by cluster", kThai, PrTextDirection_Ltr, 0, PrTextEditMotion_Right, kThaiClusters},
        {"han", kHan, PrTextDirection_Ltr, 0, PrTextEditMotion_Right, kHanCharacters},
        {"mixed across a level", kMixed, PrTextDirection_Ltr, 0, PrTextEditMotion_Right, kMixedForward},
        {"mixed reaches the text's end", kMixed, PrTextDirection_Ltr, 4, PrTextEditMotion_Left, kMixedEnd},
    };
    for (const Case& c : cases) {
        INFO(c.name);
        Shown s(c.text, c.direction);
        Array<uint32_t> walked = walk(s.geometry, s.at(c.start), c.motion);
        INFO(shownOf(walked.span()));
        CHECK(walked.span() == c.expected);
    }
}

TEST_CASE("the ends of a line are visual or by the paragraph's direction") {
    Shown s(kHebrew, PrTextDirection_Rtl);
    float goal = 0;
    bool hasGoal = false;
    auto move = [&](PrTextEditMotion motion) {
        return prTextEditMotionMove(&s.geometry, s.at(4), motion, &goal, &hasGoal).offset;
    };
    CHECK(move(PrTextEditMotion_LineStart) == 0);
    CHECK(move(PrTextEditMotion_LineEnd) == 8);
    CHECK(move(PrTextEditMotion_LineLeft) == 8);
    CHECK(move(PrTextEditMotion_LineRight) == 0);
}

TEST_CASE("a word motion runs the way that its side runs in the paragraph") {
    const char* text = "\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d \xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d";
    Shown s(text, PrTextDirection_Rtl);
    float goal = 0;
    bool hasGoal = false;
    CHECK(prTextEditMotionMove(&s.geometry, s.at(0), PrTextEditMotion_WordLeft, &goal, &hasGoal).offset == 8);
    CHECK(prTextEditMotionMove(&s.geometry, s.at(8), PrTextEditMotion_WordRight, &goal, &hasGoal).offset == 0);
}

TEST_CASE("a press past a soft wrap moves past its offset, and a line's end is a place of its own") {
    Shown s("aaaa bbbb", PrTextDirection_Ltr, PrLayoutWritingMode_HorizontalTb, 60);
    const uint32_t rightward[] = {4, 5, 6, 7, 8, 9};
    CHECK(walks(s.geometry, s.at(3), PrTextEditMotion_Right, rightward));
    const uint32_t leftward[] = {6, 5, 4, 3, 2, 1, 0};
    CHECK(walks(s.geometry, s.at(7), PrTextEditMotion_Left, leftward));
}

TEST_CASE("motion between lines keeps its goal, and ends at the edge of the text") {
    Shown s("abcdef\nab\nabcdef");
    float goal = 0;
    bool hasGoal = false;
    PrTextEditPosition at = prTextEditMotionMove(&s.geometry, s.at(5), PrTextEditMotion_LineNext, &goal, &hasGoal);
    CHECK(hasGoal);
    CHECK(at.offset == 9);
    at = prTextEditMotionMove(&s.geometry, at, PrTextEditMotion_LineNext, &goal, &hasGoal);
    CHECK(at.offset == 15);
    at = prTextEditMotionMove(&s.geometry, at, PrTextEditMotion_LineNext, &goal, &hasGoal);
    CHECK(at.offset == 16);
}

TEST_CASE("an arrow means what the writing mode makes it") {
    struct Case {
        PrLayoutWritingMode mode;
        PrTextEditCommand command;
        PrTextEditMotion motion;
    };
    const Case cases[] = {
        {PrLayoutWritingMode_HorizontalTb, PrTextEditCommand_Left, PrTextEditMotion_Left},
        {PrLayoutWritingMode_HorizontalTb, PrTextEditCommand_Down, PrTextEditMotion_LineNext},
        {PrLayoutWritingMode_HorizontalTb, PrTextEditCommand_EdgeUp, PrTextEditMotion_TextStart},
        {PrLayoutWritingMode_VerticalRl, PrTextEditCommand_Down, PrTextEditMotion_Right},
        {PrLayoutWritingMode_VerticalRl, PrTextEditCommand_Left, PrTextEditMotion_LineNext},
        {PrLayoutWritingMode_VerticalRl, PrTextEditCommand_Right, PrTextEditMotion_LinePrevious},
        {PrLayoutWritingMode_VerticalRl, PrTextEditCommand_WordDown, PrTextEditMotion_WordRight},
        {PrLayoutWritingMode_VerticalRl, PrTextEditCommand_EdgeUp, PrTextEditMotion_LineLeft},
        {PrLayoutWritingMode_VerticalRl, PrTextEditCommand_EdgeLeft, PrTextEditMotion_TextEnd},
        {PrLayoutWritingMode_VerticalLr, PrTextEditCommand_Right, PrTextEditMotion_LineNext},
        {PrLayoutWritingMode_SidewaysRl, PrTextEditCommand_Left, PrTextEditMotion_LineNext},
        {PrLayoutWritingMode_SidewaysLr, PrTextEditCommand_Up, PrTextEditMotion_Right},
        {PrLayoutWritingMode_SidewaysLr, PrTextEditCommand_Right, PrTextEditMotion_LineNext},
    };
    for (const Case& c : cases) {
        CAPTURE(uint32_t(c.mode));
        CAPTURE(uint32_t(c.command));
        Shown s(kHan, PrTextDirection_Ltr, c.mode);
        CHECK(prTextEditMotionFromCommand(&s.geometry, s.at(0), c.command, PrTextEditArrowMotion_Visual) == c.motion);
    }
}

TEST_CASE("vertical text moves along its lines and between them") {
    Shown s("\xe6\x97\xa5\xe6\x9c\xac\n\xe8\xaa\x9e", PrTextDirection_Ltr, PrLayoutWritingMode_VerticalRl);
    const uint32_t expected[] = {3, 6, 7, 10};
    CHECK(walks(s.geometry, s.at(0), PrTextEditMotion_Right, expected));
    float goal = 0;
    bool hasGoal = false;
    CHECK(prTextEditMotionMove(&s.geometry, s.at(0), PrTextEditMotion_LineNext, &goal, &hasGoal).offset == 7);
}

TEST_CASE("Backspace takes one code point, and a sequence that means one thing whole") {
    struct Case {
        const char* name;
        const char* text;
        uint32_t expected;
    };
    const Case cases[] = {
        {"latin with a mark", "e\xcc\x81", 1},
        {"hebrew with a point", "\xd7\xa9\xd6\xb8", 2},
        {"arabic with a mark", "\xd8\xa8\xd9\x8e", 2},
        {"syriac with a mark", "\xdc\x90\xdc\xb2", 2},
        {"thai with two marks", "\xe0\xb8\x97\xe0\xb8\xb5\xe0\xb9\x88", 6},
        {"devanagari conjunct", "\xe0\xa4\x95\xe0\xa5\x8d\xe0\xa4\x95", 6},
        {"balinese with a vowel", "\xe1\xac\x93\xe1\xac\xb8", 3},
        {"hangul jamo", "\xe1\x84\x92\xe1\x85\xa1\xe1\x86\xab", 6},
        {"han", "\xe6\x97\xa5", 0},
        {"han with a variation selector", "\xe8\x91\x9b\xf3\xa0\x84\x80", 0},
        {"emoji with a selector", "\xe2\x9d\xa4\xef\xb8\x8f", 0},
        {"emoji with a skin tone", "\xf0\x9f\x91\x8d\xf0\x9f\x8f\xbd", 0},
        {"an emoji sequence", "\xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x92\xbb", 0},
        {"a flag", "\xf0\x9f\x87\xaf\xf0\x9f\x87\xb5", 0},
        {"a keycap", "1\xef\xb8\x8f\xe2\x83\xa3", 0},
    };
    for (const Case& c : cases) {
        INFO(c.name);
        PrStringView text = c.text;
        CHECK(prTextEditBackspaceStart(text, uint32_t(text.size)) == c.expected);
    }
}

TEST_CASE("empty lines and the end of shown text are places for a caret") {
    Shown s("a\n\nb\n ");
    PrTextEditGeometry flat = prTextEditGeometryFromLayoutText(s.f.tree, s.text, "a\n\nb\n");
    const uint32_t expected[] = {1, 2, 3, 4, 5};
    CHECK(walks(flat, s.at(0), PrTextEditMotion_Right, expected));
    float goal = 0;
    bool hasGoal = false;
    CHECK(prTextEditMotionMove(&flat, s.at(0), PrTextEditMotion_LineNext, &goal, &hasGoal).offset == 2);
    prTextEditGeometryReset(&flat);
}

TEST_CASE("a selection collapses toward the side that the motion goes") {
    PrStringView text = "abcdef";
    PrTextEditGeometry ltr = prTextEditGeometryFromText(text, PrTextDirection_Ltr);
    PrTextEditGeometry rtl = prTextEditGeometryFromText(text, PrTextDirection_Rtl);
    PrTextEditSelection s{.anchor = 1, .caret = 4};
    CHECK(prTextEditSelectionMove(&ltr, 0, s, PrTextEditMotion_Left, PrTextEditSelectionMove_Collapse).caret == 1);
    CHECK(prTextEditSelectionMove(&ltr, 0, s, PrTextEditMotion_Right, PrTextEditSelectionMove_Collapse).caret == 4);
    CHECK(prTextEditSelectionMove(&rtl, 0, s, PrTextEditMotion_Left, PrTextEditSelectionMove_Collapse).caret == 4);
    PrTextEditSelection extended =
        prTextEditSelectionMove(&ltr, 0, s, PrTextEditMotion_Right, PrTextEditSelectionMove_Extend);
    CHECK(extended.anchor == 1);
    CHECK(extended.caret == 5);
    prTextEditGeometryReset(&ltr);
    prTextEditGeometryReset(&rtl);
}

TEST_CASE("a collapsing motion between lines moves from the end of the selection that it goes toward") {
    PrStringView text = "abc\ndef\nghi";
    PrTextEditGeometry g = prTextEditGeometryFromText(text, PrTextDirection_Ltr);
    PrTextEditSelection s{.anchor = 1, .caret = 5};
    CHECK(prTextEditSelectionMove(&g, 0, s, PrTextEditMotion_LineNext, PrTextEditSelectionMove_Collapse).caret == 9);
    CHECK(prTextEditSelectionMove(&g, 0, s, PrTextEditMotion_LinePrevious, PrTextEditSelectionMove_Collapse).caret == 0);
    prTextEditGeometryReset(&g);
}

TEST_CASE("text's own lines serve when no layout shows it") {
    PrStringView text = "ab\ncd";
    PrTextEditGeometry g = prTextEditGeometryFromText(text, PrTextDirection_Ltr);
    PrTextEditPosition at{0, 1, PrTextEditAffinity_Downstream};
    const uint32_t expected[] = {2, 3, 4, 5};
    CHECK(walks(g, at, PrTextEditMotion_Right, expected));
    float goal = 0;
    bool hasGoal = false;
    CHECK(prTextEditMotionMove(&g, at, PrTextEditMotion_LineNext, &goal, &hasGoal).offset == 4);
    prTextEditGeometryReset(&g);
}

TEST_CASE("a layout that is out of date falls back on the text's own lines, in the direction that its style gives") {
    Shown s("ab");
    PrTextEditGeometry stale = prTextEditGeometryFromLayoutText(s.f.tree, s.text, "abc");
    const uint32_t expected[] = {3};
    CHECK(walks(stale, s.at(2), PrTextEditMotion_ClusterAfter, expected));
    prTextEditGeometryReset(&stale);

    Shown hebrew(kHebrew, PrTextDirection_Rtl);
    String longer = format("{}\xd7\x90", kHebrew);
    PrTextEditGeometry rtl = prTextEditGeometryFromLayoutText(hebrew.f.tree, hebrew.text, longer);
    CHECK(prTextEditMotionFromCommand(&rtl, hebrew.at(8), PrTextEditCommand_Left, PrTextEditArrowMotion_Logical) ==
        PrTextEditMotion_ClusterAfter);
    prTextEditGeometryReset(&rtl);
}

TEST_CASE("in a tree of nodes, Left and Right cross nodes with one press for each place") {
    Tree t("foo", "bar", "baz");
    PrTextEditPosition at{t.first, 2, PrTextEditAffinity_Downstream};
    float goal = 0;
    bool hasGoal = false;
    auto right = [&]() {
        at = prTextEditMotionMove(&t.geometry, at, PrTextEditMotion_Right, &goal, &hasGoal);
    };
    right();
    CHECK(at.node == t.first);
    CHECK(at.offset == 3);
    right();
    CHECK(at.node == t.second);
    CHECK(at.offset == 1);
    right();
    right();
    CHECK(at.node == t.second);
    CHECK(at.offset == 3);
    right();
    CHECK(at.node == t.third);
    CHECK(at.offset == 0);
    at = prTextEditMotionMove(&t.geometry, at, PrTextEditMotion_Left, &goal, &hasGoal);
    CHECK(at.node == t.second);
    CHECK(at.offset == 3);
}

TEST_CASE("in a tree of nodes, a word spans the nodes of a paragraph, and stops at a block") {
    Tree t("foo", "bar baz", "qux");
    float goal = 0;
    bool hasGoal = false;
    PrTextEditPosition at{t.first, 0, PrTextEditAffinity_Downstream};
    at = prTextEditMotionMove(&t.geometry, at, PrTextEditMotion_WordAfter, &goal, &hasGoal);
    CHECK(at.node == t.second);
    CHECK(at.offset == 3);
    at = prTextEditMotionMove(&t.geometry, at, PrTextEditMotion_WordAfter, &goal, &hasGoal);
    CHECK(at.node == t.second);
    CHECK(at.offset == 7);
    at = prTextEditMotionMove(&t.geometry, at, PrTextEditMotion_WordAfter, &goal, &hasGoal);
    CHECK(at.node == t.third);
    CHECK(at.offset == 3);
    at = prTextEditMotionMove(&t.geometry, at, PrTextEditMotion_WordBefore, &goal, &hasGoal);
    CHECK(at.node == t.third);
    CHECK(at.offset == 0);
    at = prTextEditMotionMove(&t.geometry, at, PrTextEditMotion_WordBefore, &goal, &hasGoal);
    CHECK(at.node == t.second);
    CHECK(at.offset == 4);
}

TEST_CASE("in a tree of nodes, motion between lines and to the text's edges reaches every node") {
    Tree t("aaa", "bbb", "ccc");
    float goal = 0;
    bool hasGoal = false;
    PrTextEditPosition at{t.first, 1, PrTextEditAffinity_Downstream};
    PrTextEditPosition below = prTextEditMotionMove(&t.geometry, at, PrTextEditMotion_LineNext, &goal, &hasGoal);
    CHECK(below.node == t.third);
    CHECK(below.offset == 1);
    PrTextEditPosition end = prTextEditMotionMove(&t.geometry, at, PrTextEditMotion_TextEnd, &goal, &hasGoal);
    CHECK(end.node == t.third);
    CHECK(end.offset == 3);
    PrTextEditPosition start = prTextEditMotionMove(&t.geometry, below, PrTextEditMotion_TextStart, &goal, &hasGoal);
    CHECK(start.node == t.first);
    CHECK(start.offset == 0);
    PrTextEditPosition lineEnd = prTextEditMotionMove(&t.geometry, at, PrTextEditMotion_LineEnd, &goal, &hasGoal);
    CHECK(lineEnd.node == t.second);
    CHECK(lineEnd.offset == 3);
}

TEST_CASE("in a tree of nodes, a visual motion walks each script through every node") {
    struct Case {
        const char* name;
        const char* text;
        PrTextDirection direction;
        PrLayoutWritingMode mode;
        PrTextEditMotion motion;
        Span<const uint32_t> expected;
    };
    static constexpr uint32_t kLatinNodes[] = {1, 2, 3, 1, 2, 3, 0, 1, 2, 3};
    static constexpr uint32_t kRtlNodes[] = {2, 4, 6, 8, 2, 4, 6, 8, 0, 2, 4, 6, 8};
    static constexpr uint32_t kThaiNodes[] = {9, 18, 9, 18, 0, 9, 18};
    static constexpr uint32_t kHanNodes[] = {3, 6, 9, 3, 6, 9, 0, 3, 6, 9};
    const Case cases[] = {
        {"latin", "abc", PrTextDirection_Ltr, PrLayoutWritingMode_HorizontalTb, PrTextEditMotion_Right, kLatinNodes},
        {"arabic", kArabic, PrTextDirection_Rtl, PrLayoutWritingMode_HorizontalTb, PrTextEditMotion_Left, kRtlNodes},
        {"hebrew", kHebrew, PrTextDirection_Rtl, PrLayoutWritingMode_HorizontalTb, PrTextEditMotion_Left, kRtlNodes},
        {"thai", kThai, PrTextDirection_Ltr, PrLayoutWritingMode_HorizontalTb, PrTextEditMotion_Right, kThaiNodes},
        {"han", kHan, PrTextDirection_Ltr, PrLayoutWritingMode_HorizontalTb, PrTextEditMotion_Right, kHanNodes},
        {"vertical han", kHan, PrTextDirection_Ltr, PrLayoutWritingMode_VerticalRl, PrTextEditMotion_Right, kHanNodes},
    };
    for (const Case& c : cases) {
        INFO(c.name);
        Tree t(c.text, c.text, c.text, c.direction, c.mode);
        PrTextEditPosition start{t.first, 0, PrTextEditAffinity_Downstream};
        Array<uint32_t> walked = walk(t.geometry, start, c.motion);
        INFO(shownOf(walked.span()));
        CHECK(walked.span() == c.expected);
    }
}

TEST_CASE("in a tree of nodes in Hebrew, Left runs forward through the text") {
    Tree t(kHebrew, kHebrew, kHebrew, PrTextDirection_Rtl);
    float goal = 0;
    bool hasGoal = false;
    PrTextEditPosition at{t.first, 6, PrTextEditAffinity_Downstream};
    at = prTextEditMotionMove(&t.geometry, at, PrTextEditMotion_Left, &goal, &hasGoal);
    CHECK(at.node == t.first);
    CHECK(at.offset == 8);
    at = prTextEditMotionMove(&t.geometry, at, PrTextEditMotion_Left, &goal, &hasGoal);
    CHECK(at.node == t.second);
    CHECK(at.offset == 2);
}
