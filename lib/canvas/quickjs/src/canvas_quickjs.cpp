// JavaScript binding of the 2D rendering context of a canvas.
//
// html - https://html.spec.whatwg.org/

#include "pr/canvas_quickjs.h"

#include "pr/count_of.h"
#include "pr/dom_geometry_quickjs.h"
#include "pr/dom_quickjs.h"
#include "pr/float.h"
#include "pr/html.h"
#include "pr/mem.h"
#include "pr/quickjs_binding.h"
#include "pr/string_ops.h"
#include "pr/webidl_quickjs.h"

namespace pr_impl {

namespace {

JSClassID contextClass;
JSClassID gradientClass;
JSClassID patternClass;
JSClassID path2DClass;
JSClassID imageDataClass;
JSClassID textMetricsClass;

// The slot of a context's object that holds its canvas's object (jsSetReference), and the slot of an ImageData's object
// that holds its data.
constexpr uint32_t kCanvasSlot = 0;
constexpr uint32_t kDataSlot = 0;

// ---------------------------------------------------------------------------------------------------------------------
// Conversions

// Reads an IDL enumeration's value (webidl/#js-enumeration) as its index in `names`. Writes -1 for a string that names
// no value. Returns false if converting the value to a string threw.
bool readEnum(JSContext* ctx, JSValueConst value, const char* const* names, uint32_t count, int32_t* out) {
    StringArgument text = {ctx};
    if (!text.read(value))
        return false;
    *out = -1;
    for (uint32_t i = 0; i < count; ++i) {
        if (text.value() == names[i])
            *out = int32_t(i);
    }
    return true;
}

// Reads an enumeration's value as readEnum does, and throws a TypeError for a string that names no value, as a
// dictionary member or an argument of an enumeration type does.
bool requireEnum(JSContext* ctx, JSValueConst value, const char* const* names, uint32_t count, int32_t* out) {
    if (!readEnum(ctx, value, names, count, out))
        return false;
    if (*out < 0) {
        JS_ThrowTypeError(ctx, "The value is not one of the enumeration's values");
        return false;
    }
    return true;
}

// Reads `count` unrestricted doubles from the arguments, each of which must be given (webidl/#js-unrestricted-double).
bool readNumbers(JSContext* ctx, int argc, JSValueConst* argv, int first, int count, double* out) {
    if (!jsArgCount(ctx, argc, first + count))
        return false;
    for (int i = 0; i < count; ++i) {
        if (JS_ToFloat64(ctx, &out[i], argv[first + i]) < 0)
            return false;
    }
    return true;
}

// Reads `count` doubles, which must be finite (webidl/#js-double), from the arguments.
bool readFinite(JSContext* ctx, int argc, JSValueConst* argv, int count, double* out) {
    if (!readNumbers(ctx, argc, argv, 0, count, out))
        return false;
    for (int i = 0; i < count; ++i) {
        if (!isFiniteF64(out[i])) {
            JS_ThrowTypeError(ctx, "The value is not a finite number");
            return false;
        }
    }
    return true;
}

// Converts a value to a long with [EnforceRange] (webidl/#EnforceRange): a finite number, truncated, in range.
bool enforceLong(JSContext* ctx, JSValueConst value, int32_t* out) {
    double number = 0;
    if (JS_ToFloat64(ctx, &number, value) < 0)
        return false;
    if (!isFiniteF64(number)) {
        JS_ThrowTypeError(ctx, "The value is not a finite number");
        return false;
    }
    number = number < 0 ? ceilF64(number) : floorF64(number);
    if (number < -2147483648.0 || number > 2147483647.0) {
        JS_ThrowTypeError(ctx, "The value is out of range");
        return false;
    }
    *out = int32_t(number);
    return true;
}

bool enforceLongs(JSContext* ctx, int argc, JSValueConst* argv, int first, int count, int32_t* out) {
    if (!jsArgCount(ctx, argc, first + count))
        return false;
    for (int i = 0; i < count; ++i) {
        if (!enforceLong(ctx, argv[first + i], &out[i]))
            return false;
    }
    return true;
}

PrDAffine2 affineOf(const PrDomMatrix& m) {
    return PrDAffine2{m.m[0], m.m[1], m.m[4], m.m[5], m.m[12], m.m[13]};
}

// The names of the IDL enumerations that the binding converts, in the order of the C enumerations they map to.
constexpr const char* kFillRules[] = {"nonzero", "evenodd"};
constexpr const char* kLineCaps[] = {"butt", "round", "square"};
constexpr const char* kLineJoins[] = {"round", "bevel", "miter"};
constexpr const char* kSmoothingQualities[] = {"low", "medium", "high"};
constexpr const char* kColorTypes[] = {"unorm8", "float16"};
constexpr const char* kPixelFormats[] = {"rgba-unorm8", "rgba-float16"};
constexpr const char* kRepetitions[] = {"repeat", "repeat-x", "repeat-y", "no-repeat"};
constexpr const char* kTextAligns[] = {"start", "end", "left", "right", "center"};
constexpr const char* kTextBaselines[] = {"top", "hanging", "middle", "alphabetic", "ideographic", "bottom"};
constexpr const char* kDirections[] = {"ltr", "rtl", "inherit"};
constexpr const char* kFontKernings[] = {"auto", "normal", "none"};
constexpr const char* kFontStretches[] = {
    "ultra-condensed", "extra-condensed", "condensed", "semi-condensed",
    "normal", "semi-expanded", "expanded", "extra-expanded", "ultra-expanded",
};
constexpr const char* kFontVariantCaps[] = {
    "normal", "small-caps", "all-small-caps", "petite-caps",
    "all-petite-caps", "unicase", "titling-caps",
};
constexpr const char* kTextRenderings[] = {"auto", "optimizeSpeed", "optimizeLegibility", "geometricPrecision"};

// The PredefinedColorSpace enumeration (html/#predefinedcolorspace).
constexpr const char* kColorSpaceNames[] = {"srgb", "display-p3"};
constexpr PrColorSpace kColorSpaces[] = {PrColorSpace_Srgb, PrColorSpace_DisplayP3};

const char* colorSpaceName(PrColorSpace space) {
    return space == PrColorSpace_DisplayP3 ? "display-p3" : "srgb";
}

// Reads an optional CanvasFillRule argument, nonzero if it is not given.
bool readFillRule(JSContext* ctx, int argc, JSValueConst* argv, int index, PrPathFillRule* out) {
    *out = PrPathFillRule_NonZero;
    if (!jsArgGiven(argc, argv, index))
        return true;
    int32_t rule = 0;
    if (!requireEnum(ctx, argv[index], kFillRules, countOf(kFillRules), &rule))
        return false;
    *out = PrPathFillRule(rule);
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// ImageData (html/#imagedata)

// The description of an ImageData's pixels. Its object holds the array of the pixels in kDataSlot.
struct ImageDataValue {
    PrCanvasImageData data;
};

// The ImageDataSettings dictionary (html/#imagedatasettings). `hasColorSpace` is set if it names a color space.
struct ImageDataSettings {
    PrColorSpace colorSpace;
    PrCanvasPixelFormat format;
    bool hasColorSpace;
};

bool readImageDataSettings(JSContext* ctx, JSValueConst value, ImageDataSettings* out) {
    *out = ImageDataSettings{.colorSpace = PrColorSpace_Srgb, .format = PrCanvasPixelFormat_RgbaUnorm8};
    bool present = false;
    if (!jsDictionary(ctx, value, &present))
        return false;
    if (!present)
        return true;
    constexpr const char* kMembers[] = {"colorSpace", "pixelFormat"};
    for (const char* name : kMembers) {
        JSValue member = JS_GetPropertyStr(ctx, value, name);
        if (JS_IsException(member))
            return false;
        bool ok = true;
        if (!JS_IsUndefined(member)) {
            bool space = name[0] == 'c';
            int32_t index = 0;
            if (space)
                ok = requireEnum(ctx, member, kColorSpaceNames, countOf(kColorSpaceNames), &index);
            else
                ok = requireEnum(ctx, member, kPixelFormats, countOf(kPixelFormats), &index);
            if (ok && space) {
                out->colorSpace = kColorSpaces[index];
                out->hasColorSpace = true;
            } else if (ok) {
                out->format = PrCanvasPixelFormat(index);
            }
        }
        JS_FreeValue(ctx, member);
        if (!ok)
            return false;
    }
    return true;
}

// Returns a new ImageData of `extent` pixels in `settings`' format and color space, or `space` if the settings name
// none (html/#initialize-an-imagedata-object). Its pixels are `array`, or new transparent black pixels if `array` is
// undefined. Takes ownership of `array`.
JSValue newImageData(JSContext* ctx, PrUVec2 extent, const ImageDataSettings& settings, PrColorSpace space, JSValue array) {
    if (JS_IsUndefined(array)) {
        uint64_t elements = uint64_t(extent.x) * extent.y * 4;
        if (elements > 0x7fffffffu) {
            JS_ThrowRangeError(ctx, "The image data is too large");
            return JS_EXCEPTION;
        }
        JSValue length = JS_NewFloat64(ctx, double(elements));
        JSTypedArrayEnum type =
            settings.format == PrCanvasPixelFormat_RgbaUnorm8 ? JS_TYPED_ARRAY_UINT8C : JS_TYPED_ARRAY_FLOAT16;
        array = JS_NewTypedArray(ctx, 1, &length, type);
        if (JS_IsException(array))
            return array;
    }
    ImageDataValue value{
        .data = PrCanvasImageData{
            .extent = extent,
            .format = settings.format,
            .colorSpace = settings.hasColorSpace ? settings.colorSpace : space,
        },
    };
    JSValue object = jsNewValue(ctx, imageDataClass, value);
    if (JS_IsException(object)) {
        JS_FreeValue(ctx, array);
        return object;
    }
    jsSetReference(ctx, object, kDataSlot, array);
    JS_FreeValue(ctx, array);
    return object;
}

// Returns the pixels of an ImageData's array, or null with an InvalidStateError thrown if its buffer is detached.
void* PR_NULLABLE imageDataPixels(JSContext* ctx, JSValueConst object) {
    JSValue array = jsReference(ctx, object, kDataSlot);
    size_t offset = 0;
    size_t length = 0;
    size_t elementSize = 0;
    JSValue buffer = JS_GetTypedArrayBuffer(ctx, array, &offset, &length, &elementSize);
    JS_FreeValue(ctx, array);
    if (JS_IsException(buffer))
        return nullptr;
    size_t size = 0;
    uint8_t* bytes = JS_GetArrayBuffer(ctx, &size, buffer);
    JS_FreeValue(ctx, buffer);
    if (bytes == nullptr) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        jsDomExceptionThrow(ctx, "InvalidStateError");
        return nullptr;
    }
    return bytes + offset;
}

ImageDataValue* PR_NULLABLE imageDataOf(JSContext* ctx, JSValueConst value) {
    return jsValueOf<ImageDataValue>(ctx, value, imageDataClass, "ImageData");
}

// new ImageData(sw, sh, settings) and new ImageData(data, sw, sh, settings) (html/#dom-imagedata).
JSValue imageDataConstruct(JSContext* ctx, JSValueConst newTarget, int argc, JSValueConst* argv) {
    if (!jsArgCount(ctx, argc, 2))
        return JS_EXCEPTION;
    int type = JS_GetTypedArrayType(argv[0]);
    if (type == JS_TYPED_ARRAY_UINT8C || type == JS_TYPED_ARRAY_FLOAT16) {
        uint32_t width = 0;
        uint32_t height = 0;
        ImageDataSettings settings{};
        bool heightGiven = jsArgGiven(argc, argv, 2);
        if (JS_ToUint32(ctx, &width, argv[1]) < 0 || (heightGiven && JS_ToUint32(ctx, &height, argv[2]) < 0) ||
                !readImageDataSettings(ctx, argc > 3 ? argv[3] : JS_UNDEFINED, &settings))
            return JS_EXCEPTION;
        size_t offset = 0;
        size_t length = 0;
        size_t elementSize = 0;
        JSValue buffer = JS_GetTypedArrayBuffer(ctx, argv[0], &offset, &length, &elementSize);
        if (JS_IsException(buffer))
            return buffer;
        JS_FreeValue(ctx, buffer);
        uint32_t pixelSize = prCanvasPixelFormatSize(settings.format);
        if (length == 0 || length % pixelSize != 0)
            return jsDomExceptionThrow(ctx, "InvalidStateError");
        size_t pixels = length / pixelSize;
        if (width == 0 || pixels % width != 0 || (heightGiven && pixels / width != height))
            return jsDomExceptionThrow(ctx, "IndexSizeError");
        bool bytes = type == JS_TYPED_ARRAY_UINT8C;
        if (bytes != (settings.format == PrCanvasPixelFormat_RgbaUnorm8))
            return jsDomExceptionThrow(ctx, "InvalidStateError");
        JSValue object = newImageData(ctx, PrUVec2{width, uint32_t(pixels / width)}, settings, PrColorSpace_Srgb,
            JS_DupValue(ctx, argv[0]));
        return jsConstructed(ctx, newTarget, object);
    }
    uint32_t width = 0;
    uint32_t height = 0;
    ImageDataSettings settings{};
    if (JS_ToUint32(ctx, &width, argv[0]) < 0 || JS_ToUint32(ctx, &height, argv[1]) < 0 ||
            !readImageDataSettings(ctx, argc > 2 ? argv[2] : JS_UNDEFINED, &settings))
        return JS_EXCEPTION;
    if (width == 0 || height == 0)
        return jsDomExceptionThrow(ctx, "IndexSizeError");
    return jsConstructed(ctx, newTarget,
        newImageData(ctx, PrUVec2{width, height}, settings, PrColorSpace_Srgb, JS_UNDEFINED));
}

// Implements width and height, selected by the magic.
JSValue imageDataExtent(JSContext* ctx, JSValueConst imageData, int magic) {
    ImageDataValue* value = imageDataOf(ctx, imageData);
    if (value == nullptr)
        return JS_EXCEPTION;
    return JS_NewUint32(ctx, magic == 0 ? value->data.extent.x : value->data.extent.y);
}

JSValue imageDataData(JSContext* ctx, JSValueConst imageData) {
    return imageDataOf(ctx, imageData) != nullptr ? jsReference(ctx, imageData, kDataSlot) : JS_EXCEPTION;
}

JSValue imageDataPixelFormat(JSContext* ctx, JSValueConst imageData) {
    ImageDataValue* value = imageDataOf(ctx, imageData);
    return value != nullptr ? JS_NewString(ctx, kPixelFormats[value->data.format]) : JS_EXCEPTION;
}

JSValue imageDataColorSpace(JSContext* ctx, JSValueConst imageData) {
    ImageDataValue* value = imageDataOf(ctx, imageData);
    return value != nullptr ? JS_NewString(ctx, colorSpaceName(value->data.colorSpace)) : JS_EXCEPTION;
}

// ---------------------------------------------------------------------------------------------------------------------
// Path2D and CanvasPath (html/#path2d, html/#canvaspath)

PrCanvasPath2D* PR_NULLABLE pathOf(JSContext* ctx, JSValueConst value) {
    return jsHandleAs<PrCanvasPath2D>(ctx, value, path2DClass);
}

// new Path2D(path) (html/#dom-path2d): a copy of a Path2D, the path that SVG path data describes, or an empty path.
JSValue path2DConstruct(JSContext* ctx, JSValueConst newTarget, int argc, JSValueConst* argv) {
    PrCanvasPath2D* path = nullptr;
    if (!jsArgGiven(argc, argv, 0)) {
        path = prCanvasPath2DCreate();
    } else if (PrCanvasPath2D* other = pathOf(ctx, argv[0])) {
        path = prCanvasPath2DCreateCopy(other);
    } else {
        StringArgument data = {ctx};
        if (!data.read(argv[0]))
            return JS_EXCEPTION;
        path = prCanvasPath2DCreateFromPathData(data.value());
    }
    JSValue object = jsWrap(ctx, path2DClass, JS_NULL, path, true);
    if (JS_IsException(object)) {
        prCanvasPath2DDestroy(path);
        return object;
    }
    return jsConstructed(ctx, newTarget, object);
}

JSValue addPath(JSContext* ctx, JSValueConst pathObject, int argc, JSValueConst* argv) {
    PrCanvasPath2D* path = jsThis<PrCanvasPath2D>(ctx, pathObject, path2DClass);
    if (path == nullptr || !jsArgCount(ctx, argc, 1))
        return JS_EXCEPTION;
    PrCanvasPath2D* other = pathOf(ctx, argv[0]);
    if (other == nullptr)
        return JS_ThrowTypeError(ctx, "Expected a Path2D");
    PrDomMatrix matrix{};
    if (!prJsDomMatrix2DInitRead(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, &matrix))
        return JS_EXCEPTION;
    prCanvasPath2DAddPath(path, other, affineOf(matrix));
    return JS_UNDEFINED;
}

// The methods of CanvasPath on either of the objects that have a path (html/#canvaspath), and the class of the object.
template <typename Handle>
struct PathMethods {
    const JSClassID* id;
    void (*closePath)(Handle*);
    void (*moveTo)(Handle*, double, double);
    void (*lineTo)(Handle*, double, double);
    void (*quadraticCurveTo)(Handle*, double, double, double, double);
    void (*bezierCurveTo)(Handle*, double, double, double, double, double, double);
    PrStatus (*arcTo)(Handle*, double, double, double, double, double);
    void (*rect)(Handle*, double, double, double, double);
    PrStatus (*roundRect)(Handle*, double, double, double, double, const PrDVec2* PR_NULLABLE, uint32_t);
    PrStatus (*arc)(Handle*, double, double, double, double, double, bool);
    PrStatus (*ellipse)(Handle*, double, double, double, double, double, double, double, bool);
};

constexpr PathMethods<PrCanvasPath2D> kPath2DMethods = {
    &path2DClass, prCanvasPath2DClosePath, prCanvasPath2DMoveTo, prCanvasPath2DLineTo,
    prCanvasPath2DQuadraticCurveTo, prCanvasPath2DBezierCurveTo, prCanvasPath2DArcTo, prCanvasPath2DRect,
    prCanvasPath2DRoundRect, prCanvasPath2DArc, prCanvasPath2DEllipse,
};

constexpr PathMethods<PrCanvas> kContextMethods = {
    &contextClass, prCanvasClosePath, prCanvasMoveTo,
    prCanvasLineTo, prCanvasQuadraticCurveTo,
    prCanvasBezierCurveTo, prCanvasArcTo, prCanvasRect,
    prCanvasRoundRect, prCanvasArc, prCanvasEllipse,
};

// Throws the exception of a path method's status: an IndexSizeError for a negative radius.
JSValue pathResult(JSContext* ctx, PrStatus status) {
    return status == PrStatus_Ok ? JS_UNDEFINED : jsDomExceptionThrow(ctx, "IndexSizeError");
}

template <typename Handle, const PathMethods<Handle>* Methods>
JSValue closePath(JSContext* ctx, JSValueConst targetObject, int, JSValueConst*) {
    Handle* target = jsThis<Handle>(ctx, targetObject, *Methods->id);
    if (target == nullptr)
        return JS_EXCEPTION;
    Methods->closePath(target);
    return JS_UNDEFINED;
}

// Implements moveTo and lineTo, selected by the magic.
template <typename Handle, const PathMethods<Handle>* Methods>
JSValue pointTo(JSContext* ctx, JSValueConst targetObject, int argc, JSValueConst* argv, int magic) {
    Handle* target = jsThis<Handle>(ctx, targetObject, *Methods->id);
    double n[2];
    if (target == nullptr || !readNumbers(ctx, argc, argv, 0, 2, n))
        return JS_EXCEPTION;
    (magic == 0 ? Methods->moveTo : Methods->lineTo)(target, n[0], n[1]);
    return JS_UNDEFINED;
}

template <typename Handle, const PathMethods<Handle>* Methods>
JSValue quadraticCurveTo(JSContext* ctx, JSValueConst targetObject, int argc, JSValueConst* argv) {
    Handle* target = jsThis<Handle>(ctx, targetObject, *Methods->id);
    double n[4];
    if (target == nullptr || !readNumbers(ctx, argc, argv, 0, 4, n))
        return JS_EXCEPTION;
    Methods->quadraticCurveTo(target, n[0], n[1], n[2], n[3]);
    return JS_UNDEFINED;
}

template <typename Handle, const PathMethods<Handle>* Methods>
JSValue bezierCurveTo(JSContext* ctx, JSValueConst targetObject, int argc, JSValueConst* argv) {
    Handle* target = jsThis<Handle>(ctx, targetObject, *Methods->id);
    double n[6];
    if (target == nullptr || !readNumbers(ctx, argc, argv, 0, 6, n))
        return JS_EXCEPTION;
    Methods->bezierCurveTo(target, n[0], n[1], n[2], n[3], n[4], n[5]);
    return JS_UNDEFINED;
}

template <typename Handle, const PathMethods<Handle>* Methods>
JSValue arcTo(JSContext* ctx, JSValueConst targetObject, int argc, JSValueConst* argv) {
    Handle* target = jsThis<Handle>(ctx, targetObject, *Methods->id);
    double n[5];
    if (target == nullptr || !readNumbers(ctx, argc, argv, 0, 5, n))
        return JS_EXCEPTION;
    return pathResult(ctx, Methods->arcTo(target, n[0], n[1], n[2], n[3], n[4]));
}

template <typename Handle, const PathMethods<Handle>* Methods>
JSValue rect(JSContext* ctx, JSValueConst targetObject, int argc, JSValueConst* argv) {
    Handle* target = jsThis<Handle>(ctx, targetObject, *Methods->id);
    double n[4];
    if (target == nullptr || !readNumbers(ctx, argc, argv, 0, 4, n))
        return JS_EXCEPTION;
    Methods->rect(target, n[0], n[1], n[2], n[3]);
    return JS_UNDEFINED;
}

// Reads a radius of roundRect: a number, or a DOMPointInit, whose x and y are the corner's radii.
bool readRadius(JSContext* ctx, JSValueConst value, PrDVec2* out) {
    if (JS_IsObject(value)) {
        PrDomPoint point{0, 0, 0, 1};
        if (!prJsDomPointInitRead(ctx, value, &point))
            return false;
        *out = PrDVec2{point.x, point.y};
        return true;
    }
    double radius = 0;
    if (JS_ToFloat64(ctx, &radius, value) < 0)
        return false;
    *out = PrDVec2{radius, radius};
    return true;
}

template <typename Handle, const PathMethods<Handle>* Methods>
JSValue roundRect(JSContext* ctx, JSValueConst targetObject, int argc, JSValueConst* argv) {
    Handle* target = jsThis<Handle>(ctx, targetObject, *Methods->id);
    double n[4];
    if (target == nullptr || !readNumbers(ctx, argc, argv, 0, 4, n))
        return JS_EXCEPTION;
    // The radii are a number, a DOMPointInit, or a sequence of them (webidl/#js-union), and 0 by default.
    Array<PrDVec2> radii;
    JSValueConst given = argc > 4 ? argv[4] : JS_UNDEFINED;
    bool ok = true;
    JSValue method = JS_IsObject(given) ? jsIteratorMethod(ctx, given) : JS_UNDEFINED;
    if (JS_IsException(method)) {
        ok = false;
    } else if (JS_IsUndefined(method) || JS_IsNull(method)) {
        PrDVec2 radius{};
        ok = JS_IsUndefined(given) || readRadius(ctx, given, &radius);
        radii.push(radius);
    } else {
        ok = jsIterate(ctx, given, method, [&](JSValue item) {
            PrDVec2 radius{};
            bool read = readRadius(ctx, item, &radius);
            JS_FreeValue(ctx, item);
            if (read)
                radii.push(radius);
            return read;
        });
    }
    JS_FreeValue(ctx, method);
    PrStatus status = ok ? Methods->roundRect(target, n[0], n[1], n[2], n[3], radii.data, radii.count) : PrStatus_Ok;
    radii.reset();
    if (!ok)
        return JS_EXCEPTION;
    return status == PrStatus_Ok ? JS_UNDEFINED : JS_ThrowRangeError(ctx, "The radii are not valid");
}

template <typename Handle, const PathMethods<Handle>* Methods>
JSValue arc(JSContext* ctx, JSValueConst targetObject, int argc, JSValueConst* argv) {
    Handle* target = jsThis<Handle>(ctx, targetObject, *Methods->id);
    double n[5];
    if (target == nullptr || !readNumbers(ctx, argc, argv, 0, 5, n))
        return JS_EXCEPTION;
    bool counterclockwise = argc > 5 && JS_ToBool(ctx, argv[5]) == 1;
    return pathResult(ctx, Methods->arc(target, n[0], n[1], n[2], n[3], n[4], counterclockwise));
}

template <typename Handle, const PathMethods<Handle>* Methods>
JSValue ellipse(JSContext* ctx, JSValueConst targetObject, int argc, JSValueConst* argv) {
    Handle* target = jsThis<Handle>(ctx, targetObject, *Methods->id);
    double n[7];
    if (target == nullptr || !readNumbers(ctx, argc, argv, 0, 7, n))
        return JS_EXCEPTION;
    bool counterclockwise = argc > 7 && JS_ToBool(ctx, argv[7]) == 1;
    return pathResult(ctx, Methods->ellipse(target, n[0], n[1], n[2], n[3], n[4], n[5], n[6], counterclockwise));
}

// The members of CanvasPath, in IDL order, for objects of `Handle` whose methods are `methods`.
#define PR_CANVAS_PATH(Handle, methods)                                                                                \
    PR_JS_METHOD("closePath", 0, (closePath<Handle, &methods>)),                                                       \
        PR_JS_MAGIC("moveTo", 2, (pointTo<Handle, &methods>), 0),                                                      \
        PR_JS_MAGIC("lineTo", 2, (pointTo<Handle, &methods>), 1),                                                      \
        PR_JS_METHOD("quadraticCurveTo", 4, (quadraticCurveTo<Handle, &methods>)),                                     \
        PR_JS_METHOD("bezierCurveTo", 6, (bezierCurveTo<Handle, &methods>)),                                           \
        PR_JS_METHOD("arcTo", 5, (arcTo<Handle, &methods>)), PR_JS_METHOD("rect", 4, (rect<Handle, &methods>)),       \
        PR_JS_METHOD("roundRect", 4, (roundRect<Handle, &methods>)),                                                   \
        PR_JS_METHOD("arc", 5, (arc<Handle, &methods>)), PR_JS_METHOD("ellipse", 7, (ellipse<Handle, &methods>))

// ---------------------------------------------------------------------------------------------------------------------
// CanvasGradient and CanvasPattern

JSValue addColorStop(JSContext* ctx, JSValueConst gradientObject, int argc, JSValueConst* argv) {
    PrCanvasGradient* gradient = jsThis<PrCanvasGradient>(ctx, gradientObject, gradientClass);
    double offset = 0;
    StringArgument color = {ctx};
    if (gradient == nullptr || !readFinite(ctx, argc, argv, 1, &offset) || !color.read(argv[1]))
        return JS_EXCEPTION;
    switch (prCanvasGradientAddColorStop(gradient, offset, color.value())) {
        case PrStatus_Ok: return JS_UNDEFINED;
        case PrStatus_OutOfRange: return jsDomExceptionThrow(ctx, "IndexSizeError");
        default: return jsDomExceptionThrow(ctx, "SyntaxError");
    }
}

JSValue patternSetTransform(JSContext* ctx, JSValueConst patternObject, int argc, JSValueConst* argv) {
    PrCanvasPattern* pattern = jsThis<PrCanvasPattern>(ctx, patternObject, patternClass);
    PrDomMatrix matrix{};
    if (pattern == nullptr || !prJsDomMatrix2DInitRead(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, &matrix))
        return JS_EXCEPTION;
    prCanvasPatternSetTransform(pattern, affineOf(matrix));
    return JS_UNDEFINED;
}

void gradientRetain(void* handle) {
    prCanvasGradientRetain(static_cast<PrCanvasGradient*>(handle));
}

void gradientRelease(void* handle) {
    prCanvasGradientRelease(static_cast<PrCanvasGradient*>(handle));
}

void patternRetain(void* handle) {
    prCanvasPatternRetain(static_cast<PrCanvasPattern*>(handle));
}

void patternRelease(void* handle) {
    prCanvasPatternRelease(static_cast<PrCanvasPattern*>(handle));
}

constexpr JsReferences kGradientReferences = {gradientRetain, gradientRelease, false};
constexpr JsReferences kPatternReferences = {patternRetain, patternRelease, false};

// Returns a new object that holds the reference to a gradient or a pattern that the caller passes.
JSValue newGradient(JSContext* ctx, PrCanvasGradient* gradient) {
    JSValue object = jsWrap(ctx, gradientClass, JS_NULL, gradient, true);
    if (JS_IsException(object))
        prCanvasGradientRelease(gradient);
    return object;
}

JSValue newPattern(JSContext* ctx, PrCanvasPattern* pattern) {
    JSValue object = jsWrap(ctx, patternClass, JS_NULL, pattern, true);
    if (JS_IsException(object))
        prCanvasPatternRelease(pattern);
    return object;
}

// ---------------------------------------------------------------------------------------------------------------------
// Image sources (html/#canvasimagesource)

// The outcome of checking the usability of an image argument (html/#check-the-usability-of-the-image-argument).
enum {
    Usability_Good,         // the source draws
    Usability_Bad,          // the source draws nothing, and the call does nothing
    Usability_Broken,       // the call throws an InvalidStateError
    Usability_Exception,    // the conversion threw
};
typedef uint8_t Usability;

// Reads a CanvasImageSource argument (html/#canvasimagesource) and checks its usability. A good source is written to
// `out` with one reference to its image for the caller. A value that is not a source throws a TypeError.
Usability imageSourceOf(JSContext* ctx, JSValueConst value, PrCanvasImageSource* out) {
    *out = PrCanvasImageSource{};
    PrDomElement* element = jsHandleAs<PrDomElement>(ctx, value, prJsDomElementClass);
    PrDomEventTarget* target = element != nullptr ? prDomElementAsEventTarget(element) : nullptr;
    if (PrHtmlImageElement* image = prHtmlImageElementQuery(target)) {
        PrDomImageState state = prHtmlImageElementState(image);
        if (state == PrDomImageState_Broken)
            return Usability_Broken;
        if (state != PrDomImageState_CompletelyAvailable || !prHtmlImageElementImageSource(image, out))
            return Usability_Bad;
        if (out->extent.x == 0 || out->extent.y == 0) {
            prDrawImageRelease(out->image);
            *out = PrCanvasImageSource{};
            return Usability_Bad;
        }
        return Usability_Good;
    }
    if (PrHtmlCanvasElement* canvas = prHtmlCanvasElementQuery(target))
        return prHtmlCanvasElementImageSource(canvas, out) ? Usability_Good : Usability_Broken;
    if (element != nullptr && prDomElementNamespace(element) == PrDomNamespace_Html &&
            (prDomElementTag(element) == PrDomTag_Video))
        return Usability_Bad;
    JS_ThrowTypeError(ctx, "Expected a CanvasImageSource");
    return Usability_Exception;
}

// ---------------------------------------------------------------------------------------------------------------------
// CanvasRenderingContext2D

PrCanvas* PR_NULLABLE contextOf(JSContext* ctx, JSValueConst object) {
    return jsThis<PrCanvas>(ctx, object, contextClass);
}

JSValue canvasOf(JSContext* ctx, JSValueConst canvasContext) {
    return contextOf(ctx, canvasContext) != nullptr ? jsReference(ctx, canvasContext, kCanvasSlot) : JS_EXCEPTION;
}

JSValue getContextAttributes(JSContext* ctx, JSValueConst canvasContext, int, JSValueConst*) {
    PrCanvas* canvas = contextOf(ctx, canvasContext);
    if (canvas == nullptr)
        return JS_EXCEPTION;
    PrCanvasSettings settings = prCanvasGetContextAttributes(canvas);
    JSValue object = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, object, "alpha", JS_NewBool(ctx, settings.alpha));
    JS_SetPropertyStr(ctx, object, "colorSpace", JS_NewString(ctx, colorSpaceName(settings.colorSpace)));
    JS_SetPropertyStr(ctx, object, "colorType", JS_NewString(ctx, kColorTypes[settings.colorType]));
    JS_SetPropertyStr(ctx, object, "desynchronized", JS_NewBool(ctx, settings.desynchronized));
    JS_SetPropertyStr(ctx, object, "willReadFrequently", JS_NewBool(ctx, settings.willReadFrequently));
    return object;
}

// Implements the methods that take no argument and return nothing, selected by the magic: save, restore, reset,
// beginPath and resetTransform.
JSValue noArguments(JSContext* ctx, JSValueConst canvasContext, int, JSValueConst*, int magic) {
    PrCanvas* canvas = contextOf(ctx, canvasContext);
    if (canvas == nullptr)
        return JS_EXCEPTION;
    void (*const kMethods[])(PrCanvas*) = {
        prCanvasSave, prCanvasRestore, prCanvasReset,
        prCanvasBeginPath, prCanvasResetTransform,
    };
    kMethods[magic](canvas);
    return JS_UNDEFINED;
}

JSValue isContextLost(JSContext* ctx, JSValueConst canvasContext, int, JSValueConst*) {
    PrCanvas* canvas = contextOf(ctx, canvasContext);
    return canvas != nullptr ? JS_NewBool(ctx, prCanvasIsContextLost(canvas)) : JS_EXCEPTION;
}

// --- CanvasTransform ---

// Implements scale and translate, selected by the magic.
JSValue twoNumbers(JSContext* ctx, JSValueConst canvasContext, int argc, JSValueConst* argv, int magic) {
    PrCanvas* canvas = contextOf(ctx, canvasContext);
    double n[2];
    if (canvas == nullptr || !readNumbers(ctx, argc, argv, 0, 2, n))
        return JS_EXCEPTION;
    (magic == 0 ? prCanvasScale : prCanvasTranslate)(canvas, n[0], n[1]);
    return JS_UNDEFINED;
}

JSValue rotate(JSContext* ctx, JSValueConst canvasContext, int argc, JSValueConst* argv) {
    PrCanvas* canvas = contextOf(ctx, canvasContext);
    double angle = 0;
    if (canvas == nullptr || !readNumbers(ctx, argc, argv, 0, 1, &angle))
        return JS_EXCEPTION;
    prCanvasRotate(canvas, angle);
    return JS_UNDEFINED;
}

JSValue transform(JSContext* ctx, JSValueConst canvasContext, int argc, JSValueConst* argv) {
    PrCanvas* canvas = contextOf(ctx, canvasContext);
    double n[6];
    if (canvas == nullptr || !readNumbers(ctx, argc, argv, 0, 6, n))
        return JS_EXCEPTION;
    prCanvasTransform(canvas, PrDAffine2{n[0], n[1], n[2], n[3], n[4], n[5]});
    return JS_UNDEFINED;
}

JSValue getTransform(JSContext* ctx, JSValueConst canvasContext, int, JSValueConst*) {
    PrCanvas* canvas = contextOf(ctx, canvasContext);
    if (canvas == nullptr)
        return JS_EXCEPTION;
    PrDAffine2 t = prCanvasGetTransform(canvas);
    double values[] = {t.a, t.b, t.c, t.d, t.e, t.f};
    PrDomMatrix matrix{};
    prDomMatrixFromValues(values, 6, &matrix);
    return prJsDomMatrixNew(ctx, matrix);
}

// setTransform(a, b, c, d, e, f) and setTransform(transform) (html/#dom-context-2d-settransform). Overload resolution
// takes the six numbers for six or more arguments, the dictionary for fewer than two, and throws for any other count.
JSValue setTransform(JSContext* ctx, JSValueConst canvasContext, int argc, JSValueConst* argv) {
    PrCanvas* canvas = contextOf(ctx, canvasContext);
    if (canvas == nullptr)
        return JS_EXCEPTION;
    if (argc >= 6) {
        double n[6];
        if (!readNumbers(ctx, argc, argv, 0, 6, n))
            return JS_EXCEPTION;
        prCanvasSetTransform(canvas, PrDAffine2{n[0], n[1], n[2], n[3], n[4], n[5]});
        return JS_UNDEFINED;
    }
    if (argc > 1)
        return JS_ThrowTypeError(ctx, "setTransform takes a matrix or six numbers");
    PrDomMatrix matrix{};
    if (!prJsDomMatrix2DInitRead(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, &matrix))
        return JS_EXCEPTION;
    prCanvasSetTransform(canvas, affineOf(matrix));
    return JS_UNDEFINED;
}

// --- CanvasCompositing and CanvasImageSmoothing ---

// The double attributes, by the magic of their accessors.
enum {
    NumberAttribute_GlobalAlpha,
    NumberAttribute_LineWidth,
    NumberAttribute_MiterLimit,
    NumberAttribute_LineDashOffset,
    NumberAttribute_ShadowOffsetX,
    NumberAttribute_ShadowOffsetY,
    NumberAttribute_ShadowBlur,
};
typedef uint8_t NumberAttribute;

JSValue numberAttribute(JSContext* ctx, JSValueConst canvasContext, int magic) {
    PrCanvas* canvas = contextOf(ctx, canvasContext);
    if (canvas == nullptr)
        return JS_EXCEPTION;
    switch (magic) {
        case NumberAttribute_GlobalAlpha: return JS_NewFloat64(ctx, prCanvasGlobalAlpha(canvas));
        case NumberAttribute_LineWidth: return JS_NewFloat64(ctx, prCanvasLineWidth(canvas));
        case NumberAttribute_MiterLimit: return JS_NewFloat64(ctx, prCanvasMiterLimit(canvas));
        case NumberAttribute_LineDashOffset:
            return JS_NewFloat64(ctx, prCanvasLineDashOffset(canvas));
        case NumberAttribute_ShadowOffsetX:
            return JS_NewFloat64(ctx, prCanvasShadowOffset(canvas).x);
        case NumberAttribute_ShadowOffsetY:
            return JS_NewFloat64(ctx, prCanvasShadowOffset(canvas).y);
        default: return JS_NewFloat64(ctx, prCanvasShadowBlur(canvas));
    }
}

JSValue setNumberAttribute(JSContext* ctx, JSValueConst canvasContext, JSValueConst input, int magic) {
    PrCanvas* canvas = contextOf(ctx, canvasContext);
    double value = 0;
    if (canvas == nullptr || JS_ToFloat64(ctx, &value, input) < 0)
        return JS_EXCEPTION;
    PrDVec2 offset = prCanvasShadowOffset(canvas);
    switch (magic) {
        case NumberAttribute_GlobalAlpha: prCanvasSetGlobalAlpha(canvas, value); break;
        case NumberAttribute_LineWidth: prCanvasSetLineWidth(canvas, value); break;
        case NumberAttribute_MiterLimit: prCanvasSetMiterLimit(canvas, value); break;
        case NumberAttribute_LineDashOffset: prCanvasSetLineDashOffset(canvas, value); break;
        case NumberAttribute_ShadowOffsetX:
            prCanvasSetShadowOffset(canvas, PrDVec2{value, offset.y});
            break;
        case NumberAttribute_ShadowOffsetY:
            prCanvasSetShadowOffset(canvas, PrDVec2{offset.x, value});
            break;
        default: prCanvasSetShadowBlur(canvas, value); break;
    }
    return JS_UNDEFINED;
}

// The DOMString attributes, by the magic of their accessors.
enum {
    StringAttribute_GlobalCompositeOperation,
    StringAttribute_ShadowColor,
    StringAttribute_Filter,
    StringAttribute_Lang,
    StringAttribute_Font,
    StringAttribute_LetterSpacing,
    StringAttribute_WordSpacing,
};
typedef uint8_t StringAttribute;

JSValue stringAttribute(JSContext* ctx, JSValueConst canvasContext, int magic) {
    PrCanvas* canvas = contextOf(ctx, canvasContext);
    if (canvas == nullptr)
        return JS_EXCEPTION;
    switch (magic) {
        case StringAttribute_GlobalCompositeOperation:
            return jsNewStringView(ctx, prCanvasGlobalCompositeOperation(canvas));
        case StringAttribute_ShadowColor:
            return jsNewStringView(ctx, prCanvasShadowColor(canvas));
        case StringAttribute_Lang: return jsNewStringView(ctx, prCanvasLang(canvas));
        case StringAttribute_Font: return jsNewStringView(ctx, prCanvasFont(canvas));
        case StringAttribute_LetterSpacing:
            return jsNewStringView(ctx, prCanvasLetterSpacing(canvas));
        case StringAttribute_WordSpacing:
            return jsNewStringView(ctx, prCanvasWordSpacing(canvas));
        default:
            return jsNewStringView(ctx, prCanvasFilter(canvas));
    }
}

JSValue setStringAttribute(JSContext* ctx, JSValueConst canvasContext, JSValueConst input, int magic) {
    PrCanvas* canvas = contextOf(ctx, canvasContext);
    StringArgument value = {ctx};
    if (canvas == nullptr || !value.read(input))
        return JS_EXCEPTION;
    switch (magic) {
        case StringAttribute_GlobalCompositeOperation:
            prCanvasSetGlobalCompositeOperation(canvas, value.value());
            break;
        case StringAttribute_ShadowColor:
            prCanvasSetShadowColor(canvas, value.value());
            break;
        case StringAttribute_Lang: prCanvasSetLang(canvas, value.value()); break;
        case StringAttribute_Font: prCanvasSetFont(canvas, value.value()); break;
        case StringAttribute_LetterSpacing:
            prCanvasSetLetterSpacing(canvas, value.value());
            break;
        case StringAttribute_WordSpacing:
            prCanvasSetWordSpacing(canvas, value.value());
            break;
        default:
            prCanvasSetFilter(canvas, value.value());
            break;
    }
    return JS_UNDEFINED;
}

JSValue imageSmoothingEnabled(JSContext* ctx, JSValueConst canvasContext) {
    PrCanvas* canvas = contextOf(ctx, canvasContext);
    return canvas != nullptr ? JS_NewBool(ctx, prCanvasImageSmoothingEnabled(canvas)) : JS_EXCEPTION;
}

JSValue setImageSmoothingEnabled(JSContext* ctx, JSValueConst canvasContext, JSValueConst input) {
    PrCanvas* canvas = contextOf(ctx, canvasContext);
    if (canvas == nullptr)
        return JS_EXCEPTION;
    prCanvasSetImageSmoothingEnabled(canvas, JS_ToBool(ctx, input) == 1);
    return JS_UNDEFINED;
}

// The attributes of IDL enumerations, by the magic of their accessors. An invalid value is ignored
// (webidl/#js-attributes).
enum {
    EnumAttribute_ImageSmoothingQuality,
    EnumAttribute_LineCap,
    EnumAttribute_LineJoin,
    EnumAttribute_TextAlign,
    EnumAttribute_TextBaseline,
    EnumAttribute_Direction,
    EnumAttribute_FontKerning,
    EnumAttribute_FontStretch,
    EnumAttribute_FontVariantCaps,
    EnumAttribute_TextRendering,
};
typedef uint8_t EnumAttribute;

// The names of an enumeration's values.
struct EnumNames {
    const char* const* names;
    uint32_t count;
};

// The names of each enumerated attribute's values, by EnumAttribute.
constexpr EnumNames kEnumAttributes[] = {
    {kSmoothingQualities, countOf(kSmoothingQualities)},
    {kLineCaps, countOf(kLineCaps)},
    {kLineJoins, countOf(kLineJoins)},
    {kTextAligns, countOf(kTextAligns)},
    {kTextBaselines, countOf(kTextBaselines)},
    {kDirections, countOf(kDirections)},
    {kFontKernings, countOf(kFontKernings)},
    {kFontStretches, countOf(kFontStretches)},
    {kFontVariantCaps, countOf(kFontVariantCaps)},
    {kTextRenderings, countOf(kTextRenderings)},
};

JSValue enumAttribute(JSContext* ctx, JSValueConst canvasContext, int magic) {
    PrCanvas* canvas = contextOf(ctx, canvasContext);
    if (canvas == nullptr)
        return JS_EXCEPTION;
    uint32_t value = 0;
    switch (magic) {
        case EnumAttribute_ImageSmoothingQuality:
            value = prCanvasImageSmoothingQuality(canvas);
            break;
        case EnumAttribute_LineCap: value = prCanvasLineCap(canvas); break;
        case EnumAttribute_LineJoin: value = prCanvasLineJoin(canvas); break;
        case EnumAttribute_TextAlign: value = prCanvasTextAlign(canvas); break;
        case EnumAttribute_TextBaseline: value = prCanvasTextBaseline(canvas); break;
        case EnumAttribute_Direction: value = prCanvasDirection(canvas); break;
        case EnumAttribute_FontKerning: value = prCanvasFontKerning(canvas); break;
        case EnumAttribute_FontStretch: value = prCanvasFontStretch(canvas); break;
        case EnumAttribute_FontVariantCaps: value = prCanvasFontVariantCaps(canvas); break;
        default: value = prCanvasTextRendering(canvas); break;
    }
    return JS_NewString(ctx, kEnumAttributes[magic].names[value]);
}

JSValue setEnumAttribute(JSContext* ctx, JSValueConst canvasContext, JSValueConst input, int magic) {
    PrCanvas* canvas = contextOf(ctx, canvasContext);
    int32_t value = 0;
    if (canvas == nullptr || !readEnum(ctx, input, kEnumAttributes[magic].names, kEnumAttributes[magic].count, &value))
        return JS_EXCEPTION;
    if (value < 0)
        return JS_UNDEFINED;
    switch (magic) {
        case EnumAttribute_ImageSmoothingQuality:
            prCanvasSetImageSmoothingQuality(canvas, PrCanvasImageSmoothingQuality(value));
            break;
        case EnumAttribute_LineCap: prCanvasSetLineCap(canvas, PrCanvasLineCap(value)); break;
        case EnumAttribute_LineJoin: prCanvasSetLineJoin(canvas, PrCanvasLineJoin(value)); break;
        case EnumAttribute_TextAlign:
            prCanvasSetTextAlign(canvas, PrCanvasTextAlign(value));
            break;
        case EnumAttribute_TextBaseline:
            prCanvasSetTextBaseline(canvas, PrCanvasTextBaseline(value));
            break;
        case EnumAttribute_Direction:
            prCanvasSetDirection(canvas, PrCanvasDirection(value));
            break;
        case EnumAttribute_FontKerning:
            prCanvasSetFontKerning(canvas, PrCanvasFontKerning(value));
            break;
        case EnumAttribute_FontStretch:
            prCanvasSetFontStretch(canvas, PrCanvasFontStretch(value));
            break;
        case EnumAttribute_FontVariantCaps:
            prCanvasSetFontVariantCaps(canvas, PrCanvasFontVariantCaps(value));
            break;
        default:
            prCanvasSetTextRendering(canvas, PrCanvasTextRendering(value));
            break;
    }
    return JS_UNDEFINED;
}

// --- CanvasFillStrokeStyles ---

// Implements fillStyle and strokeStyle, selected by the magic, which is a PrCanvasStyleRole.
JSValue style(JSContext* ctx, JSValueConst canvasContext, int magic) {
    PrCanvas* canvas = contextOf(ctx, canvasContext);
    if (canvas == nullptr)
        return JS_EXCEPTION;
    PrCanvasStyle value = prCanvasStyle(canvas, PrCanvasStyleRole(magic));
    switch (value.kind) {
        case PrCanvasStyleKind_Gradient: return jsWrap(ctx, gradientClass, JS_NULL, value.gradient, false);
        case PrCanvasStyleKind_Pattern: return jsWrap(ctx, patternClass, JS_NULL, value.pattern, false);
        default: return jsNewStringView(ctx, value.color);
    }
}

// The value is a (DOMString or CanvasGradient or CanvasPattern) (webidl/#js-union).
JSValue setStyle(JSContext* ctx, JSValueConst canvasContext, JSValueConst input, int magic) {
    PrCanvas* canvas = contextOf(ctx, canvasContext);
    if (canvas == nullptr)
        return JS_EXCEPTION;
    PrCanvasStyleRole role = PrCanvasStyleRole(magic);
    if (PrCanvasGradient* gradient = jsHandleAs<PrCanvasGradient>(ctx, input, gradientClass)) {
        prCanvasSetStyleGradient(canvas, role, gradient);
        return JS_UNDEFINED;
    }
    if (PrCanvasPattern* pattern = jsHandleAs<PrCanvasPattern>(ctx, input, patternClass)) {
        prCanvasSetStylePattern(canvas, role, pattern);
        return JS_UNDEFINED;
    }
    StringArgument color = {ctx};
    if (!color.read(input))
        return JS_EXCEPTION;
    prCanvasSetStyleColor(canvas, role, color.value());
    return JS_UNDEFINED;
}

JSValue createLinearGradient(JSContext* ctx, JSValueConst canvasContext, int argc, JSValueConst* argv) {
    PrCanvas* canvas = contextOf(ctx, canvasContext);
    double n[4];
    if (canvas == nullptr || !readFinite(ctx, argc, argv, 4, n))
        return JS_EXCEPTION;
    return newGradient(ctx, prCanvasGradientCreateLinear(n[0], n[1], n[2], n[3]));
}

JSValue createRadialGradient(JSContext* ctx, JSValueConst canvasContext, int argc, JSValueConst* argv) {
    PrCanvas* canvas = contextOf(ctx, canvasContext);
    double n[6];
    if (canvas == nullptr || !readFinite(ctx, argc, argv, 6, n))
        return JS_EXCEPTION;
    PrCanvasGradient* gradient = nullptr;
    if (prCanvasGradientCreateRadial(n[0], n[1], n[2], n[3], n[4], n[5], &gradient) != PrStatus_Ok)
        return jsDomExceptionThrow(ctx, "IndexSizeError");
    return newGradient(ctx, gradient);
}

JSValue createConicGradient(JSContext* ctx, JSValueConst canvasContext, int argc, JSValueConst* argv) {
    PrCanvas* canvas = contextOf(ctx, canvasContext);
    double n[3];
    if (canvas == nullptr || !readFinite(ctx, argc, argv, 3, n))
        return JS_EXCEPTION;
    return newGradient(ctx, prCanvasGradientCreateConic(n[0], n[1], n[2]));
}

JSValue createPattern(JSContext* ctx, JSValueConst canvasContext, int argc, JSValueConst* argv) {
    PrCanvas* canvas = contextOf(ctx, canvasContext);
    if (canvas == nullptr || !jsArgCount(ctx, argc, 2))
        return JS_EXCEPTION;
    PrCanvasImageSource source{};
    Usability usability = imageSourceOf(ctx, argv[0], &source);
    if (usability == Usability_Exception)
        return JS_EXCEPTION;
    // [LegacyNullToEmptyString] repetition, where the empty string is repeat.
    StringArgument repetition = {ctx};
    if (!repetition.readNullable(argv[1])) {
        prDrawImageRelease(source.image);
        return JS_EXCEPTION;
    }
    if (usability == Usability_Broken)
        return jsDomExceptionThrow(ctx, "InvalidStateError");
    if (usability == Usability_Bad)
        return JS_NULL;
    PrCanvasRepetition mode = PrCanvasRepetition_Repeat;
    bool known = repetition.value().size == 0;
    for (uint32_t i = 0; i < countOf(kRepetitions); ++i) {
        if (repetition.value() == kRepetitions[i]) {
            mode = PrCanvasRepetition(i);
            known = true;
        }
    }
    if (!known) {
        prDrawImageRelease(source.image);
        return jsDomExceptionThrow(ctx, "SyntaxError");
    }
    PrCanvasPattern* pattern = prCanvasPatternCreate(&source, mode);
    prDrawImageRelease(source.image);
    return newPattern(ctx, pattern);
}

// --- CanvasRect ---

// Implements clearRect, fillRect and strokeRect, selected by the magic.
JSValue rectangle(JSContext* ctx, JSValueConst canvasContext, int argc, JSValueConst* argv, int magic) {
    PrCanvas* canvas = contextOf(ctx, canvasContext);
    double n[4];
    if (canvas == nullptr || !readNumbers(ctx, argc, argv, 0, 4, n))
        return JS_EXCEPTION;
    void (*const kMethods[])(PrCanvas*, double, double, double, double) = {
        prCanvasClearRect, prCanvasFillRect,
        prCanvasStrokeRect,
    };
    kMethods[magic](canvas, n[0], n[1], n[2], n[3]);
    return JS_UNDEFINED;
}

// --- CanvasDrawPath ---

// Implements fill and clip, selected by the magic: fill(fillRule) and fill(path, fillRule) (html/#dom-context-2d-fill).
JSValue fillOrClip(JSContext* ctx, JSValueConst canvasContext, int argc, JSValueConst* argv, int magic) {
    PrCanvas* canvas = contextOf(ctx, canvasContext);
    if (canvas == nullptr)
        return JS_EXCEPTION;
    PrCanvasPath2D* path = argc > 0 ? pathOf(ctx, argv[0]) : nullptr;
    PrPathFillRule rule = PrPathFillRule_NonZero;
    if (!readFillRule(ctx, argc, argv, path != nullptr ? 1 : 0, &rule))
        return JS_EXCEPTION;
    (magic == 0 ? prCanvasFill : prCanvasClip)(canvas, path, rule);
    return JS_UNDEFINED;
}

JSValue stroke(JSContext* ctx, JSValueConst canvasContext, int argc, JSValueConst* argv) {
    PrCanvas* canvas = contextOf(ctx, canvasContext);
    if (canvas == nullptr)
        return JS_EXCEPTION;
    PrCanvasPath2D* path = argc > 0 ? pathOf(ctx, argv[0]) : nullptr;
    if (argc > 0 && path == nullptr)
        return JS_ThrowTypeError(ctx, "Expected a Path2D");
    prCanvasStroke(canvas, path);
    return JS_UNDEFINED;
}

// isPointInPath(x, y, fillRule) and isPointInPath(path, x, y, fillRule) (html/#dom-context-2d-ispointinpath).
JSValue isPointInPath(JSContext* ctx, JSValueConst canvasContext, int argc, JSValueConst* argv) {
    PrCanvas* canvas = contextOf(ctx, canvasContext);
    if (canvas == nullptr)
        return JS_EXCEPTION;
    PrCanvasPath2D* path = argc > 0 ? pathOf(ctx, argv[0]) : nullptr;
    int first = path != nullptr ? 1 : 0;
    double n[2];
    PrPathFillRule rule = PrPathFillRule_NonZero;
    if (!readNumbers(ctx, argc, argv, first, 2, n) || !readFillRule(ctx, argc, argv, first + 2, &rule))
        return JS_EXCEPTION;
    return JS_NewBool(ctx, prCanvasIsPointInPath(canvas, path, n[0], n[1], rule));
}

JSValue isPointInStroke(JSContext* ctx, JSValueConst canvasContext, int argc, JSValueConst* argv) {
    PrCanvas* canvas = contextOf(ctx, canvasContext);
    if (canvas == nullptr)
        return JS_EXCEPTION;
    PrCanvasPath2D* path = argc > 0 ? pathOf(ctx, argv[0]) : nullptr;
    double n[2];
    if (!readNumbers(ctx, argc, argv, path != nullptr ? 1 : 0, 2, n))
        return JS_EXCEPTION;
    return JS_NewBool(ctx, prCanvasIsPointInStroke(canvas, path, n[0], n[1]));
}

// --- CanvasUserInterface ---

// drawFocusIfNeeded(element) and drawFocusIfNeeded(path, element) (html/#dom-context-2d-drawfocusifneeded). The ring is
// drawn if the element is focused and is a descendant of the context's canvas.
JSValue drawFocusIfNeeded(JSContext* ctx, JSValueConst canvasContext, int argc, JSValueConst* argv) {
    PrCanvas* context = contextOf(ctx, canvasContext);
    if (context == nullptr || !jsArgCount(ctx, argc, 1))
        return JS_EXCEPTION;
    PrCanvasPath2D* path = pathOf(ctx, argv[0]);
    if (path != nullptr && !jsArgCount(ctx, argc, 2))
        return JS_EXCEPTION;
    PrDomElement* element = jsHandleAs<PrDomElement>(ctx, argv[path != nullptr ? 1 : 0], prJsDomElementClass);
    if (element == nullptr)
        return JS_ThrowTypeError(ctx, "Expected an Element");
    JSValue canvasObject = jsReference(ctx, canvasContext, kCanvasSlot);
    PrDomElement* canvas = jsHandleAs<PrDomElement>(ctx, canvasObject, prJsDomElementClass);
    JS_FreeValue(ctx, canvasObject);
    PrDomNode* node = prDomElementAsNode(element);
    if (canvas == nullptr || canvas == element || !prDomNodeContains(prDomElementAsNode(canvas), node))
        return JS_UNDEFINED;
    PrDomDocument* document = prDomNodeOwnerDocument(node);
    if (document == nullptr || prHtmlActiveElement(prDomDocumentAsNode(document)) != element)
        return JS_UNDEFINED;
    prCanvasDrawFocusRing(context, path);
    return JS_UNDEFINED;
}

// --- CanvasText ---

// Implements fillText and strokeText, selected by the magic (html/#dom-context-2d-filltext).
JSValue drawText(JSContext* ctx, JSValueConst canvasContext, int argc, JSValueConst* argv, int magic) {
    PrCanvas* canvas = contextOf(ctx, canvasContext);
    StringArgument text = {ctx};
    double n[3];
    if (canvas == nullptr || !jsArgCount(ctx, argc, 3) || !text.read(argv[0]) || !readNumbers(ctx, argc, argv, 1, 2, n))
        return JS_EXCEPTION;
    bool limited = jsArgGiven(argc, argv, 3);
    if (limited && JS_ToFloat64(ctx, &n[2], argv[3]) < 0)
        return JS_EXCEPTION;
    (magic == 0 ? prCanvasFillText : prCanvasStrokeText)(canvas, text.value(), n[0], n[1], limited ? &n[2] : nullptr);
    return JS_UNDEFINED;
}

JSValue measureText(JSContext* ctx, JSValueConst canvasContext, int argc, JSValueConst* argv) {
    PrCanvas* canvas = contextOf(ctx, canvasContext);
    StringArgument text = {ctx};
    if (canvas == nullptr || !jsArgCount(ctx, argc, 1) || !text.read(argv[0]))
        return JS_EXCEPTION;
    return jsNewValue(ctx, textMetricsClass, prCanvasMeasureText(canvas, text.value()));
}

// The attributes of TextMetrics (html/#textmetrics), in IDL order, by the magic of their getter.
JSValue textMetric(JSContext* ctx, JSValueConst metricsObject, int magic) {
    PrCanvasTextMetrics* metrics = jsValueOf<PrCanvasTextMetrics>(ctx, metricsObject, textMetricsClass, "TextMetrics");
    if (metrics == nullptr)
        return JS_EXCEPTION;
    const double values[] = {
        metrics->width, metrics->actualBoundingBoxLeft, metrics->actualBoundingBoxRight,
        metrics->fontBoundingBoxAscent, metrics->fontBoundingBoxDescent, metrics->actualBoundingBoxAscent,
        metrics->actualBoundingBoxDescent, metrics->emHeightAscent, metrics->emHeightDescent,
        metrics->hangingBaseline, metrics->alphabeticBaseline, metrics->ideographicBaseline,
    };
    return JS_NewFloat64(ctx, values[magic]);
}

// --- CanvasDrawImage ---

// drawImage with 3, 5 or 9 arguments (html/#dom-context-2d-drawimage). Overload resolution throws for any other count.
JSValue drawImage(JSContext* ctx, JSValueConst canvasContext, int argc, JSValueConst* argv) {
    PrCanvas* canvas = contextOf(ctx, canvasContext);
    if (canvas == nullptr || !jsArgCount(ctx, argc, 3))
        return JS_EXCEPTION;
    int count = argc >= 9 ? 9 : argc;
    if (count != 3 && count != 5 && count != 9)
        return JS_ThrowTypeError(ctx, "drawImage takes 3, 5 or 9 arguments");
    PrCanvasImageSource source{};
    Usability usability = imageSourceOf(ctx, argv[0], &source);
    if (usability == Usability_Exception)
        return JS_EXCEPTION;
    double n[8];
    if (!readNumbers(ctx, argc, argv, 1, count - 1, n)) {
        prDrawImageRelease(source.image);
        return JS_EXCEPTION;
    }
    if (usability == Usability_Broken)
        return jsDomExceptionThrow(ctx, "InvalidStateError");
    if (usability == Usability_Bad)
        return JS_UNDEFINED;
    double sx = 0;
    double sy = 0;
    double sw = double(source.extent.x);
    double sh = double(source.extent.y);
    double dx = n[0];
    double dy = n[1];
    double dw = sw;
    double dh = sh;
    if (count == 5) {
        dw = n[2];
        dh = n[3];
    } else if (count == 9) {
        sx = n[0];
        sy = n[1];
        sw = n[2];
        sh = n[3];
        dx = n[4];
        dy = n[5];
        dw = n[6];
        dh = n[7];
    }
    prCanvasDrawImage(canvas, &source, sx, sy, sw, sh, dx, dy, dw, dh);
    prDrawImageRelease(source.image);
    return JS_UNDEFINED;
}

// --- CanvasImageData ---

// Returns the color space of the context's bitmap.
PrColorSpace contextSpace(const PrCanvas* canvas) {
    return prCanvasGetContextAttributes(canvas).colorSpace;
}

// createImageData(sw, sh, settings) and createImageData(imagedata) (html/#dom-context-2d-createimagedata).
JSValue createImageData(JSContext* ctx, JSValueConst canvasContext, int argc, JSValueConst* argv) {
    PrCanvas* canvas = contextOf(ctx, canvasContext);
    if (canvas == nullptr || !jsArgCount(ctx, argc, 1))
        return JS_EXCEPTION;
    if (argc == 1) {
        ImageDataValue* from = imageDataOf(ctx, argv[0]);
        if (from == nullptr)
            return JS_EXCEPTION;
        ImageDataSettings settings{
            .colorSpace = contextSpace(canvas),
            .format = PrCanvasPixelFormat_RgbaUnorm8,
            .hasColorSpace = true,
        };
        return newImageData(ctx, from->data.extent, settings, settings.colorSpace, JS_UNDEFINED);
    }
    int32_t n[2];
    ImageDataSettings settings{};
    if (!enforceLongs(ctx, argc, argv, 0, 2, n) ||
            !readImageDataSettings(ctx, argc > 2 ? argv[2] : JS_UNDEFINED, &settings))
        return JS_EXCEPTION;
    if (n[0] == 0 || n[1] == 0)
        return jsDomExceptionThrow(ctx, "IndexSizeError");
    PrUVec2 extent{uint32_t(n[0] < 0 ? -int64_t(n[0]) : n[0]), uint32_t(n[1] < 0 ? -int64_t(n[1]) : n[1])};
    return newImageData(ctx, extent, settings, contextSpace(canvas), JS_UNDEFINED);
}

JSValue getImageData(JSContext* ctx, JSValueConst canvasContext, int argc, JSValueConst* argv) {
    PrCanvas* canvas = contextOf(ctx, canvasContext);
    int32_t n[4];
    ImageDataSettings settings{};
    if (canvas == nullptr || !enforceLongs(ctx, argc, argv, 0, 4, n) ||
            !readImageDataSettings(ctx, argc > 4 ? argv[4] : JS_UNDEFINED, &settings))
        return JS_EXCEPTION;
    if (n[2] == 0 || n[3] == 0)
        return jsDomExceptionThrow(ctx, "IndexSizeError");
    if (!prCanvasIsOriginClean(canvas))
        return jsDomExceptionThrow(ctx, "SecurityError");
    // A negative width or height counts from the far edge of the rectangle.
    int64_t x = n[0];
    int64_t y = n[1];
    int64_t w = n[2];
    int64_t h = n[3];
    if (w < 0) {
        x += w;
        w = -w;
    }
    if (h < 0) {
        y += h;
        h = -h;
    }
    if (x < INT32_MIN || y < INT32_MIN)
        return JS_ThrowRangeError(ctx, "The rectangle is out of range");
    PrUVec2 extent{uint32_t(w), uint32_t(h)};
    JSValue object = newImageData(ctx, extent, settings, contextSpace(canvas), JS_UNDEFINED);
    if (JS_IsException(object))
        return object;
    void* pixels = imageDataPixels(ctx, object);
    ImageDataValue* value = imageDataOf(ctx, object);
    if (pixels == nullptr || value == nullptr) {
        JS_FreeValue(ctx, object);
        return JS_EXCEPTION;
    }
    prCanvasGetImageData(canvas, PrIVec2{int32_t(x), int32_t(y)}, &value->data, pixels);
    return object;
}

// putImageData with 3 or 7 arguments (html/#dom-context-2d-putimagedata). Overload resolution throws for 4 to 6.
JSValue putImageData(JSContext* ctx, JSValueConst canvasContext, int argc, JSValueConst* argv) {
    PrCanvas* canvas = contextOf(ctx, canvasContext);
    if (canvas == nullptr || !jsArgCount(ctx, argc, 3))
        return JS_EXCEPTION;
    if (argc > 3 && argc < 7)
        return JS_ThrowTypeError(ctx, "putImageData takes 3 or 7 arguments");
    ImageDataValue* value = imageDataOf(ctx, argv[0]);
    if (value == nullptr)
        return JS_EXCEPTION;
    int32_t n[6];
    if (!enforceLongs(ctx, argc, argv, 1, argc >= 7 ? 6 : 2, n))
        return JS_EXCEPTION;
    if (argc < 7) {
        n[2] = 0;
        n[3] = 0;
        n[4] = int32_t(value->data.extent.x);
        n[5] = int32_t(value->data.extent.y);
    }
    void* pixels = imageDataPixels(ctx, argv[0]);
    if (pixels == nullptr)
        return JS_EXCEPTION;
    prCanvasPutImageData(canvas, &value->data, pixels, PrIVec2{n[0], n[1]}, n[2], n[3], n[4], n[5]);
    return JS_UNDEFINED;
}

// --- CanvasPathDrawingStyles ---

JSValue setLineDash(JSContext* ctx, JSValueConst canvasContext, int argc, JSValueConst* argv) {
    PrCanvas* canvas = contextOf(ctx, canvasContext);
    if (canvas == nullptr || !jsArgCount(ctx, argc, 1))
        return JS_EXCEPTION;
    Array<double> segments;
    bool ok = jsSequence(ctx, argv[0], [&](JSValue item) {
        double segment = 0;
        bool read = JS_ToFloat64(ctx, &segment, item) >= 0;
        JS_FreeValue(ctx, item);
        if (read)
            segments.push(segment);
        return read;
    });
    if (ok)
        prCanvasSetLineDash(canvas, segments.data, segments.count);
    segments.reset();
    return ok ? JS_UNDEFINED : JS_EXCEPTION;
}

JSValue getLineDash(JSContext* ctx, JSValueConst canvasContext, int, JSValueConst*) {
    PrCanvas* canvas = contextOf(ctx, canvasContext);
    if (canvas == nullptr)
        return JS_EXCEPTION;
    uint32_t count = prCanvasLineDashCount(canvas);
    const double* dashes = prCanvasGetLineDash(canvas);
    JSValue array = JS_NewArray(ctx);
    for (uint32_t i = 0; i < count; ++i)
        JS_SetPropertyUint32(ctx, array, i, JS_NewFloat64(ctx, dashes[i]));
    return array;
}

// ---------------------------------------------------------------------------------------------------------------------
// The prototypes

constexpr JSCFunctionListEntry kContextProto[] = {
    PR_JS_TAG("CanvasRenderingContext2D"),
    PR_JS_GET("canvas", canvasOf),
    PR_JS_METHOD("getContextAttributes", 0, getContextAttributes),
    PR_JS_MAGIC("save", 0, noArguments, 0),
    PR_JS_MAGIC("restore", 0, noArguments, 1),
    PR_JS_MAGIC("reset", 0, noArguments, 2),
    PR_JS_METHOD("isContextLost", 0, isContextLost),
    PR_JS_MAGIC("scale", 2, twoNumbers, 0),
    PR_JS_METHOD("rotate", 1, rotate),
    PR_JS_MAGIC("translate", 2, twoNumbers, 1),
    PR_JS_METHOD("transform", 6, transform),
    PR_JS_METHOD("getTransform", 0, getTransform),
    PR_JS_METHOD("setTransform", 0, setTransform),
    PR_JS_MAGIC("resetTransform", 0, noArguments, 4),
    PR_JS_MAGIC_GET_SET("globalAlpha", numberAttribute, setNumberAttribute, NumberAttribute_GlobalAlpha),
    PR_JS_MAGIC_GET_SET("globalCompositeOperation", stringAttribute, setStringAttribute,
        StringAttribute_GlobalCompositeOperation),
    PR_JS_GET_SET("imageSmoothingEnabled", imageSmoothingEnabled, setImageSmoothingEnabled),
    PR_JS_MAGIC_GET_SET("imageSmoothingQuality", enumAttribute, setEnumAttribute, EnumAttribute_ImageSmoothingQuality),
    PR_JS_MAGIC_GET_SET("strokeStyle", style, setStyle, PrCanvasStyleRole_Stroke),
    PR_JS_MAGIC_GET_SET("fillStyle", style, setStyle, PrCanvasStyleRole_Fill),
    PR_JS_METHOD("createLinearGradient", 4, createLinearGradient),
    PR_JS_METHOD("createRadialGradient", 6, createRadialGradient),
    PR_JS_METHOD("createConicGradient", 3, createConicGradient),
    PR_JS_METHOD("createPattern", 2, createPattern),
    PR_JS_MAGIC_GET_SET("shadowOffsetX", numberAttribute, setNumberAttribute, NumberAttribute_ShadowOffsetX),
    PR_JS_MAGIC_GET_SET("shadowOffsetY", numberAttribute, setNumberAttribute, NumberAttribute_ShadowOffsetY),
    PR_JS_MAGIC_GET_SET("shadowBlur", numberAttribute, setNumberAttribute, NumberAttribute_ShadowBlur),
    PR_JS_MAGIC_GET_SET("shadowColor", stringAttribute, setStringAttribute, StringAttribute_ShadowColor),
    PR_JS_MAGIC_GET_SET("filter", stringAttribute, setStringAttribute, StringAttribute_Filter),
    PR_JS_MAGIC("clearRect", 4, rectangle, 0),
    PR_JS_MAGIC("fillRect", 4, rectangle, 1),
    PR_JS_MAGIC("strokeRect", 4, rectangle, 2),
    PR_JS_MAGIC("beginPath", 0, noArguments, 3),
    PR_JS_MAGIC("fill", 0, fillOrClip, 0),
    PR_JS_METHOD("stroke", 0, stroke),
    PR_JS_METHOD("drawFocusIfNeeded", 1, drawFocusIfNeeded),
    PR_JS_MAGIC("clip", 0, fillOrClip, 1),
    PR_JS_METHOD("isPointInPath", 2, isPointInPath),
    PR_JS_METHOD("isPointInStroke", 2, isPointInStroke),
    PR_JS_MAGIC("fillText", 3, drawText, 0),
    PR_JS_MAGIC("strokeText", 3, drawText, 1),
    PR_JS_METHOD("measureText", 1, measureText),
    PR_JS_METHOD("drawImage", 3, drawImage),
    PR_JS_METHOD("createImageData", 1, createImageData),
    PR_JS_METHOD("getImageData", 4, getImageData),
    PR_JS_METHOD("putImageData", 3, putImageData),
    PR_JS_MAGIC_GET_SET("lineWidth", numberAttribute, setNumberAttribute, NumberAttribute_LineWidth),
    PR_JS_MAGIC_GET_SET("lineCap", enumAttribute, setEnumAttribute, EnumAttribute_LineCap),
    PR_JS_MAGIC_GET_SET("lineJoin", enumAttribute, setEnumAttribute, EnumAttribute_LineJoin),
    PR_JS_MAGIC_GET_SET("miterLimit", numberAttribute, setNumberAttribute, NumberAttribute_MiterLimit),
    PR_JS_METHOD("setLineDash", 1, setLineDash),
    PR_JS_METHOD("getLineDash", 0, getLineDash),
    PR_JS_MAGIC_GET_SET("lineDashOffset", numberAttribute, setNumberAttribute, NumberAttribute_LineDashOffset),
    PR_JS_MAGIC_GET_SET("lang", stringAttribute, setStringAttribute, StringAttribute_Lang),
    PR_JS_MAGIC_GET_SET("font", stringAttribute, setStringAttribute, StringAttribute_Font),
    PR_JS_MAGIC_GET_SET("textAlign", enumAttribute, setEnumAttribute, EnumAttribute_TextAlign),
    PR_JS_MAGIC_GET_SET("textBaseline", enumAttribute, setEnumAttribute, EnumAttribute_TextBaseline),
    PR_JS_MAGIC_GET_SET("direction", enumAttribute, setEnumAttribute, EnumAttribute_Direction),
    PR_JS_MAGIC_GET_SET("letterSpacing", stringAttribute, setStringAttribute, StringAttribute_LetterSpacing),
    PR_JS_MAGIC_GET_SET("fontKerning", enumAttribute, setEnumAttribute, EnumAttribute_FontKerning),
    PR_JS_MAGIC_GET_SET("fontStretch", enumAttribute, setEnumAttribute, EnumAttribute_FontStretch),
    PR_JS_MAGIC_GET_SET("fontVariantCaps", enumAttribute, setEnumAttribute, EnumAttribute_FontVariantCaps),
    PR_JS_MAGIC_GET_SET("textRendering", enumAttribute, setEnumAttribute, EnumAttribute_TextRendering),
    PR_JS_MAGIC_GET_SET("wordSpacing", stringAttribute, setStringAttribute, StringAttribute_WordSpacing),
    PR_CANVAS_PATH(PrCanvas, kContextMethods),
};

constexpr JSCFunctionListEntry kGradientProto[] = {
    PR_JS_TAG("CanvasGradient"),
    PR_JS_METHOD("addColorStop", 2, addColorStop),
};

constexpr JSCFunctionListEntry kPatternProto[] = {
    PR_JS_TAG("CanvasPattern"),
    PR_JS_METHOD("setTransform", 0, patternSetTransform),
};

constexpr JSCFunctionListEntry kPath2DProto[] = {
    PR_JS_TAG("Path2D"),
    PR_JS_METHOD("addPath", 1, addPath),
    PR_CANVAS_PATH(PrCanvasPath2D, kPath2DMethods),
};

constexpr JSCFunctionListEntry kTextMetricsProto[] = {
    PR_JS_TAG("TextMetrics"),
    PR_JS_MAGIC_GET("width", textMetric, 0),
    PR_JS_MAGIC_GET("actualBoundingBoxLeft", textMetric, 1),
    PR_JS_MAGIC_GET("actualBoundingBoxRight", textMetric, 2),
    PR_JS_MAGIC_GET("fontBoundingBoxAscent", textMetric, 3),
    PR_JS_MAGIC_GET("fontBoundingBoxDescent", textMetric, 4),
    PR_JS_MAGIC_GET("actualBoundingBoxAscent", textMetric, 5),
    PR_JS_MAGIC_GET("actualBoundingBoxDescent", textMetric, 6),
    PR_JS_MAGIC_GET("emHeightAscent", textMetric, 7),
    PR_JS_MAGIC_GET("emHeightDescent", textMetric, 8),
    PR_JS_MAGIC_GET("hangingBaseline", textMetric, 9),
    PR_JS_MAGIC_GET("alphabeticBaseline", textMetric, 10),
    PR_JS_MAGIC_GET("ideographicBaseline", textMetric, 11),
};

constexpr JSCFunctionListEntry kImageDataProto[] = {
    PR_JS_TAG("ImageData"),
    PR_JS_MAGIC_GET("width", imageDataExtent, 0),
    PR_JS_MAGIC_GET("height", imageDataExtent, 1),
    PR_JS_GET("data", imageDataData),
    PR_JS_GET("pixelFormat", imageDataPixelFormat),
    PR_JS_GET("colorSpace", imageDataColorSpace),
};

constexpr JsClassDefinition kClasses[] = {
    {PR_JS_CLASS("CanvasRenderingContext2D", contextClass, jsViewFinalizer<&contextClass>, kContextProto)},
    {
        PR_JS_CLASS("CanvasGradient", gradientClass, (jsFinalizer<&gradientClass, prCanvasGradientRelease>),
            kGradientProto),
        .references = &kGradientReferences,
    },
    {
        PR_JS_CLASS("CanvasPattern", patternClass, (jsFinalizer<&patternClass, prCanvasPatternRelease>), kPatternProto),
        .references = &kPatternReferences,
    },
    {
        PR_JS_CLASS("Path2D", path2DClass, (jsFinalizer<&path2DClass, prCanvasPath2DDestroy>), kPath2DProto),
        .constructor = path2DConstruct,
    },
    {
        .name = "ImageData",
        .id = &imageDataClass,
        .finalizer = jsValueFinalizer,
        .mark = jsValueMark,
        .proto = kImageDataProto,
        .protoCount = int(countOf(kImageDataProto)),
        .constructor = imageDataConstruct,
        .length = 2,
    },
    {
        .name = "TextMetrics",
        .id = &textMetricsClass,
        .finalizer = jsValueFinalizer,
        .mark = jsValueMark,
        .proto = kTextMetricsProto,
        .protoCount = int(countOf(kTextMetricsProto)),
    },
};

} // namespace

} // namespace pr_impl

using namespace pr_impl;

void prJsCanvasBind(JSContext* ctx) {
    for (const JsClassDefinition& definition : kClasses)
        JS_FreeValue(ctx, jsDefineClass(ctx, definition));
}

bool prJsCanvasSettingsRead(JSContext* ctx, JSValueConst value, PrCanvasSettings* out) {
    *out = prCanvasSettingsDefault();
    if (!JS_IsObject(value))
        return true;
    // The members, in lexicographic order (webidl/#js-dictionary).
    if (!jsDictBool(ctx, value, "alpha", &out->alpha))
        return false;
    constexpr const char* kMembers[] = {"colorSpace", "colorType"};
    for (const char* name : kMembers) {
        JSValue member = JS_GetPropertyStr(ctx, value, name);
        if (JS_IsException(member))
            return false;
        bool ok = true;
        if (!JS_IsUndefined(member)) {
            int32_t index = 0;
            bool space = name[5] == 'S';
            if (space)
                ok = requireEnum(ctx, member, kColorSpaceNames, countOf(kColorSpaceNames), &index);
            else
                ok = requireEnum(ctx, member, kColorTypes, countOf(kColorTypes), &index);
            if (ok && space)
                out->colorSpace = kColorSpaces[index];
            else if (ok)
                out->colorType = PrCanvasColorType(index);
        }
        JS_FreeValue(ctx, member);
        if (!ok)
            return false;
    }
    return jsDictBool(ctx, value, "desynchronized", &out->desynchronized) &&
        jsDictBool(ctx, value, "willReadFrequently", &out->willReadFrequently);
}

JSValue prJsCanvasRenderingContext2DNew(JSContext* ctx, PrCanvas* context, JSValueConst canvas) {
    JSValue object = jsWrap(ctx, contextClass, canvas, context, false);
    if (!JS_IsException(object))
        jsSetReference(ctx, object, kCanvasSlot, canvas);
    return object;
}
