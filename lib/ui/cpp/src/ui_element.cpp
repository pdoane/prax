// Elements in C++: nodes of the layout tree created in immediate mode, and styled by chained calls.

#include "pr/ui_element.h"

#include "pr/alloc.h"
#include "pr/assert.h"
#include "pr/float.h"
#include "pr/layout_animation.h"
#include "pr/layout_query.h"
#include "pr/layout_unit.h"
#include "pr/mem.h"
#include "pr/path_data.h"
#include "pr/text_font.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr {

// What a font that the fonts made was made of.
PR_NO_PADDING_BEGIN
struct UiFontKey {
    float size;
    float weight;
    UiFamily family;
    uint8_t italic;
    uint8_t _pad;
};
PR_NO_PADDING_END

// The fonts' state behind their public face.
struct UiResources::State {
    // A font that the resources made, which they hold a reference to.
    struct Entry {
        UiFontKey key;
        PrTextFont* font;
    };

    PrFontSet* PR_NULLABLE set;
    RawArray<PrFontFamily> families;    // by UiFamily
    RawArray<Entry> made;
    PrPathCache* paths;
};

// The builder's state behind its public face.
struct UiBuilder::State {
    PrUi* ui;
    PrLayoutTree* tree;
    UiResources::State* PR_NULLABLE fonts;
    UiResources* PR_NULLABLE resources;
    UiFontKey defaultKey;
    PrLayoutGroupIndex defaultFont;
    PrLayoutGroupIndex defaultColor;

    // The style of an element that the round closed: its node and kind, and its ops in `closedOps`, header aside.
    struct Closed {
        PrLayoutNode node;          // none once the element is open again
        UiKind kind;
        uint32_t first;
        uint32_t count;
    };

    // The open element, whose style its chain writes, and its style, header first. `ops` is empty while no element is
    // open.
    PrLayoutNode open;
    UiKind openKind;
    RawArray<UiOp> ops;

    // The styles of the elements that the round closed, which an element opened again starts from. The build's end
    // empties them.
    RawArray<Closed> closed;
    RawArray<UiOp> closedOps;

    // Scratch for the parts of the path element that the builder is creating. The layout tree copies the parts when the
    // builder sets the element's content, so the array is reused for the next path.
    RawArray<PrLayoutPathPart> pathParts;

    // The builder, the roots of the memos being built, innermost last, and the root of the memo that it built last.
    UiBuilder* self;
    RawArray<PrLayoutNode> memos;
    PrLayoutNode built;
};

namespace {

// The identity of an element's style in the context's record cache, and the owner of the tweens of its transitions.
constexpr char kStyleType = 0;

// What an element inherits from the node that it is created under: its text's font and color groups.
struct Inherited {
    PrLayoutGroupIndex font;
    PrLayoutGroupIndex color;
};

// The identity of the builder's state on a memo's root.
constexpr char kMemoOwner = 0;

// What the builder keeps on a memo's root: the ops of the root's style that its last build gave it, which a call on a
// kept root starts from.
struct MemoRoot {
    UiKind kind;
    uint32_t opCount;
    UiOp* PR_NULLABLE ops;
};

void destroyMemoRoot(void* data) {
    prMemoryFree(static_cast<MemoRoot*>(data)->ops);
}

MemoRoot& memoRootOf(UiBuilder::State& b, PrLayoutNode node) {
    void* data = prUiStateWithDestroy(b.ui, node, &kMemoOwner, 0, uint32_t(sizeof(MemoRoot)), destroyMemoRoot);
    return *static_cast<MemoRoot*>(data);
}

void setLengths(PrLayoutLength* lengths, uint16_t sides, PrLayoutLength value) {
    for (uint32_t side = 0; side < PrRectSide_Count; ++side) {
        if (sides & (1u << side))
            lengths[side] = value;
    }
}

PrLayoutLength lengthOf(const UiOp& op) {
    return PrLayoutLength{op.a};
}
float floatOf(const UiOp& op) {
    return bitCast<float>(op.a);
}
float secondFloatOf(const UiOp& op) {
    return bitCast<float>(op.b);
}
PrColor16 colorOf(const UiOp& op) {
    return bitCast<PrColor16>(uint64_t(op.a) | (uint64_t(op.b) << 32));
}
void setColor(UiOp& op, PrColor16 color) {
    uint64_t packed = bitCast<uint64_t>(color);
    op.a = uint32_t(packed);
    op.b = uint32_t(packed >> 32);
}

// Returns the matrix of one step of a transform list.
PrMatrix4 transformStep(const UiOp& op) {
    constexpr float kRadians = 3.14159265358979f / 180;
    float value = floatOf(op);
    switch (op.code) {
        case UiOpCode_RotateX: return prMatrix4Rotate(PrVec3{1, 0, 0}, value * kRadians);
        case UiOpCode_RotateY: return prMatrix4Rotate(PrVec3{0, 1, 0}, value * kRadians);
        case UiOpCode_RotateZ: return prMatrix4Rotate(PrVec3{0, 0, 1}, value * kRadians);
        case UiOpCode_TranslateX: return prMatrix4Translate(PrVec3{value, 0, 0});
        case UiOpCode_TranslateY: return prMatrix4Translate(PrVec3{0, value, 0});
        case UiOpCode_TranslateZ: return prMatrix4Translate(PrVec3{0, 0, value});
        default: return prMatrix4Scale(PrVec3{value, value, 1});
    }
}

// Returns the font that `key` names, which the builder's fonts make on its first use, or null with no fonts.
PrTextFont* PR_NULLABLE fontFor(UiBuilder::State& b, const UiFontKey& key) {
    UiResources::State* fonts = b.fonts;
    if (fonts == nullptr || fonts->set == nullptr)
        return nullptr;
    for (uint32_t i = 0; i < fonts->made.count; ++i) {
        if (prMemEquals(&fonts->made.data[i].key, &key, sizeof key))
            return fonts->made.data[i].font;
    }
    PrFontStyle style = prFontStyleDefault();
    style.size = key.size;
    style.weight = key.weight;
    if (key.italic)
        style.slope = PrFontSlope_Italic;
    const PrFontFamily& family = fonts->families.data[key.family < fonts->families.count ? key.family : 0];
    PrTextFontDesc desc{.set = fonts->set, .families = &family, .familyCount = 1, .style = style};
    PrTextFont* made = prTextFontCreate(&desc);
    fonts->made.push(UiResources::State::Entry{.key = key, .font = made});
    return made;
}

// Returns what `font` was made of, or the builder's default if its fonts did not make it.
const UiFontKey& keyFor(const UiBuilder::State& b, PrTextFont* PR_NULLABLE font) {
    for (uint32_t i = 0; b.fonts != nullptr && i < b.fonts->made.count; ++i) {
        if (b.fonts->made.data[i].font == font)
            return b.fonts->made.data[i].key;
    }
    return b.defaultKey;
}

// Returns what an element created under `node` inherits: the groups of the node's record, which may be another
// library's, or the builder's defaults at the root and for a group that the node leaves at layout's default.
Inherited inheritedFrom(const UiBuilder::State& b, PrLayoutNode node) {
    if (node == kPrLayoutNodeNone)
        return Inherited{.font = b.defaultFont, .color = b.defaultColor};
    PrLayoutStyle record = prLayoutStyleRecord(b.tree, prLayoutTreeFrameStyle(b.tree, node));
    PrLayoutGroupIndex font = record.groups[PrLayoutGroup_Font];
    PrLayoutGroupIndex color = record.groups[PrLayoutGroup_Color];
    return Inherited{.font = font != 0 ? font : b.defaultFont, .color = color != 0 ? color : b.defaultColor};
}

// Returns true if an op's value moves over a transition rather than at once.
bool transitions(UiOpCode code) {
    switch (code) {
        case UiOpCode_Background:
        case UiOpCode_BorderColor:
        case UiOpCode_TextColor:
        case UiOpCode_ShadowColor:
        case UiOpCode_GradientFrom:
        case UiOpCode_GradientTo:
        case UiOpCode_OutlineColor:
        case UiOpCode_Opacity:
        case UiOpCode_RotateX:
        case UiOpCode_RotateY:
        case UiOpCode_RotateZ:
        case UiOpCode_TranslateX:
        case UiOpCode_TranslateY:
        case UiOpCode_TranslateZ:
        case UiOpCode_Scale:
            return true;
        default:
            return false;
    }
}

// Moves each value of the open element's style that transitions toward its target over `seconds`, in place. Each
// value's tween is keyed by its op's place among the style's ops of its code, so that a style with the same ops in each
// round tweens each value on its own.
void tweenOps(UiBuilder::State& b, float seconds) {
    // Tailwind's default easing, cubic-bezier(0.4, 0, 0.2, 1).
    PrLayoutEasing easing{.x1 = 0.4, .y1 = 0, .x2 = 0.2, .y2 = 1, .kind = PrLayoutEasingKind_CubicBezier};
    uint8_t seen[UiOpCode_Count] = {};
    for (uint32_t i = 1; i < b.ops.count; ++i) {
        UiOp& op = b.ops.data[i];
        if (!transitions(op.code))
            continue;
        uint64_t key = (uint64_t(op.code) << 16) | (uint64_t(seen[op.code]++) << 4);
        bool color = op.code != UiOpCode_Opacity && op.code < UiOpCode_RotateX;
        if (color) {
            PrColor value = prColorFromColor16(colorOf(op));
            float* channels[4] = {&value.r, &value.g, &value.b, &value.a};
            for (uint32_t c = 0; c < 4; ++c)
                *channels[c] = prUiTween(b.ui, b.open, &kStyleType, key | c, *channels[c], double(seconds), &easing);
            setColor(op, prColor16FromColor(value));
        } else {
            op.a = bitCast<uint32_t>(prUiTween(b.ui, b.open, &kStyleType, key, floatOf(op), double(seconds), &easing));
        }
    }
}

// Returns the record of the open element's style under `inherited`: the memo's, if the last frame made the same style
// at this place in the round, or one made from the style's ops.
PrLayoutStyleIndex recordOf(UiBuilder::State& b, const Inherited& inherited) {
    b.ops.data[0] = UiOp{
        .code = UiOpCode_Header,
        .aux = uint16_t(b.openKind | b.ops.count << kUiHeaderKindBits),
        .a = inherited.font,
        .b = inherited.color,
    };
    const UiOp* ops = b.ops.data;
    uint32_t count = b.ops.count;
    uint32_t bytes = uint32_t(sizeof(UiOp) * count);
    PrLayoutStyleIndex kept = 0;
    // The kept style is as long as its header says, so the rest compares once the headers match.
    const void* found = prUiRecordFind(b.ui, &kStyleType, &b, &kept);
    if (found != nullptr && prMemEquals(found, ops, sizeof(UiOp)) && prMemEquals(found, ops, bytes))
        return kept;

    PrLayoutBoxStyle box = prLayoutBoxStyleDefault();
    box.display = b.openKind == UiKind_Span ? PrLayoutDisplay_Inline : PrLayoutDisplay_Block;
    box.boxSizing = PrLayoutBoxSizing_BorderBox;
    PrLayoutFlexStyle flex = prLayoutFlexStyleDefault();
    PrLayoutGridStyle grid = prLayoutGridStyleDefault();
    PrLayoutDecorationStyle decoration = prLayoutDecorationStyleDefault();
    PrLayoutEffectStyle effect = prLayoutEffectStyleDefault();
    PrLayoutCompositingStyle compositing = prLayoutCompositingStyleDefault();
    PrLayoutFontStyle font{};
    PrLayoutColorStyle color{};
    UiFontKey fontKey{};
    bool fontRead = false;
    bool fontKeyChanged = false;
    bool colorRead = false;
    bool flexSet = false;
    bool gridSet = false;
    bool decorationSet = false;
    bool effectSet = false;
    bool compositingSet = false;
    bool borderColorSet = false;
    // What the style sets that lives in the tree's lists, which the record interns once the ops are read.
    constexpr uint32_t kMaxShadows = 8;
    PrLayoutShadow shadows[kMaxShadows];
    uint32_t shadowCount = 0;
    PrLayoutGradientKind gradientKind = PrLayoutGradientKind_Linear;
    float gradientAngle = 180;
    PrColor16 gradientColors[2] = {};
    bool gradientSet = false;
    PrDrawBorderStyle borderStyle = PrDrawBorderStyle_Solid;
    uint32_t columns = 0;
    uint32_t rows = 0;
    float blurPx = 0;
    float backdropPx = 0;
    // The inherited text groups are read only if the style sets a text property.
    auto touchFont = [&] {
        if (!fontRead) {
            font = prLayoutFontStyleRecord(b.tree, inherited.font);
            fontKey = keyFor(b, font.font);
            fontRead = true;
        }
    };
    auto touchColor = [&] {
        if (!colorRead) {
            color = prLayoutColorStyleRecord(b.tree, inherited.color);
            colorRead = true;
        }
    };

    for (uint32_t i = 1; i < count; ++i) {
        const UiOp& op = ops[i];
        switch (op.code) {
            case UiOpCode_Display: box.display = PrLayoutDisplay(op.a); break;
            case UiOpCode_Position: box.position = PrLayoutPosition(op.a); break;
            case UiOpCode_FlexDirection: flex.direction = PrLayoutFlexDirection(op.a); flexSet = true; break;
            case UiOpCode_FlexWrap: flex.wrap = PrLayoutFlexWrap(op.a); flexSet = true; break;
            case UiOpCode_Grow: flex.grow = floatOf(op); flexSet = true; break;
            case UiOpCode_Shrink: flex.shrink = floatOf(op); flexSet = true; break;
            case UiOpCode_Basis: flex.basis = lengthOf(op); flexSet = true; break;
            case UiOpCode_AlignItems: flex.alignItems = PrLayoutAlignment(op.a); flexSet = true; break;
            case UiOpCode_AlignSelf: flex.alignSelf = PrLayoutAlignment(op.a); flexSet = true; break;
            case UiOpCode_AlignContent: flex.alignContent = PrLayoutAlignment(op.a); flexSet = true; break;
            case UiOpCode_JustifyContent: flex.justifyContent = PrLayoutAlignment(op.a); flexSet = true; break;
            case UiOpCode_RowGap: flex.rowGap = lengthOf(op); flexSet = true; break;
            case UiOpCode_ColumnGap: flex.columnGap = lengthOf(op); flexSet = true; break;
            case UiOpCode_GridColumns: columns = op.a; gridSet = true; break;
            case UiOpCode_GridRows: rows = op.a; gridSet = true; break;
            case UiOpCode_ColumnSpan:
                grid.columnEnd = PrLayoutGridLine{.integer = int16_t(op.a), .kind = PrLayoutGridLineKind_Span};
                gridSet = true;
                break;
            case UiOpCode_RowSpan:
                grid.rowEnd = PrLayoutGridLine{.integer = int16_t(op.a), .kind = PrLayoutGridLineKind_Span};
                gridSet = true;
                break;
            case UiOpCode_Width: box.width = lengthOf(op); break;
            case UiOpCode_Height: box.height = lengthOf(op); break;
            case UiOpCode_MinWidth: box.minWidth = lengthOf(op); break;
            case UiOpCode_MinHeight: box.minHeight = lengthOf(op); break;
            case UiOpCode_MaxWidth: box.maxWidth = lengthOf(op); break;
            case UiOpCode_MaxHeight: box.maxHeight = lengthOf(op); break;
            case UiOpCode_AspectRatio: box.aspectRatio = floatOf(op); break;
            case UiOpCode_Padding: setLengths(box.padding, op.aux, lengthOf(op)); break;
            case UiOpCode_Margin: setLengths(box.margin, op.aux, lengthOf(op)); break;
            case UiOpCode_Inset: setLengths(box.inset, op.aux, lengthOf(op)); break;
            case UiOpCode_BorderWidth: {
                float width = floatOf(op);
                for (uint32_t side = 0; side < PrRectSide_Count; ++side) {
                    if (op.aux & (1u << side)) {
                        box.border[side] = prLayoutUnitFromPx(width);
                        decoration.borderStyle[side] = width > 0 ? PrDrawBorderStyle_Solid : PrDrawBorderStyle_None;
                    }
                }
                decorationSet = true;
                break;
            }
            case UiOpCode_BorderColor:
                for (uint32_t side = 0; side < PrRectSide_Count; ++side) {
                    if (op.aux & (1u << side))
                        decoration.borderColor[side] = colorOf(op);
                }
                borderColorSet = true;
                decorationSet = true;
                break;
            case UiOpCode_BorderStyle: borderStyle = PrDrawBorderStyle(op.a); break;
            case UiOpCode_Radius:
                for (uint32_t corner = 0; corner < PrRectCorner_Count; ++corner) {
                    if (op.aux & (1u << corner)) {
                        decoration.radii[corner * 2] = prLayoutLengthPx(floatOf(op));
                        decoration.radii[corner * 2 + 1] = prLayoutLengthPx(floatOf(op));
                    }
                }
                decorationSet = true;
                break;
            case UiOpCode_Background: decoration.background = colorOf(op); decorationSet = true; break;
            case UiOpCode_GradientKind:
                gradientKind = PrLayoutGradientKind(op.aux);
                gradientAngle = floatOf(op);
                gradientSet = true;
                decorationSet = true;
                break;
            case UiOpCode_GradientFrom: gradientColors[0] = colorOf(op); break;
            case UiOpCode_GradientTo: gradientColors[1] = colorOf(op); break;
            case UiOpCode_ShadowOffset:
                if (shadowCount < kMaxShadows) {
                    shadows[shadowCount++] = PrLayoutShadow{
                        .offset = PrVec2{floatOf(op), secondFloatOf(op)},
                        .inset = op.aux != 0,
                    };
                }
                decorationSet = true;
                break;
            case UiOpCode_ShadowBlur:
                if (shadowCount != 0) {
                    shadows[shadowCount - 1].blur = floatOf(op);
                    shadows[shadowCount - 1].spread = secondFloatOf(op);
                }
                break;
            case UiOpCode_ShadowColor:
                if (shadowCount != 0)
                    shadows[shadowCount - 1].color = colorOf(op);
                break;
            case UiOpCode_OutlineWidth:
                decoration.outlineWidth = prLayoutUnitFromPx(floatOf(op));
                decoration.outlineOffset = prLayoutUnitFromPx(secondFloatOf(op));
                decoration.outlineStyle = PrDrawBorderStyle_Solid;
                decorationSet = true;
                break;
            case UiOpCode_OutlineColor: decoration.outlineColor = colorOf(op); break;
            case UiOpCode_ObjectFit: decoration.objectFit = PrLayoutObjectFit(op.a); decorationSet = true; break;
            case UiOpCode_ObjectPosition:
                decoration.objectPositionX = PrLayoutLength{op.a};
                decoration.objectPositionY = PrLayoutLength{op.b};
                decorationSet = true;
                break;
            case UiOpCode_OverflowX: box.overflowX = PrLayoutOverflow(op.a); break;
            case UiOpCode_OverflowY: box.overflowY = PrLayoutOverflow(op.a); break;
            case UiOpCode_Opacity: effect.opacity = floatOf(op); effectSet = true; break;
            case UiOpCode_ZIndex:
                box.zIndexed = 1;
                effect.zIndex = int32_t(op.a);
                effectSet = true;
                break;
            case UiOpCode_Blur: blurPx = floatOf(op); break;
            case UiOpCode_BackdropBlur: backdropPx = floatOf(op); break;
            case UiOpCode_RotateX:
            case UiOpCode_RotateY:
            case UiOpCode_RotateZ:
            case UiOpCode_TranslateX:
            case UiOpCode_TranslateY:
            case UiOpCode_TranslateZ:
            case UiOpCode_Scale: {
                // A later step of the list applies to the element first, as CSS composes a transform list.
                PrMatrix4 step = transformStep(op);
                effect.transform.matrix = prMatrix4Concat(&step, &effect.transform.matrix);
                box.transformed = 1;
                effectSet = true;
                break;
            }
            case UiOpCode_Perspective:
                effect.perspective = floatOf(op);
                box.transformed = 1;
                effectSet = true;
                break;
            case UiOpCode_BackfaceHidden: effect.backfaceHidden = op.a != 0; effectSet = true; break;
            case UiOpCode_Visibility: touchColor(); color.visibility = PrLayoutVisibility(op.a); break;
            case UiOpCode_PointerEvents: touchColor(); color.pointerEvents = PrLayoutPointerEvents(op.a); break;
            case UiOpCode_BoxSizing: box.boxSizing = PrLayoutBoxSizing(op.a); break;
            case UiOpCode_TextColor: touchColor(); color.color = colorOf(op); break;
            case UiOpCode_FontFamily: touchFont(); fontKey.family = UiFamily(op.a); fontKeyChanged = true; break;
            case UiOpCode_FontSize: touchFont(); fontKey.size = floatOf(op); fontKeyChanged = true; break;
            case UiOpCode_FontWeight: touchFont(); fontKey.weight = floatOf(op); fontKeyChanged = true; break;
            case UiOpCode_FontItalic: touchFont(); fontKey.italic = uint8_t(op.a); fontKeyChanged = true; break;
            case UiOpCode_LineHeight: touchFont(); font.lineHeight = prLayoutLineHeightPx(floatOf(op)); break;
            case UiOpCode_TextAlign: touchFont(); font.textAlign = PrLayoutTextAlign(op.a); break;
            case UiOpCode_TextWrap: touchFont(); font.textWrapMode = PrLayoutTextWrapMode(op.a); break;
            case UiOpCode_WhiteSpace: touchFont(); font.whiteSpaceCollapse = PrLayoutWhiteSpaceCollapse(op.a); break;
            case UiOpCode_LetterSpacing: touchFont(); font.letterSpacing = floatOf(op); break;
            case UiOpCode_TextTransform: touchFont(); font.textTransform = PrTextTransform(op.a); break;
            case UiOpCode_Transition: break;
            default: PR_ASSERT(false); break;
        }
    }

    // A border with no color of its own takes the text's, as CSS's currentcolor does.
    if (decorationSet && !borderColorSet) {
        PrColor16 text = colorRead ? color.color : prLayoutColorStyleRecord(b.tree, inherited.color).color;
        for (PrColor16& side : decoration.borderColor)
            side = text;
    }
    if (borderStyle != PrDrawBorderStyle_Solid) {
        for (uint32_t side = 0; side < PrRectSide_Count; ++side) {
            if (decoration.borderStyle[side] == PrDrawBorderStyle_Solid)
                decoration.borderStyle[side] = borderStyle;
        }
    }
    if (shadowCount != 0)
        decoration.shadows = prLayoutShadowIntern(b.tree, shadows, shadowCount);
    if (gradientSet) {
        PrLayoutColorStop stops[2] = {
            PrLayoutColorStop{.position = prLayoutLengthAuto(), .color = gradientColors[0]},
            PrLayoutColorStop{.position = prLayoutLengthAuto(), .color = gradientColors[1]},
        };
        PrLayoutBackgroundLayer layer = prLayoutBackgroundLayerDefault();
        layer.kind = PrLayoutImageKind_Gradient;
        layer.clip = PrLayoutBackgroundBox_BorderBox;
        layer.origin = PrLayoutBackgroundBox_BorderBox;
        layer.gradient = prLayoutGradientDefault();
        layer.gradient.kind = gradientKind;
        layer.gradient.angle = gradientAngle;
        layer.gradient.stopCount = 2;
        layer.gradient.stops = stops;
        decoration.backgroundLayers = prLayoutBackgroundIntern(b.tree, &layer, 1);
    }
    // A track list of equal shares, each of which may shrink below its content: repeat(n, minmax(0, 1fr)).
    auto shares = [&](uint32_t n) {
        PrLayoutGridTrack track{.min = prLayoutTrackBreadthPx(0), .max = prLayoutTrackBreadthFlex(1)};
        PrLayoutGridTrack tracks[64];
        n = n < 64 ? n : 64;
        for (uint32_t i = 0; i < n; ++i)
            tracks[i] = track;
        PrLayoutGridTrackList list{.tracks = tracks, .trackCount = n};
        return prLayoutGridTracksIntern(b.tree, &list);
    };
    if (columns != 0)
        grid.templateColumns = shares(columns);
    if (rows != 0)
        grid.templateRows = shares(rows);
    if (blurPx > 0) {
        PrLayoutFilter filter{.kind = PrLayoutFilterKind_Blur, .amount = blurPx};
        compositing.filter = prLayoutFilterIntern(b.tree, &filter, 1);
        box.filtered = 1;
        compositingSet = true;
    }
    if (backdropPx > 0) {
        PrLayoutFilter filter{.kind = PrLayoutFilterKind_Blur, .amount = backdropPx};
        compositing.backdrop = prLayoutFilterIntern(b.tree, &filter, 1);
        box.filtered = 1;
        compositingSet = true;
    }

    PrLayoutStyle record = prLayoutStyleDefault();
    record.groups[PrLayoutGroup_Box] = prLayoutBoxStyleIntern(b.tree, &box);
    if (flexSet)
        record.groups[PrLayoutGroup_Flex] = prLayoutFlexStyleIntern(b.tree, &flex);
    if (gridSet)
        record.groups[PrLayoutGroup_Grid] = prLayoutGridStyleIntern(b.tree, &grid);
    if (decorationSet)
        record.groups[PrLayoutGroup_Decoration] = prLayoutDecorationStyleIntern(b.tree, &decoration);
    if (effectSet)
        record.groups[PrLayoutGroup_Effect] = prLayoutEffectStyleIntern(b.tree, &effect);
    if (compositingSet)
        record.groups[PrLayoutGroup_Compositing] = prLayoutCompositingStyleIntern(b.tree, &compositing);
    if (fontRead) {
        if (fontKeyChanged)
            font.font = fontFor(b, fontKey);
        record.groups[PrLayoutGroup_Font] = prLayoutFontStyleIntern(b.tree, &font);
    } else {
        record.groups[PrLayoutGroup_Font] = inherited.font;
    }
    record.groups[PrLayoutGroup_Color] = colorRead ? prLayoutColorStyleIntern(b.tree, &color) : inherited.color;
    PrUiPart part{.type = &kStyleType, .owner = &b, .bytes = ops, .size = bytes};
    return prUiRecordStore(b.ui, &part, prLayoutStyleIntern(b.tree, &record), 0);
}

// Gives the open element its record, and closes it, keeping its style for the round.
void closeOpen(UiBuilder::State& b) {
    if (b.ops.count == 0)
        return;
    b.closed.push(UiBuilder::State::Closed{
        .node = b.open,
        .kind = b.openKind,
        .first = b.closedOps.count,
        .count = b.ops.count - 1,
    });
    b.closedOps.append(b.ops.data + 1, b.ops.count - 1);
    for (uint32_t i = 1; i < b.ops.count; ++i) {
        if (b.ops.data[i].code == UiOpCode_Transition) {
            tweenOps(b, floatOf(b.ops.data[i]));
            break;
        }
    }
    PrLayoutStyleIndex record = recordOf(b, inheritedFrom(b, prLayoutTreeFrameParent(b.tree, b.open)));
    prLayoutTreeFrameSetStyle(b.tree, b.open, record);
    b.open = kPrLayoutNodeNone;
    b.ops.clear();
}

// Closes the round's last element, and lets go of the round's styles.
void closeDeferred(void* PR_NULLABLE context, PrUi*) {
    UiBuilder::State& b = *static_cast<UiBuilder::State*>(context);
    closeOpen(b);
    b.closed.clear();
    b.closedOps.clear();
}

// Closes the open element, and opens `node` again, an element that the round closed, with the style that it had.
void reopen(UiBuilder::State& b, PrLayoutNode node) {
    closeOpen(b);
    for (uint32_t i = b.closed.count; i-- > 0;) {
        UiBuilder::State::Closed& c = b.closed.data[i];
        if (c.node != node)
            continue;
        c.node = kPrLayoutNodeNone;
        b.open = node;
        b.openKind = c.kind;
        b.ops.push(UiOp{});
        b.ops.append(b.closedOps.data + c.first, c.count);
        prUiDefer(b.ui, closeDeferred, &b);
        return;
    }
    PR_ASSERT(false);
}

// Closes the open element, and creates an element of `kind` under `parent`, which the build's end closes if nothing
// else does first.
UiElement openElement(UiBuilder& builder, UiBuilder::State& b, PrLayoutNode parent, PrUiKey key, UiKind kind) {
    closeOpen(b);
    PrLayoutNode node = prUiElement(b.ui, parent, key, 0, 0);
    b.open = node;
    b.openKind = kind;
    b.ops.push(UiOp{});
    prUiDefer(b.ui, closeDeferred, &b);
    return UiElement(&builder, node);
}

// Returns the ops of the style that the round gave `node`, header aside, writing their number to `count`: the open
// element's, or a closed element's. A node that has neither has none.
const UiOp* PR_NULLABLE opsGiven(const UiBuilder::State& b, PrLayoutNode node, uint32_t* count) {
    if (b.ops.count != 0 && b.open == node) {
        *count = b.ops.count - 1;
        return b.ops.data + 1;
    }
    for (uint32_t i = b.closed.count; i-- > 0;) {
        const UiBuilder::State::Closed& c = b.closed.data[i];
        if (c.node == node) {
            *count = c.count;
            return b.closedOps.data + c.first;
        }
    }
    *count = 0;
    return nullptr;
}

// Opens `node`, a memo's root that the round created, with no style, so that its build styles it.
void openRoot(UiBuilder::State& b, PrLayoutNode node, UiKind kind) {
    closeOpen(b);
    b.open = node;
    b.openKind = kind;
    b.ops.push(UiOp{});
    prUiDefer(b.ui, closeDeferred, &b);
}

// What a kept memo's root keeps while a round keeps its subtree: its record, which the keep gave it, and the ops of its
// style, which a call on the root opens it again with.
void keepRoot(UiBuilder::State& b, PrLayoutNode node, const MemoRoot& memo) {
    b.closed.push(UiBuilder::State::Closed{
        .node = node,
        .kind = memo.kind,
        .first = b.closedOps.count,
        .count = memo.opCount,
    });
    b.closedOps.append(memo.ops, memo.opCount);
    prUiDefer(b.ui, closeDeferred, &b);
}

} // namespace

PrUi* UiElement::ui() const {
    return builder_->ui();
}

UiElement& UiElement::text(PrStringView text) {
    prLayoutTreeFrameText(builder_->tree(), node_, 0, text);
    return *this;
}

bool UiElement::edit(const UiEdit& edit, PrStringView* changed) {
    addFlags(PrUiFlag_Focusable | PrUiFlag_Pressable);
    // The text keeps its spaces, as the text of an input does. The space that the edit shows after empty text then
    // holds the line of the caret, and an edit of one line does not wrap.
    whitespacePre();
    if ((edit.flags & PrUiTextEditFlag_Multiline) == 0)
        whitespaceNowrap();
    if (!prUiIsDisabled(ui(), node_))
        cursor(PrAppCursor_Text);
    PrUiTextEditDesc desc{
        .control = node_,
        .parent = node_,
        .value = edit.value,
        .flags = edit.flags,
        .inputKind = edit.kind,
        .selection = prColor16FromColor(edit.selection),
        .caret = prColor16FromColor(edit.caret),
    };
    prUiTextEdit(ui(), &desc);
    return prUiTextEditChanged(ui(), node_, changed);
}

bool UiElement::hoveredFor(double seconds) const {
    PrUi* context = ui();
    if (!prUiIsHovered(context, node_))
        return false;
    double rested = prUiHoverSeconds(context, node_);
    if (rested >= seconds)
        return true;
    prUiRequestFrameAt(context, prUiCurrentRound(context)->time + (seconds - rested));
    return false;
}

bool UiElement::dismissed(PrLayoutNode from) {
    PrUi* context = ui();
    bool escape = prUiKeyPressed(context, node_, PrAppKey_Escape, 0);
    if (from != kPrLayoutNodeNone)
        escape = prUiKeyPressed(context, from, PrAppKey_Escape, 0) || escape;
    bool outside = false;
    for (uint8_t button = 0; button < 3 && !outside; ++button) {
        outside = prUiPressedOutside(context, node_, button) &&
            (from == kPrLayoutNodeNone || prUiPressedOutside(context, from, button));
    }
    return escape || outside;
}

UiElement& UiElement::overflow(PrLayoutOverflow x, PrLayoutOverflow y) {
    if (x == PrLayoutOverflow_Scroll || y == PrLayoutOverflow_Scroll)
        addFlags(PrUiFlag_Scrollable);
    set(uiOpValue(UiOpCode_OverflowX, x));
    return set(uiOpValue(UiOpCode_OverflowY, y));
}

UiBuilder::UiBuilder(const UiBuilderDesc& desc) : state_(allocZeroed<State>()) {
    State* b = state_;
    b->self = this;
    b->ui = desc.ui;
    b->tree = prUiTree(desc.ui);
    b->fonts = desc.resources != nullptr ? desc.resources->state_ : nullptr;
    b->resources = desc.resources;
    b->defaultKey = UiFontKey{.size = desc.textSize, .weight = desc.fontWeight, .family = UiFamily_Sans};
    b->open = kPrLayoutNodeNone;
    PrLayoutFontStyle font = prLayoutFontStyleDefault();
    font.font = fontFor(*b, b->defaultKey);
    b->defaultFont = prLayoutFontStyleIntern(b->tree, &font);
    prLayoutGroupRetain(b->tree, PrLayoutGroup_Font, b->defaultFont);
    PrLayoutColorStyle color = prLayoutColorStyleDefault();
    color.color = prColor16FromColor(desc.textColor);
    b->defaultColor = prLayoutColorStyleIntern(b->tree, &color);
    prLayoutGroupRetain(b->tree, PrLayoutGroup_Color, b->defaultColor);
}

UiBuilder::~UiBuilder() {
    State* b = state_;
    PR_ASSERT(b->ops.count == 0);
    prUiForget(b->ui, b);
    prLayoutGroupRelease(b->tree, PrLayoutGroup_Font, b->defaultFont);
    prLayoutGroupRelease(b->tree, PrLayoutGroup_Color, b->defaultColor);
    b->ops.reset();
    b->closed.reset();
    b->closedOps.reset();
    b->memos.reset();
    b->pathParts.reset();
    prMemoryFree(b);
}

UiElement UiBuilder::div(PrLayoutNode parent, PrUiKey key) {
    return openElement(*this, *state_, parent, key, UiKind_Div);
}

UiElement UiBuilder::span(PrLayoutNode parent, PrUiKey key) {
    return openElement(*this, *state_, parent, key, UiKind_Span);
}

UiElement UiBuilder::replaced(PrLayoutNode parent, PrUiKey key) {
    State& b = *state_;
    closeOpen(b);
    PrLayoutNode node = prLayoutTreeFrameReplaced(b.tree, parent, prUiLayoutKey(b.ui, parent, key), 0);
    openRoot(b, node, UiKind_Div);
    return UiElement(this, node);
}

UiElement UiBuilder::path(PrLayoutNode parent, const UiPath& path, PrUiKey key) {
    UiElement element = replaced(parent, key);
    PrLayoutTree* tree = state_->tree;
    PrLayoutNaturalSize natural{
        .width = prLayoutUnitFromPx(path.viewBox.x),
        .height = prLayoutUnitFromPx(path.viewBox.y),
        .ratio = path.viewBox.y > 0 ? path.viewBox.x / path.viewBox.y : 0,
    };
    prLayoutTreeFrameSetNaturalSize(tree, element, natural);
    UiResources* resources = state_->resources;
    PR_ASSERT(resources != nullptr);
    RawArray<PrLayoutPathPart>& parts = state_->pathParts;
    parts.clear();
    for (const UiPathPart& part : path.parts)
        parts.push(PrLayoutPathPart{.path = resources->path(part.data), .strokeWidth = part.strokeWidth});
    PrLayoutContent content{};
    content.kind = PrLayoutContentKind_Path;
    content.path = PrLayoutPath{
        .parts = parts.data,
        .partCount = parts.count,
        .viewBox = path.viewBox,
        .color = prColor16FromColor(path.color),
        .rule = path.rule,
        .join = path.join,
        .cap = path.cap,
    };
    prLayoutTreeFrameSetContent(tree, element, &content);
    return element;
}

UiElement UiBuilder::image(PrLayoutNode parent, const UiImage& image, PrUiKey key) {
    UiElement element = replaced(parent, key);
    PrLayoutTree* tree = state_->tree;
    const PrTextureDesc* desc = prTextureSourceDesc(image.source);
    PrUVec2 extent = image.upright ? prTextureDescDisplayedExtent(desc) : desc->extent;
    PrLayoutNaturalSize natural{
        .width = prLayoutUnitFromPx(float(extent.x)),
        .height = prLayoutUnitFromPx(float(extent.y)),
        .ratio = extent.y != 0 ? float(extent.x) / float(extent.y) : 0,
    };
    prLayoutTreeFrameSetNaturalSize(tree, element, natural);
    PrLayoutContent content{};
    content.kind = PrLayoutContentKind_Image;
    content.image = image.source;
    content.limit = image.limit;
    content.rendering = image.rendering;
    content.priority = image.priority;
    content.upright = image.upright;
    prLayoutTreeFrameSetContent(tree, element, &content);
    return element;
}

UiElement UiBuilder::popup(PrLayoutNode from, PrUiKey key, const PrAppPopupPlacement& placement) {
    State& b = *state_;
    closeOpen(b);
    PrUiPortalDesc desc{.kind = PrUiPortalKind_Popup, .parent = prUiNodeViewport(b.ui, from), .placement = placement};
    PrLayoutNode portal = prUiPortal(b.ui, key, &desc, 0);
    UiElement content = openElement(*this, b, portal, 0, UiKind_Div);
    // The popup takes focus when it opens. The keys that it claims, such as Escape, then reach it whatever had focus
    // before.
    content.pressFocus();
    uint8_t& opened = state<uint8_t>(portal);
    if (opened == 0) {
        opened = 1;
        content.takeFocus();
    }
    return content;
}

UiElement UiBuilder::tooltip(PrLayoutNode anchor, PrUiKey key) {
    State& b = *state_;
    closeOpen(b);
    PrUiPortalDesc desc{
        .kind = PrUiPortalKind_Popup,
        .parent = prUiNodeViewport(b.ui, anchor),
        .placement = {
            .anchor = prLayoutRectToPx(prLayoutNodeBounds(prUiTree(b.ui), anchor, PrLayoutCoordinates_Viewport)),
            .side = PrAppPopupSide_Below,
            .align = PrAppPopupAlign_Center,
            .adjust = PrAppPopupAdjustFlag_Flip | PrAppPopupAdjustFlag_Slide,
            .offset = PrVec2{0, 6},
        },
    };
    PrLayoutNode portal = prUiPortal(b.ui, key, &desc, PrUiFlag_Transparent | PrUiFlag_SubtreeTransparent);
    return openElement(*this, b, portal, 0, UiKind_Div);
}

UiBuilder& UiBuilder::memoBegin(void* PR_NULLABLE context, PrLayoutNode root) {
    State& b = *static_cast<State*>(context);
    openRoot(b, root, UiKind_Div);
    b.memos.push(root);
    return *b.self;
}

void UiBuilder::memoEnd(void* PR_NULLABLE context, PrLayoutNode root) {
    State& b = *static_cast<State*>(context);
    b.memos.pop();
    MemoRoot& kept = memoRootOf(b, root);
    kept.kind = UiKind_Div;
    uint32_t count = 0;
    const UiOp* ops = opsGiven(b, root, &count);
    if (count > kept.opCount || (count == 0 && kept.ops != nullptr)) {
        prMemoryFree(kept.ops);
        kept.ops = count != 0 ? static_cast<UiOp*>(prMemoryAlloc(count * sizeof(UiOp))) : nullptr;
    }
    if (count != 0)
        prMemCopy(kept.ops, ops, count * sizeof(UiOp));
    kept.opCount = count;
    b.built = root;
}

UiElement UiBuilder::memoBuild(PrLayoutNode parent, PrUiKey key, const PrUiMemoType& type, const void* inputs,
        void* PR_NULLABLE result) {
    State& b = *state_;
    // The parent's style is its record before the root inherits from it.
    closeOpen(b);
    Inherited inherited = inheritedFrom(b, parent);
    MemoInputs memo{.font = inherited.font, .color = inherited.color, .inputs = inputs};
    b.built = kPrLayoutNodeNone;
    PrUiMemoDesc desc{
        .type = &type,
        .context = &b,
        .inputs = &memo,
        .size = uint32_t(sizeof memo),
        .result = result,
        .key = key,
    };
    PrLayoutNode node = prUiMemo(b.ui, parent, &desc);
    // A memo that the round kept takes the style that its last build gave its root, which a call on it starts from.
    if (b.built != node)
        keepRoot(b, node, memoRootOf(b, node));
    return UiElement(this, node);
}

UiResources::UiResources(PrFontSet* PR_NULLABLE set, PrFontFamily sans, PrFontFamily mono) :
        state_(allocZeroed<State>()) {
    state_->set = set;
    state_->families.push(sans);
    state_->families.push(mono);
    state_->paths = prPathCacheCreate();
}

UiResources::~UiResources() {
    for (uint32_t i = 0; i < state_->made.count; ++i)
        prTextFontRelease(state_->made.data[i].font);
    state_->made.reset();
    state_->families.reset();
    prPathCacheDestroy(state_->paths);
    prMemoryFree(state_);
}

const PrPath* UiResources::path(PrStringView data) {
    return prPathCacheFind(state_->paths, data);
}

UiFamily UiResources::family(PrFontFamily family) {
    state_->families.push(family);
    return UiFamily(state_->families.count - 1);
}

PrUi* UiBuilder::ui() const {
    return state_->ui;
}

UiResources* PR_NULLABLE UiBuilder::resources() const {
    return state_->resources;
}

PrLayoutTree* UiBuilder::tree() const {
    return state_->tree;
}

double UiBuilder::time() const {
    // A memo that reads the time shows what changes with it.
    if (state_->memos.count != 0)
        prUiRequestMotion(state_->ui, state_->memos.last());
    return prUiCurrentRound(state_->ui)->time;
}

float UiBuilder::progress(const UiAnimation& animation) {
    PrLayoutTiming timing = prLayoutTimingDefault();
    timing.start = animation.start * 1000;
    timing.delay = animation.delay * 1000;
    timing.duration = animation.duration * 1000;
    timing.iterations = animation.iterations;
    timing.direction = animation.alternate ? PrLayoutPlaybackDirection_Alternate : PrLayoutPlaybackDirection_Normal;
    timing.fill = PrLayoutFill_Both;
    double now = prUiCurrentRound(state_->ui)->time;
    bool finished = false;
    double input = prLayoutTimingProgress(&timing, now * 1000, &finished);
    if (!finished) {
        // The memo being built shows the animation, so it builds again when the animation next moves.
        PrLayoutNode shows = state_->memos.count != 0 ? state_->memos.last() : kPrLayoutNodeNone;
        double begins = animation.start + animation.delay;
        if (now < begins) {
            if (shows != kPrLayoutNodeNone)
                prUiRequestMotionAt(state_->ui, shows, begins);
            else
                prUiRequestFrameAt(state_->ui, begins);
        } else if (shows != kPrLayoutNodeNone) {
            prUiRequestMotion(state_->ui, shows);
        } else {
            prUiRequestFrame(state_->ui);
        }
    }
    if (input < 0)
        return 0;
    // Tailwind's curves (tailwindcss.com/docs/transition-timing-function).
    PrLayoutEasing easing = prLayoutEasingLinear();
    switch (animation.ease) {
        case UiEase_In: easing = {.x1 = 0.4, .y1 = 0, .x2 = 1, .y2 = 1, .kind = PrLayoutEasingKind_CubicBezier}; break;
        case UiEase_Out: easing = {.x1 = 0, .y1 = 0, .x2 = 0.2, .y2 = 1, .kind = PrLayoutEasingKind_CubicBezier}; break;
        case UiEase_InOut:
            easing = {.x1 = 0.4, .y1 = 0, .x2 = 0.2, .y2 = 1, .kind = PrLayoutEasingKind_CubicBezier};
            break;
        default: break;
    }
    return float(prLayoutEase(&easing, input, false));
}

float UiBuilder::animate(const UiAnimation& animation, float from, float to) {
    return from + (to - from) * progress(animation);
}

RawArray<UiOp>& UiBuilder::opsOf(PrLayoutNode node) {
    if (state_->ops.count == 0 || state_->open != node)
        reopen(*state_, node);
    return state_->ops;
}

} // namespace pr

PR_ASSUME_NONNULL_END
