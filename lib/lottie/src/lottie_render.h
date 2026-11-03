// Drawing a Lottie animation into a draw list, and the render state that compositions, layers and shape contents share.
//
// lottie-spec - https://lottie.github.io/lottie-spec/

#pragma once

#include "pr/draw_list.h"
#include "pr/path.h"
#include "pr/raw_array.h"
#include "pr/vec.h"

#include "lottie/src/lottie_model.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// A Bézier vertex: its point and the control points of its tangents.
struct LottieVertex {
    PrVec2 point;
    PrVec2 in;
    PrVec2 out;
};

// A run of vertices in a geometry, open or closed.
struct LottieContour {
    uint32_t first;
    uint32_t count;
    bool closed;
};

// Contours and the vertices they span. Modifiers rewrite them by appending.
struct LottieGeometry {
    RawArray<LottieVertex> vertices;
    RawArray<LottieContour> contours;
};

// A group of a shape layer's contents, as it is drawn. The group's styles draw under its transform and opacity.
struct LottieShapeGroup {
    uint32_t parent;
    PrAffine2 transform;
    float opacity;
    PrDrawBlendMode blend;
};

// A shape among a layer's contents. Its contours, and the styles and modifiers that apply to it, are runs of the
// render's `refs`.
struct LottieShapeEntry {
    uint32_t item;
    uint32_t group;
    uint32_t contours;                  // refs to contours in the render's geometry
    uint32_t contourCount;
    uint32_t styles;                    // refs to the render's styles
    uint32_t styleCount;
    uint32_t modifiers;                 // refs to the render's modifiers, nearest first
    uint32_t modifierCount;
    bool merged;                        // absorbed by merge paths, so that no later style takes it
};

// A style among a layer's contents, and the path it paints.
struct LottieShapeStyle {
    uint32_t item;
    uint32_t group;
    PrPath* path;                       // the style's shapes, in the space of its group
};

// A modifier among a layer's contents.
struct LottieShapeModifier {
    uint32_t item;
    uint32_t group;
};

// The commands a layer's contents draw, in order: groups opening and closing around styles.
enum {
    LottieShapeOp_Open,
    LottieShapeOp_Close,
    LottieShapeOp_Style,
};
typedef uint8_t LottieShapeOp;

// A command that a layer's contents draw.
struct LottieShapeStep {
    LottieShapeOp op;
    uint32_t index;                     // Open: the group; Style: the style
};

// The state of one draw of an animation.
struct LottieRender {
    const LottieAnimation* a;
    PrDrawList* list;
    RawArray<uint32_t> comps;           // compositions of the precomps being drawn, innermost last
    uint32_t instances;                 // number of layers drawn inside precomps so far
    PrAffine2 toRoot;                   // from the drawing space to the root composition's space

    // Scratch for shape contents. Drawing each layer resets it by count.
    LottieGeometry geometry;
    RawArray<LottieShapeGroup> groups;
    RawArray<LottieShapeEntry> shapes;
    RawArray<LottieShapeStyle> styles;
    RawArray<LottieShapeModifier> modifiers;
    RawArray<LottieShapeStep> steps;
    RawArray<uint32_t> refs;
    RawArray<PrPath*> paths;               // the styles' paths, kept between layers
    RawArray<float> values;                // storage for a property's value
    RawArray<PrDrawColorStop> stops;
    RawArray<float> dashes;
    PrPath* path;                       // a working path
};

// The maximum nesting depth of precomps. Deeper content is not drawn.
constexpr uint32_t kLottieMaxDepth = 32;

// The maximum number of layers that precomps draw in one render. If a composition holds several precomps of another
// composition, the other composition's layers draw once for each precomp, so the count can grow exponentially with
// depth.
constexpr uint32_t kLottieMaxInstances = 100000;

// The control point distance of a quarter circle's cubic, for a radius of one: 4/3 (sqrt(2) - 1).
constexpr float kLottieQuarterArc = 0.55228475f;

// Begins a render of the animation into `list`. lottieRenderReset frees what the render holds.
void lottieRenderInit(LottieRender& r, const LottieAnimation& a, PrDrawList* list);
void lottieRenderReset(LottieRender& r);

// Draws composition `comp` at `frame`, in the composition's own time, clipped to its size. `extent` is the size that
// the composition plays at: the animation's for the root, and the precomp layer's for a precomp. A camera looks at its
// center.
void lottieDrawComp(LottieRender& r, uint32_t comp, double frame, PrVec2 extent);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
