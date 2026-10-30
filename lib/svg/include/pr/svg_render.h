// Rendering SVG: an svg element's content drawn into a draw list.
//
// SVG2 - https://svgwg.org/svg2-draft/render.html

#pragma once

#include "pr/base.h"
#include "pr/color.h"
#include "pr/draw_list.h"
#include "pr/font_set.h"
#include "pr/layout_tree.h"
#include "pr/status.h"
#include "pr/string_view.h"
#include "pr/svg.h"
#include "pr/text.h"
#include "pr/vec.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// An SVG document shown as an image (svg-integration/#secure-static-mode), drawn at whatever size the image is shown:
// its viewBox and preserveAspectRatio place the content in that size, and its root's background color, which is the
// canvas's, fills it. An image is immutable and reference counted. It is safe to draw and release on any thread.
typedef struct PrSvgImage PrSvgImage;

// Loads the document that a reference in `from` to another document names, such as a use element's href into another
// file (SVG2/linking.html#processingURL). `url` is the reference without its fragment, resolved against the base URL of
// `from`, or as written if it does not resolve, as a relative reference does not against about:blank. Returns null if
// the document cannot be loaded. The host keeps the returned document alive until the call returns, and the draw takes
// a reference to it. The document is loaded once for each URL.
typedef PrDomDocument* PR_NULLABLE (*PrSvgLoadFn)(void* PR_NULLABLE context, PrDomDocument* from, PrStringView url);

// An image that a reference names, as the host decodes it: a raster image or an SVG image. Exactly one of the two is
// set.
typedef struct PrSvgLoadedImage {
    const PrDrawImage* PR_NULLABLE raster;  // arranged as its orientation displays it
    const PrSvgImage* PR_NULLABLE svg;      // created with the fragment of the reference (prSvgImageCreate)
} PrSvgLoadedImage;

// Loads the image that a reference in `from` names, such as an image element's href. `url` is the reference as written,
// with its fragment, and the host resolves it against the origin of `from`. Writes the image, with a reference for the
// caller, and returns true, or returns false if nothing loads or the bytes are not an image that the host decodes.
typedef bool (*PrSvgLoadImageFn)(void* PR_NULLABLE context, PrDomDocument* from, PrStringView url, PrSvgLoadedImage* out);

// A custom property that the host gives every element, such as a palette entry of an OpenType SVG glyph. `name` starts
// with two dashes.
typedef struct PrSvgCustomProperty {
    PrStringView name;
    PrStringView value;
} PrSvgCustomProperty;

// What the host provides to a draw: its services, and the context that the draw renders into.
typedef struct PrSvgDrawOptions {
    // The families that text is set in, beyond the document's own SVG fonts. The set's aliases and default family
    // apply.
    PrFontSet* PR_NULLABLE fonts;

    PrTextShaper* PR_NULLABLE shaper;       // shapes text in `fonts`; without one, only SVG fonts draw

    // The functions that load the documents and the images that references name, called with `loadContext`. Without a
    // function, nothing of its kind loads.
    PrSvgLoadFn PR_NULLABLE load;
    PrSvgLoadImageFn PR_NULLABLE loadImage;
    void* PR_NULLABLE loadContext;

    // The values that var() resolves to. Any other name takes the fallback of its var().
    const PrSvgCustomProperty* PR_NULLABLE properties;
    uint32_t propertyCount;

    PrColor color;                          // the initial value of the color property, if `hasColor` is set
    bool hasColor;

    // The HDR headroom of the display, in stops, which colors that depend on it resolve for (css-color-hdr-1). Zero is
    // an SDR display.
    float headroom;
} PrSvgDrawOptions;

// Returns options with no services, no custom properties, and the initial value of the color property.
PrSvgDrawOptions prSvgDrawOptionsDefault(void);

// Draws the content of the svg element `svg` into `list`, in a viewport whose width and height are `viewport` in CSS
// pixels. The top left of the viewport is the origin of the list's current transform. The element's own width, height,
// viewBox and preserveAspectRatio place the content in the viewport (SVG2/coords.html). Both axes of `viewport` are
// zero or more. The list's state is the same after the call as before. Null options mean the defaults.
void prSvgElementDraw(PrSvgElement* svg, PrVec2 viewport, const PrSvgDrawOptions* PR_NULLABLE options, PrDrawList* list);

// Draws the content of the outermost svg element `svg` into `list`, as CSS lays it out: its width and height are the
// CSS width and height of its box, so the content box, whose width and height are `extent` in CSS pixels, is its
// viewport (SVG2/geometry.html#Sizing). The box's top left is the origin of the list's current transform. The element's
// viewBox and preserveAspectRatio place the content in the viewport. The box applies the element's own opacity and
// display, so the drawing leaves them out. Null options mean the defaults.
void prSvgElementDrawBox(PrSvgElement* svg, PrVec2 extent, const PrSvgDrawOptions* PR_NULLABLE options, PrDrawList* list);

// The natural dimensions of an outermost svg element in CSS (css-images-3/#natural-dimensions), in CSS pixels. A width
// or height is negative if the element has none, and a ratio of width over height is zero if it has none.
typedef struct PrSvgNaturalSize {
    float width;
    float height;
    float ratio;
} PrSvgNaturalSize;

// Returns the natural dimensions of the outermost svg element `svg` (SVG2/coords.html#SizingSVGInCSS). Its width and
// height attributes give its width and height if they are absolute lengths. Its width and height give its ratio if it
// has both, and its viewBox gives it otherwise.
PrSvgNaturalSize prSvgElementNaturalSize(const PrSvgElement* svg);

// Creates an image of the outermost svg element `svg`, the root of its own document, styled by that document's sheets.
// `fragment` is the fragment of the URL that names the image, without its `#`, or the null view for none. A fragment
// that names a view (SVG2/linking.html#SVGFragmentIdentifiers), by svgView(), by a media fragment's xywh or by the ID
// of a view element, gives the viewBox and the preserveAspectRatio that the image is drawn with, and its ratio unless
// its width and height give one. The element that the fragment names is the document's target
// (selectors-4/#the-target-pseudo). The image keeps nothing of the document, which the caller may release at once. A
// host that loads the image's own images through `options` loads none but data: URLs, as secure static mode requires.
// Null options mean the defaults.
const PrSvgImage* prSvgImageCreate(PrSvgElement* svg, PrStringView fragment, const PrSvgDrawOptions* PR_NULLABLE options);

// Adds or releases a reference. The last release frees the image.
void prSvgImageRetain(const PrSvgImage* image);
void prSvgImageRelease(const PrSvgImage* PR_NULLABLE image);

// Returns the natural dimensions of the image's root (prSvgElementNaturalSize).
PrSvgNaturalSize prSvgImageNaturalSize(const PrSvgImage* image);

// Returns a painter that draws the image. The painter's retain and release add and release references to the image.
PrLayoutPainter prSvgImagePainter(const PrSvgImage* image);

// Draws the image into `list` over the rectangle from the origin to `extent`, in CSS pixels.
void prSvgImageDraw(const PrSvgImage* image, PrVec2 extent, PrDrawList* list);

// Draws `element` and its descendants into `list`, without the rest of the document. They are drawn in the user space
// of the element's parent, whose origin is the origin of the list's current transform. The element is styled as it is
// in its document, and its references resolve in its document. Percentages resolve against a viewport whose width and
// height are `viewport`. An OpenType SVG glyph is drawn this way. Null options mean the defaults.
void prSvgElementDrawInPlace(PrSvgElement* element, PrVec2 viewport,
    const PrSvgDrawOptions* PR_NULLABLE options, PrDrawList* list);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
