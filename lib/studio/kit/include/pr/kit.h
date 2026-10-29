// Studio Kit: the widgets of the Studio design system, a component library over lib/ui and lib/layout.
//
// A widget is a function of the kit, the parent that it is created under, a key, what it shows and how it looks. It
// creates its nodes in the open round of the kit's context (pr/ui.h), sets their input flags, handles its own input,
// and returns the node that the caller places, restyles or puts children under. A widget's parent is an argument rather
// than an open scope. A setting that applies to every widget in a region, such as disabling them, belongs to a
// container and holds for everything created under it.
//
// The kit is one of any number of libraries that build one tree. A kit's widget may be created under another library's
// node, and another library's widget under a kit's node. The kit needs no call to begin a round, and it owns no part of
// the tree that another library needs, the root included. Keys, the overlay, edit gestures and records belong to the
// context, which every library shares.
//
// A kit is used from its context's thread, during a build.

#pragma once

#include "pr/base.h"
#include "pr/kit_theme.h"
#include "pr/layout_frame.h"
#include "pr/layout_tree.h"
#include "pr/ui.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// ---------------------------------------------------------------------------------------------------------------------
// Kits

// The widgets of one theme, for one context.
typedef struct PrKit PrKit;

// Creates a kit for `ui` with `theme`, which the kit copies. `ui` must outlive the kit.
PrKit* prKitCreate(PrUi* ui, const PrKitTheme* theme);

// Destroys the kit and the records and fonts that it made.
void prKitDestroy(PrKit* PR_NULLABLE kit);

// Sets the kit's theme, which the kit copies. Call between rounds. The next round declares every widget in the new
// theme.
void prKitSetTheme(PrKit* kit, const PrKitTheme* theme);

// Returns the kit's theme, context and tree.
const PrKitTheme* prKitTheme(const PrKit* kit);
PrUi* prKitUi(const PrKit* kit);
PrLayoutTree* prKitTree(const PrKit* kit);

// ---------------------------------------------------------------------------------------------------------------------
// Events

// Returns true if the widget's node was activated in the current round: a button or a selectable box by a click, or by
// Enter or Space while it has focus.
bool prKitActivated(const PrKit* kit, PrLayoutNode node);

// Returns true if the widget asked its caller to remove something in the current round: a chip by its remove control,
// or a graph or a timeline by Delete or Backspace while it had focus, for its selection. The caller removes it.
bool prKitRemoved(const PrKit* kit, PrLayoutNode node);

// ---------------------------------------------------------------------------------------------------------------------
// Sizes and placement

// How a size is given. Zero is auto: the size that the content takes.
enum {
    PrKitSizeKind_Auto,
    PrKitSizeKind_Px,
    PrKitSizeKind_Percent,          // a percentage of the parent's size along the same axis
    PrKitSizeKind_Fill,             // the room that the parent leaves, across its axis
};
typedef uint8_t PrKitSizeKind;

// One size along an axis. A widget's style is the recipe of its records, so a style has no padding, and equal styles
// are equal bytes.
PR_NO_PADDING_BEGIN
typedef struct PrKitSize {
    float value;
    PrKitSizeKind kind;
    uint8_t _pad[3];
} PrKitSize;
PR_NO_PADDING_END

// Return a size in px, a percentage of the parent's size, or the room that the parent leaves across its axis.
static inline PrKitSize prKitPx(float px) {
    PrKitSize size = {px, PrKitSizeKind_Px, {0, 0, 0}};
    return size;
}
static inline PrKitSize prKitPercent(float percent) {
    PrKitSize size = {percent, PrKitSizeKind_Percent, {0, 0, 0}};
    return size;
}
static inline PrKitSize prKitFill(void) {
    PrKitSize size = {0, PrKitSizeKind_Fill, {0, 0, 0}};
    return size;
}

// Lengths on each side, in px.
typedef struct PrKitEdges {
    float top;
    float right;
    float bottom;
    float left;
} PrKitEdges;

// Return equal lengths on every side, or a vertical length and a horizontal one.
static inline PrKitEdges prKitEdgesAll(float all) {
    PrKitEdges edges = {all, all, all, all};
    return edges;
}
static inline PrKitEdges prKitEdgesXY(float vertical, float horizontal) {
    PrKitEdges edges = {vertical, horizontal, vertical, horizontal};
    return edges;
}

// How a widget takes room along its parent's main axis. Zero keeps the widget's own behavior.
enum {
    PrKitFlex_Own,
    PrKitFlex_Auto,                 // its content's size, shrinking if its parent runs out of room
    PrKitFlex_Fixed,                // its content's size, never shrinking
    PrKitFlex_Fill,                 // a share of the parent's room, by weight, regardless of its content
    PrKitFlex_Grow,                 // its content's size plus a share of the room that its parent has left
};
typedef uint8_t PrKitFlex;

// Where a widget sits across its parent's main axis, or where a container puts its children across its own.
enum {
    PrKitAlign_Auto,                // the parent's choice for its children
    PrKitAlign_Stretch,
    PrKitAlign_Start,
    PrKitAlign_End,
    PrKitAlign_Center,
    PrKitAlign_Baseline,
};
typedef uint8_t PrKitAlign;

// How a widget sits in its parent. Zero is a widget's own placement: its sizes and flex, no limits, no margins, and its
// parent's alignment. A container's style holds one; another widget is placed with prKitPlace.
PR_NO_PADDING_BEGIN
typedef struct PrKitPlace {
    PrKitSize width;
    PrKitSize height;
    PrKitSize minWidth;
    PrKitSize minHeight;
    PrKitSize maxWidth;
    PrKitSize maxHeight;
    PrKitEdges margin;
    PrKitFlex flex;
    PrKitAlign align;
    uint8_t _pad[2];
    float weight;                   // Fill and Grow: the share; 0 means 1
} PrKitPlace;
PR_NO_PADDING_END

// Restyles a node that a widget created in the current round with `place`, so that the caller can place the widget. A
// widget's node that is placed the same way each round keeps its records, and costs one lookup.
void prKitPlace(PrKit* kit, PrLayoutNode node, const PrKitPlace* place);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
