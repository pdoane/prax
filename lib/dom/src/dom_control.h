// Form controls
//
// html - https://html.spec.whatwg.org/multipage/forms.html
//
// The state that HTML gives form-associated elements and forms: a control's value, its checkedness and its form owner,
// and the controls that a form owns. What script and the user change is held in a record that the element creates on
// first use (DomElementState::control), and what follows from the attributes alone is read from them. An input whose
// value is not empty, or that is checked, has a record.
//
// The steps that keep the state current run as the tree changes: when a control or its ancestor is inserted or removed,
// when its attributes change, and when it is cloned. A document runs none of them until it creates a form-associated
// element or a form (DomDocument::controls).

#pragma once

#include "pr/dom.h"
#include "pr/raw_array.h"
#include "pr/raw_string.h"
#include "pr/string_view.h"
#include "pr/text_buffer.h"
#include "pr/text_edit.h"

#include "dom/src/dom_node.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// The state of a form-associated element. A textarea's value is its raw value (html/#concept-textarea-raw-value).
struct DomControl {
    // The text of a text control, created when the control first holds a value. It holds an input's value in the value
    // mode. It holds a textarea's raw value if the textarea is dirty, and otherwise follows the child text content.
    PrTextEditBuffer* PR_NULLABLE text;
    PrTextEditView* PR_NULLABLE view;       // holds the selection in `text`, as bytes (html/#textFieldSelection)
    PrTextEditHistory* PR_NULLABLE history; // the user's edits, from the first of them

    // The text that an input method is composing, which the next update replaces. Empty if none is.
    uint32_t markStart;
    uint32_t markEnd;

    PrDomSelectionDirection selectionDirection;
    bool selectQueued;                  // a task to fire select is queued (html/#textFieldSelection)
    PrRawString defaultOverride;        // html/#concept-output-default-value-override, or the null string for none
    PrDomElement* PR_NULLABLE form;     // html/#form-owner, which lists the element among its controls
    bool dirtyValue;                    // html/#concept-fe-dirty
    bool checked;                       // html/#concept-fe-checked
    bool dirtyChecked;                  // html/#concept-input-checked-dirty-flag
    bool indeterminate;                 // html/#dom-input-indeterminate
    bool parserInserted;                // html/#parser-inserted-flag
};

// An entry of a form's past names map (html/#past-names-map): a name that the form's named getter found an element by.
struct DomPastName {
    PrRawString name;
    PrDomElement* element;
};

// The state of a form element.
struct DomForm {
    // The form-associated elements whose form owner is the form, in no order. An element of the list is freed only
    // after it leaves the list, and the form clears its elements' owner when it is freed.
    RawArray<PrDomElement* PR_NONNULL> controls;

    // The past names map, whose elements are form-associated elements that the form owns. An element's entries leave
    // the map when the element leaves the form.
    RawArray<DomPastName> pastNames;

    bool constructingEntryList;         // html/#constructing-entry-list
};

// One entry of an entry list (xhr/#concept-formdata-entry), whose value is a string.
struct DomEntry {
    PrRawString name;
    PrRawString value;
};

// An entry list (xhr/#concept-formdata-entry-list), which a FormData and a FormDataEvent share. It counts the
// references to it.
struct DomEntryList {
    RawArray<DomEntry> entries;
    uint32_t references;
};

// Creates an empty entry list, with one reference that the caller holds.
DomEntryList* domEntryListCreate();

// Adds or releases a reference. The last release frees the list.
void domEntryListRetain(DomEntryList* list);
void domEntryListRelease(DomEntryList* PR_NULLABLE list);

// Appends an entry to the list.
void domEntryListAppend(DomEntryList* list, PrStringView name, PrStringView value);

// ---------------------------------------------------------------------------------------------------------------------
// Categories (html/#categories)

// Returns true if the element is form-associated (html/#form-associated-element): an HTML button, fieldset, img, input,
// object, output, select or textarea.
bool domIsFormAssociated(const PrDomElement* element);

// Returns true if the element is listed (html/#category-listed): form-associated and not an img.
bool domIsListed(const PrDomElement* element);

// Returns true if the element is submittable (html/#category-submit): a button, input, select or textarea.
bool domIsSubmittable(const PrDomElement* element);

// Returns true if the element is resettable (html/#category-reset): an input, output, select or textarea.
bool domIsResettable(const PrDomElement* element);

// Returns true if the element is labelable (html/#category-label).
bool domIsLabelable(const PrDomElement* element);

// Returns true if the element is a button (html/#concept-button): a button element, or an input that is a submit,
// image, reset or button input.
bool domIsButton(const PrDomElement* element);

// Returns true if the element is a submit button (html/#concept-submit-button): a submit or image input, or a button
// element whose type is submit, or auto without a command, a commandfor, or a select parent.
bool domIsSubmitButton(const PrDomElement* element);

// Returns the record of a form-associated element, or of a form, creating it if the element has none.
DomControl* domControlFromElement(PrDomElement* element);
DomForm* domFormFromElement(PrDomElement* form);

// Returns the record of the element, or null if it has none.
const DomControl* PR_NULLABLE domControl(const PrDomElement* element);

// Maps `name` to `element` in the form's past names map, or returns the element that the map holds for `name`, or null.
void domFormRememberName(PrDomElement* form, PrStringView name, PrDomElement* element);
PrDomElement* PR_NULLABLE domFormPastName(const PrDomElement* form, PrStringView name);

// ---------------------------------------------------------------------------------------------------------------------
// Values

// The modes of an input's value IDL attribute (html/#dom-input-value).
enum {
    DomValueMode_Value,
    DomValueMode_Default,
    DomValueMode_DefaultOn,
    DomValueMode_Filename,
};
typedef uint8_t DomValueMode;

// Returns the state of the input's type attribute.
PrDomInputType domElementInputType(const PrDomElement* input);

// Returns the value mode of the input type `type`.
DomValueMode domInputValueMode(PrDomInputType type);

// Returns the text of a text control, valid until the text changes. Text of no value is empty.
PrStringView domControlText(const DomControl* control);

// Replaces the text of a text control, and keeps the selection at its offsets, clamped to the text.
void domControlSetText(DomControl* control, PrStringView text);

// Returns the state of the text control `element`, with its text current with its API value.
DomControl* domTextControlSync(PrDomElement* element);

// Writes the value of a form control (html/#concept-fe-value) to `out`, which it clears first: an input's value in the
// value mode, its value attribute in the default modes, or for a textarea its API value
// (html/#concept-textarea-api-value). An input in the filename mode has no files, and its value is empty.
void domControlValue(const PrDomElement* element, PrTextBuffer* out);

// Returns true if the control's value is empty, without building it.
bool domControlValueEmpty(const PrDomElement* element);

// Sets an input's value as the value IDL attribute does in the input's mode (html/#dom-input-value), or a textarea's
// raw value as its value IDL attribute does (html/#dom-textarea-value). Returns InvalidState, the setter's
// "InvalidStateError", for a value other than the empty string in the filename mode.
PrStatus domControlSetValue(PrDomElement* element, PrStringView value);

// Writes the textarea's child text content (html/#dom-textarea-defaultvalue) to `out`, which it clears first.
void domTextareaDefaultValue(const PrDomElement* textarea, PrTextBuffer* out);

// Runs the steps that follow a change to the children of a textarea that is not dirty: its raw value follows its child
// text content, which restyles it. Does nothing for any other element.
void domTextareaChildrenChanged(PrDomElement* element);

// ---------------------------------------------------------------------------------------------------------------------
// Numbers, dates and times of an input (html/#common-input-element-apis)

// The members of an input that apply to its type (html/#concept-input-apply).
enum {
    DomInputApplyFlag_ValueAsDate = 1 << 0,
    DomInputApplyFlag_ValueAsNumber = 1 << 1,   // valueAsNumber, stepUp, stepDown, and the step, min and max
};
typedef uint8_t DomInputApplyFlags;

// Returns the members that apply to an input of `type`.
DomInputApplyFlags domInputApplies(PrDomInputType type);

// Converts a string to a number by the rules of the input's type (html/#concept-input-value-string-number). Writes the
// number to `out` and returns true, or returns false for a string that does not convert or a type without the rules.
bool domInputStringToNumber(const PrDomElement* input, PrStringView text, double* out);

// Converts a number to a string by the rules of the input's type (html/#concept-input-value-number-string), and appends
// it to `out`. `value` must be finite.
void domInputNumberToString(const PrDomElement* input, double value, PrTextBuffer* out);

// Converts a string to the time value of a Date, in milliseconds since the epoch, by the rules of a type to which
// valueAsDate applies (html/#concept-input-value-string-date). Returns false for a string that does not convert.
bool domInputStringToDate(const PrDomElement* input, PrStringView text, double* out);

// Converts the time value of a Date to a string by the rules of a type to which valueAsDate applies
// (html/#concept-input-value-date-string), and appends it to `out`. `time` must be finite.
void domInputDateToString(const PrDomElement* input, double time, PrTextBuffer* out);

// The minimum, maximum and allowed value step of an input, as numbers (html/#concept-input-min,
// html/#concept-input-max, html/#concept-input-step). A bound is NaN where the input has none, and the step is NaN if
// the input has no allowed value step.
struct DomInputRange {
    double minimum;
    double maximum;
    double step;
    double stepBase;                    // html/#concept-input-min-zero
};

// Returns the range of an input to which valueAsNumber applies.
DomInputRange domInputRange(const PrDomElement* input);

// Steps the input's value by `n` steps, up or down (html/#dom-input-stepup). Returns InvalidState if the methods do not
// apply or the input has no allowed value step, and changes nothing in that case.
PrStatus domInputStep(PrDomElement* input, int32_t n, bool up);

// ---------------------------------------------------------------------------------------------------------------------
// Checkedness

// Returns the checkedness of an input (html/#concept-fe-checked), which every input has and only a checkbox and a radio
// show.
bool domInputChecked(const PrDomElement* input);

// Sets the checkedness of a checkbox or a radio, and its dirty checkedness flag if `dirty`. A radio that becomes
// checked unchecks the other radios of its group. Each change restyles the elements that it changes.
void domInputSetChecked(PrDomElement* input, bool checked, bool dirty);

// Returns true if the input's indeterminate IDL attribute is set, and sets it, which restyles a checkbox.
bool domInputIndeterminate(const PrDomElement* input);
void domInputSetIndeterminate(PrDomElement* input, bool value);

// Returns true if a radio in the group of `radio` (html/#radio-button-group) is checked.
bool domRadioGroupChecked(const PrDomElement* radio);

// ---------------------------------------------------------------------------------------------------------------------
// Form owners (html/#association-of-controls-and-forms)

// Returns the element's form owner (html/#form-owner), or null if it has none.
PrDomElement* PR_NULLABLE domFormOwner(const PrDomElement* element);

// Associates a form-associated element that the parser creates with `form`, the parser's form element pointer, and sets
// its parser inserted flag (html/#create-an-element-for-the-token). The parser calls it only where the rules of that
// step allow.
void domFormAssociateParserInserted(PrDomElement* element, PrDomElement* form);

// Resets the form owner of a form-associated element (html/#reset-the-form-owner).
void domFormResetOwner(PrDomElement* element);

// Run the HTML insertion steps (html/#html-element-insertion-steps) and removing steps
// (html/#html-element-removing-steps) for the form-associated elements in the tree at `node`, which has been linked
// under its parent or removed from `parent`. A form-associated element resets its form owner, and a connected radio
// that is checked unchecks the rest of its group. An element with an ID that joins a document resets the owners of the
// controls with a form attribute.
void domControlsLinked(PrDomNode* node);
void domControlsUnlinked(PrDomNode* node, PrDomNode* parent);

// Resets the owners of the controls with a form attribute if an element with an ID left the document with the tree at
// `node`, which is disconnected and whose IDs the document no longer holds.
void domControlsDisconnected(PrDomNode* node);

// Runs the steps of a change to an attribute that a control's state reads: its value, type, checked, name or form
// attribute, or one that its type's value sanitization reads. The old value is the null view for none.
void domControlAttributeChanged(PrDomElement* element, PrStringView name, PrStringView oldValue);

// Runs the steps of an id attribute change of a connected element: the owners of the controls with a form attribute
// reset.
void domControlsIdChanged(PrDomElement* element);

// Copies the state that the cloning steps of an input or a textarea copy
// (html/#the-input-element:concept-node-clone-ext) from `element` to `clone`.
void domControlCloned(const PrDomElement* element, PrDomElement* clone);

// Releases the record of a form-associated element or a form, before the element is freed.
void domControlDestroy(PrDomElement* element);

// Records that the document holds form-associated elements or forms, after one is created in it.
void domControlCreated(PrDomElement* element);

// Moves what the element's documents record of it from `from` to its document, after it is adopted.
void domControlAdopted(PrDomElement* element, PrDomDocument* from);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
