// JavaScript binding of SVG.
//
// SVG2 - https://svgwg.org/svg2-draft/

#include "pr/svg_quickjs.h"

#include "pr/count_of.h"
#include "pr/css_quickjs.h"
#include "pr/dom_geometry_quickjs.h"
#include "pr/dom_quickjs.h"
#include "pr/float.h"
#include "pr/quickjs_binding.h"
#include "pr/svg.h"
#include "pr/svg_length.h"
#include "pr/webidl_quickjs.h"

namespace pr_impl {

JSClassID svgRectElementClass;
JSClassID svgCircleElementClass;
JSClassID svgEllipseElementClass;
JSClassID svgLineElementClass;
JSClassID svgPathElementClass;
JSClassID svgPolylineElementClass;
JSClassID svgPolygonElementClass;

namespace {

// The class of each interface, by PrSvgInterface.
JSClassID* const kInterfaceClasses[] = {
    &prJsSvgElementClass, &prJsSvgGraphicsElementClass, &svgRectElementClass, &svgCircleElementClass,
    &svgEllipseElementClass, &svgLineElementClass, &svgPathElementClass, &svgPolylineElementClass,
    &svgPolygonElementClass, &prJsSvgSvgElementClass, &prJsSvgTextContentElementClass, &prJsSvgAElementClass,
    &prJsSvgUseElementClass, &prJsSvgSwitchElementClass, &prJsSvgSymbolElementClass, &prJsSvgScriptElementClass,
    &prJsSvgViewElementClass, &prJsSvgImageElementClass, &prJsSvgForeignObjectElementClass,
    &prJsSvgAnimateElementClass, &prJsSvgSetElementClass, &prJsSvgAnimateMotionElementClass,
    &prJsSvgAnimateTransformElementClass,
};

JSClassID classifyElement(void* handle) {
    PrDomElement* element = static_cast<PrDomElement*>(handle);
    PrSvgElement* svg = prSvgElementQuery(prDomElementAsEventTarget(element));
    if (svg == nullptr)
        return prJsDomElementClass;
    return *kInterfaceClasses[prSvgElementInterface(svg)];
}

PrSvgElement* PR_NULLABLE textContentOf(JSContext* ctx, JSValueConst self) {
    return jsThis<PrSvgElement>(ctx, self, prJsSvgTextContentElementClass);
}

// Reads argument `index` as an unsigned long (webidl/#js-unsigned-long), which must be given.
bool indexOf(JSContext* ctx, int argc, JSValueConst* argv, int index, uint32_t* out) {
    return jsArgCount(ctx, argc, index + 1) && JS_ToUint32(ctx, out, argv[index]) >= 0;
}

// Returns `value`, or throws an IndexSizeError for a character that is not there
// (SVG2/text.html#__svg__SVGTextContentElement__getSubStringLength).
JSValue orIndexSize(JSContext* ctx, PrStatus status, JSValue value) {
    if (status == PrStatus_Ok)
        return value;
    JS_FreeValue(ctx, value);
    return jsDomExceptionThrow(ctx, "IndexSizeError");
}

// ---------------------------------------------------------------------------------------------------------------------
// SVGGraphicsElement

// Reads a boolean member of a dictionary, or `fallback` if it is undefined.
bool readMember(JSContext* ctx, JSValueConst dictionary, const char* name, bool fallback, bool* out) {
    JSValue value = JS_GetPropertyStr(ctx, dictionary, name);
    if (JS_IsException(value))
        return false;
    int truth = JS_IsUndefined(value) ? int(fallback) : JS_ToBool(ctx, value);
    JS_FreeValue(ctx, value);
    *out = truth == 1;
    return truth >= 0;
}

// Implements getBBox (SVG2/types.html#__svg__SVGGraphicsElement__getBBox) and its SVGBoundingBoxOptions argument.
JSValue getBBox(JSContext* ctx, JSValueConst self, int argc, JSValueConst* argv) {
    PrSvgElement* element = jsThis<PrSvgElement>(ctx, self, prJsSvgGraphicsElementClass);
    if (element == nullptr)
        return JS_EXCEPTION;
    PrSvgBoundingBoxOptions options = prSvgBoundingBoxOptionsDefault();
    JSValueConst dictionary = argc > 0 ? argv[0] : JS_UNDEFINED;
    if (!JS_IsUndefined(dictionary) && !JS_IsNull(dictionary)) {
        if (!JS_IsObject(dictionary))
            return JS_ThrowTypeError(ctx, "Expected an SVGBoundingBoxOptions");
        if (!readMember(ctx, dictionary, "clipped", false, &options.clipped)
            || !readMember(ctx, dictionary, "fill", true, &options.fill)
            || !readMember(ctx, dictionary, "markers", false, &options.markers)
            || !readMember(ctx, dictionary, "stroke", false, &options.stroke))
            return JS_EXCEPTION;
    }
    return prJsDomRectNew(ctx, prSvgGraphicsElementGetBBox(element, &options));
}

// Implements getCTM and getScreenCTM, selected by the magic. A transform that cannot be found is null.
JSValue getCtm(JSContext* ctx, JSValueConst self, int, JSValueConst*, int magic) {
    PrSvgElement* element = jsThis<PrSvgElement>(ctx, self, prJsSvgGraphicsElementClass);
    if (element == nullptr)
        return JS_EXCEPTION;
    PrDomMatrix matrix = prDomMatrixIdentity();
    bool found = magic == 0 ? prSvgGraphicsElementGetCtm(element, &matrix)
                            : prSvgGraphicsElementGetScreenCtm(element, &matrix);
    return found ? prJsDomMatrixNew(ctx, matrix) : JS_NULL;
}

// ---------------------------------------------------------------------------------------------------------------------
// SVGGeometryElement

// Throws an InvalidStateError for an element whose geometry cannot be computed, as for one that does not render
// (SVG2/types.html#__svg__SVGGeometryElement__isPointInFill).
JSValue orInvalidState(JSContext* ctx, PrStatus status, JSValue value) {
    if (status == PrStatus_Ok)
        return value;
    JS_FreeValue(ctx, value);
    return jsDomExceptionThrow(ctx, "InvalidStateError");
}

// Implements isPointInFill and isPointInStroke, selected by the magic.
JSValue isPointIn(JSContext* ctx, JSValueConst self, int argc, JSValueConst* argv, int magic) {
    PrSvgElement* element = jsThis<PrSvgElement>(ctx, self, prJsSvgGeometryElementClass);
    PrDomPoint point{0, 0, 0, 1};
    if (element == nullptr || !prJsDomPointInitRead(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, &point))
        return JS_EXCEPTION;
    bool inside = false;
    PrStatus status = magic == 0 ? prSvgGeometryElementIsPointInFill(element, point, &inside)
                                 : prSvgGeometryElementIsPointInStroke(element, point, &inside);
    return orInvalidState(ctx, status, JS_NewBool(ctx, inside));
}

JSValue getTotalLength(JSContext* ctx, JSValueConst self, int, JSValueConst*) {
    PrSvgElement* element = jsThis<PrSvgElement>(ctx, self, prJsSvgGeometryElementClass);
    if (element == nullptr)
        return JS_EXCEPTION;
    float length = 0;
    PrStatus status = prSvgGeometryElementGetTotalLength(element, &length);
    return orInvalidState(ctx, status, JS_NewFloat64(ctx, double(length)));
}

JSValue getPointAtLength(JSContext* ctx, JSValueConst self, int argc, JSValueConst* argv) {
    PrSvgElement* element = jsThis<PrSvgElement>(ctx, self, prJsSvgGeometryElementClass);
    double distance = 0;
    if (element == nullptr || !jsArgCount(ctx, argc, 1) || JS_ToFloat64(ctx, &distance, argv[0]) < 0)
        return JS_EXCEPTION;
    if (!isFiniteF64(distance))
        return JS_ThrowTypeError(ctx, "The distance is not a finite number");
    PrDomPoint point{0, 0, 0, 1};
    PrStatus status = prSvgGeometryElementGetPointAtLength(element, float(distance), &point);
    if (status != PrStatus_Ok)
        return jsDomExceptionThrow(ctx, "InvalidStateError");
    return prJsDomPointNew(ctx, point);
}

// ---------------------------------------------------------------------------------------------------------------------
// SVGLength and SVGAnimatedLength

constexpr JsConstant kLengthConstants[] = {
    {"SVG_LENGTHTYPE_UNKNOWN", PrSvgLengthType_Unknown}, {"SVG_LENGTHTYPE_NUMBER", PrSvgLengthType_Number},
    {"SVG_LENGTHTYPE_PERCENTAGE", PrSvgLengthType_Percentage}, {"SVG_LENGTHTYPE_EMS", PrSvgLengthType_Ems},
    {"SVG_LENGTHTYPE_EXS", PrSvgLengthType_Exs}, {"SVG_LENGTHTYPE_PX", PrSvgLengthType_Px},
    {"SVG_LENGTHTYPE_CM", PrSvgLengthType_Cm}, {"SVG_LENGTHTYPE_MM", PrSvgLengthType_Mm},
    {"SVG_LENGTHTYPE_IN", PrSvgLengthType_In}, {"SVG_LENGTHTYPE_PT", PrSvgLengthType_Pt},
    {"SVG_LENGTHTYPE_PC", PrSvgLengthType_Pc},
};

// Throws the exception of a length's status: NoModificationAllowedError for an animated value, SyntaxError for a string
// that is not a length, and NotSupportedError for a unit type that is not one.
JSValue lengthResult(JSContext* ctx, PrStatus status) {
    switch (status) {
        case PrStatus_Ok: return JS_UNDEFINED;
        case PrStatus_ReadOnly: return jsDomExceptionThrow(ctx, "NoModificationAllowedError");
        case PrStatus_Syntax: return jsDomExceptionThrow(ctx, "SyntaxError");
        default: return jsDomExceptionThrow(ctx, "NotSupportedError");
    }
}

// Reads a float argument (webidl/#js-float), which must be finite.
bool floatOf(JSContext* ctx, JSValueConst value, float* out) {
    double number = 0;
    if (JS_ToFloat64(ctx, &number, value) < 0)
        return false;
    if (!isFiniteF64(number) || !isFiniteF32(float(number))) {
        JS_ThrowTypeError(ctx, "The value is not a finite float");
        return false;
    }
    *out = float(number);
    return true;
}

// Implements the setters of value and valueInSpecifiedUnits, selected by the magic.
JSValue setLengthNumber(JSContext* ctx, JSValueConst self, JSValueConst input, int magic) {
    PrSvgLength* length = jsThis<PrSvgLength>(ctx, self, prJsSvgLengthClass);
    float value = 0;
    if (length == nullptr || !floatOf(ctx, input, &value))
        return JS_EXCEPTION;
    return lengthResult(ctx, magic == 0 ? prSvgLengthSetValue(length, value)
                                        : prSvgLengthSetValueInSpecifiedUnits(length, value));
}

JSValue lengthValue(JSContext* ctx, JSValueConst self, int magic) {
    PrSvgLength* length = jsThis<PrSvgLength>(ctx, self, prJsSvgLengthClass);
    if (length == nullptr)
        return JS_EXCEPTION;
    return JS_NewFloat64(ctx, double(magic == 0 ? prSvgLengthValue(length) : prSvgLengthValueInSpecifiedUnits(length)));
}

JSValue setValueAsString(JSContext* ctx, JSValueConst self, JSValueConst input) {
    PrSvgLength* length = jsThis<PrSvgLength>(ctx, self, prJsSvgLengthClass);
    PrStringView text{};
    if (length == nullptr || !jsReadString(ctx, input, &text))
        return JS_EXCEPTION;
    return lengthResult(ctx, prSvgLengthSetValueAsString(length, text));
}

// Implements newValueSpecifiedUnits and convertToSpecifiedUnits, selected by the magic.
JSValue specifyUnits(JSContext* ctx, JSValueConst self, int argc, JSValueConst* argv, int magic) {
    PrSvgLength* length = jsThis<PrSvgLength>(ctx, self, prJsSvgLengthClass);
    uint32_t type = 0;
    float value = 0;
    if (length == nullptr || !indexOf(ctx, argc, argv, 0, &type)
        || (magic == 0 && (!jsArgCount(ctx, argc, 2) || !floatOf(ctx, argv[1], &value))))
        return JS_EXCEPTION;
    // An unsigned short wraps; a value past the constants is not a unit type.
    PrSvgLengthType unit = PrSvgLengthType(type & 0xffff);
    return lengthResult(ctx, magic == 0 ? prSvgLengthNewValueSpecifiedUnits(length, unit, value)
                                        : prSvgLengthConvertToSpecifiedUnits(length, unit));
}

// Implements baseVal and animVal, selected by the magic, each the same object every time.
JSValue animatedLengthValue(JSContext* ctx, JSValueConst self, int magic) {
    PrSvgAnimatedLength* animated = jsThis<PrSvgAnimatedLength>(ctx, self, prJsSvgAnimatedLengthClass);
    if (animated == nullptr)
        return JS_EXCEPTION;
    return jsSameObject(ctx, self, magic == 0 ? "baseVal" : "animVal", [&] {
        PrSvgLength* length =
            magic == 0 ? prSvgAnimatedLengthBaseVal(animated) : prSvgAnimatedLengthAnimVal(animated);
        JSValue object = jsWrap(ctx, prJsSvgLengthClass, self, length, true);
        if (JS_IsException(object))
            prSvgLengthDestroy(length);
        return object;
    });
}

// The names of the length attributes that the element interfaces reflect, which the magic of their getters indexes.
constexpr const char* kLengthNames[] = {
    "x", "y", "width", "height", "cx", "cy", "r", "rx", "ry", "x1", "y1", "x2", "y2", "fx", "fy", "fr", "refX", "refY",
    "markerWidth", "markerHeight",
};

// Implements the getter of the SVGAnimatedLength member that the magic names. The getter returns the same object every
// time.
JSValue animatedLength(JSContext* ctx, JSValueConst self, int magic) {
    PrSvgElement* element = jsThis<PrSvgElement>(ctx, self, prJsSvgElementClass);
    if (element == nullptr)
        return JS_EXCEPTION;
    const char* name = kLengthNames[magic];
    return jsSameObject(ctx, self, name, [&] {
        PrSvgAnimatedLength* animated = prSvgAnimatedLengthCreate(element, name);
        if (animated == nullptr)
            return JS_UNDEFINED;
        JSValue object = jsWrap(ctx, prJsSvgAnimatedLengthClass, self, animated, true);
        if (JS_IsException(object))
            prSvgAnimatedLengthDestroy(animated);
        return object;
    });
}

JSValue createSvgLength(JSContext* ctx, JSValueConst self, int, JSValueConst*) {
    if (jsThis<PrSvgElement>(ctx, self, prJsSvgSvgElementClass) == nullptr)
        return JS_EXCEPTION;
    PrSvgLength* length = prSvgLengthCreate();
    JSValue object = jsWrap(ctx, prJsSvgLengthClass, JS_NULL, length, true);
    if (JS_IsException(object))
        prSvgLengthDestroy(length);
    return object;
}

#define PR_SVG_LENGTH(name, index) PR_JS_MAGIC_GET(name, animatedLength, index)

// ---------------------------------------------------------------------------------------------------------------------
// SVGAnimatedString

// The names of the string attributes that the element interfaces reflect, which the magic of their getters indexes.
constexpr const char* kStringNames[] = {"href", "target"};

JSValue animatedString(JSContext* ctx, JSValueConst self, int magic) {
    PrSvgElement* element = jsThis<PrSvgElement>(ctx, self, prJsSvgElementClass);
    if (element == nullptr)
        return JS_EXCEPTION;
    const char* name = kStringNames[magic];
    return jsSameObject(ctx, self, name, [&] {
        PrSvgAnimatedString* animated = prSvgAnimatedStringCreate(element, name);
        if (animated == nullptr)
            return JS_UNDEFINED;
        JSValue object = jsWrap(ctx, prJsSvgAnimatedStringClass, self, animated, true);
        if (JS_IsException(object))
            prSvgAnimatedStringDestroy(animated);
        return object;
    });
}

JSValue animatedStringValue(JSContext* ctx, JSValueConst self) {
    PrSvgAnimatedString* animated = jsThis<PrSvgAnimatedString>(ctx, self, prJsSvgAnimatedStringClass);
    return animated != nullptr ? jsNewStringView(ctx, prSvgAnimatedStringBaseVal(animated)) : JS_EXCEPTION;
}

JSValue setAnimatedStringValue(JSContext* ctx, JSValueConst self, JSValueConst input) {
    PrSvgAnimatedString* animated = jsThis<PrSvgAnimatedString>(ctx, self, prJsSvgAnimatedStringClass);
    PrStringView text{};
    if (animated == nullptr || !jsReadString(ctx, input, &text))
        return JS_EXCEPTION;
    prSvgAnimatedStringSetBaseVal(animated, text);
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------------------------------------------------
// SVGTextContentElement

JSValue getNumberOfChars(JSContext* ctx, JSValueConst self, int, JSValueConst*) {
    PrSvgElement* element = textContentOf(ctx, self);
    return element != nullptr ? JS_NewInt64(ctx, prSvgTextContentElementGetNumberOfChars(element)) : JS_EXCEPTION;
}

JSValue getComputedTextLength(JSContext* ctx, JSValueConst self, int, JSValueConst*) {
    PrSvgElement* element = textContentOf(ctx, self);
    return element != nullptr ? JS_NewFloat64(ctx, double(prSvgTextContentElementGetComputedTextLength(element)))
                              : JS_EXCEPTION;
}

JSValue getSubStringLength(JSContext* ctx, JSValueConst self, int argc, JSValueConst* argv) {
    PrSvgElement* element = textContentOf(ctx, self);
    uint32_t first = 0;
    uint32_t count = 0;
    if (element == nullptr || !indexOf(ctx, argc, argv, 0, &first) || !indexOf(ctx, argc, argv, 1, &count))
        return JS_EXCEPTION;
    float length = 0;
    PrStatus status = prSvgTextContentElementGetSubStringLength(element, first, count, &length);
    return orIndexSize(ctx, status, JS_NewFloat64(ctx, double(length)));
}

// Implements getStartPositionOfChar and getEndPositionOfChar, selected by the magic.
JSValue getPositionOfChar(JSContext* ctx, JSValueConst self, int argc, JSValueConst* argv, int magic) {
    PrSvgElement* element = textContentOf(ctx, self);
    uint32_t index = 0;
    if (element == nullptr || !indexOf(ctx, argc, argv, 0, &index))
        return JS_EXCEPTION;
    PrDomPoint point{0, 0, 0, 1};
    PrStatus status = magic == 0 ? prSvgTextContentElementGetStartPositionOfChar(element, index, &point)
                                 : prSvgTextContentElementGetEndPositionOfChar(element, index, &point);
    if (status != PrStatus_Ok)
        return jsDomExceptionThrow(ctx, "IndexSizeError");
    return prJsDomPointNew(ctx, point);
}

JSValue getExtentOfChar(JSContext* ctx, JSValueConst self, int argc, JSValueConst* argv) {
    PrSvgElement* element = textContentOf(ctx, self);
    uint32_t index = 0;
    if (element == nullptr || !indexOf(ctx, argc, argv, 0, &index))
        return JS_EXCEPTION;
    PrDomRect rect{};
    if (prSvgTextContentElementGetExtentOfChar(element, index, &rect) != PrStatus_Ok)
        return jsDomExceptionThrow(ctx, "IndexSizeError");
    return prJsDomRectNew(ctx, rect);
}

JSValue getRotationOfChar(JSContext* ctx, JSValueConst self, int argc, JSValueConst* argv) {
    PrSvgElement* element = textContentOf(ctx, self);
    uint32_t index = 0;
    if (element == nullptr || !indexOf(ctx, argc, argv, 0, &index))
        return JS_EXCEPTION;
    float rotation = 0;
    PrStatus status = prSvgTextContentElementGetRotationOfChar(element, index, &rotation);
    return orIndexSize(ctx, status, JS_NewFloat64(ctx, double(rotation)));
}

JSValue getCharNumAtPosition(JSContext* ctx, JSValueConst self, int argc, JSValueConst* argv) {
    PrSvgElement* element = textContentOf(ctx, self);
    PrDomPoint point{};
    if (element == nullptr || !prJsDomPointInitRead(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, &point))
        return JS_EXCEPTION;
    return JS_NewInt32(ctx, prSvgTextContentElementGetCharNumAtPosition(element, point));
}

// ---------------------------------------------------------------------------------------------------------------------
// SVGSVGElement

// The factories of SVGSVGElement (SVG2/struct.html#__svg__SVGSVGElement__createSVGPoint): a new, detached point, matrix
// or rect, of the geometry interfaces that SVGPoint, SVGMatrix and SVGRect name.
JSValue createSvgPoint(JSContext* ctx, JSValueConst self, int, JSValueConst*) {
    if (jsThis<PrSvgElement>(ctx, self, prJsSvgSvgElementClass) == nullptr)
        return JS_EXCEPTION;
    return prJsDomPointNew(ctx, PrDomPoint{0, 0, 0, 1});
}

JSValue createSvgMatrix(JSContext* ctx, JSValueConst self, int, JSValueConst*) {
    if (jsThis<PrSvgElement>(ctx, self, prJsSvgSvgElementClass) == nullptr)
        return JS_EXCEPTION;
    return prJsDomMatrixNew(ctx, prDomMatrixIdentity());
}

JSValue createSvgRect(JSContext* ctx, JSValueConst self, int, JSValueConst*) {
    if (jsThis<PrSvgElement>(ctx, self, prJsSvgSvgElementClass) == nullptr)
        return JS_EXCEPTION;
    return prJsDomRectNew(ctx, PrDomRect{});
}

// ---------------------------------------------------------------------------------------------------------------------
// The prototypes

constexpr JSCFunctionListEntry kElementProto[] = {
    PR_JS_TAG("SVGElement"),
    PR_JS_GET("ownerSVGElement",
              (jsObjectGetter<&prJsSvgElementClass, prSvgElementOwnerSvgElement, &prJsDomElementClass>)),
    PR_JS_CSS_INLINE_STYLE,
};

constexpr JSCFunctionListEntry kGraphicsElementProto[] = {
    PR_JS_TAG("SVGGraphicsElement"),
    PR_JS_METHOD("getBBox", 0, getBBox),
    PR_JS_MAGIC("getCTM", 0, getCtm, 0),
    PR_JS_MAGIC("getScreenCTM", 0, getCtm, 1),
};

constexpr JSCFunctionListEntry kGeometryElementProto[] = {
    PR_JS_TAG("SVGGeometryElement"),
    PR_JS_MAGIC("isPointInFill", 0, isPointIn, 0),
    PR_JS_MAGIC("isPointInStroke", 0, isPointIn, 1),
    PR_JS_METHOD("getTotalLength", 0, getTotalLength),
    PR_JS_METHOD("getPointAtLength", 1, getPointAtLength),
};

constexpr JSCFunctionListEntry kRectElementProto[] = {
    PR_JS_TAG("SVGRectElement"),
    PR_SVG_LENGTH("x", 0), PR_SVG_LENGTH("y", 1), PR_SVG_LENGTH("width", 2), PR_SVG_LENGTH("height", 3),
    PR_SVG_LENGTH("rx", 7), PR_SVG_LENGTH("ry", 8),
};

constexpr JSCFunctionListEntry kCircleElementProto[] = {
    PR_JS_TAG("SVGCircleElement"),
    PR_SVG_LENGTH("cx", 4), PR_SVG_LENGTH("cy", 5), PR_SVG_LENGTH("r", 6),
};

constexpr JSCFunctionListEntry kEllipseElementProto[] = {
    PR_JS_TAG("SVGEllipseElement"),
    PR_SVG_LENGTH("cx", 4), PR_SVG_LENGTH("cy", 5), PR_SVG_LENGTH("rx", 7), PR_SVG_LENGTH("ry", 8),
};

constexpr JSCFunctionListEntry kLineElementProto[] = {
    PR_JS_TAG("SVGLineElement"),
    PR_SVG_LENGTH("x1", 9), PR_SVG_LENGTH("y1", 10), PR_SVG_LENGTH("x2", 11), PR_SVG_LENGTH("y2", 12),
};

constexpr JSCFunctionListEntry kPathElementProto[] = {
    PR_JS_TAG("SVGPathElement"),
};

constexpr JSCFunctionListEntry kPolylineElementProto[] = {
    PR_JS_TAG("SVGPolylineElement"),
};

constexpr JSCFunctionListEntry kPolygonElementProto[] = {
    PR_JS_TAG("SVGPolygonElement"),
};

constexpr JSCFunctionListEntry kSvgElementProto[] = {
    PR_JS_TAG("SVGSVGElement"),
    PR_SVG_LENGTH("x", 0), PR_SVG_LENGTH("y", 1), PR_SVG_LENGTH("width", 2), PR_SVG_LENGTH("height", 3),
    PR_JS_METHOD("createSVGPoint", 0, createSvgPoint),
    PR_JS_METHOD("createSVGMatrix", 0, createSvgMatrix),
    PR_JS_METHOD("createSVGRect", 0, createSvgRect),
    PR_JS_METHOD("createSVGLength", 0, createSvgLength),
};

constexpr JSCFunctionListEntry kTextContentElementProto[] = {
    PR_JS_TAG("SVGTextContentElement"),
    PR_JS_METHOD("getNumberOfChars", 0, getNumberOfChars),
    PR_JS_METHOD("getComputedTextLength", 0, getComputedTextLength),
    PR_JS_METHOD("getSubStringLength", 2, getSubStringLength),
    PR_JS_MAGIC("getStartPositionOfChar", 1, getPositionOfChar, 0),
    PR_JS_MAGIC("getEndPositionOfChar", 1, getPositionOfChar, 1),
    PR_JS_METHOD("getExtentOfChar", 1, getExtentOfChar),
    PR_JS_METHOD("getRotationOfChar", 1, getRotationOfChar),
    PR_JS_METHOD("getCharNumAtPosition", 0, getCharNumAtPosition),
};

constexpr JSCFunctionListEntry kAElementProto[] = {
    PR_JS_TAG("SVGAElement"),
    PR_JS_MAGIC_GET("href", animatedString, 0),
    PR_JS_MAGIC_GET("target", animatedString, 1),
};

constexpr JSCFunctionListEntry kUseElementProto[] = {
    PR_JS_TAG("SVGUseElement"),
    PR_JS_MAGIC_GET("href", animatedString, 0),
    PR_SVG_LENGTH("x", 0), PR_SVG_LENGTH("y", 1), PR_SVG_LENGTH("width", 2), PR_SVG_LENGTH("height", 3),
};

constexpr JSCFunctionListEntry kSwitchElementProto[] = {
    PR_JS_TAG("SVGSwitchElement"),
};

constexpr JSCFunctionListEntry kSymbolElementProto[] = {
    PR_JS_TAG("SVGSymbolElement"),
};

constexpr JSCFunctionListEntry kScriptElementProto[] = {
    PR_JS_TAG("SVGScriptElement"),
    PR_JS_MAGIC_GET("href", animatedString, 0),
};

constexpr JSCFunctionListEntry kViewElementProto[] = {
    PR_JS_TAG("SVGViewElement"),
};

constexpr JSCFunctionListEntry kImageElementProto[] = {
    PR_JS_TAG("SVGImageElement"),
    PR_JS_MAGIC_GET("href", animatedString, 0),
    PR_SVG_LENGTH("x", 0), PR_SVG_LENGTH("y", 1), PR_SVG_LENGTH("width", 2), PR_SVG_LENGTH("height", 3),
};

constexpr JSCFunctionListEntry kForeignObjectElementProto[] = {
    PR_JS_TAG("SVGForeignObjectElement"),
    PR_SVG_LENGTH("x", 0), PR_SVG_LENGTH("y", 1), PR_SVG_LENGTH("width", 2), PR_SVG_LENGTH("height", 3),
};

constexpr JSCFunctionListEntry kAnimationElementProto[] = {
    PR_JS_TAG("SVGAnimationElement"),
};

constexpr JSCFunctionListEntry kAnimateElementProto[] = {
    PR_JS_TAG("SVGAnimateElement"),
};

constexpr JSCFunctionListEntry kSetElementProto[] = {
    PR_JS_TAG("SVGSetElement"),
};

constexpr JSCFunctionListEntry kAnimateMotionElementProto[] = {
    PR_JS_TAG("SVGAnimateMotionElement"),
};

constexpr JSCFunctionListEntry kAnimateTransformElementProto[] = {
    PR_JS_TAG("SVGAnimateTransformElement"),
};

constexpr JSCFunctionListEntry kLengthProto[] = {
    PR_JS_TAG("SVGLength"),
    PR_JS_GET("unitType", (jsNumberGetter<&prJsSvgLengthClass, prSvgLengthUnitType>)),
    PR_JS_MAGIC_GET_SET("value", lengthValue, setLengthNumber, 0),
    PR_JS_MAGIC_GET_SET("valueInSpecifiedUnits", lengthValue, setLengthNumber, 1),
    PR_JS_GET_SET("valueAsString", (jsStringGetter<&prJsSvgLengthClass, prSvgLengthValueAsString>), setValueAsString),
    PR_JS_MAGIC("newValueSpecifiedUnits", 2, specifyUnits, 0),
    PR_JS_MAGIC("convertToSpecifiedUnits", 1, specifyUnits, 1),
};

constexpr JSCFunctionListEntry kAnimatedLengthProto[] = {
    PR_JS_TAG("SVGAnimatedLength"),
    PR_JS_MAGIC_GET("baseVal", animatedLengthValue, 0),
    PR_JS_MAGIC_GET("animVal", animatedLengthValue, 1),
};

constexpr JSCFunctionListEntry kAnimatedStringProto[] = {
    PR_JS_TAG("SVGAnimatedString"),
    PR_JS_GET_SET("baseVal", animatedStringValue, setAnimatedStringValue),
    PR_JS_GET("animVal", animatedStringValue),
};

// The definition of an element interface, which holds no handle of its own.
#define PR_SVG_ELEMENT(name, id, proto, baseId)                                                                        \
    {PR_JS_CLASS(name, id, jsViewFinalizer<&id>, proto), .base = &baseId}

constexpr JsClassDefinition kClasses[] = {
    PR_SVG_ELEMENT("SVGElement", prJsSvgElementClass, kElementProto, prJsDomElementClass),
    PR_SVG_ELEMENT("SVGGraphicsElement", prJsSvgGraphicsElementClass, kGraphicsElementProto, prJsSvgElementClass),
    PR_SVG_ELEMENT("SVGGeometryElement", prJsSvgGeometryElementClass, kGeometryElementProto,
                   prJsSvgGraphicsElementClass),
    PR_SVG_ELEMENT("SVGRectElement", svgRectElementClass, kRectElementProto, prJsSvgGeometryElementClass),
    PR_SVG_ELEMENT("SVGCircleElement", svgCircleElementClass, kCircleElementProto, prJsSvgGeometryElementClass),
    PR_SVG_ELEMENT("SVGEllipseElement", svgEllipseElementClass, kEllipseElementProto, prJsSvgGeometryElementClass),
    PR_SVG_ELEMENT("SVGLineElement", svgLineElementClass, kLineElementProto, prJsSvgGeometryElementClass),
    PR_SVG_ELEMENT("SVGPathElement", svgPathElementClass, kPathElementProto, prJsSvgGeometryElementClass),
    PR_SVG_ELEMENT("SVGPolylineElement", svgPolylineElementClass, kPolylineElementProto, prJsSvgGeometryElementClass),
    PR_SVG_ELEMENT("SVGPolygonElement", svgPolygonElementClass, kPolygonElementProto, prJsSvgGeometryElementClass),
    PR_SVG_ELEMENT("SVGSVGElement", prJsSvgSvgElementClass, kSvgElementProto, prJsSvgGraphicsElementClass),
    PR_SVG_ELEMENT("SVGTextContentElement", prJsSvgTextContentElementClass, kTextContentElementProto,
                   prJsSvgGraphicsElementClass),
    PR_SVG_ELEMENT("SVGAElement", prJsSvgAElementClass, kAElementProto, prJsSvgGraphicsElementClass),
    PR_SVG_ELEMENT("SVGUseElement", prJsSvgUseElementClass, kUseElementProto, prJsSvgGraphicsElementClass),
    PR_SVG_ELEMENT("SVGSwitchElement", prJsSvgSwitchElementClass, kSwitchElementProto, prJsSvgGraphicsElementClass),
    PR_SVG_ELEMENT("SVGSymbolElement", prJsSvgSymbolElementClass, kSymbolElementProto, prJsSvgGraphicsElementClass),
    PR_SVG_ELEMENT("SVGScriptElement", prJsSvgScriptElementClass, kScriptElementProto, prJsSvgElementClass),
    PR_SVG_ELEMENT("SVGViewElement", prJsSvgViewElementClass, kViewElementProto, prJsSvgElementClass),
    PR_SVG_ELEMENT("SVGImageElement", prJsSvgImageElementClass, kImageElementProto, prJsSvgGraphicsElementClass),
    PR_SVG_ELEMENT("SVGForeignObjectElement", prJsSvgForeignObjectElementClass, kForeignObjectElementProto,
                   prJsSvgGraphicsElementClass),
    PR_SVG_ELEMENT("SVGAnimationElement", prJsSvgAnimationElementClass, kAnimationElementProto, prJsSvgElementClass),
    PR_SVG_ELEMENT("SVGAnimateElement", prJsSvgAnimateElementClass, kAnimateElementProto, prJsSvgAnimationElementClass),
    PR_SVG_ELEMENT("SVGSetElement", prJsSvgSetElementClass, kSetElementProto, prJsSvgAnimationElementClass),
    PR_SVG_ELEMENT("SVGAnimateMotionElement", prJsSvgAnimateMotionElementClass, kAnimateMotionElementProto,
                   prJsSvgAnimationElementClass),
    PR_SVG_ELEMENT("SVGAnimateTransformElement", prJsSvgAnimateTransformElementClass, kAnimateTransformElementProto,
                   prJsSvgAnimationElementClass),
    {PR_JS_CLASS("SVGLength", prJsSvgLengthClass, (jsFinalizer<&prJsSvgLengthClass, prSvgLengthDestroy>), kLengthProto),
     PR_JS_CONSTANTS(kLengthConstants)},
    {PR_JS_CLASS("SVGAnimatedLength", prJsSvgAnimatedLengthClass,
                 (jsFinalizer<&prJsSvgAnimatedLengthClass, prSvgAnimatedLengthDestroy>), kAnimatedLengthProto)},
    {PR_JS_CLASS("SVGAnimatedString", prJsSvgAnimatedStringClass,
                 (jsFinalizer<&prJsSvgAnimatedStringClass, prSvgAnimatedStringDestroy>), kAnimatedStringProto)},
};

} // namespace

} // namespace pr_impl

using namespace pr_impl;

// The classes that other modules' bindings refer to.
JSClassID prJsSvgElementClass;
JSClassID prJsSvgGraphicsElementClass;
JSClassID prJsSvgGeometryElementClass;
JSClassID prJsSvgSvgElementClass;
JSClassID prJsSvgTextContentElementClass;
JSClassID prJsSvgAElementClass;
JSClassID prJsSvgUseElementClass;
JSClassID prJsSvgSwitchElementClass;
JSClassID prJsSvgSymbolElementClass;
JSClassID prJsSvgScriptElementClass;
JSClassID prJsSvgViewElementClass;
JSClassID prJsSvgImageElementClass;
JSClassID prJsSvgForeignObjectElementClass;
JSClassID prJsSvgAnimationElementClass;
JSClassID prJsSvgAnimateElementClass;
JSClassID prJsSvgSetElementClass;
JSClassID prJsSvgAnimateMotionElementClass;
JSClassID prJsSvgAnimateTransformElementClass;
JSClassID prJsSvgLengthClass;
JSClassID prJsSvgAnimatedLengthClass;
JSClassID prJsSvgAnimatedStringClass;

void prJsSvgBind(JSContext* ctx) {
    for (const JsClassDefinition& definition : kClasses)
        JS_FreeValue(ctx, jsDefineClass(ctx, definition));
    jsAddClassifier(JS_GetRuntime(ctx), prJsDomElementClass, classifyElement);
}
