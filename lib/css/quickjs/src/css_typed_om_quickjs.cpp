// JavaScript binding of the CSS Typed OM.
//
// css-typed-om-1 - https://drafts.css-houdini.org/css-typed-om-1/
//
// A Typed OM object holds its own fields. The objects that it holds are its wrapper's references, so script sees the
// same objects that it stored. The engine's values are PrCssValue. A member that computes converts the objects into a
// PrCssValue and calls C. A value that C returns is reified into new objects according to the rules of the interfaces.

#include "pr/css_typed_om_quickjs.h"

#include "pr/alloc.h"
#include "pr/color_space.h"
#include "pr/count_of.h"
#include "pr/dom_geometry_quickjs.h"
#include "pr/float.h"
#include "pr/mem.h"
#include "pr/quickjs_binding.h"
#include "pr/string_ops.h"
#include "pr/text_buffer.h"
#include "pr/webidl_observable_array_quickjs.h"
#include "pr/webidl_quickjs.h"

namespace pr_impl {

namespace {

JSClassID cssStyleValueClass;
JSClassID cssKeywordValueClass;
JSClassID cssUnparsedValueClass;
JSClassID cssVariableReferenceValueClass;
JSClassID cssNumericValueClass;
JSClassID cssUnitValueClass;
JSClassID cssMathValueClass;
JSClassID cssMathSumClass;
JSClassID cssMathProductClass;
JSClassID cssMathNegateClass;
JSClassID cssMathInvertClass;
JSClassID cssMathMinClass;
JSClassID cssMathMaxClass;
JSClassID cssMathClampClass;
JSClassID cssNumericArrayClass;
JSClassID cssTransformValueClass;
JSClassID cssTransformComponentClass;
JSClassID cssTranslateClass;
JSClassID cssRotateClass;
JSClassID cssScaleClass;
JSClassID cssSkewClass;
JSClassID cssSkewXClass;
JSClassID cssSkewYClass;
JSClassID cssPerspectiveClass;
JSClassID cssMatrixComponentClass;
JSClassID cssImageValueClass;
JSClassID cssColorValueClass;
JSClassID cssRgbClass;
JSClassID cssHslClass;
JSClassID cssHwbClass;
JSClassID cssLabClass;
JSClassID cssLchClass;
JSClassID cssOklabClass;
JSClassID cssOklchClass;
JSClassID cssColorClass;
JSClassID stylePropertyMapReadOnlyClass;

// ---------------------------------------------------------------------------------------------------------------------
// Objects

// The interface of an object, which determines what it holds. A numeric value is an object of a kind from Unit to
// Clamp, and a transform component is an object of a kind from Translate to Matrix.
enum {
    TypedKind_Style,               // CSSStyleValue: a value that no subclass describes, held in `value`
    TypedKind_Keyword,             // CSSKeywordValue, with its value in `text`
    TypedKind_Unparsed,            // CSSUnparsedValue, whose segments are strings and variable references

    // CSSVariableReferenceValue. `text` is the custom property's name, and the fallback is an unparsed value or none.
    TypedKind_VariableReference,

    TypedKind_Unit,                // CSSUnitValue, with `number` in `unit`
    TypedKind_Sum,                 // CSSMathSum, of one or more numeric values in a CSSNumericArray
    TypedKind_Product,             // CSSMathProduct, likewise
    TypedKind_Negate,              // CSSMathNegate, of one numeric value
    TypedKind_Invert,              // CSSMathInvert, of one numeric value
    TypedKind_Min,                 // CSSMathMin, of one or more numeric values in a CSSNumericArray
    TypedKind_Max,                 // CSSMathMax, likewise
    TypedKind_Clamp,               // CSSMathClamp: its lower bound, value and upper bound
    TypedKind_Transform,           // CSSTransformValue, of its components
    TypedKind_Translate,           // CSSTranslate: x, y and z
    TypedKind_Rotate,              // CSSRotate: x, y, z and the angle
    TypedKind_Scale,               // CSSScale: x, y and z
    TypedKind_Skew,                // CSSSkew: ax and ay
    TypedKind_SkewX,               // CSSSkewX: ax
    TypedKind_SkewY,               // CSSSkewY: ay
    TypedKind_Perspective,         // CSSPerspective: its length, a numeric value or the keyword none
    TypedKind_Matrix,              // CSSMatrixComponent: its DOMMatrix
    TypedKind_Image,               // CSSImageValue, which `value` holds

    // A CSSColorValue written with `colorFunction`. It holds its channels and its alpha or, for a CSSColor, its color
    // space, observable array of channels, and alpha.
    TypedKind_Color,

    TypedKind_NumericArray,        // CSSNumericArray, of numeric values
};
typedef uint8_t TypedKind;

// The function that a color value is written with, which determines its interface (css-typed-om-1/#colorvalue-objects).
// The interface is CSSColor for color(), and otherwise the interface named after the function.
enum {
    ColorFunction_Rgb,
    ColorFunction_Hsl,
    ColorFunction_Hwb,
    ColorFunction_Lab,
    ColorFunction_Lch,
    ColorFunction_Oklab,
    ColorFunction_Oklch,
    ColorFunction_Color,
};
typedef uint8_t ColorFunction;

// What a channel of a color value holds besides a keyword, and how a number given for it is interpreted
// (css-typed-om-1/#typedefdef-csscolorrgbcomp and the types after it).
enum {
    ChannelType_RgbComp,   // a number or a percentage, where a given number is a fraction
    ChannelType_Percent,   // a percentage, where a given number is a fraction
    ChannelType_Number,    // a number
    ChannelType_Angle,     // an angle, where a given number is in degrees
};
typedef uint8_t ChannelType;

// The class of each kind of object.
constexpr const JSClassID* kKindClasses[] = {
    &cssStyleValueClass,               // Style
    &cssKeywordValueClass,             // Keyword
    &cssUnparsedValueClass,            // Unparsed
    &cssVariableReferenceValueClass,   // VariableReference
    &cssUnitValueClass,                // Unit
    &cssMathSumClass,                  // Sum
    &cssMathProductClass,              // Product
    &cssMathNegateClass,               // Negate
    &cssMathInvertClass,               // Invert
    &cssMathMinClass,                  // Min
    &cssMathMaxClass,                  // Max
    &cssMathClampClass,                // Clamp
    &cssTransformValueClass,           // Transform
    &cssTranslateClass,                // Translate
    &cssRotateClass,                   // Rotate
    &cssScaleClass,                    // Scale
    &cssSkewClass,                     // Skew
    &cssSkewXClass,                    // SkewX
    &cssSkewYClass,                    // SkewY
    &cssPerspectiveClass,              // Perspective
    &cssMatrixComponentClass,          // Matrix
    &cssImageValueClass,               // Image
    nullptr,                           // Color, whose class is named by its function
    &cssNumericArrayClass,             // NumericArray
};

// The class of a color value of each ColorFunction.
constexpr const JSClassID* kColorClasses[] = {
    &cssRgbClass, &cssHslClass,   &cssHwbClass,   &cssLabClass,
    &cssLchClass, &cssOklabClass, &cssOklchClass, &cssColorClass,
};

// The name of the function that a color value is written with, indexed by ColorFunction.
constexpr PrStringView kColorFunctionNames[] = {"rgb", "hsl",   "hwb",   "lab", "lch", "oklab", "oklch", "color"};

// The types of the channels of a color value before its alpha, indexed by ColorFunction.
constexpr ChannelType kColorChannels[][3] = {
    {ChannelType_RgbComp, ChannelType_RgbComp, ChannelType_RgbComp}, // rgb
    {ChannelType_Angle, ChannelType_Percent, ChannelType_Percent},   // hsl
    {ChannelType_Angle, ChannelType_Percent, ChannelType_Percent},   // hwb
    {ChannelType_Percent, ChannelType_Number, ChannelType_Number},   // lab
    {ChannelType_Percent, ChannelType_Percent, ChannelType_Angle},   // lch
    {ChannelType_Percent, ChannelType_Number, ChannelType_Number},   // oklab
    {ChannelType_Percent, ChannelType_Percent, ChannelType_Angle},   // oklch
    {ChannelType_Percent, ChannelType_Percent, ChannelType_Percent}, // color
};

// Returns the type of the channel at `index` of a color value written with `function`, counting its channels and then
// its alpha.
ChannelType channelType(ColorFunction function, uint32_t index) {
    return index < 3 ? kColorChannels[function][index] : ChannelType_Percent;
}

bool isNumeric(TypedKind kind) {
    return kind >= TypedKind_Unit && kind <= TypedKind_Clamp;
}

bool isComponent(TypedKind kind) {
    return kind >= TypedKind_Translate && kind <= TypedKind_Matrix;
}

// The fields that a Typed OM object holds itself. The objects that it holds are its wrapper's references 0 to
// `references`, as its kind (TypedKind) describes them.
struct TypedObject {
    TypedKind kind;
    PrCssUnit unit;
    ColorFunction colorFunction;
    bool is2D;
    double number;
    PrRawString text;
    PrCssDeclarationName property;   // property that a style value is a value of
    PrCssValue* PR_NULLABLE value;   // trees of a style value or an image
    uint32_t references;
    bool hasType;                    // true if it is a numeric value, whose type is `type`, which
    PrCssNumericType type;           // is checked at creation and never changes afterward
    void* PR_NULLABLE binding;       // the object (JsClassDefinition::slot)
};

TypedObject typedFields(TypedKind kind, PrCssUnit unit = PrCssUnit_Number, double number = 0, bool is2D = false) {
    TypedObject object = {};
    object.kind = kind;
    object.unit = unit;
    object.number = number;
    object.is2D = is2D;
    object.property = prCssDeclarationNameFromProperty(PrCssProperty_Count);
    return object;
}

void destroyTyped(TypedObject* object) {
    prRawStringReset(&object->text);
    prCssDeclarationNameReset(&object->property);
    prCssValueDestroy(object->value);
    prMemoryFree(object);
}

// Returns the slot in which a TypedObject keeps its object, for the classes that kTypedBases names.
void* PR_NULLABLE* PR_NONNULL typedBinding(void* handle) {
    return &static_cast<TypedObject*>(handle)->binding;
}

// The classes whose objects hold a TypedObject. Each includes the classes deriving from it.
constexpr const JSClassID* kTypedBases[] = {
    &cssStyleValueClass,
    &cssVariableReferenceValueClass,
    &cssTransformComponentClass,
    &cssNumericArrayClass,
};

// Those classes and every class that derives from one, which prJsCssTypedOmBind finds. Like the class ids, the set
// belongs to the process.
JsClassSet typedClasses;

TypedObject* PR_NULLABLE typedOf(JSValueConst value) {
    return static_cast<TypedObject*>(jsHandleInSet(value, &typedClasses));
}

TypedObject* PR_NULLABLE typedIn(JSContext* ctx, JSValueConst value, JSClassID base) {
    TypedObject* object = static_cast<TypedObject*>(jsHandleIn(ctx, value, base));
    if (object == nullptr)
        JS_ThrowTypeError(ctx, "Illegal invocation");
    return object;
}

// Writes the type of a unit value in `unit`. Returns false for the resolution alias x, which is not a unit that a Typed
// OM value has, and for a unit that measures no base type.
bool unitType(PrCssUnit unit, PrCssNumericType* out) {
    return unit != PrCssUnit_X && prCssNumericTypeFromUnit(unit, out);
}

// Returns a new object of class `id` that holds `fields` and takes ownership of their strings and value. A unit value's
// type is the type of its unit, however the value is created.
JSValue newTyped(JSContext* ctx, JSClassID id, const TypedObject& fields) {
    TypedObject* object = alloc<TypedObject>();
    *object = fields;
    if (object->kind == TypedKind_Unit)
        object->hasType = unitType(object->unit, &object->type);
    JSValue value = jsWrap(ctx, id, JS_UNDEFINED, object, true);
    if (JS_IsException(value))
        destroyTyped(object);
    return value;
}

// Returns a new unit value.
JSValue newUnit(JSContext* ctx, double number, PrCssUnit unit) {
    return newTyped(ctx, cssUnitValueClass, typedFields(TypedKind_Unit, unit, number));
}

// Returns a new keyword value of `text`.
JSValue newKeyword(JSContext* ctx, PrStringView text) {
    TypedObject fields = typedFields(TypedKind_Keyword);
    fields.text = prRawStringCopy(text);
    return newTyped(ctx, cssKeywordValueClass, fields);
}

// Adds `child` to the objects that `object` holds, taking ownership of it.
bool hold(JSContext* ctx, JSValueConst object, JSValue child) {
    TypedObject* typed = typedOf(object);
    bool ok = !JS_IsException(child);
    if (ok) {
        jsSetReference(ctx, object, typed->references, child);
        ++typed->references;
    }
    JS_FreeValue(ctx, child);
    return ok;
}

TypedObject* PR_NULLABLE styleValueOf(JSContext* ctx, JSValueConst value) {
    TypedObject* object = typedOf(value);
    if (object != nullptr && object->kind != TypedKind_NumericArray)
        return object;
    JS_ThrowTypeError(ctx, "Expected a CSSStyleValue");
    return nullptr;
}

// Calls `visit` with each child of an object in order, with a string segment as a string, until `visit` returns false.
template <typename Visit>
bool eachChild(JSContext* ctx, JSValueConst value, const TypedObject* object, Visit visit) {
    auto held = [&](JSValueConst holder, uint32_t index) {
        JSValue child = jsReference(ctx, holder, index);
        bool ok = JS_IsNull(child) || JS_IsUndefined(child) || visit(child);
        JS_FreeValue(ctx, child);
        return ok;
    };
    bool ok = true;
    switch (object->kind) {
        case TypedKind_Sum:
        case TypedKind_Product:
        case TypedKind_Min:
        case TypedKind_Max: {
            JSValue values = jsReference(ctx, value, 0);
            const TypedObject* array = typedOf(values);
            for (uint32_t i = 0; ok && array != nullptr && i < array->references; ++i)
                ok = held(values, i);
            JS_FreeValue(ctx, values);
            return ok;
        }
        case TypedKind_Color:
            if (object->colorFunction != ColorFunction_Color)
                break;
            ok = held(value, 0);
            if (ok) {
                JSValue channels = jsReference(ctx, value, 1);
                for (uint32_t i = 0; ok && i < jsObservableArrayLength(ctx, channels); ++i) {
                    JSValue channel = jsObservableArrayItem(ctx, channels, i);
                    ok = visit(channel);
                    JS_FreeValue(ctx, channel);
                }
                JS_FreeValue(ctx, channels);
            }
            return ok && held(value, 2);
        case TypedKind_Matrix:
            return true;
        default:
            break;
    }
    for (uint32_t i = 0; ok && i < object->references; ++i)
        ok = held(value, i);
    return ok;
}

// Objects that the call's scope holds and frees: the children of an object, as eachChild visits them, or objects that a
// caller creates.
struct Values {
    JSContext* ctx;
    RawArray<JSValue>* values;

    bool read(JSValueConst value, const TypedObject* object) {
        return eachChild(ctx, value, object, [&](JSValueConst child) {
            values->push(JS_DupValue(ctx, child));
            return true;
        });
    }

    // Takes `value`, and returns true if it is an object. Returns false for JS_UNDEFINED and JS_EXCEPTION.
    bool add(JSValue value) {
        if (JS_IsException(value) || JS_IsUndefined(value))
            return false;
        values->push(value);
        return true;
    }

    // The objects, which the caller then owns. The array stays valid for the call.
    JSValue* take() {
        values->count = 0;
        return values->data;
    }
};

Values heldValues(JSContext* ctx) {
    RawArray<JSValue>* values = allocZeroed<RawArray<JSValue>>();
    auto release = [](JSContext* context, void* held) {
        RawArray<JSValue>* values = static_cast<RawArray<JSValue>*>(held);
        for (uint32_t i = 0; i < values->count; ++i)
            JS_FreeValue(context, values->data[i]);
        values->reset();
        prMemoryFree(values);
    };
    jsHoldCustom(ctx, release, values);
    return Values{ctx, values};
}

// ---------------------------------------------------------------------------------------------------------------------
// Values
//
// The PrCssValue objects that the binding creates to pass to C, and those that C writes for it. They are taken from the
// runtime's pool and returned to it.

struct TreePool {
    RawArray<PrCssValue*> free;
};

// The number of values that a pool keeps, and the most nodes that a kept value may hold. A pool keeps as many values as
// a call uses, which can nest several deep when a call runs script, each no larger than an ordinary call grows it. The
// others are destroyed when released.
constexpr uint32_t kTreeKeptValues = 4;
constexpr uint32_t kTreeKeptNodes = 256;

constexpr char kTreePoolKey = 0;

TreePool* treePoolOf(JSContext* ctx) {
    auto make = []() -> void* { return allocZeroed<TreePool>(); };
    auto destroy = [](void* held) {
        TreePool* pool = static_cast<TreePool*>(held);
        for (uint32_t i = 0; i < pool->free.count; ++i)
            prCssValueDestroy(pool->free.data[i]);
        pool->free.reset();
        prMemoryFree(pool);
    };
    return static_cast<TreePool*>(jsRuntimeOwned(JS_GetRuntime(ctx), &kTreePoolKey, make, destroy));
}

// A value that the call's scope holds, and returns to the pool.
struct Tree {
    PrCssValue* value;
};

Tree heldTree(JSContext* ctx) {
    TreePool* pool = treePoolOf(ctx);
    PrCssValue* value = pool->free.count != 0 ? pool->free.data[--pool->free.count] : prCssValueCreate();
    auto release = [](JSContext* context, void* held) {
        PrCssValue* value = static_cast<PrCssValue*>(held);
        TreePool* pool = treePoolOf(context);
        if (pool->free.count == kTreeKeptValues || prCssValueSize(value) > kTreeKeptNodes) {
            prCssValueDestroy(value);
            return;
        }
        prCssValueClear(value);
        pool->free.push(value);
    };
    jsHoldCustom(ctx, release, value);
    return Tree{value};
}

// Throws the error for a status from building a value: a RangeError for a value that is too deep, such as a
// self-containing value, and a TypeError for one that no value can represent.
bool built(JSContext* ctx, PrStatus status) {
    if (status == PrStatus_OutOfRange)
        JS_ThrowRangeError(ctx, "The value is nested too deeply");
    else if (status != PrStatus_Ok)
        JS_ThrowTypeError(ctx, "Invalid value");
    return status == PrStatus_Ok;
}

bool addNode(JSContext* ctx, PrCssValue* out, const PrCssValueFields& fields) {
    return built(ctx, prCssValueAdd(out, &fields));
}

bool openNode(JSContext* ctx, PrCssValue* out, const PrCssValueFields& fields) {
    return built(ctx, prCssValueOpen(out, &fields));
}

bool closeNode(JSContext* ctx, PrCssValue* out) {
    return built(ctx, prCssValueClose(out));
}

// Returns the node of a numeric value of `number` in `unit`.
PrCssValueFields numericFields(double number, PrCssUnit unit) {
    PrCssValueFields fields = {};
    if (unit == PrCssUnit_Number)
        fields.kind = PrCssValueKind_Number;
    else if (unit == PrCssUnit_Percent)
        fields.kind = PrCssValueKind_Percentage;
    else
        fields.kind = PrCssValueKind_Dimension;
    fields.number.value = number;
    fields.number.unit = unit;
    return fields;
}

PrCssValueFields keywordFields(PrCssKeyword keyword) {
    PrCssValueFields fields = {};
    fields.kind = PrCssValueKind_Keyword;
    fields.keyword = keyword;
    return fields;
}

PrCssValueFields functionFields(PrCssKeyword name) {
    PrCssValueFields fields = {};
    fields.kind = PrCssValueKind_Function;
    fields.keyword = name;
    return fields;
}

PrCssValueFields literalFields(char character) {
    PrCssValueFields fields = {};
    fields.kind = PrCssValueKind_Literal;
    fields.literal = uint8_t(character);
    return fields;
}

// Returns the node of a keyword value: the keyword that its value names, or an ident if it names none.
PrCssValueFields identFields(PrStringView text) {
    PrCssKeyword keyword = prCssKeywordFromText(text);
    if (keyword != PrCssKeyword_Unknown)
        return keywordFields(keyword);
    PrCssValueFields fields = {};
    fields.kind = PrCssValueKind_Ident;
    fields.text = text;
    return fields;
}

// ---------------------------------------------------------------------------------------------------------------------
// Types (css-typed-om-1/#numeric-typing)

// Returns true if a type's only nonzero entry is `base` to the first power or, for PrCssNumericBaseType_Count, if it
// has no nonzero entry. There is no percent hint, except `hint` if `hint` is allowed.
bool typeIs(const PrCssNumericType& type, uint32_t base, bool hint = false) {
    for (uint32_t i = 0; i < PrCssNumericBaseType_Count; ++i) {
        if (type.exponents[i] != (i == base ? 1 : 0))
            return false;
    }
    return !type.hasPercentHint || (hint && type.percentHint == PrCssNumericBaseType(base));
}

// The information about an object that is needed to check its parent (css-typed-om-1/#cssstylevalue-match-a-grammar):
// its kind, its type if it is a numeric value with one, and a keyword's value.
struct Summary {
    TypedKind kind;
    bool string;              // a string segment
    bool hasType;
    PrCssNumericType type;
    PrStringView keyword;
};

bool matchesType(const Summary& value, uint32_t base, bool hint = false) {
    return value.hasType && typeIs(value.type, base, hint);
}

bool matchesLength(const Summary& value) {
    return matchesType(value, PrCssNumericBaseType_Length);
}

bool matchesLengthPercentage(const Summary& value) {
    return matchesType(value, PrCssNumericBaseType_Length, true) || matchesType(value, PrCssNumericBaseType_Percent);
}

bool matchesNumber(const Summary& value) {
    return matchesType(value, PrCssNumericBaseType_Count);
}

bool matchesAngle(const Summary& value) {
    return matchesType(value, PrCssNumericBaseType_Angle);
}

bool matchesPercentage(const Summary& value) {
    return matchesType(value, PrCssNumericBaseType_Percent);
}

// Writes the summary of a child, with its keyword's view pointing into the object. Returns false, with the exception
// thrown, for a value that is not a Typed OM object.
bool summaryOf(JSContext* ctx, JSValueConst value, Summary* out) {
    *out = Summary{};
    if (JS_IsString(value)) {
        out->string = true;
        return true;
    }
    const TypedObject* object = styleValueOf(ctx, value);
    if (object == nullptr)
        return false;
    out->kind = object->kind;
    out->hasType = object->hasType;
    out->type = object->type;
    if (object->kind == TypedKind_Keyword)
        out->keyword = prRawStringView(&object->text);
    return true;
}

// Returns true if a channel of `type` accepts the value (css-typed-om-1/#rectify-a-csscolorrgbcomp and the steps after
// it). A hue accepts any keyword, although css-typed-om-1/#rectify-a-csscolorangle accepts none alone.
bool channelValid(const Summary& value, ChannelType type) {
    if (value.kind == TypedKind_Keyword && !value.string)
        return type == ChannelType_Angle || prAsciiEqualsFold(value.keyword, "none");
    switch (type) {
        case ChannelType_RgbComp: return matchesNumber(value) || matchesPercentage(value);
        case ChannelType_Percent: return matchesPercentage(value);
        case ChannelType_Number:  return matchesNumber(value);
        default:                  return matchesAngle(value);
    }
}

// Writes the type of a math value of `kind` over operands with the types of `children`
// (css-typed-om-1/#cssnumericvalue-type).
bool mathType(TypedKind kind, const Summary* children, uint32_t count, PrCssNumericType* out) {
    for (uint32_t i = 0; i < count; ++i) {
        if (!children[i].hasType)
            return false;
    }
    switch (kind) {
        case TypedKind_Negate:
            *out = children[0].type;
            return true;
        case TypedKind_Invert:
            *out = prCssNumericTypeInvert(&children[0].type);
            return true;
        case TypedKind_Product:
            *out = children[0].type;
            for (uint32_t i = 1; i < count; ++i) {
                if (!prCssNumericTypeMultiply(out, &children[i].type, out))
                    return false;
            }
            return true;
        default:
            *out = children[0].type;
            for (uint32_t i = 1; i < count; ++i) {
                if (!prCssNumericTypeAdd(out, &children[i].type, out))
                    return false;
            }
            return true;
    }
}

// Returns true if an object of `fields`, over children with the given summaries, is allowed by its interface's
// constructor and setters. For example, the operands of a math value must be numeric values whose types add or
// multiply, a translation's arguments must be lengths and percentages, and a rotation's angle must be an angle. Throws
// a TypeError if not, or a SyntaxError for a color value's channel whose type does not accept it. Returns the object's
// type if it has one.
bool childrenValid(JSContext* ctx, const TypedObject& fields, const Summary* children,
        uint32_t count, bool* hasType, PrCssNumericType* type) {
    *hasType = false;
    auto numeric = [&](uint32_t first) {
        for (uint32_t i = first; i < count; ++i) {
            if (children[i].string || !isNumeric(children[i].kind))
                return false;
        }
        return true;
    };
    bool ok = true;
    switch (fields.kind) {
        case TypedKind_Keyword:
            ok = prRawStringView(&fields.text).size != 0;
            break;
        case TypedKind_VariableReference:
            ok = prStringStartsWith(prRawStringView(&fields.text), "--") &&
                (count == 0 || (!children[0].string && children[0].kind == TypedKind_Unparsed));
            break;
        case TypedKind_Unparsed:
            for (uint32_t i = 0; ok && i < count; ++i)
                ok = children[i].string || children[i].kind == TypedKind_VariableReference;
            break;
        case TypedKind_Unit:
            ok = unitType(fields.unit, type);
            *hasType = ok;
            break;
        case TypedKind_Sum:
        case TypedKind_Product:
        case TypedKind_Min:
        case TypedKind_Max:
        case TypedKind_Negate:
        case TypedKind_Invert:
        case TypedKind_Clamp: {
            bool single = fields.kind == TypedKind_Negate || fields.kind == TypedKind_Invert;
            uint32_t wanted = fields.kind == TypedKind_Clamp ? 3 : single ? 1 : 0;
            ok = count != 0 && (wanted == 0 || count == wanted) && numeric(0) && mathType(fields.kind, children, count, type);
            *hasType = ok;
            break;
        }
        case TypedKind_Transform:
            for (uint32_t i = 0; ok && i < count; ++i)
                ok = !children[i].string && isComponent(children[i].kind);
            break;
        case TypedKind_Translate:
            ok = count == 3 && numeric(0) && matchesLengthPercentage(children[0]) &&
                matchesLengthPercentage(children[1]) && matchesLength(children[2]);
            break;
        case TypedKind_Rotate:
            ok = count == 4 && numeric(0);
            for (uint32_t i = 0; ok && i < 4; ++i)
                ok = i < 3 ? matchesNumber(children[i]) : matchesAngle(children[i]);
            break;
        case TypedKind_Scale:
            ok = count == 3 && numeric(0);
            for (uint32_t i = 0; ok && i < count; ++i)
                ok = matchesNumber(children[i]);
            break;
        case TypedKind_Skew:
        case TypedKind_SkewX:
        case TypedKind_SkewY:
            ok = count == (fields.kind == TypedKind_Skew ? 2u : 1u) && numeric(0);
            for (uint32_t i = 0; ok && i < count; ++i)
                ok = matchesAngle(children[i]);
            break;
        case TypedKind_Perspective:
            ok = count == 1 && !children[0].string && (matchesLength(children[0]) ||
                (children[0].kind == TypedKind_Keyword && prAsciiEqualsFold(children[0].keyword, "none")));
            break;
        case TypedKind_Color: {
            // A color() value's color space is not a channel.
            uint32_t first = fields.colorFunction == ColorFunction_Color ? 1 : 0;
            if (fields.colorFunction == ColorFunction_Color)
                ok = count >= 2 && children[0].kind == TypedKind_Keyword;
            else
                ok = count == 4;
            for (uint32_t i = first; ok && i < count; ++i) {
                ok = !children[i].string && (isNumeric(children[i].kind) || children[i].kind == TypedKind_Keyword);
                if (ok && !channelValid(children[i], channelType(fields.colorFunction, i - first))) {
                    jsDomExceptionThrow(ctx, "SyntaxError");
                    return false;
                }
            }
            break;
        }
        default:
            break;
    }
    if (!ok)
        JS_ThrowTypeError(ctx, "Invalid value");
    return ok;
}

// Returns true if the object is allowed by its constructor and setters. Throws a TypeError if not, or the SyntaxError
// for a color value's channel whose type does not accept it. Its children were checked when they were created, and a
// numeric child's type does not change. This check therefore looks only at the children and no deeper, and keeps the
// object's own type for its parent.
bool valid(JSContext* ctx, JSValueConst value) {
    TypedObject* object = typedOf(value);
    RawArray<Summary> children;
    bool ok = eachChild(ctx, value, object, [&](JSValueConst child) {
        Summary summary;
        bool read = summaryOf(ctx, child, &summary);
        if (read)
            children.push(summary);
        return read;
    });
    bool hasType = false;
    PrCssNumericType type = {};
    ok = ok && childrenValid(ctx, *object, children.data, children.count, &hasType, &type);
    children.reset();
    if (!ok)
        return false;
    object->hasType = hasType;
    object->type = type;
    return true;
}

// Returns a new object of `id` that holds `fields` and `children`, which it takes, if its constructor allows it.
JSValue construct(JSContext* ctx, JSValueConst newTarget, JSClassID id, const TypedObject& fields,
        JSValue* PR_NULLABLE children = nullptr, uint32_t count = 0) {
    JSValue object = newTyped(ctx, id, fields);
    bool ok = !JS_IsException(object);
    for (uint32_t i = 0; i < count; ++i) {
        if (ok)
            ok = hold(ctx, object, children[i]);
        else
            JS_FreeValue(ctx, children[i]);
    }
    if (ok && !valid(ctx, object))
        ok = false;
    if (!ok) {
        JS_FreeValue(ctx, object);
        return JS_EXCEPTION;
    }
    return JS_IsUndefined(newTarget) ? object : jsConstructed(ctx, newTarget, object);
}

// Returns a new math value of `kind` over `operands`, which it takes. For a kind that takes a list, the operands are in
// a CSSNumericArray.
JSValue newMath(JSContext* ctx, TypedKind kind, JSValue* operands, uint32_t count,
        JSValueConst newTarget = JS_UNDEFINED) {
    if (kind != TypedKind_Sum && kind != TypedKind_Product && kind != TypedKind_Min && kind != TypedKind_Max)
        return construct(ctx, newTarget, *kKindClasses[kind], typedFields(kind), operands, count);
    JSValue array = newTyped(ctx, cssNumericArrayClass, typedFields(TypedKind_NumericArray));
    for (uint32_t i = 0; i < count; ++i) {
        if (JS_IsException(array) || !hold(ctx, array, operands[i])) {
            for (uint32_t j = i + 1; j < count; ++j)
                JS_FreeValue(ctx, operands[j]);
            JS_FreeValue(ctx, array);
            return JS_EXCEPTION;
        }
    }
    if (JS_IsException(array))
        return array;
    return construct(ctx, newTarget, *kKindClasses[kind], typedFields(kind), &array, 1);
}

// How an argument is converted into an object: a number becomes a CSSUnitValue
// (css-typed-om-1/#rectify-a-numberish-value), and a string becomes a CSSKeywordValue
// (css-typed-om-1/#rectify-a-keywordish-value).
enum {
    Rectify_None,
    Rectify_Numberish,
    Rectify_Keywordish,
};
typedef uint8_t Rectify;

// Returns the object that an argument stands for.
JSValue rectified(JSContext* ctx, JSValueConst value, Rectify rectify) {
    if (rectify == Rectify_Numberish && JS_IsNumber(value)) {
        double number = 0;
        JS_ToFloat64(ctx, &number, value);
        return newUnit(ctx, number, PrCssUnit_Number);
    }
    if (rectify == Rectify_Keywordish && JS_IsString(value)) {
        StringArgument text = {ctx};
        if (!text.read(value))
            return JS_EXCEPTION;
        return newKeyword(ctx, text.value());
    }
    if (styleValueOf(ctx, value) == nullptr)
        return JS_EXCEPTION;
    return JS_DupValue(ctx, value);
}

// ---------------------------------------------------------------------------------------------------------------------
// Objects to values
//
// A unit value becomes a Number, a Percentage or a Dimension. A math value becomes a Math over its calculation, whose
// operands are the nodes of its tree. A transform component becomes the function it is written as, and a color value
// becomes a Color. A style value becomes the trees it holds.

// Appends a numeric value as a node of a calculation tree: a Calc node over its operands, a min(), max() or clamp()
// over its arguments, or a numeric value.
bool flattenCalculation(JSContext* ctx, JSValueConst value, PrCssValue* out) {
    const TypedObject* object = typedOf(value);
    if (object == nullptr || !isNumeric(object->kind)) {
        JS_ThrowTypeError(ctx, "Expected a CSSNumericValue");
        return false;
    }
    if (object->kind == TypedKind_Unit)
        return addNode(ctx, out, numericFields(object->number, object->unit));
    bool function = object->kind == TypedKind_Min || object->kind == TypedKind_Max || object->kind == TypedKind_Clamp;
    PrCssValueFields fields = {};
    if (function) {
        PrCssKeyword name = PrCssKeyword_Clamp;
        if (object->kind == TypedKind_Min)
            name = PrCssKeyword_Min;
        else if (object->kind == TypedKind_Max)
            name = PrCssKeyword_Max;
        fields = functionFields(name);
    } else {
        fields.kind = PrCssValueKind_Calc;
        if (object->kind == TypedKind_Sum)
            fields.calc = PrCssCalc_Sum;
        else if (object->kind == TypedKind_Product)
            fields.calc = PrCssCalc_Product;
        else if (object->kind == TypedKind_Negate)
            fields.calc = PrCssCalc_Negate;
        else
            fields.calc = PrCssCalc_Invert;
    }
    if (!openNode(ctx, out, fields))
        return false;
    bool first = true;
    bool ok = eachChild(ctx, value, object, [&](JSValueConst child) {
        if (function && !first && !addNode(ctx, out, literalFields(',')))
            return false;
        first = false;
        return flattenCalculation(ctx, child, out);
    });
    return ok && closeNode(ctx, out);
}

// Appends a numeric value as a value: a unit value alone or, for a math value, a Math over its tree.
bool flattenNumeric(JSContext* ctx, JSValueConst value, PrCssValue* out) {
    const TypedObject* object = typedOf(value);
    if (object == nullptr || object->kind == TypedKind_Unit || !isNumeric(object->kind))
        return flattenCalculation(ctx, value, out);
    PrCssValueFields math = {};
    math.kind = PrCssValueKind_Math;
    return openNode(ctx, out, math) && flattenCalculation(ctx, value, out) && closeNode(ctx, out);
}

// Appends a component's argument, which is a numeric value or a keyword.
bool flattenArgument(JSContext* ctx, JSValueConst value, PrCssValue* out) {
    const TypedObject* object = typedOf(value);
    if (object != nullptr && object->kind == TypedKind_Keyword)
        return addNode(ctx, out, identFields(prRawStringView(&object->text)));
    return flattenNumeric(ctx, value, out);
}

// Appends the function `name` over `count` arguments of `children` starting at `first`, with a ',' between each pair.
bool flattenFunction(JSContext* ctx, PrCssKeyword name, const Values& children, uint32_t first,
        uint32_t count, PrCssValue* out) {
    if (!openNode(ctx, out, functionFields(name)))
        return false;
    for (uint32_t i = first; i < first + count; ++i) {
        if (i != first && !addNode(ctx, out, literalFields(',')))
            return false;
        if (!flattenArgument(ctx, children.values->data[i], out))
            return false;
    }
    return closeNode(ctx, out);
}

// The DOMMatrix of a matrix component, which its object holds.
const PrDomMatrix* PR_NULLABLE heldMatrix(JSContext* ctx, JSValueConst value) {
    JSValue held = jsReference(ctx, value, 0);
    const PrDomMatrix* matrix = prJsDomMatrixFromValue(ctx, held);
    JS_FreeValue(ctx, held);
    return matrix;
}

// Appends a transform component as the function that it is written as (css-typed-om-1/#transformvalue-serialization).
bool flattenComponent(JSContext* ctx, JSValueConst value, PrCssValue* out) {
    const TypedObject* object = typedOf(value);
    if (object == nullptr || !isComponent(object->kind)) {
        JS_ThrowTypeError(ctx, "Expected a CSSTransformComponent");
        return false;
    }
    bool flat = object->is2D;
    if (object->kind == TypedKind_Matrix) {
        const PrDomMatrix* matrix = heldMatrix(ctx, value);
        if (matrix == nullptr)
            return false;
        constexpr uint32_t k2D[] = {0, 1, 4, 5, 12, 13};
        if (!openNode(ctx, out, functionFields(flat ? PrCssKeyword_Matrix : PrCssKeyword_Matrix3d)))
            return false;
        for (uint32_t i = 0; i < (flat ? 6u : 16u); ++i) {
            if ((i != 0 && !addNode(ctx, out, literalFields(','))) || !addNode(ctx, out,
                    numericFields(matrix->m[flat ? k2D[i] : i], PrCssUnit_Number)))
                return false;
        }
        return closeNode(ctx, out);
    }
    Values children = heldValues(ctx);
    if (!children.read(value, object))
        return false;
    switch (object->kind) {
        case TypedKind_Translate:
            return flattenFunction(ctx, flat ? PrCssKeyword_Translate : PrCssKeyword_Translate3d, children, 0, flat ? 2 : 3, out);
        case TypedKind_Rotate:
            return flattenFunction(ctx, flat ? PrCssKeyword_Rotate : PrCssKeyword_Rotate3d, children, flat ? 3 : 0,
                flat ? 1 : 4, out);
        case TypedKind_Scale:
            return flattenFunction(ctx, flat ? PrCssKeyword_Scale : PrCssKeyword_Scale3d, children, 0, flat ? 2 : 3, out);
        case TypedKind_Skew:
            return flattenFunction(ctx, PrCssKeyword_Skew, children, 0, 2, out);
        case TypedKind_SkewX:
            return flattenFunction(ctx, PrCssKeyword_Skewx, children, 0, 1, out);
        case TypedKind_SkewY:
            return flattenFunction(ctx, PrCssKeyword_Skewy, children, 0, 1, out);
        default:
            return flattenFunction(ctx, PrCssKeyword_Perspective, children, 0, 1, out);
    }
}

// Writes the color space that a CSSColor names. Returns false if it names none.
bool colorSpaceOf(PrStringView name, PrColorSpace* out) {
    for (PrColorSpace space = 0; space < PrColorSpace_Count; ++space) {
        if (prAsciiEqualsFold(name, prColorSpaceName(space))) {
            *out = space;
            return true;
        }
    }
    return false;
}

// Appends a color value as its Color.
bool flattenColor(JSContext* ctx, JSValueConst value, const TypedObject* object, PrCssValue* out) {
    Values children = heldValues(ctx);
    if (!children.read(value, object))
        return false;
    PrCssValueFields color = {};
    color.kind = PrCssValueKind_Color;
    uint32_t first = 0;
    switch (object->colorFunction) {
        case ColorFunction_Rgb:
            color.color.space = PrColorSpace_Srgb;
            color.color.legacy = true;
            break;
        case ColorFunction_Hsl:   color.color.space = PrColorSpace_Hsl; break;
        case ColorFunction_Hwb:   color.color.space = PrColorSpace_Hwb; break;
        case ColorFunction_Lab:   color.color.space = PrColorSpace_Lab; break;
        case ColorFunction_Lch:   color.color.space = PrColorSpace_Lch; break;
        case ColorFunction_Oklab: color.color.space = PrColorSpace_Oklab; break;
        case ColorFunction_Oklch: color.color.space = PrColorSpace_Oklch; break;
        default: {
            const TypedObject* space = typedOf(children.values->data[0]);
            if (space == nullptr || !colorSpaceOf(prRawStringView(&space->text), &color.color.space)) {
                JS_ThrowTypeError(ctx, "Not a color space");
                return false;
            }
            first = 1;
            break;
        }
    }
    if (!openNode(ctx, out, color))
        return false;
    for (uint32_t i = first; i < children.values->count; ++i) {
        if (!flattenArgument(ctx, children.values->data[i], out))
            return false;
    }
    return closeNode(ctx, out);
}

// Appends the text of an object (css-typed-om-1/#stylevalue-serialization).
bool serializeValue(JSContext* ctx, JSValueConst value, PrTextBuffer* out);

// Appends a style value as its trees: the trees that it holds, or what its interface serializes it as. Returns false,
// with the exception thrown, for a value that no PrCssValue can represent.
bool flattenStyle(JSContext* ctx, JSValueConst value, PrCssValue* out) {
    const TypedObject* object = styleValueOf(ctx, value);
    if (object == nullptr)
        return false;
    switch (object->kind) {
        case TypedKind_Style:
        case TypedKind_Image:
            for (uint32_t tree = 0; tree < prCssValueSize(object->value); tree = prCssValueNext(object->value, tree)) {
                if (!built(ctx, prCssValueAddTree(out, object->value, tree)))
                    return false;
            }
            return true;
        case TypedKind_Keyword:
            return addNode(ctx, out, identFields(prRawStringView(&object->text)));
        case TypedKind_Unparsed: {
            PrTextBuffer text = {};
            bool ok = serializeValue(ctx, value, &text);
            PrCssValueFields tokens = {};
            tokens.kind = PrCssValueKind_Tokens;
            tokens.text = prTextBufferView(&text);
            ok = ok && addNode(ctx, out, tokens);
            prTextBufferReset(&text);
            return ok;
        }
        case TypedKind_Transform:
            return eachChild(ctx, value, object, [&](JSValueConst child) { return flattenComponent(ctx, child, out); });
        case TypedKind_Color:
            return flattenColor(ctx, value, object, out);
        default:
            if (isComponent(object->kind))
                return flattenComponent(ctx, value, out);
            if (isNumeric(object->kind))
                return flattenNumeric(ctx, value, out);
            JS_ThrowTypeError(ctx, "Invalid value");
            return false;
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Serialization (css-typed-om-1/#stylevalue-serialization)

// The state of the serializer: the output, the value used to write numbers, and the current depth.
struct Serializer {
    JSContext* ctx;
    PrTextBuffer* out;
    PrCssValue* scratch;
    uint32_t depth;
};

bool serializeTree(Serializer* s, JSValueConst value, bool nested, bool parenless);
bool serializeObject(Serializer* s, JSValueConst value, bool nested, bool parenless);

// Appends the trees of `value` from `first` up to `end`, in the way that a declaration's value is written.
void appendValue(PrTextBuffer* out, const PrCssValue* value, uint32_t first, uint32_t end) {
    PrRawString text = prCssValueSerialize(value, first, end);
    prTextBufferAppend(out, prRawStringView(&text));
    prRawStringReset(&text);
}

void serializeNumber(Serializer* s, double number) {
    prCssValueClear(s->scratch);
    PrCssValueFields fields = numericFields(number, PrCssUnit_Number);
    prCssValueAdd(s->scratch, &fields);
    appendValue(s->out, s->scratch, 0, 1);
}

void serializeUnit(Serializer* s, const TypedObject* object) {
    serializeNumber(s, object->number);
    if (object->unit == PrCssUnit_Percent)
        prTextBufferAppendChar(s->out, '%');
    else if (object->unit != PrCssUnit_Number)
        prTextBufferAppend(s->out, prCssUnitName(object->unit));
}

// css-typed-om-1/#calc-serialization
bool serializeMath(Serializer* s, JSValueConst value, const TypedObject* object, bool nested, bool parenless) {
    TypedKind kind = object->kind;
    Values children = heldValues(s->ctx);
    if (!children.read(value, object))
        return false;
    if (kind == TypedKind_Min || kind == TypedKind_Max || kind == TypedKind_Clamp) {
        prTextBufferAppend(s->out, kind == TypedKind_Min   ? "min(" : kind == TypedKind_Max ? "max(" : "clamp(");
        for (uint32_t i = 0; i < children.values->count; ++i) {
            if (i != 0)
                prTextBufferAppend(s->out, ", ");
            if (!serializeTree(s, children.values->data[i], false, true))
                return false;
        }
        prTextBufferAppendChar(s->out, ')');
        return true;
    }
    if (!parenless)
        prTextBufferAppend(s->out, nested ? "(" : "calc(");
    bool ok = true;
    switch (kind) {
        case TypedKind_Sum:
        case TypedKind_Product: {
            bool sum = kind == TypedKind_Sum;
            TypedKind inverse = sum ? TypedKind_Negate : TypedKind_Invert;
            for (uint32_t i = 0; ok && i < children.values->count; ++i) {
                JSValueConst child = children.values->data[i];
                const TypedObject* operand = typedOf(child);
                if (i == 0) {
                    ok = serializeTree(s, child, true, false);
                } else if (operand->kind == inverse) {
                    prTextBufferAppend(s->out, sum ? " - " : " / ");
                    JSValue inner = jsReference(s->ctx, child, 0);
                    ok = serializeTree(s, inner, true, false);
                    JS_FreeValue(s->ctx, inner);
                } else {
                    prTextBufferAppend(s->out, sum ? " + " : " * ");
                    ok = serializeTree(s, child, true, false);
                }
            }
            break;
        }
        case TypedKind_Negate:
            prTextBufferAppendChar(s->out, '-');
            ok = serializeTree(s, children.values->data[0], true, false);
            break;
        default:
            prTextBufferAppend(s->out, "1 / ");
            ok = serializeTree(s, children.values->data[0], true, false);
            break;
    }
    if (ok && !parenless)
        prTextBufferAppendChar(s->out, ')');
    return ok;
}

// Returns true if two numeric values are equal (css-typed-om-1/#equal-numeric-value).
bool numericEquals(JSContext* ctx, JSValueConst a, JSValueConst b) {
    Tree x = heldTree(ctx);
    Tree y = heldTree(ctx);
    if (!flattenNumeric(ctx, a, x.value) || !flattenNumeric(ctx, b, y.value)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return false;
    }
    return prCssValueEquals(x.value, 0, y.value, 0);
}

// Serializes `count` arguments of a transform component, starting at `first`, in the form that the function `name`
// takes them.
bool serializeArguments(Serializer* s, const TypedObject* object, const Values& children,
        PrStringView name, uint32_t first, uint32_t count) {
    prTextBufferAppend(s->out, name);
    for (uint32_t i = first; i < first + count; ++i) {
        if (i != first)
            prTextBufferAppend(s->out, ", ");
        JSValueConst argument = children.values->data[i];
        const TypedObject* at = typedOf(argument);
        // A negative perspective is out of range, so a calculation clamps it.
        bool clamp = object->kind == TypedKind_Perspective && at->kind == TypedKind_Unit && at->number < 0;
        if (clamp)
            prTextBufferAppend(s->out, "calc(");
        if (!serializeTree(s, argument, false, false))
            return false;
        if (clamp)
            prTextBufferAppendChar(s->out, ')');
    }
    prTextBufferAppendChar(s->out, ')');
    return true;
}

bool serializeComponent(Serializer* s, JSValueConst value, const TypedObject* object) {
    bool flat = object->is2D;
    if (object->kind == TypedKind_Matrix) {
        const PrDomMatrix* matrix = heldMatrix(s->ctx, value);
        if (matrix == nullptr)
            return false;
        PrDomMatrix copy = *matrix;
        copy.is2D = flat;
        PrRawString text = {};
        // A matrix with an element that is not finite has no serialization
        // (geometry-1/#dommatrixreadonly-stringification-behavior).
        if (prCssMatrixToString(&copy, &text) != PrStatus_Ok) {
            jsDomExceptionThrow(s->ctx, "InvalidStateError");
            return false;
        }
        prTextBufferAppend(s->out, prRawStringView(&text));
        prRawStringReset(&text);
        return true;
    }
    Values children = heldValues(s->ctx);
    if (!children.read(value, object))
        return false;
    switch (object->kind) {
        case TypedKind_Translate:
            return serializeArguments(s, object, children, flat ? "translate(" : "translate3d(", 0, flat ? 2 : 3);
        case TypedKind_Rotate:
            return serializeArguments(s, object, children, flat ? "rotate(" : "rotate3d(", flat ? 3 : 0, flat ? 1 : 4);
        case TypedKind_Scale: {
            bool same = flat && numericEquals(s->ctx, children.values->data[0], children.values->data[1]);
            return serializeArguments(s, object, children, flat ? "scale(" : "scale3d(", 0, flat ? (same ? 1 : 2) : 3);
        }
        case TypedKind_Skew: {
            const TypedObject* ay = typedOf(children.values->data[1]);
            bool zero = ay->kind == TypedKind_Unit && ay->number == 0;
            return serializeArguments(s, object, children, "skew(", 0, zero ? 1 : 2);
        }
        case TypedKind_SkewX: return serializeArguments(s, object, children, "skewX(", 0, 1);
        case TypedKind_SkewY: return serializeArguments(s, object, children, "skewY(", 0, 1);
        default:          return serializeArguments(s, object, children, "perspective(", 0, 1);
    }
}

// Serializes a color value in the form of its function: its channels, and its alpha if it is not 100%.
bool serializeColor(Serializer* s, JSValueConst value, const TypedObject* object) {
    Values children = heldValues(s->ctx);
    if (!children.read(value, object))
        return false;
    prTextBufferAppend(s->out, kColorFunctionNames[object->colorFunction]);
    prTextBufferAppendChar(s->out, '(');
    uint32_t first = 0;
    if (object->colorFunction == ColorFunction_Color) {
        if (!serializeTree(s, children.values->data[0], false, false))
            return false;
        prTextBufferAppendChar(s->out, ' ');
        first = 1;
    }
    uint32_t alpha = children.values->count - 1;
    for (uint32_t i = first; i < alpha; ++i) {
        if (i != first)
            prTextBufferAppendChar(s->out, ' ');
        if (!serializeTree(s, children.values->data[i], false, false))
            return false;
    }
    const TypedObject* at = typedOf(children.values->data[alpha]);
    if (!(at->kind == TypedKind_Unit && at->unit == PrCssUnit_Percent && at->number == 100.0)) {
        prTextBufferAppend(s->out, " / ");
        if (!serializeTree(s, children.values->data[alpha], false, false))
            return false;
    }
    prTextBufferAppendChar(s->out, ')');
    return true;
}

bool serializeSegments(Serializer* s, JSValueConst value, const TypedObject* object) {
    return eachChild(s->ctx, value, object, [&](JSValueConst child) {
        if (!JS_IsString(child))
            return serializeTree(s, child, false, false);
        StringArgument text = {s->ctx};
        if (!text.read(child))
            return false;
        prTextBufferAppend(s->out, text.value());
        return true;
    });
}

// Serializes a value that is no deeper than a PrCssValue may be. A self-containing value exceeds that depth. Throws a
// RangeError for a deeper value.
bool serializeTree(Serializer* s, JSValueConst value, bool nested, bool parenless) {
    if (s->depth == kPrCssValueMaxDepth) {
        JS_ThrowRangeError(s->ctx, "The value is nested too deeply");
        return false;
    }
    ++s->depth;
    bool ok = serializeObject(s, value, nested, parenless);
    --s->depth;
    return ok;
}

bool serializeObject(Serializer* s, JSValueConst value, bool nested, bool parenless) {
    const TypedObject* object = typedOf(value);
    switch (object->kind) {
        case TypedKind_Style:
        case TypedKind_Image:
            appendValue(s->out, object->value, 0, prCssValueSize(object->value));
            return true;
        case TypedKind_Keyword:
            prTextBufferAppend(s->out, prRawStringView(&object->text));
            return true;
        case TypedKind_Unparsed:
            return serializeSegments(s, value, object);
        case TypedKind_VariableReference: {
            // Only a comma is written before the fallback. The fallback holds the whitespace after the comma that it
            // was reified from, although css-typed-om-1/#unparsedvalue-serialization writes a comma and a space.
            prTextBufferAppend(s->out, "var(");
            prTextBufferAppend(s->out, prRawStringView(&object->text));
            JSValue fallback = jsReference(s->ctx, value, 0);
            bool ok = true;
            if (!JS_IsNull(fallback) && !JS_IsUndefined(fallback)) {
                prTextBufferAppendChar(s->out, ',');
                ok = serializeTree(s, fallback, false, false);
            }
            JS_FreeValue(s->ctx, fallback);
            prTextBufferAppendChar(s->out, ')');
            return ok;
        }
        case TypedKind_Unit:
            serializeUnit(s, object);
            return true;
        case TypedKind_Color:
            return serializeColor(s, value, object);
        case TypedKind_Transform: {
            bool first = true;
            return eachChild(s->ctx, value, object, [&](JSValueConst child) {
                if (!first)
                    prTextBufferAppendChar(s->out, ' ');
                first = false;
                return serializeComponent(s, child, typedOf(child));
            });
        }
        default:
            if (isNumeric(object->kind))
                return serializeMath(s, value, object, nested, parenless);
            return serializeComponent(s, value, object);
    }
}

bool serializeValue(JSContext* ctx, JSValueConst value, PrTextBuffer* out) {
    Tree scratch = heldTree(ctx);
    Serializer s = {ctx, out, scratch.value, 0};
    return serializeTree(&s, value, false, false);
}

// ---------------------------------------------------------------------------------------------------------------------
// Values to objects (css-typed-om-1/#reify-stylevalue)
//
// A reifier returns JS_UNDEFINED for a value that its interface does not describe. The value then falls through to the
// reifier for an interface that does.

JSValue newChannels(JSContext* ctx, JSValueConst owner, JSValue* channels, uint32_t count);

// The maximum nesting depth of a reference's fallback. Beyond it, the reference is kept as text in a string segment.
constexpr uint32_t kUnparsedDepth = kPrCssValueMaxDepth / 2;

// css-typed-om-1/#reify-a-list-of-component-values, for tokens written as `text`: string segments and the var()
// references among them. Each fallback is its own unparsed value.
JSValue reifyTokens(JSContext* ctx, PrStringView text, uint32_t depth = 0) {
    JSValue unparsed = newTyped(ctx, cssUnparsedValueClass, typedFields(TypedKind_Unparsed));
    if (JS_IsException(unparsed))
        return unparsed;
    uint32_t count = depth < kUnparsedDepth ? prCssVariableReferences(text, nullptr, 0) : 0;
    RawArray<PrCssVariableReference> references;
    references.reserve(count);
    references.count = count;
    prCssVariableReferences(text, references.data, count);
    uint32_t written = 0;   // byte at which the next string segment starts
    bool ok = true;
    for (uint32_t i = 0; ok && i < count; ++i) {
        const PrCssVariableReference& reference = references.data[i];
        if (reference.start > written) {
            PrStringView segment = {text.data + written, reference.start - written};
            ok = hold(ctx, unparsed, jsNewStringView(ctx, segment));
        }
        TypedObject fields = typedFields(TypedKind_VariableReference);
        fields.text = prRawStringCopy(reference.name);
        JSValue variable = newTyped(ctx, cssVariableReferenceValueClass, fields);
        if (ok && !JS_IsException(variable) && reference.hasFallback) {
            PrStringView fallback = {
                text.data + reference.fallbackStart,
                reference.fallbackEnd - reference.fallbackStart,
            };
            ok = hold(ctx, variable, reifyTokens(ctx, fallback, depth + 1));
        }
        ok = ok && hold(ctx, unparsed, variable);
        written = reference.end;
    }
    if (ok && written < text.size)
        ok = hold(ctx, unparsed, jsNewStringView(ctx, PrStringView{text.data + written, text.size - written}));
    references.reset();
    if (!ok) {
        JS_FreeValue(ctx, unparsed);
        return JS_EXCEPTION;
    }
    return unparsed;
}

// Returns a numeric value as a unit value, or JS_UNDEFINED for one in a unit that Typed OM does not have.
JSValue reifyNumber(JSContext* ctx, const PrCssValueFields& fields) {
    PrCssNumericType type;
    if (fields.number.unit == PrCssUnit_Unknown || !unitType(fields.number.unit, &type))
        return JS_UNDEFINED;
    return newUnit(ctx, fields.number.value, fields.number.unit);
}

bool isNumericKind(PrCssValueKind kind) {
    return kind == PrCssValueKind_Integer || kind == PrCssValueKind_Number ||
        kind == PrCssValueKind_Percentage || kind == PrCssValueKind_Dimension;
}

// Returns a node of a calculation tree (css-values-4/#calculation-tree) as a math value.
JSValue reifyCalculation(JSContext* ctx, const PrCssValue* value, uint32_t node) {
    PrCssValueFields fields = prCssValueFields(value, node);
    if (isNumericKind(fields.kind))
        return reifyNumber(ctx, fields);
    TypedKind kind;
    if (fields.kind == PrCssValueKind_Calc) {
        constexpr TypedKind kOperators[] = {TypedKind_Sum, TypedKind_Product, TypedKind_Negate, TypedKind_Invert};
        kind = kOperators[fields.calc];
    } else if (fields.kind == PrCssValueKind_Function && fields.keyword == PrCssKeyword_Min) {
        kind = TypedKind_Min;
    } else if (fields.kind == PrCssValueKind_Function && fields.keyword == PrCssKeyword_Max) {
        kind = TypedKind_Max;
    } else if (fields.kind == PrCssValueKind_Function && fields.keyword == PrCssKeyword_Clamp) {
        kind = TypedKind_Clamp;
    } else {
        return JS_UNDEFINED;
    }
    Values operands = heldValues(ctx);
    for (uint32_t child = node + 1; child < prCssValueNext(value, node); child = prCssValueNext(value, child)) {
        PrCssValueFields at = prCssValueFields(value, child);
        if (at.kind == PrCssValueKind_Literal && at.literal == ',')
            continue;
        JSValue operand = reifyCalculation(ctx, value, at.kind == PrCssValueKind_Math ? child + 1 : child);
        if (!operands.add(operand))
            return operand;
    }
    uint32_t count = operands.values->count;
    return newMath(ctx, kind, operands.take(), count);
}

// css-typed-om-1/#reify-a-math-expression: a calculation of one value is a sum of that value.
JSValue reifyMath(JSContext* ctx, const PrCssValue* value, uint32_t node) {
    uint32_t root = node + 1;
    JSValue reified = reifyCalculation(ctx, value, root);
    if (JS_IsException(reified) || JS_IsUndefined(reified) || !isNumericKind(prCssValueFields(value, root).kind))
        return reified;
    return newMath(ctx, TypedKind_Sum, &reified, 1);
}

// Returns a numeric argument of a transform function. A plain zero is `zero`, which is the unit of the argument's
// length or angle, or a number.
JSValue reifyArgument(JSContext* ctx, const PrCssValue* value, uint32_t node, PrCssUnit zero) {
    PrCssValueFields fields = prCssValueFields(value, node);
    if (fields.kind == PrCssValueKind_Math)
        return reifyMath(ctx, value, node);
    if ((fields.kind == PrCssValueKind_Integer || fields.kind == PrCssValueKind_Number) && zero != PrCssUnit_Number)
        return fields.number.value == 0 ? newUnit(ctx, 0, zero) : JS_UNDEFINED;
    if (!isNumericKind(fields.kind))
        return JS_UNDEFINED;
    return reifyNumber(ctx, fields);
}

// css-typed-om-1/#reify-a-transform-function
JSValue reifyComponent(JSContext* ctx, const PrCssValue* value, uint32_t node) {
    PrCssValueFields function = prCssValueFields(value, node);
    if (function.kind != PrCssValueKind_Function)
        return JS_UNDEFINED;
    uint32_t args[16];
    uint32_t count = 0;
    for (uint32_t child = node + 1; child < prCssValueNext(value, node); child = prCssValueNext(value, child)) {
        PrCssValueFields at = prCssValueFields(value, child);
        if (at.kind == PrCssValueKind_Literal && at.literal == ',')
            continue;
        if (count == 16)
            return JS_UNDEFINED;
        args[count++] = child;
    }
    Values made = heldValues(ctx);
    auto add = [&](uint32_t at, PrCssUnit zero) {
        return made.add(reifyArgument(ctx, value, args[at], zero));
    };
    auto unit = [&](double number, PrCssUnit in) { return made.add(newUnit(ctx, number, in)); };
    auto component = [&](TypedKind kind, bool is2D) {
        uint32_t taken = made.values->count;
        return construct(ctx, JS_UNDEFINED, *kKindClasses[kind],
            typedFields(kind, PrCssUnit_Number, 0, is2D), made.take(), taken);
    };
    PrCssKeyword name = function.keyword;
    switch (name) {
        case PrCssKeyword_Matrix:
        case PrCssKeyword_Matrix3d: {
            double values[16];
            for (uint32_t i = 0; i < count; ++i) {
                PrCssValueFields at = prCssValueFields(value, args[i]);
                if (at.kind != PrCssValueKind_Integer && at.kind != PrCssValueKind_Number)
                    return JS_UNDEFINED;
                values[i] = at.number.value;
            }
            PrDomMatrix matrix;
            if (prDomMatrixFromValues(values, count, &matrix) != PrStatus_Ok)
                return JS_UNDEFINED;
            JSValue held = prJsDomMatrixNew(ctx, matrix);
            if (JS_IsException(held))
                return held;
            TypedObject fields = typedFields(TypedKind_Matrix, PrCssUnit_Number, 0, name == PrCssKeyword_Matrix);
            return construct(ctx, JS_UNDEFINED, cssMatrixComponentClass, fields, &held, 1);
        }
        case PrCssKeyword_Translate:
        case PrCssKeyword_Translatex:
        case PrCssKeyword_Translatey:
        case PrCssKeyword_Translatez:
        case PrCssKeyword_Translate3d:
        case PrCssKeyword_Scale:
        case PrCssKeyword_Scalex:
        case PrCssKeyword_Scaley:
        case PrCssKeyword_Scalez:
        case PrCssKeyword_Scale3d: {
            // Which argument gives x, y and z, and the identity value for a component that no argument gives.
            bool translate = name == PrCssKeyword_Translate || name == PrCssKeyword_Translatex ||
                name == PrCssKeyword_Translatey || name == PrCssKeyword_Translatez || name == PrCssKeyword_Translate3d;
            bool x = name == PrCssKeyword_Translatex || name == PrCssKeyword_Scalex;
            bool y = name == PrCssKeyword_Translatey || name == PrCssKeyword_Scaley;
            bool z = name == PrCssKeyword_Translatez || name == PrCssKeyword_Scalez;
            bool full = name == PrCssKeyword_Translate3d || name == PrCssKeyword_Scale3d;
            bool both = name == PrCssKeyword_Translate || name == PrCssKeyword_Scale;
            uint32_t from[3] = {
                x || both || full ? 0u : UINT32_MAX,
                y ? 0u : full ? 1u : both ? (count > 1 ? 1u : translate ? UINT32_MAX : 0u) : UINT32_MAX,
                z ? 0u : full ? 2u : UINT32_MAX,
            };
            PrCssUnit in = translate ? PrCssUnit_Px : PrCssUnit_Number;
            double identity = translate ? 0 : 1;
            for (uint32_t i = 0; i < 3; ++i) {
                if (!(from[i] == UINT32_MAX ? unit(identity, in) : add(from[i], in)))
                    return JS_UNDEFINED;
            }
            return component(translate ? TypedKind_Translate : TypedKind_Scale, !z && !full);
        }
        case PrCssKeyword_Rotate:
        case PrCssKeyword_Rotatex:
        case PrCssKeyword_Rotatey:
        case PrCssKeyword_Rotatez:
        case PrCssKeyword_Rotate3d:
            if (name == PrCssKeyword_Rotate3d) {
                for (uint32_t i = 0; i < 4; ++i) {
                    if (!add(i, i < 3 ? PrCssUnit_Number : PrCssUnit_Deg))
                        return JS_UNDEFINED;
                }
            } else {
                // rotate() and rotateZ() rotate about z, rotateX() about x and rotateY() about y.
                for (uint32_t i = 0; i < 3; ++i) {
                    bool about = i == (name == PrCssKeyword_Rotatex   ? 0 : name == PrCssKeyword_Rotatey ? 1 : 2);
                    if (!unit(about ? 1 : 0, PrCssUnit_Number))
                        return JS_UNDEFINED;
                }
                if (!add(0, PrCssUnit_Deg))
                    return JS_UNDEFINED;
            }
            return component(TypedKind_Rotate, name == PrCssKeyword_Rotate);
        case PrCssKeyword_Skew:
            if (!add(0, PrCssUnit_Deg) || !(count > 1 ? add(1, PrCssUnit_Deg) : unit(0, PrCssUnit_Deg)))
                return JS_UNDEFINED;
            return component(TypedKind_Skew, true);
        case PrCssKeyword_Skewx:
        case PrCssKeyword_Skewy:
            if (!add(0, PrCssUnit_Deg))
                return JS_UNDEFINED;
            return component(name == PrCssKeyword_Skewx ? TypedKind_SkewX : TypedKind_SkewY, true);
        case PrCssKeyword_Perspective: {
            PrCssValueFields at = prCssValueFields(value, args[0]);
            bool none = at.kind == PrCssValueKind_Keyword && at.keyword == PrCssKeyword_None;
            bool ok = none ? made.add(newKeyword(ctx, "none")) : add(0, PrCssUnit_Px);
            if (!ok)
                return JS_UNDEFINED;
            return component(TypedKind_Perspective, false);
        }
        default:
            return JS_UNDEFINED;
    }
}

// css-typed-om-1/#reify-a-transform-list, for the trees from `first` up to `end`.
JSValue reifyTransform(JSContext* ctx, const PrCssValue* value, uint32_t first, uint32_t end) {
    Values components = heldValues(ctx);
    for (uint32_t tree = first; tree < end; tree = prCssValueNext(value, tree)) {
        JSValue component = reifyComponent(ctx, value, tree);
        if (!components.add(component))
            return component;
    }
    uint32_t count = components.values->count;
    return construct(ctx, JS_UNDEFINED, cssTransformValueClass, typedFields(TypedKind_Transform), components.take(), count);
}

// css-typed-om-1/#reify-color, for the Color at `node`.
JSValue reifyColor(JSContext* ctx, const PrCssValue* value, uint32_t node) {
    PrCssValueFields color = prCssValueFields(value, node);
    ColorFunction function;
    switch (color.color.space) {
        case PrColorSpace_Srgb:
            function = color.color.legacy ? ColorFunction_Rgb : ColorFunction_Color;
            break;
        case PrColorSpace_Hsl:   function = ColorFunction_Hsl; break;
        case PrColorSpace_Hwb:   function = ColorFunction_Hwb; break;
        case PrColorSpace_Lab:   function = ColorFunction_Lab; break;
        case PrColorSpace_Lch:   function = ColorFunction_Lch; break;
        case PrColorSpace_Oklab: function = ColorFunction_Oklab; break;
        case PrColorSpace_Oklch: function = ColorFunction_Oklch; break;
        default:                 function = ColorFunction_Color; break;
    }
    PrCssColorChannel channels[4];
    prCssValueColorChannels(value, node, channels);
    Values made = heldValues(ctx);
    for (uint32_t i = 0; i < 4; ++i) {
        PrCssValueFields at = prCssValueFields(value, channels[i].node);
        JSValue channel = JS_UNDEFINED;
        if (at.kind == PrCssValueKind_Keyword) {
            channel = newKeyword(ctx, prCssKeywordName(at.keyword));
        } else if (at.kind == PrCssValueKind_Math) {
            channel = reifyMath(ctx, value, channels[i].node);
        } else if (isNumericKind(at.kind)) {
            // A channel in the form that its type holds: a number if the type accepts one, a percentage if it accepts
            // one, and degrees for a hue written as a number. Each conversion uses the meanings of the two forms in the
            // space.
            bool number = at.kind == PrCssValueKind_Integer || at.kind == PrCssValueKind_Number;
            double v = at.number.value;
            PrCssUnit unit = at.number.unit;
            switch (channelType(function, i)) {
                case ChannelType_Percent:
                    if (number) {
                        v = v * channels[i].number / channels[i].percent;
                        unit = PrCssUnit_Percent;
                    }
                    break;
                case ChannelType_Number:
                    if (at.kind == PrCssValueKind_Percentage) {
                        v = v * channels[i].percent / channels[i].number;
                        unit = PrCssUnit_Number;
                    }
                    break;
                case ChannelType_Angle:
                    if (number)
                        unit = PrCssUnit_Deg;
                    break;
                default:
                    break;
            }
            channel = reifyNumber(ctx, numericFields(v, unit));
        }
        if (!made.add(channel))
            return channel;
    }
    TypedObject fields = typedFields(TypedKind_Color);
    fields.colorFunction = function;
    if (function != ColorFunction_Color)
        return construct(ctx, JS_UNDEFINED, *kColorClasses[function], fields, made.take(), 4);

    // A CSSColor holds its color space, the observable array of its channels, and its alpha.
    JSValue object = newTyped(ctx, cssColorClass, fields);
    JSValue space = newKeyword(ctx, prColorSpaceName(color.color.space));
    JSValue* taken = made.take();
    bool ok = !JS_IsException(object) && hold(ctx, object, space) &&
        hold(ctx, object, newChannels(ctx, object, taken, 3)) && hold(ctx, object, taken[3]);
    if (!ok) {
        JS_FreeValue(ctx, object);
        return JS_EXCEPTION;
    }
    return object;
}

// Returns a style value of `kind` for `property` that holds the trees of `value` from `first` up to `end`.
JSValue newStyle(JSContext* ctx, TypedKind kind, const PrCssValue* value, uint32_t first,
        uint32_t end, const PrCssDeclarationName* property) {
    TypedObject fields = typedFields(kind);
    fields.property = prCssDeclarationNameCopy(property);
    fields.value = prCssValueCreate();
    for (uint32_t tree = first; tree < end; tree = prCssValueNext(value, tree))
        prCssValueAddTree(fields.value, value, tree);
    return newTyped(ctx, *kKindClasses[kind], fields);
}

// Reifies a value of `property`, or an item of a list-valued property, which is the trees from `first` up to `end`. A
// shorthand's value that has one component, a keyword or a number, is reified as that component.
JSValue reifyItem(JSContext* ctx, const PrCssValue* value, uint32_t first, uint32_t end,
        const PrCssDeclarationName* property) {
    bool shorthand = property->property != PrCssProperty_Count && prCssPropertyLonghands(property->property, nullptr, 0) != 0;
    uint32_t single = first;
    while (single + 1 < end && prCssValueNext(value, single) == end &&
            prCssValueFields(value, single).kind == PrCssValueKind_Longhand)
        ++single;
    if (single < end && prCssValueNext(value, single) == end) {
        PrCssValueFields fields = prCssValueFields(value, single);
        JSValue reified = JS_UNDEFINED;
        PrColor8 color;
        switch (fields.kind) {
            case PrCssValueKind_Keyword:
                // A color is a CSSStyleValue, although css-typed-om-1/#reify-color makes it a CSSColorValue.
                if (shorthand || !prCssColorFromKeyword(fields.keyword, &color))
                    reified = newKeyword(ctx, prCssKeywordName(fields.keyword));
                break;
            case PrCssValueKind_Ident: {
                PrRawString text = prCssValueSerialize(value, single, end);
                reified = newKeyword(ctx, prRawStringView(&text));
                prRawStringReset(&text);
                break;
            }
            case PrCssValueKind_Url:
                if (!shorthand)
                    reified = newStyle(ctx, TypedKind_Image, value, first, end, property);
                break;
            case PrCssValueKind_Math:
                if (!shorthand)
                    reified = reifyMath(ctx, value, single);
                break;
            default:
                if (isNumericKind(fields.kind))
                    reified = reifyNumber(ctx, fields);
                break;
        }
        if (!JS_IsUndefined(reified))
            return reified;
    }
    if (property->property == PrCssProperty_Transform) {
        JSValue transform = reifyTransform(ctx, value, first, end);
        if (!JS_IsUndefined(transform))
            return transform;
    }
    return newStyle(ctx, TypedKind_Style, value, first, end, property);
}

// css-typed-om-1/#reify-property: passes to `each` the objects of `value`, a declared value of `property`, one per item
// for a list-valued property.
template <typename Each>
bool reifyValue(JSContext* ctx, const PrCssValue* value, const PrCssDeclarationName* property, Each each) {
    uint32_t size = prCssValueSize(value);
    if (size == 0)
        return true;
    bool custom = property->property == PrCssProperty_Count;
    if (custom || (prCssValueNext(value, 0) == size && prCssValueFields(value, 0).kind == PrCssValueKind_Tokens)) {
        PrCssValueFields tokens = prCssValueFields(value, 0);
        // css-variables-1/#serializing-custom-props: an empty custom property is a single space.
        PrStringView text = tokens.kind == PrCssValueKind_Tokens ? tokens.text : PrStringView{};
        JSValue unparsed = reifyTokens(ctx, custom && text.size == 0 ? " " : text);
        return !JS_IsException(unparsed) && each(unparsed);
    }
    bool list = prCssPropertyListValued(property->property) && prCssPropertyLonghands(property->property, nullptr, 0) == 0;
    uint32_t start = 0;
    for (uint32_t tree = 0; tree <= size; tree = tree < size ? prCssValueNext(value, tree) : tree + 1) {
        if (tree < size) {
            PrCssValueFields fields = prCssValueFields(value, tree);
            if (!list || fields.kind != PrCssValueKind_Literal || fields.literal != ',')
                continue;
        }
        JSValue item = reifyItem(ctx, value, start, tree, property);
        if (JS_IsException(item) || !each(item))
            return false;
        start = tree < size ? prCssValueNext(value, tree) : tree;
    }
    return true;
}

// Returns the objects of `value` for `property`, as an array.
JSValue reifyArray(JSContext* ctx, const PrCssValue* value, const PrCssDeclarationName* property) {
    JSValue array = JS_NewArray(ctx);
    uint32_t index = 0;
    bool ok = reifyValue(ctx, value, property, [&](JSValue item) {
        JS_SetPropertyUint32(ctx, array, index++, item);
        return true;
    });
    if (!ok) {
        JS_FreeValue(ctx, array);
        return JS_EXCEPTION;
    }
    return array;
}

// Returns the first object of `value` for `property`, or undefined if there is none.
JSValue reifyFirst(JSContext* ctx, const PrCssValue* value, const PrCssDeclarationName* property) {
    JSValue first = JS_UNDEFINED;
    bool ok = reifyValue(ctx, value, property, [&](JSValue item) {
        if (JS_IsUndefined(first))
            first = item;
        else
            JS_FreeValue(ctx, item);
        return true;
    });
    if (!ok) {
        JS_FreeValue(ctx, first);
        return JS_EXCEPTION;
    }
    return first;
}

// ---------------------------------------------------------------------------------------------------------------------
// CSSStyleValue (css-typed-om-1/#cssstylevalue)

JSValue toString(JSContext* ctx, JSValueConst styleValue, int, JSValueConst*) {
    if (styleValueOf(ctx, styleValue) == nullptr && typedIn(ctx, styleValue, cssTransformComponentClass) == nullptr)
        return JS_EXCEPTION;
    PrTextBuffer text = {};
    JSValue result = JS_EXCEPTION;
    if (serializeValue(ctx, styleValue, &text))
        result = jsNewStringView(ctx, prTextBufferView(&text));
    prTextBufferReset(&text);
    return result;
}

// A property argument, which is the name that script's string refers to in a block of properties: a property or a
// custom property. Throws a TypeError if it is neither.
struct PropertyArgument {
    PrCssDeclarationName name = prCssDeclarationNameFromProperty(PrCssProperty_Count);

    // The call's scope holds the name.
    bool read(JSContext* ctx, JSValueConst input) {
        StringArgument text = {ctx};
        if (!text.read(input))
            return false;
        if (prCssPropertyNameFromText(text.value(), &name)) {
            jsHoldString(ctx, name.author);
            return true;
        }
        JS_ThrowTypeError(ctx, "Not a property");
        return false;
    }
};

// Implements parse and parseAll, selected by the magic.
JSValue parse(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv, int magic) {
    PropertyArgument property = {};
    StringArgument text = {ctx};
    if (!jsArgCount(ctx, argc, 2) || !property.read(ctx, argv[0]) || !text.read(argv[1]))
        return JS_EXCEPTION;
    Tree value = heldTree(ctx);
    PrStatus status = prCssValueParse(&property.name, text.value(), value.value);
    if (status == PrStatus_InvalidData)
        return JS_ThrowTypeError(ctx, "Not a property");
    if (status != PrStatus_Ok)
        return jsDomExceptionThrow(ctx, "SyntaxError");
    return magic == 1 ? reifyArray(ctx, value.value, &property.name) : reifyFirst(ctx, value.value, &property.name);
}

// ---------------------------------------------------------------------------------------------------------------------
// CSSKeywordValue, CSSUnparsedValue and CSSVariableReferenceValue

// css-typed-om-1/#dom-csskeywordvalue-csskeywordvalue and
// css-typed-om-1/#dom-cssvariablereferencevalue-cssvariablereferencevalue, selected by the kind. The fallback is an
// unparsed value or null.
template <TypedKind Kind>
JSValue textConstruct(JSContext* ctx, JSValueConst newTarget, int argc, JSValueConst* argv) {
    StringArgument text = {ctx};
    if (!jsArgCount(ctx, argc, 1) || !text.read(argv[0]))
        return JS_EXCEPTION;
    TypedObject fields = typedFields(Kind);
    fields.text = prRawStringCopy(text.value());
    if (Kind == TypedKind_Keyword)
        return construct(ctx, newTarget, cssKeywordValueClass, fields);
    JSValue fallback = jsArgGiven(argc, argv, 1) ? JS_DupValue(ctx, argv[1]) : JS_NULL;
    if (!JS_IsNull(fallback) && jsHandleIn(ctx, fallback, cssUnparsedValueClass) == nullptr) {
        JS_FreeValue(ctx, fallback);
        prRawStringReset(&fields.text);
        return JS_ThrowTypeError(ctx, "Expected a CSSUnparsedValue");
    }
    return construct(ctx, newTarget, cssVariableReferenceValueClass, fields, &fallback, 1);
}

JSValue textGet(JSContext* ctx, JSValueConst typedObject) {
    TypedObject* object = typedOf(typedObject);
    return object != nullptr ? jsNewString(ctx, &object->text) : JS_ThrowTypeError(ctx, "Illegal invocation");
}

// Sets a keyword's value, or a variable reference's name.
JSValue textSet(JSContext* ctx, JSValueConst typedObject, JSValueConst input) {
    TypedObject* object = typedOf(typedObject);
    StringArgument text = {ctx};
    if (object == nullptr)
        return JS_ThrowTypeError(ctx, "Illegal invocation");
    if (!text.read(input))
        return JS_EXCEPTION;
    PrRawString previous = object->text;
    object->text = prRawStringCopy(text.value());
    if (!valid(ctx, typedObject)) {
        prRawStringReset(&object->text);
        object->text = previous;
        return JS_EXCEPTION;
    }
    prRawStringReset(&previous);
    return JS_UNDEFINED;
}

// Reads a sequence argument's items, each converted by `item`. Returns false with the exception pending.
template <typename Item>
bool sequence(JSContext* ctx, JSValueConst value, RawArray<JSValue>* out, Item item) {
    bool ok = jsSequence(ctx, value, [&](JSValue element) {
        JSValue made = item(element);
        JS_FreeValue(ctx, element);
        if (JS_IsException(made))
            return false;
        out->push(made);
        return true;
    });
    if (!ok) {
        for (uint32_t i = 0; i < out->count; ++i)
            JS_FreeValue(ctx, out->data[i]);
        out->reset();
    }
    return ok;
}

// Returns an unparsed value's segment: a variable reference, or anything else as a string.
JSValue segmentOf(JSContext* ctx, JSValueConst value) {
    if (jsHandleIn(ctx, value, cssVariableReferenceValueClass) != nullptr)
        return JS_DupValue(ctx, value);
    return JS_ToString(ctx, value);
}

JSValue unparsedConstruct(JSContext* ctx, JSValueConst newTarget, int argc, JSValueConst* argv) {
    RawArray<JSValue> segments;
    if (!jsArgCount(ctx, argc, 1) || !sequence(ctx, argv[0], &segments, [&](JSValue v) { return segmentOf(ctx, v); }))
        return JS_EXCEPTION;
    JSValue result = construct(ctx, newTarget, cssUnparsedValueClass, typedFields(TypedKind_Unparsed), segments.data,
        segments.count);
    segments.reset();
    return result;
}

// ---------------------------------------------------------------------------------------------------------------------
// Held objects, and the indexed properties of an unparsed value's segments, a numeric array's values and a transform
// value's components

// Returns the object that `typedObject` holds at the index that `magic` gives, or null if there is none.
JSValue heldGet(JSContext* ctx, JSValueConst typedObject, int magic) {
    if (typedOf(typedObject) == nullptr)
        return JS_ThrowTypeError(ctx, "Illegal invocation");
    JSValue held = jsReference(ctx, typedObject, uint32_t(magic & 0xff));
    return JS_IsUndefined(held) ? JS_NULL : held;
}

// Replaces the object that `previousObject` holds at `index` with `value`. Keeps the replacement if `previousObject` is
// then allowed by its setters, and throws a TypeError if not.
bool replaceHeld(JSContext* ctx, JSValueConst previousObject, uint32_t index, JSValueConst value) {
    JSValue previous = jsReference(ctx, previousObject, index);
    jsSetReference(ctx, previousObject, index, value);
    bool ok = valid(ctx, previousObject);
    if (!ok)
        jsSetReference(ctx, previousObject, index, previous);
    JS_FreeValue(ctx, previous);
    return ok;
}

// The magic of a setter for a component's argument: the index at which the argument is held, plus kNumberish if it
// converts a number to a CSSUnitValue.
constexpr int kNumberish = 1 << 8;

JSValue heldSet(JSContext* ctx, JSValueConst okObject, JSValueConst input, int magic) {
    if (typedOf(okObject) == nullptr)
        return JS_ThrowTypeError(ctx, "Illegal invocation");
    JSValue value = rectified(ctx, input, (magic & kNumberish) != 0 ? Rectify_Numberish : Rectify_None);
    if (JS_IsException(value))
        return value;
    bool ok = replaceHeld(ctx, okObject, uint32_t(magic & 0xff), value);
    JS_FreeValue(ctx, value);
    return ok ? JS_UNDEFINED : JS_EXCEPTION;
}

uint32_t heldLength(void* handle) {
    return static_cast<TypedObject*>(handle)->references;
}

JSValue heldItem(JSContext* ctx, JSValueConst object, void*, uint32_t index) {
    return jsReference(ctx, object, index);
}

// Sets an item at `index` of a css-typed-om-1/#cssunparsedvalue, or appends one at the index after the last: a segment
// of an unparsed value, or a component of a transform value. A segment can make a value contain itself. Only flattening
// detects that, as a value that is too deep.
bool heldSetItem(JSContext* ctx, JSValueConst jsObject, void* handle, uint32_t index, JSValueConst value) {
    TypedObject* object = static_cast<TypedObject*>(handle);
    if (index > object->references) {
        JS_ThrowRangeError(ctx, "Index out of range");
        return false;
    }
    if (object->kind == TypedKind_Transform && jsHandleIn(ctx, value, cssTransformComponentClass) == nullptr) {
        JS_ThrowTypeError(ctx, "Expected a CSSTransformComponent");
        return false;
    }
    JSValue item = object->kind == TypedKind_Unparsed ? segmentOf(ctx, value) : JS_DupValue(ctx, value);
    bool ok = !JS_IsException(item);
    if (ok) {
        jsSetReference(ctx, jsObject, index, item);
        if (index == object->references)
            ++object->references;
    }
    JS_FreeValue(ctx, item);
    return ok;
}

constexpr JsIndexed kReadIndexed = {heldLength, heldItem, nullptr};
constexpr JsIndexed kWriteIndexed = {heldLength, heldItem, heldSetItem};

JSValue lengthGet(JSContext* ctx, JSValueConst typedObject) {
    TypedObject* object = typedOf(typedObject);
    return object != nullptr ? JS_NewUint32(ctx, object->references) : JS_ThrowTypeError(ctx, "Illegal invocation");
}

// ---------------------------------------------------------------------------------------------------------------------
// CSSNumericValue (css-typed-om-1/#numeric-value)

// The arithmetic operations of CSSNumericValue, selected by the magic of its members.
enum {
    Operation_Add,
    Operation_Sub,
    Operation_Mul,
    Operation_Div,
    Operation_Min,
    Operation_Max,
};
typedef uint8_t Operation;

// css-typed-om-1/#cssmath-negate-a-cssnumericvalue, or #cssnumericvalue-invert, of `value`, which it takes. Throws a
// RangeError for the inverse of zero.
JSValue opposite(JSContext* ctx, JSValue value, bool invert) {
    const TypedObject* object = typedOf(value);
    TypedKind wrapper = invert ? TypedKind_Invert : TypedKind_Negate;
    JSValue result;
    if (object->kind == wrapper) {
        result = jsReference(ctx, value, 0);
    } else if (object->kind == TypedKind_Unit && (!invert || object->unit == PrCssUnit_Number)) {
        if (invert && object->number == 0)
            result = JS_ThrowRangeError(ctx, "The result is out of range");
        else
            result = newUnit(ctx, invert ? 1 / object->number : -object->number, object->unit);
    } else {
        JSValue operand = JS_DupValue(ctx, value);
        result = newMath(ctx, wrapper, &operand, 1);
    }
    JS_FreeValue(ctx, value);
    return result;
}

// Gathers the operands of `typedObject` combined with the arguments by `operation`. `typedObject` is flattened into its
// operands if a sum is added to or a product is multiplied. Each argument is rectified as a numeric value, negated if
// it is subtracted, and inverted if it divides.
bool gatherOperands(JSContext* ctx, JSValueConst typedObject, int argc, JSValueConst* argv, Operation operation, Values* out) {
    const TypedObject* object = typedOf(typedObject);
    bool sum = operation == Operation_Add || operation == Operation_Sub;
    bool product = operation == Operation_Mul || operation == Operation_Div;
    if ((sum && object->kind == TypedKind_Sum) || (product && object->kind == TypedKind_Product)) {
        Values children = heldValues(ctx);
        if (!children.read(typedObject, object))
            return false;
        for (uint32_t i = 0; i < children.values->count; ++i)
            out->add(JS_DupValue(ctx, children.values->data[i]));
    } else {
        out->add(JS_DupValue(ctx, typedObject));
    }
    for (int i = 0; i < argc; ++i) {
        JSValue operand = rectified(ctx, argv[i], Rectify_Numberish);
        if (!JS_IsException(operand) && !isNumeric(typedOf(operand)->kind)) {
            JS_FreeValue(ctx, operand);
            operand = JS_ThrowTypeError(ctx, "Expected a CSSNumericValue");
        }
        if (!JS_IsException(operand) && (operation == Operation_Sub || operation == Operation_Div))
            operand = opposite(ctx, operand, operation == Operation_Div);
        if (!out->add(operand))
            return false;
    }
    return true;
}

// Implements add, sub, mul, div, min and max, selected by the magic, which is the Operation
// (css-typed-om-1/#dom-cssnumericvalue-add and the methods after it).
JSValue combine(JSContext* ctx, JSValueConst object, int argc, JSValueConst* argv, int magic) {
    Operation operation = Operation(magic);
    if (typedIn(ctx, object, cssNumericValueClass) == nullptr)
        return JS_EXCEPTION;
    Values operands = heldValues(ctx);
    if (!gatherOperands(ctx, object, argc, argv, operation, &operands))
        return JS_EXCEPTION;
    uint32_t count = operands.values->count;
    const TypedObject* first = typedOf(operands.values->data[0]);
    bool multiply = operation == Operation_Mul || operation == Operation_Div;

    // Unit values of the same unit combine into one unit value. So do products of unit values in which all but one are
    // numbers.
    bool units = true;
    PrCssUnit unit = multiply ? PrCssUnit(PrCssUnit_Number) : first->unit;
    double result = first->number;
    if (multiply)
        result = 1;
    else if (operation == Operation_Add || operation == Operation_Sub)
        result = 0;
    for (uint32_t i = 0; units && i < count; ++i) {
        const TypedObject* operand = typedOf(operands.values->data[i]);
        if (multiply) {
            units = operand->kind == TypedKind_Unit && (operand->unit == PrCssUnit_Number || unit == PrCssUnit_Number);
            if (units && operand->unit != PrCssUnit_Number)
                unit = operand->unit;
            result *= operand->number;
        } else {
            units = operand->kind == TypedKind_Unit && operand->unit == unit;
            double x = operand->number;
            if (operation == Operation_Min)
                result = x < result ? x : result;
            else if (operation == Operation_Max)
                result = x > result ? x : result;
            else
                result += x;
        }
    }
    if (units)
        return newUnit(ctx, result, unit);

    TypedKind kind = TypedKind_Sum;
    if (multiply)
        kind = TypedKind_Product;
    else if (operation == Operation_Min)
        kind = TypedKind_Min;
    else if (operation == Operation_Max)
        kind = TypedKind_Max;
    JSValue math = newMath(ctx, kind, operands.take(), count);
    if (JS_IsException(math)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return JS_ThrowTypeError(ctx, "The values do not combine");
    }
    return math;
}

JSValue equals(JSContext* ctx, JSValueConst object, int argc, JSValueConst* argv) {
    if (typedIn(ctx, object, cssNumericValueClass) == nullptr)
        return JS_EXCEPTION;
    Tree tree = heldTree(ctx);
    if (!flattenNumeric(ctx, object, tree.value))
        return JS_EXCEPTION;
    bool equal = true;
    for (int i = 0; i < argc; ++i) {
        JSValue other = rectified(ctx, argv[i], Rectify_Numberish);
        if (JS_IsException(other))
            return other;
        Tree against = heldTree(ctx);
        bool numeric = isNumeric(typedOf(other)->kind);
        bool flattened = numeric && flattenNumeric(ctx, other, against.value);
        JS_FreeValue(ctx, other);
        if (numeric && !flattened)
            return JS_EXCEPTION;
        equal = equal && flattened && prCssValueEquals(tree.value, 0, against.value, 0);
    }
    return JS_NewBool(ctx, equal);
}

// Reads a unit's name. Throws a SyntaxError if it names no unit (css-typed-om-1/#cssnumericvalue-create-a-type).
bool unitArgument(JSContext* ctx, JSValueConst value, PrCssUnit* out) {
    StringArgument name = {ctx};
    if (!name.read(value))
        return false;
    if (!prCssUnitFromName(name.value(), out)) {
        jsDomExceptionThrow(ctx, "SyntaxError");
        return false;
    }
    return true;
}

JSValue to(JSContext* ctx, JSValueConst object, int argc, JSValueConst* argv) {
    PrCssUnit unit;
    if (typedIn(ctx, object, cssNumericValueClass) == nullptr || !jsArgCount(ctx, argc, 1) ||
            !unitArgument(ctx, argv[0], &unit))
        return JS_EXCEPTION;
    Tree tree = heldTree(ctx);
    if (!flattenNumeric(ctx, object, tree.value))
        return JS_EXCEPTION;
    PrCssNumericType type;
    double number = 0;
    if (!unitType(unit, &type) || prCssValueTo(tree.value, 0, unit, &number) != PrStatus_Ok)
        return JS_ThrowTypeError(ctx, "The value cannot be written in that unit");
    return newUnit(ctx, number, unit);
}

JSValue toSum(JSContext* ctx, JSValueConst object, int argc, JSValueConst* argv) {
    if (typedIn(ctx, object, cssNumericValueClass) == nullptr)
        return JS_EXCEPTION;
    RawArray<PrCssUnit> units;
    bool ok = true;
    for (int i = 0; ok && i < argc; ++i) {
        PrCssUnit unit;
        PrCssNumericType type;
        ok = unitArgument(ctx, argv[i], &unit);
        if (ok && !unitType(unit, &type)) {
            JS_ThrowTypeError(ctx, "The value cannot be written in that unit");
            ok = false;
        }
        if (ok)
            units.push(unit);
    }
    Tree tree = heldTree(ctx);
    ok = ok && flattenNumeric(ctx, object, tree.value);
    Tree sum = heldTree(ctx);
    PrStatus status = ok ? prCssValueToSum(tree.value, 0, units.data, units.count, sum.value) : PrStatus_Ok;
    units.reset();
    if (!ok)
        return JS_EXCEPTION;
    if (status != PrStatus_Ok)
        return JS_ThrowTypeError(ctx, "The value cannot be written in those units");
    JSValue result = reifyMath(ctx, sum.value, 0);
    return JS_IsUndefined(result) ? JS_ThrowTypeError(ctx, "The value cannot be written in those units") : result;
}

// css-typed-om-1/#dom-cssnumericvalue-type, as a CSSNumericType dictionary: the type that the value has kept since it
// was created.
JSValue type(JSContext* ctx, JSValueConst jsObject, int, JSValueConst*) {
    const TypedObject* object = typedIn(ctx, jsObject, cssNumericValueClass);
    if (object == nullptr)
        return JS_EXCEPTION;
    if (!object->hasType)
        return JS_ThrowTypeError(ctx, "The value has no type");
    const PrCssNumericType& numeric = object->type;
    constexpr PrStringView kBaseTypes[] = {
        "length",    "angle",      "time",
        "frequency", "resolution", "flex",
        "percent",
    };
    JSValue result = JS_NewObject(ctx);
    for (uint32_t i = 0; i < PrCssNumericBaseType_Count; ++i) {
        if (numeric.exponents[i] != 0)
            JS_SetPropertyStr(ctx, result, kBaseTypes[i].data, JS_NewInt32(ctx, numeric.exponents[i]));
    }
    if (numeric.hasPercentHint)
        JS_SetPropertyStr(ctx, result, "percentHint", jsFixedString(ctx, kBaseTypes[numeric.percentHint]));
    return result;
}

JSValue parseNumeric(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    StringArgument text = {ctx};
    if (!jsArgCount(ctx, argc, 1) || !text.read(argv[0]))
        return JS_EXCEPTION;
    Tree value = heldTree(ctx);
    if (prCssValueParseNumeric(text.value(), value.value) != PrStatus_Ok)
        return jsDomExceptionThrow(ctx, "SyntaxError");
    PrCssValueFields fields = prCssValueFields(value.value, 0);
    JSValue result = fields.kind == PrCssValueKind_Math ? reifyMath(ctx, value.value, 0) : reifyNumber(ctx, fields);
    return JS_IsUndefined(result) ? jsDomExceptionThrow(ctx, "SyntaxError") : result;
}

// ---------------------------------------------------------------------------------------------------------------------
// CSSUnitValue and the math values

JSValue unitConstruct(JSContext* ctx, JSValueConst newTarget, int argc, JSValueConst* argv) {
    double number = 0;
    StringArgument name = {ctx};
    if (!jsArgCount(ctx, argc, 2) || JS_ToFloat64(ctx, &number, argv[0]) < 0 || !name.read(argv[1]))
        return JS_EXCEPTION;
    if (!prF64IsFinite(number))
        return JS_ThrowTypeError(ctx, "The value is not finite");
    PrCssUnit unit;
    if (!prCssUnitFromName(name.value(), &unit))
        return JS_ThrowTypeError(ctx, "Not a unit");
    return construct(ctx, newTarget, cssUnitValueClass, typedFields(TypedKind_Unit, unit, number));
}

JSValue numberGet(JSContext* ctx, JSValueConst jsObject) {
    TypedObject* object = typedIn(ctx, jsObject, cssUnitValueClass);
    return object != nullptr ? JS_NewFloat64(ctx, object->number) : JS_EXCEPTION;
}

JSValue numberSet(JSContext* ctx, JSValueConst jsObject, JSValueConst input) {
    TypedObject* object = typedIn(ctx, jsObject, cssUnitValueClass);
    double number = 0;
    if (object == nullptr || JS_ToFloat64(ctx, &number, input) < 0)
        return JS_EXCEPTION;
    if (!prF64IsFinite(number))
        return JS_ThrowTypeError(ctx, "The value is not finite");
    object->number = number;
    return JS_UNDEFINED;
}

JSValue unitGet(JSContext* ctx, JSValueConst jsObject) {
    TypedObject* object = typedIn(ctx, jsObject, cssUnitValueClass);
    return object != nullptr ? jsNewStringView(ctx, prCssUnitName(object->unit)) : JS_EXCEPTION;
}

// css-typed-om-1/#enumdef-cssmathoperator
JSValue operatorGet(JSContext* ctx, JSValueConst jsObject) {
    TypedObject* object = typedIn(ctx, jsObject, cssMathValueClass);
    if (object == nullptr)
        return JS_EXCEPTION;
    constexpr PrStringView kOperators[] = {"sum",    "product", "negate", "invert", "min",     "max", "clamp"};
    return jsFixedString(ctx, kOperators[object->kind - TypedKind_Sum]);
}

// The constructor of a math value that takes a list. It holds its operands in a CSSNumericArray.
template <TypedKind Kind>
JSValue listConstruct(JSContext* ctx, JSValueConst newTarget, int argc, JSValueConst* argv) {
    if (argc == 0)
        return jsDomExceptionThrow(ctx, "SyntaxError");
    Values operands = heldValues(ctx);
    for (int i = 0; i < argc; ++i) {
        if (!operands.add(rectified(ctx, argv[i], Rectify_Numberish)))
            return JS_EXCEPTION;
    }
    return newMath(ctx, Kind, operands.take(), uint32_t(argc), newTarget);
}

// Returns a new object of `kind` that holds the first `count` arguments, each rectified as `rectify` specifies.
JSValue argumentsConstruct(JSContext* ctx, JSValueConst newTarget, int argc, JSValueConst* argv,
        TypedKind kind, int count, const Rectify* rectify, bool is2D = true) {
    if (!jsArgCount(ctx, argc, count))
        return JS_EXCEPTION;
    JSValue children[4];
    for (int i = 0; i < count; ++i) {
        children[i] = rectified(ctx, argv[i], rectify[i]);
        if (JS_IsException(children[i])) {
            for (int j = 0; j < i; ++j)
                JS_FreeValue(ctx, children[j]);
            return JS_EXCEPTION;
        }
    }
    return construct(ctx, newTarget, *kKindClasses[kind],
        typedFields(kind, PrCssUnit_Number, 0, is2D), children, uint32_t(count));
}

constexpr Rectify kNumberishArguments[] = {Rectify_Numberish, Rectify_Numberish, Rectify_Numberish, Rectify_Numberish};
constexpr Rectify kNumericArguments[] = {Rectify_None, Rectify_None, Rectify_None, Rectify_None};

constexpr Rectify kKeywordishArguments[] = {Rectify_Keywordish};

// The constructor of an object of `Kind` that holds its first `Count` arguments, each rectified as `Rectifies`
// specifies.
template <TypedKind Kind, int Count, const Rectify* PR_NONNULL Rectifies, bool Is2D = true>
JSValue fixedConstruct(JSContext* ctx, JSValueConst newTarget, int argc, JSValueConst* argv) {
    return argumentsConstruct(ctx, newTarget, argc, argv, Kind, Count, Rectifies, Is2D);
}

// ---------------------------------------------------------------------------------------------------------------------
// CSSTransformValue and the components (css-typed-om-1/#transformvalue-objects)

JSValue transformConstruct(JSContext* ctx, JSValueConst newTarget, int argc, JSValueConst* argv) {
    RawArray<JSValue> components;
    auto component = [&](JSValue value) {
        if (jsHandleIn(ctx, value, cssTransformComponentClass) == nullptr)
            return JS_ThrowTypeError(ctx, "Expected a CSSTransformComponent");
        return JS_DupValue(ctx, value);
    };
    if (!jsArgCount(ctx, argc, 1) || !sequence(ctx, argv[0], &components, component))
        return JS_EXCEPTION;
    if (components.count == 0) {
        components.reset();
        return JS_ThrowTypeError(ctx, "A transform has a component");
    }
    JSValue result = construct(ctx, newTarget, cssTransformValueClass,
        typedFields(TypedKind_Transform), components.data, components.count);
    components.reset();
    return result;
}

// Returns true if each component of a transform value is 2D.
JSValue transformIs2D(JSContext* ctx, JSValueConst componentObject) {
    TypedObject* object = typedIn(ctx, componentObject, cssTransformValueClass);
    if (object == nullptr)
        return JS_EXCEPTION;
    bool flat = true;
    for (uint32_t i = 0; flat && i < object->references; ++i) {
        JSValue component = jsReference(ctx, componentObject, i);
        TypedObject* held = typedOf(component);
        flat = held != nullptr && held->is2D;
        JS_FreeValue(ctx, component);
    }
    return JS_NewBool(ctx, flat);
}

JSValue componentIs2D(JSContext* ctx, JSValueConst jsObject) {
    TypedObject* object = typedIn(ctx, jsObject, cssTransformComponentClass);
    return object != nullptr ? JS_NewBool(ctx, object->is2D) : JS_EXCEPTION;
}

// Setting is2D on a skew or a perspective does nothing. A skew is always 2D, and a perspective never is
// (css-typed-om-1/#dom-cssskew-is2d, css-typed-om-1/#dom-cssperspective-is2d).
JSValue setComponentIs2D(JSContext* ctx, JSValueConst jsObject, JSValueConst input) {
    TypedObject* object = typedIn(ctx, jsObject, cssTransformComponentClass);
    if (object == nullptr)
        return JS_EXCEPTION;
    bool fixed = object->kind == TypedKind_Skew || object->kind == TypedKind_SkewX ||
        object->kind == TypedKind_SkewY || object->kind == TypedKind_Perspective;
    if (!fixed)
        object->is2D = JS_ToBool(ctx, input) != 0;
    return JS_UNDEFINED;
}

// Writes the matrix of a transform component (css-typed-om-1/#dom-csstransformcomponent-tomatrix). Returns false, with
// a TypeError thrown, for a component holding a length that is not a fixed number of pixels.
bool componentMatrix(JSContext* ctx, JSValueConst component, PrDomMatrix* out) {
    Tree tree = heldTree(ctx);
    if (!flattenComponent(ctx, component, tree.value))
        return false;
    if (prCssValueTransformMatrix(tree.value, 0, out) != PrStatus_Ok) {
        JS_ThrowTypeError(ctx, "The transform has a length no number of pixels gives");
        return false;
    }
    return true;
}

// css-typed-om-1/#dom-csstransformvalue-tomatrix, and a component's toMatrix.
JSValue toMatrix(JSContext* ctx, JSValueConst typedObject, int, JSValueConst*) {
    const TypedObject* object = typedOf(typedObject);
    if (object == nullptr || (object->kind != TypedKind_Transform && !isComponent(object->kind)))
        return JS_ThrowTypeError(ctx, "Illegal invocation");
    PrDomMatrix matrix = prDomMatrixIdentity();
    bool ok = true;
    if (object->kind != TypedKind_Transform) {
        ok = componentMatrix(ctx, typedObject, &matrix);
    } else {
        ok = eachChild(ctx, typedObject, object, [&](JSValueConst component) {
            PrDomMatrix next;
            if (!componentMatrix(ctx, component, &next))
                return false;
            prDomMatrixMultiply(&matrix, &next);
            return true;
        });
    }
    return ok ? prJsDomMatrixNew(ctx, matrix) : JS_EXCEPTION;
}

// css-typed-om-1/#dom-csstranslate-csstranslate. An omitted z is 0px and makes the translation 2D.
JSValue translateConstruct(JSContext* ctx, JSValueConst newTarget, int argc, JSValueConst* argv) {
    if (jsArgGiven(argc, argv, 2))
        return argumentsConstruct(ctx, newTarget, argc, argv, TypedKind_Translate, 3, kNumericArguments, false);
    if (!jsArgCount(ctx, argc, 2))
        return JS_EXCEPTION;
    JSValue args[3] = {argv[0], argv[1], newUnit(ctx, 0, PrCssUnit_Px)};
    JSValue result = argumentsConstruct(ctx, newTarget, 3, args, TypedKind_Translate, 3, kNumericArguments);
    JS_FreeValue(ctx, args[2]);
    return result;
}

// css-typed-om-1/#dom-cssrotate-cssrotate. An angle alone rotates about z, in 2D.
JSValue rotateConstruct(JSContext* ctx, JSValueConst newTarget, int argc, JSValueConst* argv) {
    constexpr Rectify kRotate[] = {Rectify_Numberish, Rectify_Numberish, Rectify_Numberish, Rectify_None};
    if (argc >= 4)
        return argumentsConstruct(ctx, newTarget, argc, argv, TypedKind_Rotate, 4, kRotate, false);
    if (argc != 1)
        return JS_ThrowTypeError(ctx, "CSSRotate takes an angle, or x, y, z and an angle");
    JSValue args[4] = {JS_NewFloat64(ctx, 0), JS_NewFloat64(ctx, 0), JS_NewFloat64(ctx, 1), argv[0]};
    return argumentsConstruct(ctx, newTarget, 4, args, TypedKind_Rotate, 4, kRotate);
}

// css-typed-om-1/#dom-cssscale-cssscale. An omitted z is 1 and makes the scale 2D.
JSValue scaleConstruct(JSContext* ctx, JSValueConst newTarget, int argc, JSValueConst* argv) {
    if (jsArgGiven(argc, argv, 2))
        return argumentsConstruct(ctx, newTarget, argc, argv, TypedKind_Scale, 3, kNumberishArguments, false);
    if (!jsArgCount(ctx, argc, 2))
        return JS_EXCEPTION;
    JSValue args[3] = {argv[0], argv[1], JS_NewFloat64(ctx, 1)};
    return argumentsConstruct(ctx, newTarget, 3, args, TypedKind_Scale, 3, kNumberishArguments);
}

// css-typed-om-1/#dom-cssmatrixcomponent-cssmatrixcomponent. The component holds a copy of a read-only matrix as a
// DOMMatrix. It is 2D as its options specify, or otherwise if the matrix is 2D.
JSValue matrixConstruct(JSContext* ctx, JSValueConst newTarget, int argc, JSValueConst* argv) {
    if (!jsArgCount(ctx, argc, 1))
        return JS_EXCEPTION;
    PrDomMatrix* matrix = prJsDomMatrixFromValue(ctx, argv[0]);
    if (matrix == nullptr)
        return JS_EXCEPTION;
    bool is2D = matrix->is2D;
    bool present = false;
    JSValueConst options = argc > 1 ? argv[1] : JS_UNDEFINED;
    if (!jsDictionary(ctx, options, &present) || (present && !jsDictBool(ctx, options, "is2D", &is2D)))
        return JS_EXCEPTION;
    JSValue held = prJsDomMatrixIsMutable(ctx, argv[0]) ? JS_DupValue(ctx, argv[0]) : prJsDomMatrixNew(ctx, *matrix);
    if (JS_IsException(held))
        return held;
    return construct(ctx, newTarget, cssMatrixComponentClass, typedFields(TypedKind_Matrix, PrCssUnit_Number, 0, is2D), &held, 1);
}

JSValue matrixGet(JSContext* ctx, JSValueConst object) {
    return typedIn(ctx, object, cssMatrixComponentClass) != nullptr ? jsReference(ctx, object, 0) : JS_EXCEPTION;
}

JSValue matrixSet(JSContext* ctx, JSValueConst object, JSValueConst input) {
    if (typedIn(ctx, object, cssMatrixComponentClass) == nullptr)
        return JS_EXCEPTION;
    if (!prJsDomMatrixIsMutable(ctx, input))
        return JS_ThrowTypeError(ctx, "Expected a DOMMatrix");
    jsSetReference(ctx, object, 0, input);
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------------------------------------------------
// CSSColorValue and its subclasses (css-typed-om-1/#colorvalue-objects)

// The IDL type that a channel argument converts to before it is rectified.
enum {
    ColorIdl_Channel,     // (CSSNumberish or CSSKeywordish), as for CSSColorRGBComp and the others
    ColorIdl_Numberish,   // CSSNumberish
    ColorIdl_Numeric,     // CSSNumericValue
};
typedef uint8_t ColorIdl;

// Returns the IDL type that a channel of `function` at `index` converts to. CSSHWB takes a CSSNumericValue for its hue
// and a CSSNumberish for the rest. CSSColor takes a CSSNumberish for its alpha.
ColorIdl colorIdl(ColorFunction function, uint32_t index, bool alpha) {
    if (function == ColorFunction_Hwb)
        return index == 0 ? ColorIdl_Numeric : ColorIdl_Numberish;
    return function == ColorFunction_Color && alpha ? ColorIdl_Numberish : ColorIdl_Channel;
}

// Returns an argument converted to `idl` (webidl/#js-union). A number is rectified in the form that a channel of `type`
// accepts: as a fraction of a percentage, a number, or degrees.
JSValue colorConvert(JSContext* ctx, JSValueConst value, ChannelType type, ColorIdl idl) {
    if (jsHandleIn(ctx, value, cssNumericValueClass) != nullptr ||
            (idl == ColorIdl_Channel && jsHandleIn(ctx, value, cssKeywordValueClass) != nullptr))
        return JS_DupValue(ctx, value);
    if (idl == ColorIdl_Numeric)
        return JS_ThrowTypeError(ctx, "Expected a CSSNumericValue");
    if (idl == ColorIdl_Numberish || JS_IsNumber(value)) {
        double number = 0;
        if (JS_ToFloat64(ctx, &number, value) < 0)
            return JS_EXCEPTION;
        if (!prF64IsFinite(number))
            return JS_ThrowTypeError(ctx, "The value is not finite");
        switch (type) {
            case ChannelType_Number: break;
            case ChannelType_Angle: return newUnit(ctx, number, PrCssUnit_Deg);
            default: return newUnit(ctx, number * 100, PrCssUnit_Percent);
        }
        return newUnit(ctx, number, PrCssUnit_Number);
    }
    JSValue text = JS_ToString(ctx, value);
    if (JS_IsException(text))
        return text;
    JSValue keyword = rectified(ctx, text, Rectify_Keywordish);
    JS_FreeValue(ctx, text);
    return keyword;
}

// css-typed-om-1/#rectify-a-csscolorrgbcomp and the steps after it, for a converted argument. Throws a SyntaxError for
// a value that the channel does not accept.
bool colorRectify(JSContext* ctx, JSValueConst value, ChannelType type) {
    Summary summary;
    if (!summaryOf(ctx, value, &summary))
        return false;
    if (!channelValid(summary, type)) {
        jsDomExceptionThrow(ctx, "SyntaxError");
        return false;
    }
    return true;
}

// Returns a channel value given for `type`, as its object, or JS_EXCEPTION.
JSValue colorChannel(JSContext* ctx, JSValueConst value, ChannelType type, ColorIdl idl) {
    JSValue channel = colorConvert(ctx, value, type, idl);
    if (!JS_IsException(channel) && !colorRectify(ctx, channel, type)) {
        JS_FreeValue(ctx, channel);
        return JS_EXCEPTION;
    }
    return channel;
}

// The set hook of a CSSColor's channels. css-typed-om-1/#dom-csscolor-channels rectifies each value that is set. A
// deleted value needs nothing.
JSValue channelsSet(JSContext* ctx, JSValueConst, JSValueConst value, uint32_t) {
    return colorChannel(ctx, value, ChannelType_Percent, ColorIdl_Channel);
}

constexpr JsObservableArrayHooks kChannelsHooks = {channelsSet, nullptr};

// Returns the channels of a CSSColor, holding `channels`, which it takes.
JSValue newChannels(JSContext* ctx, JSValueConst owner, JSValue* channels, uint32_t count) {
    JSValue array = jsObservableArrayNew(ctx, owner, &kChannelsHooks);
    JSValue items = JS_NewArray(ctx);
    for (uint32_t i = 0; i < count; ++i)
        JS_SetPropertyUint32(ctx, items, i, channels[i]);
    bool ok = !JS_IsException(array) && jsObservableArraySet(ctx, array, items);
    JS_FreeValue(ctx, items);
    if (!ok) {
        JS_FreeValue(ctx, array);
        return JS_EXCEPTION;
    }
    return array;
}

// The constructors of the color values other than CSSColor, which take three channels and an alpha. The alpha is 1 if
// it is not given.
template <ColorFunction Function>
JSValue colorConstruct(JSContext* ctx, JSValueConst newTarget, int argc, JSValueConst* argv) {
    if (!jsArgCount(ctx, argc, 3))
        return JS_EXCEPTION;
    JSValue children[4];
    int converted = 0;
    // Each argument is converted first, and then each is rectified. Only the alpha is optional. For the other channels,
    // undefined is a value.
    for (; converted < 4; ++converted) {
        JSValueConst given = converted < 3 || jsArgGiven(argc, argv, converted) ? argv[converted] : JS_NewFloat64(ctx, 1);
        ChannelType type = channelType(Function, uint32_t(converted));
        children[converted] = colorConvert(ctx, given, type, colorIdl(Function, uint32_t(converted), converted == 3));
        if (JS_IsException(children[converted]))
            break;
    }
    bool ok = converted == 4;
    for (int i = 0; ok && i < 4; ++i)
        ok = colorRectify(ctx, children[i], channelType(Function, uint32_t(i)));
    if (!ok) {
        for (int i = 0; i < converted; ++i)
            JS_FreeValue(ctx, children[i]);
        return JS_EXCEPTION;
    }
    TypedObject fields = typedFields(TypedKind_Color);
    fields.colorFunction = Function;
    return construct(ctx, newTarget, *kColorClasses[Function], fields, children, 4);
}

// css-typed-om-1/#dom-csscolor-csscolor, which takes a color space, a sequence of channels, and an alpha. The alpha is
// 1 if it is not given.
JSValue colorFunctionConstruct(JSContext* ctx, JSValueConst newTarget, int argc, JSValueConst* argv) {
    if (!jsArgCount(ctx, argc, 2))
        return JS_EXCEPTION;
    JSValue children[3] = {rectified(ctx, argv[0], Rectify_Keywordish), JS_UNDEFINED, JS_UNDEFINED};
    if (JS_IsException(children[0]))
        return children[0];
    RawArray<JSValue> channels;
    auto channel = [&](JSValue value) {
        return colorChannel(ctx, value, ChannelType_Percent, ColorIdl_Channel);
    };
    bool ok = sequence(ctx, argv[1], &channels, channel);
    if (ok) {
        JSValueConst given = jsArgGiven(argc, argv, 2) ? argv[2] : JS_NewFloat64(ctx, 1);
        children[2] = colorChannel(ctx, given, ChannelType_Percent, ColorIdl_Numberish);
        ok = !JS_IsException(children[2]);
    }
    JSValue object = JS_EXCEPTION;
    if (ok) {
        TypedObject fields = typedFields(TypedKind_Color);
        fields.colorFunction = ColorFunction_Color;
        object = newTyped(ctx, cssColorClass, fields);
        ok = !JS_IsException(object);
    }
    if (ok) {
        children[1] = newChannels(ctx, object, channels.data, channels.count);
        channels.count = 0;
        ok = !JS_IsException(children[1]);
    }
    JS_FreeValue(ctx, object);
    for (uint32_t i = 0; i < channels.count; ++i)
        JS_FreeValue(ctx, channels.data[i]);
    channels.reset();
    if (!ok) {
        for (JSValue child : children)
            JS_FreeValue(ctx, child);
        return JS_EXCEPTION;
    }
    TypedObject fields = typedFields(TypedKind_Color);
    fields.colorFunction = ColorFunction_Color;
    return construct(ctx, newTarget, cssColorClass, fields, children, 3);
}

// The setter of a color value's channel or alpha, held at the index that `magic` gives. A CSSColor holds its color
// space, channels and alpha at 0, 1 and 2.
JSValue colorSet(JSContext* ctx, JSValueConst channelsObject, JSValueConst input, int magic) {
    TypedObject* object = typedIn(ctx, channelsObject, cssColorValueClass);
    if (object == nullptr)
        return JS_EXCEPTION;
    uint32_t index = uint32_t(magic);
    JSValue value;
    if (object->colorFunction != ColorFunction_Color) {
        value = colorChannel(ctx, input, channelType(object->colorFunction, index),
            colorIdl(object->colorFunction, index, index == 3));
    } else if (index == 1) {
        JSValue channels = jsReference(ctx, channelsObject, 1);
        bool ok = jsObservableArraySet(ctx, channels, input);
        JS_FreeValue(ctx, channels);
        return ok ? JS_UNDEFINED : JS_EXCEPTION;
    } else if (index == 0) {
        JSValue text;
        if (jsHandleIn(ctx, input, cssKeywordValueClass) != nullptr)
            text = JS_DupValue(ctx, input);
        else
            text = JS_ToString(ctx, input);
        value = JS_IsException(text) ? text : rectified(ctx, text, Rectify_Keywordish);
        JS_FreeValue(ctx, text);
    } else {
        value = colorChannel(ctx, input, ChannelType_Percent, ColorIdl_Numberish);
    }
    if (JS_IsException(value))
        return value;
    jsSetReference(ctx, channelsObject, index, value);
    JS_FreeValue(ctx, value);
    return JS_UNDEFINED;
}

// css-typed-om-1/#dom-csscolorvalue-parse, which returns a color value. A color keyword that names no channels becomes
// a keyword, and a color that is not absolute becomes a CSSStyleValue.
JSValue parseColor(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    StringArgument text = {ctx};
    if (!jsArgCount(ctx, argc, 1) || !text.read(argv[0]))
        return JS_EXCEPTION;
    Tree value = heldTree(ctx);
    if (prCssValueParseColor(text.value(), value.value) != PrStatus_Ok)
        return jsDomExceptionThrow(ctx, "SyntaxError");
    PrCssValueFields fields = prCssValueFields(value.value, 0);
    if (fields.kind == PrCssValueKind_Keyword)
        return newKeyword(ctx, prCssKeywordName(fields.keyword));
    JSValue color = fields.kind == PrCssValueKind_Color ? reifyColor(ctx, value.value, 0) : JS_UNDEFINED;
    if (!JS_IsUndefined(color))
        return color;
    PrCssDeclarationName none = prCssDeclarationNameFromProperty(PrCssProperty_Count);
    return newStyle(ctx, TypedKind_Style, value.value, 0, prCssValueSize(value.value), &none);
}

// ---------------------------------------------------------------------------------------------------------------------
// StylePropertyMapReadOnly and StylePropertyMap (css-typed-om-1/#the-stylepropertymap), which are views of a
// declaration block

// Implements get, getAll and has, selected by the magic.
JSValue mapGet(JSContext* ctx, JSValueConst mapObject, int argc, JSValueConst* argv, int magic) {
    PrCssStyleDeclaration* map = jsThis<PrCssStyleDeclaration>(ctx, mapObject, stylePropertyMapReadOnlyClass);
    PropertyArgument property = {};
    if (map == nullptr || !jsArgCount(ctx, argc, 1) || !property.read(ctx, argv[0]))
        return JS_EXCEPTION;
    Tree value = heldTree(ctx);
    if (prCssStyleDeclarationValue(map, &property.name, value.value) != PrStatus_Ok)
        return JS_ThrowTypeError(ctx, "Not a property");
    if (magic == 0)
        return reifyFirst(ctx, value.value, &property.name);
    if (magic == 1)
        return reifyArray(ctx, value.value, &property.name);
    return JS_NewBool(ctx, prCssValueSize(value.value) != 0);
}

JSValue mapSize(JSContext* ctx, JSValueConst mapObject) {
    PrCssStyleDeclaration* map = jsThis<PrCssStyleDeclaration>(ctx, mapObject, stylePropertyMapReadOnlyClass);
    return map != nullptr ? JS_NewUint32(ctx, prCssStyleDeclarationLength(map)) : JS_EXCEPTION;
}

// Returns the map's entries, as an array of [property, values] arrays.
JSValue mapEntries(JSContext* ctx, PrCssStyleDeclaration* map) {
    JSValue entries = JS_NewArray(ctx);
    for (uint32_t i = 0; i < prCssStyleDeclarationLength(map); ++i) {
        PrCssDeclarationName name = {};
        prCssStyleDeclarationNameAt(map, i, &name);
        Tree value = heldTree(ctx);
        prCssStyleDeclarationValue(map, &name, value.value);
        JSValue items = reifyArray(ctx, value.value, &name);
        if (JS_IsException(items)) {
            prCssDeclarationNameReset(&name);
            JS_FreeValue(ctx, entries);
            return items;
        }
        JSValue key;
        if (name.property != PrCssProperty_Count)
            key = jsFixedString(ctx, prCssPropertyName(name.property));
        else
            key = jsNewString(ctx, &name.author);
        JS_SetPropertyUint32(ctx, entries, i, jsEntry(ctx, key, items));
        prCssDeclarationNameReset(&name);
    }
    return entries;
}

// Implements entries, keys and values, selected by the magic, which is the JsMapPart.
JSValue mapIterate(JSContext* ctx, JSValueConst mapObject, int, JSValueConst*, int magic) {
    PrCssStyleDeclaration* map = jsThis<PrCssStyleDeclaration>(ctx, mapObject, stylePropertyMapReadOnlyClass);
    if (map == nullptr)
        return JS_EXCEPTION;
    JSValue entries = mapEntries(ctx, map);
    return JS_IsException(entries) ? entries : jsMapIterate(ctx, entries, JsMapPart(magic));
}

JSValue mapForEach(JSContext* ctx, JSValueConst mapObject, int argc, JSValueConst* argv) {
    PrCssStyleDeclaration* map = jsThis<PrCssStyleDeclaration>(ctx, mapObject, stylePropertyMapReadOnlyClass);
    if (map == nullptr)
        return JS_EXCEPTION;
    JSValue entries = mapEntries(ctx, map);
    return JS_IsException(entries) ? entries : jsMapForEach(ctx, mapObject, entries, argc, argv);
}

// Returns true if two declaration names name the same property.
bool propertyEquals(const PrCssDeclarationName& a, const PrCssDeclarationName& b) {
    if (a.property != b.property)
        return false;
    if (a.property != PrCssProperty_Count)
        return true;
    return prRawStringView(&a.author) == prRawStringView(&b.author);
}

// Returns true if a value holds tokens, which append() does not accept (css-typed-om-1/#dom-stylepropertymap-append).
bool holdsTokens(const PrCssValue* value) {
    for (uint32_t node = 0; node < prCssValueSize(value); ++node) {
        if (prCssValueFields(value, node).kind == PrCssValueKind_Tokens)
            return true;
    }
    return false;
}

// Implements set and append, selected by the magic. Each value is a CSSStyleValue, or a string parsed for the property
// (css-typed-om-1/#dom-stylepropertymap-set, css-typed-om-1/#dom-stylepropertymap-append).
JSValue mapSet(JSContext* ctx, JSValueConst mapObject, int argc, JSValueConst* argv, int magic) {
    PrCssStyleDeclaration* map = jsThis<PrCssStyleDeclaration>(ctx, mapObject, prJsCssStylePropertyMapClass);
    PropertyArgument property = {};
    if (map == nullptr || !jsArgCount(ctx, argc, 1) || !property.read(ctx, argv[0]))
        return JS_EXCEPTION;
    bool append = magic == 1;
    bool list = property.name.property != PrCssProperty_Count && prCssPropertyListValued(property.name.property);
    if ((append && !list) || (!list && argc > 2))
        return JS_ThrowTypeError(ctx, "The property does not take the values");
    Tree values = heldTree(ctx);
    for (int i = 1; i < argc; ++i) {
        if (i > 1 && !addNode(ctx, values.value, literalFields(',')))
            return JS_EXCEPTION;
        const TypedObject* object = typedOf(argv[i]);
        if (object != nullptr) {
            bool styled = object->kind == TypedKind_Style || object->kind == TypedKind_Image;
            bool taken = !(styled && !propertyEquals(object->property, property.name)) &&
                !(append && object->kind == TypedKind_Unparsed);
            if (!taken)
                return JS_ThrowTypeError(ctx, "The property does not take the value");
            if (!flattenStyle(ctx, argv[i], values.value))
                return JS_EXCEPTION;
            continue;
        }
        StringArgument text = {ctx};
        if (!text.read(argv[i]))
            return JS_EXCEPTION;
        Tree parsed = heldTree(ctx);
        PrStatus status = prCssValueParse(&property.name, text.value(), parsed.value);
        if (status != PrStatus_Ok || (append && holdsTokens(parsed.value)))
            return JS_ThrowTypeError(ctx, "The property does not take the value");
        for (uint32_t tree = 0; tree < prCssValueSize(parsed.value); tree = prCssValueNext(parsed.value, tree)) {
            if (!built(ctx, prCssValueAddTree(values.value, parsed.value, tree)))
                return JS_EXCEPTION;
        }
    }
    PrStatus status;
    if (append)
        status = prCssStyleDeclarationAppendValue(map, &property.name, values.value);
    else
        status = prCssStyleDeclarationSetValue(map, &property.name, values.value, false);
    if (status != PrStatus_Ok)
        return JS_ThrowTypeError(ctx, "The property does not take the values");
    return JS_UNDEFINED;
}

JSValue mapDelete(JSContext* ctx, JSValueConst mapObject, int argc, JSValueConst* argv) {
    PrCssStyleDeclaration* map = jsThis<PrCssStyleDeclaration>(ctx, mapObject, prJsCssStylePropertyMapClass);
    PropertyArgument property = {};
    if (map == nullptr || !jsArgCount(ctx, argc, 1) || !property.read(ctx, argv[0]))
        return JS_EXCEPTION;
    prCssStyleDeclarationRemoveProperty(map, &property.name, nullptr);
    return JS_UNDEFINED;
}

JSValue mapClear(JSContext* ctx, JSValueConst mapObject, int, JSValueConst*) {
    PrCssStyleDeclaration* map = jsThis<PrCssStyleDeclaration>(ctx, mapObject, prJsCssStylePropertyMapClass);
    if (map == nullptr)
        return JS_EXCEPTION;
    prCssStyleDeclarationSetCssText(map, "");
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------------------------------------------------
// The CSS namespace's factories (css-typed-om-1/#numeric-factory), one per unit, each creating a CSSUnitValue in that
// unit

JSValue factory(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv, int magic) {
    double number = 0;
    if (!jsArgCount(ctx, argc, 1) || JS_ToFloat64(ctx, &number, argv[0]) < 0)
        return JS_EXCEPTION;
    if (!prF64IsFinite(number))
        return JS_ThrowTypeError(ctx, "The value is not finite");
    return newUnit(ctx, number, PrCssUnit(magic));
}

// Returns a factory's name: the unit's name as the namespace spells it, which is lowercase except for three units.
PrStringView factoryName(PrCssUnit unit) {
    switch (unit) {
        case PrCssUnit_Q: return "Q";
        case PrCssUnit_Hz: return "Hz";
        case PrCssUnit_KHz: return "kHz";
        default: return prCssUnitName(unit);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Classes

#define PR_HELD(name, index) PR_JS_MAGIC_GET(name, heldGet, index)
#define PR_HELD_SET(name, index) PR_JS_MAGIC_GET_SET(name, heldGet, heldSet, index)

constexpr JSCFunctionListEntry kStyleValueProto[] = {PR_JS_TAG("CSSStyleValue"), PR_JS_METHOD("toString", 0, toString)};

constexpr JSCFunctionListEntry kStyleValueStatic[] = {
    PR_JS_MAGIC("parse", 2, parse, 0),
    PR_JS_MAGIC("parseAll", 2, parse, 1),
};

constexpr JSCFunctionListEntry kKeywordProto[] = {
    PR_JS_TAG("CSSKeywordValue"),
    PR_JS_GET_SET("value", textGet, textSet),
};

constexpr JSCFunctionListEntry kUnparsedProto[] = {PR_JS_TAG("CSSUnparsedValue"), PR_JS_GET("length", lengthGet)};

constexpr JSCFunctionListEntry kVariableProto[] = {
    PR_JS_TAG("CSSVariableReferenceValue"),
    PR_JS_GET_SET("variable", textGet, textSet),
    PR_HELD("fallback", 0),
};

constexpr JSCFunctionListEntry kNumericProto[] = {
    PR_JS_TAG("CSSNumericValue"),
    PR_JS_MAGIC("add", 0, combine, Operation_Add),
    PR_JS_MAGIC("sub", 0, combine, Operation_Sub),
    PR_JS_MAGIC("mul", 0, combine, Operation_Mul),
    PR_JS_MAGIC("div", 0, combine, Operation_Div),
    PR_JS_MAGIC("min", 0, combine, Operation_Min),
    PR_JS_MAGIC("max", 0, combine, Operation_Max),
    PR_JS_METHOD("equals", 0, equals),
    PR_JS_METHOD("to", 1, to),
    PR_JS_METHOD("toSum", 0, toSum),
    PR_JS_METHOD("type", 0, type),
};

constexpr JSCFunctionListEntry kNumericStatic[] = {PR_JS_METHOD("parse", 1, parseNumeric)};

constexpr JSCFunctionListEntry kUnitProto[] = {
    PR_JS_TAG("CSSUnitValue"),
    PR_JS_GET_SET("value", numberGet, numberSet),
    PR_JS_GET("unit", unitGet),
};

constexpr JSCFunctionListEntry kMathProto[] = {PR_JS_TAG("CSSMathValue"), PR_JS_GET("operator", operatorGet)};

constexpr JSCFunctionListEntry kSumProto[] = {PR_JS_TAG("CSSMathSum"), PR_HELD("values", 0)};
constexpr JSCFunctionListEntry kProductProto[] = {PR_JS_TAG("CSSMathProduct"), PR_HELD("values", 0)};
constexpr JSCFunctionListEntry kNegateProto[] = {PR_JS_TAG("CSSMathNegate"), PR_HELD("value", 0)};
constexpr JSCFunctionListEntry kInvertProto[] = {PR_JS_TAG("CSSMathInvert"), PR_HELD("value", 0)};
constexpr JSCFunctionListEntry kMinProto[] = {PR_JS_TAG("CSSMathMin"), PR_HELD("values", 0)};
constexpr JSCFunctionListEntry kMaxProto[] = {PR_JS_TAG("CSSMathMax"), PR_HELD("values", 0)};

constexpr JSCFunctionListEntry kClampProto[] = {
    PR_JS_TAG("CSSMathClamp"),
    PR_HELD("lower", 0),
    PR_HELD("value", 1),
    PR_HELD("upper", 2),
};

constexpr JSCFunctionListEntry kNumericArrayProto[] = {PR_JS_TAG("CSSNumericArray"), PR_JS_GET("length", lengthGet)};

constexpr JSCFunctionListEntry kTransformProto[] = {
    PR_JS_TAG("CSSTransformValue"),
    PR_JS_GET("length", lengthGet),
    PR_JS_GET("is2D", transformIs2D),
    PR_JS_METHOD("toMatrix", 0, toMatrix),
};

constexpr JSCFunctionListEntry kComponentProto[] = {
    PR_JS_TAG("CSSTransformComponent"),
    PR_JS_GET_SET("is2D", componentIs2D, setComponentIs2D),
    PR_JS_METHOD("toMatrix", 0, toMatrix),
    PR_JS_METHOD("toString", 0, toString),
};

constexpr JSCFunctionListEntry kTranslateProto[] = {
    PR_JS_TAG("CSSTranslate"),
    PR_HELD_SET("x", 0),
    PR_HELD_SET("y", 1),
    PR_HELD_SET("z", 2),
};

constexpr JSCFunctionListEntry kRotateProto[] = {
    PR_JS_TAG("CSSRotate"),
    PR_HELD_SET("x", 0 | kNumberish),
    PR_HELD_SET("y", 1 | kNumberish),
    PR_HELD_SET("z", 2 | kNumberish),
    PR_HELD_SET("angle", 3),
};

constexpr JSCFunctionListEntry kScaleProto[] = {
    PR_JS_TAG("CSSScale"),
    PR_HELD_SET("x", 0 | kNumberish),
    PR_HELD_SET("y", 1 | kNumberish),
    PR_HELD_SET("z", 2 | kNumberish),
};

constexpr JSCFunctionListEntry kSkewProto[] = {PR_JS_TAG("CSSSkew"), PR_HELD_SET("ax", 0), PR_HELD_SET("ay", 1)};
constexpr JSCFunctionListEntry kSkewXProto[] = {PR_JS_TAG("CSSSkewX"), PR_HELD_SET("ax", 0)};
constexpr JSCFunctionListEntry kSkewYProto[] = {PR_JS_TAG("CSSSkewY"), PR_HELD_SET("ay", 0)};
constexpr JSCFunctionListEntry kPerspectiveProto[] = {PR_JS_TAG("CSSPerspective"), PR_HELD_SET("length", 0)};

constexpr JSCFunctionListEntry kMatrixProto[] = {
    PR_JS_TAG("CSSMatrixComponent"),
    PR_JS_GET_SET("matrix", matrixGet, matrixSet),
};

constexpr JSCFunctionListEntry kImageProto[] = {PR_JS_TAG("CSSImageValue")};

#define PR_CHANNEL(name, index) PR_JS_MAGIC_GET_SET(name, heldGet, colorSet, index)

constexpr JSCFunctionListEntry kColorValueProto[] = {PR_JS_TAG("CSSColorValue")};
constexpr JSCFunctionListEntry kColorValueStatic[] = {PR_JS_METHOD("parse", 1, parseColor)};

constexpr JSCFunctionListEntry kRgbProto[] = {
    PR_JS_TAG("CSSRGB"), PR_CHANNEL("r", 0),     PR_CHANNEL("g", 1),
    PR_CHANNEL("b", 2),  PR_CHANNEL("alpha", 3),
};
constexpr JSCFunctionListEntry kHslProto[] = {
    PR_JS_TAG("CSSHSL"), PR_CHANNEL("h", 0),     PR_CHANNEL("s", 1),
    PR_CHANNEL("l", 2),  PR_CHANNEL("alpha", 3),
};
constexpr JSCFunctionListEntry kHwbProto[] = {
    PR_JS_TAG("CSSHWB"), PR_CHANNEL("h", 0),     PR_CHANNEL("w", 1),
    PR_CHANNEL("b", 2),  PR_CHANNEL("alpha", 3),
};
constexpr JSCFunctionListEntry kLabProto[] = {
    PR_JS_TAG("CSSLab"), PR_CHANNEL("l", 0),     PR_CHANNEL("a", 1),
    PR_CHANNEL("b", 2),  PR_CHANNEL("alpha", 3),
};
constexpr JSCFunctionListEntry kLchProto[] = {
    PR_JS_TAG("CSSLCH"), PR_CHANNEL("l", 0),     PR_CHANNEL("c", 1),
    PR_CHANNEL("h", 2),  PR_CHANNEL("alpha", 3),
};
constexpr JSCFunctionListEntry kOklabProto[] = {
    PR_JS_TAG("CSSOKLab"), PR_CHANNEL("l", 0),     PR_CHANNEL("a", 1),
    PR_CHANNEL("b", 2),    PR_CHANNEL("alpha", 3),
};
constexpr JSCFunctionListEntry kOklchProto[] = {
    PR_JS_TAG("CSSOKLCH"), PR_CHANNEL("l", 0),     PR_CHANNEL("c", 1),
    PR_CHANNEL("h", 2),    PR_CHANNEL("alpha", 3),
};
constexpr JSCFunctionListEntry kColorProto[] = {
    PR_JS_TAG("CSSColor"),
    PR_CHANNEL("colorSpace", 0),
    PR_CHANNEL("channels", 1),
    PR_CHANNEL("alpha", 2),
};

#undef PR_CHANNEL

constexpr JSCFunctionListEntry kMapReadOnlyProto[] = {
    PR_JS_TAG("StylePropertyMapReadOnly"),
    PR_JS_MAGIC("get", 1, mapGet, 0),
    PR_JS_MAGIC("getAll", 1, mapGet, 1),
    PR_JS_MAGIC("has", 1, mapGet, 2),
    PR_JS_GET("size", mapSize),
    PR_JS_MAGIC("entries", 0, mapIterate, JsMapPart_Entries),
    PR_JS_MAGIC("keys", 0, mapIterate, JsMapPart_Keys),
    PR_JS_MAGIC("values", 0, mapIterate, JsMapPart_Values),
    PR_JS_METHOD("forEach", 1, mapForEach),
};

constexpr JSCFunctionListEntry kMapProto[] = {
    PR_JS_TAG("StylePropertyMap"),
    PR_JS_MAGIC("set", 1, mapSet, 0),
    PR_JS_MAGIC("append", 1, mapSet, 1),
    PR_JS_METHOD("delete", 1, mapDelete),
    PR_JS_METHOD("clear", 0, mapClear),
};

#undef PR_HELD
#undef PR_HELD_SET

// A class whose objects hold a TypedObject.
#define PR_TYPED(interface, id, proto)                                                             \
    PR_JS_CLASS(interface, id, (jsFinalizer<&id, destroyTyped>), proto)

constexpr JsClassDefinition kClasses[] = {
    {
        PR_TYPED("CSSStyleValue", cssStyleValueClass, kStyleValueProto),
        .statics = kStyleValueStatic,
        .staticCount = int(countOf(kStyleValueStatic)),
        .slot = typedBinding,
    },
    {
        PR_TYPED("CSSKeywordValue", cssKeywordValueClass, kKeywordProto),
        .constructor = textConstruct<TypedKind_Keyword>,
        .length = 1,
        .base = &cssStyleValueClass,
    },
    {
        PR_TYPED("CSSUnparsedValue", cssUnparsedValueClass, kUnparsedProto),
        .constructor = unparsedConstruct,
        .length = 1,
        .base = &cssStyleValueClass,
        .indexed = &kWriteIndexed,
        .iterable = true,
    },
    {
        PR_TYPED("CSSVariableReferenceValue", cssVariableReferenceValueClass, kVariableProto),
        .constructor = textConstruct<TypedKind_VariableReference>,
        .length = 1,
        .slot = typedBinding,
    },
    {
        PR_TYPED("CSSNumericValue", cssNumericValueClass, kNumericProto),
        .statics = kNumericStatic,
        .staticCount = int(countOf(kNumericStatic)),
        .base = &cssStyleValueClass,
    },
    {
        PR_TYPED("CSSUnitValue", cssUnitValueClass, kUnitProto),
        .constructor = unitConstruct,
        .length = 2,
        .base = &cssNumericValueClass,
    },
    {PR_TYPED("CSSMathValue", cssMathValueClass, kMathProto), .base = &cssNumericValueClass},
    {
        PR_TYPED("CSSMathSum", cssMathSumClass, kSumProto),
        .constructor = listConstruct<TypedKind_Sum>,
        .base = &cssMathValueClass,
    },
    {
        PR_TYPED("CSSMathProduct", cssMathProductClass, kProductProto),
        .constructor = listConstruct<TypedKind_Product>,
        .base = &cssMathValueClass,
    },
    {
        PR_TYPED("CSSMathNegate", cssMathNegateClass, kNegateProto),
        .constructor = fixedConstruct<TypedKind_Negate,
        1,
        kNumberishArguments>,
        .length = 1,
        .base = &cssMathValueClass,
    },
    {
        PR_TYPED("CSSMathInvert", cssMathInvertClass, kInvertProto),
        .constructor = fixedConstruct<TypedKind_Invert,
        1,
        kNumberishArguments>,
        .length = 1,
        .base = &cssMathValueClass,
    },
    {
        PR_TYPED("CSSMathMin", cssMathMinClass, kMinProto),
        .constructor = listConstruct<TypedKind_Min>,
        .base = &cssMathValueClass,
    },
    {
        PR_TYPED("CSSMathMax", cssMathMaxClass, kMaxProto),
        .constructor = listConstruct<TypedKind_Max>,
        .base = &cssMathValueClass,
    },
    {
        PR_TYPED("CSSMathClamp", cssMathClampClass, kClampProto),
        .constructor = fixedConstruct<TypedKind_Clamp,
        3,
        kNumberishArguments>,
        .length = 3,
        .base = &cssMathValueClass,
    },
    {
        PR_TYPED("CSSNumericArray", cssNumericArrayClass, kNumericArrayProto),
        .indexed = &kReadIndexed,
        .iterable = true,
        .slot = typedBinding,
    },
    {
        PR_TYPED("CSSTransformValue", cssTransformValueClass, kTransformProto),
        .constructor = transformConstruct,
        .length = 1,
        .base = &cssStyleValueClass,
        .indexed = &kWriteIndexed,
        .iterable = true,
    },
    {PR_TYPED("CSSTransformComponent", cssTransformComponentClass, kComponentProto), .slot = typedBinding},
    {
        PR_TYPED("CSSTranslate", cssTranslateClass, kTranslateProto),
        .constructor = translateConstruct,
        .length = 2,
        .base = &cssTransformComponentClass,
    },
    {
        PR_TYPED("CSSRotate", cssRotateClass, kRotateProto),
        .constructor = rotateConstruct,
        .length = 1,
        .base = &cssTransformComponentClass,
    },
    {
        PR_TYPED("CSSScale", cssScaleClass, kScaleProto),
        .constructor = scaleConstruct,
        .length = 2,
        .base = &cssTransformComponentClass,
    },
    {
        PR_TYPED("CSSSkew", cssSkewClass, kSkewProto),
        .constructor = fixedConstruct<TypedKind_Skew,
        2,
        kNumericArguments>,
        .length = 2,
        .base = &cssTransformComponentClass,
    },
    {
        PR_TYPED("CSSSkewX", cssSkewXClass, kSkewXProto),
        .constructor = fixedConstruct<TypedKind_SkewX,
        1,
        kNumericArguments>,
        .length = 1,
        .base = &cssTransformComponentClass,
    },
    {
        PR_TYPED("CSSSkewY", cssSkewYClass, kSkewYProto),
        .constructor = fixedConstruct<TypedKind_SkewY,
        1,
        kNumericArguments>,
        .length = 1,
        .base = &cssTransformComponentClass,
    },
    {
        PR_TYPED("CSSPerspective", cssPerspectiveClass, kPerspectiveProto),
        .constructor = fixedConstruct<TypedKind_Perspective,
        1,
        kKeywordishArguments,
        false>,
        .length = 1,
        .base = &cssTransformComponentClass,
    },
    {
        PR_TYPED("CSSMatrixComponent", cssMatrixComponentClass, kMatrixProto),
        .constructor = matrixConstruct,
        .length = 1,
        .base = &cssTransformComponentClass,
    },
    {PR_TYPED("CSSImageValue", cssImageValueClass, kImageProto), .base = &cssStyleValueClass},
    {
        PR_TYPED("CSSColorValue", cssColorValueClass, kColorValueProto),
        .statics = kColorValueStatic,
        .staticCount = int(countOf(kColorValueStatic)),
        .base = &cssStyleValueClass,
    },
    {
        PR_TYPED("CSSRGB", cssRgbClass, kRgbProto),
        .constructor = colorConstruct<ColorFunction_Rgb>,
        .length = 3,
        .base = &cssColorValueClass,
    },
    {
        PR_TYPED("CSSHSL", cssHslClass, kHslProto),
        .constructor = colorConstruct<ColorFunction_Hsl>,
        .length = 3,
        .base = &cssColorValueClass,
    },
    {
        PR_TYPED("CSSHWB", cssHwbClass, kHwbProto),
        .constructor = colorConstruct<ColorFunction_Hwb>,
        .length = 3,
        .base = &cssColorValueClass,
    },
    {
        PR_TYPED("CSSLab", cssLabClass, kLabProto),
        .constructor = colorConstruct<ColorFunction_Lab>,
        .length = 3,
        .base = &cssColorValueClass,
    },
    {
        PR_TYPED("CSSLCH", cssLchClass, kLchProto),
        .constructor = colorConstruct<ColorFunction_Lch>,
        .length = 3,
        .base = &cssColorValueClass,
    },
    {
        PR_TYPED("CSSOKLab", cssOklabClass, kOklabProto),
        .constructor = colorConstruct<ColorFunction_Oklab>,
        .length = 3,
        .base = &cssColorValueClass,
    },
    {
        PR_TYPED("CSSOKLCH", cssOklchClass, kOklchProto),
        .constructor = colorConstruct<ColorFunction_Oklch>,
        .length = 3,
        .base = &cssColorValueClass,
    },
    {
        PR_TYPED("CSSColor", cssColorClass, kColorProto),
        .constructor = colorFunctionConstruct,
        .length = 2,
        .base = &cssColorValueClass,
    },
    // webidl/#define-the-iteration-methods: a pair iterable's @@iterator is entries.
    {
        PR_JS_CLASS("StylePropertyMapReadOnly", stylePropertyMapReadOnlyClass,
            jsViewFinalizer<&stylePropertyMapReadOnlyClass>, kMapReadOnlyProto),
        .iterator = "entries",
    },
    {
        PR_JS_CLASS("StylePropertyMap", prJsCssStylePropertyMapClass, jsViewFinalizer<&prJsCssStylePropertyMapClass>,
            kMapProto),
        .base = &stylePropertyMapReadOnlyClass,
    },
};

#undef PR_TYPED

} // namespace

} // namespace pr_impl

using namespace pr_impl;

// The classes that other modules' bindings refer to.
JSClassID prJsCssStylePropertyMapClass;

void prJsCssTypedOmBind(JSContext* ctx, JSValueConst css) {
    jsObservableArrayBind(ctx);
    for (const JsClassDefinition& definition : kClasses)
        JS_FreeValue(ctx, jsDefineClass(ctx, definition));
    // Every class has been numbered by now, so the numbers read here are the ones that this runtime assigned.
    for (const JsClassDefinition& definition : kClasses) {
        for (const JSClassID* base : kTypedBases) {
            if (jsDerives(JS_GetRuntime(ctx), *definition.id, *base))
                jsClassSetAdd(&typedClasses, *definition.id);
        }
    }

    // A factory for each unit that a CSSUnitValue can have.
    for (PrCssUnit unit = 0; unit < PrCssUnit_Count; ++unit) {
        PrCssNumericType type;
        if (!unitType(unit, &type))
            continue;
        PrStringView name = factoryName(unit);
        char terminated[16] = {};
        prMemCopy(terminated, name.data, name.size < sizeof(terminated) - 1 ? name.size : sizeof(terminated) - 1);
        JSValue function =
            JS_NewCFunctionMagic(ctx, JsCallScoped<factory>::call, terminated, 1, JS_CFUNC_generic_magic, int(unit));
        JS_DefinePropertyValueStr(ctx, css, terminated, function,
            JS_PROP_ENUMERABLE | JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    }
}
