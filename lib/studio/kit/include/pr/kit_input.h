// Studio Kit's text fields, sliders, numeric fields, vectors, and the inspector.
//
// A bound control reads its value through a pointer each round and writes the round's edits through it. The value is
// read and written during the call that creates the control, so the pointer needs to be valid only for that call. A
// text control takes its text as a value, and the caller takes the new text from prKitTextChanged.

#pragma once

#include "pr/color.h"
#include "pr/kit.h"
#include "pr/kit_button.h"
#include "pr/kit_icon.h"
#include "pr/kit_text.h"
#include "pr/raw_string.h"
#include "pr/string_view.h"
#include "pr/text_edit.h"
#include "pr/ui_text_edit.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// ---------------------------------------------------------------------------------------------------------------------
// Scalars

// The type in which a numeric control's value is stored. Controls compute in double. An integer write is rounded and
// clamped to the integer's type, and a 64-bit integer is limited to the precision of a double.
enum {
    PrKitScalarKind_F32,
    PrKitScalarKind_F64,
    PrKitScalarKind_I8,
    PrKitScalarKind_I16,
    PrKitScalarKind_I32,
    PrKitScalarKind_I64,
    PrKitScalarKind_U8,
    PrKitScalarKind_U16,
    PrKitScalarKind_U32,
    PrKitScalarKind_U64,
};
typedef uint8_t PrKitScalarKind;

// A numeric value bound by its address and its type. A vector's later components follow the first, spaced by the type's
// size.
typedef struct PrKitScalar {
    void* at;
    PrKitScalarKind kind;
} PrKitScalar;

// Return a binding to a value of each type.
static inline PrKitScalar prKitScalarF32(float* at) {
    PrKitScalar s = {at, PrKitScalarKind_F32};
    return s;
}
static inline PrKitScalar prKitScalarF64(double* at) {
    PrKitScalar s = {at, PrKitScalarKind_F64};
    return s;
}
static inline PrKitScalar prKitScalarI8(int8_t* at) {
    PrKitScalar s = {at, PrKitScalarKind_I8};
    return s;
}
static inline PrKitScalar prKitScalarI16(int16_t* at) {
    PrKitScalar s = {at, PrKitScalarKind_I16};
    return s;
}
static inline PrKitScalar prKitScalarI32(int32_t* at) {
    PrKitScalar s = {at, PrKitScalarKind_I32};
    return s;
}
static inline PrKitScalar prKitScalarI64(int64_t* at) {
    PrKitScalar s = {at, PrKitScalarKind_I64};
    return s;
}
static inline PrKitScalar prKitScalarU8(uint8_t* at) {
    PrKitScalar s = {at, PrKitScalarKind_U8};
    return s;
}
static inline PrKitScalar prKitScalarU16(uint16_t* at) {
    PrKitScalar s = {at, PrKitScalarKind_U16};
    return s;
}
static inline PrKitScalar prKitScalarU32(uint32_t* at) {
    PrKitScalar s = {at, PrKitScalarKind_U32};
    return s;
}
static inline PrKitScalar prKitScalarU64(uint64_t* at) {
    PrKitScalar s = {at, PrKitScalarKind_U64};
    return s;
}

// The values that a numeric field writes, within the range of its type: at least `min` if `hasMin` is true, and at most
// `max` if `hasMax` is true. `step` is the change for each px that a scrub drags, or 0.05 for zero.
typedef struct PrKitNumberRange {
    double step;
    double min;
    double max;
    bool hasMin;
    bool hasMax;
} PrKitNumberRange;

// ---------------------------------------------------------------------------------------------------------------------
// Numeric fields

// The content of a numeric field: the value that it shows and edits. `precision` is the number of fraction digits that
// a float shows, or two for zero. An integer shows none.
typedef struct PrKitNumber {
    PrUiKey key;
    PrKitScalar value;
    PrKitNumberRange range;
    PrStringView axis;              // a tag before the value, such as a vector's X
    PrStringView unit;              // a tag after the value
    uint8_t precision;
    bool disabled;
} PrKitNumber;

// How a numeric field looks. Zero is a medium field with its tag in the muted tone.
typedef struct PrKitNumberStyle {
    PrKitControlSize size;
    PrKitTone axisTone;             // the tag's tone; Auto is muted
} PrKitNumberStyle;

// Creates a numeric field and applies the round's edits to its value, clamped to its range. A horizontal drag scrubs
// the value by one step for each px, or by a tenth of a step with Shift. While the field has focus, Up and Down add and
// subtract ten steps, or one step with Shift, and an integer moves by at least one. Each drag and each key press is one
// gesture. In a row, the field fills the room that its parent leaves.
PrLayoutNode prKitNumberField(PrKit* kit, PrLayoutNode parent, PrKitNumber number, PrKitNumberStyle style);

// The content of a vector field: `count` components, from two to four, starting at `value`, each in `range`. Unless
// `untagged` is true, the components are tagged X, Y, Z and W in the axis tones.
typedef struct PrKitVector {
    PrUiKey key;
    PrKitScalar value;
    uint32_t count;
    PrKitNumberRange range;
    uint8_t precision;              // fraction digits that each component shows, as PrKitNumber's precision counts them
    bool untagged;
    bool disabled;
} PrKitVector;

// Creates a row with a numeric field for each component, and returns the row. Each field has its own gestures. The axis
// tone in `style` is ignored.
PrLayoutNode prKitVectorField(PrKit* kit, PrLayoutNode parent, PrKitVector vector, PrKitNumberStyle style);

// ---------------------------------------------------------------------------------------------------------------------
// Text

// How a text control looks. Zero is a medium control in the body role.
typedef struct PrKitFieldStyle {
    PrKitControlSize size;
    bool mono;                      // the text is set in the monospaced role, as for paths, ids and commands
    bool bare;                      // no frame of its own, as in a row that draws one, such as a palette's search row
} PrKitFieldStyle;

// How a style of a text area's buffer looks: its text's role and tone.
typedef struct PrKitTextAreaStyle {
    PrKitTextRole role;
    PrKitTone tone;
} PrKitTextAreaStyle;

// What a text field edits and shows.
typedef struct PrKitTextField {
    PrUiKey key;
    PrStringView text;
    PrTextEditBuffer* PR_NULLABLE buffer; // holds the text instead of `text`, for a caller that owns a buffer
    PrStringView placeholder;             // shown faint while the text is empty
    PrKitIconKind icon;                   // before the text, or none

    // Earlier lines, oldest first, that Up and Down recall into the field, as a console's history.
    const PrStringView* PR_NULLABLE history;
    uint32_t historyCount;

    bool clearable;                 // shows a control that empties the text while it has any
    bool selectOnFocus;             // focus that comes by a key rather than a press selects the whole text
    bool disabled;
} PrKitTextField;

// Creates a text field: one line of text with a caret and a selection, edited by the pointer and the keys as
// pr/ui_text_edit.h describes. A press places the caret, a drag selects, a double press selects a word and a triple
// press the whole text. Each edit is one gesture. Enter is left to the caller, which reads it with prUiEvents on the
// field. The text scrolls along its line to keep the caret in view. In a row, the field fills the room that its parent
// leaves.
PrLayoutNode prKitTextField(PrKit* kit, PrLayoutNode parent, PrKitTextField field, PrKitFieldStyle style);

// What a text area edits and shows: text that wraps at the area's width, and `rows` lines of room, or 3 for zero.
typedef struct PrKitTextArea {
    PrUiKey key;
    PrStringView text;
    PrTextEditBuffer* PR_NULLABLE buffer;   // holds the text instead of `text`, for a caller that owns a buffer
    PrStringView placeholder;
    uint32_t rows;
    bool disabled;

    // How each style of the buffer's text looks, by PrTextEditStyle, with style zero first. A style past the table
    // looks like style zero. With no table, all of the text looks like the area's own.
    const PrKitTextAreaStyle* PR_NULLABLE styles;
    uint32_t styleCount;
} PrKitTextArea;

// Creates a text area, edited as a text field is, with line feeds of its own: Return inserts one, a triple press
// selects a line, and Up and Down move the caret between the lines as they wrap. Its text scrolls in its box to keep
// the caret in view.
PrLayoutNode prKitTextArea(PrKit* kit, PrLayoutNode parent, PrKitTextArea area, PrKitFieldStyle style);

// What a suggest field edits and offers: a text field, and the values that its list suggests, which the caller filters
// as the text changes.
typedef struct PrKitSuggestField {
    PrKitTextField field;
    const PrStringView* PR_NULLABLE suggestions;
    uint32_t count;
} PrKitSuggestField;

// Creates a suggest field: a text field whose list of suggestions opens below it as focus comes to it, if the list has
// any. Activating a suggestion replaces the text with it, as one gesture, closes the list, and leaves focus in the
// field.
PrLayoutNode prKitSuggestField(PrKit* kit, PrLayoutNode parent, PrKitSuggestField suggest, PrKitFieldStyle style);

// Returns true if the text control `field`, or the search row of the command palette whose list is `field`, changed its
// text in the current round, and writes the new text to `text`. The text is valid until the end of the round. A control
// shows the text that the caller gives it in the next round, so a caller that keeps its old text rejects the change.
bool prKitTextChanged(const PrKit* kit, PrLayoutNode field, PrStringView* text);

// ---------------------------------------------------------------------------------------------------------------------
// Sliders

// How a slider's track maps onto its range.
enum {
    PrKitSliderScale_Linear,
    PrKitSliderScale_Ratio,         // equal lengths of track span equal factors; the range must be positive
};
typedef uint8_t PrKitSliderScale;

// What a slider edits: `value` between `min` and `max`, or between 0 and 1 if both are zero. `step` is the change from
// one key press, as a fraction of the track, or a hundredth for zero.
typedef struct PrKitSlider {
    PrUiKey key;
    PrKitScalar value;
    double min;
    double max;
    float step;
    PrKitSliderScale scale;
    bool disabled;
} PrKitSlider;

// How a slider looks. Zero is medium.
typedef struct PrKitSliderStyle {
    PrKitControlSize size;
} PrKitSliderStyle;

// Creates a slider: a track, the run filled up to the value, and a knob at the value. A press sets the value at the
// pointer and a drag follows the pointer. Everything from the press to the release is one gesture. While the slider has
// focus, Left and Right move the value by its step, or by ten steps with Shift, and Home and End move it to its ends.
// In a row, the slider fills the room that its parent leaves.
PrLayoutNode prKitSlider(PrKit* kit, PrLayoutNode parent, PrKitSlider slider, PrKitSliderStyle style);

// ---------------------------------------------------------------------------------------------------------------------
// The inspector

// A group of an inspector's rows under a header that discloses them. `open` is the caller's flag, which an activation
// of the header flips. If `open` is null, the kit keeps a flag for the group, which starts open.
typedef struct PrKitInspectorGroup {
    PrUiKey key;
    PrStringView name;
    PrStringView badge;             // a tag after the name, or empty for none
    PrKitBadgeVariant badgeVariant;
    bool* PR_NULLABLE open;
} PrKitInspectorGroup;

// Creates a group and its header, and flips the group's flag if the header is activated. Returns the column that the
// group's rows go under, or none while the group is closed.
PrLayoutNode prKitInspectorGroup(PrKit* kit, PrLayoutNode parent, PrKitInspectorGroup group);

// The content of an inspector row: its label.
typedef struct PrKitInspectorRow {
    PrUiKey key;
    PrStringView label;
} PrKitInspectorRow;

// The style of an inspector row's label. Zero is the small body text.
typedef struct PrKitInspectorRowStyle {
    bool mono;
} PrKitInspectorRowStyle;

// Creates a row with its label in a column of fixed width. Returns the node that the row's controls go under, which
// fills the rest of the row. Any control may go under it.
PrLayoutNode prKitInspectorRow(PrKit* kit, PrLayoutNode parent, PrKitInspectorRow row, PrKitInspectorRowStyle style);

// A field that refers to an asset: the asset's name beside a thumbnail. The thumbnail is the icon, or a square of
// `tint` if the alpha of `tint` is above zero. The field is a drop target for payloads (pr/ui.h) of `kind`.
typedef struct PrKitAssetField {
    PrUiKey key;
    PrStringView name;
    PrKitIconKind icon;
    PrColor tint;
    uint64_t kind;
    bool missing;                   // the asset is unavailable
    bool empty;                     // no asset is referenced
    bool disabled;
} PrKitAssetField;

// Creates an asset field, with buttons in its gutter to browse for an asset, and to locate and clear the referenced
// one. A payload of the field's kind marks the field while it is in flight over it, and a payload of another kind
// passes it by. In a row, the field fills the room that its parent leaves.
PrLayoutNode prKitAssetField(PrKit* kit, PrLayoutNode parent, PrKitAssetField field);

// Return true if the asset field's browse, locate or clear button was activated in the current round.
bool prKitBrowsed(const PrKit* kit, PrLayoutNode field);
bool prKitLocated(const PrKit* kit, PrLayoutNode field);
bool prKitCleared(const PrKit* kit, PrLayoutNode field);

// Returns true if a payload of the asset field's kind was dropped on it in the current round, and writes the payload's
// value to `value`.
bool prKitDropped(const PrKit* kit, PrLayoutNode field, uint64_t* value);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
