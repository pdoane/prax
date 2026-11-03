// Forms: form controls, forms, and their entry lists
//
// html - https://html.spec.whatwg.org/multipage/forms.html

#include "pr/html_forms.h"
#include "html/src/html_forms.h"

#include "pr/alloc.h"
#include "pr/ascii.h"
#include "pr/color_space.h"
#include "pr/css.h"
#include "pr/float.h"
#include "pr/memory.h"
#include "pr/sort.h"
#include "pr/string_ops.h"
#include "pr/text_buffer.h"
#include "pr/text_edit.h"
#include "pr/url.h"

#include "dom/src/dom_attribute.h"
#include "dom/src/dom_collection.h"
#include "dom/src/dom_control.h"
#include "dom/src/dom_element_state.h"
#include "dom/src/dom_event.h"
#include "dom/src/dom_hooks.h"
#include "dom/src/dom_microsyntax.h"
#include "dom/src/dom_node.h"
#include "dom/src/dom_range.h"
#include "dom/src/dom_select.h"
#include "html/src/html_interface.h"

namespace pr_impl {
namespace {

PrRawString stringOf(const PrTextBuffer* text) {
    return prRawStringCopy(prTextBufferView(text));
}

// Returns the number that `name` of `element` parses to by the rules for parsing floating-point number values, or
// `fallback` if it has no such attribute or it does not parse.
double parsedOr(const PrDomElement* element, PrStringView name, double fallback) {
    double value = 0;
    PrStringView text = domAttributeValue(element, name);
    return text.data != nullptr && domParseFloat(text, &value) ? value : fallback;
}

// Returns true if `element` has a datalist ancestor.
bool inDatalist(const PrDomElement* element) {
    for (const PrDomNode* at = element->parent; at != nullptr; at = at->parent) {
        if (domIsHtml(domElementFromNode(at), PrDomTag_Datalist))
            return true;
    }
    return false;
}

// Returns true if the element is an auto-directionality form-associated element
// (html/#auto-directionality-form-associated-elements), whose dirname attribute submits its direction.
bool autoDirectionality(const PrDomElement* element) {
    if (domIsHtml(element, PrDomTag_Textarea))
        return true;
    if (!domIsHtml(element, PrDomTag_Input))
        return false;
    switch (domElementInputType(element)) {
        case PrDomInputType_Hidden:
        case PrDomInputType_Text:
        case PrDomInputType_Search:
        case PrDomInputType_Tel:
        case PrDomInputType_Url:
        case PrDomInputType_Email:
        case PrDomInputType_Password:
        case PrDomInputType_Submit:
        case PrDomInputType_Reset:
        case PrDomInputType_Button:
            return true;
        default:
            return false;
    }
}

// Appends the entries of one control to `list` (html/#constructing-the-form-data-set, the steps for each field).
void appendEntries(DomEntryList* list, PrDomElement* field, const PrDomElement* PR_NULLABLE submitter) {
    if (inDatalist(field) || domElementDisabled(field) || (domIsButton(field) && field != submitter))
        return;
    bool input = domIsHtml(field, PrDomTag_Input);
    PrDomInputType type = input ? domElementInputType(field) : PrDomInputType_Text;
    if (input && (type == PrDomInputType_Checkbox || type == PrDomInputType_Radio) && !domInputChecked(field))
        return;
    PrStringView name = domAttributeValue(field, "name");
    if (input && type == PrDomInputType_Image) {
        // A submitter that no click activated has the coordinate (0, 0).
        PrTextBuffer prefix = {};
        if (name.size != 0) {
            prTextBufferAppend(&prefix, name);
            prTextBufferAppendChar(&prefix, '.');
        }
        uint32_t size = prefix.size;
        prTextBufferAppendChar(&prefix, 'x');
        domEntryListAppend(list, prTextBufferView(&prefix), "0");
        prefix.size = size;
        prTextBufferAppendChar(&prefix, 'y');
        domEntryListAppend(list, prTextBufferView(&prefix), "0");
        prTextBufferReset(&prefix);
        return;
    }
    if (name.size == 0)
        return;
    PrTextBuffer value = {};
    if (domIsHtml(field, PrDomTag_Select)) {
        RawArray<PrDomElement* PR_NONNULL> options;
        domSelectOptions(field, &options);
        for (uint32_t i = 0; i < options.count; ++i) {
            if (!domOptionSelected(options.data[i]) || domOptionDisabled(options.data[i]))
                continue;
            domOptionValue(options.data[i], &value);
            domEntryListAppend(list, name, prTextBufferView(&value));
        }
        options.reset();
    } else if (input && (type == PrDomInputType_Checkbox || type == PrDomInputType_Radio)) {
        PrStringView attribute = domAttributeValue(field, "value");
        domEntryListAppend(list, name, attribute.data != nullptr ? attribute : "on");
    } else if (input && type == PrDomInputType_File) {
        // A File entry waits for the File API.
    } else if (input && type == PrDomInputType_Hidden && prAsciiEqualsFold(name, "_charset_")) {
        domEntryListAppend(list, name, "UTF-8");
    } else if (domIsHtml(field, PrDomTag_Button)) {
        domEntryListAppend(list, name, domAttributeValue(field, "value"));
    } else {
        domControlValue(field, &value);
        domEntryListAppend(list, name, prTextBufferView(&value));
    }
    prTextBufferReset(&value);
    PrStringView dirname = domAttributeValue(field, "dirname");
    if (dirname.size != 0 && autoDirectionality(field))
        domEntryListAppend(list, dirname, domElementRtl(field) ? "rtl" : "ltr");
}

// html/#constructing-the-form-data-set: returns the entry list of the form with `submitter`, which the caller releases,
// or null while the form is already constructing its entry list.
DomEntryList* PR_NULLABLE constructEntryList(PrHtmlFormElement* form, PrHtmlElement* PR_NULLABLE submitter) {
    DomForm* record = domFormFromElement(form);
    if (record->constructingEntryList)
        return nullptr;
    record->constructingEntryList = true;
    // The controls are the form's submittable elements in the form's tree, in tree order.
    RawArray<PrDomNode* PR_NONNULL> controls;
    PrDomNode* root = domNodeRoot(form);
    for (PrDomNode* node = domNodeNext(root, root); node != nullptr; node = domNodeNext(root, node)) {
        PrDomElement* element = domElementFromNode(node);
        if (element != nullptr && domIsSubmittable(element) && domFormOwner(element) == form) {
            domNodeRetain(element);
            controls.push(element);
        }
    }
    DomEntryList* list = domEntryListCreate();
    for (uint32_t i = 0; i < controls.count; ++i)
        appendEntries(list, static_cast<PrDomElement*>(controls.data[i]), submitter);
    domNodeReleaseEach(controls.data, controls.count);
    controls.reset();

    // The event's FormData is the entry list itself, so that its listeners change what the form submits.
    PrDomEventInit init = prDomEventInitDefault();
    init.bubbles = true;
    PrHtmlFormDataEvent* event = prHtmlFormDataEventCreate("formdata", &init, static_cast<PrHtmlFormData*>(list));
    event->trusted = true;
    domNodeRetain(form);
    domEventDispatch(prHtmlFormElementAsEventTarget(form), event);
    domEventRelease(event);
    record = domFormFromElement(form);
    record->constructingEntryList = false;
    domNodeRelease(form);

    DomEntryList* clone = domEntryListCreate();
    for (uint32_t i = 0; i < list->entries.count; ++i)
        domEntryListAppend(clone, prRawStringView(&list->entries.data[i].name),
            prRawStringView(&list->entries.data[i].value));
    domEntryListRelease(list);
    return clone;
}

// Returns the event's data, creating it.
DomControlEventData* controlDataOf(PrDomEvent* event) {
    if (event->control == nullptr)
        event->control = allocZeroed<DomControlEventData>();
    return event->control;
}

template <typename Event>
Event* PR_NULLABLE eventQuery(PrDomEvent* PR_NULLABLE object, PrDomEventKind kind) {
    return object != nullptr && object->kind == kind ? static_cast<Event*>(object) : nullptr;
}

// Returns the source of a ToggleEvent or a CommandEvent, retargeted against its current target.
PrDomElement* PR_NULLABLE retargetedSource(PrDomEvent* event) {
    PrDomEventTarget* source = event->control != nullptr ? event->control->source : nullptr;
    return domElementFromNode(domNodeFromTarget(domRetarget(source, event->currentTarget)));
}

// Returns the number of UTF-16 code units in `text`.
uint32_t unitsOf(PrStringView text) {
    uint32_t units = 0;
    for (size_t i = 0; i < text.size; ++i) {
        uint8_t byte = uint8_t(text.data[i]);
        if ((byte & 0xc0) != 0x80)
            units += byte >= 0xf0 ? 2 : 1;
    }
    return units;
}

// Returns the byte of `text` at `units` UTF-16 code units, or its end. A unit inside a surrogate pair stands at the
// start of its code point.
uint32_t byteFromUnits(PrStringView text, uint32_t units) {
    uint32_t seen = 0;
    for (size_t i = 0; i < text.size; ++i) {
        uint8_t byte = uint8_t(text.data[i]);
        if ((byte & 0xc0) == 0x80)
            continue;
        seen += byte >= 0xf0 ? 2 : 1;
        if (seen > units)
            return uint32_t(i);
    }
    return uint32_t(text.size);
}

void dropSelect(void* data) {
    PrDomElement* element = static_cast<PrDomElement*>(data);
    domControlFromElement(element)->selectQueued = false;
    domNodeUnhold(element);
}

void fireSelect(void* data) {
    PrDomElement* element = static_cast<PrDomElement*>(data);
    domControlFromElement(element)->selectQueued = false;
    PrDomEventInit init = prDomEventInitDefault();
    init.bubbles = true;
    domEventFire(prDomNodeAsEventTarget(prDomElementAsNode(element)), "select", init);
    domNodeUnhold(element);
}

// Queues the select event of `element`, unless one is already queued (html/#textFieldSelection).
void queueSelect(PrDomElement* element, DomControl* control) {
    const DomDocumentHost* host = element->document->host;
    if (control->selectQueued || host == nullptr)
        return;
    control->selectQueued = true;
    domNodeHold(element);
    host->queueTask(host->context, fireSelect, element, dropSelect);
}

PrTextEditSelection selectionOf(const DomControl* control) {
    return prTextEditViewSelections(control->view)[0];
}

uint32_t lowOf(const PrTextEditSelection& s) {
    return prTextEditSelectionStart(s);
}

uint32_t highOf(const PrTextEditSelection& s) {
    return prTextEditSelectionEnd(s);
}

// Sets the selection in bytes of the text, which are clamped as the specification clamps units. Returns true if the
// selection changed.
bool setSelectionBytes(DomControl* control, uint32_t start, uint32_t end, PrDomSelectionDirection direction) {
    uint32_t size = uint32_t(domControlText(control).size);
    end = end < size ? end : size;
    start = start < end ? start : end;
    PrTextEditSelection was = selectionOf(control);
    bool changed = lowOf(was) != start || highOf(was) != end || control->selectionDirection != direction;
    bool backward = direction == PrDomSelectionDirection_Backwards;
    PrTextEditSelection next{.anchor = backward ? end : start, .caret = backward ? start : end};
    prTextEditViewSetSelections(control->view, &next, 1);
    control->selectionDirection = direction;
    return changed;
}

// Fires a beforeinput or input of an edit. Returns true if no listener canceled it.
bool fireInput(PrDomElement* element, PrStringView type, PrStringView data, PrStringView inputType, bool composing,
        bool cancelable) {
    PrDomUiEventInit init = prDomUiEventInitDefault();
    init.event.bubbles = true;
    init.event.cancelable = cancelable;
    init.event.composed = true;
    PrDomInputEvent* event = prDomInputEventCreate(type, &init, data, inputType, composing);
    prDomInputEventAsEvent(event)->trusted = true;
    bool proceed = domEventDispatch(prDomNodeAsEventTarget(prDomElementAsNode(element)), prDomInputEventAsEvent(event));
    prDomEventRelease(prDomInputEventAsEvent(event));
    return proceed;
}

// Applies one replacement as the user's edit, with the events around it. The selection of the view lands after the new
// text.
bool editControl(PrDomElement* element, uint32_t start, uint32_t end, PrStringView text, PrStringView inputType,
        PrTextEditOrigin origin, PrTextEditMerge merge) {
    bool composing = origin == PrTextEditOrigin_Composition;
    PrRawString data = prRawStringCopy(text);
    bool proceed = fireInput(element, "beforeinput", prRawStringView(&data), inputType, composing, !composing);
    // A listener may have changed the control or removed it, so the edit reads the state after the event.
    DomControl* control = domTextControlSync(element);
    uint32_t size = uint32_t(domControlText(control).size);
    bool applied = false;
    if (proceed && start <= size && end <= size) {
        if (control->history == nullptr)
            control->history = prTextEditHistoryCreate(control->text);
        PrTextEditChange change{.start = start, .end = end, .text = prRawStringView(&data)};
        applied = prTextEditViewApply(control->view, &change, 1, origin, merge);
        control->dirtyValue = true;
        control->selectionDirection = PrDomSelectionDirection_None;
    }
    if (applied) {
        domNotifyStatesChanged(element);
        fireInput(element, "input", prRawStringView(&data), inputType, composing, false);
    }
    prRawStringReset(&data);
    return applied;
}

} // namespace

void htmlColorWellValue(const PrDomElement* input, PrStringView text, PrTextBuffer* out) {
    PrCssUsedColor color{};
    if (text.data == nullptr || prCssParseColor(text, input, &color) != PrStatus_Ok)
        color = PrCssUsedColor{.space = PrColorSpace_Srgb, .legacy = true, .alpha = 1};
    bool alpha = domHasAttribute(input, "alpha");
    if (!alpha)
        color.alpha = 1;
    // html/#attr-input-colorspace: Limited sRGB is the missing value default and the invalid value default.
    bool p3 = prAsciiEqualsFold(domAttributeValue(input, "colorspace"), "display-p3");
    PrColorSpace space = p3 ? PrColorSpace_DisplayP3 : PrColorSpace_Srgb;
    double converted[3];
    for (uint32_t i = 0; i < 3; ++i)
        converted[i] = (color.missing & (1u << i)) != 0 ? 0 : color.components[i];
    if (color.space != space)
        prColorConvert(color.space, space, converted, converted);
    PrCssUsedColor result{.space = space, .legacy = false, .alpha = color.alpha};
    for (uint32_t i = 0; i < 3; ++i)
        result.components[i] = converted[i];
    PrCssColorSerialization serialization = PrCssColorSerialization_Css;
    if (!p3) {
        // Each component rounds toward positive infinity to a whole number of 255ths.
        for (uint32_t i = 0; i < 3; ++i) {
            double steps = prF64Floor(converted[i] * 255 + 0.5);
            result.components[i] = (steps < 0 ? 0 : steps > 255 ? 255 : steps) / 255;
        }
        if (!alpha) {
            result.legacy = true;
            serialization = PrCssColorSerialization_HtmlCompatible;
        }
    }
    PrRawString serialized = prCssUsedColorSerialize(&result, serialization);
    prTextBufferAppend(out, prRawStringView(&serialized));
    prRawStringReset(&serialized);
}

bool htmlTextControlIsEditable(const PrDomElement* element) {
    if (!prHtmlTextControlSelectionApplies(element))
        return false;
    return domAttributeValue(element, "disabled").data == nullptr && domAttributeValue(element, "readonly").data == nullptr;
}

bool htmlTextControlInsert(PrDomElement* element, PrStringView text, PrStringView inputType) {
    const DomControl* control = domTextControlSync(element);
    PrTextEditSelection s = selectionOf(control);
    PrTextEditMerge merge = text == "\n" ? PrTextEditMerge_None : PrTextEditMerge_Typing;
    return editControl(element, lowOf(s), highOf(s), text, inputType, PrTextEditOrigin_Local, merge);
}

bool htmlTextControlDelete(PrDomElement* element, uint32_t at, PrStringView inputType) {
    const DomControl* control = domTextControlSync(element);
    PrTextEditSelection s = selectionOf(control);
    if (s.anchor != s.caret)
        return editControl(element, lowOf(s), highOf(s), "", inputType, PrTextEditOrigin_Local, PrTextEditMerge_None);
    if (at == s.caret)
        return false;
    uint32_t low = at < s.caret ? at : s.caret;
    uint32_t high = at < s.caret ? s.caret : at;
    return editControl(element, low, high, "", inputType, PrTextEditOrigin_Local, PrTextEditMerge_Deleting);
}

void htmlTextControlCompose(PrDomElement* element, PrStringView text) {
    DomControl* control = domTextControlSync(element);
    PrTextEditSelection s = selectionOf(control);
    uint32_t start = control->markEnd > control->markStart ? control->markStart : lowOf(s);
    uint32_t end = control->markEnd > control->markStart ? control->markEnd : highOf(s);
    if (start == end && text.size == 0)
        return;
    if (editControl(element, start, end, text, "insertCompositionText", PrTextEditOrigin_Composition,
            PrTextEditMerge_None)) {
        control->markStart = start;
        control->markEnd = start + uint32_t(text.size);
    }
}

bool htmlTextControlUndo(PrDomElement* element, bool redo) {
    DomControl* control = domTextControlSync(element);
    PrStringView inputType = "historyUndo";
    if (redo)
        inputType = "historyRedo";
    if (control->history == nullptr)
        return false;
    if (redo ? !prTextEditHistoryCanRedo(control->history) : !prTextEditHistoryCanUndo(control->history))
        return false;
    if (!fireInput(element, "beforeinput", PrStringView{}, inputType, false, true))
        return false;
    bool applied = false;
    if (redo)
        applied = prTextEditHistoryRedo(control->history, control->view);
    else
        applied = prTextEditHistoryUndo(control->history, control->view);
    if (applied) {
        domNotifyStatesChanged(element);
        fireInput(element, "input", PrStringView{}, inputType, false, false);
    }
    return applied;
}

void htmlTextControlSetSelection(PrDomElement* element, const PrTextEditSelection& selection) {
    DomControl* control = domTextControlSync(element);
    PrTextEditSelection s = selectionOf(control);
    prTextEditViewSetSelections(control->view, &selection, 1);
    PrTextEditSelection next = selectionOf(control);
    bool backward = next.caret < next.anchor;
    control->selectionDirection = backward ? PrDomSelectionDirection_Backwards : PrDomSelectionDirection_Forwards;
    if (next.anchor != next.caret && (next.anchor != s.anchor || next.caret != s.caret))
        queueSelect(element, control);
}

} // namespace pr_impl

using namespace pr_impl;

// ---------------------------------------------------------------------------------------------------------------------
// Form-associated elements

PrHtmlFormElement* PR_NULLABLE prHtmlElementForm(PrHtmlElement* element) {
    if (domIsHtml(element, PrDomTag_Legend)) {
        PrDomElement* parent = domElementFromNode(element->parent);
        return domIsHtml(parent, PrDomTag_Fieldset) ? static_cast<PrHtmlFormElement*>(domFormOwner(parent)) : nullptr;
    }
    return static_cast<PrHtmlFormElement*>(domFormOwner(element));
}

PrDomNodeList* PR_NULLABLE prHtmlElementLabels(PrHtmlElement* element) {
    if (!domIsLabelable(element))
        return nullptr;
    return domNodeListCreateFiltered(element, DomNodeListFilter_Labels, PrStringView{});
}

PrRawString prHtmlElementFormActionUrl(const PrHtmlElement* element, PrStringView name) {
    PrStringView action = domAttributeValue(element, name);
    if (action.size == 0)
        return prUrlSerialize(element->document->url, PrUrlSerialization_Whole);
    PrUrl* url = nullptr;
    PrRawString out;
    if (domDocumentParseUrl(element->document, action, &url) == PrStatus_Ok)
        out = prUrlSerialize(url, PrUrlSerialization_Whole);
    else
        out = prRawStringCopy(action);
    prUrlDestroy(url);
    return out;
}

// ---------------------------------------------------------------------------------------------------------------------
// HTMLFormElement

PrHtmlFormElement* PR_NULLABLE prHtmlFormElementQuery(PrDomEventTarget* PR_NULLABLE object) {
    return htmlElementQuery<PrHtmlFormElement>(object, PrDomTag_Form);
}

PrDomHtmlCollection* prHtmlFormElementElements(PrHtmlFormElement* element) {
    return domHtmlCollectionCreate(element, DomCollectionFilter_FormControls, PrStringView{});
}

PrDomNodeList* prHtmlFormElementRadioNodeList(PrHtmlFormElement* element, PrStringView name, bool images) {
    return domNodeListCreateFiltered(element, images ? DomNodeListFilter_FormImages : DomNodeListFilter_Radio, name);
}

uint32_t prHtmlFormElementNamedItem(PrHtmlFormElement* element, PrStringView name, PrDomElement* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    if (name.size == 0)
        return 0;
    uint32_t count = 0;
    for (uint32_t pass = 0; pass < 2; ++pass) {
        PrDomNodeList* list = prHtmlFormElementRadioNodeList(element, name, pass == 1);
        count = domNodeListLength(list);
        if (count == 1)
            *out = domElementFromNode(domNodeListItem(list, 0));
        domNodeListDestroy(list);
        if (count != 0)
            break;
    }
    if (count == 0) {
        *out = domFormPastName(element, name);
        return *out != nullptr ? 1 : 0;
    }
    if (count == 1)
        domFormRememberName(element, name, *out);
    return count;
}

uint32_t prHtmlFormElementNames(PrHtmlFormElement* formElement, PrRawString* PR_NULLABLE items, uint32_t capacity) {
    // html/#the-form-element:supported-property-names: the names of the form's controls, then of its img elements, then
    // of its past names map, each element's ID before its name, in the tree order of their elements.
    struct Source {
        PrStringView name;
        PrDomElement* element;
        uint32_t rank;              // 0 for an ID, 1 for a name, 2 for a past name
        uint32_t order;             // the place of the source in the list, among those of one element and rank
    };
    RawArray<Source> sources;
    PrDomNode* root = domNodeRoot(formElement);
    for (PrDomNode* node = domNodeNext(root, root); node != nullptr; node = domNodeNext(root, node)) {
        PrDomElement* element = domElementFromNode(node);
        if (element == nullptr || domFormOwner(element) != formElement)
            continue;
        bool listed = domIsListed(element) &&
            !(domIsHtml(element, PrDomTag_Input) && domElementInputType(element) == PrDomInputType_Image);
        if (!listed && !domIsHtml(element, PrDomTag_Img))
            continue;
        sources.push(Source{domElementId(element), element, 0, sources.count});
        sources.push(Source{domAttributeValue(element, "name"), element, 1, sources.count});
    }
    if (const DomForm* record = formElement->state != nullptr ? formElement->state->form : nullptr) {
        for (uint32_t i = 0; i < record->pastNames.count; ++i)
            sources.push(Source{
                prRawStringView(&record->pastNames.data[i].name), record->pastNames.data[i].element, 2,
                sources.count,
            });
    }
    sortItems(sources.data, sources.count, [](const Source& a, const Source& b) {
        if (a.element != b.element)
            return domNodePrecedes(a.element, b.element);
        return a.rank != b.rank ? a.rank < b.rank : a.order < b.order;
    });
    uint32_t count = 0;
    for (uint32_t i = 0; i < sources.count; ++i) {
        PrStringView name = sources.data[i].name;
        bool seen = name.size == 0;
        for (uint32_t j = 0; j < i && !seen; ++j)
            seen = sources.data[j].name == name;
        if (seen)
            continue;
        if (items != nullptr && count < capacity)
            items[count] = prRawStringCopy(name);
        ++count;
    }
    sources.reset();
    return count;
}

// ---------------------------------------------------------------------------------------------------------------------
// HTMLInputElement

PrHtmlInputElement* PR_NULLABLE prHtmlInputElementQuery(PrDomEventTarget* PR_NULLABLE object) {
    return htmlElementQuery<PrHtmlInputElement>(object, PrDomTag_Input);
}

PrDomInputType prHtmlInputElementType(const PrHtmlInputElement* element) {
    return domElementInputType(element);
}

PrRawString prHtmlInputElementValue(const PrHtmlInputElement* element) {
    PrTextBuffer value = {};
    domControlValue(element, &value);
    PrRawString out = stringOf(&value);
    prTextBufferReset(&value);
    return out;
}

PrStatus prHtmlInputElementSetValue(PrHtmlInputElement* element, PrStringView value) {
    return domControlSetValue(element, value);
}

bool prHtmlInputElementChecked(const PrHtmlInputElement* element) {
    return domInputChecked(element);
}

void prHtmlInputElementSetChecked(PrHtmlInputElement* element, bool value) {
    domInputSetChecked(element, value, true);
}

bool prHtmlInputElementIndeterminate(const PrHtmlInputElement* element) {
    return domInputIndeterminate(element);
}

void prHtmlInputElementSetIndeterminate(PrHtmlInputElement* element, bool value) {
    domInputSetIndeterminate(element, value);
}

double prHtmlInputElementValueAsNumber(const PrHtmlInputElement* element) {
    if ((domInputApplies(domElementInputType(element)) & DomInputApplyFlag_ValueAsNumber) == 0)
        return kPrF64Nan;
    PrTextBuffer text = {};
    domControlValue(element, &text);
    double value = kPrF64Nan;
    if (!domInputStringToNumber(element, prTextBufferView(&text), &value))
        value = kPrF64Nan;
    prTextBufferReset(&text);
    return value;
}

PrStatus prHtmlInputElementSetValueAsNumber(PrHtmlInputElement* element, double value) {
    if (!prF64IsFinite(value) && !prF64IsNan(value))
        return PrStatus_WrongType;
    if ((domInputApplies(domElementInputType(element)) & DomInputApplyFlag_ValueAsNumber) == 0)
        return PrStatus_InvalidState;
    if (prF64IsNan(value))
        return domControlSetValue(element, "");
    PrTextBuffer text = {};
    domInputNumberToString(element, value, &text);
    PrStatus status = domControlSetValue(element, prTextBufferView(&text));
    prTextBufferReset(&text);
    return status;
}

bool prHtmlInputElementValueAsDate(const PrHtmlInputElement* element, double* out) {
    if ((domInputApplies(domElementInputType(element)) & DomInputApplyFlag_ValueAsDate) == 0)
        return false;
    PrTextBuffer text = {};
    domControlValue(element, &text);
    bool converted = domInputStringToDate(element, prTextBufferView(&text), out);
    prTextBufferReset(&text);
    return converted;
}

PrStatus prHtmlInputElementSetValueAsDate(PrHtmlInputElement* element, double time) {
    if ((domInputApplies(domElementInputType(element)) & DomInputApplyFlag_ValueAsDate) == 0)
        return PrStatus_InvalidState;
    if (!prF64IsFinite(time))
        return domControlSetValue(element, "");
    PrTextBuffer text = {};
    domInputDateToString(element, time, &text);
    PrStatus status = domControlSetValue(element, prTextBufferView(&text));
    prTextBufferReset(&text);
    return status;
}

PrStatus prHtmlInputElementStepUp(PrHtmlInputElement* element, int32_t n) {
    return domInputStep(element, n, true);
}

PrStatus prHtmlInputElementStepDown(PrHtmlInputElement* element, int32_t n) {
    return domInputStep(element, n, false);
}

PrDomElement* PR_NULLABLE prHtmlInputElementList(PrHtmlInputElement* element) {
    // html/#attr-input-list: the attribute does not apply to these types.
    switch (domElementInputType(element)) {
        case PrDomInputType_Hidden:
        case PrDomInputType_Password:
        case PrDomInputType_Checkbox:
        case PrDomInputType_Radio:
        case PrDomInputType_File:
        case PrDomInputType_Submit:
        case PrDomInputType_Image:
        case PrDomInputType_Reset:
        case PrDomInputType_Button:
            return nullptr;
        default:
            break;
    }
    PrStringView id = domAttributeValue(element, "list");
    if (id.data == nullptr)
        return nullptr;
    PrDomElement* found = domElementById(domNodeRoot(element), id);
    return domIsHtml(found, PrDomTag_Datalist) ? found : nullptr;
}

uint32_t prHtmlInputElementWidth(const PrHtmlInputElement* element) {
    return prHtmlInputElementType(element) == PrDomInputType_Image ? htmlImageDimension(element, true) : 0;
}

uint32_t prHtmlInputElementHeight(const PrHtmlInputElement* element) {
    return prHtmlInputElementType(element) == PrDomInputType_Image ? htmlImageDimension(element, false) : 0;
}

void prHtmlInputElementSetWidth(PrHtmlInputElement* element, uint32_t value) {
    htmlSetReflectedUnsigned(element, "width", value);
}

void prHtmlInputElementSetHeight(PrHtmlInputElement* element, uint32_t value) {
    htmlSetReflectedUnsigned(element, "height", value);
}

// ---------------------------------------------------------------------------------------------------------------------
// HTMLButtonElement

PrHtmlButtonElement* PR_NULLABLE prHtmlButtonElementQuery(PrDomEventTarget* PR_NULLABLE object) {
    return htmlElementQuery<PrHtmlButtonElement>(object, PrDomTag_Button);
}

PrStringView prHtmlButtonElementType(const PrHtmlButtonElement* element) {
    if (domIsSubmitButton(element))
        return "submit";
    DomButtonType type = domButtonType(domAttributeValue(element, "type"));
    return type == DomButtonType_Auto ? PrStringView{"button"} : domButtonTypeKeyword(type);
}

// ---------------------------------------------------------------------------------------------------------------------
// HTMLTextAreaElement

PrHtmlTextAreaElement* PR_NULLABLE prHtmlTextAreaElementQuery(PrDomEventTarget* PR_NULLABLE object) {
    return htmlElementQuery<PrHtmlTextAreaElement>(object, PrDomTag_Textarea);
}

PrRawString prHtmlTextAreaElementValue(const PrHtmlTextAreaElement* element) {
    PrTextBuffer value = {};
    domControlValue(element, &value);
    PrRawString out = stringOf(&value);
    prTextBufferReset(&value);
    return out;
}

void prHtmlTextAreaElementSetValue(PrHtmlTextAreaElement* element, PrStringView value) {
    domControlSetValue(element, value);
}

PrRawString prHtmlTextAreaElementDefaultValue(const PrHtmlTextAreaElement* element) {
    PrTextBuffer value = {};
    domTextareaDefaultValue(element, &value);
    PrRawString out = stringOf(&value);
    prTextBufferReset(&value);
    return out;
}

void prHtmlTextAreaElementSetDefaultValue(PrHtmlTextAreaElement* element, PrStringView value) {
    domStringReplaceAll(element, value);
}

uint32_t prHtmlTextAreaElementTextLength(const PrHtmlTextAreaElement* element) {
    PrTextBuffer value = {};
    domControlValue(element, &value);
    // A code point counts one UTF-16 code unit, and one past the Basic Multilingual Plane, which takes four bytes in
    // UTF-8, counts two.
    uint32_t length = 0;
    for (uint32_t i = 0; i < value.size; ++i) {
        uint8_t byte = uint8_t(value.data[i]);
        if ((byte & 0xc0) != 0x80)
            length += byte >= 0xf0 ? 2 : 1;
    }
    prTextBufferReset(&value);
    return length;
}

// ---------------------------------------------------------------------------------------------------------------------
// Text control selection

bool prHtmlTextControlSelectionApplies(const PrDomElement* element) {
    if (domIsHtml(element, PrDomTag_Textarea))
        return true;
    if (!domIsHtml(element, PrDomTag_Input))
        return false;
    PrDomInputType type = domInputType(domAttributeValue(element, "type"));
    return type == PrDomInputType_Text || type == PrDomInputType_Search || type == PrDomInputType_Url ||
        type == PrDomInputType_Tel || type == PrDomInputType_Password;
}

uint32_t prHtmlTextControlSelectionStart(PrDomElement* element) {
    const DomControl* control = domTextControlSync(element);
    PrStringView text = domControlText(control);
    return unitsOf(PrStringView{text.data, lowOf(selectionOf(control))});
}

uint32_t prHtmlTextControlSelectionEnd(PrDomElement* element) {
    const DomControl* control = domTextControlSync(element);
    PrStringView text = domControlText(control);
    return unitsOf(PrStringView{text.data, highOf(selectionOf(control))});
}

PrDomSelectionDirection prHtmlTextControlSelectionDirection(PrDomElement* element) {
    return domTextControlSync(element)->selectionDirection;
}

void prHtmlTextControlSetSelectionRange(PrDomElement* element, uint32_t start, uint32_t end,
        PrDomSelectionDirection direction) {
    DomControl* control = domTextControlSync(element);
    PrStringView text = domControlText(control);
    if (setSelectionBytes(control, byteFromUnits(text, start), byteFromUnits(text, end), direction))
        queueSelect(element, control);
}

void prHtmlTextControlSelect(PrDomElement* element) {
    if (!prHtmlTextControlSelectionApplies(element))
        return;
    DomControl* control = domTextControlSync(element);
    if (setSelectionBytes(control, 0, uint32_t(domControlText(control).size), PrDomSelectionDirection_None))
        queueSelect(element, control);
}

PrStatus prHtmlTextControlSetRangeText(PrDomElement* element, PrStringView replacement, uint32_t start, uint32_t end,
        PrHtmlSelectionMode mode) {
    if (start > end)
        return PrStatus_OutOfRange;
    DomControl* control = domTextControlSync(element);
    PrStringView text = domControlText(control);
    uint32_t low = byteFromUnits(text, start);
    uint32_t high = byteFromUnits(text, end);
    PrTextEditSelection was = selectionOf(control);
    uint32_t selectionStart = lowOf(was);
    uint32_t selectionEnd = highOf(was);

    // Replacement text is the value's, so an input's sanitization applies by the same rule as its setter.
    PrTextEditChange change{.start = low, .end = high, .text = replacement};
    prTextEditBufferApply(control->text, &change, 1, PrTextEditOrigin_Program);
    control->dirtyValue = true;
    domNotifyStatesChanged(element);

    int64_t delta = int64_t(replacement.size) - int64_t(high - low);
    uint32_t newEnd = low + uint32_t(replacement.size);
    switch (mode) {
        case PrHtmlSelectionMode_Select:
            selectionStart = low;
            selectionEnd = newEnd;
            break;
        case PrHtmlSelectionMode_Start:
            selectionStart = selectionEnd = low;
            break;
        case PrHtmlSelectionMode_End:
            selectionStart = selectionEnd = newEnd;
            break;
        default:
            if (selectionStart > high)
                selectionStart = uint32_t(int64_t(selectionStart) + delta);
            else if (selectionStart > low)
                selectionStart = low;
            if (selectionEnd > high)
                selectionEnd = uint32_t(int64_t(selectionEnd) + delta);
            else if (selectionEnd > low)
                selectionEnd = newEnd;
            break;
    }
    // The edit already moved the selection, so what changed is measured from before it.
    setSelectionBytes(control, selectionStart, selectionEnd, control->selectionDirection);
    PrTextEditSelection now = selectionOf(control);
    if (lowOf(now) != lowOf(was) || highOf(now) != highOf(was))
        queueSelect(element, control);
    return PrStatus_Ok;
}

// ---------------------------------------------------------------------------------------------------------------------
// HTMLFieldSetElement, HTMLLegendElement and HTMLLabelElement

PrHtmlFieldSetElement* PR_NULLABLE prHtmlFieldSetElementQuery(PrDomEventTarget* PR_NULLABLE object) {
    return htmlElementQuery<PrHtmlFieldSetElement>(object, PrDomTag_Fieldset);
}

PrHtmlLegendElement* PR_NULLABLE prHtmlLegendElementQuery(PrDomEventTarget* PR_NULLABLE object) {
    return htmlElementQuery<PrHtmlLegendElement>(object, PrDomTag_Legend);
}

PrHtmlLabelElement* PR_NULLABLE prHtmlLabelElementQuery(PrDomEventTarget* PR_NULLABLE object) {
    return htmlElementQuery<PrHtmlLabelElement>(object, PrDomTag_Label);
}

PrDomHtmlCollection* prHtmlFieldSetElementElements(PrHtmlFieldSetElement* element) {
    return domHtmlCollectionCreate(element, DomCollectionFilter_FieldSetElements, PrStringView{});
}

PrDomElement* PR_NULLABLE prHtmlLabelElementControl(PrHtmlLabelElement* element) {
    return const_cast<PrDomElement*>(domLabeledControl(element));
}

// ---------------------------------------------------------------------------------------------------------------------
// HTMLOutputElement

PrHtmlOutputElement* PR_NULLABLE prHtmlOutputElementQuery(PrDomEventTarget* PR_NULLABLE object) {
    return htmlElementQuery<PrHtmlOutputElement>(object, PrDomTag_Output);
}

PrRawString prHtmlOutputElementValue(const PrHtmlOutputElement* element) {
    return domNodeTextContent(element);
}

void prHtmlOutputElementSetValue(PrHtmlOutputElement* element, PrStringView value) {
    DomControl* control = domControlFromElement(element);
    if (prRawStringIsNull(&control->defaultOverride))
        control->defaultOverride = domNodeTextContent(element);
    domStringReplaceAll(element, value);
}

PrRawString prHtmlOutputElementDefaultValue(const PrHtmlOutputElement* element) {
    const DomControl* control = domControl(element);
    if (control == nullptr || prRawStringIsNull(&control->defaultOverride))
        return domNodeTextContent(element);
    return prRawStringClone(&control->defaultOverride);
}

void prHtmlOutputElementSetDefaultValue(PrHtmlOutputElement* element, PrStringView value) {
    DomControl* control = domControlFromElement(element);
    if (prRawStringIsNull(&control->defaultOverride))
        domStringReplaceAll(element, value);
    else
        prRawStringReplace(&control->defaultOverride, value);
}

// ---------------------------------------------------------------------------------------------------------------------
// HTMLProgressElement and HTMLMeterElement

PrHtmlProgressElement* PR_NULLABLE prHtmlProgressElementQuery(PrDomEventTarget* PR_NULLABLE object) {
    return htmlElementQuery<PrHtmlProgressElement>(object, PrDomTag_Progress);
}

PrHtmlMeterElement* PR_NULLABLE prHtmlMeterElementQuery(PrDomEventTarget* PR_NULLABLE object) {
    return htmlElementQuery<PrHtmlMeterElement>(object, PrDomTag_Meter);
}

double prHtmlProgressElementValue(const PrHtmlProgressElement* element) {
    double value = parsedOr(element, "value", 0);
    double max = prHtmlProgressElementMax(element);
    return value < 0 ? 0 : value > max ? max : value;
}

double prHtmlProgressElementMax(const PrHtmlProgressElement* element) {
    double max = parsedOr(element, "max", 1);
    return max > 0 ? max : 1;
}

double prHtmlProgressElementPosition(const PrHtmlProgressElement* element) {
    if (domAttributeValue(element, "value").data == nullptr)
        return -1;
    return prHtmlProgressElementValue(element) / prHtmlProgressElementMax(element);
}

PrHtmlMeterValues prHtmlMeterElementValues(const PrHtmlMeterElement* element) {
    PrHtmlMeterValues values{};
    values.min = parsedOr(element, "min", 0);
    values.max = parsedOr(element, "max", 1);
    if (values.max < values.min)
        values.max = values.min;
    values.value = parsedOr(element, "value", 0);
    values.value = values.value < values.min ? values.min : values.value > values.max ? values.max : values.value;
    values.low = parsedOr(element, "low", values.min);
    values.low = values.low < values.min ? values.min : values.low > values.max ? values.max : values.low;
    values.high = parsedOr(element, "high", values.max);
    values.high = values.high < values.low ? values.low : values.high > values.max ? values.max : values.high;
    values.optimum = parsedOr(element, "optimum", values.min + (values.max - values.min) / 2);
    values.optimum =
        values.optimum < values.min   ? values.min : values.optimum > values.max ? values.max : values.optimum;
    return values;
}

PrStatus prHtmlElementSetReflectedDouble(PrHtmlElement* element, PrStringView name, double value) {
    if (!prF64IsFinite(value))
        return PrStatus_WrongType;
    // html/#dom-progress-max is limited to only positive numbers.
    if (domIsHtml(element, PrDomTag_Progress) && name == "max" && value <= 0)
        return PrStatus_Ok;
    PrTextBuffer text = {};
    domAppendFloat(&text, value);
    domSetAttribute(element, PrDomNamespace_None, name, prTextBufferView(&text));
    prTextBufferReset(&text);
    return PrStatus_Ok;
}

// ---------------------------------------------------------------------------------------------------------------------
// HTMLDetailsElement

PrHtmlDetailsElement* PR_NULLABLE prHtmlDetailsElementQuery(PrDomEventTarget* PR_NULLABLE object) {
    return htmlElementQuery<PrHtmlDetailsElement>(object, PrDomTag_Details);
}

// ---------------------------------------------------------------------------------------------------------------------
// FormData

PrStatus prHtmlFormDataCreate(PrHtmlFormElement* PR_NULLABLE form, PrHtmlElement* PR_NULLABLE submitter,
        PrHtmlFormData* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    if (form == nullptr) {
        *out = static_cast<PrHtmlFormData*>(domEntryListCreate());
        return PrStatus_Ok;
    }
    if (submitter != nullptr && !domIsSubmitButton(submitter))
        return PrStatus_WrongType;
    if (submitter != nullptr && domFormOwner(submitter) != form)
        return PrStatus_NotFound;
    DomEntryList* list = constructEntryList(form, submitter);
    if (list == nullptr)
        return PrStatus_InvalidState;
    *out = static_cast<PrHtmlFormData*>(list);
    return PrStatus_Ok;
}

void prHtmlFormDataRetain(PrHtmlFormData* formData) {
    domEntryListRetain(formData);
}

void prHtmlFormDataRelease(PrHtmlFormData* PR_NULLABLE formData) {
    domEntryListRelease(formData);
}

void prHtmlFormDataAppend(PrHtmlFormData* formData, PrStringView name, PrStringView value) {
    domEntryListAppend(formData, name, value);
}

void prHtmlFormDataDelete(PrHtmlFormData* formData, PrStringView name) {
    for (uint32_t i = formData->entries.count; i-- != 0;) {
        DomEntry& entry = formData->entries.data[i];
        if (prRawStringView(&entry.name) == name) {
            prRawStringReset(&entry.name);
            prRawStringReset(&entry.value);
            formData->entries.erase(i);
        }
    }
}

PrStringView prHtmlFormDataGet(const PrHtmlFormData* formData, PrStringView name) {
    for (uint32_t i = 0; i < formData->entries.count; ++i) {
        if (prRawStringView(&formData->entries.data[i].name) == name)
            return prRawStringView(&formData->entries.data[i].value);
    }
    return {};
}

uint32_t prHtmlFormDataGetAll(const PrHtmlFormData* formData, PrStringView name, PrStringView* PR_NULLABLE items,
        uint32_t capacity) {
    uint32_t count = 0;
    for (uint32_t i = 0; i < formData->entries.count; ++i) {
        if (prRawStringView(&formData->entries.data[i].name) != name)
            continue;
        if (items != nullptr && count < capacity)
            items[count] = prRawStringView(&formData->entries.data[i].value);
        ++count;
    }
    return count;
}

bool prHtmlFormDataHas(const PrHtmlFormData* formData, PrStringView name) {
    for (uint32_t i = 0; i < formData->entries.count; ++i) {
        if (prRawStringView(&formData->entries.data[i].name) == name)
            return true;
    }
    return false;
}

void prHtmlFormDataSet(PrHtmlFormData* formData, PrStringView name, PrStringView value) {
    uint32_t first = formData->entries.count;
    for (uint32_t i = formData->entries.count; i-- != 0;) {
        DomEntry& entry = formData->entries.data[i];
        if (prRawStringView(&entry.name) != name)
            continue;
        if (first != formData->entries.count) {
            prRawStringReset(&formData->entries.data[first].name);
            prRawStringReset(&formData->entries.data[first].value);
            formData->entries.erase(first);
        }
        first = i;
    }
    if (first == formData->entries.count)
        domEntryListAppend(formData, name, value);
    else
        prRawStringReplace(&formData->entries.data[first].value, value);
}

uint32_t prHtmlFormDataCount(const PrHtmlFormData* formData) {
    return formData->entries.count;
}

bool prHtmlFormDataEntry(const PrHtmlFormData* formData, uint32_t index, PrStringView* name, PrStringView* value) {
    if (index >= formData->entries.count)
        return false;
    *name = prRawStringView(&formData->entries.data[index].name);
    *value = prRawStringView(&formData->entries.data[index].value);
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// SubmitEvent, FormDataEvent, ToggleEvent and CommandEvent

PrHtmlSubmitEvent* PR_NULLABLE prHtmlSubmitEventQuery(PrDomEvent* PR_NULLABLE object) {
    return eventQuery<PrHtmlSubmitEvent>(object, PrDomEventKind_Submit);
}

PrHtmlFormDataEvent* PR_NULLABLE prHtmlFormDataEventQuery(PrDomEvent* PR_NULLABLE object) {
    return eventQuery<PrHtmlFormDataEvent>(object, PrDomEventKind_FormData);
}

PrHtmlToggleEvent* PR_NULLABLE prHtmlToggleEventQuery(PrDomEvent* PR_NULLABLE object) {
    return eventQuery<PrHtmlToggleEvent>(object, PrDomEventKind_Toggle);
}

PrHtmlCommandEvent* PR_NULLABLE prHtmlCommandEventQuery(PrDomEvent* PR_NULLABLE object) {
    return eventQuery<PrHtmlCommandEvent>(object, PrDomEventKind_Command);
}

PrHtmlSubmitEvent* prHtmlSubmitEventCreate(PrStringView type, const PrDomEventInit* PR_NULLABLE init,
        PrHtmlElement* PR_NULLABLE submitter) {
    PrHtmlSubmitEvent* event = domEventCreate<PrHtmlSubmitEvent>(
        type, init != nullptr ? *init : prDomEventInitDefault(), PrDomEventKind_Submit);
    if (submitter != nullptr)
        domEventTargetAssign(&controlDataOf(event)->source, prHtmlElementAsEventTarget(submitter));
    return event;
}

PrHtmlFormDataEvent* prHtmlFormDataEventCreate(PrStringView type, const PrDomEventInit* PR_NULLABLE init,
        PrHtmlFormData* formData) {
    PrHtmlFormDataEvent* event = domEventCreate<PrHtmlFormDataEvent>(
        type, init != nullptr ? *init : prDomEventInitDefault(), PrDomEventKind_FormData);
    domEntryListRetain(formData);
    controlDataOf(event)->entries = formData;
    return event;
}

PrHtmlToggleEvent* prHtmlToggleEventCreate(PrStringView type, const PrDomEventInit* PR_NULLABLE init,
        PrStringView oldState, PrStringView newState, PrDomElement* PR_NULLABLE source) {
    PrHtmlToggleEvent* event = domEventCreate<PrHtmlToggleEvent>(
        type, init != nullptr ? *init : prDomEventInitDefault(), PrDomEventKind_Toggle);
    DomControlEventData* data = controlDataOf(event);
    data->oldState = prRawStringCopy(oldState);
    data->newState = prRawStringCopy(newState);
    if (source != nullptr)
        domEventTargetAssign(&data->source, prDomElementAsEventTarget(source));
    return event;
}

PrHtmlCommandEvent* prHtmlCommandEventCreate(PrStringView type, const PrDomEventInit* PR_NULLABLE init,
        PrStringView command, PrDomElement* PR_NULLABLE source) {
    PrHtmlCommandEvent* event = domEventCreate<PrHtmlCommandEvent>(
        type, init != nullptr ? *init : prDomEventInitDefault(), PrDomEventKind_Command);
    DomControlEventData* data = controlDataOf(event);
    data->command = prRawStringCopy(command);
    if (source != nullptr)
        domEventTargetAssign(&data->source, prDomElementAsEventTarget(source));
    return event;
}

PrHtmlElement* PR_NULLABLE prHtmlSubmitEventSubmitter(PrHtmlSubmitEvent* event) {
    PrDomEventTarget* source = event->control != nullptr ? event->control->source : nullptr;
    return static_cast<PrHtmlElement*>(domElementFromNode(domNodeFromTarget(source)));
}

PrHtmlFormData* prHtmlFormDataEventFormData(PrHtmlFormDataEvent* event) {
    return static_cast<PrHtmlFormData*>(event->control->entries);
}

PrStringView prHtmlToggleEventOldState(const PrHtmlToggleEvent* event) {
    return prRawStringView(&event->control->oldState);
}

PrStringView prHtmlToggleEventNewState(const PrHtmlToggleEvent* event) {
    return prRawStringView(&event->control->newState);
}

PrStringView prHtmlCommandEventCommand(const PrHtmlCommandEvent* event) {
    return prRawStringView(&event->control->command);
}

PrDomElement* PR_NULLABLE prHtmlToggleEventSource(PrHtmlToggleEvent* event) {
    return retargetedSource(event);
}

PrDomElement* PR_NULLABLE prHtmlCommandEventSource(PrHtmlCommandEvent* event) {
    return retargetedSource(event);
}

PrHtmlSelectElement* PR_NULLABLE prHtmlSelectElementQuery(PrDomEventTarget* PR_NULLABLE object) {
    return htmlElementQuery<PrHtmlSelectElement>(object, PrDomTag_Select);
}

PrStringView prHtmlSelectElementType(const PrHtmlSelectElement* select) {
    return domHasAttribute(select, "multiple") ? "select-multiple" : "select-one";
}

PrDomHtmlCollection* prHtmlSelectElementOptions(PrHtmlSelectElement* select) {
    return domHtmlCollectionCreate(select, DomCollectionFilter_SelectOptions, PrStringView{});
}

PrDomHtmlCollection* prHtmlSelectElementSelectedOptions(PrHtmlSelectElement* select) {
    return domHtmlCollectionCreate(select, DomCollectionFilter_SelectedOptions, PrStringView{});
}

uint32_t prHtmlSelectElementLength(PrHtmlSelectElement* select) {
    RawArray<PrDomElement* PR_NONNULL> options;
    domSelectOptions(select, &options);
    uint32_t length = options.count;
    options.reset();
    return length;
}

void prHtmlSelectElementSetLength(PrHtmlSelectElement* select, uint32_t length) {
    if (length > 100000)
        return;
    RawArray<PrDomElement* PR_NONNULL> options;
    domSelectOptions(select, &options);
    for (uint32_t i = options.count; i < length; ++i)
        domInsert(select, domElementCreate(select->document, PrDomNamespace_Html, PrDomTag_Option, "option"), nullptr);
    for (uint32_t i = options.count; i > length; --i)
        domRemoveFromParent(options.data[i - 1]);
    options.reset();
}

PrHtmlOptionElement* PR_NULLABLE prHtmlSelectElementItem(PrHtmlSelectElement* select, uint32_t index) {
    RawArray<PrDomElement* PR_NONNULL> options;
    domSelectOptions(select, &options);
    PrDomElement* option = index < options.count ? options.data[index] : nullptr;
    options.reset();
    return static_cast<PrHtmlOptionElement*>(option);
}

PrStatus prHtmlSelectElementSetItem(PrHtmlSelectElement* select, uint32_t index, PrHtmlOptionElement* PR_NULLABLE option) {
    if (option == nullptr) {
        prHtmlSelectElementRemove(select, int32_t(index));
        return PrStatus_Ok;
    }
    uint32_t length = prHtmlSelectElementLength(select);
    if (index < length) {
        PrHtmlOptionElement* replaced = prHtmlSelectElementItem(select, index);
        return replaced == option ? PrStatus_Ok : domReplace(replaced->parent, option, replaced);
    }
    prHtmlSelectElementSetLength(select, index);
    return domPreInsert(select, option, nullptr);
}

PrStatus prHtmlSelectElementAdd(PrHtmlSelectElement* select, PrHtmlElement* element, PrHtmlElement* PR_NULLABLE before) {
    if (domIsInclusiveAncestor(element, select))
        return PrStatus_NotAllowed;
    if (before != nullptr && (before == select || !domIsInclusiveAncestor(select, before)))
        return PrStatus_NotFound;
    if (element == before)
        return PrStatus_Ok;
    return domPreInsert(before != nullptr ? before->parent : select, element, before);
}

PrStatus prHtmlSelectElementAddAt(PrHtmlSelectElement* select, PrHtmlElement* element, uint32_t index) {
    return prHtmlSelectElementAdd(select, element, prHtmlSelectElementItem(select, index));
}

void prHtmlSelectElementRemove(PrHtmlSelectElement* select, int32_t index) {
    if (index < 0)
        return;
    if (PrHtmlOptionElement* option = prHtmlSelectElementItem(select, uint32_t(index)))
        domRemoveFromParent(option);
}

int32_t prHtmlSelectElementSelectedIndex(PrHtmlSelectElement* select) {
    return domSelectSelectedIndex(select);
}

void prHtmlSelectElementSetSelectedIndex(PrHtmlSelectElement* select, int32_t index) {
    domSelectSetSelectedIndex(select, index);
}

PrRawString prHtmlSelectElementValue(PrHtmlSelectElement* select) {
    int32_t index = domSelectSelectedIndex(select);
    if (index < 0)
        return prRawStringEmpty();
    return prHtmlOptionElementValue(prHtmlSelectElementItem(select, uint32_t(index)));
}

void prHtmlSelectElementSetValue(PrHtmlSelectElement* select, PrStringView value) {
    domSelectSetValue(select, value);
}

PrHtmlOptionElement* PR_NULLABLE prHtmlOptionElementQuery(PrDomEventTarget* PR_NULLABLE object) {
    return htmlElementQuery<PrHtmlOptionElement>(object, PrDomTag_Option);
}

PrHtmlOptionElement* prHtmlOptionElementCreate(PrDomDocument* document, const PrHtmlOptionDesc* desc) {
    PrDomElement* option = domElementCreate(document, PrDomNamespace_Html, PrDomTag_Option, "option");
    domNodeRetain(option);
    if (desc->text.size != 0)
        prDomNodeSetTextContent(option, desc->text);
    if (desc->value.data != nullptr)
        domSetAttribute(option, PrDomNamespace_None, "value", desc->value);
    if (desc->defaultSelected)
        domSetAttribute(option, PrDomNamespace_None, "selected", "");
    if (desc->selected)
        domOptionSetSelected(option, true);
    return static_cast<PrHtmlOptionElement*>(option);
}

PrHtmlFormElement* PR_NULLABLE prHtmlOptionElementForm(PrHtmlOptionElement* option) {
    PrDomElement* select = domNearestAncestorSelect(option);
    return select != nullptr ? prHtmlElementForm(static_cast<PrHtmlElement*>(select)) : nullptr;
}

PrRawString prHtmlOptionElementText(const PrHtmlOptionElement* option) {
    PrTextBuffer text = {};
    domOptionText(option, &text);
    return prTextBufferTake(&text);
}

void prHtmlOptionElementSetText(PrHtmlOptionElement* option, PrStringView value) {
    prDomNodeSetTextContent(option, value);
}

PrRawString prHtmlOptionElementValue(const PrHtmlOptionElement* option) {
    PrTextBuffer text = {};
    domOptionValue(option, &text);
    return prTextBufferTake(&text);
}

PrRawString prHtmlOptionElementLabel(const PrHtmlOptionElement* option) {
    PrStringView label = domAttributeValue(option, "label");
    return label.size != 0 ? prRawStringCopy(label) : prHtmlOptionElementText(option);
}

bool prHtmlOptionElementSelected(const PrHtmlOptionElement* option) {
    return domOptionSelected(option);
}

void prHtmlOptionElementSetSelected(PrHtmlOptionElement* option, bool value) {
    domOptionSetSelected(option, value);
}

uint32_t prHtmlOptionElementIndex(PrHtmlOptionElement* option) {
    PrDomElement* select = domNearestAncestorSelect(option);
    if (select == nullptr)
        return 0;
    RawArray<PrDomElement* PR_NONNULL> options;
    domSelectOptions(select, &options);
    uint32_t index = options.find(option);
    options.reset();
    return index == kArrayMissing ? 0 : index;
}

PrHtmlDataListElement* PR_NULLABLE prHtmlDataListElementQuery(PrDomEventTarget* PR_NULLABLE object) {
    return htmlElementQuery<PrHtmlDataListElement>(object, PrDomTag_Datalist);
}

PrDomHtmlCollection* prHtmlDataListElementOptions(PrHtmlDataListElement* list) {
    return domHtmlCollectionCreate(list, DomCollectionFilter_DataListOptions, PrStringView{});
}
