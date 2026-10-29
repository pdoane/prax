// SVG filters: filter elements as resources of the SVG tree, and an element's filter list as a lib/draw filter graph.
//
// filter-effects-1 - https://drafts.fxtf.org/filter-effects-1/
//
// A filter element is built once, with its primitives' attributes and the properties of their styles, and becomes nodes
// of a graph where an element that it applies to is drawn, in that element's user space and for its bounding box. A
// filter list joins those nodes with the nodes of its filter functions (css_filter.h), each item reading the result of
// the one before it.

#pragma once

#include "pr/draw_filter.h"

#include "svg/src/svg_draw.h"
#include "svg/src/svg_tree.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Builds the filter resource `index` from its element `e` (filter-effects-1/#FilterElement): its region and its
// primitives, with the properties of each primitive's style. The elements that feImage primitives reference are built
// in the filter's content group.
void svgBuildFilter(SvgBuilder& b, SvgResourceIndex index, const PrDomElement* e);

// Frees what a filter holds outside the tree's arrays: its primitives, their numbers and merge inputs, and the pictures
// of its images. The content group is the tree's to free.
void svgFilterFree(SvgFilter& filter);

// Returns true if two filters draw the same, apart from the content of their content groups.
bool svgFilterSame(const SvgFilter& a, const SvgFilter& b);

// Returns true if a primitive of the filter reads what lies beneath what the filter applies to: BackgroundImage or
// BackgroundAlpha.
bool svgFilterReadsBackdrop(const SvgFilter& filter);

// What an element's filter list does to it.
enum {
    SvgFiltered_None,           // no filter applies
    SvgFiltered_Filter,         // a filter applies
    SvgFiltered_Hidden,         // the element is not rendered
};
typedef uint8_t SvgFiltered;

// The graph of an element's filter list, and the area that it may draw into in the element's user space, or an empty
// area for anywhere. The holder holds a reference to the graph.
struct SvgFilterGraph {
    const PrDrawFilter* PR_NULLABLE filter;
    PrRect region;
};

// Makes the filter list of the node `index` a graph in the drawer's user space (filter-effects-1/#FilterProperty), and
// writes it to `out` if a filter applies. A filter element of the list hides the element if it has no primitives, if
// its region has no area, or if it applies inside its own feImage content or nests too deep, which is an error.
SvgFiltered svgFilterFromNode(SvgDrawer& d, SvgNodeIndex index, SvgFilterGraph* out);

// Appends to `builder` the nodes of the filter `index`, for what has no node in `tree` and whose object bounding box is
// `box`, reading `input` as its SourceGraphic. Writes the node of its result and its region. Returns false if the
// filter hides what it applies to (svgFilterFromNode). Percentages in user space are of `viewport`.
bool svgAppendFilter(const SvgTree* tree, SvgResourceIndex index, PrRect box, PrVec2 viewport,
                     PrDrawFilterInput input, PrDrawFilterBuilder* builder, PrDrawFilterInput* result,
                     PrRect* region);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
