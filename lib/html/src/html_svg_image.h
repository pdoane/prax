// SVG images (svg-integration/#secure-static-mode, svg-integration/#secure-animated-mode)
//
// An SVG image draws differently in each environment that it reads: the color scheme of the element that shows it, the
// media features of the embedding document, and the size that it is laid out at (DomImageVariants). A variant is built
// from the image's bytes the first time that an environment asks for one, and environments that differ only in what the
// image did not read share a variant. An image that reads its size is built again at each size that it is drawn at.
//
// An SVG image whose document animates, by SMIL or by CSS animations and transitions, is a document of its own, shown
// by a page of its own whose documents are sandboxed of every capability, as an iframe's can be: no script runs,
// nothing navigates, nothing but a data: URL loads, and no input reaches it. Each image is a placement of its own, with
// its own document, presentation and timeline. The page that shows the image runs the image's rendering updates in its
// own (html/#update-the-rendering), at the image's time: the time since the image's first update, from the start of its
// URL's media fragment, and held at that fragment's end. Each update lays the image's document out and records its
// scene. The image's painter plays the last scene, with its composited animations sampled at the image's time, placed
// in the size that the image is shown at; its revision (PrLayoutPainter::revision) changes when what it plays does.
// Until its first update, and for an image that does not animate, the painter draws the image's document as it is
// (pr/svg_render.h).

#pragma once

#include "pr/html_page.h"
#include "pr/layout_style.h"
#include "pr/string_view.h"
#include "pr/svg_element.h"

#include "dom/src/dom_node_base.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Returns the natural dimensions of the outermost svg element `svg` in layout units (prSvgElementNaturalSize).
PrLayoutNaturalSize htmlSvgNaturalSize(const PrSvgElement* svg);

// Returns the variants of the SVG image `bytes`, named by the absolute URL `url`, that `page` shows, with a reference
// for the caller, and writes its natural dimensions. Each variant's document prefers the color scheme of its
// environment (css-color-adjust-1/#page-color-scheme). With `rendering`, the image shows as an image element shows it
// (svgImageDrawRendering). Returns no variants if the bytes do not parse as an SVG document.
DomImageVariants htmlSvgImageVariants(PrHtmlPage* page, PrStringView bytes, PrStringView url, bool rendering,
    PrLayoutNaturalSize* natural);

// Returns true if an animated image that `page` shows wants a rendering update: its first, or one that its animations
// or events ask for, before the end of its media fragment.
bool htmlSvgImagesWantFrame(const PrHtmlPage* page);

// Runs the rendering update of each animated image that `page` shows, at the time of the page's update, `now` on the
// page's clock, and forgets each image that nothing shows any more. Returns true if what a painter plays changed.
bool htmlSvgImagesFrame(PrHtmlPage* page, double now);

// Destroys the pages of the animated images that `page` shows, as `page` is destroyed. Their painters draw their last
// scenes from then on.
void htmlSvgImagesReset(PrHtmlPage* page);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
