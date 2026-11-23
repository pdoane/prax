// JavaScript binding of the geometry interfaces
//
// geometry-1 - https://drafts.csswg.org/geometry/

#pragma once

#include "pr/dom_geometry.h"

#include "quickjs.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// The classes of DOMMatrixReadOnly and DOMMatrix.
extern JSClassID prJsDomMatrixReadOnlyClass;
extern JSClassID prJsDomMatrixClass;

// Defines the geometry interfaces on the global object of `ctx`.
void prJsDomGeometryBind(JSContext* ctx);

// Returns the matrix that a DOMMatrixReadOnly holds, or null with a TypeError thrown. A matrix that reflects a value
// that another module holds is read first, and a change made through the result is kept by prJsDomMatrixCommit.
PrDomMatrix* PR_NULLABLE prJsDomMatrixFromValue(JSContext* ctx, JSValueConst value);

// Returns true if `value` is a DOMMatrix, rather than a read-only matrix or anything else.
bool prJsDomMatrixIsMutable(JSContext* ctx, JSValueConst value);

// Reads the 2D matrix that a DOMMatrix2DInit describes (geometry-1/#create-a-dommatrix-from-the-2d-dictionary), after
// geometry-1/#matrix-validate-and-fixup-2d. Undefined and null describe the identity. Returns false if reading it
// threw, or with a TypeError thrown if an alias and its element disagree.
bool prJsDomMatrix2DInitRead(JSContext* ctx, JSValueConst value, PrDomMatrix* out);

// Reads the point that a DOMPointInit describes (geometry-1/#dictdef-dompointinit). Returns false if reading it threw.
bool prJsDomPointInitRead(JSContext* ctx, JSValueConst value, PrDomPoint* out);

// Returns a new DOMPoint that holds `point`.
JSValue prJsDomPointNew(JSContext* ctx, PrDomPoint point);

// Returns a new DOMMatrix that holds `matrix`.
JSValue prJsDomMatrixNew(JSContext* ctx, PrDomMatrix matrix);

// Returns a new DOMRect, or a new DOMRectReadOnly, that holds `rect`.
JSValue prJsDomRectNew(JSContext* ctx, PrDomRect rect);
JSValue prJsDomRectReadOnlyNew(JSContext* ctx, PrDomRect rect);

// Writes the rectangle of `value`, a DOMRect, to `out`. Returns false, with a TypeError thrown, for any other value.
bool prJsDomRectRead(JSContext* ctx, JSValueConst value, PrDomRect* out);

// The operations of a DOMPoint or DOMRect whose value another module holds, such as an item of an SVGPointList. Each
// operation takes the object's handle, which begins with a PrJsDomReflected. `read` writes the current value to `out`.
// `write` stores a new value, and returns false if it threw an exception. A value is a point's x, y, z and w, or a
// rectangle's x, y, width and height. `retain` and `release` count the object's reference to its handle.
typedef struct PrJsDomReflection {
    void (*read)(const void* handle, double out[PR_NONNULL 4]);
    bool (*write)(JSContext* ctx, void* handle, const double value[PR_NONNULL 4]);
    void (*retain)(void* handle);
    void (*release)(void* handle);
} PrJsDomReflection;

// The first member of the handle of a reflecting DOMPoint or DOMRect. It points to the handle's operations.
typedef struct PrJsDomReflected {
    const PrJsDomReflection* reflection;
} PrJsDomReflected;

// Returns the handle of `value` if it is a reflecting DOMPoint or DOMRect, and null otherwise.
PrJsDomReflected* PR_NULLABLE prJsDomReflectedFromValue(JSContext* ctx, JSValueConst value);

// Writes the point of `value`, a DOMPoint, to `out`. Returns false and throws a TypeError for any other value.
bool prJsDomPointRead(JSContext* ctx, JSValueConst value, PrDomPoint* out);

// Returns the DOMPoint or DOMRect that reflects `handle`. A handle has one object for as long as that object lives. A
// new object keeps `owner` alive.
JSValue prJsDomPointReflecting(JSContext* ctx, JSValueConst owner, PrJsDomReflected* handle);
JSValue prJsDomRectReflecting(JSContext* ctx, JSValueConst owner, PrJsDomReflected* handle);

// The operations of a DOMMatrix whose value another module holds, such as the matrix of an SVGTransform. Each operation
// takes the object's handle, which begins with a PrJsDomReflectedMatrix. `read` writes the current value to `out`.
// `write` stores a new value, and returns false if it threw an exception. `retain` and `release` count the object's
// reference to its handle.
typedef struct PrJsDomMatrixReflection {
    void (*read)(const void* handle, PrDomMatrix* out);
    bool (*write)(JSContext* ctx, void* handle, const PrDomMatrix* value);
    void (*retain)(void* handle);
    void (*release)(void* handle);
} PrJsDomMatrixReflection;

// The first members of the handle of a reflecting DOMMatrix. The object's operations work on `matrix`, which the
// binding reads before each use and writes back after each change.
typedef struct PrJsDomReflectedMatrix {
    const PrJsDomMatrixReflection* reflection;
    PrDomMatrix matrix;
} PrJsDomReflectedMatrix;

// Returns the DOMMatrix that reflects `handle`. A handle has one object for as long as that object lives. A new object
// keeps `owner` alive.
JSValue prJsDomMatrixReflecting(JSContext* ctx, JSValueConst owner, PrJsDomReflectedMatrix* handle);

// Writes back a change made through prJsDomMatrixFromValue to `value`, if it reflects a value that another module
// holds. Returns false if the write threw an exception.
bool prJsDomMatrixCommit(JSContext* ctx, JSValueConst value);

// Returns a new DOMRectList of `count` new DOMRects that hold `rects`.
JSValue prJsDomRectListNew(JSContext* ctx, const PrDomRect* PR_NULLABLE rects, uint32_t count);

// What parses a transform list into a matrix for the DOMMatrix constructors
// (geometry-1/#parse-a-string-into-an-abstract-matrix). Returns Syntax for text that is not one.
typedef struct PrJsDomMatrixParser {
    PrStatus (*parse)(PrStringView text, PrDomMatrix* out);
} PrJsDomMatrixParser;

// Gives the runtime the parser through which the DOMMatrix constructors take a string, or removes it if `parser` is
// null. CSS gives it. Without one, a string throws a TypeError, as it does where no CSS parses it.
void prJsDomSetMatrixParser(JSRuntime* runtime, const PrJsDomMatrixParser* PR_NULLABLE parser);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
