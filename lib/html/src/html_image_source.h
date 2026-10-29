// The image source of an img element: the candidates that its srcset, and its picture's source elements, offer, and the
// candidate that it selects for the display.
//
// html/#images-processing-model - https://html.spec.whatwg.org/multipage/images.html#images-processing-model

#pragma once

#include "pr/array.h"
#include "pr/dom.h"
#include "pr/image.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

struct CssMediaEnvironment;

// A candidate of a srcset attribute (html/#image-candidate-string): its URL, and the descriptor that says how large it
// is, if any. The URL is a view of the attribute's value.
struct HtmlImageCandidate {
    PrStringView url;
    double density;                 // the pixel density descriptor (x), or 0 if it has none
    uint32_t width;                 // the width descriptor (w), or 0 if it has none
};

// Appends the candidates of a srcset attribute's value to `out`, skipping those whose descriptors are in error
// (html/#parse-a-srcset-attribute).
void htmlParseSrcset(PrStringView value, Array<HtmlImageCandidate>* out);

// What an img element's source is selected for: the environment that media conditions are evaluated against, the
// display's device pixels per CSS pixel, the viewport's width and the initial font size in CSS pixels, which a sizes
// attribute's lengths resolve against, and the codec whose types an image can be.
struct HtmlImageEnvironment {
    const CssMediaEnvironment* PR_NULLABLE media;
    double pixelRatio;
    double viewportWidth;
    double fontSize;
    PrImageCodec* PR_NULLABLE images;
};

// Returns the source size that a sizes attribute's value gives, in CSS pixels (html/#parse-a-sizes-attribute): the
// length of the first entry whose media condition matches. An absent or invalid value gives 100vw.
double htmlSourceSize(PrStringView value, const HtmlImageEnvironment& environment);

// The image source that an img element selects: the URL, a view of an attribute of the img or of a source element, or
// the null view if it has none, and the image's pixel density.
struct HtmlImageSource {
    PrStringView url;
    double density;
};

// Selects the image source of an img element (html/#update-the-source-set, html/#select-an-image-source). The source
// set is that of the first source element before the img in its picture parent that has candidates, whose media matches
// and whose type is one that the environment decodes, or else the img's own srcset and src. Of the set's candidates,
// the one with the lowest density at least the display's pixel ratio is selected, or else the one with the highest
// density.
HtmlImageSource htmlSelectImageSource(const PrDomElement* img, const HtmlImageEnvironment& environment);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
