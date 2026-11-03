// JavaScript binding of the element interfaces that forms and script interfaces leave: the elements of content, tables
// and select controls. Their behavior is in the C API, and what is here converts arguments and reflects attributes.

#include "html/quickjs/src/html_elements_quickjs.h"

#include "pr/count_of.h"
#include "pr/dom_tag.h"
#include "pr/html_forms.h"
#include "pr/html_table.h"
#include "pr/quickjs_callback.h"

#include "html/quickjs/src/custom_elements_quickjs.h"
#include "html/quickjs/src/html_reflect_quickjs.h"

namespace pr_impl {

namespace {

JSClassID htmlBodyElementClass;
JSClassID htmlBrElementClass;
JSClassID htmlDataElementClass;
JSClassID htmlDataListElementClass;
JSClassID htmlDivElementClass;
JSClassID htmlDListElementClass;
JSClassID htmlEmbedElementClass;
JSClassID htmlFontElementClass;
JSClassID htmlFrameElementClass;
JSClassID htmlFrameSetElementClass;
JSClassID htmlHeadElementClass;
JSClassID htmlHeadingElementClass;
JSClassID htmlHrElementClass;
JSClassID htmlHtmlElementClass;
JSClassID htmlLiElementClass;
JSClassID htmlMapElementClass;
JSClassID htmlMarqueeElementClass;
JSClassID htmlMenuElementClass;
JSClassID htmlModElementClass;
JSClassID htmlObjectElementClass;
JSClassID htmlOListElementClass;
JSClassID htmlOptGroupElementClass;
JSClassID htmlOptionElementClass;
JSClassID htmlOptionsCollectionClass;
JSClassID htmlParagraphElementClass;
JSClassID htmlParamElementClass;
JSClassID htmlPreElementClass;
JSClassID htmlQuoteElementClass;
JSClassID htmlSelectElementClass;
JSClassID htmlSpanElementClass;
JSClassID htmlTableCaptionElementClass;
JSClassID htmlTableCellElementClass;
JSClassID htmlTableColElementClass;
JSClassID htmlTableElementClass;
JSClassID htmlTableRowElementClass;
JSClassID htmlTableSectionElementClass;
JSClassID htmlTimeElementClass;
JSClassID htmlTitleElementClass;
JSClassID htmlTrackElementClass;
JSClassID htmlUListElementClass;
JSClassID htmlUnknownElementClass;

// ---------------------------------------------------------------------------------------------------------------------
// Helpers

JSValue elementToJs(JSContext* ctx, JSValueConst object, PrDomElement* PR_NULLABLE element) {
    return jsWrapAs(ctx, prJsDomElementClass, object, element, false);
}

// Returns the same collection at each read of the [SameObject] attribute `name`, which `create` makes.
template <JsLiteral Name, typename Create>
JSValue sameCollection(JSContext* ctx, JSValueConst ownerObject, JSClassID id, Create create) {
    return jsSameObject(ctx, ownerObject, Name.text, [&] {
        PrDomHtmlCollection* collection = create();
        JSValue object = jsWrapAs(ctx, id, ownerObject, collection, true);
        if (JS_IsException(object))
            prDomHtmlCollectionDestroy(collection);
        return object;
    });
}

// Returns undefined for a call that succeeded, and throws what its status means otherwise.
JSValue statusToJs(JSContext* ctx, PrStatus status) {
    return status == PrStatus_Ok ? JS_UNDEFINED : prJsDomThrow(ctx, status);
}

// Reads an index argument, which is -1 if it is not given, as the insert and delete methods default it.
bool readIndex(JSContext* ctx, int argc, JSValueConst* argv, int32_t* out) {
    *out = -1;
    return !jsArgGiven(argc, argv, 0) || JS_ToInt32(ctx, out, argv[0]) >= 0;
}

// ---------------------------------------------------------------------------------------------------------------------
// HTMLTitleElement (html/#htmltitleelement)

JSValue titleText(JSContext* ctx, JSValueConst elementObject) {
    PrDomElement* element = jsThis<PrDomElement>(ctx, elementObject, htmlTitleElementClass);
    if (element == nullptr)
        return JS_EXCEPTION;
    return jsTakeString(ctx, prDomNodeChildTextContent(prDomElementAsNode(element)));
}

JSValue setTitleText(JSContext* ctx, JSValueConst elementObject, JSValueConst input) {
    PrDomElement* element = jsThis<PrDomElement>(ctx, elementObject, htmlTitleElementClass);
    StringArgument value = {ctx};
    if (element == nullptr || !value.read(input))
        return JS_EXCEPTION;
    prDomNodeSetTextContent(prDomElementAsNode(element), value.value());
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------------------------------------------------
// HTMLMapElement (html/#htmlmapelement) and HTMLDataListElement (html/#htmldatalistelement)

JSValue mapAreas(JSContext* ctx, JSValueConst mapObject) {
    PrHtmlMapElement* map = jsThis<PrHtmlMapElement>(ctx, mapObject, htmlMapElementClass);
    if (map == nullptr)
        return JS_EXCEPTION;
    return sameCollection<"areas">(ctx, mapObject, prJsDomHtmlCollectionClass, [&] { return prHtmlMapElementAreas(map); });
}

JSValue dataListOptions(JSContext* ctx, JSValueConst listObject) {
    PrHtmlDataListElement* list = jsThis<PrHtmlDataListElement>(ctx, listObject, htmlDataListElementClass);
    if (list == nullptr)
        return JS_EXCEPTION;
    return sameCollection<"options">(ctx, listObject, prJsDomHtmlCollectionClass, [&] {
        return prHtmlDataListElementOptions(list);
    });
}

// ---------------------------------------------------------------------------------------------------------------------
// HTMLOptionElement (html/#htmloptionelement)

using OptionText = PrString (*)(const PrHtmlOptionElement*);

template <OptionText Read>
JSValue optionString(JSContext* ctx, JSValueConst optionObject) {
    PrHtmlOptionElement* option = jsThis<PrHtmlOptionElement>(ctx, optionObject, htmlOptionElementClass);
    return option != nullptr ? jsTakeString(ctx, Read(option)) : JS_EXCEPTION;
}

JSValue setOptionText(JSContext* ctx, JSValueConst optionObject, JSValueConst input) {
    PrHtmlOptionElement* option = jsThis<PrHtmlOptionElement>(ctx, optionObject, htmlOptionElementClass);
    StringArgument value = {ctx};
    if (option == nullptr || !value.read(input))
        return JS_EXCEPTION;
    prHtmlOptionElementSetText(option, value.value());
    return JS_UNDEFINED;
}

JSValue optionSelected(JSContext* ctx, JSValueConst optionObject) {
    PrHtmlOptionElement* option = jsThis<PrHtmlOptionElement>(ctx, optionObject, htmlOptionElementClass);
    return option != nullptr ? JS_NewBool(ctx, prHtmlOptionElementSelected(option)) : JS_EXCEPTION;
}

JSValue setOptionSelected(JSContext* ctx, JSValueConst optionObject, JSValueConst input) {
    PrHtmlOptionElement* option = jsThis<PrHtmlOptionElement>(ctx, optionObject, htmlOptionElementClass);
    if (option == nullptr)
        return JS_EXCEPTION;
    prHtmlOptionElementSetSelected(option, JS_ToBool(ctx, input) > 0);
    return JS_UNDEFINED;
}

JSValue optionIndex(JSContext* ctx, JSValueConst optionObject) {
    PrHtmlOptionElement* option = jsThis<PrHtmlOptionElement>(ctx, optionObject, htmlOptionElementClass);
    return option != nullptr ? JS_NewUint32(ctx, prHtmlOptionElementIndex(option)) : JS_EXCEPTION;
}

JSValue optionForm(JSContext* ctx, JSValueConst optionObject) {
    PrHtmlOptionElement* option = jsThis<PrHtmlOptionElement>(ctx, optionObject, htmlOptionElementClass);
    if (option == nullptr)
        return JS_EXCEPTION;
    PrHtmlFormElement* form = prHtmlOptionElementForm(option);
    return elementToJs(ctx, optionObject, form != nullptr ? prHtmlFormElementAsElement(form) : nullptr);
}

// html/#dom-option, the legacy factory function Option(text, value, defaultSelected, selected).
JSValue optionFactory(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    PrDomDocument* document = prJsDomGlobalDocument(ctx);
    if (document == nullptr)
        return JS_ThrowTypeError(ctx, "Illegal constructor");
    StringArgument text = {ctx};
    StringArgument value = {ctx};
    PrHtmlOptionDesc desc{};
    if (jsArgGiven(argc, argv, 0)) {
        if (!text.read(argv[0]))
            return JS_EXCEPTION;
        desc.text = text.value();
    }
    if (jsArgGiven(argc, argv, 1)) {
        if (!value.read(argv[1]))
            return JS_EXCEPTION;
        desc.value = value.value();
    }
    desc.defaultSelected = argc > 2 && JS_ToBool(ctx, argv[2]) > 0;
    desc.selected = argc > 3 && JS_ToBool(ctx, argv[3]) > 0;
    PrHtmlOptionElement* option = prHtmlOptionElementCreate(document, &desc);
    // The object replaces the reference that the option was created with.
    JSValue object = jsWrapAs(ctx, prJsDomElementClass, JS_UNDEFINED, prHtmlOptionElementAsElement(option), false);
    prDomNodeRelease(prHtmlOptionElementAsNode(option));
    return object;
}

// ---------------------------------------------------------------------------------------------------------------------
// HTMLSelectElement (html/#htmlselectelement) and HTMLOptionsCollection (html/#htmloptionscollection)

// Returns the select of an options collection, whose root it is.
PrHtmlSelectElement* collectionSelect(void* handle) {
    PrDomNode* root = prDomHtmlCollectionRoot(static_cast<PrDomHtmlCollection*>(handle));
    return prHtmlSelectElementQuery(prDomNodeAsEventTarget(root));
}

PrHtmlSelectElement* PR_NULLABLE selectThis(JSContext* ctx, JSValueConst object) {
    return jsThis<PrHtmlSelectElement>(ctx, object, htmlSelectElementClass);
}

PrHtmlSelectElement* PR_NULLABLE optionsThis(JSContext* ctx, JSValueConst collectionObject) {
    PrDomHtmlCollection* collection = jsThis<PrDomHtmlCollection>(ctx, collectionObject, htmlOptionsCollectionClass);
    return collection != nullptr ? collectionSelect(collection) : nullptr;
}

// The members that a select and its options collection share take the select from either, as `This` gives it.
using SelectThis = PrHtmlSelectElement* PR_NULLABLE (*)(JSContext*, JSValueConst);

template <SelectThis This>
JSValue selectLength(JSContext* ctx, JSValueConst selectObject) {
    PrHtmlSelectElement* select = This(ctx, selectObject);
    return select != nullptr ? JS_NewUint32(ctx, prHtmlSelectElementLength(select)) : JS_EXCEPTION;
}

template <SelectThis This>
JSValue setSelectLength(JSContext* ctx, JSValueConst selectObject, JSValueConst input) {
    PrHtmlSelectElement* select = This(ctx, selectObject);
    uint32_t length = 0;
    if (select == nullptr || JS_ToUint32(ctx, &length, input) < 0)
        return JS_EXCEPTION;
    prHtmlSelectElementSetLength(select, length);
    return JS_UNDEFINED;
}

template <SelectThis This>
JSValue selectedIndex(JSContext* ctx, JSValueConst selectObject) {
    PrHtmlSelectElement* select = This(ctx, selectObject);
    return select != nullptr ? JS_NewInt32(ctx, prHtmlSelectElementSelectedIndex(select)) : JS_EXCEPTION;
}

template <SelectThis This>
JSValue setSelectedIndex(JSContext* ctx, JSValueConst selectObject, JSValueConst input) {
    PrHtmlSelectElement* select = This(ctx, selectObject);
    int32_t index = 0;
    if (select == nullptr || JS_ToInt32(ctx, &index, input) < 0)
        return JS_EXCEPTION;
    prHtmlSelectElementSetSelectedIndex(select, index);
    return JS_UNDEFINED;
}

// add(element, before), whose element is an option or an optgroup, and whose before is an element, an index or null.
template <SelectThis This>
JSValue selectAdd(JSContext* ctx, JSValueConst selectObject, int argc, JSValueConst* argv) {
    PrHtmlSelectElement* select = This(ctx, selectObject);
    if (select == nullptr || !jsArgCount(ctx, argc, 1))
        return JS_EXCEPTION;
    PrDomElement* given = jsThis<PrDomElement>(ctx, argv[0], htmlElementClass);
    if (given == nullptr)
        return JS_EXCEPTION;
    PrDomTag tag = prDomElementTag(given);
    if (tag != PrDomTag_Option && tag != PrDomTag_Optgroup)
        return JS_ThrowTypeError(ctx, "Expected an option or an optgroup");
    PrHtmlElement* element = prHtmlElementQuery(prDomElementAsEventTarget(given));
    JSValueConst before = argc > 1 ? argv[1] : JS_NULL;
    if (JS_IsNull(before) || JS_IsUndefined(before))
        return statusToJs(ctx, prHtmlSelectElementAdd(select, element, nullptr));
    if (JS_IsObject(before)) {
        PrDomElement* reference = jsThis<PrDomElement>(ctx, before, htmlElementClass);
        if (reference == nullptr)
            return JS_EXCEPTION;
        PrHtmlElement* html = prHtmlElementQuery(prDomElementAsEventTarget(reference));
        return statusToJs(ctx, prHtmlSelectElementAdd(select, element, html));
    }
    int32_t index = 0;
    if (JS_ToInt32(ctx, &index, before) < 0)
        return JS_EXCEPTION;
    if (index < 0)
        return statusToJs(ctx, prHtmlSelectElementAdd(select, element, nullptr));
    return statusToJs(ctx, prHtmlSelectElementAddAt(select, element, uint32_t(index)));
}

// remove(index) on either. A select's remove() with no argument is ChildNode's.
template <SelectThis This>
JSValue selectRemove(JSContext* ctx, JSValueConst selectObject, int argc, JSValueConst* argv) {
    PrHtmlSelectElement* select = This(ctx, selectObject);
    if (select == nullptr)
        return JS_EXCEPTION;
    if (argc == 0) {
        if (This == selectThis) {
            prDomNodeRemove(prHtmlSelectElementAsNode(select));
            return JS_UNDEFINED;
        }
        return jsArgCount(ctx, argc, 1) ? JS_UNDEFINED : JS_EXCEPTION;
    }
    int32_t index = 0;
    if (JS_ToInt32(ctx, &index, argv[0]) < 0)
        return JS_EXCEPTION;
    prHtmlSelectElementRemove(select, index);
    return JS_UNDEFINED;
}

// The indexed setter that a select and its options collection share (html/#dom-htmloptionscollection-setter).
bool setSelectItem(JSContext* ctx, PrHtmlSelectElement* select, uint32_t index, JSValueConst value) {
    PrHtmlOptionElement* option = nullptr;
    if (!JS_IsNull(value) && !JS_IsUndefined(value)) {
        option = jsThis<PrHtmlOptionElement>(ctx, value, htmlOptionElementClass);
        if (option == nullptr)
            return false;
    }
    PrStatus status = prHtmlSelectElementSetItem(select, index, option);
    if (status == PrStatus_Ok)
        return true;
    prJsDomThrow(ctx, status);
    return false;
}

uint32_t selectIndexedLength(void* handle) {
    return prHtmlSelectElementLength(static_cast<PrHtmlSelectElement*>(handle));
}

JSValue selectIndexedItem(JSContext* ctx, JSValueConst object, void* handle, uint32_t index) {
    PrHtmlOptionElement* option = prHtmlSelectElementItem(static_cast<PrHtmlSelectElement*>(handle), index);
    return elementToJs(ctx, object, prHtmlOptionElementAsElement(option));
}

bool selectIndexedSet(JSContext* ctx, JSValueConst, void* handle, uint32_t index, JSValueConst value) {
    return setSelectItem(ctx, static_cast<PrHtmlSelectElement*>(handle), index, value);
}

constexpr JsIndexed kSelectIndexed = {selectIndexedLength, selectIndexedItem, selectIndexedSet};

uint32_t optionsIndexedLength(void* handle) {
    return prHtmlSelectElementLength(collectionSelect(handle));
}

JSValue optionsIndexedItem(JSContext* ctx, JSValueConst object, void* handle, uint32_t index) {
    PrHtmlOptionElement* option = prHtmlSelectElementItem(collectionSelect(handle), index);
    return elementToJs(ctx, object, prHtmlOptionElementAsElement(option));
}

bool optionsIndexedSet(JSContext* ctx, JSValueConst, void* handle, uint32_t index, JSValueConst value) {
    return setSelectItem(ctx, collectionSelect(handle), index, value);
}

constexpr JsIndexed kOptionsIndexed = {optionsIndexedLength, optionsIndexedItem, optionsIndexedSet};

bool optionsNames(JSContext* ctx, void* handle, Array<JSAtom>* out) {
    return jsNameAtoms(ctx, static_cast<const PrDomHtmlCollection*>(handle), prDomHtmlCollectionNames, out);
}

JSValue optionsNamed(JSContext* ctx, JSValueConst object, void* handle, PrStringView name) {
    PrDomElement* element = prDomHtmlCollectionNamedItem(static_cast<PrDomHtmlCollection*>(handle), name);
    return element != nullptr ? elementToJs(ctx, object, element) : JS_UNDEFINED;
}

// [LegacyUnenumerableNamedProperties], as HTMLCollection's.
constexpr JsNamed kOptionsNamed = {optionsNames, optionsNamed, nullptr, nullptr, JsNamedFlag_Unenumerable};

JSValue selectOptions(JSContext* ctx, JSValueConst selectObject) {
    PrHtmlSelectElement* select = selectThis(ctx, selectObject);
    if (select == nullptr)
        return JS_EXCEPTION;
    return sameCollection<"options">(ctx, selectObject, htmlOptionsCollectionClass, [&] {
        return prHtmlSelectElementOptions(select);
    });
}

JSValue selectSelectedOptions(JSContext* ctx, JSValueConst selectObject) {
    PrHtmlSelectElement* select = selectThis(ctx, selectObject);
    if (select == nullptr)
        return JS_EXCEPTION;
    return sameCollection<"selectedOptions">(ctx, selectObject, prJsDomHtmlCollectionClass, [&] {
        return prHtmlSelectElementSelectedOptions(select);
    });
}

JSValue selectType(JSContext* ctx, JSValueConst selectObject) {
    PrHtmlSelectElement* select = selectThis(ctx, selectObject);
    return select != nullptr ? jsFixedString(ctx, prHtmlSelectElementType(select)) : JS_EXCEPTION;
}

JSValue selectItem(JSContext* ctx, JSValueConst selectObject, int argc, JSValueConst* argv) {
    PrHtmlSelectElement* select = selectThis(ctx, selectObject);
    uint32_t index = 0;
    if (select == nullptr || !jsArgCount(ctx, argc, 1) || JS_ToUint32(ctx, &index, argv[0]) < 0)
        return JS_EXCEPTION;
    return elementToJs(ctx, selectObject, prHtmlOptionElementAsElement(prHtmlSelectElementItem(select, index)));
}

JSValue selectNamedItem(JSContext* ctx, JSValueConst selectObject, int argc, JSValueConst* argv) {
    PrHtmlSelectElement* select = selectThis(ctx, selectObject);
    StringArgument name = {ctx};
    if (select == nullptr || !jsArgCount(ctx, argc, 1) || !name.read(argv[0]))
        return JS_EXCEPTION;
    PrDomHtmlCollection* options = prHtmlSelectElementOptions(select);
    PrDomElement* element = prDomHtmlCollectionNamedItem(options, name.value());
    prDomHtmlCollectionDestroy(options);
    return elementToJs(ctx, selectObject, element);
}

JSValue selectValue(JSContext* ctx, JSValueConst selectObject) {
    PrHtmlSelectElement* select = selectThis(ctx, selectObject);
    return select != nullptr ? jsTakeString(ctx, prHtmlSelectElementValue(select)) : JS_EXCEPTION;
}

JSValue setSelectValue(JSContext* ctx, JSValueConst selectObject, JSValueConst input) {
    PrHtmlSelectElement* select = selectThis(ctx, selectObject);
    StringArgument value = {ctx};
    if (select == nullptr || !value.read(input))
        return JS_EXCEPTION;
    prHtmlSelectElementSetValue(select, value.value());
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------------------------------------------------
// HTMLTableElement (html/#htmltableelement)

PrHtmlTableElement* PR_NULLABLE tableThis(JSContext* ctx, JSValueConst object) {
    return jsThis<PrHtmlTableElement>(ctx, object, htmlTableElementClass);
}

// caption, tHead and tFoot, with the part as the magic.
JSValue tablePart(JSContext* ctx, JSValueConst tableObject, int magic) {
    PrHtmlTableElement* table = tableThis(ctx, tableObject);
    if (table == nullptr)
        return JS_EXCEPTION;
    return elementToJs(ctx, tableObject, prHtmlTableElementPart(table, PrHtmlTablePart(magic)));
}

JSValue setTablePart(JSContext* ctx, JSValueConst tableObject, JSValueConst input, int magic) {
    PrHtmlTableElement* table = tableThis(ctx, tableObject);
    if (table == nullptr)
        return JS_EXCEPTION;
    PrDomElement* element = nullptr;
    if (!JS_IsNull(input) && !JS_IsUndefined(input)) {
        element = jsThis<PrDomElement>(ctx, input, htmlElementClass);
        if (element == nullptr)
            return JS_EXCEPTION;
    }
    PrStatus status = prHtmlTableElementSetPart(table, PrHtmlTablePart(magic), element);
    if (status == PrStatus_WrongType)
        return JS_ThrowTypeError(ctx, "The element's interface is not the attribute's");
    return statusToJs(ctx, status);
}

JSValue tableCreatePart(JSContext* ctx, JSValueConst tableObject, int, JSValueConst*, int magic) {
    PrHtmlTableElement* table = tableThis(ctx, tableObject);
    if (table == nullptr)
        return JS_EXCEPTION;
    return elementToJs(ctx, tableObject, prHtmlTableElementEnsurePart(table, PrHtmlTablePart(magic)));
}

JSValue tableDeletePart(JSContext* ctx, JSValueConst tableObject, int, JSValueConst*, int magic) {
    PrHtmlTableElement* table = tableThis(ctx, tableObject);
    if (table == nullptr)
        return JS_EXCEPTION;
    prHtmlTableElementDeletePart(table, PrHtmlTablePart(magic));
    return JS_UNDEFINED;
}

JSValue tableBodies(JSContext* ctx, JSValueConst tableObject) {
    PrHtmlTableElement* table = tableThis(ctx, tableObject);
    if (table == nullptr)
        return JS_EXCEPTION;
    return sameCollection<"tBodies">(ctx, tableObject, prJsDomHtmlCollectionClass,
        [&] { return prHtmlTableElementTBodies(table); });
}

JSValue tableCreateBody(JSContext* ctx, JSValueConst tableObject, int, JSValueConst*) {
    PrHtmlTableElement* table = tableThis(ctx, tableObject);
    return table != nullptr ? elementToJs(ctx, tableObject, prHtmlTableElementAddTBody(table)) : JS_EXCEPTION;
}

JSValue tableRows(JSContext* ctx, JSValueConst tableObject) {
    PrHtmlTableElement* table = tableThis(ctx, tableObject);
    if (table == nullptr)
        return JS_EXCEPTION;
    return sameCollection<"rows">(ctx, tableObject, prJsDomHtmlCollectionClass,
        [&] { return prHtmlTableElementRows(table); });
}

// insertRow and insertCell, whose `Insert` is the call of the interface `Handle` with the class `Class`.
template <typename Handle, JSClassID* Class, auto Insert>
JSValue insertInto(JSContext* ctx, JSValueConst handleObject, int argc, JSValueConst* argv) {
    Handle* handle = jsThis<Handle>(ctx, handleObject, *Class);
    int32_t index = -1;
    if (handle == nullptr || !readIndex(ctx, argc, argv, &index))
        return JS_EXCEPTION;
    PrDomElement* element = nullptr;
    PrStatus status = Insert(handle, index, &element);
    return status == PrStatus_Ok ? elementToJs(ctx, handleObject, element) : prJsDomThrow(ctx, status);
}

// deleteRow and deleteCell, whose index is required.
template <typename Handle, JSClassID* Class, auto Delete>
JSValue deleteFrom(JSContext* ctx, JSValueConst handleObject, int argc, JSValueConst* argv) {
    Handle* handle = jsThis<Handle>(ctx, handleObject, *Class);
    int32_t index = 0;
    if (handle == nullptr || !jsArgCount(ctx, argc, 1) || JS_ToInt32(ctx, &index, argv[0]) < 0)
        return JS_EXCEPTION;
    return statusToJs(ctx, Delete(handle, index));
}

// ---------------------------------------------------------------------------------------------------------------------
// HTMLTableSectionElement, HTMLTableRowElement and HTMLTableCellElement (html/#tables)

JSValue sectionRows(JSContext* ctx, JSValueConst sectionObject) {
    PrHtmlTableSectionElement* section =
        jsThis<PrHtmlTableSectionElement>(ctx, sectionObject, htmlTableSectionElementClass);
    if (section == nullptr)
        return JS_EXCEPTION;
    return sameCollection<"rows">(ctx, sectionObject, prJsDomHtmlCollectionClass,
        [&] { return prHtmlTableSectionElementRows(section); });
}

JSValue rowCells(JSContext* ctx, JSValueConst rowObject) {
    PrHtmlTableRowElement* row = jsThis<PrHtmlTableRowElement>(ctx, rowObject, htmlTableRowElementClass);
    if (row == nullptr)
        return JS_EXCEPTION;
    return sameCollection<"cells">(ctx, rowObject, prJsDomHtmlCollectionClass,
        [&] { return prHtmlTableRowElementCells(row); });
}

JSValue rowIndex(JSContext* ctx, JSValueConst rowObject) {
    PrHtmlTableRowElement* row = jsThis<PrHtmlTableRowElement>(ctx, rowObject, htmlTableRowElementClass);
    return row != nullptr ? JS_NewInt32(ctx, prHtmlTableRowElementRowIndex(row)) : JS_EXCEPTION;
}

JSValue sectionRowIndex(JSContext* ctx, JSValueConst rowObject) {
    PrHtmlTableRowElement* row = jsThis<PrHtmlTableRowElement>(ctx, rowObject, htmlTableRowElementClass);
    return row != nullptr ? JS_NewInt32(ctx, prHtmlTableRowElementSectionRowIndex(row)) : JS_EXCEPTION;
}

JSValue cellIndex(JSContext* ctx, JSValueConst cellObject) {
    PrHtmlTableCellElement* cell = jsThis<PrHtmlTableCellElement>(ctx, cellObject, htmlTableCellElementClass);
    return cell != nullptr ? JS_NewInt32(ctx, prHtmlTableCellElementCellIndex(cell)) : JS_EXCEPTION;
}

JSValue cellColSpan(JSContext* ctx, JSValueConst cellObject) {
    PrHtmlTableCellElement* cell = jsThis<PrHtmlTableCellElement>(ctx, cellObject, htmlTableCellElementClass);
    return cell != nullptr ? JS_NewUint32(ctx, prHtmlTableCellElementColSpan(cell)) : JS_EXCEPTION;
}

JSValue cellRowSpan(JSContext* ctx, JSValueConst cellObject) {
    PrHtmlTableCellElement* cell = jsThis<PrHtmlTableCellElement>(ctx, cellObject, htmlTableCellElementClass);
    return cell != nullptr ? JS_NewUint32(ctx, prHtmlTableCellElementRowSpan(cell)) : JS_EXCEPTION;
}

constexpr PrStringView kScopeStates[] = {"row", "col", "rowgroup", "colgroup"};
constexpr JsReflectedKeywords kScopeKeywords = {kScopeStates, uint32_t(countOf(kScopeStates)), "", ""};

// ---------------------------------------------------------------------------------------------------------------------
// Prototypes

constexpr JSCFunctionListEntry kBodyProto[] = {
    PR_JS_TAG("HTMLBodyElement"),
    PR_REFLECT_STRING("text", "text"),
    PR_REFLECT_STRING("link", "link"),
    PR_REFLECT_STRING("vLink", "vlink"),
    PR_REFLECT_STRING("aLink", "alink"),
    PR_REFLECT_STRING("bgColor", "bgcolor"),
    PR_REFLECT_STRING("background", "background"),
    PR_JS_DOM_WINDOW_EVENT_HANDLER_MEMBERS
};

constexpr JSCFunctionListEntry kBrProto[] = {PR_JS_TAG("HTMLBRElement"), PR_REFLECT_STRING("clear", "clear")};

constexpr JSCFunctionListEntry kDataProto[] = {PR_JS_TAG("HTMLDataElement"), PR_REFLECT_STRING("value", "value")};

constexpr JSCFunctionListEntry kDataListProto[] = {
    PR_JS_TAG("HTMLDataListElement"),
    PR_JS_GET("options", dataListOptions),
};

constexpr JSCFunctionListEntry kDivProto[] = {PR_JS_TAG("HTMLDivElement"), PR_REFLECT_STRING("align", "align")};

constexpr JSCFunctionListEntry kDListProto[] = {PR_JS_TAG("HTMLDListElement"), PR_REFLECT_BOOL("compact", "compact")};

constexpr JSCFunctionListEntry kEmbedProto[] = {
    PR_JS_TAG("HTMLEmbedElement"),
    PR_REFLECT_URL("src", "src"),
    PR_REFLECT_STRING("type", "type"),
    PR_REFLECT_STRING("width", "width"),
    PR_REFLECT_STRING("height", "height"),
    PR_REFLECT_STRING("align", "align"),
    PR_REFLECT_STRING("name", "name"),
};

constexpr JSCFunctionListEntry kFontProto[] = {
    PR_JS_TAG("HTMLFontElement"),
    PR_REFLECT_STRING("color", "color"),
    PR_REFLECT_STRING("face", "face"),
    PR_REFLECT_STRING("size", "size"),
};

constexpr JSCFunctionListEntry kFrameProto[] = {
    PR_JS_TAG("HTMLFrameElement"),
    PR_REFLECT_STRING("name", "name"),
    PR_REFLECT_STRING("scrolling", "scrolling"),
    PR_REFLECT_URL("src", "src"),
    PR_REFLECT_STRING("frameBorder", "frameborder"),
    PR_REFLECT_URL("longDesc", "longdesc"),
    PR_REFLECT_BOOL("noResize", "noresize"),
    PR_REFLECT_STRING("marginHeight", "marginheight"),
    PR_REFLECT_STRING("marginWidth", "marginwidth"),
};

constexpr JSCFunctionListEntry kFrameSetProto[] = {
    PR_JS_TAG("HTMLFrameSetElement"),
    PR_REFLECT_STRING("cols", "cols"),
    PR_REFLECT_STRING("rows", "rows"),
    PR_JS_DOM_WINDOW_EVENT_HANDLER_MEMBERS
};

constexpr JSCFunctionListEntry kHeadProto[] = {PR_JS_TAG("HTMLHeadElement")};

constexpr JSCFunctionListEntry kHeadingProto[] = {PR_JS_TAG("HTMLHeadingElement"), PR_REFLECT_STRING("align", "align")};

constexpr JSCFunctionListEntry kHrProto[] = {
    PR_JS_TAG("HTMLHRElement"),
    PR_REFLECT_STRING("align", "align"),
    PR_REFLECT_STRING("color", "color"),
    PR_REFLECT_BOOL("noShade", "noshade"),
    PR_REFLECT_STRING("size", "size"),
    PR_REFLECT_STRING("width", "width"),
};

constexpr JSCFunctionListEntry kHtmlProto[] = {PR_JS_TAG("HTMLHtmlElement"), PR_REFLECT_STRING("version", "version")};

constexpr JSCFunctionListEntry kLiProto[] = {
    PR_JS_TAG("HTMLLIElement"),
    PR_REFLECT_INTEGER("value", "value", PrHtmlReflectedInteger_Long, 0),
    PR_REFLECT_STRING("type", "type"),
};

constexpr JSCFunctionListEntry kMapProto[] = {
    PR_JS_TAG("HTMLMapElement"),
    PR_REFLECT_STRING("name", "name"),
    PR_JS_GET("areas", mapAreas),
};

constexpr JSCFunctionListEntry kMarqueeProto[] = {
    PR_JS_TAG("HTMLMarqueeElement"),
    PR_REFLECT_STRING("behavior", "behavior"),
    PR_REFLECT_STRING("bgColor", "bgcolor"),
    PR_REFLECT_STRING("direction", "direction"),
    PR_REFLECT_STRING("height", "height"),
    PR_REFLECT_INTEGER("hspace", "hspace", PrHtmlReflectedInteger_Unsigned, 0),
    PR_REFLECT_INTEGER("loop", "loop", PrHtmlReflectedInteger_Long, -1),
    PR_REFLECT_INTEGER("scrollAmount", "scrollamount", PrHtmlReflectedInteger_Unsigned, 6),
    PR_REFLECT_INTEGER("scrollDelay", "scrolldelay", PrHtmlReflectedInteger_Unsigned, 85),
    PR_REFLECT_BOOL("trueSpeed", "truespeed"),
    PR_REFLECT_INTEGER("vspace", "vspace", PrHtmlReflectedInteger_Unsigned, 0),
    PR_REFLECT_STRING("width", "width"),
};

constexpr JSCFunctionListEntry kMenuProto[] = {PR_JS_TAG("HTMLMenuElement"), PR_REFLECT_BOOL("compact", "compact")};

constexpr JSCFunctionListEntry kModProto[] = {
    PR_JS_TAG("HTMLModElement"),
    PR_REFLECT_URL("cite", "cite"),
    PR_REFLECT_STRING("dateTime", "datetime"),
};

constexpr JSCFunctionListEntry kObjectProto[] = {
    PR_JS_TAG("HTMLObjectElement"),
    PR_REFLECT_URL("data", "data"),
    PR_REFLECT_STRING("type", "type"),
    PR_REFLECT_STRING("name", "name"),
    PR_REFLECT_STRING("useMap", "usemap"),
    PR_REFLECT_STRING("width", "width"),
    PR_REFLECT_STRING("height", "height"),
    PR_REFLECT_STRING("align", "align"),
    PR_REFLECT_STRING("archive", "archive"),
    PR_REFLECT_STRING("code", "code"),
    PR_REFLECT_BOOL("declare", "declare"),
    PR_REFLECT_INTEGER("hspace", "hspace", PrHtmlReflectedInteger_Unsigned, 0),
    PR_REFLECT_STRING("standby", "standby"),
    PR_REFLECT_INTEGER("vspace", "vspace", PrHtmlReflectedInteger_Unsigned, 0),
    PR_REFLECT_URL("codeBase", "codebase"),
    PR_REFLECT_STRING("codeType", "codetype"),
    PR_REFLECT_STRING("border", "border"),
};

constexpr JSCFunctionListEntry kOListProto[] = {
    PR_JS_TAG("HTMLOListElement"),
    PR_REFLECT_BOOL("reversed", "reversed"),
    PR_REFLECT_INTEGER("start", "start", PrHtmlReflectedInteger_Long, 1),
    PR_REFLECT_STRING("type", "type"),
    PR_REFLECT_BOOL("compact", "compact"),
};

constexpr JSCFunctionListEntry kOptGroupProto[] = {
    PR_JS_TAG("HTMLOptGroupElement"),
    PR_REFLECT_BOOL("disabled", "disabled"),
    PR_REFLECT_STRING("label", "label"),
};

constexpr JSCFunctionListEntry kOptionProto[] = {
    PR_JS_TAG("HTMLOptionElement"),
    PR_REFLECT_BOOL("disabled", "disabled"),
    PR_JS_GET("form", optionForm),
    PR_JS_GET_SET("label", optionString<prHtmlOptionElementLabel>, jsSetReflectString<"label">),
    PR_REFLECT_BOOL("defaultSelected", "selected"),
    PR_JS_GET_SET("selected", optionSelected, setOptionSelected),
    PR_JS_GET_SET("value", optionString<prHtmlOptionElementValue>, jsSetReflectString<"value">),
    PR_JS_GET_SET("text", optionString<prHtmlOptionElementText>, setOptionText),
    PR_JS_GET("index", optionIndex),
};

constexpr JSCFunctionListEntry kParagraphProto[] = {
    PR_JS_TAG("HTMLParagraphElement"),
    PR_REFLECT_STRING("align", "align"),
};

constexpr JSCFunctionListEntry kParamProto[] = {
    PR_JS_TAG("HTMLParamElement"),
    PR_REFLECT_STRING("name", "name"),
    PR_REFLECT_STRING("value", "value"),
    PR_REFLECT_STRING("type", "type"),
    PR_REFLECT_STRING("valueType", "valuetype"),
};

constexpr JSCFunctionListEntry kPreProto[] = {
    PR_JS_TAG("HTMLPreElement"),
    PR_REFLECT_INTEGER("width", "width", PrHtmlReflectedInteger_Long, 0),
};

constexpr JSCFunctionListEntry kQuoteProto[] = {PR_JS_TAG("HTMLQuoteElement"), PR_REFLECT_URL("cite", "cite")};

constexpr JSCFunctionListEntry kSelectProto[] = {
    PR_JS_TAG("HTMLSelectElement"),
    PR_REFLECT_STRING("autocomplete", "autocomplete"),
    PR_REFLECT_BOOL("disabled", "disabled"),
    PR_REFLECT_BOOL("multiple", "multiple"),
    PR_REFLECT_STRING("name", "name"),
    PR_REFLECT_BOOL("required", "required"),
    PR_REFLECT_INTEGER("size", "size", PrHtmlReflectedInteger_Unsigned, 0),
    PR_JS_GET("type", selectType),
    PR_JS_GET("options", selectOptions),
    PR_JS_GET_SET("length", selectLength<selectThis>, setSelectLength<selectThis>),
    PR_JS_METHOD("item", 1, selectItem),
    PR_JS_METHOD("namedItem", 1, selectNamedItem),
    PR_JS_METHOD("add", 1, selectAdd<selectThis>),
    PR_JS_METHOD("remove", 0, selectRemove<selectThis>),
    PR_JS_GET("selectedOptions", selectSelectedOptions),
    PR_JS_GET_SET("selectedIndex", selectedIndex<selectThis>, setSelectedIndex<selectThis>),
    PR_JS_GET_SET("value", selectValue, setSelectValue),
};

constexpr JSCFunctionListEntry kSpanProto[] = {PR_JS_TAG("HTMLSpanElement")};

constexpr JSCFunctionListEntry kTableCaptionProto[] = {
    PR_JS_TAG("HTMLTableCaptionElement"),
    PR_REFLECT_STRING("align", "align"),
};

constexpr JSCFunctionListEntry kTableCellProto[] = {
    PR_JS_TAG("HTMLTableCellElement"),
    PR_JS_GET_SET("colSpan", cellColSpan, (jsSetReflectInteger<"colspan", PrHtmlReflectedInteger_Unsigned, 1>)),
    PR_JS_GET_SET("rowSpan", cellRowSpan, (jsSetReflectInteger<"rowspan", PrHtmlReflectedInteger_Unsigned, 1>)),
    PR_REFLECT_STRING("headers", "headers"),
    PR_JS_GET("cellIndex", cellIndex),
    PR_REFLECT_ENUMERATED("scope", "scope", &kScopeKeywords),
    PR_REFLECT_STRING("abbr", "abbr"),
    PR_REFLECT_STRING("axis", "axis"),
    PR_REFLECT_STRING("align", "align"),
    PR_REFLECT_STRING("ch", "char"),
    PR_REFLECT_STRING("chOff", "charoff"),
    PR_REFLECT_BOOL("noWrap", "nowrap"),
    PR_REFLECT_STRING("vAlign", "valign"),
    PR_REFLECT_STRING("width", "width"),
    PR_REFLECT_STRING("height", "height"),
    PR_REFLECT_STRING("bgColor", "bgcolor"),
};

constexpr JSCFunctionListEntry kTableColProto[] = {
    PR_JS_TAG("HTMLTableColElement"),
    PR_REFLECT_INTEGER("span", "span", PrHtmlReflectedInteger_Unsigned, 1),
    PR_REFLECT_STRING("align", "align"),
    PR_REFLECT_STRING("ch", "char"),
    PR_REFLECT_STRING("chOff", "charoff"),
    PR_REFLECT_STRING("vAlign", "valign"),
    PR_REFLECT_STRING("width", "width"),
};

constexpr JSCFunctionListEntry kTableProto[] = {
    PR_JS_TAG("HTMLTableElement"),
    PR_JS_MAGIC_GET_SET("caption", tablePart, setTablePart, PrHtmlTablePart_Caption),
    PR_JS_MAGIC("createCaption", 0, tableCreatePart, PrHtmlTablePart_Caption),
    PR_JS_MAGIC("deleteCaption", 0, tableDeletePart, PrHtmlTablePart_Caption),
    PR_JS_MAGIC_GET_SET("tHead", tablePart, setTablePart, PrHtmlTablePart_THead),
    PR_JS_MAGIC("createTHead", 0, tableCreatePart, PrHtmlTablePart_THead),
    PR_JS_MAGIC("deleteTHead", 0, tableDeletePart, PrHtmlTablePart_THead),
    PR_JS_MAGIC_GET_SET("tFoot", tablePart, setTablePart, PrHtmlTablePart_TFoot),
    PR_JS_MAGIC("createTFoot", 0, tableCreatePart, PrHtmlTablePart_TFoot),
    PR_JS_MAGIC("deleteTFoot", 0, tableDeletePart, PrHtmlTablePart_TFoot),
    PR_JS_GET("tBodies", tableBodies),
    PR_JS_METHOD("createTBody", 0, tableCreateBody),
    PR_JS_GET("rows", tableRows),
    PR_JS_METHOD("insertRow", 0, (insertInto<PrHtmlTableElement, &htmlTableElementClass, prHtmlTableElementInsertRow>)),
    PR_JS_METHOD("deleteRow", 1, (deleteFrom<PrHtmlTableElement, &htmlTableElementClass, prHtmlTableElementDeleteRow>)),
    PR_REFLECT_STRING("align", "align"),
    PR_REFLECT_STRING("border", "border"),
    PR_REFLECT_STRING("frame", "frame"),
    PR_REFLECT_STRING("rules", "rules"),
    PR_REFLECT_STRING("summary", "summary"),
    PR_REFLECT_STRING("width", "width"),
    PR_REFLECT_STRING("bgColor", "bgcolor"),
    PR_REFLECT_STRING("cellPadding", "cellpadding"),
    PR_REFLECT_STRING("cellSpacing", "cellspacing"),
};

constexpr JSCFunctionListEntry kTableRowProto[] = {
    PR_JS_TAG("HTMLTableRowElement"),
    PR_JS_GET("rowIndex", rowIndex),
    PR_JS_GET("sectionRowIndex", sectionRowIndex),
    PR_JS_GET("cells", rowCells),
    PR_JS_METHOD("insertCell", 0,
        (insertInto<PrHtmlTableRowElement, &htmlTableRowElementClass, prHtmlTableRowElementInsertCell>)),
    PR_JS_METHOD("deleteCell", 1,
        (deleteFrom<PrHtmlTableRowElement, &htmlTableRowElementClass, prHtmlTableRowElementDeleteCell>)),
    PR_REFLECT_STRING("align", "align"),
    PR_REFLECT_STRING("ch", "char"),
    PR_REFLECT_STRING("chOff", "charoff"),
    PR_REFLECT_STRING("vAlign", "valign"),
    PR_REFLECT_STRING("bgColor", "bgcolor"),
};

constexpr JSCFunctionListEntry kTableSectionProto[] = {
    PR_JS_TAG("HTMLTableSectionElement"),
    PR_JS_GET("rows", sectionRows),
    PR_JS_METHOD("insertRow", 0,
        (insertInto<PrHtmlTableSectionElement, &htmlTableSectionElementClass, prHtmlTableSectionElementInsertRow>)),
    PR_JS_METHOD("deleteRow", 1,
        (deleteFrom<PrHtmlTableSectionElement, &htmlTableSectionElementClass, prHtmlTableSectionElementDeleteRow>)),
    PR_REFLECT_STRING("align", "align"),
    PR_REFLECT_STRING("ch", "char"),
    PR_REFLECT_STRING("chOff", "charoff"),
    PR_REFLECT_STRING("vAlign", "valign"),
};

constexpr JSCFunctionListEntry kTimeProto[] = {PR_JS_TAG("HTMLTimeElement"), PR_REFLECT_STRING("dateTime", "datetime")};

constexpr JSCFunctionListEntry kTitleProto[] = {
    PR_JS_TAG("HTMLTitleElement"),
    PR_JS_GET_SET("text", titleText, setTitleText),
};

constexpr JSCFunctionListEntry kTrackProto[] = {
    PR_JS_TAG("HTMLTrackElement"),
    PR_REFLECT_URL("src", "src"),
    PR_REFLECT_STRING("srclang", "srclang"),
    PR_REFLECT_STRING("label", "label"),
    PR_REFLECT_BOOL("default", "default"),
};

constexpr JSCFunctionListEntry kUListProto[] = {
    PR_JS_TAG("HTMLUListElement"),
    PR_REFLECT_BOOL("compact", "compact"),
    PR_REFLECT_STRING("type", "type"),
};

constexpr JSCFunctionListEntry kUnknownProto[] = {PR_JS_TAG("HTMLUnknownElement")};

constexpr JSCFunctionListEntry kOptionsCollectionProto[] = {
    PR_JS_TAG("HTMLOptionsCollection"),
    PR_JS_GET_SET("length", selectLength<optionsThis>, setSelectLength<optionsThis>),
    PR_JS_METHOD("add", 1, selectAdd<optionsThis>),
    PR_JS_METHOD("remove", 1, selectRemove<optionsThis>),
    PR_JS_GET_SET("selectedIndex", selectedIndex<optionsThis>, setSelectedIndex<optionsThis>),
};

// Each constructs what a customized built-in element's definition names, and nothing else.
#define PR_ELEMENT_CLASS(name, id, proto)                                                          \
    {                                                                                              \
        PR_JS_CLASS(name, id, jsViewFinalizer<&id>, proto),                                        \
        .constructor = jsCustomElementsConstruct,                                                  \
        .base = &htmlElementClass,                                                                 \
    }

constexpr JsClassDefinition kClasses[] = {
    PR_ELEMENT_CLASS("HTMLBodyElement", htmlBodyElementClass, kBodyProto),
    PR_ELEMENT_CLASS("HTMLBRElement", htmlBrElementClass, kBrProto),
    PR_ELEMENT_CLASS("HTMLDataElement", htmlDataElementClass, kDataProto),
    PR_ELEMENT_CLASS("HTMLDataListElement", htmlDataListElementClass, kDataListProto),
    PR_ELEMENT_CLASS("HTMLDivElement", htmlDivElementClass, kDivProto),
    PR_ELEMENT_CLASS("HTMLDListElement", htmlDListElementClass, kDListProto),
    PR_ELEMENT_CLASS("HTMLEmbedElement", htmlEmbedElementClass, kEmbedProto),
    PR_ELEMENT_CLASS("HTMLFontElement", htmlFontElementClass, kFontProto),
    PR_ELEMENT_CLASS("HTMLFrameElement", htmlFrameElementClass, kFrameProto),
    PR_ELEMENT_CLASS("HTMLFrameSetElement", htmlFrameSetElementClass, kFrameSetProto),
    PR_ELEMENT_CLASS("HTMLHeadElement", htmlHeadElementClass, kHeadProto),
    PR_ELEMENT_CLASS("HTMLHeadingElement", htmlHeadingElementClass, kHeadingProto),
    PR_ELEMENT_CLASS("HTMLHRElement", htmlHrElementClass, kHrProto),
    PR_ELEMENT_CLASS("HTMLHtmlElement", htmlHtmlElementClass, kHtmlProto),
    PR_ELEMENT_CLASS("HTMLLIElement", htmlLiElementClass, kLiProto),
    PR_ELEMENT_CLASS("HTMLMapElement", htmlMapElementClass, kMapProto),
    PR_ELEMENT_CLASS("HTMLMarqueeElement", htmlMarqueeElementClass, kMarqueeProto),
    PR_ELEMENT_CLASS("HTMLMenuElement", htmlMenuElementClass, kMenuProto),
    PR_ELEMENT_CLASS("HTMLModElement", htmlModElementClass, kModProto),
    PR_ELEMENT_CLASS("HTMLObjectElement", htmlObjectElementClass, kObjectProto),
    PR_ELEMENT_CLASS("HTMLOListElement", htmlOListElementClass, kOListProto),
    PR_ELEMENT_CLASS("HTMLOptGroupElement", htmlOptGroupElementClass, kOptGroupProto),
    PR_ELEMENT_CLASS("HTMLOptionElement", htmlOptionElementClass, kOptionProto),
    PR_ELEMENT_CLASS("HTMLParagraphElement", htmlParagraphElementClass, kParagraphProto),
    PR_ELEMENT_CLASS("HTMLParamElement", htmlParamElementClass, kParamProto),
    PR_ELEMENT_CLASS("HTMLPreElement", htmlPreElementClass, kPreProto),
    PR_ELEMENT_CLASS("HTMLQuoteElement", htmlQuoteElementClass, kQuoteProto),
    {
        PR_JS_CLASS("HTMLSelectElement", htmlSelectElementClass, jsViewFinalizer<&htmlSelectElementClass>,
            kSelectProto),
        .constructor = jsCustomElementsConstruct,
        .base = &htmlElementClass,
        .indexed = &kSelectIndexed,
    },
    {
        PR_JS_CLASS("HTMLOptionsCollection", htmlOptionsCollectionClass,
            (jsFinalizer<&htmlOptionsCollectionClass, prDomHtmlCollectionDestroy>), kOptionsCollectionProto),
        .base = &prJsDomHtmlCollectionClass,
        .indexed = &kOptionsIndexed,
        .named = &kOptionsNamed,
    },
    PR_ELEMENT_CLASS("HTMLSpanElement", htmlSpanElementClass, kSpanProto),
    PR_ELEMENT_CLASS("HTMLTableCaptionElement", htmlTableCaptionElementClass, kTableCaptionProto),
    PR_ELEMENT_CLASS("HTMLTableCellElement", htmlTableCellElementClass, kTableCellProto),
    PR_ELEMENT_CLASS("HTMLTableColElement", htmlTableColElementClass, kTableColProto),
    PR_ELEMENT_CLASS("HTMLTableElement", htmlTableElementClass, kTableProto),
    PR_ELEMENT_CLASS("HTMLTableRowElement", htmlTableRowElementClass, kTableRowProto),
    PR_ELEMENT_CLASS("HTMLTableSectionElement", htmlTableSectionElementClass, kTableSectionProto),
    PR_ELEMENT_CLASS("HTMLTimeElement", htmlTimeElementClass, kTimeProto),
    PR_ELEMENT_CLASS("HTMLTitleElement", htmlTitleElementClass, kTitleProto),
    PR_ELEMENT_CLASS("HTMLTrackElement", htmlTrackElementClass, kTrackProto),
    PR_ELEMENT_CLASS("HTMLUListElement", htmlUListElementClass, kUListProto),
    PR_ELEMENT_CLASS("HTMLUnknownElement", htmlUnknownElementClass, kUnknownProto),
};

} // namespace

void jsHtmlDefineFactory(JSContext* ctx, const char* name, JSCFunction* function, int length, JSValueConst constructor) {
    JSValue factory = JS_NewCFunction2(ctx, function, name, length, JS_CFUNC_constructor, 0);
    JS_DefinePropertyValueStr(ctx, factory, "prototype", JS_GetPropertyStr(ctx, constructor, "prototype"), 0);
    JSValue global = JS_GetGlobalObject(ctx);
    JS_DefinePropertyValueStr(ctx, global, name, factory, JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE);
    JS_FreeValue(ctx, global);
}

void jsHtmlElementsBind(JSContext* ctx) {
    for (const JsClassDefinition& definition : kClasses) {
        JSValue constructor = jsDefineClass(ctx, definition);
        if (definition.id == &htmlOptionElementClass)
            jsHtmlDefineFactory(ctx, "Option", JsCallScoped<optionFactory>::call, 0, constructor);
        JS_FreeValue(ctx, constructor);
    }
}

JSClassID jsHtmlElementsClass(PrDomElement* element) {
    switch (prDomElementTag(element)) {
        case PrDomTag_Applet:
        case PrDomTag_Bgsound:
        case PrDomTag_Keygen:
            return htmlUnknownElementClass;
        case PrDomTag_Unknown:
            return prDomIsValidCustomElementName(prDomElementLocalName(element)) ? 0 : htmlUnknownElementClass;
        case PrDomTag_Blockquote:
        case PrDomTag_Q:
            return htmlQuoteElementClass;
        case PrDomTag_Body: return htmlBodyElementClass;
        case PrDomTag_Br: return htmlBrElementClass;
        case PrDomTag_Caption: return htmlTableCaptionElementClass;
        case PrDomTag_Col:
        case PrDomTag_Colgroup:
            return htmlTableColElementClass;
        case PrDomTag_Data: return htmlDataElementClass;
        case PrDomTag_Datalist: return htmlDataListElementClass;
        case PrDomTag_Del:
        case PrDomTag_Ins:
            return htmlModElementClass;
        case PrDomTag_Div: return htmlDivElementClass;
        case PrDomTag_Dl: return htmlDListElementClass;
        case PrDomTag_Embed: return htmlEmbedElementClass;
        case PrDomTag_Font: return htmlFontElementClass;
        case PrDomTag_Frame: return htmlFrameElementClass;
        case PrDomTag_Frameset: return htmlFrameSetElementClass;
        case PrDomTag_H1:
        case PrDomTag_H2:
        case PrDomTag_H3:
        case PrDomTag_H4:
        case PrDomTag_H5:
        case PrDomTag_H6:
            return htmlHeadingElementClass;
        case PrDomTag_Head: return htmlHeadElementClass;
        case PrDomTag_Hr: return htmlHrElementClass;
        case PrDomTag_Html: return htmlHtmlElementClass;
        case PrDomTag_Li: return htmlLiElementClass;
        case PrDomTag_Listing:
        case PrDomTag_Pre:
        case PrDomTag_Xmp:
            return htmlPreElementClass;
        case PrDomTag_Map: return htmlMapElementClass;
        case PrDomTag_Marquee: return htmlMarqueeElementClass;
        case PrDomTag_Menu: return htmlMenuElementClass;
        case PrDomTag_Object: return htmlObjectElementClass;
        case PrDomTag_Ol: return htmlOListElementClass;
        case PrDomTag_Optgroup: return htmlOptGroupElementClass;
        case PrDomTag_Option: return htmlOptionElementClass;
        case PrDomTag_P: return htmlParagraphElementClass;
        case PrDomTag_Param: return htmlParamElementClass;
        case PrDomTag_Select: return htmlSelectElementClass;
        case PrDomTag_Span: return htmlSpanElementClass;
        case PrDomTag_Table: return htmlTableElementClass;
        case PrDomTag_Tbody:
        case PrDomTag_Tfoot:
        case PrDomTag_Thead:
            return htmlTableSectionElementClass;
        case PrDomTag_Td:
        case PrDomTag_Th:
            return htmlTableCellElementClass;
        case PrDomTag_Time: return htmlTimeElementClass;
        case PrDomTag_Title: return htmlTitleElementClass;
        case PrDomTag_Tr: return htmlTableRowElementClass;
        case PrDomTag_Track: return htmlTrackElementClass;
        case PrDomTag_Ul: return htmlUListElementClass;
        default: return 0;
    }
}

} // namespace pr_impl
