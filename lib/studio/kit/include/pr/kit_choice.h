// Studio Kit's tabs, segmented controls, checkboxes, toggles and radios, progress bars, meters and spinners.

#pragma once

#include "pr/kit.h"
#include "pr/kit_box.h"
#include "pr/kit_button.h"
#include "pr/kit_icon.h"
#include "pr/kit_text.h"
#include "pr/string.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// ---------------------------------------------------------------------------------------------------------------------
// Choices
//
// A segmented control, a radio group and a tab strip are each a container, and the caller creates their items under
// them. The caller holds the chosen value, in a type of its own. Each item takes from its content whether it is
// selected, and reports a click as an activation (prKitActivated), on which the caller updates its value. While an item
// has focus, the arrow keys along the container's axis move the focus to the neighboring item, and that item is
// activated in the next round.

// The size and direction of a choice widget. The items of a container use the container's style. Zero is medium, and a
// radio group in a column.
typedef struct PrKitChoiceStyle {
    PrKitControlSize size;          // height of a strip, or of the row that holds a checkbox, toggle or radio
    PrKitDirection direction;       // axis along which a radio group lays out its radios
} PrKitChoiceStyle;

// A segmented control or a radio group.
typedef struct PrKitChoices {
    uint64_t key;
    bool disabled;                  // true to disable every item in the container
} PrKitChoices;

// A segment or a radio. It shows a label, an icon, or both.
typedef struct PrKitChoice {
    uint64_t key;                   // identity within the container, or 0 to match by position
    PrStringView label;
    PrKitIconKind icon;
    bool selected;
    bool disabled;
} PrKitChoice;

// Creates a segmented control, or a segment under one. The control is an outlined track, and its segments share its
// width equally. Left and Right move between segments.
PrLayoutNode prKitSegmented(PrKit* kit, PrLayoutNode parent, PrKitChoices choices, PrKitChoiceStyle style);
PrLayoutNode prKitSegment(PrKit* kit, PrLayoutNode segmented, PrKitChoice choice);

// Creates a radio group, or a radio under one. Each radio is a row with a ring, which is filled if the radio is
// selected. The arrow keys along the group's axis move between radios.
PrLayoutNode prKitRadioGroup(PrKit* kit, PrLayoutNode parent, PrKitChoices choices, PrKitChoiceStyle style);
PrLayoutNode prKitRadio(PrKit* kit, PrLayoutNode group, PrKitChoice choice);

// The content of a checkbox or a toggle: the flag that it reads and flips, and its label. The label is empty if the
// caller places its own content beside the control.
typedef struct PrKitCheck {
    uint64_t key;
    bool* value;
    PrStringView label;
    bool disabled;
} PrKitCheck;

// Creates a checkbox or a toggle switch: a row of the box or the switch and the label. The row flips the flag when it
// is activated (prUiActivated). A toggle's knob eases along its track as the flag changes.
PrLayoutNode prKitCheckbox(PrKit* kit, PrLayoutNode parent, PrKitCheck check, PrKitChoiceStyle style);
PrLayoutNode prKitToggle(PrKit* kit, PrLayoutNode parent, PrKitCheck check, PrKitChoiceStyle style);

// ---------------------------------------------------------------------------------------------------------------------
// Tabs

// The content of a tab strip. If the strip is reorderable, the user can drag a tab to another place in it, and an
// accent caret marks where the tab will go.
typedef struct PrKitTabStrip {
    uint64_t key;
    bool disabled;                  // true to disable every tab in the strip
    bool reorderable;
} PrKitTabStrip;

// One tab in a strip: its identity and its state.
typedef struct PrKitTab {
    uint64_t key;                   // identity within the strip, or 0 to match by position
    bool selected;
    bool disabled;
} PrKitTab;

// Creates a tab strip, or a tab under one. The strip has a rule along its bottom, and the selected tab is underlined in
// the accent. Left and Right move between tabs.
//
// A tab is a cell that the caller fills: what it shows goes under it, in order, from the tab's parts below or any of
// the kit's widgets, such as an image (prKitImage) or a badge (prKitBadge).
PrLayoutNode prKitTabStrip(PrKit* kit, PrLayoutNode parent, PrKitTabStrip strip, PrKitChoiceStyle style);
PrLayoutNode prKitTab(PrKit* kit, PrLayoutNode strip, PrKitTab tab);

// Creates a tab's label, in the text color of the tab's state.
PrLayoutNode prKitTabLabel(PrKit* kit, PrLayoutNode tab, PrKitLabel label);

// Creates a tab's close control, which only the pointer can use, so that the arrow keys move between tabs. A click
// activates it (prKitActivated). The tab remains until the caller stops creating it.
PrLayoutNode prKitTabClose(PrKit* kit, PrLayoutNode tab);

// Returns true if the user dropped one of the strip's tabs at another place in the strip in the current round. Writes
// the tab's old index to `from` and its new index to `to`. Both indices count the tabs in the order that the previous
// round created them. To apply the move, the caller removes the tab at `from` and inserts it at `to`.
bool prKitMoved(const PrKit* kit, PrLayoutNode strip, uint32_t* from, uint32_t* to);

// ---------------------------------------------------------------------------------------------------------------------
// Progress

// The content of a progress bar. `value` is the fraction of the task that is done, clamped to the range 0 to 1. An
// indeterminate bar ignores `value` and shows a band that sweeps across its track.
typedef struct PrKitProgress {
    uint64_t key;
    float value;
    bool indeterminate;
} PrKitProgress;

// Creates a progress bar: a track as wide as its parent allows, which the caller places (prKitPlace) in a row. An
// indeterminate bar requests frames while it shows, and none while it is scrolled or clipped out of view.
PrLayoutNode prKitProgress(PrKit* kit, PrLayoutNode parent, PrKitProgress progress);

// The number of segments in a meter's bar.
enum { kPrKitMeterSegments = 12 };

// The content of a meter: a reading between `low` and `high`. If both `low` and `high` are zero, the range is 0 to 1.
typedef struct PrKitMeter {
    uint64_t key;
    float value;
    float low;
    float high;
} PrKitMeter;

// The style of a meter: the tone of the segments that the reading lights. Zero is the primary tone.
typedef struct PrKitMeterStyle {
    PrKitTone tone;
} PrKitMeterStyle;

// Creates a meter: a bar of segments, with the segments up to the reading lit. It takes room in the same way as a
// progress bar.
PrLayoutNode prKitMeter(PrKit* kit, PrLayoutNode parent, PrKitMeter meter, PrKitMeterStyle style);

// The content of a spinner.
typedef struct PrKitSpinner {
    uint64_t key;
} PrKitSpinner;

// The style of a spinner: the side of its square in px. Zero is 16 px.
typedef struct PrKitSpinnerStyle {
    float size;
} PrKitSpinnerStyle;

// Creates a spinner: the spinner icon turning in its square, in the accent color. It requests frames while it shows,
// and none while it is scrolled or clipped out of view.
PrLayoutNode prKitSpinner(PrKit* kit, PrLayoutNode parent, PrKitSpinner spinner, PrKitSpinnerStyle style);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
