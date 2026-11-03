// Studio Kit's boxes: containers that lay their children out in a row or a column, and the space between them.

#pragma once

#include "pr/geom.h"
#include "pr/kit.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// The axis along which a box lays out its children.
enum {
    PrKitDirection_Column,
    PrKitDirection_Row,
};
typedef uint8_t PrKitDirection;

// Where a box puts its children along its axis, and the room between them.
enum {
    PrKitJustify_Start,
    PrKitJustify_End,
    PrKitJustify_Center,
    PrKitJustify_SpaceBetween,
    PrKitJustify_SpaceAround,
    PrKitJustify_SpaceEvenly,
};
typedef uint8_t PrKitJustify;

// What a box does with children that do not fit it.
enum {
    PrKitOverflow_Visible,
    PrKitOverflow_Clip,             // cuts them at its padding box
    PrKitOverflow_Scroll,           // a scroll container that the user scrolls with the wheel, in both axes
    PrKitOverflow_ScrollY,          // a scroll container that the user scrolls in its block axis; clips the other axis
};
typedef uint8_t PrKitOverflow;

// What a box holds apart from how it looks: its identity, and whether the widgets made under it are disabled. A
// disabled box shows what it holds, which takes no input.
typedef struct PrKitBox {
    PrUiKey key;
    bool disabled;
} PrKitBox;

// How a box sits in its parent and lays out its children along one axis. Zero is a column with its children at the
// start and stretched across it, with no gap and no padding.
PR_NO_PADDING_BEGIN
typedef struct PrKitBoxStyle {
    PrKitDirection direction;
    PrKitJustify justify;
    PrKitAlign align;               // where children with auto alignment sit across the axis; auto means stretch
    PrKitOverflow overflow;
    bool wrap;                      // children that do not fit go on additional lines
    uint8_t _pad[3];
    float gap;                      // px between children, and between lines
    PrKitEdges padding;
    PrKitPlace place;
} PrKitBoxStyle;
PR_NO_PADDING_END

// Creates a box under `parent`.
PrLayoutNode prKitBox(PrKit* kit, PrLayoutNode parent, PrKitBox box, PrKitBoxStyle style);

// Creates a box whose children may layer over one another. It is a positioned box, which the layers in it cover.
PrLayoutNode prKitStack(PrKit* kit, PrLayoutNode parent, PrKitBox box, PrKitBoxStyle style);

// Creates a box that covers the padding box of the nearest enclosing stack. The box takes no room among its siblings
// and paints above them. Layers paint in the order they are created.
PrLayoutNode prKitLayer(PrKit* kit, PrLayoutNode parent, PrKitBox box, PrKitBoxStyle style);

// A box that stands in 3D: Kit's one 3D affordance. Its content is laid out flat in the plane, as its style lays it
// out, and takes input where it is drawn. Planes nest: a plane whose `perspective` is set is the eye for the planes
// created directly under it, so a row of panels that share one eye is a plane with a perspective that holds a plane for
// each panel. A plane is a stacking context, and the containing block of the layers in it.
typedef struct PrKitPlane {
    PrUiKey key;
    bool disabled;

    // The plane's transform, about its center, in px, or null for none. A transform is a value that moves, so the
    // plane's record is made in each round that gives one.
    const PrMatrix4* PR_NULLABLE transform;
} PrKitPlane;

// How a plane lays out its content and stands among the planes around it.
PR_NO_PADDING_BEGIN
typedef struct PrKitPlaneStyle {
    PrKitBoxStyle box;

    // The distance in px from the eye to this plane, which projects the transforms of the planes directly under it,
    // about this plane's center, or 0 for none. A distance below 1 px is 1 px.
    float perspective;

    // The plane's stack level among the planes around it, used if nonzero. A plane at a higher level paints above one
    // at a lower level, whatever their order.
    int32_t zIndex;

    // The plane paints nothing and takes no input while its transform, with its parent's perspective, shows the back of
    // the plane.
    bool backfaceHidden;

    uint8_t _pad[3];
} PrKitPlaneStyle;
PR_NO_PADDING_END

// Creates a plane under `parent`.
PrLayoutNode prKitPlane(PrKit* kit, PrLayoutNode parent, PrKitPlane plane, PrKitPlaneStyle style);

// An empty box that takes the room its parent has left along its axis.
typedef struct PrKitSpacer {
    PrUiKey key;
} PrKitSpacer;

// Creates a spacer under `parent`.
PrLayoutNode prKitSpacer(PrKit* kit, PrLayoutNode parent, PrKitSpacer spacer);

// A one px rule across its parent's axis, which separates its siblings. In a row, the rule is vertical.
typedef struct PrKitDivider {
    PrUiKey key;
} PrKitDivider;

// Creates a divider under `parent`.
PrLayoutNode prKitDivider(PrKit* kit, PrLayoutNode parent, PrKitDivider divider);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
