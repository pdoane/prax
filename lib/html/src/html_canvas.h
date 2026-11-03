// Canvas elements: a canvas element's rendering context, and how its window shows the context's bitmap.
//
// html - https://html.spec.whatwg.org/multipage/canvas.html

#pragma once

#include "pr/canvas.h"
#include "pr/dom.h"
#include "pr/html.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// The state of a canvas element (html/#the-canvas-element). The element owns the state, which is created when it is
// first needed and destroyed with the element.
struct HtmlCanvas {
    PrDomElement* element;
    PrCanvas* PR_NULLABLE context;    // the 2D context, which the state owns
    PrHtmlCanvasContextMode mode;
    PrHtmlWindow* PR_NULLABLE queued;                   // the window that will show the bitmap, or null
};

// Returns the canvas element's state, creating it if it has none.
HtmlCanvas* htmlCanvasFromElement(PrDomElement* element);

// Returns the canvas element's state, or null if it has none.
const HtmlCanvas* PR_NULLABLE htmlCanvasFind(const PrDomElement* element);

// Creates the canvas's 2D context (html/#2d-context-creation-algorithm), with a bitmap of the extent that its width and
// height attributes give. The context draws with the renderer of the window of the element's document, or with none for
// a document without a window.
PrCanvas* htmlCanvasCreateContext2D(HtmlCanvas* canvas, const PrCanvasSettings& settings);

// Implement prHtmlCanvasElementToDataUrl and prHtmlCanvasElementImageSource for a canvas element.
PrStatus htmlCanvasToDataUrl(PrDomElement* element, PrStringView type, PrRawString* out);
bool htmlCanvasImageSource(PrDomElement* element, PrCanvasImageSource* out);

// Handles a change to a canvas element: its connection, if `attribute` is the null view, or a change of that attribute.
// A change of its width or height sets its context's bitmap dimensions (html/#concept-canvas-set-bitmap-dimensions) and
// its natural size.
void htmlCanvasChanged(PrDomElement* element, PrStringView attribute);

// Shows the bitmaps of the window's canvases that changed since they were last shown, as part of a rendering update
// (html/#update-the-rendering).
void htmlCanvasShow(PrHtmlWindow* window);

// Forgets the window's canvases that wait to be shown. Called when the window's document goes away.
void htmlCanvasReset(PrHtmlWindow* window);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
