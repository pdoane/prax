// The parts that the gallery's sections share: section and panel frames, cards, captions and grids.

#pragma once

#include "pr/kit.h"
#include "pr/kit_box.h"
#include "pr/kit_button.h"
#include "pr/kit_choice.h"
#include "pr/kit_input.h"
#include "pr/kit_surface.h"
#include "pr/kit_text.h"
#include "pr/print.h"
#include "pr/span.h"
#include "pr/string.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

namespace gallery {

using namespace pr;

// The styles that the gallery's sections share.
constexpr PrKitTextStyle kBodySecondary = {.tone = PrKitTone_Secondary};
constexpr PrKitTextStyle kLede = {.tone = PrKitTone_Muted, .align = PrKitTextAlign_End};
constexpr PrKitTextStyle kStrong = {.role = PrKitTextRole_Strong};
constexpr PrKitTextStyle kTitle = {.role = PrKitTextRole_Title};
constexpr PrKitTextStyle kCaption = {.role = PrKitTextRole_Caption};
constexpr PrKitTextStyle kCaptionAccent = {.role = PrKitTextRole_Caption, .tone = PrKitTone_Accent};
constexpr PrKitTextStyle kCaptionFaint = {.role = PrKitTextRole_Caption, .tone = PrKitTone_Faint};
constexpr PrKitTextStyle kMeta = {.role = PrKitTextRole_Meta};
constexpr PrKitTextStyle kMetaAccent = {.role = PrKitTextRole_Meta, .tone = PrKitTone_Accent};
constexpr PrKitTextStyle kMetaMuted = {.role = PrKitTextRole_Meta, .tone = PrKitTone_Muted};
constexpr PrKitTextStyle kMono = {.role = PrKitTextRole_Mono};
constexpr PrKitTextStyle kMonoSecondary = {.role = PrKitTextRole_Mono, .tone = PrKitTone_Secondary};
constexpr PrKitTextStyle kMonoMuted = {.role = PrKitTextRole_Mono, .tone = PrKitTone_Muted};
constexpr PrKitTextStyle kMonoFaint = {.role = PrKitTextRole_Mono, .tone = PrKitTone_Faint};
constexpr PrKitTextStyle kMonoStrong = {.role = PrKitTextRole_MonoStrong};
constexpr PrKitButtonStyle kPrimary = {.variant = PrKitButtonVariant_Primary};
constexpr PrKitButtonStyle kGhost = {.variant = PrKitButtonVariant_Ghost};
constexpr PrKitButtonStyle kDanger = {.variant = PrKitButtonVariant_Danger};

// Creates a card: a box on the panel background with a rule on every side, laid out as `box` says.
PrLayoutNode card(PrKit* kit, PrLayoutNode parent, const PrKitBoxStyle& box);

// Creates a label of `text` in `style` that takes the room its parent leaves.
PrLayoutNode fillLabel(PrKit* kit, PrLayoutNode parent, PrStringView text, const PrKitTextStyle& style);

// Creates a caption: `text` in the caption role and the muted tone.
PrLayoutNode caption(PrKit* kit, PrLayoutNode parent, PrStringView text);

// Creates a row led by `text` as a caption in a column `labelWidth` px wide, and returns the row, which takes the
// specimen after the caption.
PrLayoutNode captionRow(PrKit* kit, PrLayoutNode parent, PrStringView text, float labelWidth);

// Creates a numbered section: a header of its number, its title and, at the header's end, its lede, then its
// description, if any, in a column of a fixed measure. Returns the section, which takes the section's panels.
PrLayoutNode sectionFrame(PrKit* kit, PrLayoutNode parent, PrStringView number, PrStringView title, PrStringView lede,
    PrStringView description);

// Creates a panel of specimens: a card with a captioned header and `tags` at its end, and returns its body. The body of
// a bare panel has no padding and no gap, so that its content reaches the card's edges and meets edge to edge.
PrLayoutNode demoPanel(PrKit* kit, PrLayoutNode parent, PrStringView title, PrStringView tags, bool bare);

// Creates a segmented control of `count` items. The item at index `*selected` is selected, and activating an item
// writes its index to `selected`.
PrLayoutNode segmented(PrKit* kit, PrLayoutNode parent, const PrKitChoice* items, uint32_t count, uint32_t* selected,
    PrKitChoiceStyle style);

// A grid of equal columns, declared a cell at a time.
struct Grid {
    PrKit* kit;
    PrLayoutNode parent;
    PrLayoutNode row;
    uint32_t columns;
    uint32_t count;
    float gap;
    bool ruled;                     // a rule between cells, and no gap
};

// Begins a grid of `columns` columns under `parent`, its cells `gap` px apart, or ruled apart if `ruled` is true.
Grid gridBegin(PrKit* kit, PrLayoutNode parent, uint32_t columns, float gap, bool ruled);

// Creates the grid's next cell, a column, and returns it.
PrLayoutNode gridCell(Grid& grid);

// Fills the grid's last row with empty cells, so that its cells are as wide as the others.
void gridEnd(Grid& grid);

} // namespace gallery

PR_ASSUME_NONNULL_END
