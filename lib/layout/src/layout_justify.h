// Justification: finding the justification opportunities of a line (css-text-3/#justification). When the line is
// placed, its free space is distributed over these opportunities.
//
// css-text-3 - https://drafts.csswg.org/css-text-3/

#pragma once

#include "layout/src/layout_line.h"
#include "layout/src/layout_shaping.h"
#include "layout/src/layout_space.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// A justification opportunity. It records the byte before which its share of the free space is inserted, and whether it
// is a gap between two typographic character units. A gap widens only the boxes that are open around both units. The
// opportunity of a word separator widens the box that holds the separator.
struct LayoutJustifyOpportunity {
    uint32_t byte;
    bool gap;
};

// Orders opportunities by byte, for searches by byte.
inline bool operator<(uint32_t byte, const LayoutJustifyOpportunity& opportunity) {
    return byte < opportunity.byte;
}

// The justification state of the line being built.
struct LayoutJustifier {
    Array<LayoutJustifyOpportunity> opportunities;  // in order
    Array<uint32_t> open;                           // scratch: the Open items around the text being read
};

// Frees the memory that a justifier holds.
void layoutJustifierReset(LayoutJustifier& justifier);

// Finds the justification opportunities of `line` (css-text-3/#justification-opportunity) in a container styled by
// `containerStyle`. `styles` holds the style of each item. Only the content after the line's last preserved tab
// stretches, so that the tab keeps its tab stop. The white space that ends the line does not stretch. A word separator
// creates an opportunity after itself, according to the text-justify of its own text. A gap between two typographic
// character units creates an opportunity according to the text-justify of the nearest box that holds both units. Units
// are grapheme clusters that shaping did not merge. The edge of an inline box neither creates nor removes an
// opportunity. An atomic inline or a forced break separates the units on either side of it.
void layoutJustifyFind(LayoutState& state, LayoutJustifier& justifier, const LayoutInlineShaping& t,
                       const PrLayoutStyleIndex* styles, PrLayoutStyleIndex containerStyle,
                       const LayoutLineContent& line);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
