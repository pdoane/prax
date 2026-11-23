// The JavaScript binding of the SVG DOM's data types, and the members of element interfaces that reflect attributes as
// them.
//
// SVG2 - https://svgwg.org/svg2-draft/types.html

#include "svg/quickjs/src/svg_types_quickjs.h"

#include "pr/alloc.h"
#include "pr/count_of.h"
#include "pr/dom_geometry_quickjs.h"
#include "pr/dom_quickjs.h"
#include "pr/float.h"
#include "pr/memory.h"
#include "pr/raw_array.h"
#include "pr/svg_element.h"
#include "pr/svg_quickjs.h"
#include "pr/svg_types.h"
#include "pr/webidl_quickjs.h"

namespace pr_impl {

namespace {

// What each member reflects.
struct Member {
    const char* name;
    PrSvgAttribute attribute;
    uint8_t part;
};

#define PR_SVG_MEMBER_ENTRY(name, attribute, part) {#name, PrSvgAttribute_##attribute, part},
constexpr Member kMembers[] = {PR_SVG_MEMBERS(PR_SVG_MEMBER_ENTRY)};
#undef PR_SVG_MEMBER_ENTRY
static_assert(countOf(kMembers) == SvgMember_Count);

JSClassID animatedBooleanClass;
JSClassID animatedEnumerationClass;
JSClassID animatedIntegerClass;
JSClassID animatedNumberClass;
JSClassID animatedStringClass;
JSClassID animatedLengthClass;
JSClassID animatedAngleClass;
JSClassID animatedRectClass;
JSClassID animatedPreserveAspectRatioClass;
JSClassID animatedNumberListClass;
JSClassID animatedLengthListClass;
JSClassID animatedTransformListClass;
JSClassID lengthClass;
JSClassID angleClass;
JSClassID numberClass;
JSClassID transformClass;
JSClassID preserveAspectRatioClass;
JSClassID numberListClass;
JSClassID lengthListClass;
JSClassID transformListClass;
JSClassID pointListClass;
JSClassID stringListClass;
JSClassID unitTypesClass;

// Returns `value`, or throws the exception of a failed status and frees `value`: NoModificationAllowedError for a read
// only value, IndexSizeError for an index past the end, SyntaxError for text that does not parse, TypeError for a value
// that is not one, and NotSupportedError for a type that is not one.
JSValue result(JSContext* ctx, PrStatus status, JSValue value = JS_UNDEFINED) {
    if (status == PrStatus_Ok)
        return value;
    JS_FreeValue(ctx, value);
    switch (status) {
        case PrStatus_ReadOnly: return jsDomExceptionThrow(ctx, "NoModificationAllowedError");
        case PrStatus_OutOfRange: return jsDomExceptionThrow(ctx, "IndexSizeError");
        case PrStatus_Syntax: return jsDomExceptionThrow(ctx, "SyntaxError");
        case PrStatus_InvalidData: return JS_ThrowTypeError(ctx, "The value is not one of the enumeration's");
        default: return jsDomExceptionThrow(ctx, "NotSupportedError");
    }
}

// Reads a float (webidl/#js-float), which must be finite.
bool readFloat(JSContext* ctx, JSValueConst value, float* out) {
    double number = 0;
    if (JS_ToFloat64(ctx, &number, value) < 0)
        return false;
    if (!prF64IsFinite(number) || !prF32IsFinite(float(number))) {
        JS_ThrowTypeError(ctx, "The value is not a finite float");
        return false;
    }
    *out = float(number);
    return true;
}

// Reads an unsigned short (webidl/#js-unsigned-short), which wraps.
bool readUnsignedShort(JSContext* ctx, JSValueConst value, uint16_t* out) {
    uint32_t number = 0;
    if (JS_ToUint32(ctx, &number, value) < 0)
        return false;
    *out = uint16_t(number);
    return true;
}

PrSvgElement* retained(PrSvgElement* element) {
    prDomNodeRetain(prSvgElementAsNode(element));
    return element;
}

void release(PrSvgElement* PR_NULLABLE element) {
    if (element != nullptr)
        prDomNodeRelease(prSvgElementAsNode(element));
}

PrSvgLayer layerOf(bool animated) {
    return animated ? PrSvgLayer_Animated : PrSvgLayer_Base;
}

// ---------------------------------------------------------------------------------------------------------------------
// The objects of the SVG DOM

// The kinds of value, and of the items of a list.
enum {
    Kind_Number,
    Kind_Length,
    Kind_Angle,
    Kind_Transform,
    Kind_Ratio,
    Kind_Point,
    Kind_String,
};
typedef uint8_t Kind;

// A value of one kind, or for a string, its text.
struct Item {
    union {
        float number;
        PrSvgLength length;
        PrSvgAngle angle;
        PrSvgTransform transform;
        PrSvgPreserveAspectRatio ratio;
        PrDomPoint point;
    };
    PrRawString text;
};

// An animated value: the member of an element's interface that reflects an attribute.
struct Animated {
    PrSvgElement* element;              // holds a reference
    PrSvgAttribute attribute;
    uint8_t part;                       // of a pair: 1 or 2, or 0 for the whole value
};

struct List;

// A value of the SVG DOM: one that reflects an element's attribute, an item of a list, or a detached value with its
// own. An item of a list holds a reference to the list, whose pointer back to the item is not counted. The object of a
// point is a DOMPoint that reflects the value through `reflected`, and the matrix of a transform is a DOMMatrix that
// reflects it through `matrix` (SVG2/coords.html#__svg__SVGTransform__matrix). Either begins the value, so that the
// value is the handle of the object that reflects it.
struct Value {
    // The reflection of the DOMPoint of a point, or of the DOMMatrix of a transform.
    union {
        PrJsDomReflected reflected;
        PrJsDomReflectedMatrix matrix;
    };

    PrSvgElement* PR_NULLABLE element;  // holds a reference, for a value that reflects an attribute
    List* PR_NULLABLE list;             // the list that the value is an item of
    uint32_t index;
    uint32_t references;
    PrSvgAttribute attribute;
    Kind kind;
    bool animated;                      // the animated value, which is read only
    Item own;                           // the value of a detached value
};

// A list interface object (SVG2/types.html#TermListInterface): the items of an attribute, as they were when the list
// last synchronized with it, and the object of each item that the list has given out.
struct List {
    PrSvgElement* element;              // holds a reference
    RawArray<Item> items;
    RawArray<Value*> objects;           // by position, as many as `items`; null for an item without one
    uint32_t references;
    PrSvgAttribute attribute;
    Kind kind;
    bool animated;
};

Animated* PR_NULLABLE animatedCreate(PrSvgElement* element, const Member& member, PrSvgValueType type) {
    if (prSvgElementAttributeType(element, member.attribute) != type)
        return nullptr;
    Animated* animated = allocZeroed<Animated>();
    *animated = Animated{retained(element), member.attribute, member.part};
    return animated;
}

void animatedDestroy(Animated* PR_NULLABLE animated) {
    if (animated == nullptr)
        return;
    release(animated->element);
    prMemoryFree(animated);
}

void itemReset(Item* item) {
    prRawStringReset(&item->text);
}

void itemsReset(RawArray<Item>* items) {
    for (uint32_t i = 0; i < items->count; ++i)
        itemReset(&items->data[i]);
    items->reset();
}

// Reads the items of a list's attribute, of its type.
template <typename T>
void readTyped(const List* list, uint32_t (*get)(const PrSvgElement*, PrSvgAttribute, PrSvgLayer, T* PR_NULLABLE,
        uint32_t), T Item::*field, RawArray<Item>* out) {
    PrSvgLayer layer = layerOf(list->animated);
    uint32_t count = get(list->element, list->attribute, layer, nullptr, 0);
    RawArray<T> values;
    values.resize(count);
    get(list->element, list->attribute, layer, values.data, count);
    for (uint32_t i = 0; i < count; ++i) {
        Item& item = out->emplace();
        item = Item{};
        item.*field = values.data[i];
    }
    values.reset();
}

void readItems(const List* list, RawArray<Item>* out) {
    switch (list->kind) {
        case Kind_Number: readTyped(list, prSvgElementGetNumberList, &Item::number, out); break;
        case Kind_Length: readTyped(list, prSvgElementGetLengthList, &Item::length, out); break;
        case Kind_Transform: readTyped(list, prSvgElementGetTransformList, &Item::transform, out); break;
        case Kind_Point: readTyped(list, prSvgElementGetPointList, &Item::point, out); break;
        default: {
            uint32_t count = prSvgElementGetStringList(list->element, list->attribute, nullptr, 0);
            RawArray<PrStringView> views;
            views.resize(count);
            prSvgElementGetStringList(list->element, list->attribute, views.data, count);
            for (uint32_t i = 0; i < count; ++i) {
                Item& item = out->emplace();
                item = Item{};
                item.text = prRawStringCopy(views.data[i]);
            }
            views.reset();
            break;
        }
    }
}

// Writes the items of a list to its attribute.
template <typename T>
PrStatus writeTyped(List* list, PrStatus (*set)(PrSvgElement*, PrSvgAttribute, const T* PR_NULLABLE, uint32_t),
        T Item::*field) {
    RawArray<T> values;
    for (uint32_t i = 0; i < list->items.count; ++i)
        values.push(list->items.data[i].*field);
    PrStatus status = set(list->element, list->attribute, values.data, values.count);
    values.reset();
    return status;
}

PrStatus writeItems(List* list) {
    switch (list->kind) {
        case Kind_Number: return writeTyped(list, prSvgElementSetNumberList, &Item::number);
        case Kind_Length: return writeTyped(list, prSvgElementSetLengthList, &Item::length);
        case Kind_Transform: return writeTyped(list, prSvgElementSetTransformList, &Item::transform);
        case Kind_Point: return writeTyped(list, prSvgElementSetPointList, &Item::point);
        default: {
            RawArray<PrStringView> views;
            for (uint32_t i = 0; i < list->items.count; ++i)
                views.push(prRawStringView(&list->items.data[i].text));
            PrStatus status = prSvgElementSetStringList(list->element, list->attribute, views.data, views.count);
            views.reset();
            return status;
        }
    }
}

void listRetain(List* list) {
    ++list->references;
}

void listRelease(List* PR_NULLABLE list) {
    if (list == nullptr || --list->references != 0)
        return;
    // Each item in the list holds a reference to it, so none is left.
    release(list->element);
    itemsReset(&list->items);
    list->objects.reset();
    prMemoryFree(list);
}

List* listCreate(PrSvgElement* element, PrSvgAttribute attribute, Kind kind, bool animated) {
    List* list = allocZeroed<List>();
    list->element = retained(element);
    list->attribute = attribute;
    list->kind = kind;
    list->animated = animated;
    list->references = 1;
    return list;
}

void pointRead(const void* handle, double out[4]);
bool pointWrite(JSContext* ctx, void* handle, const double value[4]);
void retainValue(void* handle);
void releaseValue(void* handle);

// The operations through which the DOMPoint of a point value reads and writes it.
constexpr PrJsDomReflection kPointReflection = {pointRead, pointWrite, retainValue, releaseValue};

void matrixRead(const void* handle, PrDomMatrix* out);
bool matrixWrite(JSContext* ctx, void* handle, const PrDomMatrix* value);

// The operations through which the DOMMatrix of a transform value reads it, and writes it as a matrix transform.
constexpr PrJsDomMatrixReflection kMatrixReflection = {matrixRead, matrixWrite, retainValue, releaseValue};

// Creates a value with one reference: one that reflects an attribute, if `element` is given, or a detached one.
Value* valueCreate(Kind kind, PrSvgElement* PR_NULLABLE element, PrSvgAttribute attribute, bool animated, const Item& own) {
    Value* value = allocZeroed<Value>();
    if (kind == Kind_Transform)
        value->matrix.reflection = &kMatrixReflection;
    else
        value->reflected.reflection = &kPointReflection;
    value->kind = kind;
    value->element = element != nullptr ? retained(element) : nullptr;
    value->attribute = attribute;
    value->animated = animated;
    value->own = own;
    value->references = 1;
    return value;
}

void valueRetain(Value* value) {
    ++value->references;
}

void valueRelease(Value* PR_NULLABLE value) {
    if (value == nullptr || --value->references != 0)
        return;
    if (List* list = value->list) {
        list->objects.data[value->index] = nullptr;
        listRelease(list);
    }
    release(value->element);
    itemReset(&value->own);
    prMemoryFree(value);
}

bool isDetached(const Value* value) {
    return value->list == nullptr && value->element == nullptr;
}

// Makes an item's object a detached value with the value `last` (SVG2/types.html#TermDetach). The object leaves its
// position, which the caller removes or fills.
void detach(Value* item, const Item& last) {
    List* list = item->list;
    list->objects.data[item->index] = nullptr;
    item->own = last;
    item->own.text = PrRawString{};
    item->list = nullptr;
    item->animated = false;
    listRelease(list);
}

// Makes a detached value the object of item `index`, which has none.
void attach(List* list, Value* item, uint32_t index) {
    PR_ASSERT(isDetached(item) && list->objects.data[index] == nullptr);
    item->list = list;
    item->index = index;
    item->animated = list->animated;
    listRetain(list);
    list->objects.data[index] = item;
}

// Brings the list up to date with its attribute (SVG2/types.html#TermSynchronizeList). The objects of the items past
// the attribute's last are detached, with the values that they last had, and the rest read the attribute's items.
void listSync(List* list) {
    RawArray<Item> now;
    readItems(list, &now);
    for (uint32_t i = now.count; i < list->objects.count; ++i) {
        if (list->objects.data[i] != nullptr)
            detach(list->objects.data[i], list->items.data[i]);
    }
    uint32_t had = list->objects.count < now.count ? list->objects.count : now.count;
    list->objects.resize(now.count);
    for (uint32_t i = had; i < now.count; ++i)
        list->objects.data[i] = nullptr;
    itemsReset(&list->items);
    list->items = now;
}

// Returns a value's value: its item of its list, what its attribute reads as, or its own.
Item valueOf(Value* value) {
    if (value->list != nullptr)
        listSync(value->list);
    if (value->list != nullptr) {
        Item item = value->list->items.data[value->index];
        item.text = PrRawString{};
        return item;
    }
    if (value->element == nullptr)
        return value->own;
    Item item{};
    PrSvgLayer layer = layerOf(value->animated);
    switch (value->kind) {
        case Kind_Length: prSvgElementGetLength(value->element, value->attribute, layer, &item.length); break;
        case Kind_Angle: prSvgElementGetAngle(value->element, value->attribute, layer, &item.angle); break;
        case Kind_Ratio:
            prSvgElementGetPreserveAspectRatio(value->element, value->attribute, layer, &item.ratio);
            break;
        default: break;
    }
    return item;
}

// Returns what a length resolves against: its element's, or nothing's for a detached length.
PrSvgLengthBasis basisOf(const Value* value) {
    if (value->list != nullptr)
        return prSvgElementLengthBasis(value->list->element, value->list->attribute);
    if (value->element != nullptr)
        return prSvgElementLengthBasis(value->element, value->attribute);
    return prSvgLengthBasisDefault();
}

// Sets a value: its item of its list, its attribute, or its own.
PrStatus setValue(Value* value, const Item& item) {
    if (value->animated)
        return PrStatus_ReadOnly;
    if (value->list != nullptr)
        listSync(value->list);
    if (List* list = value->list) {
        Item& at = list->items.data[value->index];
        prRawStringReset(&at.text);
        at = item;
        at.text = PrRawString{};
        return writeItems(list);
    }
    if (value->element == nullptr) {
        // A detached length owns the text of a CSS function, which a new value replaces.
        if (value->kind == Kind_Length)
            itemReset(&value->own);
        value->own = item;
        return PrStatus_Ok;
    }
    switch (value->kind) {
        case Kind_Length: return prSvgElementSetLength(value->element, value->attribute, item.length);
        case Kind_Angle: return prSvgElementSetAngle(value->element, value->attribute, item.angle);
        case Kind_Ratio: return prSvgElementSetPreserveAspectRatio(value->element, value->attribute, item.ratio);
        default: return PrStatus_NotSupported;
    }
}

// Returns the class of the objects of values of `kind`.
JSClassID classOf(Kind kind) {
    switch (kind) {
        case Kind_Number: return numberClass;
        case Kind_Length: return lengthClass;
        case Kind_Angle: return angleClass;
        case Kind_Transform: return transformClass;
        default: return preserveAspectRatioClass;
    }
}

void retainValue(void* handle) {
    valueRetain(static_cast<Value*>(handle));
}

void releaseValue(void* handle) {
    valueRelease(static_cast<Value*>(handle));
}

void pointRead(const void* handle, double out[4]) {
    PrDomPoint point = valueOf(static_cast<Value*>(const_cast<void*>(handle))).point;
    out[0] = point.x;
    out[1] = point.y;
    out[2] = point.z;
    out[3] = point.w;
}

bool pointWrite(JSContext* ctx, void* handle, const double value[4]) {
    Item item{};
    item.point = PrDomPoint{value[0], value[1], value[2], value[3]};
    return !JS_IsException(result(ctx, setValue(static_cast<Value*>(handle), item)));
}

void matrixRead(const void* handle, PrDomMatrix* out) {
    PrSvgTransform transform = valueOf(static_cast<Value*>(const_cast<void*>(handle))).transform;
    *out = prSvgTransformMatrix(&transform);
}

bool matrixWrite(JSContext* ctx, void* handle, const PrDomMatrix* value) {
    Item item{};
    item.transform = prSvgTransformFromMatrix(*value);
    return !JS_IsException(result(ctx, setValue(static_cast<Value*>(handle), item)));
}

void retainList(void* handle) {
    listRetain(static_cast<List*>(handle));
}

// Returns the object of a value, which it may have already, and releases the caller's reference. A new object keeps
// `owner` alive.
JSValue valueObject(JSContext* ctx, JSValueConst owner, Value* PR_NULLABLE value) {
    if (value == nullptr)
        return JS_NULL;
    JSValue object = JS_UNDEFINED;
    if (value->kind == Kind_Point)
        object = prJsDomPointReflecting(ctx, owner, &value->reflected);
    else
        object = jsWrap(ctx, classOf(value->kind), owner, value, false, retainValue);
    valueRelease(value);
    return object;
}

JSValue listObject(JSContext* ctx, JSClassID id, JSValueConst owner, List* list) {
    JSValue object = jsWrap(ctx, id, owner, list, false, retainList);
    listRelease(list);
    return object;
}

Value* PR_NULLABLE valueThis(JSContext* ctx, JSValueConst object, JSClassID id) {
    return jsThis<Value>(ctx, object, id);
}

// ---------------------------------------------------------------------------------------------------------------------
// SVGAnimatedBoolean, SVGAnimatedEnumeration, SVGAnimatedInteger, SVGAnimatedNumber and SVGAnimatedString, whose
// baseVal (magic 0) and animVal (1) are values

// The getter of an animated member of an element interface, which creates the animated value once.
JSValue animatedMember(JSContext* ctx, JSValueConst elementObject, int member, JSClassID id, PrSvgValueType type) {
    PrSvgElement* element = jsThis<PrSvgElement>(ctx, elementObject, prJsSvgElementClass);
    if (element == nullptr)
        return JS_EXCEPTION;
    return jsSameObject(ctx, elementObject, kMembers[member].name, [&] {
        Animated* animated = animatedCreate(element, kMembers[member], type);
        if (animated == nullptr)
            return JS_UNDEFINED;
        JSValue object = jsWrap(ctx, id, elementObject, animated, true);
        if (JS_IsException(object))
            animatedDestroy(animated);
        return object;
    });
}

JSValue booleanValue(JSContext* ctx, JSValueConst object, int magic) {
    Animated* animated = jsThis<Animated>(ctx, object, animatedBooleanClass);
    if (animated == nullptr)
        return JS_EXCEPTION;
    bool value = false;
    prSvgElementGetBoolean(animated->element, animated->attribute, layerOf(magic == 1), &value);
    return JS_NewBool(ctx, value);
}

JSValue setBooleanValue(JSContext* ctx, JSValueConst object, JSValueConst input, int) {
    Animated* animated = jsThis<Animated>(ctx, object, animatedBooleanClass);
    int truth = animated != nullptr ? JS_ToBool(ctx, input) : -1;
    if (truth < 0)
        return JS_EXCEPTION;
    return result(ctx, prSvgElementSetBoolean(animated->element, animated->attribute, truth == 1));
}

// The values of an enumeration are its interface's constants, which the C enumerations take. A keyword that a C
// enumeration adds after them has no constant: it reflects as unknown, and cannot be set.
uint8_t lastConstant(const Animated* animated) {
    if (animated->attribute == PrSvgAttribute_EdgeMode)
        return PrSvgEdgeMode_None;
    return 255;
}
JSValue enumerationValue(JSContext* ctx, JSValueConst object, int magic) {
    Animated* animated = jsThis<Animated>(ctx, object, animatedEnumerationClass);
    if (animated == nullptr)
        return JS_EXCEPTION;
    uint8_t value = 0;
    prSvgElementGetEnumeration(animated->element, animated->attribute, layerOf(magic == 1), &value);
    return JS_NewInt32(ctx, value <= lastConstant(animated) ? value : 0);
}

JSValue setEnumerationValue(JSContext* ctx, JSValueConst object, JSValueConst input, int) {
    Animated* animated = jsThis<Animated>(ctx, object, animatedEnumerationClass);
    uint16_t value = 0;
    if (animated == nullptr || !readUnsignedShort(ctx, input, &value))
        return JS_EXCEPTION;
    if (value == 0 || value > lastConstant(animated))
        return result(ctx, PrStatus_InvalidData);
    return result(ctx, prSvgElementSetEnumeration(animated->element, animated->attribute, uint8_t(value)));
}

JSValue integerValue(JSContext* ctx, JSValueConst object, int magic) {
    Animated* animated = jsThis<Animated>(ctx, object, animatedIntegerClass);
    if (animated == nullptr)
        return JS_EXCEPTION;
    int32_t values[2] = {};
    if (animated->part == 0)
        prSvgElementGetInteger(animated->element, animated->attribute, layerOf(magic == 1), &values[0]);
    else
        prSvgElementGetIntegerPair(animated->element, animated->attribute, layerOf(magic == 1), values);
    return JS_NewInt32(ctx, values[animated->part == 2 ? 1 : 0]);
}

JSValue setIntegerValue(JSContext* ctx, JSValueConst object, JSValueConst input, int) {
    Animated* animated = jsThis<Animated>(ctx, object, animatedIntegerClass);
    int32_t value = 0;
    if (animated == nullptr || JS_ToInt32(ctx, &value, input) < 0)
        return JS_EXCEPTION;
    if (animated->part == 0)
        return result(ctx, prSvgElementSetInteger(animated->element, animated->attribute, value));
    // A number of a pair keeps the other.
    int32_t pair[2] = {};
    prSvgElementGetIntegerPair(animated->element, animated->attribute, PrSvgLayer_Base, pair);
    pair[animated->part - 1] = value;
    return result(ctx, prSvgElementSetIntegerPair(animated->element, animated->attribute, pair[0], pair[1]));
}

JSValue numberValue(JSContext* ctx, JSValueConst object, int magic) {
    Animated* animated = jsThis<Animated>(ctx, object, animatedNumberClass);
    if (animated == nullptr)
        return JS_EXCEPTION;
    float values[2] = {};
    if (animated->part == 0)
        prSvgElementGetNumber(animated->element, animated->attribute, layerOf(magic == 1), &values[0]);
    else
        prSvgElementGetNumberPair(animated->element, animated->attribute, layerOf(magic == 1), values);
    return JS_NewFloat64(ctx, double(values[animated->part == 2 ? 1 : 0]));
}

JSValue setNumberValue(JSContext* ctx, JSValueConst object, JSValueConst input, int) {
    Animated* animated = jsThis<Animated>(ctx, object, animatedNumberClass);
    float value = 0;
    if (animated == nullptr || !readFloat(ctx, input, &value))
        return JS_EXCEPTION;
    if (animated->part == 0)
        return result(ctx, prSvgElementSetNumber(animated->element, animated->attribute, value));
    float pair[2] = {};
    prSvgElementGetNumberPair(animated->element, animated->attribute, PrSvgLayer_Base, pair);
    pair[animated->part - 1] = value;
    return result(ctx, prSvgElementSetNumberPair(animated->element, animated->attribute, pair[0], pair[1]));
}

JSValue stringValue(JSContext* ctx, JSValueConst object, int magic) {
    Animated* animated = jsThis<Animated>(ctx, object, animatedStringClass);
    if (animated == nullptr)
        return JS_EXCEPTION;
    return jsNewStringView(ctx, prSvgElementGetString(animated->element, animated->attribute, layerOf(magic == 1)));
}

JSValue setStringValue(JSContext* ctx, JSValueConst object, JSValueConst input, int) {
    Animated* animated = jsThis<Animated>(ctx, object, animatedStringClass);
    PrStringView text{};
    if (animated == nullptr || !jsReadString(ctx, input, &text))
        return JS_EXCEPTION;
    return result(ctx, prSvgElementSetString(animated->element, animated->attribute, text));
}

// The base or animated value of an element's rectangle attribute, which a DOMRect reflects. A missing attribute, or one
// in error, reads as a zero rectangle. The animated value is read only.
struct ReflectedRect {
    PrJsDomReflected reflected;
    PrSvgElement* element;              // holds a reference
    PrSvgAttribute attribute;
    uint32_t references;
    bool animated;
};

void reflectedRectRead(const void* handle, double out[4]) {
    const ReflectedRect* r = static_cast<const ReflectedRect*>(handle);
    PrDomRect rect{};
    if (!prSvgElementGetRect(r->element, r->attribute, layerOf(r->animated), &rect))
        rect = PrDomRect{};
    out[0] = rect.x;
    out[1] = rect.y;
    out[2] = rect.width;
    out[3] = rect.height;
}

bool reflectedRectWrite(JSContext* ctx, void* handle, const double value[4]) {
    const ReflectedRect* r = static_cast<const ReflectedRect*>(handle);
    if (r->animated)
        return !JS_IsException(result(ctx, PrStatus_ReadOnly));
    PrDomRect rect{value[0], value[1], value[2], value[3]};
    return !JS_IsException(result(ctx, prSvgElementSetRect(r->element, r->attribute, rect)));
}

void reflectedRectRetain(void* handle) {
    ++static_cast<ReflectedRect*>(handle)->references;
}

void reflectedRectRelease(void* handle) {
    ReflectedRect* r = static_cast<ReflectedRect*>(handle);
    if (--r->references != 0)
        return;
    release(r->element);
    prMemoryFree(r);
}

constexpr PrJsDomReflection kRectReflection = {
    reflectedRectRead, reflectedRectWrite, reflectedRectRetain, reflectedRectRelease,
};

// Implements SVGAnimatedRect's baseVal (magic 0) and animVal (1). Each read returns the same DOMRect, which reflects
// the attribute.
JSValue rectValue(JSContext* ctx, JSValueConst object, int magic) {
    Animated* animated = jsThis<Animated>(ctx, object, animatedRectClass);
    if (animated == nullptr)
        return JS_EXCEPTION;
    return jsSameObject(ctx, object, magic == 0 ? "baseVal" : "animVal", [&] {
        ReflectedRect* r = allocZeroed<ReflectedRect>();
        *r = ReflectedRect{
            .reflected = {&kRectReflection},
            .element = retained(animated->element),
            .attribute = animated->attribute,
            .references = 1,
            .animated = magic == 1,
        };
        JSValue rect = prJsDomRectReflecting(ctx, object, &r->reflected);
        reflectedRectRelease(r);
        return rect;
    });
}

// ---------------------------------------------------------------------------------------------------------------------
// SVGAnimatedLength, SVGAnimatedAngle and SVGAnimatedPreserveAspectRatio, whose baseVal (magic 0) and animVal (1) are
// objects, and the animated lists

template <const JSClassID* PR_NONNULL Id, Kind K>
JSValue animatedObject(JSContext* ctx, JSValueConst object, int magic) {
    Animated* animated = jsThis<Animated>(ctx, object, *Id);
    if (animated == nullptr)
        return JS_EXCEPTION;
    return jsSameObject(ctx, object, magic == 0 ? "baseVal" : "animVal", [&] {
        Value* value = valueCreate(K, animated->element, animated->attribute, magic == 1, Item{});
        return valueObject(ctx, object, value);
    });
}

template <const JSClassID* PR_NONNULL Id, const JSClassID* PR_NONNULL ListId, Kind K>
JSValue animatedList(JSContext* ctx, JSValueConst object, int magic) {
    Animated* animated = jsThis<Animated>(ctx, object, *Id);
    if (animated == nullptr)
        return JS_EXCEPTION;
    return jsSameObject(ctx, object, magic == 0 ? "baseVal" : "animVal", [&] {
        return listObject(ctx, *ListId, object, listCreate(animated->element, animated->attribute, K, magic == 1));
    });
}

// ---------------------------------------------------------------------------------------------------------------------
// SVGLength and SVGAngle

constexpr JsConstant kLengthConstants[] = {
    {"SVG_LENGTHTYPE_UNKNOWN", 0}, {"SVG_LENGTHTYPE_NUMBER", 1}, {"SVG_LENGTHTYPE_PERCENTAGE", 2},
    {"SVG_LENGTHTYPE_EMS", 3}, {"SVG_LENGTHTYPE_EXS", 4}, {"SVG_LENGTHTYPE_PX", 5}, {"SVG_LENGTHTYPE_CM", 6},
    {"SVG_LENGTHTYPE_MM", 7}, {"SVG_LENGTHTYPE_IN", 8}, {"SVG_LENGTHTYPE_PT", 9}, {"SVG_LENGTHTYPE_PC", 10},
};

constexpr JsConstant kAngleConstants[] = {
    {"SVG_ANGLETYPE_UNKNOWN", 0}, {"SVG_ANGLETYPE_UNSPECIFIED", 1}, {"SVG_ANGLETYPE_DEG", 2},
    {"SVG_ANGLETYPE_RAD", 3}, {"SVG_ANGLETYPE_GRAD", 4},
};

// The unit of each constant of SVGLength's and SVGAngle's types, by its value. A unit that no constant names is of the
// unknown type.
constexpr PrCssUnit kLengthUnits[] = {
    PrCssUnit_Unknown, PrCssUnit_Number, PrCssUnit_Percent, PrCssUnit_Em, PrCssUnit_Ex, PrCssUnit_Px,
    PrCssUnit_Cm, PrCssUnit_Mm, PrCssUnit_In, PrCssUnit_Pt, PrCssUnit_Pc,
};
constexpr PrCssUnit kAngleUnits[] = {PrCssUnit_Unknown, PrCssUnit_Number, PrCssUnit_Deg, PrCssUnit_Rad, PrCssUnit_Grad};

// The members of SVGLength and SVGAngle, which have the same shape, by what each kind of value does.
struct UnitValue {
    const JSClassID* PR_NONNULL id;
    const PrCssUnit* PR_NONNULL units;  // the unit of each constant of the interface's types
    uint32_t constants;

    // Returns the unit and the number of a value, and its value in user units or degrees.
    PrCssUnit (*unit)(const Item&);
    float (*number)(const Item&);
    float (*resolved)(const Value*, const Item&);

    // Returns a value of `number` in `unit`, a value of `resolved` user units or degrees in `unit`, or the value of
    // `item` converted to `unit`.
    Item (*make)(PrCssUnit unit, float number);
    Item (*fromResolved)(const Value*, PrCssUnit unit, float resolved);
    Item (*convert)(const Value*, const Item& item, PrCssUnit unit);

    bool (*parse)(PrStringView text, Item* out);
    PrRawString (*serialize)(const Item&);
};

constexpr UnitValue kLength = {
    &lengthClass,
    kLengthUnits,
    countOf(kLengthUnits),
    [](const Item& item) { return item.length.unit; },
    [](const Item& item) { return item.length.value; },
    [](const Value* value, const Item& item) { return prSvgLengthResolve(item.length, basisOf(value)); },
    [](PrCssUnit unit, float number) {
        Item item{};
        item.length = PrSvgLength{.value = number, .unit = unit};
        return item;
    },
    [](const Value* value, PrCssUnit unit, float resolved) {
        Item item{};
        item.length = prSvgLengthConvert(PrSvgLength{resolved, PrCssUnit_Number}, unit, basisOf(value));
        return item;
    },
    [](const Value* value, const Item& from, PrCssUnit unit) {
        Item item{};
        item.length = prSvgLengthConvert(from.length, unit, basisOf(value));
        return item;
    },
    [](PrStringView text, Item* out) { return prSvgLengthParse(text, &out->length); },
    [](const Item& item) { return prSvgLengthSerialize(item.length); },
};

constexpr UnitValue kAngle = {
    &angleClass,
    kAngleUnits,
    countOf(kAngleUnits),
    [](const Item& item) { return item.angle.unit; },
    [](const Item& item) { return item.angle.value; },
    [](const Value*, const Item& item) { return prSvgAngleDegrees(item.angle); },
    [](PrCssUnit unit, float number) {
        Item item{};
        item.angle = PrSvgAngle{.value = number, .unit = unit};
        return item;
    },
    [](const Value*, PrCssUnit unit, float resolved) {
        Item item{};
        item.angle = prSvgAngleConvert(PrSvgAngle{resolved, PrCssUnit_Deg}, unit);
        return item;
    },
    [](const Value*, const Item& from, PrCssUnit unit) {
        Item item{};
        item.angle = prSvgAngleConvert(from.angle, unit);
        return item;
    },
    [](PrStringView text, Item* out) { return prSvgAngleParse(text, &out->angle); },
    [](const Item& item) { return prSvgAngleSerialize(item.angle); },
};

// Returns the constant of a unit's type, or 0, the unknown type, for a unit that no constant names.
uint32_t constantOf(const UnitValue* u, PrCssUnit unit) {
    for (uint32_t i = 1; i < u->constants; ++i) {
        if (u->units[i] == unit)
            return i;
    }
    return 0;
}

template <const UnitValue* PR_NONNULL U>
JSValue unitType(JSContext* ctx, JSValueConst object) {
    Value* value = valueThis(ctx, object, *U->id);
    if (value == nullptr)
        return JS_EXCEPTION;
    return JS_NewInt32(ctx, int32_t(constantOf(U, U->unit(valueOf(value)))));
}

// Implements value (magic 0) and valueInSpecifiedUnits (1). valueInSpecifiedUnits is the number in the length's own
// unit, even a unit that the interface has no constant for.
template <const UnitValue* PR_NONNULL U>
JSValue unitNumber(JSContext* ctx, JSValueConst object, int magic) {
    Value* value = valueThis(ctx, object, *U->id);
    if (value == nullptr)
        return JS_EXCEPTION;
    Item item = valueOf(value);
    if (magic == 0)
        return JS_NewFloat64(ctx, double(U->resolved(value, item)));
    return JS_NewFloat64(ctx, double(U->number(item)));
}

// Setting value converts the new value to the length's unit (SVG2/types.html#__svg__SVGLength__value). Setting
// valueInSpecifiedUnits keeps the unit too. A length of a CSS value that no unit expresses becomes a number.
template <const UnitValue* PR_NONNULL U>
JSValue setUnitNumber(JSContext* ctx, JSValueConst object, JSValueConst input, int magic) {
    Value* value = valueThis(ctx, object, *U->id);
    float number = 0;
    if (value == nullptr || !readFloat(ctx, input, &number))
        return JS_EXCEPTION;
    if (value->animated)
        return result(ctx, PrStatus_ReadOnly);
    Item item = valueOf(value);
    PrCssUnit unit = U->unit(item);
    if (U == &kLength && unit == PrCssUnit_Unknown)
        unit = PrCssUnit_Number;
    if (magic == 0)
        return result(ctx, setValue(value, U->fromResolved(value, unit, number)));
    return result(ctx, setValue(value, U->make(unit, number)));
}

template <const UnitValue* PR_NONNULL U>
JSValue unitString(JSContext* ctx, JSValueConst object) {
    Value* value = valueThis(ctx, object, *U->id);
    if (value == nullptr)
        return JS_EXCEPTION;
    Item item = valueOf(value);
    PrRawString text{};
    if (U == &kLength && value->list == nullptr && value->element != nullptr)
        text = prSvgElementGetLengthText(value->element, value->attribute, layerOf(value->animated));
    else if (U == &kLength && item.length.unit == PrCssUnit_Unknown)
        text = prRawStringCopy(prRawStringView(&value->own.text));
    else
        text = U->serialize(item);
    JSValue string = jsNewString(ctx, &text);
    prRawStringReset(&text);
    return string;
}

template <const UnitValue* PR_NONNULL U>
JSValue setUnitString(JSContext* ctx, JSValueConst object, JSValueConst input) {
    Value* value = valueThis(ctx, object, *U->id);
    PrStringView text{};
    if (value == nullptr || !jsReadString(ctx, input, &text))
        return JS_EXCEPTION;
    if (value->animated)
        return result(ctx, PrStatus_ReadOnly);
    if (U == &kLength && value->list == nullptr && value->element != nullptr)
        return result(ctx, prSvgElementSetLengthText(value->element, value->attribute, text));
    Item item{};
    if (U->parse(text, &item))
        return result(ctx, setValue(value, item));
    // A detached length holds a CSS function as a value that no unit expresses, and keeps its text.
    if (U != &kLength || value->list != nullptr || !prSvgLengthIsCssFunction(text))
        return result(ctx, PrStatus_Syntax);
    item.length = PrSvgLength{.value = 0, .unit = PrCssUnit_Unknown};
    item.text = prRawStringCopy(prStringTrim(text));
    return result(ctx, setValue(value, item));
}

// Implements newValueSpecifiedUnits (magic 0) and convertToSpecifiedUnits (1), which take a unit that the interface has
// a constant for.
template <const UnitValue* PR_NONNULL U>
JSValue specifyUnits(JSContext* ctx, JSValueConst object, int argc, JSValueConst* argv, int magic) {
    Value* value = valueThis(ctx, object, *U->id);
    uint16_t unit = 0;
    float number = 0;
    if (value == nullptr || !jsArgCount(ctx, argc, magic == 0 ? 2 : 1) || !readUnsignedShort(ctx, argv[0], &unit) ||
            (magic == 0 && !readFloat(ctx, argv[1], &number)))
        return JS_EXCEPTION;
    if (value->animated)
        return result(ctx, PrStatus_ReadOnly);
    if (unit == 0 || unit >= U->constants)
        return result(ctx, PrStatus_NotSupported);
    // A CSS value that no unit expresses cannot be converted.
    if (magic == 1 && U == &kLength && valueOf(value).length.unit == PrCssUnit_Unknown)
        return result(ctx, PrStatus_NotSupported);
    if (magic == 0)
        return result(ctx, setValue(value, U->make(U->units[unit], number)));
    return result(ctx, setValue(value, U->convert(value, valueOf(value), U->units[unit])));
}

#define PR_SVG_UNIT_VALUE_MEMBERS(U)                                                                                   \
    PR_JS_GET("unitType", (unitType<U>)), PR_JS_MAGIC_GET_SET("value", (unitNumber<U>), (setUnitNumber<U>), 0),       \
    PR_JS_MAGIC_GET_SET("valueInSpecifiedUnits", (unitNumber<U>), (setUnitNumber<U>), 1),                             \
    PR_JS_GET_SET("valueAsString", (unitString<U>), (setUnitString<U>)),                                              \
    PR_JS_MAGIC("newValueSpecifiedUnits", 2, (specifyUnits<U>), 0),                                                   \
    PR_JS_MAGIC("convertToSpecifiedUnits", 1, (specifyUnits<U>), 1)

// ---------------------------------------------------------------------------------------------------------------------
// SVGPreserveAspectRatio, SVGNumber and SVGTransform

constexpr JsConstant kPreserveAspectRatioConstants[] = {
    {"SVG_PRESERVEASPECTRATIO_UNKNOWN", 0}, {"SVG_PRESERVEASPECTRATIO_NONE", PrSvgAlign_None},
    {"SVG_PRESERVEASPECTRATIO_XMINYMIN", PrSvgAlign_XMinYMin},
    {"SVG_PRESERVEASPECTRATIO_XMIDYMIN", PrSvgAlign_XMidYMin},
    {"SVG_PRESERVEASPECTRATIO_XMAXYMIN", PrSvgAlign_XMaxYMin},
    {"SVG_PRESERVEASPECTRATIO_XMINYMID", PrSvgAlign_XMinYMid},
    {"SVG_PRESERVEASPECTRATIO_XMIDYMID", PrSvgAlign_XMidYMid},
    {"SVG_PRESERVEASPECTRATIO_XMAXYMID", PrSvgAlign_XMaxYMid},
    {"SVG_PRESERVEASPECTRATIO_XMINYMAX", PrSvgAlign_XMinYMax},
    {"SVG_PRESERVEASPECTRATIO_XMIDYMAX", PrSvgAlign_XMidYMax},
    {"SVG_PRESERVEASPECTRATIO_XMAXYMAX", PrSvgAlign_XMaxYMax}, {"SVG_MEETORSLICE_UNKNOWN", 0},
    {"SVG_MEETORSLICE_MEET", PrSvgMeetOrSlice_Meet}, {"SVG_MEETORSLICE_SLICE", PrSvgMeetOrSlice_Slice},
};

// Implements align (magic 0) and meetOrSlice (1).
JSValue ratioValue(JSContext* ctx, JSValueConst object, int magic) {
    Value* value = valueThis(ctx, object, preserveAspectRatioClass);
    if (value == nullptr)
        return JS_EXCEPTION;
    PrSvgPreserveAspectRatio ratio = valueOf(value).ratio;
    return JS_NewInt32(ctx, magic == 0 ? ratio.align : ratio.meetOrSlice);
}

JSValue setRatioValue(JSContext* ctx, JSValueConst object, JSValueConst input, int magic) {
    Value* value = valueThis(ctx, object, preserveAspectRatioClass);
    uint16_t number = 0;
    if (value == nullptr || !readUnsignedShort(ctx, input, &number))
        return JS_EXCEPTION;
    if (value->animated)
        return result(ctx, PrStatus_ReadOnly);
    uint16_t last = magic == 0 ? uint16_t(PrSvgAlign_XMaxYMax) : uint16_t(PrSvgMeetOrSlice_Slice);
    if (number == 0 || number > last)
        return result(ctx, PrStatus_NotSupported);
    Item item = valueOf(value);
    if (magic == 0)
        item.ratio.align = uint8_t(number);
    else
        item.ratio.meetOrSlice = uint8_t(number);
    return result(ctx, setValue(value, item));
}

JSValue numberItemValue(JSContext* ctx, JSValueConst object) {
    Value* value = valueThis(ctx, object, numberClass);
    return value != nullptr ? JS_NewFloat64(ctx, double(valueOf(value).number)) : JS_EXCEPTION;
}

JSValue setNumberItemValue(JSContext* ctx, JSValueConst object, JSValueConst input) {
    Value* value = valueThis(ctx, object, numberClass);
    float number = 0;
    if (value == nullptr || !readFloat(ctx, input, &number))
        return JS_EXCEPTION;
    Item item{};
    item.number = number;
    return result(ctx, setValue(value, item));
}

constexpr JsConstant kTransformConstants[] = {
    {"SVG_TRANSFORM_UNKNOWN", 0}, {"SVG_TRANSFORM_MATRIX", PrSvgTransformType_Matrix},
    {"SVG_TRANSFORM_TRANSLATE", PrSvgTransformType_Translate}, {"SVG_TRANSFORM_SCALE", PrSvgTransformType_Scale},
    {"SVG_TRANSFORM_ROTATE", PrSvgTransformType_Rotate}, {"SVG_TRANSFORM_SKEWX", PrSvgTransformType_SkewX},
    {"SVG_TRANSFORM_SKEWY", PrSvgTransformType_SkewY},
};

Item transformItem(const PrSvgTransform& transform) {
    Item item{};
    item.transform = transform;
    return item;
}

// Implements type (magic 0), matrix (1), which is the transform's one DOMMatrix, and angle (2).
JSValue transformMember(JSContext* ctx, JSValueConst object, int magic) {
    Value* value = valueThis(ctx, object, transformClass);
    if (value == nullptr)
        return JS_EXCEPTION;
    if (magic == 1)
        return prJsDomMatrixReflecting(ctx, object, &value->matrix);
    PrSvgTransform transform = valueOf(value).transform;
    if (magic == 0)
        return JS_NewInt32(ctx, transform.type);
    return JS_NewFloat64(ctx, double(prSvgTransformAngle(&transform)));
}

JSValue setMatrix(JSContext* ctx, JSValueConst object, int argc, JSValueConst* argv) {
    Value* value = valueThis(ctx, object, transformClass);
    PrDomMatrix matrix = prDomMatrixIdentity();
    if (value == nullptr || !prJsDomMatrix2DInitRead(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, &matrix))
        return JS_EXCEPTION;
    return result(ctx, setValue(value, transformItem(prSvgTransformFromMatrix(matrix))));
}

// Implements setTranslate (magic 0), setScale (1), setRotate (2), setSkewX (3) and setSkewY (4).
JSValue setTransformKind(JSContext* ctx, JSValueConst object, int argc, JSValueConst* argv, int magic) {
    static constexpr int kCounts[] = {2, 2, 3, 1, 1};
    static constexpr PrSvgTransformType kTypes[] = {
        PrSvgTransformType_Translate, PrSvgTransformType_Scale,
        PrSvgTransformType_Rotate, PrSvgTransformType_SkewX, PrSvgTransformType_SkewY,
    };
    Value* value = valueThis(ctx, object, transformClass);
    if (value == nullptr || !jsArgCount(ctx, argc, kCounts[magic]))
        return JS_EXCEPTION;
    PrSvgTransform transform{.type = kTypes[magic]};
    for (int i = 0; i < kCounts[magic]; ++i) {
        if (!readFloat(ctx, argv[i], &transform.values[i]))
            return JS_EXCEPTION;
    }
    return result(ctx, setValue(value, transformItem(transform)));
}

// ---------------------------------------------------------------------------------------------------------------------
// The operations of lists

List* PR_NULLABLE listThis(JSContext* ctx, JSValueConst object, JSClassID id) {
    return jsThis<List>(ctx, object, id);
}

// Gives the objects from position `from` on their positions again, after an item is inserted or removed.
void renumber(List* list, uint32_t from) {
    for (uint32_t i = from; i < list->objects.count; ++i) {
        if (list->objects.data[i] != nullptr)
            list->objects.data[i]->index = i;
    }
}

PrStatus listClear(List* list) {
    if (list->animated)
        return PrStatus_ReadOnly;
    listSync(list);
    for (uint32_t i = 0; i < list->objects.count; ++i) {
        if (list->objects.data[i] != nullptr)
            detach(list->objects.data[i], list->items.data[i]);
    }
    list->objects.clear();
    itemsReset(&list->items);
    return writeItems(list);
}

// Puts `item` before item `index`, or after the last item, or in place of item `index` if `replace`, whose object is
// detached. Writes the position of the item to `at`, which has no object. The caller writes the attribute.
PrStatus listPut(List* list, const Item& item, uint32_t index, bool replace, uint32_t* at) {
    if (list->animated)
        return PrStatus_ReadOnly;
    listSync(list);
    if (replace && index >= list->items.count)
        return PrStatus_OutOfRange;
    Item placed = item;
    placed.text = prRawStringIsNull(&item.text) ? PrRawString{} : prRawStringClone(&item.text);
    if (replace) {
        if (list->objects.data[index] != nullptr)
            detach(list->objects.data[index], list->items.data[index]);
        itemReset(&list->items.data[index]);
        list->items.data[index] = placed;
        *at = index;
    } else {
        *at = index < list->items.count ? index : list->items.count;
        list->items.insert(*at, placed);
        list->objects.insert(*at, nullptr);
        renumber(list, *at + 1);
    }
    return PrStatus_Ok;
}

// Removes item `index`, detaching its object.
PrStatus listRemove(List* list, uint32_t index) {
    if (list->animated)
        return PrStatus_ReadOnly;
    listSync(list);
    if (index >= list->items.count)
        return PrStatus_OutOfRange;
    if (list->objects.data[index] != nullptr)
        detach(list->objects.data[index], list->items.data[index]);
    itemReset(&list->items.data[index]);
    list->items.erase(index);
    list->objects.erase(index);
    renumber(list, index);
    return writeItems(list);
}

// Returns the object of item `index` of a synchronized list, which is created if the item has none, with a reference
// for the caller.
Value* itemObject(List* list, uint32_t index) {
    Value* item = list->objects.data[index];
    if (item != nullptr) {
        valueRetain(item);
        return item;
    }
    Value* created = valueCreate(list->kind, nullptr, list->attribute, false, Item{});
    attach(list, created, index);
    return created;
}

// Returns `given` if it is detached, or else a detached copy of it whose value is `value`, with a reference for the
// caller.
Value* itemToPlace(const List* list, Value* given, const Item& value) {
    if (isDetached(given)) {
        valueRetain(given);
        return given;
    }
    return valueCreate(list->kind, nullptr, list->attribute, false, value);
}

// Puts `given`, or a copy of it if it is not detached, before item `index`, after the last item, or in place of item
// `index` if `replace`. Writes its object to `out`. The copy is made first, so that an item that replaces itself is
// copied rather than detached by its own replacement and put back.
PrStatus itemPut(List* list, Value* given, uint32_t index, bool replace, Value* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    Item value = valueOf(given);
    Value* placed = itemToPlace(list, given, value);
    uint32_t at = 0;
    PrStatus status = listPut(list, value, index, replace, &at);
    if (status != PrStatus_Ok) {
        valueRelease(placed);
        return status;
    }
    attach(list, placed, at);
    *out = placed;
    return writeItems(list);
}

// Empties the list and puts `given`, or a copy of it if it is not detached, in it. The copy is made first, so that an
// item of the list is copied too.
PrStatus itemInitialize(List* list, Value* given, Value* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    if (list->animated)
        return PrStatus_ReadOnly;
    Value* placed = itemToPlace(list, given, valueOf(given));
    listClear(list);
    PrStatus status = itemPut(list, placed, 0, false, out);
    valueRelease(placed);
    return status;
}

// Returns an item's object, or throws for a failed call.
JSValue itemResult(JSContext* ctx, JSValueConst listObject, PrStatus status, Value* PR_NULLABLE item) {
    if (status != PrStatus_Ok) {
        valueRelease(item);
        return result(ctx, status);
    }
    return valueObject(ctx, listObject, item);
}

// The class of a list of each kind, and of its items' objects.
JSClassID listClassOf(Kind kind) {
    switch (kind) {
        case Kind_Number: return numberListClass;
        case Kind_Length: return lengthListClass;
        case Kind_Transform: return transformListClass;
        case Kind_Point: return pointListClass;
        default: return stringListClass;
    }
}

template <Kind K>
JSValue listLength(JSContext* ctx, JSValueConst object) {
    List* list = listThis(ctx, object, listClassOf(K));
    if (list == nullptr)
        return JS_EXCEPTION;
    listSync(list);
    return JS_NewUint32(ctx, list->items.count);
}

template <Kind K>
JSValue listClearMethod(JSContext* ctx, JSValueConst object, int, JSValueConst*) {
    List* list = listThis(ctx, object, listClassOf(K));
    return list != nullptr ? result(ctx, listClear(list)) : JS_EXCEPTION;
}

// Returns item `index` of a list whose items are objects: its object.
JSValue getObjectItem(JSContext* ctx, JSValueConst object, List* list, uint32_t index) {
    listSync(list);
    if (index >= list->items.count)
        return result(ctx, PrStatus_OutOfRange);
    return valueObject(ctx, object, itemObject(list, index));
}

// Returns item `index` of a list of strings.
JSValue getPlainItem(JSContext* ctx, List* list, uint32_t index) {
    listSync(list);
    if (index >= list->items.count)
        return result(ctx, PrStatus_OutOfRange);
    return jsNewStringView(ctx, prRawStringView(&list->items.data[index].text));
}

bool hasObjects(Kind kind) {
    return kind == Kind_Number || kind == Kind_Length || kind == Kind_Transform || kind == Kind_Point;
}

// Returns the value of an item passed to a method of a list of kind `kind`, with a reference for the caller. Returns
// null and throws a TypeError for an argument of the wrong type. The item of a point list is a DOMPoint. A DOMPoint
// that reflects a point value gives that value, and any other DOMPoint gives a new detached value with its point.
Value* PR_NULLABLE givenItem(JSContext* ctx, Kind kind, JSValueConst input) {
    if (kind != Kind_Point) {
        Value* given = valueThis(ctx, input, classOf(kind));
        if (given != nullptr)
            valueRetain(given);
        return given;
    }
    PrJsDomReflected* reflected = prJsDomReflectedFromValue(ctx, input);
    if (reflected != nullptr && reflected->reflection == &kPointReflection) {
        Value* given = reinterpret_cast<Value*>(reflected);
        valueRetain(given);
        return given;
    }
    Item item{};
    if (!prJsDomPointRead(ctx, input, &item.point))
        return nullptr;
    return valueCreate(Kind_Point, nullptr, PrSvgAttribute_Unknown, false, item);
}

template <Kind K>
JSValue getItemMethod(JSContext* ctx, JSValueConst object, int argc, JSValueConst* argv) {
    List* list = listThis(ctx, object, listClassOf(K));
    uint32_t index = 0;
    if (list == nullptr || !jsArgCount(ctx, argc, 1) || JS_ToUint32(ctx, &index, argv[0]) < 0)
        return JS_EXCEPTION;
    return hasObjects(K) ? getObjectItem(ctx, object, list, index) : getPlainItem(ctx, list, index);
}

// Reads the string passed to a method of a list of strings into `item`, and writes the method's result to `out`.
bool readPlain(JSContext* ctx, JSValueConst input, Item* item, JSValue* out) {
    PrStringView text{};
    if (!jsReadString(ctx, input, &text))
        return false;
    item->text = prRawStringCopy(text);
    *out = jsNewStringView(ctx, text);
    return !JS_IsException(*out);
}

// Implements initialize (magic 0), insertItemBefore (1), replaceItem (2) and appendItem (3).
template <Kind K>
JSValue putMethod(JSContext* ctx, JSValueConst object, int argc, JSValueConst* argv, int magic) {
    List* list = listThis(ctx, object, listClassOf(K));
    bool indexed = magic == 1 || magic == 2;
    uint32_t index = 0;
    if (list == nullptr || !jsArgCount(ctx, argc, indexed ? 2 : 1))
        return JS_EXCEPTION;
    if (hasObjects(K)) {
        Value* given = givenItem(ctx, K, argv[0]);
        if (given == nullptr || (indexed && JS_ToUint32(ctx, &index, argv[1]) < 0)) {
            valueRelease(given);
            return JS_EXCEPTION;
        }
        Value* item = nullptr;
        PrStatus status = PrStatus_Ok;
        if (magic == 0) {
            status = itemInitialize(list, given, &item);
        } else if (magic == 2) {
            status = itemPut(list, given, index, true, &item);
        } else {
            if (magic == 3)
                listSync(list);
            status = itemPut(list, given, magic == 3 ? list->items.count : index, false, &item);
        }
        valueRelease(given);
        return itemResult(ctx, object, status, item);
    }
    Item item{};
    JSValue returned = JS_UNDEFINED;
    if (!readPlain(ctx, argv[0], &item, &returned) || (indexed && JS_ToUint32(ctx, &index, argv[1]) < 0)) {
        itemReset(&item);
        JS_FreeValue(ctx, returned);
        return JS_EXCEPTION;
    }
    PrStatus status = PrStatus_Ok;
    if (magic == 0)
        status = listClear(list);
    uint32_t at = 0;
    if (status == PrStatus_Ok) {
        if (magic == 3)
            listSync(list);
        uint32_t where = magic == 0 ? 0 : magic == 3 ? list->items.count : index;
        status = listPut(list, item, where, magic == 2, &at);
    }
    if (status == PrStatus_Ok)
        status = writeItems(list);
    itemReset(&item);
    return result(ctx, status, returned);
}

template <Kind K>
JSValue removeMethod(JSContext* ctx, JSValueConst object, int argc, JSValueConst* argv) {
    List* list = listThis(ctx, object, listClassOf(K));
    uint32_t index = 0;
    if (list == nullptr || !jsArgCount(ctx, argc, 1) || JS_ToUint32(ctx, &index, argv[0]) < 0)
        return JS_EXCEPTION;
    if (list->animated)
        return result(ctx, PrStatus_ReadOnly);
    listSync(list);
    if (index >= list->items.count)
        return result(ctx, PrStatus_OutOfRange);
    if (hasObjects(K)) {
        // The item's object, which leaves the list detached.
        Value* item = itemObject(list, index);
        PrStatus status = listRemove(list, index);
        return itemResult(ctx, JS_UNDEFINED, status, item);
    }
    JSValue removed = getPlainItem(ctx, list, index);
    return result(ctx, listRemove(list, index), removed);
}

// The indexed properties of a list (SVG2/types.html#TermListInterface).
template <Kind K>
uint32_t indexedCount(void* handle) {
    List* list = static_cast<List*>(handle);
    listSync(list);
    return list->items.count;
}

template <Kind K>
JSValue indexedAt(JSContext* ctx, JSValueConst object, void* handle, uint32_t index) {
    List* list = static_cast<List*>(handle);
    return hasObjects(K) ? getObjectItem(ctx, object, list, index) : getPlainItem(ctx, list, index);
}

template <Kind K>
bool setIndexedAt(JSContext* ctx, JSValueConst object, void* handle, uint32_t index, JSValueConst input) {
    Value* given = givenItem(ctx, K, input);
    if (given == nullptr)
        return false;
    Value* item = nullptr;
    PrStatus status = itemPut(static_cast<List*>(handle), given, index, true, &item);
    valueRelease(given);
    JSValue done = itemResult(ctx, object, status, item);
    bool ok = !JS_IsException(done);
    JS_FreeValue(ctx, done);
    return ok;
}

#define PR_SVG_LIST_MEMBERS(K)                                                                                         \
    PR_JS_GET("length", (listLength<K>)), PR_JS_GET("numberOfItems", (listLength<K>)),                               \
    PR_JS_METHOD("clear", 0, (listClearMethod<K>)), PR_JS_MAGIC("initialize", 1, (putMethod<K>), 0),                \
    PR_JS_METHOD("getItem", 1, (getItemMethod<K>)), PR_JS_MAGIC("insertItemBefore", 2, (putMethod<K>), 1),           \
    PR_JS_MAGIC("replaceItem", 2, (putMethod<K>), 2), PR_JS_METHOD("removeItem", 1, (removeMethod<K>)),              \
    PR_JS_MAGIC("appendItem", 1, (putMethod<K>), 3)

constexpr JsIndexed kNumberIndexed = {indexedCount<Kind_Number>, indexedAt<Kind_Number>, setIndexedAt<Kind_Number>};
constexpr JsIndexed kLengthIndexed = {indexedCount<Kind_Length>, indexedAt<Kind_Length>, setIndexedAt<Kind_Length>};
constexpr JsIndexed kTransformIndexed = {
    indexedCount<Kind_Transform>, indexedAt<Kind_Transform>, setIndexedAt<Kind_Transform>,
};
constexpr JsIndexed kPointIndexed = {indexedCount<Kind_Point>, indexedAt<Kind_Point>, setIndexedAt<Kind_Point>};
constexpr JsIndexed kStringIndexed = {indexedCount<Kind_String>, indexedAt<Kind_String>, nullptr};

// Returns a new, detached transform of a matrix.
JSValue transformFromMatrix(JSContext* ctx, JSValueConst input) {
    PrDomMatrix matrix = prDomMatrixIdentity();
    if (!prJsDomMatrix2DInitRead(ctx, input, &matrix))
        return JS_EXCEPTION;
    Item item = transformItem(prSvgTransformFromMatrix(matrix));
    return valueObject(ctx, JS_UNDEFINED, valueCreate(Kind_Transform, nullptr, PrSvgAttribute_Unknown, false, item));
}

JSValue createFromMatrix(JSContext* ctx, JSValueConst object, int argc, JSValueConst* argv) {
    if (listThis(ctx, object, transformListClass) == nullptr)
        return JS_EXCEPTION;
    return transformFromMatrix(ctx, argc > 0 ? argv[0] : JS_UNDEFINED);
}

// Replaces the items of a transform list with one matrix, their product, and returns its object, or null for an empty
// list (SVG2/coords.html#__svg__SVGTransformList__consolidate).
JSValue consolidate(JSContext* ctx, JSValueConst object, int, JSValueConst*) {
    List* list = listThis(ctx, object, transformListClass);
    if (list == nullptr)
        return JS_EXCEPTION;
    if (list->animated)
        return result(ctx, PrStatus_ReadOnly);
    listSync(list);
    if (list->items.count == 0)
        return JS_NULL;
    PrDomMatrix product = prDomMatrixIdentity();
    for (uint32_t i = 0; i < list->items.count; ++i) {
        PrDomMatrix matrix = prSvgTransformMatrix(&list->items.data[i].transform);
        prDomMatrixMultiply(&product, &matrix);
    }
    Value* single = valueCreate(Kind_Transform, nullptr, list->attribute, false,
        transformItem(prSvgTransformFromMatrix(product)));
    Value* item = nullptr;
    PrStatus status = listClear(list);
    if (status == PrStatus_Ok)
        status = itemPut(list, single, 0, false, &item);
    valueRelease(single);
    return itemResult(ctx, object, status, item);
}

// ---------------------------------------------------------------------------------------------------------------------
// The classes

void valueFinalize(Value* PR_NULLABLE value) {
    valueRelease(value);
}

void listFinalize(List* PR_NULLABLE list) {
    listRelease(list);
}

#define PR_SVG_ANIMATED_SCALAR(get, set)                                                                               \
    PR_JS_MAGIC_GET_SET("baseVal", get, set, 0), PR_JS_MAGIC_GET("animVal", get, 1)

constexpr JSCFunctionListEntry kAnimatedBooleanProto[] = {
    PR_JS_TAG("SVGAnimatedBoolean"), PR_SVG_ANIMATED_SCALAR(booleanValue, setBooleanValue),
};

constexpr JSCFunctionListEntry kAnimatedEnumerationProto[] = {
    PR_JS_TAG("SVGAnimatedEnumeration"), PR_SVG_ANIMATED_SCALAR(enumerationValue, setEnumerationValue),
};

constexpr JSCFunctionListEntry kAnimatedIntegerProto[] = {
    PR_JS_TAG("SVGAnimatedInteger"), PR_SVG_ANIMATED_SCALAR(integerValue, setIntegerValue),
};

constexpr JSCFunctionListEntry kAnimatedNumberProto[] = {
    PR_JS_TAG("SVGAnimatedNumber"), PR_SVG_ANIMATED_SCALAR(numberValue, setNumberValue),
};

constexpr JSCFunctionListEntry kAnimatedStringProto[] = {
    PR_JS_TAG("SVGAnimatedString"), PR_SVG_ANIMATED_SCALAR(stringValue, setStringValue),
};

#define PR_SVG_ANIMATED_OBJECT(get) PR_JS_MAGIC_GET("baseVal", get, 0), PR_JS_MAGIC_GET("animVal", get, 1)

constexpr JSCFunctionListEntry kAnimatedLengthProto[] = {
    PR_JS_TAG("SVGAnimatedLength"), PR_SVG_ANIMATED_OBJECT((animatedObject<&animatedLengthClass, Kind_Length>)),
};

constexpr JSCFunctionListEntry kAnimatedAngleProto[] = {
    PR_JS_TAG("SVGAnimatedAngle"), PR_SVG_ANIMATED_OBJECT((animatedObject<&animatedAngleClass, Kind_Angle>)),
};

constexpr JSCFunctionListEntry kAnimatedRectProto[] = {PR_JS_TAG("SVGAnimatedRect"), PR_SVG_ANIMATED_OBJECT(rectValue)};

constexpr JSCFunctionListEntry kAnimatedPreserveAspectRatioProto[] = {
    PR_JS_TAG("SVGAnimatedPreserveAspectRatio"),
    PR_SVG_ANIMATED_OBJECT((animatedObject<&animatedPreserveAspectRatioClass, Kind_Ratio>)),
};

constexpr JSCFunctionListEntry kAnimatedNumberListProto[] = {
    PR_JS_TAG("SVGAnimatedNumberList"),
    PR_SVG_ANIMATED_OBJECT((animatedList<&animatedNumberListClass, &numberListClass, Kind_Number>)),
};

constexpr JSCFunctionListEntry kAnimatedLengthListProto[] = {
    PR_JS_TAG("SVGAnimatedLengthList"),
    PR_SVG_ANIMATED_OBJECT((animatedList<&animatedLengthListClass, &lengthListClass, Kind_Length>)),
};

constexpr JSCFunctionListEntry kAnimatedTransformListProto[] = {
    PR_JS_TAG("SVGAnimatedTransformList"),
    PR_SVG_ANIMATED_OBJECT((animatedList<&animatedTransformListClass, &transformListClass, Kind_Transform>)),
};

constexpr JSCFunctionListEntry kLengthProto[] = {PR_JS_TAG("SVGLength"), PR_SVG_UNIT_VALUE_MEMBERS(&kLength)};

constexpr JSCFunctionListEntry kAngleProto[] = {PR_JS_TAG("SVGAngle"), PR_SVG_UNIT_VALUE_MEMBERS(&kAngle)};

constexpr JSCFunctionListEntry kNumberProto[] = {
    PR_JS_TAG("SVGNumber"), PR_JS_GET_SET("value", numberItemValue, setNumberItemValue),
};

constexpr JSCFunctionListEntry kTransformProto[] = {
    PR_JS_TAG("SVGTransform"),
    PR_JS_MAGIC_GET("type", transformMember, 0),
    PR_JS_MAGIC_GET("matrix", transformMember, 1),
    PR_JS_MAGIC_GET("angle", transformMember, 2),
    PR_JS_METHOD("setMatrix", 0, setMatrix),
    PR_JS_MAGIC("setTranslate", 2, setTransformKind, 0),
    PR_JS_MAGIC("setScale", 2, setTransformKind, 1),
    PR_JS_MAGIC("setRotate", 3, setTransformKind, 2),
    PR_JS_MAGIC("setSkewX", 1, setTransformKind, 3),
    PR_JS_MAGIC("setSkewY", 1, setTransformKind, 4),
};

constexpr JSCFunctionListEntry kPreserveAspectRatioProto[] = {
    PR_JS_TAG("SVGPreserveAspectRatio"),
    PR_JS_MAGIC_GET_SET("align", ratioValue, setRatioValue, 0),
    PR_JS_MAGIC_GET_SET("meetOrSlice", ratioValue, setRatioValue, 1),
};

constexpr JSCFunctionListEntry kNumberListProto[] = {PR_JS_TAG("SVGNumberList"), PR_SVG_LIST_MEMBERS(Kind_Number)};

constexpr JSCFunctionListEntry kLengthListProto[] = {PR_JS_TAG("SVGLengthList"), PR_SVG_LIST_MEMBERS(Kind_Length)};

constexpr JSCFunctionListEntry kTransformListProto[] = {
    PR_JS_TAG("SVGTransformList"), PR_SVG_LIST_MEMBERS(Kind_Transform),
    PR_JS_METHOD("createSVGTransformFromMatrix", 0, createFromMatrix), PR_JS_METHOD("consolidate", 0, consolidate),
};

constexpr JSCFunctionListEntry kPointListProto[] = {PR_JS_TAG("SVGPointList"), PR_SVG_LIST_MEMBERS(Kind_Point)};

constexpr JSCFunctionListEntry kStringListProto[] = {PR_JS_TAG("SVGStringList"), PR_SVG_LIST_MEMBERS(Kind_String)};

constexpr JsConstant kUnitTypeConstants[] = {
    {"SVG_UNIT_TYPE_UNKNOWN", 0}, {"SVG_UNIT_TYPE_USERSPACEONUSE", PrSvgUnits_UserSpaceOnUse},
    {"SVG_UNIT_TYPE_OBJECTBOUNDINGBOX", PrSvgUnits_ObjectBoundingBox},
};

constexpr JSCFunctionListEntry kUnitTypesProto[] = {PR_JS_TAG("SVGUnitTypes")};

#define PR_SVG_ANIMATED(name, id, proto) {PR_JS_CLASS(name, id, (jsFinalizer<&id, animatedDestroy>), proto)}
#define PR_SVG_VALUE(name, id, proto, constants)                                                                       \
    {PR_JS_CLASS(name, id, (jsFinalizer<&id, valueFinalize>), proto), PR_JS_CONSTANTS(constants)}
#define PR_SVG_LIST(name, id, proto, indexing)                                                                         \
    {PR_JS_CLASS(name, id, (jsFinalizer<&id, listFinalize>), proto), .indexed = &indexing}

constexpr JsClassDefinition kClasses[] = {
    PR_SVG_ANIMATED("SVGAnimatedBoolean", animatedBooleanClass, kAnimatedBooleanProto),
    PR_SVG_ANIMATED("SVGAnimatedEnumeration", animatedEnumerationClass, kAnimatedEnumerationProto),
    PR_SVG_ANIMATED("SVGAnimatedInteger", animatedIntegerClass, kAnimatedIntegerProto),
    PR_SVG_ANIMATED("SVGAnimatedNumber", animatedNumberClass, kAnimatedNumberProto),
    PR_SVG_ANIMATED("SVGAnimatedString", animatedStringClass, kAnimatedStringProto),
    PR_SVG_ANIMATED("SVGAnimatedLength", animatedLengthClass, kAnimatedLengthProto),
    PR_SVG_ANIMATED("SVGAnimatedAngle", animatedAngleClass, kAnimatedAngleProto),
    PR_SVG_ANIMATED("SVGAnimatedRect", animatedRectClass, kAnimatedRectProto),
    PR_SVG_ANIMATED("SVGAnimatedPreserveAspectRatio", animatedPreserveAspectRatioClass,
        kAnimatedPreserveAspectRatioProto),
    PR_SVG_ANIMATED("SVGAnimatedNumberList", animatedNumberListClass, kAnimatedNumberListProto),
    PR_SVG_ANIMATED("SVGAnimatedLengthList", animatedLengthListClass, kAnimatedLengthListProto),
    PR_SVG_ANIMATED("SVGAnimatedTransformList", animatedTransformListClass, kAnimatedTransformListProto),
    PR_SVG_VALUE("SVGLength", lengthClass, kLengthProto, kLengthConstants),
    PR_SVG_VALUE("SVGAngle", angleClass, kAngleProto, kAngleConstants),
    {PR_JS_CLASS("SVGNumber", numberClass, (jsFinalizer<&numberClass, valueFinalize>), kNumberProto)},
    PR_SVG_VALUE("SVGTransform", transformClass, kTransformProto, kTransformConstants),
    PR_SVG_VALUE("SVGPreserveAspectRatio", preserveAspectRatioClass, kPreserveAspectRatioProto,
        kPreserveAspectRatioConstants),
    PR_SVG_LIST("SVGNumberList", numberListClass, kNumberListProto, kNumberIndexed),
    PR_SVG_LIST("SVGLengthList", lengthListClass, kLengthListProto, kLengthIndexed),
    PR_SVG_LIST("SVGTransformList", transformListClass, kTransformListProto, kTransformIndexed),
    PR_SVG_LIST("SVGPointList", pointListClass, kPointListProto, kPointIndexed),
    PR_SVG_LIST("SVGStringList", stringListClass, kStringListProto, kStringIndexed),
    {
        PR_JS_CLASS("SVGUnitTypes", unitTypesClass, jsViewFinalizer<&unitTypesClass>, kUnitTypesProto),
        PR_JS_CONSTANTS(kUnitTypeConstants)
    },
};

// Creates a detached value for a factory of SVGSVGElement.
JSValue detached(JSContext* ctx, JSValueConst svg, Kind kind, const Item& own) {
    if (jsThis<PrSvgElement>(ctx, svg, prJsSvgSvgElementClass) == nullptr)
        return JS_EXCEPTION;
    return valueObject(ctx, JS_UNDEFINED, valueCreate(kind, nullptr, PrSvgAttribute_Unknown, false, own));
}

// The getter of a list that is a member of an element interface itself: requiredExtensions and systemLanguage, and
// points and animatedPoints.
JSValue memberList(JSContext* ctx, JSValueConst elementObject, int member, Kind kind, PrSvgValueType type) {
    PrSvgElement* element = jsThis<PrSvgElement>(ctx, elementObject, prJsSvgElementClass);
    if (element == nullptr)
        return JS_EXCEPTION;
    const Member& reflected = kMembers[member];
    return jsSameObject(ctx, elementObject, reflected.name, [&] {
        if (prSvgElementAttributeType(element, reflected.attribute) != type)
            return JS_UNDEFINED;
        bool animated = member == SvgMember_animatedPoints;
        return listObject(ctx, listClassOf(kind), elementObject,
            listCreate(element, reflected.attribute, kind, animated));
    });
}

} // namespace

JSValue svgAnimatedBoolean(JSContext* ctx, JSValueConst element, int member) {
    return animatedMember(ctx, element, member, animatedBooleanClass, PrSvgValueType_Boolean);
}

JSValue svgAnimatedEnumeration(JSContext* ctx, JSValueConst element, int member) {
    PrSvgValueType type = PrSvgValueType_Enumeration;
    if (kMembers[member].attribute == PrSvgAttribute_Orient)
        type = PrSvgValueType_Orient;
    return animatedMember(ctx, element, member, animatedEnumerationClass, type);
}

JSValue svgAnimatedInteger(JSContext* ctx, JSValueConst element, int member) {
    PrSvgValueType type = kMembers[member].part != 0 ? PrSvgValueType_IntegerPair : PrSvgValueType_Integer;
    return animatedMember(ctx, element, member, animatedIntegerClass, type);
}

JSValue svgAnimatedNumber(JSContext* ctx, JSValueConst element, int member) {
    PrSvgValueType type = kMembers[member].part != 0 ? PrSvgValueType_NumberPair : PrSvgValueType_Number;
    return animatedMember(ctx, element, member, animatedNumberClass, type);
}

JSValue svgAnimatedString(JSContext* ctx, JSValueConst element, int member) {
    return animatedMember(ctx, element, member, animatedStringClass, PrSvgValueType_String);
}

JSValue svgAnimatedLength(JSContext* ctx, JSValueConst element, int member) {
    return animatedMember(ctx, element, member, animatedLengthClass, PrSvgValueType_Length);
}

JSValue svgAnimatedAngle(JSContext* ctx, JSValueConst element, int member) {
    return animatedMember(ctx, element, member, animatedAngleClass, PrSvgValueType_Orient);
}

JSValue svgAnimatedRect(JSContext* ctx, JSValueConst element, int member) {
    return animatedMember(ctx, element, member, animatedRectClass, PrSvgValueType_Rect);
}

JSValue svgAnimatedPreserveAspectRatio(JSContext* ctx, JSValueConst element, int member) {
    return animatedMember(ctx, element, member, animatedPreserveAspectRatioClass, PrSvgValueType_PreserveAspectRatio);
}

JSValue svgAnimatedNumberList(JSContext* ctx, JSValueConst element, int member) {
    return animatedMember(ctx, element, member, animatedNumberListClass, PrSvgValueType_NumberList);
}

JSValue svgAnimatedLengthList(JSContext* ctx, JSValueConst element, int member) {
    return animatedMember(ctx, element, member, animatedLengthListClass, PrSvgValueType_LengthList);
}

JSValue svgAnimatedTransformList(JSContext* ctx, JSValueConst element, int member) {
    return animatedMember(ctx, element, member, animatedTransformListClass, PrSvgValueType_TransformList);
}

JSValue svgStringList(JSContext* ctx, JSValueConst element, int member) {
    return memberList(ctx, element, member, Kind_String, PrSvgValueType_StringList);
}

JSValue svgPointList(JSContext* ctx, JSValueConst element, int member) {
    return memberList(ctx, element, member, Kind_Point, PrSvgValueType_PointList);
}

JSValue svgCreateNumber(JSContext* ctx, JSValueConst svg, int, JSValueConst*) {
    return detached(ctx, svg, Kind_Number, Item{});
}

JSValue svgCreatePoint(JSContext* ctx, JSValueConst svg, int, JSValueConst*) {
    Item item{};
    item.point = PrDomPoint{0, 0, 0, 1};
    return detached(ctx, svg, Kind_Point, item);
}

JSValue svgCreateLength(JSContext* ctx, JSValueConst svg, int, JSValueConst*) {
    Item item{};
    item.length = PrSvgLength{.value = 0, .unit = PrCssUnit_Number};
    return detached(ctx, svg, Kind_Length, item);
}

JSValue svgCreateAngle(JSContext* ctx, JSValueConst svg, int, JSValueConst*) {
    Item item{};
    item.angle = PrSvgAngle{.value = 0, .unit = PrCssUnit_Number};
    return detached(ctx, svg, Kind_Angle, item);
}

JSValue svgCreateTransform(JSContext* ctx, JSValueConst svg, int, JSValueConst*) {
    return detached(ctx, svg, Kind_Transform, transformItem(prSvgTransformFromMatrix(prDomMatrixIdentity())));
}

JSValue svgCreateTransformFromMatrix(JSContext* ctx, JSValueConst svg, int argc, JSValueConst* argv) {
    if (jsThis<PrSvgElement>(ctx, svg, prJsSvgSvgElementClass) == nullptr)
        return JS_EXCEPTION;
    return transformFromMatrix(ctx, argc > 0 ? argv[0] : JS_UNDEFINED);
}

bool svgAngleOf(JSContext* ctx, JSValueConst value, PrSvgAngle* out) {
    Value* angle = valueThis(ctx, value, angleClass);
    if (angle == nullptr)
        return false;
    *out = valueOf(angle).angle;
    return true;
}

void svgTypesBind(JSContext* ctx) {
    for (const JsClassDefinition& definition : kClasses)
        JS_FreeValue(ctx, jsDefineClass(ctx, definition));
}

} // namespace pr_impl
