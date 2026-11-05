// The viewer's zoom: the standard steps that the zoom commands move between, the scroll offset that keeps a document's
// point at the pane's center fixed across a change of zoom, and the range of a scroll offset.

#pragma once

#include "pr/base.h"

#include <cmath>

PR_ASSUME_NONNULL_BEGIN

namespace viewer {

// The zooms that zooming in and out step between, as fractions of the natural size, in increasing order.
constexpr float kZoomSteps[] = {
    0.25f, 0.33f, 0.5f, 0.67f, 0.75f, 0.8f, 0.9f, 1, 1.1f, 1.25f, 1.5f, 1.75f, 2, 2.5f, 3, 4, 5,
};

// Returns the first step above `zoom`, or the largest step if there is none. A zoom between steps, as fitting a width
// gives, moves to the step beside it.
inline float zoomIn(float zoom) {
    for (float step : kZoomSteps)
        if (step > zoom * 1.001f)
            return step;
    return kZoomSteps[sizeof(kZoomSteps) / sizeof(float) - 1];
}

// Returns the last step below `zoom`, or the smallest step if there is none.
inline float zoomOut(float zoom) {
    for (size_t i = sizeof(kZoomSteps) / sizeof(float); i-- > 0;)
        if (kZoomSteps[i] < zoom * 0.999f)
            return kZoomSteps[i];
    return kZoomSteps[0];
}

// Returns the scroll offset along an axis, in CSS pixels, that keeps the document's point at the center of a pane
// `size` logical pixels across where it is, when the zoom changes from `from` to `to`. The pane shows `size / zoom` CSS
// pixels from the offset on. The offset is never negative.
inline float anchoredScroll(float scroll, float size, float from, float to) {
    float center = scroll + size / (2 * from);
    return std::fmax(center - size / (2 * to), 0.0f);
}

// Returns the scroll offset along an axis clamped to what a viewport of `port` CSS pixels can show of `content` CSS
// pixels: from the start to where the viewport's end meets the content's.
inline float clampScroll(float scroll, float content, float port) {
    return std::fmax(0.0f, std::fmin(scroll, content - port));
}

} // namespace viewer

PR_ASSUME_NONNULL_END
