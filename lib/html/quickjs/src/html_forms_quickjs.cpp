// JavaScript binding of forms.
//
// html - https://html.spec.whatwg.org/multipage/forms.html

#include "html/quickjs/src/html_forms_quickjs.h"

#include "pr/count_of.h"
#include "pr/dom_quickjs.h"
#include "pr/float.h"
#include "pr/html.h"
#include "pr/html_forms.h"
#include "pr/quickjs_binding.h"
#include "pr/string_ops.h"
#include "pr/webidl_quickjs.h"

#include "html/quickjs/src/html_bindings_quickjs.h"
#include "html/quickjs/src/html_reflect_quickjs.h"

namespace pr_impl {
namespace {

JSClassID htmlFormElementClass;
JSClassID htmlFormControlsCollectionClass;
JSClassID htmlRadioNodeListClass;
JSClassID htmlInputElementClass;
JSClassID htmlButtonElementClass;
JSClassID htmlTextAreaElementClass;
JSClassID htmlFieldSetElementClass;
JSClassID htmlLegendElementClass;
JSClassID htmlLabelElementClass;
JSClassID htmlOutputElementClass;
JSClassID htmlProgressElementClass;
JSClassID htmlMeterElementClass;
JSClassID htmlDetailsElementClass;
JSClassID htmlFormDataClass;
JSClassID htmlFormDataIteratorClass;
JSClassID htmlSubmitEventClass;
JSClassID htmlFormDataEventClass;
JSClassID htmlToggleEventClass;
JSClassID htmlCommandEventClass;

// ---------------------------------------------------------------------------------------------------------------------
// The members that form controls share

// The keywords of the enumerated attributes that reflect limited to only known values.
constexpr PrStringView kAutocompleteKeywords[] = {"on", "off"};
constexpr PrStringView kEnctypeKeywords[] = {"application/x-www-form-urlencoded", "multipart/form-data",
                                             "text/plain"};
constexpr PrStringView kMethodKeywords[] = {"get", "post", "dialog"};
constexpr PrStringView kColorSpaceKeywords[] = {"limited-srgb", "display-p3"};

// A form's attributes default to their first state, and a submitter's override nothing while they are missing.
constexpr JsReflectedKeywords kAutocomplete = {kAutocompleteKeywords, 2, "on", "on"};
constexpr JsReflectedKeywords kEnctype = {kEnctypeKeywords, 3, "application/x-www-form-urlencoded",
                                          "application/x-www-form-urlencoded"};
constexpr JsReflectedKeywords kFormEnctype = {kEnctypeKeywords, 3, "", "application/x-www-form-urlencoded"};
constexpr JsReflectedKeywords kMethod = {kMethodKeywords, 3, "get", "get"};
constexpr JsReflectedKeywords kFormMethod = {kMethodKeywords, 3, "", "get"};
constexpr JsReflectedKeywords kColorSpace = {kColorSpaceKeywords, 2, "limited-srgb", "limited-srgb"};

PrHtmlElement* PR_NULLABLE htmlThis(JSContext* ctx, JSValueConst self) {
    return jsThis<PrHtmlElement>(ctx, self, htmlElementClass);
}

// form, the element's form owner.
JSValue elementForm(JSContext* ctx, JSValueConst self) {
    PrHtmlElement* element = htmlThis(ctx, self);
    if (element == nullptr)
        return JS_EXCEPTION;
    PrHtmlFormElement* form = prHtmlElementForm(element);
    PrDomElement* wrapped = form != nullptr ? prHtmlFormElementAsElement(form) : nullptr;
    return jsWrapAs(ctx, prJsDomElementClass, JS_UNDEFINED, wrapped, false);
}

// labels, the same NodeList at each read, or null for an element that no label can label.
JSValue elementLabels(JSContext* ctx, JSValueConst self) {
    PrHtmlElement* element = htmlThis(ctx, self);
    if (element == nullptr)
        return JS_EXCEPTION;
    PrDomNodeList* list = prHtmlElementLabels(element);
    if (list == nullptr)
        return JS_NULL;
    JSValue object = jsSameObject(ctx, self, "labels", [&] {
        JSValue made = jsWrapAs(ctx, prJsDomNodeListClass, self, list, true);
        if (JS_IsException(made))
            prDomNodeListDestroy(list);
        list = nullptr;
        return made;
    });
    prDomNodeListDestroy(list);
    return object;
}

// action and formAction, which name the document's URL while the attribute is missing or empty. The setter is a
// DOMString attribute's setter.
template <JsLiteral Name>
JSValue formActionUrl(JSContext* ctx, JSValueConst self) {
    PrHtmlElement* element = htmlThis(ctx, self);
    return element != nullptr ? jsTakeString(ctx, prHtmlElementFormActionUrl(element, Name.text)) : JS_EXCEPTION;
}

// The type of an element whose type is its local name, such as a fieldset's.
template <JsLiteral Type>
JSValue fixedType(JSContext* ctx, JSValueConst self) {
    return htmlThis(ctx, self) != nullptr ? jsFixedString(ctx, Type.text) : JS_EXCEPTION;
}

// The members of a submit button that override its form's (html/#form-submission-attributes).
#define PR_SUBMITTER_ATTRIBUTES                                                                    \
    PR_JS_GET_SET("formAction", formActionUrl<"formaction">, jsSetReflectString<"formaction">),    \
        PR_REFLECT_ENUMERATED("formEnctype", "formenctype", &kFormEnctype),                        \
        PR_REFLECT_ENUMERATED("formMethod", "formmethod", &kFormMethod),                           \
        PR_REFLECT_BOOL("formNoValidate", "formnovalidate"), PR_REFLECT_STRING("formTarget", "formtarget")

// ---------------------------------------------------------------------------------------------------------------------
// RadioNodeList (html/#radionodelist) and HTMLFormControlsCollection (html/#htmlformcontrolscollection)

PrDomNodeList* PR_NULLABLE nodeListThis(JSContext* ctx, JSValueConst self, JSClassID id) {
    return jsThis<PrDomNodeList>(ctx, self, id);
}

uint32_t radioLength(void* handle) {
    return prDomNodeListLength(static_cast<PrDomNodeList*>(handle));
}

JSValue radioItem(JSContext* ctx, JSValueConst self, void* handle, uint32_t index) {
    PrDomNode* node = prDomNodeListNodeAt(static_cast<PrDomNodeList*>(handle), index);
    return jsWrapAs(ctx, prJsDomNodeClass, self, node, false);
}

constexpr JsIndexed kRadioIndexed = {radioLength, radioItem, nullptr};

// Returns the radio of the list whose value is `value`, or the checked radio for the null view.
PrHtmlInputElement* PR_NULLABLE findRadio(PrDomNodeList* list, PrStringView value) {
    for (uint32_t i = 0; i < prDomNodeListLength(list); ++i) {
        PrHtmlInputElement* input = prHtmlInputElementQuery(prDomNodeAsEventTarget(prDomNodeListNodeAt(list, i)));
        if (input == nullptr || prHtmlInputElementType(input) != PrDomInputType_Radio)
            continue;
        if (value.data == nullptr) {
            if (prHtmlInputElementChecked(input))
                return input;
            continue;
        }
        PrStringView own = prDomElementGetAttribute(prHtmlInputElementAsElement(input), "value");
        if (own.data == nullptr ? value == "on" : own == value)
            return input;
    }
    return nullptr;
}

// html/#dom-radionodelist-value: the value of the checked radio, or the empty string if none is checked.
JSValue radioValue(JSContext* ctx, JSValueConst self) {
    PrDomNodeList* list = nodeListThis(ctx, self, htmlRadioNodeListClass);
    if (list == nullptr)
        return JS_EXCEPTION;
    PrHtmlInputElement* checked = findRadio(list, PrStringView{});
    return checked != nullptr ? jsTakeString(ctx, prHtmlInputElementValue(checked)) : jsFixedString(ctx, "");
}

// Checks the first radio whose value is the given value.
JSValue setRadioValue(JSContext* ctx, JSValueConst self, JSValueConst input) {
    PrDomNodeList* list = nodeListThis(ctx, self, htmlRadioNodeListClass);
    StringArgument value = {ctx};
    if (list == nullptr || !value.read(input))
        return JS_EXCEPTION;
    if (PrHtmlInputElement* radio = findRadio(list, value.value()))
        prHtmlInputElementSetChecked(radio, true);
    return JS_UNDEFINED;
}

uint32_t controlsLength(void* handle) {
    return prDomHtmlCollectionLength(static_cast<PrDomHtmlCollection*>(handle));
}

JSValue controlsItem(JSContext* ctx, JSValueConst self, void* handle, uint32_t index) {
    PrDomElement* element = prDomHtmlCollectionElementAt(static_cast<PrDomHtmlCollection*>(handle), index);
    return jsWrapAs(ctx, prJsDomElementClass, self, element, false);
}

// html/#dom-htmlformcontrolscollection-nameditem: the one control of the name, or a RadioNodeList of several, or
// undefined. A collection's controls are those of one form, which the first of them names.
JSValue controlsNamed(JSContext* ctx, JSValueConst self, void* handle, PrStringView name) {
    PrDomHtmlCollection* collection = static_cast<PrDomHtmlCollection*>(handle);
    if (name.size == 0)
        return JS_UNDEFINED;
    PrDomElement* found = nullptr;
    uint32_t count = 0;
    for (uint32_t i = 0; i < prDomHtmlCollectionLength(collection); ++i) {
        PrDomElement* element = prDomHtmlCollectionElementAt(collection, i);
        if (prDomElementGetAttribute(element, "id") == name || prDomElementGetAttribute(element, "name") == name) {
            found = found != nullptr ? found : element;
            ++count;
        }
    }
    if (count == 0)
        return JS_UNDEFINED;
    if (count == 1)
        return jsWrapAs(ctx, prJsDomElementClass, self, found, false);
    PrHtmlFormElement* form = prHtmlElementForm(prHtmlElementQuery(prDomElementAsEventTarget(found)));
    PrDomNodeList* list = prHtmlFormElementRadioNodeList(form, name, false);
    JSValue object = jsWrapAs(ctx, htmlRadioNodeListClass, self, list, true);
    if (JS_IsException(object))
        prDomNodeListDestroy(list);
    return object;
}

bool controlsNames(JSContext* ctx, void* handle, Array<JSAtom>* out) {
    return jsNameAtoms(ctx, static_cast<const PrDomHtmlCollection*>(handle), prDomHtmlCollectionNames, out);
}

constexpr JsIndexed kControlsIndexed = {controlsLength, controlsItem, nullptr};

// [LegacyUnenumerableNamedProperties], as HTMLCollection's.
constexpr JsNamed kControlsNamed = {controlsNames, controlsNamed, nullptr, nullptr, JsNamedFlag_Unenumerable};

// namedItem, which returns null where the named getter returns undefined.
JSValue controlsNamedItem(JSContext* ctx, JSValueConst self, int argc, JSValueConst* argv) {
    PrDomHtmlCollection* collection = jsThis<PrDomHtmlCollection>(ctx, self, htmlFormControlsCollectionClass);
    StringArgument name = {ctx};
    if (collection == nullptr || !jsArgCount(ctx, argc, 1) || !name.read(argv[0]))
        return JS_EXCEPTION;
    JSValue value = controlsNamed(ctx, self, collection, name.value());
    return JS_IsUndefined(value) ? JS_NULL : value;
}

// ---------------------------------------------------------------------------------------------------------------------
// HTMLFormElement (html/#htmlformelement)

PrHtmlFormElement* PR_NULLABLE formThis(JSContext* ctx, JSValueConst self) {
    return jsThis<PrHtmlFormElement>(ctx, self, htmlFormElementClass);
}

// elements, the same collection at each read.
JSValue formElements(JSContext* ctx, JSValueConst self) {
    PrHtmlFormElement* form = formThis(ctx, self);
    if (form == nullptr)
        return JS_EXCEPTION;
    return jsSameObject(ctx, self, "elements", [&] {
        PrDomHtmlCollection* collection = prHtmlFormElementElements(form);
        JSValue object = jsWrapAs(ctx, htmlFormControlsCollectionClass, self, collection, true);
        if (JS_IsException(object))
            prDomHtmlCollectionDestroy(collection);
        return object;
    });
}

uint32_t formLength(void* handle) {
    PrDomHtmlCollection* collection = prHtmlFormElementElements(static_cast<PrHtmlFormElement*>(handle));
    uint32_t length = prDomHtmlCollectionLength(collection);
    prDomHtmlCollectionDestroy(collection);
    return length;
}

JSValue formItem(JSContext* ctx, JSValueConst self, void* handle, uint32_t index) {
    PrDomHtmlCollection* collection = prHtmlFormElementElements(static_cast<PrHtmlFormElement*>(handle));
    PrDomElement* element = prDomHtmlCollectionElementAt(collection, index);
    prDomHtmlCollectionDestroy(collection);
    return jsWrapAs(ctx, prJsDomElementClass, self, element, false);
}

JSValue formLengthGetter(JSContext* ctx, JSValueConst self) {
    PrHtmlFormElement* form = formThis(ctx, self);
    return form != nullptr ? JS_NewUint32(ctx, formLength(form)) : JS_EXCEPTION;
}

bool formNames(JSContext* ctx, void* handle, Array<JSAtom>* out) {
    PrHtmlFormElement* form = static_cast<PrHtmlFormElement*>(handle);
    Array<PrString> names;
    names.reserve(prHtmlFormElementNames(form, nullptr, 0));
    names.count = prHtmlFormElementNames(form, names.data, names.capacity);
    bool ok = true;
    for (uint32_t i = 0; i < names.count; ++i) {
        PrStringView name = prStringView(&names.data[i]);
        JSAtom atom = ok ? JS_NewAtomLen(ctx, name.data, name.size) : JS_ATOM_NULL;
        ok = ok && atom != JS_ATOM_NULL;
        if (atom != JS_ATOM_NULL)
            out->push(atom);
        prStringReset(&names.data[i]);
    }
    names.reset();
    return ok;
}

// html/#dom-form-nameditem: the one element of the name, or a RadioNodeList of several, or undefined.
JSValue formNamed(JSContext* ctx, JSValueConst self, void* handle, PrStringView name) {
    PrHtmlFormElement* form = static_cast<PrHtmlFormElement*>(handle);
    PrDomElement* element = nullptr;
    uint32_t count = prHtmlFormElementNamedItem(form, name, &element);
    if (count == 0)
        return JS_UNDEFINED;
    if (count == 1)
        return jsWrapAs(ctx, prJsDomElementClass, self, element, false);
    PrDomNodeList* list = prHtmlFormElementRadioNodeList(form, name, false);
    if (prDomNodeListLength(list) == 0) {
        prDomNodeListDestroy(list);
        list = prHtmlFormElementRadioNodeList(form, name, true);
    }
    JSValue object = jsWrapAs(ctx, htmlRadioNodeListClass, self, list, true);
    if (JS_IsException(object))
        prDomNodeListDestroy(list);
    return object;
}

constexpr JsIndexed kFormIndexed = {formLength, formItem, nullptr};

// [LegacyOverrideBuiltIns] and [LegacyUnenumerableNamedProperties].
constexpr JsNamed kFormNamed = {formNames, formNamed, nullptr, nullptr,
                                JsNamedFlag_OverrideBuiltIns | JsNamedFlag_Unenumerable};

// relList, the same DOMTokenList of the rel attribute at each read.
JSValue formRelList(JSContext* ctx, JSValueConst self) {
    PrHtmlFormElement* form = formThis(ctx, self);
    if (form == nullptr)
        return JS_EXCEPTION;
    return jsSameObject(ctx, self, "relList", [&] {
        PrDomTokenList* list = prDomTokenListCreate(prHtmlFormElementAsElement(form), "rel");
        JSValue object = jsWrapAs(ctx, prJsDomTokenListClass, self, list, true);
        if (JS_IsException(object))
            prDomTokenListDestroy(list);
        return object;
    });
}

// ---------------------------------------------------------------------------------------------------------------------
// HTMLInputElement (html/#htmlinputelement)

PrHtmlInputElement* PR_NULLABLE inputThis(JSContext* ctx, JSValueConst self) {
    return jsThis<PrHtmlInputElement>(ctx, self, htmlInputElementClass);
}

// type, which reflects its attribute limited to the known values. Its setter is a DOMString attribute's setter.
JSValue inputType(JSContext* ctx, JSValueConst self) {
    PrHtmlInputElement* input = inputThis(ctx, self);
    if (input == nullptr)
        return JS_EXCEPTION;
    return jsFixedString(ctx, prDomInputTypeKeyword(prHtmlInputElementType(input)));
}

// value, whose setter treats null as the empty string ([LegacyNullToEmptyString]).
JSValue setInputValue(JSContext* ctx, JSValueConst self, JSValueConst input) {
    PrHtmlInputElement* element = inputThis(ctx, self);
    StringArgument value = {ctx};
    if (element == nullptr || !value.readNullable(input))
        return JS_EXCEPTION;
    return prHtmlInputElementSetValue(element, value.value()) == PrStatus_Ok
               ? JS_UNDEFINED
               : jsDomExceptionThrow(ctx, "InvalidStateError");
}

JSValue setInputChecked(JSContext* ctx, JSValueConst self, JSValueConst input) {
    PrHtmlInputElement* element = inputThis(ctx, self);
    if (element == nullptr)
        return JS_EXCEPTION;
    prHtmlInputElementSetChecked(element, JS_ToBool(ctx, input));
    return JS_UNDEFINED;
}

JSValue setInputIndeterminate(JSContext* ctx, JSValueConst self, JSValueConst input) {
    PrHtmlInputElement* element = inputThis(ctx, self);
    if (element == nullptr)
        return JS_EXCEPTION;
    prHtmlInputElementSetIndeterminate(element, JS_ToBool(ctx, input));
    return JS_UNDEFINED;
}

JSValue inputValueAsNumber(JSContext* ctx, JSValueConst self) {
    PrHtmlInputElement* element = inputThis(ctx, self);
    return element != nullptr ? JS_NewFloat64(ctx, prHtmlInputElementValueAsNumber(element)) : JS_EXCEPTION;
}

JSValue setInputValueAsNumber(JSContext* ctx, JSValueConst self, JSValueConst input) {
    PrHtmlInputElement* element = inputThis(ctx, self);
    double value = 0;
    if (element == nullptr || JS_ToFloat64(ctx, &value, input) < 0)
        return JS_EXCEPTION;
    switch (prHtmlInputElementSetValueAsNumber(element, value)) {
        case PrStatus_Ok: return JS_UNDEFINED;
        case PrStatus_WrongType: return JS_ThrowTypeError(ctx, "The value is infinite");
        default: return jsDomExceptionThrow(ctx, "InvalidStateError");
    }
}

// valueAsDate, a Date or null.
JSValue inputValueAsDate(JSContext* ctx, JSValueConst self) {
    PrHtmlInputElement* element = inputThis(ctx, self);
    if (element == nullptr)
        return JS_EXCEPTION;
    double time = 0;
    return prHtmlInputElementValueAsDate(element, &time) ? JS_NewDate(ctx, time) : JS_NULL;
}

// The setter takes null or a Date, and an invalid Date or null empties the value.
JSValue setInputValueAsDate(JSContext* ctx, JSValueConst self, JSValueConst input) {
    PrHtmlInputElement* element = inputThis(ctx, self);
    if (element == nullptr)
        return JS_EXCEPTION;
    double time = kNanF64;
    if (!JS_IsNull(input)) {
        if (!JS_IsDate(input))
            return JS_ThrowTypeError(ctx, "The value is not a Date");
        if (JS_ToFloat64(ctx, &time, input) < 0)
            return JS_EXCEPTION;
    }
    return prHtmlInputElementSetValueAsDate(element, time) == PrStatus_Ok
               ? JS_UNDEFINED
               : jsDomExceptionThrow(ctx, "InvalidStateError");
}

// stepUp and stepDown, selected by the magic, which take an optional long that defaults to 1.
JSValue inputStep(JSContext* ctx, JSValueConst self, int argc, JSValueConst* argv, int magic) {
    PrHtmlInputElement* element = inputThis(ctx, self);
    int32_t n = 1;
    if (element == nullptr || (jsArgGiven(argc, argv, 0) && JS_ToInt32(ctx, &n, argv[0]) < 0))
        return JS_EXCEPTION;
    PrStatus status = magic == 0 ? prHtmlInputElementStepUp(element, n) : prHtmlInputElementStepDown(element, n);
    return status == PrStatus_Ok ? JS_UNDEFINED : jsDomExceptionThrow(ctx, "InvalidStateError");
}

JSValue inputList(JSContext* ctx, JSValueConst self) {
    PrHtmlInputElement* element = inputThis(ctx, self);
    return element != nullptr ? jsWrapAs(ctx, prJsDomElementClass, JS_UNDEFINED, prHtmlInputElementList(element), false)
                              : JS_EXCEPTION;
}

// files, which is null until the File API comes, and whose setter ignores null.
JSValue inputFiles(JSContext* ctx, JSValueConst self) {
    return inputThis(ctx, self) != nullptr ? JS_NULL : JS_EXCEPTION;
}

JSValue setInputFiles(JSContext* ctx, JSValueConst self, JSValueConst) {
    return inputThis(ctx, self) != nullptr ? JS_UNDEFINED : JS_EXCEPTION;
}

// ---------------------------------------------------------------------------------------------------------------------
// HTMLButtonElement (html/#htmlbuttonelement)

JSValue buttonType(JSContext* ctx, JSValueConst self) {
    PrHtmlButtonElement* button = jsThis<PrHtmlButtonElement>(ctx, self, htmlButtonElementClass);
    return button != nullptr ? jsFixedString(ctx, prHtmlButtonElementType(button)) : JS_EXCEPTION;
}

// ---------------------------------------------------------------------------------------------------------------------
// HTMLFieldSetElement (html/#htmlfieldsetelement) and HTMLLabelElement (html/#htmllabelelement)

JSValue fieldSetElements(JSContext* ctx, JSValueConst self) {
    PrHtmlFieldSetElement* fieldset = jsThis<PrHtmlFieldSetElement>(ctx, self, htmlFieldSetElementClass);
    if (fieldset == nullptr)
        return JS_EXCEPTION;
    return jsSameObject(ctx, self, "elements", [&] {
        PrDomHtmlCollection* collection = prHtmlFieldSetElementElements(fieldset);
        JSValue object = jsWrapAs(ctx, prJsDomHtmlCollectionClass, self, collection, true);
        if (JS_IsException(object))
            prDomHtmlCollectionDestroy(collection);
        return object;
    });
}

JSValue labelControl(JSContext* ctx, JSValueConst self) {
    PrHtmlLabelElement* label = jsThis<PrHtmlLabelElement>(ctx, self, htmlLabelElementClass);
    return label != nullptr ? jsWrapAs(ctx, prJsDomElementClass, JS_UNDEFINED, prHtmlLabelElementControl(label), false)
                            : JS_EXCEPTION;
}

// form, the form owner of the labeled control (html/#dom-label-form).
JSValue labelForm(JSContext* ctx, JSValueConst self) {
    PrHtmlLabelElement* label = jsThis<PrHtmlLabelElement>(ctx, self, htmlLabelElementClass);
    if (label == nullptr)
        return JS_EXCEPTION;
    PrDomElement* control = prHtmlLabelElementControl(label);
    PrHtmlElement* element = control != nullptr ? prHtmlElementQuery(prDomElementAsEventTarget(control)) : nullptr;
    PrHtmlFormElement* form = element != nullptr ? prHtmlElementForm(element) : nullptr;
    PrDomElement* wrapped = form != nullptr ? prHtmlFormElementAsElement(form) : nullptr;
    return jsWrapAs(ctx, prJsDomElementClass, JS_UNDEFINED, wrapped, false);
}

// ---------------------------------------------------------------------------------------------------------------------
// HTMLOutputElement (html/#htmloutputelement)

// htmlFor, the same DOMTokenList of the for attribute at each read. Setting it sets the list's value ([PutForwards]).
JSValue outputFor(JSContext* ctx, JSValueConst self) {
    PrHtmlOutputElement* output = jsThis<PrHtmlOutputElement>(ctx, self, htmlOutputElementClass);
    if (output == nullptr)
        return JS_EXCEPTION;
    return jsSameObject(ctx, self, "htmlFor", [&] {
        PrDomTokenList* list = prDomTokenListCreate(prHtmlOutputElementAsElement(output), "for");
        JSValue object = jsWrapAs(ctx, prJsDomTokenListClass, self, list, true);
        if (JS_IsException(object))
            prDomTokenListDestroy(list);
        return object;
    });
}

// ---------------------------------------------------------------------------------------------------------------------
// HTMLProgressElement (html/#htmlprogresselement) and HTMLMeterElement (html/#htmlmeterelement)

// The progress element's value, which is zero while it is indeterminate.
JSValue progressValue(JSContext* ctx, JSValueConst self) {
    PrHtmlProgressElement* progress = jsThis<PrHtmlProgressElement>(ctx, self, htmlProgressElementClass);
    if (progress == nullptr)
        return JS_EXCEPTION;
    return JS_NewFloat64(ctx, prHtmlProgressElementPosition(progress) < 0 ? 0 : prHtmlProgressElementValue(progress));
}

// One of the meter's values.
template <double PrHtmlMeterValues::* Value>
JSValue meterValue(JSContext* ctx, JSValueConst self) {
    PrHtmlMeterElement* meter = jsThis<PrHtmlMeterElement>(ctx, self, htmlMeterElementClass);
    if (meter == nullptr)
        return JS_EXCEPTION;
    PrHtmlMeterValues values = prHtmlMeterElementValues(meter);
    return JS_NewFloat64(ctx, values.*Value);
}

// The setter of a double attribute of a progress or a meter element, which throws a TypeError for a value that is not
// finite.
template <JsLiteral Name>
JSValue setReflectedDouble(JSContext* ctx, JSValueConst self, JSValueConst input) {
    PrHtmlElement* element = htmlThis(ctx, self);
    double value = 0;
    if (element == nullptr || JS_ToFloat64(ctx, &value, input) < 0)
        return JS_EXCEPTION;
    return prHtmlElementSetReflectedDouble(element, Name.text, value) == PrStatus_Ok
               ? JS_UNDEFINED
               : JS_ThrowTypeError(ctx, "The value is not finite");
}

// ---------------------------------------------------------------------------------------------------------------------
// FormData (xhr/#interface-formdata)

PrHtmlFormData* PR_NULLABLE formDataThis(JSContext* ctx, JSValueConst self) {
    return jsThis<PrHtmlFormData>(ctx, self, htmlFormDataClass);
}

void formDataRetain(void* handle) {
    prHtmlFormDataRetain(static_cast<PrHtmlFormData*>(handle));
}

void formDataRelease(void* handle) {
    prHtmlFormDataRelease(static_cast<PrHtmlFormData*>(handle));
}

constexpr JsReferences kFormDataReferences = {formDataRetain, formDataRelease, false};

// xhr/#dom-formdata: constructor(optional HTMLFormElement form, optional HTMLElement? submitter = null).
JSValue formDataConstruct(JSContext* ctx, JSValueConst newTarget, int argc, JSValueConst* argv) {
    PrHtmlFormElement* form = nullptr;
    PrHtmlElement* submitter = nullptr;
    if (jsArgGiven(argc, argv, 0)) {
        form = jsHandleAs<PrHtmlFormElement>(ctx, argv[0], htmlFormElementClass);
        if (form == nullptr)
            return JS_ThrowTypeError(ctx, "The form is not an HTMLFormElement");
    }
    if (jsArgGiven(argc, argv, 1) && !JS_IsNull(argv[1])) {
        submitter = jsHandleAs<PrHtmlElement>(ctx, argv[1], htmlElementClass);
        if (submitter == nullptr)
            return JS_ThrowTypeError(ctx, "The submitter is not an HTMLElement");
    }
    PrHtmlFormData* data = nullptr;
    switch (prHtmlFormDataCreate(form, submitter, &data)) {
        case PrStatus_Ok: break;
        case PrStatus_WrongType: return JS_ThrowTypeError(ctx, "The submitter is not a submit button");
        case PrStatus_NotFound: return jsDomExceptionThrow(ctx, "NotFoundError");
        default: return jsDomExceptionThrow(ctx, "InvalidStateError");
    }
    JSValue object = jsWrap(ctx, htmlFormDataClass, JS_UNDEFINED, data, true);
    if (JS_IsException(object))
        prHtmlFormDataRelease(data);
    return jsConstructed(ctx, newTarget, object);
}

enum {
    FormDataMethod_Append,
    FormDataMethod_Delete,
    FormDataMethod_Get,
    FormDataMethod_GetAll,
    FormDataMethod_Has,
    FormDataMethod_Set,
};
typedef uint8_t FormDataMethod;

// The methods that take a name, selected by the magic. The value of an entry is a string.
JSValue formDataMethod(JSContext* ctx, JSValueConst self, int argc, JSValueConst* argv, int magic) {
    PrHtmlFormData* data = formDataThis(ctx, self);
    bool pair = magic == FormDataMethod_Append || magic == FormDataMethod_Set;
    StringArgument name = {ctx};
    StringArgument value = {ctx};
    if (data == nullptr || !jsArgCount(ctx, argc, pair ? 2 : 1) || !name.read(argv[0])
        || (pair && !value.read(argv[1])))
        return JS_EXCEPTION;
    switch (FormDataMethod(magic)) {
        case FormDataMethod_Append: prHtmlFormDataAppend(data, name.value(), value.value()); break;
        case FormDataMethod_Delete: prHtmlFormDataDelete(data, name.value()); break;
        case FormDataMethod_Get: {
            return jsNewStringView(ctx, prHtmlFormDataGet(data, name.value()));
        }
        case FormDataMethod_GetAll: {
            Array<PrStringView> values;
            values.reserve(prHtmlFormDataGetAll(data, name.value(), nullptr, 0));
            values.count = prHtmlFormDataGetAll(data, name.value(), values.data, values.capacity);
            JSValue all = JS_NewArray(ctx);
            for (uint32_t i = 0; i < values.count; ++i)
                JS_SetPropertyUint32(ctx, all, i, jsNewStringView(ctx, values.data[i]));
            values.reset();
            return all;
        }
        case FormDataMethod_Has: return JS_NewBool(ctx, prHtmlFormDataHas(data, name.value()));
        case FormDataMethod_Set: prHtmlFormDataSet(data, name.value(), value.value()); break;
    }
    return JS_UNDEFINED;
}

// Returns the [name, value] pair at the index argv[0], or undefined past the end.
JSValue formDataEntryAt(JSContext* ctx, JSValueConst self, int, JSValueConst* argv) {
    PrHtmlFormData* data = formDataThis(ctx, self);
    uint32_t index = 0;
    if (data == nullptr || JS_ToUint32(ctx, &index, argv[0]) < 0)
        return JS_EXCEPTION;
    PrStringView name = {};
    PrStringView value = {};
    if (!prHtmlFormDataEntry(data, index, &name, &value))
        return JS_UNDEFINED;
    return jsEntry(ctx, jsNewStringView(ctx, name), jsNewStringView(ctx, value));
}

// entries, keys and values, selected by the part of each entry that the magic names.
JSValue formDataIterate(JSContext* ctx, JSValueConst self, int, JSValueConst*, int magic) {
    if (formDataThis(ctx, self) == nullptr)
        return JS_EXCEPTION;
    return jsPairIterator(ctx, htmlFormDataIteratorClass, self, formDataEntryAt, JsMapPart(magic));
}

JSValue formDataForEach(JSContext* ctx, JSValueConst self, int argc, JSValueConst* argv) {
    if (formDataThis(ctx, self) == nullptr)
        return JS_EXCEPTION;
    return jsPairForEach(ctx, self, formDataEntryAt, argc, argv);
}

// ---------------------------------------------------------------------------------------------------------------------
// SubmitEvent (html/#submitevent), FormDataEvent (html/#formdataevent), ToggleEvent (html/#toggleevent) and
// CommandEvent (html/#commandevent)

// Reads the nullable element member `name` of a dictionary, which must be of class `id` or derived from it.
template <typename Handle>
bool readElementMember(JSContext* ctx, JSValueConst dictionary, const char* name, JSClassID id,
                       Handle* PR_NULLABLE* PR_NONNULL out) {
    JSValue value = JS_GetPropertyStr(ctx, dictionary, name);
    if (JS_IsException(value))
        return false;
    bool ok = true;
    if (!JS_IsUndefined(value) && !JS_IsNull(value)) {
        *out = jsHandleAs<Handle>(ctx, value, id);
        if (*out == nullptr) {
            JS_ThrowTypeError(ctx, "The %s member is not of its interface", name);
            ok = false;
        }
    }
    JS_FreeValue(ctx, value);
    return ok;
}

// Reads the event's type and its init dictionary, and the EventInit members of the dictionary if it is present.
bool readEventArguments(JSContext* ctx, int argc, JSValueConst* argv, StringArgument* type, PrDomEventInit* init,
                        bool* present) {
    JSValueConst dictionary = argc > 1 ? argv[1] : JS_UNDEFINED;
    return jsArgCount(ctx, argc, 1) && type->read(argv[0]) && jsDictionary(ctx, dictionary, present)
           && (!*present || prJsDomEventInitRead(ctx, dictionary, init));
}

template <typename Event>
JSValue wrapEvent(JSContext* ctx, JSValueConst newTarget, JSClassID id, Event* event) {
    JSValue object = jsWrap(ctx, id, JS_UNDEFINED, event, true);
    if (JS_IsException(object))
        prDomEventRelease(reinterpret_cast<PrDomEvent*>(event));
    return jsConstructed(ctx, newTarget, object);
}

// SubmitEventInit, after EventInit: submitter.
JSValue submitConstruct(JSContext* ctx, JSValueConst newTarget, int argc, JSValueConst* argv) {
    StringArgument type = {ctx};
    PrDomEventInit init = prDomEventInitDefault();
    bool present = false;
    PrHtmlElement* submitter = nullptr;
    if (!readEventArguments(ctx, argc, argv, &type, &init, &present)
        || (present && !readElementMember(ctx, argv[1], "submitter", htmlElementClass, &submitter)))
        return JS_EXCEPTION;
    return wrapEvent(ctx, newTarget, htmlSubmitEventClass, prHtmlSubmitEventCreate(type.value(), &init, submitter));
}

// FormDataEventInit, after EventInit: formData, which is required.
JSValue formDataEventConstruct(JSContext* ctx, JSValueConst newTarget, int argc, JSValueConst* argv) {
    StringArgument type = {ctx};
    PrDomEventInit init = prDomEventInitDefault();
    bool present = false;
    PrHtmlFormData* data = nullptr;
    if (!readEventArguments(ctx, argc, argv, &type, &init, &present)
        || (present && !readElementMember(ctx, argv[1], "formData", htmlFormDataClass, &data)))
        return JS_EXCEPTION;
    if (data == nullptr)
        return JS_ThrowTypeError(ctx, "The formData member is required");
    return wrapEvent(ctx, newTarget, htmlFormDataEventClass, prHtmlFormDataEventCreate(type.value(), &init, data));
}

// ToggleEventInit, after EventInit: newState, oldState and source.
JSValue toggleConstruct(JSContext* ctx, JSValueConst newTarget, int argc, JSValueConst* argv) {
    StringArgument type = {ctx};
    StringArgument newState = {ctx, ""};
    StringArgument oldState = {ctx, ""};
    PrDomEventInit init = prDomEventInitDefault();
    bool present = false;
    PrDomElement* source = nullptr;
    if (!readEventArguments(ctx, argc, argv, &type, &init, &present)
        || (present
            && (!jsDictString(ctx, argv[1], "newState", &newState) || !jsDictString(ctx, argv[1], "oldState", &oldState)
                || !readElementMember(ctx, argv[1], "source", prJsDomElementClass, &source))))
        return JS_EXCEPTION;
    return wrapEvent(ctx, newTarget, htmlToggleEventClass,
                     prHtmlToggleEventCreate(type.value(), &init, oldState.value(), newState.value(), source));
}

// CommandEventInit, after EventInit: command and source.
JSValue commandConstruct(JSContext* ctx, JSValueConst newTarget, int argc, JSValueConst* argv) {
    StringArgument type = {ctx};
    StringArgument command = {ctx, ""};
    PrDomEventInit init = prDomEventInitDefault();
    bool present = false;
    PrDomElement* source = nullptr;
    if (!readEventArguments(ctx, argc, argv, &type, &init, &present)
        || (present
            && (!jsDictString(ctx, argv[1], "command", &command)
                || !readElementMember(ctx, argv[1], "source", prJsDomElementClass, &source))))
        return JS_EXCEPTION;
    return wrapEvent(ctx, newTarget, htmlCommandEventClass,
                     prHtmlCommandEventCreate(type.value(), &init, command.value(), source));
}

JSValue submitter(JSContext* ctx, JSValueConst self) {
    PrHtmlSubmitEvent* event = jsThis<PrHtmlSubmitEvent>(ctx, self, htmlSubmitEventClass);
    if (event == nullptr)
        return JS_EXCEPTION;
    PrHtmlElement* element = prHtmlSubmitEventSubmitter(event);
    PrDomElement* wrapped = element != nullptr ? prHtmlElementAsElement(element) : nullptr;
    return jsWrapAs(ctx, prJsDomElementClass, JS_UNDEFINED, wrapped, false);
}

// formData, the same object at each read.
JSValue eventFormData(JSContext* ctx, JSValueConst self) {
    PrHtmlFormDataEvent* event = jsThis<PrHtmlFormDataEvent>(ctx, self, htmlFormDataEventClass);
    if (event == nullptr)
        return JS_EXCEPTION;
    return jsSameObject(ctx, self, "formData",
                        [&] { return jsWrap(ctx, htmlFormDataClass, JS_UNDEFINED, prHtmlFormDataEventFormData(event),
                                            false); });
}

// source, of a ToggleEvent or a CommandEvent, selected by the magic.
JSValue eventSource(JSContext* ctx, JSValueConst self, int magic) {
    PrDomElement* source = nullptr;
    if (magic == 0) {
        PrHtmlToggleEvent* event = jsThis<PrHtmlToggleEvent>(ctx, self, htmlToggleEventClass);
        if (event == nullptr)
            return JS_EXCEPTION;
        source = prHtmlToggleEventSource(event);
    } else {
        PrHtmlCommandEvent* event = jsThis<PrHtmlCommandEvent>(ctx, self, htmlCommandEventClass);
        if (event == nullptr)
            return JS_EXCEPTION;
        source = prHtmlCommandEventSource(event);
    }
    return jsWrapAs(ctx, prJsDomElementClass, JS_UNDEFINED, source, false);
}

// ---------------------------------------------------------------------------------------------------------------------
// Classes

// The members of every listed element (html/#category-listed) beside its disabled and name.
#define PR_FORM_ASSOCIATED PR_JS_GET("form", elementForm)
#define PR_LABELABLE PR_JS_GET("labels", elementLabels)

constexpr JSCFunctionListEntry kFormElementProto[] = {
    PR_JS_TAG("HTMLFormElement"),
    PR_REFLECT_STRING("acceptCharset", "accept-charset"),
    PR_JS_GET_SET("action", formActionUrl<"action">, jsSetReflectString<"action">),
    PR_REFLECT_ENUMERATED("autocomplete", "autocomplete", &kAutocomplete),
    PR_REFLECT_ENUMERATED("enctype", "enctype", &kEnctype),
    PR_REFLECT_ENUMERATED("encoding", "enctype", &kEnctype),
    PR_REFLECT_ENUMERATED("method", "method", &kMethod),
    PR_REFLECT_STRING("name", "name"),
    PR_REFLECT_BOOL("noValidate", "novalidate"),
    PR_REFLECT_STRING("target", "target"),
    PR_REFLECT_STRING("rel", "rel"),
    PR_JS_GET_SET("relList", formRelList, (jsPutForwards<"relList", "value">)),
    PR_JS_GET("elements", formElements),
    PR_JS_GET("length", formLengthGetter),
};

constexpr JSCFunctionListEntry kFormControlsCollectionProto[] = {
    PR_JS_TAG("HTMLFormControlsCollection"),
    PR_JS_METHOD("namedItem", 1, controlsNamedItem),
};

constexpr JSCFunctionListEntry kRadioNodeListProto[] = {
    PR_JS_TAG("RadioNodeList"),
    PR_JS_GET_SET("value", radioValue, setRadioValue),
};

constexpr JSCFunctionListEntry kInputElementProto[] = {
    PR_JS_TAG("HTMLInputElement"),
    PR_REFLECT_STRING("accept", "accept"),
    PR_REFLECT_BOOL("alpha", "alpha"),
    PR_REFLECT_STRING("alt", "alt"),
    PR_REFLECT_BOOL("defaultChecked", "checked"),
    PR_JS_GET_SET("checked", (jsBoolGetter<&htmlInputElementClass, prHtmlInputElementChecked>), setInputChecked),
    PR_REFLECT_ENUMERATED("colorSpace", "colorspace", &kColorSpace),
    PR_REFLECT_STRING("dirName", "dirname"),
    PR_REFLECT_BOOL("disabled", "disabled"),
    PR_FORM_ASSOCIATED,
    PR_JS_GET_SET("files", inputFiles, setInputFiles),
    PR_SUBMITTER_ATTRIBUTES,
    PR_JS_GET_SET("height", (jsNumberGetter<&htmlInputElementClass, prHtmlInputElementHeight>),
                  (jsUint32Setter<&htmlInputElementClass, prHtmlInputElementSetHeight>)),
    PR_JS_GET_SET("indeterminate", (jsBoolGetter<&htmlInputElementClass, prHtmlInputElementIndeterminate>),
                  setInputIndeterminate),
    PR_JS_GET("list", inputList),
    PR_REFLECT_STRING("max", "max"),
    PR_REFLECT_INTEGER("maxLength", "maxlength", PrHtmlReflectedInteger_NonNegative, -1),
    PR_REFLECT_STRING("min", "min"),
    PR_REFLECT_INTEGER("minLength", "minlength", PrHtmlReflectedInteger_NonNegative, -1),
    PR_REFLECT_BOOL("multiple", "multiple"),
    PR_REFLECT_STRING("name", "name"),
    PR_REFLECT_STRING("pattern", "pattern"),
    PR_REFLECT_STRING("placeholder", "placeholder"),
    PR_REFLECT_BOOL("readOnly", "readonly"),
    PR_REFLECT_BOOL("required", "required"),
    PR_REFLECT_INTEGER("size", "size", PrHtmlReflectedInteger_Positive, 20),
    PR_REFLECT_URL("src", "src"),
    PR_REFLECT_STRING("step", "step"),
    PR_JS_GET_SET("type", inputType, jsSetReflectString<"type">),
    PR_REFLECT_STRING("defaultValue", "value"),
    PR_JS_GET_SET("value", (jsStringGetter<&htmlInputElementClass, prHtmlInputElementValue>), setInputValue),
    PR_JS_GET_SET("valueAsDate", inputValueAsDate, setInputValueAsDate),
    PR_JS_GET_SET("valueAsNumber", inputValueAsNumber, setInputValueAsNumber),
    PR_JS_GET_SET("width", (jsNumberGetter<&htmlInputElementClass, prHtmlInputElementWidth>),
                  (jsUint32Setter<&htmlInputElementClass, prHtmlInputElementSetWidth>)),
    PR_JS_MAGIC("stepUp", 0, inputStep, 0),
    PR_JS_MAGIC("stepDown", 0, inputStep, 1),
    PR_LABELABLE,
    PR_REFLECT_STRING("align", "align"),
    PR_REFLECT_STRING("useMap", "usemap"),
};

constexpr JSCFunctionListEntry kButtonElementProto[] = {
    PR_JS_TAG("HTMLButtonElement"),
    PR_REFLECT_BOOL("disabled", "disabled"),
    PR_FORM_ASSOCIATED,
    PR_SUBMITTER_ATTRIBUTES,
    PR_REFLECT_STRING("name", "name"),
    PR_JS_GET_SET("type", buttonType, jsSetReflectString<"type">),
    PR_REFLECT_STRING("value", "value"),
    PR_LABELABLE,
};

constexpr JSCFunctionListEntry kTextAreaElementProto[] = {
    PR_JS_TAG("HTMLTextAreaElement"),
    PR_REFLECT_INTEGER("cols", "cols", PrHtmlReflectedInteger_Positive, 20),
    PR_REFLECT_STRING("dirName", "dirname"),
    PR_REFLECT_BOOL("disabled", "disabled"),
    PR_FORM_ASSOCIATED,
    PR_REFLECT_INTEGER("maxLength", "maxlength", PrHtmlReflectedInteger_NonNegative, -1),
    PR_REFLECT_INTEGER("minLength", "minlength", PrHtmlReflectedInteger_NonNegative, -1),
    PR_REFLECT_STRING("name", "name"),
    PR_REFLECT_STRING("placeholder", "placeholder"),
    PR_REFLECT_BOOL("readOnly", "readonly"),
    PR_REFLECT_BOOL("required", "required"),
    PR_REFLECT_INTEGER("rows", "rows", PrHtmlReflectedInteger_Positive, 2),
    PR_REFLECT_STRING("wrap", "wrap"),
    PR_JS_GET("type", fixedType<"textarea">),
    PR_JS_GET_SET("defaultValue",
                  (jsStringGetter<&htmlTextAreaElementClass, prHtmlTextAreaElementDefaultValue>),
                  (jsStringSetter<&htmlTextAreaElementClass, prHtmlTextAreaElementSetDefaultValue>)),
    // value's setter treats null as the empty string ([LegacyNullToEmptyString]).
    PR_JS_GET_SET("value", (jsStringGetter<&htmlTextAreaElementClass, prHtmlTextAreaElementValue>),
                  (jsStringSetter<&htmlTextAreaElementClass, prHtmlTextAreaElementSetValue, true>)),
    PR_JS_GET("textLength", (jsNumberGetter<&htmlTextAreaElementClass, prHtmlTextAreaElementTextLength>)),
    PR_LABELABLE,
};

constexpr JSCFunctionListEntry kFieldSetElementProto[] = {
    PR_JS_TAG("HTMLFieldSetElement"),
    PR_REFLECT_BOOL("disabled", "disabled"),
    PR_FORM_ASSOCIATED,
    PR_REFLECT_STRING("name", "name"),
    PR_JS_GET("type", fixedType<"fieldset">),
    PR_JS_GET("elements", fieldSetElements),
};

constexpr JSCFunctionListEntry kLegendElementProto[] = {
    PR_JS_TAG("HTMLLegendElement"),
    PR_FORM_ASSOCIATED,
    PR_REFLECT_STRING("align", "align"),
};

constexpr JSCFunctionListEntry kLabelElementProto[] = {
    PR_JS_TAG("HTMLLabelElement"),
    PR_JS_GET("form", labelForm),
    PR_REFLECT_STRING("htmlFor", "for"),
    PR_JS_GET("control", labelControl),
};

constexpr JSCFunctionListEntry kOutputElementProto[] = {
    PR_JS_TAG("HTMLOutputElement"),
    PR_JS_GET_SET("htmlFor", outputFor, (jsPutForwards<"htmlFor", "value">)),
    PR_FORM_ASSOCIATED,
    PR_REFLECT_STRING("name", "name"),
    PR_JS_GET("type", fixedType<"output">),
    PR_JS_GET_SET("defaultValue", (jsStringGetter<&htmlOutputElementClass, prHtmlOutputElementDefaultValue>),
                  (jsStringSetter<&htmlOutputElementClass, prHtmlOutputElementSetDefaultValue>)),
    PR_JS_GET_SET("value", (jsStringGetter<&htmlOutputElementClass, prHtmlOutputElementValue>),
                  (jsStringSetter<&htmlOutputElementClass, prHtmlOutputElementSetValue>)),
    PR_LABELABLE,
};

constexpr JSCFunctionListEntry kProgressElementProto[] = {
    PR_JS_TAG("HTMLProgressElement"),
    PR_JS_GET_SET("value", progressValue, setReflectedDouble<"value">),
    PR_JS_GET_SET("max", (jsNumberGetter<&htmlProgressElementClass, prHtmlProgressElementMax>),
                  setReflectedDouble<"max">),
    PR_JS_GET("position", (jsNumberGetter<&htmlProgressElementClass, prHtmlProgressElementPosition>)),
    PR_LABELABLE,
};

constexpr JSCFunctionListEntry kMeterElementProto[] = {
    PR_JS_TAG("HTMLMeterElement"),
    PR_JS_GET_SET("value", meterValue<&PrHtmlMeterValues::value>, setReflectedDouble<"value">),
    PR_JS_GET_SET("min", meterValue<&PrHtmlMeterValues::min>, setReflectedDouble<"min">),
    PR_JS_GET_SET("max", meterValue<&PrHtmlMeterValues::max>, setReflectedDouble<"max">),
    PR_JS_GET_SET("low", meterValue<&PrHtmlMeterValues::low>, setReflectedDouble<"low">),
    PR_JS_GET_SET("high", meterValue<&PrHtmlMeterValues::high>, setReflectedDouble<"high">),
    PR_JS_GET_SET("optimum", meterValue<&PrHtmlMeterValues::optimum>, setReflectedDouble<"optimum">),
    PR_LABELABLE,
};

constexpr JSCFunctionListEntry kDetailsElementProto[] = {
    PR_JS_TAG("HTMLDetailsElement"),
    PR_REFLECT_STRING("name", "name"),
    PR_REFLECT_BOOL("open", "open"),
};

constexpr JSCFunctionListEntry kFormDataProto[] = {
    PR_JS_TAG("FormData"),
    PR_JS_MAGIC("append", 2, formDataMethod, FormDataMethod_Append),
    PR_JS_MAGIC("delete", 1, formDataMethod, FormDataMethod_Delete),
    PR_JS_MAGIC("get", 1, formDataMethod, FormDataMethod_Get),
    PR_JS_MAGIC("getAll", 1, formDataMethod, FormDataMethod_GetAll),
    PR_JS_MAGIC("has", 1, formDataMethod, FormDataMethod_Has),
    PR_JS_MAGIC("set", 2, formDataMethod, FormDataMethod_Set),
    PR_JS_MAGIC("entries", 0, formDataIterate, JsMapPart_Entries),
    PR_JS_MAGIC("keys", 0, formDataIterate, JsMapPart_Keys),
    PR_JS_MAGIC("values", 0, formDataIterate, JsMapPart_Values),
    PR_JS_METHOD("forEach", 1, formDataForEach),
};

constexpr JSCFunctionListEntry kSubmitEventProto[] = {
    PR_JS_TAG("SubmitEvent"),
    PR_JS_GET("submitter", submitter),
};

constexpr JSCFunctionListEntry kFormDataEventProto[] = {
    PR_JS_TAG("FormDataEvent"),
    PR_JS_GET("formData", eventFormData),
};

constexpr JSCFunctionListEntry kToggleEventProto[] = {
    PR_JS_TAG("ToggleEvent"),
    PR_JS_GET("oldState", (jsStringGetter<&htmlToggleEventClass, prHtmlToggleEventOldState>)),
    PR_JS_GET("newState", (jsStringGetter<&htmlToggleEventClass, prHtmlToggleEventNewState>)),
    PR_JS_MAGIC_GET("source", eventSource, 0),
};

constexpr JSCFunctionListEntry kCommandEventProto[] = {
    PR_JS_TAG("CommandEvent"),
    PR_JS_MAGIC_GET("source", eventSource, 1),
    PR_JS_GET("command", (jsStringGetter<&htmlCommandEventClass, prHtmlCommandEventCommand>)),
};

#define PR_ELEMENT_CLASS(interface, id, proto)                                                     \
    {PR_JS_CLASS(interface, id, jsViewFinalizer<&id>, proto), .base = &htmlElementClass}

constexpr JsClassDefinition kClasses[] = {
    {PR_JS_CLASS("HTMLFormElement", htmlFormElementClass, jsViewFinalizer<&htmlFormElementClass>,
                 kFormElementProto),
     .base = &htmlElementClass, .indexed = &kFormIndexed, .named = &kFormNamed},
    {PR_JS_CLASS("HTMLFormControlsCollection", htmlFormControlsCollectionClass,
                 (jsFinalizer<&htmlFormControlsCollectionClass, prDomHtmlCollectionDestroy>),
                 kFormControlsCollectionProto),
     .base = &prJsDomHtmlCollectionClass, .indexed = &kControlsIndexed, .named = &kControlsNamed},
    {PR_JS_CLASS("RadioNodeList", htmlRadioNodeListClass,
                 (jsFinalizer<&htmlRadioNodeListClass, prDomNodeListDestroy>), kRadioNodeListProto),
     .base = &prJsDomNodeListClass, .indexed = &kRadioIndexed, .iterable = true},
    PR_ELEMENT_CLASS("HTMLInputElement", htmlInputElementClass, kInputElementProto),
    PR_ELEMENT_CLASS("HTMLButtonElement", htmlButtonElementClass, kButtonElementProto),
    PR_ELEMENT_CLASS("HTMLTextAreaElement", htmlTextAreaElementClass, kTextAreaElementProto),
    PR_ELEMENT_CLASS("HTMLFieldSetElement", htmlFieldSetElementClass, kFieldSetElementProto),
    PR_ELEMENT_CLASS("HTMLLegendElement", htmlLegendElementClass, kLegendElementProto),
    PR_ELEMENT_CLASS("HTMLLabelElement", htmlLabelElementClass, kLabelElementProto),
    PR_ELEMENT_CLASS("HTMLOutputElement", htmlOutputElementClass, kOutputElementProto),
    PR_ELEMENT_CLASS("HTMLProgressElement", htmlProgressElementClass, kProgressElementProto),
    PR_ELEMENT_CLASS("HTMLMeterElement", htmlMeterElementClass, kMeterElementProto),
    PR_ELEMENT_CLASS("HTMLDetailsElement", htmlDetailsElementClass, kDetailsElementProto),
    {PR_JS_CLASS("FormData", htmlFormDataClass, (jsFinalizer<&htmlFormDataClass, prHtmlFormDataRelease>),
                 kFormDataProto),
     .constructor = formDataConstruct, .iterator = "entries", .references = &kFormDataReferences},
    {PR_JS_CLASS("SubmitEvent", htmlSubmitEventClass, jsViewFinalizer<&htmlSubmitEventClass>, kSubmitEventProto),
     .constructor = submitConstruct, .length = 1, .base = &prJsDomEventClass},
    {PR_JS_CLASS("FormDataEvent", htmlFormDataEventClass, jsViewFinalizer<&htmlFormDataEventClass>,
                 kFormDataEventProto),
     .constructor = formDataEventConstruct, .length = 2, .base = &prJsDomEventClass},
    {PR_JS_CLASS("ToggleEvent", htmlToggleEventClass, jsViewFinalizer<&htmlToggleEventClass>, kToggleEventProto),
     .constructor = toggleConstruct, .length = 1, .base = &prJsDomEventClass},
    {PR_JS_CLASS("CommandEvent", htmlCommandEventClass, jsViewFinalizer<&htmlCommandEventClass>,
                 kCommandEventProto),
     .constructor = commandConstruct, .length = 1, .base = &prJsDomEventClass},
};

} // namespace

void jsHtmlFormsBind(JSContext* ctx) {
    for (const JsClassDefinition& definition : kClasses)
        JS_FreeValue(ctx, jsDefineClass(ctx, definition));
    jsPairIteratorBind(ctx, &htmlFormDataIteratorClass, "FormData Iterator");
}

JSClassID jsHtmlFormsElementClass(PrDomElement* element) {
    PrDomEventTarget* target = prDomElementAsEventTarget(element);
    if (prHtmlFormElementQuery(target) != nullptr)
        return htmlFormElementClass;
    if (prHtmlInputElementQuery(target) != nullptr)
        return htmlInputElementClass;
    if (prHtmlButtonElementQuery(target) != nullptr)
        return htmlButtonElementClass;
    if (prHtmlTextAreaElementQuery(target) != nullptr)
        return htmlTextAreaElementClass;
    if (prHtmlFieldSetElementQuery(target) != nullptr)
        return htmlFieldSetElementClass;
    if (prHtmlLegendElementQuery(target) != nullptr)
        return htmlLegendElementClass;
    if (prHtmlLabelElementQuery(target) != nullptr)
        return htmlLabelElementClass;
    if (prHtmlOutputElementQuery(target) != nullptr)
        return htmlOutputElementClass;
    if (prHtmlProgressElementQuery(target) != nullptr)
        return htmlProgressElementClass;
    if (prHtmlMeterElementQuery(target) != nullptr)
        return htmlMeterElementClass;
    if (prHtmlDetailsElementQuery(target) != nullptr)
        return htmlDetailsElementClass;
    return 0;
}

JSClassID jsHtmlFormsEventClass(PrDomEvent* event) {
    switch (prDomEventKind(event)) {
        case PrDomEventKind_Submit: return htmlSubmitEventClass;
        case PrDomEventKind_FormData: return htmlFormDataEventClass;
        case PrDomEventKind_Toggle: return htmlToggleEventClass;
        case PrDomEventKind_Command: return htmlCommandEventClass;
        default: return 0;
    }
}

} // namespace pr_impl
