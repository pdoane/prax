// Filters: the items of a computed <filter-value-list>, as layout takes them, and blend modes, as lib/draw takes them.
//
// filter-effects-1 - https://drafts.fxtf.org/filter-effects-1/
// compositing-1 - https://drafts.fxtf.org/compositing-1/
//
// A filter function resolves its arguments when its element's style is read. It becomes nodes of a filter graph where
// it is drawn (prLayoutFilterAppend), in the space of the content that it filters. The user of a reference to a filter
// element resolves the reference: the SVG tree for SVG content, and the presentation for an HTML box.

#pragma once

#include "pr/array.h"
#include "pr/draw.h"
#include "pr/draw_filter.h"
#include "pr/layout_style.h"
#include "pr/string_view.h"

#include "css/src/css_style.h"
#include "css/src/css_value.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// An item of a computed <filter-value-list>, with its arguments resolved and omitted arguments given their defaults
// (filter-effects-1/#computed-values-of-filter-functions), as layout takes it. A reference to a filter element has kind
// Source, no source, and its URL in `url`.
struct CssFilterFunction {
    PrLayoutFilter filter;
    PrStringView url;           // Source: the URL, in the text of the computed value
};

// Appends the items of the computed value of `filter`, `run`, to `out`, with the color of drop-shadow() resolved in
// `colors`. none appends nothing.
void cssFilterFunctions(CssValueRun run, const CssColorContext& colors, Array<CssFilterFunction>* out);

// Writes the blend mode that the <blend-mode> keyword `keyword` names to `out` (compositing-1/#ltblendmodegt). Returns
// false for any other keyword.
bool cssBlendModeFromKeyword(PrCssKeyword keyword, PrDrawBlendMode* out);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
