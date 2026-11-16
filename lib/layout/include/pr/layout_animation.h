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
//
// An animation effect's timing places its iterations in its local time, which its animation gives it from a timeline.
// Times are milliseconds. A time that is unresolved is NaN.

// The direction iterations play in (web-animations-1/#playback-direction).
enum {
    PrLayoutPlaybackDirection_Normal,
    PrLayoutPlaybackDirection_Reverse,
    PrLayoutPlaybackDirection_Alternate,
    PrLayoutPlaybackDirection_AlternateReverse,
};
typedef uint8_t PrLayoutPlaybackDirection;

// Where an effect has an effect outside its active interval (web-animations-1/#fill-modes).
enum {
    PrLayoutFill_None,
    PrLayoutFill_Forwards,
    PrLayoutFill_Backwards,
    PrLayoutFill_Both,
};
typedef uint8_t PrLayoutFill;

// How an effect's iterations play in its local time (web-animations-1/#the-effecttiming-dictionaries). The easing
// shapes the progress of each iteration.
typedef struct PrLayoutEffectTiming {
    double delay;                       // may be negative
    double endDelay;                    // may be negative
    double iterationStart;              // at least zero
    double iterations;                  // at least zero, and may be infinite
    double duration;                    // of one iteration, at least zero, and may be infinite
    PrLayoutEasing easing;
    PrLayoutPlaybackDirection direction;
    PrLayoutFill fill;
} PrLayoutEffectTiming;

// Returns the default timing: no delays, one iteration of zero duration, linear easing, normal direction and no fill.
PrLayoutEffectTiming prLayoutEffectTimingDefault(void);

// Where an effect's local time falls (web-animations-1/#animation-effect-phases-and-states). The phase is None while
// the local time is unresolved.
enum {
    PrLayoutEffectPhase_None,
    PrLayoutEffectPhase_Before,
    PrLayoutEffectPhase_Active,
    PrLayoutEffectPhase_After,
};
typedef uint8_t PrLayoutEffectPhase;

// An effect's timing at one local time, where a value that is unresolved is NaN
// (web-animations-1/#calculating-the-transformed-progress).
typedef struct PrLayoutComputedTiming {
    double endTime;                     // the end of the end delay, at least zero
    double activeDuration;
    double localTime;
    double activeTime;
    double overallProgress;
    double simpleProgress;              // the simple iteration progress, in [0, 1]
    double currentIteration;            // a whole number, which may be infinite
    double progress;                    // the transformed progress, after direction and easing
    PrLayoutEffectPhase phase;
    bool forwards;                      // true if the current iteration plays forwards
} PrLayoutComputedTiming;

// Computes the timing of an effect at `localTime`, for an animation that plays at `rate`. The sign of the rate decides
// the phase at the boundaries of the active interval: a backwards animation is before its start at the start, and a
// forwards one past its end at the end.
PrLayoutComputedTiming prLayoutEffectTimingCompute(const PrLayoutEffectTiming* timing, double localTime, double rate);

// The time of an animation (web-animations-1/#the-current-time-of-an-animation). While the hold time is resolved, it is
// the current time. Otherwise the current time runs from the start time at the rate, on the timeline's time.
typedef struct PrLayoutPlayback {
    double startTime;
    double holdTime;
    double rate;
} PrLayoutPlayback;

// Returns a playback with no start time or hold time, at a rate of 1.
PrLayoutPlayback prLayoutPlaybackDefault(void);

// Returns the playback's current time when its timeline's time is `timelineTime`, or NaN if it is unresolved.
double prLayoutPlaybackCurrentTime(const PrLayoutPlayback* playback, double timelineTime);

// Returns true if the current time of the playback moves as the timeline's time does.
bool prLayoutPlaybackMoves(const PrLayoutPlayback* playback);

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
    PrLayoutPlayback playback;          // on the timeline whose time the track is sampled at
    PrLayoutEffectTiming timing;
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
