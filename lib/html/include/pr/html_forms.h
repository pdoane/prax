// Forms: form controls, forms, and their entry lists
//
// html - https://html.spec.whatwg.org/multipage/forms.html
// html - https://html.spec.whatwg.org/multipage/form-elements.html
// html - https://html.spec.whatwg.org/multipage/input.html
// html - https://html.spec.whatwg.org/multipage/form-control-infrastructure.html
//
// A control's value, checkedness and form owner are its element's, so each call reads and writes them at once. A
// control's other members reflect its attributes, which the binding reads through the element.

#pragma once

#include "pr/base.h"
#include "pr/dom.h"
#include "pr/html.h"
#include "pr/status.h"
#include "pr/string.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// The interfaces that this header declares, each named for its web interface.
typedef struct PrHtmlFormElement PrHtmlFormElement;
typedef struct PrHtmlInputElement PrHtmlInputElement;
typedef struct PrHtmlButtonElement PrHtmlButtonElement;
typedef struct PrHtmlTextAreaElement PrHtmlTextAreaElement;
typedef struct PrHtmlFieldSetElement PrHtmlFieldSetElement;
typedef struct PrHtmlLegendElement PrHtmlLegendElement;
typedef struct PrHtmlLabelElement PrHtmlLabelElement;
typedef struct PrHtmlOutputElement PrHtmlOutputElement;
typedef struct PrHtmlProgressElement PrHtmlProgressElement;
typedef struct PrHtmlMeterElement PrHtmlMeterElement;
typedef struct PrHtmlDetailsElement PrHtmlDetailsElement;
typedef struct PrHtmlSelectElement PrHtmlSelectElement;
typedef struct PrHtmlOptionElement PrHtmlOptionElement;
typedef struct PrHtmlDataListElement PrHtmlDataListElement;
typedef struct PrHtmlFormData PrHtmlFormData;
typedef struct PrHtmlSubmitEvent PrHtmlSubmitEvent;
typedef struct PrHtmlFormDataEvent PrHtmlFormDataEvent;
typedef struct PrHtmlToggleEvent PrHtmlToggleEvent;
typedef struct PrHtmlCommandEvent PrHtmlCommandEvent;

// ---------------------------------------------------------------------------------------------------------------------
// Form-associated elements (html/#association-of-controls-and-forms)

// Returns the element's form owner (html/#form-owner), or null if it has none or is not form-associated. A legend's
// form is its parent fieldset's form owner (html/#dom-legend-form).
PrHtmlFormElement* PR_NULLABLE prHtmlElementForm(PrHtmlElement* element);

// Creates the live list of the labels whose labeled control is the element (html/#dom-lfe-labels), which the caller
// destroys. Returns null for an element that is not labelable, and for a hidden input.
PrDomNodeList* PR_NULLABLE prHtmlElementLabels(PrHtmlElement* element);

// Returns the URL that a form or a submit button's attribute `name` names, action or formaction, parsed against the
// document base URL (html/#dom-fs-action). A missing or empty attribute names the document's URL.
PrString prHtmlElementFormActionUrl(const PrHtmlElement* element, PrStringView name);

// ---------------------------------------------------------------------------------------------------------------------
// HTMLFormElement : HTMLElement (html)

// An HTMLFormElement as each interface it inherits from.
PR_HANDLE_UPCAST(HtmlFormElement, HtmlElement, HtmlElement)
PR_HANDLE_UPCAST(HtmlFormElement, DomElement, Element)
PR_HANDLE_UPCAST(HtmlFormElement, DomNode, Node)
PR_HANDLE_UPCAST(HtmlFormElement, DomEventTarget, EventTarget)

// Returns `object` as an HTMLFormElement, or null if it is not one.
PrHtmlFormElement* PR_NULLABLE prHtmlFormElementQuery(PrDomEventTarget* PR_NULLABLE object);

// Creates the live collection of the form's controls (html/#dom-form-elements), which the caller destroys.
PrDomHtmlCollection* prHtmlFormElementElements(PrHtmlFormElement* element);

// Creates the live list of the form's controls whose name or ID is `name` (html/#radionodelist), or of its img elements
// of that name or ID if `images` (html/#dom-form-nameditem). The caller destroys the list.
PrDomNodeList* prHtmlFormElementRadioNodeList(PrHtmlFormElement* element, PrStringView name, bool images);

// Finds the elements that the form's named getter returns for `name` (html/#dom-form-nameditem): its controls whose
// name or ID is `name`, or its img elements if it has no such control. Returns how many it found. Writes the element to
// `out` if there is exactly one, and the form's past names map remembers it. If there is none, writes the element that
// the map holds for `name` and counts it as one. Otherwise writes null, and the caller makes a list of them with
// prHtmlFormElementRadioNodeList.
uint32_t prHtmlFormElementNamedItem(PrHtmlFormElement* element, PrStringView name, PrDomElement* PR_NULLABLE* PR_NONNULL out);

// Writes up to `capacity` of the form's supported property names (html/#the-form-element:supported-property-names) to
// `items`, which the caller releases, and returns the total number of names.
uint32_t prHtmlFormElementNames(PrHtmlFormElement* formElement, PrString* PR_NULLABLE items, uint32_t capacity);

// ---------------------------------------------------------------------------------------------------------------------
// HTMLInputElement : HTMLElement (html)
//
// An image button loads its image by the same steps as an img element.

// An HTMLInputElement as each interface it inherits from.
PR_HANDLE_UPCAST(HtmlInputElement, HtmlElement, HtmlElement)
PR_HANDLE_UPCAST(HtmlInputElement, DomElement, Element)
PR_HANDLE_UPCAST(HtmlInputElement, DomNode, Node)
PR_HANDLE_UPCAST(HtmlInputElement, DomEventTarget, EventTarget)

// Returns `object` as an HTMLInputElement, or null if it is not one.
PrHtmlInputElement* PR_NULLABLE prHtmlInputElementQuery(PrDomEventTarget* PR_NULLABLE object);

// Returns the state of the type attribute, or Text if the attribute names no state (html/#dom-input-type).
PrDomInputType prHtmlInputElementType(const PrHtmlInputElement* element);

// Returns the input's value as its mode gives it (html/#dom-input-value), and sets it. Setting returns InvalidState,
// the setter's "InvalidStateError", for a value other than the empty string in the filename mode.
PrString prHtmlInputElementValue(const PrHtmlInputElement* element);
PrStatus prHtmlInputElementSetValue(PrHtmlInputElement* element, PrStringView value);

// Returns the checkedness (html/#dom-input-checked), and sets it with the dirty checkedness flag.
bool prHtmlInputElementChecked(const PrHtmlInputElement* element);
void prHtmlInputElementSetChecked(PrHtmlInputElement* element, bool value);

// Returns the indeterminate IDL attribute (html/#dom-input-indeterminate), and sets it.
bool prHtmlInputElementIndeterminate(const PrHtmlInputElement* element);
void prHtmlInputElementSetIndeterminate(PrHtmlInputElement* element, bool value);

// Returns the value as a number (html/#dom-input-valueasnumber), NaN if it is not one or the attribute does not apply.
double prHtmlInputElementValueAsNumber(const PrHtmlInputElement* element);

// Sets the value from a number, or to the empty string for NaN. Returns InvalidState if the attribute does not apply,
// and WrongType, the setter's TypeError, for an infinite number. Either changes nothing.
PrStatus prHtmlInputElementSetValueAsNumber(PrHtmlInputElement* element, double value);

// Writes the value as a Date's time value to `out` (html/#dom-input-valueasdate). Returns false for the attribute's
// null: a value that does not convert, or a type to which the attribute does not apply.
bool prHtmlInputElementValueAsDate(const PrHtmlInputElement* element, double* out);

// Sets the value from a Date's time value, or to the empty string for NaN, the time value of a null or invalid Date.
// Returns InvalidState, and changes nothing, if the attribute does not apply.
PrStatus prHtmlInputElementSetValueAsDate(PrHtmlInputElement* element, double time);

// Steps the value up or down by `n` times the allowed value step (html/#dom-input-stepup). Returns InvalidState if the
// methods do not apply or the input has no allowed value step.
PrStatus prHtmlInputElementStepUp(PrHtmlInputElement* element, int32_t n);
PrStatus prHtmlInputElementStepDown(PrHtmlInputElement* element, int32_t n);

// Returns the datalist that the list attribute names (html/#dom-input-list), or null if it names none.
PrDomElement* PR_NULLABLE prHtmlInputElementList(PrHtmlInputElement* element);

// Returns an image button's image size, computed by the rules of prHtmlImageElementWidth, or zero for any other input
// (html/#dom-input-width). The setters set the width or height content attribute.
uint32_t prHtmlInputElementWidth(const PrHtmlInputElement* element);
uint32_t prHtmlInputElementHeight(const PrHtmlInputElement* element);
void prHtmlInputElementSetWidth(PrHtmlInputElement* element, uint32_t value);
void prHtmlInputElementSetHeight(PrHtmlInputElement* element, uint32_t value);

// ---------------------------------------------------------------------------------------------------------------------
// HTMLButtonElement : HTMLElement (html)

// An HTMLButtonElement as each interface it inherits from.
PR_HANDLE_UPCAST(HtmlButtonElement, HtmlElement, HtmlElement)
PR_HANDLE_UPCAST(HtmlButtonElement, DomElement, Element)
PR_HANDLE_UPCAST(HtmlButtonElement, DomNode, Node)
PR_HANDLE_UPCAST(HtmlButtonElement, DomEventTarget, EventTarget)

// Returns `object` as an HTMLButtonElement, or null if it is not one.
PrHtmlButtonElement* PR_NULLABLE prHtmlButtonElementQuery(PrDomEventTarget* PR_NULLABLE object);

// Returns the type IDL attribute (html/#dom-button-type): "submit" for a submit button, "button" for the Auto state of
// any other button, and otherwise the keyword of its state. The text lives as long as the process.
PrStringView prHtmlButtonElementType(const PrHtmlButtonElement* element);

// ---------------------------------------------------------------------------------------------------------------------
// HTMLTextAreaElement : HTMLElement (html)

// An HTMLTextAreaElement as each interface it inherits from.
PR_HANDLE_UPCAST(HtmlTextAreaElement, HtmlElement, HtmlElement)
PR_HANDLE_UPCAST(HtmlTextAreaElement, DomElement, Element)
PR_HANDLE_UPCAST(HtmlTextAreaElement, DomNode, Node)
PR_HANDLE_UPCAST(HtmlTextAreaElement, DomEventTarget, EventTarget)

// Returns `object` as an HTMLTextAreaElement, or null if it is not one.
PrHtmlTextAreaElement* PR_NULLABLE prHtmlTextAreaElementQuery(PrDomEventTarget* PR_NULLABLE object);

// Returns the API value (html/#dom-textarea-value), and sets the raw value with the dirty value flag.
PrString prHtmlTextAreaElementValue(const PrHtmlTextAreaElement* element);
void prHtmlTextAreaElementSetValue(PrHtmlTextAreaElement* element, PrStringView value);

// Returns the child text content (html/#dom-textarea-defaultvalue), and replaces the children with a text node of
// `value`.
PrString prHtmlTextAreaElementDefaultValue(const PrHtmlTextAreaElement* element);
void prHtmlTextAreaElementSetDefaultValue(PrHtmlTextAreaElement* element, PrStringView value);

// Returns the length of the API value in UTF-16 code units (html/#dom-textarea-textlength).
uint32_t prHtmlTextAreaElementTextLength(const PrHtmlTextAreaElement* element);

// ---------------------------------------------------------------------------------------------------------------------
// HTMLFieldSetElement, HTMLLegendElement and HTMLLabelElement : HTMLElement (html)

// Each interface as each interface it inherits from.
PR_HANDLE_UPCAST(HtmlFieldSetElement, HtmlElement, HtmlElement)
PR_HANDLE_UPCAST(HtmlFieldSetElement, DomElement, Element)
PR_HANDLE_UPCAST(HtmlFieldSetElement, DomNode, Node)
PR_HANDLE_UPCAST(HtmlFieldSetElement, DomEventTarget, EventTarget)
PR_HANDLE_UPCAST(HtmlLegendElement, HtmlElement, HtmlElement)
PR_HANDLE_UPCAST(HtmlLegendElement, DomElement, Element)
PR_HANDLE_UPCAST(HtmlLegendElement, DomNode, Node)
PR_HANDLE_UPCAST(HtmlLegendElement, DomEventTarget, EventTarget)
PR_HANDLE_UPCAST(HtmlLabelElement, HtmlElement, HtmlElement)
PR_HANDLE_UPCAST(HtmlLabelElement, DomElement, Element)
PR_HANDLE_UPCAST(HtmlLabelElement, DomNode, Node)
PR_HANDLE_UPCAST(HtmlLabelElement, DomEventTarget, EventTarget)

// Return `object` as each interface, or null if it is not one.
PrHtmlFieldSetElement* PR_NULLABLE prHtmlFieldSetElementQuery(PrDomEventTarget* PR_NULLABLE object);
PrHtmlLegendElement* PR_NULLABLE prHtmlLegendElementQuery(PrDomEventTarget* PR_NULLABLE object);
PrHtmlLabelElement* PR_NULLABLE prHtmlLabelElementQuery(PrDomEventTarget* PR_NULLABLE object);

// Creates the live collection of the listed elements under the fieldset (html/#dom-fieldset-elements), which the caller
// destroys.
PrDomHtmlCollection* prHtmlFieldSetElementElements(PrHtmlFieldSetElement* element);

// Returns the label's labeled control (html/#dom-label-control), or null if it has none.
PrDomElement* PR_NULLABLE prHtmlLabelElementControl(PrHtmlLabelElement* element);

// ---------------------------------------------------------------------------------------------------------------------
// HTMLOutputElement : HTMLElement (html)

// An HTMLOutputElement as each interface it inherits from.
PR_HANDLE_UPCAST(HtmlOutputElement, HtmlElement, HtmlElement)
PR_HANDLE_UPCAST(HtmlOutputElement, DomElement, Element)
PR_HANDLE_UPCAST(HtmlOutputElement, DomNode, Node)
PR_HANDLE_UPCAST(HtmlOutputElement, DomEventTarget, EventTarget)

// Returns `object` as an HTMLOutputElement, or null if it is not one.
PrHtmlOutputElement* PR_NULLABLE prHtmlOutputElementQuery(PrDomEventTarget* PR_NULLABLE object);

// Returns the output's descendant text content (html/#dom-output-value), and sets it, keeping the default value that
// the output had as its default value override.
PrString prHtmlOutputElementValue(const PrHtmlOutputElement* element);
void prHtmlOutputElementSetValue(PrHtmlOutputElement* element, PrStringView value);

// Returns the default value (html/#dom-output-defaultvalue): the default value override, or the descendant text content
// if there is none. Setting it replaces the descendant text content while there is no override, and replaces the
// override otherwise.
PrString prHtmlOutputElementDefaultValue(const PrHtmlOutputElement* element);
void prHtmlOutputElementSetDefaultValue(PrHtmlOutputElement* element, PrStringView value);

// ---------------------------------------------------------------------------------------------------------------------
// HTMLProgressElement and HTMLMeterElement : HTMLElement (html)

// Each interface as each interface it inherits from.
PR_HANDLE_UPCAST(HtmlProgressElement, HtmlElement, HtmlElement)
PR_HANDLE_UPCAST(HtmlProgressElement, DomElement, Element)
PR_HANDLE_UPCAST(HtmlProgressElement, DomNode, Node)
PR_HANDLE_UPCAST(HtmlProgressElement, DomEventTarget, EventTarget)
PR_HANDLE_UPCAST(HtmlMeterElement, HtmlElement, HtmlElement)
PR_HANDLE_UPCAST(HtmlMeterElement, DomElement, Element)
PR_HANDLE_UPCAST(HtmlMeterElement, DomNode, Node)
PR_HANDLE_UPCAST(HtmlMeterElement, DomEventTarget, EventTarget)

// Return `object` as each interface, or null if it is not one.
PrHtmlProgressElement* PR_NULLABLE prHtmlProgressElementQuery(PrDomEventTarget* PR_NULLABLE object);
PrHtmlMeterElement* PR_NULLABLE prHtmlMeterElementQuery(PrDomEventTarget* PR_NULLABLE object);

// Return a progress element's current value and maximum value (html/#concept-progress-value), and its position, which
// is -1 for an indeterminate progress bar (html/#dom-progress-position).
double prHtmlProgressElementValue(const PrHtmlProgressElement* element);
double prHtmlProgressElementMax(const PrHtmlProgressElement* element);
double prHtmlProgressElementPosition(const PrHtmlProgressElement* element);

// The values of a meter (html/#concept-meter-actual), each after the constraints that the others put on it.
typedef struct PrHtmlMeterValues {
    double value;
    double min;
    double max;
    double low;
    double high;
    double optimum;
} PrHtmlMeterValues;

// Returns the meter's values.
PrHtmlMeterValues prHtmlMeterElementValues(const PrHtmlMeterElement* element);

// Sets the content attribute `name` of a progress or a meter element to the best representation of `value` as a
// floating-point number, as a reflecting double attribute does. A progress element's max is set only for a value above
// zero. Returns WrongType, the setter's TypeError, for a value that is not finite, and sets nothing.
PrStatus prHtmlElementSetReflectedDouble(PrHtmlElement* element, PrStringView name, double value);

// ---------------------------------------------------------------------------------------------------------------------
// HTMLDetailsElement : HTMLElement (html)

// An HTMLDetailsElement as each interface it inherits from.
PR_HANDLE_UPCAST(HtmlDetailsElement, HtmlElement, HtmlElement)
PR_HANDLE_UPCAST(HtmlDetailsElement, DomElement, Element)
PR_HANDLE_UPCAST(HtmlDetailsElement, DomNode, Node)
PR_HANDLE_UPCAST(HtmlDetailsElement, DomEventTarget, EventTarget)

// Returns `object` as an HTMLDetailsElement, or null if it is not one.
PrHtmlDetailsElement* PR_NULLABLE prHtmlDetailsElementQuery(PrDomEventTarget* PR_NULLABLE object);

// ---------------------------------------------------------------------------------------------------------------------
// FormData (xhr)
//
// A list of entries, each a name and a string value (xhr/#interface-formdata). A FormData is reference counted, and a
// FormDataEvent holds one. A File entry waits for the File API.

// Creates a FormData with one reference for the caller (xhr/#dom-formdata). Without a form, its entry list is empty.
// With one, the list is the form's entry list (html/#constructing-the-form-data-set) with `submitter`, which fires
// formdata at the form. Returns WrongType, the constructor's TypeError, for a submitter that is not a submit button,
// NotFound for one whose form owner is not `form`, and InvalidState for a form that is already constructing its entry
// list. It then writes null.
PrStatus prHtmlFormDataCreate(PrHtmlFormElement* PR_NULLABLE form, PrHtmlElement* PR_NULLABLE submitter,
    PrHtmlFormData* PR_NULLABLE* PR_NONNULL out);

// Adds or releases a reference. The last release frees the FormData.
void prHtmlFormDataRetain(PrHtmlFormData* formData);
void prHtmlFormDataRelease(PrHtmlFormData* PR_NULLABLE formData);

// Appends an entry (xhr/#dom-formdata-append).
void prHtmlFormDataAppend(PrHtmlFormData* formData, PrStringView name, PrStringView value);

// Removes every entry named `name` (xhr/#dom-formdata-delete).
void prHtmlFormDataDelete(PrHtmlFormData* formData, PrStringView name);

// Returns the value of the first entry named `name`, or the null view if there is none (xhr/#dom-formdata-get).
PrStringView prHtmlFormDataGet(const PrHtmlFormData* formData, PrStringView name);

// Writes up to `capacity` of the values of the entries named `name` to `items`, and returns the number of such entries
// (xhr/#dom-formdata-getall).
uint32_t prHtmlFormDataGetAll(const PrHtmlFormData* formData, PrStringView name, PrStringView* PR_NULLABLE items,
    uint32_t capacity);

// Returns true if an entry is named `name` (xhr/#dom-formdata-has).
bool prHtmlFormDataHas(const PrHtmlFormData* formData, PrStringView name);

// Replaces the first entry named `name` and removes the others, or appends an entry if none is named `name`
// (xhr/#dom-formdata-set).
void prHtmlFormDataSet(PrHtmlFormData* formData, PrStringView name, PrStringView value);

// Returns the number of entries, and writes the name and value of the entry at `index`. Returns false past the last
// entry.
uint32_t prHtmlFormDataCount(const PrHtmlFormData* formData);
bool prHtmlFormDataEntry(const PrHtmlFormData* formData, uint32_t index, PrStringView* name, PrStringView* value);

// ---------------------------------------------------------------------------------------------------------------------
// SubmitEvent, FormDataEvent, ToggleEvent and CommandEvent : Event (html)

// Each event as the interface it inherits from.
PR_HANDLE_UPCAST(HtmlSubmitEvent, DomEvent, Event)
PR_HANDLE_UPCAST(HtmlFormDataEvent, DomEvent, Event)
PR_HANDLE_UPCAST(HtmlToggleEvent, DomEvent, Event)
PR_HANDLE_UPCAST(HtmlCommandEvent, DomEvent, Event)

// Return `object` as each event, or null if it is not one.
PrHtmlSubmitEvent* PR_NULLABLE prHtmlSubmitEventQuery(PrDomEvent* PR_NULLABLE object);
PrHtmlFormDataEvent* PR_NULLABLE prHtmlFormDataEventQuery(PrDomEvent* PR_NULLABLE object);
PrHtmlToggleEvent* PR_NULLABLE prHtmlToggleEventQuery(PrDomEvent* PR_NULLABLE object);
PrHtmlCommandEvent* PR_NULLABLE prHtmlCommandEventQuery(PrDomEvent* PR_NULLABLE object);

// Create each event of `type`, with one reference that the caller holds. An event holds a reference to its submitter,
// source and form data, and copies its strings.
PrHtmlSubmitEvent* prHtmlSubmitEventCreate(PrStringView type, const PrDomEventInit* PR_NULLABLE init,
    PrHtmlElement* PR_NULLABLE submitter);
PrHtmlFormDataEvent* prHtmlFormDataEventCreate(PrStringView type, const PrDomEventInit* PR_NULLABLE init,
    PrHtmlFormData* formData);
PrHtmlToggleEvent* prHtmlToggleEventCreate(PrStringView type, const PrDomEventInit* PR_NULLABLE init,
    PrStringView oldState, PrStringView newState, PrDomElement* PR_NULLABLE source);
PrHtmlCommandEvent* prHtmlCommandEventCreate(PrStringView type, const PrDomEventInit* PR_NULLABLE init,
    PrStringView command, PrDomElement* PR_NULLABLE source);

// html/#dom-submitevent-submitter and html/#dom-formdataevent-formdata
PrHtmlElement* PR_NULLABLE prHtmlSubmitEventSubmitter(PrHtmlSubmitEvent* event);
PrHtmlFormData* prHtmlFormDataEventFormData(PrHtmlFormDataEvent* event);

// html/#dom-toggleevent-oldstate, html/#dom-toggleevent-newstate and html/#dom-commandevent-command
PrStringView prHtmlToggleEventOldState(const PrHtmlToggleEvent* event);
PrStringView prHtmlToggleEventNewState(const PrHtmlToggleEvent* event);
PrStringView prHtmlCommandEventCommand(const PrHtmlCommandEvent* event);

// Returns the event's source retargeted against its current target, as html/#dom-toggleevent-source and
// html/#dom-commandevent-source read it, or null if it has none.
PrDomElement* PR_NULLABLE prHtmlToggleEventSource(PrHtmlToggleEvent* event);
PrDomElement* PR_NULLABLE prHtmlCommandEventSource(PrHtmlCommandEvent* event);

// ---------------------------------------------------------------------------------------------------------------------
// HTMLSelectElement : HTMLElement (html)

// An HTMLSelectElement as each interface it inherits from.
PR_HANDLE_UPCAST(HtmlSelectElement, HtmlElement, HtmlElement)
PR_HANDLE_UPCAST(HtmlSelectElement, DomElement, Element)
PR_HANDLE_UPCAST(HtmlSelectElement, DomNode, Node)
PR_HANDLE_UPCAST(HtmlSelectElement, DomEventTarget, EventTarget)

// Returns `object` as an HTMLSelectElement, or null if it is not one.
PrHtmlSelectElement* PR_NULLABLE prHtmlSelectElementQuery(PrDomEventTarget* PR_NULLABLE object);

// Returns "select-multiple" or "select-one" (html/#dom-select-type). The text lives as long as the process.
PrStringView prHtmlSelectElementType(const PrHtmlSelectElement* select);

// Create the live collections of the select's list of options and of those that are selected (html/#dom-select-options,
// html/#dom-select-selectedoptions). The caller destroys each.
PrDomHtmlCollection* prHtmlSelectElementOptions(PrHtmlSelectElement* select);
PrDomHtmlCollection* prHtmlSelectElementSelectedOptions(PrHtmlSelectElement* select);

// Returns the number of options in the list.
uint32_t prHtmlSelectElementLength(PrHtmlSelectElement* select);

// Appends new options, or removes the last ones, until the list has `length` options. A length above 100000 changes
// nothing (html/#dom-htmloptionscollection-length).
void prHtmlSelectElementSetLength(PrHtmlSelectElement* select, uint32_t length);

// Returns the option at `index` of the list, or null past the end.
PrHtmlOptionElement* PR_NULLABLE prHtmlSelectElementItem(PrHtmlSelectElement* select, uint32_t index);

// Puts `option` at `index` of the list, which replaces the option there, or removes that option for a null `option`. An
// index past the end adds new options up to it (html/#dom-htmloptionscollection-setter). Returns what inserting
// returns.
PrStatus prHtmlSelectElementSetItem(PrHtmlSelectElement* select, uint32_t index, PrHtmlOptionElement* PR_NULLABLE option);

// Inserts `element`, an option or optgroup, before `before`, or at the end of the select for null, or before the option
// at `index` (html/#dom-htmloptionscollection-add). An index past the end appends. Returns NotAllowed if `element`
// contains the select, and NotFound if `before` is not under the select.
PrStatus prHtmlSelectElementAdd(PrHtmlSelectElement* select, PrHtmlElement* element, PrHtmlElement* PR_NULLABLE before);
PrStatus prHtmlSelectElementAddAt(PrHtmlSelectElement* select, PrHtmlElement* element, uint32_t index);

// Removes the option at `index` of the list, if there is one (html/#dom-htmloptionscollection-remove).
void prHtmlSelectElementRemove(PrHtmlSelectElement* select, int32_t index);

// Returns the index of the first selected option, or -1, and makes the option at `index` the only one selected
// (html/#dom-select-selectedindex).
int32_t prHtmlSelectElementSelectedIndex(PrHtmlSelectElement* select);
void prHtmlSelectElementSetSelectedIndex(PrHtmlSelectElement* select, int32_t index);

// Returns the value of the first selected option, or the empty string, and makes the first option with `value` the only
// one selected (html/#dom-select-value).
PrString prHtmlSelectElementValue(PrHtmlSelectElement* select);
void prHtmlSelectElementSetValue(PrHtmlSelectElement* select, PrStringView value);

// ---------------------------------------------------------------------------------------------------------------------
// HTMLOptionElement : HTMLElement (html)

// An HTMLOptionElement as each interface it inherits from.
PR_HANDLE_UPCAST(HtmlOptionElement, HtmlElement, HtmlElement)
PR_HANDLE_UPCAST(HtmlOptionElement, DomElement, Element)
PR_HANDLE_UPCAST(HtmlOptionElement, DomNode, Node)
PR_HANDLE_UPCAST(HtmlOptionElement, DomEventTarget, EventTarget)

// Returns `object` as an HTMLOptionElement, or null if it is not one.
PrHtmlOptionElement* PR_NULLABLE prHtmlOptionElementQuery(PrDomEventTarget* PR_NULLABLE object);

// What a new option starts with. A null view of `text` or `value` leaves it out.
typedef struct PrHtmlOptionDesc {
    PrStringView text;
    PrStringView value;
    bool defaultSelected;       // sets the selected attribute
    bool selected;
} PrHtmlOptionDesc;

// Creates an option in `document`, with one reference for the caller (html/#dom-option).
PrHtmlOptionElement* prHtmlOptionElementCreate(PrDomDocument* document, const PrHtmlOptionDesc* desc);

// Returns the form owner of the option's select, or null (html/#dom-option-form).
PrHtmlFormElement* PR_NULLABLE prHtmlOptionElementForm(PrHtmlOptionElement* option);

// Returns the option's text (html/#dom-option-text), and replaces its children with a text node of `value`.
PrString prHtmlOptionElementText(const PrHtmlOptionElement* option);
void prHtmlOptionElementSetText(PrHtmlOptionElement* option, PrStringView value);

// Return the value attribute or the text, and the label attribute if it is not empty or the text
// (html/#dom-option-value, html/#dom-option-label).
PrString prHtmlOptionElementValue(const PrHtmlOptionElement* option);
PrString prHtmlOptionElementLabel(const PrHtmlOptionElement* option);

// Returns the selectedness, and sets it as a script does (html/#dom-option-selected).
bool prHtmlOptionElementSelected(const PrHtmlOptionElement* option);
void prHtmlOptionElementSetSelected(PrHtmlOptionElement* option, bool value);

// Returns the option's index in its select's list of options, or 0 if it has no select (html/#dom-option-index).
uint32_t prHtmlOptionElementIndex(PrHtmlOptionElement* option);

// ---------------------------------------------------------------------------------------------------------------------
// HTMLDataListElement : HTMLElement (html)

// Returns `object` as an HTMLDataListElement, or null if it is not one.
PrHtmlDataListElement* PR_NULLABLE prHtmlDataListElementQuery(PrDomEventTarget* PR_NULLABLE object);

// Creates the live collection of the option descendants (html/#dom-datalist-options), which the caller destroys.
PrDomHtmlCollection* prHtmlDataListElementOptions(PrHtmlDataListElement* list);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
