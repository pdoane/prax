#include "pr/float.h"
#include "pr/layout_style.h"
#include "pr/layout_tree.h"
#include "pr/text_harfbuzz.h"

#include "doctest/doctest.h"

#include <initializer_list>

extern "C" int prTestLayoutCApi(void);

namespace pr_impl {

namespace {

// Returns the pixels of a length that is pixels, or NaN for any other length.
float px(PrLayoutLength length) {
    return prLayoutWordKind(length.bits) == PrLayoutWordKind_Px ? prLayoutUnitToPx(prLayoutWordPxValue(length.bits)) : kPrF32Nan;
}

// An empty tree over an empty font set.
struct Tree {
    PrFontSet* fonts = prFontSetCreate(nullptr);
    PrTextShaper* shaper = prTextHarfBuzzCreate();
    PrLayoutTree* tree = nullptr;

    Tree() {
        PrTextFontDesc defaultFontDesc{.set = fonts, .style = prFontStyleDefault()};
        PrTextFont* defaultFont = prTextFontCreate(&defaultFontDesc);
        PrLayoutTreeDesc desc{.font = defaultFont, .shaper = shaper, .targetPixelRatio = 1};
        tree = prLayoutTreeCreate(&desc);
        prTextFontRelease(defaultFont);
    }

    ~Tree() {
        prLayoutTreeDestroy(tree);
        prTextShaperDestroy(shaper);
        prFontSetDestroy(fonts);
    }
};

// Lays out `root` in an empty viewport. A layout first frees the records and groups that nothing names.
void collect(PrLayoutTree* tree, PrLayoutNode root = kPrLayoutNodeNone) {
    PrLayoutRoot viewport{root, PrLayoutVec{}};
    prLayoutTreeLayout(tree, &viewport, 1);
}

PrLayoutStyle withBox(PrLayoutTree* tree, float width) {
    PrLayoutBoxStyle box = prLayoutBoxStyleDefault();
    box.width = prLayoutLengthPx(width);
    PrLayoutStyle style = prLayoutStyleDefault();
    style.groups[PrLayoutGroup_Box] = prLayoutBoxStyleIntern(tree, &box);
    return style;
}

// Return leaves of a calculation, or an operation over `operands` nodes.
PrLayoutCalcNode px(double value) {
    return prLayoutCalcLeaf(PrLayoutCalcLeaf_Px, value);
}
PrLayoutCalcNode percent(double value) {
    return prLayoutCalcLeaf(PrLayoutCalcLeaf_Percent, value);
}
PrLayoutCalcNode number(double value) {
    return prLayoutCalcLeaf(PrLayoutCalcLeaf_Number, value);
}
PrLayoutCalcNode none() {
    return prLayoutCalcLeaf(PrLayoutCalcLeaf_None, 0);
}
PrLayoutCalcNode op(PrLayoutCalcOp operation, uint32_t operands) {
    return prLayoutCalcOperation(operation, operands);
}

// Returns the pixels that `length` resolves to against `base` pixels.
float resolved(const Tree& c, PrLayoutLength length, float base) {
    return prLayoutUnitToPx(prLayoutLengthResolve(c.tree, length, prLayoutUnitFromPx(base)));
}

// The groups' sizes, which determine the cost of hashing and comparing a style.
static_assert(sizeof(PrLayoutFontStyle) == 40);
static_assert(sizeof(PrLayoutBoxStyle) == 112);
static_assert(sizeof(PrLayoutEffectStyle) == 132);
static_assert(sizeof(PrLayoutColorStyle) == 12);
static_assert(sizeof(PrLayoutDecorationStyle) == 124);
static_assert(sizeof(PrLayoutFlexStyle) == 32);
static_assert(sizeof(PrLayoutTableStyle) == 16);

// Words made at compile time.
static_assert(prLayoutLengthPx(10).bits == 10u * 64 << 1);
static_assert(prLayoutLengthPercent(50).bits == (50u * 65536 << 2 | 1));
static_assert(prLayoutWordKind(prLayoutNumber(1.5f).bits) == PrLayoutWordKind_Number);
static_assert(prLayoutWordNumberValue(prLayoutNumber(1.5f).bits) == 1.5f);

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a word's kind is the prefix code in its low bits, and its payload reads back") {
    CHECK(prLayoutWordKind(prLayoutWordPx(-3.25f)) == PrLayoutWordKind_Px);
    CHECK(prLayoutUnitToPx(prLayoutWordPxValue(prLayoutWordPx(-3.25f))) == -3.25f);
    CHECK(prLayoutWordKind(prLayoutWordPercent(-12.5f)) == PrLayoutWordKind_Percent);
    CHECK(prLayoutWordPercentValue(prLayoutWordPercent(-12.5f)) == -12.5f);
    CHECK(prLayoutWordKind(prLayoutWordNumber(-0.75f)) == PrLayoutWordKind_Number);
    CHECK(prLayoutWordNumberValue(prLayoutWordNumber(-0.75f)) == -0.75f);
    CHECK(prLayoutWordKind(prLayoutWordCompound(12345)) == PrLayoutWordKind_Compound);
    CHECK(prLayoutWordCompoundIndex(prLayoutWordCompound(12345)) == 12345);
    CHECK(prLayoutWordKind(prLayoutWordInteger(-7)) == PrLayoutWordKind_Integer);
    CHECK(prLayoutWordIntegerValue(prLayoutWordInteger(-7)) == -7);
    CHECK(prLayoutWordKind(prLayoutWordKeyword(PrLayoutKeyword_Normal)) == PrLayoutWordKind_Keyword);
    CHECK(prLayoutWordKeywordValue(prLayoutWordKeyword(PrLayoutKeyword_Normal)) == PrLayoutKeyword_Normal);
}

TEST_CASE("pixels and percentages truncate toward zero and saturate, and a number keeps equal values equal") {
    // Pixels truncate toward zero to units, and saturate at 31 bits. The run-time conversion and the constant one
    // agree.
    volatile float tenth = 0.1f;
    volatile float big = 1e9f;
    volatile float nan = kPrF32Nan;
    for (float sign : {1.0f, -1.0f}) {
        CHECK(prLayoutWordPxValue(prLayoutWordPx(sign * tenth)).raw == int32_t(sign) * 6);
        CHECK(prLayoutWordPxValue(prLayoutWordPx(sign * 0.7f)).raw == int32_t(sign) * 44);
        CHECK(prLayoutWordPercentValue(prLayoutWordPercent(sign * 12.5f)) == sign * 12.5f);
    }
    static_assert(prLayoutWordPxValue(prLayoutWordPx(-0.7f)).raw == -44);
    static_assert(prLayoutWordPxValue(prLayoutWordPx(1e9f)).raw == kPrLayoutWordPxMax);
    CHECK(prLayoutWordPxValue(prLayoutWordPx(big)).raw == kPrLayoutWordPxMax);
    CHECK(prLayoutWordPxValue(prLayoutWordPx(-big)).raw == kPrLayoutWordPxMin);
    CHECK(prLayoutWordPx(nan) == prLayoutWordPx(0));
    CHECK(prLayoutWordPercentValue(prLayoutWordPercent(big)) == float(kPrLayoutWordPercentMax) / 65536);
    CHECK(prLayoutWordPercentValue(prLayoutWordPercent(-big)) == float(kPrLayoutWordPercentMin) / 65536);
    CHECK(prLayoutWordPercent(nan) == prLayoutWordPercent(0));
    CHECK(prLayoutWordPxValue(prLayoutWordPxUnits(INT32_MAX)).raw == kPrLayoutWordPxMax);
    // The three lowest mantissa bits drop toward zero, -0 is 0, and NaN is 0.
    CHECK(prLayoutWordNumberValue(prLayoutWordNumber(1.0000001f)) == 1.0f);
    CHECK(prLayoutWordNumber(-0.0f) == prLayoutWordNumber(0.0f));
    CHECK(prLayoutWordNumber(nan) == prLayoutWordNumber(0.0f));
}

TEST_CASE("a calculation interns as pixels, a percentage, or one compound for equal calculations") {
    Tree c;
    // calc(10px + 2 * 5px) has no percentage, so it is pixels.
    PrLayoutCalcNode pixels[] = {op(PrLayoutCalcOp_Sum, 2), px(10), op(PrLayoutCalcOp_Product, 2), number(2), px(5)};
    CHECK(prLayoutLengthIntern(c.tree, pixels, 5).bits == prLayoutLengthPx(20).bits);
    PrLayoutCalcNode thirty[] = {percent(30)};
    CHECK(prLayoutLengthIntern(c.tree, thirty, 1).bits == prLayoutLengthPercent(30).bits);
    // calc(10px + 50%).
    PrLayoutCalcNode sum[] = {op(PrLayoutCalcOp_Sum, 2), px(10), percent(50)};
    PrLayoutLength a = prLayoutLengthIntern(c.tree, sum, 3);
    CHECK(prLayoutWordKind(a.bits) == PrLayoutWordKind_Compound);
    CHECK(prLayoutLengthIntern(c.tree, sum, 3).bits == a.bits);
    CHECK(resolved(c, a, 200) == 110.0f);
    // A product of two lengths, and a number, are not lengths.
    PrLayoutCalcNode area[] = {op(PrLayoutCalcOp_Product, 2), px(2), percent(5)};
    CHECK(prLayoutLengthIntern(c.tree, area, 3).bits == prLayoutLengthAuto().bits);
    PrLayoutCalcNode three[] = {number(3)};
    CHECK(prLayoutLengthIntern(c.tree, three, 1).bits == prLayoutLengthAuto().bits);
    // calc(20% * 8px / 1px) is a length, because its units cancel as types multiply.
    PrLayoutCalcNode cancelled[] = {
        op(PrLayoutCalcOp_Product, 3), percent(20),
        px(8), op(PrLayoutCalcOp_Invert, 1), px(1),
    };
    CHECK(resolved(c, prLayoutLengthIntern(c.tree, cancelled, 5), 520) == 832.0f);
}

TEST_CASE("a calculation computes in doubles, and only its result becomes units") {
    Tree c;
    // A third as a word's percentage, 2184533/65536 %, is one unit short of 100px against 300px.
    PrLayoutCalcNode third[] = {percent(100.0 / 3)};
    PrLayoutLength exact = prLayoutLengthIntern(c.tree, third, 1);
    CHECK(prLayoutWordKind(exact.bits) == PrLayoutWordKind_Compound);
    CHECK(resolved(c, exact, 300) == 100.0f);
    CHECK(resolved(c, prLayoutLengthPercent(float(100.0 / 3)), 300) < 100.0f);
    // An infinity stays infinite until the result saturates.
    PrLayoutCalcNode infinite[] = {op(PrLayoutCalcOp_Sum, 2), percent(kPrF64Infinity), px(-1e300)};
    PrLayoutLength saturated = prLayoutLengthIntern(c.tree, infinite, 3);
    CHECK(prLayoutLengthResolve(c.tree, saturated, prLayoutUnitFromPx(10)).raw == INT32_MAX);
    // NaN propagates, even past an infinity, and a NaN result is 0 against any base.
    PrLayoutCalcNode nan[] = {op(PrLayoutCalcOp_Hypot, 2), percent(kPrF64Infinity), px(kPrF64Nan)};
    CHECK(prLayoutLengthIntern(c.tree, nan, 3).bits == prLayoutLengthPx(0).bits);
    PrLayoutCalcNode step[] = {op(PrLayoutCalcOp_RoundUp, 2), percent(30), px(kPrF64Nan)};
    CHECK(prLayoutLengthIntern(c.tree, step, 3).bits == prLayoutLengthPx(0).bits);
}

TEST_CASE("min(), max() and clamp() over percentages compute against the base") {
    Tree c;
    PrLayoutCalcNode least[] = {op(PrLayoutCalcOp_Min, 2), percent(50), px(100)};
    PrLayoutLength min = prLayoutLengthIntern(c.tree, least, 3);
    CHECK(resolved(c, min, 300) == 100.0f);
    CHECK(resolved(c, min, 100) == 50.0f);
    // clamp(none, 50%, 80px) is min(50%, 80px), with its lower bound left out.
    PrLayoutCalcNode clamp[] = {op(PrLayoutCalcOp_Clamp, 3), none(), percent(50), px(80)};
    PrLayoutLength clamped = prLayoutLengthIntern(c.tree, clamp, 4);
    CHECK(resolved(c, clamped, 300) == 80.0f);
    CHECK(resolved(c, clamped, 100) == 50.0f);
    // round(up, 30%, 16px) against 100px is 32px.
    PrLayoutCalcNode round[] = {op(PrLayoutCalcOp_RoundUp, 2), percent(30), px(16)};
    CHECK(resolved(c, prLayoutLengthIntern(c.tree, round, 3), 100) == 32.0f);
}

TEST_CASE("a compound lives while a group names it, and collecting frees it after") {
    // Each group whose lengths may be compounds: the box's width, the font's indent, and positions and origins.
    auto boxNaming = [](PrLayoutTree* tree, PrLayoutLength length) {
        PrLayoutBoxStyle box = prLayoutBoxStyleDefault();
        box.width = length;
        PrLayoutStyle style = prLayoutStyleDefault();
        style.groups[PrLayoutGroup_Box] = prLayoutBoxStyleIntern(tree, &box);
        return style;
    };
    auto fontNaming = [](PrLayoutTree* tree, PrLayoutLength length) {
        PrLayoutFontStyle font = prLayoutFontStyleDefault();
        font.textIndent = length;
        PrLayoutStyle style = prLayoutStyleDefault();
        style.groups[PrLayoutGroup_Font] = prLayoutFontStyleIntern(tree, &font);
        return style;
    };
    auto decorationNaming = [](PrLayoutTree* tree, PrLayoutLength length) {
        PrLayoutDecorationStyle decoration = prLayoutDecorationStyleDefault();
        decoration.objectPositionX = length;
        PrLayoutStyle style = prLayoutStyleDefault();
        style.groups[PrLayoutGroup_Decoration] = prLayoutDecorationStyleIntern(tree, &decoration);
        return style;
    };
    auto effectsNaming = [](PrLayoutTree* tree, PrLayoutLength length) {
        PrLayoutEffectStyle effects = prLayoutEffectStyleDefault();
        effects.originX = length;
        PrLayoutStyle style = prLayoutStyleDefault();
        style.groups[PrLayoutGroup_Effect] = prLayoutEffectStyleIntern(tree, &effects);
        return style;
    };
    PrLayoutStyle (*namings[])(PrLayoutTree*, PrLayoutLength) = {
        boxNaming, fontNaming, decorationNaming,
        effectsNaming,
    };
    for (PrLayoutStyle (*naming)(PrLayoutTree*, PrLayoutLength) : namings) {
        Tree c;
        PrLayoutCalcNode sum[] = {op(PrLayoutCalcOp_Sum, 2), px(4), percent(10)};
        PrLayoutLength length = prLayoutLengthIntern(c.tree, sum, 3);
        PrLayoutStyle style = naming(c.tree, length);
        uint32_t record = prLayoutStyleIntern(c.tree, &style);
        prLayoutStyleRetain(c.tree, record);
        collect(c.tree);
        CHECK(resolved(c, length, 100) == 14.0f);
        CHECK(prLayoutLengthIntern(c.tree, sum, 3).bits == length.bits);
        prLayoutStyleRelease(c.tree, record);
        collect(c.tree);
        // Its index is free, so a different calculation takes it.
        PrLayoutCalcNode other[] = {op(PrLayoutCalcOp_Sum, 2), px(5), percent(10)};
        CHECK(prLayoutLengthIntern(c.tree, other, 3).bits == length.bits);
    }
}

TEST_CASE("the public headers compile as C") {
    CHECK(prTestLayoutCApi() == 0);
}

TEST_CASE("index 0 of each table holds its default") {
    Tree c;
    PrLayoutFontStyle font = prLayoutFontStyleDefault();
    PrLayoutColorStyle color = prLayoutColorStyleDefault();
    PrLayoutBoxStyle box = prLayoutBoxStyleDefault();
    PrLayoutDecorationStyle decoration = prLayoutDecorationStyleDefault();
    PrLayoutEffectStyle effects = prLayoutEffectStyleDefault();
    PrLayoutStyle style = prLayoutStyleDefault();
    CHECK(prLayoutFontStyleIntern(c.tree, &font) == 0);
    CHECK(prLayoutColorStyleIntern(c.tree, &color) == 0);
    CHECK(prLayoutBoxStyleIntern(c.tree, &box) == 0);
    CHECK(prLayoutDecorationStyleIntern(c.tree, &decoration) == 0);
    CHECK(prLayoutEffectStyleIntern(c.tree, &effects) == 0);
    CHECK(prLayoutStyleIntern(c.tree, &style) == 0);
    CHECK(prLayoutBoxStyleRecord(c.tree, 0).display == PrLayoutDisplay_Inline);
    CHECK(prLayoutFontStyleRecord(c.tree, 0).font == nullptr);
}

TEST_CASE("equal groups and records intern to one index") {
    Tree c;
    PrLayoutStyle a = withBox(c.tree, 10);
    PrLayoutStyle b = withBox(c.tree, 10);
    PrLayoutStyle d = withBox(c.tree, 20);
    CHECK(a.groups[PrLayoutGroup_Box] == b.groups[PrLayoutGroup_Box]);
    CHECK(a.groups[PrLayoutGroup_Box] != d.groups[PrLayoutGroup_Box]);
    uint32_t first = prLayoutStyleIntern(c.tree, &a);
    CHECK(prLayoutStyleIntern(c.tree, &b) == first);
    CHECK(prLayoutStyleIntern(c.tree, &d) != first);
    CHECK(px(prLayoutBoxStyleRecord(c.tree, prLayoutStyleRecord(c.tree, first).groups[PrLayoutGroup_Box]).width) == 10.0f);
}

TEST_CASE("collecting frees what nothing names and keeps what a node or a caller names") {
    Tree c;
    PrLayoutStyle named = withBox(c.tree, 1);
    PrLayoutStyle held = withBox(c.tree, 2);
    PrLayoutStyle loose = withBox(c.tree, 3);
    uint32_t namedIndex = prLayoutStyleIntern(c.tree, &named);
    uint32_t heldIndex = prLayoutStyleIntern(c.tree, &held);
    uint32_t looseIndex = prLayoutStyleIntern(c.tree, &loose);
    PrLayoutNode node = prLayoutNodeCreateElement(c.tree, kPrLayoutNodeNone, namedIndex);
    prLayoutStyleRetain(c.tree, heldIndex);
    collect(c.tree);

    // The loose record and its box are freed, so their indices are reused by what is interned next.
    PrLayoutStyle next = withBox(c.tree, 4);
    CHECK(next.groups[PrLayoutGroup_Box] == loose.groups[PrLayoutGroup_Box]);
    CHECK(prLayoutStyleIntern(c.tree, &next) == looseIndex);
    CHECK(prLayoutStyleIntern(c.tree, &named) == namedIndex);
    CHECK(prLayoutStyleIntern(c.tree, &held) == heldIndex);

    // Once the node and the caller release them, they are freed too.
    prLayoutTreeErase(c.tree, node);
    prLayoutStyleRelease(c.tree, heldIndex);
    collect(c.tree);
    collect(c.tree);
    PrLayoutStyle again = withBox(c.tree, 5);
    uint32_t againIndex = prLayoutStyleIntern(c.tree, &again);
    CHECK((againIndex == namedIndex || againIndex == heldIndex || againIndex == looseIndex));
}

TEST_CASE("a group that a caller holds outlives the collect, and records made of it later name it") {
    Tree c;
    PrLayoutBoxStyle box = prLayoutBoxStyleDefault();
    box.width = prLayoutLengthPx(7);
    PrLayoutGroupIndex held = prLayoutBoxStyleIntern(c.tree, &box);
    prLayoutGroupRetain(c.tree, PrLayoutGroup_Box, held);
    collect(c.tree);
    CHECK(prLayoutTreeVerify(c.tree, nullptr));
    CHECK(prLayoutBoxStyleIntern(c.tree, &box) == held);

    PrLayoutStyle record = prLayoutStyleDefault();
    record.groups[PrLayoutGroup_Box] = held;
    uint32_t index = prLayoutStyleIntern(c.tree, &record);
    CHECK(px(prLayoutBoxStyleRecord(c.tree, prLayoutStyleRecord(c.tree, index).groups[PrLayoutGroup_Box]).width) == 7.0f);

    // Released, and named by no record that survives, the group is freed and its index taken by the next box.
    prLayoutGroupRelease(c.tree, PrLayoutGroup_Box, held);
    collect(c.tree);
    collect(c.tree);
    CHECK(prLayoutTreeVerify(c.tree, nullptr));
    PrLayoutBoxStyle other = prLayoutBoxStyleDefault();
    other.width = prLayoutLengthPx(8);
    CHECK(prLayoutBoxStyleIntern(c.tree, &other) == held);
}

TEST_CASE("a created group or record is its own entry, which interning never finds, and lives as an interned one") {
    Tree c;
    PrLayoutBoxStyle box = prLayoutBoxStyleDefault();
    box.width = prLayoutLengthPx(9);
    PrLayoutGroupIndex interned = prLayoutBoxStyleIntern(c.tree, &box);
    PrLayoutGroupIndex created = prLayoutBoxStyleCreate(c.tree, &box);
    PrLayoutGroupIndex again = prLayoutBoxStyleCreate(c.tree, &box);
    // Three entries of one value: interning finds only its own.
    CHECK(created != interned);
    CHECK(again != created);
    CHECK(prLayoutBoxStyleIntern(c.tree, &box) == interned);
    CHECK(px(prLayoutBoxStyleRecord(c.tree, created).width) == 9.0f);

    PrLayoutStyle record = prLayoutStyleDefault();
    record.groups[PrLayoutGroup_Box] = created;
    PrLayoutStyleIndex style = prLayoutStyleCreate(c.tree, &record);
    CHECK(prLayoutStyleCreate(c.tree, &record) != style);
    CHECK(prLayoutStyleIntern(c.tree, &record) != style);
    PrLayoutNode node = prLayoutNodeCreateElement(c.tree, kPrLayoutNodeNone, style);
    prLayoutGroupRetain(c.tree, PrLayoutGroup_Box, again);
    collect(c.tree);
    CHECK(prLayoutTreeVerify(c.tree, nullptr));
    // The node names the created record, which names its created group, and the caller holds the other.
    CHECK(prLayoutStyleRecord(c.tree, style).groups[PrLayoutGroup_Box] == created);
    CHECK(px(prLayoutBoxStyleRecord(c.tree, again).width) == 9.0f);
    CHECK(prLayoutGroupReferences(c.tree, PrLayoutGroup_Box, again) == 1);
    CHECK(prLayoutGroupReferences(c.tree, PrLayoutGroup_Box, created) == 1);
    prLayoutGroupRetain(c.tree, PrLayoutGroup_Box, created);
    CHECK(prLayoutGroupReferences(c.tree, PrLayoutGroup_Box, created) == 2);
    prLayoutGroupRelease(c.tree, PrLayoutGroup_Box, created);

    // Released, they are freed as interned entries are, and their indices are taken again.
    prLayoutTreeErase(c.tree, node);
    prLayoutGroupRelease(c.tree, PrLayoutGroup_Box, again);
    collect(c.tree);
    collect(c.tree);
    CHECK(prLayoutTreeVerify(c.tree, nullptr));
    PrLayoutBoxStyle other = prLayoutBoxStyleDefault();
    other.width = prLayoutLengthPx(10);
    PrLayoutGroupIndex next = prLayoutBoxStyleIntern(c.tree, &other);
    CHECK((next == created || next == again || next == interned));
}

TEST_CASE("an entry listed for collecting more than once is freed once, and one held again is kept") {
    Tree c;
    PrLayoutStyle twice = withBox(c.tree, 1);
    PrLayoutStyle back = withBox(c.tree, 2);
    uint32_t twiceIndex = prLayoutStyleIntern(c.tree, &twice);
    uint32_t backIndex = prLayoutStyleIntern(c.tree, &back);
    // Each falls to no references more than once before the layout. The second is held again afterward.
    for (int i = 0; i < 3; ++i) {
        prLayoutStyleRetain(c.tree, twiceIndex);
        prLayoutStyleRelease(c.tree, twiceIndex);
        prLayoutStyleRetain(c.tree, backIndex);
        prLayoutStyleRelease(c.tree, backIndex);
    }
    prLayoutStyleRetain(c.tree, backIndex);
    CHECK(prLayoutTreeVerify(c.tree, nullptr));
    collect(c.tree);
    CHECK(prLayoutTreeVerify(c.tree, nullptr));
    CHECK(prLayoutStyleIntern(c.tree, &back) == backIndex);
    // The freed index is taken once, by the next record interned, and the one after takes another.
    PrLayoutStyle first = withBox(c.tree, 3);
    PrLayoutStyle second = withBox(c.tree, 4);
    CHECK(prLayoutStyleIntern(c.tree, &first) == twiceIndex);
    CHECK(prLayoutStyleIntern(c.tree, &second) != twiceIndex);
    prLayoutStyleRelease(c.tree, backIndex);
}

TEST_CASE("a node's old record is freed by the layout after its style changes") {
    Tree c;
    PrLayoutStyle old = withBox(c.tree, 1);
    uint32_t oldIndex = prLayoutStyleIntern(c.tree, &old);
    PrLayoutNode node = prLayoutNodeCreateElement(c.tree, kPrLayoutNodeNone, oldIndex);
    collect(c.tree, node);
    PrLayoutStyle now = withBox(c.tree, 2);
    prLayoutNodeSetStyle(c.tree, node, prLayoutStyleIntern(c.tree, &now));
    collect(c.tree, node);
    CHECK(prLayoutTreeVerify(c.tree, nullptr));
    // The record and its box group are both free: what is interned next takes their indices.
    PrLayoutStyle next = withBox(c.tree, 3);
    CHECK(next.groups[PrLayoutGroup_Box] == old.groups[PrLayoutGroup_Box]);
    CHECK(prLayoutStyleIntern(c.tree, &next) == oldIndex);
}

TEST_CASE("a font group holds its text font while the tree holds the group") {
    Tree c;
    PrFontStyle style = prFontStyleDefault();
    style.size = 20;
    PrTextFontDesc desc{.set = c.fonts, .style = style};
    PrTextFont* font = prTextFontCreate(&desc);
    PrLayoutFontStyle group = prLayoutFontStyleDefault();
    group.font = font;
    uint32_t index = prLayoutFontStyleIntern(c.tree, &group);
    CHECK(prLayoutFontStyleIntern(c.tree, &group) == index);
    prTextFontRelease(font);
    // The group's reference keeps the text font.
    CHECK(prTextFontStyle(prLayoutFontStyleRecord(c.tree, index).font).size == 20.0f);
    // Once no record names the group, a collect frees it and releases the text font.
    collect(c.tree);
}
