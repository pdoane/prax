// JavaScript binding of SVG.
//
// SVG2 - https://svgwg.org/svg2-draft/

#pragma once

#include "pr/svg.h"

#include "quickjs.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Defines SVGElement on the global object of `ctx`, and adds the classifier that gives it to elements in the SVG
// namespace. The DOM's binding must be defined first.
void prJsSvgBind(JSContext* ctx);

// The classes of SVGElement and the interfaces derived from it. Other modules add members to them.
extern JSClassID prJsSvgElementClass;
extern JSClassID prJsSvgGraphicsElementClass;
extern JSClassID prJsSvgGeometryElementClass;
extern JSClassID prJsSvgSvgElementClass;
extern JSClassID prJsSvgTextContentElementClass;
extern JSClassID prJsSvgAElementClass;
extern JSClassID prJsSvgUseElementClass;
extern JSClassID prJsSvgSwitchElementClass;
extern JSClassID prJsSvgSymbolElementClass;
extern JSClassID prJsSvgScriptElementClass;
extern JSClassID prJsSvgViewElementClass;
extern JSClassID prJsSvgImageElementClass;
extern JSClassID prJsSvgForeignObjectElementClass;
extern JSClassID prJsSvgAnimationElementClass;
extern JSClassID prJsSvgAnimateElementClass;
extern JSClassID prJsSvgSetElementClass;
extern JSClassID prJsSvgAnimateMotionElementClass;
extern JSClassID prJsSvgAnimateTransformElementClass;
extern JSClassID prJsSvgLengthClass;
extern JSClassID prJsSvgAnimatedLengthClass;
extern JSClassID prJsSvgAnimatedStringClass;

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
