// The cascade of a document that has no presentation: an SVG image, an OpenType SVG glyph's document, or a document
// that a harness draws.
//
// css-cascade-5 - https://drafts.csswg.org/css-cascade-5/
//
// Such a document does not change while it is drawn, so its styles are computed once, as they are first needed, and
// kept. An element's style needs its ancestors' styles, which are computed first. Drawing one element of a large
// document, as a glyph does, styles only that element's ancestors and subtree.

#pragma once

#include "pr/array.h"
#include "pr/dom.h"
#include "pr/hash_index.h"
#include "pr/svg_render.h"

#include "css/src/css_cascade.h"
#include "css/src/css_style.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// An element's computed style, which the cascade holds a reference to.
struct SvgCascadeEntry {
    const PrDomElement* element;
    const CssComputedStyle* style;
};

// The rules of a document and the styles computed from them.
struct SvgCascade {
    PrDomDocument* document;
    Array<CssSheetData*> userAgent;     // SVG's sheet, then the host's rule for the root
    CssRuleSet userAgentRules;
    CssRuleSet rules;
    CssStyleTable table;
    CssStyleBuilder builder;
    CssCascadeScratch scratch;
    CssViewport viewport;               // what viewport-relative lengths resolve against
    Array<SvgCascadeEntry> styles;
    KeyIndex index;                     // `styles` by the address of their elements
    CssFontBases rootFont;              // the root's font bases, known once the root is styled
};

// Prepares the cascade of `document`, with its sheets as they are now. The root inherits the host's color and custom
// properties from `options`, and a document without a browsing context takes the documents that its references name
// from the loader of `options` (dom_resource.h). Viewport-relative lengths are relative to `viewport`. The cascade
// holds no reference to `document`, which must outlive it.
void svgCascadeInit(SvgCascade* cascade, PrDomDocument* document, const PrSvgDrawOptions& options, PrVec2 viewport);

// Frees what the cascade holds.
void svgCascadeReset(SvgCascade* cascade);

// Returns the computed style of `element`, an element of the document, computing it and its ancestors' styles if they
// are not yet computed. The cascade holds the reference.
const CssComputedStyle* svgCascadeStyle(SvgCascade* cascade, const PrDomElement* element);

// Returns the style of `element` matched where it is, but inheriting from `parent`, as an element of a use element's
// shadow tree inherits (SVG2/struct.html#UseElement). `element` may be outside the document, as such a tree's elements
// are. The caller holds one reference.
const CssComputedStyle* svgCascadeStyleUnder(SvgCascade* cascade, const PrDomElement* element,
                                             const CssComputedStyle* PR_NULLABLE parent);

// Returns the style of the ::first-line or ::first-letter `pseudo` of `element`, inheriting from `parent`, or null if
// no rule styles it. The caller holds one reference.
const CssComputedStyle* PR_NULLABLE svgCascadePseudoStyleUnder(SvgCascade* cascade, const PrDomElement* element,
                                                               CssPseudo pseudo,
                                                               const CssComputedStyle* PR_NULLABLE parent);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
