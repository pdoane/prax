// Paged media: a tree's viewport root laid out as pages (css-page-3).
//
// A paginated tree lays its viewport's root out one page at a time, each page's area a fragmentainer that the root
// resumes in from the break token of the page before (css-break-4). The producer resolves the @page rules, which layout
// knows nothing of, and layout asks it for each page's box as it reaches the page. The viewport holds the pages,
// stacked down it in order, so that geometry, hit testing and painting read one tree. The viewport scrolls through the
// pages as it scrolls a document on the screen. A page's margin boxes are boxes of the tree. The producer gives them
// once the content is laid out into pages, since what they show, such as the page count, depends on the whole layout.

#pragma once

#include "pr/base.h"
#include "pr/color.h"
#include "pr/draw.h"
#include "pr/geom.h"
#include "pr/layout_style.h"
#include "pr/layout_tree.h"
#include "pr/layout_unit.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// The side of a spread that a page is on (css-page-3/#spread-pseudos). The first page is a right page if the root's
// direction is left-to-right, and a left page otherwise, and the sides alternate from there.
enum {
    PrLayoutPageSide_Left,
    PrLayoutPageSide_Right,
};
typedef uint8_t PrLayoutPageSide;

// What the selectors of @page rules match (css-page-3/#page-selectors): a page's index from zero, its name, its side,
// and whether it is the first page or a blank page. A page's name is the used page value of the first box on it
// (css-page-3/#using-named-pages). A blank page is one that a break to a side of the spread inserts, and has the name
// of the page after it.
typedef struct PrLayoutPageContext {
    uint32_t index;
    PrLayoutName name;              // kPrLayoutNameNone for none
    PrLayoutPageSide side;
    bool first;
    bool blank;
} PrLayoutPageContext;

// The marks that a page's sheet shows outside the page's bleed area (css-page-3/#marks).
enum {
    PrLayoutPageMarkFlag_None = 0,
    PrLayoutPageMarkFlag_Crop = 1u << 0,    // lines that extend the page box's edges, which show where to trim
    PrLayoutPageMarkFlag_Cross = 1u << 1,   // registration marks outside the middle of each edge, which align sheets
    PrLayoutPageMarkFlag_All = PrLayoutPageMarkFlag_Crop | PrLayoutPageMarkFlag_Cross,
};
typedef uint8_t PrLayoutPageMarkFlags;

// A page box (css-page-3/#page-model): its extent, its margins by PrRectSide, which surround its page area, and the
// background that fills its bleed area, straight alpha. The page area holds the page's content, and the canvas's color
// fills it.
//
// The bleed area extends `bleed` past the page box on every side, or lies inside it by a negative `bleed`
// (css-page-3/#bleed). The page's rendering is clipped to it, so content that overflows the page box shows in it. The
// producer resolves the descriptor's auto value. The page is printed on a sheet that holds the bleed area and the marks
// (prLayoutPageBoxSheet).
PR_NO_PADDING_BEGIN
typedef struct PrLayoutPageBox {
    PrLayoutVec extent;
    PrLayoutUnit margin[PrRectSide_Count];
    PrLayoutUnit bleed;
    PrColor16 background;
    PrLayoutPageMarkFlags marks;
    uint8_t _pad[3];
} PrLayoutPageBox;
PR_NO_PADDING_END

// Returns a US letter page, 816 by 1056 px, with no margins, no bleed, no marks and a transparent background.
PrLayoutPageBox prLayoutPageBoxDefault(void);

// Returns the extent of the page area of `box`, which layout lays the page out in. Margins that leave no page area are
// reduced to fit.
PrLayoutVec prLayoutPageBoxArea(const PrLayoutPageBox* box);

// The sheet that a page is printed on: the page box with its bleed area and its marks around it, in units from the
// sheet's top left. A page with no bleed and no marks is printed on a sheet that is its page box.
typedef struct PrLayoutPageSheet {
    PrLayoutVec extent;
    PrLayoutRect bleed;             // the bleed area, which clips the page's rendering
    PrLayoutRect trim;              // the page box, where the sheet is trimmed
} PrLayoutPageSheet;

// Returns the sheet of `box`. Each side of the page box gets the same room on the sheet: the bleed area, and outside it
// a band for the marks if the box has any. A crop mark starts outside the bleed area, and at least 6 pt from the page
// box. It runs 24 pt along the line of the page box's edge. A registration mark is a circle with a cross, centered on
// the band outside the middle of each edge. The band ends 6 pt past the marks.
PrLayoutPageSheet prLayoutPageBoxSheet(const PrLayoutPageBox* box);

// Records what a page shows of its box into `list`, with the top left of the page's sheet at `origin`: the box's
// background over the bleed area, `canvas`, the canvas's color, over the page area, and the marks outside the bleed
// area. Marks are drawn in black, 0.5 pt wide. The caller plays the page's content after it, clipped to the bleed area,
// as prLayoutTreePaintPage does.
void prLayoutPageBoxPaint(const PrLayoutPageBox* box, PrColor16 canvas, PrVec2 origin, PrDrawList* list);

// Writes the box of the page that `page` describes to `box`, which holds the default box. Called on the thread that
// lays the tree out, during the layout, once for each page in order. Margins that leave no page area are reduced to
// fit, and so is a negative bleed that leaves no bleed area.
typedef void (*PrLayoutPageBoxFn)(void* PR_NULLABLE context, const PrLayoutPageContext* page, PrLayoutPageBox* box);

// How a tree is paginated: the function that gives each page's box, called with `context`, and how the viewport stacks
// the pages. The sheets of the pages (prLayoutPageBoxSheet) go down the viewport `gap` apart, each centered across the
// widest sheet, and the stack is centered across the viewport if it is narrower. The viewport takes the extent that the
// tree is laid out in, and scrolls through the stack in both axes.
typedef struct PrLayoutPagination {
    PrLayoutPageBoxFn box;
    void* PR_NULLABLE context;
    PrLayoutUnit gap;
} PrLayoutPagination;

// Sets how the tree lays its viewport's root out: as pages, or continuously if `pagination` is null. A paginated tree
// lays its content out in the page areas, whatever extent the viewport is given (prLayoutTreeLayout), and that extent
// is the scrollport through which the viewport shows the pages. Setting it lays out the viewport's root again at the
// next layout, so a producer whose page boxes changed sets it again.
void prLayoutTreeSetPagination(PrLayoutTree* tree, const PrLayoutPagination* PR_NULLABLE pagination);

// Returns what the first page of `root` is when the tree lays it out as pages: the first page, named by the start page
// value of the root (css-page-3/#using-named-pages), on the side that the root's direction gives. It reads the tree's
// records as they are, without a layout. A producer reads it to resolve the first page's area, the initial containing
// block of paged media (css-page-3/#page-model), before the layout that asks for it.
PrLayoutPageContext prLayoutNodeStartPage(const PrLayoutTree* tree, PrLayoutNode root);

// Returns the number of pages of the last layout, which is zero for a tree that is not paginated.
uint32_t prLayoutTreePageCount(const PrLayoutTree* tree);

// A page of the last layout: what its box was resolved for, its box, and the offset of the box's top left in the
// viewport's content, in which geometry and hit testing measure the page's boxes.
typedef struct PrLayoutPage {
    PrLayoutPageContext context;
    PrLayoutPageBox box;
    PrLayoutVec offset;

    // The node of the first content on the page in the flow's order, or kPrLayoutNodeNone for a page with none, such as
    // a blank page. Content is a text, a replaced box, or a box that has a size and no content inside it. Content that
    // comes before this node in tree order is on an earlier page.
    PrLayoutNode first;
} PrLayoutPage;

// Returns the page at `index`, which must be less than the page count.
PrLayoutPage prLayoutTreePage(const PrLayoutTree* tree, uint32_t index);

// Returns the index of the page that holds the first box of `node`, or of the placeholder of a running box
// (PrLayoutPosition_Running), or UINT32_MAX if the last layout gave it none. The cost is the node's depth plus the
// logarithm of the page count.
uint32_t prLayoutNodePage(const PrLayoutTree* tree, PrLayoutNode node);

// The page-margin boxes (css-page-3/#margin-boxes), in the order in which they paint (css-page-3/#painting): the boxes
// of the top margin from left to right, of the right margin from top to bottom, of the bottom margin from right to
// left, and of the left margin from bottom to top. A corner box fills the corner where two margins meet.
enum {
    PrLayoutMarginBox_TopLeftCorner,
    PrLayoutMarginBox_TopLeft,
    PrLayoutMarginBox_TopCenter,
    PrLayoutMarginBox_TopRight,
    PrLayoutMarginBox_TopRightCorner,
    PrLayoutMarginBox_RightTop,
    PrLayoutMarginBox_RightMiddle,
    PrLayoutMarginBox_RightBottom,
    PrLayoutMarginBox_BottomRightCorner,
    PrLayoutMarginBox_BottomRight,
    PrLayoutMarginBox_BottomCenter,
    PrLayoutMarginBox_BottomLeft,
    PrLayoutMarginBox_BottomLeftCorner,
    PrLayoutMarginBox_LeftBottom,
    PrLayoutMarginBox_LeftMiddle,
    PrLayoutMarginBox_LeftTop,
    PrLayoutMarginBox_Count,
};
typedef uint8_t PrLayoutMarginBox;

// Sets the margin boxes of the first `count` pages: `boxes` holds PrLayoutMarginBox_Count nodes for each page, in
// PrLayoutMarginBox order, each kPrLayoutNodeNone for a box that the page does not generate. A page past `count` has no
// margin boxes. Each node is a child of the viewport's root whose display is PrLayoutDisplay_PageMargin, and is the
// margin box of one page.
//
// A page lays its margin boxes out once its content is laid out, sized and placed in its margins as css-page-3 says
// (css-page-3/#margin-dimension). They take no part in the content's layout, so a layout that follows changes to the
// margin boxes alone, as setting them is, lays out only the margin boxes and keeps the pages' content.
void prLayoutTreeSetPageMargins(PrLayoutTree* tree, const PrLayoutNode* PR_NULLABLE boxes, uint32_t count);

// Writes the margin boxes that were set for the page at `index` to `boxes`, PrLayoutMarginBox_Count nodes in
// PrLayoutMarginBox order, each kPrLayoutNodeNone for a box that the page does not generate.
void prLayoutTreePageMargins(const PrLayoutTree* tree, uint32_t index, PrLayoutNode* boxes);

// Records the page at `index` into `list`, in pixels from the top left of its sheet (prLayoutPageBoxSheet): what the
// page shows of its box (prLayoutPageBoxPaint), and the tree's content on the page, clipped to the bleed area. The
// viewport plays unscrolled, and every other scroll container at its offset. The list embeds the tree's recordings
// without copying them, and the lists that they embed for other pages lie outside the clip, where a consumer skips them
// (prDrawListMisses).
void prLayoutTreePaintPage(PrLayoutTree* tree, uint32_t index, PrDrawList* list);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
