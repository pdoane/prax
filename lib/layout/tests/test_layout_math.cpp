// Math layout: math boxes laid out from the MATH constants of New Computer Modern Math and of WPT's math test fonts,
// whose values are known, and checked against the positions that TeXbook Appendix G and the OpenType MATH table give.
// Spaces (mspace) of known sizes stand in for content, so that every position follows from the constants alone.

#include "pr/compress_builtin.h"
#include "pr/font_math.h"
#include "pr/layout_query.h"
#include "pr/layout_style.h"
#include "pr/layout_tree.h"

#include "layout/src/layout_fragment.h"
#include "layout/src/layout_tree.h"
#include "layout/src/layout_unit.h"
#include "layout/tests/layout_fixture.h"

#include "tools/harness/test.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace pr_impl {

namespace {

constexpr float kSize = 32;

std::vector<uint8_t> readFile(const std::string& path) {
    std::vector<uint8_t> bytes;
    FILE* file = std::fopen(path.c_str(), "rb");
    REQUIRE(file != nullptr);
    uint8_t buffer[65536];
    size_t read = 0;
    while ((read = std::fread(buffer, 1, sizeof(buffer), file)) != 0)
        bytes.insert(bytes.end(), buffer, buffer + read);
    std::fclose(file);
    return bytes;
}

// A tree whose math is set at kSize px in New Computer Modern Math, or in the font that the constructor names: a font
// file's path, or the name of a WPT math test font such as "radical-rtlm".
struct MathFixture {
    Fixture f;
    const PrFont* font = nullptr;
    PrTextFont* text = nullptr;
    PrTextDirection direction = PrTextDirection_Ltr;    // the direction that style() gives each box

    explicit MathFixture(const char* wpt = nullptr) {
        PrStatus status = PrStatus_Ok;
        if (wpt == nullptr || std::strchr(wpt, '/') != nullptr) {
            std::string path = wpt != nullptr ? wpt : std::string(PR_FONT_BUNDLE_DIR) + "/NewCMMath-Regular.otf";
            std::vector<uint8_t> bytes = readFile(path);
            status = prFontCreate(bytes, 0, nullptr, &font);
        } else {
            std::vector<uint8_t> bytes = readFile(std::string(PR_FONTS_DIR) + "/math/" + wpt + ".woff");
            PrCompressCodec* codec = prCompressBuiltinCreate();
            status = prFontCreateWoff(bytes, 0, nullptr, codec, &font);
            prCompressCodecDestroy(codec);
        }
        REQUIRE(status == PrStatus_Ok);
        PrFontFaceDescriptor descriptor = prFontFaceDescriptorDefault();
        descriptor.family = "TestMath";
        descriptor.automatic = PrFontFaceAutoFlags(descriptor.automatic & ~PrFontFaceAutoFlag_Family);
        prFontSetAdd(f.fonts, &descriptor, font);
        text = f.textFont(kSize, {prFontFamilyFromName("TestMath")});
    }

    ~MathFixture() { prFontRelease(font); }

    const PrFontMathConstants& c() const { return *prFontMathConstants(font); }

    // Returns `ems` of the font in units, rounded as layout rounds a constant.
    PrLayoutUnit u(float ems) const { return prLayoutUnitFromPx(ems * kSize); }

    // Returns a record of a box of `display`, in the math font with `flags` at math depth `depth`, laid out by `math`.
    PrLayoutStyleIndex style(PrLayoutDisplay display, const PrLayoutMathStyle& math, PrLayoutMathFlags flags = 0,
            uint8_t depth = 0, PrLayoutBoxStyle box = prLayoutBoxStyleDefault()) {
        box.display = display;
        PrLayoutFontStyle fontStyle = prLayoutFontStyleDefault();
        fontStyle.font = text;
        fontStyle.mathFlags = flags;
        fontStyle.mathDepth = depth;
        fontStyle.direction = direction;
        PrLayoutStyle record = prLayoutStyleDefault();
        record.groups[PrLayoutGroup_Box] = prLayoutBoxStyleIntern(f.tree, &box);
        record.groups[PrLayoutGroup_Font] = prLayoutFontStyleIntern(f.tree, &fontStyle);
        record.groups[PrLayoutGroup_Math] = prLayoutMathStyleIntern(f.tree, &math);
        return prLayoutStyleIntern(f.tree, &record);
    }

    // Creates a math box of `kind` under `parent`.
    PrLayoutNode box(PrLayoutNode parent, PrLayoutMathKind kind, PrLayoutMathFlags flags = 0,
            PrLayoutMathStyle math = prLayoutMathStyleDefault()) {
        math.kind = kind;
        PrLayoutNode node = prLayoutNodeCreateElement(f.tree, parent, style(PrLayoutDisplay_BlockMath, math, flags));
        return node;
    }

    // Creates the math root, an inline-level math box in a block of `width`.
    PrLayoutNode root(PrLayoutMathFlags flags = 0) {
        PrLayoutNode body = f.element(kPrLayoutNodeNone, block());
        PrLayoutMathStyle math = prLayoutMathStyleDefault();
        math.kind = PrLayoutMathKind_Row;
        return prLayoutNodeCreateElement(f.tree, body, style(PrLayoutDisplay_InlineMath, math, flags));
    }

    // Creates an mspace of `width`, `height` above its baseline and `depth` below it.
    PrLayoutNode space(PrLayoutNode parent, float width, float height, float depth, PrLayoutMathFlags flags = 0) {
        PrLayoutMathStyle math = prLayoutMathStyleDefault();
        math.kind = PrLayoutMathKind_Space;
        math.spaceAscent = prLayoutUnitFromPx(height);
        PrLayoutBoxStyle box = prLayoutBoxStyleDefault();
        box.width = prLayoutLengthPx(width);
        box.height = prLayoutLengthPx(height + depth);
        return prLayoutNodeCreateElement(f.tree, parent, style(PrLayoutDisplay_BlockMath, math, flags, 0, box));
    }

    // Creates a token of `kind` holding `text`.
    PrLayoutNode token(PrLayoutNode parent, const char* value, PrLayoutMathKind kind = PrLayoutMathKind_Token,
            PrLayoutMathStyle math = prLayoutMathStyleDefault(), PrLayoutMathFlags flags = 0, uint8_t depth = 0) {
        math.kind = kind;
        PrLayoutNode node = prLayoutNodeCreateElement(f.tree, parent, style(PrLayoutDisplay_BlockMath, math, flags, depth));
        prLayoutNodeCreateText(f.tree, node, value);
        return node;
    }

    void layout() {
        f.layout(px(4000, 1000));
        CHECK(verified(f.tree));
    }

    PrLayoutRect bounds(PrLayoutNode node) { return prLayoutNodeBounds(f.tree, node, PrLayoutCoordinates_Viewport); }
};

// Returns an operator's math group with `flags`.
PrLayoutMathStyle op(PrLayoutMathOperatorFlags flags, float lspace = 0, float rspace = 0) {
    PrLayoutMathStyle math = prLayoutMathStyleDefault();
    math.embellished = true;
    math.operatorFlags = flags;
    math.leftSpace = prLayoutUnitFromPx(lspace);
    math.rightSpace = prLayoutUnitFromPx(rspace);
    return math;
}

// Returns `unit` in px, which a failed check prints.
double pxOf(PrLayoutUnit unit) {
    return double(prLayoutUnitToPx(unit));
}

PrLayoutUnit height(PrLayoutRect r) {
    return r.bottom - r.top;
}
PrLayoutUnit width(PrLayoutRect r) {
    return r.right - r.left;
}

// Returns the fragment of `node` under `at`, or null.
const LayoutFragment* PR_NULLABLE fragmentOf(const LayoutFragment* at, LayoutNodeIndex node) {
    if (at->node == node)
        return at;
    for (uint32_t i = 0; i < at->childCount; ++i) {
        if (const LayoutFragment* found = fragmentOf(at->children[i].fragment, node))
            return found;
    }
    return nullptr;
}

// Returns a math group of a part of a stack.
PrLayoutMathStyle stackPart(PrLayoutMathStackPart part, int32_t position = 0) {
    PrLayoutMathStyle math = prLayoutMathStyleDefault();
    math.stackPart = part;
    math.stackFlags = PrLayoutMathStackFlag_RowStart;
    math.stackPosition = position;
    return math;
}

// Returns the math group of a stack aligned as `align` says, its columns with no space between them.
PrLayoutMathStyle stackOf(PrLayoutMathAlign align) {
    PrLayoutMathStyle math = prLayoutMathStyleDefault();
    math.stackAlign = align;
    math.charAlign = PrLayoutMathAlign_Right;
    math.decimalPoint = '.';
    math.tableAlign = PrLayoutMathTableAlign_Baseline;
    return math;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("math row: children side by side on one baseline, sized by their margin boxes") {
    MathFixture m;
    PrLayoutNode math = m.root();
    PrLayoutNode a = m.space(math, 10, 20, 5);
    PrLayoutNode b = m.space(math, 30, 8, 12);
    m.layout();
    PrLayoutRect ra = m.bounds(a);
    PrLayoutRect rb = m.bounds(b);
    PrLayoutRect root = m.bounds(math);
    CHECK(pxOf(width(root)) == pxOf(layoutUnitPx(40)));
    CHECK(pxOf(height(root)) == pxOf(layoutUnitPx(32)));
    CHECK(pxOf(rb.left) == pxOf(ra.right));
    // Both baselines are 20px below the root's top.
    CHECK(pxOf(ra.top) == pxOf(root.top));
    CHECK(pxOf(rb.top + layoutUnitPx(8)) == pxOf(root.top + layoutUnitPx(20)));
}

TEST_CASE("math row: operators take their spaces, except inside an embellished operator") {
    MathFixture m;
    PrLayoutNode math = m.root();
    PrLayoutNode a = m.space(math, 10, 10, 0);
    PrLayoutNode plus = m.token(math, "+", PrLayoutMathKind_Operator, op(0, 4, 6));
    PrLayoutNode b = m.space(math, 10, 10, 0);
    m.layout();
    CHECK(pxOf(m.bounds(plus).left) == pxOf(m.bounds(a).right + layoutUnitPx(4)));
    CHECK(pxOf(m.bounds(b).left) == pxOf(m.bounds(plus).right + layoutUnitPx(6)));
}

TEST_CASE("math fraction: numerator and denominator shifted by the MATH constants about the axis") {
    MathFixture m;
    PrLayoutNode math = m.root();
    PrLayoutNode frac = m.box(math, PrLayoutMathKind_Fraction);
    PrLayoutNode num = m.space(frac, 20, 4, 2);
    PrLayoutNode den = m.space(frac, 40, 3, 1);
    m.layout();
    const PrFontMathConstants& c = m.c();
    PrLayoutUnit axis = m.u(c.axisHeight);
    PrLayoutUnit rule = m.u(c.fractionRuleThickness);
    // Display style: math-style normal.
    PrLayoutUnit numShift = layoutUnitMax(m.u(c.fractionNumeratorDisplayStyleShiftUp),
        axis + PrLayoutUnit{rule.raw / 2} + m.u(c.fractionNumDisplayStyleGapMin) + layoutUnitPx(2));
    PrLayoutUnit denShift = layoutUnitMax(m.u(c.fractionDenominatorDisplayStyleShiftDown),
        PrLayoutUnit{rule.raw / 2} + m.u(c.fractionDenomDisplayStyleGapMin) + layoutUnitPx(3) - axis);
    PrLayoutRect rn = m.bounds(num);
    PrLayoutRect rd = m.bounds(den);
    // The baselines are numShift + denShift apart.
    PrLayoutUnit numBaseline = rn.top + layoutUnitPx(4);
    PrLayoutUnit denBaseline = rd.top + layoutUnitPx(3);
    CHECK(pxOf(denBaseline - numBaseline) == pxOf(numShift + denShift));
    // The numerator is centered over the wider denominator.
    CHECK(pxOf(rn.left - rd.left) == pxOf(layoutUnitPx(10)));
    CHECK(pxOf(width(m.bounds(frac))) == pxOf(layoutUnitPx(40)));
}

TEST_CASE("math fraction: compact style uses the text style constants, and no bar makes a stack") {
    MathFixture m;
    PrLayoutNode math = m.root(PrLayoutMathFlag_CompactStyle);
    PrLayoutMathStyle stack = prLayoutMathStyleDefault();
    stack.lineThickness = prLayoutLengthPx(0);
    PrLayoutNode frac = m.box(math, PrLayoutMathKind_Fraction, PrLayoutMathFlag_CompactStyle, stack);
    PrLayoutNode num = m.space(frac, 20, 1, 1);
    PrLayoutNode den = m.space(frac, 20, 1, 1);
    m.layout();
    const PrFontMathConstants& c = m.c();
    PrLayoutUnit top = m.u(c.stackTopShiftUp);
    PrLayoutUnit bottom = m.u(c.stackBottomShiftDown);
    PrLayoutUnit gap = (bottom - layoutUnitPx(1)) + (top - layoutUnitPx(1));
    PrLayoutUnit delta = m.u(c.stackGapMin) - gap;
    if (delta > kLayoutUnitZero) {
        top += PrLayoutUnit{delta.raw / 2};
        bottom += delta - PrLayoutUnit{delta.raw / 2};
    }
    CHECK(pxOf((m.bounds(den).top + layoutUnitPx(1)) - (m.bounds(num).top + layoutUnitPx(1))) == pxOf(top + bottom));
}

TEST_CASE("math scripts: a box base takes the baseline drops, and the scripts follow SpaceAfterScript") {
    MathFixture m("scripts-superscriptbaselinedropmax10000");
    PrLayoutNode math = m.root();
    PrLayoutNode sup = m.box(math, PrLayoutMathKind_Superscript);
    PrLayoutNode base = m.space(sup, 30, 50, 0);
    PrLayoutNode script = m.space(sup, 10, 5, 0);
    m.layout();
    const PrFontMathConstants& c = m.c();
    PrLayoutUnit shift = layoutUnitMax(m.u(c.superscriptShiftUp),
        layoutUnitMax(m.u(c.superscriptBottomMin), layoutUnitPx(50) - m.u(c.superscriptBaselineDropMax)));
    PrLayoutUnit baseBaseline = m.bounds(base).top + layoutUnitPx(50);
    PrLayoutUnit scriptBaseline = m.bounds(script).top + layoutUnitPx(5);
    CHECK(pxOf(baseBaseline - scriptBaseline) == pxOf(shift));
    CHECK(pxOf(m.bounds(script).left) == pxOf(m.bounds(base).right));
    CHECK(pxOf(width(m.bounds(sup))) == pxOf(layoutUnitPx(40) + m.u(c.spaceAfterScript)));
}

TEST_CASE("math scripts: a subscript and a superscript keep SubSuperscriptGapMin between them") {
    MathFixture m("scripts-subsuperscriptgapmin11000");
    PrLayoutNode math = m.root();
    PrLayoutNode subsup = m.box(math, PrLayoutMathKind_SubSuperscript);
    m.space(subsup, 30, 15, 15);
    PrLayoutNode sub = m.space(subsup, 10, 5, 0);
    PrLayoutNode sup = m.space(subsup, 10, 0, 5);
    m.layout();
    const PrFontMathConstants& c = m.c();
    // The gap between the subscript's ink top and the superscript's ink bottom.
    PrLayoutUnit gap = m.bounds(sub).top - m.bounds(sup).bottom;
    CHECK(pxOf(gap) == pxOf(m.u(c.subSuperscriptGapMin)));
}

TEST_CASE("math under and over: limits of a large operator, and accents on the base's baseline") {
    MathFixture m("limits-upperlimitgapmin7000");
    PrLayoutNode math = m.root();
    PrLayoutNode over = m.box(math, PrLayoutMathKind_Over);
    PrLayoutNode base = m.space(over, 30, 10, 0);
    PrLayoutNode limit = m.space(over, 10, 4, 3);
    m.layout();
    const PrFontMathConstants& c = m.c();
    // Not a large operator: the overscript's ink bottom is OverbarVerticalGap above the base's ink top.
    CHECK(pxOf(m.bounds(base).top - m.bounds(limit).bottom) == pxOf(m.u(c.overbarVerticalGap)));
    // Centered over the base.
    CHECK(pxOf(m.bounds(limit).left - m.bounds(base).left) == pxOf(layoutUnitPx(10)));
}

TEST_CASE("math under and over: an accent whose ink starts at its baseline sits on its base's ink") {
    MathFixture m("underover-accentbaseheight4000-overbarextraascender3000");
    const PrFontMathConstants& c = m.c();
    float accentBase = c.accentBaseHeight * kSize;
    PrLayoutNode math = m.root();
    PrLayoutMathStyle accent = prLayoutMathStyleDefault();
    accent.scriptFlags = PrLayoutMathScriptFlag_Accent;
    PrLayoutNode low = m.box(math, PrLayoutMathKind_Over, 0, accent);
    PrLayoutNode lowBase = m.space(low, 20, accentBase / 2, 0);
    PrLayoutNode lowAccent = m.space(low, 20, 3, 0);
    PrLayoutNode high = m.box(math, PrLayoutMathKind_Over, 0, accent);
    PrLayoutNode highBase = m.space(high, 20, accentBase + 10, 0);
    PrLayoutNode highAccent = m.space(high, 20, 3, 0);
    m.layout();
    CHECK(pxOf(m.bounds(lowAccent).bottom) == pxOf(m.bounds(lowBase).top));
    CHECK(pxOf(m.bounds(highAccent).bottom) == pxOf(m.bounds(highBase).top));
}

TEST_CASE("math radical: the sign covers the base, its gap and its rule") {
    MathFixture m;
    PrLayoutNode math = m.root();
    PrLayoutNode sqrt = m.box(math, PrLayoutMathKind_SquareRoot);
    PrLayoutNode base = m.space(sqrt, 30, 20, 10);
    m.layout();
    const PrFontMathConstants& c = m.c();
    PrLayoutRect r = m.bounds(sqrt);
    PrLayoutRect b = m.bounds(base);
    // The base follows the sign, and the bar's top is at least the gap and the rule above the base's ink.
    CHECK(pxOf(b.left) > pxOf(r.left));
    CHECK(b.top - r.top >= m.u(c.radicalDisplayStyleVerticalGap) + m.u(c.radicalRuleThickness) +
        m.u(c.radicalExtraAscender));
    CHECK(pxOf(r.right) == pxOf(b.right));
}

TEST_CASE("math radical: a nested radical sits inside its parent's sign, its rule a fixed step below its parent's") {
    // √(√(√(...x))), around a tall space, so that each sign is an assembly of the size asked for, and around a short
    // one, so that the signs step between the font's sizes, each taller than asked for by its own amount.
    constexpr uint32_t kLevels = 5;
    for (float height : {100.0f, 6.0f}) {
        CAPTURE(height);
        MathFixture m;
        PrLayoutNode math = m.root();
        PrLayoutNode parent = math;
        PrLayoutNode radicals[kLevels] = {};
        for (PrLayoutNode& radical : radicals) {
            radical = m.box(parent, PrLayoutMathKind_SquareRoot);
            parent = radical;
        }
        m.space(parent, 30, height, height / 2);
        m.layout();
        const PrFontMathConstants& c = m.c();
        // Each radical's rule and sign, in px from the viewport's top.
        struct Drawn {
            float ruleTop;
            float signTop;
            float signBottom;
        };
        Drawn drawn[kLevels] = {};
        for (uint32_t i = 0; i < kLevels; ++i) {
            const LayoutFragment* fragment = fragmentOf(m.f.tree->fragment, layoutNodeIndex(m.f.tree, radicals[i]));
            REQUIRE(fragment != nullptr);
            LayoutMathData data = layoutFragmentMathData(fragment);
            float top = prLayoutUnitToPx(m.bounds(radicals[i]).top);
            for (uint32_t p = 0; p < data.paintCount; ++p) {
                const LayoutMathPaint& paint = data.paints[p];
                if (paint.kind == LayoutMathPaintKind_Rule) {
                    drawn[i].ruleTop = top + paint.rect.top;
                } else if (paint.kind == LayoutMathPaintKind_Glyphs) {
                    drawn[i].signTop = top + paint.rect.top;
                    drawn[i].signBottom = top + paint.rect.bottom;
                }
            }
        }
        // Each inner rule is below its parent's by the parent's gap and rule and the inner radical's extra ascender,
        // whatever size each sign steps to, so that the rules are evenly spaced (mathml-core/#radicals-msqrt-mroot).
        // Each inner sign is no taller than its parent's.
        float step = (c.radicalDisplayStyleVerticalGap + c.radicalRuleThickness + c.radicalExtraAscender) * kSize;
        for (uint32_t i = 1; i < kLevels; ++i) {
            CAPTURE(i);
            CHECK(drawn[i].ruleTop - drawn[i - 1].ruleTop == doctest::Approx(step).epsilon(0.01));
            CHECK(drawn[i].signBottom - drawn[i].signTop <= drawn[i - 1].signBottom - drawn[i - 1].signTop);
        }
    }
}

TEST_CASE("math root: a display formula's box holds the ink of everything it draws, and its lines do not reach past it") {
    // An italic p, whose ink starts before its box, then limits, scripts, a fraction, a radical, an accent and a brace.
    MathFixture m;
    PrLayoutNode body = m.f.element(kPrLayoutNodeNone, block());
    PrLayoutMathStyle row = prLayoutMathStyleDefault();
    row.kind = PrLayoutMathKind_Row;
    PrLayoutNode math = prLayoutNodeCreateElement(m.f.tree, body, m.style(PrLayoutDisplay_BlockMath, row));
    m.token(math, "\xF0\x9D\x91\x9D");
    PrLayoutNode sum = m.box(math, PrLayoutMathKind_UnderOver);
    m.token(sum, "\xE2\x88\x91", PrLayoutMathKind_Operator, op(PrLayoutMathOperatorFlag_LargeOp));
    m.token(sum, "n=1");
    m.token(sum, "\xE2\x88\x9E");
    PrLayoutNode sup = m.box(math, PrLayoutMathKind_Superscript);
    m.token(sup, "\xF0\x9D\x9C\x8B");
    m.token(sup, "2");
    PrLayoutNode frac = m.box(math, PrLayoutMathKind_Fraction);
    m.token(frac, "1");
    m.token(frac, "6");
    PrLayoutNode sqrt = m.box(math, PrLayoutMathKind_SquareRoot);
    m.token(sqrt, "x");
    PrLayoutMathStyle accent = prLayoutMathStyleDefault();
    accent.scriptFlags = PrLayoutMathScriptFlag_Accent;
    PrLayoutNode hat = m.box(math, PrLayoutMathKind_Over, 0, accent);
    m.token(hat, "A");
    m.token(hat, "^", PrLayoutMathKind_Operator,
        op(PrLayoutMathOperatorFlag_Stretchy | PrLayoutMathOperatorFlag_InlineAxis));
    PrLayoutNode brace = m.box(math, PrLayoutMathKind_Over);
    m.token(brace, "abc");
    m.token(brace, "\xE2\x8F\x9E", PrLayoutMathKind_Operator,
        op(PrLayoutMathOperatorFlag_Stretchy | PrLayoutMathOperatorFlag_InlineAxis));
    m.layout();
    const LayoutFragment* root = fragmentOf(m.f.tree->fragment, layoutNodeIndex(m.f.tree, math));
    REQUIRE(root != nullptr);
    // The ink of every glyph and paint below the root, from its top left, in px.
    PrRect ink{1e9f, 1e9f, -1e9f, -1e9f};
    auto add = [&](PrRect r) {
        ink = PrRect{
            std::min(ink.left, r.left), std::min(ink.top, r.top), std::max(ink.right, r.right),
            std::max(ink.bottom, r.bottom),
        };
    };
    auto walk = [&](auto& recurse, const LayoutFragment* f, PrVec2 at) -> void {
        if (f->kind == LayoutFragmentKind_Text) {
            const PrFont* const* fonts = layoutFragmentTextFonts(f);
            for (uint32_t g = 0; g < f->text.glyphCount; ++g) {
                const PrFontGlyph& glyph = f->text.glyphs[g];
                PrRect b = prFontGlyphInkBounds(fonts[0], glyph.id);
                float x = at.x + glyph.position.x;
                float y = at.y + glyph.position.y;
                float size = f->text.size;
                if (b.right > b.left)
                    add(PrRect{x + b.left * size, y + b.top * size, x + b.right * size, y + b.bottom * size});
            }
        }
        if ((f->flags & LayoutFragmentFlag_Math) != 0) {
            LayoutMathData data = layoutFragmentMathData(f);
            for (uint32_t i = 0; i < data.paintCount; ++i) {
                PrRect r = data.paints[i].rect;
                add(PrRect{at.x + r.left, at.y + r.top, at.x + r.right, at.y + r.bottom});
            }
        }
        for (uint32_t i = 0; i < f->childCount; ++i)
            recurse(recurse, f->children[i].fragment, PrVec2{
                at.x + prLayoutUnitToPx(f->children[i].offset.x),
                at.y + prLayoutUnitToPx(f->children[i].offset.y),
            });
    };
    walk(walk, root, PrVec2{0, 0});
    // The box holds the ink, and the half pixel past the outlines that antialiasing may touch.
    PrVec2 extent = prLayoutVecToPx(root->extent);
    CHECK(ink.left >= 0.5f);
    CHECK(ink.top >= 0.5f);
    CHECK(ink.right <= extent.x - 0.5f);
    CHECK(ink.bottom <= extent.y - 0.5f);
    // The lines of its tokens reach past their ink-sized boxes, but not past the root's box.
    PrRect overflow = prLayoutRectToPx(root->overflow);
    CHECK(overflow.top >= 0);
    CHECK(overflow.bottom <= extent.y);
}

TEST_CASE("math tokens: a token's box is the ink of its glyphs") {
    MathFixture m;
    PrLayoutNode math = m.root();
    PrLayoutNode x = m.token(math, "x");
    PrLayoutNode y = m.token(math, "y");
    m.layout();
    uint16_t glyph = prFontCodePointGlyph(m.font, 'x');
    PrRect ink = prFontGlyphInkBounds(m.font, glyph);
    PrLayoutUnit expected = prLayoutUnitFromPxCeil(-ink.top * kSize) + prLayoutUnitFromPxCeil(ink.bottom * kSize);
    CHECK(pxOf(height(m.bounds(x))) == pxOf(expected));
    // y descends below the baseline, and x does not.
    CHECK(pxOf(m.bounds(y).bottom) > pxOf(m.bounds(x).bottom));
}

TEST_CASE("math operators: a stretchy fence covers what it encloses about the axis") {
    MathFixture m;
    PrLayoutNode math = m.root();
    PrLayoutMathOperatorFlags flags = PrLayoutMathOperatorFlag_Stretchy | PrLayoutMathOperatorFlag_Symmetric;
    PrLayoutNode open = m.token(math, "(", PrLayoutMathKind_Operator, op(flags));
    m.space(math, 10, 60, 40);
    PrLayoutNode fence = m.token(math, ")", PrLayoutMathKind_Operator, op(flags | PrLayoutMathOperatorFlag_Fence));
    m.layout();
    const PrFontMathConstants& c = m.c();
    float axis = c.axisHeight * kSize;
    float target = 2 * std::max(60 - axis, 40 + axis);
    // The plain stretchy operator covers the target. The fence covers at least 90.1% of it, and at most half an em
    // less.
    CHECK(prLayoutUnitToPx(height(m.bounds(open))) >= target - 1);
    CHECK(prLayoutUnitToPx(height(m.bounds(fence))) >= std::max(target * 0.901f, target - kSize / 2) - 1);
}

TEST_CASE("math operators: an assembly of many extenders draws every part, its top part at its top") {
    // A space hundreds of ems tall, which a parenthesis covers with hundreds of extenders.
    MathFixture m;
    PrLayoutNode math = m.root();
    PrLayoutMathOperatorFlags flags = PrLayoutMathOperatorFlag_Stretchy | PrLayoutMathOperatorFlag_Symmetric;
    PrLayoutNode open = m.token(math, "(", PrLayoutMathKind_Operator, op(flags));
    m.space(math, 10, 10000, 10000);
    m.layout();
    const LayoutFragment* fragment = fragmentOf(m.f.tree->fragment, layoutNodeIndex(m.f.tree, open));
    REQUIRE(fragment != nullptr);
    LayoutMathData data = layoutFragmentMathData(fragment);
    REQUIRE(data.paintCount == 1);
    const LayoutMathPaint& paint = data.paints[0];
    REQUIRE(paint.kind == LayoutMathPaintKind_Glyphs);
    CHECK(paint.glyphCount > 256);
    // The ink of the glyphs reaches the top and the bottom of the box that they cover.
    float top = paint.rect.bottom;
    float bottom = paint.rect.top;
    for (uint32_t g = 0; g < paint.glyphCount; ++g) {
        const PrFontGlyph& glyph = data.glyphs[paint.glyphStart + g];
        PrRect ink = prFontGlyphInkBounds(m.font, glyph.id);
        top = std::min(top, glyph.position.y + ink.top * paint.size);
        bottom = std::max(bottom, glyph.position.y + ink.bottom * paint.size);
    }
    CHECK(top == doctest::Approx(paint.rect.top).epsilon(0.001));
    CHECK(bottom == doctest::Approx(paint.rect.bottom).epsilon(0.001));
}

TEST_CASE("math operators: a large operator in display style reaches DisplayOperatorMinHeight") {
    MathFixture m;
    PrLayoutNode math = m.root();
    PrLayoutNode sum = m.token(math, "\xE2\x88\x91", PrLayoutMathKind_Operator,
        op(PrLayoutMathOperatorFlag_LargeOp | PrLayoutMathOperatorFlag_Symmetric));
    PrLayoutNode inlineSum = m.token(math, "\xE2\x88\x91", PrLayoutMathKind_Operator,
        op(PrLayoutMathOperatorFlag_LargeOp | PrLayoutMathOperatorFlag_Symmetric), PrLayoutMathFlag_CompactStyle);
    m.layout();
    CHECK(prLayoutUnitToPx(height(m.bounds(sum))) >= m.c().displayOperatorMinHeight * kSize - 1);
    CHECK(pxOf(height(m.bounds(sum))) > pxOf(height(m.bounds(inlineSum))));
}

TEST_CASE("math intrinsic sizes: a math box in a shrinking container is as wide as its layout") {
    MathFixture m;
    PrLayoutBoxStyle shrink = block();
    shrink.floating = PrLayoutFloat_Left;
    PrLayoutNode body = m.f.element(kPrLayoutNodeNone, block());
    PrLayoutNode container = prLayoutNodeCreateElement(m.f.tree, body, m.f.style(shrink));
    PrLayoutMathStyle row = prLayoutMathStyleDefault();
    row.kind = PrLayoutMathKind_Row;
    PrLayoutNode math = prLayoutNodeCreateElement(m.f.tree, container, m.style(PrLayoutDisplay_BlockMath, row));
    PrLayoutNode frac = m.box(math, PrLayoutMathKind_Fraction);
    m.space(frac, 25, 4, 2);
    m.space(frac, 35, 4, 2);
    m.layout();
    CHECK(pxOf(width(m.bounds(container))) == pxOf(layoutUnitPx(35)));
}

TEST_CASE("math script levels: text at a math depth takes the font's script forms") {
    MathFixture m;
    PrLayoutNode math = m.root();
    PrLayoutNode plain = m.token(math, "\xE2\x80\xB2");
    PrLayoutNode script = m.token(math, "\xE2\x80\xB2", PrLayoutMathKind_Token, prLayoutMathStyleDefault(), 0, 1);
    m.layout();
    // New Computer Modern Math's prime has a script form, which is drawn larger relative to its em.
    CHECK(pxOf(height(m.bounds(plain))) != pxOf(height(m.bounds(script))));
}

TEST_CASE("math operators: a stretchy accent covers its base along the inline axis") {
    MathFixture m;
    PrLayoutNode math = m.root();
    PrLayoutMathStyle accent = prLayoutMathStyleDefault();
    accent.scriptFlags = PrLayoutMathScriptFlag_Accent;
    PrLayoutNode over = m.box(math, PrLayoutMathKind_Over, 0, accent);
    m.space(over, 60, 10, 0);
    PrLayoutNode hat = m.token(over, "^", PrLayoutMathKind_Operator,
        op(PrLayoutMathOperatorFlag_Stretchy | PrLayoutMathOperatorFlag_InlineAxis));
    m.layout();
    CHECK(pxOf(width(m.bounds(hat))) >= 50);
}

TEST_CASE("math fenced: fences and separators drawn as operators around and between the children") {
    MathFixture m;
    PrLayoutMathStyle fenced = prLayoutMathStyleDefault();
    PrLayoutMathOperatorFlags stretchy =
        PrLayoutMathOperatorFlag_Stretchy | PrLayoutMathOperatorFlag_Symmetric | PrLayoutMathOperatorFlag_Fence;
    fenced.fences[PrLayoutMathFenceSlot_Open] = PrLayoutMathFence{.codePoint = '(', .flags = stretchy};
    fenced.fences[PrLayoutMathFenceSlot_Close] = PrLayoutMathFence{.codePoint = ')', .flags = stretchy};
    fenced.fences[PrLayoutMathFenceSlot_Separators] =
        PrLayoutMathFence{.codePoint = ',', .rightSpace = layoutUnitPx(3)};
    fenced.separatorCount = 1;
    PrLayoutNode math = m.root();
    PrLayoutNode box = m.box(math, PrLayoutMathKind_Fenced, 0, fenced);
    PrLayoutNode a = m.space(box, 10, 10, 0);
    PrLayoutNode b = m.space(box, 10, 60, 40);
    PrLayoutNode c = m.space(box, 10, 10, 0);
    m.layout();
    auto advance = [&](uint32_t codePoint) {
        return prLayoutUnitFromPx(prFontGlyphAdvance(m.font, prFontCodePointGlyph(m.font, codePoint), 0) * kSize);
    };
    PrLayoutRect rb = m.bounds(box);
    // The opening parenthesis stretches past its own advance to cover b.
    CHECK(pxOf(m.bounds(a).left - rb.left) > pxOf(advance('(')));
    CHECK(pxOf(m.bounds(b).left - m.bounds(a).right) == pxOf(advance(',') + layoutUnitPx(3)));
    CHECK(pxOf(m.bounds(c).left - m.bounds(b).right) == pxOf(advance(',') + layoutUnitPx(3)));
    // The fences cover at least 90.1% of b's height and depth, centered on the axis.
    float axis = m.c().axisHeight * kSize;
    float target = 2 * std::max(60 - axis, 40 + axis);
    CHECK(prLayoutUnitToPx(height(rb)) >= std::max(target * 0.901f, target - kSize / 2) - 1);
}

TEST_CASE("math right to left: a radical takes its sign's rtlm form, and its base comes before the sign") {
    // WPT's font draws the radical 4em wide, and its rtlm form 1em wide.
    for (PrTextDirection direction : {PrTextDirection_Ltr, PrTextDirection_Rtl}) {
        CAPTURE(int(direction));
        MathFixture m("radical-rtlm");
        m.direction = direction;
        PrLayoutNode math = m.root();
        PrLayoutNode sqrt = m.box(math, PrLayoutMathKind_SquareRoot);
        PrLayoutNode base = m.space(sqrt, 10, 5, 0);
        m.layout();
        PrLayoutRect r = m.bounds(sqrt);
        PrLayoutRect b = m.bounds(base);
        bool rtl = direction == PrTextDirection_Rtl;
        CHECK(pxOf(width(r)) == pxOf(layoutUnitPx(10) + m.u(rtl ? 1 : 4)));
        CHECK(pxOf(rtl ? b.left : b.right) == pxOf(rtl ? r.left : r.right));
    }
}

TEST_CASE("math right to left: a mirrored operator that the font has no mirrored form of is drawn mirrored") {
    MathFixture m;
    m.direction = PrTextDirection_Rtl;
    PrLayoutNode math = m.root();
    PrLayoutNode sum = m.token(math, "\xE2\x88\x91", PrLayoutMathKind_Operator, op(0));
    m.token(math, "(", PrLayoutMathKind_Operator, op(0));
    m.layout();
    auto find = [](auto& recurse, const LayoutFragment* at, PrLayoutNode node) -> const LayoutFragment* {
        if (at->node == node)
            return at;
        for (uint32_t i = 0; i < at->childCount; ++i) {
            if (const LayoutFragment* found = recurse(recurse, at->children[i].fragment, node))
                return found;
        }
        return nullptr;
    };
    auto mirrored = [](const LayoutFragment* fragment) {
        uint32_t count = 0;
        if ((fragment->flags & LayoutFragmentFlag_Math) == 0)
            return count;
        LayoutMathData data = layoutFragmentMathData(fragment);
        for (uint32_t i = 0; i < data.paintCount; ++i)
            count += data.paints[i].mirrored ? 1 : 0;
        return count;
    };
    auto total = [&](auto& recurse, const LayoutFragment* at) -> uint32_t {
        uint32_t count = mirrored(at);
        for (uint32_t i = 0; i < at->childCount; ++i)
            count += recurse(recurse, at->children[i].fragment);
        return count;
    };
    const LayoutFragment* fragment = find(find, m.f.tree->fragment, sum);
    REQUIRE(fragment != nullptr);
    CHECK(mirrored(fragment) == 1);
    // Shaping gives the parenthesis its mirror's glyph, which needs no mirrored drawing.
    CHECK(total(total, m.f.tree->fragment) == 1);
    // A cached paint drawn the other way fails verification.
    LayoutMathData data = layoutFragmentMathData(fragment);
    for (uint32_t i = 0; i < data.paintCount; ++i)
        const_cast<LayoutMathPaint&>(data.paints[i]).mirrored = !data.paints[i].mirrored;
    CHECK(!prLayoutTreeVerify(m.f.tree, nullptr));
}

TEST_CASE("math stack: numbers aligned on their decimal points, each character in its column, and a line below") {
    MathFixture m;
    PrLayoutNode math = m.root();
    PrLayoutNode stack = m.box(math, PrLayoutMathKind_Stack, 0, stackOf(PrLayoutMathAlign_DecimalPoint));
    PrLayoutNode a = m.token(stack, "4.24", PrLayoutMathKind_Token, stackPart(PrLayoutMathStackPart_Number));
    PrLayoutNode b = m.token(stack, "33.1", PrLayoutMathKind_Token, stackPart(PrLayoutMathStackPart_Number));
    PrLayoutNode line = prLayoutNodeCreateElement(m.f.tree, stack,
        m.style(PrLayoutDisplay_BlockMath, stackPart(PrLayoutMathStackPart_Line)));
    m.layout();
    auto advance = [&](uint32_t codePoint) {
        return prLayoutUnitFromPx(prFontGlyphAdvance(m.font, prFontCodePointGlyph(m.font, codePoint), 0) * kSize);
    };
    PrLayoutUnit digit = advance('3');
    PrLayoutRect rs = m.bounds(stack);
    // Five columns: 3, 3 and 4, the points, 2 and 1, and 4.
    CHECK(pxOf(width(rs)) == pxOf(digit * 4 + advance('.')));
    CHECK(pxOf(m.bounds(a).left - rs.left) == pxOf(digit));
    CHECK(pxOf(m.bounds(b).left) == pxOf(rs.left));
    // The line is the font's overbar gap below the second row's ink, and the rows are a baselineskip apart.
    const PrFontMathConstants& c = m.c();
    CHECK(pxOf(m.bounds(line).top - m.bounds(b).bottom) ==
        pxOf(layoutUnitMax(m.u(c.overbarVerticalGap), layoutUnitPx(1))));
    // The bottoms of their ink differ by less than a pixel from their baselines.
    CHECK(std::abs(pxOf(m.bounds(b).bottom - m.bounds(a).bottom) - pxOf(m.u(1.2f))) < 1);
}

TEST_CASE("math stack: a row's position moves it left by columns, and a carry sits over its column of the next row") {
    MathFixture m;
    PrLayoutNode math = m.root();
    PrLayoutNode stack = m.box(math, PrLayoutMathKind_Stack, 0, stackOf(PrLayoutMathAlign_Right));
    PrLayoutMathStyle carryStyle = stackPart(PrLayoutMathStackPart_Carry);
    carryStyle.carryLocation = PrLayoutMathCarryLocation_N;
    PrLayoutNode carry = m.token(stack, "1", PrLayoutMathKind_Token, carryStyle, 0, 1);
    PrLayoutNode a = m.token(stack, "123", PrLayoutMathKind_Token, stackPart(PrLayoutMathStackPart_Number));
    PrLayoutNode b = m.token(stack, "45", PrLayoutMathKind_Token, stackPart(PrLayoutMathStackPart_Number, 1));
    m.layout();
    PrLayoutUnit digit = prLayoutUnitFromPx(prFontGlyphAdvance(m.font, prFontCodePointGlyph(m.font, '3'), 0) * kSize);
    PrLayoutRect ra = m.bounds(a);
    PrLayoutRect rb = m.bounds(b);
    PrLayoutRect rc = m.bounds(carry);
    // 45 ends under the 2.
    CHECK(pxOf(rb.right) == pxOf(ra.left + digit * 2));
    // The carry is centered over the 3, half the overbar gap above its ink.
    PrLayoutUnit three = ra.left + digit * 2;
    CHECK(std::abs(prLayoutUnitToPx((rc.left + rc.right) - (three * 2 + digit))) <= 1.0f / 32);
    PrLayoutUnit gap = layoutUnitMax(m.u(m.c().overbarVerticalGap), layoutUnitPx(1));
    CHECK(pxOf(ra.top - rc.bottom) == pxOf(PrLayoutUnit{gap.raw / 2}));
}

TEST_CASE("math line breaks: a forced break starts a line with its operator, a baselineskip below and 2em in") {
    MathFixture m;
    PrLayoutNode math = m.root();
    PrLayoutNode a = m.space(math, 10, 10, 0);
    PrLayoutMathStyle forced = op(0, 4, 4);
    forced.breakPenalty = kPrLayoutMathForcedBreak;
    PrLayoutNode plus = m.token(math, "+", PrLayoutMathKind_Operator, forced);
    PrLayoutNode b = m.space(math, 10, 10, 0);
    m.layout();
    PrLayoutRect root = m.bounds(math);
    CHECK(pxOf(m.bounds(a).left) == pxOf(root.left));
    CHECK(pxOf(m.bounds(plus).left) == pxOf(root.left + m.u(2)));
    CHECK(pxOf(m.bounds(b).left) == pxOf(m.bounds(plus).right + layoutUnitPx(4)));
    CHECK(pxOf(m.bounds(b).top - m.bounds(a).top) == pxOf(m.u(1.2f)));
    // After the break, the operator ends the first line instead.
    MathFixture n;
    PrLayoutNode math2 = n.root();
    PrLayoutNode c = n.space(math2, 10, 10, 0);
    forced.breakStyle = PrLayoutMathBreakStyle_After;
    PrLayoutNode plus2 = n.token(math2, "+", PrLayoutMathKind_Operator, forced);
    PrLayoutNode d = n.space(math2, 10, 10, 0);
    n.layout();
    CHECK(pxOf(n.bounds(plus2).left) == pxOf(n.bounds(c).right + layoutUnitPx(4)));
    CHECK(pxOf(n.bounds(d).left) == pxOf(n.bounds(math2).left + n.u(2)));
}

TEST_CASE("math line breaks: the first and last lines take their own indents, lines their leading, and × shows a break") {
    MathFixture m;
    PrLayoutNode body = m.f.element(kPrLayoutNodeNone, block());
    PrLayoutBoxStyle narrow = block();
    narrow.width = prLayoutLengthPx(200);
    PrLayoutNode container = prLayoutNodeCreateElement(m.f.tree, body, m.f.style(narrow));
    PrLayoutMathStyle row = prLayoutMathStyleDefault();
    row.kind = PrLayoutMathKind_Row;
    PrLayoutNode math = prLayoutNodeCreateElement(m.f.tree, container, m.style(PrLayoutDisplay_BlockMath, row));
    // Three lines: the first right-aligned, the last left-aligned past 5px, 30px of leading between the lines.
    PrLayoutMathStyle first = op(0);
    first.breakPenalty = kPrLayoutMathForcedBreak;
    first.indentFirst = PrLayoutMathIndent_Right;
    first.lineLeading = layoutUnitPx(30);
    PrLayoutMathStyle last = op(0);
    last.breakPenalty = kPrLayoutMathForcedBreak;
    last.indentLast = PrLayoutMathIndent_Left;
    last.indentShiftLast = layoutUnitPx(5);
    // An invisible times that breaks the second line shows as a multiplication sign at its end.
    last.breakChar = 0xD7;
    last.breakStyle = PrLayoutMathBreakStyle_After;
    PrLayoutNode a = m.space(math, 20, 10, 0);
    PrLayoutNode plus = m.token(math, "+", PrLayoutMathKind_Operator, first);
    m.space(math, 20, 10, 0);
    PrLayoutNode times = m.token(math, "\xE2\x81\xA2", PrLayoutMathKind_Operator, last);
    PrLayoutNode c = m.space(math, 20, 10, 0);
    m.layout();
    PrLayoutRect box = m.bounds(container);
    CHECK(pxOf(m.bounds(a).right) == pxOf(box.right));
    CHECK(pxOf(m.bounds(c).left) == pxOf(box.left + layoutUnitPx(5)));
    // The + is the tallest box of the second line.
    CHECK(pxOf(m.bounds(plus).top - m.bounds(a).bottom) == pxOf(layoutUnitPx(30)));
    // The sign is drawn after the invisible times, at the end of the second line.
    const LayoutFragment* fragment = fragmentOf(m.f.tree->fragment, layoutNodeIndex(m.f.tree, math));
    REQUIRE(fragment != nullptr);
    LayoutMathData data = layoutFragmentMathData(fragment);
    uint16_t sign = prFontCodePointGlyph(m.font, 0xD7);
    bool drawn = false;
    for (uint32_t i = 0; i < data.glyphCount; ++i)
        drawn = drawn || data.glyphs[i].id == sign;
    CHECK(drawn);
    CHECK(m.bounds(times).bottom < m.bounds(c).top);
}

TEST_CASE("math line breaks: a display formula breaks at its operators to fit its container, after its relation") {
    MathFixture m;
    PrLayoutNode body = m.f.element(kPrLayoutNodeNone, block());
    PrLayoutBoxStyle narrow = block();
    narrow.width = prLayoutLengthPx(150);
    PrLayoutNode container = prLayoutNodeCreateElement(m.f.tree, body, m.f.style(narrow));
    PrLayoutMathStyle row = prLayoutMathStyleDefault();
    row.kind = PrLayoutMathKind_Row;
    PrLayoutNode math = prLayoutNodeCreateElement(m.f.tree, container, m.style(PrLayoutDisplay_BlockMath, row));
    PrLayoutMathStyle equals = op(0, 8, 8);
    equals.breakPenalty = 500;
    PrLayoutMathStyle plus = op(0, 6, 6);
    plus.breakPenalty = 700;
    std::vector<PrLayoutNode> terms;
    std::vector<PrLayoutNode> pluses;
    terms.push_back(m.space(math, 30, 10, 0));
    PrLayoutNode relation = m.token(math, "=", PrLayoutMathKind_Operator, equals);
    for (int i = 0; i < 5; ++i) {
        terms.push_back(m.space(math, 30, 10, 0));
        if (i < 4)
            pluses.push_back(m.token(math, "+", PrLayoutMathKind_Operator, plus));
    }
    m.layout();
    PrLayoutRect box = m.bounds(container);
    PrLayoutRect first = m.bounds(terms[0]);
    for (PrLayoutNode term : terms)
        CHECK(m.bounds(term).right <= box.right);
    CHECK(m.bounds(terms.back()).top > first.top);
    // Each line after the first starts with a +, where the right side of the equation starts.
    PrLayoutUnit rhs = m.bounds(relation).right + layoutUnitPx(8);
    int starts = 0;
    for (PrLayoutNode node : pluses) {
        PrLayoutRect r = m.bounds(node);
        if (r.top > first.top + layoutUnitPx(10)) {
            if (pxOf(r.left) == pxOf(rhs))
                ++starts;
        }
    }
    CHECK(starts >= 1);
}

#if defined(PR_STIX_MATH_FONT)
TEST_CASE("math under and over: an accent over a base taller than FlattenedAccentBaseHeight takes its flattened form") {
    MathFixture m(PR_STIX_MATH_FONT);
    float flattened = m.c().flattenedAccentBaseHeight * kSize;
    PrLayoutMathStyle accent = prLayoutMathStyleDefault();
    accent.scriptFlags = PrLayoutMathScriptFlag_Accent;
    PrLayoutNode math = m.root();
    PrLayoutNode low = m.box(math, PrLayoutMathKind_Over, 0, accent);
    m.space(low, 20, flattened / 2, 0);
    PrLayoutNode lowHat = m.token(low, "\xCC\x82", PrLayoutMathKind_Operator, op(0));
    PrLayoutNode high = m.box(math, PrLayoutMathKind_Over, 0, accent);
    m.space(high, 20, flattened + 4, 0);
    PrLayoutNode highHat = m.token(high, "\xCC\x82", PrLayoutMathKind_Operator, op(0));
    m.layout();
    uint16_t hat = prFontCodePointGlyph(m.font, 0x0302);
    uint16_t flat = prFontFeatureGlyph(m.font, PR_FONT_TAG('f', 'l', 'a', 'c'), hat);
    REQUIRE(flat != hat);
    auto inkHeight = [&](uint16_t glyph) {
        PrRect ink = prFontGlyphInkBounds(m.font, glyph);
        return prLayoutUnitFromPxCeil(-ink.top * kSize) + prLayoutUnitFromPxCeil(ink.bottom * kSize);
    };
    CHECK(pxOf(height(m.bounds(lowHat))) == pxOf(inkHeight(hat)));
    CHECK(pxOf(height(m.bounds(highHat))) == pxOf(inkHeight(flat)));
}
#endif
