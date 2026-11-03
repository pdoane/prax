// Shaping: the items of a run of inline-level siblings, and the parts of its text that do not depend on the line width:
// the text with white space collapsed and transformed, its break opportunities and its glyphs.
//
// css-text-3 - https://drafts.csswg.org/css-text-3/
//
// The tree caches a run's shaping on the run's first node. A layout reuses the cached shaping while the run holds the
// same nodes and none of them has changed, so a new width breaks lines without shaping again. A change inside an atomic
// inline or a float does not invalidate the shaping, because its own cache lays it out. Before a layout, the runs that
// the layout will lay out again are shaped together, ahead of it.
//
// lib/text itemizes and shapes the text, and finds its break opportunities (pr/text_shaping.h). A shaping describes its
// items to lib/text, one piece for each, and reads the results through it.
//
// A shaping is immutable once shaped. Layout and measurement resolve each item's style and sizes into scratch memory of
// their own.

#pragma once

#include "pr/array.h"
#include "pr/text_break.h"
#include "pr/text_itemize.h"
#include "pr/text_shaping.h"
#include "pr/unicode_bidi.h"
#include "pr/unicode_line_break.h"

#include "layout/src/layout_box.h"
#include "layout/src/layout_space.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// ---------------------------------------------------------------------------------------------------------------------
// Items

// What an item of an inline formatting context is.
enum {
    LayoutInlineItemKind_Text,
    LayoutInlineItemKind_Open,      // an inline box's start
    LayoutInlineItemKind_Close,     // an inline box's end
    LayoutInlineItemKind_Atomic,    // an atomic inline: a replaced element, an inline-block or an inline table
    LayoutInlineItemKind_Float,     // a float, at its place in the text
    LayoutInlineItemKind_Break,     // a line break node: a forced break, which nothing of its own style aligns
    LayoutInlineItemKind_Block,     // a block-level box in flow in an inline box, which ends a segment
    LayoutInlineItemKind_OutOfFlow, // a box out of flow, at its place in the text, which its placeholder marks
};
typedef uint8_t LayoutInlineItemKind;

// One item, in tree order.
struct LayoutInlineItem {
    LayoutInlineItemKind kind;
    bool fallback;                  // Text: true if some glyphs are in a face other than the font's first available

    // Open, Close: true if the box's edge here interrupts shaping between the text on either side of it
    // (css-text-3/#boundary-shaping). This is the case if the box has a non-zero inline-axis margin, border or padding
    // here, has a vertical alignment other than its initial one, or isolates its content.
    bool breaksShaping;

    // Open, Close: the box's unicode-bidi, whose controls it adds around its content
    // (css-writing-modes-4/#unicode-bidi). Atomic: the unicode-bidi of a replaced element, which makes the item strong
    // in its direction if it embeds or overrides (css-writing-modes-4/#bidi-atomic-inlines), and normal for any other
    // atomic inline.
    PrTextBidi unicodeBidi;

    // Atomic: true if the item is the anonymous inline table that wraps the table parts from `node` through `runLast`,
    // which an inline box holds (css-tables-3/#fixup-algorithm). It has the initial box style, and inherits from the
    // inline box.
    bool wraps;

    LayoutNodeIndex node;           // the text node, or the inline box's, atomic inline's or block-level box's element
    LayoutNodeIndex runLast;        // Atomic that wraps: the last node of its run

    // The font group of the record that styles the item, which for a text item is the record of its parent. Shaping
    // reads it. A change to the group changes the node's record, which invalidates the shaping.
    PrLayoutGroupIndex font;

    uint32_t start;                 // Text, Atomic, Break: its bytes of the collapsed text; others: its place in it
    uint32_t end;
    uint32_t glyphStart;            // Text: its glyphs
    uint32_t glyphEnd;
    uint32_t runStart;              // Text: the shaping runs that hold its bytes
    uint32_t runEnd;
};

// Returns the box style of the atomic inline or inline box of `item`, whose record is `style`. An anonymous inline
// table has the initial box style.
inline const PrLayoutBoxStyle& layoutInlineItemBox(const LayoutStyles* styles, const LayoutInlineItem& item,
        PrLayoutStyleIndex style) {
    return item.wraps ? layoutAnonymousTableStyle() : layoutBoxStyle(styles, style);
}

// A shaping run: the unit that shaping works in (css-text-3/#boundary-shaping). It is a maximal range of a segment's
// text in one shaping style, face, script and orientation, with no boundary between its bytes that interrupts shaping,
// and no preserved tab or line feed, which are shaped into nothing. It may span items: a glyph belongs to the item that
// holds the first byte of its cluster. Its context is the text around it that shaping reads to join characters and
// choose forms. The context extends across a change that only needs another shaper call, such as a change of face or
// script, and stops at a boundary that interrupts shaping or at the segment's bounds.
struct LayoutShapingRun {
    uint32_t start;                 // its bytes
    uint32_t end;
    uint32_t contextStart;          // the bytes its context spans, around its own
    uint32_t contextEnd;
    uint32_t glyphStart;            // its glyphs in text order; a right-to-left run's are reversed from shaping's
    uint32_t glyphEnd;
    PrLayoutGroupIndex font;        // the font group of its first item, whose shaping style every item of it has
    LayoutFaceIndex face;           // its place among the faces the font group matches
    PrScript script;                // resolved (UAX24): Zzzz if its text has no script but Common and Inherited
    uint8_t level;                  // its bidi embedding level (UAX9); shaped right to left if odd

    // True if its glyphs are upright on a vertical line, shaped top to bottom (css-writing-modes-4/#text-orientation).
    // Other runs are shaped as on a horizontal line, and turned with the line if it is vertical.
    bool upright;
};

// The items and bytes of a segment of a run. A Block item ends a segment, and the next segment starts after it, so the
// segments are the run's items before the first Block item, between two, and after the last. Each segment is an inline
// formatting context of its own, in an anonymous block box (CSS2/#anonymous-block-level). Break opportunities, the
// context a text run is shaped in, and measurement stop at a segment's bounds.
struct LayoutInlineSegment {
    uint32_t byteStart;
    uint32_t byteEnd;
    uint32_t itemStart;
    uint32_t itemEnd;               // the Block item after its last item, if one follows
};

// No mark: an item that adds no isolate initiator or terminator (LayoutInlineShaping::marks).
constexpr uint8_t kLayoutMarkNone = 0xff;

// Where reordering a paragraph on one unbroken line splits an inline box (css-writing-modes-4/#bidi-box-model): the
// box's Open item, and the byte before which its content splits.
struct LayoutBidiSplit {
    uint32_t open;
    uint32_t byte;
};

// The shaping of a run: its nodes in order, their text with white space collapsed and text transformed as each style
// says, the items, the break opportunities, and the glyphs. A preserved segment break is a line feed, which forces a
// line break. Shaping gives no glyph for a line feed or a preserved tab.
struct LayoutInlineShaping {
    PrLayoutGroupIndex containerFont; // the font group of the container's record, which its own text runs are set in

    // True if the items are in the records of the first formatted line (layoutNodeLineStyle). Its text and items are
    // those of the shaping that holds it, collected in its records: an item's font group may differ, and nothing else.
    bool lined;

    // The shaping of the context as on the first formatted line of its container, if the container or an element in it
    // has a record for that line (prLayoutNodeSetFirstLineStyle), or null. A lined shaping holds none.
    LayoutInlineShaping* PR_NULLABLE firstLine;

    // The font group of the container's record for the first formatted line, or `containerFont` if it has none.
    PrLayoutGroupIndex containerLineFont;

    // The font groups it names, the container's and its items', each once and each holding a reference
    // (layoutFontRetain). Its runs index the groups' faces, so collection must not free a group and reuse its index
    // while the tree caches the shaping. `styles` releases them.
    LayoutStyles* PR_NULLABLE styles;
    Array<uint32_t> fonts;

    PrTextBidi containerBidi;  // the container's unicode-bidi, which its paragraphs' levels read
    Array<uint32_t> nodes;
    Array<char> text;
    Array<LayoutInlineItem> items;
    Array<LayoutInlineSegment> segments; // at least one, in order
    Array<PrTextSpaceFlags> spaces;      // by byte

    // By byte: the offset in its node's text of the character that the byte was collected from, which queries and
    // painting map positions in the text through. A space that white space collapsing kept stands for the first space
    // of its run, and each byte that a text-transform made stands for the character it replaced. A byte that no text
    // node holds, such as an atomic inline's, is 0.
    Array<uint32_t> sources;
    bool tabs;                           // true if some byte is a preserved tab

    // If it has tabs, in px: the distance between tab stops, which is eight spaces of the container's font
    // (CSS2/#white-space-prop), and the minimum advance of a tab, which is half the advance of its 0
    // (css-text-3/#tab-size-property).
    float tabSize;
    float tabMinimum;

    // The text as lib/text shapes it, whose pieces are the items, one for one, and what that holds.
    PrTextShaping* PR_NULLABLE shaped;
    PrTextShapingInfo info;

    Array<LayoutShapingRun> runs;   // in the text's order

    // True if some byte's or paragraph's level is odd, so that lines reorder. Otherwise every level is even, and lines
    // are placed in text order.
    bool bidi;

    Array<LayoutBidiSplit> splits;          // sorted by Open item, then byte

    uint32_t last;                  // the context's last node, a sibling of its first

    // The collection that filled it, unique among its tree's collections (LayoutCache::collections). A record of the
    // lines laid out from it names this value. Zero for a shaping that no tree caches.
    uint32_t collected;
};

// Frees what a shaping holds, including a shaping the tree caches.
void layoutShapingReset(LayoutInlineShaping& shaping);
void layoutShapingDestroy(LayoutInlineShaping* PR_NULLABLE shaping);

// The scratch memory that finding breaks, itemizing and shaping use while they run.
struct LayoutShapingScratch {
    PrTextShapingScratch* PR_NULLABLE text;
    Array<PrTextPiece> pieces;           // the items as lib/text takes them, one for one
    Array<PrUnicodeBidiClass> classes;   // a paragraph's bidi classes, for its line levels
    Array<uint8_t> bidiLevels;           // their levels
    Array<uint32_t> bidiOpen;            // the Open items of the boxes open across paragraphs, outermost first
    Array<uint32_t> unitStarts;          // a paragraph's units of one level and the same boxes, by their first byte
    Array<uint8_t> unitLevels;
    Array<uint32_t> unitOrder;      // by visual position: the unit there
    Array<uint32_t> unitVisual;     // by unit: its visual position
    Array<uint32_t> unitRuns;       // the first unit of each box open in a paragraph
    Array<uint32_t> positions;      // scratch: the visual positions of a box's units
    Array<uint32_t> boxBytes;       // each box's Open item and its units in a paragraph
    Array<uint8_t> lineLevels;      // a paragraph's levels with L1, by byte
};

// Frees the scratch memory.
void layoutShapingScratchReset(LayoutShapingScratch& scratch);

// ---------------------------------------------------------------------------------------------------------------------
// Shaping

// Returns the shaping of the run from `first` through `last`. Returns the tree's cached shaping if it is current.
// Otherwise collects and shapes the run into the cache, or into `fresh` if `state` does not cache. Clears the dirty
// flags of the run's nodes, because the run's layout lays out what changed in them. Atomic inlines, floats and
// block-level boxes keep their flags, which their own layouts read.
const LayoutInlineShaping& layoutShaping(LayoutState& state, LayoutInlineShaping& fresh, uint32_t first, uint32_t last);

// Shapes the inline formatting contexts that the next layout of the tree from `root` lays out again and whose shaping
// is not current, so that the layout finds each current.
void layoutShapeAhead(LayoutState& state, uint32_t root);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
