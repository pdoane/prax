// Animation values: easing, the lists of the animation properties, and interpolating computed values.
//
// css-easing-2 - https://drafts.csswg.org/css-easing-2/
// css-values-4 - https://drafts.csswg.org/css-values-4/
// css-transforms-1 - https://drafts.csswg.org/css-transforms-1/
// web-animations-1 - https://drafts.csswg.org/web-animations-1/

#include "css/src/css_animation.h"

#include "pr/count_of.h"
#include "pr/float.h"
#include "pr/mem.h"

#include "css/src/css_color_value.h"
#include "css/src/css_keyword.h"
#include "css/src/css_math.h"
#include "css/src/css_shape.h"
#include "css/src/css_style.h"
#include "css/src/css_transform.h"
#include "css/src/css_unit.h"

namespace pr_impl {

namespace {

// Collects the numbers among the arguments of a function, up to `capacity`, and returns how many it collected.
// `keyword` receives the last keyword argument, if there is one.
uint32_t functionArguments(CssValueRun run, double* numbers, uint32_t capacity, PrCssKeyword* keyword) {
    uint32_t count = 0;
    *keyword = PrCssKeyword_Count;
    CssValueRun children = cssValueChildren(run, 0);
    for (uint32_t i = 0; i < children.count; i += children.values[i].next) {
        const CssValueNode* value = &children.values[i];
        if (cssValueIsNumeric(value->kind) && count < capacity)
            numbers[count++] = value->number;
        else if (value->kind == PrCssValueKind_Keyword)
            *keyword = cssValueKeyword(value);
    }
    return count;
}

// Returns true if a computed value of `property` may be below zero.
bool allowsNegative(PrCssProperty property) {
    switch (property) {
        case PrCssProperty_MarginTop:
        case PrCssProperty_MarginRight:
        case PrCssProperty_MarginBottom:
        case PrCssProperty_MarginLeft:
        case PrCssProperty_Top:
        case PrCssProperty_Right:
        case PrCssProperty_Bottom:
        case PrCssProperty_Left:
        case PrCssProperty_Transform:
        case PrCssProperty_TransformOrigin:
        case PrCssProperty_Translate:
        case PrCssProperty_Rotate:
        case PrCssProperty_Scale:
        case PrCssProperty_TextIndent:
        case PrCssProperty_LetterSpacing:
        case PrCssProperty_WordSpacing:
        case PrCssProperty_OutlineOffset:
        case PrCssProperty_BaselineShift:
        case PrCssProperty_Cx:
        case PrCssProperty_Cy:
        case PrCssProperty_X:
        case PrCssProperty_Y:
        case PrCssProperty_StrokeDashoffset:
        case PrCssProperty_MaskPosition:
        case PrCssProperty_Clip:
        case PrCssProperty_ClipPath:
            return true;
        default:
            return false;
    }
}

// Returns true if two runs have the same shape: the same nodes, except for their numbers.
bool sameShape(CssValueRun a, CssValueRun b) {
    if (a.count != b.count)
        return false;
    for (uint32_t i = 0; i < a.count; ++i) {
        const CssValueNode& x = a.values[i];
        const CssValueNode& y = b.values[i];
        if (x.kind != y.kind || x.unit != y.unit || x.aux != y.aux || x.next != y.next)
            return false;
        if (cssValueHasText(x.kind)) {
            PrStringView s = cssValueText(a, &x);
            PrStringView t = cssValueText(b, &y);
            if (s.size != t.size || !memEquals(s.data, t.data, s.size))
                return false;
        }
    }
    return true;
}

// Returns true if the numbers inside `value` are terms that the property's range does not apply to: a calculation's, or
// a path()'s.
bool holdsTerms(const CssValueNode& value) {
    return value.kind == PrCssValueKind_Math || cssValueIsFunction(&value, PrCssKeyword_Path);
}

// Returns `number`, a number of a computed value of `property` that an animation combined, rounded if `value` is an
// integer and clamped to the property's range. A term of a calculation is not clamped, because the range applies to the
// calculation's result, and nor is an argument of path data, which has no range (css_path.h).
double combinedNumber(PrCssProperty property, const CssValueNode& value, double number, bool term) {
    if (value.kind == PrCssValueKind_Integer)
        number = floorF64(number + 0.5);
    if (term)
        return number;
    if (!allowsNegative(property) && number < 0)
        number = 0;
    if (property == PrCssProperty_Opacity && number > 1)
        number = 1;
    if (property == PrCssProperty_FontWeight)
        number = number < 1 ? 1 : number > 1000 ? 1000 : number;
    return number;
}

// Appends `to`, with each number interpolated between its value in `from` and its value in `to`. `from` must have the
// same shape as `to`.
void addBetween(CssValuePool* out, PrCssProperty property, CssValueRun from, CssValueRun to, double progress) {
    CssValueSpan span = cssValuePoolAddRun(out, to);
    uint32_t termEnd = 0;
    for (uint32_t i = 0; i < span.count; ++i) {
        CssValueNode& value = out->values.data[span.start + i];
        if (i >= termEnd && holdsTerms(value))
            termEnd = i + value.next;
        if (!cssValueIsNumeric(value.kind))
            continue;
        double number = lerpF64(from.values[i].number, to.values[i].number, progress);
        value.number = combinedNumber(property, value, number, i < termEnd);
    }
}

// Returns true if the value at `index` of `run` is a <length-percentage>: a length, a percentage, or a calculation of
// them.
bool isLengthPercentage(CssValueRun run, uint32_t index) {
    const CssValueNode* value = &run.values[index];
    if (value->kind == PrCssValueKind_Percentage)
        return true;
    if (value->kind == PrCssValueKind_Dimension)
        return cssUnitCategory(value->unit) == CssUnitCategory_Length;
    if (value->kind != PrCssValueKind_Math)
        return false;
    PrCssNumericType type{};
    if (!cssMathType(run, index, &type))
        return false;
    CssUnitCategory category = cssMathCategory(type);
    return category == CssUnitCategory_Length || category == CssUnitCategory_Percent;
}

// Returns true if `a` and `b` are functions of one name, whatever their arguments.
bool sameFunction(const CssValueNode& a, const CssValueNode& b) {
    return a.kind == PrCssValueKind_Function && b.kind == PrCssValueKind_Function
           && cssValueKeyword(&a) == cssValueKeyword(&b);
}

// Writes to `fromOut` and `toOut` the values of `from` and `to` in a common shape, if they differ only in that some
// <length-percentage> is a length in one and a percentage or a calculation in the other, at the top or among the
// arguments of functions of one name. Each such pair is written as a calculation of a sum of px and a percentage
// (css-values-4/#combine-math), which interpolates number by number. Returns false if the values differ in any other
// way.
bool mixedShapes(CssValueRun from, CssValueRun to, CssValuePool* fromOut, CssValuePool* toOut) {
    static constexpr PrCssUnit kUnits[] = {PrCssUnit_Px, PrCssUnit_Percent};
    uint32_t a = 0;
    uint32_t b = 0;
    bool mixed = false;
    while (a < from.count && b < to.count) {
        CssValueRun x{from.values + a, from.values[a].next, from.text};
        CssValueRun y{to.values + b, to.values[b].next, to.text};
        if (sameShape(x, y)) {
            cssValuePoolAddRun(fromOut, x);
            cssValuePoolAddRun(toOut, y);
        } else if (sameFunction(from.values[a], to.values[b])) {
            uint32_t fromAt = cssValuePoolAdd(fromOut, from.values[a]);
            uint32_t toAt = cssValuePoolAdd(toOut, to.values[b]);
            CssValueRun xArgs{x.values + 1, x.count - 1, x.text};
            CssValueRun yArgs{y.values + 1, y.count - 1, y.text};
            if (!mixedShapes(xArgs, yArgs, fromOut, toOut) && !sameShape(xArgs, yArgs))
                return false;
            fromOut->values.data[fromAt].next = fromOut->values.count - fromAt;
            toOut->values.data[toAt].next = toOut->values.count - toAt;
            mixed = true;
        } else if (isLengthPercentage(from, a) && isLengthPercentage(to, b)
                   && cssMathToSum(from, a, kUnits, 2, fromOut) && cssMathToSum(to, b, kUnits, 2, toOut)) {
            mixed = true;
        } else {
            return false;
        }
        a += from.values[a].next;
        b += to.values[b].next;
    }
    return mixed && a == from.count && b == to.count;
}

// Appends `b` with each number replaced by its sum with the number in the same place of `a`. `a` must have the same
// shape as `b`. A scale function's factors accumulate as their sum less one
// (css-transforms-2/#combining-transform-lists).
void addSum(CssValuePool* out, PrCssProperty property, CssValueRun a, CssValueRun b, bool scales) {
    CssValueSpan span = cssValuePoolAddRun(out, b);
    PrCssKeyword function = PrCssKeyword_Count;
    uint32_t functionEnd = 0;
    uint32_t termEnd = 0;
    for (uint32_t i = 0; i < span.count; ++i) {
        CssValueNode& value = out->values.data[span.start + i];
        if (i >= termEnd && holdsTerms(value))
            termEnd = i + value.next;
        if (value.kind == PrCssValueKind_Function) {
            function = cssValueKeyword(&value);
            functionEnd = i + value.next;
        } else if (i >= functionEnd) {
            function = PrCssKeyword_Count;
        }
        if (!cssValueIsNumeric(value.kind))
            continue;
        bool scale = scales && (function == PrCssKeyword_Scale || function == PrCssKeyword_Scalex
                                || function == PrCssKeyword_Scaley || function == PrCssKeyword_Scalez
                                || function == PrCssKeyword_Scale3d);
        double number = a.values[i].number + value.number - (scale ? 1 : 0);
        value.number = combinedNumber(property, value, number, i < termEnd);
    }
}

// Appends `run`, a list of values, with each calculation of a sum of px and a percentage whose px is zero written as
// its percentage, among the top-level values and the arguments of functions. A zero percentage is kept, because it
// still resolves against its basis.
void addSimplified(CssValuePool* out, CssValueRun run) {
    for (uint32_t i = 0; i < run.count; i += run.values[i].next) {
        const CssValueNode* value = &run.values[i];
        bool sum = value->kind == PrCssValueKind_Math && value->next == 4;
        const CssValueNode* px = sum ? &run.values[i + 2] : nullptr;
        sum = sum && run.values[i + 1].kind == PrCssValueKind_Calc && px->kind == PrCssValueKind_Dimension
              && px->unit == PrCssUnit_Px && px->number == 0 && run.values[i + 3].kind == PrCssValueKind_Percentage;
        if (sum) {
            cssValuePoolAdd(out, run.values[i + 3]);
        } else if (value->kind == PrCssValueKind_Function) {
            uint32_t at = cssValuePoolAdd(out, *value);
            addSimplified(out, cssValueChildren(run, i));
            out->values.data[at].next = out->values.count - at;
        } else {
            cssValuePoolAddRun(out, CssValueRun{value, value->next, run.text});
        }
    }
}

// Returns the ratio of `run`, a computed aspect-ratio: its first number divided by its second. Returns zero if `run`
// has no ratio or a degenerate one.
double ratioOf(CssValueRun run) {
    double terms[2] = {0, 0};
    uint32_t count = 0;
    for (uint32_t i = 0; i < run.count && count < 2; ++i) {
        if (cssValueIsNumeric(run.values[i].kind))
            terms[count++] = run.values[i].number;
    }
    return count == 2 && terms[0] > 0 && terms[1] > 0 && isFiniteF64(terms[0] / terms[1]) ? terms[0] / terms[1] : 0;
}

// Appends `to`, with its ratio interpolated between the ratios of `from` and `to` by the logarithm of each. `from` must
// have the same shape as `to` and hold a ratio too. The result is written as the interpolated ratio to 1
// (css-values-4/#combine-ratio).
void addRatioBetween(CssValuePool* out, CssValueRun from, CssValueRun to, double progress) {
    double ratio = expF64(lerpF64(logF64(ratioOf(from)), logF64(ratioOf(to)), progress));
    CssValueSpan span = cssValuePoolAddRun(out, to);
    uint32_t count = 0;
    for (uint32_t i = 0; i < span.count && count < 2; ++i) {
        CssValueNode& value = out->values.data[span.start + i];
        if (cssValueIsNumeric(value.kind))
            value.number = count++ == 0 ? ratio : 1;
    }
}

// Appends the identity of the functions in `list` (css-transforms-1/#interpolation-of-transforms): `list`, with the
// arguments of each function replaced by values that transform nothing.
void addIdentity(CssValuePool* out, CssValueRun list) {
    CssValueSpan span = cssValuePoolAddRun(out, list);
    for (uint32_t i = 0; i < span.count; i += out->values.data[span.start + i].next) {
        const CssValueNode& function = out->values.data[span.start + i];
        if (function.kind != PrCssValueKind_Function)
            continue;
        PrCssKeyword name = cssValueKeyword(&function);
        bool scale = name == PrCssKeyword_Scale || name == PrCssKeyword_Scalex || name == PrCssKeyword_Scaley
                     || name == PrCssKeyword_Scalez || name == PrCssKeyword_Scale3d;
        uint32_t argument = 0;
        for (uint32_t k = i + 1; k < i + function.next; ++k) {
            CssValueNode& value = out->values.data[span.start + k];
            if (!cssValueIsNumeric(value.kind))
                continue;
            // A matrix's identity is 1 0 0 1 0 0.
            bool one = scale || (name == PrCssKeyword_Matrix && (argument == 0 || argument == 3));
            value.number = one ? 1 : 0;
            ++argument;
        }
    }
}

bool isNone(CssValueRun run) {
    return run.count == 1 && cssValueIsKeyword(run.values, PrCssKeyword_None);
}

// Returns true if the transform list `run` has a matrix() or a matrix3d(), which accumulate only by decomposition
// (css-transforms-2/#combining-transform-lists).
bool hasMatrix(CssValueRun run) {
    for (uint32_t i = 0; i < run.count; i += run.values[i].next) {
        PrCssKeyword name = run.values[i].kind == PrCssValueKind_Function ? cssValueKeyword(&run.values[i])
                                                                          : PrCssKeyword_Count;
        if (name == PrCssKeyword_Matrix || name == PrCssKeyword_Matrix3d)
            return true;
    }
    return false;
}

// Writes `from` and `to` as transform lists of one length (css-transforms-1/#interpolation-of-transforms): none is the
// empty list, and the shorter list is extended by the identity functions of the longer list's remaining functions.
// Returns false if a function of one list differs in kind from the function in the same place of the other, or both
// lists are empty.
bool paddedTransforms(CssValueRun from, CssValueRun to, CssValuePool* fromOut, CssValuePool* toOut) {
    CssValueRun a = isNone(from) ? CssValueRun{from.values, 0, from.text} : from;
    CssValueRun b = isNone(to) ? CssValueRun{to.values, 0, to.text} : to;
    if (a.count == 0 && b.count == 0)
        return false;
    uint32_t i = 0;
    uint32_t j = 0;
    for (; i < a.count && j < b.count; i += a.values[i].next, j += b.values[j].next) {
        if (!sameShape(CssValueRun{a.values + i, a.values[i].next, a.text},
                       CssValueRun{b.values + j, b.values[j].next, b.text}))
            return false;
    }
    cssValuePoolAddRun(fromOut, a);
    cssValuePoolAddRun(toOut, b);
    if (i < a.count)
        addIdentity(toOut, CssValueRun{a.values + i, a.count - i, a.text});
    if (j < b.count)
        addIdentity(fromOut, CssValueRun{b.values + j, b.count - j, b.text});
    return true;
}

// How a translate's x or y is written: a length, a percentage, or a calculation of both.
enum {
    TranslateKind_Length,
    TranslateKind_Percent,
    TranslateKind_Calc,
};
typedef uint8_t TranslateKind;

// The components of a computed translate, rotate or scale (css-transforms-2/#individual-transforms), with the
// identity's for those that the value leaves out.
struct Individual {
    // translate: x, y and z in px; scale: x, y and z; rotate: the axis, then the angle in radians.
    double v[4];

    // A translate's x and y: the percentages that add to their px in `v`, and how each is written.
    double percent[2];
    TranslateKind kind[2];

    bool none;
};

bool individualOf(PrCssProperty property, CssValueRun run, Individual* out) {
    *out = Individual{};
    if (property == PrCssProperty_Scale)
        out->v[0] = out->v[1] = out->v[2] = 1;
    if (property == PrCssProperty_Rotate)
        out->v[2] = 1;
    if (run.count == 0 || isNone(run)) {
        out->none = true;
        return true;
    }
    uint32_t count = 0;
    const CssValueNode* nodes[4];
    for (uint32_t i = 0; i < run.count; i += run.values[i].next) {
        if (count == 4)
            return false;
        nodes[count++] = &run.values[i];
    }
    switch (property) {
        case PrCssProperty_Translate:
            if (count > 3)
                return false;
            for (uint32_t k = 0; k < count; ++k) {
                const CssValueNode* n = nodes[k];
                if (n->kind == PrCssValueKind_Percentage && k < 2) {
                    out->kind[k] = TranslateKind_Percent;
                    out->percent[k] = n->number;
                } else if (n->kind == PrCssValueKind_Math && k < 2) {
                    static constexpr PrCssUnit kUnits[] = {PrCssUnit_Px, PrCssUnit_Percent};
                    CssValuePool sum{};
                    bool ok = cssMathToSum(run, uint32_t(n - run.values), kUnits, 2, &sum);
                    // The sum is a Math value over a Sum of the px and the percentage, in that order.
                    if (ok) {
                        out->kind[k] = TranslateKind_Calc;
                        out->v[k] = sum.values.data[2].number;
                        out->percent[k] = sum.values.data[3].number;
                    }
                    cssValuePoolReset(&sum);
                    if (!ok)
                        return false;
                } else if (n->kind == PrCssValueKind_Dimension && cssUnitCategory(n->unit) == CssUnitCategory_Length) {
                    out->v[k] = n->number * cssUnitScale(n->unit);
                } else if ((n->kind == PrCssValueKind_Number || n->kind == PrCssValueKind_Integer) && n->number == 0) {
                    out->v[k] = 0;
                } else {
                    return false;
                }
            }
            return true;
        case PrCssProperty_Scale:
            if (count > 3)
                return false;
            for (uint32_t k = 0; k < count; ++k) {
                const CssValueNode* n = nodes[k];
                if (n->kind == PrCssValueKind_Percentage)
                    out->v[k] = n->number / 100;
                else if (n->kind == PrCssValueKind_Number || n->kind == PrCssValueKind_Integer)
                    out->v[k] = n->number;
                else
                    return false;
            }
            if (count == 1)
                out->v[1] = out->v[0];
            return true;
        case PrCssProperty_Rotate: {
            const CssValueNode* angle = nodes[count - 1];
            if (angle->kind != PrCssValueKind_Dimension || cssUnitCategory(angle->unit) != CssUnitCategory_Angle)
                return false;
            out->v[3] = angle->number * cssUnitScale(angle->unit) * kPiF64 / 180;
            if (count == 2 && nodes[0]->kind == PrCssValueKind_Keyword) {
                PrCssKeyword axis = cssValueKeyword(nodes[0]);
                out->v[0] = axis == PrCssKeyword_X ? 1 : 0;
                out->v[1] = axis == PrCssKeyword_Y ? 1 : 0;
                out->v[2] = axis == PrCssKeyword_Z ? 1 : 0;
            } else if (count == 4) {
                for (uint32_t k = 0; k < 3; ++k) {
                    if (nodes[k]->kind != PrCssValueKind_Number && nodes[k]->kind != PrCssValueKind_Integer)
                        return false;
                    out->v[k] = nodes[k]->number;
                }
            } else if (count != 1) {
                return false;
            }
            double length = sqrtF64(out->v[0] * out->v[0] + out->v[1] * out->v[1] + out->v[2] * out->v[2]);
            if (length > 0) {
                for (uint32_t k = 0; k < 3; ++k)
                    out->v[k] /= length;
            } else {
                out->v[0] = out->v[1] = 0;
                out->v[2] = 1;
            }
            return true;
        }
        default:
            return false;
    }
}

// Returns how the combination of the translations `a` and `b` writes its x or y. A zero length stands for the other
// side's kind, and two sides of different kinds combine as a calculation (css-values-4/#combine-math).
TranslateKind combinedKind(const Individual& a, const Individual& b, uint32_t k) {
    bool zeroA = a.kind[k] == TranslateKind_Length && a.v[k] == 0;
    bool zeroB = b.kind[k] == TranslateKind_Length && b.v[k] == 0;
    TranslateKind kindA = zeroA ? b.kind[k] : a.kind[k];
    TranslateKind kindB = zeroB ? kindA : b.kind[k];
    return kindA == kindB ? kindA : TranslateKind(TranslateKind_Calc);
}

// Writes the unit quaternion of a rotation by `angle` radians about the unit `axis`.
void quaternionOf(const double axis[3], double angle, double out[4]) {
    for (uint32_t k = 0; k < 3; ++k)
        out[k] = axis[k] * sinF64(angle / 2);
    out[3] = cosF64(angle / 2);
}

// Returns the rotation of the transform list `a` `b`: about their common axis if they share one, and otherwise as the
// product of their quaternions.
Individual composeRotations(const Individual& a, const Individual& b) {
    Individual m{};
    bool sameAxis = absF64(a.v[0] - b.v[0]) < 1e-6 && absF64(a.v[1] - b.v[1]) < 1e-6 && absF64(a.v[2] - b.v[2]) < 1e-6;
    if (sameAxis || a.v[3] == 0 || b.v[3] == 0) {
        const Individual& axis = a.v[3] == 0 ? b : a;
        for (uint32_t k = 0; k < 3; ++k)
            m.v[k] = axis.v[k];
        // A whole turn is no rotation, so the sum is kept within a turn. No rotation is about z.
        m.v[3] = fmodF64(a.v[3] + b.v[3], 2 * kPiF64);
        if (m.v[3] == 0) {
            m.v[0] = m.v[1] = 0;
            m.v[2] = 1;
        }
        return m;
    }
    double p[4];
    double q[4];
    quaternionOf(a.v, a.v[3], p);
    quaternionOf(b.v, b.v[3], q);
    // The Hamilton product p * q, the quaternion of the matrix product of `a` and `b` as a transform list composes
    // them.
    double r[4] = {
        p[3] * q[0] + p[0] * q[3] + p[1] * q[2] - p[2] * q[1],
        p[3] * q[1] - p[0] * q[2] + p[1] * q[3] + p[2] * q[0],
        p[3] * q[2] + p[0] * q[1] - p[1] * q[0] + p[2] * q[3],
        p[3] * q[3] - p[0] * q[0] - p[1] * q[1] - p[2] * q[2],
    };
    double length = sqrtF64(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
    for (uint32_t k = 0; k < 3; ++k)
        m.v[k] = length > 0 ? r[k] / length : k == 2 ? 1 : 0;
    m.v[3] = 2 * atan2F64(length, r[3]);
    return m;
}

// Appends a computed value of translate, rotate or scale with the components `m`, in its shortest form.
void addIndividual(CssValuePool* out, PrCssProperty property, const Individual& m) {
    auto px = [](double v) { return cssNumberValue(PrCssValueKind_Dimension, PrCssUnit_Px, v); };
    auto percentage = [](double v) { return cssNumberValue(PrCssValueKind_Percentage, PrCssUnit_Percent, v); };
    auto length = [&](uint32_t k) {
        TranslateKind kind = k < 2 ? m.kind[k] : TranslateKind_Length;
        if (kind == TranslateKind_Length) {
            cssValuePoolAdd(out, px(m.v[k]));
        } else if (kind == TranslateKind_Percent || m.v[k] == 0) {
            // A calculation with no px simplifies to its percentage, which a zero percentage does not.
            cssValuePoolAdd(out, percentage(m.percent[k]));
        } else {
            // A Math value over a Sum of the percentage and the px.
            CssValueNode math = {};
            math.kind = PrCssValueKind_Math;
            uint32_t at = cssValuePoolAdd(out, math);
            CssValueNode sum = {};
            sum.kind = PrCssValueKind_Calc;
            sum.aux = PrCssCalc_Sum;
            uint32_t operands = cssValuePoolAdd(out, sum);
            cssValuePoolAdd(out, percentage(m.percent[k]));
            cssValuePoolAdd(out, px(m.v[k]));
            out->values.data[operands].next = out->values.count - operands;
            out->values.data[at].next = out->values.count - at;
        }
    };
    auto number = [](double v) { return cssNumberValue(PrCssValueKind_Number, PrCssUnit_Number, v); };
    switch (property) {
        case PrCssProperty_Translate: {
            // The shortest form that holds the value (css-transforms-2/#individual-transform-serialization). y is left
            // out only as a length of zero, which a percentage of zero is not.
            bool y = m.v[1] != 0 || m.kind[1] != TranslateKind_Length;
            uint32_t count = m.v[2] != 0 ? 3 : y ? 2 : 1;
            for (uint32_t k = 0; k < count; ++k)
                length(k);
            break;
        }
        case PrCssProperty_Scale: {
            uint32_t count = m.v[2] != 1 ? 3 : m.v[1] != m.v[0] ? 2 : 1;
            for (uint32_t k = 0; k < count; ++k)
                cssValuePoolAdd(out, number(m.v[k]));
            break;
        }
        default: {
            double degrees = m.v[3] * 180 / kPiF64;
            if (m.v[0] == 0 && m.v[1] == 0 && m.v[2] > 0) {
                // A rotation about z is an angle alone.
            } else if (m.v[1] == 0 && m.v[2] == 0 && m.v[0] > 0) {
                cssValuePoolAdd(out, cssKeywordValue(PrCssKeyword_X));
            } else if (m.v[0] == 0 && m.v[2] == 0 && m.v[1] > 0) {
                cssValuePoolAdd(out, cssKeywordValue(PrCssKeyword_Y));
            } else {
                for (uint32_t k = 0; k < 3; ++k)
                    cssValuePoolAdd(out, number(m.v[k]));
            }
            cssValuePoolAdd(out, cssNumberValue(PrCssValueKind_Dimension, PrCssUnit_Deg, degrees));
            break;
        }
    }
}

// Interpolates two computed values of translate, rotate or scale (css-transforms-2/#individual-transforms), and appends
// the result if `out` is not null. Returns false if either value is not one that the property computes to, or both are
// none.
bool individualBetween(CssValuePool* PR_NULLABLE out, PrCssProperty property, CssValueRun from, CssValueRun to,
                       double progress) {
    Individual a{};
    Individual b{};
    if (!individualOf(property, from, &a) || !individualOf(property, to, &b))
        return false;
    if (a.none && b.none)
        return false;
    Individual m{};
    if (property == PrCssProperty_Translate) {
        for (uint32_t k = 0; k < 2; ++k) {
            m.kind[k] = combinedKind(a, b, k);
            m.percent[k] = lerpF64(a.percent[k], b.percent[k], progress);
        }
    }
    if (property == PrCssProperty_Rotate) {
        bool same = true;
        for (uint32_t k = 0; k < 3; ++k)
            same = same && absF64(a.v[k] - b.v[k]) < 1e-6;
        if (a.none || a.v[3] == 0) {
            for (uint32_t k = 0; k < 3; ++k)
                a.v[k] = b.v[k];
            same = true;
        } else if (b.none || b.v[3] == 0) {
            for (uint32_t k = 0; k < 3; ++k)
                b.v[k] = a.v[k];
            same = true;
        }
        if (same) {
            for (uint32_t k = 0; k < 3; ++k)
                m.v[k] = a.v[k];
            m.v[3] = lerpF64(a.v[3], b.v[3], progress);
        } else {
            // Slerp of the rotations' quaternions (css-transforms-2/#interpolation-of-decomposed-3d-matrix-values).
            double qa[4];
            double qb[4];
            for (uint32_t k = 0; k < 3; ++k) {
                qa[k] = a.v[k] * sinF64(a.v[3] / 2);
                qb[k] = b.v[k] * sinF64(b.v[3] / 2);
            }
            qa[3] = cosF64(a.v[3] / 2);
            qb[3] = cosF64(b.v[3] / 2);
            double product = qa[0] * qb[0] + qa[1] * qb[1] + qa[2] * qb[2] + qa[3] * qb[3];
            product = product < -1 ? -1 : product > 1 ? 1 : product;
            double q[4];
            if (absF64(product) == 1) {
                for (uint32_t k = 0; k < 4; ++k)
                    q[k] = qa[k];
            } else {
                double theta = acosF64(product);
                double w = sinF64(progress * theta) / sqrtF64(1 - product * product);
                for (uint32_t k = 0; k < 4; ++k)
                    q[k] = qa[k] * (cosF64(progress * theta) - product * w) + qb[k] * w;
            }
            double length = sqrtF64(q[0] * q[0] + q[1] * q[1] + q[2] * q[2]);
            for (uint32_t k = 0; k < 3; ++k)
                m.v[k] = length > 0 ? q[k] / length : k == 2 ? 1 : 0;
            m.v[3] = 2 * atan2F64(length, q[3]);
        }
    } else {
        for (uint32_t k = 0; k < 3; ++k)
            m.v[k] = lerpF64(a.v[k], b.v[k], progress);
    }
    if (out != nullptr)
        addIndividual(out, property, m);
    return true;
}

// Returns true if `run` is one value, with its children if it has any, as a color written with its components is.
bool isOneValue(CssValueRun run) {
    return run.count != 0 && run.values[0].next == run.count;
}

bool isIndividualTransform(PrCssProperty property) {
    return property == PrCssProperty_Translate || property == PrCssProperty_Rotate || property == PrCssProperty_Scale;
}

// An argument of a transform function or of an individual transform property: a number, an angle in radians, or a
// length in px with a percentage, as a fraction of its basis.
struct TransformArgument {
    double value;
    double fraction;
    bool keyword;                   // an axis keyword, or none
};

bool transformArgument(CssValueRun run, uint32_t index, TransformArgument* out) {
    const CssValueNode* a = &run.values[index];
    *out = TransformArgument{};
    switch (a->kind) {
        case PrCssValueKind_Number:
        case PrCssValueKind_Integer:
            out->value = a->number;
            return true;
        case PrCssValueKind_Percentage:
            out->fraction = a->number / 100;
            return true;
        case PrCssValueKind_Keyword:
            out->keyword = true;
            return true;
        case PrCssValueKind_Dimension:
            if (cssUnitCategory(a->unit) == CssUnitCategory_Angle) {
                out->value = a->number * cssUnitScale(a->unit) * kPiF64 / 180;
                return true;
            }
            return cssResolveLength(run, index, 0, &out->value);
        case PrCssValueKind_Math: {
            if (cssMathTo(run, index, PrCssUnit_Number, &out->value))
                return true;
            double degrees = 0;
            if (cssMathTo(run, index, PrCssUnit_Deg, &degrees)) {
                out->value = degrees * kPiF64 / 180;
                return true;
            }
            // A calculation of a length and a percentage: its value at a basis of zero, and how it grows with the
            // basis.
            double full = 0;
            if (!cssResolveLength(run, index, 0, &out->value) || !cssResolveLength(run, index, 100, &full))
                return false;
            out->fraction = (full - out->value) / 100;
            return true;
        }
        default:
            return false;
    }
}

// Reads the arguments of `run`, skipping commas, up to `capacity`. Returns their number, or zero if one is not an
// argument that a transform takes.
uint32_t transformArguments(CssValueRun run, TransformArgument* out, uint32_t capacity) {
    uint32_t count = 0;
    for (uint32_t k = 0; k < run.count; k += run.values[k].next) {
        if (run.values[k].kind == PrCssValueKind_Literal)
            continue;
        if (count == capacity || !transformArgument(run, k, &out[count]))
            return 0;
        ++count;
    }
    return count;
}

PrLayoutTransformFunction translation(double x, double xFraction, double y, double yFraction, double z) {
    PrLayoutTransformFunction f{.kind = PrLayoutTransformKind_Translate};
    f.values[0] = float(x);
    f.values[1] = float(y);
    f.values[2] = float(z);
    f.values[3] = float(xFraction);
    f.values[4] = float(yFraction);
    return f;
}

PrLayoutTransformFunction scaling(double x, double y, double z) {
    PrLayoutTransformFunction f{.kind = PrLayoutTransformKind_Scale};
    f.values[0] = float(x);
    f.values[1] = float(y);
    f.values[2] = float(z);
    return f;
}

PrLayoutTransformFunction rotation(double x, double y, double z, double angle) {
    PrLayoutTransformFunction f{.kind = PrLayoutTransformKind_Rotate};
    f.values[0] = float(x);
    f.values[1] = float(y);
    f.values[2] = float(z);
    f.values[3] = float(angle);
    return f;
}

// A scale factor, which may be written as a percentage.
double factorOf(const TransformArgument& a) {
    return a.value + a.fraction;
}

void add(PrLayoutTransformList* out, const PrLayoutTransformFunction& f) {
    if (out->count < kPrLayoutTransformListMaxFunctions)
        out->functions[out->count++] = f;
}

// Appends the function of a computed <transform-function>. Leaves out a function whose arguments do not match it.
void addFunction(PrLayoutTransformList* out, CssValueRun function) {
    TransformArgument v[16];
    uint32_t count = transformArguments(cssValueChildren(function, 0), v, 16);
    PrCssKeyword name = cssValueKeyword(function.values);
    // perspective(none) is the identity, and has a keyword for its argument.
    if (name == PrCssKeyword_Perspective && count == 1 && v[0].keyword) {
        add(out, PrLayoutTransformFunction{.kind = PrLayoutTransformKind_Perspective});
        return;
    }
    if (count == 0)
        return;
    for (uint32_t i = 0; i < count; ++i) {
        if (v[i].keyword)
            return;
    }
    switch (name) {
        case PrCssKeyword_Translate:
            add(out, translation(v[0].value, v[0].fraction, count >= 2 ? v[1].value : 0, count >= 2 ? v[1].fraction : 0,
                                 0));
            break;
        case PrCssKeyword_Translate3d:
            if (count == 3)
                add(out, translation(v[0].value, v[0].fraction, v[1].value, v[1].fraction, v[2].value));
            break;
        case PrCssKeyword_Translatex: add(out, translation(v[0].value, v[0].fraction, 0, 0, 0)); break;
        case PrCssKeyword_Translatey: add(out, translation(0, 0, v[0].value, v[0].fraction, 0)); break;
        case PrCssKeyword_Translatez: add(out, translation(0, 0, 0, 0, v[0].value)); break;
        case PrCssKeyword_Scale:
            add(out, scaling(factorOf(v[0]), factorOf(v[count >= 2 ? 1 : 0]), 1));
            break;
        case PrCssKeyword_Scale3d:
            if (count == 3)
                add(out, scaling(factorOf(v[0]), factorOf(v[1]), factorOf(v[2])));
            break;
        case PrCssKeyword_Scalex: add(out, scaling(factorOf(v[0]), 1, 1)); break;
        case PrCssKeyword_Scaley: add(out, scaling(1, factorOf(v[0]), 1)); break;
        case PrCssKeyword_Scalez: add(out, scaling(1, 1, factorOf(v[0]))); break;
        case PrCssKeyword_Rotate:
        case PrCssKeyword_Rotatez: add(out, rotation(0, 0, 1, v[0].value)); break;
        case PrCssKeyword_Rotatex: add(out, rotation(1, 0, 0, v[0].value)); break;
        case PrCssKeyword_Rotatey: add(out, rotation(0, 1, 0, v[0].value)); break;
        case PrCssKeyword_Rotate3d:
            if (count == 4)
                add(out, rotation(v[0].value, v[1].value, v[2].value, v[3].value));
            break;
        case PrCssKeyword_Skew:
        case PrCssKeyword_Skewx:
        case PrCssKeyword_Skewy: {
            PrLayoutTransformFunction f{.kind = PrLayoutTransformKind_Skew};
            f.values[0] = name == PrCssKeyword_Skewy ? 0 : float(v[0].value);
            f.values[1] = name == PrCssKeyword_Skewy ? float(v[0].value)
                          : name == PrCssKeyword_Skew && count >= 2 ? float(v[1].value)
                                                                    : 0;
            add(out, f);
            break;
        }
        case PrCssKeyword_Perspective: {
            // A distance below 1px is 1px (css-transforms-2/#funcdef-perspective).
            PrLayoutTransformFunction f{.kind = PrLayoutTransformKind_Perspective};
            f.values[0] = float(v[0].value < 1 ? 1 : v[0].value);
            add(out, f);
            break;
        }
        case PrCssKeyword_Matrix: {
            if (count != 6)
                break;
            PrLayoutTransformFunction f{.kind = PrLayoutTransformKind_Matrix};
            const uint32_t elements[6] = {0, 1, 4, 5, 12, 13};
            f.values[10] = f.values[15] = 1;
            for (uint32_t k = 0; k < 6; ++k)
                f.values[elements[k]] = float(v[k].value);
            add(out, f);
            break;
        }
        case PrCssKeyword_Matrix3d: {
            if (count != 16)
                break;
            PrLayoutTransformFunction f{.kind = PrLayoutTransformKind_Matrix};
            for (uint32_t k = 0; k < 16; ++k)
                f.values[k] = float(v[k].value);
            add(out, f);
            break;
        }
        default:
            break;
    }
}

// Returns the transform functions of a computed transform, which may be none.
PrLayoutTransformList transformListOf(CssValueRun transform) {
    CssValueRun empty{transform.values, 0, transform.text};
    return cssTransformFunctions(empty, empty, empty, isNone(transform) ? empty : transform);
}

// Returns true if a function of `list` translates by a fraction of the reference box, which a matrix cannot hold
// without the box.
bool hasFraction(const PrLayoutTransformList& list) {
    for (uint32_t i = 0; i < list.count; ++i) {
        const PrLayoutTransformFunction& f = list.functions[i];
        if (f.kind == PrLayoutTransformKind_Translate && (f.values[3] != 0 || f.values[4] != 0))
            return true;
    }
    return false;
}

// Returns true if the transform list `run` has a function that interpolates only as a matrix: matrix(), matrix3d() or
// perspective() (css-transforms-2/#interpolation-of-transform-functions).
bool hasMatrixLike(CssValueRun run) {
    for (uint32_t i = 0; i < run.count; i += run.values[i].next) {
        PrCssKeyword name = run.values[i].kind == PrCssValueKind_Function ? cssValueKeyword(&run.values[i])
                                                                          : PrCssKeyword_Count;
        if (name == PrCssKeyword_Matrix || name == PrCssKeyword_Matrix3d || name == PrCssKeyword_Perspective)
            return true;
    }
    return false;
}

// Appends the function `name` with `count` arguments, separated by commas.
void addFunctionValue(CssValuePool* out, PrCssKeyword name, const CssValueNode* arguments, uint32_t count) {
    CssValueNode function = {};
    function.kind = PrCssValueKind_Function;
    function.aux = name;
    uint32_t at = cssValuePoolAdd(out, function);
    for (uint32_t i = 0; i < count; ++i) {
        if (i != 0)
            cssValuePoolAdd(out, cssLiteralValue(','));
        cssValuePoolAdd(out, arguments[i]);
    }
    out->values.data[at].next = out->values.count - at;
}

// Appends the functions of `list` as the CSS functions of their primitives (css-transforms-2/#transform-primitives), or
// none for the empty list. No function of the list translates by a fraction of the reference box.
void addTransformList(CssValuePool* out, const PrLayoutTransformList& list) {
    if (list.count == 0) {
        cssValuePoolAdd(out, cssKeywordValue(PrCssKeyword_None));
        return;
    }
    auto number = [](float v) { return cssNumberValue(PrCssValueKind_Number, PrCssUnit_Number, double(v)); };
    auto px = [](float v) { return cssNumberValue(PrCssValueKind_Dimension, PrCssUnit_Px, double(v)); };
    auto degrees = [](float radians) {
        return cssNumberValue(PrCssValueKind_Dimension, PrCssUnit_Deg, double(radians) * 180 / kPiF64);
    };
    for (uint32_t i = 0; i < list.count; ++i) {
        const PrLayoutTransformFunction& f = list.functions[i];
        const float* v = f.values;
        switch (f.kind) {
            case PrLayoutTransformKind_Translate: {
                const CssValueNode arguments[] = {px(v[0]), px(v[1]), px(v[2])};
                addFunctionValue(out, PrCssKeyword_Translate3d, arguments, 3);
                break;
            }
            case PrLayoutTransformKind_Scale: {
                const CssValueNode arguments[] = {number(v[0]), number(v[1]), number(v[2])};
                addFunctionValue(out, PrCssKeyword_Scale3d, arguments, 3);
                break;
            }
            case PrLayoutTransformKind_Rotate: {
                const CssValueNode arguments[] = {number(v[0]), number(v[1]), number(v[2]), degrees(v[3])};
                addFunctionValue(out, PrCssKeyword_Rotate3d, arguments, 4);
                break;
            }
            case PrLayoutTransformKind_Skew: {
                const CssValueNode arguments[] = {degrees(v[0]), degrees(v[1])};
                addFunctionValue(out, PrCssKeyword_Skew, arguments, 2);
                break;
            }
            case PrLayoutTransformKind_Perspective: {
                const CssValueNode argument = v[0] > 0 ? px(v[0]) : cssKeywordValue(PrCssKeyword_None);
                addFunctionValue(out, PrCssKeyword_Perspective, &argument, 1);
                break;
            }
            default: {
                CssValueNode arguments[16];
                for (uint32_t k = 0; k < 16; ++k)
                    arguments[k] = number(v[k]);
                addFunctionValue(out, PrCssKeyword_Matrix3d, arguments, 16);
                break;
            }
        }
    }
}

// Appends the transform list at `progress` between the computed transforms `from` and `to`, as the engine's transform
// lists interpolate them: function by function while their primitives match, and the rest of each list as a decomposed
// matrix (css-transforms-1/#interpolation-of-transforms). Returns false, appending nothing, if a list translates by a
// fraction of the reference box, which a computed value cannot resolve, or if the lists interpolate only discretely.
bool addTransformsBetween(CssValuePool* PR_NULLABLE out, CssValueRun from, CssValueRun to, double progress) {
    PrLayoutTransformList a = transformListOf(from);
    PrLayoutTransformList b = transformListOf(to);
    if (hasFraction(a) || hasFraction(b) || !prLayoutTransformListInterpolates(&a, &b, PrVec2{0, 0}))
        return false;
    if (out != nullptr) {
        PrLayoutTransformList between = prLayoutTransformListInterpolate(&a, &b, progress, PrVec2{0, 0});
        addTransformList(out, between);
    }
    return true;
}

// The most items that the lists of a repeatable list repeat to before they combine.
constexpr uint32_t kMaxRepeatedItems = 256;

// Writes `a` and `b` to `aOut` and `bOut`, each repeated to the least common multiple of their numbers of items, and
// returns true, if `property` animates as a repeatable list and the numbers differ (web-animations-1/#repeatable-list).
// Returns false, and writes nothing, otherwise.
bool repeatedLists(PrCssProperty property, CssValueRun a, CssValueRun b, CssValuePool* aOut, CssValuePool* bOut) {
    if (!bitsetTest(kCssPropertyRepeatableList, property))
        return false;
    CssListItems x = cssListItems(a);
    CssListItems y = cssListItems(b);
    // A list that fills CssListItems may hold more items than it lists.
    if (x.count == 0 || y.count == 0 || x.count == y.count || x.count == countOf(x.starts)
        || y.count == countOf(y.starts))
        return false;
    uint32_t common = x.count;
    while (common % y.count != 0)
        common += x.count;
    if (common > kMaxRepeatedItems)
        return false;
    for (uint32_t i = 0; i < common; ++i) {
        if (i != 0) {
            cssValuePoolAdd(aOut, cssLiteralValue(','));
            cssValuePoolAdd(bOut, cssLiteralValue(','));
        }
        cssValuePoolAddRun(aOut, cssListItem(a, x, i));
        cssValuePoolAddRun(bOut, cssListItem(b, y, i));
    }
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// Sides, clips and shapes

// Returns true if `property` takes one to four values, one for each side, that repeat as the values of margin do. These
// properties are a mask border's slices, widths and outsets.
bool takesSides(PrCssProperty property) {
    return property == PrCssProperty_MaskBorderSlice || property == PrCssProperty_MaskBorderWidth
           || property == PrCssProperty_MaskBorderOutset;
}

// Returns true if `property` is a corner's radius: a horizontal radius, then a vertical one that is the horizontal one
// if it is not given (css-backgrounds-3/#border-radius).
bool isCorner(PrCssProperty property) {
    return property == PrCssProperty_BorderTopLeftRadius || property == PrCssProperty_BorderTopRightRadius
           || property == PrCssProperty_BorderBottomRightRadius || property == PrCssProperty_BorderBottomLeftRadius;
}

// Appends `run`, a corner's radius, with both of its radii.
void addBothRadii(CssValuePool* out, CssValueRun run) {
    cssValuePoolAddRun(out, run);
    if (run.count != 0 && run.values[0].next == run.count)
        cssValuePoolAddRun(out, run);
}

// Appends `run`, a corner's radius, as its computed value serializes: one radius if its two are equal.
void addFewestRadii(CssValuePool* out, CssValueRun run) {
    uint32_t second = run.count != 0 ? run.values[0].next : 0;
    if (second != 0 && second < run.count
        && cssValueRunEquals(cssValueSubtree(run, 0), cssValueSubtree(run, second)))
        cssValuePoolAddRun(out, cssValueSubtree(run, 0));
    else
        cssValuePoolAddRun(out, run);
}

// Returns true if the value at `index` of `run` is the `fill` keyword, which a mask border's slices hold apart from
// their sides.
bool isFill(CssValueRun run, uint32_t index) {
    return cssValueIsKeyword(&run.values[index], PrCssKeyword_Fill);
}

// Appends the values `items` of `run`, `count` of them from one to four, as a value for each of the four sides. The
// values repeat as the values of margin do.
void addFour(CssValuePool* out, CssValueRun run, const uint32_t items[4], uint32_t count) {
    static constexpr uint32_t kRepeat[4][4] = {{0, 0, 0, 0}, {0, 1, 0, 1}, {0, 1, 2, 1}, {0, 1, 2, 3}};
    for (uint32_t side = 0; side < 4; ++side)
        cssValuePoolAddRun(out, cssValueSubtree(run, items[kRepeat[count - 1][side]]));
}

// Returns true if the four values `a` of `run` equal the four values `b` of it, value by value.
bool sameFour(CssValueRun run, const uint32_t a[4], const uint32_t b[4]) {
    for (uint32_t side = 0; side < 4; ++side) {
        if (!cssValueRunEquals(cssValueSubtree(run, a[side]), cssValueSubtree(run, b[side])))
            return false;
    }
    return true;
}

// Appends the values `items` of `run`, a value for each of the four sides, in the fewest values that give them.
void addFewest(CssValuePool* out, CssValueRun run, const uint32_t items[4]) {
    auto same = [&](uint32_t a, uint32_t b) {
        return cssValueRunEquals(cssValueSubtree(run, items[a]), cssValueSubtree(run, items[b]));
    };
    uint32_t written = !same(1, 3) ? 4 : !same(0, 2) ? 3 : !same(0, 1) ? 2 : 1;
    for (uint32_t side = 0; side < written; ++side)
        cssValuePoolAddRun(out, cssValueSubtree(run, items[side]));
}

// Appends `run`, a value of a property that takesSides, with a value for each of the four sides, and `fill` after them.
void addFourSides(CssValuePool* out, CssValueRun run) {
    uint32_t items[4] = {};
    uint32_t count = 0;
    for (uint32_t i = 0; i < run.count; i += run.values[i].next) {
        if (!isFill(run, i) && count < 4)
            items[count++] = i;
    }
    if (count == 0) {
        cssValuePoolAddRun(out, run);
        return;
    }
    addFour(out, run, items, count);
    for (uint32_t i = 0; i < run.count; i += run.values[i].next) {
        if (isFill(run, i))
            cssValuePoolAddRun(out, cssValueSubtree(run, i));
    }
}

// Appends `run`, written with a value for each of the four sides, in the fewest values that give them.
void addFewestSides(CssValuePool* out, CssValueRun run) {
    uint32_t items[4] = {};
    uint32_t count = 0;
    for (uint32_t i = 0; i < run.count; i += run.values[i].next) {
        if (!isFill(run, i) && count < 4)
            items[count++] = i;
    }
    if (count != 4) {
        cssValuePoolAddRun(out, run);
        return;
    }
    addFewest(out, run, items);
    for (uint32_t i = 0; i < run.count; i += run.values[i].next) {
        if (isFill(run, i))
            cssValuePoolAddRun(out, cssValueSubtree(run, i));
    }
}

// The groups of an inset()'s arguments (css-shapes-1/#funcdef-basic-shape-inset): its insets, then the horizontal and
// the vertical radii of its corners, each as the indices of up to four values in the function's run.
struct InsetGroups {
    uint32_t items[3][4];
    uint32_t counts[3];
};

InsetGroups insetGroupsOf(CssValueRun inset) {
    InsetGroups groups{};
    uint32_t group = 0;
    for (uint32_t i = 1; i < inset.count; i += inset.values[i].next) {
        if (cssValueIsKeyword(&inset.values[i], PrCssKeyword_Round))
            group = 1;
        else if (cssValueIsLiteral(&inset.values[i], '/'))
            group = 2;
        else if (groups.counts[group] < 4)
            groups.items[group][groups.counts[group]++] = i;
    }
    return groups;
}

// Appends `inset`, a computed inset(), with four insets, then `round`, four horizontal radii, `/` and four vertical
// radii. Radii it does not give are 0px, and vertical radii it does not give are its horizontal ones.
void addFullInset(CssValuePool* out, CssValueRun inset) {
    InsetGroups groups = insetGroupsOf(inset);
    uint32_t at = cssValuePoolAdd(out, inset.values[0]);
    addFour(out, inset, groups.items[0], groups.counts[0]);
    cssValuePoolAdd(out, cssKeywordValue(PrCssKeyword_Round));
    for (uint32_t group = 1; group < 3; ++group) {
        if (group == 2)
            cssValuePoolAdd(out, cssLiteralValue('/'));
        uint32_t from = groups.counts[group] != 0 ? group : 1;
        if (groups.counts[from] != 0) {
            addFour(out, inset, groups.items[from], groups.counts[from]);
            continue;
        }
        for (uint32_t corner = 0; corner < 4; ++corner)
            cssValuePoolAdd(out, cssNumberValue(PrCssValueKind_Dimension, PrCssUnit_Px, 0));
    }
    out->values.data[at].next = out->values.count - at;
}

// Appends `inset`, written as addFullInset writes it, as its computed value serializes: each group in its fewest
// values, the vertical radii only if they differ from the horizontal, and no radii if every one is zero.
void addShortInset(CssValuePool* out, CssValueRun inset) {
    InsetGroups groups = insetGroupsOf(inset);
    if (groups.counts[0] != 4 || groups.counts[1] != 4 || groups.counts[2] != 4) {
        cssValuePoolAddRun(out, inset);
        return;
    }
    uint32_t at = cssValuePoolAdd(out, inset.values[0]);
    addFewest(out, inset, groups.items[0]);
    bool rounds = false;
    for (uint32_t group = 1; group < 3; ++group) {
        for (uint32_t corner = 0; corner < 4; ++corner) {
            const CssValueNode& radius = inset.values[groups.items[group][corner]];
            rounds = rounds || !cssValueIsNumeric(radius.kind) || radius.number != 0;
        }
    }
    if (rounds) {
        cssValuePoolAdd(out, cssKeywordValue(PrCssKeyword_Round));
        addFewest(out, inset, groups.items[1]);
        if (!sameFour(inset, groups.items[1], groups.items[2])) {
            cssValuePoolAdd(out, cssLiteralValue('/'));
            addFewest(out, inset, groups.items[2]);
        }
    }
    out->values.data[at].next = out->values.count - at;
}

// Appends a computed clip-path with its inset(), if it has one, written as addFullInset writes it if `full`, and as
// addShortInset writes it otherwise.
void addClipPathInsets(CssValuePool* out, CssValueRun run, bool full) {
    for (uint32_t i = 0; i < run.count; i += run.values[i].next) {
        CssValueRun item = cssValueSubtree(run, i);
        if (run.values[i].kind != PrCssValueKind_Function || cssValueKeyword(&run.values[i]) != PrCssKeyword_Inset)
            cssValuePoolAddRun(out, item);
        else if (full)
            addFullInset(out, item);
        else
            addShortInset(out, item);
    }
}

// Appends a computed clip with the commas between its edges dropped, as an interpolated rect() serializes them.
void addWithoutCommas(CssValuePool* out, CssValueRun run) {
    for (uint32_t i = 0; i < run.count; i += run.values[i].next) {
        const CssValueNode* value = &run.values[i];
        if (value->kind != PrCssValueKind_Function) {
            cssValuePoolAddRun(out, cssValueSubtree(run, i));
            continue;
        }
        uint32_t at = cssValuePoolAdd(out, *value);
        for (uint32_t k = i + 1; k < i + value->next; k += run.values[k].next) {
            if (!cssValueIsLiteral(&run.values[k], ','))
                cssValuePoolAddRun(out, cssValueSubtree(run, k));
        }
        out->values.data[at].next = out->values.count - at;
    }
}

// Returns true if `property` is combined in a form of its own (normalized), and its result is written back in the form
// of a computed value (addCombined).
bool reshapes(PrCssProperty property) {
    return takesSides(property) || isCorner(property) || property == PrCssProperty_Clip
           || property == PrCssProperty_ClipPath;
}

// Returns true if `run` is a clip-path whose circle or ellipse is sized by an extent keyword, which combines only
// discretely.
bool hasKeywordRadius(PrCssProperty property, CssValueRun run) {
    if (property != PrCssProperty_ClipPath)
        return false;
    for (uint32_t i = 0; i < run.count; i += run.values[i].next) {
        const CssValueNode* value = &run.values[i];
        PrCssKeyword name = value->kind == PrCssValueKind_Function ? cssValueKeyword(value) : PrCssKeyword_Count;
        if ((name == PrCssKeyword_Circle || name == PrCssKeyword_Ellipse) && value->next > 1
            && run.values[i + 1].kind == PrCssValueKind_Keyword
            && cssValueKeyword(&run.values[i + 1]) != PrCssKeyword_At)
            return true;
    }
    return false;
}

// Returns `run` in the form that combining reads. If that form differs from `run`, writes it to `scratch`. The form has
// a value for each side of a property that takesSides, no commas in a clip, every inset and radius of a clip path's
// inset(), and both radii of a corner.
CssValueRun normalized(PrCssProperty property, CssValueRun run, CssValuePool* scratch) {
    if (takesSides(property))
        addFourSides(scratch, run);
    else if (property == PrCssProperty_Clip)
        addWithoutCommas(scratch, run);
    else if (property == PrCssProperty_ClipPath)
        addClipPathInsets(scratch, run, true);
    else if (isCorner(property))
        addBothRadii(scratch, run);
    else
        return run;
    return cssValuePoolRun(scratch, CssValueSpan{0, scratch->values.count});
}

// Appends `run`, the result of combining, in the form of a computed value. A property that takesSides is written in its
// fewest values, and a corner's radii as one radius if they are equal. A clip path's inset() is written in its fewest
// values, and a radius of its circle() or ellipse() that went below zero is clamped to zero.
void addCombined(CssValuePool* out, PrCssProperty property, CssValueRun run) {
    if (takesSides(property)) {
        addFewestSides(out, run);
        return;
    }
    if (isCorner(property)) {
        addFewestRadii(out, run);
        return;
    }
    if (property != PrCssProperty_ClipPath) {
        cssValuePoolAddRun(out, run);
        return;
    }
    CssValueSpan span{out->values.count, 0};
    addClipPathInsets(out, run, false);
    span.count = out->values.count - span.start;
    for (uint32_t i = span.start; i < span.start + span.count; i += out->values.data[i].next) {
        const CssValueNode& value = out->values.data[i];
        PrCssKeyword name = value.kind == PrCssValueKind_Function ? cssValueKeyword(&value) : PrCssKeyword_Count;
        if (name != PrCssKeyword_Circle && name != PrCssKeyword_Ellipse)
            continue;
        for (uint32_t k = i + 1; k < i + value.next; k += out->values.data[k].next) {
            CssValueNode& radius = out->values.data[k];
            if (radius.kind == PrCssValueKind_Keyword)
                break;
            bool number = radius.kind == PrCssValueKind_Dimension || radius.kind == PrCssValueKind_Percentage;
            if (number && radius.number < 0)
                radius.number = 0;
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Filter lists

bool isFilterList(PrCssProperty property) {
    return property == PrCssProperty_Filter || property == PrCssProperty_BackdropFilter;
}

// The indices of the filter functions in the run of a computed <filter-value-list>.
struct FilterItems {
    uint32_t starts[64];
    uint32_t count;
};

// Writes the filter functions of `run` to `out`. none is the empty list. Returns false if the list holds a URL,
// anything but functions, or more than 64 functions.
bool filterItemsOf(CssValueRun run, FilterItems* out) {
    *out = FilterItems{};
    if (isNone(run))
        return true;
    for (uint32_t i = 0; i < run.count; i += run.values[i].next) {
        if (run.values[i].kind != PrCssValueKind_Function || out->count == 64
            || cssValueKeyword(&run.values[i]) == PrCssKeyword_Url)
            return false;
        out->starts[out->count++] = i;
    }
    return true;
}

// Returns true if the filter function `name` does nothing at an amount of 1. Accumulation adds the amounts of such a
// function one-based (filter-effects-1/#accumulation).
bool oneBased(PrCssKeyword name) {
    return name == PrCssKeyword_Brightness || name == PrCssKeyword_Contrast || name == PrCssKeyword_Opacity
           || name == PrCssKeyword_Saturate;
}

// Appends the computed filter function named `name` at its initial value for interpolation
// (filter-effects-1/#interpolation-of-filter-functions): an amount of 1 for a one-based function and 0 otherwise, a
// blur of 0px, a turn of 0deg, and a transparent shadow with no offset or blur.
void addInitialFilter(CssValuePool* out, PrCssKeyword name) {
    uint32_t at = cssValuePoolAdd(out, cssFunctionValue(name));
    if (name == PrCssKeyword_Blur) {
        cssValuePoolAdd(out, cssNumberValue(PrCssValueKind_Dimension, PrCssUnit_Px, 0));
    } else if (name == PrCssKeyword_HueRotate) {
        cssValuePoolAdd(out, cssNumberValue(PrCssValueKind_Dimension, PrCssUnit_Deg, 0));
    } else if (name == PrCssKeyword_DropShadow) {
        cssValuePoolAddColor(out, PrColor8{0, 0, 0, 0});
        for (uint32_t k = 0; k < 3; ++k)
            cssValuePoolAdd(out, cssNumberValue(PrCssValueKind_Dimension, PrCssUnit_Px, 0));
    } else {
        cssValuePoolAdd(out, cssNumberValue(PrCssValueKind_Number, PrCssUnit_Number, oneBased(name) ? 1 : 0));
    }
    out->values.data[at].next = out->values.count - at;
}

// Writes `from` and `to` as filter lists of one length whose functions pair by name
// (filter-effects-1/#interpolation-of-filters): none is the empty list, and the shorter list is extended by the initial
// values of the longer list's remaining functions. Returns false if either list holds a URL, the functions of a pair
// differ, or both lists are empty.
bool paddedFilters(CssValueRun from, CssValueRun to, CssValuePool* fromOut, CssValuePool* toOut) {
    FilterItems a{};
    FilterItems b{};
    if (!filterItemsOf(from, &a) || !filterItemsOf(to, &b) || (a.count == 0 && b.count == 0))
        return false;
    uint32_t common = a.count < b.count ? a.count : b.count;
    for (uint32_t i = 0; i < common; ++i) {
        if (cssValueKeyword(&from.values[a.starts[i]]) != cssValueKeyword(&to.values[b.starts[i]]))
            return false;
    }
    for (uint32_t i = 0; i < a.count; ++i)
        cssValuePoolAddRun(fromOut, cssValueSubtree(from, a.starts[i]));
    for (uint32_t i = 0; i < b.count; ++i)
        cssValuePoolAddRun(toOut, cssValueSubtree(to, b.starts[i]));
    for (uint32_t i = a.count; i < b.count; ++i)
        addInitialFilter(fromOut, cssValueKeyword(&to.values[b.starts[i]]));
    for (uint32_t i = b.count; i < a.count; ++i)
        addInitialFilter(toOut, cssValueKeyword(&from.values[a.starts[i]]));
    return true;
}

// Returns `amount`, a filter function's argument after combining, in the function's range
// (filter-effects-1/#supported-filter-functions): at least zero, and at most one for grayscale(), invert(), opacity()
// and sepia(). A turn has no range.
double filterAmount(PrCssKeyword name, double amount) {
    if (name == PrCssKeyword_HueRotate)
        return amount;
    amount = amount < 0 ? 0 : amount;
    bool clamps = name == PrCssKeyword_Grayscale || name == PrCssKeyword_Invert || name == PrCssKeyword_Opacity
                  || name == PrCssKeyword_Sepia;
    return clamps && amount > 1 ? 1 : amount;
}

// Combines one pair of filter functions of the same name, `a` and `b`, into `out`. If `sum` is false, the pair is
// interpolated at `progress`. Otherwise it is added, one-based if the function is (filter-effects-1/#accumulation).
// Colors interpolate or add as any color does, resolved in `colors`. Returns false, appending nothing, if the arguments
// of the pair are not of one shape.
bool combineFilter(CssValuePool* out, CssValueRun a, CssValueRun b, double progress, bool sum,
                   const CssColorContext& colors) {
    PrCssKeyword name = cssValueKeyword(a.values);
    uint32_t mark = out->values.count;
    uint32_t at = cssValuePoolAdd(out, b.values[0]);
    uint32_t i = 1;
    uint32_t k = 1;
    uint32_t argument = 0;
    for (; i < a.count && k < b.count; i += a.values[i].next, k += b.values[k].next, ++argument) {
        CssValueRun x = cssValueSubtree(a, i);
        CssValueRun y = cssValueSubtree(b, k);
        CssColor cx{};
        CssColor cy{};
        if (cssResolveColorComponents(x, 0, colors, &cx) && cssResolveColorComponents(y, 0, colors, &cy)) {
            bool legacy = cssColorsAreLegacy(x.values, 0, x.count) && cssColorsAreLegacy(y.values, 0, y.count);
            PrColorSpace space = legacy ? PrColorSpace_Srgb : PrColorSpace_Oklab;
            CssColor mixed{};
            if (sum) {
                // Colors add premultiplied, with their alpha clamped to [0, 1].
                CssColor p = cssColorConvert(cx, space);
                CssColor q = cssColorConvert(cy, space);
                double alpha = p.alpha + q.alpha > 1 ? 1 : p.alpha + q.alpha;
                mixed = CssColor{space, {}, alpha, 0};
                for (uint32_t c = 0; c < 3; ++c)
                    mixed.c[c] = alpha > 0 ? (p.c[c] * p.alpha + q.c[c] * q.alpha) / alpha : 0;
            } else {
                mixed = cssColorInterpolate(cx, cy, progress, space, PrColorHueMethod_Shorter);
                mixed.alpha = mixed.alpha < 0 ? 0 : mixed.alpha > 1 ? 1 : mixed.alpha;
            }
            if (legacy)
                cssValuePoolAddLegacyColor(out, mixed);
            else
                cssValuePoolAddColorComponents(out, mixed);
            continue;
        }
        if (x.count != 1 || y.count != 1 || x.values[0].kind != y.values[0].kind
            || x.values[0].unit != y.values[0].unit || !cssValueIsNumeric(x.values[0].kind)) {
            out->values.count = mark;
            return false;
        }
        double p = x.values[0].number;
        double q = y.values[0].number;
        double value = !sum ? lerpF64(p, q, progress) : oneBased(name) ? p + q - 1 : p + q;
        // drop-shadow()'s offsets have no range, and its blur is never negative.
        bool offset = name == PrCssKeyword_DropShadow && argument < 3;
        CssValueNode node = y.values[0];
        node.number = offset ? value : filterAmount(name, value);
        cssValuePoolAdd(out, node);
    }
    if (i < a.count || k < b.count) {
        out->values.count = mark;
        return false;
    }
    out->values.data[at].next = out->values.count - at;
    return true;
}

// Combines two filter lists of one length whose functions pair by name, function by function, into `out`. Returns
// false, appending nothing, if a pair does not combine.
bool combineFilters(CssValuePool* out, CssValueRun a, CssValueRun b, double progress, bool sum,
                    const CssColorContext& colors) {
    uint32_t mark = out->values.count;
    for (uint32_t i = 0, k = 0; i < a.count && k < b.count; i += a.values[i].next, k += b.values[k].next) {
        if (!combineFilter(out, cssValueSubtree(a, i), cssValueSubtree(b, k), progress, sum, colors)) {
            out->values.count = mark;
            return false;
        }
    }
    if (out->values.count == mark)
        cssValuePoolAdd(out, cssKeywordValue(PrCssKeyword_None));
    return true;
}

// Appends the filter list at `progress` between `from` and `to` (filter-effects-1/#interpolation-of-filters), or
// returns false if the lists interpolate only discretely, appending nothing.
bool addFiltersBetween(CssValuePool* PR_NULLABLE out, CssValueRun from, CssValueRun to, double progress,
                       const CssColorContext& colors) {
    CssValuePool a{};
    CssValuePool b{};
    CssValuePool scratch{};
    bool paired = paddedFilters(from, to, &a, &b);
    bool combined = paired
                    && combineFilters(out != nullptr ? out : &scratch,
                                      cssValuePoolRun(&a, CssValueSpan{0, a.values.count}),
                                      cssValuePoolRun(&b, CssValueSpan{0, b.values.count}), progress, false, colors);
    cssValuePoolReset(&a);
    cssValuePoolReset(&b);
    cssValuePoolReset(&scratch);
    return combined;
}

// Appends `value` combined with `underlying` (filter-effects-1/#addition, filter-effects-1/#accumulation). Addition
// appends the value's list to the underlying list. Accumulation combines the lists function by function if they pair as
// interpolation pairs them, and otherwise gives the value, as replace does.
void addFiltersComposite(CssValuePool* out, CssValueRun underlying, CssValueRun value, bool accumulate,
                         const CssColorContext& colors) {
    if (!accumulate) {
        if (isNone(underlying) || isNone(value)) {
            cssValuePoolAddRun(out, isNone(value) ? underlying : value);
            return;
        }
        cssValuePoolAddRun(out, underlying);
        cssValuePoolAddRun(out, value);
        return;
    }
    CssValuePool a{};
    CssValuePool b{};
    bool combined = paddedFilters(underlying, value, &a, &b)
                    && combineFilters(out, cssValuePoolRun(&a, CssValueSpan{0, a.values.count}),
                                      cssValuePoolRun(&b, CssValueSpan{0, b.values.count}), 0, true, colors);
    if (!combined)
        cssValuePoolAddRun(out, value);
    cssValuePoolReset(&a);
    cssValuePoolReset(&b);
}

// ---------------------------------------------------------------------------------------------------------------------
// Images

// Returns true if `property` takes a list of <image>s, or one, whose filter() images interpolate.
bool isImageList(PrCssProperty property) {
    return property == PrCssProperty_BackgroundImage || property == PrCssProperty_MaskImage
           || property == PrCssProperty_ListStyleImage || property == PrCssProperty_BorderImageSource
           || property == PrCssProperty_MaskBorderSource;
}

// The parts of a computed filter() (filter-effects-1/#FilterCSSImageValue): its input and its filter list.
struct FilterImage {
    CssValueRun input;
    CssValueRun filters;
};

// Writes the parts of `image` to `out` if it is a filter() and nothing else. Returns false otherwise.
bool filterImageOf(CssValueRun image, FilterImage* out) {
    if (image.count == 0 || image.values[0].next != image.count
        || !cssValueIsFunction(image.values, PrCssKeyword_Filter))
        return false;
    uint32_t comma = 1;
    while (comma < image.count && !cssValueIsLiteral(&image.values[comma], ','))
        comma += image.values[comma].next;
    if (comma == image.count)
        return false;
    out->input = CssValueRun{image.values + 1, comma - 1, image.text};
    out->filters = CssValueRun{image.values + comma + 1, image.count - comma - 1, image.text};
    return true;
}

// Appends the image list at `progress` between `from` and `to`, or returns false if the lists interpolate only
// discretely, appending nothing. Two filter() images of the same input interpolate by their filter lists
// (filter-effects-1/#interpolating-filter-image). The lists must be of one length, and each other pair of images must
// be equal. Any other pair is a generic image, which interpolates as cross-fade() would
// (css-images-4/#interpolating-images), and the engine draws no cross-fade(), so the pair is discrete.
bool addImagesBetween(CssValuePool* PR_NULLABLE out, CssValueRun from, CssValueRun to, double progress,
                      const CssColorContext& colors) {
    CssListItems a = cssListItems(from);
    CssListItems b = cssListItems(to);
    if (a.count != b.count || a.count == 0)
        return false;
    bool filtered = false;
    for (uint32_t i = 0; i < a.count; ++i) {
        CssValueRun x = cssListItem(from, a, i);
        CssValueRun y = cssListItem(to, b, i);
        if (cssValueRunEquals(x, y))
            continue;
        FilterImage p{};
        FilterImage q{};
        if (!filterImageOf(x, &p) || !filterImageOf(y, &q) || !cssValueRunEquals(p.input, q.input)
            || !addFiltersBetween(nullptr, p.filters, q.filters, 0, colors))
            return false;
        filtered = true;
    }
    if (!filtered || out == nullptr)
        return filtered;
    for (uint32_t i = 0; i < a.count; ++i) {
        if (i != 0)
            cssValuePoolAdd(out, cssLiteralValue(','));
        CssValueRun x = cssListItem(from, a, i);
        CssValueRun y = cssListItem(to, b, i);
        FilterImage p{};
        FilterImage q{};
        if (cssValueRunEquals(x, y) || !filterImageOf(x, &p) || !filterImageOf(y, &q)) {
            cssValuePoolAddRun(out, x);
            continue;
        }
        uint32_t at = cssValuePoolAdd(out, x.values[0]);
        cssValuePoolAddRun(out, p.input);
        cssValuePoolAdd(out, cssLiteralValue(','));
        addFiltersBetween(out, p.filters, q.filters, progress, colors);
        out->values.data[at].next = out->values.count - at;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// Track lists

// Returns true if `property` takes a track list, which combines track by track (css-grid-1/#track-sizing).
bool isTrackList(PrCssProperty property) {
    return property == PrCssProperty_GridTemplateColumns || property == PrCssProperty_GridTemplateRows;
}

// The items of a computed track list, or of the list that a repeat() repeats: its tracks and repeat()s, and the
// <line-names> before each and after the last. A line with no names has an empty run.
struct TrackItems {
    static constexpr uint32_t kCapacity = 64;
    CssValueRun names[kCapacity + 1];
    CssValueRun tracks[kCapacity];
    uint32_t count;
};

// Writes the items of `run` to `out`. Returns false if `run` is not a list of tracks (none, or subgrid) or holds more
// items than `out` can.
bool trackItemsOf(CssValueRun run, TrackItems* out) {
    out->count = 0;
    out->names[0] = CssValueRun{run.values, 0, run.text};
    for (uint32_t i = 0; i < run.count;) {
        const CssValueNode* value = &run.values[i];
        if (cssValueIsLiteral(value, '[')) {
            uint32_t end = i;
            while (end < run.count && !cssValueIsLiteral(&run.values[end], ']'))
                end += run.values[end].next;
            end = end < run.count ? end + run.values[end].next : end;
            out->names[out->count] = CssValueRun{value, end - i, run.text};
            i = end;
            continue;
        }
        if (cssValueIsKeyword(value, PrCssKeyword_None) || cssValueIsKeyword(value, PrCssKeyword_Subgrid)
            || out->count == TrackItems::kCapacity)
            return false;
        out->tracks[out->count++] = CssValueRun{value, value->next, run.text};
        i += value->next;
        out->names[out->count] = CssValueRun{run.values + i, 0, run.text};
    }
    return out->count != 0;
}

// The parts of a computed repeat(): its count with the comma after it, and the list that it repeats.
struct Repetition {
    CssValueRun count;
    CssValueRun list;
};

Repetition repetitionOf(CssValueRun track) {
    CssValueRun args = cssValueChildren(track, 0);
    uint32_t list = args.count != 0 ? args.values[0].next : 0;
    if (list < args.count && cssValueIsLiteral(&args.values[list], ','))
        list += args.values[list].next;
    return Repetition{.count = CssValueRun{args.values, list, args.text},
                      .list = CssValueRun{args.values + list, args.count - list, args.text}};
}

// Returns true if the items `a` and `b` combine item by item (css-grid-1/#repeat-interpolation): there are as many of
// each, and each pair is two tracks, or two repeat()s of one count whose lists combine item by item. A pair of tracks
// that does not combine is a discrete step of its own.
bool itemsCombine(const TrackItems& a, const TrackItems& b) {
    if (a.count != b.count)
        return false;
    for (uint32_t k = 0; k < a.count; ++k) {
        bool repeats = cssValueIsFunction(a.tracks[k].values, PrCssKeyword_Repeat);
        if (repeats != cssValueIsFunction(b.tracks[k].values, PrCssKeyword_Repeat))
            return false;
        if (!repeats)
            continue;
        Repetition p = repetitionOf(a.tracks[k]);
        Repetition q = repetitionOf(b.tracks[k]);
        TrackItems x;
        TrackItems y;
        if (!cssValueRunEquals(p.count, q.count) || !trackItemsOf(p.list, &x) || !trackItemsOf(q.list, &y)
            || !itemsCombine(x, y))
            return false;
    }
    return true;
}

// Writes the items of the track lists `a` and `b` to `aOut` and `bOut`, and returns true if they combine item by item.
bool trackListsCombine(CssValueRun a, CssValueRun b, TrackItems* aOut, TrackItems* bOut) {
    return trackItemsOf(a, aOut) && trackItemsOf(b, bOut) && itemsCombine(*aOut, *bOut);
}

// How two track lists combine: interpolated at `progress`, or added. A pair that does not combine gives `first`'s side
// of it, and the names of each line come from that side too.
struct TrackOp {
    PrCssProperty property;
    double progress;
    bool add;
    bool first;
};

// Appends the combination of the track breadths `a` and `b`: two of one shape number by number, and two
// <length-percentage>s as sums of px and a percentage (css-values-4/#combine-math).
void addBreadths(CssValuePool* out, const TrackOp& op, CssValueRun a, CssValueRun b) {
    static constexpr PrCssUnit kUnits[] = {PrCssUnit_Px, PrCssUnit_Percent};
    auto combined = [&](CssValuePool* to, CssValueRun x, CssValueRun y) {
        if (op.add)
            addSum(to, op.property, x, y, false);
        else
            addBetween(to, op.property, x, y, op.progress);
    };
    if (sameShape(a, b)) {
        combined(out, a, b);
        return;
    }
    CssValuePool x{};
    CssValuePool y{};
    CssValuePool mixed{};
    if (isLengthPercentage(a, 0) && isLengthPercentage(b, 0) && cssMathToSum(a, 0, kUnits, 2, &x)
        && cssMathToSum(b, 0, kUnits, 2, &y)) {
        combined(&mixed, cssValuePoolRun(&x, CssValueSpan{0, x.values.count}),
                 cssValuePoolRun(&y, CssValueSpan{0, y.values.count}));
        addSimplified(out, cssValuePoolRun(&mixed, CssValueSpan{0, mixed.values.count}));
    } else {
        cssValuePoolAddRun(out, op.first ? a : b);
    }
    cssValuePoolReset(&x);
    cssValuePoolReset(&y);
    cssValuePoolReset(&mixed);
}

// Appends the combination of the tracks `a` and `b`, neither a repeat(): two breadths, or two minmax()es or
// fit-content()s argument by argument. Tracks of different forms do not combine.
void addTracks(CssValuePool* out, const TrackOp& op, CssValueRun a, CssValueRun b) {
    bool functions = a.values[0].kind == PrCssValueKind_Function || b.values[0].kind == PrCssValueKind_Function;
    if (!functions) {
        addBreadths(out, op, a, b);
        return;
    }
    CssValueRun x = cssValueChildren(a, 0);
    CssValueRun y = cssValueChildren(b, 0);
    auto arguments = [](CssValueRun run) {
        uint32_t count = 0;
        for (uint32_t i = 0; i < run.count; i += run.values[i].next)
            ++count;
        return count;
    };
    if (!sameFunction(a.values[0], b.values[0]) || arguments(x) != arguments(y)) {
        cssValuePoolAddRun(out, op.first ? a : b);
        return;
    }
    uint32_t at = cssValuePoolAdd(out, (op.first ? a : b).values[0]);
    for (uint32_t i = 0, j = 0; i < x.count && j < y.count; i += x.values[i].next, j += y.values[j].next) {
        CssValueRun p = cssValueSubtree(x, i);
        CssValueRun q = cssValueSubtree(y, j);
        if (x.values[i].kind == PrCssValueKind_Literal || y.values[j].kind == PrCssValueKind_Literal)
            cssValuePoolAddRun(out, op.first ? p : q);
        else
            addBreadths(out, op, p, q);
    }
    out->values.data[at].next = out->values.count - at;
}

// Appends the combination of the items `a` and `b`, which combine item by item.
void addTrackItems(CssValuePool* out, const TrackOp& op, const TrackItems& a, const TrackItems& b) {
    const TrackItems& named = op.first ? a : b;
    for (uint32_t k = 0; k < a.count; ++k) {
        cssValuePoolAddRun(out, named.names[k]);
        if (!cssValueIsFunction(a.tracks[k].values, PrCssKeyword_Repeat)) {
            addTracks(out, op, a.tracks[k], b.tracks[k]);
            continue;
        }
        Repetition p = repetitionOf(a.tracks[k]);
        Repetition q = repetitionOf(b.tracks[k]);
        TrackItems x;
        TrackItems y;
        trackItemsOf(p.list, &x);
        trackItemsOf(q.list, &y);
        uint32_t at = cssValuePoolAdd(out, named.tracks[k].values[0]);
        cssValuePoolAddRun(out, p.count);
        addTrackItems(out, op, x, y);
        out->values.data[at].next = out->values.count - at;
    }
    cssValuePoolAddRun(out, named.names[a.count]);
}

bool interpolable(PrCssProperty property, CssValueRun from, CssValueRun to, const CssColorContext& colors) {
    if (cssValueRunEquals(from, to))
        return false;
    if (property == PrCssProperty_Visibility)
        return (from.count == 1 && cssValueIsKeyword(from.values, PrCssKeyword_Visible))
               || (to.count == 1 && cssValueIsKeyword(to.values, PrCssKeyword_Visible));
    if (bitsetTest(kCssPropertyDiscrete, property))
        return false;
    if (cssIsCornerShape(property))
        return true;
    PrColor a{};
    PrColor b{};
    if (isOneValue(from) && isOneValue(to) && cssResolveColor(from, 0, colors, &a)
        && cssResolveColor(to, 0, colors, &b))
        return true;
    if (property == PrCssProperty_AspectRatio)
        return sameShape(from, to) && ratioOf(from) > 0 && ratioOf(to) > 0;
    if (isIndividualTransform(property))
        return individualBetween(nullptr, property, from, to, 0);
    if (sameShape(from, to))
        return true;
    CssValuePool x{};
    CssValuePool y{};
    if (repeatedLists(property, from, to, &x, &y)) {
        bool repeated = interpolable(property, cssValuePoolRun(&x, CssValueSpan{0, x.values.count}),
                                     cssValuePoolRun(&y, CssValueSpan{0, y.values.count}), colors);
        cssValuePoolReset(&x);
        cssValuePoolReset(&y);
        return repeated;
    }
    bool shaped = property == PrCssProperty_Transform
                      ? paddedTransforms(from, to, &x, &y) || addTransformsBetween(nullptr, from, to, 0)
                      : mixedShapes(from, to, &x, &y);
    cssValuePoolReset(&x);
    cssValuePoolReset(&y);
    return shaped;
}

void interpolate(CssValuePool* out, PrCssProperty property, CssValueRun from, CssValueRun to, double progress,
                 const CssColorContext& colors) {
    // Visibility is visible between two values if either of them is visible (web-animations-1/#animating-visibility).
    if (property == PrCssProperty_Visibility) {
        bool fromVisible = from.count == 1 && cssValueIsKeyword(from.values, PrCssKeyword_Visible);
        bool toVisible = to.count == 1 && cssValueIsKeyword(to.values, PrCssKeyword_Visible);
        if ((fromVisible || toVisible) && progress > 0 && progress < 1) {
            cssValuePoolAddRun(out, fromVisible ? from : to);
            return;
        }
    }
    if (bitsetTest(kCssPropertyDiscrete, property)) {
        cssValuePoolAddRun(out, progress < 0.5 ? from : to);
        return;
    }
    CssColor a{};
    CssColor b{};
    if (isOneValue(from) && isOneValue(to) && cssResolveColorComponents(from, 0, colors, &a)
        && cssResolveColorComponents(to, 0, colors, &b)
        && (cssIsColorProperty(property) || from.values[0].kind == PrCssValueKind_Color
            || to.values[0].kind == PrCssValueKind_Color)) {
        // Two legacy colors interpolate in sRGB and stay legacy, and any others in Oklab
        // (css-color-4/#interpolation-space). The result keeps its precision and any component outside its gamut, which
        // serialization rounds and clamps. An extrapolated alpha is clamped to [0, 1].
        bool legacy = cssColorsAreLegacy(from.values, 0, from.count) && cssColorsAreLegacy(to.values, 0, to.count);
        CssColor mixed = cssColorInterpolate(a, b, progress, legacy ? PrColorSpace_Srgb : PrColorSpace_Oklab,
                                             PrColorHueMethod_Shorter);
        mixed.alpha = mixed.alpha < 0 ? 0 : mixed.alpha > 1 ? 1 : mixed.alpha;
        // oklab() clamps its lightness to [0, 1], which is right for a mix that extrapolates past its colors. A mix
        // between colors brighter than SDR white, which resolved for an HDR display, keeps its range in XYZ instead.
        constexpr double kSdrLightness = 1 + 1e-6;
        if (!legacy && (cssColorConvert(a, PrColorSpace_Oklab).c[0] > kSdrLightness
                        || cssColorConvert(b, PrColorSpace_Oklab).c[0] > kSdrLightness))
            mixed = cssColorConvert(mixed, PrColorSpace_XyzD65);
        if (legacy)
            cssValuePoolAddLegacyColor(out, mixed);
        else
            cssValuePoolAddColorComponents(out, mixed);
        return;
    }
    bool matrixLike = property == PrCssProperty_Transform && (hasMatrixLike(from) || hasMatrixLike(to));
    if (property == PrCssProperty_Transform && (!sameShape(from, to) || matrixLike)) {
        // Two lists are themselves at either end, while none is its identity functions.
        if ((progress == 0 || progress == 1) && !isNone(from) && !isNone(to)) {
            cssValuePoolAddRun(out, progress == 0 ? from : to);
            return;
        }
        // Lists whose functions pair by name interpolate their arguments, and any others as the engine's transform
        // lists do.
        CssValuePool x{};
        CssValuePool y{};
        if (!matrixLike && paddedTransforms(from, to, &x, &y))
            addBetween(out, property, cssValuePoolRun(&x, CssValueSpan{0, x.values.count}),
                       cssValuePoolRun(&y, CssValueSpan{0, y.values.count}), progress);
        else if (!addTransformsBetween(out, from, to, progress))
            cssValuePoolAddRun(out, progress < 0.5 ? from : to);
        cssValuePoolReset(&x);
        cssValuePoolReset(&y);
        return;
    }
    if (isIndividualTransform(property)) {
        if (!individualBetween(out, property, from, to, progress))
            cssValuePoolAddRun(out, progress < 0.5 ? from : to);
        return;
    }
    if (property == PrCssProperty_AspectRatio) {
        if (sameShape(from, to) && ratioOf(from) > 0 && ratioOf(to) > 0)
            addRatioBetween(out, from, to, progress);
        else
            cssValuePoolAddRun(out, progress < 0.5 ? from : to);
        return;
    }
    CssValuePool repeatedFrom{};
    CssValuePool repeatedTo{};
    if (repeatedLists(property, from, to, &repeatedFrom, &repeatedTo)) {
        // Lists whose items do not all interpolate switch discretely between the values as they are written.
        CssValueRun f = cssValuePoolRun(&repeatedFrom, CssValueSpan{0, repeatedFrom.values.count});
        CssValueRun t = cssValuePoolRun(&repeatedTo, CssValueSpan{0, repeatedTo.values.count});
        if (interpolable(property, f, t, colors))
            interpolate(out, property, f, t, progress, colors);
        else
            cssValuePoolAddRun(out, progress < 0.5 ? from : to);
        cssValuePoolReset(&repeatedFrom);
        cssValuePoolReset(&repeatedTo);
        return;
    }
    if (sameShape(from, to)) {
        addBetween(out, property, from, to, progress);
        return;
    }
    CssValuePool x{};
    CssValuePool y{};
    CssValuePool mixed{};
    if (mixedShapes(from, to, &x, &y)) {
        addBetween(&mixed, property, cssValuePoolRun(&x, CssValueSpan{0, x.values.count}),
                   cssValuePoolRun(&y, CssValueSpan{0, y.values.count}), progress);
        addSimplified(out, cssValuePoolRun(&mixed, CssValueSpan{0, mixed.values.count}));
    } else {
        cssValuePoolAddRun(out, progress < 0.5 ? from : to);
    }
    cssValuePoolReset(&x);
    cssValuePoolReset(&y);
    cssValuePoolReset(&mixed);
}

void combine(CssValuePool* out, PrCssProperty property, CssValueRun underlying, CssValueRun value,
             CssComposite composite, const CssColorContext& colors) {
    // A dash array does not add, so it composites as it replaces (SVG2/painting.html#StrokeDashing).
    if (composite == CssComposite_Replace || property == PrCssProperty_Visibility
        || property == PrCssProperty_StrokeDasharray || bitsetTest(kCssPropertyDiscrete, property)) {
        cssValuePoolAddRun(out, value);
        return;
    }
    bool accumulate = composite == CssComposite_Accumulate;
    // Colors add premultiplied, with their alpha clamped to [0, 1].
    CssColor a{};
    CssColor b{};
    if (isOneValue(underlying) && isOneValue(value) && cssResolveColorComponents(underlying, 0, colors, &a)
        && cssResolveColorComponents(value, 0, colors, &b)) {
        bool legacy = cssColorsAreLegacy(underlying.values, 0, underlying.count)
                      && cssColorsAreLegacy(value.values, 0, value.count);
        PrColorSpace space = legacy ? PrColorSpace_Srgb : PrColorSpace_Oklab;
        CssColor x = cssColorConvert(a, space);
        CssColor y = cssColorConvert(b, space);
        double ax = (x.missing & 8u) != 0 ? 0 : x.alpha;
        double ay = (y.missing & 8u) != 0 ? 0 : y.alpha;
        double alpha = ax + ay;
        alpha = alpha > 1 ? 1 : alpha;
        CssColor sum{space, {}, alpha, 0};
        for (uint32_t i = 0; i < 3; ++i) {
            double cx = (x.missing & (1u << i)) != 0 ? 0 : x.c[i];
            double cy = (y.missing & (1u << i)) != 0 ? 0 : y.c[i];
            double premultiplied = cx * ax + cy * ay;
            sum.c[i] = alpha > 0 ? premultiplied / alpha : 0;
        }
        if (legacy)
            cssValuePoolAddLegacyColor(out, sum);
        else
            cssValuePoolAddColorComponents(out, sum);
        return;
    }
    if (isIndividualTransform(property)) {
        Individual u{};
        Individual v{};
        if (!individualOf(property, underlying, &u) || !individualOf(property, value, &v) || u.none) {
            cssValuePoolAddRun(out, value);
            return;
        }
        if (v.none) {
            cssValuePoolAddRun(out, underlying);
            return;
        }
        Individual m{};
        if (property == PrCssProperty_Translate) {
            for (uint32_t k = 0; k < 3; ++k)
                m.v[k] = u.v[k] + v.v[k];
            for (uint32_t k = 0; k < 2; ++k) {
                m.kind[k] = combinedKind(u, v, k);
                m.percent[k] = u.percent[k] + v.percent[k];
            }
        } else if (property == PrCssProperty_Scale) {
            for (uint32_t k = 0; k < 3; ++k)
                m.v[k] = accumulate ? u.v[k] + v.v[k] - 1 : u.v[k] * v.v[k];
        } else {
            m = composeRotations(u, v);
        }
        addIndividual(out, property, m);
        return;
    }
    if (property == PrCssProperty_Transform) {
        if (isNone(underlying) || isNone(value)) {
            cssValuePoolAddRun(out, isNone(value) ? underlying : value);
        } else if (!accumulate) {
            // The underlying list followed by the value's (css-transforms-2/#combining-transform-lists).
            cssValuePoolAddRun(out, underlying);
            cssValuePoolAddRun(out, value);
        } else if (sameShape(underlying, value) && !hasMatrix(value)) {
            addSum(out, property, underlying, value, true);
        } else {
            cssValuePoolAddRun(out, value);
        }
        return;
    }
    CssValuePool repeatedUnderlying{};
    CssValuePool repeatedValue{};
    if (repeatedLists(property, underlying, value, &repeatedUnderlying, &repeatedValue)) {
        // Lists whose items do not all add give the value, as it is written.
        CssValueRun u = cssValuePoolRun(&repeatedUnderlying, CssValueSpan{0, repeatedUnderlying.values.count});
        CssValueRun v = cssValuePoolRun(&repeatedValue, CssValueSpan{0, repeatedValue.values.count});
        CssValuePool x{};
        CssValuePool y{};
        if (sameShape(u, v) || mixedShapes(u, v, &x, &y))
            combine(out, property, u, v, composite, colors);
        else
            cssValuePoolAddRun(out, value);
        cssValuePoolReset(&x);
        cssValuePoolReset(&y);
        cssValuePoolReset(&repeatedUnderlying);
        cssValuePoolReset(&repeatedValue);
        return;
    }
    if (sameShape(underlying, value)) {
        addSum(out, property, underlying, value, false);
        return;
    }
    CssValuePool x{};
    CssValuePool y{};
    CssValuePool mixed{};
    if (mixedShapes(underlying, value, &x, &y)) {
        addSum(&mixed, property, cssValuePoolRun(&x, CssValueSpan{0, x.values.count}),
               cssValuePoolRun(&y, CssValueSpan{0, y.values.count}), false);
        addSimplified(out, cssValuePoolRun(&mixed, CssValueSpan{0, mixed.values.count}));
    } else {
        cssValuePoolAddRun(out, value);
    }
    cssValuePoolReset(&x);
    cssValuePoolReset(&y);
    cssValuePoolReset(&mixed);
}

} // namespace

CssListItems cssListItems(CssValueRun run) {
    CssListItems items{};
    if (run.count == 0)
        return items;
    items.starts[items.count++] = 0;
    for (uint32_t i = 0; i < run.count; i += run.values[i].next) {
        if (cssValueIsLiteral(&run.values[i], ',') && items.count < countOf(items.starts))
            items.starts[items.count++] = i + 1;
    }
    return items;
}

CssValueRun cssListItem(CssValueRun run, const CssListItems& items, uint32_t index) {
    if (items.count == 0)
        return CssValueRun{run.values, 0, run.text};
    uint32_t k = index % items.count;
    uint32_t start = items.starts[k];
    uint32_t end = k + 1 < items.count ? items.starts[k + 1] - 1 : run.count;
    return CssValueRun{run.values + start, end - start, run.text};
}

double cssTimeMilliseconds(CssValueRun run, double fallback) {
    if (run.count != 1 || run.values[0].kind != PrCssValueKind_Dimension)
        return fallback;
    if (run.values[0].unit == PrCssUnit_S)
        return run.values[0].number * 1000;
    if (run.values[0].unit == PrCssUnit_Ms)
        return run.values[0].number;
    return fallback;
}

PrLayoutEasing cssEasing(CssValueRun easing) {
    PrLayoutEasing out = prLayoutEasingLinear();
    auto curve = [&](double x1, double y1, double x2, double y2) {
        out.kind = PrLayoutEasingKind_CubicBezier;
        out.x1 = x1;
        out.y1 = y1;
        out.x2 = x2;
        out.y2 = y2;
    };
    auto steps = [&](uint32_t count, PrLayoutStepPosition position) {
        out.kind = PrLayoutEasingKind_Steps;
        out.steps = count;
        out.position = position;
    };
    if (easing.count == 0)
        return out;
    const CssValueNode* value = easing.values;
    if (value->kind == PrCssValueKind_Keyword) {
        switch (cssValueKeyword(value)) {
            case PrCssKeyword_Ease: curve(0.25, 0.1, 0.25, 1); break;
            case PrCssKeyword_EaseIn: curve(0.42, 0, 1, 1); break;
            case PrCssKeyword_EaseOut: curve(0, 0, 0.58, 1); break;
            case PrCssKeyword_EaseInOut: curve(0.42, 0, 0.58, 1); break;
            case PrCssKeyword_StepStart: steps(1, PrLayoutStepPosition_JumpStart); break;
            case PrCssKeyword_StepEnd: steps(1, PrLayoutStepPosition_JumpEnd); break;
            default: break;
        }
        return out;
    }
    if (value->kind != PrCssValueKind_Function)
        return out;
    double numbers[4] = {};
    PrCssKeyword keyword = PrCssKeyword_Count;
    uint32_t count = functionArguments(easing, numbers, 4, &keyword);
    switch (cssValueKeyword(value)) {
        case PrCssKeyword_CubicBezier:
            if (count == 4)
                curve(numbers[0], numbers[1], numbers[2], numbers[3]);
            break;
        case PrCssKeyword_Steps:
            if (count >= 1 && numbers[0] >= 1) {
                PrLayoutStepPosition position = keyword == PrCssKeyword_JumpStart || keyword == PrCssKeyword_Start
                                                    ? PrLayoutStepPosition_JumpStart
                                                : keyword == PrCssKeyword_JumpBoth ? PrLayoutStepPosition_JumpBoth
                                                : keyword == PrCssKeyword_JumpNone ? PrLayoutStepPosition_JumpNone
                                                                                   : PrLayoutStepPosition_JumpEnd;
                steps(uint32_t(numbers[0]), position);
            }
            break;
        default:
            break;
    }
    return out;
}

PrLayoutTransformList cssTransformFunctions(CssValueRun translate, CssValueRun rotate, CssValueRun scale,
                                            CssValueRun transform) {
    PrLayoutTransformList out{};
    TransformArgument v[4];
    uint32_t count = transformArguments(translate, v, 3);
    if (count != 0 && !v[0].keyword)
        add(&out, translation(v[0].value, v[0].fraction, count >= 2 ? v[1].value : 0, count >= 2 ? v[1].fraction : 0,
                              count >= 3 ? v[2].value : 0));
    count = transformArguments(rotate, v, 4);
    if (count == 1 && !v[0].keyword) {
        add(&out, rotation(0, 0, 1, v[0].value));
    } else if (count == 2 && v[0].keyword) {
        PrCssKeyword axis = cssValueKeyword(rotate.values);
        add(&out, rotation(axis == PrCssKeyword_X ? 1 : 0, axis == PrCssKeyword_Y ? 1 : 0,
                           axis == PrCssKeyword_Z ? 1 : 0, v[1].value));
    } else if (count == 4) {
        add(&out, rotation(v[0].value, v[1].value, v[2].value, v[3].value));
    }
    count = transformArguments(scale, v, 3);
    if (count != 0 && !v[0].keyword)
        add(&out, scaling(factorOf(v[0]), factorOf(v[count >= 2 ? 1 : 0]), count >= 3 ? factorOf(v[2]) : 1));
    for (uint32_t i = 0; i < transform.count; i += transform.values[i].next) {
        if (transform.values[i].kind == PrCssValueKind_Function)
            addFunction(&out, cssValueSubtree(transform, i));
    }
    return out;
}

bool cssInterpolable(PrCssProperty property, CssValueRun from, CssValueRun to, const CssColorContext& colors) {
    if (isFilterList(property))
        return !cssValueRunEquals(from, to) && addFiltersBetween(nullptr, from, to, 0, colors);
    if (isImageList(property))
        return addImagesBetween(nullptr, from, to, 0, colors);
    if (isTrackList(property)) {
        TrackItems a;
        TrackItems b;
        return !cssValueRunEquals(from, to) && trackListsCombine(from, to, &a, &b);
    }
    if (!reshapes(property))
        return interpolable(property, from, to, colors);
    if (hasKeywordRadius(property, from) || hasKeywordRadius(property, to))
        return false;
    CssValuePool x{};
    CssValuePool y{};
    bool result = interpolable(property, normalized(property, from, &x), normalized(property, to, &y), colors);
    cssValuePoolReset(&x);
    cssValuePoolReset(&y);
    return result;
}

void cssInterpolate(CssValuePool* out, PrCssProperty property, CssValueRun from, CssValueRun to, double progress,
                    const CssColorContext& colors) {
    if (cssIsCornerShape(property)) {
        cssCornerShapeInterpolate(out, from, to, progress);
        return;
    }
    if (isFilterList(property)) {
        if (!addFiltersBetween(out, from, to, progress, colors))
            cssValuePoolAddRun(out, progress < 0.5 ? from : to);
        return;
    }
    if (isImageList(property)) {
        if (!addImagesBetween(out, from, to, progress, colors))
            cssValuePoolAddRun(out, progress < 0.5 ? from : to);
        return;
    }
    if (isTrackList(property)) {
        TrackItems a;
        TrackItems b;
        TrackOp op{.property = property, .progress = progress, .add = false, .first = progress < 0.5};
        if (trackListsCombine(from, to, &a, &b))
            addTrackItems(out, op, a, b);
        else
            cssValuePoolAddRun(out, progress < 0.5 ? from : to);
        return;
    }
    if (!reshapes(property)) {
        interpolate(out, property, from, to, progress, colors);
        return;
    }
    // A pair that interpolates only discretely keeps the values as they are written.
    if (!cssInterpolable(property, from, to, colors)) {
        cssValuePoolAddRun(out, progress < 0.5 ? from : to);
        return;
    }
    CssValuePool x{};
    CssValuePool y{};
    CssValuePool between{};
    interpolate(&between, property, normalized(property, from, &x), normalized(property, to, &y), progress, colors);
    addCombined(out, property, cssValuePoolRun(&between, CssValueSpan{0, between.values.count}));
    cssValuePoolReset(&x);
    cssValuePoolReset(&y);
    cssValuePoolReset(&between);
}

CssComposite cssCompositeFromValue(CssValueRun item) {
    if (item.count != 1 || item.values[0].kind != PrCssValueKind_Keyword)
        return CssComposite_Replace;
    switch (cssValueKeyword(item.values)) {
        case PrCssKeyword_Add: return CssComposite_Add;
        case PrCssKeyword_Accumulate: return CssComposite_Accumulate;
        default: return CssComposite_Replace;
    }
}

void cssComposite(CssValuePool* out, PrCssProperty property, CssValueRun underlying, CssValueRun value,
                  CssComposite composite, const CssColorContext& colors) {
    if (isFilterList(property)) {
        if (composite == CssComposite_Replace)
            cssValuePoolAddRun(out, value);
        else
            addFiltersComposite(out, underlying, value, composite == CssComposite_Accumulate, colors);
        return;
    }
    // Track sizes add as their numbers do, for add and accumulate alike.
    if (isTrackList(property)) {
        TrackItems a;
        TrackItems b;
        if (composite != CssComposite_Replace && trackListsCombine(underlying, value, &a, &b))
            addTrackItems(out, TrackOp{.property = property, .progress = 0, .add = true, .first = false}, a, b);
        else
            cssValuePoolAddRun(out, value);
        return;
    }
    if (!reshapes(property)) {
        combine(out, property, underlying, value, composite, colors);
        return;
    }
    if (hasKeywordRadius(property, underlying) || hasKeywordRadius(property, value)) {
        cssValuePoolAddRun(out, value);
        return;
    }
    CssValuePool x{};
    CssValuePool y{};
    CssValuePool combined{};
    combine(&combined, property, normalized(property, underlying, &x), normalized(property, value, &y), composite,
            colors);
    addCombined(out, property, cssValuePoolRun(&combined, CssValueSpan{0, combined.values.count}));
    cssValuePoolReset(&x);
    cssValuePoolReset(&y);
    cssValuePoolReset(&combined);
}

} // namespace pr_impl
