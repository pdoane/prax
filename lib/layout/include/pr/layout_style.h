// Style records: the styles of a layout tree's nodes.
//
// A style record is layout's input for a box. It is inspired by CSS computed values: lengths are absolute or
// percentages, values a parent decides are already decided, and nothing records how a value was written. A record is a
// group index of each kind (PrLayoutGroup). A group is a small structure of related values (pr/layout_value.h),
// interned by a tree in a table of its kind, whose index 0 holds the group's default. Records are interned the same
// way, so two boxes styled alike name the same record, and a record compares as a number.
//
// Each group kind is either inherited or not, as its properties are in CSS, and is read either by layout or only by
// painting, so the index of a changed group says what the change needs.
//
// A producer that already holds one group for each value it makes can create its groups and records instead, which
// hashes nothing: a created entry is never found by its value, and two entries created of one value are two entries.
// Layout compares records and groups by index, so a producer that creates two of one value costs work, never output.
//
// An index stays valid while a node of its tree names it or a caller holds a reference to it. Otherwise it stays valid
// until the tree's next layout. Each layout first frees the records and groups that nothing names, interned or created.

#pragma once

#include "pr/base.h"
#include "pr/color.h"
#include "pr/draw.h"
#include "pr/draw_filter.h"
#include "pr/font.h"
#include "pr/font_set.h"
#include "pr/geom.h"
#include "pr/layout_unit.h"
#include "pr/layout_value.h"
#include "pr/locale.h"
#include "pr/path.h"
#include "pr/raw_string.h"
#include "pr/string_view.h"
#include "pr/text.h"
#include "pr/text_break.h"
#include "pr/text_font.h"
#include "pr/text_itemize.h"
#include "pr/text_transform.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// A tree of nodes styled by records it interns (pr/layout_tree.h).
typedef struct PrLayoutTree PrLayoutTree;

// ---------------------------------------------------------------------------------------------------------------------
// Records

// The kinds of group. A record names one group of each kind.
enum {
    PrLayoutGroup_Font,             // inherited, read by layout
    PrLayoutGroup_Color,            // inherited, read by painting
    PrLayoutGroup_Box,              // not inherited, read by layout
    PrLayoutGroup_Decoration,       // not inherited, read by painting
    PrLayoutGroup_Effect,           // not inherited, read by compositing
    PrLayoutGroup_Flex,             // not inherited, read by layout
    PrLayoutGroup_Table,            // not inherited, read by layout: tables, and fragmentation
    PrLayoutGroup_Compositing,      // not inherited, read by compositing
    PrLayoutGroup_Grid,             // not inherited, read by layout
    PrLayoutGroup_Math,             // not inherited, read by layout
    PrLayoutGroup_Count,
};
typedef uint8_t PrLayoutGroup;

// A group of one kind, by its place in its tree's table of that kind. Index 0 is the kind's default.
typedef uint32_t PrLayoutGroupIndex;

// A record, by its place in its tree's table of records. Index 0 is the default record.
typedef uint32_t PrLayoutStyleIndex;

// No record: the record of a text node that has none of its own and takes its parent's (prLayoutNodeSetStyle).
static const PrLayoutStyleIndex kPrLayoutStyleNone = 0xffffffffu;

PR_NO_PADDING_BEGIN
// A style record: a group index of each kind.
typedef struct PrLayoutStyle {
    PrLayoutGroupIndex groups[PrLayoutGroup_Count];
} PrLayoutStyle;
PR_NO_PADDING_END

// Returns the record of every default: index 0 of each kind.
PrLayoutStyle prLayoutStyleDefault(void);

// Interns a record of groups that the tree holds, and returns its index. Record 0 is the default record.
PrLayoutStyleIndex prLayoutStyleIntern(PrLayoutTree* tree, const PrLayoutStyle* style);

// Creates a record of groups that the tree holds, and returns its index.
PrLayoutStyleIndex prLayoutStyleCreate(PrLayoutTree* tree, const PrLayoutStyle* style);

// Returns the record at `index`.
PrLayoutStyle prLayoutStyleRecord(const PrLayoutTree* tree, PrLayoutStyleIndex index);

// Adds or releases a caller's reference to the record at `index`. The reference keeps the record and its groups alive
// through the tree's layouts.
void prLayoutStyleRetain(PrLayoutTree* tree, PrLayoutStyleIndex index);
void prLayoutStyleRelease(PrLayoutTree* tree, PrLayoutStyleIndex index);

// Adds or releases a caller's reference to the group of `kind` at `index`. The reference keeps the group alive through
// the tree's layouts, so that a caller may keep a group's index and make records of it in later frames.
void prLayoutGroupRetain(PrLayoutTree* tree, PrLayoutGroup kind, PrLayoutGroupIndex index);
void prLayoutGroupRelease(PrLayoutTree* tree, PrLayoutGroup kind, PrLayoutGroupIndex index);

// Returns the references to the group of `kind` at `index`: those of the records that name it and of callers. A caller
// that holds the only reference knows that no record names the group.
uint32_t prLayoutGroupReferences(const PrLayoutTree* tree, PrLayoutGroup kind, PrLayoutGroupIndex index);

// ---------------------------------------------------------------------------------------------------------------------
// Names

// A <custom-ident> that a group names, as its tree interns it: grid lines or a grid area (css-grid-1/#named-lines), or
// a page (css-page-3/#using-named-pages). Equal names are equal numbers. A name is valid for the tree's life.
typedef uint32_t PrLayoutName;

// No name.
enum { kPrLayoutNameNone = 0 };

// Interns `name`, a <custom-ident>, and returns its number.
PrLayoutName prLayoutNameIntern(PrLayoutTree* tree, PrStringView name);

// ---------------------------------------------------------------------------------------------------------------------
// Font group

// The direction in which blocks stack, the orientation of lines, and how glyphs sit on them
// (css-writing-modes-4/#block-flow). The vertical modes set glyphs by text-orientation. The sideways modes set every
// glyph sideways, as a horizontal line turned a quarter turn: clockwise for sideways-rl, and counter-clockwise for
// sideways-lr, which reads from the bottom.
enum {
    PrLayoutWritingMode_HorizontalTb,   // horizontal lines, stacked top to bottom
    PrLayoutWritingMode_VerticalRl,     // vertical lines, stacked right to left
    PrLayoutWritingMode_VerticalLr,     // vertical lines, stacked left to right
    PrLayoutWritingMode_SidewaysRl,     // vertical lines, stacked right to left
    PrLayoutWritingMode_SidewaysLr,     // vertical lines, stacked left to right
};
typedef uint8_t PrLayoutWritingMode;

// How glyphs sit on a line of a vertical writing mode (css-writing-modes-4/#text-orientation): by each character's
// vertical orientation, all upright, or all sideways. The sideways writing modes and horizontal lines ignore it.
enum {
    PrLayoutTextOrientation_Mixed,
    PrLayoutTextOrientation_Upright,
    PrLayoutTextOrientation_Sideways,
};
typedef uint8_t PrLayoutTextOrientation;

// The baseline by which a box aligns its own text and the children that align by default
// (css-inline-3/#dominant-baseline-property). Auto is central in the vertical writing modes if text-orientation is
// mixed or upright, and alphabetic otherwise.
enum {
    PrLayoutDominantBaseline_Auto,
    PrLayoutDominantBaseline_TextBottom,
    PrLayoutDominantBaseline_Alphabetic,
    PrLayoutDominantBaseline_Ideographic,
    PrLayoutDominantBaseline_Middle,
    PrLayoutDominantBaseline_Central,
    PrLayoutDominantBaseline_Mathematical,
    PrLayoutDominantBaseline_TextTop,
    PrLayoutDominantBaseline_Hanging,
};
typedef uint8_t PrLayoutDominantBaseline;

// Which white space of text collapses and which is preserved (css-text-4/#white-space-collapsing): spaces and tabs, and
// segment breaks. A preserved segment break makes a forced line break.
enum {
    PrLayoutWhiteSpaceCollapse_Collapse,        // both collapse, a segment break into a space
    PrLayoutWhiteSpaceCollapse_Discard,         // both are removed
    PrLayoutWhiteSpaceCollapse_Preserve,        // both are preserved
    PrLayoutWhiteSpaceCollapse_PreserveBreaks,  // spaces and tabs collapse, and segment breaks are preserved
    PrLayoutWhiteSpaceCollapse_PreserveSpaces,  // spaces and tabs are preserved, and segment breaks become spaces
    PrLayoutWhiteSpaceCollapse_BreakSpaces,     // like preserve, and a line may wrap after any space
};
typedef uint8_t PrLayoutWhiteSpaceCollapse;

// Whether text may wrap at its soft wrap opportunities (css-text-4/#text-wrap-mode).
enum {
    PrLayoutTextWrapMode_Wrap,
    PrLayoutTextWrapMode_Nowrap,
};
typedef uint8_t PrLayoutTextWrapMode;

// Where a line's content goes in its line box (css-text-3/#text-align-property): against its start or end edge, its
// line-left or line-right edge, centered, or justified to fill it.
enum {
    PrLayoutTextAlign_Start,
    PrLayoutTextAlign_End,
    PrLayoutTextAlign_Left,
    PrLayoutTextAlign_Right,
    PrLayoutTextAlign_Center,
    PrLayoutTextAlign_Justify,

    // For the last line only (text-align-last): the same as the other lines, but start if they justify.
    PrLayoutTextAlign_Auto,
};
typedef uint8_t PrLayoutTextAlign;

// What a justified line stretches (css-text-3/#text-justify-property): its word separators for auto and inter-word,
// nothing for none, and the gaps between its characters for inter-character.
enum {
    PrLayoutTextJustify_Auto,
    PrLayoutTextJustify_None,
    PrLayoutTextJustify_InterWord,
    PrLayoutTextJustify_InterCharacter,
};
typedef uint8_t PrLayoutTextJustify;

// Which lines text-indent indents (css-text-3/#text-indent-property): the first line of a block container, and with
// EachLine also each line after a forced break. Hanging indents every other line instead.
enum {
    PrLayoutTextIndentFlag_Hanging = 1u << 0,
    PrLayoutTextIndentFlag_EachLine = 1u << 1,
};
typedef uint8_t PrLayoutTextIndentFlags;

// The math layout state that a box inherits.
enum {
    PrLayoutMathFlag_CompactStyle = 1u << 0,    // math-style is compact (mathml-core/#the-math-style-property)
    PrLayoutMathFlag_CompactShift = 1u << 1,    // math-shift is compact (mathml-core/#the-math-shift)
    PrLayoutMathFlag_Dotless = 1u << 2,         // text takes a MATH font's dotless forms (dtls), as under an accent
};
typedef uint8_t PrLayoutMathFlags;

PR_NO_PADDING_BEGIN
// How text is set: its font, what it asks of shaping, the height of its lines, how its white space collapses and its
// lines wrap, how it is transformed, how a line's content sits in its line box, the writing mode and direction of the
// content a box holds, the language of its text, and the state of math layout. Inherited, and read by layout.
typedef struct PrLayoutFontStyle {
    // What its text is set in, at its size and for its language (pr/text_font.h), or the tree's default font if null. A
    // group holds a reference to the font while the tree holds the group.
    PrTextFont* PR_NULLABLE font;

    // The forms and features that its text is shaped with, or null for the shaper's defaults. A group holds a reference
    // to the set while the tree holds the group. Groups compare the set by its address, so two equal sets make two
    // distinct groups.
    const PrTextFeatureSet* PR_NULLABLE features;

    // The space added after each typographic character unit of its text, and after each word separator, in px
    // (css-text-4/#letter-spacing-property, css-text-4/#word-spacing-property).
    float letterSpacing;
    float wordSpacing;

    PrLayoutLineHeight lineHeight;
    PrLayoutLength textIndent;          // a percentage of the block container's content inline size
    PrLayoutWritingMode writingMode;
    PrTextDirection direction;
    PrLayoutWhiteSpaceCollapse whiteSpaceCollapse;
    PrLayoutTextWrapMode textWrapMode;
    PrLayoutTextAlign textAlign;        // the alignment of every line except the last (text-align-all)
    PrLayoutTextAlign textAlignLast;    // the alignment of the last line, and of each line before a forced break
    PrLayoutTextJustify textJustify;
    PrLayoutTextIndentFlags textIndentFlags;
    PrLayoutTextOrientation textOrientation;
    PrLayoutDominantBaseline dominantBaseline;
    PrTextWordBreak wordBreak;          // where lines may break within its words (css-text-4/#word-break-property)
    PrTextOverflowWrap overflowWrap;    // whether a word that fits no line breaks (css-text-4/#overflow-wrap-property)

    // How its text is transformed after its white space collapses (css-text-4/#text-transform-property). The transform
    // of a text's own record applies, also on the first formatted line, whose records give it no other.
    PrTextTransform textTransform;

    // The fewest lines of a block container that a fragmentation break may leave in a fragmentainer before it and after
    // it (css-break-4/#widows-orphans), at least 1. A larger count than 255 is 255.
    uint8_t orphans;
    uint8_t widows;

    // The computed math-depth (mathml-core/#the-math-script-level-property), clamped to 0 through 255. Text at depth 1
    // is shaped with the font's first level of script forms (ssty), and text at a greater depth with the second.
    uint8_t mathDepth;

    PrLayoutMathFlags mathFlags;
    uint8_t _pad[7];
} PrLayoutFontStyle;
PR_NO_PADDING_END

// Returns the default font group: the tree's default font, a normal line height, collapsing white space and wrapping
// lines, lines aligned to their start edge, the last line aligned like the others, no indent, horizontal-tb and ltr
// with mixed orientation and the auto dominant baseline, and two lines kept on each side of a break.
PrLayoutFontStyle prLayoutFontStyleDefault(void);

// Interns or creates a font group and returns its index, or returns the font group at `index`.
PrLayoutGroupIndex prLayoutFontStyleIntern(PrLayoutTree* tree, const PrLayoutFontStyle* font);
PrLayoutGroupIndex prLayoutFontStyleCreate(PrLayoutTree* tree, const PrLayoutFontStyle* font);
PrLayoutFontStyle prLayoutFontStyleRecord(const PrLayoutTree* tree, PrLayoutGroupIndex index);

// ---------------------------------------------------------------------------------------------------------------------
// Color group

// Whether a box's content is visible (css-display-4/#visibility).
enum {
    PrLayoutVisibility_Visible,
    PrLayoutVisibility_Hidden,
};
typedef uint8_t PrLayoutVisibility;

// Whether a table cell with no content paints its borders and backgrounds in the separated-borders model
// (css-tables-3/#empty-cell-rendering).
enum {
    PrLayoutEmptyCells_Show,
    PrLayoutEmptyCells_Hide,
};
typedef uint8_t PrLayoutEmptyCells;

// Whether hit testing can find a box as the target of a pointer (css-ui-4/#pointer-events-control). A box that is not a
// target lets the pointer through to what lies beneath, but its descendants may still be targets.
enum {
    PrLayoutPointerEvents_Auto,
    PrLayoutPointerEvents_None,

    // The content that the box's painter draws decides where the pointer reaches it, as an outermost svg element's
    // descendants do. Hit testing finds the box if its rectangle is hit, and the client that draws the content decides
    // which part of the content is the target.
    PrLayoutPointerEvents_Content,
};
typedef uint8_t PrLayoutPointerEvents;

// Whether a user's wheel over a box waits for the box's document, which may cancel it, before anything scrolls. The
// border box of a box that waits, and the area that its descendants reach, are its scene's wheel region
// (prLayoutCompositorWheelBlocks).
enum {
    PrLayoutWheelEvents_Passive,
    PrLayoutWheelEvents_Blocking,
};
typedef uint8_t PrLayoutWheelEvents;

PR_NO_PADDING_BEGIN
// The color of text, whether a box is visible, whether an empty table cell paints, whether a box takes the pointer, and
// whether a wheel over it waits. A group's colors are extended sRGB with straight alpha, as draw lists carry them, in
// 16-bit floats. Read by painting and hit testing. Each field but `wheelEvents` is inherited.
typedef struct PrLayoutColorStyle {
    PrColor16 color;
    PrLayoutVisibility visibility;
    PrLayoutEmptyCells emptyCells;
    PrLayoutPointerEvents pointerEvents;
    PrLayoutWheelEvents wheelEvents;
} PrLayoutColorStyle;
PR_NO_PADDING_END

// Returns opaque black, visible, empty cells shown, the pointer taken, and passive to the wheel.
PrLayoutColorStyle prLayoutColorStyleDefault(void);

// Interns or creates a color group and returns its index, or returns the color group at `index`.
PrLayoutGroupIndex prLayoutColorStyleIntern(PrLayoutTree* tree, const PrLayoutColorStyle* color);
PrLayoutGroupIndex prLayoutColorStyleCreate(PrLayoutTree* tree, const PrLayoutColorStyle* color);
PrLayoutColorStyle prLayoutColorStyleRecord(const PrLayoutTree* tree, PrLayoutGroupIndex index);

// ---------------------------------------------------------------------------------------------------------------------
// Box group

// The kind of box a node generates (css-display-4/#the-display-properties).
enum {
    PrLayoutDisplay_Block,          // a block-level block container
    PrLayoutDisplay_FlowRoot,       // a block-level block container that establishes a block formatting context
    PrLayoutDisplay_Inline,         // an inline box
    PrLayoutDisplay_InlineBlock,    // an inline-level block container that establishes a block formatting context
    PrLayoutDisplay_None,
    PrLayoutDisplay_Flex,           // a block-level flex container (css-flexbox-2/#flex-containers)
    PrLayoutDisplay_InlineFlex,     // an inline-level flex container, atomic as an inline-block is
    PrLayoutDisplay_Grid,           // a block-level grid container (css-grid-1/#grid-containers)
    PrLayoutDisplay_InlineGrid,     // an inline-level grid container, atomic as an inline-block is

    // An outside marker, placed beside its list item's first line (css-lists-3/#list-style-position-property). Only a
    // block container's first child is one, and anywhere else it generates no box.
    PrLayoutDisplay_Marker,

    // A table (css-tables-3/#table-structure), block-level or inline-level, and the boxes inside it. If the parent of a
    // table-internal box is not the part that the box belongs in, layout wraps the box in the anonymous parts that are
    // missing (css-tables-3/#fixup).
    PrLayoutDisplay_Table,
    PrLayoutDisplay_InlineTable,
    PrLayoutDisplay_TableRowGroup,
    PrLayoutDisplay_TableHeaderGroup,
    PrLayoutDisplay_TableFooterGroup,
    PrLayoutDisplay_TableRow,
    PrLayoutDisplay_TableColumnGroup,
    PrLayoutDisplay_TableColumn,
    PrLayoutDisplay_TableCell,
    PrLayoutDisplay_TableCaption,

    // A page-margin box (css-page-3/#margin-boxes): a child of a paginated tree's viewport root, which a page that
    // names it lays out in its margin (prLayoutTreeSetPageMargins), as a block container that establishes a block
    // formatting context and a stacking context. It takes no part in its parent's layout, and generates no box anywhere
    // else.
    PrLayoutDisplay_PageMargin,

    PrLayoutDisplay_BlockMath,      // a block-level math box (mathml-core/#new-display-math-value), content centered
    PrLayoutDisplay_InlineMath,     // an atomic inline-level math box, aligned by its alphabetic baseline
};
typedef uint8_t PrLayoutDisplay;

// Returns true if a box of `display` is inline-level: it takes part in its parent's inline formatting context
// (css-display-4/#inline-level).
bool prLayoutDisplayIsInlineLevel(PrLayoutDisplay display);

// Returns true if an element of `display` generates an inline box, whose size its content determines rather than its
// sizing properties (CSS2/#inline-width). A replaced element's box is atomic whatever its display.
bool prLayoutDisplayIsInlineBox(PrLayoutDisplay display);

// Returns true if `display` is layout-internal: a table-internal display or table-caption, which has meaning only
// inside a table (css-display-3/#layout-specific-display).
bool prLayoutDisplayIsLayoutInternal(PrLayoutDisplay display);

// What the sizing properties measure (css-sizing-3/#box-sizing).
enum {
    PrLayoutBoxSizing_ContentBox,
    PrLayoutBoxSizing_BorderBox,
};
typedef uint8_t PrLayoutBoxSizing;

// What a box does with content that overflows it (css-overflow-3/#overflow-control). Hidden and Scroll make a scroll
// container, which clips its content and scrolls it. Clip clips the content, and makes neither a scroll container nor a
// formatting context.
enum {
    PrLayoutOverflow_Visible,
    PrLayoutOverflow_Hidden,
    PrLayoutOverflow_Scroll,
    PrLayoutOverflow_Clip,
};
typedef uint8_t PrLayoutOverflow;

// What a scroll container does along an axis with a user's scroll that it cannot take
// (css-overscroll-1/#overscroll-behavior-properties). Contain and None keep the scroll from chaining to the containers
// around it. Auto and Chain let it chain. Layout draws no overscroll affordance, so None acts as Contain does, and
// Chain as Auto does.
enum {
    PrLayoutOverscroll_Auto,
    PrLayoutOverscroll_Contain,
    PrLayoutOverscroll_None,
    PrLayoutOverscroll_Chain,
};
typedef uint8_t PrLayoutOverscroll;

// Whether a box and its subtree take part in choosing the anchors of the scroll containers around them, up to the next
// scroll container inside them (css-scroll-anchoring-1/#exclusion-api). A scroll container whose value is None has no
// anchor.
enum {
    PrLayoutOverflowAnchor_Auto,
    PrLayoutOverflowAnchor_None,
};
typedef uint8_t PrLayoutOverflowAnchor;

// Which side of its containing block a box floats to (CSS2/#float-position, css-logical-1/#float-clear). Left and Right
// are the line-left and line-right sides; InlineStart and InlineEnd follow the containing block's direction.
enum {
    PrLayoutFloat_None,
    PrLayoutFloat_Left,
    PrLayoutFloat_Right,
    PrLayoutFloat_InlineStart,
    PrLayoutFloat_InlineEnd,
};
typedef uint8_t PrLayoutFloat;

// Which earlier floats a box is placed below (CSS2/#flow-control, css-logical-1/#float-clear), by the same sides as
// PrLayoutFloat.
enum {
    PrLayoutClear_None,
    PrLayoutClear_Left,
    PrLayoutClear_Right,
    PrLayoutClear_Both,
    PrLayoutClear_InlineStart,
    PrLayoutClear_InlineEnd,
};
typedef uint8_t PrLayoutClear;

// The baseline by which an inline-level box aligns with the same baseline of its parent
// (css-inline-3/#alignment-baseline-property). Baseline is the dominant baseline, which is alphabetic in the lines that
// layout lays out.
enum {
    PrLayoutAlignmentBaseline_Baseline,
    PrLayoutAlignmentBaseline_TextBottom,
    PrLayoutAlignmentBaseline_Alphabetic,
    PrLayoutAlignmentBaseline_Ideographic,
    PrLayoutAlignmentBaseline_Middle,
    PrLayoutAlignmentBaseline_Central,
    PrLayoutAlignmentBaseline_Mathematical,
    PrLayoutAlignmentBaseline_TextTop,
    PrLayoutAlignmentBaseline_Hanging,
};
typedef uint8_t PrLayoutAlignmentBaseline;

// Which of an inline-level box's baselines it aligns by (css-inline-3/#baseline-source). Auto selects the last baseline
// for an inline-block.
enum {
    PrLayoutBaselineSource_Auto,
    PrLayoutBaselineSource_First,
    PrLayoutBaselineSource_Last,
};
typedef uint8_t PrLayoutBaselineSource;

// How a box is positioned (css-position-3/#position-property). Absolute and Fixed take the box out of flow and place it
// in its containing block. Sticky is laid out as Relative with no offset, and is then moved by its insets from the
// scrollport of its nearest scroll container as that container scrolls (css-position-3/#stickypos-insets). Its insets'
// percentages are of the scrollport's size. An inline box does not stick. Running takes a running element out of flow
// (css-gcpm-3/#running-elements): it leaves a placeholder at its static position, which places it on a page
// (prLayoutNodePage), and no box of its own. Its producer lays copies of it out in page-margin boxes.
enum {
    PrLayoutPosition_Static,
    PrLayoutPosition_Relative,
    PrLayoutPosition_Absolute,
    PrLayoutPosition_Fixed,
    PrLayoutPosition_Running,       // after Fixed, so that the positions out of flow are one range
    PrLayoutPosition_Sticky,
};
typedef uint8_t PrLayoutPosition;

PR_NO_PADDING_BEGIN
// A box's size, its edges, its position, how it aligns on a line, and how its content is reordered. Sides are indexed
// by PrRectSide. A border's width is its used width, which is zero if its style is none. The enums pack into five
// bytes. Not inherited, and read by layout.
//
// A box out of flow records whether the display that its producer blockified was inline-level, which places its static
// position as for an inline-level box (css-position-3/#staticpos-rect). A transform other than none makes a box the
// containing block of the boxes out of flow in it if transforms apply to it (css-transforms-1/#transformable-element),
// so layout reads whether it has one, and only compositing reads its matrix. Similarly, whether its z-index is auto
// marks the fragments through which a stacking order is found, and only painting reads the integer. Layout reads
// whether its visibility is collapse: a collapsed flex item takes no room in its line except for its line's cross size
// (css-flexbox-2/#visibility-collapse), and the paint group's visibility decides what paints.
//
// An aspect-ratio of auto is zero, as is a degenerate ratio (css-sizing-4/#aspect-ratio). A ratio given with auto
// yields to a replaced box's natural ratio, and relates content boxes. A ratio given alone relates the boxes that
// box-sizing names.
//
// A box with inline-size containment (css-contain-2/#containment-inline-size) measures no content in its inline axis.
// Its intrinsic inline size is its edges and a content size: `intrinsicCharacters` characters if it is not zero, as
// HTML sizes a text control, and otherwise the contain-intrinsic size of that axis in px, or zero for none
// (css-sizing-4/#intrinsic-size-override). A count of characters n is (n - 1) times the average advance plus the
// maximum advance of the box's first available font (html/#converting-a-character-width-to-pixels).
typedef struct PrLayoutBoxStyle {
    PrLayoutLength width;
    PrLayoutLength height;
    PrLayoutLength minWidth;
    PrLayoutLength minHeight;
    PrLayoutLength maxWidth;
    PrLayoutLength maxHeight;
    PrLayoutLength containIntrinsicWidth;
    PrLayoutLength containIntrinsicHeight;
    PrLayoutLength margin[PrRectSide_Count];
    PrLayoutLength padding[PrRectSide_Count];
    PrLayoutLength inset[PrRectSide_Count];     // a positioned box's insets
    PrLayoutUnit border[PrRectSide_Count];
    float aspectRatio;                          // width over height of its preferred aspect ratio, or 0 for auto
    PrLayoutBaselineShift baselineShift;        // an inline-level box: how it moves once aligned
    PrLayoutDisplay display;
    PrLayoutPosition position : 3;
    uint8_t zIndexed : 1;                               // its z-index is not auto; the effects group has the integer
    PrLayoutOverflow overflowX : 2;
    PrLayoutOverflow overflowY : 2;
    PrLayoutBoxSizing boxSizing : 1;
    uint8_t staticInline : 1;                           // out of flow: blockified from an inline-level display
    uint8_t collapsed : 1;                              // its visibility is collapse
    uint8_t aspectRatioAuto : 1;                        // its aspect-ratio gives auto with the ratio
    PrLayoutBaselineSource baselineSource : 2;          // an inline-level block container: which baseline it has
    PrLayoutOverflowAnchor overflowAnchor : 1;

    // Its filter or backdrop-filter is not none. Unless it is the root, this makes it a containing block for all of its
    // descendants, as an inline box too (filter-effects-1/#FilterProperty). The compositing group has the values.
    uint8_t filtered : 1;

    PrLayoutFloat floating : 3;
    PrLayoutClear clear : 3;

    // It is absolutely positioned and its clip is a rectangle. The rectangle clips its content as an overflow clip
    // does, and makes no stacking context (css-masking-1/#clip-property). The compositing group has the rectangle.
    uint8_t clipped : 1;

    uint8_t containInline : 1;                          // it has inline-size containment

    // An inline-level box: how it aligns with its parent. A table cell reads these and baselineShift as its
    // vertical-align in its rows (css-tables-3/#row-layout). A baseline-shift of top or bottom aligns the cell to the
    // top or the bottom, and an alignment-baseline of middle to the middle. Any other value aligns it by its baseline.
    PrLayoutAlignmentBaseline alignmentBaseline : 4;
    PrTextBidi unicodeBidi : 3;

    // Its transform or its perspective is not none, or will-change names one, which makes it a containing block for all
    // of its descendants and a stacking context. The effects group has the values.
    uint8_t transformed : 1;

    // A scroll container: what a user's scroll that it cannot take does along each axis. The root's values apply to the
    // viewport.
    PrLayoutOverscroll overscrollX : 2;
    PrLayoutOverscroll overscrollY : 2;

    // It has size containment in its block axis, so that its content does not size it there and it does not break in a
    // fragmentation context (css-contain-2/#containment-size, css-break-4/#monolithic).
    uint8_t containBlock : 1;
    uint8_t _pad2 : 3;

    uint16_t intrinsicCharacters;
} PrLayoutBoxStyle;
PR_NO_PADDING_END

// Returns the default box group: an inline box with auto sizes, no edges and no limits, aligned by its baseline with
// its parent's baseline, normal unicode-bidi, and no containment.
PrLayoutBoxStyle prLayoutBoxStyleDefault(void);

// Interns or creates a box group and returns its index, or returns the box group at `index`.
PrLayoutGroupIndex prLayoutBoxStyleIntern(PrLayoutTree* tree, const PrLayoutBoxStyle* box);
PrLayoutGroupIndex prLayoutBoxStyleCreate(PrLayoutTree* tree, const PrLayoutBoxStyle* box);
PrLayoutBoxStyle prLayoutBoxStyleRecord(const PrLayoutTree* tree, PrLayoutGroupIndex index);

// ---------------------------------------------------------------------------------------------------------------------
// Decoration group

// How a replaced box's content fits its content box (css-images-3/#the-object-fit).
enum {
    PrLayoutObjectFit_Fill,
    PrLayoutObjectFit_Contain,
    PrLayoutObjectFit_Cover,
    PrLayoutObjectFit_None,
    PrLayoutObjectFit_ScaleDown,
};
typedef uint8_t PrLayoutObjectFit;

// The box that a background is positioned in or painted within (css-backgrounds-3/#background-origin).
enum {
    PrLayoutBackgroundBox_BorderBox,
    PrLayoutBackgroundBox_PaddingBox,
    PrLayoutBackgroundBox_ContentBox,
};
typedef uint8_t PrLayoutBackgroundBox;

// A box's background layers, by their place in their tree's table of backgrounds (prLayoutBackgroundIntern).
typedef uint32_t PrLayoutBackgroundIndex;

// No background layers: the index of an empty list, which paints only the background color.
enum { kPrLayoutBackgroundNone = 0 };

// What a background image is fixed to (css-backgrounds-3/#background-attachment): the box, the viewport, or the box's
// content, which scrolls with it.
enum {
    PrLayoutBackgroundAttachment_Scroll,
    PrLayoutBackgroundAttachment_Fixed,
    PrLayoutBackgroundAttachment_Local,
};
typedef uint8_t PrLayoutBackgroundAttachment;

// The lines that decorate text (css-text-decor-3/#text-decoration-line-property).
enum {
    PrLayoutTextDecorationLine_Underline = 1u << 0,
    PrLayoutTextDecorationLine_Overline = 1u << 1,
    PrLayoutTextDecorationLine_LineThrough = 1u << 2,
};
typedef uint8_t PrLayoutTextDecorationLines;

PR_NO_PADDING_BEGIN
// What paints a box's background, border and outline, and places a replaced box's content in its content box. Sides are
// indexed by PrRectSide. Not inherited, and read by painting. An outline is drawn around the border box,
// `outlineOffset` outside it, and takes no room (css-ui-4/#outline-props). Its width is zero if its style is none.
//
// The background color paints below the layers, in the bottom layer's painting area
// (css-backgrounds-3/#background-color). It paints within the box that `backgroundClip` names. If
// `backgroundAttachment` is Local and the box is a scroll container, it is attached to the box's content and scrolls
// with it.
//
// The border box's corners shape its background, its border and its outline, what clips its content as a container, a
// replaced box's content, where the box is hit, and a clip path or backdrop filter of the border box's edges. The other
// boxes' corners follow from them (css-backgrounds-3/#corner-shaping). Corners that overlap are reduced
// (css-backgrounds-3/#corner-overlap).
//
// A multi-column container paints its column rule in the middle of each gap between two of its columns that hold
// content, and the rule takes no room (css-multicol-1/#column-gaps-and-rules). Its width is zero if its style is none.
//
// A box's text decoration lines are drawn across the text of the box and of its in-flow content, other than the content
// of atomic inlines and of boxes out of flow (css-text-decor-3/#line-decoration), in their color, style and thickness,
// or the thickness that the text's font gives where `textDecorationThickness` is zero. The lines are placed by the
// metrics of each text's font. Underlines and overlines paint below the text, and lines through it above.
typedef struct PrLayoutDecorationStyle {
    PrColor16 background;
    PrColor16 borderColor[PrRectSide_Count];
    PrColor16 outlineColor;
    PrColor16 columnRuleColor;
    PrColor16 textDecorationColor;
    PrLayoutLength objectPositionX;     // where fitted content sits (css-images-3/#the-object-position)
    PrLayoutLength objectPositionY;

    // The radii of the corners (css-backgrounds-3/#border-radius), ordered as PrLayoutShape::radii: horizontal then
    // vertical for each corner from the top left clockwise. A percentage is of the border box's width or height.
    PrLayoutLength radii[8];

    // The shape of each corner by PrRectCorner, as PrCorner::shape: K of superellipse(K)
    // (css-borders-4/#typedef-corner-shape-value), 1 for round.
    float cornerShapes[PrRectCorner_Count];

    PrLayoutUnit outlineWidth;
    PrLayoutUnit outlineOffset;
    PrLayoutUnit columnRuleWidth;
    PrLayoutUnit textDecorationThickness;
    PrLayoutBackgroundIndex backgroundLayers;
    PrDrawBorderStyle borderStyle[PrRectSide_Count];
    PrDrawBorderStyle outlineStyle;
    PrLayoutObjectFit objectFit;
    PrLayoutBackgroundBox backgroundClip;
    PrLayoutBackgroundAttachment backgroundAttachment;
    PrDrawBorderStyle columnRuleStyle;
    PrLayoutTextDecorationLines textDecorationLine;
    PrDrawDecorationStyle textDecorationStyle;
    uint8_t _pad[1];
} PrLayoutDecorationStyle;
PR_NO_PADDING_END

// Returns a transparent background, no border, square corners, and content that fills the content box, centered.
PrLayoutDecorationStyle prLayoutDecorationStyleDefault(void);

// Interns or creates a decoration group and returns its index, or returns the decoration group at `index`.
PrLayoutGroupIndex prLayoutDecorationStyleIntern(PrLayoutTree* tree, const PrLayoutDecorationStyle* decoration);
PrLayoutGroupIndex prLayoutDecorationStyleCreate(PrLayoutTree* tree, const PrLayoutDecorationStyle* decoration);
PrLayoutDecorationStyle prLayoutDecorationStyleRecord(const PrLayoutTree* tree, PrLayoutGroupIndex index);

// ---------------------------------------------------------------------------------------------------------------------
// Sources of effects

// What a source writes of the mask that it draws (PrLayoutSourceFunctions::draw): the area outside of which the mask is
// zero, and how its values mask.
typedef struct PrLayoutSourceMask {
    PrRect area;
    PrDrawMaskMode mode;
    PrColorSpace space;             // Luminance: the space that luminance is computed in
} PrLayoutSourceMask;

// What a source writes of the filter that it appends (PrLayoutSourceFunctions::filter): the node of its result, and the
// region outside of which the result is transparent black, or an empty rectangle for no limit.
typedef struct PrLayoutSourceFilter {
    PrDrawFilterInput result;
    PrRect region;
} PrLayoutSourceFilter;

// The functions by which the client that styles the tree draws a clip path, a mask layer or an item of a filter list,
// such as the clipPath, mask or filter element that a URL names (css-masking-1/#the-clip-path,
// css-masking-1/#the-mask-image, filter-effects-1/#FilterProperty). A box's object bounding box is its border box. Its
// user space has its origin at the border box's top left (css-transforms-1/#transform-box), and its percentages are of
// the border box.
//
// The functions are called on the thread that records a scene, which may be a layout worker during a job. So they may
// read only state that the client changes between jobs. Each must be deterministic in its arguments and that state.
typedef struct PrLayoutSourceFunctions {
    // Records into `list` the coverage of a clip path, or the content of a mask, for a box whose object bounding box is
    // `box`, in the box's user space, and writes how the list masks to `out`. Returns false if the box draws nothing,
    // such as for an empty clip path or a mask region of no area. Null for a source that is neither.
    bool (*PR_NULLABLE draw)(void* PR_NULLABLE context, PrRect box, PrDrawList* list, PrLayoutSourceMask* out);

    // Appends to `builder` the nodes of a filter that reads `input` as its source, for a box whose border box is `box`,
    // in the box's user space, and writes its result to `out`. The filter's object bounding box is `bounds`, which
    // covers the box and its descendants, so that a filter region reaches the descendants that overflow the box.
    // Returns false if the box draws nothing, such as for a filter without primitives. Null for a source that is not a
    // filter.
    bool (*PR_NULLABLE filter)(void* PR_NULLABLE context, PrRect box, PrRect bounds, PrDrawFilterInput input,
        PrDrawFilterBuilder* builder, PrLayoutSourceFilter* out);

    // Returns true if `point`, in the box's user space, is inside a clip path for a box whose object bounding box is
    // `box`. Hit testing calls it. Null for a source that is not a clip path, or that leaves every point inside.
    bool (*PR_NULLABLE contains)(void* PR_NULLABLE context, PrRect box, PrVec2 point);

    // Adds or releases a reference to the context. Called on any thread. Null if the client keeps the context alive
    // while the tree may use the source.
    void (*PR_NULLABLE retain)(void* PR_NULLABLE context);
    void (*PR_NULLABLE release)(void* PR_NULLABLE context);
} PrLayoutSourceFunctions;

PR_NO_PADDING_BEGIN
// A source of an effect: its functions, which are static, and their context. `version` is the client's count of the
// changes to what the source draws. A group or a list that names a source compares by all three, so a client that
// raises the version and sets the group again repaints what the source changed.
typedef struct PrLayoutSource {
    const PrLayoutSourceFunctions* PR_NULLABLE functions;   // null for no source
    void* PR_NULLABLE context;
    uint32_t version;
    uint32_t _pad;
} PrLayoutSource;
PR_NO_PADDING_END

// ---------------------------------------------------------------------------------------------------------------------
// Filters

// The kinds of item of a filter list (filter-effects-1/#supported-filter-functions), and a source that the client
// draws.
enum {
    PrLayoutFilterKind_Blur,
    PrLayoutFilterKind_Brightness,
    PrLayoutFilterKind_Contrast,
    PrLayoutFilterKind_DropShadow,
    PrLayoutFilterKind_Grayscale,
    PrLayoutFilterKind_HueRotate,
    PrLayoutFilterKind_Invert,
    PrLayoutFilterKind_Opacity,
    PrLayoutFilterKind_Saturate,
    PrLayoutFilterKind_Sepia,
    PrLayoutFilterKind_Source,
};
typedef uint8_t PrLayoutFilterKind;

// An item of a filter list, with its arguments resolved and omitted arguments given their defaults
// (filter-effects-1/#computed-values-of-filter-functions). Its kind decides which fields it uses.
typedef struct PrLayoutFilter {
    PrLayoutFilterKind kind;

    // The amount of a color function, hue-rotate's angle in radians, or blur's standard deviation in pixels.
    float amount;

    // DropShadow: the offset and the standard deviation in pixels, and the color, extended sRGB with straight alpha.
    PrVec2 offset;
    float sigma;
    PrColor16 color;

    PrLayoutSource source;              // Source
} PrLayoutFilter;

// Appends the nodes of `filter`, which is a filter function and not a source, to `builder`, reading `input`, and
// returns the node of its result. The nodes compute in sRGB (filter-effects-1/#FilterProperty), and a blur reads past
// its input's region by `edge`.
PrDrawFilterInput prLayoutFilterAppend(const PrLayoutFilter* filter, PrDrawFilterInput input, PrImageEdgeMode edge,
    PrDrawFilterBuilder* builder);

// An interned filter list (prLayoutFilterIntern).
typedef uint32_t PrLayoutFilterIndex;

// No filter: the list of none.
enum { kPrLayoutFilterNone = 0 };

// Interns a filter list of `count` items, in the order they apply, and returns its index. No items is
// kPrLayoutFilterNone. Equal lists are one entry. The compositing groups that name an entry keep it, and otherwise it
// stays valid until the next layout of the tree.
PrLayoutFilterIndex prLayoutFilterIntern(PrLayoutTree* tree, const PrLayoutFilter* filters, uint32_t count);

// ---------------------------------------------------------------------------------------------------------------------
// Images and backgrounds

// The natural dimensions of a replaced element's content (css-images-3/#natural-dimensions). Layout sizes the box from
// them if its style leaves a size auto. A negative width or height means the content has none in that axis; a zero
// `ratio` (width over height) means it has no natural ratio. When both width and height are present, their ratio is
// used and `ratio` is ignored.
typedef struct PrLayoutNaturalSize {
    PrLayoutUnit width;
    PrLayoutUnit height;
    float ratio;
} PrLayoutNaturalSize;

// Returns natural dimensions with no width, height or ratio, as for content that scales to any size.
PrLayoutNaturalSize prLayoutNaturalSizeNone(void);

// How an image is resampled when it is drawn at another size (css-images-3/#the-image-rendering). Auto filters it
// smoothly, and CrispEdges and Pixelated keep its texels sharp.
enum {
    PrLayoutImageRendering_Auto,
    PrLayoutImageRendering_CrispEdges,
    PrLayoutImageRendering_Pixelated,
};
typedef uint8_t PrLayoutImageRendering;

// Draws content into `list` over the rectangle from the origin to `extent`, in pixels, which is the size that the
// content is shown at. Called on the thread that records the scene, which may be a layout worker during a job, so it
// may read only state that the client changes between jobs. It must be deterministic in `extent` and that state.
typedef void (*PrLayoutPaintFn)(void* PR_NULLABLE context, PrVec2 extent, PrDrawList* list);

// Returns the block size in px that content takes at an inline size of `inlineSize` px, as a document's height follows
// the width that it is laid out at. Called on the thread that lays out the tree, which may be a layout worker during a
// job, so it may read only state that the client changes between jobs. It must be deterministic in `inlineSize` and
// that state.
typedef float (*PrLayoutMeasureFn)(void* PR_NULLABLE context, float inlineSize);

// Content that draws itself at whatever size it is shown, as an SVG image or an application's canvas does, rather than
// being scaled from one drawing: a callback, and the context that it is called with. A painter with `retain` and
// `release` shares its context, which the tree holds a reference to while it uses the painter, and they may be called
// on any thread. Without them, the client keeps the context alive while the tree may call the painter.
//
// A painter that measures has a block size for each inline size, and no natural size: an auto inline size fills the
// containing block, as a block does, and an auto block size is what `measure` gives for the used inline size. Content
// that measures differently at the same inline size has a new revision (PrLayoutContent::revision), which lays the
// element out again.
typedef struct PrLayoutPainter {
    void* PR_NULLABLE context;
    PrLayoutPaintFn paint;
    PrLayoutMeasureFn PR_NULLABLE measure;
    void (*PR_NULLABLE retain)(void* PR_NULLABLE context);
    void (*PR_NULLABLE release)(void* PR_NULLABLE context);
} PrLayoutPainter;

// Adds or releases a reference to the painter's context, if the painter shares it.
void prLayoutPainterRetain(PrLayoutPainter painter);
void prLayoutPainterRelease(PrLayoutPainter painter);

// What an image is (css-images-3/#image-values): nothing, a decoded image, a draw list, a gradient, or a painter. A
// decoded image and a draw list cover their natural size. A gradient and a painter draw at whatever size they are
// given.
enum {
    PrLayoutImageKind_None,
    PrLayoutImageKind_Image,
    PrLayoutImageKind_List,
    PrLayoutImageKind_Gradient,
    PrLayoutImageKind_Painter,
};
typedef uint8_t PrLayoutImageKind;

// The shape of a gradient (css-images-4/#gradients). A linear gradient's colors vary along a line through the image's
// center, a radial gradient's out from its center along rays to an ending shape, and a conic gradient's around its
// center.
enum {
    PrLayoutGradientKind_Linear,
    PrLayoutGradientKind_Radial,
    PrLayoutGradientKind_Conic,
};
typedef uint8_t PrLayoutGradientKind;

// The corner that a linear gradient's line points to (css-images-3/#linear-gradient-syntax). Its angle then depends on
// the image's size: the line is perpendicular to the diagonal between the two corners next to the one named. None
// points it by its angle instead.
enum {
    PrLayoutGradientCorner_None,
    PrLayoutGradientCorner_TopLeft,
    PrLayoutGradientCorner_TopRight,
    PrLayoutGradientCorner_BottomRight,
    PrLayoutGradientCorner_BottomLeft,
};
typedef uint8_t PrLayoutGradientCorner;

// The ending shape of a radial gradient (css-images-3/#radial-gradient-syntax).
enum {
    PrLayoutRadialShape_Ellipse,
    PrLayoutRadialShape_Circle,
};
typedef uint8_t PrLayoutRadialShape;

// How a radial gradient's ending shape is sized (css-images-3/#typedef-radial-size): to meet the sides or corners of
// the image nearest to or farthest from its center, or by radii that the gradient gives.
enum {
    PrLayoutRadialExtent_FarthestCorner,
    PrLayoutRadialExtent_ClosestSide,
    PrLayoutRadialExtent_ClosestCorner,
    PrLayoutRadialExtent_FarthestSide,
    PrLayoutRadialExtent_Radii,
};
typedef uint8_t PrLayoutRadialExtent;

// A color stop, or a transition hint between the stops around it (css-images-4/#color-stop-syntax).
enum {
    PrLayoutColorStopKind_Color,
    PrLayoutColorStopKind_Hint,
};
typedef uint8_t PrLayoutColorStopKind;

PR_NO_PADDING_BEGIN
// A gradient's color stop or transition hint. Its position is along the gradient line of a linear gradient or the ray
// of a radial gradient, where a percentage is of the line's or ray's length; a conic gradient's position is a
// percentage of a turn. An auto position places a color stop as css-images-4/#color-stop-fixup says. A color stop at
// none is at the position of the color stop before it, wherever that resolves, and makes a hard edge there; the fixup
// does not count it. A hint's position is neither auto nor none, and its color is not read.
typedef struct PrLayoutColorStop {
    PrLayoutLength position;
    PrColor16 color;                    // extended sRGB with straight alpha

    // The hue in degrees in the gradient's polar interpolation space, if the color states one there, for the reasons
    // that PrDrawColorStop gives. Used if `hasHue` is set.
    float hue;
    bool hasHue;

    PrLayoutColorStopKind kind;
    uint8_t _pad[2];
} PrLayoutColorStop;
PR_NO_PADDING_END

// A gradient (css-images-4/#gradients). Its colors mix in `space` (css-color-4/#interpolation-space), with hues taking
// `hueMethod` in a polar space. A repeating gradient repeats its stops' range along its line, ray or turn.
typedef struct PrLayoutGradient {
    PrLayoutGradientKind kind;
    PrColorSpace space;
    PrColorHueMethod hueMethod;
    bool repeating;

    // Linear: the direction of its line, as a corner, or else as `angle`. Conic: the turn at which its stops start, as
    // `angle`. Degrees clockwise from up.
    PrLayoutGradientCorner corner;
    float angle;

    // Radial: its ending shape, and how the shape is sized. Its radii are used if `extent` is Radii. A circle's radius
    // is `radiusX`, a length. An ellipse's radii may be percentages, of the image's width and height.
    PrLayoutRadialShape shape;
    PrLayoutRadialExtent extent;
    PrLayoutLength radiusX;
    PrLayoutLength radiusY;

    // Radial and conic: its center, where a percentage is of the image's width or height.
    PrLayoutLength centerX;
    PrLayoutLength centerY;

    // The stops and hints in order, at least two color stops among them. Hints come between color stops.
    uint32_t stopCount;
    const PrLayoutColorStop* stops;
} PrLayoutGradient;

// Returns a linear gradient from top to bottom, interpolating in Oklab, with no stops, and centered radial and conic
// fields: a farthest-corner ellipse and a conic gradient that starts up.
PrLayoutGradient prLayoutGradientDefault(void);

// Records `gradient` over `rect`, in a gradient box of `box` (css-images-3/#gradient-box), as a background paints it.
// The gradient's lengths must be pixels or percentages. A compound length (prLayoutLengthIntern) belongs to a tree,
// which this call has none of.
void prLayoutGradientDraw(const PrLayoutGradient* gradient, PrRect box, PrRect rect, PrDrawList* list);

// How a background image repeats along one axis to fill its painting area (css-backgrounds-3/#background-repeat): in
// whole copies from its position, spaced so that whole copies fill the area, scaled so that whole copies fill it, or
// once.
enum {
    PrLayoutBackgroundRepeat_Repeat,
    PrLayoutBackgroundRepeat_Space,
    PrLayoutBackgroundRepeat_Round,
    PrLayoutBackgroundRepeat_NoRepeat,
};
typedef uint8_t PrLayoutBackgroundRepeat;

// How a background image is sized (css-backgrounds-3/#background-size): by its lengths, or scaled to cover or to fit
// within its positioning area, keeping its ratio.
enum {
    PrLayoutBackgroundSize_Lengths,
    PrLayoutBackgroundSize_Cover,
    PrLayoutBackgroundSize_Contain,
};
typedef uint8_t PrLayoutBackgroundSize;

// One background layer (css-backgrounds-3/#backgrounds): an image, and how it is sized, placed, repeated, clipped and
// blended.
typedef struct PrLayoutBackgroundLayer {
    PrLayoutImageKind kind;
    PrLayoutImageRendering rendering;           // Image and List
    PrLayoutBackgroundRepeat repeatX;
    PrLayoutBackgroundRepeat repeatY;
    PrLayoutBackgroundAttachment attachment;
    PrLayoutBackgroundBox origin;               // the box that it is positioned and sized in
    PrLayoutBackgroundBox clip;                 // the box that it paints within
    PrLayoutBackgroundSize size;

    // How the layer composites onto the layers below it and the background color
    // (compositing-1/#background-blend-mode): its blend mode, and its operator, which is source-over or plus-lighter.
    // The background color and the layers are an isolated group if any layer blends otherwise than normally and
    // source-over. A mask layer does not read them.
    PrDrawBlendMode blendMode;
    PrDrawComposite composite;

    // Its size with `size` Lengths, where a percentage is of the positioning area. Auto takes the image's natural size
    // in that axis, or its ratio applied to the other axis.
    PrLayoutLength sizeX;
    PrLayoutLength sizeY;

    // Where the image is placed in its positioning area: a percentage places the point that far across the image at the
    // point that far across the area, and pixels move it from there (css-backgrounds-3/#background-position).
    PrLayoutLength positionX;
    PrLayoutLength positionY;

    // Image, List and Painter: the image's natural dimensions. Image: the brightest color that it paints, as a multiple
    // of SDR white, or zero for no limit (PrDrawImageFill::limit).
    PrLayoutNaturalSize natural;
    float limit;

    // The filter list of a filter() image (filter-effects-1/#FilterCSSImageValue), `filterCount` items in the order
    // they apply, or none. The list filters the image as it is drawn at its concrete object size, and its result is
    // clipped to that size. A blur extends the image's edges. The tree copies the items and holds a reference to each
    // source that they name.
    uint32_t filterCount;
    const PrLayoutFilter* PR_NULLABLE filters;

    union {
        const PrDrawImage* image;               // Image: the tree retains a reference
        PrDrawList* list;                       // List: in its natural space; the tree freezes and retains it
        PrLayoutGradient gradient;              // Gradient: the tree copies its stops

        // Painter: drawn at the tile's size. The tree retains a reference if the painter shares its context, which must
        // draw the same for the same size for as long as the layer is interned, since a layer is never invalidated.
        PrLayoutPainter painter;
    };
} PrLayoutBackgroundLayer;

// Returns a layer with no image, repeated, scrolling, positioned in the padding box and painted within the border box,
// at its natural size, at the top left, blending normally, and unfiltered.
PrLayoutBackgroundLayer prLayoutBackgroundLayerDefault(void);

// Interns a box's background layers, `count` of them from the top layer down, and returns their index. No layers is
// kPrLayoutBackgroundNone. A layer with no image is kept, since it takes a place in the layers' order. Equal lists are
// one entry. The decoration groups that name an entry keep it, and otherwise it stays valid until the next layout of
// the tree.
PrLayoutBackgroundIndex prLayoutBackgroundIntern(PrLayoutTree* tree, const PrLayoutBackgroundLayer* layers, uint32_t count);

// The number of layers of the background at `index`, and the layer at `layer`, valid while the entry is.
uint32_t prLayoutBackgroundLayerCount(const PrLayoutTree* tree, PrLayoutBackgroundIndex index);
PrLayoutBackgroundLayer prLayoutBackgroundLayer(const PrLayoutTree* tree, PrLayoutBackgroundIndex index, uint32_t layer);

// ---------------------------------------------------------------------------------------------------------------------
// Effects group

PR_NO_PADDING_BEGIN
// A box's transform, affine in the size of its reference box: `matrix`, with `width * perWidth + height * perHeight`
// added to its last column. The matrix maps column vectors (x, y, z, w) from the box's space, with the transform's
// origin at zero, to its parent's. The form is exact for a transform whose translations by a percentage of the box all
// apply before every perspective, which holds for any 2D transform.
typedef struct PrLayoutTransform {
    PrMatrix4 matrix;
    float perWidth[4];
    float perHeight[4];
} PrLayoutTransform;
PR_NO_PADDING_END

// Returns the identity transform.
PrLayoutTransform prLayoutTransformIdentity(void);

// Returns `transform`, acting on x and y, as a box transform that does not depend on the box's size.
PrLayoutTransform prLayoutTransformFromAffine2(PrAffine2 transform);

// Returns true if `transform` is the identity at every size.
bool prLayoutTransformIsIdentity(const PrLayoutTransform* transform);

// Returns the matrix of `transform` for a reference box of `extent`, in pixels.
PrMatrix4 prLayoutTransformResolve(const PrLayoutTransform* transform, PrVec2 extent);

// The box that a transform's percentages and its origin are relative to (css-transforms-1/#transform-box). For a box
// with CSS layout, fill-box is the content box, and stroke-box and view-box are the border box.
enum {
    PrLayoutTransformBox_ViewBox,
    PrLayoutTransformBox_ContentBox,
    PrLayoutTransformBox_BorderBox,
    PrLayoutTransformBox_FillBox,
    PrLayoutTransformBox_StrokeBox,
};
typedef uint8_t PrLayoutTransformBox;

PR_NO_PADDING_BEGIN
// How a box and its content composite: its opacity, its transform about its origin, the perspective of its children,
// and its stack level. A box with an opacity below one, a transform, or `layer` paints whole, as a layer that a
// compositor can move and fade without painting it again. Not inherited, and read by compositing and painting.
//
// The transform applies about its origin: painting composes T(origin) * transform * T(-origin) once the reference box
// resolves the origin. The origin's x and y are relative to the reference box, and its z is in pixels.
//
// A perspective applies to the transforms of the box's children, about the perspective origin, which is relative to the
// reference box (css-transforms-2/#perspective-property). A child's transform is then T(p) * perspective(distance) *
// T(-p) * T(offset) * T(origin) * transform * T(-origin), where `p` is the perspective origin and `offset` is the
// child's position in this box, scroll offset included. A transform that is not affine on the child's plane paints the
// child as a projected group, which flattens into this box's plane.
typedef struct PrLayoutEffectStyle {
    PrLayoutTransform transform;
    PrLayoutLength originX;
    PrLayoutLength originY;
    PrLayoutLength perspectiveOriginX;
    PrLayoutLength perspectiveOriginY;
    float originZ;
    float perspective;              // the distance to the eye in pixels, at least 1, or 0 for none
    float opacity;
    int32_t zIndex;                 // used if the box group's zIndexed is set (css-position-3/#stacking)
    bool layer;                     // paints whole at full opacity and no transform, as a box whose effects animate

    // The box paints nothing and is not hit while its transform, with its parent's perspective, shows the back of its
    // plane to the eye (css-transforms-2/#backface-visibility-property).
    bool backfaceHidden;

    PrLayoutTransformBox transformBox;
    uint8_t _pad;
} PrLayoutEffectStyle;
PR_NO_PADDING_END

// Returns full opacity, the identity transform about the center of the border box, and no perspective, whose origin is
// the center.
PrLayoutEffectStyle prLayoutEffectStyleDefault(void);

// Interns or creates an effects group and returns its index, or returns the effects group at `index`.
PrLayoutGroupIndex prLayoutEffectStyleIntern(PrLayoutTree* tree, const PrLayoutEffectStyle* effects);
PrLayoutGroupIndex prLayoutEffectStyleCreate(PrLayoutTree* tree, const PrLayoutEffectStyle* effects);
PrLayoutEffectStyle prLayoutEffectStyleRecord(const PrLayoutTree* tree, PrLayoutGroupIndex index);

// ---------------------------------------------------------------------------------------------------------------------
// Basic shapes

// The box that a basic shape, a clip path or a mask layer is placed in (css-masking-1/#typedef-geometry-box). For a box
// with CSS layout, fill-box is the content box, and stroke-box and view-box are the border box. The first three have
// the values of PrLayoutBackgroundBox.
enum {
    PrLayoutGeometryBox_BorderBox,
    PrLayoutGeometryBox_PaddingBox,
    PrLayoutGeometryBox_ContentBox,
    PrLayoutGeometryBox_MarginBox,
};
typedef uint8_t PrLayoutGeometryBox;

// The kinds of basic shape (css-shapes-1/#basic-shape-functions). rect() and xywh() compute to an inset().
enum {
    PrLayoutShapeKind_Inset,
    PrLayoutShapeKind_Circle,
    PrLayoutShapeKind_Ellipse,
    PrLayoutShapeKind_Polygon,
    PrLayoutShapeKind_Path,
};
typedef uint8_t PrLayoutShapeKind;

// The maximum number of vertices of a polygon that a tree interns. A longer polygon keeps its first vertices.
enum { kPrLayoutShapeMaxPoints = 65536 };

// A basic shape, placed in a reference box that the caller gives (css-shapes-1/#basic-shape-functions). Its kind
// decides which fields it uses. A percentage is of the reference box's width along x and of its height along y, except
// a circle's radius, which is of the box's normalized diagonal (css-shapes-1/#funcdef-basic-shape-circle).
typedef struct PrLayoutShape {
    PrLayoutShapeKind kind;
    PrPathFillRule rule;                // Polygon and Path

    // Circle and Ellipse: how each radius is sized, by `radiusX` or `radiusY` for an extent of Radii. A circle reads x
    // alone. An ellipse's corner extents take the side's distance scaled as an ellipse through the corner would be.
    PrLayoutRadialExtent extentX;
    PrLayoutRadialExtent extentY;
    PrLayoutLength radiusX;
    PrLayoutLength radiusY;

    // Circle and Ellipse: the center, from the reference box's top left.
    PrLayoutLength centerX;
    PrLayoutLength centerY;

    // Inset: the insets from the sides, by PrRectSide, and the corners' radii, horizontal then vertical for each corner
    // from the top left clockwise. A pair of insets that is larger than the box is reduced in proportion. The radii are
    // reduced as css-backgrounds-3/#corner-overlap reduces them.
    PrLayoutLength inset[PrRectSide_Count];
    PrLayoutLength radii[8];

    // Polygon: x and y of each vertex, from the reference box's top left, `pointCount` pairs. The tree copies them.
    uint32_t pointCount;
    const PrLayoutLength* PR_NULLABLE points;

    // Polygon: the radius that rounds each vertex (css-shapes-1/#funcdef-basic-shape-polygon), in pixels.
    float round;

    // Path: the outline in the reference box's space from its top left. The tree copies it.
    const PrPath* PR_NULLABLE path;
} PrLayoutShape;

// Returns an inset of zero with square corners.
PrLayoutShape prLayoutShapeDefault(void);

// Appends the outline of `shape` in the reference box `box` to `out`. A compound length that the shape names is read
// from `tree`, which interned it. Without a tree, the shape's lengths must be pixels or percentages. A shape whose
// outline encloses nothing appends nothing.
void prLayoutShapePath(const PrLayoutShape* shape, const PrLayoutTree* PR_NULLABLE tree, PrRect box, PrPath* out);

// An interned basic shape (prLayoutShapeIntern).
typedef uint32_t PrLayoutShapeIndex;

// No shape.
enum { kPrLayoutShapeNone = 0 };

// Interns a basic shape, and returns its index. Equal shapes are one entry. The compositing groups that name an entry
// keep it, and otherwise it stays valid until the next layout of the tree.
PrLayoutShapeIndex prLayoutShapeIntern(PrLayoutTree* tree, const PrLayoutShape* shape);

// ---------------------------------------------------------------------------------------------------------------------
// Masks

// How a mask layer's values mask (css-masking-1/#the-mask-mode): by their alpha, by their luminance times their alpha,
// or as the source says, which is alpha for an image.
enum {
    PrLayoutMaskMode_Alpha,
    PrLayoutMaskMode_Luminance,
    PrLayoutMaskMode_MatchSource,
};
typedef uint8_t PrLayoutMaskMode;

// A mask layer (css-masking-1/#positioned-masks): an image, or a source that draws a mask, how its values mask, and how
// it composites onto the layers below it (css-masking-1/#the-mask-composite). A layer with a source ignores its image,
// and the source places its mask itself.
typedef struct PrLayoutMaskLayer {
    // The image, sized, placed, repeated and clipped as a background layer is. Its attachment is not read. An image of
    // kind None is a transparent layer, which masks everything away unless a layer above it brings it back.
    PrLayoutBackgroundLayer image;

    PrLayoutSource source;
    PrLayoutMaskMode mode;
    PrDrawComposite composite;
    bool unclipped;                     // its painting area is not clipped (no-clip), in place of the image's clip
    uint8_t _pad;
} PrLayoutMaskLayer;

// Returns a layer with no image, at the defaults of PrLayoutBackgroundLayer, positioned in the border box, matching its
// source, and adding to the layers below it.
PrLayoutMaskLayer prLayoutMaskLayerDefault(void);

// A side of a mask border's width or outset (css-backgrounds-3/#border-image-width): a length, a multiple of the
// border's width on that side, or auto, which takes the width of the image's slice.
enum {
    PrLayoutBorderImageKind_Length,
    PrLayoutBorderImageKind_Number,
    PrLayoutBorderImageKind_Auto,
};
typedef uint8_t PrLayoutBorderImageKind;

// A mask border (css-masking-1/#mask-borders): an image cut into nine pieces, which mask the border image area as
// border-image draws them (css-backgrounds-3/#border-image-process). The middle masks nothing unless `fill` is set.
typedef struct PrLayoutMaskBorder {
    // The image. Its size, position, repetition, origin and clip are not read. A border of kind None masks nothing.
    PrLayoutBackgroundLayer image;

    // The slices, inward from each side by PrRectSide: pixels of the image, or percentages of its size.
    PrLayoutLength slice[PrRectSide_Count];
    bool fill;

    // The widths and the outsets, by PrRectSide. A length's percentage is of the border image area's width or height.
    // An outset is never auto.
    PrLayoutBorderImageKind widthKind[PrRectSide_Count];
    PrLayoutLength width[PrRectSide_Count];
    float widthNumber[PrRectSide_Count];
    PrLayoutBorderImageKind outsetKind[PrRectSide_Count];
    PrLayoutLength outset[PrRectSide_Count];
    float outsetNumber[PrRectSide_Count];

    PrDrawSliceMode horizontal;         // the top and bottom edges, and the middle
    PrDrawSliceMode vertical;           // the left and right edges, and the middle
    PrDrawMaskMode mode;
} PrLayoutMaskBorder;

// Returns a border with no image, slices of zero, auto widths, no outsets, stretched edges and alpha mode.
PrLayoutMaskBorder prLayoutMaskBorderDefault(void);

// A box's mask: its layers, from the top layer down, and its border, which masks after them.
typedef struct PrLayoutMask {
    uint32_t layerCount;
    const PrLayoutMaskLayer* PR_NULLABLE layers;    // the tree copies them, with their gradients' stops
    PrLayoutMaskBorder border;
} PrLayoutMask;

// An interned mask (prLayoutMaskIntern).
typedef uint32_t PrLayoutMaskIndex;

// No mask.
enum { kPrLayoutMaskNone = 0 };

// Interns a mask, and returns its index. A mask with no layers and a border of no image is kPrLayoutMaskNone. Equal
// masks are one entry. The compositing groups that name an entry keep it, and otherwise it stays valid until the next
// layout of the tree.
PrLayoutMaskIndex prLayoutMaskIntern(PrLayoutTree* tree, const PrLayoutMask* mask);

// ---------------------------------------------------------------------------------------------------------------------
// Compositing group

// What clips a box (css-masking-1/#the-clip-path): nothing, a basic shape in a reference box, the edges of a reference
// box with its corners, or a source.
enum {
    PrLayoutClipKind_None,
    PrLayoutClipKind_Shape,
    PrLayoutClipKind_Box,
    PrLayoutClipKind_Source,
};
typedef uint8_t PrLayoutClipKind;

PR_NO_PADDING_BEGIN
// How a box's content is clipped, masked, filtered and blended (css-masking-1, filter-effects-1, filter-effects-2,
// compositing-1). Not inherited, and read by compositing and hit testing. A box with a clip path, a mask, a filter, a
// backdrop filter, a blend mode or isolation makes a stacking context and is a group of its own. The group applies its
// filter, then its clip path, then its mask, then its opacity (css-masking-1/#placement), and blends onto what lies
// beneath. Layout reads whether `filter` or `backdrop` is set from the box group's `filtered`.
//
// `clip` is the legacy clip property's rectangle (css-masking-1/#clip-property): offsets of each side from the border
// box's top left, by PrRectSide, where auto is the border box's own edge. It applies only to an absolutely positioned
// box. It makes no group: the box clips as a container (PrLayoutBoxStyle::clipped).
typedef struct PrLayoutCompositingStyle {
    PrLayoutSource clipSource;          // Source
    PrLayoutShapeIndex clipShape;       // Shape
    PrLayoutMaskIndex mask;
    PrLayoutFilterIndex filter;
    PrLayoutFilterIndex backdrop;       // backdrop-filter (filter-effects-2/#BackdropFilterProperty)
    PrLayoutLength clip[PrRectSide_Count];
    PrLayoutClipKind clipKind;
    PrLayoutGeometryBox clipBox;        // Shape and Box
    bool clips;                         // `clip` is a rectangle rather than auto
    bool isolated;                      // isolation: isolate (compositing-1/#isolation)
    PrDrawBlendMode blendMode;          // mix-blend-mode (compositing-1/#mix-blend-mode)
    PrDrawComposite composite;          // source-over, or plus-lighter for that mix-blend-mode

    // The box is a backdrop root and a stacking context, though it applies nothing, because will-change names a
    // property that would make it one (filter-effects-2/#BackdropRoot, css-will-change-1/#will-change).
    bool backdropRoot;
    uint8_t _pad[1];
} PrLayoutCompositingStyle;
PR_NO_PADDING_END

// Returns no clip path, no mask, no filter, auto clip, normal blending and no isolation.
PrLayoutCompositingStyle prLayoutCompositingStyleDefault(void);

// Interns or creates a compositing group and returns its index, or returns the compositing group at `index`.
PrLayoutGroupIndex prLayoutCompositingStyleIntern(PrLayoutTree* tree, const PrLayoutCompositingStyle* compositing);
PrLayoutGroupIndex prLayoutCompositingStyleCreate(PrLayoutTree* tree, const PrLayoutCompositingStyle* compositing);
PrLayoutCompositingStyle prLayoutCompositingStyleRecord(const PrLayoutTree* tree, PrLayoutGroupIndex index);

// ---------------------------------------------------------------------------------------------------------------------
// Flex group

// How a flex container's items run (css-flexbox-2/#flex-direction-property): along its inline axis or its block axis,
// each forward or reversed.
enum {
    PrLayoutFlexDirection_Row,
    PrLayoutFlexDirection_RowReverse,
    PrLayoutFlexDirection_Column,
    PrLayoutFlexDirection_ColumnReverse,
};
typedef uint8_t PrLayoutFlexDirection;

// Whether a flex container's items wrap onto more lines, and which way the lines stack
// (css-flexbox-2/#flex-wrap-property).
enum {
    PrLayoutFlexWrap_NoWrap,
    PrLayoutFlexWrap_Wrap,
    PrLayoutFlexWrap_WrapReverse,
};
typedef uint8_t PrLayoutFlexWrap;

// A value of an alignment property (css-align-3/#alignment-values), with the overflow position given with it. `left`
// and `right` are physical sides.
enum {
    PrLayoutAlignment_Auto,         // takes the parent's justify-items or align-items
    PrLayoutAlignment_Normal,
    PrLayoutAlignment_Stretch,
    PrLayoutAlignment_Baseline,     // first baseline
    PrLayoutAlignment_LastBaseline,
    PrLayoutAlignment_Center,
    PrLayoutAlignment_Start,
    PrLayoutAlignment_End,
    PrLayoutAlignment_SelfStart,
    PrLayoutAlignment_SelfEnd,
    PrLayoutAlignment_FlexStart,
    PrLayoutAlignment_FlexEnd,
    PrLayoutAlignment_Left,
    PrLayoutAlignment_Right,
    PrLayoutAlignment_SpaceBetween,
    PrLayoutAlignment_SpaceAround,
    PrLayoutAlignment_SpaceEvenly,

    // The overflow position (css-align-3/#overflow-values), in the bits above the value.
    PrLayoutAlignmentFlag_Safe = 1u << 6,
    PrLayoutAlignmentFlag_Unsafe = 1u << 7,
};
typedef uint8_t PrLayoutAlignment;

// The bits of a PrLayoutAlignment that hold its value rather than its overflow position.
enum { kPrLayoutAlignmentValueMask = (1u << 6) - 1 };

PR_NO_PADDING_BEGIN
// How a flex container lays its items out, what a flex item gives the container that lays it out (css-flexbox-2), and
// how a box is aligned in its parent (css-align-3). Not inherited, and read by layout.
typedef struct PrLayoutFlexStyle {
    // What a flex item gives the container that lays it out: how it flexes, and its place in the container's order
    // (css-flexbox-2/#flex-property, css-display-4/#order-property).
    float grow;
    float shrink;
    PrLayoutLength basis;                       // auto, content, or a size
    int32_t order;

    // The gaps between a flex container's items and between its lines (css-gaps-1/#column-row-gap). Normal is zero.
    PrLayoutLength rowGap;
    PrLayoutLength columnGap;

    // The alignment properties (css-align-3). First a container's, which distribute its content and set its children's
    // default alignment, then a box's own, whose auto value takes its parent's default in the same axis.
    PrLayoutAlignment justifyContent;
    PrLayoutAlignment alignContent;
    PrLayoutAlignment justifyItems;
    PrLayoutAlignment alignItems;
    PrLayoutAlignment justifySelf;
    PrLayoutAlignment alignSelf;

    // How a flex container's items run: along which axis, and onto how many lines.
    PrLayoutFlexDirection direction : 2;
    PrLayoutFlexWrap wrap : 2;
    uint8_t _pad0 : 4;
    uint8_t _pad1;
} PrLayoutFlexStyle;
PR_NO_PADDING_END

// Returns each property's initial value.
PrLayoutFlexStyle prLayoutFlexStyleDefault(void);

// Interns or creates a flex group and returns its index, or returns the flex group at `index`.
PrLayoutGroupIndex prLayoutFlexStyleIntern(PrLayoutTree* tree, const PrLayoutFlexStyle* flex);
PrLayoutGroupIndex prLayoutFlexStyleCreate(PrLayoutTree* tree, const PrLayoutFlexStyle* flex);
PrLayoutFlexStyle prLayoutFlexStyleRecord(const PrLayoutTree* tree, PrLayoutGroupIndex index);

// ---------------------------------------------------------------------------------------------------------------------
// Fragmentation

// What a box asks of the break opportunity before it or after it (css-break-4/#break-between). Auto asks nothing. The
// avoid values ask that no break of their kind fall there, and Avoid asks it of every kind. The other values force a
// break: a page break, a page break that puts the next page on a given side, a column break, a break of the innermost
// fragmentation context for Always, and of every context for All.
enum {
    PrLayoutBreak_Auto,
    PrLayoutBreak_Avoid,
    PrLayoutBreak_AvoidPage,
    PrLayoutBreak_AvoidColumn,
    PrLayoutBreak_Always,
    PrLayoutBreak_All,
    PrLayoutBreak_Page,
    PrLayoutBreak_Left,
    PrLayoutBreak_Right,
    PrLayoutBreak_Recto,
    PrLayoutBreak_Verso,
    PrLayoutBreak_Column,
};
typedef uint8_t PrLayoutBreak;

// What a box asks of the break opportunities inside it (css-break-4/#break-within).
enum {
    PrLayoutBreakInside_Auto,           // nothing
    PrLayoutBreakInside_Avoid,          // that no break of any kind fall there
    PrLayoutBreakInside_AvoidPage,      // that no page break fall there
    PrLayoutBreakInside_AvoidColumn,    // that no column break fall there
};
typedef uint8_t PrLayoutBreakInside;

// How a box's edges paint where it breaks (css-break-4/#break-decoration): as one box sliced at the break, or with its
// border, padding and backgrounds on each piece.
enum {
    PrLayoutBoxDecorationBreak_Slice,
    PrLayoutBoxDecorationBreak_Clone,
};
typedef uint8_t PrLayoutBoxDecorationBreak;

// How a multi-column container fills its columns (css-multicol-1/#cf).
enum {
    PrLayoutColumnFill_Balance,     // balanced in each column set, except a last set in a definite height
    PrLayoutColumnFill_Auto,        // filled in turn
    PrLayoutColumnFill_BalanceAll,  // balanced in every set
};
typedef uint8_t PrLayoutColumnFill;

// Whether a box in a multi-column container spans all of its columns (css-multicol-1/#column-span).
enum {
    PrLayoutColumnSpan_None,
    PrLayoutColumnSpan_All,
};
typedef uint8_t PrLayoutColumnSpan;

// The most columns that a multi-column container lays out in one column set. A larger count is this many.
enum { kPrLayoutMaxColumns = 1000 };

PR_NO_PADDING_BEGIN
// Where a box breaks across fragmentainers (css-break-4), what a multi-column container's columns are (css-multicol-1),
// and which page a box goes on (css-page-3/#using-named-pages). Not inherited, and read by layout. The table group
// holds it (PrLayoutTableStyle::fragmentation).
//
// A block container is a multi-column container if its column count or its column width is not auto
// (css-multicol-1/#the-multi-column-model). The flex group's column gap is its gap between columns, and normal is 1em
// there (css-multicol-1/#column-gap). Its rule is the decoration group's.
typedef struct PrLayoutFragmentationStyle {
    PrLayoutLength columnWidth;     // auto or a length
    PrLayoutName page;              // kPrLayoutNameNone for auto
    uint16_t columnCount;           // 0 for auto
    PrLayoutBreak breakBefore;
    PrLayoutBreak breakAfter;
    PrLayoutBreakInside breakInside;
    PrLayoutBoxDecorationBreak boxDecorationBreak;
    PrLayoutColumnFill columnFill;
    PrLayoutColumnSpan columnSpan;
} PrLayoutFragmentationStyle;
PR_NO_PADDING_END

// Returns each property's initial value: no break asked for, slices at breaks, and no columns.
PrLayoutFragmentationStyle prLayoutFragmentationStyleDefault(void);

// ---------------------------------------------------------------------------------------------------------------------
// Table group

// Whether a table's cells have borders of their own, separated by the table's border spacing, or share borders that the
// table resolves among its parts (css-tables-3/#border-collapse-property).
enum {
    PrLayoutBorderCollapse_Separate,
    PrLayoutBorderCollapse_Collapse,
};
typedef uint8_t PrLayoutBorderCollapse;

// Which side of its table's grid a caption goes on (css-tables-3/#caption-side-property).
enum {
    PrLayoutCaptionSide_Top,
    PrLayoutCaptionSide_Bottom,
};
typedef uint8_t PrLayoutCaptionSide;

// Whether a table sizes its columns from its cells' content, or from its first row and its column boxes alone
// (css-tables-3/#table-layout-property). A table whose width is auto sizes its columns from their content either way.
enum {
    PrLayoutTableLayout_Auto,
    PrLayoutTableLayout_Fixed,
};
typedef uint8_t PrLayoutTableLayout;

// The largest number of columns that a cell can span, and the largest number of rows (html/#attr-tdth-colspan,
// html/#attr-tdth-rowspan).
enum { kPrLayoutTableMaxColumnSpan = 1000 };
enum { kPrLayoutTableMaxRowSpan = 65534 };

PR_NO_PADDING_BEGIN
// How a table lays out its parts, how many tracks a cell or a column spans, and where a box breaks across
// fragmentainers. Not inherited, and read by layout.
//
// The group holds both because few boxes have either: most boxes are not table parts, and most declare no break, column
// or page. Most records then name the default group, and a record holds one group index for both.
//
// CSS inherits border-collapse, border-spacing and caption-side. Layout reads each of them only on the box that it
// applies to: border-spacing and border-collapse on the table, and caption-side on the caption. A producer gives each
// box its computed values. An anonymous table takes the values of its parent.
//
// The spans are not CSS properties. The document language gives them (css-tables-3/#dimensioning-the-row-column-grid).
typedef struct PrLayoutTableStyle {
    PrLayoutUnit borderSpacing[2];  // horizontal and vertical, at least zero (css-tables-3/#border-spacing-property)
    uint16_t columnSpan;            // a cell, a column or a column group: 1 to kPrLayoutTableMaxColumnSpan
    uint16_t rowSpan;               // a cell: at most kPrLayoutTableMaxRowSpan, or 0 for the rest of its row group
    PrLayoutBorderCollapse borderCollapse;
    PrLayoutCaptionSide captionSide;
    PrLayoutTableLayout tableLayout;
    uint8_t _pad;
    PrLayoutFragmentationStyle fragmentation;
} PrLayoutTableStyle;
PR_NO_PADDING_END

// Returns each property's initial value, spans of one, and the initial fragmentation values.
PrLayoutTableStyle prLayoutTableStyleDefault(void);

// Interns or creates a table group and returns its index, or returns the table group at `index`.
PrLayoutGroupIndex prLayoutTableStyleIntern(PrLayoutTree* tree, const PrLayoutTableStyle* table);
PrLayoutGroupIndex prLayoutTableStyleCreate(PrLayoutTree* tree, const PrLayoutTableStyle* table);
PrLayoutTableStyle prLayoutTableStyleRecord(const PrLayoutTree* tree, PrLayoutGroupIndex index);

// ---------------------------------------------------------------------------------------------------------------------
// Grid group

// The furthest line from the explicit grid that a placement names, in either direction (css-grid-1/#overlarge-grids).
// The implicit grid holds the lines from -kPrLayoutGridMaxLine through kPrLayoutGridMaxLine, and a placement past them
// is clamped to them.
enum { kPrLayoutGridMaxLine = 10000 };

// A track sizing function (css-grid-1/#track-sizing): its minimum and maximum breadths, as minmax() gives them. A
// single breadth is both. A minimum of a flex factor is auto. fit-content(limit) is a minimum of auto and a maximum of
// fit-content, with the limit, a length, that caps it (css-grid-1/#funcdef-grid-template-columns-fit-content).
typedef struct PrLayoutGridTrack {
    PrLayoutTrackBreadth min;
    PrLayoutTrackBreadth max;
    PrLayoutLength limit;           // used if `max` is fit-content
} PrLayoutGridTrack;

// A name of the line at `line` of a track list, which counts the lines from 0 before the first track.
typedef struct PrLayoutGridLineName {
    uint32_t line;
    PrLayoutName name;
} PrLayoutGridLineName;

// How a track list's automatic repetition repeats its tracks (css-grid-1/#auto-repeat): not at all, as many times as
// fit, or as many times as fit with its empty tracks collapsed after placement.
enum {
    PrLayoutGridRepeat_None,
    PrLayoutGridRepeat_AutoFill,
    PrLayoutGridRepeat_AutoFit,
};
typedef uint8_t PrLayoutGridRepeat;

// A track list (css-grid-1/#track-sizing): its tracks and the names of its lines, with at most one automatic
// repetition. A repetition by a count is given as its tracks repeated, with the names of the lines between repetitions
// merged.
//
// The automatic repetition repeats `repeatCount` of the tracks from `repeatStart`. The names of the lines inside one
// repetition are in `repeatNames`, numbered from 0 before the repetition's first track to `repeatCount` after its last.
// The names in `names` at the lines that bound the repetition are the names outside it, and stay at those lines.
typedef struct PrLayoutGridTrackList {
    const PrLayoutGridTrack* PR_NULLABLE tracks;
    uint32_t trackCount;
    const PrLayoutGridLineName* PR_NULLABLE names;
    uint32_t nameCount;
    PrLayoutGridRepeat repeat;
    uint32_t repeatStart;
    uint32_t repeatCount;
    const PrLayoutGridLineName* PR_NULLABLE repeatNames;
    uint32_t repeatNameCount;
} PrLayoutGridTrackList;

// A track list, by its place in its tree's table of track lists.
typedef uint32_t PrLayoutGridTracksIndex;

// The track list with no tracks: none, as an explicit grid's tracks, and a single auto track, as implicit tracks.
enum { kPrLayoutGridTracksNone = 0 };

// Interns a track list and returns its index. A list with no tracks is kPrLayoutGridTracksNone. Equal lists are one
// entry. The grid groups that name an entry keep it, and otherwise it stays valid until the next layout of the tree.
PrLayoutGridTracksIndex prLayoutGridTracksIntern(PrLayoutTree* tree, const PrLayoutGridTrackList* list);

// A named grid area (css-grid-1/#grid-template-areas-property): its name, and the lines that bound it, counted from 0
// at the start of the explicit grid.
typedef struct PrLayoutGridArea {
    PrLayoutName name;
    uint16_t rowStart;
    uint16_t rowEnd;
    uint16_t columnStart;
    uint16_t columnEnd;
} PrLayoutGridArea;

// The named areas of a grid template, which has `rowCount` rows and `columnCount` columns, each at most
// kPrLayoutGridMaxLine.
typedef struct PrLayoutGridAreas {
    uint32_t rowCount;
    uint32_t columnCount;
    const PrLayoutGridArea* PR_NULLABLE areas;
    uint32_t areaCount;
} PrLayoutGridAreas;

// A grid template's areas, by their place in their tree's table of areas.
typedef uint32_t PrLayoutGridAreasIndex;

// No template areas.
enum { kPrLayoutGridAreasNone = 0 };

// Interns the areas of a grid template and returns their index. A template with no rows or no columns is
// kPrLayoutGridAreasNone. Equal templates are one entry, which lives as an interned track list does.
PrLayoutGridAreasIndex prLayoutGridAreasIntern(PrLayoutTree* tree, const PrLayoutGridAreas* areas);

// What a grid-placement property contributes to an item's placement (css-grid-1/#line-placement): nothing, a line, or a
// span.
enum {
    PrLayoutGridLineKind_Auto,
    PrLayoutGridLineKind_Line,
    PrLayoutGridLineKind_Span,
};
typedef uint8_t PrLayoutGridLineKind;

PR_NO_PADDING_BEGIN
// A value of a grid-placement property. A line is the `integer`th line, counting only the lines named `name` if it has
// a name, and counting back from the end of the explicit grid if `integer` is negative. A name with an integer of zero
// is a <custom-ident> alone, which names an area's edge before it names a line. A span is `integer` lines, of those
// named `name` if it has a name. An integer is clamped to kPrLayoutGridMaxLine.
typedef struct PrLayoutGridLine {
    PrLayoutName name;
    int16_t integer;
    PrLayoutGridLineKind kind;
    uint8_t _pad;
} PrLayoutGridLine;
PR_NO_PADDING_END

// How auto-placement places a grid container's items (css-grid-1/#grid-auto-flow-property): filling each column in turn
// rather than each row, and filling the holes that earlier items left.
enum {
    PrLayoutGridAutoFlowFlag_Column = 1u << 0,
    PrLayoutGridAutoFlowFlag_Dense = 1u << 1,
};
typedef uint8_t PrLayoutGridAutoFlowFlags;

PR_NO_PADDING_BEGIN
// How a grid container defines its grid and places its items, and where a grid item goes in its container's grid
// (css-grid-1). Not inherited, and read by layout.
//
// A grid container reads the gaps, the alignment properties and the order of its items from the flex group.
typedef struct PrLayoutGridStyle {
    // A grid container's explicit grid (css-grid-1/#explicit-grids), and the sizes of the tracks that its templates do
    // not size (css-grid-1/#auto-tracks), whose list repeats as needed.
    PrLayoutGridTracksIndex templateColumns;
    PrLayoutGridTracksIndex templateRows;
    PrLayoutGridAreasIndex templateAreas;
    PrLayoutGridTracksIndex autoColumns;
    PrLayoutGridTracksIndex autoRows;

    // Where a grid item goes, or the area that is the containing block of a box out of flow whose containing block is a
    // grid container (css-grid-1/#abspos-items).
    PrLayoutGridLine columnStart;
    PrLayoutGridLine columnEnd;
    PrLayoutGridLine rowStart;
    PrLayoutGridLine rowEnd;

    PrLayoutGridAutoFlowFlags autoFlow;
    uint8_t _pad[3];
} PrLayoutGridStyle;
PR_NO_PADDING_END

// Returns each property's initial value: no explicit grid, auto implicit tracks, auto placement, and rows filled in
// turn.
PrLayoutGridStyle prLayoutGridStyleDefault(void);

// Interns or creates a grid group and returns its index, or returns the grid group at `index`.
PrLayoutGroupIndex prLayoutGridStyleIntern(PrLayoutTree* tree, const PrLayoutGridStyle* grid);
PrLayoutGroupIndex prLayoutGridStyleCreate(PrLayoutTree* tree, const PrLayoutGridStyle* grid);
PrLayoutGridStyle prLayoutGridStyleRecord(const PrLayoutTree* tree, PrLayoutGroupIndex index);

// ---------------------------------------------------------------------------------------------------------------------
// Math group

// The layout algorithm of a math box (mathml-core/#layout-algorithms, mathml4/#presm). A box whose in-flow children do
// not fit its algorithm, such as an msub without exactly two, is laid out as a row, as MathML Core specifies for
// invalid markup. A box whose display is not a math display has no math layout of its own, and its math parent measures
// it by its box. A table cell of any kind but None, such as an mtd, lays out its content as a row.
enum {
    PrLayoutMathKind_None,              // not a MathML element, laid out as a row by a math display
    PrLayoutMathKind_Row,               // mrow and the elements laid out as one, such as math and mstyle
    PrLayoutMathKind_Token,             // mi, mn, mtext and ms, laid out in flow and measured by their ink
    PrLayoutMathKind_Operator,          // mo, a token that may stretch or be drawn large
    PrLayoutMathKind_Space,             // mspace
    PrLayoutMathKind_Fraction,          // mfrac
    PrLayoutMathKind_SquareRoot,        // msqrt
    PrLayoutMathKind_Root,              // mroot
    PrLayoutMathKind_Padded,            // mpadded
    PrLayoutMathKind_Subscript,         // msub
    PrLayoutMathKind_Superscript,       // msup
    PrLayoutMathKind_SubSuperscript,    // msubsup
    PrLayoutMathKind_Under,             // munder
    PrLayoutMathKind_Over,              // mover
    PrLayoutMathKind_UnderOver,         // munderover
    PrLayoutMathKind_Multiscripts,      // mmultiscripts
    PrLayoutMathKind_Prescripts,        // mprescripts, a row that separates the prescripts of an mmultiscripts
    PrLayoutMathKind_Table,             // mtable, a table box aligned by PrLayoutMathStyle::tableAlign
    PrLayoutMathKind_Enclose,           // menclose (mathml4/#presm_menclose)
    PrLayoutMathKind_Fenced,            // mfenced (mathml4/#presm_mfenced), a row between the fences and separators of
                                        // PrLayoutMathStyle::fences
    PrLayoutMathKind_Stack,             // mstack (mathml4/#presm_elementary), rows of digits aligned in columns
    PrLayoutMathKind_LongDivision,      // mlongdiv, a stack with a divisor and a result
};
typedef uint8_t PrLayoutMathKind;

// The properties that an embellished operator (mathml-core/#embellished-operators) takes from its core operator. The
// producer resolves them from the core operator's attributes and the operator dictionary
// (mathml-core/#dictionary-based-attributes). A row sizes a fence as TeX sizes a delimiter, to at least 90.1% of what
// it encloses and at most half an em short of it (TeXbook Appendix G, rule 19, \delimiterfactor and
// \delimitershortfall). A prefix fence and the postfix fence that matches it enclose a group that no line break splits.
enum {
    PrLayoutMathOperatorFlag_Stretchy = 1u << 0,
    PrLayoutMathOperatorFlag_Symmetric = 1u << 1,
    PrLayoutMathOperatorFlag_LargeOp = 1u << 2,
    PrLayoutMathOperatorFlag_MovableLimits = 1u << 3,
    PrLayoutMathOperatorFlag_Fence = 1u << 4,
    PrLayoutMathOperatorFlag_InlineAxis = 1u << 5,      // stretches inline (mathml-core/#stretchy-operator-axis)
    PrLayoutMathOperatorFlag_Prefix = 1u << 6,          // the form is prefix (mathml-core/#dfn-form)
    PrLayoutMathOperatorFlag_Postfix = 1u << 7,         // the form is postfix
};
typedef uint8_t PrLayoutMathOperatorFlags;

// Flags of an munder, mover or munderover (mathml-core/#underscripts-and-overscripts-munder-mover-munderover) and of an
// mfrac (mathml4/#presm_mfrac).
enum {
    PrLayoutMathScriptFlag_Accent = 1u << 0,            // the overscript is an accent
    PrLayoutMathScriptFlag_AccentUnder = 1u << 1,       // the underscript is an accent
    PrLayoutMathScriptFlag_Bevelled = 1u << 2,          // the fraction is bevelled
};
typedef uint8_t PrLayoutMathScriptFlags;

// The inline-axis alignment, as MathML 4 defines it, of the numerator and the denominator of a fraction (numalign,
// denomalign), of the scripts of an munder, mover or munderover (align), and of the rows of a stack (stackalign).
enum {
    PrLayoutMathAlign_Center,
    PrLayoutMathAlign_Left,
    PrLayoutMathAlign_Right,
    PrLayoutMathAlign_DecimalPoint,     // the rows of a stack on their decimal points (mathml4/#presm_mstack)
};
typedef uint8_t PrLayoutMathAlign;

// The role of an item of an mstack or an mlongdiv (mathml4/#presm_elementary). The rows, groups and carries of a stack
// pass their children through to it as items, as display: contents does. The producer marks each item with its part,
// whether it starts a row, and the position of its row.
enum {
    PrLayoutMathStackPart_None,         // not an item of a stack
    PrLayoutMathStackPart_Number,       // an mn, with a column for each character
    PrLayoutMathStackPart_Digit,        // any other element of a row, which takes one column
    PrLayoutMathStackPart_Carry,        // a carry, borrow or crossout of a column of the next row
    PrLayoutMathStackPart_Line,         // an msline
    PrLayoutMathStackPart_Divisor,      // the divisor of an mlongdiv
    PrLayoutMathStackPart_Result,       // the result of an mlongdiv
};
typedef uint8_t PrLayoutMathStackPart;

// Where a carry sits beside the character of its column, as a compass direction (mathml4/#presm_mscarries).
enum {
    PrLayoutMathCarryLocation_N,
    PrLayoutMathCarryLocation_Ne,
    PrLayoutMathCarryLocation_E,
    PrLayoutMathCarryLocation_Se,
    PrLayoutMathCarryLocation_S,
    PrLayoutMathCarryLocation_Sw,
    PrLayoutMathCarryLocation_W,
    PrLayoutMathCarryLocation_Nw,
};
typedef uint8_t PrLayoutMathCarryLocation;

// Whether an item starts a row of its stack, and how a carry crosses out the character of its column.
enum {
    PrLayoutMathStackFlag_RowStart = 1u << 0,
    PrLayoutMathStackFlag_UpDiagonalStrike = 1u << 1,
    PrLayoutMathStackFlag_DownDiagonalStrike = 1u << 2,
    PrLayoutMathStackFlag_VerticalStrike = 1u << 3,
    PrLayoutMathStackFlag_HorizontalStrike = 1u << 4,
};
typedef uint8_t PrLayoutMathStackFlags;

// Where an operator goes when a line breaks at it (mathml4/#presm_linebreaking). Auto is After in inline math because
// TeX breaks a formula in a paragraph after its relations and binary operators.
enum {
    PrLayoutMathBreakStyle_Auto,        // Before in display math, and After in inline math
    PrLayoutMathBreakStyle_Before,      // at the start of the next line
    PrLayoutMathBreakStyle_After,       // at the end of the line that the break ends
    PrLayoutMathBreakStyle_Duplicate,   // at both
};
typedef uint8_t PrLayoutMathBreakStyle;

// The alignment of the line after a break within the formula's width (mathml4/#presm_linebreaking, indentalign).
enum {
    PrLayoutMathIndent_Auto,            // as the formula decides
    PrLayoutMathIndent_Left,
    PrLayoutMathIndent_Center,
    PrLayoutMathIndent_Right,
};
typedef uint8_t PrLayoutMathIndent;

// The limits of PrLayoutMathStyle::breakPenalty. A penalty between them is the cost, as in TeX, that a break at the box
// adds to a breaking of the formula.
enum {
    kPrLayoutMathForcedBreak = -10000,  // forces a break
    kPrLayoutMathNoBreak = 10000,       // forbids a break
};

// The styles of long division (mathml4/#presm_mlongdiv), named for where the divisor and the result go beside the
// dividend.
enum {
    PrLayoutMathLongDivision_LeftTop,
    PrLayoutMathLongDivision_StackedRightRight,
    PrLayoutMathLongDivision_MediumStackedRightRight,
    PrLayoutMathLongDivision_ShortStackedRightRight,
    PrLayoutMathLongDivision_RightTop,
    PrLayoutMathLongDivision_LeftSlashRight,
    PrLayoutMathLongDivision_LeftParenRight,
    PrLayoutMathLongDivision_ColonRightEqualRight,
    PrLayoutMathLongDivision_StackedLeftLeft,
    PrLayoutMathLongDivision_StackedLeftLineTop,
};
typedef uint8_t PrLayoutMathLongDivision;

// The pseudo-unit of the inner box that a dimension of an mpadded is a multiple of (mathml4/#presm_mpadded).
enum {
    PrLayoutMathPseudo_None,
    PrLayoutMathPseudo_Width,
    PrLayoutMathPseudo_Height,
    PrLayoutMathPseudo_Depth,
};
typedef uint8_t PrLayoutMathPseudo;

// How a dimension of an mpadded relates to the inner box's value. MathML Core treats a percentage as absent
// (mathml-core/#inner-box-and-requested-parameters).
enum {
    PrLayoutMathPaddingMode_Absent,     // the inner box's value, or zero for lspace and voffset
    PrLayoutMathPaddingMode_Set,        // the given value
    PrLayoutMathPaddingMode_Increase,   // the inner box's value plus the given value
    PrLayoutMathPaddingMode_Decrease,   // the inner box's value minus the given value
};
typedef uint8_t PrLayoutMathPaddingMode;

PR_NO_PADDING_BEGIN
// A dimension of an mpadded: `factor` times the inner box's pseudo-unit, plus `length`.
typedef struct PrLayoutMathPadding {
    float factor;
    PrLayoutUnit length;
    PrLayoutMathPseudo pseudo;
    PrLayoutMathPaddingMode mode;
    uint8_t _pad[2];
} PrLayoutMathPadding;
PR_NO_PADDING_END

// The dimensions of an mpadded, which index PrLayoutMathStyle::padding.
enum {
    PrLayoutMathPadded_Width,
    PrLayoutMathPadded_Height,
    PrLayoutMathPadded_Depth,
    PrLayoutMathPadded_LeftSpace,
    PrLayoutMathPadded_VerticalOffset,
    PrLayoutMathPadded_Count,
};
typedef uint8_t PrLayoutMathPadded;

// The part of an mtable that sits on the baseline of its parent (mathml4/#presm_mtable).
enum {
    PrLayoutMathTableAlign_Axis,        // the center, on the math axis rather than the baseline, as in MathML Core
    PrLayoutMathTableAlign_Center,
    PrLayoutMathTableAlign_Top,
    PrLayoutMathTableAlign_Bottom,
    PrLayoutMathTableAlign_Baseline,    // the baseline of the row that PrLayoutMathStyle::tableRow names
};
typedef uint8_t PrLayoutMathTableAlign;

// The notations of an menclose (mathml4/#presm_menclose), drawn around or over its content.
enum {
    PrLayoutMathNotationFlag_LongDivision = 1u << 0,
    PrLayoutMathNotationFlag_Actuarial = 1u << 1,
    PrLayoutMathNotationFlag_PhasorAngle = 1u << 2,
    PrLayoutMathNotationFlag_Radical = 1u << 3,
    PrLayoutMathNotationFlag_Box = 1u << 4,
    PrLayoutMathNotationFlag_RoundedBox = 1u << 5,
    PrLayoutMathNotationFlag_Circle = 1u << 6,
    PrLayoutMathNotationFlag_Left = 1u << 7,
    PrLayoutMathNotationFlag_Right = 1u << 8,
    PrLayoutMathNotationFlag_Top = 1u << 9,
    PrLayoutMathNotationFlag_Bottom = 1u << 10,
    PrLayoutMathNotationFlag_UpDiagonalStrike = 1u << 11,
    PrLayoutMathNotationFlag_DownDiagonalStrike = 1u << 12,
    PrLayoutMathNotationFlag_VerticalStrike = 1u << 13,
    PrLayoutMathNotationFlag_HorizontalStrike = 1u << 14,
    PrLayoutMathNotationFlag_Madruwb = 1u << 15,
    PrLayoutMathNotationFlag_UpDiagonalArrow = 1u << 16,
};
typedef uint32_t PrLayoutMathNotationFlags;

PR_NO_PADDING_BEGIN
// A fence or a separator of an mfenced, which the box draws without an element of its own. The producer resolves its
// properties as for the mo of the expanded form (mathml4/#presm_mfenced). Its spaces are in px.
typedef struct PrLayoutMathFence {
    uint32_t codePoint;                 // 0 for none
    PrLayoutUnit leftSpace;
    PrLayoutUnit rightSpace;
    PrLayoutMathOperatorFlags flags;
    uint8_t _pad[3];
} PrLayoutMathFence;
PR_NO_PADDING_END

// The indices of PrLayoutMathStyle::fences.
enum {
    PrLayoutMathFenceSlot_Open,
    PrLayoutMathFenceSlot_Close,
    PrLayoutMathFenceSlot_Separators,   // the first separator, followed by the rest
};
typedef uint8_t PrLayoutMathFenceSlot;

// The maximum number of fences and separators that an mfenced keeps, two fences and six separators. The last separator
// repeats after them, as the last character of the separators attribute does.
enum { kPrLayoutMathMaxFences = 8 };

PR_NO_PADDING_BEGIN
// How a math box lays out its children (mathml-core/#layout-algorithms). The producer resolves the values from the
// element's attributes and the operator dictionary. Not inherited, and read by layout. A box's math parent also reads
// its kind and its embellishment.
//
// An embellished operator has the properties of its core operator, which its math parent reads to space and stretch it.
// The producer resolves its spaces to px with the core operator's font. A size limit is in px or a percentage of the
// unstretched glyph's size, and a maximum of none means no maximum.
typedef struct PrLayoutMathStyle {
    PrLayoutMathKind kind;
    bool embellished;
    PrLayoutMathOperatorFlags operatorFlags;
    PrLayoutMathScriptFlags scriptFlags;
    PrLayoutUnit leftSpace;
    PrLayoutUnit rightSpace;
    PrLayoutLength minSize;
    PrLayoutLength maxSize;

    // Fraction: the thickness of the bar, in px, as a percentage of the font's FractionRuleThickness, or auto for that
    // thickness (mathml-core/#fractions-mfrac), and the alignment of the numerator and the denominator.
    PrLayoutLength lineThickness;
    PrLayoutMathAlign numeratorAlign;
    PrLayoutMathAlign denominatorAlign;

    PrLayoutMathAlign scriptAlign;                         // Under, Over and UnderOver
    PrLayoutMathTableAlign tableAlign;                     // Table
    int32_t tableRow;                                      // Table: the row that Baseline aligns, counted from 1 at the
                                                           // top or from -1 at the bottom, or 0 for none
    PrLayoutUnit spaceAscent;                              // Space: the requested line-ascent
                                                           // (mathml-core/#space-mspace)
    PrLayoutMathPadding padding[PrLayoutMathPadded_Count]; // Padded
    PrLayoutMathNotationFlags notations;                   // Enclose

    // Fenced: the fences, then `separatorCount` separators.
    PrLayoutMathFence fences[kPrLayoutMathMaxFences];
    uint8_t separatorCount;

    // Stack and LongDivision: the column that the rows align on, the side of its column that a narrower character goes
    // to, the style of a long division, the space between columns in px, and the character that DecimalPoint aligns on.
    PrLayoutMathAlign stackAlign;
    PrLayoutMathAlign charAlign;
    PrLayoutMathLongDivision longDivision;
    PrLayoutUnit charSpacing;
    uint32_t decimalPoint;

    // An item of a stack: its part, its flags, where it sits if it is a carry, and the shift of its row in columns
    // toward the left (mathml4/#presm_msgroup). A Line spans `lineColumns` columns, or every column if 0. It extends
    // `overhangs` px past them on the left and the right, and is `lineThickness` thick.
    PrLayoutMathStackPart stackPart;
    PrLayoutMathStackFlags stackFlags;
    PrLayoutMathCarryLocation carryLocation;
    uint8_t _pad2;
    int32_t stackPosition;
    uint32_t lineColumns;
    PrLayoutUnit overhangs[2];

    // An embellished operator or an mspace: the penalty of a line break at it, where the operator goes at such a break,
    // and the alignment of the next line, shifted by `indentShift` px (mathml4/#presm_linebreaking). The first line of
    // a broken formula is aligned by the indentalignfirst and indentshiftfirst of the operator that ends it, and the
    // last line by the indentalignlast and indentshiftlast of the operator that starts it. Lines are `lineLeading` px
    // apart, or as the formula decides if it is negative. An invisible times at a break is drawn as `breakChar` unless
    // it is 0.
    int32_t breakPenalty;
    PrLayoutMathBreakStyle breakStyle;
    PrLayoutMathIndent indent;
    PrLayoutMathIndent indentFirst;
    PrLayoutMathIndent indentLast;
    PrLayoutUnit indentShift;
    PrLayoutUnit indentShiftFirst;
    PrLayoutUnit indentShiftLast;
    PrLayoutUnit lineLeading;
    uint32_t breakChar;
} PrLayoutMathStyle;
PR_NO_PADDING_END

// Returns no kind, no embellished operator, with no spaces, no minimum, no maximum, the font's line thickness, centered
// parts, absent padding, a table on the axis, no notations and no line break.
PrLayoutMathStyle prLayoutMathStyleDefault(void);

// Interns or creates a math group and returns its index, or returns the math group at `index`.
PrLayoutGroupIndex prLayoutMathStyleIntern(PrLayoutTree* tree, const PrLayoutMathStyle* math);
PrLayoutGroupIndex prLayoutMathStyleCreate(PrLayoutTree* tree, const PrLayoutMathStyle* math);
PrLayoutMathStyle prLayoutMathStyleRecord(const PrLayoutTree* tree, PrLayoutGroupIndex index);

// ---------------------------------------------------------------------------------------------------------------------
// Lengths

// Interns a calculation of lengths and percentages (css-values-4/#calculation-tree), given as `count` nodes in
// preorder, and returns the length it is. The length is the pixels it computes to if it reads no percentage, 0 if it
// computes NaN, a percentage if it is one that a word holds exactly, and otherwise a compound value. The groups whose
// lengths name a compound keep it until a layout of the tree frees it. Equal calculations are one compound. A malformed
// calculation, or one whose types do not make a length, is auto.
PrLayoutLength prLayoutLengthIntern(PrLayoutTree* tree, const PrLayoutCalcNode* nodes, uint32_t count);

// Returns the units `length` resolves to against `base`: its pixels, its percentage of the base, or what its
// calculation computes with the base. A keyword is zero.
PrLayoutUnit prLayoutLengthResolve(const PrLayoutTree* tree, PrLayoutLength length, PrLayoutUnit base);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
