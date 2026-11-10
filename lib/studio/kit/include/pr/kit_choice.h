// Studio Kit's tabs, segmented controls, checkboxes, toggles and radios, progress bars, meters and spinners.

#pragma once

#include "pr/kit.h"
#include "pr/kit_box.h"
#include "pr/kit_button.h"
#include "pr/kit_icon.h"
#include "pr/kit_text.h"
#include "pr/raw_string.h"
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
    PrUiKey key;
    bool disabled;                  // true to disable every item in the container
} PrKitChoices;

// A segment or a radio. It shows a label, an icon, or both.
typedef struct PrKitChoice {
    PrUiKey key;                   // identity within the container, or 0 to match by position
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
    PrUiKey key;
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

// The tabs of a strip: which tabs it holds, their order, and the selected tab. The application creates one for each
// strip and passes it to the strip in each round. The application adds and removes the tabs, and the strip changes
// their order and the selection as the user drags and chooses them. So the application's own list of tabs needs no
// order. A tab is an id that the application chooses, nonzero and unique within the strip's group.
typedef struct PrKitTabs PrKitTabs;

// Creates the tabs of a strip that holds no tabs yet.
PrKitTabs* prKitTabsCreate(void);
void prKitTabsDestroy(PrKitTabs* PR_NULLABLE tabs);

// Returns the number of tabs, the tab at `index` in the strip's order, or 0 past the last, and the index of the tab
// `id`, or the number of tabs if the strip does not hold it.
uint32_t prKitTabsCount(const PrKitTabs* tabs);
uint64_t prKitTabsAt(const PrKitTabs* tabs, uint32_t index);
uint32_t prKitTabsIndex(const PrKitTabs* tabs, uint64_t id);

// Returns the selected tab, or 0 if there are no tabs.
uint64_t prKitTabsSelected(const PrKitTabs* tabs);

// Adds the tab `id`, which the strip does not hold, at `index` in the strip's order, or last if `index` is past the
// last tab. The first tab is selected.
void prKitTabsInsert(PrKitTabs* tabs, uint64_t id, uint32_t index);

// Removes the tab `id`, which the strip holds. If it was selected, the tab after it is selected, or the last tab.
void prKitTabsRemove(PrKitTabs* tabs, uint64_t id);

// Moves the tab `id` from the strip `from`, which holds it, to `index` in the order of the strip `to`, counted without
// the tab, or last if `index` is past the last tab, and selects it there. `from` selects another tab as prKitTabsRemove
// does. `from` and `to` may be the same strip.
void prKitTabsMove(PrKitTabs* from, PrKitTabs* to, uint64_t id, uint32_t index);

// Selects the tab `id`, which the strip holds.
void prKitTabsSelect(PrKitTabs* tabs, uint64_t id);

// Selects the tab `step` places after the selected one in the strip's order, wrapping around the ends, and returns it,
// or returns 0 if there are no tabs.
uint64_t prKitTabsStep(PrKitTabs* tabs, int32_t step);

// The content of a tab strip of `tabs`. If the strip is reorderable, the user can drag a tab to another place in it,
// and an accent caret marks where the tab will go.
//
// Reorderable strips of the same nonzero `group` exchange tabs, in any of the application's windows, and a tab dropped
// outside every window tears off. The application chooses the group, which is the kind of a dragged tab's payload
// (prUiDragging). A zero group gives the strip a group of its own. A strip that `collapsesSingle` takes no room while
// it holds one tab, unless a tab of its group is dragged over its window.
typedef struct PrKitTabStrip {
    PrUiKey key;
    PrKitTabs* tabs;
    uint64_t group;
    bool reorderable;
    bool collapsesSingle;
    bool disabled;                  // true to disable every tab in the strip
} PrKitTabStrip;

// One tab of a strip, by its id.
typedef struct PrKitTab {
    uint64_t id;
    bool disabled;
} PrKitTab;

// Creates a tab strip, or a tab under one. The strip has a rule along its bottom, and the selected tab is underlined in
// the accent. A click on a tab selects it, and Left and Right select the neighboring tab.
//
// A round creates the tabs that the strip holds, in any order, and the strip shows them in its own order. A tab is a
// cell that the caller fills: what it shows goes under it, in order, from the tab's parts below or any of the kit's
// widgets, such as an image (prKitImage) or a badge (prKitBadge).
PrLayoutNode prKitTabStrip(PrKit* kit, PrLayoutNode parent, PrKitTabStrip strip, PrKitChoiceStyle style);
PrLayoutNode prKitTab(PrKit* kit, PrLayoutNode strip, PrKitTab tab);

// Creates a tab's label, in the text color of the tab's state.
PrLayoutNode prKitTabLabel(PrKit* kit, PrLayoutNode tab, PrKitLabel label);

// Creates a tab's close control, which only the pointer can use, so that the arrow keys move between tabs. A click
// activates it (prKitActivated).
PrLayoutNode prKitTabClose(PrKit* kit, PrLayoutNode tab);

// Where the user dropped a tab of another strip, or of this one.
enum {
    PrKitTabMoveKind_Received,      // on this strip, from another strip of its group
    PrKitTabMoveKind_TornOff,       // from this strip, outside every window, as into a window of its own
};
typedef uint8_t PrKitTabMoveKind;

// A tab that the user moved out of its strip: its id, and where it went. A received tab was dropped at `index` in the
// strip's order. A torn-off tab was dropped at `position`, whose display is kPrAppDisplayNone if the platform does not
// report it.
typedef struct PrKitTabMove {
    uint64_t id;
    PrKitTabMoveKind kind;
    uint32_t index;
    PrAppDisplayPosition position;
} PrKitTabMove;

// Returns true if the user moved a tab out of its strip in the current round, and writes the move to `out`. The strip
// reports the move as it is created, before its tabs, and the application carries it out (prKitTabsMove): a received
// tab goes to this strip at the move's index, and a torn-off tab to a strip of its own. A move within the strip is the
// strip's own, and is not reported.
bool prKitTabMoved(const PrKit* kit, PrLayoutNode strip, PrKitTabMove* out);

// ---------------------------------------------------------------------------------------------------------------------
// Progress

// The content of a progress bar. `value` is the fraction of the task that is done, clamped to the range 0 to 1. An
// indeterminate bar ignores `value` and shows a band that sweeps across its track.
typedef struct PrKitProgress {
    PrUiKey key;
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
    PrUiKey key;
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
    PrUiKey key;
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
