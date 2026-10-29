// Math Functions
//
// css-values-4 - https://drafts.csswg.org/css-values-4/
// css-values-5 - https://drafts.csswg.org/css-values-5/
// css-typed-om-1 - https://drafts.css-houdini.org/css-typed-om-1/
//
// A math function in place of a numeric value is stored as its calculation tree (css-values-4/#calc-internal): a
// PrCssValueKind_Math value over the tree's root, in preorder like every value. The matcher matches a math function by
// the grammar that webref gives it. The functions here turn that match into a tree, check its type against the place
// where it stands, and simplify it as a specified value.

#pragma once

#include "pr/array.h"
#include "pr/css.h"
#include "pr/string_view.h"

#include "css/src/css_unit.h"
#include "css/src/css_value.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// The place where a math function appears (css-values-4/#calc-type-checking).
struct CssMathSlot {
    // The type that the function's value resolves to. CssUnitCategory_Number also stands for an <integer>, and
    // CssUnitCategory_Percent for a <percentage>. CssUnitCategory_Unknown accepts any type, and a percentage is then
    // its own type (css-typed-om-1/#dom-cssnumericvalue-parse).
    CssUnitCategory category;

    // What a percentage in the function resolves against: a dimension's category, CssUnitCategory_Percent if it stays a
    // percentage, or CssUnitCategory_Unknown if no percentage may appear.
    CssUnitCategory percentages;
};

// One node of a calculation tree while it is built and simplified.
struct CssMathNode {
    // A numeric value, a calc-operator node, a math function, or a value kept as written.
    CssValueNode value;

    uint32_t child;     // the first child, or kCssMathNone
    uint32_t sibling;   // the next child of the parent, or kCssMathNone

    // For a value kept as written, its start in CssMathScratch::leaves. kCssMathNone for every other node.
    uint32_t leaf;
};

// The index that refers to no node.
constexpr uint32_t kCssMathNone = ~0u;

// The maximum nesting of parentheses and functions in a calculation. Beyond it, the calculation is invalid
// (css-values-4/#calc-syntax).
constexpr uint32_t kCssMathMaxDepth = 64;

// Scratch memory for building a calculation tree, kept across math functions.
struct CssMathScratch {
    Array<CssMathNode> nodes;

    // The values kept as written: functions that are not math functions, and the keywords and commas among the
    // arguments.
    Array<CssValueNode> leaves;
};

// Frees the scratch's memory.
void cssMathScratchReset(CssMathScratch* scratch);

// Replaces the values from `first` to the end of the pool with a PrCssValueKind_Math value. The values are a math
// function as its grammar matched it. The new value holds the function's calculation tree
// (css-values-4/#parse-a-calculation), simplified (css-values-4/#calc-simplification). A function that is not a math
// function is a leaf of the tree and is kept whole. Returns false, leaving the pool unmodified, if the function's type
// is failure or does not resolve to what `slot` accepts.
bool cssMathReplace(CssValuePool* pool, uint32_t first, CssMathSlot slot, CssMathScratch* scratch);

// Simplifies the PrCssValueKind_Math value at `first` as cssMathReplace does. The value is the last in the pool, and
// its tree was built by a caller rather than written. Returns false, leaving the pool unmodified, if the tree is not a
// calculation or its type does not resolve to what `slot` accepts.
bool cssMathAdopt(CssValuePool* pool, uint32_t first, CssMathSlot slot, CssMathScratch* scratch);

// Writes to `out` the two values of calc(`value`) for a numeric value, simplified.
void cssMathWrap(CssValueNode value, CssValueNode out[PR_NONNULL 2]);

// Adds the values of calc(`value`) for a numeric value, simplified.
void cssMathAddValue(CssValuePool* pool, CssValueNode value);

// ---------------------------------------------------------------------------------------------------------------------
// Numeric values of any type (css-typed-om-1/#numeric-value)
//
// A numeric value is a number, a percentage, a dimension, a PrCssValueKind_Math value, or a node of a calculation tree.
// Its unit is a PrCssUnit.

// Sets `out` to the unit of a number, a percentage, or a dimension in a unit that the tables name. Returns false for
// any other value.
bool cssNumericUnit(const CssValueNode* value, PrCssUnit* out);

// Returns a number, a percentage or a dimension with the value `number` in `unit`.
CssValueNode cssNumericValue(double number, PrCssUnit unit);

// Returns the name of `unit` in the form that a numeric value's unit uses (css-typed-om-1/#dom-cssunitvalue-unit):
// "number", "percent", or the unit lowercased.
PrStringView cssNumericUnitName(PrCssUnit unit);

// Sets `out` to the unit that `name` names, compared ASCII case-insensitively. Returns false if there is none.
bool cssNumericUnitFromName(PrStringView name, PrCssUnit* out);

// Sets `out` to the type of the numeric value at `index` (css-typed-om-1/#cssnumericvalue-type). A percentage has the
// percentage type. Returns false for a value whose type is failure.
bool cssMathType(CssValueRun run, uint32_t index, PrCssNumericType* out);

// Returns the category that a value of `type` measures, or CssUnitCategory_Unknown if the type is not one that a math
// function resolves to.
CssUnitCategory cssMathCategory(const PrCssNumericType& type);

// Sets `out` to the type of a value in `unit` (css-typed-om-1/#cssnumericvalue-create-a-type). Returns false for a unit
// that measures no base type.
bool cssNumericTypeOfUnit(PrCssUnit unit, PrCssNumericType* out);

// Sets `out` to the type of a sum of `a` and `b` (css-typed-om-1/#cssnumericvalue-add-two-types). Returns false if the
// types cannot be added.
bool cssNumericTypeAdd(const PrCssNumericType& a, const PrCssNumericType& b, PrCssNumericType* out);

// Sets `out` to the type of a product of `a` and `b` (css-typed-om-1/#cssnumericvalue-multiply-two-types). Returns
// false if the types cannot be multiplied.
bool cssNumericTypeMultiply(const PrCssNumericType& a, const PrCssNumericType& b,
                            PrCssNumericType* out);

// Returns the type of the inverse of a value of `type` (css-typed-om-1/#cssnumericvalue-invert-a-type).
PrCssNumericType cssNumericTypeInvert(const PrCssNumericType& type);

// Converts the numeric value at `index` to a number in `unit` (css-typed-om-1/#dom-cssnumericvalue-to). Returns false
// if the value cannot be converted.
bool cssMathTo(CssValueRun run, uint32_t index, PrCssUnit unit, double* out);

// Adds a PrCssValueKind_Math value over a Sum of numeric values in `units`, or in the value's own units if `count` is
// zero (css-typed-om-1/#dom-cssnumericvalue-tosum). Returns false, adding nothing, if the value at `index` cannot be
// expressed in those units.
bool cssMathToSum(CssValueRun run, uint32_t index, const PrCssUnit* PR_NULLABLE units,
                  uint32_t count, CssValuePool* out);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
