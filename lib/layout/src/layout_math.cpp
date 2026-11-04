// Math layout: the content of math boxes, as TeX and the OpenType MATH table lay out formulas.

#include "layout/src/layout_math.h"

#include "pr/assert.h"
#include "pr/float.h"
#include "pr/font_math.h"
#include "pr/hash.h"
#include "pr/locale_common.h"
#include "pr/unicode_bidi.h"
#include "pr/utf8.h"

#include "layout/src/layout_block.h"
#include "layout/src/layout_box.h"
#include "layout/src/layout_intrinsic.h"
#include "layout/src/layout_position.h"
#include "layout/src/layout_sizing.h"

namespace pr_impl {

namespace {

// The inline size available to a child of a math box. Math content does not wrap, so a child that shrinks to fit takes
// its max-content size.
constexpr PrLayoutUnit kMathAvailable = layoutUnitPx(1 << 20);

// TeX's \delimiterfactor and \delimitershortfall (TeXbook Appendix G, rule 19), which size a fence. A fence covers at
// least 901/1000 of what it encloses, and falls short of it by at most half an em (5pt of a 10pt font).
constexpr float kDelimiterFactor = 0.901f;
constexpr float kDelimiterShortfall = 0.5f;

// The constants of a font without a face.
const PrFontMathConstants kNoConstants{};

// The font whose MATH constants lay out a box: the first available face of its font group, at its size.
struct MathFont {
    const PrFont* PR_NULLABLE font;
    float size;                     // px per em
    const PrFontMathConstants* c;

    // Returns `ems` ems of the font in layout units.
    PrLayoutUnit u(float ems) const { return prLayoutUnitFromPx(ems * size); }
};

MathFont mathFontOf(LayoutState& state, LayoutNodeIndex node) {
    const LayoutFontFaces& faces = layoutFontFaces(state.styles, layoutTreeNode(state.tree, node).style);
    if (faces.font == nullptr || faces.firstFace == kLayoutFaceNone)
        return MathFont{nullptr, prLayoutUnitToPx(faces.size), &kNoConstants};
    const PrFont* font = layoutFaceFont(faces, faces.firstFace);
    return MathFont{font, prLayoutUnitToPx(faces.size), prFontMathConstants(font)};
}

// A child of a math box: an element, or a run of text that is an anonymous item. Its metrics are those of its margin
// box, about its alphabetic baseline. Its position is the inline offset of its margin box and the shift of its baseline
// above the box's baseline.
struct Item {
    LayoutNodeIndex node;
    bool run;
    bool mathBox;                   // true if its display is a math display
    const PrLayoutBoxStyle* box;
    const PrLayoutMathStyle* math;
    const LayoutFragment* PR_NULLABLE fragment;     // a reference that the box holds until it places the fragment

    PrLayoutUnit width;
    PrLayoutUnit ascent;
    PrLayoutUnit descent;
    PrLayoutUnit inkAscent;
    PrLayoutUnit inkDescent;
    PrLayoutUnit italic;
    PrLayoutUnit attachment;        // from the margin box's inline-start edge, or kLayoutMathNoAttachment
    const PrFont* PR_NULLABLE glyphFont;
    float glyphSize;
    uint16_t glyph;
    bool extended;
    PrLayoutUnit marginStart;
    PrLayoutUnit baseline;          // from the block-start edge of its border box

    PrLayoutUnit x;
    PrLayoutUnit shift;

    // A fence or separator that the box draws itself (PrLayoutMathStyle::fences), which has no node. `fence` is its
    // character, or 0 for the item of a child. `fenceGlyph` is the glyph of the character in the box's font,
    // `fenceSize` the size in px that it stretches to or 0 if it is drawn unstretched, and `fenceDelta` how far below
    // its baseline the origin of its glyphs lies.
    uint32_t fence;
    uint16_t fenceGlyph;
    float fenceSize;
    PrLayoutUnit fenceDelta;
};

// The extent of a group of items laid out together, about their common baseline, with its inline size, line metrics,
// ink, italic correction and attachment.
struct Extent {
    PrLayoutUnit width;
    PrLayoutUnit ascent;
    PrLayoutUnit descent;
    PrLayoutUnit inkAscent;
    PrLayoutUnit inkDescent;
    PrLayoutUnit italic;
    PrLayoutUnit attachment;
};

// The math layout of one box.
struct Math {
    LayoutState& state;
    const LayoutMathInput& input;
    LayoutNodeIndex node;
    LayoutMode mode;
    const PrLayoutMathStyle& self;
    MathFont font;
    bool compact;                   // true if math-style is compact
    bool cramped;                   // true if math-shift is compact
    bool root;                      // true for the box of a math element, which its parent does not lay out as math
    LayoutSpace childSpace;
    RawArray<Item> items;
    RawArray<LayoutNodeIndex> outOfFlow;
    RawArray<PrLayoutMathStyle> fenceStyles; // the math groups of the fences among the items, never reallocated
    uint32_t paints;                         // the box's first paint in the scratch
    uint32_t glyphs;
};

const PrLayoutMathStyle& defaultMath() {
    static const PrLayoutMathStyle math = prLayoutMathStyleDefault();
    return math;
}

PrLayoutUnit half(PrLayoutUnit unit) {
    return PrLayoutUnit{unit.raw / 2};
}

// Returns true if the item is an embellished operator.
bool embellished(const Item& it) {
    return it.mathBox && it.math->embellished;
}

bool hasOperatorFlag(const Item& it, PrLayoutMathOperatorFlags flag) {
    return embellished(it) && (it.math->operatorFlags & flag) != 0;
}

// Returns true if the item stretches along the block axis, or along the inline axis.
bool stretchesBlock(const Item& it) {
    return hasOperatorFlag(it, PrLayoutMathOperatorFlag_Stretchy) &&
        !hasOperatorFlag(it, PrLayoutMathOperatorFlag_InlineAxis);
}
bool stretchesInline(const Item& it) {
    return hasOperatorFlag(it, PrLayoutMathOperatorFlag_Stretchy) &&
        hasOperatorFlag(it, PrLayoutMathOperatorFlag_InlineAxis);
}

// Returns true if the item is a character box, a single glyph that is not an extended shape. The scripts of a character
// box take no baseline drop (TeXbook Appendix G, rule 18a; OpenType MATH, SuperscriptBaselineDropMax).
bool isCharacter(const Item& it) {
    return it.glyph != 0 && !it.extended;
}

// Reads the metrics of the item's fragment.
void measure(Math& m, Item& it) {
    const LayoutFragment* f = it.fragment;
    LayoutLogicalSize size = layoutLogicalSize(m.mode, f->extent);
    PrLayoutUnit margin[LayoutSide_Count] = {};
    if (!it.run) {
        LayoutEdges e = layoutEdgesFromBox(m.state.styles, layoutLogicalBox(*it.box, m.mode), kLayoutUnitZero);
        for (LayoutSide side = 0; side < LayoutSide_Count; ++side)
            margin[side] = e.margin[side];
    }
    LayoutBaseline first = layoutFragmentBaseline(f, LayoutBaselineEdge_First);
    PrLayoutUnit baseline = first.present ? first.offset : size.blockSize;
    // A table aligns by its center on the math axis, or as its math group says (mathml-core/#table-or-matrix-mtable,
    // mathml4/#presm_mtable).
    if (it.math->kind == PrLayoutMathKind_Table && !it.mathBox) {
        switch (it.math->tableAlign) {
            case PrLayoutMathTableAlign_Axis:
                baseline = half(size.blockSize) + m.font.u(m.font.c->axisHeight);
                break;
            case PrLayoutMathTableAlign_Center:
                baseline = half(size.blockSize);
                break;
            case PrLayoutMathTableAlign_Top:
                baseline = kLayoutUnitZero;
                break;
            case PrLayoutMathTableAlign_Bottom:
                baseline = size.blockSize;
                break;
            default:
                break;
        }
    }
    it.baseline = baseline;
    it.marginStart = margin[LayoutSide_InlineStart];
    it.width = margin[LayoutSide_InlineStart] + size.inlineSize + margin[LayoutSide_InlineEnd];
    it.ascent = margin[LayoutSide_BlockStart] + baseline;
    it.descent = size.blockSize - baseline + margin[LayoutSide_BlockEnd];
    it.italic = kLayoutUnitZero;
    it.attachment = kLayoutMathNoAttachment;
    it.glyph = 0;
    it.glyphFont = nullptr;
    it.extended = false;
    if ((f->flags & LayoutFragmentFlag_Math) != 0) {
        const LayoutMathMetrics& metrics = layoutFragmentMathData(f).metrics;
        it.inkAscent = metrics.inkAscent;
        it.inkDescent = metrics.inkDescent;
        it.italic = metrics.italic;
        if (metrics.attachment != kLayoutMathNoAttachment)
            it.attachment = metrics.attachment + it.marginStart;
        it.glyph = metrics.glyph;
        it.glyphFont = metrics.glyphFont;
        it.glyphSize = metrics.glyphSize;
        it.extended = metrics.extended;
    } else {
        it.inkAscent = baseline;
        it.inkDescent = size.blockSize - baseline;
    }
}

void layOutFence(Math& m, Item& it, const LayoutMathStretch* PR_NULLABLE stretch);

// Lays out the item, with the stretch size constraint `stretch` or none, in place of any layout it had.
void layOut(Math& m, Item& it, const LayoutMathStretch* PR_NULLABLE stretch) {
    if (it.fence != 0) {
        layOutFence(m, it, stretch);
        return;
    }
    layoutFragmentRelease(it.fragment);
    LayoutSpace space = m.childSpace;
    space.stretch = stretch != nullptr && layoutMathStretchIsActive(*stretch) ? stretch : nullptr;
    LayoutResult result;
    if (it.run) {
        // An anonymous item is as wide as its text, which does not wrap.
        LayoutIntrinsicSizes sizes = layoutIntrinsicSizes(m.state, it.node, layoutIntrinsicInputFromSpace(space));
        LayoutRequests requests{
            .inlineAxis = LayoutRequest{.size = sizes.maxContent, .kind = LayoutRequestKind_Fixed, .definite = true},
        };
        result = layoutFinal(m.state, it.node, space, requests);
    } else {
        result = layoutBlock(m.state, it.node, space);
    }
    it.fragment = result.fragment;
    measure(m, it);
}

// Returns the extent of the items from `begin` to `end` where they are placed, about the box's baseline.
Extent extentOf(const Math& m, uint32_t begin, uint32_t end) {
    Extent e{.attachment = kLayoutMathNoAttachment};
    bool any = false;
    PrLayoutUnit left = kLayoutUnitZero;
    PrLayoutUnit right = kLayoutUnitZero;
    for (uint32_t i = begin; i < end; ++i) {
        const Item& it = m.items.data[i];
        if (!any) {
            e.ascent = it.ascent + it.shift;
            e.descent = it.descent - it.shift;
            e.inkAscent = it.inkAscent + it.shift;
            e.inkDescent = it.inkDescent - it.shift;
            left = it.x;
            right = it.x + it.width;
            any = true;
            continue;
        }
        e.ascent = layoutUnitMax(e.ascent, it.ascent + it.shift);
        e.descent = layoutUnitMax(e.descent, it.descent - it.shift);
        e.inkAscent = layoutUnitMax(e.inkAscent, it.inkAscent + it.shift);
        e.inkDescent = layoutUnitMax(e.inkDescent, it.inkDescent - it.shift);
        left = layoutUnitMin(left, it.x);
        right = layoutUnitMax(right, it.x + it.width);
    }
    e.width = right - left;
    return e;
}

// Moves the items from `begin` to `end` by `x` along the inline axis and by `shift` up.
void moveItems(Math& m, uint32_t begin, uint32_t end, PrLayoutUnit x, PrLayoutUnit shift) {
    for (uint32_t i = begin; i < end; ++i) {
        m.items.data[i].x += x;
        m.items.data[i].shift += shift;
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Paints

// Pushes a rule covering the rectangle from inline offset `x` and block offset `top` from the math content box's
// block-start edge, of `width` and `height`. The rectangle is logical, in px.
void pushRule(Math& m, PrLayoutUnit x, PrLayoutUnit top, PrLayoutUnit width, PrLayoutUnit height,
        LayoutMathPaintKind kind = LayoutMathPaintKind_Rule, float thickness = 0) {
    float left = prLayoutUnitToPx(x);
    float y = prLayoutUnitToPx(top);
    m.state.scratch->mathPaints.push(LayoutMathPaint{
        .rect = PrRect{left, y, left + prLayoutUnitToPx(width), y + prLayoutUnitToPx(height)},
        .thickness = thickness,
        .kind = kind,
    });
}

// Pushes a stroke of `thickness` px from (x0, y0) to (x1, y1), each a logical offset from the box's baseline at its
// start, with y down, ending in a head if `kind` is Arrow.
void pushLine(Math& m, PrLayoutUnit x0, PrLayoutUnit y0, PrLayoutUnit x1, PrLayoutUnit y1, float thickness,
        LayoutMathPaintKind kind = LayoutMathPaintKind_Line) {
    m.state.scratch->mathPaints.push(LayoutMathPaint{
        .rect = PrRect{prLayoutUnitToPx(x0), prLayoutUnitToPx(y0), prLayoutUnitToPx(x1), prLayoutUnitToPx(y1)},
        .thickness = thickness,
        .kind = kind,
    });
}

// Pushes a paint of the kept glyphs from `start` on, which are placed in ems about their origin, moved to `origin` px
// from the math content box's start corner and covering `box` there, drawn mirrored about the center of `box` if
// `mirrored` is set.
void pushKeptGlyphs(LayoutState& state, const PrFont* PR_NULLABLE font, float size, uint32_t start, PrVec2 origin,
        PrRect box, bool mirrored) {
    RawArray<PrFontGlyph>& kept = state.scratch->mathGlyphs;
    for (uint32_t i = start; i < kept.count; ++i) {
        PrVec2& position = kept.data[i].position;
        position = PrVec2{origin.x + position.x * size, origin.y + position.y * size};
    }
    state.scratch->mathPaints.push(LayoutMathPaint{
        .rect = box,
        .font = font,
        .size = size,
        .glyphStart = start,
        .glyphCount = kept.count - start,
        .kind = LayoutMathPaintKind_Glyphs,
        .mirrored = mirrored,
    });
}

// Pushes `count` glyphs, placed in ems about their origin, as pushKeptGlyphs does.
void pushGlyphs(LayoutState& state, const PrFont* PR_NULLABLE font, float size, const PrFontGlyph* glyphs,
        uint32_t count, PrVec2 origin, PrRect box, bool mirrored = false) {
    uint32_t start = state.scratch->mathGlyphs.count;
    state.scratch->mathGlyphs.append(glyphs, count);
    pushKeptGlyphs(state, font, size, start, origin, box, mirrored);
}

// A glyph stretched along an axis: its box in ems, the number of glyphs that draw it and the first of them, and their
// font. The glyphs themselves are placed when they are drawn (pushStretched), because an assembly may need any number.
struct Stretched {
    PrFontMathStretchInfo info;
    const PrFont* PR_NULLABLE font;
    uint32_t count;
    uint16_t first;

    // What the stretch was asked for, which places its glyphs again.
    uint16_t glyph;
    uint32_t codePoint;
    PrFontMathAxis axis;
    float size;                     // in ems
};

// Stretches `glyph` of `font`, the glyph of `codePoint` or one of its forms, along `axis` to `size` px. Returns false
// if the font does not stretch it. A font without a MathVariants table stretches it with the Unicode parts.
bool stretchGlyph(const PrFont* font, float em, uint32_t codePoint, uint16_t glyph, PrFontMathAxis axis, float size,
        Stretched* out) {
    *out = Stretched{.font = font, .glyph = glyph, .codePoint = codePoint, .axis = axis, .size = size / em};
    PrFontGlyph first{};
    out->count = prFontMathCodePointStretch(font, codePoint, glyph, axis, out->size, &out->info, &first, 1);
    out->first = first.id;
    return out->info.kind != PrFontMathStretchKind_None;
}

// Returns `glyph` of `font` drawn as it is, as a stretch of one glyph.
Stretched unstretched(const PrFont* font, uint16_t glyph) {
    PrRect bounds = prFontGlyphInkBounds(font, glyph);
    return Stretched{
        .info = PrFontMathStretchInfo{
            .kind = PrFontMathStretchKind_Glyph,
            .advance = prFontGlyphAdvance(font, glyph, 0),
            .ascent = -bounds.top,
            .descent = bounds.bottom,
            .italicsCorrection = prFontMathGlyphItalicsCorrection(font, glyph),
        },
        .font = font,
        .count = 1,
        .first = glyph,
        .glyph = glyph,
    };
}

// Pushes the glyphs of `stretched`, drawn at `size` px an em, as pushGlyphs does.
void pushStretched(LayoutState& state, const Stretched& stretched, float size, PrVec2 origin, PrRect box, bool mirrored = false) {
    if (stretched.count <= 1 || stretched.font == nullptr) {
        PrFontGlyph glyph{.id = stretched.first};
        pushGlyphs(state, stretched.font, size, &glyph, stretched.count, origin, box, mirrored);
        return;
    }
    // The stretch places its glyphs again, straight into the kept glyphs.
    RawArray<PrFontGlyph>& kept = state.scratch->mathGlyphs;
    uint32_t start = kept.count;
    kept.resize(start + stretched.count);
    PrFontMathStretchInfo info{};
    uint32_t count = prFontMathCodePointStretch(stretched.font, stretched.codePoint, stretched.glyph, stretched.axis,
        stretched.size, &info, kept.data + start, stretched.count);
    PR_ASSERT(count == stretched.count);
    pushKeptGlyphs(state, stretched.font, size, start, origin, box, mirrored);
}

// Stretches `glyph` of `font` as stretchGlyph does. If `font` has no MATH table and cannot build `codePoint` from the
// Unicode parts, it uses a face of the font group `group` that either has every part, so that one face draws the whole
// assembly (mathml-core/#unicode-based-glyph-assemblies), or has a MATH table that stretches the glyph. The faces tried
// are those in which the group sets each part on its own, which may come from its fallback families.
bool stretchGlyphInFaces(LayoutState& state, PrLayoutGroupIndex group, const PrFont* font, float em,
        uint32_t codePoint, uint16_t glyph, PrFontMathAxis axis, float size, Stretched* out) {
    if (stretchGlyph(font, em, codePoint, glyph, axis, size, out))
        return true;
    if (prFontHasMath(font) || codePoint == 0)
        return false;
    uint32_t parts[4] = {};
    uint32_t count = prFontMathUnicodeParts(codePoint, axis, parts, 4);
    for (uint32_t i = 0; i < count && i < 4; ++i) {
        char bytes[kPrUtf8MaxBytes];
        uint32_t length = prUtf8Encode(parts[i], bytes);
        LayoutFaceIndex face = layoutFaceFor(state.styles, group, PrStringView{bytes, length}, PrScript_Zyyy);
        if (face == kLayoutFaceNone)
            continue;
        const PrFont* other = layoutFaceFont(layoutFontFacesOfGroup(state.styles, group), face);
        uint16_t otherGlyph = prFontCodePointGlyph(other, codePoint);
        if (other == font || otherGlyph == 0)
            continue;
        // The face must stretch the operator, because its unstretched glyph is no better than the first face's.
        Stretched found{};
        if (stretchGlyph(other, em, codePoint, otherGlyph, axis, size, &found) &&
                (found.info.kind == PrFontMathStretchKind_Assembly || found.first != otherGlyph)) {
            *out = found;
            return true;
        }
    }
    return false;
}

// A glyph that math layout draws for a character, with the character and whether the glyph is drawn mirrored.
struct DrawnGlyph {
    uint16_t glyph;
    uint32_t codePoint;
    bool mirrored;
};

// Returns the glyph that draws `codePoint` in `font`, mirrored in a right-to-left formula as MathML Core gets a glyph
// for a character (mathml-core/#dfn-get-a-glyph). The mirrored glyph is the rtlm form, or else the glyph of the
// Bidi_Mirroring_Glyph. A character that a right-to-left run mirrors but that has neither, such as the radical sign, is
// drawn mirrored, which MathML Core does not do.
DrawnGlyph drawnGlyph(const PrFont* font, uint32_t codePoint, bool rtl) {
    DrawnGlyph drawn{prFontCodePointGlyph(font, codePoint), codePoint, false};
    if (!rtl || drawn.glyph == 0)
        return drawn;
    uint16_t form = prFontFeatureGlyph(font, PR_FONT_TAG('r', 't', 'l', 'm'), drawn.glyph);
    if (form != drawn.glyph) {
        drawn.glyph = form;
        return drawn;
    }
    uint32_t mirror = prUnicodeBidiMirror(codePoint);
    uint16_t mirrorGlyph = mirror != codePoint ? prFontCodePointGlyph(font, mirror) : 0;
    if (mirrorGlyph != 0)
        return DrawnGlyph{mirrorGlyph, mirror, false};
    drawn.mirrored = prUnicodeBidiIsMirrored(codePoint);
    return drawn;
}

// Returns true if the box lays out right to left.
bool isRtl(const Math& m) {
    return m.mode.direction == PrTextDirection_Rtl;
}

// A spacing accent, and the combining accent of the same shape that math fonts give their wide variants to.
struct AccentPair {
    uint32_t spacing;
    uint32_t combining;
};

constexpr AccentPair kAccentPairs[] = {
    {0x005E, 0x0302}, {0x02C6, 0x0302}, {0x007E, 0x0303}, {0x02DC, 0x0303}, {0x00AF, 0x0305}, {0x203E, 0x0305},
    {0x02C7, 0x030C}, {0x02D8, 0x0306}, {0x005F, 0x0332},
};

// Returns the glyph in `font` of the combining accent that has the shape of the spacing accent `glyph`, or 0 if there
// is none. Math fonts give their wide variants to the combining accents, as New Computer Modern Math does for the
// circumflex and the tilde. So a spacing accent stretches as its combining form if that reaches further.
uint16_t combiningGlyph(const PrFont* font, uint32_t codePoint) {
    for (const AccentPair& pair : kAccentPairs) {
        if (pair.spacing == codePoint)
            return prFontCodePointGlyph(font, pair.combining);
    }
    return 0;
}

// Returns the first code point of the text of `node` that is not white space, or 0 if there is none.
uint32_t firstCodePoint(const LayoutTree* tree, LayoutNodeIndex node) {
    if (node == kPrLayoutNodeNone)
        return 0;
    PrStringView text = layoutNodeText(tree, node);
    for (size_t at = 0; at < text.size;) {
        uint32_t codePoint = 0;
        uint32_t length = prUtf8Decode(text, at, &codePoint);
        if (length == 0)
            return 0;
        if (codePoint != ' ' && codePoint != '\t' && codePoint != '\n' && codePoint != '\r')
            return codePoint;
        at += length;
    }
    return 0;
}

// Returns true if the embellished operator `it` is a horizontal bar, an overline or an underline, which TeX sets with
// the bar's gap (TeXbook Appendix G, rules 9 and 10; OpenType MATH, OverbarVerticalGap) rather than a stretch stack's.
bool isBar(const LayoutTree* tree, const Item& it) {
    LayoutNodeIndex node = it.node;
    while (node != kPrLayoutNodeNone && layoutTreeNode(tree, node).kind != PrLayoutNodeKind_Text)
        node = layoutTreeNode(tree, node).first;
    switch (firstCodePoint(tree, node)) {
        case 0x005F:
        case 0x00AF:
        case 0x02C9:
        case 0x0305:
        case 0x0332:
        case 0x203E:
        case 0x2015:
        case 0x23AF:
            return true;
        default:
            return false;
    }
}

// Returns true if the embellished operator `it` is stretched along the inline axis and is drawn as a brace, a bracket
// or an arrow rather than as an accent or a bar. Such an operator is the top or bottom of a stretch stack, which TeX
// keeps clear of its base (\overbrace, \overrightarrow) whether or not MathML makes it an accent.
bool isStackPart(const LayoutTree* tree, const Item& it) {
    if (!stretchesInline(it) || isBar(tree, it))
        return false;
    LayoutNodeIndex node = it.node;
    while (node != kPrLayoutNodeNone && layoutTreeNode(tree, node).kind != PrLayoutNodeKind_Text)
        node = layoutTreeNode(tree, node).first;
    uint32_t codePoint = firstCodePoint(tree, node);
    for (const AccentPair& pair : kAccentPairs) {
        if (pair.spacing == codePoint || pair.combining == codePoint)
            return false;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// Rows (mathml-core/#layout-of-mrow)

// Lays out the items from `begin` to `end` with the algorithm for stretching operators along the block axis
// (mathml-core/#algorithm-for-stretching-operators-along-the-block-axis). Given a constraint, the box is an embellished
// operator, whose embellished child takes it. Otherwise the operators that stretch along the block axis cover the ink
// of the others.
void stretchRow(Math& m, uint32_t begin, uint32_t end, const LayoutMathStretch* PR_NULLABLE stretch) {
    // An embellished operator's operator covers the constraint, and the ink of the space-like items beside it, which it
    // would cover as a row's operator.
    if (stretch != nullptr) {
        LayoutMathStretch covered = *stretch;
        for (uint32_t i = begin; i < end; ++i) {
            Item& it = m.items.data[i];
            if (embellished(it))
                continue;
            layOut(m, it, nullptr);
            if (covered.axis == LayoutMathStretchAxis_Block) {
                covered.size = layoutUnitMax(covered.size, it.inkAscent);
                covered.depth = layoutUnitMax(covered.depth, it.inkDescent);
            }
        }
        for (uint32_t i = begin; i < end; ++i) {
            if (embellished(m.items.data[i]))
                layOut(m, m.items.data[i], &covered);
        }
        return;
    }
    bool toStretch = false;
    bool others = false;
    LayoutMathStretch target{.axis = LayoutMathStretchAxis_Block};
    for (uint32_t i = begin; i < end; ++i) {
        Item& it = m.items.data[i];
        if (stretchesBlock(it)) {
            toStretch = true;
            continue;
        }
        layOut(m, it, nullptr);
        target.size = others ? layoutUnitMax(target.size, it.inkAscent) : it.inkAscent;
        target.depth = others ? layoutUnitMax(target.depth, it.inkDescent) : it.inkDescent;
        others = true;
    }
    if (!toStretch)
        return;
    // Operators that only stretch beside one another cover the tallest of them unstretched, as a constraint of zero
    // does with their minimum sizes, so that a row of fences has fences of one size.
    if (!others) {
        target = LayoutMathStretch{.axis = LayoutMathStretchAxis_Block};
        bool first = true;
        for (uint32_t i = begin; i < end; ++i) {
            Item& it = m.items.data[i];
            layOut(m, it, nullptr);
            target.size = first ? it.inkAscent : layoutUnitMax(target.size, it.inkAscent);
            target.depth = first ? it.inkDescent : layoutUnitMax(target.depth, it.inkDescent);
            first = false;
        }
    }
    for (uint32_t i = begin; i < end; ++i) {
        if (stretchesBlock(m.items.data[i]))
            layOut(m, m.items.data[i], &target);
    }
}

// Places the items from `begin` to `end`, already laid out, in a row from inline offset zero on the baseline, with the
// spaces around embellished operators if `addSpace` is set, and italic correction after a slanted item that a straight
// one follows. Returns the row's extent with the italic correction of its last item. The width holds that correction
// only in a math element's box. Elsewhere the parent adds it as it adds a token's, so that an mrow around an item
// changes nothing.
Extent placeRow(Math& m, uint32_t begin, uint32_t end, bool addSpace) {
    PrLayoutUnit x = kLayoutUnitZero;
    PrLayoutUnit previousItalic = kLayoutUnitZero;
    for (uint32_t i = begin; i < end; ++i) {
        Item& it = m.items.data[i];
        bool operatorItem = embellished(it);
        bool slanted = !operatorItem && it.italic != kLayoutUnitZero;
        if (!slanted)
            x += previousItalic;
        if (operatorItem && addSpace)
            x += it.math->leftSpace;
        it.x = x;
        it.shift = kLayoutUnitZero;
        x += it.width;
        previousItalic = slanted ? it.italic : kLayoutUnitZero;
        if (operatorItem && addSpace)
            x += it.math->rightSpace;
    }
    Extent e = extentOf(m, begin, end);
    e.width = m.root ? x + previousItalic : x;
    e.italic = previousItalic;
    if (end == begin + 1)
        e.attachment = m.items.data[begin].attachment;
    return e;
}

// Lays out and places the items from `begin` to `end` as a row.
Extent row(Math& m, uint32_t begin, uint32_t end, const LayoutMathStretch* PR_NULLABLE stretch) {
    stretchRow(m, begin, end, stretch);
    // The math element adds the spaces of an embellished operator that it holds, and an embellished operator itself
    // leaves them to its parent (mathml-core/#layout-of-mrow).
    bool addSpace = m.root || !m.self.embellished;
    return placeRow(m, begin, end, addSpace);
}

// ---------------------------------------------------------------------------------------------------------------------
// Fractions (mathml-core/#fractions-mfrac)

PrLayoutUnit alignedX(PrLayoutMathAlign align, PrLayoutUnit width, PrLayoutUnit within) {
    switch (align) {
        case PrLayoutMathAlign_Left:
            return kLayoutUnitZero;
        case PrLayoutMathAlign_Right:
            return within - width;
        default:
            return half(within - width);
    }
}

Extent fraction(Math& m, const LayoutMathStretch* PR_NULLABLE stretch) {
    Item& num = m.items.data[0];
    Item& den = m.items.data[1];
    layOut(m, num, stretch);
    layOut(m, den, nullptr);
    const PrFontMathConstants& c = *m.font.c;
    PrLayoutUnit axis = m.font.u(c.axisHeight);
    PrLayoutUnit defaultThickness = m.font.u(c.fractionRuleThickness);
    PrLayoutUnit thickness = defaultThickness;
    if (!layoutLengthIsAuto(m.self.lineThickness))
        thickness =
            layoutUnitMax(kLayoutUnitZero, layoutResolve(m.state.styles, m.self.lineThickness, defaultThickness));

    // A bevelled fraction sets its numerator raised and its denominator lowered on either side of a slash, apart by the
    // font's skewed fraction gaps (mathml4/#presm_mfrac).
    if ((m.self.scriptFlags & PrLayoutMathScriptFlag_Bevelled) != 0) {
        PrLayoutUnit gapX = m.font.u(c.skewedFractionHorizontalGap);
        PrLayoutUnit gapY = m.font.u(c.skewedFractionVerticalGap);
        num.shift = axis + half(gapY) + num.inkDescent;
        den.shift = axis - half(gapY) - den.inkAscent;
        num.x = kLayoutUnitZero;
        den.x = num.width + gapX;
        Extent e = extentOf(m, 0, 2);
        e.width = den.x + den.width;
        PrLayoutUnit top = e.inkAscent;
        PrLayoutUnit bottom = e.inkDescent;
        PrLayoutUnit middle = num.width + half(gapX);
        PrLayoutUnit slant = half(top + bottom) / 3;
        pushLine(m, middle - slant, bottom, middle + slant, -top, prLayoutUnitToPx(thickness));
        return e;
    }

    PrLayoutUnit width = layoutUnitMax(num.width, den.width);
    num.x = alignedX(m.self.numeratorAlign, num.width, width);
    den.x = alignedX(m.self.denominatorAlign, den.width, width);
    if (thickness == kLayoutUnitZero) {
        // A fraction with no bar is a stack (mathml-core/#fraction-with-zero-line-thickness).
        PrLayoutUnit top = m.font.u(m.compact ? c.stackTopShiftUp : c.stackTopDisplayStyleShiftUp);
        PrLayoutUnit bottom = m.font.u(m.compact ? c.stackBottomShiftDown : c.stackBottomDisplayStyleShiftDown);
        PrLayoutUnit gapMin = m.font.u(m.compact ? c.stackGapMin : c.stackDisplayStyleGapMin);
        PrLayoutUnit gap = (bottom - den.inkAscent) + (top - num.inkDescent);
        PrLayoutUnit delta = gapMin - gap;
        if (delta > kLayoutUnitZero) {
            top += half(delta);
            bottom += delta - half(delta);
        }
        num.shift = top;
        den.shift = -bottom;
        Extent e = extentOf(m, 0, 2);
        e.width = width;
        return e;
    }
    PrLayoutUnit numShift = layoutUnitMax(
        m.font.u(m.compact ? c.fractionNumeratorShiftUp : c.fractionNumeratorDisplayStyleShiftUp),
        axis + half(thickness) + m.font.u(m.compact ? c.fractionNumeratorGapMin : c.fractionNumDisplayStyleGapMin) +
            num.inkDescent);
    PrLayoutUnit denShift = layoutUnitMax(
        m.font.u(m.compact ? c.fractionDenominatorShiftDown : c.fractionDenominatorDisplayStyleShiftDown),
        half(thickness) + m.font.u(m.compact ? c.fractionDenominatorGapMin : c.fractionDenomDisplayStyleGapMin) +
            den.inkAscent - axis);
    num.shift = numShift;
    den.shift = -denShift;
    Extent e = extentOf(m, 0, 2);
    e.width = width;
    PrLayoutUnit barTop = axis + half(thickness);
    e.ascent = layoutUnitMax(e.ascent, barTop);
    e.descent = layoutUnitMax(e.descent, thickness - barTop);
    e.inkAscent = layoutUnitMax(e.inkAscent, barTop);
    e.inkDescent = layoutUnitMax(e.inkDescent, thickness - barTop);
    // The bar is recorded about the baseline, and placed once the box's ascent is known (finishPaints).
    pushRule(m, kLayoutUnitZero, -barTop, width, thickness);
    return e;
}

// ---------------------------------------------------------------------------------------------------------------------
// Radicals (mathml-core/#radicals-msqrt-mroot)

// Lays out a radical sign over the items from `begin` to `end`, laid out as a row with operator spaces if `inRow` is
// set, and returns its extent. The sign covers the base's ink, its gap and its rule. The rule is the gap above the
// base's ink, and a sign that the font draws taller hangs from the rule and reaches further below the base
// (mathml-core/#radicals-msqrt-mroot). TeX instead shares the excess between the gaps above and below the base (TeXbook
// Appendix G, rule 11), which spaces the rules of nested radicals unevenly as their signs step between sizes.
Extent radical(Math& m, uint32_t begin, uint32_t end, const LayoutMathStretch* PR_NULLABLE stretch, bool inRow) {
    Extent base{};
    if (inRow) {
        base = row(m, begin, end, stretch);
    } else {
        stretchRow(m, begin, end, stretch);
        base = placeRow(m, begin, end, false);
    }
    const PrFontMathConstants& c = *m.font.c;
    PrLayoutUnit rule = m.font.u(c.radicalRuleThickness);
    PrLayoutUnit gap = m.font.u(m.compact ? c.radicalVerticalGap : c.radicalDisplayStyleVerticalGap);
    PrLayoutUnit extra = m.font.u(c.radicalExtraAscender);
    PrLayoutUnit target = rule + gap + base.inkAscent + base.inkDescent;
    Stretched sign{};
    DrawnGlyph glyph = m.font.font != nullptr ? drawnGlyph(m.font.font, 0x221A, isRtl(m)) : DrawnGlyph{};
    bool drawn = glyph.glyph != 0 && stretchGlyph(m.font.font, m.font.size, glyph.codePoint, glyph.glyph,
        PrFontMathAxis_Vertical, prLayoutUnitToPx(target), &sign);
    PrLayoutUnit signAdvance = drawn ? m.font.u(sign.info.advance) : kLayoutUnitZero;
    PrLayoutUnit signAscent = drawn ? m.font.u(sign.info.ascent) : target;
    PrLayoutUnit signDescent = drawn ? m.font.u(sign.info.descent) : kLayoutUnitZero;
    moveItems(m, begin, end, signAdvance, kLayoutUnitZero);
    Extent e{.attachment = kLayoutMathNoAttachment};
    e.width = signAdvance + base.width;
    PrLayoutUnit barTop = base.inkAscent + gap + rule;
    e.ascent = layoutUnitMax(base.ascent, barTop + extra);
    PrLayoutUnit signBottom = signAscent + signDescent - barTop;
    e.descent = layoutUnitMax(base.descent, signBottom);
    // The ink holds the extra ascender above the rule, as TeX's radical box holds a kern of the rule's thickness above
    // it (TeXbook Appendix G, rule 11), so that a radical in the base of another takes a larger sign whose rule clears
    // its own.
    e.inkAscent = barTop + extra;
    e.inkDescent = layoutUnitMax(base.inkDescent, signBottom);
    pushRule(m, signAdvance, -barTop, base.width, rule);
    if (drawn) {
        // The sign's top meets the top of the bar.
        float originY = prLayoutUnitToPx(signAscent - barTop);
        pushStretched(m.state, sign, m.font.size, PrVec2{0, originY},
            PrRect{0, -prLayoutUnitToPx(barTop), prLayoutUnitToPx(signAdvance), prLayoutUnitToPx(signBottom)},
            glyph.mirrored);
    }
    return e;
}

Extent root(Math& m, const LayoutMathStretch* PR_NULLABLE stretch) {
    // The index lays out first, so that the base and its sign follow it among the items.
    Item& index = m.items.data[1];
    layOut(m, index, nullptr);
    // An mroot's base is one child, not a row, so that no operator spaces it (mathml-core/#radicals-msqrt-mroot).
    Extent b = radical(m, 0, 1, stretch, false);
    const PrFontMathConstants& c = *m.font.c;
    PrLayoutUnit before = layoutUnitMax(kLayoutUnitZero, m.font.u(c.radicalKernBeforeDegree));
    PrLayoutUnit after = layoutUnitMax(-index.width, m.font.u(c.radicalKernAfterDegree));
    PrLayoutUnit offset = before + index.width + after;
    // The base and the sign move past the index, which takes the paints of the sign along (finishPaints moves them).
    m.items.data[0].x += offset;
    for (uint32_t i = m.paints; i < m.state.scratch->mathPaints.count; ++i) {
        PrRect& r = m.state.scratch->mathPaints.data[i].rect;
        float dx = prLayoutUnitToPx(offset);
        r.left += dx;
        r.right += dx;
    }
    for (uint32_t i = m.glyphs; i < m.state.scratch->mathGlyphs.count; ++i)
        m.state.scratch->mathGlyphs.data[i].position.x += prLayoutUnitToPx(offset);
    index.x = before;
    PrLayoutUnit height = b.ascent + b.descent;
    PrLayoutUnit raise = PrLayoutUnit{int32_t(float(height.raw) * c.radicalDegreeBottomRaisePercent)};
    index.shift = -b.descent + raise + index.descent;
    Extent e = b;
    e.width = offset + b.width;
    e.ascent = layoutUnitMax(b.ascent, index.shift + index.ascent);
    e.descent = layoutUnitMax(b.descent, index.descent - index.shift);
    e.inkAscent = layoutUnitMax(b.inkAscent, index.shift + index.inkAscent);
    return e;
}

// ---------------------------------------------------------------------------------------------------------------------
// Scripts (mathml-core/#script-and-limit-schemata)

// Returns the cut-in kern of a script beside a base glyph (OpenType MATH, MathKernInfo). It is the least of the sums of
// the base's corner kern and the script's opposite corner kern at the two correction heights. `heights` are measured
// from the base's baseline, and `shift` is the script's baseline above it. Returns zero unless both are glyphs.
PrLayoutUnit cornerKern(const Item& base, PrFontMathCorner baseCorner, const Item& script,
        PrFontMathCorner scriptCorner, PrLayoutUnit shift, PrLayoutUnit first, PrLayoutUnit second) {
    if (base.glyph == 0 || script.glyph == 0 || base.glyphFont == nullptr || script.glyphFont == nullptr)
        return kLayoutUnitZero;
    auto sum = [&](PrLayoutUnit height) {
        float baseKern = prFontMathGlyphKern(base.glyphFont, base.glyph, baseCorner,
            prLayoutUnitToPx(height) / base.glyphSize) * base.glyphSize;
        float scriptKern = prFontMathGlyphKern(script.glyphFont, script.glyph, scriptCorner,
            prLayoutUnitToPx(height - shift) / script.glyphSize) * script.glyphSize;
        return baseKern + scriptKern;
    };
    return prLayoutUnitFromPx(prF32Min(sum(first), sum(second)));
}

// The vertical shifts of a base's scripts.
struct ScriptShifts {
    PrLayoutUnit sub;
    PrLayoutUnit super;
};

// Returns the shifts of the scripts `subs` and `supers` of `base` (mathml-core/#base-with-subscript-and-superscript),
// each list `count` items long, with items absent as null. A base that is a character box takes no baseline drop
// (TeXbook Appendix G, rule 18a).
ScriptShifts scriptShifts(Math& m, const Item& base, Item* const* subs, Item* const* supers, uint32_t count) {
    const PrFontMathConstants& c = *m.font.c;
    bool character = isCharacter(base);
    ScriptShifts s{};
    bool anySub = false;
    bool anySuper = false;
    s.sub = m.font.u(c.subscriptShiftDown);
    if (!character)
        s.sub = layoutUnitMax(s.sub, m.font.u(c.subscriptBaselineDropMin) + base.inkDescent);
    s.super = m.font.u(m.cramped ? c.superscriptShiftUpCramped : c.superscriptShiftUp);
    if (!character)
        s.super = layoutUnitMax(s.super, base.inkAscent - m.font.u(c.superscriptBaselineDropMax));
    for (uint32_t i = 0; i < count; ++i) {
        if (subs[i] != nullptr) {
            anySub = true;
            s.sub = layoutUnitMax(s.sub, subs[i]->inkAscent - m.font.u(c.subscriptTopMax));
        }
        if (supers[i] != nullptr) {
            anySuper = true;
            s.super = layoutUnitMax(s.super, m.font.u(c.superscriptBottomMin) + supers[i]->inkDescent);
        }
    }
    if (!anySub || !anySuper)
        return s;
    // The gap between each pair of scripts is at least SubSuperscriptGapMin. The superscript rises first, as far as
    // SuperscriptBottomMaxWithSubscript allows, and then the subscript drops (TeXbook Appendix G, rule 18e).
    for (uint32_t i = 0; i < count; ++i) {
        if (subs[i] == nullptr || supers[i] == nullptr)
            continue;
        PrLayoutUnit gapMin = m.font.u(c.subSuperscriptGapMin);
        PrLayoutUnit gap = (s.sub - subs[i]->inkAscent) + (s.super - supers[i]->inkDescent);
        if (gap >= gapMin)
            continue;
        PrLayoutUnit delta = m.font.u(c.superscriptBottomMaxWithSubscript) - (s.super - supers[i]->inkDescent);
        if (delta > kLayoutUnitZero) {
            delta = layoutUnitMin(delta, gapMin - gap);
            s.super += delta;
            gap += delta;
        }
        if (gapMin > gap)
            s.sub += gapMin - gap;
    }
    return s;
}

// Lays out a base with pairs of scripts after it and before it (mathml-core/#base-with-prescripts-and-postscripts). An
// msub, msup or msubsup is laid out as one pair. `post` and `pre` list each pair's subscript then superscript, either
// absent as null. A single pair kerns its scripts into the corners of a base glyph.
Extent scripts(Math& m, Item& base, Item* const* post, uint32_t postPairs, Item* const* pre, uint32_t prePairs,
        const LayoutMathStretch* PR_NULLABLE stretch) {
    layOut(m, base, stretch);
    for (uint32_t i = 0; i < postPairs * 2; ++i) {
        if (post[i] != nullptr)
            layOut(m, *post[i], nullptr);
    }
    for (uint32_t i = 0; i < prePairs * 2; ++i) {
        if (pre[i] != nullptr)
            layOut(m, *pre[i], nullptr);
    }
    uint32_t pairs = postPairs + prePairs;
    RawArray<Item*> subs;
    RawArray<Item*> supers;
    for (uint32_t i = 0; i < postPairs; ++i) {
        subs.push(post[i * 2]);
        supers.push(post[i * 2 + 1]);
    }
    for (uint32_t i = 0; i < prePairs; ++i) {
        subs.push(pre[i * 2]);
        supers.push(pre[i * 2 + 1]);
    }
    ScriptShifts shifts = scriptShifts(m, base, subs.data, supers.data, pairs);
    subs.reset();
    supers.reset();
    const PrFontMathConstants& c = *m.font.c;
    PrLayoutUnit after = m.font.u(c.spaceAfterScript);
    bool largeOp = hasOperatorFlag(base, PrLayoutMathOperatorFlag_LargeOp);
    PrLayoutUnit italic = largeOp ? kLayoutUnitZero : base.italic;
    PrLayoutUnit largeOpItalic = largeOp ? base.italic : kLayoutUnitZero;

    PrLayoutUnit x = kLayoutUnitZero;
    for (uint32_t i = 0; i < prePairs; ++i) {
        Item* sub = pre[i * 2];
        Item* super = pre[i * 2 + 1];
        PrLayoutUnit width = layoutUnitMax(sub != nullptr ? sub->width : kLayoutUnitZero,
            super != nullptr ? super->width : kLayoutUnitZero);
        x += after;
        if (sub != nullptr) {
            sub->x = x + width - sub->width;
            sub->shift = -shifts.sub;
        }
        if (super != nullptr) {
            super->x = x + width - super->width;
            super->shift = shifts.super;
        }
        x += width;
    }
    base.x = x;
    base.shift = kLayoutUnitZero;
    x += base.width;
    for (uint32_t i = 0; i < postPairs; ++i) {
        Item* sub = post[i * 2];
        Item* super = post[i * 2 + 1];
        PrLayoutUnit end = x;
        if (sub != nullptr) {
            PrLayoutUnit kern = kLayoutUnitZero;
            if (i == 0)
                kern = cornerKern(base, PrFontMathCorner_BottomRight, *sub, PrFontMathCorner_TopLeft, -shifts.sub,
                    sub->inkAscent - shifts.sub, -base.inkDescent);
            sub->x = x - largeOpItalic + kern;
            sub->shift = -shifts.sub;
            end = layoutUnitMax(end, sub->x + sub->width);
        }
        if (super != nullptr) {
            PrLayoutUnit kern = kLayoutUnitZero;
            if (i == 0)
                kern = cornerKern(base, PrFontMathCorner_TopRight, *super, PrFontMathCorner_BottomLeft, shifts.super,
                    shifts.super - super->inkDescent, base.inkAscent);
            super->x = x + italic + kern;
            super->shift = shifts.super;
            end = layoutUnitMax(end, super->x + super->width);
        }
        x = end + after;
    }
    Extent e = extentOf(m, 0, m.items.count);
    e.width = x;
    e.italic = kLayoutUnitZero;
    e.attachment = kLayoutMathNoAttachment;
    return e;
}

// ---------------------------------------------------------------------------------------------------------------------
// Underscripts and overscripts (mathml-core/#underscripts-and-overscripts-munder-mover-munderover)

// Lays out the base and scripts with the algorithm for stretching operators along the inline axis
// (mathml-core/#algorithm-for-stretching-operators-along-the-inline-axis). An accent over a base whose ink rises past
// FlattenedAccentBaseHeight takes its flattened form (OpenType MATH, flac), as a capital's accent is drawn lower.
void stretchInline(Math& m, Item& base, Item* PR_NULLABLE under, Item* PR_NULLABLE over,
        const LayoutMathStretch* PR_NULLABLE stretch) {
    Item* all[3] = {&base, under, over};
    bool laid[3] = {};
    if (stretch != nullptr) {
        layOut(m, base, stretch);
        laid[0] = true;
    }
    bool accent = (m.self.scriptFlags & PrLayoutMathScriptFlag_Accent) != 0;
    const LayoutMathStretch flat{.flattened = true};
    // The constraint of the part at `i`, laid out without stretching.
    auto plain = [&](uint32_t i) -> const LayoutMathStretch* {
        bool flatten = i == 2 && accent && laid[0] && base.inkAscent > m.font.u(m.font.c->flattenedAccentBaseHeight);
        return flatten ? &flat : nullptr;
    };
    bool toStretch = false;
    bool others = laid[0];
    LayoutMathStretch target{.size = laid[0] ? base.width : kLayoutUnitZero, .axis = LayoutMathStretchAxis_Inline};
    for (uint32_t i = 0; i < 3; ++i) {
        if (all[i] == nullptr || laid[i])
            continue;
        if (stretchesInline(*all[i])) {
            toStretch = true;
            continue;
        }
        layOut(m, *all[i], plain(i));
        laid[i] = true;
        target.size = others ? layoutUnitMax(target.size, all[i]->width) : all[i]->width;
        others = true;
    }
    if (!toStretch)
        return;
    if (!others)
        target.size = kLayoutUnitZero;
    for (uint32_t i = 0; i < 3; ++i) {
        if (all[i] == nullptr || laid[i])
            continue;
        LayoutMathStretch constraint = target;
        constraint.flattened = plain(i) != nullptr;
        layOut(m, *all[i], &constraint);
    }
}

Extent underOver(Math& m, Item& base, Item* PR_NULLABLE under, Item* PR_NULLABLE over,
        const LayoutMathStretch* PR_NULLABLE stretch) {
    stretchInline(m, base, under, over, stretch);
    const PrFontMathConstants& c = *m.font.c;
    bool largeOp = hasOperatorFlag(base, PrLayoutMathOperatorFlag_LargeOp);
    bool stretchy = stretchesInline(base);
    PrLayoutUnit largeOpItalic = largeOp ? base.italic : kLayoutUnitZero;
    bool accent = (m.self.scriptFlags & PrLayoutMathScriptFlag_Accent) != 0;
    bool accentUnder = (m.self.scriptFlags & PrLayoutMathScriptFlag_AccentUnder) != 0;

    // Inline offsets about the center of the base, then moved so that the leftmost starts at zero. An accent aligns its
    // attachment with the base's (OpenType MATH, MathTopAccentAttachment), and a limit of a large operator moves by
    // half its italic correction (TeXbook Appendix G, rule 13a).
    PrLayoutUnit baseAnchor = half(base.width);
    if (accent && !largeOp && !stretchy && base.attachment != kLayoutMathNoAttachment)
        baseAnchor = base.attachment;
    base.x = -baseAnchor;
    PrLayoutUnit lo = base.x;
    PrLayoutUnit hi = base.x + base.width;
    auto alignScript = [&](Item& script, PrLayoutUnit anchor, PrLayoutUnit offset) {
        script.x = -anchor + offset;
        lo = layoutUnitMin(lo, script.x);
        hi = layoutUnitMax(hi, script.x + script.width);
    };
    if (over != nullptr) {
        PrLayoutUnit anchor = half(over->width);
        if (accent && over->attachment != kLayoutMathNoAttachment)
            anchor = over->attachment;
        alignScript(*over, anchor, half(largeOpItalic));
    }
    if (under != nullptr)
        alignScript(*under, half(under->width), -half(largeOpItalic));
    // MathML 4's align puts the scripts at the left or right edge of the widest of the three instead.
    if (m.self.scriptAlign != PrLayoutMathAlign_Center) {
        PrLayoutUnit widest = layoutUnitMax(base.width, layoutUnitMax(under != nullptr ? under->width : kLayoutUnitZero,
            over != nullptr ? over->width : kLayoutUnitZero));
        Item* parts[3] = {&base, under, over};
        for (Item* part : parts) {
            if (part != nullptr)
                part->x = alignedX(m.self.scriptAlign, part->width, widest);
        }
        lo = kLayoutUnitZero;
        hi = widest;
    }
    base.x -= lo;
    if (under != nullptr)
        under->x -= lo;
    if (over != nullptr)
        over->x -= lo;
    base.shift = kLayoutUnitZero;

    if (under != nullptr) {
        PrLayoutUnit shift = kLayoutUnitZero;
        PrLayoutUnit extra = kLayoutUnitZero;
        if (largeOp) {
            shift = layoutUnitMax(m.font.u(c.lowerLimitBaselineDropMin) - under->inkAscent,
                m.font.u(c.lowerLimitGapMin));
        } else if (stretchy) {
            // The gap below a stretched base is StretchStackGapBelowMin, and the gap above it StretchStackGapAboveMin
            // (OpenType MATH), which MathML Core swaps.
            shift = layoutUnitMax(m.font.u(c.stretchStackBottomShiftDown) - under->inkAscent,
                m.font.u(c.stretchStackGapBelowMin));
        } else if (isStackPart(m.state.tree, *under)) {
            // A brace, bracket or arrow stretched under the base is the bottom of a stretch stack, whose gap to the
            // base's ink is StretchStackGapAboveMin (OpenType MATH), as TeX keeps \underbrace clear of its base.
            shift = m.font.u(c.stretchStackGapAboveMin);
        } else {
            shift = accentUnder ? kLayoutUnitZero : m.font.u(c.underbarVerticalGap);
            extra = m.font.u(c.underbarExtraDescender);
        }
        // The underscript's ink top is `shift` below the base's ink bottom.
        under->shift = -(base.inkDescent + shift + under->inkAscent);
        under->descent += extra;
    }
    if (over != nullptr) {
        PrLayoutUnit extra = kLayoutUnitZero;
        if (largeOp) {
            PrLayoutUnit gap = layoutUnitMax(m.font.u(c.upperLimitGapMin) + over->inkDescent,
                m.font.u(c.upperLimitBaselineRiseMin));
            over->shift = base.inkAscent + gap;
        } else if (stretchy) {
            PrLayoutUnit gap = layoutUnitMax(m.font.u(c.stretchStackGapAboveMin) + over->inkDescent,
                m.font.u(c.stretchStackTopShiftUp));
            over->shift = base.inkAscent + gap;
        } else if (isStackPart(m.state.tree, *over)) {
            // A brace, bracket or arrow stretched over the base is the top of a stretch stack, whose gap to the base's
            // ink is StretchStackGapBelowMin (OpenType MATH), as TeX keeps \overbrace clear of its base.
            over->shift = base.inkAscent + m.font.u(c.stretchStackGapBelowMin) + over->inkDescent;
        } else if (accent) {
            // An accent sits on the base's baseline, raised by as much as the base's ink rises past AccentBaseHeight
            // (TeXbook Appendix G, rule 12; OpenType MATH, AccentBaseHeight), and never lower than the base's ink,
            // which an accent glyph is drawn above but an arbitrary overscript need not be.
            over->shift = layoutUnitMax(layoutUnitMax(kLayoutUnitZero, base.inkAscent - m.font.u(c.accentBaseHeight)),
                base.inkAscent + over->inkDescent);
            extra = m.font.u(c.overbarExtraAscender);
        } else {
            over->shift = base.inkAscent + m.font.u(c.overbarVerticalGap) + over->inkDescent;
            extra = m.font.u(c.overbarExtraAscender);
        }
        over->ascent += extra;
    }
    Extent e = extentOf(m, 0, m.items.count);
    e.width = hi - lo;
    e.attachment = base.attachment != kLayoutMathNoAttachment ? base.attachment + base.x : kLayoutMathNoAttachment;
    return e;
}

// ---------------------------------------------------------------------------------------------------------------------
// Padded, space and enclose

PrLayoutUnit paddedValue(const PrLayoutMathPadding& p, PrLayoutUnit inner, const Extent& e) {
    PrLayoutUnit pseudo = kLayoutUnitZero;
    switch (p.pseudo) {
        case PrLayoutMathPseudo_Width:
            pseudo = e.width;
            break;
        case PrLayoutMathPseudo_Height:
            pseudo = e.ascent;
            break;
        case PrLayoutMathPseudo_Depth:
            pseudo = e.descent;
            break;
        default:
            break;
    }
    PrLayoutUnit value = layoutUnitScaled(pseudo, p.factor) + p.length;
    switch (p.mode) {
        case PrLayoutMathPaddingMode_Set:
            return value;
        case PrLayoutMathPaddingMode_Increase:
            return inner + value;
        case PrLayoutMathPaddingMode_Decrease:
            return inner - value;
        default:
            return inner;
    }
}

// Lays out an mpadded (mathml-core/#layout-of-mpadded, mathml4/#presm_mpadded).
Extent padded(Math& m, const LayoutMathStretch* PR_NULLABLE stretch) {
    Extent inner = row(m, 0, m.items.count, stretch);
    const PrLayoutMathPadding* p = m.self.padding;
    PrLayoutUnit width = paddedValue(p[PrLayoutMathPadded_Width], inner.width, inner);
    PrLayoutUnit height = layoutUnitMax(kLayoutUnitZero, paddedValue(p[PrLayoutMathPadded_Height], inner.ascent, inner));
    PrLayoutUnit depth = layoutUnitMax(kLayoutUnitZero, paddedValue(p[PrLayoutMathPadded_Depth], inner.descent, inner));
    // MathML Core clamps a negative lspace to zero. MathML 4's pseudo-units and increments may move the content back,
    // as TeX's \llap and \clap do.
    const PrLayoutMathPadding& left = p[PrLayoutMathPadded_LeftSpace];
    PrLayoutUnit lspace = paddedValue(left, kLayoutUnitZero, inner);
    if (left.pseudo == PrLayoutMathPseudo_None && left.mode == PrLayoutMathPaddingMode_Set)
        lspace = layoutUnitMax(kLayoutUnitZero, lspace);
    PrLayoutUnit voffset = paddedValue(p[PrLayoutMathPadded_VerticalOffset], kLayoutUnitZero, inner);
    moveItems(m, 0, m.items.count, lspace, voffset);
    Extent e{.attachment = kLayoutMathNoAttachment};
    e.width = width;
    e.ascent = height;
    e.descent = depth;
    e.inkAscent = height;
    e.inkDescent = depth;
    return e;
}

// Lays out an menclose (mathml4/#presm_menclose). Its content is a row, padded by a gap of three rule thicknesses
// inside each notation that frames it, and the notations are strokes of the font's overbar rule thickness.
Extent enclose(Math& m, const LayoutMathStretch* PR_NULLABLE stretch) {
    Extent inner = row(m, 0, m.items.count, stretch);
    const PrFontMathConstants& c = *m.font.c;
    PrLayoutMathNotationFlags n = m.self.notations;
    PrLayoutUnit rule = layoutUnitMax(m.font.u(c.overbarRuleThickness), layoutUnitPx(1));
    PrLayoutUnit gap = rule * 3;
    bool left = (n & (PrLayoutMathNotationFlag_Box | PrLayoutMathNotationFlag_RoundedBox |
        PrLayoutMathNotationFlag_Left | PrLayoutMathNotationFlag_PhasorAngle)) != 0;
    bool right = (n & (PrLayoutMathNotationFlag_Box | PrLayoutMathNotationFlag_RoundedBox |
        PrLayoutMathNotationFlag_Right | PrLayoutMathNotationFlag_Actuarial | PrLayoutMathNotationFlag_Madruwb)) != 0;
    bool top = (n & (PrLayoutMathNotationFlag_Box | PrLayoutMathNotationFlag_RoundedBox |
        PrLayoutMathNotationFlag_Top | PrLayoutMathNotationFlag_Actuarial | PrLayoutMathNotationFlag_LongDivision |
        PrLayoutMathNotationFlag_Radical)) != 0;
    bool bottom = (n & (PrLayoutMathNotationFlag_Box | PrLayoutMathNotationFlag_RoundedBox |
        PrLayoutMathNotationFlag_Bottom | PrLayoutMathNotationFlag_Madruwb | PrLayoutMathNotationFlag_PhasorAngle)) != 0;
    bool circle = (n & PrLayoutMathNotationFlag_Circle) != 0;
    bool arrow = (n & PrLayoutMathNotationFlag_UpDiagonalArrow) != 0;

    PrLayoutUnit padLeft = left ? gap + rule : kLayoutUnitZero;
    PrLayoutUnit padRight = right ? gap + rule : kLayoutUnitZero;
    PrLayoutUnit padTop = top ? gap + rule : kLayoutUnitZero;
    PrLayoutUnit padBottom = bottom ? gap + rule : kLayoutUnitZero;
    if (circle || arrow) {
        // An ellipse through the corners of the padded content is wider by a factor of √2.
        PrLayoutUnit w = layoutUnitScaled(inner.width + gap * 2, 0.2071f) + gap + rule;
        PrLayoutUnit h = layoutUnitScaled(inner.inkAscent + inner.inkDescent + gap * 2, 0.2071f) + gap + rule;
        padLeft = layoutUnitMax(padLeft, w);
        padRight = layoutUnitMax(padRight, w);
        padTop = layoutUnitMax(padTop, h);
        padBottom = layoutUnitMax(padBottom, h);
    }
    // A long division draws a parenthesis before its content, and a radical its sign, which take room there.
    PrLayoutUnit signWidth = kLayoutUnitZero;
    Stretched sign{};
    bool signed_ = false;
    PrLayoutUnit contentTop = inner.inkAscent + padTop;
    PrLayoutUnit contentBottom = inner.inkDescent + padBottom;
    uint32_t codePoint = 0;
    if ((n & PrLayoutMathNotationFlag_Radical) != 0)
        codePoint = 0x221A;
    else if ((n & PrLayoutMathNotationFlag_LongDivision) != 0)
        codePoint = 0x0029;
    DrawnGlyph glyph{};
    if (codePoint != 0 && m.font.font != nullptr) {
        glyph = drawnGlyph(m.font.font, codePoint, isRtl(m));
        signed_ = glyph.glyph != 0 && stretchGlyph(m.font.font, m.font.size, glyph.codePoint, glyph.glyph,
            PrFontMathAxis_Vertical, prLayoutUnitToPx(contentTop + contentBottom), &sign);
        if (signed_)
            signWidth = m.font.u(sign.info.advance);
    }
    padLeft = layoutUnitMax(padLeft, signWidth);
    moveItems(m, 0, m.items.count, padLeft, kLayoutUnitZero);
    Extent e{.attachment = kLayoutMathNoAttachment};
    e.width = padLeft + inner.width + padRight;
    e.inkAscent = contentTop;
    e.inkDescent = contentBottom;
    e.ascent = layoutUnitMax(inner.ascent, contentTop);
    e.descent = layoutUnitMax(inner.descent, contentBottom);

    // Strokes are centered on lines `rule` / 2 inside the frame's edges.
    PrLayoutUnit width = e.width;
    float t = prLayoutUnitToPx(rule);
    PrLayoutUnit halfRule = half(rule);
    PrLayoutUnit frameLeft = signWidth != kLayoutUnitZero ? signWidth : kLayoutUnitZero;
    if (left)
        pushRule(m, frameLeft, -contentTop, rule, contentTop + contentBottom);
    if (right)
        pushRule(m, width - rule, -contentTop, rule, contentTop + contentBottom);
    if (top)
        pushRule(m, frameLeft, -contentTop, width - frameLeft, rule);
    if (bottom)
        pushRule(m, kLayoutUnitZero, contentBottom - rule, width, rule);
    if ((n & PrLayoutMathNotationFlag_RoundedBox) != 0) {
        // The rounded box replaces the square sides it shares.
        m.state.scratch->mathPaints.count -= 4;
        pushRule(m, halfRule, -contentTop + halfRule, width - rule, contentTop + contentBottom - rule,
            LayoutMathPaintKind_RoundBox, t);
    }
    if (circle)
        pushRule(m, halfRule, -contentTop + halfRule, width - rule, contentTop + contentBottom - rule,
            LayoutMathPaintKind_Ellipse, t);
    PrLayoutUnit innerLeft = padLeft;
    PrLayoutUnit innerRight = padLeft + inner.width;
    PrLayoutUnit innerTop = -inner.inkAscent;
    PrLayoutUnit innerHeight = inner.inkAscent + inner.inkDescent;
    if ((n & PrLayoutMathNotationFlag_HorizontalStrike) != 0)
        pushRule(m, innerLeft, -m.font.u(c.axisHeight) - halfRule, inner.width, rule);
    if ((n & PrLayoutMathNotationFlag_VerticalStrike) != 0)
        pushRule(m, innerLeft + half(inner.width) - halfRule, innerTop, rule, innerHeight);
    PrLayoutUnit innerBottom = innerTop + innerHeight;
    if ((n & PrLayoutMathNotationFlag_DownDiagonalStrike) != 0)
        pushLine(m, innerLeft, innerTop, innerRight, innerBottom, t);
    if ((n & PrLayoutMathNotationFlag_UpDiagonalStrike) != 0)
        pushLine(m, innerLeft, innerBottom, innerRight, innerTop, t);
    if (arrow)
        pushLine(m, kLayoutUnitZero, contentBottom, width, -contentTop, t, LayoutMathPaintKind_Arrow);
    // A phasor angle is the bottom side and a line from its left end up to the top right.
    if ((n & PrLayoutMathNotationFlag_PhasorAngle) != 0)
        pushLine(m, frameLeft, contentBottom - halfRule, width, -contentTop, t);
    if (signed_) {
        PrLayoutUnit signAscent = m.font.u(sign.info.ascent);
        float originY = prLayoutUnitToPx(signAscent - contentTop);
        pushStretched(m.state, sign, m.font.size, PrVec2{0, originY},
            PrRect{0, -prLayoutUnitToPx(contentTop), prLayoutUnitToPx(signWidth), prLayoutUnitToPx(contentBottom)},
            glyph.mirrored);
    }
    return e;
}

// ---------------------------------------------------------------------------------------------------------------------
// Fences (mathml4/#presm_mfenced)

// The ink line-ascent and line-descent that an operator stretched along the block axis covers.
struct BlockReach {
    PrLayoutUnit ascent;
    PrLayoutUnit descent;
};

// Returns what an operator with `flags` covers for the block-axis constraint `stretch`
// (mathml-core/#algorithm-for-stretching-operators-along-the-block-axis). A symmetric operator covers as much above the
// math axis `axis` as below it. A fence covers at least 90.1% of the constraint and falls short of it by at most half
// an em of `em` px, as TeX sizes a delimiter (TeXbook Appendix G, rule 19).
BlockReach blockReach(const LayoutMathStretch& stretch, PrLayoutUnit axis, PrLayoutMathOperatorFlags flags, float em) {
    BlockReach r{stretch.size, stretch.depth};
    if ((flags & PrLayoutMathOperatorFlag_Symmetric) != 0) {
        PrLayoutUnit reach = layoutUnitMax(r.ascent - axis, r.descent + axis);
        r.ascent = reach + axis;
        r.descent = reach - axis;
    }
    PrLayoutUnit target = r.ascent + r.descent;
    if ((flags & PrLayoutMathOperatorFlag_Fence) != 0 && target > kLayoutUnitZero) {
        float size = prLayoutUnitToPx(target);
        float least = prF32Max(size * kDelimiterFactor, size - kDelimiterShortfall * em);
        PrLayoutUnit cut = target - prLayoutUnitFromPx(least);
        r.ascent -= half(cut);
        r.descent -= cut - half(cut);
    }
    return r;
}

// Lays out a fence or a separator as the mo of the expanded form would be laid out. It is one glyph of the box's font,
// measured by its ink. A stretchy one stretches along the block axis to the constraint, and never below its own size.
void layOutFence(Math& m, Item& it, const LayoutMathStretch* PR_NULLABLE stretch) {
    const PrFont* font = m.font.font;
    float em = m.font.size;
    it.width = it.ascent = it.descent = it.inkAscent = it.inkDescent = it.italic = kLayoutUnitZero;
    it.attachment = kLayoutMathNoAttachment;
    it.glyph = 0;
    it.glyphFont = nullptr;
    it.extended = false;
    it.fenceSize = 0;
    it.fenceDelta = kLayoutUnitZero;
    DrawnGlyph drawn = font != nullptr ? drawnGlyph(font, it.fence, isRtl(m)) : DrawnGlyph{};
    it.fenceGlyph = drawn.glyph;
    if (it.fenceGlyph == 0)
        return;
    PrRect ink = prFontGlyphInkBounds(font, it.fenceGlyph);
    PrLayoutUnit inkAscent = prLayoutUnitFromPxCeil(-ink.top * em);
    PrLayoutUnit inkDescent = prLayoutUnitFromPxCeil(ink.bottom * em);
    it.width = prLayoutUnitFromPx(prFontGlyphAdvance(font, it.fenceGlyph, 0) * em);
    it.italic = prLayoutUnitFromPx(prFontMathGlyphItalicsCorrection(font, it.fenceGlyph) * em);
    it.glyph = it.fenceGlyph;
    it.glyphFont = font;
    it.glyphSize = em;
    it.extended = prFontMathGlyphIsExtendedShape(font, it.fenceGlyph);
    bool stretchy = (it.math->operatorFlags & PrLayoutMathOperatorFlag_Stretchy) != 0;
    if (stretchy && stretch != nullptr && stretch->axis == LayoutMathStretchAxis_Block) {
        PrLayoutUnit axis = m.font.u(m.font.c->axisHeight);
        BlockReach reach = blockReach(*stretch, axis, it.math->operatorFlags, em);
        PrLayoutUnit target = layoutUnitMax(reach.ascent + reach.descent, inkAscent + inkDescent);
        Stretched glyphs{};
        if (stretchGlyph(font, em, drawn.codePoint, it.fenceGlyph, PrFontMathAxis_Vertical, prLayoutUnitToPx(target),
                &glyphs)) {
            PrLayoutUnit glyphAscent = prLayoutUnitFromPx(glyphs.info.ascent * em);
            PrLayoutUnit glyphDescent = prLayoutUnitFromPx(glyphs.info.descent * em);
            it.fenceDelta = half((glyphAscent - glyphDescent) - (reach.ascent - reach.descent));
            it.fenceSize = prLayoutUnitToPx(target);
            inkAscent = glyphAscent - it.fenceDelta;
            inkDescent = glyphDescent + it.fenceDelta;
            it.width = prLayoutUnitFromPx(glyphs.info.advance * em);
            it.italic = prLayoutUnitFromPx(glyphs.info.italicsCorrection * em);
            it.glyph = glyphs.count == 1 ? glyphs.first : 0;
            it.extended = glyphs.count != 1 || prFontMathGlyphIsExtendedShape(font, it.glyph);
        }
    }
    it.ascent = it.inkAscent = inkAscent;
    it.descent = it.inkDescent = inkDescent;
}

// Pushes the glyphs of a fence or a separator where it is placed, about the box's baseline.
void drawFence(Math& m, const Item& it) {
    if (it.fenceGlyph == 0)
        return;
    DrawnGlyph drawn = drawnGlyph(m.font.font, it.fence, isRtl(m));
    Stretched glyphs{};
    if (it.fenceSize <= 0 || !stretchGlyph(m.font.font, m.font.size, drawn.codePoint, it.fenceGlyph,
            PrFontMathAxis_Vertical, it.fenceSize, &glyphs))
        glyphs = unstretched(m.font.font, it.fenceGlyph);
    float x = prLayoutUnitToPx(it.x);
    pushStretched(m.state, glyphs, m.font.size, PrVec2{x, prLayoutUnitToPx(it.fenceDelta - it.shift)}, PrRect{
        x, prLayoutUnitToPx(-it.shift - it.inkAscent), x + prLayoutUnitToPx(it.width),
        prLayoutUnitToPx(it.inkDescent - it.shift),
    }, drawn.mirrored);
}

// Lays out an mfenced as its expanded form (mathml4/#presm_mfenced), which is its children as a row between its opening
// and closing fences, with a separator after each child but the last. Its fences and separators stretch over the
// children as a row's stretchy operators do.
Extent fenced(Math& m, const LayoutMathStretch* PR_NULLABLE stretch) {
    const PrLayoutMathStyle& self = m.self;
    uint32_t arguments = m.items.count;
    RawArray<Item> items;
    m.fenceStyles.reserve(arguments + 2);
    auto addFence = [&](const PrLayoutMathFence& fence) {
        if (fence.codePoint == 0)
            return;
        PrLayoutMathStyle style = prLayoutMathStyleDefault();
        style.kind = PrLayoutMathKind_Operator;
        style.embellished = true;
        style.operatorFlags = fence.flags;
        style.leftSpace = fence.leftSpace;
        style.rightSpace = fence.rightSpace;
        PR_ASSERT(m.fenceStyles.count < m.fenceStyles.capacity);
        m.fenceStyles.push(style);
        items.push(Item{
            .node = kPrLayoutNodeNone,
            .mathBox = true,
            .box = &layoutAnonymousBoxStyle(),
            .math = &m.fenceStyles.data[m.fenceStyles.count - 1],
            .fence = fence.codePoint,
        });
    };
    addFence(self.fences[PrLayoutMathFenceSlot_Open]);
    for (uint32_t i = 0; i < arguments; ++i) {
        items.push(m.items.data[i]);
        if (i + 1 < arguments && self.separatorCount != 0) {
            uint32_t separator = i < self.separatorCount ? i : self.separatorCount - 1u;
            addFence(self.fences[PrLayoutMathFenceSlot_Separators + separator]);
        }
    }
    addFence(self.fences[PrLayoutMathFenceSlot_Close]);
    m.items.reset();
    m.items = items;
    return row(m, 0, m.items.count, stretch);
}

// ---------------------------------------------------------------------------------------------------------------------
// Elementary math (mathml4/#presm_elementary)

// The digit of a stack's row that an item takes whole, rather than a character of a number.
constexpr uint32_t kStackWhole = 0xFFFFFFFFu;

// The column of no digit, for a line that spans every column.
constexpr int32_t kStackNoColumn = INT32_MIN;

// A digit of a stack's row, which is a character of a number or an item that takes a column whole. Its column is
// counted from the alignment column toward the right.
struct StackDigit {
    uint32_t item;
    uint32_t glyph;                 // the character among the number's glyphs, or kStackWhole
    PrLayoutUnit width;
    PrLayoutUnit offset;            // the character's origin from the item's inline-start edge, as its line sets it
    PrLayoutUnit x;                 // where the character goes, from the stack's inline-start edge
    int32_t column;
    bool decimal;                   // true if the character is the stack's decimal point
};

// A row of a stack, with its items and digits, its part, and its position down from the stack's top. A Line row spans
// the columns from `firstColumn` to `lastColumn`, or every column if they are kStackNoColumn.
struct StackRow {
    uint32_t firstItem;
    uint32_t endItem;
    uint32_t firstDigit;
    uint32_t endDigit;
    PrLayoutMathStackPart part;     // Number for a row of digits, Carry or Line
    bool bar;                       // true for a line that the long division draws itself
    int32_t firstColumn;
    int32_t lastColumn;
    PrLayoutUnit baseline;
    PrLayoutUnit above;             // the room it takes above its baseline, with the carries over it
    PrLayoutUnit below;
    PrLayoutUnit inkAscent;
    PrLayoutUnit inkDescent;
    PrLayoutUnit top;               // a Line's top
    PrLayoutUnit thickness;         // a Line's thickness
};

// What a stack draws beside its items, positioned down from its top. It is a rule over the rectangle from (x0, y0) to
// (x1, y1), a stroke from (x0, y0) to (x1, y1), or the glyph of `codePoint` over that rectangle. The glyph's origin is
// `origin` below its top, and the glyph is stretched to `size` if that is not zero.
struct StackPaint {
    LayoutMathPaintKind kind;
    PrLayoutUnit x0;
    PrLayoutUnit y0;
    PrLayoutUnit x1;
    PrLayoutUnit y1;
    float thickness;
    uint32_t codePoint;
    float size;
    PrLayoutUnit origin;
};

// The layout of a stack, with its digits, rows and columns, the place of each item down from the stack's top, and what
// it draws.
struct Stack {
    RawArray<StackDigit> digits;
    RawArray<StackRow> rows;
    RawArray<PrLayoutUnit> columnX;         // by column from firstColumn
    RawArray<PrLayoutUnit> columnWidth;     // by column from firstColumn
    RawArray<PrLayoutUnit> itemY;           // by item, the baseline
    RawArray<StackPaint> paints;
    int32_t firstColumn;
    PrLayoutUnit width;
    PrLayoutUnit height;

    void reset() {
        digits.reset();
        rows.reset();
        columnX.reset();
        columnWidth.reset();
        itemY.reset();
        paints.reset();
    }
};

// Returns the part of the stack that the item is. An item that its producer did not mark, such as a run of text, is a
// digit of a row of its own.
PrLayoutMathStackPart partOf(const Item& it) {
    PrLayoutMathStackPart part = it.math->stackPart;
    return part == PrLayoutMathStackPart_None ? PrLayoutMathStackPart_Digit : part;
}

// Appends the glyphs of the fragment `f` at inline offset `x` px and of its descendants. Each glyph gives the inline
// offset of its origin and its advance in px, and the text gives its code points that are not white space.
void collectGlyphs(const LayoutTree* tree, const LayoutFragment* f, float x, RawArray<PrVec2>* glyphs,
        RawArray<uint32_t>* codePoints) {
    if (f->kind == LayoutFragmentKind_Text) {
        const LayoutFragmentText& text = f->text;
        const PrFont* const* fonts = layoutFragmentTextFonts(f);
        const LayoutFragmentFontRange* ranges = layoutFragmentTextRanges(f);
        uint32_t range = 0;
        for (uint32_t g = 0; g < text.glyphCount; ++g) {
            while (range + 1 < text.rangeCount && g >= ranges[range].glyphEnd)
                ++range;
            float advance = prFontGlyphAdvance(fonts[ranges[range].font], text.glyphs[g].id, text.size) * text.size;
            glyphs->push(PrVec2{x + text.glyphs[g].position.x, advance});
        }
        PrStringView value = layoutNodeText(tree, f->node);
        for (size_t at = 0; at < value.size;) {
            uint32_t codePoint = 0;
            uint32_t length = prUtf8Decode(value, at, &codePoint);
            if (length == 0)
                break;
            at += length;
            if (codePoint != ' ' && codePoint != '\t' && codePoint != '\n' && codePoint != '\r')
                codePoints->push(codePoint);
        }
        return;
    }
    for (uint32_t i = 0; i < f->childCount; ++i)
        collectGlyphs(tree, f->children[i].fragment, x + prLayoutUnitToPx(f->children[i].offset.x), glyphs, codePoints);
}

// Appends the digits of the number `item`, one for each of its characters (mathml4/#presm_msrow), or one for the whole
// item if its characters are not one glyph each.
void numberDigits(Math& m, uint32_t item, Stack& s) {
    Item& it = m.items.data[item];
    RawArray<PrVec2> glyphs;
    RawArray<uint32_t> codePoints;
    collectGlyphs(m.state.tree, it.fragment, 0, &glyphs, &codePoints);
    if (glyphs.count < 2) {
        s.digits.push(StackDigit{
            .item = item,
            .glyph = kStackWhole,
            .width = it.width,
            .decimal = codePoints.count == 1 && codePoints.data[0] == m.self.decimalPoint,
        });
    } else {
        for (uint32_t g = 0; g < glyphs.count; ++g) {
            s.digits.push(StackDigit{
                .item = item,
                .glyph = g,
                .width = prLayoutUnitFromPx(glyphs.data[g].y),
                .offset = prLayoutUnitFromPx(glyphs.data[g].x),
                .decimal = codePoints.count == glyphs.count && codePoints.data[g] == m.self.decimalPoint,
            });
        }
    }
    glyphs.reset();
    codePoints.reset();
}

// Returns true if the item's characters take a column each, as in an mn or a token that is the result of a long
// division.
bool isNumber(const Item& it) {
    PrLayoutMathStackPart part = partOf(it);
    return part == PrLayoutMathStackPart_Number ||
        (part == PrLayoutMathStackPart_Result && it.math->kind == PrLayoutMathKind_Token);
}

// Appends the rows of the items from `begin` to `end`, laid out, and their digits. An item starts a row if its producer
// marked it so, or if it is of another kind of row than the row before it.
void buildRows(Math& m, uint32_t begin, uint32_t end, Stack& s) {
    for (uint32_t i = begin; i < end; ++i) {
        Item& it = m.items.data[i];
        PrLayoutMathStackPart part = partOf(it);
        PrLayoutMathStackPart kind = PrLayoutMathStackPart_Number;
        if (part == PrLayoutMathStackPart_Carry || part == PrLayoutMathStackPart_Line)
            kind = part;
        bool start = s.rows.count == 0 || (it.math->stackFlags & PrLayoutMathStackFlag_RowStart) != 0 ||
            s.rows.data[s.rows.count - 1].part != kind || kind == PrLayoutMathStackPart_Line;
        if (start) {
            s.rows.push(StackRow{
                .firstItem = i,
                .firstDigit = s.digits.count,
                .part = kind,
                .firstColumn = kStackNoColumn,
                .lastColumn = kStackNoColumn,
            });
        }
        StackRow& row = s.rows.data[s.rows.count - 1];
        row.endItem = i + 1;
        if (isNumber(it))
            numberDigits(m, i, s);
        else if (kind != PrLayoutMathStackPart_Line)
            s.digits.push(StackDigit{
                .item = i,
                .glyph = kStackWhole,
                .width = kind == PrLayoutMathStackPart_Carry ? kLayoutUnitZero : it.width,
            });
        row.endDigit = s.digits.count;
    }
}

// Returns the digit of a row that its stack's alignment puts in column 0 (mathml4/#presm_mstack), counted from the
// row's first digit. It is the last digit for right, the first for left, and the middle digit or the one right of it
// for center. For decimalpoint, it is the row's first decimal point, or else the place after the row's first number.
int32_t anchorOf(Math& m, Stack& s, const StackRow& row) {
    int32_t count = int32_t(row.endDigit - row.firstDigit);
    switch (m.self.stackAlign) {
        case PrLayoutMathAlign_Left: return 0;
        case PrLayoutMathAlign_Right: return count - 1;
        case PrLayoutMathAlign_Center: return count / 2;
        default: break;
    }
    for (uint32_t d = row.firstDigit; d < row.endDigit; ++d) {
        if (s.digits.data[d].decimal)
            return int32_t(d - row.firstDigit);
    }
    for (uint32_t d = row.firstDigit; d < row.endDigit; ++d) {
        if (!isNumber(m.items.data[s.digits.data[d].item]))
            continue;
        uint32_t number = s.digits.data[d].item;
        while (d < row.endDigit && s.digits.data[d].item == number)
            ++d;
        return int32_t(d - row.firstDigit);
    }
    return count;
}

// Returns the next row of digits after the row at `index`, or the row count if there is none.
uint32_t nextDigitRow(const Stack& s, uint32_t index) {
    uint32_t r = index + 1;
    while (r < s.rows.count && s.rows.data[r].part != PrLayoutMathStackPart_Number)
        ++r;
    return r;
}

// Puts each digit in its column, and each line across its columns. A row's position moves it that many columns toward
// the left. A carry goes over the column of the next row that its place among the carries matches, from the right
// (mathml4/#presm_mscarries), and a line of a given length spans the columns that a number of as many digits would.
void assignColumns(Math& m, Stack& s) {
    for (StackRow& row : s.rows.span()) {
        if (row.part != PrLayoutMathStackPart_Number)
            continue;
        int32_t position = m.items.data[row.firstItem].math->stackPosition;
        int32_t anchor = anchorOf(m, s, row);
        for (uint32_t d = row.firstDigit; d < row.endDigit; ++d)
            s.digits.data[d].column = int32_t(d - row.firstDigit) - anchor - position;
    }
    for (uint32_t r = 0; r < s.rows.count; ++r) {
        StackRow& row = s.rows.data[r];
        const PrLayoutMathStyle& style = *m.items.data[row.firstItem].math;
        if (row.part == PrLayoutMathStackPart_Carry) {
            uint32_t next = nextDigitRow(s, r);
            int32_t last = 0;
            if (next < s.rows.count && s.rows.data[next].endDigit > s.rows.data[next].firstDigit)
                last = s.digits.data[s.rows.data[next].endDigit - 1].column;
            int32_t count = int32_t(row.endDigit - row.firstDigit);
            for (uint32_t d = row.firstDigit; d < row.endDigit; ++d)
                s.digits.data[d].column = last - (count - 1 - int32_t(d - row.firstDigit)) - style.stackPosition;
        } else if (row.part == PrLayoutMathStackPart_Line && !row.bar && style.lineColumns != 0) {
            int32_t count = int32_t(style.lineColumns);
            int32_t anchor = count;
            if (m.self.stackAlign == PrLayoutMathAlign_Left)
                anchor = 0;
            else if (m.self.stackAlign == PrLayoutMathAlign_Right)
                anchor = count - 1;
            else if (m.self.stackAlign == PrLayoutMathAlign_Center)
                anchor = count / 2;
            row.firstColumn = -anchor - style.stackPosition;
            row.lastColumn = row.firstColumn + count - 1;
        }
    }
}

// Sizes the columns and sets where each starts (mathml4/#presm_mstack). A column is as wide as its widest digit, or as
// a 0 if it has none, and `charSpacing` apart from the next. Carries take no width.
void layColumns(Math& m, Stack& s) {
    bool any = false;
    int32_t first = 0;
    int32_t last = 0;
    auto include = [&](int32_t column) {
        first = any ? (column < first ? column : first) : column;
        last = any ? (column > last ? column : last) : column;
        any = true;
    };
    for (const StackDigit& d : s.digits.span())
        include(d.column);
    for (const StackRow& row : s.rows.span()) {
        if (row.firstColumn != kStackNoColumn) {
            include(row.firstColumn);
            include(row.lastColumn);
        }
    }
    s.firstColumn = first;
    uint32_t count = any ? uint32_t(last - first + 1) : 0;
    s.columnX.resize(count);
    s.columnWidth.resize(count);
    for (uint32_t c = 0; c < count; ++c)
        s.columnWidth.data[c] = kLayoutUnitZero;
    for (const StackDigit& d : s.digits.span()) {
        if (partOf(m.items.data[d.item]) == PrLayoutMathStackPart_Carry)
            continue;
        PrLayoutUnit& width = s.columnWidth.data[d.column - first];
        width = layoutUnitMax(width, d.width);
    }
    PrLayoutUnit zero = kLayoutUnitZero;
    if (m.font.font != nullptr)
        zero = prLayoutUnitFromPx(prFontGlyphAdvance(m.font.font, prFontCodePointGlyph(m.font.font, '0'), m.font.size) *
            m.font.size);
    PrLayoutUnit x = kLayoutUnitZero;
    for (uint32_t c = 0; c < count; ++c) {
        if (s.columnWidth.data[c] == kLayoutUnitZero)
            s.columnWidth.data[c] = zero;
        s.columnX.data[c] = x;
        x += s.columnWidth.data[c] + (c + 1 < count ? m.self.charSpacing : kLayoutUnitZero);
    }
    s.width = x;
}

// Places each digit in its column as charalign says (mathml4/#presm_mstack), and lays out each number again with its
// characters at those places. A digit wider than its column overflows the other side. A carry goes beside its column as
// its location says, at its center for a location that is not east or west.
void placeColumns(Math& m, Stack& s) {
    for (StackDigit& d : s.digits.span()) {
        PrLayoutUnit columnX = s.columnX.data[d.column - s.firstColumn];
        PrLayoutUnit columnWidth = s.columnWidth.data[d.column - s.firstColumn];
        Item& it = m.items.data[d.item];
        if (partOf(it) == PrLayoutMathStackPart_Carry) {
            switch (it.math->carryLocation) {
                case PrLayoutMathCarryLocation_Ne:
                case PrLayoutMathCarryLocation_Se: d.x = columnX + columnWidth - half(it.width); break;
                case PrLayoutMathCarryLocation_Nw:
                case PrLayoutMathCarryLocation_Sw: d.x = columnX - half(it.width); break;
                case PrLayoutMathCarryLocation_E: d.x = columnX + columnWidth; break;
                case PrLayoutMathCarryLocation_W: d.x = columnX - it.width; break;
                default: d.x = columnX + half(columnWidth - it.width); break;
            }
        } else {
            d.x = columnX + alignedX(m.self.charAlign, d.width, columnWidth);
        }
        if (d.glyph == kStackWhole)
            it.x = d.x;
    }
    RawArray<PrLayoutUnit> columns;
    for (uint32_t d = 0; d < s.digits.count;) {
        uint32_t item = s.digits.data[d].item;
        uint32_t end = d;
        while (end < s.digits.count && s.digits.data[end].item == item)
            ++end;
        if (s.digits.data[d].glyph != kStackWhole) {
            Item& it = m.items.data[item];
            it.x = s.digits.data[d].x - s.digits.data[d].offset;
            columns.resize(0);
            for (uint32_t k = d; k < end; ++k)
                columns.push(s.digits.data[k].x - it.x);
            LayoutMathStretch constraint{
                .columnCount = columns.count,
                .columnsHash = prHashBytes(columns.data, sizeof(PrLayoutUnit) * columns.count, 0),
                .columns = columns.data,
            };
            layOut(m, it, &constraint);
        }
        d = end;
    }
    columns.reset();
}

// Returns the ink above and below the baseline of the digit of a row at a column, or of the row if it has none there.
void inkAt(Math& m, const Stack& s, const StackRow& row, int32_t column, PrLayoutUnit* ascent,
        PrLayoutUnit* descent, const StackDigit** found) {
    *ascent = row.inkAscent;
    *descent = row.inkDescent;
    *found = nullptr;
    for (uint32_t d = row.firstDigit; d < row.endDigit; ++d) {
        if (s.digits.data[d].column != column)
            continue;
        const Item& it = m.items.data[s.digits.data[d].item];
        *ascent = it.inkAscent;
        *descent = it.inkDescent;
        *found = &s.digits.data[d];
        return;
    }
}

// Sets the rows one below another, down from the stack's top (mathml4/#presm_msline). A row of digits goes a
// baselineskip of 1.2em below the last, or further if their ink and carries would come closer than the font's overbar
// gap. A line goes that gap below the ink above it, with as much room below it. Carries sit over, under or beside the
// character of their column in the next row, half that gap from its ink, and cross it out as they say.
void stackRows(Math& m, Stack& s) {
    const PrFontMathConstants& c = *m.font.c;
    PrLayoutUnit pitch = m.font.u(1.2f);
    PrLayoutUnit gap = layoutUnitMax(m.font.u(c.overbarVerticalGap), layoutUnitPx(1));
    PrLayoutUnit carryGap = half(gap);
    PrLayoutUnit rule = m.font.u(c.fractionRuleThickness);
    s.itemY.resizeZeroed(m.items.count);
    for (StackRow& row : s.rows.span()) {
        for (uint32_t i = row.firstItem; i < row.endItem; ++i) {
            const Item& it = m.items.data[i];
            bool first = i == row.firstItem;
            row.above = first ? it.ascent : layoutUnitMax(row.above, it.ascent);
            row.below = first ? it.descent : layoutUnitMax(row.below, it.descent);
            row.inkAscent = first ? it.inkAscent : layoutUnitMax(row.inkAscent, it.inkAscent);
            row.inkDescent = first ? it.inkDescent : layoutUnitMax(row.inkDescent, it.inkDescent);
        }
        if (row.part == PrLayoutMathStackPart_Line) {
            const PrLayoutMathStyle& style = row.bar ? defaultMath() : *m.items.data[row.firstItem].math;
            row.thickness = rule;
            if (!layoutLengthIsAuto(style.lineThickness))
                row.thickness = layoutUnitMax(kLayoutUnitZero, layoutResolve(m.state.styles, style.lineThickness, rule));
        }
    }
    // The room that carries take over and under the next row of digits.
    for (uint32_t r = 0; r < s.rows.count; ++r) {
        const StackRow& carries = s.rows.data[r];
        uint32_t next = nextDigitRow(s, r);
        if (carries.part != PrLayoutMathStackPart_Carry || next == s.rows.count)
            continue;
        StackRow& row = s.rows.data[next];
        for (uint32_t d = carries.firstDigit; d < carries.endDigit; ++d) {
            const Item& carry = m.items.data[s.digits.data[d].item];
            PrLayoutUnit ascent;
            PrLayoutUnit descent;
            const StackDigit* digit;
            inkAt(m, s, row, s.digits.data[d].column, &ascent, &descent, &digit);
            PrLayoutUnit height = carry.inkAscent + carry.inkDescent;
            switch (carry.math->carryLocation) {
                case PrLayoutMathCarryLocation_N:
                case PrLayoutMathCarryLocation_Ne:
                case PrLayoutMathCarryLocation_Nw:
                    row.above = layoutUnitMax(row.above, ascent + carryGap + height);
                    break;
                case PrLayoutMathCarryLocation_S:
                case PrLayoutMathCarryLocation_Se:
                case PrLayoutMathCarryLocation_Sw:
                    row.below = layoutUnitMax(row.below, descent + carryGap + height);
                    break;
                default:
                    break;
            }
        }
    }
    PrLayoutUnit bottom = kLayoutUnitZero;         // the lowest ink so far
    PrLayoutUnit lastBaseline = kLayoutUnitZero;
    bool anyRow = false;
    bool afterLine = false;
    for (StackRow& row : s.rows.span()) {
        if (row.part == PrLayoutMathStackPart_Carry)
            continue;
        if (row.part == PrLayoutMathStackPart_Line) {
            row.top = anyRow || afterLine ? bottom + gap : kLayoutUnitZero;
            bottom = row.top + row.thickness;
            afterLine = true;
            continue;
        }
        if (!anyRow && !afterLine)
            row.baseline = row.above;
        else if (afterLine)
            row.baseline = bottom + gap + row.above;
        else
            row.baseline = layoutUnitMax(lastBaseline + pitch, bottom + gap + row.above);
        bottom = row.baseline + row.below;
        lastBaseline = row.baseline;
        anyRow = true;
        afterLine = false;
        for (uint32_t i = row.firstItem; i < row.endItem; ++i)
            s.itemY.data[i] = row.baseline;
    }
    s.height = bottom;
    // Carries and their crossouts, now that the rows they mark are placed.
    for (uint32_t r = 0; r < s.rows.count; ++r) {
        const StackRow& carries = s.rows.data[r];
        uint32_t next = nextDigitRow(s, r);
        if (carries.part != PrLayoutMathStackPart_Carry)
            continue;
        const StackRow* row = next < s.rows.count ? &s.rows.data[next] : nullptr;
        for (uint32_t d = carries.firstDigit; d < carries.endDigit; ++d) {
            const Item& carry = m.items.data[s.digits.data[d].item];
            PrLayoutUnit ascent = kLayoutUnitZero;
            PrLayoutUnit descent = kLayoutUnitZero;
            const StackDigit* digit = nullptr;
            PrLayoutUnit baseline = kLayoutUnitZero;
            if (row != nullptr) {
                inkAt(m, s, *row, s.digits.data[d].column, &ascent, &descent, &digit);
                baseline = row->baseline;
            }
            PrLayoutUnit y = baseline - ascent - carryGap - carry.inkDescent;
            switch (carry.math->carryLocation) {
                case PrLayoutMathCarryLocation_S:
                case PrLayoutMathCarryLocation_Se:
                case PrLayoutMathCarryLocation_Sw:
                    y = baseline + descent + carryGap + carry.inkAscent;
                    break;
                case PrLayoutMathCarryLocation_E:
                case PrLayoutMathCarryLocation_W:
                    y = baseline - half(ascent - descent) + half(carry.inkAscent - carry.inkDescent);
                    break;
                default:
                    break;
            }
            s.itemY.data[s.digits.data[d].item] = y;
            PrLayoutMathStackFlags strikes = carry.math->stackFlags;
            if (digit == nullptr || (strikes & ~PrLayoutMathStackFlag_RowStart) == 0)
                continue;
            PrLayoutUnit left = digit->x;
            PrLayoutUnit right = digit->x + digit->width;
            PrLayoutUnit top = baseline - ascent;
            PrLayoutUnit lower = baseline + descent;
            float thickness = prLayoutUnitToPx(rule);
            auto stroke = [&](PrLayoutUnit x0, PrLayoutUnit y0, PrLayoutUnit x1, PrLayoutUnit y1) {
                s.paints.push(StackPaint{
                    .kind = LayoutMathPaintKind_Line,
                    .x0 = x0,
                    .y0 = y0,
                    .x1 = x1,
                    .y1 = y1,
                    .thickness = thickness,
                });
            };
            if ((strikes & PrLayoutMathStackFlag_UpDiagonalStrike) != 0)
                stroke(left, lower, right, top);
            if ((strikes & PrLayoutMathStackFlag_DownDiagonalStrike) != 0)
                stroke(left, top, right, lower);
            if ((strikes & PrLayoutMathStackFlag_VerticalStrike) != 0)
                stroke(half(left + right), top, half(left + right), lower);
            if ((strikes & PrLayoutMathStackFlag_HorizontalStrike) != 0)
                stroke(left, half(top + lower), right, half(top + lower));
        }
    }
    // Lines across their columns, or across the stack.
    for (const StackRow& row : s.rows.span()) {
        if (row.part != PrLayoutMathStackPart_Line || row.bar)
            continue;
        const PrLayoutMathStyle& style = *m.items.data[row.firstItem].math;
        PrLayoutUnit left = kLayoutUnitZero;
        PrLayoutUnit right = s.width;
        if (row.firstColumn != kStackNoColumn) {
            left = s.columnX.data[row.firstColumn - s.firstColumn];
            right = s.columnX.data[row.lastColumn - s.firstColumn] + s.columnWidth.data[row.lastColumn - s.firstColumn];
        }
        s.paints.push(StackPaint{
            .kind = LayoutMathPaintKind_Rule,
            .x0 = left - style.overhangs[0],
            .y0 = row.top,
            .x1 = right + style.overhangs[1],
            .y1 = row.top + row.thickness,
        });
        s.itemY.data[row.firstItem] = row.top;
    }
}

// Lays out the stack of the items from `begin` to `end`, with the result of a long division as its first row if
// `result` names it, and a bar that the long division draws below that row.
void layStack(Math& m, uint32_t begin, uint32_t end, uint32_t result, Stack& s) {
    for (uint32_t i = begin; i < end; ++i)
        layOut(m, m.items.data[i], nullptr);
    if (result != kStackWhole) {
        layOut(m, m.items.data[result], nullptr);
        buildRows(m, result, result + 1, s);
        s.rows.push(StackRow{
            .firstItem = result + 1,
            .endItem = result + 1,
            .firstDigit = s.digits.count,
            .endDigit = s.digits.count,
            .part = PrLayoutMathStackPart_Line,
            .bar = true,
            .firstColumn = kStackNoColumn,
            .lastColumn = kStackNoColumn,
        });
    }
    buildRows(m, begin, end, s);
    assignColumns(m, s);
    layColumns(m, s);
    placeColumns(m, s);
    stackRows(m, s);
}

// Returns the row of the stack that the box's alignment names (mathml4/#presm_mstack). The rows of digits are counted
// from 1 at the top, or from -1 at the bottom.
const StackRow* PR_NULLABLE alignedRow(const Stack& s, int32_t index) {
    uint32_t count = 0;
    for (const StackRow& row : s.rows.span())
        count += row.part == PrLayoutMathStackPart_Number ? 1 : 0;
    if (count == 0)
        return nullptr;
    int32_t wanted = index > 0 ? index - 1 : int32_t(count) + index;
    if (index == 0 || wanted < 0 || wanted >= int32_t(count))
        wanted = 0;
    for (const StackRow& row : s.rows.span()) {
        if (row.part == PrLayoutMathStackPart_Number && wanted-- == 0)
            return &row;
    }
    return nullptr;
}

// Returns the baseline, down from its top, of a box of `height` that has the box's alignment. `row` is the baseline of
// the row that it aligns by.
PrLayoutUnit alignedBaseline(const Math& m, PrLayoutUnit height, PrLayoutUnit row) {
    switch (m.self.tableAlign) {
        case PrLayoutMathTableAlign_Top: return kLayoutUnitZero;
        case PrLayoutMathTableAlign_Bottom: return height;
        case PrLayoutMathTableAlign_Center: return half(height);
        case PrLayoutMathTableAlign_Axis: return half(height) + m.font.u(m.font.c->axisHeight);
        default: return row;
    }
}

// Places the items and the paints of a stack laid out down from its top, its baseline `baseline` below its top, and
// returns its extent, `width` wide.
Extent finishStack(Math& m, Stack& s, PrLayoutUnit baseline, PrLayoutUnit width) {
    for (uint32_t i = 0; i < m.items.count; ++i)
        m.items.data[i].shift = baseline - s.itemY.data[i];
    for (const StackPaint& p : s.paints.span()) {
        if (p.kind == LayoutMathPaintKind_Rule) {
            pushRule(m, p.x0, p.y0 - baseline, p.x1 - p.x0, p.y1 - p.y0);
        } else if (p.kind == LayoutMathPaintKind_Line) {
            pushLine(m, p.x0, p.y0 - baseline, p.x1, p.y1 - baseline, p.thickness);
        } else if (m.font.font != nullptr) {
            DrawnGlyph glyph = drawnGlyph(m.font.font, p.codePoint, isRtl(m));
            Stretched glyphs{};
            if (p.size <= 0 || !stretchGlyph(m.font.font, m.font.size, glyph.codePoint, glyph.glyph,
                    PrFontMathAxis_Vertical, p.size, &glyphs))
                glyphs = unstretched(m.font.font, glyph.glyph);
            float x = prLayoutUnitToPx(p.x0);
            pushStretched(m.state, glyphs, m.font.size, PrVec2{x, prLayoutUnitToPx(p.y0 + p.origin - baseline)},
                PrRect{x, prLayoutUnitToPx(p.y0 - baseline), prLayoutUnitToPx(p.x1), prLayoutUnitToPx(p.y1 - baseline)},
                glyph.mirrored);
        }
    }
    Extent e = extentOf(m, 0, m.items.count);
    e.width = width;
    e.ascent = layoutUnitMax(e.ascent, baseline);
    e.descent = layoutUnitMax(e.descent, s.height - baseline);
    e.inkAscent = layoutUnitMax(e.inkAscent, baseline);
    e.inkDescent = layoutUnitMax(e.inkDescent, s.height - baseline);
    e.italic = kLayoutUnitZero;
    e.attachment = kLayoutMathNoAttachment;
    return e;
}

// Lays out an mstack (mathml4/#presm_mstack).
Extent stack(Math& m) {
    Stack s{};
    layStack(m, 0, m.items.count, kStackWhole, s);
    const StackRow* row = alignedRow(s, m.self.tableRow);
    Extent e = finishStack(m, s, alignedBaseline(m, s.height, row != nullptr ? row->baseline : kLayoutUnitZero), s.width);
    s.reset();
    return e;
}

// A glyph that a long division draws, with its character, advance and ink, stretched to cover `size` if that is not
// zero.
struct DivisionSign {
    uint32_t codePoint;
    float size;
    PrLayoutUnit advance;
    PrLayoutUnit ascent;
    PrLayoutUnit descent;
};

DivisionSign divisionSign(Math& m, uint32_t codePoint, PrLayoutUnit size) {
    DivisionSign sign{.codePoint = codePoint, .size = prLayoutUnitToPx(size)};
    if (m.font.font == nullptr)
        return sign;
    DrawnGlyph glyph = drawnGlyph(m.font.font, codePoint, isRtl(m));
    Stretched glyphs{};
    if (sign.size > 0 && glyph.glyph != 0 &&
            stretchGlyph(m.font.font, m.font.size, glyph.codePoint, glyph.glyph, PrFontMathAxis_Vertical, sign.size,
                &glyphs)) {
        sign.advance = m.font.u(glyphs.info.advance);
        sign.ascent = m.font.u(glyphs.info.ascent);
        sign.descent = m.font.u(glyphs.info.descent);
        return sign;
    }
    sign.size = 0;
    PrRect ink = prFontGlyphInkBounds(m.font.font, glyph.glyph);
    sign.advance = m.font.u(prFontGlyphAdvance(m.font.font, glyph.glyph, 0));
    sign.ascent = m.font.u(-ink.top);
    sign.descent = m.font.u(ink.bottom);
    return sign;
}

// Pushes `sign` with its ink's top at `top` and its origin at `x`, down from the stack's top.
void pushSign(Stack& s, const DivisionSign& sign, PrLayoutUnit x, PrLayoutUnit top) {
    s.paints.push(StackPaint{
        .kind = LayoutMathPaintKind_Glyphs,
        .x0 = x,
        .y0 = top,
        .x1 = x + sign.advance,
        .y1 = top + sign.ascent + sign.descent,
        .codePoint = sign.codePoint,
        .size = sign.size,
        .origin = sign.ascent,
    });
}

// Lays out an mlongdiv (mathml4/#presm_mlongdiv). Its divisor and result go around the stack of its other children as
// its longdivstyle places them, with the rules and signs of that style. The divisor sits on the baseline of the
// dividend, the stack's first row, and so does a result beside the dividend.
Extent longDivision(Math& m) {
    Item& divisor = m.items.data[0];
    Item& result = m.items.data[1];
    PrLayoutMathLongDivision style = m.self.longDivision;
    bool resultOnTop = style == PrLayoutMathLongDivision_LeftTop || style == PrLayoutMathLongDivision_RightTop;
    Stack s{};
    layOut(m, divisor, nullptr);
    layStack(m, 2, m.items.count, resultOnTop ? 1 : kStackWhole, s);
    if (!resultOnTop)
        layOut(m, result, nullptr);
    const PrFontMathConstants& c = *m.font.c;
    PrLayoutUnit rule = m.font.u(c.fractionRuleThickness);
    PrLayoutUnit space = m.font.u(5.0f / 18);
    PrLayoutUnit gap = layoutUnitMax(m.font.u(c.overbarVerticalGap), layoutUnitPx(1));
    // The dividend is the first row of digits after the bar or at the top.
    const StackRow* dividend = nullptr;
    const StackRow* bar = nullptr;
    for (const StackRow& row : s.rows.span()) {
        if (row.bar)
            bar = &row;
        if (row.part == PrLayoutMathStackPart_Number && dividend == nullptr && (!resultOnTop || bar != nullptr))
            dividend = &row;
    }
    PrLayoutUnit dividendBaseline = dividend != nullptr ? dividend->baseline : kLayoutUnitZero;
    PrLayoutUnit dividendTop = dividend != nullptr ? dividendBaseline - dividend->inkAscent : kLayoutUnitZero;
    PrLayoutUnit dividendBottom = dividend != nullptr ? dividendBaseline + dividend->inkDescent : kLayoutUnitZero;
    PrLayoutUnit stackShift = kLayoutUnitZero;        // where the stack starts
    PrLayoutUnit width = s.width;
    auto moveStack = [&](PrLayoutUnit dx) {
        for (uint32_t i = 2; i < m.items.count; ++i)
            m.items.data[i].x += dx;
        if (resultOnTop)
            result.x += dx;
        for (StackPaint& p : s.paints.span()) {
            p.x0 += dx;
            p.x1 += dx;
        }
        stackShift += dx;
    };
    auto rulePaint = [&](PrLayoutUnit x0, PrLayoutUnit y0, PrLayoutUnit x1, PrLayoutUnit y1) {
        s.paints.push(StackPaint{.kind = LayoutMathPaintKind_Rule, .x0 = x0, .y0 = y0, .x1 = x1, .y1 = y1});
    };
    auto setY = [&](Item& it, PrLayoutUnit y) { s.itemY.data[uint32_t(&it - m.items.data)] = y; };
    switch (style) {
        case PrLayoutMathLongDivision_LeftTop:
        case PrLayoutMathLongDivision_RightTop: {
            // The bar runs over the dividend from the bracket, which reaches from the bar down past the dividend. The
            // divisor goes before the bracket, or after the dividend behind a rule.
            PrLayoutUnit barTop = bar != nullptr ? bar->top : kLayoutUnitZero;
            PrLayoutUnit bracketBottom = layoutUnitMax(dividendBottom, dividendBaseline) + gap;
            bool left = style == PrLayoutMathLongDivision_LeftTop;
            if (left) {
                DivisionSign bracket = divisionSign(m, ')', bracketBottom - barTop);
                divisor.x = kLayoutUnitZero;
                moveStack(divisor.width + space + bracket.advance);
                pushSign(s, bracket, divisor.width + space, barTop);
                rulePaint(divisor.width + space, barTop, stackShift + s.width, barTop + rule);
                width = stackShift + s.width;
            } else {
                rulePaint(kLayoutUnitZero, barTop, s.width + space, barTop + rule);
                rulePaint(s.width + space, barTop, s.width + space + rule, bracketBottom);
                divisor.x = s.width + space * 2 + rule;
                width = divisor.x + divisor.width;
            }
            setY(divisor, dividendBaseline);
            break;
        }
        case PrLayoutMathLongDivision_StackedRightRight:
        case PrLayoutMathLongDivision_MediumStackedRightRight:
        case PrLayoutMathLongDivision_ShortStackedRightRight:
        case PrLayoutMathLongDivision_StackedLeftLeft:
        case PrLayoutMathLongDivision_StackedLeftLineTop: {
            // The divisor over a rule over the result, beside the dividend behind a vertical rule, which reaches down
            // the whole stack, past the result, or past the divisor only.
            bool right = style == PrLayoutMathLongDivision_StackedRightRight ||
                style == PrLayoutMathLongDivision_MediumStackedRightRight ||
                style == PrLayoutMathLongDivision_ShortStackedRightRight;
            PrLayoutUnit blockWidth = layoutUnitMax(divisor.width, result.width) + space * 2;
            PrLayoutUnit divisorBottom = dividendBaseline + divisor.inkDescent + gap;
            PrLayoutUnit resultBaseline = divisorBottom + rule + gap + result.inkAscent;
            PrLayoutUnit resultBottom = resultBaseline + result.inkDescent + gap;
            PrLayoutUnit top = layoutUnitMin(dividendTop, dividendBaseline - divisor.inkAscent) - gap;
            PrLayoutUnit lineBottom = layoutUnitMax(s.height, resultBottom);
            if (style == PrLayoutMathLongDivision_MediumStackedRightRight)
                lineBottom = resultBottom;
            else if (style == PrLayoutMathLongDivision_ShortStackedRightRight)
                lineBottom = divisorBottom + rule;
            PrLayoutUnit lineX = right ? s.width + space : blockWidth;
            if (!right)
                moveStack(blockWidth + rule + space);
            PrLayoutUnit blockX = right ? lineX + rule : kLayoutUnitZero;
            rulePaint(lineX, top, lineX + rule, lineBottom);
            rulePaint(blockX, divisorBottom, blockX + blockWidth, divisorBottom + rule);
            if (style == PrLayoutMathLongDivision_StackedLeftLineTop)
                rulePaint(stackShift, top, stackShift + s.width, top + rule);
            divisor.x = blockX + space;
            result.x = blockX + space;
            setY(divisor, dividendBaseline);
            setY(result, resultBaseline);
            s.height = layoutUnitMax(s.height, resultBaseline + result.descent);
            width = right ? blockX + blockWidth : stackShift + s.width;
            break;
        }
        case PrLayoutMathLongDivision_LeftSlashRight:
        case PrLayoutMathLongDivision_LeftParenRight: {
            // The divisor, the dividend and the result go in a row, with the dividend between strokes or parentheses
            // that reach over the dividend's row, as in divisor / dividend \ result or divisor ) dividend ( result.
            PrLayoutUnit top = dividendTop - gap;
            PrLayoutUnit bottom = layoutUnitMax(dividendBottom, dividendBaseline) + gap;
            PrLayoutUnit x = divisor.width + space;
            divisor.x = kLayoutUnitZero;
            if (style == PrLayoutMathLongDivision_LeftSlashRight) {
                PrLayoutUnit slant = half(bottom - top) / 2;
                float thickness = prLayoutUnitToPx(rule);
                moveStack(x + slant + space);
                s.paints.push(StackPaint{
                    .kind = LayoutMathPaintKind_Line,
                    .x0 = x,
                    .y0 = bottom,
                    .x1 = x + slant,
                    .y1 = top,
                    .thickness = thickness,
                });
                PrLayoutUnit after = stackShift + s.width + space;
                s.paints.push(StackPaint{
                    .kind = LayoutMathPaintKind_Line,
                    .x0 = after,
                    .y0 = top,
                    .x1 = after + slant,
                    .y1 = bottom,
                    .thickness = thickness,
                });
                result.x = after + slant + space;
            } else {
                DivisionSign close = divisionSign(m, ')', bottom - top);
                DivisionSign open = divisionSign(m, '(', bottom - top);
                moveStack(x + close.advance + space);
                pushSign(s, close, x, top);
                PrLayoutUnit after = stackShift + s.width + space;
                pushSign(s, open, after, top);
                result.x = after + open.advance + space;
            }
            setY(divisor, dividendBaseline);
            setY(result, dividendBaseline);
            width = result.x + result.width;
            break;
        }
        case PrLayoutMathLongDivision_ColonRightEqualRight: {
            // dividend : divisor = result, on the dividend's baseline, the colon and the equals sign spaced as
            // relations are.
            DivisionSign colon = divisionSign(m, ':', kLayoutUnitZero);
            DivisionSign equals = divisionSign(m, '=', kLayoutUnitZero);
            PrLayoutUnit x = s.width + space;
            pushSign(s, colon, x, dividendBaseline - colon.ascent);
            x += colon.advance + space;
            divisor.x = x;
            x += divisor.width + space;
            pushSign(s, equals, x, dividendBaseline - equals.ascent);
            x += equals.advance + space;
            result.x = x;
            setY(divisor, dividendBaseline);
            setY(result, dividendBaseline);
            width = result.x + result.width;
            break;
        }
    }
    // A top above the stack's, from the signs and the divisor and the result, moves everything down.
    PrLayoutUnit top = kLayoutUnitZero;
    for (uint32_t i = 0; i < m.items.count; ++i)
        top = layoutUnitMin(top, s.itemY.data[i] - m.items.data[i].ascent);
    for (const StackPaint& p : s.paints.span())
        top = layoutUnitMin(top, layoutUnitMin(p.y0, p.y1));
    if (top < kLayoutUnitZero) {
        for (uint32_t i = 0; i < m.items.count; ++i)
            s.itemY.data[i] -= top;
        for (StackPaint& p : s.paints.span()) {
            p.y0 -= top;
            p.y1 -= top;
        }
        dividendBaseline -= top;
        s.height -= top;
    }
    const StackRow* row = alignedRow(s, m.self.tableRow);
    PrLayoutUnit rowBaseline = row != nullptr ? row->baseline - top : dividendBaseline;
    if (m.self.tableRow == 0)
        rowBaseline = dividendBaseline;
    Extent e = finishStack(m, s, alignedBaseline(m, s.height, rowBaseline), width);
    s.reset();
    return e;
}

// ---------------------------------------------------------------------------------------------------------------------
// Line breaking (mathml4/#presm_linebreaking)

// A place where a row may break, with the operator or mspace that it breaks at, the first item of the next line, the
// end of the line that it ends, and the penalty of breaking there. A duplicated operator starts the next line and ends
// this one as a copy.
struct BreakPoint {
    uint32_t item;
    uint32_t next;
    uint32_t end;
    int32_t penalty;
    bool duplicate;
    uint16_t shown;                 // the glyph that shows an invisible times at the end of the line, or 0
    PrLayoutUnit shownWidth;
};

// Returns where the operator goes when a line breaks at it. Its attributes decide if they say. Otherwise it goes at the
// end of the line in a formula in compact style, as TeX breaks a formula in a paragraph, and at the start of the next
// line in other formulas, as MathML 4's dictionary has it.
PrLayoutMathBreakStyle breakStyleOf(const Math& m, const Item& it) {
    if (it.math->breakStyle != PrLayoutMathBreakStyle_Auto)
        return it.math->breakStyle;
    return m.compact ? PrLayoutMathBreakStyle_After : PrLayoutMathBreakStyle_Before;
}

// Returns the text node of the token at the core of `it`, or kPrLayoutNodeNone.
LayoutNodeIndex textOf(const Math& m, const Item& it) {
    LayoutNodeIndex node = it.node;
    while (node != kPrLayoutNodeNone && layoutTreeNode(m.state.tree, node).kind != PrLayoutNodeKind_Text)
        node = layoutTreeNode(m.state.tree, node).first;
    return node;
}

// Appends the places where the items from `begin` to `end` may break. They are the embellished operators and mspaces
// whose penalty allows a break, outside every pair of fences, with a line on either side. A forced break inside fences
// counts.
void breakPoints(const Math& m, uint32_t begin, uint32_t end, RawArray<BreakPoint>* out) {
    int32_t depth = 0;
    for (uint32_t i = begin; i < end; ++i) {
        const Item& it = m.items.data[i];
        bool fence = hasOperatorFlag(it, PrLayoutMathOperatorFlag_Fence);
        if (fence && hasOperatorFlag(it, PrLayoutMathOperatorFlag_Postfix))
            --depth;
        bool breaks = embellished(it) || (it.mathBox && it.math->kind == PrLayoutMathKind_Space);
        int32_t penalty = breaks ? it.math->breakPenalty : kPrLayoutMathNoBreak;
        if (penalty < kPrLayoutMathNoBreak && (depth == 0 || penalty <= kPrLayoutMathForcedBreak)) {
            BreakPoint point{.item = i, .next = i, .end = i, .penalty = penalty};
            switch (breakStyleOf(m, it)) {
                case PrLayoutMathBreakStyle_After:
                    point.next = point.end = i + 1;
                    break;
                case PrLayoutMathBreakStyle_Duplicate:
                    point.duplicate = true;
                    break;
                default:
                    break;
            }
            // An invisible times that a break makes visible (linebreakmultchar) ends the line it breaks, as TeX's \*
            // does.
            if (it.math->breakChar != 0 && m.font.font != nullptr && firstCodePoint(m.state.tree, textOf(m, it)) == 0x2062) {
                point.shown = prFontCodePointGlyph(m.font.font, it.math->breakChar);
                if (point.shown != 0)
                    point.shownWidth = prLayoutUnitFromPx(
                        prFontGlyphAdvance(m.font.font, point.shown, m.font.size) * m.font.size);
            }
            uint32_t lineStart = out->count != 0 ? out->data[out->count - 1].next : begin;
            if (point.end > lineStart && point.next < end)
                out->push(point);
        }
        if (fence && hasOperatorFlag(it, PrLayoutMathOperatorFlag_Prefix))
            ++depth;
    }
}

// Returns the inline size of a line of the placed items from `first` to `end`, without the spaces of an operator at
// either end, with a copy of the operator at `copy` after them unless `copy` is kStackWhole, and `shown` past that.
PrLayoutUnit lineWidthOf(const Math& m, uint32_t first, uint32_t end, uint32_t copy, PrLayoutUnit shown) {
    if (end <= first)
        return kLayoutUnitZero;
    const Item& last = m.items.data[end - 1];
    PrLayoutUnit width = last.x + last.width - m.items.data[first].x + shown;
    if (copy != kStackWhole)
        width += m.items.data[copy].math->leftSpace + m.items.data[copy].width;
    return width;
}

// Pushes the glyphs of the fragment `f`, which a copy of an operator draws with its inline-start edge at `x` and its
// border box's block-start edge at `y` px, about the box's baseline.
void copyGlyphs(Math& m, const LayoutFragment* f, float x, float y) {
    if (f->kind == LayoutFragmentKind_Text) {
        const LayoutFragmentText& text = f->text;
        const PrFont* const* fonts = layoutFragmentTextFonts(f);
        const LayoutFragmentFontRange* ranges = layoutFragmentTextRanges(f);
        uint32_t range = 0;
        for (uint32_t g = 0; g < text.glyphCount; ++g) {
            while (range + 1 < text.rangeCount && g >= ranges[range].glyphEnd)
                ++range;
            const PrFont* font = fonts[ranges[range].font];
            PrFontGlyph glyph{.id = text.glyphs[g].id};
            PrVec2 at{x + text.glyphs[g].position.x, y + text.glyphs[g].position.y};
            float advance = prFontGlyphAdvance(font, glyph.id, text.size) * text.size;
            pushGlyphs(m.state, font, text.size, &glyph, 1, at, PrRect{at.x, at.y - text.size, at.x + advance, at.y});
        }
        return;
    }
    if ((f->flags & LayoutFragmentFlag_Math) != 0) {
        // A stretched or substituted operator draws its glyphs itself.
        LayoutMathData data = layoutFragmentMathData(f);
        for (uint32_t i = 0; i < data.paintCount; ++i) {
            const LayoutMathPaint& p = data.paints[i];
            if (p.kind != LayoutMathPaintKind_Glyphs)
                continue;
            for (uint32_t g = 0; g < p.glyphCount; ++g) {
                PrFontGlyph glyph{.id = data.glyphs[p.glyphStart + g].id};
                PrVec2 at = data.glyphs[p.glyphStart + g].position;
                PrVec2 placed{x + at.x, y + at.y};
                pushGlyphs(m.state, p.font, p.size, &glyph, 1, placed,
                    PrRect{x + p.rect.left, y + p.rect.top, x + p.rect.right, y + p.rect.bottom}, p.mirrored);
            }
        }
    }
    for (uint32_t i = 0; i < f->childCount; ++i) {
        const LayoutChild& child = f->children[i];
        copyGlyphs(m, child.fragment, x + prLayoutUnitToPx(child.offset.x), y + prLayoutUnitToPx(child.offset.y));
    }
}

// Breaks the placed items from `begin` to `end`, whose row's extent is `e`, into lines no wider than `width` if it is
// not zero, and at every forced break (mathml4/#presm_linebreaking), and returns the extent of the lines.
//
// The lines are chosen as TeX chooses a paragraph's, by the breaking whose demerits sum least. A line's demerits grow
// with the cube of the room that it leaves and with the square of its break's penalty. A line wider than `width` that
// no break can shorten stays as it is. The first line starts at the row's start. Each line after it starts where its
// break's indentalign puts it, past its indentshift. By default that is after the first line's first relation, where
// the right side of an equation starts, or 2em in if that is past half the width. Lines stack a baselineskip of 1.2em
// apart, or further if their boxes would come within a quarter em of each other.
Extent breakLines(Math& m, uint32_t begin, uint32_t end, const Extent& e, PrLayoutUnit width) {
    RawArray<BreakPoint> points;
    breakPoints(m, begin, end, &points);
    bool forced = false;
    for (const BreakPoint& p : points.span())
        forced = forced || p.penalty <= kPrLayoutMathForcedBreak;
    if (points.count == 0 || (!forced && (width <= kLayoutUnitZero || e.width <= width))) {
        points.reset();
        return e;
    }
    // The first relation of the first line, past which the lines after it start.
    PrLayoutUnit start = m.items.data[begin].x;
    PrLayoutUnit indent = m.font.u(2);
    for (const BreakPoint& p : points.span()) {
        const Item& it = m.items.data[p.item];
        if (it.math->breakPenalty == 500) {
            PrLayoutUnit after = it.x + it.width + it.math->rightSpace - start;
            if (width <= kLayoutUnitZero || after <= half(width))
                indent = after;
            break;
        }
    }
    // Where a line `lineWidth` wide starts, as the break `before` says, or for the first line, as the break `after`
    // that ends it says. The first line is at the row's start unless indentalignfirst moves it, and the last line goes
    // as its break's indentalignlast says (mathml4/#presm_linebreaking).
    auto indentOf = [&](const BreakPoint* before, const BreakPoint* after, PrLayoutUnit lineWidth) {
        const BreakPoint* p = before != nullptr ? before : after;
        if (p == nullptr)
            return kLayoutUnitZero;
        const PrLayoutMathStyle& style = *m.items.data[p->item].math;
        PrLayoutMathIndent align = style.indent;
        PrLayoutUnit shift = style.indentShift;
        PrLayoutUnit at = indent;
        if (before == nullptr) {
            align = style.indentFirst;
            shift = style.indentShiftFirst;
            at = kLayoutUnitZero;
        } else if (after == nullptr) {
            align = style.indentLast;
            shift = style.indentShiftLast;
        }
        if (align == PrLayoutMathIndent_Left)
            at = kLayoutUnitZero;
        else if (align == PrLayoutMathIndent_Right && width > kLayoutUnitZero)
            at = width - lineWidth;
        else if (align == PrLayoutMathIndent_Center && width > kLayoutUnitZero)
            at = half(width - lineWidth);
        return layoutUnitMax(kLayoutUnitZero, at + shift);
    };
    // The nodes are the row's start, each break point, and the row's end.
    uint32_t nodes = points.count + 2;
    RawArray<double> best;
    RawArray<uint32_t> from;
    best.resize(nodes);
    from.resize(nodes);
    for (uint32_t k = 0; k < nodes; ++k) {
        best.data[k] = k == 0 ? 0 : 1e300;
        from.data[k] = 0;
    }
    auto lineFrom = [&](uint32_t node) { return node == 0 ? begin : points.data[node - 1].next; };
    for (uint32_t k = 1; k < nodes; ++k) {
        bool last = k == nodes - 1;
        const BreakPoint* at = last ? nullptr : &points.data[k - 1];
        uint32_t lineEnd = last ? end : at->end;
        uint32_t copy = at != nullptr && at->duplicate ? at->item : kStackWhole;
        for (uint32_t j = k; j-- > 0;) {
            if (best.data[j] >= 1e300)
                continue;
            uint32_t first = lineFrom(j);
            if (lineEnd <= first)
                continue;
            const BreakPoint* before = j == 0 ? nullptr : &points.data[j - 1];
            PrLayoutUnit lineWidth =
                lineWidthOf(m, first, lineEnd, copy, at != nullptr ? at->shownWidth : kLayoutUnitZero);
            PrLayoutUnit room = width - indentOf(before, at, lineWidth) - lineWidth;
            double demerits = 0;
            if (width > kLayoutUnitZero) {
                double slack = double(room.raw) / double(width.raw);
                double badness = slack < 0 ? 1e6 * (1 - slack) : (last ? 0 : prF64Min(100 * slack * slack * slack, 1e4));
                demerits = (10 + badness) * (10 + badness);
            }
            int32_t penalty = at != nullptr ? at->penalty : 0;
            if (penalty > 0)
                demerits += double(penalty) * double(penalty);
            else if (penalty > kPrLayoutMathForcedBreak)
                demerits -= double(penalty) * double(penalty);
            // Only forced breaks break a row that has no width to fit.
            if (width <= kLayoutUnitZero && penalty > kPrLayoutMathForcedBreak && !last)
                continue;
            if (best.data[j] + demerits < best.data[k]) {
                best.data[k] = best.data[j] + demerits;
                from.data[k] = j;
            }
            // A line does not reach past a forced break.
            if (before != nullptr && before->penalty <= kPrLayoutMathForcedBreak)
                break;
        }
    }
    // The chosen breaks, from the end back.
    RawArray<uint32_t> chosen;
    for (uint32_t k = nodes - 1; k != 0; k = from.data[k])
        chosen.push(k);
    PrLayoutUnit pitch = m.font.u(1.2f);
    PrLayoutUnit lead = m.font.u(0.25f);
    // lineleading sets the space between the lines' boxes, as the breaks' operators give it.
    const PrLayoutMathStyle& firstBreak = *m.items.data[points.data[0].item].math;
    if (firstBreak.lineLeading >= kLayoutUnitZero) {
        pitch = kLayoutUnitZero;
        lead = firstBreak.lineLeading;
    }
    Extent out{.attachment = kLayoutMathNoAttachment};
    PrLayoutUnit baseline = kLayoutUnitZero;
    PrLayoutUnit previousDescent = kLayoutUnitZero;
    uint32_t previous = 0;
    for (uint32_t c = chosen.count; c-- > 0;) {
        uint32_t k = chosen.data[c];
        bool firstLine = c == chosen.count - 1;
        bool last = k == nodes - 1;
        const BreakPoint* at = last ? nullptr : &points.data[k - 1];
        const BreakPoint* before = previous == 0 ? nullptr : &points.data[previous - 1];
        uint32_t first = lineFrom(previous);
        uint32_t lineEnd = last ? end : at->end;
        uint32_t copy = at != nullptr && at->duplicate ? at->item : kStackWhole;
        PrLayoutUnit shownWidth = at != nullptr ? at->shownWidth : kLayoutUnitZero;
        PrLayoutUnit lineWidth = lineWidthOf(m, first, lineEnd, copy, shownWidth);
        PrLayoutUnit dx = indentOf(before, at, lineWidth) - m.items.data[first].x;
        PrLayoutUnit copyX = m.items.data[lineEnd - 1].x + m.items.data[lineEnd - 1].width + dx;
        moveItems(m, first, lineEnd, dx, kLayoutUnitZero);
        Extent line = extentOf(m, first, lineEnd);
        if (copy != kStackWhole) {
            line.ascent = layoutUnitMax(line.ascent, m.items.data[copy].ascent);
            line.descent = layoutUnitMax(line.descent, m.items.data[copy].descent);
        }
        if (!firstLine)
            baseline += layoutUnitMax(pitch, previousDescent + lead + line.ascent);
        moveItems(m, first, lineEnd, kLayoutUnitZero, -baseline);
        if (copy != kStackWhole) {
            const Item& it = m.items.data[copy];
            float x = prLayoutUnitToPx(copyX + it.math->leftSpace + it.marginStart);
            float y = prLayoutUnitToPx(baseline - it.baseline);
            copyGlyphs(m, it.fragment, x, y);
            copyX += it.math->leftSpace + it.width;
        }
        if (at != nullptr && at->shown != 0) {
            PrFontGlyph glyph{.id = at->shown};
            float x = prLayoutUnitToPx(copyX);
            float y = prLayoutUnitToPx(baseline);
            PrRect ink = prFontGlyphInkBounds(m.font.font, at->shown);
            pushGlyphs(m.state, m.font.font, m.font.size, &glyph, 1, PrVec2{x, y},
                PrRect{x, y + ink.top * m.font.size, x + prLayoutUnitToPx(shownWidth), y + ink.bottom * m.font.size});
        }
        PrLayoutUnit right = m.items.data[first].x + lineWidth;
        out.width = firstLine ? right : layoutUnitMax(out.width, right);
        if (firstLine) {
            out.ascent = line.ascent;
            out.inkAscent = line.inkAscent;
        }
        out.descent = baseline + line.descent;
        out.inkDescent = baseline + line.inkDescent;
        previousDescent = line.descent;
        previous = k;
    }
    chosen.reset();
    best.reset();
    from.reset();
    points.reset();
    return out;
}

// Returns the width that a row breaks its lines to fit, which is its container's width for the row of a display
// formula, or else what its parent gives it.
PrLayoutUnit lineWidthFor(const Math& m, const LayoutMathStretch* PR_NULLABLE stretch) {
    if (stretch != nullptr && stretch->lineWidth > kLayoutUnitZero)
        return stretch->lineWidth;
    if (m.root && !m.compact && layoutIsDefinite(m.input.contentInline))
        return m.input.contentInline;
    return kLayoutUnitZero;
}

// Lays out a row whose lines may break. Its items are laid out as a row, and broken into lines if they do not fit the
// width that the row breaks to, or if an item forces a break. A row of one child that is a row passes the width on to
// that child, as MathML 4 breaks lines in an mrow at any depth.
Extent breakingRow(Math& m, const LayoutMathStretch* PR_NULLABLE stretch) {
    PrLayoutUnit width = lineWidthFor(m, stretch);
    LayoutMathStretch passed{};
    const LayoutMathStretch* own = stretch;
    if (stretch != nullptr && stretch->lineWidth > kLayoutUnitZero) {
        passed = *stretch;
        passed.lineWidth = kLayoutUnitZero;
        own = layoutMathStretchIsActive(passed) ? &passed : nullptr;
    }
    Extent e = row(m, 0, m.items.count, own);
    if (width > kLayoutUnitZero && m.items.count == 1 && e.width > width) {
        Item& only = m.items.data[0];
        if (only.mathBox && only.math->kind == PrLayoutMathKind_Row && !only.math->embellished) {
            LayoutMathStretch child{.lineWidth = width};
            layOut(m, only, &child);
            return placeRow(m, 0, 1, m.root || !m.self.embellished);
        }
    }
    return breakLines(m, 0, m.items.count, e, width);
}

// ---------------------------------------------------------------------------------------------------------------------
// The box

// Returns true if the kind lays out its children as a row whatever their number.
bool fits(const Math& m) {
    uint32_t count = m.items.count;
    switch (m.self.kind) {
        case PrLayoutMathKind_Fraction:
        case PrLayoutMathKind_Root:
        case PrLayoutMathKind_Subscript:
        case PrLayoutMathKind_Superscript:
        case PrLayoutMathKind_Under:
        case PrLayoutMathKind_Over:
            return count == 2;
        case PrLayoutMathKind_SubSuperscript:
        case PrLayoutMathKind_UnderOver:
            return count == 3;
        default:
            return true;
    }
}

// Lays out an mmultiscripts, or returns false if its children are not a base and pairs of scripts with at most one
// mprescripts between them, as a row lays it out then (mathml-core/#prescripts-and-tensor-indices-mmultiscripts).
bool multiscripts(Math& m, const LayoutMathStretch* PR_NULLABLE stretch, Extent* out) {
    uint32_t count = m.items.count;
    if (count == 0)
        return false;
    uint32_t separator = count;
    for (uint32_t i = 0; i < count; ++i) {
        const Item& it = m.items.data[i];
        if (it.mathBox && it.math->kind == PrLayoutMathKind_Prescripts) {
            if (separator != count || i == 0)
                return false;
            separator = i;
        }
    }
    uint32_t postCount = separator - 1;
    uint32_t preCount = separator == count ? 0 : count - separator - 1;
    if (postCount % 2 != 0 || preCount % 2 != 0)
        return false;
    RawArray<Item*> post;
    RawArray<Item*> pre;
    for (uint32_t i = 1; i < separator; ++i)
        post.push(&m.items.data[i]);
    for (uint32_t i = separator + 1; i < count; ++i)
        pre.push(&m.items.data[i]);
    if (separator != count) {
        Item& mark = m.items.data[separator];
        layOut(m, mark, nullptr);
        mark.width = kLayoutUnitZero;
    }
    *out = scripts(m, m.items.data[0], post.data, postCount / 2, pre.data, preCount / 2, stretch);
    if (separator != count)
        m.items.data[separator].x = m.items.data[0].x;
    post.reset();
    pre.reset();
    return true;
}

Extent layOutKind(Math& m, const LayoutMathStretch* PR_NULLABLE stretch) {
    PrLayoutMathKind kind = m.self.kind;
    if (!fits(m))
        kind = PrLayoutMathKind_Row;
    // Under and over scripts on an operator with movable limits are scripts in compact style
    // (mathml-core/#underscripts-and-overscripts-munder-mover-munderover).
    if ((kind == PrLayoutMathKind_Under || kind == PrLayoutMathKind_Over || kind == PrLayoutMathKind_UnderOver) &&
            m.compact && hasOperatorFlag(m.items.data[0], PrLayoutMathOperatorFlag_MovableLimits)) {
        if (kind == PrLayoutMathKind_Under)
            kind = PrLayoutMathKind_Subscript;
        else if (kind == PrLayoutMathKind_Over)
            kind = PrLayoutMathKind_Superscript;
        else
            kind = PrLayoutMathKind_SubSuperscript;
    }
    Item* items = m.items.data;
    switch (kind) {
        case PrLayoutMathKind_Fraction:
            return fraction(m, stretch);
        case PrLayoutMathKind_SquareRoot:
            return radical(m, 0, m.items.count, stretch, true);
        case PrLayoutMathKind_Root:
            return root(m, stretch);
        case PrLayoutMathKind_Padded:
            return padded(m, stretch);
        case PrLayoutMathKind_Enclose:
            return enclose(m, stretch);
        case PrLayoutMathKind_Fenced:
            return fenced(m, stretch);
        case PrLayoutMathKind_Stack:
            return stack(m);
        case PrLayoutMathKind_LongDivision:
            return m.items.count >= 2 ? longDivision(m) : row(m, 0, m.items.count, stretch);
        case PrLayoutMathKind_Subscript: {
            Item* pair[2] = {&items[1], nullptr};
            return scripts(m, items[0], pair, 1, nullptr, 0, stretch);
        }
        case PrLayoutMathKind_Superscript: {
            Item* pair[2] = {nullptr, &items[1]};
            return scripts(m, items[0], pair, 1, nullptr, 0, stretch);
        }
        case PrLayoutMathKind_SubSuperscript: {
            Item* pair[2] = {&items[1], &items[2]};
            return scripts(m, items[0], pair, 1, nullptr, 0, stretch);
        }
        case PrLayoutMathKind_Under:
            return underOver(m, items[0], &items[1], nullptr, stretch);
        case PrLayoutMathKind_Over:
            return underOver(m, items[0], nullptr, &items[1], stretch);
        case PrLayoutMathKind_UnderOver:
            return underOver(m, items[0], &items[1], &items[2], stretch);
        case PrLayoutMathKind_Multiscripts: {
            Extent e{};
            if (multiscripts(m, stretch, &e))
                return e;
            return row(m, 0, m.items.count, stretch);
        }
        case PrLayoutMathKind_Space: {
            Extent e{.attachment = kLayoutMathNoAttachment};
            e.ascent = layoutUnitMax(kLayoutUnitZero, m.self.spaceAscent);
            PrLayoutUnit block = layoutUnitMax(kLayoutUnitZero, m.input.contentBlock);
            e.descent = block - e.ascent;
            e.inkAscent = e.ascent;
            e.inkDescent = e.descent;
            return e;
        }
        default:
            return breakingRow(m, stretch);
    }
}

// Returns true if the kind centers its math content in a content box wider than it
// (mathml-core/#fraction-with-nonzero-line-thickness, mathml-core/#base-with-underscript).
bool centers(const Math& m) {
    PrLayoutMathKind kind = m.self.kind;
    return m.root || kind == PrLayoutMathKind_Fraction || kind == PrLayoutMathKind_Under ||
        kind == PrLayoutMathKind_Over || kind == PrLayoutMathKind_UnderOver;
}

// ---------------------------------------------------------------------------------------------------------------------
// Tokens

// The ink of a token's line, about its baseline, and its one glyph if it has one.
struct TokenInk {
    bool text;                      // true if the line holds only text
    bool any;                       // true if some glyph has ink
    float top;                      // px above the baseline, positive up
    float bottom;                   // px below the baseline, positive down
    uint32_t glyphs;
    uint16_t glyph;
    const PrFont* PR_NULLABLE font;
    float size;
    float x;                        // the glyph's origin from the border box's inline-start edge, px
    LayoutNodeIndex textNode;
};

// Measures the glyphs of the lines among `children` from `mark`, whose baseline is `baseline` units from the border
// box's block-start edge.
TokenInk inkOf(const RawArray<LayoutLogicalChild>& children, uint32_t mark, PrLayoutUnit baseline, LayoutMode mode) {
    TokenInk ink{.text = true};
    uint32_t lines = 0;
    for (uint32_t i = mark; i < children.count; ++i) {
        const LayoutLogicalChild& line = children.data[i];
        if (line.fragment->kind != LayoutFragmentKind_Line) {
            ink.text = false;
            continue;
        }
        ++lines;
        // A line's children are placed physically in it, and a horizontal line's physical axes are the box's.
        float lineX = prLayoutUnitToPx(line.offset.inlineOffset);
        float lineY = prLayoutUnitToPx(line.offset.blockOffset);
        for (uint32_t k = 0; k < line.fragment->childCount; ++k) {
            const LayoutChild& child = line.fragment->children[k];
            const LayoutFragment* f = child.fragment;
            if (f->kind != LayoutFragmentKind_Text) {
                ink.text = false;
                continue;
            }
            const LayoutFragmentText& text = f->text;
            const PrFont* const* fonts = layoutFragmentTextFonts(f);
            const LayoutFragmentFontRange* ranges = layoutFragmentTextRanges(f);
            PrVec2 at = PrVec2{lineX + prLayoutUnitToPx(child.offset.x), lineY + prLayoutUnitToPx(child.offset.y)};
            uint32_t range = 0;
            for (uint32_t g = 0; g < text.glyphCount; ++g) {
                while (range + 1 < text.rangeCount && g >= ranges[range].glyphEnd)
                    ++range;
                const PrFont* font = fonts[ranges[range].font];
                const PrFontGlyph& glyph = text.glyphs[g];
                PrRect bounds = prFontGlyphInkBounds(font, glyph.id);
                ++ink.glyphs;
                ink.glyph = glyph.id;
                ink.font = font;
                ink.size = text.size;
                ink.x = at.x + glyph.position.x;
                ink.textNode = f->node;
                if (bounds.right <= bounds.left || bounds.bottom <= bounds.top)
                    continue;
                float y = at.y + glyph.position.y - prLayoutUnitToPx(baseline);
                float top = -(y + bounds.top * text.size);
                float bottom = y + bounds.bottom * text.size;
                ink.top = ink.any ? prF32Max(ink.top, top) : top;
                ink.bottom = ink.any ? prF32Max(ink.bottom, bottom) : bottom;
                ink.any = true;
            }
        }
    }
    if (lines != 1 || !layoutIsHorizontal(mode))
        ink.text = false;
    return ink;
}

// A glyph of a token's line, with its font and size, and its origin in px from the border box's inline-start edge and
// below the line's baseline.
struct LineGlyph {
    const PrFont* font;
    float size;
    uint16_t id;
    float x;
    float y;
};

// Appends the glyphs of the lines among `children` from `mark`, whose baseline is `baseline` units from the border
// box's block-start edge, in the order of the text.
void lineGlyphsOf(const RawArray<LayoutLogicalChild>& children, uint32_t mark, PrLayoutUnit baseline,
        RawArray<LineGlyph>* out) {
    for (uint32_t i = mark; i < children.count; ++i) {
        const LayoutLogicalChild& line = children.data[i];
        if (line.fragment->kind != LayoutFragmentKind_Line)
            continue;
        float lineX = prLayoutUnitToPx(line.offset.inlineOffset);
        float lineY = prLayoutUnitToPx(line.offset.blockOffset);
        for (uint32_t k = 0; k < line.fragment->childCount; ++k) {
            const LayoutChild& child = line.fragment->children[k];
            const LayoutFragment* f = child.fragment;
            if (f->kind != LayoutFragmentKind_Text)
                continue;
            const LayoutFragmentText& text = f->text;
            const PrFont* const* fonts = layoutFragmentTextFonts(f);
            const LayoutFragmentFontRange* ranges = layoutFragmentTextRanges(f);
            uint32_t range = 0;
            for (uint32_t g = 0; g < text.glyphCount; ++g) {
                while (range + 1 < text.rangeCount && g >= ranges[range].glyphEnd)
                    ++range;
                const PrFontGlyph& glyph = text.glyphs[g];
                out->push(LineGlyph{
                    .font = fonts[ranges[range].font],
                    .size = text.size,
                    .id = glyph.id,
                    .x = lineX + prLayoutUnitToPx(child.offset.x) + glyph.position.x,
                    .y = lineY + prLayoutUnitToPx(child.offset.y) + glyph.position.y - prLayoutUnitToPx(baseline),
                });
            }
        }
    }
}

// Sets the glyphs of a number of a stack in the columns that `stretch` gives them, in place of its line. Each glyph
// goes at its offset, and a glyph past the offsets goes as far past the last as its line sets it. Returns the inline
// size that they cover.
PrLayoutUnit setInColumns(LayoutState& state, const LayoutMathInput& input, const LayoutMathStretch& stretch,
        PrLayoutUnit inkAscent, PrLayoutUnit inkDescent) {
    RawArray<LayoutLogicalChild>& children = state.scratch->children;
    RawArray<LineGlyph> glyphs;
    lineGlyphsOf(children, input.mark, input.baseline, &glyphs);
    layoutFragmentDiscard(children, input.mark);
    float shift = 0;
    float right = 0;
    for (uint32_t i = 0; i < glyphs.count; ++i) {
        const LineGlyph& g = glyphs.data[i];
        if (i < stretch.columnCount)
            shift = prLayoutUnitToPx(stretch.columns[i]) - g.x;
        float x = g.x + shift;
        float advance = prFontGlyphAdvance(g.font, g.id, g.size) * g.size;
        PrFontGlyph placed{.id = g.id};
        pushGlyphs(state, g.font, g.size, &placed, 1, PrVec2{x, prLayoutUnitToPx(inkAscent) + g.y},
            PrRect{x, 0, x + advance, prLayoutUnitToPx(inkAscent + inkDescent)});
        right = prF32Max(right, x + advance);
    }
    glyphs.reset();
    return prLayoutUnitFromPx(right);
}

// Returns where a table cell's math content goes in the `free` room its content box leaves, by its text-align.
PrLayoutUnit cellShift(PrLayoutTextAlign align, LayoutMode mode, PrLayoutUnit free) {
    bool ltr = mode.direction == PrTextDirection_Ltr;
    switch (align) {
        case PrLayoutTextAlign_Center:
        case PrLayoutTextAlign_Justify:
            return half(free);
        case PrLayoutTextAlign_End:
            return free;
        case PrLayoutTextAlign_Left:
            return ltr ? kLayoutUnitZero : free;
        case PrLayoutTextAlign_Right:
            return ltr ? free : kLayoutUnitZero;
        default:
            return kLayoutUnitZero;
    }
}

// How far past its outlines a glyph's antialiased ink may reach, in px. A font drawn without antialiasing reaches no
// further than its outlines.
constexpr float kInkBleed = 0.5f;

// The ink of a math root's content in px. Its left and right are from its inline-start edge, and its top and bottom are
// down from the baseline, so that `top` is negative above it.
struct RootInk {
    bool any;
    float left;
    float right;
    float top;
    float bottom;

    void add(float l, float t, float r, float b) {
        if (r < l || b < t)
            return;
        left = any ? prF32Min(left, l) : l;
        top = any ? prF32Min(top, t) : t;
        right = any ? prF32Max(right, r) : r;
        bottom = any ? prF32Max(bottom, b) : b;
        any = true;
    }
};

// Adds the ink of `fragment`, whose top left is at `at` px about the root's baseline, and of its descendants. The ink
// is the glyphs of text, with the pixels that antialiasing touches around them, and the paints of math boxes. A
// horizontal fragment's physical axes are the root's.
void addInk(RootInk& ink, const LayoutFragment* fragment, PrVec2 at) {
    if (fragment->kind == LayoutFragmentKind_Text) {
        const LayoutFragmentText& text = fragment->text;
        const PrFont* const* fonts = layoutFragmentTextFonts(fragment);
        const LayoutFragmentFontRange* ranges = layoutFragmentTextRanges(fragment);
        uint32_t range = 0;
        for (uint32_t g = 0; g < text.glyphCount; ++g) {
            while (range + 1 < text.rangeCount && g >= ranges[range].glyphEnd)
                ++range;
            const PrFontGlyph& glyph = text.glyphs[g];
            const PrFont* font = fonts[ranges[range].font];
            PrRect bounds = prFontGlyphInkBounds(font, glyph.id);
            float x = at.x + glyph.position.x;
            float y = at.y + glyph.position.y;
            float bleed = prFontIsAliased(font) ? 0 : kInkBleed;
            ink.add(x + bounds.left * text.size - bleed, y + bounds.top * text.size - bleed,
                x + bounds.right * text.size + bleed, y + bounds.bottom * text.size + bleed);
        }
        return;
    }
    if ((fragment->flags & LayoutFragmentFlag_Math) != 0) {
        LayoutMathData data = layoutFragmentMathData(fragment);
        for (uint32_t i = 0; i < data.paintCount; ++i) {
            const LayoutMathPaint& paint = data.paints[i];
            float half = paint.thickness / 2;
            PrRect r = paint.rect;
            ink.add(at.x + prF32Min(r.left, r.right) - half, at.y + prF32Min(r.top, r.bottom) - half,
                at.x + prF32Max(r.left, r.right) + half, at.y + prF32Max(r.top, r.bottom) + half);
        }
    }
    for (uint32_t i = 0; i < fragment->childCount; ++i) {
        const LayoutChild& child = fragment->children[i];
        if (layoutFragmentIsPoint(child.fragment) || (child.fragment->flags & LayoutFragmentFlag_OutOfFlow) != 0)
            continue;
        addInk(ink, child.fragment,
            PrVec2{at.x + prLayoutUnitToPx(child.offset.x), at.y + prLayoutUnitToPx(child.offset.y)});
    }
}

// Grows the extent `e` of a math root's content, laid out left to right, so that it holds the ink of its glyphs and
// paints, with the pixels that antialiasing touches around glyphs. TeX's boxes are its glyphs' metrics, which a glyph's
// ink may reach or pass, as an italic letter's overhang does, and a box that clips its content must not cut what the
// content draws. The content moves along the inline axis by the ink that starts before it.
void coverInk(Math& m, Extent& e) {
    RootInk ink{};
    for (uint32_t i = 0; i < m.items.count; ++i) {
        const Item& it = m.items.data[i];
        float x = prLayoutUnitToPx(it.x + it.marginStart);
        if (it.fragment == nullptr) {
            ink.add(x, -prLayoutUnitToPx(it.inkAscent + it.shift), x + prLayoutUnitToPx(it.width),
                prLayoutUnitToPx(it.inkDescent - it.shift));
            continue;
        }
        addInk(ink, it.fragment, PrVec2{x, -prLayoutUnitToPx(it.shift + it.baseline)});
    }
    for (uint32_t i = m.paints; i < m.state.scratch->mathPaints.count; ++i) {
        const LayoutMathPaint& paint = m.state.scratch->mathPaints.data[i];
        float half = paint.thickness / 2;
        PrRect r = paint.rect;
        ink.add(prF32Min(r.left, r.right) - half, prF32Min(r.top, r.bottom) - half,
            prF32Max(r.left, r.right) + half, prF32Max(r.top, r.bottom) + half);
    }
    if (!ink.any)
        return;
    e.ascent = layoutUnitMax(e.ascent, prLayoutUnitFromPxCeil(-ink.top));
    e.descent = layoutUnitMax(e.descent, prLayoutUnitFromPxCeil(ink.bottom));
    if (isRtl(m))
        return;
    PrLayoutUnit before = prLayoutUnitFromPxCeil(prF32Max(0, -ink.left));
    PrLayoutUnit after = prLayoutUnitFromPxCeil(prF32Max(0, ink.right - prLayoutUnitToPx(e.width)));
    if (before > kLayoutUnitZero) {
        moveItems(m, 0, m.items.count, before, kLayoutUnitZero);
        float dx = prLayoutUnitToPx(before);
        for (uint32_t i = m.paints; i < m.state.scratch->mathPaints.count; ++i) {
            PrRect& r = m.state.scratch->mathPaints.data[i].rect;
            r.left += dx;
            r.right += dx;
        }
        for (uint32_t i = m.glyphs; i < m.state.scratch->mathGlyphs.count; ++i)
            m.state.scratch->mathGlyphs.data[i].position.x += dx;
        if (e.attachment != kLayoutMathNoAttachment)
            e.attachment += before;
    }
    e.width += before + after;
}

} // namespace

LayoutMathOutcome layoutMathContent(LayoutState& state, const LayoutMathInput& input) {
    LayoutTree* tree = state.tree;
    LayoutNodeIndex node = input.node;
    const LayoutTreeNode& n = layoutTreeNode(tree, node);
    const PrLayoutFontStyle& fontStyle = layoutFontStyle(state.styles, n.style);
    LayoutNodeIndex parent = n.parent;
    Math m{
        .state = state,
        .input = input,
        .node = node,
        .mode = input.content->mode,
        .self = layoutMathStyle(state.styles, n.style),
        .font = mathFontOf(state, node),
        .compact = (fontStyle.mathFlags & PrLayoutMathFlag_CompactStyle) != 0,
        .cramped = (fontStyle.mathFlags & PrLayoutMathFlag_CompactShift) != 0,
        .root = parent == kPrLayoutNodeNone || layoutTreeNode(tree, parent).children != LayoutChildren_Math,
        .childSpace = LayoutSpace{
            .mode = input.content->mode,
            .available = kMathAvailable,
            .percentInline = layoutUnitMax(kLayoutUnitZero, input.percentInline),
            .percentBlock = layoutUnit(-1),
            .containing = LayoutLogicalSize{layoutUnitMax(kLayoutUnitZero, input.percentInline), layoutUnit(-1)},
            .blockLimit = layoutUnit(-1),
            .fallback = input.content->fallback,
        },
        .paints = state.scratch->mathPaints.count,
        .glyphs = state.scratch->mathGlyphs.count,
    };
    layoutEachItem(tree, node, [&](uint32_t c, const PrLayoutBoxStyle& box) {
        if (layoutIsOutOfFlow(tree, c)) {
            m.outOfFlow.push(c);
            return;
        }
        m.items.push(Item{
            .node = c,
            .mathBox = layoutIsMathBox(box),
            .box = &box,
            .math = &layoutMathStyle(state.styles, layoutTreeNode(tree, c).style),
        });
    }, [&](uint32_t first, uint32_t) {
        m.items.push(Item{.node = first, .run = true, .box = &layoutAnonymousBoxStyle(), .math = &defaultMath()});
    });
    Extent e = layOutKind(m, input.stretch);
    if (m.root && layoutIsHorizontal(m.mode))
        coverInk(m, e);

    LayoutMathOutcome outcome{};
    outcome.inlineSize = e.width;
    outcome.baseline = e.ascent;
    outcome.blockSize = e.ascent + e.descent;
    outcome.sized = true;
    outcome.inlineSized = true;
    if (layoutIsDefinite(input.contentInline) && input.contentInline > e.width) {
        PrLayoutUnit free = input.contentInline - e.width;
        if (layoutBoxStyle(state.styles, n.style).display == PrLayoutDisplay_TableCell) {
            // A cell places its row as its text-align places a line, as MathML 4's columnalign gives it
            // (mathml4/#presm_mtable).
            outcome.shift = cellShift(fontStyle.textAlign, m.mode, free);
        } else if (centers(m)) {
            outcome.shift = half(free);
        }
    }
    outcome.metrics = LayoutMathMetrics{
        .inkAscent = e.inkAscent,
        .inkDescent = e.inkDescent,
        .italic = e.italic,
        .attachment = e.attachment,
    };
    if (e.attachment != kLayoutMathNoAttachment)
        outcome.metrics.attachment = e.attachment + outcome.shift;
    // An embellished operator of one child takes its glyph's identity, so that its scripts kern into its corners.
    if (m.items.count == 1 && m.self.kind == PrLayoutMathKind_Row && m.items.data[0].fragment != nullptr) {
        const Item& only = m.items.data[0];
        outcome.metrics.glyph = only.glyph;
        outcome.metrics.glyphFont = only.glyphFont;
        outcome.metrics.glyphSize = only.glyphSize;
        outcome.metrics.extended = only.extended;
    }

    // The children go at their places, with their baselines `shift` above the math content box's baseline.
    RawArray<LayoutLogicalChild>& children = state.scratch->children;
    for (uint32_t i = 0; i < m.items.count; ++i) {
        Item& it = m.items.data[i];
        if (it.fence != 0) {
            drawFence(m, it);
            continue;
        }
        if (it.fragment == nullptr)
            layOut(m, it, nullptr);
        LayoutLogicalOffset at{
            input.origin.inlineOffset + outcome.shift + it.x + it.marginStart,
            input.origin.blockOffset + e.ascent - it.shift - it.baseline,
        };
        children.push(LayoutLogicalChild{it.fragment, at});
        it.fragment = nullptr;
    }
    for (uint32_t i = 0; i < m.outOfFlow.count; ++i) {
        LayoutNodeIndex c = m.outOfFlow.data[i];
        LayoutStaticEdges edges = layoutStaticEdgesAligned(m.mode, LayoutStaticAlign_Start, LayoutStaticAlign_Start,
            false, false);
        children.push(LayoutLogicalChild{
            layoutFragmentCreatePlaceholder(c, edges, layoutIsFixed(tree, c)),
            input.origin,
        });
    }
    // Paints recorded about the baseline move to the math content box's corner.
    float dx = prLayoutUnitToPx(outcome.shift);
    float dy = prLayoutUnitToPx(e.ascent);
    for (uint32_t i = m.paints; i < state.scratch->mathPaints.count; ++i) {
        PrRect& r = state.scratch->mathPaints.data[i].rect;
        r = PrRect{r.left + dx, r.top + dy, r.right + dx, r.bottom + dy};
    }
    for (uint32_t i = m.glyphs; i < state.scratch->mathGlyphs.count; ++i) {
        PrVec2& p = state.scratch->mathGlyphs.data[i].position;
        p = PrVec2{p.x + dx, p.y + dy};
    }
    outcome.paints = m.paints;
    outcome.glyphs = m.glyphs;
    m.items.reset();
    m.outOfFlow.reset();
    m.fenceStyles.reset();
    return outcome;
}

// ---------------------------------------------------------------------------------------------------------------------
// Tokens

LayoutMathOutcome layoutMathToken(LayoutState& state, const LayoutMathInput& input) {
    LayoutTree* tree = state.tree;
    const LayoutTreeNode& n = layoutTreeNode(tree, input.node);
    const PrLayoutMathStyle& math = layoutMathStyle(state.styles, n.style);
    RawArray<LayoutLogicalChild>& children = state.scratch->children;
    LayoutMode mode = input.content->mode;
    LayoutMathOutcome outcome{
        .metrics = LayoutMathMetrics{.attachment = kLayoutMathNoAttachment},
        .paints = state.scratch->mathPaints.count,
        .glyphs = state.scratch->mathGlyphs.count,
    };
    PrLayoutUnit baseline = input.baseline;
    if (baseline == kLayoutBaselineNone) {
        // A token with no line, such as an empty one, has no ink and its baseline at its content's block-start edge.
        outcome.sized = true;
        return outcome;
    }
    TokenInk ink = inkOf(children, input.mark, baseline, mode);
    if (!ink.text) {
        // A token that is not only text on one line is measured by its box (mathml-core/#layout-of-mtext).
        outcome.baseline = baseline - input.origin.blockOffset;
        return outcome;
    }
    PrLayoutUnit inkAscent = ink.any ? prLayoutUnitFromPxCeil(ink.top) : kLayoutUnitZero;
    PrLayoutUnit inkDescent = ink.any ? prLayoutUnitFromPxCeil(ink.bottom) : kLayoutUnitZero;
    outcome.sized = true;
    outcome.metrics.inkAscent = inkAscent;
    outcome.metrics.inkDescent = inkDescent;
    if (input.stretch != nullptr && input.stretch->columnCount != 0) {
        outcome.inlineSize = setInColumns(state, input, *input.stretch, inkAscent, inkDescent);
        outcome.inlineSized = true;
        outcome.baseline = inkAscent;
        outcome.blockSize = inkAscent + inkDescent;
        return outcome;
    }
    if (ink.glyphs == 1 && ink.font != nullptr) {
        outcome.metrics.glyph = ink.glyph;
        outcome.metrics.glyphFont = ink.font;
        outcome.metrics.glyphSize = ink.size;
        outcome.metrics.extended = prFontMathGlyphIsExtendedShape(ink.font, ink.glyph);
        outcome.metrics.italic = prLayoutUnitFromPx(prFontMathGlyphItalicsCorrection(ink.font, ink.glyph) * ink.size);
        outcome.metrics.attachment = prLayoutUnitFromPx(ink.x - prLayoutUnitToPx(input.origin.inlineOffset) +
            prFontMathGlyphTopAccentAttachment(ink.font, ink.glyph) * ink.size);
    }

    // An operator of one glyph stretches to the constraint that its parent gives it, or is drawn large in display style
    // (mathml-core/#layout-of-operators).
    bool stretched = false;
    Stretched glyphs{};
    PrLayoutUnit delta = kLayoutUnitZero;
    const LayoutMathStretch stretch = input.stretch != nullptr ? *input.stretch : LayoutMathStretch{};
    // A flattened accent takes the flattened form of its glyph (OpenType MATH, flac), and stretches that form.
    uint16_t glyph = ink.glyph;
    auto flattenedOf = [&](uint16_t g) {
        return stretch.flattened && g != 0 ? prFontFeatureGlyph(ink.font, PR_FONT_TAG('f', 'l', 'a', 'c'), g) : g;
    };
    if (ink.glyphs == 1 && ink.font != nullptr)
        glyph = flattenedOf(ink.glyph);
    if (math.kind == PrLayoutMathKind_Operator && ink.glyphs == 1 && ink.font != nullptr) {
        uint32_t codePoint = firstCodePoint(tree, ink.textNode);
        const PrFontMathConstants& c = *prFontMathConstants(ink.font);
        const PrLayoutFontStyle& fontStyle = layoutFontStyle(state.styles, n.style);
        PrLayoutGroupIndex fontGroup = layoutStyle(state.styles, n.style).groups[PrLayoutGroup_Font];
        PrLayoutUnit axis = prLayoutUnitFromPx(c.axisHeight * ink.size);
        PrLayoutMathOperatorFlags flags = math.operatorFlags;
        bool stretchy = (flags & PrLayoutMathOperatorFlag_Stretchy) != 0;
        bool inlineAxis = (flags & PrLayoutMathOperatorFlag_InlineAxis) != 0;
        bool symmetric = (flags & PrLayoutMathOperatorFlag_Symmetric) != 0;
        if (stretchy && inlineAxis && stretch.axis == LayoutMathStretchAxis_Inline) {
            float target = prLayoutUnitToPx(stretch.size);
            stretched = stretchGlyphInFaces(state, fontGroup, ink.font, ink.size, codePoint, glyph,
                PrFontMathAxis_Horizontal, target, &glyphs);
            uint16_t wide = flattenedOf(combiningGlyph(ink.font, codePoint));
            if (wide != 0 && (!stretched || glyphs.info.advance * ink.size < target)) {
                Stretched other{};
                if (stretchGlyph(ink.font, ink.size, 0, wide, PrFontMathAxis_Horizontal, target, &other) &&
                        (!stretched || other.info.advance > glyphs.info.advance)) {
                    glyphs = other;
                    stretched = true;
                }
            }
            if (stretched) {
                // A glyph stretched along the inline axis keeps its own ink above and below its baseline.
                inkAscent = prLayoutUnitFromPx(glyphs.info.ascent * ink.size);
                inkDescent = prLayoutUnitFromPx(glyphs.info.descent * ink.size);
            }
        } else if (stretchy && !inlineAxis && stretch.axis == LayoutMathStretchAxis_Block) {
            BlockReach reach = blockReach(stretch, axis, flags, ink.size);
            PrLayoutUnit ascent = reach.ascent;
            PrLayoutUnit descent = reach.descent;
            PrLayoutUnit target = ascent + descent;
            // The size limits are lengths, or percentages of the glyph's unstretched height.
            PrLayoutUnit glyphHeight = inkAscent + inkDescent;
            PrLayoutUnit minSize =
                layoutUnitMax(kLayoutUnitZero, layoutResolve(state.styles, math.minSize, glyphHeight));
            bool bounded = !layoutLengthIsNone(math.maxSize);
            PrLayoutUnit maxSize = bounded ? layoutResolve(state.styles, math.maxSize, glyphHeight) : kLayoutUnitZero;
            if (bounded && maxSize < minSize)
                maxSize = minSize;
            auto scaled = [&](PrLayoutUnit size) {
                if (target <= kLayoutUnitZero) {
                    ascent = half(size) + axis;
                } else {
                    double above = double((ascent - axis).raw) * double(size.raw) / double(target.raw);
                    ascent = layoutUnitMax(kLayoutUnitZero, PrLayoutUnit{int32_t(above)} + axis);
                }
                descent = size - ascent;
                target = size;
            };
            if (target <= kLayoutUnitZero || target < minSize)
                scaled(minSize);
            else if (bounded && maxSize < target)
                scaled(maxSize);
            stretched = stretchGlyphInFaces(state, fontGroup, ink.font, ink.size, codePoint, glyph,
                PrFontMathAxis_Vertical, prLayoutUnitToPx(target), &glyphs);
            if (stretched) {
                PrLayoutUnit glyphAscent = prLayoutUnitFromPx(glyphs.info.ascent * ink.size);
                PrLayoutUnit glyphDescent = prLayoutUnitFromPx(glyphs.info.descent * ink.size);
                delta = half((glyphAscent - glyphDescent) - (ascent - descent));
                inkAscent = glyphAscent - delta;
                inkDescent = glyphDescent + delta;
            }
        } else if ((flags & PrLayoutMathOperatorFlag_LargeOp) != 0 &&
                (fontStyle.mathFlags & PrLayoutMathFlag_CompactStyle) == 0) {
            // A large operator in display style takes the first size at least DisplayOperatorMinHeight, and larger than
            // its text size, as TeX takes the successor of its glyph (TeXbook Appendix G, rule 13), centered on the
            // axis if it is symmetric.
            float text = prLayoutUnitToPx(inkAscent + inkDescent) + 1.0f / 64;
            stretched = stretchGlyph(ink.font, ink.size, codePoint, glyph, PrFontMathAxis_Vertical,
                prF32Max(c.displayOperatorMinHeight * ink.size, text), &glyphs);
            if (stretched) {
                PrLayoutUnit glyphAscent = prLayoutUnitFromPx(glyphs.info.ascent * ink.size);
                PrLayoutUnit glyphDescent = prLayoutUnitFromPx(glyphs.info.descent * ink.size);
                if (symmetric)
                    delta = half((glyphAscent - glyphDescent) - axis * 2);
                inkAscent = glyphAscent - delta;
                inkDescent = glyphDescent + delta;
            }
        }
    }
    // In a right-to-left formula, shaping has given a mirrored character its rtlm form or its mirror's glyph. An
    // operator whose font has neither, such as a summation or an integral sign, is drawn mirrored (drawnGlyph).
    bool mirrored = false;
    if (mode.direction == PrTextDirection_Rtl && math.kind == PrLayoutMathKind_Operator && ink.glyphs == 1 &&
            ink.font != nullptr) {
        uint32_t codePoint = firstCodePoint(tree, ink.textNode);
        mirrored = prUnicodeBidiIsMirrored(codePoint) && ink.glyph == prFontCodePointGlyph(ink.font, codePoint) &&
            drawnGlyph(ink.font, codePoint, true).mirrored;
    }
    bool substituted = !stretched && (glyph != ink.glyph || mirrored);
    if (substituted) {
        // A glyph that takes another form unstretched is drawn as that form, measured by its own ink.
        glyphs = unstretched(ink.font, glyph);
        inkAscent = prLayoutUnitFromPxCeil(glyphs.info.ascent * ink.size);
        inkDescent = prLayoutUnitFromPxCeil(glyphs.info.descent * ink.size);
        stretched = true;
    }
    if (stretched) {
        // The line gives way to the glyphs that the operator draws.
        layoutFragmentDiscard(children, input.mark);
        uint16_t first = glyphs.count != 0 ? glyphs.first : ink.glyph;
        float advance = glyphs.info.advance * ink.size;
        pushStretched(state, glyphs, ink.size, PrVec2{0, prLayoutUnitToPx(inkAscent + delta)},
            PrRect{0, 0, advance, prLayoutUnitToPx(inkAscent + inkDescent)}, mirrored);
        outcome.inlineSize = prLayoutUnitFromPx(advance);
        outcome.inlineSized = true;
        outcome.metrics.italic = prLayoutUnitFromPx(glyphs.info.italicsCorrection * ink.size);
        outcome.metrics.attachment = kLayoutMathNoAttachment;
        outcome.metrics.glyph = glyphs.count == 1 ? first : 0;
        outcome.metrics.glyphFont = glyphs.font;
        outcome.metrics.extended = glyphs.count != 1 || prFontMathGlyphIsExtendedShape(glyphs.font, first);
        outcome.metrics.inkAscent = inkAscent;
        outcome.metrics.inkDescent = inkDescent;
        if (substituted)
            outcome.metrics.attachment = prLayoutUnitFromPx(prFontMathGlyphTopAccentAttachment(ink.font, glyph) * ink.size);
    } else {
        // The line moves so that its baseline is the ink's top below the content box's block-start edge.
        PrLayoutUnit move = input.origin.blockOffset + inkAscent - baseline;
        for (uint32_t i = input.mark; i < children.count; ++i)
            children.data[i].offset.blockOffset += move;
    }
    outcome.baseline = inkAscent;
    outcome.blockSize = inkAscent + inkDescent;
    return outcome;
}

LayoutFragment* layoutMathFinish(LayoutState& state, LayoutFragment* built, const LayoutMathOutcome& outcome,
        LayoutLogicalOffset origin, PrLayoutUnit inlineSize, LayoutMode mode) {
    LayoutScratch& scratch = *state.scratch;
    float originX = prLayoutUnitToPx(origin.inlineOffset);
    float originY = prLayoutUnitToPx(origin.blockOffset);
    float width = prLayoutUnitToPx(inlineSize);
    bool reversed = mode.direction == PrTextDirection_Rtl;
    for (uint32_t i = outcome.paints; i < scratch.mathPaints.count; ++i) {
        LayoutMathPaint& p = scratch.mathPaints.data[i];
        PrRect r = p.rect;
        float left = r.left + originX;
        float right = r.right + originX;
        float dx = originX;
        if (reversed) {
            // The paint is mirrored along the inline axis. A stroke keeps its ends, and a box its corners, while its
            // glyphs move with it unmirrored.
            bool stroke = p.kind == LayoutMathPaintKind_Line || p.kind == LayoutMathPaintKind_Arrow;
            dx = width - right - r.left;
            float mirroredLeft = width - left;
            float mirroredRight = width - right;
            left = stroke ? mirroredLeft : mirroredRight;
            right = stroke ? mirroredRight : mirroredLeft;
        }
        p.rect = PrRect{left, r.top + originY, right, r.bottom + originY};
        if (p.kind == LayoutMathPaintKind_Glyphs) {
            for (uint32_t g = p.glyphStart; g < p.glyphStart + p.glyphCount; ++g) {
                PrVec2& at = scratch.mathGlyphs.data[g].position;
                at = PrVec2{at.x + dx, at.y + originY};
            }
            p.glyphStart -= outcome.glyphs;
        }
    }
    LayoutMathMetrics metrics = outcome.metrics;
    if (metrics.attachment != kLayoutMathNoAttachment)
        metrics.attachment += origin.inlineOffset;
    LayoutMathData data{
        .metrics = metrics,
        .paints = scratch.mathPaints.data + outcome.paints,
        .paintCount = scratch.mathPaints.count - outcome.paints,
        .glyphs = scratch.mathGlyphs.data + outcome.glyphs,
        .glyphCount = scratch.mathGlyphs.count - outcome.glyphs,
    };
    if (data.paintCount == 0)
        data.paints = nullptr;
    if (data.glyphCount == 0)
        data.glyphs = nullptr;
    LayoutFragment* fragment = layoutFragmentMath(built, data);
    // The lines of a token reach as far as their line boxes, past the ink that sizes the token's box. They add nothing
    // to the box's scrollable overflow, so that math content scrolls only as far as its boxes, which hold its ink.
    PrLayoutRect overflow = layoutRectAt(kLayoutVecZero, fragment->extent);
    for (uint32_t i = 0; i < fragment->childCount; ++i) {
        const LayoutChild& child = fragment->children[i];
        if (child.fragment->kind != LayoutFragmentKind_Line && layoutFragmentReachesOverflow(child.fragment))
            overflow = layoutRectUnion(overflow,
                layoutRectMoved(layoutPassedOverflow(state.tree, child.fragment), child.offset));
    }
    fragment->overflow = overflow;
    scratch.mathPaints.count = outcome.paints;
    scratch.mathGlyphs.count = outcome.glyphs;
    return fragment;
}

void layoutMathDiscard(LayoutState& state, const LayoutMathOutcome& outcome) {
    state.scratch->mathPaints.count = outcome.paints;
    state.scratch->mathGlyphs.count = outcome.glyphs;
}

LayoutIntrinsicSizes layoutMathIntrinsic(LayoutState& state, LayoutNodeIndex node, const LayoutIntrinsicInput& input) {
    LayoutOuterMode mode = layoutNodeOuterMode(state.tree, node);
    LayoutSpace content{
        .mode = mode.mode,
        .available = kMathAvailable,
        .percentInline = kLayoutUnitZero,
        .percentBlock = layoutUnit(-1),
        .containing = LayoutLogicalSize{kLayoutUnitZero, layoutUnit(-1)},
        .blockLimit = layoutUnit(-1),
        .fallback = input.fallback,
    };
    uint32_t mark = state.scratch->children.count;
    LayoutMathInput math{
        .node = node,
        .content = &content,
        .contentInline = layoutUnit(-1),
        .contentBlock = layoutUnit(-1),
        .percentInline = layoutUnit(-1),
        .mark = mark,
        .baseline = kLayoutBaselineNone,
    };
    LayoutMathOutcome outcome = layoutMathContent(state, math);
    layoutFragmentDiscard(state.scratch->children, mark);
    layoutMathDiscard(state, outcome);
    const PrLayoutBoxStyle& box = layoutBoxStyle(state.styles, layoutTreeNode(state.tree, node).style);
    PrLayoutUnit edges =
        layoutEdgesInline(layoutEdgesFromBox(state.styles, layoutLogicalBox(box, input.mode), layoutUnit(-1)));
    LayoutIntrinsicSizes sizes{};
    sizes.minContent = outcome.inlineSize + edges;
    sizes.maxContent = sizes.minContent;
    return sizes;
}

} // namespace pr_impl
