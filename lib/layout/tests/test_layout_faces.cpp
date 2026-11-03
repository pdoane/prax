// Text fragments across faces. A text fragment holds the glyphs of one text node on a line, between the boundaries that
// change placement, in whatever faces its clusters fell to. A change of face or script alone therefore divides no leaf
// and no fragment, and moves no glyph. The oracle is the same text divided into text nodes at each change of face,
// which the layout places separately, as every change of face was placed before.

#include "pr/layout_query.h"
#include "pr/layout_style.h"
#include "pr/layout_tree.h"
#include "pr/memory.h"

#include "layout/src/layout_fragment.h"
#include "layout/tests/layout_fixture.h"

#include "doctest/doctest.h"

#include <algorithm>
#include <string>
#include <vector>

namespace pr_impl {

namespace {

// Adds the font file `path` to the fixture's set as family `family`, serving the code points of `ranges` if given.
void addFace(Fixture& f, const std::string& path, const char* family,
        const PrFontCodePointRange* PR_NULLABLE ranges = nullptr, uint32_t count = 0) {
    const PrFont* PR_NULLABLE loaded = nullptr;
    REQUIRE(prFontCreateFromFile(path, 0, nullptr, &loaded) == PrStatus_Ok);
    PrFontFaceDescriptor descriptor = prFontFaceDescriptorDefault();
    descriptor.family = PrStringView{family, strlen(family)};
    descriptor.automatic = PrFontFaceAutoFlags(descriptor.automatic & ~PrFontFaceAutoFlag_Family);
    descriptor.ranges = ranges;
    descriptor.rangeCount = count;
    prFontSetAdd(f.fonts, &descriptor, loaded);
    prFontRelease(loaded);
}

std::string testFont(const char* file) {
    return std::string(PR_FONTS_DIR) + "/" + file;
}

// Text in faces whose metrics differ, at 20px: Arimo for ASCII through the underscore -- capitals, digits, punctuation
// and the space -- and for the Hebrew points and the letters alef to yod; Literata for every other Latin letter; Noto
// Sans Hebrew for the other Hebrew letters; and Noto Sans JP for Han.
PrLayoutFontStyle mixed(Fixture& f) {
    static const PrFontCodePointRange kUpper[] = {{0x0000, 0x005f}, {0x05b0, 0x05c7}, {0x05d0, 0x05d9}};
    addFace(f, testFont("Arimo.ttf"), "Upper", kUpper, 3);
    addFace(f, testFont("Literata.ttf"), "Fallback");
    addFace(f, std::string(PR_FONT_BUNDLE_DIR) + "/NotoSansHebrew[wdth,wght].ttf", "Hebrew");
    addFace(f, testFont("NotoSansJP.ttf"), "Japanese");
    PrLayoutFontStyle font = prLayoutFontStyleDefault();
    font.font = f.textFont(20, {
        prFontFamilyFromName("Upper"), prFontFamilyFromName("Fallback"),
        prFontFamilyFromName("Hebrew"), prFontFamilyFromName("Japanese"),
    });
    font.lineHeight = prLayoutLineHeightPx(40);
    return font;
}

// Returns the family of a font, such as "Arimo". Two layouts' fonts belong to their own fixtures, so they are compared
// by family.
std::string familyOf(const PrFont* font) {
    PrRawString family = prFontFamilyName(font);
    PrStringView view = prRawStringView(&family);
    std::string out(view.data, view.size);
    prRawStringReset(&family);
    return out;
}

// A glyph as painted: its id, its font, the font's family, which outlives the layout, and its origin in the tree's
// space.
struct Painted {
    uint16_t id;
    const PrFont* font;
    std::string family;
    double x;
    double y;
};

// What a paint recorded of its text: the glyphs, the glyph commands, and the quarter turns pushed.
struct Paint {
    std::vector<Painted> glyphs;
    uint32_t commands = 0;
    uint32_t turns = 0;
};

// Appends what `list` draws under `transform`, including through the lists it embeds.
void appendPainted(const PrDrawList* list, PrDAffine2 transform, Paint& out) {
    std::vector<PrDAffine2> stack{transform};
    const PrDrawCommand* commands = prDrawListCommands(list);
    for (uint32_t i = 0; i < prDrawListCommandCount(list); ++i) {
        const PrDrawCommand& command = commands[i];
        const PrDAffine2& t = stack.back();
        switch (command.kind) {
            case PrDrawCommandKind_Embed: appendPainted(command.embed.list, t, out); break;
            case PrDrawCommandKind_PushTransformAffine2:
                stack.push_back(prDAffine2Concat(command.affine, t));
                out.turns += command.affine.a == 0 ? 1 : 0;
                break;
            case PrDrawCommandKind_PopTransform: stack.pop_back(); break;
            case PrDrawCommandKind_Glyphs:
                ++out.commands;
                for (uint32_t g = 0; g < command.glyphs->glyphCount; ++g) {
                    PrVec2 p = command.glyphs->glyphs[g].position;
                    double x = double(p.x);
                    double y = double(p.y);
                    out.glyphs.push_back(Painted{
                        command.glyphs->glyphs[g].id, command.glyphs->font,
                        familyOf(command.glyphs->font), t.a * x + t.c * y + t.e,
                        t.b * x + t.d * y + t.f,
                    });
                }
                break;
            default: break;
        }
    }
}

// A stage `width` px wide, in the mixed faces as `style` changes them, holding a text node for each of `pieces`. It is
// laid out and verified.
struct Laid {
    Fixture f;
    PrLayoutFontStyle font;
    PrLayoutNode stage = kPrLayoutNodeNone;
    std::vector<PrLayoutNode> texts;

    // The stage's box is `box` if given, `width` wide, and its effects are `effects` if given.
    template <typename Style>
    Laid(Style style, std::vector<const char*> pieces, float width, const PrLayoutBoxStyle* PR_NULLABLE box = nullptr,
            const PrLayoutEffectStyle* PR_NULLABLE effects = nullptr) {
        font = mixed(f);
        style(font);
        PrLayoutNode root = f.element(kPrLayoutNodeNone, block(), nullptr, nullptr, &font);
        PrLayoutBoxStyle stageBox = box != nullptr ? *box : block();
        stageBox.width = prLayoutLengthPx(width);
        stage = f.element(root, stageBox, nullptr, effects, &font);
        for (const char* piece : pieces)
            texts.push_back(f.text(stage, piece));
        f.layout(px(600, 600));
        CHECK(verified(f.tree));
    }

    PrLayoutTreeStats stats() const { return prLayoutTreeStats(f.tree); }

    // Returns what painting the tree records.
    Paint paint() {
        PrDrawList* list = prDrawListCreate();
        prLayoutTreePaint(f.tree, list);
        Paint out;
        appendPainted(list, PrDAffine2{1, 0, 0, 1, 0, 0}, out);
        prDrawListRelease(list);
        return out;
    }
};

// Returns the one text that the pieces divide.
const char* joined(const std::vector<const char*>& pieces) {
    static std::string text;
    text.clear();
    for (const char* piece : pieces)
        text += piece;
    return text.c_str();
}

// Returns the paint's glyphs in visual order: by line from the top, and from the left along each line. A line paints
// its fragments in tree order, so on a bidi line the painting order is not the visual order.
std::vector<Painted> visual(std::vector<Painted> glyphs) {
    std::stable_sort(glyphs.begin(), glyphs.end(), [](const Painted& a, const Painted& b) {
        return a.y != b.y ? a.y < b.y : a.x < b.x;
    });
    return glyphs;
}

// Checks that two paints place the same glyphs, in the same fonts, in the same visual order, at the same origins.
void checkSamePlacement(const Paint& mergedPaint, const Paint& dividedPaint) {
    std::vector<Painted> merged = visual(mergedPaint.glyphs);
    std::vector<Painted> divided = visual(dividedPaint.glyphs);
    REQUIRE(merged.size() == divided.size());
    for (size_t i = 0; i < merged.size(); ++i) {
        CAPTURE(i);
        CHECK(merged[i].id == divided[i].id);
        CHECK(merged[i].family == divided[i].family);
        CHECK(merged[i].x == doctest::Approx(divided[i].x).epsilon(1e-5));
        CHECK(merged[i].y == doctest::Approx(divided[i].y).epsilon(1e-5));
    }
}

// Returns the number of times that the font changes from one glyph of a paint to the next.
uint32_t fontChanges(const Paint& paint) {
    uint32_t changes = 0;
    for (size_t i = 1; i < paint.glyphs.size(); ++i)
        changes += paint.glyphs[i].family != paint.glyphs[i - 1].family ? 1 : 0;
    return changes;
}

// Lays out `pieces` as one text node and as one text node per piece, styled by `style`, on a stage `width` wide, and
// checks that the single node places every glyph where the divided nodes do. Writes the single node's count of text
// fragments made, and its paint, if asked.
template <typename Style>
void checkAsDivided(Style style, std::vector<const char*> pieces, float width,
        uint32_t* PR_NULLABLE fragments = nullptr, Paint* PR_NULLABLE paint = nullptr) {
    Laid one(style, {joined(pieces)}, width);
    Laid many(style, pieces, width);
    Paint merged = one.paint();
    checkSamePlacement(merged, many.paint());
    if (fragments != nullptr)
        *fragments = one.stats().textFragmentsMade;
    if (paint != nullptr)
        *paint = merged;
}

void horizontal(PrLayoutFontStyle&) {}

// Returns the one rectangle of a text node on its line, in `coordinates`.
PrRect rectOf(Laid& laid, uint32_t text, PrLayoutCoordinates coordinates) {
    PrLayoutRect rect{};
    REQUIRE(prLayoutNodeBoxes(laid.f.tree, laid.texts[text], coordinates, &rect, 1) == 1);
    return prLayoutRectToPx(rect);
}

// Returns a point over the shorter face's glyphs that only the taller face's content area reaches, from the rectangles
// of each face in the divided text.
PrVec2 enclosedPoint(PrRect a, PrRect b) {
    PrRect taller = b.bottom - b.top > a.bottom - a.top ? b : a;
    PrRect shorter = taller.left == b.left ? a : b;
    float y = taller.top < shorter.top ? (taller.top + shorter.top) / 2 : (taller.bottom + shorter.bottom) / 2;
    return PrVec2{(shorter.left + shorter.right) / 2, y};
}

// A list that records glyphs directly, as a batch of a recording does: the kinds of its commands, and the font families
// of its glyph commands.
struct Batch {
    std::vector<PrDrawCommandKind> kinds;
    std::vector<std::string> families;
};

// Appends the lists of `list`, and those it embeds, that record glyphs, in the order they play.
void appendBatches(const PrDrawList* list, std::vector<Batch>& out) {
    Batch batch;
    const PrDrawCommand* commands = prDrawListCommands(list);
    for (uint32_t i = 0; i < prDrawListCommandCount(list); ++i) {
        const PrDrawCommand& command = commands[i];
        if (command.kind == PrDrawCommandKind_Embed) {
            appendBatches(command.embed.list, out);
            continue;
        }
        batch.kinds.push_back(command.kind);
        if (command.kind == PrDrawCommandKind_Glyphs)
            batch.families.push_back(familyOf(command.glyphs->font));
    }
    if (!batch.families.empty())
        out.push_back(batch);
}

// Returns true if the pushes and pops of `batch` balance, none popping what it did not push.
bool balanced(const Batch& batch) {
    int depth = 0;
    for (PrDrawCommandKind kind : batch.kinds) {
        depth += kind == PrDrawCommandKind_PushTransformAffine2 ? 1 : kind == PrDrawCommandKind_PopTransform ? -1 : 0;
        if (depth < 0)
            return false;
    }
    return depth == 0;
}

// Returns the batches that painting sideways text of `pieces`, one text node per piece, on a vertical line records,
// with each fragment under a turn. Writes the layout's counts to `stats`.
std::vector<Batch> sidewaysBatches(const std::vector<const char*>& pieces, PrLayoutTreeStats* stats) {
    auto sideways = [](PrLayoutFontStyle& font) {
        font.writingMode = PrLayoutWritingMode_VerticalRl;
        font.textOrientation = PrLayoutTextOrientation_Sideways;
    };
    Laid laid(sideways, pieces, 4000);
    PrDrawList* list = prDrawListCreate();
    prLayoutTreePaint(laid.f.tree, list);
    std::vector<Batch> batches;
    appendBatches(list, batches);
    prDrawListRelease(list);
    *stats = laid.stats();
    return batches;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a change of face alone makes no leaf and no fragment, and moves no glyph") {
    uint32_t fragments = 0;
    Paint paint;
    checkAsDivided(horizontal, {"AA", "bb", " CC", "dd"}, 400, &fragments, &paint);
    CHECK(fragments == 1);
    // Arimo, Literata, Arimo, Literata: a glyph command for each range.
    CHECK(fontChanges(paint) == 3);
    CHECK(paint.commands == 4);
}

TEST_CASE("text of one face across a change of script is one range") {
    // Latin and Greek in Arimo, with the space between them taking the script before it.
    auto arimo = [](PrLayoutFontStyle& font) { font.font = textFontOf(20); };
    Laid one(arimo, {"bb \xce\xb1\xce\xb2"}, 400);
    Paint paint = one.paint();
    CHECK(one.stats().textFragmentsMade == 1);
    CHECK(fontChanges(paint) == 0);
    CHECK(paint.commands == 1);
}

TEST_CASE("glyphs of faces with different metrics sit on the item's dominant baseline, whatever its name") {
    for (PrLayoutDominantBaseline name : {PrLayoutDominantBaseline_Alphabetic, PrLayoutDominantBaseline_Central,
        PrLayoutDominantBaseline_Hanging, PrLayoutDominantBaseline_TextTop}) {
        CAPTURE(int(name));
        auto style = [name](PrLayoutFontStyle& font) { font.dominantBaseline = name; };
        uint32_t fragments = 0;
        checkAsDivided(style, {"AA", "bb", "CC"}, 400, &fragments);
        CHECK(fragments == 1);
    }
}

TEST_CASE("a line that ends inside a shaping run makes the ranges of each line") {
    // Literata first, which serves the space: "bbbb bbbb" is one shaping run, of one face, script and item, between Han
    // in Noto Sans JP. The line wraps between its words.
    auto literataFirst = [](PrLayoutFontStyle& font) {
        font.font = textFontOf(20, {prFontFamilyFromName("Fallback"), prFontFamilyFromName("Japanese")});
    };
    const char* han = "\xe6\x97\xa5\xe6\x97\xa5";
    uint32_t fragments = 0;
    Paint paint;
    checkAsDivided(literataFirst, {han, "bbbb bbbb", han}, 100, &fragments, &paint);
    // Two lines, each Han and Latin in two ranges.
    CHECK(fragments == 2);
    CHECK(paint.commands == 4);
    std::vector<double> lines;
    std::vector<std::string> latin;
    for (const Painted& p : paint.glyphs) {
        if (lines.empty() || p.y > lines.back() + 20)
            lines.push_back(p.y);
        if (p.family != "Noto Sans JP")
            latin.push_back(p.family);
    }
    REQUIRE(lines.size() == 2);
    // The run's glyphs on both lines, including its space, are in its one face.
    REQUIRE(latin.size() >= 8);
    for (const std::string& family : latin)
        CHECK(family == "Literata");
}

TEST_CASE("a right-to-left leaf of several faces places its ranges in visual order") {
    auto rtl = [](PrLayoutFontStyle& font) { font.direction = PrTextDirection_Rtl; };
    uint32_t fragments = 0;
    Paint paint;
    // Alef to gimel in Arimo, kaf with a dagesh, lamed and mem in Noto Sans Hebrew, then bet with a dagesh in Arimo:
    // ranges of unequal length, and clusters of more than one code point.
    checkAsDivided(rtl, {"\xd7\x90\xd7\x91\xd7\x92", "\xd7\x9b\xd6\xbc\xd7\x9c\xd7\x9e", "\xd7\x91\xd6\xbc"}, 400,
        &fragments, &paint);
    CHECK(fragments == 1);
    CHECK(fontChanges(paint) == 2);
}

TEST_CASE("faces of different metrics in one upright leaf, and in one sideways leaf in either turn") {
    struct Case {
        PrLayoutWritingMode mode;
        PrLayoutTextOrientation orientation;
    };
    for (Case c : {Case{PrLayoutWritingMode_VerticalRl, PrLayoutTextOrientation_Upright},
        Case{PrLayoutWritingMode_VerticalLr, PrLayoutTextOrientation_Upright},
                Case{PrLayoutWritingMode_VerticalRl, PrLayoutTextOrientation_Sideways},
                Case{PrLayoutWritingMode_SidewaysLr, PrLayoutTextOrientation_Mixed}}) {
        CAPTURE(int(c.mode));
        CAPTURE(int(c.orientation));
        auto style = [c](PrLayoutFontStyle& font) {
            font.writingMode = c.mode;
            font.textOrientation = c.orientation;
        };
        uint32_t fragments = 0;
        Paint paint;
        checkAsDivided(style, {"AA", "bb", "CC"}, 400, &fragments, &paint);
        CHECK(fragments == 1);
        // A turned fragment pushes its turn once, for all of its ranges.
        CHECK(paint.turns == (c.orientation == PrLayoutTextOrientation_Upright ? 0u : 1u));
    }
}

TEST_CASE("a change of level, a preserved tab, hanging white space and a change of orientation still divide a leaf") {
    SUBCASE("level") {
        Laid one(horizontal, {"AA \xd7\x90\xd7\x91"}, 400);
        CHECK(one.stats().textFragmentsMade == 2);
    }
    SUBCASE("tab") {
        auto preserved = [](PrLayoutFontStyle& font) { font.whiteSpaceCollapse = PrLayoutWhiteSpaceCollapse_Preserve; };
        Laid one(preserved, {"AA\tbb"}, 400);
        CHECK(one.stats().textFragmentsMade == 2);
        CHECK(one.stats().textLeavesPlaced == 2);
    }
    SUBCASE("hanging white space") {
        auto ended = [](PrLayoutFontStyle& font) {
            font.whiteSpaceCollapse = PrLayoutWhiteSpaceCollapse_Preserve;
            font.textAlign = PrLayoutTextAlign_Center;
        };
        Laid one(ended, {"AAbb   "}, 400);
        CHECK(one.stats().textFragmentsMade == 2);
    }
    SUBCASE("orientation") {
        auto vertical = [](PrLayoutFontStyle& font) { font.writingMode = PrLayoutWritingMode_VerticalRl; };
        // Latin sideways, Han upright.
        Laid one(vertical, {"AA\xe6\x97\xa5\xe6\x9c\xac"}, 400);
        CHECK(one.stats().textFragmentsMade == 2);
    }
}

TEST_CASE("justification widens each opportunity once where a change of face used to divide leaves") {
    for (PrTextDirection direction : {PrTextDirection_Ltr, PrTextDirection_Rtl}) {
        CAPTURE(int(direction));
        auto justified = [direction](PrLayoutFontStyle& font) {
            font.textAlign = PrLayoutTextAlign_Justify;
            font.direction = direction;
        };
        checkAsDivided(justified, {
            "AAbb", " CC", "dd ", "AA", "bb ", "\xd7\x90\xd7\x91", "\xd7\x9b\xd7\x9c",
            " AAbb CCdd AAbb",
        }, 200);
    }
}

TEST_CASE("preserved white space that hangs at a line's end is placed as divided text places it") {
    auto preserved = [](PrLayoutFontStyle& font) {
        font.whiteSpaceCollapse = PrLayoutWhiteSpaceCollapse_Preserve;
        font.textAlign = PrLayoutTextAlign_End;
    };
    checkAsDivided(preserved, {"AA", "bb", "   ", "CC", "dd  "}, 120);
}

TEST_CASE("white space that ends a line takes the paragraph's level, and is placed in its order (UAX9 L1)") {
    // An en space then an em space, which advance differently, between two words of the other direction, with the line
    // wrapping after them. Shaping sets the spaces at the words' level, and L1 sets them at the paragraph's level,
    // which reverses the order in which they are placed.
    struct Case {
        PrTextDirection direction;
        const char* text;
    };
    for (Case c : {Case{PrTextDirection_Rtl, "abc\xe2\x80\x82\xe2\x80\x83" "def"},
        Case{PrTextDirection_Ltr, "\xd7\x90\xd7\x91\xd7\x92\xe2\x80\x82\xe2\x80\x83\xd7\x93\xd7\x94\xd7\x95"}}) {
        CAPTURE(int(c.direction));
        Fixture f;
        PrLayoutFontStyle font = prLayoutFontStyleDefault();
        font.font = f.textFont(20);
        font.direction = c.direction;
        PrLayoutNode root = f.element(kPrLayoutNodeNone, block(), nullptr, nullptr, &font);
        PrLayoutBoxStyle box = block();
        box.width = prLayoutLengthPx(70);
        PrLayoutNode stage = f.element(root, box, nullptr, nullptr, &font);
        f.text(stage, c.text);
        f.layout(px(600, 600));
        CHECK(verified(f.tree));
        PrDrawList* list = prDrawListCreate();
        prLayoutTreePaint(f.tree, list);
        Paint paint;
        appendPainted(list, PrDAffine2{1, 0, 0, 1, 0, 0}, paint);
        prDrawListRelease(list);
        // The first line's glyphs: three letters and the two spaces.
        std::vector<Painted> line;
        for (const Painted& p : visual(paint.glyphs)) {
            if (p.y < 20)
                line.push_back(p);
        }
        REQUIRE(line.size() == 5);
        // The spaces are the glyphs whose origins are 10 and 20 apart from their neighbours in visual order. At the
        // paragraph's level, the en space comes first in the paragraph's direction.
        const PrFont* arimo = line[0].font;
        uint16_t en = prFontCodePointGlyph(arimo, 0x2002);
        uint16_t em = prFontCodePointGlyph(arimo, 0x2003);
        REQUIRE(en != 0);
        REQUIRE(em != 0);
        size_t enAt = 0;
        size_t emAt = 0;
        for (size_t i = 0; i < line.size(); ++i) {
            enAt = line[i].id == en ? i : enAt;
            emAt = line[i].id == em ? i : emAt;
        }
        if (c.direction == PrTextDirection_Rtl) {
            // Visually from the left: the em space, the en space, then the letters.
            CHECK(emAt == 0);
            CHECK(enAt == 1);
            CHECK(line[1].x - line[0].x == doctest::Approx(20));
            CHECK(line[2].x - line[1].x == doctest::Approx(10));
        } else {
            // Visually from the left: the letters, the en space, then the em space.
            CHECK(enAt == 3);
            CHECK(emAt == 4);
            CHECK(line[4].x - line[3].x == doctest::Approx(10));
        }
    }
}

TEST_CASE("a text node of faces with different metrics is hit across its one rectangle, of the faces' content areas") {
    Laid one(horizontal, {"AAbb"}, 400);
    Laid many(horizontal, {"AA", "bb"}, 400);
    PrLayoutRect rects[4];
    REQUIRE(prLayoutNodeBoxes(one.f.tree, one.texts[0], PrLayoutCoordinates_Layout, rects, 4) == 1);
    PrRect merged = prLayoutRectToPx(rects[0]);
    REQUIRE(prLayoutNodeBoxes(many.f.tree, many.texts[0], PrLayoutCoordinates_Layout, rects, 4) == 1);
    PrRect arimo = prLayoutRectToPx(rects[0]);
    REQUIRE(prLayoutNodeBoxes(many.f.tree, many.texts[1], PrLayoutCoordinates_Layout, rects, 4) == 1);
    PrRect literata = prLayoutRectToPx(rects[0]);
    // The one rectangle spans both faces' content areas, which differ.
    CHECK(merged.left == arimo.left);
    CHECK(merged.right == literata.right);
    CHECK(merged.top == std::min(arimo.top, literata.top));
    CHECK(merged.bottom == std::max(arimo.bottom, literata.bottom));
    REQUIRE((arimo.top != literata.top || arimo.bottom != literata.bottom));
    // A point over the capitals that only the taller face's content area reaches hits the text node.
    PrRect taller = literata.bottom - literata.top > arimo.bottom - arimo.top ? literata : arimo;
    PrRect shorter = taller.left == literata.left ? arimo : literata;
    float y = taller.top < shorter.top ? (taller.top + shorter.top) / 2 : (taller.bottom + shorter.bottom) / 2;
    PrVec2 point{(shorter.left + shorter.right) / 2, y};
    CHECK(one.f.hit(point) == one.texts[0]);
    CHECK(many.f.hit(point) != many.texts[0]);
}

TEST_CASE("the area a text node's one rectangle newly encloses is hit through a transformed parent, and not where clipped") {
    auto noWrap = [](PrLayoutFontStyle& font) { font.textWrapMode = PrLayoutTextWrapMode_Nowrap; };
    SUBCASE("a transformed parent") {
        PrLayoutEffectStyle scaled = prLayoutEffectStyleDefault();
        scaled.transform = prLayoutTransformFromAffine2(prAffine2Scale(PrVec2{2, 2}));
        Laid one(noWrap, {"AAbb"}, 400, nullptr, &scaled);
        Laid many(noWrap, {"AA", "bb"}, 400, nullptr, &scaled);
        PrVec2 point = enclosedPoint(rectOf(many, 0, PrLayoutCoordinates_Viewport),
            rectOf(many, 1, PrLayoutCoordinates_Viewport));
        CHECK(one.f.hit(point) == one.texts[0]);
        CHECK(many.f.hit(point) != many.texts[0]);
        CHECK(many.f.hit(point) != many.texts[1]);
    }
    SUBCASE("a clip") {
        // The capitals after the small letters, past the edge of the clipping stage, which ends at the small letters'
        // edge.
        Laid open(noWrap, {"bbAA"}, 400);
        Laid divided(noWrap, {"bb", "AA"}, 400);
        PrRect small = rectOf(divided, 0, PrLayoutCoordinates_Layout);
        PrVec2 point = enclosedPoint(small, rectOf(divided, 1, PrLayoutCoordinates_Layout));
        CHECK(open.f.hit(point) == open.texts[0]);
        PrLayoutBoxStyle clipping = block();
        clipping.overflowX = PrLayoutOverflow_Hidden;
        clipping.overflowY = PrLayoutOverflow_Hidden;
        Laid clipped(noWrap, {"bbAA"}, small.right - small.left, &clipping);
        CHECK(clipped.f.hit(point) != clipped.texts[0]);
    }
}

TEST_CASE("characters that shape to no glyph make no range, among faces") {
    // A soft hyphen, a word joiner, a zero-width joiner and non-joiner, and a variation selector, between faces.
    uint32_t fragments = 0;
    checkAsDivided(horizontal,
        {"AA\xc2\xad", "bb\xe2\x81\xa0", "CC\xe2\x80\x8d", "dd\xe2\x80\x8c", "EE\xef\xb8\x8f", "ff"}, 400, &fragments);
    CHECK(fragments == 1);
}

TEST_CASE("the counts of a line of faces: one leaf, one fragment of its fonts and ranges, one line child") {
    Laid one(horizontal, {"AAbb CCdd"}, 400);
    Paint paint = one.paint();
    PrLayoutTreeStats stats = one.stats();
    CHECK(stats.textLeavesPlaced == 1);
    CHECK(stats.textFragmentsMade == 1);
    CHECK(stats.lineChildrenMade == 1);
    // Arimo and Literata, in four ranges, of nine glyphs, each a cluster with a caret stop.
    REQUIRE(paint.glyphs.size() == 9);
    CHECK(stats.textFragmentBytes == layoutFragmentTextBytes(2, 4, 9, 9));
    CHECK(stats.textFragmentsRecorded == 1);
    CHECK(stats.glyphCommandsRecorded == 4);
}

TEST_CASE("a text node has a rectangle for each placement fragment on a line") {
    SUBCASE("faces") {
        Laid one(horizontal, {"AAbb CCdd"}, 400);
        CHECK(prLayoutNodeBoxes(one.f.tree, one.texts[0], PrLayoutCoordinates_Layout, nullptr, 0) == 1);
    }
    SUBCASE("orientation") {
        auto vertical = [](PrLayoutFontStyle& font) { font.writingMode = PrLayoutWritingMode_VerticalRl; };
        Laid one(vertical, {"AA\xe6\x97\xa5\xe6\x9c\xac"}, 400);
        CHECK(prLayoutNodeBoxes(one.f.tree, one.texts[0], PrLayoutCoordinates_Layout, nullptr, 0) == 2);
    }
    SUBCASE("a preserved tab") {
        auto preserved = [](PrLayoutFontStyle& font) { font.whiteSpaceCollapse = PrLayoutWhiteSpaceCollapse_Preserve; };
        Laid one(preserved, {"AA\tbb"}, 400);
        CHECK(prLayoutNodeBoxes(one.f.tree, one.texts[0], PrLayoutCoordinates_Layout, nullptr, 0) == 2);
    }
}

TEST_CASE("a fragment with a font in two ranges releases each, and a changed face lays out as a fresh layout does") {
    auto cycle = [] {
        Laid one(horizontal, {"AAbbAA"}, 400);
        Paint paint = one.paint();
        CHECK(paint.commands == 3);
        // Another family list for the same node. The incremental layout matches one from scratch (Laid verifies).
        PrLayoutFontStyle other = one.font;
        other.font = one.f.textFont(20, {prFontFamilyFromName("Fallback"), prFontFamilyFromName("Upper")});
        PrLayoutBoxStyle box = block();
        box.width = prLayoutLengthPx(400);
        prLayoutNodeSetStyle(one.f.tree, one.stage, one.f.style(box, nullptr, nullptr, &other));
        one.f.layout(px(600, 600));
        CHECK(verified(one.f.tree));
    };
    cycle();
    size_t live = prMemoryLiveCount();
    cycle();
    CHECK(prMemoryLiveCount() == live);
}

TEST_CASE("a fragment is whole in its batch, which ends after the fragment that brings it to 64 commands") {
    // 63 fragments of one range, which is one command short of a batch, then a fragment of six ranges, alternating
    // Arimo and Literata, then one more fragment of one range.
    std::vector<const char*> pieces(63, "A");
    pieces.push_back("AbAbAb");
    pieces.push_back("A");
    PrLayoutTreeStats stats{};
    std::vector<Batch> batches = sidewaysBatches(pieces, &stats);
    REQUIRE(batches.size() == 2);
    CHECK(batches[0].families.size() == 69);
    CHECK(batches[1].families.size() == 1);
    for (const Batch& batch : batches)
        CHECK(balanced(batch));
    CHECK(batches[0].kinds.back() == PrDrawCommandKind_PopTransform);
    CHECK(stats.glyphCommandsRecorded == 70);
    CHECK(stats.textFragmentsRecorded == 65);
}

TEST_CASE("a fragment whose own ranges fill a batch ends it among them, closing its turn and opening it again") {
    // 10 fragments of one range, then a fragment of 70 ranges, alternating Arimo and Literata, as a long line of two
    // fonts makes, then one more fragment of one range. The batch ends at the fragment's 64th range.
    std::vector<const char*> pieces(10, "A");
    std::string many;
    for (int i = 0; i < 35; ++i)
        many += "Ab";
    pieces.push_back(many.c_str());
    pieces.push_back("A");
    PrLayoutTreeStats stats{};
    std::vector<Batch> batches = sidewaysBatches(pieces, &stats);
    REQUIRE(batches.size() == 2);
    REQUIRE(batches[0].families.size() == 74);
    REQUIRE(batches[1].families.size() == 7);
    for (const Batch& batch : batches)
        CHECK(balanced(batch));
    CHECK(batches[0].families.back() == "Literata");
    CHECK(batches[0].kinds.back() == PrDrawCommandKind_PopTransform);
    CHECK(batches[1].kinds.front() == PrDrawCommandKind_PushTransformAffine2);
    for (size_t i = 0; i < 6; ++i)
        CHECK(batches[1].families[i] == (i % 2 == 0 ? "Arimo" : "Literata"));
    CHECK(stats.glyphCommandsRecorded == 81);
}

TEST_CASE("fragments of many ranges, sideways and upright, are recorded where divided text records them") {
    // Enough fragments of one range each to bring a batch near its end, then fragments of many ranges, in a vertical
    // line. Every glyph is painted where the divided text paints it.
    for (PrLayoutTextOrientation orientation : {PrLayoutTextOrientation_Sideways, PrLayoutTextOrientation_Upright}) {
        CAPTURE(int(orientation));
        auto vertical = [orientation](PrLayoutFontStyle& font) {
            font.writingMode = PrLayoutWritingMode_VerticalRl;
            font.textOrientation = orientation;
        };
        std::vector<const char*> pieces;
        for (int i = 0; i < 30; ++i)
            pieces.push_back("A\xd7\x90 ");
        for (int i = 0; i < 6; ++i) {
            pieces.push_back("Ab");
            pieces.push_back("Ab");
            pieces.push_back("Ab ");
        }
        Laid one(vertical, {joined(pieces)}, 2000);
        Laid many(vertical, pieces, 2000);
        Paint merged = one.paint();
        checkSamePlacement(merged, many.paint());
        CHECK(merged.commands == one.stats().glyphCommandsRecorded);
    }
}
