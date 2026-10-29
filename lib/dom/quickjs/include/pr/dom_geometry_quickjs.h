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

// Returns the matrix that a DOMMatrixReadOnly holds, or null with a TypeError thrown.
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
