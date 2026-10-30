// Layout animations: easing, timing, transform function lists, and tracks of composited effects.
//
// css-easing-2 - https://drafts.csswg.org/css-easing-2/
// css-transforms-1 - https://drafts.csswg.org/css-transforms-1/
// css-transforms-2 - https://drafts.csswg.org/css-transforms-2/
// web-animations-1 - https://drafts.csswg.org/web-animations-1/
//
// A compositor needs these to animate a box's opacity and transform on its own, without the client that started the
// animation: the animation's timing, its easing, and keyframes of the values it moves between. A client that animates
// other values uses the same easing and timing.

#pragma once

#include "pr/base.h"
#include "pr/geom.h"
#include "pr/layout_style.h"
#include "pr/layout_tree.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// ---------------------------------------------------------------------------------------------------------------------
// Easing (css-easing-2/#easing-functions)

// The kinds of easing function.
enum {
    PrLayoutEasingKind_Linear,
    PrLayoutEasingKind_CubicBezier,     // through (x1, y1) and (x2, y2)
    PrLayoutEasingKind_Steps,           // `steps` steps that jump at `position`
};
typedef uint8_t PrLayoutEasingKind;

// Where a step easing function jumps (css-easing-2/#typedef-step-position).
enum {
    PrLayoutStepPosition_JumpEnd,
    PrLayoutStepPosition_JumpStart,
    PrLayoutStepPosition_JumpBoth,
    PrLayoutStepPosition_JumpNone,
};
typedef uint8_t PrLayoutStepPosition;

// An easing function.
typedef struct PrLayoutEasing {
    // The control points of a CubicBezier, in doubles so that an output that a script computes exactly, such as a
    // channel that rounds at one half, comes out as it does in script.
    double x1;
    double y1;
    double x2;
    double y2;

    uint32_t steps;                     // Steps: at least 1
    PrLayoutEasingKind kind;
    PrLayoutStepPosition position;      // Steps
} PrLayoutEasing;

// Returns the linear easing function.
PrLayoutEasing prLayoutEasingLinear(void);

// Returns the output progress of `easing` for the input progress `input`. `before` says whether the input is before the
// start of the interval, which a step at the start depends on (css-easing-2/#step-easing-algo).
double prLayoutEase(const PrLayoutEasing* easing, double input, bool before);

// ---------------------------------------------------------------------------------------------------------------------
// Timing (web-animations-1/#timing-model)

// The direction iterations play in (web-animations-1/#playback-direction).
enum {
    PrLayoutPlaybackDirection_Normal,
    PrLayoutPlaybackDirection_Reverse,
    PrLayoutPlaybackDirection_Alternate,
    PrLayoutPlaybackDirection_AlternateReverse,
};
typedef uint8_t PrLayoutPlaybackDirection;

// Where an animation has an effect outside its active interval (web-animations-1/#fill-mode).
enum {
    PrLayoutFill_None,
    PrLayoutFill_Forwards,
    PrLayoutFill_Backwards,
    PrLayoutFill_Both,
};
typedef uint8_t PrLayoutFill;

// When an animation runs, and how its iterations play. Times are milliseconds on the timeline the animation runs on.
typedef struct PrLayoutTiming {
    double start;                       // the time its delay starts
    double delay;                       // may be negative
    double duration;                    // of one iteration
    double iterations;                  // may be infinite
    PrLayoutPlaybackDirection direction;
    PrLayoutFill fill;
    bool paused;                        // true if it holds at `hold` past its start
    double hold;
} PrLayoutTiming;

// Returns the default timing: normal direction, no fill, and one iteration of zero duration, starting at zero.
PrLayoutTiming prLayoutTimingDefault(void);

// Returns the directed iteration progress of an animation at `time` (web-animations-1/#the-iteration-progress), in [0,
// 1], or a negative value if the animation has no effect at that time. `finished` says whether its active interval is
// over.
double prLayoutTimingProgress(const PrLayoutTiming* timing, double time, bool* finished);

// ---------------------------------------------------------------------------------------------------------------------
// Transforms (css-transforms-2/#transform-functions)

// The kinds of transform function, each with the values it reads. Each kind is a primitive
// (css-transforms-2/#transform-primitives): translateX() is a Translate, rotateY() is a Rotate about the y axis, and
// matrix() is a Matrix.
enum {
    PrLayoutTransformKind_Translate,    // x, y, z in pixels, then x per width and y per height of the reference box
    PrLayoutTransformKind_Scale,        // x, y and z
    PrLayoutTransformKind_Rotate,       // the axis's x, y and z, then the angle in radians, as rotate3d() takes them
    PrLayoutTransformKind_Skew,         // x and y angles, in radians
    PrLayoutTransformKind_Perspective,  // the distance in pixels, at least 1, or 0 for none
    PrLayoutTransformKind_Matrix,       // the 16 elements in the order matrix3d() lists them, column by column
};
typedef uint8_t PrLayoutTransformKind;

// One transform function.
typedef struct PrLayoutTransformFunction {
    float values[16];
    PrLayoutTransformKind kind;
} PrLayoutTransformFunction;

// The maximum number of functions in a transform list.
enum { kPrLayoutTransformListMaxFunctions = 16 };

// A list of transform functions, leftmost first, which is what a <transform-list> computes to
// (css-transforms-1/#transform-property). The empty list is the identity.
typedef struct PrLayoutTransformList {
    uint32_t count;
    PrLayoutTransformFunction functions[kPrLayoutTransformListMaxFunctions];
} PrLayoutTransformList;

// Returns the matrix that the list composes to for a reference box of `extent`, in pixels, with each function applying
// before those to its left.
PrMatrix4 prLayoutTransformListMatrix(const PrLayoutTransformList* list, PrVec2 extent);

// Returns the transform that the list composes to, in the form that the effects group holds. The form is exact unless a
// translation by a percentage is to the left of a function with perspective, and so applies after it: such a percentage
// counts as zero.
PrLayoutTransform prLayoutTransformListTransform(const PrLayoutTransformList* list);

// Returns the list at `progress` between `from` and `to` (css-transforms-1/#interpolation-of-transforms). The shorter
// list is extended with the identity functions of the longer list's kinds. Functions of the same kind interpolate one
// by one, as css-transforms-2/#interpolation-of-transform-functions describes; from the first pair of different kinds
// on, the rest of each list composes to a matrix, and the two matrices interpolate decomposed
// (css-transforms-2/#interpolation-of-3d-matrices, or css-transforms-1's 2D form if both are 2D). `extent` is the
// reference box, which resolves percentages in functions that interpolate as matrices. A pair of matrices that cannot
// be decomposed interpolates discretely.
PrLayoutTransformList prLayoutTransformListInterpolate(const PrLayoutTransformList* from,
    const PrLayoutTransformList* to, double progress, PrVec2 extent);

// Returns true if prLayoutTransformListInterpolate interpolates `from` and `to` other than discretely: every pair of
// matrices that it interpolates can be decomposed. A transition between lists that do not starts no animation
// (css-transitions-1/#starting).
bool prLayoutTransformListInterpolates(const PrLayoutTransformList* from, const PrLayoutTransformList* to, PrVec2 extent);

// ---------------------------------------------------------------------------------------------------------------------
// Tracks

// The composited effects of a box: its opacity and its transform about its origin. A track sets one of them.
enum {
    PrLayoutEffect_Opacity,
    PrLayoutEffect_Transform,
};
typedef uint8_t PrLayoutEffect;

// A keyframe of a track: where it is in an iteration, its value, and the easing from it to the next.
typedef struct PrLayoutKeyframe {
    double offset;                      // in [0, 1]
    float opacity;                      // Opacity
    PrLayoutTransformList transform;    // Transform
    PrLayoutEasing easing;
} PrLayoutKeyframe;

// An animation of one effect of the box of `node`. Its keyframes are in order of offset, and include offsets 0 and 1.
typedef struct PrLayoutTrack {
    PrLayoutNode node;
    PrLayoutEffect effect;
    PrLayoutTiming timing;
    const PrLayoutKeyframe* keyframes;
    uint32_t keyframeCount;
} PrLayoutTrack;

// Sets the value that a track gives its effect at `time`: an opacity, or a transform's matrix relative to its origin,
// depending on the track's effect. `extent` is the extent of the box's reference box in pixels, which resolves the
// transform's percentages. Returns false if the track has no effect at that time.
bool prLayoutTrackSample(const PrLayoutTrack* track, double time, PrVec2 extent, float* opacity, PrMatrix4* transform);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
