// The JavaScript binding of the SVG DOM's data types, and the members of element interfaces that reflect attributes as
// them.
//
// SVG2 - https://svgwg.org/svg2-draft/types.html
//
// The SVG DOM's objects are the binding's: an SVGAnimatedLength, the SVGLength of its baseVal, a list and the objects
// of its items are views of an element's typed attribute (pr/svg_types.h), which they read and write through the C API.
// The binding keeps the identity that script sees: the same object for a member, a list's value or one of its items.

#pragma once

#include "pr/quickjs_binding.h"
#include "pr/svg_types.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// The members of the element interfaces that reflect an attribute as one of the SVG DOM's types: each member's name as
// the interface spells it, the attribute that it reflects, and which number of a pair it is (1 or 2, or 0 for the whole
// value). The magic of each member's getter is its SvgMember.
#define PR_SVG_MEMBERS(M)                                                                                              \
    M(x, X, 0) M(y, Y, 0) M(width, Width, 0) M(height, Height, 0) M(rx, Rx, 0) M(ry, Ry, 0) M(cx, Cx, 0)             \
    M(cy, Cy, 0) M(r, R, 0) M(x1, X1, 0) M(y1, Y1, 0) M(x2, X2, 0) M(y2, Y2, 0) M(fx, Fx, 0) M(fy, Fy, 0)            \
    M(fr, Fr, 0) M(refX, RefX, 0) M(refY, RefY, 0) M(markerWidth, MarkerWidth, 0) M(markerHeight, MarkerHeight, 0)  \
    M(textLength, TextLength, 0) M(startOffset, StartOffset, 0) M(pathLength, PathLength, 0) M(offset, Offset, 0)   \
    M(slope, Slope, 0) M(intercept, Intercept, 0) M(amplitude, Amplitude, 0) M(exponent, Exponent, 0) M(k1, K1, 0)  \
    M(k2, K2, 0) M(k3, K3, 0) M(k4, K4, 0) M(divisor, Divisor, 0) M(bias, Bias, 0)                                  \
    M(kernelUnitLengthX, KernelUnitLength, 1) M(kernelUnitLengthY, KernelUnitLength, 2)                             \
    M(surfaceScale, SurfaceScale, 0) M(diffuseConstant, DiffuseConstant, 0)                                         \
    M(specularConstant, SpecularConstant, 0) M(specularExponent, SpecularExponent, 0) M(azimuth, Azimuth, 0)        \
    M(elevation, Elevation, 0) M(z, Z, 0) M(pointsAtX, PointsAtX, 0) M(pointsAtY, PointsAtY, 0)                     \
    M(pointsAtZ, PointsAtZ, 0) M(limitingConeAngle, LimitingConeAngle, 0) M(scale, Scale, 0) M(dx, Dx, 0)           \
    M(dy, Dy, 0) M(stdDeviationX, StdDeviation, 1) M(stdDeviationY, StdDeviation, 2) M(radiusX, Radius, 1)          \
    M(radiusY, Radius, 2) M(baseFrequencyX, BaseFrequency, 1) M(baseFrequencyY, BaseFrequency, 2) M(seed, Seed, 0)  \
    M(orderX, Order, 1) M(orderY, Order, 2) M(targetX, TargetX, 0) M(targetY, TargetY, 0)                           \
    M(numOctaves, NumOctaves, 0) M(preserveAlpha, PreserveAlpha, 0) M(lengthAdjust, LengthAdjust, 0)                \
    M(method, Method, 0) M(spacing, Spacing, 0) M(side, Side, 0) M(markerUnits, MarkerUnits, 0)                     \
    M(orientType, Orient, 0)                                                                                        \
    M(gradientUnits, GradientUnits, 0) M(spreadMethod, SpreadMethod, 0) M(patternUnits, PatternUnits, 0)            \
    M(patternContentUnits, PatternContentUnits, 0) M(filterUnits, FilterUnits, 0)                                   \
    M(primitiveUnits, PrimitiveUnits, 0) M(mode, Mode, 0) M(type, Type, 0) M(operator, Operator, 0)                 \
    M(edgeMode, EdgeMode, 0) M(xChannelSelector, XChannelSelector, 0) M(yChannelSelector, YChannelSelector, 0)      \
    M(stitchTiles, StitchTiles, 0) M(clipPathUnits, ClipPathUnits, 0) M(maskUnits, MaskUnits, 0)                    \
    M(maskContentUnits, MaskContentUnits, 0) M(orientAngle, Orient, 0) M(viewBox, ViewBox, 0)                       \
    M(preserveAspectRatio, PreserveAspectRatio, 0) M(transform, Transform, 0)                                       \
    M(gradientTransform, GradientTransform, 0) M(patternTransform, PatternTransform, 0) M(rotate, Rotate, 0)        \
    M(values, Values, 0) M(tableValues, TableValues, 0) M(kernelMatrix, KernelMatrix, 0) M(href, Href, 0)           \
    M(target, Target, 0) M(result, Result, 0) M(in1, In, 0) M(in2, In2, 0) M(crossOrigin, Crossorigin, 0)           \
    M(className, Class, 0) M(requiredExtensions, RequiredExtensions, 0) M(systemLanguage, SystemLanguage, 0)       \
    M(points, Points, 0) M(animatedPoints, Points, 0)

// The magic of the getter of each member.
#define PR_SVG_MEMBER_ENUMERATOR(name, attribute, part) SvgMember_##name,
enum {
    PR_SVG_MEMBERS(PR_SVG_MEMBER_ENUMERATOR)
    SvgMember_Count,
};
#undef PR_SVG_MEMBER_ENUMERATOR

// The getters of the members of each type. Each returns the same object every time.
JSValue svgAnimatedBoolean(JSContext* ctx, JSValueConst element, int member);
JSValue svgAnimatedEnumeration(JSContext* ctx, JSValueConst element, int member);
JSValue svgAnimatedInteger(JSContext* ctx, JSValueConst element, int member);
JSValue svgAnimatedNumber(JSContext* ctx, JSValueConst element, int member);
JSValue svgAnimatedString(JSContext* ctx, JSValueConst element, int member);
JSValue svgAnimatedLength(JSContext* ctx, JSValueConst element, int member);
JSValue svgAnimatedAngle(JSContext* ctx, JSValueConst element, int member);
JSValue svgAnimatedRect(JSContext* ctx, JSValueConst element, int member);
JSValue svgAnimatedPreserveAspectRatio(JSContext* ctx, JSValueConst element, int member);
JSValue svgAnimatedNumberList(JSContext* ctx, JSValueConst element, int member);
JSValue svgAnimatedLengthList(JSContext* ctx, JSValueConst element, int member);
JSValue svgAnimatedTransformList(JSContext* ctx, JSValueConst element, int member);
JSValue svgStringList(JSContext* ctx, JSValueConst element, int member);

// The getter of points and animatedPoints (SVG2/shapes.html#InterfaceSVGAnimatedPoints).
JSValue svgPointList(JSContext* ctx, JSValueConst element, int member);

// The entry of a prototype's member list for each type of member.
#define PR_SVG_BOOLEAN(name) PR_JS_MAGIC_GET(#name, svgAnimatedBoolean, SvgMember_##name)
#define PR_SVG_ENUMERATION(name) PR_JS_MAGIC_GET(#name, svgAnimatedEnumeration, SvgMember_##name)
#define PR_SVG_INTEGER(name) PR_JS_MAGIC_GET(#name, svgAnimatedInteger, SvgMember_##name)
#define PR_SVG_NUMBER(name) PR_JS_MAGIC_GET(#name, svgAnimatedNumber, SvgMember_##name)
#define PR_SVG_STRING(name) PR_JS_MAGIC_GET(#name, svgAnimatedString, SvgMember_##name)
#define PR_SVG_LENGTH(name) PR_JS_MAGIC_GET(#name, svgAnimatedLength, SvgMember_##name)
#define PR_SVG_ANGLE(name) PR_JS_MAGIC_GET(#name, svgAnimatedAngle, SvgMember_##name)
#define PR_SVG_RECT(name) PR_JS_MAGIC_GET(#name, svgAnimatedRect, SvgMember_##name)
#define PR_SVG_PRESERVE_ASPECT_RATIO(name) PR_JS_MAGIC_GET(#name, svgAnimatedPreserveAspectRatio, SvgMember_##name)
#define PR_SVG_NUMBER_LIST(name) PR_JS_MAGIC_GET(#name, svgAnimatedNumberList, SvgMember_##name)
#define PR_SVG_LENGTH_LIST(name) PR_JS_MAGIC_GET(#name, svgAnimatedLengthList, SvgMember_##name)
#define PR_SVG_TRANSFORM_LIST(name) PR_JS_MAGIC_GET(#name, svgAnimatedTransformList, SvgMember_##name)
#define PR_SVG_STRING_LIST(name) PR_JS_MAGIC_GET(#name, svgStringList, SvgMember_##name)
#define PR_SVG_POINT_LIST(name) PR_JS_MAGIC_GET(#name, svgPointList, SvgMember_##name)

// The factories of SVGSVGElement: a new, detached number, length, angle or transform, and a transform of a matrix
// (SVG2/struct.html#__svg__SVGSVGElement__createSVGNumber).
JSValue svgCreateNumber(JSContext* ctx, JSValueConst svg, int argc, JSValueConst* argv);
JSValue svgCreatePoint(JSContext* ctx, JSValueConst svg, int argc, JSValueConst* argv);
JSValue svgCreateLength(JSContext* ctx, JSValueConst svg, int argc, JSValueConst* argv);
JSValue svgCreateAngle(JSContext* ctx, JSValueConst svg, int argc, JSValueConst* argv);
JSValue svgCreateTransform(JSContext* ctx, JSValueConst svg, int argc, JSValueConst* argv);
JSValue svgCreateTransformFromMatrix(JSContext* ctx, JSValueConst svg, int argc, JSValueConst* argv);

// Writes the value of the SVGAngle `value` to `out`, or throws a TypeError and returns false if it is not one.
bool svgAngleOf(JSContext* ctx, JSValueConst value, PrSvgAngle* out);

// Defines the SVG DOM's data types on the global object of `ctx`.
void svgTypesBind(JSContext* ctx);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
