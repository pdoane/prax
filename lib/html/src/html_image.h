// Images
//
// mimesniff - https://mimesniff.spec.whatwg.org/#matching-an-image-type-pattern

#pragma once

#include "pr/raw_string.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Returns true if `bytes` are an image of a type that the engine accepts, and writes the natural size that the image's
// header records. Writes zero for a dimension that the image leaves to its containing document, which an SVG image can
// do.
bool htmlImageSize(PrStringView bytes, uint32_t* width, uint32_t* height);

// Returns true if `bytes` are an SVG image. The engine draws an SVG image itself instead of passing it to the host's
// decoder.
bool htmlImageIsSvg(PrStringView bytes);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
