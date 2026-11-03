// JavaScript binding of the geometry interfaces.
//
// geometry-1 - https://drafts.csswg.org/geometry/

#include "pr/dom_geometry_quickjs.h"

#include "pr/count_of.h"
#include "pr/dom_geometry.h"
#include "pr/float.h"
#include "pr/quickjs_binding.h"
#include "pr/webidl_quickjs.h"

namespace pr_impl {

namespace {

// The key under which the runtime stores its matrix parser (prJsDomSetMatrixParser).
constexpr char kMatrixParserKey = 0;

// Parses a string that a constructor takes through the runtime's parser. Returns NotSupported if it has none.
PrStatus parseMatrix(JSContext* ctx, PrStringView text, PrDomMatrix* out) {
    const PrJsDomMatrixParser* parser =
        static_cast<const PrJsDomMatrixParser*>(jsRuntimeData(JS_GetRuntime(ctx), &kMatrixParserKey));
    return parser != nullptr ? parser->parse(text, out) : PrStatus_NotSupported;
}
JSClassID domPointReadOnlyClass;
JSClassID domPointClass;
JSClassID domRectReadOnlyClass;
JSClassID domRectClass;
JSClassID domQuadClass;
JSClassID domRectListClass;

// Returns the class that a constructor or static creates: DOMMatrixReadOnly for magic 0, and DOMMatrix for 1.
JSClassID classOf(int magic) {
    return magic == 0 ? prJsDomMatrixReadOnlyClass : prJsDomMatrixClass;
}

PrDomMatrix* PR_NULLABLE readOnlyOf(JSContext* ctx, JSValueConst object) {
    return jsValueOf<PrDomMatrix>(ctx, object, prJsDomMatrixReadOnlyClass, "DOMMatrixReadOnly");
}

PrDomMatrix* PR_NULLABLE mutableOf(JSContext* ctx, JSValueConst object) {
    return jsValueOf<PrDomMatrix>(ctx, object, prJsDomMatrixClass, "DOMMatrix");
}

// ---------------------------------------------------------------------------------------------------------------------
// DOMMatrixInit (geometry-1/#dictdef-dommatrixinit). Its members are read in Web IDL order: the members of the
// inherited DOMMatrix2DInit, then its own, each set in lexicographic order. Its validation and fixup distinguish an
// omitted member from a given one, which only a dictionary can do.

// A 2D element and its alias, as the dictionary gave them.
struct Aliased {
    const char* alias;
    const char* element;
    uint32_t index;        // into PrDomMatrix::m
    double fallback;       // value of the identity matrix at `index`
};

constexpr Aliased kAliased[] = {
    {"a", "m11", 0, 1}, {"b", "m12", 1, 0}, {"c", "m21", 4, 0},
    {"d", "m22", 5, 1}, {"e", "m41", 12, 0}, {"f", "m42", 13, 0},
};

struct MatrixElement {
    const char* name;
    uint32_t index;
};
constexpr MatrixElement kElements3D[] = {
    {"m13", 2}, {"m14", 3}, {"m23", 6}, {"m24", 7}, {"m31", 8},
    {"m32", 9}, {"m33", 10}, {"m34", 11}, {"m43", 14}, {"m44", 15},
};

// Reads the matrix that a DOMMatrixInit describes (geometry-1/#create-a-dommatrix-from-the-dictionary), after
// geometry-1/#matrix-validate-and-fixup. Throws a TypeError if an alias and its element disagree, or if is2D is true
// for a matrix with a 3D element that differs from the identity matrix.
bool matrixFromInit(JSContext* ctx, JSValueConst value, PrDomMatrix* out) {
    *out = prDomMatrixIdentity();
    bool present = false;
    if (!jsDictionary(ctx, value, &present))
        return false;
    if (!present)
        return true;
    double aliases[6] = {};
    double elements[6] = {};
    bool aliasGiven[6] = {};
    bool elementGiven[6] = {};
    for (uint32_t i = 0; i < 6; ++i) {
        if (!jsDictNumber(ctx, value, kAliased[i].alias, &aliases[i], &aliasGiven[i]))
            return false;
    }
    for (uint32_t i = 0; i < 6; ++i) {
        if (!jsDictNumber(ctx, value, kAliased[i].element, &elements[i], &elementGiven[i]))
            return false;
    }
    bool is2D = false;
    bool is2DGiven = false;
    if (!jsDictBool(ctx, value, "is2D", &is2D, &is2DGiven))
        return false;
    bool flat = true;
    for (const MatrixElement& element : kElements3D) {
        if (!jsDictNumber(ctx, value, element.name, &out->m[element.index]))
            return false;
        flat = flat && out->m[element.index] == prDomMatrixIdentity().m[element.index];
    }
    for (uint32_t i = 0; i < 6; ++i) {
        if (aliasGiven[i] && elementGiven[i] && !sameValueZeroF64(aliases[i], elements[i])) {
            JS_ThrowTypeError(ctx, "%s and %s disagree", kAliased[i].alias, kAliased[i].element);
            return false;
        }
        out->m[kAliased[i].index] = elementGiven[i] ? elements[i] : aliasGiven[i] ? aliases[i] : kAliased[i].fallback;
    }
    if (is2DGiven && is2D && !flat) {
        JS_ThrowTypeError(ctx, "A 2D matrix has only the identity's 3D elements");
        return false;
    }
    out->is2D = is2DGiven ? is2D : flat;
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// Operations. Each reads its arguments first and then applies them to a matrix, so a read-only method can copy the
// matrix after its arguments are converted.

struct Arguments {
    double n[6];
    PrDomMatrix matrix;
};

// Reads arguments 0 to `count`, each by ToNumber, or as its default if it is not given.
bool readNumbers(JSContext* ctx, int argc, JSValueConst* argv, const double* defaults, uint32_t count, Arguments* out) {
    for (uint32_t i = 0; i < count; ++i) {
        if (!jsArgNumber(ctx, argc, argv, int(i), defaults[i], &out->n[i]))
            return false;
    }
    return true;
}

template <uint32_t Count, int D0 = 0, int D1 = 0, int D2 = 0, int D3 = 0>
bool readDefaults(JSContext* ctx, int argc, JSValueConst* argv, Arguments* out) {
    constexpr double kDefaults[] = {D0, D1, D2, D3};
    return readNumbers(ctx, argc, argv, kDefaults, Count, out);
}

bool readScale(JSContext* ctx, int argc, JSValueConst* argv, Arguments* out) {
    constexpr double kDefaults[] = {1, 0, 1, 0, 0, 0};
    if (!readNumbers(ctx, argc, argv, kDefaults, 6, out))
        return false;
    if (!jsArgGiven(argc, argv, 1))
        out->n[1] = out->n[0];
    return true;
}

bool readRotate(JSContext* ctx, int argc, JSValueConst* argv, Arguments* out) {
    if (!readDefaults<3>(ctx, argc, argv, out))
        return false;
    if (!jsArgGiven(argc, argv, 1) && !jsArgGiven(argc, argv, 2)) {
        out->n[2] = out->n[0];
        out->n[0] = 0;
    }
    return true;
}

bool readOther(JSContext* ctx, int argc, JSValueConst* argv, Arguments* out) {
    return matrixFromInit(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, &out->matrix);
}

bool readNothing(JSContext*, int, JSValueConst*, Arguments*) {
    return true;
}

struct Operation {
    bool (*read)(JSContext* ctx, int argc, JSValueConst* argv, Arguments* out);
    void (*apply)(PrDomMatrix* matrix, const Arguments* arguments);
};

enum {
    OperationKind_Translate,
    OperationKind_Scale,
    OperationKind_Scale3d,
    OperationKind_ScaleNonUniform,
    OperationKind_Rotate,
    OperationKind_RotateFromVector,
    OperationKind_RotateAxisAngle,
    OperationKind_SkewX,
    OperationKind_SkewY,
    OperationKind_Multiply,
    OperationKind_PreMultiply,
    OperationKind_FlipX,
    OperationKind_FlipY,
    OperationKind_Invert,
};
typedef uint8_t OperationKind;

// geometry-1/#dommatrixreadonly and #dommatrix. Each read-only method applies its Self counterpart to a copy.
constexpr Operation kOperations[] = {
    {
        readDefaults<3>,
        [](PrDomMatrix* m, const Arguments* a) {
            prDomMatrixTranslate(m, a->n[0], a->n[1], a->n[2]);
        },
    },
    {
        readScale,
        [](PrDomMatrix* m, const Arguments* a) {
            prDomMatrixScale(m, a->n[0], a->n[1], a->n[2], a->n[3], a->n[4], a->n[5]);
        },
    },
    {
        readDefaults<4,
        1>,
        [](PrDomMatrix* m, const Arguments* a) {
            prDomMatrixScale(m, a->n[0], a->n[0], a->n[0], a->n[1], a->n[2], a->n[3]);
        },
    },
    {
        readDefaults<2,
        1,
        1>,
        [](PrDomMatrix* m, const Arguments* a) { prDomMatrixScale(m, a->n[0], a->n[1], 1, 0, 0, 0); },
    },
    {
        readRotate,
        [](PrDomMatrix* m, const Arguments* a) { prDomMatrixRotate(m, a->n[0], a->n[1], a->n[2]); },
    },
    {
        readDefaults<2>,
        [](PrDomMatrix* m, const Arguments* a) { prDomMatrixRotateFromVector(m, a->n[0], a->n[1]); },
    },
    {
        readDefaults<4>,
        [](PrDomMatrix* m, const Arguments* a) {
            prDomMatrixRotateAxisAngle(m, a->n[0], a->n[1], a->n[2], a->n[3]);
        },
    },
    {readDefaults<1>, [](PrDomMatrix* m, const Arguments* a) { prDomMatrixSkewX(m, a->n[0]); }},
    {readDefaults<1>, [](PrDomMatrix* m, const Arguments* a) { prDomMatrixSkewY(m, a->n[0]); }},
    {readOther, [](PrDomMatrix* m, const Arguments* a) { prDomMatrixMultiply(m, &a->matrix); }},
    {readOther, [](PrDomMatrix* m, const Arguments* a) { prDomMatrixPreMultiply(m, &a->matrix); }},
    {readNothing, [](PrDomMatrix* m, const Arguments*) { prDomMatrixScale(m, -1, 1, 1, 0, 0, 0); }},
    {readNothing, [](PrDomMatrix* m, const Arguments*) { prDomMatrixScale(m, 1, -1, 1, 0, 0, 0); }},
    {readNothing, [](PrDomMatrix* m, const Arguments*) { prDomMatrixInvert(m); }},
};

// Implements a read-only method, which returns a new DOMMatrix with the operation applied to a copy.
JSValue applyToCopy(JSContext* ctx, JSValueConst matrix, int argc, JSValueConst* argv, int magic) {
    Arguments arguments = {};
    if (readOnlyOf(ctx, matrix) == nullptr || !kOperations[magic].read(ctx, argc, argv, &arguments))
        return JS_EXCEPTION;
    PrDomMatrix copy = *readOnlyOf(ctx, matrix);
    kOperations[magic].apply(&copy, &arguments);
    return jsNewValue(ctx, prJsDomMatrixClass, copy);
}

// Implements a DOMMatrix Self method, which applies the operation in place and returns the matrix.
JSValue applyToSelf(JSContext* ctx, JSValueConst matrix, int argc, JSValueConst* argv, int magic) {
    Arguments arguments = {};
    if (mutableOf(ctx, matrix) == nullptr || !kOperations[magic].read(ctx, argc, argv, &arguments))
        return JS_EXCEPTION;
    kOperations[magic].apply(mutableOf(ctx, matrix), &arguments);
    return JS_DupValue(ctx, matrix);
}

// ---------------------------------------------------------------------------------------------------------------------
// Constructors and statics

// geometry-1/#dom-dommatrixreadonly-dommatrixreadonly
template <int Magic>
JSValue construct(JSContext* ctx, JSValueConst newTarget, int argc, JSValueConst* argv) {
    PrDomMatrix matrix = prDomMatrixIdentity();
    if (jsArgGiven(argc, argv, 0)) {
        JsDoubleSequenceOrString init = {};
        bool read = init.read(ctx, argv[0]);
        PrStatus status;
        if (!read)
            status = PrStatus_Ok;
        else if (init.sequence)
            status = prDomMatrixFromValues(init.values.data, init.values.count, &matrix);
        else
            status = parseMatrix(ctx, jsStringView(&init.string), &matrix);
        init.reset(ctx);
        if (!read)
            return JS_EXCEPTION;
        if (status == PrStatus_InvalidData)
            return JS_ThrowTypeError(ctx, "A matrix takes 6 or 16 values");
        if (status == PrStatus_NotSupported)
            return JS_ThrowTypeError(ctx, "A string describes no matrix where no CSS parses it");
        if (status == PrStatus_Syntax)
            return jsDomExceptionThrow(ctx, "SyntaxError");
    }
    return jsConstructed(ctx, newTarget, jsNewValue(ctx, classOf(Magic), matrix));
}

// geometry-1/#dom-dommatrixreadonly-frommatrix
JSValue fromMatrix(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv, int magic) {
    PrDomMatrix matrix;
    if (!matrixFromInit(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, &matrix))
        return JS_EXCEPTION;
    return jsNewValue(ctx, classOf(magic), matrix);
}

// geometry-1/#dom-dommatrixreadonly-fromfloat32array and fromFloat64Array, selected by the magic's low bit.
JSValue fromFloatArray(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv, int magic) {
    double values[16];
    uint32_t count = 0;
    JSTypedArrayEnum type = (magic & 1) != 0 ? JS_TYPED_ARRAY_FLOAT64 : JS_TYPED_ARRAY_FLOAT32;
    if (!jsReadFloatArray(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, type, values, 16, &count))
        return JS_EXCEPTION;
    PrDomMatrix matrix;
    if (prDomMatrixFromValues(values, count, &matrix) != PrStatus_Ok)
        return JS_ThrowTypeError(ctx, "A matrix takes 6 or 16 values");
    return jsNewValue(ctx, classOf(magic >> 1), matrix);
}

// ---------------------------------------------------------------------------------------------------------------------
// Attributes and the rest of DOMMatrixReadOnly

// Implements a to f, and m11 to m44, selected by the index of their element.
JSValue elementGet(JSContext* ctx, JSValueConst matrixObject, int magic) {
    PrDomMatrix* matrix = readOnlyOf(ctx, matrixObject);
    return matrix != nullptr ? JS_NewFloat64(ctx, matrix->m[magic]) : JS_EXCEPTION;
}

JSValue elementSet(JSContext* ctx, JSValueConst matrix, JSValueConst input, int magic) {
    double value = 0;
    if (mutableOf(ctx, matrix) == nullptr || JS_ToFloat64(ctx, &value, input) < 0)
        return JS_EXCEPTION;
    prDomMatrixSetElement(mutableOf(ctx, matrix), uint32_t(magic), value);
    return JS_UNDEFINED;
}

JSValue is2DGet(JSContext* ctx, JSValueConst matrixObject) {
    PrDomMatrix* matrix = readOnlyOf(ctx, matrixObject);
    return matrix != nullptr ? JS_NewBool(ctx, matrix->is2D) : JS_EXCEPTION;
}

JSValue isIdentityGet(JSContext* ctx, JSValueConst matrixObject) {
    PrDomMatrix* matrix = readOnlyOf(ctx, matrixObject);
    return matrix != nullptr ? JS_NewBool(ctx, prDomMatrixIsIdentity(matrix)) : JS_EXCEPTION;
}

// Implements toFloat32Array and toFloat64Array, selected by the magic.
JSValue toFloatArray(JSContext* ctx, JSValueConst matrixObject, int, JSValueConst*, int magic) {
    PrDomMatrix* matrix = readOnlyOf(ctx, matrixObject);
    if (matrix == nullptr)
        return JS_EXCEPTION;
    return jsNewFloatArray(ctx, magic != 0 ? JS_TYPED_ARRAY_FLOAT64 : JS_TYPED_ARRAY_FLOAT32, matrix->m, 16);
}

// The attributes, in the order in which the interface declares them.
struct MatrixAttribute {
    const char* name;
    uint32_t index;
};
constexpr MatrixAttribute kAttributes[] = {
    {"a", 0},    {"b", 1},    {"c", 4},    {"d", 5},    {"e", 12},   {"f", 13},
    {"m11", 0},  {"m12", 1},  {"m13", 2},  {"m14", 3},  {"m21", 4},  {"m22", 5},
    {"m23", 6},  {"m24", 7},  {"m31", 8},  {"m32", 9},  {"m33", 10}, {"m34", 11},
    {"m41", 12}, {"m42", 13}, {"m43", 14}, {"m44", 15},
};

// webidl/#default-tojson-steps
JSValue toJson(JSContext* ctx, JSValueConst matrixObject, int, JSValueConst*) {
    PrDomMatrix* matrix = readOnlyOf(ctx, matrixObject);
    if (matrix == nullptr)
        return JS_EXCEPTION;
    JSValue result = JS_NewObject(ctx);
    for (const MatrixAttribute& attribute : kAttributes)
        JS_SetPropertyStr(ctx, result, attribute.name, JS_NewFloat64(ctx, matrix->m[attribute.index]));
    JS_SetPropertyStr(ctx, result, "is2D", JS_NewBool(ctx, matrix->is2D));
    JS_SetPropertyStr(ctx, result, "isIdentity", JS_NewBool(ctx, prDomMatrixIsIdentity(matrix)));
    return result;
}

// ---------------------------------------------------------------------------------------------------------------------
// Points (geometry-1/#DOMPoint)

// Returns the class that a constructor or static creates: the read-only class for magic 0, and the mutable class for 1.
JSClassID pointClassOf(int magic) {
    return magic == 0 ? domPointReadOnlyClass : domPointClass;
}

PrDomPoint* PR_NULLABLE pointOf(JSContext* ctx, JSValueConst object) {
    return jsValueOf<PrDomPoint>(ctx, object, domPointReadOnlyClass, "DOMPointReadOnly");
}

// geometry-1/#dom-dompointreadonly-dompointreadonly
template <int Magic>
JSValue constructPoint(JSContext* ctx, JSValueConst newTarget, int argc, JSValueConst* argv) {
    PrDomPoint point;
    if (!jsArgNumber(ctx, argc, argv, 0, 0, &point.x) || !jsArgNumber(ctx, argc, argv, 1, 0, &point.y) ||
            !jsArgNumber(ctx, argc, argv, 2, 0, &point.z) || !jsArgNumber(ctx, argc, argv, 3, 1, &point.w))
        return JS_EXCEPTION;
    return jsConstructed(ctx, newTarget, jsNewValue(ctx, pointClassOf(Magic), point));
}

// geometry-1/#dom-dompointreadonly-frompoint
JSValue fromPoint(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv, int magic) {
    PrDomPoint point;
    if (!prJsDomPointInitRead(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, &point))
        return JS_EXCEPTION;
    return jsNewValue(ctx, pointClassOf(magic), point);
}

// Implements x, y, z and w, selected by their offset in PrDomPoint.
JSValue pointGet(JSContext* ctx, JSValueConst pointObject, int magic) {
    PrDomPoint* point = pointOf(ctx, pointObject);
    return point != nullptr ? JS_NewFloat64(ctx, (&point->x)[magic]) : JS_EXCEPTION;
}

JSValue pointSet(JSContext* ctx, JSValueConst pointObject, JSValueConst input, int magic) {
    PrDomPoint* point = jsValueOf<PrDomPoint>(ctx, pointObject, domPointClass, "DOMPoint");
    double value = 0;
    if (point == nullptr || JS_ToFloat64(ctx, &value, input) < 0)
        return JS_EXCEPTION;
    (&point->x)[magic] = value;
    return JS_UNDEFINED;
}

// geometry-1/#dom-dompointreadonly-matrixtransform
JSValue matrixTransform(JSContext* ctx, JSValueConst point, int argc, JSValueConst* argv) {
    PrDomMatrix matrix;
    if (pointOf(ctx, point) == nullptr || !matrixFromInit(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, &matrix))
        return JS_EXCEPTION;
    return jsNewValue(ctx, domPointClass, prDomPointMatrixTransform(pointOf(ctx, point), &matrix));
}

// geometry-1/#dom-dommatrixreadonly-transformpoint
JSValue transformPoint(JSContext* ctx, JSValueConst matrix, int argc, JSValueConst* argv) {
    PrDomPoint point;
    if (readOnlyOf(ctx, matrix) == nullptr || !prJsDomPointInitRead(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, &point))
        return JS_EXCEPTION;
    return jsNewValue(ctx, domPointClass, prDomPointMatrixTransform(&point, readOnlyOf(ctx, matrix)));
}

JSValue pointToJson(JSContext* ctx, JSValueConst pointObject, int, JSValueConst*) {
    PrDomPoint* point = pointOf(ctx, pointObject);
    if (point == nullptr)
        return JS_EXCEPTION;
    JSValue result = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, result, "x", JS_NewFloat64(ctx, point->x));
    JS_SetPropertyStr(ctx, result, "y", JS_NewFloat64(ctx, point->y));
    JS_SetPropertyStr(ctx, result, "z", JS_NewFloat64(ctx, point->z));
    JS_SetPropertyStr(ctx, result, "w", JS_NewFloat64(ctx, point->w));
    return result;
}

// ---------------------------------------------------------------------------------------------------------------------
// Rectangles (geometry-1/#DOMRect)

// Reads the rectangle that a DOMRectInit describes (geometry-1/#dictdef-domrectinit), with its members read in
// lexicographic order.
bool rectFromInit(JSContext* ctx, JSValueConst value, PrDomRect* out) {
    *out = {};
    bool present = false;
    if (!jsDictionary(ctx, value, &present))
        return false;
    return !present ||
        (jsDictNumber(ctx, value, "height", &out->height) && jsDictNumber(ctx, value, "width", &out->width) &&
            jsDictNumber(ctx, value, "x", &out->x) && jsDictNumber(ctx, value, "y", &out->y));
}

JSClassID rectClassOf(int magic) {
    return magic == 0 ? domRectReadOnlyClass : domRectClass;
}

PrDomRect* PR_NULLABLE rectOf(JSContext* ctx, JSValueConst object) {
    return jsValueOf<PrDomRect>(ctx, object, domRectReadOnlyClass, "DOMRectReadOnly");
}

// geometry-1/#dom-domrectreadonly-domrectreadonly
template <int Magic>
JSValue constructRect(JSContext* ctx, JSValueConst newTarget, int argc, JSValueConst* argv) {
    PrDomRect rect;
    if (!jsArgNumber(ctx, argc, argv, 0, 0, &rect.x) || !jsArgNumber(ctx, argc, argv, 1, 0, &rect.y) ||
            !jsArgNumber(ctx, argc, argv, 2, 0, &rect.width) || !jsArgNumber(ctx, argc, argv, 3, 0, &rect.height))
        return JS_EXCEPTION;
    return jsConstructed(ctx, newTarget, jsNewValue(ctx, rectClassOf(Magic), rect));
}

// geometry-1/#dom-domrectreadonly-fromrect
JSValue fromRect(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv, int magic) {
    PrDomRect rect;
    if (!rectFromInit(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, &rect))
        return JS_EXCEPTION;
    return jsNewValue(ctx, rectClassOf(magic), rect);
}

// Implements x, y, width and height, selected by their offset in PrDomRect.
JSValue rectGet(JSContext* ctx, JSValueConst rectObject, int magic) {
    PrDomRect* rect = rectOf(ctx, rectObject);
    return rect != nullptr ? JS_NewFloat64(ctx, (&rect->x)[magic]) : JS_EXCEPTION;
}

JSValue rectSet(JSContext* ctx, JSValueConst rectObject, JSValueConst input, int magic) {
    PrDomRect* rect = jsValueOf<PrDomRect>(ctx, rectObject, domRectClass, "DOMRect");
    double value = 0;
    if (rect == nullptr || JS_ToFloat64(ctx, &value, input) < 0)
        return JS_EXCEPTION;
    (&rect->x)[magic] = value;
    return JS_UNDEFINED;
}

// The functions that compute the edges, in the order in which the interface declares them.
constexpr double (*kEdges[])(const PrDomRect*) = {prDomRectTop, prDomRectRight, prDomRectBottom, prDomRectLeft};

JSValue edgeGet(JSContext* ctx, JSValueConst rectObject, int magic) {
    PrDomRect* rect = rectOf(ctx, rectObject);
    return rect != nullptr ? JS_NewFloat64(ctx, kEdges[magic](rect)) : JS_EXCEPTION;
}

JSValue rectToJson(JSContext* ctx, JSValueConst rectObject, int, JSValueConst*) {
    PrDomRect* rect = rectOf(ctx, rectObject);
    if (rect == nullptr)
        return JS_EXCEPTION;
    JSValue result = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, result, "x", JS_NewFloat64(ctx, rect->x));
    JS_SetPropertyStr(ctx, result, "y", JS_NewFloat64(ctx, rect->y));
    JS_SetPropertyStr(ctx, result, "width", JS_NewFloat64(ctx, rect->width));
    JS_SetPropertyStr(ctx, result, "height", JS_NewFloat64(ctx, rect->height));
    JS_SetPropertyStr(ctx, result, "top", JS_NewFloat64(ctx, prDomRectTop(rect)));
    JS_SetPropertyStr(ctx, result, "right", JS_NewFloat64(ctx, prDomRectRight(rect)));
    JS_SetPropertyStr(ctx, result, "bottom", JS_NewFloat64(ctx, prDomRectBottom(rect)));
    JS_SetPropertyStr(ctx, result, "left", JS_NewFloat64(ctx, prDomRectLeft(rect)));
    return result;
}

// ---------------------------------------------------------------------------------------------------------------------
// Quadrilaterals (geometry-1/#DOMQuad)
//
// A quad's corners are DOMPoint objects that it owns, which script accesses through p1 to p4 and may change. The quad
// holds the addresses of the points inside those objects. The addresses stay fixed while the quad keeps the objects
// alive.

struct QuadCorners {
    PrDomPoint* PR_NONNULL p[4];
};

constexpr const char* kCorners[] = {"p1", "p2", "p3", "p4"};

// Returns a new DOMQuad whose corners are new DOMPoints at the corners of `quad`.
JSValue newQuad(JSContext* ctx, const PrDomQuad& quad) {
    JSValue points[4];
    QuadCorners corners;
    for (uint32_t i = 0; i < 4; ++i) {
        points[i] = jsNewValue(ctx, domPointClass, quad.p[i]);
        if (JS_IsException(points[i])) {
            for (uint32_t j = 0; j < i; ++j)
                JS_FreeValue(ctx, points[j]);
            return JS_EXCEPTION;
        }
        corners.p[i] = jsHandleAs<PrDomPoint>(ctx, points[i], domPointClass);
    }
    JSValue result = jsNewValue(ctx, domQuadClass, corners);
    for (uint32_t i = 0; i < 4; ++i) {
        if (!JS_IsException(result))
            jsSetSameObject(ctx, result, kCorners[i], points[i]);
        JS_FreeValue(ctx, points[i]);
    }
    return result;
}

QuadCorners* PR_NULLABLE quadOf(JSContext* ctx, JSValueConst object) {
    return jsValueOf<QuadCorners>(ctx, object, domQuadClass, "DOMQuad");
}

// geometry-1/#dom-domquad-domquad
JSValue constructQuad(JSContext* ctx, JSValueConst newTarget, int argc, JSValueConst* argv) {
    PrDomQuad quad;
    for (int i = 0; i < 4; ++i) {
        if (!prJsDomPointInitRead(ctx, i < argc ? argv[i] : JS_UNDEFINED, &quad.p[i]))
            return JS_EXCEPTION;
    }
    return jsConstructed(ctx, newTarget, newQuad(ctx, quad));
}

// geometry-1/#dom-domquad-fromrect
JSValue quadFromRect(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    PrDomRect rect;
    if (!rectFromInit(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, &rect))
        return JS_EXCEPTION;
    return newQuad(ctx, prDomQuadFromRect(&rect));
}

// geometry-1/#dom-domquad-fromquad. Its DOMQuadInit (geometry-1/#dictdef-domquadinit) is read in lexicographic order.
JSValue quadFromQuad(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    JSValueConst init = argc > 0 ? argv[0] : JS_UNDEFINED;
    bool present = false;
    if (!jsDictionary(ctx, init, &present))
        return JS_EXCEPTION;
    PrDomQuad quad;
    for (uint32_t i = 0; i < 4; ++i) {
        JSValue member = present ? JS_GetPropertyStr(ctx, init, kCorners[i]) : JS_UNDEFINED;
        bool read = !JS_IsException(member) && prJsDomPointInitRead(ctx, member, &quad.p[i]);
        JS_FreeValue(ctx, member);
        if (!read)
            return JS_EXCEPTION;
    }
    return newQuad(ctx, quad);
}

JSValue cornerGet(JSContext* ctx, JSValueConst quad, int magic) {
    if (quadOf(ctx, quad) == nullptr)
        return JS_EXCEPTION;
    return jsSameObject(ctx, quad, kCorners[magic]);
}

// geometry-1/#dom-domquad-getbounds
JSValue getBounds(JSContext* ctx, JSValueConst quadObject, int, JSValueConst*) {
    QuadCorners* corners = quadOf(ctx, quadObject);
    if (corners == nullptr)
        return JS_EXCEPTION;
    PrDomQuad quad;
    for (uint32_t i = 0; i < 4; ++i)
        quad.p[i] = *corners->p[i];
    return jsNewValue(ctx, domRectClass, prDomQuadGetBounds(&quad));
}

JSValue quadToJson(JSContext* ctx, JSValueConst quad, int, JSValueConst*) {
    if (quadOf(ctx, quad) == nullptr)
        return JS_EXCEPTION;
    JSValue result = JS_NewObject(ctx);
    for (const char* corner : kCorners)
        JS_SetPropertyStr(ctx, result, corner, jsSameObject(ctx, quad, corner));
    return result;
}

#define PR_ELEMENT(name, index) PR_JS_MAGIC_GET(name, elementGet, index)
#define PR_WRITABLE_ELEMENT(name, index) PR_JS_MAGIC_GET_SET(name, elementGet, elementSet, index)
#define PR_METHOD(name, operation) PR_JS_MAGIC(name, 0, applyToCopy, operation)
#define PR_SELF_METHOD(name, operation) PR_JS_MAGIC(name, 0, applyToSelf, operation)

constexpr JSCFunctionListEntry kDomMatrixReadOnlyProto[] = {
    PR_JS_TAG("DOMMatrixReadOnly"),
    PR_ELEMENT("a", 0),
    PR_ELEMENT("b", 1),
    PR_ELEMENT("c", 4),
    PR_ELEMENT("d", 5),
    PR_ELEMENT("e", 12),
    PR_ELEMENT("f", 13),
    PR_ELEMENT("m11", 0),
    PR_ELEMENT("m12", 1),
    PR_ELEMENT("m13", 2),
    PR_ELEMENT("m14", 3),
    PR_ELEMENT("m21", 4),
    PR_ELEMENT("m22", 5),
    PR_ELEMENT("m23", 6),
    PR_ELEMENT("m24", 7),
    PR_ELEMENT("m31", 8),
    PR_ELEMENT("m32", 9),
    PR_ELEMENT("m33", 10),
    PR_ELEMENT("m34", 11),
    PR_ELEMENT("m41", 12),
    PR_ELEMENT("m42", 13),
    PR_ELEMENT("m43", 14),
    PR_ELEMENT("m44", 15),
    PR_JS_GET("is2D", is2DGet),
    PR_JS_GET("isIdentity", isIdentityGet),
    PR_METHOD("translate", OperationKind_Translate),
    PR_METHOD("scale", OperationKind_Scale),
    PR_METHOD("scaleNonUniform", OperationKind_ScaleNonUniform),
    PR_METHOD("scale3d", OperationKind_Scale3d),
    PR_METHOD("rotate", OperationKind_Rotate),
    PR_METHOD("rotateFromVector", OperationKind_RotateFromVector),
    PR_METHOD("rotateAxisAngle", OperationKind_RotateAxisAngle),
    PR_METHOD("skewX", OperationKind_SkewX),
    PR_METHOD("skewY", OperationKind_SkewY),
    PR_METHOD("multiply", OperationKind_Multiply),
    PR_METHOD("flipX", OperationKind_FlipX),
    PR_METHOD("flipY", OperationKind_FlipY),
    PR_METHOD("inverse", OperationKind_Invert),
    PR_JS_METHOD("transformPoint", 0, transformPoint),
    PR_JS_MAGIC("toFloat32Array", 0, toFloatArray, 0),
    PR_JS_MAGIC("toFloat64Array", 0, toFloatArray, 1),
    PR_JS_METHOD("toJSON", 0, toJson),
};

constexpr JSCFunctionListEntry kDomMatrixProto[] = {
    PR_JS_TAG("DOMMatrix"),
    PR_WRITABLE_ELEMENT("a", 0), PR_WRITABLE_ELEMENT("b", 1), PR_WRITABLE_ELEMENT("c", 4),
    PR_WRITABLE_ELEMENT("d", 5), PR_WRITABLE_ELEMENT("e", 12), PR_WRITABLE_ELEMENT("f", 13),
    PR_WRITABLE_ELEMENT("m11", 0), PR_WRITABLE_ELEMENT("m12", 1), PR_WRITABLE_ELEMENT("m13", 2),
    PR_WRITABLE_ELEMENT("m14", 3), PR_WRITABLE_ELEMENT("m21", 4), PR_WRITABLE_ELEMENT("m22", 5),
    PR_WRITABLE_ELEMENT("m23", 6), PR_WRITABLE_ELEMENT("m24", 7), PR_WRITABLE_ELEMENT("m31", 8),
    PR_WRITABLE_ELEMENT("m32", 9), PR_WRITABLE_ELEMENT("m33", 10), PR_WRITABLE_ELEMENT("m34", 11),
    PR_WRITABLE_ELEMENT("m41", 12), PR_WRITABLE_ELEMENT("m42", 13), PR_WRITABLE_ELEMENT("m43", 14),
    PR_WRITABLE_ELEMENT("m44", 15),
    PR_SELF_METHOD("multiplySelf", OperationKind_Multiply),
    PR_SELF_METHOD("preMultiplySelf", OperationKind_PreMultiply),
    PR_SELF_METHOD("translateSelf", OperationKind_Translate),
    PR_SELF_METHOD("scaleSelf", OperationKind_Scale),
    PR_SELF_METHOD("scale3dSelf", OperationKind_Scale3d),
    PR_SELF_METHOD("rotateSelf", OperationKind_Rotate),
    PR_SELF_METHOD("rotateFromVectorSelf", OperationKind_RotateFromVector),
    PR_SELF_METHOD("rotateAxisAngleSelf", OperationKind_RotateAxisAngle),
    PR_SELF_METHOD("skewXSelf", OperationKind_SkewX),
    PR_SELF_METHOD("skewYSelf", OperationKind_SkewY),
    PR_SELF_METHOD("invertSelf", OperationKind_Invert),
};

// The statics of each class. The magic's low bit selects the class, and the bits above it select a float array's kind.
constexpr JSCFunctionListEntry kDomMatrixReadOnlyStatic[] = {
    PR_JS_MAGIC("fromMatrix", 0, fromMatrix, 0),
    PR_JS_MAGIC("fromFloat32Array", 1, fromFloatArray, 0),
    PR_JS_MAGIC("fromFloat64Array", 1, fromFloatArray, 1),
};

constexpr JSCFunctionListEntry kDomMatrixStatic[] = {
    PR_JS_MAGIC("fromMatrix", 0, fromMatrix, 1),
    PR_JS_MAGIC("fromFloat32Array", 1, fromFloatArray, 2),
    PR_JS_MAGIC("fromFloat64Array", 1, fromFloatArray, 3),
};

constexpr JSCFunctionListEntry kDomPointReadOnlyProto[] = {
    PR_JS_TAG("DOMPointReadOnly"),
    PR_JS_MAGIC_GET("x", pointGet, 0),
    PR_JS_MAGIC_GET("y", pointGet, 1),
    PR_JS_MAGIC_GET("z", pointGet, 2),
    PR_JS_MAGIC_GET("w", pointGet, 3),
    PR_JS_METHOD("matrixTransform", 0, matrixTransform),
    PR_JS_METHOD("toJSON", 0, pointToJson),
};

constexpr JSCFunctionListEntry kDomPointProto[] = {
    PR_JS_TAG("DOMPoint"),
    PR_JS_MAGIC_GET_SET("x", pointGet, pointSet, 0),
    PR_JS_MAGIC_GET_SET("y", pointGet, pointSet, 1),
    PR_JS_MAGIC_GET_SET("z", pointGet, pointSet, 2),
    PR_JS_MAGIC_GET_SET("w", pointGet, pointSet, 3),
};

constexpr JSCFunctionListEntry kDomPointReadOnlyStatic[] = {PR_JS_MAGIC("fromPoint", 0, fromPoint, 0)};
constexpr JSCFunctionListEntry kDomPointStatic[] = {PR_JS_MAGIC("fromPoint", 0, fromPoint, 1)};

constexpr JSCFunctionListEntry kDomRectReadOnlyProto[] = {
    PR_JS_TAG("DOMRectReadOnly"),
    PR_JS_MAGIC_GET("x", rectGet, 0),
    PR_JS_MAGIC_GET("y", rectGet, 1),
    PR_JS_MAGIC_GET("width", rectGet, 2),
    PR_JS_MAGIC_GET("height", rectGet, 3),
    PR_JS_MAGIC_GET("top", edgeGet, 0),
    PR_JS_MAGIC_GET("right", edgeGet, 1),
    PR_JS_MAGIC_GET("bottom", edgeGet, 2),
    PR_JS_MAGIC_GET("left", edgeGet, 3),
    PR_JS_METHOD("toJSON", 0, rectToJson),
};

constexpr JSCFunctionListEntry kDomRectProto[] = {
    PR_JS_TAG("DOMRect"),
    PR_JS_MAGIC_GET_SET("x", rectGet, rectSet, 0),
    PR_JS_MAGIC_GET_SET("y", rectGet, rectSet, 1),
    PR_JS_MAGIC_GET_SET("width", rectGet, rectSet, 2),
    PR_JS_MAGIC_GET_SET("height", rectGet, rectSet, 3),
};

constexpr JSCFunctionListEntry kDomRectReadOnlyStatic[] = {PR_JS_MAGIC("fromRect", 0, fromRect, 0)};
constexpr JSCFunctionListEntry kDomRectStatic[] = {PR_JS_MAGIC("fromRect", 0, fromRect, 1)};

constexpr JSCFunctionListEntry kDomQuadProto[] = {
    PR_JS_TAG("DOMQuad"),
    PR_JS_MAGIC_GET("p1", cornerGet, 0),
    PR_JS_MAGIC_GET("p2", cornerGet, 1),
    PR_JS_MAGIC_GET("p3", cornerGet, 2),
    PR_JS_MAGIC_GET("p4", cornerGet, 3),
    PR_JS_METHOD("getBounds", 0, getBounds),
    PR_JS_METHOD("toJSON", 0, quadToJson),
};

constexpr JSCFunctionListEntry kDomQuadStatic[] = {
    PR_JS_METHOD("fromRect", 0, quadFromRect),
    PR_JS_METHOD("fromQuad", 0, quadFromQuad),
};

// ---------------------------------------------------------------------------------------------------------------------
// DOMRectList (geometry-1/#DOMRectList): a list's handle holds its count, and the list keeps the objects of its
// rectangles, so that each index gives the same object.

// The rectangle objects that a list keeps, as an array.
JSValue rectsOf(JSContext* ctx, JSValueConst list) {
    return jsSameObject(ctx, list, "rects");
}

uint32_t rectListLength(void* handle) {
    return *static_cast<uint32_t*>(handle);
}

JSValue rectListItem(JSContext* ctx, JSValueConst rectsObject, void*, uint32_t index) {
    JSValue rects = rectsOf(ctx, rectsObject);
    JSValue item = JS_GetPropertyUint32(ctx, rects, index);
    JS_FreeValue(ctx, rects);
    return item;
}

constexpr JsIndexed kRectListIndexed = {rectListLength, rectListItem, nullptr};

JSValue rectListLengthGet(JSContext* ctx, JSValueConst countObject) {
    uint32_t* count = jsValueOf<uint32_t>(ctx, countObject, domRectListClass, "DOMRectList");
    return count != nullptr ? JS_NewUint32(ctx, *count) : JS_EXCEPTION;
}

JSValue rectListItemMethod(JSContext* ctx, JSValueConst countObject, int argc, JSValueConst* argv) {
    uint32_t* count = jsValueOf<uint32_t>(ctx, countObject, domRectListClass, "DOMRectList");
    double index = 0;
    if (count == nullptr || !jsArgCount(ctx, argc, 1) || !jsArgNumber(ctx, argc, argv, 0, 0, &index))
        return JS_EXCEPTION;
    // An unsigned long argument (webidl/#js-unsigned-long): the index modulo 2^32.
    double wrapped = isFiniteF64(index) ? fmodF64(index < 0 ? ceilF64(index) : floorF64(index), 4294967296.0) : 0;
    if (wrapped < 0)
        wrapped += 4294967296.0;
    uint32_t at = uint32_t(wrapped);
    return at < *count ? rectListItem(ctx, countObject, count, at) : JS_NULL;
}

constexpr JSCFunctionListEntry kDomRectListProto[] = {
    PR_JS_TAG("DOMRectList"),
    PR_JS_GET("length", rectListLengthGet),
    PR_JS_METHOD("item", 1, rectListItemMethod),
};

#undef PR_ELEMENT
#undef PR_WRITABLE_ELEMENT
#undef PR_METHOD
#undef PR_SELF_METHOD

} // namespace

} // namespace pr_impl

using namespace pr_impl;

// The classes that other modules' bindings refer to.
JSClassID prJsDomMatrixReadOnlyClass;
JSClassID prJsDomMatrixClass;

void prJsDomGeometryBind(JSContext* ctx) {
    JSValue readOnly = jsDefineClass(ctx, {
        .name = "DOMMatrixReadOnly",
        .id = &prJsDomMatrixReadOnlyClass,
        .finalizer = jsValueFinalizer,
        .mark = jsValueMark,
        .proto = kDomMatrixReadOnlyProto,
        .protoCount = int(countOf(kDomMatrixReadOnlyProto)),
        .statics = kDomMatrixReadOnlyStatic,
        .staticCount = int(countOf(kDomMatrixReadOnlyStatic)),
        .constructor = construct<0>,
    });
    JS_FreeValue(ctx, readOnly);
    JSValue matrix = jsDefineClass(ctx, {
        .name = "DOMMatrix",
        .id = &prJsDomMatrixClass,
        .finalizer = jsValueFinalizer,
        .mark = jsValueMark,
        .proto = kDomMatrixProto,
        .protoCount = int(countOf(kDomMatrixProto)),
        .statics = kDomMatrixStatic,
        .staticCount = int(countOf(kDomMatrixStatic)),
        .constructor = construct<1>,
        .base = &prJsDomMatrixReadOnlyClass,
    });
    // geometry-1/#dommatrix: [LegacyWindowAlias=(SVGMatrix, WebKitCSSMatrix)]
    JSValue global = JS_GetGlobalObject(ctx);
    constexpr const char* kAliases[] = {"SVGMatrix", "WebKitCSSMatrix"};
    for (const char* alias : kAliases)
        JS_DefinePropertyValueStr(ctx, global, alias, JS_DupValue(ctx, matrix),
            JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    JS_FreeValue(ctx, global);
    JS_FreeValue(ctx, matrix);

    const JsClassDefinition kClasses[] = {
        {
            .name = "DOMPointReadOnly",
            .id = &domPointReadOnlyClass,
            .finalizer = jsValueFinalizer,
            .mark = jsValueMark,
            .proto = kDomPointReadOnlyProto,
            .protoCount = int(countOf(kDomPointReadOnlyProto)),
            .statics = kDomPointReadOnlyStatic,
            .staticCount = int(countOf(kDomPointReadOnlyStatic)),
            .constructor = constructPoint<0>,
        },
        {
            .name = "DOMPoint",
            .id = &domPointClass,
            .finalizer = jsValueFinalizer,
            .mark = jsValueMark,
            .proto = kDomPointProto,
            .protoCount = int(countOf(kDomPointProto)),
            .statics = kDomPointStatic,
            .staticCount = int(countOf(kDomPointStatic)),
            .constructor = constructPoint<1>,
            .base = &domPointReadOnlyClass,
        },
        {
            .name = "DOMRectReadOnly",
            .id = &domRectReadOnlyClass,
            .finalizer = jsValueFinalizer,
            .mark = jsValueMark,
            .proto = kDomRectReadOnlyProto,
            .protoCount = int(countOf(kDomRectReadOnlyProto)),
            .statics = kDomRectReadOnlyStatic,
            .staticCount = int(countOf(kDomRectReadOnlyStatic)),
            .constructor = constructRect<0>,
        },
        {
            .name = "DOMRect",
            .id = &domRectClass,
            .finalizer = jsValueFinalizer,
            .mark = jsValueMark,
            .proto = kDomRectProto,
            .protoCount = int(countOf(kDomRectProto)),
            .statics = kDomRectStatic,
            .staticCount = int(countOf(kDomRectStatic)),
            .constructor = constructRect<1>,
            .base = &domRectReadOnlyClass,
        },
        {
            .name = "DOMQuad",
            .id = &domQuadClass,
            .finalizer = jsValueFinalizer,
            .mark = jsValueMark,
            .proto = kDomQuadProto,
            .protoCount = int(countOf(kDomQuadProto)),
            .statics = kDomQuadStatic,
            .staticCount = int(countOf(kDomQuadStatic)),
            .constructor = constructQuad,
        },
        {
            .name = "DOMRectList",
            .id = &domRectListClass,
            .finalizer = jsValueFinalizer,
            .mark = jsValueMark,
            .proto = kDomRectListProto,
            .protoCount = int(countOf(kDomRectListProto)),
            .indexed = &kRectListIndexed,
            .iterable = true,
        },
    };
    for (const JsClassDefinition& definition : kClasses)
        JS_FreeValue(ctx, jsDefineClass(ctx, definition));
}

PrDomMatrix* PR_NULLABLE prJsDomMatrixFromValue(JSContext* ctx, JSValueConst value) {
    return readOnlyOf(ctx, value);
}

bool prJsDomMatrixIsMutable(JSContext* ctx, JSValueConst value) {
    return jsHandleIn(ctx, value, prJsDomMatrixClass) != nullptr;
}

bool prJsDomMatrix2DInitRead(JSContext* ctx, JSValueConst value, PrDomMatrix* out) {
    *out = prDomMatrixIdentity();
    bool present = false;
    if (!jsDictionary(ctx, value, &present))
        return false;
    if (!present)
        return true;
    double aliases[6] = {};
    double elements[6] = {};
    bool aliasGiven[6] = {};
    bool elementGiven[6] = {};
    for (uint32_t i = 0; i < 6; ++i) {
        if (!jsDictNumber(ctx, value, kAliased[i].alias, &aliases[i], &aliasGiven[i]))
            return false;
    }
    for (uint32_t i = 0; i < 6; ++i) {
        if (!jsDictNumber(ctx, value, kAliased[i].element, &elements[i], &elementGiven[i]))
            return false;
    }
    for (uint32_t i = 0; i < 6; ++i) {
        if (aliasGiven[i] && elementGiven[i] && !sameValueZeroF64(aliases[i], elements[i])) {
            JS_ThrowTypeError(ctx, "%s and %s disagree", kAliased[i].alias, kAliased[i].element);
            return false;
        }
        out->m[kAliased[i].index] = elementGiven[i] ? elements[i] : aliasGiven[i] ? aliases[i] : kAliased[i].fallback;
    }
    return true;
}

bool prJsDomPointInitRead(JSContext* ctx, JSValueConst value, PrDomPoint* out) {
    *out = {0, 0, 0, 1};
    bool present = false;
    if (!jsDictionary(ctx, value, &present))
        return false;
    // The members are read in lexicographic order.
    return !present || (jsDictNumber(ctx, value, "w", &out->w) && jsDictNumber(ctx, value, "x", &out->x) &&
        jsDictNumber(ctx, value, "y", &out->y) && jsDictNumber(ctx, value, "z", &out->z));
}

JSValue prJsDomPointNew(JSContext* ctx, PrDomPoint point) {
    return jsNewValue(ctx, domPointClass, point);
}

JSValue prJsDomMatrixNew(JSContext* ctx, PrDomMatrix matrix) {
    return jsNewValue(ctx, prJsDomMatrixClass, matrix);
}

JSValue prJsDomRectNew(JSContext* ctx, PrDomRect rect) {
    return jsNewValue(ctx, domRectClass, rect);
}

JSValue prJsDomRectReadOnlyNew(JSContext* ctx, PrDomRect rect) {
    return jsNewValue(ctx, domRectReadOnlyClass, rect);
}

JSValue prJsDomRectListNew(JSContext* ctx, const PrDomRect* PR_NULLABLE rects, uint32_t count) {
    JSValue list = jsNewValue(ctx, domRectListClass, count);
    if (JS_IsException(list))
        return list;
    JSValue array = JS_NewArray(ctx);
    for (uint32_t i = 0; i < count; ++i) {
        JSValue rect = prJsDomRectNew(ctx, rects[i]);
        if (JS_IsException(rect) || JS_SetPropertyUint32(ctx, array, i, rect) < 0) {
            JS_FreeValue(ctx, array);
            JS_FreeValue(ctx, list);
            return JS_EXCEPTION;
        }
    }
    jsSetSameObject(ctx, list, "rects", array);
    JS_FreeValue(ctx, array);
    return list;
}

void prJsDomSetMatrixParser(JSRuntime* runtime, const PrJsDomMatrixParser* PR_NULLABLE parser) {
    jsSetRuntimeData(runtime, &kMatrixParserKey, parser);
}
