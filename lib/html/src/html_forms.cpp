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
#include "pr/url.h"

#include "dom/src/dom_attribute.h"
#include "dom/src/dom_collection.h"
#include "dom/src/dom_control.h"
#include "dom/src/dom_element_state.h"
#include "dom/src/dom_event.h"
#include "dom/src/dom_microsyntax.h"
#include "dom/src/dom_node.h"
#include "dom/src/dom_select.h"
#include "html/src/html_interface.h"

namespace pr_impl {
namespace {

PrString stringOf(const TextBuffer* text) {
    return prStringCopy(textView(text));
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
        TextBuffer prefix;
        if (name.size != 0) {
            textAppend(&prefix, name);
            textAppendChar(&prefix, '.');
        }
        uint32_t size = prefix.text.count;
        textAppendChar(&prefix, 'x');
        domEntryListAppend(list, textView(&prefix), "0");
        prefix.text.count = size;
        textAppendChar(&prefix, 'y');
        domEntryListAppend(list, textView(&prefix), "0");
        textReset(&prefix);
        return;
    }
    if (name.size == 0)
        return;
    TextBuffer value;
    if (domIsHtml(field, PrDomTag_Select)) {
        Array<PrDomElement* PR_NONNULL> options;
        domSelectOptions(field, &options);
        for (uint32_t i = 0; i < options.count; ++i) {
            if (!domOptionSelected(options.data[i]) || domOptionDisabled(options.data[i]))
                continue;
            domOptionValue(options.data[i], &value);
            domEntryListAppend(list, name, textView(&value));
        }
        options.reset();
    } else if (input && (type == PrDomInputType_Checkbox || type == PrDomInputType_Radio)) {
        PrStringView attribute = domAttributeValue(field, "value");
        domEntryListAppend(list, name, attribute.data != nullptr ? attribute : "on");
    } else if (input && type == PrDomInputType_File) {
        // A File entry waits for the File API.
    } else if (input && type == PrDomInputType_Hidden && asciiEqualsFold(name, "_charset_")) {
        domEntryListAppend(list, name, "UTF-8");
    } else if (domIsHtml(field, PrDomTag_Button)) {
        domEntryListAppend(list, name, domAttributeValue(field, "value"));
    } else {
        domControlValue(field, &value);
        domEntryListAppend(list, name, textView(&value));
    }
    textReset(&value);
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
    Array<PrDomNode* PR_NONNULL> controls;
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
        domEntryListAppend(clone, prStringView(&list->entries.data[i].name),
                           prStringView(&list->entries.data[i].value));
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

} // namespace

void htmlColorWellValue(const PrDomElement* input, PrStringView text, TextBuffer* out) {
    PrCssUsedColor color{};
    if (text.data == nullptr || prCssParseColor(text, input, &color) != PrStatus_Ok)
        color = PrCssUsedColor{.space = PrColorSpace_Srgb, .legacy = true, .alpha = 1};
    bool alpha = domHasAttribute(input, "alpha");
    if (!alpha)
        color.alpha = 1;
    // html/#attr-input-colorspace: Limited sRGB is the missing value default and the invalid value default.
    bool p3 = asciiEqualsFold(domAttributeValue(input, "colorspace"), "display-p3");
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
            double steps = floorF64(converted[i] * 255 + 0.5);
            result.components[i] = (steps < 0 ? 0 : steps > 255 ? 255 : steps) / 255;
        }
        if (!alpha) {
            result.legacy = true;
            serialization = PrCssColorSerialization_HtmlCompatible;
        }
    }
    PrString serialized = prCssUsedColorSerialize(&result, serialization);
    textAppend(out, prStringView(&serialized));
    prStringReset(&serialized);
}

} // namespace pr_impl

using namespace pr_impl;

// ---------------------------------------------------------------------------------------------------------------------
// Form-associated elements

PrHtmlFormElement* PR_NULLABLE prHtmlElementForm(PrHtmlElement* self) {
    if (domIsHtml(self, PrDomTag_Legend)) {
        PrDomElement* parent = domElementFromNode(self->parent);
        return domIsHtml(parent, PrDomTag_Fieldset) ? static_cast<PrHtmlFormElement*>(domFormOwner(parent))
                                                    : nullptr;
    }
    return static_cast<PrHtmlFormElement*>(domFormOwner(self));
}

PrDomNodeList* PR_NULLABLE prHtmlElementLabels(PrHtmlElement* self) {
    if (!domIsLabelable(self))
        return nullptr;
    return domNodeListCreateFiltered(self, DomNodeListFilter_Labels, PrStringView{});
}

PrString prHtmlElementFormActionUrl(const PrHtmlElement* self, PrStringView name) {
    PrStringView action = domAttributeValue(self, name);
    if (action.size == 0)
        return prUrlSerialize(self->document->url, PrUrlSerialization_Whole);
    PrUrl* url = nullptr;
    PrString out = domDocumentParseUrl(self->document, action, &url) == PrStatus_Ok
                       ? prUrlSerialize(url, PrUrlSerialization_Whole)
                       : prStringCopy(action);
    prUrlDestroy(url);
    return out;
}

// ---------------------------------------------------------------------------------------------------------------------
// HTMLFormElement

PrHtmlFormElement* PR_NULLABLE prHtmlFormElementQuery(PrDomEventTarget* PR_NULLABLE object) {
    return htmlElementQuery<PrHtmlFormElement>(object, PrDomTag_Form);
}

PrDomHtmlCollection* prHtmlFormElementElements(PrHtmlFormElement* self) {
    return domHtmlCollectionCreate(self, DomCollectionFilter_FormControls, PrStringView{});
}

PrDomNodeList* prHtmlFormElementRadioNodeList(PrHtmlFormElement* self, PrStringView name, bool images) {
    return domNodeListCreateFiltered(self, images ? DomNodeListFilter_FormImages : DomNodeListFilter_Radio, name);
}

uint32_t prHtmlFormElementNamedItem(PrHtmlFormElement* self, PrStringView name,
                                    PrDomElement* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    if (name.size == 0)
        return 0;
    uint32_t count = 0;
    for (uint32_t pass = 0; pass < 2; ++pass) {
        PrDomNodeList* list = prHtmlFormElementRadioNodeList(self, name, pass == 1);
        count = domNodeListLength(list);
        if (count == 1)
            *out = domElementFromNode(domNodeListItem(list, 0));
        domNodeListDestroy(list);
        if (count != 0)
            break;
    }
    if (count == 0) {
        *out = domFormPastName(self, name);
        return *out != nullptr ? 1 : 0;
    }
    if (count == 1)
        domFormRememberName(self, name, *out);
    return count;
}

uint32_t prHtmlFormElementNames(PrHtmlFormElement* self, PrString* PR_NULLABLE items, uint32_t capacity) {
    // html/#the-form-element:supported-property-names: the names of the form's controls, then of its img elements, then
    // of its past names map, each element's ID before its name, in the tree order of their elements.
    struct Source {
        PrStringView name;
        PrDomElement* element;
        uint32_t rank;              // 0 for an ID, 1 for a name, 2 for a past name
        uint32_t order;             // the place of the source in the list, among those of one element and rank
    };
    Array<Source> sources;
    PrDomNode* root = domNodeRoot(self);
    for (PrDomNode* node = domNodeNext(root, root); node != nullptr; node = domNodeNext(root, node)) {
        PrDomElement* element = domElementFromNode(node);
        if (element == nullptr || domFormOwner(element) != self)
            continue;
        bool listed = domIsListed(element)
                      && !(domIsHtml(element, PrDomTag_Input) && domElementInputType(element) == PrDomInputType_Image);
        if (!listed && !domIsHtml(element, PrDomTag_Img))
            continue;
        sources.push(Source{domElementId(element), element, 0, sources.count});
        sources.push(Source{domAttributeValue(element, "name"), element, 1, sources.count});
    }
    if (const DomForm* record = self->state != nullptr ? self->state->form : nullptr) {
        for (uint32_t i = 0; i < record->pastNames.count; ++i)
            sources.push(Source{prStringView(&record->pastNames.data[i].name), record->pastNames.data[i].element, 2,
                                sources.count});
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
            items[count] = prStringCopy(name);
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

PrDomInputType prHtmlInputElementType(const PrHtmlInputElement* self) {
    return domElementInputType(self);
}

PrString prHtmlInputElementValue(const PrHtmlInputElement* self) {
    TextBuffer value;
    domControlValue(self, &value);
    PrString out = stringOf(&value);
    textReset(&value);
    return out;
}

PrStatus prHtmlInputElementSetValue(PrHtmlInputElement* self, PrStringView value) {
    return domControlSetValue(self, value);
}

bool prHtmlInputElementChecked(const PrHtmlInputElement* self) {
    return domInputChecked(self);
}

void prHtmlInputElementSetChecked(PrHtmlInputElement* self, bool value) {
    domInputSetChecked(self, value, true);
}

bool prHtmlInputElementIndeterminate(const PrHtmlInputElement* self) {
    return domInputIndeterminate(self);
}

void prHtmlInputElementSetIndeterminate(PrHtmlInputElement* self, bool value) {
    domInputSetIndeterminate(self, value);
}

double prHtmlInputElementValueAsNumber(const PrHtmlInputElement* self) {
    if ((domInputApplies(domElementInputType(self)) & DomInputApplyFlag_ValueAsNumber) == 0)
        return kNanF64;
    TextBuffer text;
    domControlValue(self, &text);
    double value = kNanF64;
    if (!domInputStringToNumber(self, textView(&text), &value))
        value = kNanF64;
    textReset(&text);
    return value;
}

PrStatus prHtmlInputElementSetValueAsNumber(PrHtmlInputElement* self, double value) {
    if (!isFiniteF64(value) && !isNanF64(value))
        return PrStatus_WrongType;
    if ((domInputApplies(domElementInputType(self)) & DomInputApplyFlag_ValueAsNumber) == 0)
        return PrStatus_InvalidState;
    if (isNanF64(value))
        return domControlSetValue(self, "");
    TextBuffer text;
    domInputNumberToString(self, value, &text);
    PrStatus status = domControlSetValue(self, textView(&text));
    textReset(&text);
    return status;
}

bool prHtmlInputElementValueAsDate(const PrHtmlInputElement* self, double* out) {
    if ((domInputApplies(domElementInputType(self)) & DomInputApplyFlag_ValueAsDate) == 0)
        return false;
    TextBuffer text;
    domControlValue(self, &text);
    bool converted = domInputStringToDate(self, textView(&text), out);
    textReset(&text);
    return converted;
}

PrStatus prHtmlInputElementSetValueAsDate(PrHtmlInputElement* self, double time) {
    if ((domInputApplies(domElementInputType(self)) & DomInputApplyFlag_ValueAsDate) == 0)
        return PrStatus_InvalidState;
    if (!isFiniteF64(time))
        return domControlSetValue(self, "");
    TextBuffer text;
    domInputDateToString(self, time, &text);
    PrStatus status = domControlSetValue(self, textView(&text));
    textReset(&text);
    return status;
}

PrStatus prHtmlInputElementStepUp(PrHtmlInputElement* self, int32_t n) {
    return domInputStep(self, n, true);
}

PrStatus prHtmlInputElementStepDown(PrHtmlInputElement* self, int32_t n) {
    return domInputStep(self, n, false);
}

PrDomElement* PR_NULLABLE prHtmlInputElementList(PrHtmlInputElement* self) {
    // html/#attr-input-list: the attribute does not apply to these types.
    switch (domElementInputType(self)) {
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
    PrStringView id = domAttributeValue(self, "list");
    if (id.data == nullptr)
        return nullptr;
    PrDomElement* found = domElementById(domNodeRoot(self), id);
    return domIsHtml(found, PrDomTag_Datalist) ? found : nullptr;
}

uint32_t prHtmlInputElementWidth(const PrHtmlInputElement* self) {
    return prHtmlInputElementType(self) == PrDomInputType_Image ? htmlImageDimension(self, true) : 0;
}

uint32_t prHtmlInputElementHeight(const PrHtmlInputElement* self) {
    return prHtmlInputElementType(self) == PrDomInputType_Image ? htmlImageDimension(self, false) : 0;
}

void prHtmlInputElementSetWidth(PrHtmlInputElement* self, uint32_t value) {
    htmlSetReflectedUnsigned(self, "width", value);
}

void prHtmlInputElementSetHeight(PrHtmlInputElement* self, uint32_t value) {
    htmlSetReflectedUnsigned(self, "height", value);
}

// ---------------------------------------------------------------------------------------------------------------------
// HTMLButtonElement

PrHtmlButtonElement* PR_NULLABLE prHtmlButtonElementQuery(PrDomEventTarget* PR_NULLABLE object) {
    return htmlElementQuery<PrHtmlButtonElement>(object, PrDomTag_Button);
}

PrStringView prHtmlButtonElementType(const PrHtmlButtonElement* self) {
    if (domIsSubmitButton(self))
        return "submit";
    DomButtonType type = domButtonType(domAttributeValue(self, "type"));
    return type == DomButtonType_Auto ? PrStringView{"button"} : domButtonTypeKeyword(type);
}

// ---------------------------------------------------------------------------------------------------------------------
// HTMLTextAreaElement

PrHtmlTextAreaElement* PR_NULLABLE prHtmlTextAreaElementQuery(PrDomEventTarget* PR_NULLABLE object) {
    return htmlElementQuery<PrHtmlTextAreaElement>(object, PrDomTag_Textarea);
}

PrString prHtmlTextAreaElementValue(const PrHtmlTextAreaElement* self) {
    TextBuffer value;
    domControlValue(self, &value);
    PrString out = stringOf(&value);
    textReset(&value);
    return out;
}

void prHtmlTextAreaElementSetValue(PrHtmlTextAreaElement* self, PrStringView value) {
    domControlSetValue(self, value);
}

PrString prHtmlTextAreaElementDefaultValue(const PrHtmlTextAreaElement* self) {
    TextBuffer value;
    domTextareaDefaultValue(self, &value);
    PrString out = stringOf(&value);
    textReset(&value);
    return out;
}

void prHtmlTextAreaElementSetDefaultValue(PrHtmlTextAreaElement* self, PrStringView value) {
    domStringReplaceAll(self, value);
}

uint32_t prHtmlTextAreaElementTextLength(const PrHtmlTextAreaElement* self) {
    TextBuffer value;
    domControlValue(self, &value);
    // A code point counts one UTF-16 code unit, and one past the Basic Multilingual Plane, which takes four bytes in
    // UTF-8, counts two.
    uint32_t length = 0;
    for (uint32_t i = 0; i < value.text.count; ++i) {
        uint8_t byte = uint8_t(value.text.data[i]);
        if ((byte & 0xc0) != 0x80)
            length += byte >= 0xf0 ? 2 : 1;
    }
    textReset(&value);
    return length;
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

PrDomHtmlCollection* prHtmlFieldSetElementElements(PrHtmlFieldSetElement* self) {
    return domHtmlCollectionCreate(self, DomCollectionFilter_FieldSetElements, PrStringView{});
}

PrDomElement* PR_NULLABLE prHtmlLabelElementControl(PrHtmlLabelElement* self) {
    return const_cast<PrDomElement*>(domLabeledControl(self));
}

// ---------------------------------------------------------------------------------------------------------------------
// HTMLOutputElement

PrHtmlOutputElement* PR_NULLABLE prHtmlOutputElementQuery(PrDomEventTarget* PR_NULLABLE object) {
    return htmlElementQuery<PrHtmlOutputElement>(object, PrDomTag_Output);
}

PrString prHtmlOutputElementValue(const PrHtmlOutputElement* self) {
    return domNodeTextContent(self);
}

void prHtmlOutputElementSetValue(PrHtmlOutputElement* self, PrStringView value) {
    DomControl* control = domControlFromElement(self);
    if (prStringIsNull(&control->defaultOverride))
        control->defaultOverride = domNodeTextContent(self);
    domStringReplaceAll(self, value);
}

PrString prHtmlOutputElementDefaultValue(const PrHtmlOutputElement* self) {
    const DomControl* control = domControl(self);
    if (control == nullptr || prStringIsNull(&control->defaultOverride))
        return domNodeTextContent(self);
    return prStringClone(&control->defaultOverride);
}

void prHtmlOutputElementSetDefaultValue(PrHtmlOutputElement* self, PrStringView value) {
    DomControl* control = domControlFromElement(self);
    if (prStringIsNull(&control->defaultOverride))
        domStringReplaceAll(self, value);
    else
        prStringReplace(&control->defaultOverride, value);
}

// ---------------------------------------------------------------------------------------------------------------------
// HTMLProgressElement and HTMLMeterElement

PrHtmlProgressElement* PR_NULLABLE prHtmlProgressElementQuery(PrDomEventTarget* PR_NULLABLE object) {
    return htmlElementQuery<PrHtmlProgressElement>(object, PrDomTag_Progress);
}

PrHtmlMeterElement* PR_NULLABLE prHtmlMeterElementQuery(PrDomEventTarget* PR_NULLABLE object) {
    return htmlElementQuery<PrHtmlMeterElement>(object, PrDomTag_Meter);
}

double prHtmlProgressElementValue(const PrHtmlProgressElement* self) {
    double value = parsedOr(self, "value", 0);
    double max = prHtmlProgressElementMax(self);
    return value < 0 ? 0 : value > max ? max : value;
}

double prHtmlProgressElementMax(const PrHtmlProgressElement* self) {
    double max = parsedOr(self, "max", 1);
    return max > 0 ? max : 1;
}

double prHtmlProgressElementPosition(const PrHtmlProgressElement* self) {
    if (domAttributeValue(self, "value").data == nullptr)
        return -1;
    return prHtmlProgressElementValue(self) / prHtmlProgressElementMax(self);
}

PrHtmlMeterValues prHtmlMeterElementValues(const PrHtmlMeterElement* self) {
    PrHtmlMeterValues values{};
    values.min = parsedOr(self, "min", 0);
    values.max = parsedOr(self, "max", 1);
    if (values.max < values.min)
        values.max = values.min;
    values.value = parsedOr(self, "value", 0);
    values.value = values.value < values.min ? values.min : values.value > values.max ? values.max : values.value;
    values.low = parsedOr(self, "low", values.min);
    values.low = values.low < values.min ? values.min : values.low > values.max ? values.max : values.low;
    values.high = parsedOr(self, "high", values.max);
    values.high = values.high < values.low ? values.low : values.high > values.max ? values.max : values.high;
    values.optimum = parsedOr(self, "optimum", values.min + (values.max - values.min) / 2);
    values.optimum = values.optimum < values.min   ? values.min
                     : values.optimum > values.max ? values.max
                                                   : values.optimum;
    return values;
}

PrStatus prHtmlElementSetReflectedDouble(PrHtmlElement* self, PrStringView name, double value) {
    if (!isFiniteF64(value))
        return PrStatus_WrongType;
    // html/#dom-progress-max is limited to only positive numbers.
    if (domIsHtml(self, PrDomTag_Progress) && name == "max" && value <= 0)
        return PrStatus_Ok;
    TextBuffer text;
    domAppendFloat(&text, value);
    domSetAttribute(self, PrDomNamespace_None, name, textView(&text));
    textReset(&text);
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

void prHtmlFormDataRetain(PrHtmlFormData* self) {
    domEntryListRetain(self);
}

void prHtmlFormDataRelease(PrHtmlFormData* PR_NULLABLE self) {
    domEntryListRelease(self);
}

void prHtmlFormDataAppend(PrHtmlFormData* self, PrStringView name, PrStringView value) {
    domEntryListAppend(self, name, value);
}

void prHtmlFormDataDelete(PrHtmlFormData* self, PrStringView name) {
    for (uint32_t i = self->entries.count; i-- != 0;) {
        DomEntry& entry = self->entries.data[i];
        if (prStringView(&entry.name) == name) {
            prStringReset(&entry.name);
            prStringReset(&entry.value);
            self->entries.erase(i);
        }
    }
}

PrStringView prHtmlFormDataGet(const PrHtmlFormData* self, PrStringView name) {
    for (uint32_t i = 0; i < self->entries.count; ++i) {
        if (prStringView(&self->entries.data[i].name) == name)
            return prStringView(&self->entries.data[i].value);
    }
    return {};
}

uint32_t prHtmlFormDataGetAll(const PrHtmlFormData* self, PrStringView name, PrStringView* PR_NULLABLE items,
                              uint32_t capacity) {
    uint32_t count = 0;
    for (uint32_t i = 0; i < self->entries.count; ++i) {
        if (prStringView(&self->entries.data[i].name) != name)
            continue;
        if (items != nullptr && count < capacity)
            items[count] = prStringView(&self->entries.data[i].value);
        ++count;
    }
    return count;
}

bool prHtmlFormDataHas(const PrHtmlFormData* self, PrStringView name) {
    for (uint32_t i = 0; i < self->entries.count; ++i) {
        if (prStringView(&self->entries.data[i].name) == name)
            return true;
    }
    return false;
}

void prHtmlFormDataSet(PrHtmlFormData* self, PrStringView name, PrStringView value) {
    uint32_t first = self->entries.count;
    for (uint32_t i = self->entries.count; i-- != 0;) {
        DomEntry& entry = self->entries.data[i];
        if (prStringView(&entry.name) != name)
            continue;
        if (first != self->entries.count) {
            prStringReset(&self->entries.data[first].name);
            prStringReset(&self->entries.data[first].value);
            self->entries.erase(first);
        }
        first = i;
    }
    if (first == self->entries.count)
        domEntryListAppend(self, name, value);
    else
        prStringReplace(&self->entries.data[first].value, value);
}

uint32_t prHtmlFormDataCount(const PrHtmlFormData* self) {
    return self->entries.count;
}

bool prHtmlFormDataEntry(const PrHtmlFormData* self, uint32_t index, PrStringView* name, PrStringView* value) {
    if (index >= self->entries.count)
        return false;
    *name = prStringView(&self->entries.data[index].name);
    *value = prStringView(&self->entries.data[index].value);
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
                                           PrStringView oldState, PrStringView newState,
                                           PrDomElement* PR_NULLABLE source) {
    PrHtmlToggleEvent* event = domEventCreate<PrHtmlToggleEvent>(
        type, init != nullptr ? *init : prDomEventInitDefault(), PrDomEventKind_Toggle);
    DomControlEventData* data = controlDataOf(event);
    data->oldState = prStringCopy(oldState);
    data->newState = prStringCopy(newState);
    if (source != nullptr)
        domEventTargetAssign(&data->source, prDomElementAsEventTarget(source));
    return event;
}

PrHtmlCommandEvent* prHtmlCommandEventCreate(PrStringView type, const PrDomEventInit* PR_NULLABLE init,
                                             PrStringView command, PrDomElement* PR_NULLABLE source) {
    PrHtmlCommandEvent* event = domEventCreate<PrHtmlCommandEvent>(
        type, init != nullptr ? *init : prDomEventInitDefault(), PrDomEventKind_Command);
    DomControlEventData* data = controlDataOf(event);
    data->command = prStringCopy(command);
    if (source != nullptr)
        domEventTargetAssign(&data->source, prDomElementAsEventTarget(source));
    return event;
}

PrHtmlElement* PR_NULLABLE prHtmlSubmitEventSubmitter(PrHtmlSubmitEvent* self) {
    PrDomEventTarget* source = self->control != nullptr ? self->control->source : nullptr;
    return static_cast<PrHtmlElement*>(domElementFromNode(domNodeFromTarget(source)));
}

PrHtmlFormData* prHtmlFormDataEventFormData(PrHtmlFormDataEvent* self) {
    return static_cast<PrHtmlFormData*>(self->control->entries);
}

PrStringView prHtmlToggleEventOldState(const PrHtmlToggleEvent* self) {
    return prStringView(&self->control->oldState);
}

PrStringView prHtmlToggleEventNewState(const PrHtmlToggleEvent* self) {
    return prStringView(&self->control->newState);
}

PrStringView prHtmlCommandEventCommand(const PrHtmlCommandEvent* self) {
    return prStringView(&self->control->command);
}

PrDomElement* PR_NULLABLE prHtmlToggleEventSource(PrHtmlToggleEvent* self) {
    return retargetedSource(self);
}

PrDomElement* PR_NULLABLE prHtmlCommandEventSource(PrHtmlCommandEvent* self) {
    return retargetedSource(self);
}
