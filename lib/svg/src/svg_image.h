// SVG images: SVG documents shown as images (svg-integration/#secure-static-mode), drawn at whatever size they are
// shown.
//
// SVG2 - https://svgwg.org/svg2-draft/embedded.html#ImageElement

#pragma once

#include "pr/atomic.h"
#include "pr/string_view.h"
#include "pr/svg_render.h"

#include "svg/src/svg_draw.h"
#include "svg/src/svg_tree.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// An SVG image: the tree of a document that has no presentation, built once and detached from the document. The tree is
// drawn and never changed, so the image is safe on any thread.
struct SvgImage {
    mutable Atomic<uint32_t> references;
    SvgTree tree;
    PrSvgNaturalSize natural;
    PrColor background;         // the root's background color, which the canvas takes and the image fills with

    // The view that the image is drawn with: the root's viewBox and preserveAspectRatio, or those that the fragment of
    // the URL that named the image gives. The root's viewport clips.
    SvgRootView view;

    bool overflows;             // the root's overflow is visible, which an image element does not clip to its viewport

    // The size that the content is drawn at and then scaled from, for an image with a width and a height but no
    // viewBox, which scales as if its viewBox were its natural size; zero for any other.
    PrVec2 scaledFrom;
};

// Writes the view that `fragment`, the fragment of a URL without its `#`, names in the document of the outermost svg
// element `root` to `view`, which holds the root's own view, and returns the element that the fragment names, or null
// (SVG2/linking.html#SVGFragmentIdentifiers). svgView() and a media fragment's xywh give a viewBox, svgView() a
// preserveAspectRatio, and the ID of a view element its attributes. Any other fragment leaves the view as it is.
const PrDomElement* PR_NULLABLE svgImageView(PrStringView fragment, const PrDomElement* root, SvgRootView* view);

// Draws the image over the rectangle from the origin to `extent`, as CSS shows an image: its view placed in that size.
void svgImageDraw(const SvgImage* image, PrVec2 extent, PrDrawList* list);

// Draws the image as an image element shows an SVG image (SVG2/embedded.html#ImageElement): `area` is the
// image-rendering rectangle, which is the image's viewport, and the root's preserveAspectRatio is ignored, as if it
// were none. A root with a width and a height but no viewBox takes them as its viewBox (w3c/svgwg#249). The content is
// clipped to `area` unless the root's overflow is visible.
void svgImageDrawRendering(const SvgImage* image, PrRect area, PrDrawList* list);

// Adds or releases a reference. The last release frees the image.
void svgImageRetain(const SvgImage* image);
void svgImageRelease(const SvgImage* PR_NULLABLE image);

// Returns a painter that draws the image. The painter's retain and release add and release references to the image.
PrLayoutPainter svgImagePainter(const SvgImage* image);

// Returns the image that `painter` draws, if it is an SVG image's painter (svgImagePainter), or null.
const SvgImage* PR_NULLABLE svgImageOfPainter(PrLayoutPainter painter);

} // namespace pr_impl

// C opaque types
struct PrSvgImage : pr_impl::SvgImage {};

PR_ASSUME_NONNULL_END
