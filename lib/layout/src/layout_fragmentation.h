// Fragmentation: the fragmentainer that a box is laid out in, the break tokens that resume a box in the next one, and
// the break values that decide where breaks go (css-break-4).
//
// css-break-4 - https://drafts.csswg.org/css-break-4/
// css-page-3 - https://drafts.csswg.org/css-page-3/
//
// A box laid out in a fragmentation context is given its fragmentainer in its space. A box that does not finish in it
// gives back a break token with its fragment, and the next fragmentainer lays the box out again from the token.

#pragma once

#include "pr/atomic.h"
#include "pr/layout_page.h"
#include "pr/layout_style.h"

#include "layout/src/layout_index.h"
#include "layout/src/layout_line.h"
#include "layout/src/layout_logical.h"
#include "layout/src/layout_unit.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

struct LayoutStyles;
struct LayoutTree;

// The kind of a fragmentation context, which decides the breaks that its break values force or avoid
// (css-break-4/#break-types).
enum {
    LayoutFragmentainerKind_Column,
    LayoutFragmentainerKind_Page,
};
typedef uint8_t LayoutFragmentainerKind;

// The fragmentainer that a box is laid out in (LayoutFragmentation::fragmentainer): its block size, and where it ends
// in the block coordinates that the box is placed in. For a box in flow in a block formatting context, those are the
// context's coordinates. For any other box, they run from the box's border box. `atTop` is true if nothing comes before
// the box in the fragmentainer, so that the box places something even if it does not fit.
struct LayoutFragmentainer {
    PrLayoutUnit blockSize;
    PrLayoutUnit end;
    LayoutFragmentainerKind kind;
    bool atTop;
};

// Returns `fragmentainer` moved into coordinates whose origin is `at` in its own, with `atTop` as given.
inline LayoutFragmentainer layoutFragmentainerMoved(const LayoutFragmentainer& fragmentainer, PrLayoutUnit at, bool atTop) {
    return LayoutFragmentainer{fragmentainer.blockSize, fragmentainer.end - at, fragmentainer.kind, atTop};
}

struct LayoutBreakToken;

// What a fragmentation context gives a box laid out in it (LayoutSpace::fragmentation): the fragmentainer, and the
// token that the box resumes from, or null for its first fragment.
struct LayoutFragmentation {
    const LayoutFragmentainer* fragmentainer;
    const LayoutBreakToken* PR_NULLABLE resume;
};

// The side of the spread that a forced page break asks the next page to be on (css-break-4/#page-break-values).
enum {
    LayoutBreakSide_Any,
    LayoutBreakSide_Left,
    LayoutBreakSide_Right,
    LayoutBreakSide_Recto,
    LayoutBreakSide_Verso,
};
typedef uint8_t LayoutBreakSide;

// Where a box that a break crossed resumes in the next fragmentainer. `consumed` is the block size that the box's
// earlier fragments took. `child` is the child to lay out next, or none if only the rest of the box's own block size is
// left, and `childToken` is that child's token if the break crossed the child too. A child that is the first node of an
// inline formatting context resumes in segment `segment` of it, at the line that starts at `line`. A token is immutable
// and reference counted.
//
// A box that placed none of its content before the break has a token `atStart`. Its break falls between its block-start
// edge and its first content, which is a break before the box (css-break-4/#possible-breaks). A parent that has placed
// content in the fragmentainer pushes such a box to the next one instead, so that the edges of a box, or an outside
// marker, never end a fragmentainer alone.
struct LayoutBreakToken {
    mutable Atomic<uint32_t> references;
    LayoutNodeIndex node;
    PrLayoutUnit consumed;
    LayoutNodeIndex child;
    const LayoutBreakToken* PR_NULLABLE childToken;
    const LayoutBreakToken* PR_NULLABLE sibling; // the next of a list of tokens, such as those of a row's cells
    const LayoutBreakToken* PR_NULLABLE floats;  // for a formatting context root, the tokens of its floats that resume,
                                                 // linked by `sibling`
    PrLayoutUnit floatLeft;                      // for a float that resumes, the line-left edge of its margin box
    bool pushed;                                 // true if a float starts whole in the next fragmentainer
    LayoutLineStart line;
    uint32_t segment;
    bool inLine;
    bool atStart;
    bool forced;                    // a forced break ends the fragmentainer before the token
    LayoutBreakSide side;           // the side of the spread that a forced page break asks for

    // The start page value of the box after the break, if the break falls between siblings in block flow. A break
    // inside a box, such as between lines, leaves the page's name as it was.
    bool hasPage;
    PrLayoutName page;
};

// Creates a token with one reference for `node`. It takes `childToken`, a reference or null.
LayoutBreakToken* layoutBreakTokenCreate(LayoutNodeIndex node, PrLayoutUnit consumed, LayoutNodeIndex child,
    const LayoutBreakToken* PR_NULLABLE childToken);

// Adds or releases a reference. The last release frees the token and releases its child's and its sibling's.
void layoutBreakTokenRetain(const LayoutBreakToken* token);
void layoutBreakTokenRelease(const LayoutBreakToken* PR_NULLABLE token);

// What break values ask of a break opportunity in a context of one kind.
enum {
    LayoutBreakAsk_Auto,
    LayoutBreakAsk_Avoid,
    LayoutBreakAsk_Force,
};
typedef uint8_t LayoutBreakAsk;

// Returns what the break-after of `before` and the break-before of `after`, siblings in flow, ask of the opportunity
// between them in a context of `kind`, each with the values that propagate to it from its last or first child
// (css-break-4/#break-propagation). Sets `*side` to the side that a forced page break asks for. In paged media, a break
// is forced between siblings if the end page value of `before` differs from the start page value of `after`, under a
// parent whose used page value is `page` (css-page-3/#using-named-pages). Either sibling may be kPrLayoutNodeNone,
// which stands for an anonymous block box of inline content: its break values are auto and its used page value is
// `page`.
LayoutBreakAsk layoutBreakBetween(const LayoutTree* tree, LayoutNodeIndex before, LayoutNodeIndex after,
    LayoutFragmentainerKind kind, PrLayoutName page, LayoutBreakSide* side);

// Returns the used page value of `node`: its own, or that of its nearest ancestor whose value is not auto, or the empty
// name (css-page-3/#using-named-pages).
PrLayoutName layoutUsedPage(const LayoutTree* tree, LayoutNodeIndex node);

// Returns the start page value of `node`, whose parent's used page value is `page`: the used value of its first
// block-level child at the greatest depth, or its own (css-page-3/#using-named-pages). A page that starts with the box
// is of that name.
PrLayoutName layoutStartPage(const LayoutTree* tree, LayoutNodeIndex node, PrLayoutName page);

// Returns the first page of `root` laid out as pages (prLayoutNodeStartPage).
PrLayoutPageContext layoutStartPageContext(const LayoutTree* tree, LayoutNodeIndex root);

// Returns true if the run of inline-level children of `parent` from `first` through `last` generates a line box, which
// an anonymous block box holds among block-level siblings. A run generates one if it holds inline content other than
// white space that collapses away.
bool layoutRunGeneratesBox(const LayoutTree* tree, LayoutNodeIndex parent, LayoutNodeIndex first, LayoutNodeIndex last);

// Returns true if the box of `node` avoids breaks inside it in a context of `kind` (css-break-4/#break-within).
bool layoutAvoidsBreakInside(const LayoutTree* tree, LayoutNodeIndex node, LayoutFragmentainerKind kind);

// Returns true if no break may fall inside the box of `node` (css-break-4/#monolithic): a replaced box, a scroll
// container, and a box of another block flow than `mode`'s.
bool layoutIsMonolithic(const LayoutTree* tree, LayoutNodeIndex node, LayoutMode mode);

// Returns true if a forced break of `kind` may fall inside the box of `node`, at any depth. It is false at once in a
// tree whose styles force no break.
bool layoutHasForcedBreakInside(const LayoutTree* tree, LayoutNodeIndex node, LayoutFragmentainerKind kind);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
