// JavaScript binding of SVG.
//
// SVG2 - https://svgwg.org/svg2-draft/

#include "pr/svg_quickjs.h"

#include "pr/count_of.h"
#include "pr/css_quickjs.h"
#include "pr/dom_geometry_quickjs.h"
#include "pr/dom_quickjs.h"
#include "pr/dom_reflect_quickjs.h"
#include "pr/float.h"
#include "pr/path_data.h"
#include "pr/quickjs_binding.h"
#include "pr/svg.h"
#include "pr/svg_element.h"
#include "pr/svg_types.h"
#include "pr/webidl_quickjs.h"

#include "svg/quickjs/src/svg_types_quickjs.h"

namespace pr_impl {

JSClassID svgRectElementClass;
JSClassID svgCircleElementClass;
JSClassID svgEllipseElementClass;
JSClassID svgLineElementClass;
JSClassID svgPathElementClass;
JSClassID svgPolylineElementClass;
JSClassID svgPolygonElementClass;
JSClassID svgTimeEventClass;
JSClassID svgTextPositioningElementClass;
JSClassID svgGradientElementClass;
JSClassID svgTransferFunctionElementClass;
JSClassID svgGElementClass;
JSClassID svgDefsElementClass;
JSClassID svgDescElementClass;
JSClassID svgMetadataElementClass;
JSClassID svgTitleElementClass;
JSClassID svgStyleElementClass;
JSClassID svgTextElementClass;
JSClassID svgTSpanElementClass;
JSClassID svgTextPathElementClass;
JSClassID svgMarkerElementClass;
JSClassID svgLinearGradientElementClass;
JSClassID svgRadialGradientElementClass;
JSClassID svgStopElementClass;
JSClassID svgPatternElementClass;
JSClassID svgFilterElementClass;
JSClassID svgFeBlendElementClass;
JSClassID svgFeColorMatrixElementClass;
JSClassID svgFeComponentTransferElementClass;
JSClassID svgFeCompositeElementClass;
JSClassID svgFeConvolveMatrixElementClass;
JSClassID svgFeDiffuseLightingElementClass;
JSClassID svgFeDisplacementMapElementClass;
JSClassID svgFeDistantLightElementClass;
JSClassID svgFeDropShadowElementClass;
JSClassID svgFeFloodElementClass;
JSClassID svgFeFuncAElementClass;
JSClassID svgFeFuncBElementClass;
JSClassID svgFeFuncGElementClass;
JSClassID svgFeFuncRElementClass;
JSClassID svgFeGaussianBlurElementClass;
JSClassID svgFeImageElementClass;
JSClassID svgFeMergeElementClass;
JSClassID svgFeMergeNodeElementClass;
JSClassID svgFeMorphologyElementClass;
JSClassID svgFeOffsetElementClass;
JSClassID svgFePointLightElementClass;
JSClassID svgFeSpecularLightingElementClass;
JSClassID svgFeSpotLightElementClass;
JSClassID svgFeTileElementClass;
JSClassID svgFeTurbulenceElementClass;
JSClassID svgClipPathElementClass;
JSClassID svgMaskElementClass;
JSClassID svgMPathElementClass;

namespace {

PrSvgElement* PR_NULLABLE textContentOf(JSContext* ctx, JSValueConst object) {
    return jsThis<PrSvgElement>(ctx, object, prJsSvgTextContentElementClass);
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
JSValue getBBox(JSContext* ctx, JSValueConst elementObject, int argc, JSValueConst* argv) {
    PrSvgElement* element = jsThis<PrSvgElement>(ctx, elementObject, prJsSvgGraphicsElementClass);
    if (element == nullptr)
        return JS_EXCEPTION;
    PrSvgBoundingBoxOptions options = prSvgBoundingBoxOptionsDefault();
    JSValueConst dictionary = argc > 0 ? argv[0] : JS_UNDEFINED;
    if (!JS_IsUndefined(dictionary) && !JS_IsNull(dictionary)) {
        if (!JS_IsObject(dictionary))
            return JS_ThrowTypeError(ctx, "Expected an SVGBoundingBoxOptions");
        if (!readMember(ctx, dictionary, "clipped", false, &options.clipped) ||
                !readMember(ctx, dictionary, "fill", true, &options.fill) ||
                !readMember(ctx, dictionary, "markers", false, &options.markers) ||
                !readMember(ctx, dictionary, "stroke", false, &options.stroke))
            return JS_EXCEPTION;
    }
    return prJsDomRectNew(ctx, prSvgGraphicsElementGetBBox(element, &options));
}

// Implements getCTM and getScreenCTM, selected by the magic. A transform that cannot be found is null.
JSValue getCtm(JSContext* ctx, JSValueConst elementObject, int, JSValueConst*, int magic) {
    PrSvgElement* element = jsThis<PrSvgElement>(ctx, elementObject, prJsSvgGraphicsElementClass);
    if (element == nullptr)
        return JS_EXCEPTION;
    PrDomMatrix matrix = prDomMatrixIdentity();
    bool found = magic == 0 ? prSvgGraphicsElementGetCtm(element, &matrix) : prSvgGraphicsElementGetScreenCtm(element, &matrix);
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
JSValue isPointIn(JSContext* ctx, JSValueConst elementObject, int argc, JSValueConst* argv, int magic) {
    PrSvgElement* element = jsThis<PrSvgElement>(ctx, elementObject, prJsSvgGeometryElementClass);
    PrDomPoint point{0, 0, 0, 1};
    if (element == nullptr || !prJsDomPointInitRead(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, &point))
        return JS_EXCEPTION;
    bool inside = false;
    PrStatus status;
    if (magic == 0)
        status = prSvgGeometryElementIsPointInFill(element, point, &inside);
    else
        status = prSvgGeometryElementIsPointInStroke(element, point, &inside);
    return orInvalidState(ctx, status, JS_NewBool(ctx, inside));
}

JSValue getTotalLength(JSContext* ctx, JSValueConst elementObject, int, JSValueConst*) {
    PrSvgElement* element = jsThis<PrSvgElement>(ctx, elementObject, prJsSvgGeometryElementClass);
    if (element == nullptr)
        return JS_EXCEPTION;
    return JS_NewFloat64(ctx, double(prSvgGeometryElementTotalLength(element)));
}

JSValue getPointAtLength(JSContext* ctx, JSValueConst elementObject, int argc, JSValueConst* argv) {
    PrSvgElement* element = jsThis<PrSvgElement>(ctx, elementObject, prJsSvgGeometryElementClass);
    double distance = 0;
    if (element == nullptr || !jsArgCount(ctx, argc, 1) || JS_ToFloat64(ctx, &distance, argv[0]) < 0)
        return JS_EXCEPTION;
    if (!prF64IsFinite(distance))
        return JS_ThrowTypeError(ctx, "The distance is not a finite number");
    PrDomPoint point{0, 0, 0, 1};
    PrStatus status = prSvgGeometryElementGetPointAtLength(element, float(distance), &point);
    if (status != PrStatus_Ok)
        return jsDomExceptionThrow(ctx, "InvalidStateError");
    return prJsDomPointNew(ctx, point);
}

// ---------------------------------------------------------------------------------------------------------------------
// SVGTextContentElement

JSValue getNumberOfChars(JSContext* ctx, JSValueConst textContent, int, JSValueConst*) {
    PrSvgElement* element = textContentOf(ctx, textContent);
    return element != nullptr ? JS_NewInt64(ctx, prSvgTextContentElementGetNumberOfChars(element)) : JS_EXCEPTION;
}

JSValue getComputedTextLength(JSContext* ctx, JSValueConst textContent, int, JSValueConst*) {
    PrSvgElement* element = textContentOf(ctx, textContent);
    if (element == nullptr)
        return JS_EXCEPTION;
    return JS_NewFloat64(ctx, double(prSvgTextContentElementGetComputedTextLength(element)));
}

JSValue getSubStringLength(JSContext* ctx, JSValueConst textContent, int argc, JSValueConst* argv) {
    PrSvgElement* element = textContentOf(ctx, textContent);
    uint32_t first = 0;
    uint32_t count = 0;
    if (element == nullptr || !indexOf(ctx, argc, argv, 0, &first) || !indexOf(ctx, argc, argv, 1, &count))
        return JS_EXCEPTION;
    float length = 0;
    PrStatus status = prSvgTextContentElementGetSubStringLength(element, first, count, &length);
    return orIndexSize(ctx, status, JS_NewFloat64(ctx, double(length)));
}

// Implements getStartPositionOfChar and getEndPositionOfChar, selected by the magic.
JSValue getPositionOfChar(JSContext* ctx, JSValueConst textContent, int argc, JSValueConst* argv, int magic) {
    PrSvgElement* element = textContentOf(ctx, textContent);
    uint32_t index = 0;
    if (element == nullptr || !indexOf(ctx, argc, argv, 0, &index))
        return JS_EXCEPTION;
    PrDomPoint point{0, 0, 0, 1};
    PrStatus status;
    if (magic == 0)
        status = prSvgTextContentElementGetStartPositionOfChar(element, index, &point);
    else
        status = prSvgTextContentElementGetEndPositionOfChar(element, index, &point);
    if (status != PrStatus_Ok)
        return jsDomExceptionThrow(ctx, "IndexSizeError");
    return prJsDomPointNew(ctx, point);
}

JSValue getExtentOfChar(JSContext* ctx, JSValueConst textContent, int argc, JSValueConst* argv) {
    PrSvgElement* element = textContentOf(ctx, textContent);
    uint32_t index = 0;
    if (element == nullptr || !indexOf(ctx, argc, argv, 0, &index))
        return JS_EXCEPTION;
    PrDomRect rect{};
    if (prSvgTextContentElementGetExtentOfChar(element, index, &rect) != PrStatus_Ok)
        return jsDomExceptionThrow(ctx, "IndexSizeError");
    return prJsDomRectNew(ctx, rect);
}

JSValue getRotationOfChar(JSContext* ctx, JSValueConst textContent, int argc, JSValueConst* argv) {
    PrSvgElement* element = textContentOf(ctx, textContent);
    uint32_t index = 0;
    if (element == nullptr || !indexOf(ctx, argc, argv, 0, &index))
        return JS_EXCEPTION;
    float rotation = 0;
    PrStatus status = prSvgTextContentElementGetRotationOfChar(element, index, &rotation);
    return orIndexSize(ctx, status, JS_NewFloat64(ctx, double(rotation)));
}

JSValue getCharNumAtPosition(JSContext* ctx, JSValueConst textContent, int argc, JSValueConst* argv) {
    PrSvgElement* element = textContentOf(ctx, textContent);
    PrDomPoint point{};
    if (element == nullptr || !prJsDomPointInitRead(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, &point))
        return JS_EXCEPTION;
    return JS_NewInt32(ctx, prSvgTextContentElementGetCharNumAtPosition(element, point));
}

// ---------------------------------------------------------------------------------------------------------------------
// SVGSVGElement

// SVGSVGElement's createSVGMatrix and createSVGRect (SVG2/struct.html#__svg__SVGSVGElement__createSVGMatrix). Each
// returns a new matrix or rect of the geometry interface that SVGMatrix or SVGRect names.
JSValue createSvgMatrix(JSContext* ctx, JSValueConst object, int, JSValueConst*) {
    if (jsThis<PrSvgElement>(ctx, object, prJsSvgSvgElementClass) == nullptr)
        return JS_EXCEPTION;
    return prJsDomMatrixNew(ctx, prDomMatrixIdentity());
}

JSValue createSvgRect(JSContext* ctx, JSValueConst object, int, JSValueConst*) {
    if (jsThis<PrSvgElement>(ctx, object, prJsSvgSvgElementClass) == nullptr)
        return JS_EXCEPTION;
    return prJsDomRectNew(ctx, PrDomRect{});
}

// selectSubString(charnum, nchars), which throws an IndexSizeError for a character past the last.
JSValue selectSubString(JSContext* ctx, JSValueConst textContent, int argc, JSValueConst* argv) {
    PrSvgElement* element = textContentOf(ctx, textContent);
    uint32_t index = 0;
    uint32_t count = 0;
    if (element == nullptr || !jsArgCount(ctx, argc, 2) || JS_ToUint32(ctx, &index, argv[0]) < 0 ||
            JS_ToUint32(ctx, &count, argv[1]) < 0)
        return JS_EXCEPTION;
    if (prSvgTextContentElementSelectSubString(element, index, count) != PrStatus_Ok)
        return jsDomExceptionThrow(ctx, "IndexSizeError");
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------------------------------------------------
// SVGStyleElement (SVG2/styling.html#InterfaceSVGStyleElement), whose sheet is LinkStyle's
// (cssom-1/#the-linkstyle-interface)

JSValue styleSheet(JSContext* ctx, JSValueConst elementObject) {
    PrDomElement* element = jsThis<PrDomElement>(ctx, elementObject, prJsDomElementClass);
    if (element == nullptr)
        return JS_EXCEPTION;
    return jsWrap(ctx, prJsCssStyleSheetClass, elementObject, prCssElementSheet(element), false);
}

// disabled, which is the sheet's: false without one, where setting it does nothing (html/#dom-style-disabled).
JSValue styleDisabled(JSContext* ctx, JSValueConst elementObject) {
    PrDomElement* element = jsThis<PrDomElement>(ctx, elementObject, prJsDomElementClass);
    if (element == nullptr)
        return JS_EXCEPTION;
    PrCssStyleSheet* sheet = prCssElementSheet(element);
    return JS_NewBool(ctx, sheet != nullptr && prCssStyleSheetDisabled(sheet));
}

JSValue setStyleDisabled(JSContext* ctx, JSValueConst elementObject, JSValueConst input) {
    PrDomElement* element = jsThis<PrDomElement>(ctx, elementObject, prJsDomElementClass);
    if (element == nullptr)
        return JS_EXCEPTION;
    if (PrCssStyleSheet* sheet = prCssElementSheet(element))
        prCssStyleSheetSetDisabled(sheet, JS_ToBool(ctx, input) > 0);
    return JS_UNDEFINED;
}

// Document's rootElement (SVG2/struct.html#__svg__Document__rootElement): the document element if it is an svg element,
// and otherwise null.
JSValue rootElement(JSContext* ctx, JSValueConst documentObject) {
    PrDomDocument* document = jsThis<PrDomDocument>(ctx, documentObject, prJsDomDocumentClass);
    if (document == nullptr)
        return JS_EXCEPTION;
    PrDomElement* root = prDomDocumentElement(document);
    PrSvgElement* svg = root != nullptr ? prSvgElementQuery(prDomElementAsEventTarget(root)) : nullptr;
    if (svg == nullptr || prSvgElementInterface(svg) != PrSvgInterface_Svg)
        return JS_NULL;
    return jsWrapAs(ctx, prJsDomElementClass, documentObject, root, false);
}

// ---------------------------------------------------------------------------------------------------------------------
// SVGPathData (svg-paths/#InterfaceSVGPathData)

// Returns the commands of the path element's d attribute, its base value, up to the first error. A missing attribute
// has none.
RawArray<PrPathDataCommand> commandsOf(PrDomElement* path) {
    RawArray<PrPathDataCommand> commands;
    PrStringView d = prDomElementGetAttribute(path, "d");
    if (d.data != nullptr) {
        commands.resize(prPathDataParse(d, nullptr, 0));
        prPathDataParse(d, commands.data, commands.count);
    }
    return commands;
}

// Returns the command as an SVGPathSegment: its letter as the type, and its arguments as the values.
JSValue segmentOf(JSContext* ctx, const PrPathDataCommand& command) {
    JSValue segment = JS_NewObject(ctx);
    char letter[2] = {command.letter, 0};
    JS_SetPropertyStr(ctx, segment, "type", JS_NewString(ctx, letter));
    JSValue values = JS_NewArray(ctx);
    for (uint32_t i = 0; i < prPathDataArgumentCount(command.letter); ++i)
        JS_SetPropertyUint32(ctx, values, i, JS_NewFloat64(ctx, double(command.arguments[i])));
    JS_SetPropertyStr(ctx, segment, "values", values);
    return segment;
}

// getPathData(settings): the segments of the d attribute, or with `normalize`, those of the same path in absolute M, L,
// C and Z alone.
JSValue getPathData(JSContext* ctx, JSValueConst object, int argc, JSValueConst* argv) {
    PrDomElement* path = jsThis<PrDomElement>(ctx, object, svgPathElementClass);
    bool present = false;
    bool normalize = false;
    if (path == nullptr || !jsDictionary(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, &present))
        return JS_EXCEPTION;
    if (present && !jsDictBool(ctx, argv[0], "normalize", &normalize))
        return JS_EXCEPTION;
    RawArray<PrPathDataCommand> commands = commandsOf(path);
    if (normalize) {
        RawArray<PrPathDataCommand> normalized;
        normalized.resize(prPathDataNormalize(commands.data, commands.count, nullptr, 0));
        prPathDataNormalize(commands.data, commands.count, normalized.data, normalized.count);
        commands.reset();
        commands = normalized;
    }
    JSValue segments = JS_NewArray(ctx);
    for (uint32_t i = 0; i < commands.count; ++i)
        JS_SetPropertyUint32(ctx, segments, i, segmentOf(ctx, commands.data[i]));
    commands.reset();
    return segments;
}

// Reads an SVGPathSegment, and writes it as a command to `out`. Writes a letter of zero for a segment that is not a
// command of path data: an unknown or a longer type, a count of values that is not the command's, or a value that is
// not finite. Returns false, with an exception thrown, for a value that is not an SVGPathSegment.
bool readSegment(JSContext* ctx, JSValueConst input, PrPathDataCommand* out) {
    *out = PrPathDataCommand{};
    bool present = false;
    StringArgument type = {ctx};
    bool given = false;
    if (!jsDictionary(ctx, input, &present) || !jsDictString(ctx, input, "type", &type, &given))
        return false;
    JSValue values = JS_GetPropertyStr(ctx, input, "values");
    if (JS_IsException(values))
        return false;
    if (!given || JS_IsUndefined(values)) {
        JS_FreeValue(ctx, values);
        JS_ThrowTypeError(ctx, "An SVGPathSegment has a type and values");
        return false;
    }
    PrStringView letters = type.value();
    bool valid = letters.size == 1 && prStringContains("MmLlHhVvCcSsQqTtAaZz", letters);
    uint32_t count = 0;
    bool ok = jsSequence(ctx, values, [&](JSValue item) {
        double value = 0;
        bool read = JS_ToFloat64(ctx, &value, item) == 0;
        JS_FreeValue(ctx, item);
        valid = valid && read && prF64IsFinite(value) && count < kPrPathDataCommandMaxArguments;
        if (valid)
            out->arguments[count] = float(value);
        ++count;
        return read;
    });
    JS_FreeValue(ctx, values);
    if (!ok)
        return false;
    if (valid && count == prPathDataArgumentCount(letters.data[0]))
        out->letter = letters.data[0];
    return true;
}

// setPathData(pathData): sets the d attribute to the segments, up to the first that is not a command, or removes it if
// that leaves none. The segments must start with a moveto.
JSValue setPathData(JSContext* ctx, JSValueConst object, int argc, JSValueConst* argv) {
    PrDomElement* path = jsThis<PrDomElement>(ctx, object, svgPathElementClass);
    if (path == nullptr || !jsArgCount(ctx, argc, 1))
        return JS_EXCEPTION;
    RawArray<PrPathDataCommand> commands;
    bool ended = false;
    bool ok = jsSequence(ctx, argv[0], [&](JSValue item) {
        PrPathDataCommand command{};
        bool read = readSegment(ctx, item, &command);
        JS_FreeValue(ctx, item);
        bool first = commands.count == 0;
        ended = ended || command.letter == 0 || (first && command.letter != 'M' && command.letter != 'm');
        if (read && !ended)
            commands.push(command);
        return read;
    });
    if (!ok) {
        commands.reset();
        return JS_EXCEPTION;
    }
    PrStatus status = PrStatus_Ok;
    if (commands.count == 0) {
        prDomElementRemoveAttribute(path, "d");
    } else {
        PrRawString d = prRawStringNull();
        prPathDataWrite(commands.data, commands.count, &d);
        status = prDomElementSetAttribute(path, "d", prRawStringView(&d));
        prRawStringReset(&d);
    }
    commands.reset();
    return status == PrStatus_Ok ? JS_UNDEFINED : prJsDomThrow(ctx, status);
}

// getPathSegmentAtLength(distance): the segment of the d attribute that the point `distance` along the path is on, or
// null if the attribute has no segments.
JSValue getPathSegmentAtLength(JSContext* ctx, JSValueConst object, int argc, JSValueConst* argv) {
    PrDomElement* path = jsThis<PrDomElement>(ctx, object, svgPathElementClass);
    double distance = 0;
    if (path == nullptr || !jsArgCount(ctx, argc, 1) || JS_ToFloat64(ctx, &distance, argv[0]) < 0)
        return JS_EXCEPTION;
    RawArray<PrPathDataCommand> commands = commandsOf(path);
    uint32_t index = prPathDataSegmentAtLength(commands.data, commands.count, float(distance));
    JSValue segment = index < commands.count ? segmentOf(ctx, commands.data[index]) : JS_NULL;
    commands.reset();
    return segment;
}

// ---------------------------------------------------------------------------------------------------------------------
// SVGSVGElement's view and selection (SVG2/struct.html#InterfaceSVGSVGElement)

// Returns the svg element of `object`, and writes whether it is an outermost svg element, which has no svg ancestor.
PrSvgElement* PR_NULLABLE svgOf(JSContext* ctx, JSValueConst object, bool* outermost) {
    PrSvgElement* element = jsThis<PrSvgElement>(ctx, object, prJsSvgSvgElementClass);
    *outermost = element != nullptr && prSvgElementOwnerSvgElement(element) == nullptr;
    return element;
}

// Returns the svg element of `object` as an element, or null with an exception thrown.
PrDomElement* PR_NULLABLE svgElementOf(JSContext* ctx, JSValueConst object) {
    bool outermost = false;
    if (svgOf(ctx, object, &outermost) == nullptr)
        return nullptr;
    return jsThis<PrDomElement>(ctx, object, prJsDomElementClass);
}

// currentScale, the scale of the element's magnification and panning transform
// (SVG2/struct.html#__svg__SVGSVGElement__currentScale).
JSValue currentScale(JSContext* ctx, JSValueConst object) {
    bool outermost = false;
    PrSvgElement* svg = svgOf(ctx, object, &outermost);
    return svg != nullptr ? JS_NewFloat64(ctx, double(prSvgElementCurrentScale(svg))) : JS_EXCEPTION;
}

JSValue setCurrentScale(JSContext* ctx, JSValueConst object, JSValueConst input) {
    bool outermost = false;
    double scale = 0;
    PrSvgElement* svg = svgOf(ctx, object, &outermost);
    if (svg == nullptr || JS_ToFloat64(ctx, &scale, input) < 0)
        return JS_EXCEPTION;
    if (!prF64IsFinite(scale))
        return JS_ThrowTypeError(ctx, "The provided float value is non-finite");
    prSvgElementSetCurrentScale(svg, float(scale));
    return JS_UNDEFINED;
}

// The handle of the point that currentTranslate returns, which reflects the translation of its element. The handle
// holds a reference to the element.
struct TranslatePoint {
    PrJsDomReflected reflected;
    PrSvgElement* svg;
    uint32_t references;
};

void translateRead(const void* handle, double out[4]) {
    PrVec2 translate = prSvgElementCurrentTranslate(static_cast<const TranslatePoint*>(handle)->svg);
    out[0] = double(translate.x);
    out[1] = double(translate.y);
    out[2] = 0;
    out[3] = 1;
}

// A point of an svg element that is not outermost is read only (SVG2/struct.html#ReadOnlyPoint).
bool translateWrite(JSContext* ctx, void* handle, const double value[4]) {
    PrSvgElement* svg = static_cast<TranslatePoint*>(handle)->svg;
    if (prSvgElementOwnerSvgElement(svg) != nullptr) {
        jsDomExceptionThrow(ctx, "NoModificationAllowedError");
        return false;
    }
    prSvgElementSetCurrentTranslate(svg, PrVec2{float(value[0]), float(value[1])});
    return true;
}

void translateRetain(void* handle) {
    ++static_cast<TranslatePoint*>(handle)->references;
}

void translateRelease(void* handle) {
    TranslatePoint* point = static_cast<TranslatePoint*>(handle);
    if (--point->references != 0)
        return;
    prDomNodeRelease(prSvgElementAsNode(point->svg));
    prMemoryFree(point);
}

constexpr PrJsDomReflection kTranslateReflection = {translateRead, translateWrite, translateRetain, translateRelease};

// currentTranslate, the same point at each read, which reflects the translation of the element's magnification and
// panning transform (SVG2/struct.html#__svg__SVGSVGElement__currentTranslate).
JSValue currentTranslate(JSContext* ctx, JSValueConst object) {
    bool outermost = false;
    PrSvgElement* svg = svgOf(ctx, object, &outermost);
    if (svg == nullptr)
        return JS_EXCEPTION;
    return jsSameObject(ctx, object, "currentTranslate", [&] {
        TranslatePoint* point = allocZeroed<TranslatePoint>();
        *point = TranslatePoint{.reflected = {&kTranslateReflection}, .svg = svg, .references = 1};
        prDomNodeRetain(prSvgElementAsNode(svg));
        JSValue value = prJsDomPointReflecting(ctx, object, &point->reflected);
        translateRelease(point);
        return value;
    });
}

// deselectAll(), which empties the selection of the element's document.
JSValue deselectAll(JSContext* ctx, JSValueConst object, int, JSValueConst*) {
    PrDomElement* element = svgElementOf(ctx, object);
    if (element == nullptr)
        return JS_EXCEPTION;
    PrDomDocument* document = prDomNodeOwnerDocument(prDomElementAsNode(element));
    if (document != nullptr)
        prDomSelectionRemoveAllRanges(prDomDocumentSelection(document));
    return JS_UNDEFINED;
}

// getElementById(elementId): the first element in the subtree of the element whose ID is `elementId`.
JSValue svgGetElementById(JSContext* ctx, JSValueConst object, int argc, JSValueConst* argv) {
    PrDomElement* element = svgElementOf(ctx, object);
    StringArgument id = {ctx};
    if (element == nullptr || !jsArgCount(ctx, argc, 1) || !id.read(argv[0]))
        return JS_EXCEPTION;
    PrDomElement* found = prDomNodeGetElementById(prDomElementAsNode(element), id.value());
    return found != nullptr ? jsWrapAs(ctx, prJsDomElementClass, object, found, false) : JS_NULL;
}

// suspendRedraw(maxWaitMilliseconds), unsuspendRedraw(suspendHandleID), unsuspendRedrawAll() and forceRedraw(), which
// do nothing (SVG2/struct.html#__svg__SVGSVGElement__suspendRedraw). suspendRedraw returns 1.
JSValue redraw(JSContext* ctx, JSValueConst object, int argc, JSValueConst* argv, int magic) {
    bool outermost = false;
    uint32_t number = 0;
    bool takes = magic == 0 || magic == 1;
    if (svgOf(ctx, object, &outermost) == nullptr || (takes && !jsArgCount(ctx, argc, 1)) ||
            (takes && JS_ToUint32(ctx, &number, argv[0]) < 0))
        return JS_EXCEPTION;
    return magic == 0 ? JS_NewUint32(ctx, 1) : JS_UNDEFINED;
}

// checkIntersection(element, rect) and checkEnclosure(element, rect) (magic 0 and 1).
JSValue checkHit(JSContext* ctx, JSValueConst object, int argc, JSValueConst* argv, int magic) {
    bool outermost = false;
    PrSvgElement* svg = svgOf(ctx, object, &outermost);
    if (svg == nullptr || !jsArgCount(ctx, argc, 2))
        return JS_EXCEPTION;
    PrSvgElement* element = jsThis<PrSvgElement>(ctx, argv[0], prJsSvgElementClass);
    PrDomRect rect{};
    if (element == nullptr || !prJsDomRectRead(ctx, argv[1], &rect))
        return JS_EXCEPTION;
    return JS_NewBool(ctx, prSvgSvgElementCheckHit(svg, element, rect, magic == 1));
}

// getIntersectionList(rect, referenceElement) and getEnclosureList(rect, referenceElement) (magic 0 and 1), as a static
// NodeList.
JSValue hitList(JSContext* ctx, JSValueConst object, int argc, JSValueConst* argv, int magic) {
    bool outermost = false;
    PrSvgElement* svg = svgOf(ctx, object, &outermost);
    PrDomRect rect{};
    if (svg == nullptr || !jsArgCount(ctx, argc, 2) || !prJsDomRectRead(ctx, argv[0], &rect))
        return JS_EXCEPTION;
    PrSvgElement* reference = nullptr;
    if (!JS_IsNull(argv[1])) {
        reference = jsThis<PrSvgElement>(ctx, argv[1], prJsSvgElementClass);
        if (reference == nullptr)
            return JS_EXCEPTION;
    }
    PrDomNodeList* list = prDomNodeListCreate();
    prSvgSvgElementGetHitList(svg, rect, reference, magic == 1, list);
    JSValue result = jsWrapAs(ctx, prJsDomNodeListClass, object, list, true);
    if (JS_IsException(result))
        prDomNodeListDestroy(list);
    return result;
}

// ---------------------------------------------------------------------------------------------------------------------
// The time container of SVGSVGElement, SVGAnimationElement and TimeEvent (svg-animations/#IDL)

// Reads argument `index` as a float (webidl/#js-float), which must be given and finite.
bool floatOf(JSContext* ctx, int argc, JSValueConst* argv, int index, float* out) {
    double number = 0;
    if (!jsArgCount(ctx, argc, index + 1) || JS_ToFloat64(ctx, &number, argv[index]) < 0)
        return false;
    if (!prF64IsFinite(number)) {
        JS_ThrowTypeError(ctx, "The time is not a finite number");
        return false;
    }
    *out = float(number);
    return true;
}

// Implements pauseAnimations (magic 0) and unpauseAnimations (1).
JSValue pauseAnimations(JSContext* ctx, JSValueConst svgObject, int, JSValueConst*, int magic) {
    PrSvgElement* svg = jsThis<PrSvgElement>(ctx, svgObject, prJsSvgSvgElementClass);
    if (svg == nullptr)
        return JS_EXCEPTION;
    if (magic == 0)
        prSvgSvgElementPauseAnimations(svg);
    else
        prSvgSvgElementUnpauseAnimations(svg);
    return JS_UNDEFINED;
}

JSValue animationsPaused(JSContext* ctx, JSValueConst svgObject, int, JSValueConst*) {
    PrSvgElement* svg = jsThis<PrSvgElement>(ctx, svgObject, prJsSvgSvgElementClass);
    return svg != nullptr ? JS_NewBool(ctx, prSvgSvgElementAnimationsPaused(svg)) : JS_EXCEPTION;
}

JSValue svgGetCurrentTime(JSContext* ctx, JSValueConst svgObject, int, JSValueConst*) {
    PrSvgElement* svg = jsThis<PrSvgElement>(ctx, svgObject, prJsSvgSvgElementClass);
    return svg != nullptr ? JS_NewFloat64(ctx, double(prSvgSvgElementGetCurrentTime(svg))) : JS_EXCEPTION;
}

JSValue svgSetCurrentTime(JSContext* ctx, JSValueConst svgObject, int argc, JSValueConst* argv) {
    PrSvgElement* svg = jsThis<PrSvgElement>(ctx, svgObject, prJsSvgSvgElementClass);
    float seconds = 0;
    if (svg == nullptr || !floatOf(ctx, argc, argv, 0, &seconds))
        return JS_EXCEPTION;
    prSvgSvgElementSetCurrentTime(svg, seconds);
    return JS_UNDEFINED;
}

PrSvgElement* PR_NULLABLE animationOf(JSContext* ctx, JSValueConst object) {
    return jsThis<PrSvgElement>(ctx, object, prJsSvgAnimationElementClass);
}

JSValue targetElement(JSContext* ctx, JSValueConst animationObject) {
    PrSvgElement* animation = animationOf(ctx, animationObject);
    if (animation == nullptr)
        return JS_EXCEPTION;
    PrSvgElement* target = prSvgAnimationElementTargetElement(animation);
    return target != nullptr ? prJsDomWrapEventTarget(ctx, JS_UNDEFINED, prSvgElementAsEventTarget(target)) : JS_NULL;
}

// Implements getStartTime, which throws an InvalidStateError for an animation with no current interval.
JSValue getStartTime(JSContext* ctx, JSValueConst animationObject, int, JSValueConst*) {
    PrSvgElement* animation = animationOf(ctx, animationObject);
    if (animation == nullptr)
        return JS_EXCEPTION;
    float time = 0;
    if (prSvgAnimationElementGetStartTime(animation, &time) != PrStatus_Ok)
        return jsDomExceptionThrow(ctx, "InvalidStateError");
    return JS_NewFloat64(ctx, double(time));
}

JSValue animationGetCurrentTime(JSContext* ctx, JSValueConst animationObject, int, JSValueConst*) {
    PrSvgElement* animation = animationOf(ctx, animationObject);
    if (animation == nullptr)
        return JS_EXCEPTION;
    return JS_NewFloat64(ctx, double(prSvgAnimationElementGetCurrentTime(animation)));
}

// Implements getSimpleDuration, which throws a NotSupportedError for an indefinite simple duration.
JSValue getSimpleDuration(JSContext* ctx, JSValueConst animationObject, int, JSValueConst*) {
    PrSvgElement* animation = animationOf(ctx, animationObject);
    if (animation == nullptr)
        return JS_EXCEPTION;
    float duration = 0;
    if (prSvgAnimationElementGetSimpleDuration(animation, &duration) != PrStatus_Ok)
        return jsDomExceptionThrow(ctx, "NotSupportedError");
    return JS_NewFloat64(ctx, double(duration));
}

// Implements beginElement (magic 0), beginElementAt (1), endElement (2) and endElementAt (3).
JSValue beginOrEndElement(JSContext* ctx, JSValueConst animationObject, int argc, JSValueConst* argv, int magic) {
    PrSvgElement* animation = animationOf(ctx, animationObject);
    float offset = 0;
    if (animation == nullptr || ((magic & 1) != 0 && !floatOf(ctx, argc, argv, 0, &offset)))
        return JS_EXCEPTION;
    if (magic < 2)
        prSvgAnimationElementBeginElementAt(animation, offset);
    else
        prSvgAnimationElementEndElementAt(animation, offset);
    return JS_UNDEFINED;
}

PrSvgTimeEvent* PR_NULLABLE timeEventOf(JSContext* ctx, JSValueConst object) {
    PrDomEvent* event = jsThis<PrDomEvent>(ctx, object, svgTimeEventClass);
    return event != nullptr ? prSvgTimeEventFromEvent(event) : nullptr;
}

JSValue timeEventDetail(JSContext* ctx, JSValueConst eventObject) {
    PrSvgTimeEvent* event = timeEventOf(ctx, eventObject);
    return event != nullptr ? JS_NewInt32(ctx, prSvgTimeEventDetail(event)) : JS_EXCEPTION;
}

// The view of a time event is the window of the document that dispatches it. No such event names one.
JSValue timeEventView(JSContext* ctx, JSValueConst eventObject) {
    return timeEventOf(ctx, eventObject) != nullptr ? JS_NULL : JS_EXCEPTION;
}

// Implements initTimeEvent(typeArg, viewArg, detailArg), whose view and detail are optional. An event keeps no view.
JSValue initTimeEvent(JSContext* ctx, JSValueConst eventObject, int argc, JSValueConst* argv) {
    PrSvgTimeEvent* event = timeEventOf(ctx, eventObject);
    StringArgument type = {ctx};
    int32_t detail = 0;
    if (event == nullptr || !jsArgCount(ctx, argc, 1) || !type.read(argv[0]))
        return JS_EXCEPTION;
    if (argc > 2 && !JS_IsUndefined(argv[2]) && JS_ToInt32(ctx, &detail, argv[2]) < 0)
        return JS_EXCEPTION;
    prSvgTimeEventInitTimeEvent(event, type.value(), detail);
    return JS_UNDEFINED;
}

// Implements setOrientToAuto and setOrientToAngle of SVGMarkerElement (magic 0 and 1)
// (SVG2/painting.html#__svg__SVGMarkerElement__setOrientToAuto).
JSValue setOrient(JSContext* ctx, JSValueConst object, int argc, JSValueConst* argv, int magic) {
    PrSvgElement* marker = jsThis<PrSvgElement>(ctx, object, prJsSvgElementClass);
    if (marker == nullptr)
        return JS_EXCEPTION;
    if (magic == 0) {
        prSvgElementSetEnumeration(marker, PrSvgAttribute_Orient, PrSvgOrient_Auto);
        return JS_UNDEFINED;
    }
    PrSvgAngle angle{};
    if (!svgAngleOf(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, &angle))
        return JS_EXCEPTION;
    prSvgElementSetAngle(marker, PrSvgAttribute_Orient, angle);
    return JS_UNDEFINED;
}

JSClassID classifyEvent(void* handle) {
    return prSvgTimeEventFromEvent(static_cast<PrDomEvent*>(handle)) != nullptr ? svgTimeEventClass : prJsDomEventClass;
}

// Implements setStdDeviation of feGaussianBlur and feDropShadow
// (filter-effects-1/#dom-svgfegaussianblurelement-setstddeviation).
JSValue setStdDeviation(JSContext* ctx, JSValueConst object, int argc, JSValueConst* argv) {
    PrSvgElement* element = jsThis<PrSvgElement>(ctx, object, prJsSvgElementClass);
    float x = 0;
    float y = 0;
    if (element == nullptr || !floatOf(ctx, argc, argv, 0, &x) || !floatOf(ctx, argc, argv, 1, &y))
        return JS_EXCEPTION;
    prSvgElementSetNumberPair(element, PrSvgAttribute_StdDeviation, x, y);
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------------------------------------------------
// The prototypes

// The members of the mixins: SVGTests (SVG2/types.html#InterfaceSVGTests), SVGFitToViewBox
// (SVG2/types.html#InterfaceSVGFitToViewBox), SVGURIReference (SVG2/types.html#InterfaceSVGURIReference),
// SVGFilterPrimitiveStandardAttributes (filter-effects-1/#InterfaceSVGFilterPrimitiveStandardAttributes) and
// SVGAnimatedPoints (SVG2/shapes.html#InterfaceSVGAnimatedPoints).
#define PR_SVG_TESTS PR_SVG_STRING_LIST(requiredExtensions), PR_SVG_STRING_LIST(systemLanguage)
#define PR_SVG_FIT_TO_VIEW_BOX PR_SVG_RECT(viewBox), PR_SVG_PRESERVE_ASPECT_RATIO(preserveAspectRatio)
#define PR_SVG_URI_REFERENCE PR_SVG_STRING(href)
#define PR_SVG_PLACED PR_SVG_LENGTH(x), PR_SVG_LENGTH(y), PR_SVG_LENGTH(width), PR_SVG_LENGTH(height)
#define PR_SVG_PRIMITIVE PR_SVG_PLACED, PR_SVG_STRING(result)
#define PR_SVG_ANIMATED_POINTS PR_SVG_POINT_LIST(points), PR_SVG_POINT_LIST(animatedPoints)

constexpr JSCFunctionListEntry kElementProto[] = {
    PR_JS_TAG("SVGElement"),
    PR_JS_GET("ownerSVGElement",
        (jsObjectGetter<&prJsSvgElementClass, prSvgElementOwnerSvgElement, &prJsDomElementClass>)),
    // The element that establishes the element's viewport, which is its nearest svg ancestor.
    PR_JS_GET("viewportElement",
        (jsObjectGetter<&prJsSvgElementClass, prSvgElementOwnerSvgElement, &prJsDomElementClass>)),
    PR_SVG_STRING(className),
    PR_JS_CSS_INLINE_STYLE,
};

constexpr JSCFunctionListEntry kGraphicsElementProto[] = {
    PR_JS_TAG("SVGGraphicsElement"),
    PR_SVG_TRANSFORM_LIST(transform),
    PR_JS_METHOD("getBBox", 0, getBBox),
    PR_JS_MAGIC("getCTM", 0, getCtm, 0),
    PR_JS_MAGIC("getScreenCTM", 0, getCtm, 1),
    PR_SVG_TESTS,
};

constexpr JSCFunctionListEntry kGeometryElementProto[] = {
    PR_JS_TAG("SVGGeometryElement"),
    PR_SVG_NUMBER(pathLength),
    PR_JS_MAGIC("isPointInFill", 0, isPointIn, 0),
    PR_JS_MAGIC("isPointInStroke", 0, isPointIn, 1),
    PR_JS_METHOD("getTotalLength", 0, getTotalLength),
    PR_JS_METHOD("getPointAtLength", 1, getPointAtLength),
};

constexpr JSCFunctionListEntry kSvgElementProto[] = {
    PR_JS_TAG("SVGSVGElement"),
    PR_SVG_PLACED,
    PR_SVG_FIT_TO_VIEW_BOX,
    PR_JS_METHOD("createSVGNumber", 0, svgCreateNumber),
    PR_JS_METHOD("createSVGLength", 0, svgCreateLength),
    PR_JS_METHOD("createSVGAngle", 0, svgCreateAngle),
    PR_JS_METHOD("createSVGPoint", 0, svgCreatePoint),
    PR_JS_METHOD("createSVGMatrix", 0, createSvgMatrix),
    PR_JS_METHOD("createSVGRect", 0, createSvgRect),
    PR_JS_METHOD("createSVGTransform", 0, svgCreateTransform),
    PR_JS_METHOD("createSVGTransformFromMatrix", 0, svgCreateTransformFromMatrix),
    PR_JS_MAGIC("pauseAnimations", 0, pauseAnimations, 0),
    PR_JS_MAGIC("unpauseAnimations", 0, pauseAnimations, 1),
    PR_JS_METHOD("animationsPaused", 0, animationsPaused),
    PR_JS_METHOD("getCurrentTime", 0, svgGetCurrentTime),
    PR_JS_METHOD("setCurrentTime", 1, svgSetCurrentTime),
    PR_JS_GET_SET("currentScale", currentScale, setCurrentScale),
    PR_JS_GET("currentTranslate", currentTranslate),
    PR_JS_MAGIC("getIntersectionList", 2, hitList, 0),
    PR_JS_MAGIC("getEnclosureList", 2, hitList, 1),
    PR_JS_MAGIC("checkIntersection", 2, checkHit, 0),
    PR_JS_MAGIC("checkEnclosure", 2, checkHit, 1),
    PR_JS_METHOD("deselectAll", 0, deselectAll),
    PR_JS_METHOD("getElementById", 1, svgGetElementById),
    PR_JS_MAGIC("suspendRedraw", 1, redraw, 0),
    PR_JS_MAGIC("unsuspendRedraw", 1, redraw, 1),
    PR_JS_MAGIC("unsuspendRedrawAll", 0, redraw, 2),
    PR_JS_MAGIC("forceRedraw", 0, redraw, 3),
};

constexpr JSCFunctionListEntry kGElementProto[] = {PR_JS_TAG("SVGGElement")};
constexpr JSCFunctionListEntry kDefsElementProto[] = {PR_JS_TAG("SVGDefsElement")};
constexpr JSCFunctionListEntry kDescElementProto[] = {PR_JS_TAG("SVGDescElement")};
constexpr JSCFunctionListEntry kMetadataElementProto[] = {PR_JS_TAG("SVGMetadataElement")};
constexpr JSCFunctionListEntry kTitleElementProto[] = {PR_JS_TAG("SVGTitleElement")};
constexpr JSCFunctionListEntry kSymbolElementProto[] = {PR_JS_TAG("SVGSymbolElement"), PR_SVG_FIT_TO_VIEW_BOX};
constexpr JSCFunctionListEntry kUseElementProto[] = {PR_JS_TAG("SVGUseElement"), PR_SVG_URI_REFERENCE, PR_SVG_PLACED};
constexpr JSCFunctionListEntry kSwitchElementProto[] = {PR_JS_TAG("SVGSwitchElement")};
constexpr JSCFunctionListEntry kStyleElementProto[] = {
    PR_JS_TAG("SVGStyleElement"),
    PR_JS_DOM_REFLECT_STRING("type", "type"),
    PR_JS_DOM_REFLECT_STRING("media", "media"),
    PR_JS_DOM_REFLECT_STRING("title", "title"),
    PR_JS_GET_SET("disabled", styleDisabled, setStyleDisabled),
    PR_JS_GET("sheet", styleSheet),
};
constexpr JSCFunctionListEntry kPathElementProto[] = {
    PR_JS_TAG("SVGPathElement"),
    PR_JS_METHOD("getPathData", 0, getPathData),
    PR_JS_METHOD("setPathData", 1, setPathData),
    PR_JS_METHOD("getPathSegmentAtLength", 1, getPathSegmentAtLength),
};

constexpr JSCFunctionListEntry kRectElementProto[] = {
    PR_JS_TAG("SVGRectElement"), PR_SVG_PLACED, PR_SVG_LENGTH(rx), PR_SVG_LENGTH(ry),
};

constexpr JSCFunctionListEntry kCircleElementProto[] = {
    PR_JS_TAG("SVGCircleElement"), PR_SVG_LENGTH(cx), PR_SVG_LENGTH(cy), PR_SVG_LENGTH(r),
};

constexpr JSCFunctionListEntry kEllipseElementProto[] = {
    PR_JS_TAG("SVGEllipseElement"), PR_SVG_LENGTH(cx), PR_SVG_LENGTH(cy), PR_SVG_LENGTH(rx), PR_SVG_LENGTH(ry),
};

constexpr JSCFunctionListEntry kLineElementProto[] = {
    PR_JS_TAG("SVGLineElement"), PR_SVG_LENGTH(x1), PR_SVG_LENGTH(y1), PR_SVG_LENGTH(x2), PR_SVG_LENGTH(y2),
};

constexpr JSCFunctionListEntry kPolylineElementProto[] = {PR_JS_TAG("SVGPolylineElement"), PR_SVG_ANIMATED_POINTS};
constexpr JSCFunctionListEntry kPolygonElementProto[] = {PR_JS_TAG("SVGPolygonElement"), PR_SVG_ANIMATED_POINTS};

constexpr JsConstant kTextContentConstants[] = {
    {"LENGTHADJUST_UNKNOWN", 0}, {"LENGTHADJUST_SPACING", 1}, {"LENGTHADJUST_SPACINGANDGLYPHS", 2},
};

constexpr JSCFunctionListEntry kTextContentElementProto[] = {
    PR_JS_TAG("SVGTextContentElement"),
    PR_SVG_LENGTH(textLength),
    PR_SVG_ENUMERATION(lengthAdjust),
    PR_JS_METHOD("getNumberOfChars", 0, getNumberOfChars),
    PR_JS_METHOD("getComputedTextLength", 0, getComputedTextLength),
    PR_JS_METHOD("getSubStringLength", 2, getSubStringLength),
    PR_JS_MAGIC("getStartPositionOfChar", 1, getPositionOfChar, 0),
    PR_JS_MAGIC("getEndPositionOfChar", 1, getPositionOfChar, 1),
    PR_JS_METHOD("getExtentOfChar", 1, getExtentOfChar),
    PR_JS_METHOD("getRotationOfChar", 1, getRotationOfChar),
    PR_JS_METHOD("getCharNumAtPosition", 0, getCharNumAtPosition),
    PR_JS_METHOD("selectSubString", 2, selectSubString),
};

constexpr JSCFunctionListEntry kTextPositioningElementProto[] = {
    PR_JS_TAG("SVGTextPositioningElement"),
    PR_SVG_LENGTH_LIST(x), PR_SVG_LENGTH_LIST(y), PR_SVG_LENGTH_LIST(dx), PR_SVG_LENGTH_LIST(dy),
    PR_SVG_NUMBER_LIST(rotate),
};

constexpr JSCFunctionListEntry kTextElementProto[] = {PR_JS_TAG("SVGTextElement")};
constexpr JSCFunctionListEntry kTSpanElementProto[] = {PR_JS_TAG("SVGTSpanElement")};

constexpr JsConstant kTextPathConstants[] = {
    {"TEXTPATH_METHODTYPE_UNKNOWN", 0}, {"TEXTPATH_METHODTYPE_ALIGN", 1}, {"TEXTPATH_METHODTYPE_STRETCH", 2},
    {"TEXTPATH_SPACINGTYPE_UNKNOWN", 0}, {"TEXTPATH_SPACINGTYPE_AUTO", 1}, {"TEXTPATH_SPACINGTYPE_EXACT", 2},
    {"TEXTPATH_SIDETYPE_UNKNOWN", 0}, {"TEXTPATH_SIDETYPE_LEFT", 1}, {"TEXTPATH_SIDETYPE_RIGHT", 2},
};

constexpr JSCFunctionListEntry kTextPathElementProto[] = {
    PR_JS_TAG("SVGTextPathElement"), PR_SVG_LENGTH(startOffset), PR_SVG_ENUMERATION(method),
    PR_SVG_ENUMERATION(spacing), PR_SVG_ENUMERATION(side), PR_SVG_URI_REFERENCE,
};

constexpr JSCFunctionListEntry kImageElementProto[] = {
    PR_JS_TAG("SVGImageElement"), PR_SVG_PLACED, PR_SVG_PRESERVE_ASPECT_RATIO(preserveAspectRatio),
    PR_JS_DOM_REFLECT_CORS_SETTINGS("crossOrigin", "crossorigin"),
    PR_SVG_URI_REFERENCE,
};

constexpr JSCFunctionListEntry kForeignObjectElementProto[] = {PR_JS_TAG("SVGForeignObjectElement"), PR_SVG_PLACED};

constexpr JsConstant kMarkerConstants[] = {
    {"SVG_MARKERUNITS_UNKNOWN", 0}, {"SVG_MARKERUNITS_USERSPACEONUSE", 1}, {"SVG_MARKERUNITS_STROKEWIDTH", 2},
    {"SVG_MARKER_ORIENT_UNKNOWN", 0}, {"SVG_MARKER_ORIENT_AUTO", 1}, {"SVG_MARKER_ORIENT_ANGLE", 2},
    {"SVG_MARKER_ORIENT_AUTO_START_REVERSE", 3},
};

constexpr JSCFunctionListEntry kMarkerElementProto[] = {
    PR_JS_TAG("SVGMarkerElement"), PR_SVG_LENGTH(refX), PR_SVG_LENGTH(refY), PR_SVG_ENUMERATION(markerUnits),
    PR_JS_DOM_REFLECT_STRING("orient", "orient"),
    PR_SVG_LENGTH(markerWidth), PR_SVG_LENGTH(markerHeight), PR_SVG_ENUMERATION(orientType), PR_SVG_ANGLE(orientAngle),
    PR_JS_MAGIC("setOrientToAuto", 0, setOrient, 0), PR_JS_MAGIC("setOrientToAngle", 1, setOrient, 1),
    PR_SVG_FIT_TO_VIEW_BOX,
};

constexpr JsConstant kGradientConstants[] = {
    {"SVG_SPREADMETHOD_UNKNOWN", 0}, {"SVG_SPREADMETHOD_PAD", 1}, {"SVG_SPREADMETHOD_REFLECT", 2},
    {"SVG_SPREADMETHOD_REPEAT", 3},
};

constexpr JSCFunctionListEntry kGradientElementProto[] = {
    PR_JS_TAG("SVGGradientElement"), PR_SVG_ENUMERATION(gradientUnits), PR_SVG_TRANSFORM_LIST(gradientTransform),
    PR_SVG_ENUMERATION(spreadMethod), PR_SVG_URI_REFERENCE,
};

constexpr JSCFunctionListEntry kLinearGradientElementProto[] = {
    PR_JS_TAG("SVGLinearGradientElement"), PR_SVG_LENGTH(x1), PR_SVG_LENGTH(y1), PR_SVG_LENGTH(x2), PR_SVG_LENGTH(y2),
};

constexpr JSCFunctionListEntry kRadialGradientElementProto[] = {
    PR_JS_TAG("SVGRadialGradientElement"), PR_SVG_LENGTH(cx), PR_SVG_LENGTH(cy), PR_SVG_LENGTH(r), PR_SVG_LENGTH(fx),
    PR_SVG_LENGTH(fy), PR_SVG_LENGTH(fr),
};

constexpr JSCFunctionListEntry kStopElementProto[] = {PR_JS_TAG("SVGStopElement"), PR_SVG_NUMBER(offset)};

constexpr JSCFunctionListEntry kPatternElementProto[] = {
    PR_JS_TAG("SVGPatternElement"), PR_SVG_ENUMERATION(patternUnits), PR_SVG_ENUMERATION(patternContentUnits),
    PR_SVG_TRANSFORM_LIST(patternTransform), PR_SVG_PLACED, PR_SVG_FIT_TO_VIEW_BOX, PR_SVG_URI_REFERENCE,
};

constexpr JSCFunctionListEntry kScriptElementProto[] = {
    PR_JS_TAG("SVGScriptElement"),
    PR_JS_DOM_REFLECT_STRING("type", "type"),
    PR_JS_DOM_REFLECT_CORS_SETTINGS("crossOrigin", "crossorigin"),
    PR_SVG_URI_REFERENCE,
};
constexpr JSCFunctionListEntry kAElementProto[] = {
    PR_JS_TAG("SVGAElement"),
    PR_SVG_STRING(target),
    PR_JS_DOM_REFLECT_STRING("download", "download"),
    PR_JS_DOM_REFLECT_USV_STRING("ping", "ping"),
    PR_JS_DOM_REFLECT_STRING("rel", "rel"),
    PR_JS_DOM_REFLECT_SUPPORTED_TOKEN_LIST("relList", "rel", &kJsHyperlinkRel),
    PR_JS_DOM_REFLECT_STRING("hreflang", "hreflang"),
    PR_JS_DOM_REFLECT_STRING("type", "type"),
    PR_JS_DOM_REFLECT_REFERRER_POLICY("referrerPolicy", "referrerpolicy"),
    PR_SVG_URI_REFERENCE,
};
constexpr JSCFunctionListEntry kViewElementProto[] = {PR_JS_TAG("SVGViewElement"), PR_SVG_FIT_TO_VIEW_BOX};

constexpr JSCFunctionListEntry kFilterElementProto[] = {
    PR_JS_TAG("SVGFilterElement"), PR_SVG_ENUMERATION(filterUnits), PR_SVG_ENUMERATION(primitiveUnits), PR_SVG_PLACED,
};

constexpr JsConstant kFeBlendConstants[] = {
    {"SVG_FEBLEND_MODE_UNKNOWN", 0}, {"SVG_FEBLEND_MODE_NORMAL", 1}, {"SVG_FEBLEND_MODE_MULTIPLY", 2},
    {"SVG_FEBLEND_MODE_SCREEN", 3}, {"SVG_FEBLEND_MODE_DARKEN", 4}, {"SVG_FEBLEND_MODE_LIGHTEN", 5},
    {"SVG_FEBLEND_MODE_OVERLAY", 6}, {"SVG_FEBLEND_MODE_COLOR_DODGE", 7}, {"SVG_FEBLEND_MODE_COLOR_BURN", 8},
    {"SVG_FEBLEND_MODE_HARD_LIGHT", 9}, {"SVG_FEBLEND_MODE_SOFT_LIGHT", 10}, {"SVG_FEBLEND_MODE_DIFFERENCE", 11},
    {"SVG_FEBLEND_MODE_EXCLUSION", 12}, {"SVG_FEBLEND_MODE_HUE", 13}, {"SVG_FEBLEND_MODE_SATURATION", 14},
    {"SVG_FEBLEND_MODE_COLOR", 15}, {"SVG_FEBLEND_MODE_LUMINOSITY", 16},
};

constexpr JSCFunctionListEntry kFeBlendElementProto[] = {
    PR_JS_TAG("SVGFEBlendElement"), PR_SVG_STRING(in1), PR_SVG_STRING(in2), PR_SVG_ENUMERATION(mode), PR_SVG_PRIMITIVE,
};

constexpr JsConstant kFeColorMatrixConstants[] = {
    {"SVG_FECOLORMATRIX_TYPE_UNKNOWN", 0}, {"SVG_FECOLORMATRIX_TYPE_MATRIX", 1}, {"SVG_FECOLORMATRIX_TYPE_SATURATE", 2},
    {"SVG_FECOLORMATRIX_TYPE_HUEROTATE", 3}, {"SVG_FECOLORMATRIX_TYPE_LUMINANCETOALPHA", 4},
};

constexpr JSCFunctionListEntry kFeColorMatrixElementProto[] = {
    PR_JS_TAG("SVGFEColorMatrixElement"), PR_SVG_STRING(in1), PR_SVG_ENUMERATION(type), PR_SVG_NUMBER_LIST(values),
    PR_SVG_PRIMITIVE,
};

constexpr JSCFunctionListEntry kFeComponentTransferElementProto[] = {
    PR_JS_TAG("SVGFEComponentTransferElement"), PR_SVG_STRING(in1), PR_SVG_PRIMITIVE,
};

constexpr JsConstant kTransferFunctionConstants[] = {
    {"SVG_FECOMPONENTTRANSFER_TYPE_UNKNOWN", 0}, {"SVG_FECOMPONENTTRANSFER_TYPE_IDENTITY", 1},
    {"SVG_FECOMPONENTTRANSFER_TYPE_TABLE", 2}, {"SVG_FECOMPONENTTRANSFER_TYPE_DISCRETE", 3},
    {"SVG_FECOMPONENTTRANSFER_TYPE_LINEAR", 4}, {"SVG_FECOMPONENTTRANSFER_TYPE_GAMMA", 5},
};

constexpr JSCFunctionListEntry kTransferFunctionElementProto[] = {
    PR_JS_TAG("SVGComponentTransferFunctionElement"), PR_SVG_ENUMERATION(type), PR_SVG_NUMBER_LIST(tableValues),
    PR_SVG_NUMBER(slope), PR_SVG_NUMBER(intercept), PR_SVG_NUMBER(amplitude), PR_SVG_NUMBER(exponent),
    PR_SVG_NUMBER(offset),
};

constexpr JSCFunctionListEntry kFeFuncAElementProto[] = {PR_JS_TAG("SVGFEFuncAElement")};
constexpr JSCFunctionListEntry kFeFuncBElementProto[] = {PR_JS_TAG("SVGFEFuncBElement")};
constexpr JSCFunctionListEntry kFeFuncGElementProto[] = {PR_JS_TAG("SVGFEFuncGElement")};
constexpr JSCFunctionListEntry kFeFuncRElementProto[] = {PR_JS_TAG("SVGFEFuncRElement")};

constexpr JsConstant kFeCompositeConstants[] = {
    {"SVG_FECOMPOSITE_OPERATOR_UNKNOWN", 0}, {"SVG_FECOMPOSITE_OPERATOR_OVER", 1}, {"SVG_FECOMPOSITE_OPERATOR_IN", 2},
    {"SVG_FECOMPOSITE_OPERATOR_OUT", 3}, {"SVG_FECOMPOSITE_OPERATOR_ATOP", 4}, {"SVG_FECOMPOSITE_OPERATOR_XOR", 5},
    {"SVG_FECOMPOSITE_OPERATOR_ARITHMETIC", 6}, {"SVG_FECOMPOSITE_OPERATOR_LIGHTER", 7},
};

constexpr JSCFunctionListEntry kFeCompositeElementProto[] = {
    PR_JS_TAG("SVGFECompositeElement"), PR_SVG_STRING(in1), PR_SVG_STRING(in2), PR_SVG_ENUMERATION(operator),
    PR_SVG_NUMBER(k1), PR_SVG_NUMBER(k2), PR_SVG_NUMBER(k3), PR_SVG_NUMBER(k4), PR_SVG_PRIMITIVE,
};

constexpr JsConstant kEdgeModeConstants[] = {
    {"SVG_EDGEMODE_UNKNOWN", 0}, {"SVG_EDGEMODE_DUPLICATE", 1}, {"SVG_EDGEMODE_WRAP", 2}, {"SVG_EDGEMODE_NONE", 3},
};

constexpr JSCFunctionListEntry kFeConvolveMatrixElementProto[] = {
    PR_JS_TAG("SVGFEConvolveMatrixElement"), PR_SVG_STRING(in1), PR_SVG_INTEGER(orderX), PR_SVG_INTEGER(orderY),
    PR_SVG_NUMBER_LIST(kernelMatrix), PR_SVG_NUMBER(divisor), PR_SVG_NUMBER(bias), PR_SVG_INTEGER(targetX),
    PR_SVG_INTEGER(targetY), PR_SVG_ENUMERATION(edgeMode), PR_SVG_NUMBER(kernelUnitLengthX),
    PR_SVG_NUMBER(kernelUnitLengthY), PR_SVG_BOOLEAN(preserveAlpha), PR_SVG_PRIMITIVE,
};

constexpr JSCFunctionListEntry kFeDiffuseLightingElementProto[] = {
    PR_JS_TAG("SVGFEDiffuseLightingElement"), PR_SVG_STRING(in1), PR_SVG_NUMBER(surfaceScale),
    PR_SVG_NUMBER(diffuseConstant), PR_SVG_NUMBER(kernelUnitLengthX), PR_SVG_NUMBER(kernelUnitLengthY),
    PR_SVG_PRIMITIVE,
};

constexpr JSCFunctionListEntry kFeDistantLightElementProto[] = {
    PR_JS_TAG("SVGFEDistantLightElement"), PR_SVG_NUMBER(azimuth), PR_SVG_NUMBER(elevation),
};

constexpr JSCFunctionListEntry kFePointLightElementProto[] = {
    PR_JS_TAG("SVGFEPointLightElement"), PR_SVG_NUMBER(x), PR_SVG_NUMBER(y), PR_SVG_NUMBER(z),
};

constexpr JSCFunctionListEntry kFeSpotLightElementProto[] = {
    PR_JS_TAG("SVGFESpotLightElement"), PR_SVG_NUMBER(x), PR_SVG_NUMBER(y), PR_SVG_NUMBER(z), PR_SVG_NUMBER(pointsAtX),
    PR_SVG_NUMBER(pointsAtY), PR_SVG_NUMBER(pointsAtZ), PR_SVG_NUMBER(specularExponent),
    PR_SVG_NUMBER(limitingConeAngle),
};

constexpr JsConstant kChannelConstants[] = {
    {"SVG_CHANNEL_UNKNOWN", 0}, {"SVG_CHANNEL_R", 1}, {"SVG_CHANNEL_G", 2}, {"SVG_CHANNEL_B", 3}, {"SVG_CHANNEL_A", 4},
};

constexpr JSCFunctionListEntry kFeDisplacementMapElementProto[] = {
    PR_JS_TAG("SVGFEDisplacementMapElement"), PR_SVG_STRING(in1), PR_SVG_STRING(in2), PR_SVG_NUMBER(scale),
    PR_SVG_ENUMERATION(xChannelSelector), PR_SVG_ENUMERATION(yChannelSelector), PR_SVG_PRIMITIVE,
};

constexpr JSCFunctionListEntry kFeDropShadowElementProto[] = {
    PR_JS_TAG("SVGFEDropShadowElement"), PR_SVG_STRING(in1), PR_SVG_NUMBER(dx), PR_SVG_NUMBER(dy),
    PR_SVG_NUMBER(stdDeviationX), PR_SVG_NUMBER(stdDeviationY), PR_JS_METHOD("setStdDeviation", 2, setStdDeviation),
    PR_SVG_PRIMITIVE,
};

constexpr JSCFunctionListEntry kFeFloodElementProto[] = {PR_JS_TAG("SVGFEFloodElement"), PR_SVG_PRIMITIVE};

constexpr JSCFunctionListEntry kFeGaussianBlurElementProto[] = {
    PR_JS_TAG("SVGFEGaussianBlurElement"), PR_SVG_STRING(in1), PR_SVG_NUMBER(stdDeviationX),
    PR_SVG_NUMBER(stdDeviationY), PR_SVG_ENUMERATION(edgeMode), PR_JS_METHOD("setStdDeviation", 2, setStdDeviation),
    PR_SVG_PRIMITIVE,
};

constexpr JSCFunctionListEntry kFeImageElementProto[] = {
    PR_JS_TAG("SVGFEImageElement"), PR_SVG_PRESERVE_ASPECT_RATIO(preserveAspectRatio), PR_SVG_STRING(crossOrigin),
    PR_SVG_PRIMITIVE, PR_SVG_URI_REFERENCE,
};

constexpr JSCFunctionListEntry kFeMergeElementProto[] = {PR_JS_TAG("SVGFEMergeElement"), PR_SVG_PRIMITIVE};
constexpr JSCFunctionListEntry kFeMergeNodeElementProto[] = {PR_JS_TAG("SVGFEMergeNodeElement"), PR_SVG_STRING(in1)};

constexpr JsConstant kFeMorphologyConstants[] = {
    {"SVG_MORPHOLOGY_OPERATOR_UNKNOWN", 0}, {"SVG_MORPHOLOGY_OPERATOR_ERODE", 1}, {"SVG_MORPHOLOGY_OPERATOR_DILATE", 2},
};

constexpr JSCFunctionListEntry kFeMorphologyElementProto[] = {
    PR_JS_TAG("SVGFEMorphologyElement"), PR_SVG_STRING(in1), PR_SVG_ENUMERATION(operator), PR_SVG_NUMBER(radiusX),
    PR_SVG_NUMBER(radiusY), PR_SVG_PRIMITIVE,
};

constexpr JSCFunctionListEntry kFeOffsetElementProto[] = {
    PR_JS_TAG("SVGFEOffsetElement"), PR_SVG_STRING(in1), PR_SVG_NUMBER(dx), PR_SVG_NUMBER(dy), PR_SVG_PRIMITIVE,
};

constexpr JSCFunctionListEntry kFeSpecularLightingElementProto[] = {
    PR_JS_TAG("SVGFESpecularLightingElement"), PR_SVG_STRING(in1), PR_SVG_NUMBER(surfaceScale),
    PR_SVG_NUMBER(specularConstant), PR_SVG_NUMBER(specularExponent), PR_SVG_NUMBER(kernelUnitLengthX),
    PR_SVG_NUMBER(kernelUnitLengthY), PR_SVG_PRIMITIVE,
};

constexpr JSCFunctionListEntry kFeTileElementProto[] =
    {PR_JS_TAG("SVGFETileElement"), PR_SVG_STRING(in1), PR_SVG_PRIMITIVE};

constexpr JsConstant kFeTurbulenceConstants[] = {
    {"SVG_TURBULENCE_TYPE_UNKNOWN", 0}, {"SVG_TURBULENCE_TYPE_FRACTALNOISE", 1}, {"SVG_TURBULENCE_TYPE_TURBULENCE", 2},
    {"SVG_STITCHTYPE_UNKNOWN", 0}, {"SVG_STITCHTYPE_STITCH", 1}, {"SVG_STITCHTYPE_NOSTITCH", 2},
};

constexpr JSCFunctionListEntry kFeTurbulenceElementProto[] = {
    PR_JS_TAG("SVGFETurbulenceElement"), PR_SVG_NUMBER(baseFrequencyX), PR_SVG_NUMBER(baseFrequencyY),
    PR_SVG_INTEGER(numOctaves), PR_SVG_NUMBER(seed), PR_SVG_ENUMERATION(stitchTiles), PR_SVG_ENUMERATION(type),
    PR_SVG_PRIMITIVE,
};

constexpr JSCFunctionListEntry kClipPathElementProto[] = {
    PR_JS_TAG("SVGClipPathElement"), PR_SVG_ENUMERATION(clipPathUnits), PR_SVG_TRANSFORM_LIST(transform),
};

constexpr JSCFunctionListEntry kMaskElementProto[] = {
    PR_JS_TAG("SVGMaskElement"), PR_SVG_ENUMERATION(maskUnits), PR_SVG_ENUMERATION(maskContentUnits), PR_SVG_PLACED,
};

constexpr JSCFunctionListEntry kAnimationElementProto[] = {
    PR_JS_TAG("SVGAnimationElement"),
    PR_JS_GET("targetElement", targetElement),
    PR_JS_DOM_EVENT_HANDLER(onbegin),
    PR_JS_DOM_EVENT_HANDLER(onend),
    PR_JS_DOM_EVENT_HANDLER(onrepeat),
    PR_JS_METHOD("getStartTime", 0, getStartTime),
    PR_JS_METHOD("getCurrentTime", 0, animationGetCurrentTime),
    PR_JS_METHOD("getSimpleDuration", 0, getSimpleDuration),
    PR_JS_MAGIC("beginElement", 0, beginOrEndElement, 0),
    PR_JS_MAGIC("beginElementAt", 1, beginOrEndElement, 1),
    PR_JS_MAGIC("endElement", 0, beginOrEndElement, 2),
    PR_JS_MAGIC("endElementAt", 1, beginOrEndElement, 3),
    PR_SVG_TESTS,
};

constexpr JSCFunctionListEntry kAnimateElementProto[] = {PR_JS_TAG("SVGAnimateElement")};
constexpr JSCFunctionListEntry kSetElementProto[] = {PR_JS_TAG("SVGSetElement")};
constexpr JSCFunctionListEntry kAnimateMotionElementProto[] = {PR_JS_TAG("SVGAnimateMotionElement")};
constexpr JSCFunctionListEntry kAnimateTransformElementProto[] = {PR_JS_TAG("SVGAnimateTransformElement")};
constexpr JSCFunctionListEntry kMPathElementProto[] = {PR_JS_TAG("SVGMPathElement"), PR_SVG_URI_REFERENCE};

// The members of SVG's partial interface of Document (SVG2/struct.html#InterfaceDocumentExtensions).
constexpr JSCFunctionListEntry kDocumentMembers[] = {PR_JS_GET("rootElement", rootElement)};

constexpr JSCFunctionListEntry kTimeEventProto[] = {
    PR_JS_TAG("TimeEvent"),
    PR_JS_GET("view", timeEventView),
    PR_JS_GET("detail", timeEventDetail),
    PR_JS_METHOD("initTimeEvent", 1, initTimeEvent),
};

// The definition of an element interface, which holds no handle of its own, with its constants or without.
#define PR_SVG_ELEMENT(name, id, proto, baseId)                                                                        \
    {PR_JS_CLASS(name, id, jsViewFinalizer<&id>, proto), .base = &baseId}
#define PR_SVG_ELEMENT_CONSTANTS(name, id, proto, baseId, constants)                                                   \
    {PR_JS_CLASS(name, id, jsViewFinalizer<&id>, proto), PR_JS_CONSTANTS(constants), .base = &baseId}

constexpr JsClassDefinition kClasses[] = {
    PR_SVG_ELEMENT("SVGElement", prJsSvgElementClass, kElementProto, prJsDomElementClass),
    PR_SVG_ELEMENT("SVGGraphicsElement", prJsSvgGraphicsElementClass, kGraphicsElementProto, prJsSvgElementClass),
    PR_SVG_ELEMENT("SVGGeometryElement", prJsSvgGeometryElementClass, kGeometryElementProto,
        prJsSvgGraphicsElementClass),
    PR_SVG_ELEMENT_CONSTANTS("SVGTextContentElement", prJsSvgTextContentElementClass, kTextContentElementProto,
        prJsSvgGraphicsElementClass, kTextContentConstants),
    PR_SVG_ELEMENT("SVGTextPositioningElement", svgTextPositioningElementClass, kTextPositioningElementProto,
        prJsSvgTextContentElementClass),
    PR_SVG_ELEMENT_CONSTANTS("SVGGradientElement", svgGradientElementClass, kGradientElementProto, prJsSvgElementClass,
        kGradientConstants),
    PR_SVG_ELEMENT_CONSTANTS("SVGComponentTransferFunctionElement", svgTransferFunctionElementClass,
        kTransferFunctionElementProto, prJsSvgElementClass, kTransferFunctionConstants),
    PR_SVG_ELEMENT("SVGAnimationElement", prJsSvgAnimationElementClass, kAnimationElementProto, prJsSvgElementClass),
    PR_SVG_ELEMENT("SVGSVGElement", prJsSvgSvgElementClass, kSvgElementProto, prJsSvgGraphicsElementClass),
    PR_SVG_ELEMENT("SVGGElement", svgGElementClass, kGElementProto, prJsSvgGraphicsElementClass),
    PR_SVG_ELEMENT("SVGDefsElement", svgDefsElementClass, kDefsElementProto, prJsSvgGraphicsElementClass),
    PR_SVG_ELEMENT("SVGDescElement", svgDescElementClass, kDescElementProto, prJsSvgElementClass),
    PR_SVG_ELEMENT("SVGMetadataElement", svgMetadataElementClass, kMetadataElementProto, prJsSvgElementClass),
    PR_SVG_ELEMENT("SVGTitleElement", svgTitleElementClass, kTitleElementProto, prJsSvgElementClass),
    PR_SVG_ELEMENT("SVGSymbolElement", prJsSvgSymbolElementClass, kSymbolElementProto, prJsSvgGraphicsElementClass),
    PR_SVG_ELEMENT("SVGUseElement", prJsSvgUseElementClass, kUseElementProto, prJsSvgGraphicsElementClass),
    PR_SVG_ELEMENT("SVGSwitchElement", prJsSvgSwitchElementClass, kSwitchElementProto, prJsSvgGraphicsElementClass),
    PR_SVG_ELEMENT("SVGStyleElement", svgStyleElementClass, kStyleElementProto, prJsSvgElementClass),
    PR_SVG_ELEMENT("SVGPathElement", svgPathElementClass, kPathElementProto, prJsSvgGeometryElementClass),
    PR_SVG_ELEMENT("SVGRectElement", svgRectElementClass, kRectElementProto, prJsSvgGeometryElementClass),
    PR_SVG_ELEMENT("SVGCircleElement", svgCircleElementClass, kCircleElementProto, prJsSvgGeometryElementClass),
    PR_SVG_ELEMENT("SVGEllipseElement", svgEllipseElementClass, kEllipseElementProto, prJsSvgGeometryElementClass),
    PR_SVG_ELEMENT("SVGLineElement", svgLineElementClass, kLineElementProto, prJsSvgGeometryElementClass),
    PR_SVG_ELEMENT("SVGPolylineElement", svgPolylineElementClass, kPolylineElementProto, prJsSvgGeometryElementClass),
    PR_SVG_ELEMENT("SVGPolygonElement", svgPolygonElementClass, kPolygonElementProto, prJsSvgGeometryElementClass),
    PR_SVG_ELEMENT("SVGTextElement", svgTextElementClass, kTextElementProto, svgTextPositioningElementClass),
    PR_SVG_ELEMENT("SVGTSpanElement", svgTSpanElementClass, kTSpanElementProto, svgTextPositioningElementClass),
    PR_SVG_ELEMENT_CONSTANTS("SVGTextPathElement", svgTextPathElementClass, kTextPathElementProto,
        prJsSvgTextContentElementClass, kTextPathConstants),
    PR_SVG_ELEMENT("SVGImageElement", prJsSvgImageElementClass, kImageElementProto, prJsSvgGraphicsElementClass),
    PR_SVG_ELEMENT("SVGForeignObjectElement", prJsSvgForeignObjectElementClass, kForeignObjectElementProto,
        prJsSvgGraphicsElementClass),
    PR_SVG_ELEMENT_CONSTANTS("SVGMarkerElement", svgMarkerElementClass, kMarkerElementProto, prJsSvgElementClass,
        kMarkerConstants),
    PR_SVG_ELEMENT("SVGLinearGradientElement", svgLinearGradientElementClass, kLinearGradientElementProto,
        svgGradientElementClass),
    PR_SVG_ELEMENT("SVGRadialGradientElement", svgRadialGradientElementClass, kRadialGradientElementProto,
        svgGradientElementClass),
    PR_SVG_ELEMENT("SVGStopElement", svgStopElementClass, kStopElementProto, prJsSvgElementClass),
    PR_SVG_ELEMENT("SVGPatternElement", svgPatternElementClass, kPatternElementProto, prJsSvgElementClass),
    PR_SVG_ELEMENT("SVGScriptElement", prJsSvgScriptElementClass, kScriptElementProto, prJsSvgElementClass),
    PR_SVG_ELEMENT("SVGAElement", prJsSvgAElementClass, kAElementProto, prJsSvgGraphicsElementClass),
    PR_SVG_ELEMENT("SVGViewElement", prJsSvgViewElementClass, kViewElementProto, prJsSvgElementClass),
    PR_SVG_ELEMENT("SVGFilterElement", svgFilterElementClass, kFilterElementProto, prJsSvgElementClass),
    PR_SVG_ELEMENT_CONSTANTS("SVGFEBlendElement", svgFeBlendElementClass, kFeBlendElementProto, prJsSvgElementClass,
        kFeBlendConstants),
    PR_SVG_ELEMENT_CONSTANTS("SVGFEColorMatrixElement", svgFeColorMatrixElementClass, kFeColorMatrixElementProto,
        prJsSvgElementClass, kFeColorMatrixConstants),
    PR_SVG_ELEMENT("SVGFEComponentTransferElement", svgFeComponentTransferElementClass,
        kFeComponentTransferElementProto, prJsSvgElementClass),
    PR_SVG_ELEMENT_CONSTANTS("SVGFECompositeElement", svgFeCompositeElementClass, kFeCompositeElementProto,
        prJsSvgElementClass, kFeCompositeConstants),
    PR_SVG_ELEMENT_CONSTANTS("SVGFEConvolveMatrixElement", svgFeConvolveMatrixElementClass,
        kFeConvolveMatrixElementProto, prJsSvgElementClass, kEdgeModeConstants),
    PR_SVG_ELEMENT("SVGFEDiffuseLightingElement", svgFeDiffuseLightingElementClass, kFeDiffuseLightingElementProto,
        prJsSvgElementClass),
    PR_SVG_ELEMENT_CONSTANTS("SVGFEDisplacementMapElement", svgFeDisplacementMapElementClass,
        kFeDisplacementMapElementProto, prJsSvgElementClass, kChannelConstants),
    PR_SVG_ELEMENT("SVGFEDistantLightElement", svgFeDistantLightElementClass, kFeDistantLightElementProto,
        prJsSvgElementClass),
    PR_SVG_ELEMENT("SVGFEDropShadowElement", svgFeDropShadowElementClass, kFeDropShadowElementProto,
        prJsSvgElementClass),
    PR_SVG_ELEMENT("SVGFEFloodElement", svgFeFloodElementClass, kFeFloodElementProto, prJsSvgElementClass),
    PR_SVG_ELEMENT("SVGFEFuncAElement", svgFeFuncAElementClass, kFeFuncAElementProto, svgTransferFunctionElementClass),
    PR_SVG_ELEMENT("SVGFEFuncBElement", svgFeFuncBElementClass, kFeFuncBElementProto, svgTransferFunctionElementClass),
    PR_SVG_ELEMENT("SVGFEFuncGElement", svgFeFuncGElementClass, kFeFuncGElementProto, svgTransferFunctionElementClass),
    PR_SVG_ELEMENT("SVGFEFuncRElement", svgFeFuncRElementClass, kFeFuncRElementProto, svgTransferFunctionElementClass),
    PR_SVG_ELEMENT_CONSTANTS("SVGFEGaussianBlurElement", svgFeGaussianBlurElementClass, kFeGaussianBlurElementProto,
        prJsSvgElementClass, kEdgeModeConstants),
    PR_SVG_ELEMENT("SVGFEImageElement", svgFeImageElementClass, kFeImageElementProto, prJsSvgElementClass),
    PR_SVG_ELEMENT("SVGFEMergeElement", svgFeMergeElementClass, kFeMergeElementProto, prJsSvgElementClass),
    PR_SVG_ELEMENT("SVGFEMergeNodeElement", svgFeMergeNodeElementClass, kFeMergeNodeElementProto, prJsSvgElementClass),
    PR_SVG_ELEMENT_CONSTANTS("SVGFEMorphologyElement", svgFeMorphologyElementClass, kFeMorphologyElementProto,
        prJsSvgElementClass, kFeMorphologyConstants),
    PR_SVG_ELEMENT("SVGFEOffsetElement", svgFeOffsetElementClass, kFeOffsetElementProto, prJsSvgElementClass),
    PR_SVG_ELEMENT("SVGFEPointLightElement", svgFePointLightElementClass, kFePointLightElementProto,
        prJsSvgElementClass),
    PR_SVG_ELEMENT("SVGFESpecularLightingElement", svgFeSpecularLightingElementClass,
        kFeSpecularLightingElementProto, prJsSvgElementClass),
    PR_SVG_ELEMENT("SVGFESpotLightElement", svgFeSpotLightElementClass, kFeSpotLightElementProto, prJsSvgElementClass),
    PR_SVG_ELEMENT("SVGFETileElement", svgFeTileElementClass, kFeTileElementProto, prJsSvgElementClass),
    PR_SVG_ELEMENT_CONSTANTS("SVGFETurbulenceElement", svgFeTurbulenceElementClass, kFeTurbulenceElementProto,
        prJsSvgElementClass, kFeTurbulenceConstants),
    PR_SVG_ELEMENT("SVGClipPathElement", svgClipPathElementClass, kClipPathElementProto, prJsSvgElementClass),
    PR_SVG_ELEMENT("SVGMaskElement", svgMaskElementClass, kMaskElementProto, prJsSvgElementClass),
    PR_SVG_ELEMENT("SVGAnimateElement", prJsSvgAnimateElementClass, kAnimateElementProto, prJsSvgAnimationElementClass),
    PR_SVG_ELEMENT("SVGSetElement", prJsSvgSetElementClass, kSetElementProto, prJsSvgAnimationElementClass),
    PR_SVG_ELEMENT("SVGAnimateMotionElement", prJsSvgAnimateMotionElementClass, kAnimateMotionElementProto,
        prJsSvgAnimationElementClass),
    PR_SVG_ELEMENT("SVGAnimateTransformElement", prJsSvgAnimateTransformElementClass, kAnimateTransformElementProto,
        prJsSvgAnimationElementClass),
    PR_SVG_ELEMENT("SVGMPathElement", svgMPathElementClass, kMPathElementProto, prJsSvgElementClass),
    {
        PR_JS_CLASS("TimeEvent", svgTimeEventClass, jsViewFinalizer<&svgTimeEventClass>, kTimeEventProto),
        .base = &prJsDomEventClass,
    },
};

// The class of each interface, by PrSvgInterface.
const JSClassID* const kInterfaceClasses[] = {
    &prJsSvgElementClass, &prJsSvgSvgElementClass, &svgGElementClass, &svgDefsElementClass, &svgDescElementClass,
    &svgMetadataElementClass, &svgTitleElementClass, &prJsSvgSymbolElementClass, &prJsSvgUseElementClass,
    &prJsSvgSwitchElementClass, &svgStyleElementClass, &svgPathElementClass, &svgRectElementClass,
    &svgCircleElementClass, &svgEllipseElementClass, &svgLineElementClass, &svgPolylineElementClass,
    &svgPolygonElementClass, &svgTextElementClass, &svgTSpanElementClass, &svgTextPathElementClass,
    &prJsSvgImageElementClass, &prJsSvgForeignObjectElementClass, &svgMarkerElementClass,
    &svgLinearGradientElementClass, &svgRadialGradientElementClass, &svgStopElementClass, &svgPatternElementClass,
    &prJsSvgScriptElementClass, &prJsSvgAElementClass, &prJsSvgViewElementClass, &svgFilterElementClass,
    &svgFeBlendElementClass, &svgFeColorMatrixElementClass, &svgFeComponentTransferElementClass,
    &svgFeCompositeElementClass, &svgFeConvolveMatrixElementClass, &svgFeDiffuseLightingElementClass,
    &svgFeDisplacementMapElementClass, &svgFeDistantLightElementClass, &svgFeDropShadowElementClass,
    &svgFeFloodElementClass, &svgFeFuncAElementClass, &svgFeFuncBElementClass, &svgFeFuncGElementClass,
    &svgFeFuncRElementClass, &svgFeGaussianBlurElementClass, &svgFeImageElementClass, &svgFeMergeElementClass,
    &svgFeMergeNodeElementClass, &svgFeMorphologyElementClass, &svgFeOffsetElementClass, &svgFePointLightElementClass,
    &svgFeSpecularLightingElementClass, &svgFeSpotLightElementClass, &svgFeTileElementClass,
    &svgFeTurbulenceElementClass, &svgClipPathElementClass, &svgMaskElementClass, &prJsSvgAnimateElementClass,
    &prJsSvgSetElementClass, &prJsSvgAnimateMotionElementClass, &prJsSvgAnimateTransformElementClass,
    &svgMPathElementClass,
};
static_assert(countOf(kInterfaceClasses) == PrSvgInterface_MPath + 1);

JSClassID classifyElement(void* handle) {
    PrDomElement* element = static_cast<PrDomElement*>(handle);
    PrSvgElement* svg = prSvgElementQuery(prDomElementAsEventTarget(element));
    if (svg == nullptr)
        return prJsDomElementClass;
    return *kInterfaceClasses[prSvgElementInterface(svg)];
}

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

void prJsSvgBind(JSContext* ctx) {
    svgTypesBind(ctx);
    for (const JsClassDefinition& definition : kClasses)
        JS_FreeValue(ctx, jsDefineClass(ctx, definition));
    jsAddClassifier(JS_GetRuntime(ctx), prJsDomElementClass, classifyElement);
    jsAddMembers(ctx, prJsDomDocumentClass, kDocumentMembers);
    jsAddClassifier(JS_GetRuntime(ctx), prJsDomEventClass, classifyEvent);
}
