// What SVG keeps for each document that it is enabled for (prSvgEnable): the changes it follows, its SVG fonts and its
// animations.

#pragma once

#include "pr/dom.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

struct SvgAnimations;
struct SvgDocumentFonts;

// Starts following `document`. Returns false if SVG follows it already.
bool svgDocumentFollow(PrDomDocument* document);

// Returns the SVG fonts of `document`, which SVG is enabled for by this call if it was not already.
SvgDocumentFonts* svgDocumentFonts(const PrDomDocument* document);

// Returns the animations of `document`, or null if SVG does not follow it.
SvgAnimations* PR_NULLABLE svgDocumentAnimations(const PrDomDocument* document);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
