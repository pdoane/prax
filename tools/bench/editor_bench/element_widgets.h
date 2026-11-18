// The widgets that the editor of elements is built with (element_view.h): its look, and functions over lib/ui's
// elements (pr/ui_element.h), as Studio Kit's widgets are what editor_view.h is built with. They know nothing of the
// editor's model, and draw Studio's icons (pr/studio_icon.h) with the builder's resources.
//
// A widget takes its inputs as a struct of what it shows, by value, and memoizes itself on them (pr/ui_element.h,
// Memos), so a round builds only the widgets whose inputs changed or that input reached. Text that a widget shows is
// static text where its inputs take a PrStringView, and text of the caller's where they take a String.

#pragma once

#include "pr/kit_data.h"
#include "pr/string.h"
#include "pr/studio_icon.h"
#include "pr/ui_element.h"

#include <stdint.h>

PR_ASSUME_NONNULL_BEGIN

using namespace pr;

namespace widgets {

// ---------------------------------------------------------------------------------------------------------------------
// Look

// The palette: the app's background, then the panels', headers' and raised controls', fields', rules, and the fills of
// hovered and selected items, then the tones of text and icons, from the strongest down, and the accent and the colors
// of outcomes.
inline constexpr PrColor kClear = {0, 0, 0, 0};
inline constexpr PrColor kApp = prColorFromHex(0x0e1013);
inline constexpr PrColor kPanel = prColorFromHex(0x15171b);
inline constexpr PrColor kHeader = prColorFromHex(0x1b1e23);
inline constexpr PrColor kRaised = prColorFromHex(0x22252b);
inline constexpr PrColor kField = prColorFromHex(0x0f1115);
inline constexpr PrColor kLine = prColorFromHex(0x2a2e35);
inline constexpr PrColor kHover = prColorFromHex(0x262a31);
inline constexpr PrColor kSelect = prColorFromHex(0x1d3557);
inline constexpr PrColor kSelectHover = prColorFromHex(0x24406a);
inline constexpr PrColor kText = prColorFromHex(0xd9dde4);
inline constexpr PrColor kSecondary = prColorFromHex(0xa9b0bc);
inline constexpr PrColor kMuted = prColorFromHex(0x7d8591);
inline constexpr PrColor kFaint = prColorFromHex(0x545b66);
inline constexpr PrColor kAccent = prColorFromHex(0x5b9bff);
inline constexpr PrColor kAccentHover = prColorFromHex(0x7aafff);
inline constexpr PrColor kSuccess = prColorFromHex(0x46c07a);
inline constexpr PrColor kWarning = prColorFromHex(0xe0a93b);
inline constexpr PrColor kDanger = prColorFromHex(0xf0645a);

// The colors of the x, y and z axes.
inline constexpr PrColor kAxis[3] = {prColorFromHex(0xe0605a), prColorFromHex(0x6cc46a), prColorFromHex(0x5b9bff)};

// The heights of the bars, rows and controls, as Studio Kit's sizes are.
inline constexpr float kMenuBar = 26;
inline constexpr float kToolbar = 30;
inline constexpr float kTabStrip = 22;
inline constexpr float kPanelHeader = 26;
inline constexpr float kRow = 22;
inline constexpr float kControl = 20;

// An icon of Studio's set, `size` px square, in `color`, whose path the builder's resources parse once.
UiElement icon(UiBuilder& ui, PrLayoutNode parent, PrStudioIconKind which, float size, PrColor color);

// Text in the sizes of body text and captions.
UiElement body(UiElement e);
UiElement caption(UiElement e);

// A row of `height` on `background`, its items centered across it, `gap` px apart.
UiElement bar(UiBuilder& ui, PrLayoutNode parent, PrColor background, float height, float gap);

// A rule between the items of a row, and room that pushes the items after it to the row's end.
void divider(UiBuilder& ui, PrLayoutNode parent);
void spacer(UiBuilder& ui, PrLayoutNode parent);

// A caption of `text` in `color`, and a small badge of it.
void label(UiBuilder& ui, PrLayoutNode parent, PrStringView text, PrColor color);
void badge(UiBuilder& ui, PrLayoutNode parent, PrStringView text, PrColor color);

// Formats `value` into `out`, a number to two places or a count.
PrStringView number(char (&out)[32], float value);
PrStringView count(char (&out)[32], uint32_t value);

// ---------------------------------------------------------------------------------------------------------------------
// Results

// What happened to a control that the user clicks.
struct Clicked {
    bool clicked;
};

// What a choice among items chose in the round: the item's index, or -1.
struct Chosen {
    int32_t index = -1;
};

// What an edit of a number did in the round: its new value, and whether the gesture that made it ended.
struct Edited {
    bool changed = false;
    bool committed = false;
    float value = 0;
};

// What an edit of a vector did: the component that it changed, or -1, and the component's new value.
struct VectorEdited {
    int32_t axis = -1;
    bool committed = false;
    float value = 0;
};

// ---------------------------------------------------------------------------------------------------------------------
// Controls

// A square button that shows an icon, tinted when selected.
struct IconButton {
    PrStudioIconKind which;
    bool selected = false;
    float size = 26;
    bool operator==(const IconButton&) const = default;
};
UiResult<Clicked> iconButton(UiBuilder& ui, PrLayoutNode parent, const IconButton& inputs, PrUiKey key = 0);

// How a button is drawn.
enum {
    ButtonLook_Ghost,
    ButtonLook_Solid,
    ButtonLook_Primary,
};
typedef uint8_t ButtonLook;

// A button that shows an icon, text, or both.
struct Button {
    PrStringView text;
    PrStudioIconKind which = PrStudioIconKind_None;
    ButtonLook look = ButtonLook_Ghost;
    bool selected = false;
    bool operator==(const Button&) const = default;
};
UiResult<Clicked> button(UiBuilder& ui, PrLayoutNode parent, const Button& inputs, PrUiKey key = 0);

// A row of segments, one of them selected.
struct Segmented {
    const PrStringView* items;
    uint32_t count;
    uint32_t selected;
    bool operator==(const Segmented&) const = default;
};
UiResult<Chosen> segmented(UiBuilder& ui, PrLayoutNode parent, const Segmented& inputs, PrUiKey key = 0);

// A strip of tabs, one of them selected.
struct Tabs {
    const PrStringView* items;
    uint32_t count;
    uint32_t selected;
    bool operator==(const Tabs&) const = default;
};
UiResult<Chosen> tabs(UiBuilder& ui, PrLayoutNode parent, const Tabs& inputs, PrUiKey key = 0);

// A check box, and a label after it.
struct Checkbox {
    bool checked;
    PrStringView text;
    bool operator==(const Checkbox&) const = default;
};
UiResult<Clicked> checkbox(UiBuilder& ui, PrLayoutNode parent, const Checkbox& inputs, PrUiKey key = 0);

// A switch, on or off.
struct Toggle {
    bool on;
    bool operator==(const Toggle&) const = default;
};
UiResult<Clicked> toggle(UiBuilder& ui, PrLayoutNode parent, const Toggle& inputs, PrUiKey key = 0);

// A number that a horizontal drag scrubs by `step` a pixel, within its range.
struct NumberDrag {
    float value;
    float step = 0.01f;
    float min = -1e9f;
    float max = 1e9f;
    PrStringView unit;
    PrColor axis = kClear;          // a mark of the axis that the number is a component of
    bool operator==(const NumberDrag&) const = default;
};
UiResult<Edited> numberDrag(UiBuilder& ui, PrLayoutNode parent, const NumberDrag& inputs, PrUiKey key = 0);

// Three numbers, x, y and z.
struct VectorField {
    float value[3];
    float step;
    float min = -1e9f;
    bool operator==(const VectorField&) const = default;
};
UiResult<VectorEdited> vectorField(UiBuilder& ui, PrLayoutNode parent, const VectorField& inputs, PrUiKey key = 0);

// A value between `min` and `max` that the pointer sets along a track.
struct Slider {
    float value;
    float min = 0;
    float max = 1;
    bool operator==(const Slider&) const = default;
};
UiResult<Edited> slider(UiBuilder& ui, PrLayoutNode parent, const Slider& inputs, PrUiKey key = 0);

// ---------------------------------------------------------------------------------------------------------------------
// Menus

// An entry of a menu, and the command that it runs, which is the caller's.
struct MenuEntry {
    PrStringView text;
    PrStringView shortcut = {};
    uint32_t command = 0;
    bool danger = false;
    bool disabled = false;
    bool separatorBefore = false;
};

// What a menu ran in the round, if anything.
struct Ran {
    bool ran = false;
    uint32_t command = 0;
};

// A menu of a menu bar: its title, which opens and closes it, and its entries while it is open, under the title in the
// overlay. `checked` and `disabled` mark entries by their index's bit, as well as the entries' own.
struct Menu {
    PrStringView title;
    const MenuEntry* entries;
    uint32_t count;
    uint64_t checked = 0;
    uint64_t disabled = 0;
    bool operator==(const Menu&) const = default;
};
UiResult<Ran> menu(UiBuilder& ui, PrLayoutNode parent, const Menu& inputs, PrUiKey key = 0);

// A button that shows the selected item, and opens the menu of items.
struct Dropdown {
    const PrStringView* items;
    uint32_t count;
    uint32_t selected;
    PrStudioIconKind which = PrStudioIconKind_None;
    bool operator==(const Dropdown&) const = default;
};
UiResult<Chosen> dropdown(UiBuilder& ui, PrLayoutNode parent, const Dropdown& inputs, PrUiKey key = 0);

// A tip of `text` and its `shortcut` under `anchor` in the overlay, once the pointer has rested on it.
void tooltip(UiBuilder& ui, PrLayoutNode anchor, PrStringView text, PrStringView shortcut);

// ---------------------------------------------------------------------------------------------------------------------
// Panels

// A panel's header: its title, and what the caller creates after it.
UiElement panelHeader(UiBuilder& ui, PrLayoutNode parent, PrStringView title);

// What happened to a row of a tree.
struct RowEvents {
    bool clicked = false;
    bool folded = false;
    bool hid = false;
};

// A row of a tree: an item's fold, icon, name and type, and its visibility, which shows while the row is hovered.
struct TreeRow {
    String name;
    String type;
    PrStudioIconKind which;
    uint32_t depth;
    bool hasChildren;
    bool open;
    bool hidden;
    bool selected;
    bool operator==(const TreeRow&) const = default;
};
UiResult<RowEvents> treeRow(UiBuilder& ui, PrLayoutNode parent, const TreeRow& inputs, PrUiKey key = 0);

// The rows of a list that its scroll container shows, from `first` to `end`, and a few past each end.
struct ListWindow {
    uint32_t first;
    uint32_t end;
};

// Makes `root` a column that scrolls, of `count` rows `rowHeight` high, and returns the rows that it shows, after the
// room for the rows above them. The caller creates those rows under `root`, and then the room below them (listRest).
ListWindow listWindow(UiBuilder& ui, UiElement root, uint32_t count, float rowHeight);
void listRest(UiBuilder& ui, PrLayoutNode root, ListWindow window, uint32_t count, float rowHeight);

// A tile of a grid: an icon over its name.
struct Tile {
    PrStudioIconKind which;
    PrStringView name;
    bool selected;
    bool operator==(const Tile&) const = default;
};
UiResult<Clicked> tile(UiBuilder& ui, PrLayoutNode parent, const Tile& inputs, PrUiKey key = 0);

// A line of a log.
struct LogLine {
    String time;
    String message;
    String source;
    PrKitLogLevel level;
    bool operator==(const LogLine&) const = default;
};
UiElement logLine(UiBuilder& ui, PrLayoutNode parent, const LogLine& inputs, PrUiKey key = 0);

// A group's header, which opens and closes the group, with a badge if it has one.
struct GroupHeader {
    PrStringView name;
    PrStringView badge;
    PrColor badgeColor;
    bool open;
    bool operator==(const GroupHeader&) const = default;
};
UiResult<Clicked> groupHeader(UiBuilder& ui, PrLayoutNode parent, const GroupHeader& inputs, PrUiKey key = 0);

// How a property row shows and edits its value.
enum {
    FieldKind_Number,               // a number that a drag scrubs
    FieldKind_Slider,               // a number on a slider, beside the number
    FieldKind_Vector,               // three numbers
    FieldKind_Switch,               // a flag on a switch
    FieldKind_Check,                // a flag in a check box
    FieldKind_Choice,               // one of its choices, as segments
    FieldKind_Color,                // a color's swatch and its hex
    FieldKind_Asset,                // an asset's name
};
typedef uint8_t FieldKind;

// The value of a property row, of which its kind uses one part: its numbers, its flag, its choice, its color or its
// text.
struct FieldValue {
    float numbers[3];
    bool flag;
    uint32_t choice;
    PrColor color;
    String text;
    bool operator==(const FieldValue&) const = default;
};

// What an edit of a property row did in the round: the value that it gave the property, and whether the gesture that
// made it ended. A flag or a choice ends its gesture as it changes.
struct FieldEdited {
    bool changed = false;
    bool committed = false;
    FieldValue value;
};

// A row of an inspector that shows a property by its name, and edits its value as its kind says: within its range and
// in its unit if it is a number, and among its choices if it is a choice. Its inputs are one property's value, so a
// walk of many properties builds only the rows whose values changed.
struct PropertyRow {
    PrStringView name;
    FieldKind kind;
    FieldValue value;
    float step = 0.01f;
    float min = -1e9f;
    float max = 1e9f;
    PrStringView unit;
    const PrStringView* PR_NULLABLE choices = nullptr;
    uint32_t choiceCount = 0;
    bool operator==(const PropertyRow&) const = default;
};
UiResult<FieldEdited> propertyRow(UiBuilder& ui, PrLayoutNode parent, const PropertyRow& inputs, PrUiKey key = 0);

// A panel of a HUD over `parent`, in the corner that `right` and `bottom` name, its items in a row or a column.
UiElement hudPanel(UiBuilder& ui, PrLayoutNode parent, bool right, bool bottom, bool row);

// A name and its value in a HUD panel.
void readout(UiBuilder& ui, PrLayoutNode parent, PrStringView name, PrStringView value);

// ---------------------------------------------------------------------------------------------------------------------
// Docks

// A pane of a dock: its tab, and the node that its panel goes under. The pane takes `share` of its column's height, or
// the rest of it for none.
PrLayoutNode pane(UiBuilder& ui, PrLayoutNode parent, PrStringView title, float share = 0);

// A handle between two parts of a dock that a drag moves, setting `share`, the share of `container`'s extent along the
// axis of the part before the handle, or after it if `after`.
void splitter(UiBuilder& ui, PrLayoutNode parent, PrLayoutNode container, bool vertical, float* share, bool after = false);

} // namespace widgets

PR_ASSUME_NONNULL_END
