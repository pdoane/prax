// Canvas elements: a canvas element's rendering context, and how its window shows the context's bitmap.
//
// html - https://html.spec.whatwg.org/multipage/canvas.html

#include "html/src/html_canvas.h"

#include "pr/alloc.h"
#include "pr/ascii.h"
#include "pr/base64.h"
#include "pr/image.h"
#include "pr/locale.h"
#include "pr/mem.h"
#include "pr/string_ops.h"
#include "pr/text_buffer.h"

#include "css/src/css_hints.h"
#include "css/src/css_presentation.h"
#include "css/src/css_presentation_hooks.h"
#include "css/src/css_root.h"
#include "dom/src/dom_element_state.h"
#include "dom/src/dom_microsyntax.h"
#include "dom/src/dom_node.h"
#include "html/src/html_window.h"

namespace pr_impl {

namespace {

// Returns the window of the element's document, if the window shows that document.
PrHtmlWindow* PR_NULLABLE windowOf(const PrDomElement* element) {
    PrHtmlWindow* window = static_cast<PrHtmlWindow*>(element->document->global);
    return window != nullptr && window->document == element->document ? window : nullptr;
}

void forgetCanvas(void* data) {
    HtmlCanvas* canvas = static_cast<HtmlCanvas*>(data);
    prCanvasDestroy(canvas->context);
    prMemoryFree(canvas);
}

// Queues the canvas to be shown at its window's next rendering update (PrCanvasChangedFn).
void queueCanvas(void* PR_NULLABLE data) {
    HtmlCanvas* canvas = static_cast<HtmlCanvas*>(data);
    PrHtmlWindow* window = windowOf(canvas->element);
    if (canvas->queued != nullptr || window == nullptr)
        return;
    canvas->queued = window;
    domNodeHold(canvas->element);
    window->canvases.push(canvas->element);
}

// What the canvas's context sets text with (PrCanvasTextSourceFn): the fonts and the shaper that the presentation of
// the element's document sets its text with, and the element's language and directionality. A document without a
// presentation sets no text.
bool textSourceOf(void* PR_NULLABLE data, PrCanvasTextSource* out) {
    const HtmlCanvas* canvas = static_cast<const HtmlCanvas*>(data);
    const PrDomElement* element = canvas->element;
    const PrCssPresentation* presentation = cssPresentationOf(element->document);
    if (presentation == nullptr)
        return false;
    bool rtl = domElementRtl(element);
    *out = PrCanvasTextSource{
        .fonts = presentation->fonts,
        .shaper = presentation->shaper,
        .language = prLocaleFromTag(domElementLanguage(element)),
        .direction = PrTextDirection(rtl ? PrTextDirection_Rtl : PrTextDirection_Ltr),
    };
    return true;
}

// Shows the canvas's bitmap as the content of its element: the context's presentation, which the element's painter
// draws at its revision, or nothing for a canvas without a context.
void showCanvas(HtmlCanvas* canvas) {
    DomElementState* state = domElementState(canvas->element);
    prLayoutPainterRelease(state->vector);
    state->vector = PrLayoutPainter{};
    if (canvas->context != nullptr) {
        PrCanvasPresentation presentation = prCanvasPresent(canvas->context, PrDrawSampling_Linear);
        state->vector = presentation.painter;
        state->vectorRevision = presentation.revision;
    }
    if (domIsConnected(canvas->element))
        cssPresentationElementContentChanged(canvas->element);
}

} // namespace

HtmlCanvas* htmlCanvasFromElement(PrDomElement* element) {
    DomElementState* state = domElementState(element);
    if (state->forgetEmbedded == forgetCanvas)
        return static_cast<HtmlCanvas*>(state->embedded);
    HtmlCanvas* canvas = allocZeroed<HtmlCanvas>();
    canvas->element = element;
    state->embedded = canvas;
    state->forgetEmbedded = forgetCanvas;
    return canvas;
}

const HtmlCanvas* PR_NULLABLE htmlCanvasFind(const PrDomElement* element) {
    const DomElementState* state = element->state;
    return state != nullptr && state->forgetEmbedded == forgetCanvas ? static_cast<HtmlCanvas*>(state->embedded) : nullptr;
}

PrCanvas* htmlCanvasCreateContext2D(HtmlCanvas* canvas, const PrCanvasSettings& settings) {
    PR_ASSERT(canvas->mode == PrHtmlCanvasContextMode_None);
    PrHtmlWindow* window = windowOf(canvas->element);
    PrCanvasDesc desc{
        .extent = PrUVec2{
            domReflectedUnsigned(canvas->element, "width", 300),
            domReflectedUnsigned(canvas->element, "height", 150),
        },
        .settings = settings,
        .device = window != nullptr ? window->device : nullptr,
        .element = canvas->element,
        .changed = queueCanvas,
        .changedContext = canvas,
        .textSource = textSourceOf,
        .textSourceContext = canvas,
    };
    canvas->context = prCanvasCreate(&desc);
    canvas->mode = PrHtmlCanvasContextMode_2d;
    return canvas->context;
}

PrStatus htmlCanvasToDataUrl(PrDomElement* element, PrStringView type, PrString* out) {
    *out = prStringNull();
    const HtmlCanvas* canvas = htmlCanvasFind(element);
    PrCanvas* context = canvas != nullptr ? canvas->context : nullptr;
    if (context != nullptr && !prCanvasIsOriginClean(context))
        return PrStatus_NotAllowed;
    PrUVec2 extent;
    if (context != nullptr)
        extent = prCanvasExtent(context);
    else
        extent = PrUVec2{domReflectedUnsigned(element, "width", 300), domReflectedUnsigned(element, "height", 150)};
    PrHtmlWindow* window = windowOf(element);
    PrImageCodec* codec = window != nullptr ? window->images : nullptr;
    // A bitmap without pixels, or one that cannot be serialized, is the empty data: URL (html/#dom-canvas-todataurl).
    if (extent.x == 0 || extent.y == 0 || extent.x > kPrCanvasBitmapMaxSize || extent.y > kPrCanvasBitmapMaxSize ||
            codec == nullptr) {
        *out = prStringCopy("data:,");
        return PrStatus_Ok;
    }
    Array<uint8_t> pixels;
    pixels.resize(extent.x * extent.y * 4);
    prMemZero(pixels.data, pixels.count);
    if (context != nullptr) {
        PrCanvasImageData data{
            .extent = extent,
            .format = PrCanvasPixelFormat_RgbaUnorm8,
            .colorSpace = PrColorSpace_Srgb,
        };
        prCanvasGetImageData(context, PrIVec2{0, 0}, &data, pixels.data);
    }
    PrPixelView view{pixels.data, size_t(extent.x) * 4, extent};
    // The type is a MIME type in any case, and a type that the codec does not encode is PNG
    // (html/#a-serialisation-of-the-bitmap-as-a-file).
    PrTextBuffer mime = {};
    prTextBufferAppend(&mime, type);
    prAsciiLowerText(mime.data, mime.size);
    size_t size = 0;
    if (prImageCodecEncode(codec, prTextBufferView(&mime), view, nullptr, 0, &size) != PrStatus_Ok) {
        prTextBufferReset(&mime);
        prTextBufferAppend(&mime, "image/png");
        if (prImageCodecEncode(codec, prTextBufferView(&mime), view, nullptr, 0, &size) != PrStatus_Ok) {
            prTextBufferReset(&mime);
            pixels.reset();
            *out = prStringCopy("data:,");
            return PrStatus_Ok;
        }
    }
    Array<uint8_t> file;
    file.resize(uint32_t(size));
    prImageCodecEncode(codec, prTextBufferView(&mime), view, file.data, size, &size);
    PrStringView bytes{reinterpret_cast<const char*>(file.data), size};
    PrTextBuffer url = {};
    prTextBufferAppend(&url, "data:");
    prTextBufferAppend(&url, prTextBufferView(&mime));
    prTextBufferAppend(&url, ";base64,");
    uint32_t start = url.size;
    prTextBufferResize(&url, start + uint32_t(prBase64EncodedSize(size)));
    prBase64Encode(bytes, url.data + start, url.size - start);
    *out = prTextBufferTake(&url);
    prTextBufferReset(&mime);
    file.reset();
    pixels.reset();
    return PrStatus_Ok;
}

bool htmlCanvasImageSource(PrDomElement* element, PrCanvasImageSource* out) {
    *out = PrCanvasImageSource{};
    const HtmlCanvas* canvas = htmlCanvasFind(element);
    if (canvas != nullptr && canvas->context != nullptr) {
        const PrDrawImage* image = prCanvasSnapshot(canvas->context);
        if (image == nullptr)
            return false;
        PrUVec2 extent = prCanvasExtent(canvas->context);
        *out = PrCanvasImageSource{
            .image = image,
            .extent = PrVec2{float(extent.x), float(extent.y)},
            .originClean = prCanvasIsOriginClean(canvas->context),
        };
        return true;
    }
    // A canvas without a context has a transparent bitmap of its size.
    PrUVec2 extent{domReflectedUnsigned(element, "width", 300), domReflectedUnsigned(element, "height", 150)};
    if (extent.x == 0 || extent.y == 0 || extent.x > kPrCanvasBitmapMaxSize || extent.y > kPrCanvasBitmapMaxSize)
        return false;
    Array<uint8_t> pixels;
    pixels.resize(extent.x * extent.y * 4);
    prMemZero(pixels.data, pixels.count);
    PrDrawImageDesc desc{
        .pixels = PrPixelView{pixels.data, size_t(extent.x) * 4, extent},
        .format = PrPixelFormat_Rgba8,
        .alpha = PrDrawAlpha_Premultiplied,
        .space = PrColorSpace_Srgb,
    };
    const PrDrawImage* image = nullptr;
    bool made = prDrawImageCreate(&desc, &image) == PrStatus_Ok;
    pixels.reset();
    if (!made)
        return false;
    *out = PrCanvasImageSource{.image = image, .extent = PrVec2{float(extent.x), float(extent.y)}, .originClean = true};
    return true;
}

void htmlCanvasChanged(PrDomElement* element, PrStringView attribute) {
    if (attribute != "width" && attribute != "height")
        return;
    if (const HtmlCanvas* found = htmlCanvasFind(element); found != nullptr && found->context != nullptr) {
        PrUVec2 extent{domReflectedUnsigned(element, "width", 300), domReflectedUnsigned(element, "height", 150)};
        prCanvasSetExtent(found->context, extent);
    }
    // The natural size follows the attributes, whether or not the canvas has a context.
    if (domIsConnected(element))
        cssPresentationElementContentChanged(element);
}

void htmlCanvasShow(PrHtmlWindow* window) {
    Array<PrDomElement*> canvases = window->canvases;
    window->canvases = {};
    for (uint32_t i = 0; i < canvases.count; ++i) {
        PrDomElement* element = canvases.data[i];
        HtmlCanvas* canvas = htmlCanvasFromElement(element);
        canvas->queued = nullptr;
        if (element->document == window->document)
            showCanvas(canvas);
        domNodeUnhold(element);
    }
    canvases.reset();
}

void htmlCanvasReset(PrHtmlWindow* window) {
    for (uint32_t i = 0; i < window->canvases.count; ++i) {
        PrDomElement* element = window->canvases.data[i];
        htmlCanvasFromElement(element)->queued = nullptr;
        domNodeUnhold(element);
    }
    window->canvases.reset();
}

} // namespace pr_impl
