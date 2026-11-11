// The CompositorServices backend of immersive sessions.
//
// A frame runs as CompositorServices orders it: the session queries the layer renderer's next frame, predicts its
// timing, runs an empty update, waits for the frame's optimal input time, starts the submission and queries the
// drawable. It then sets the drawable's depth range and the device's pose at the frame's presentation time, which ARKit
// predicts, and computes the views. The surface holds the drawable until the frame ends, when the submission ends.

#include "pr/surface_compositor_services.h"

#include "pr/alloc.h"
#include "pr/surface_backend.h"
#include "pr/time.h"

#import <CompositorServices/CompositorServices.h>

#import <ARKit/ARKit.h>
#import <QuartzCore/QuartzCore.h>

// What a session holds, which ARC keeps.
@interface PrCompositorServicesState : NSObject {
    @public
    cp_layer_renderer_t renderer;
    ar_session_t PR_NULLABLE tracking;
    ar_world_tracking_provider_t PR_NULLABLE world;
    ar_device_anchor_t PR_NULLABLE anchor;
    cp_frame_t PR_NULLABLE frame;                   // the begun frame, valid until it ends
    cp_drawable_t PR_NULLABLE drawable;             // its drawable for the device's own displays
}
@end

@implementation PrCompositorServicesState
@end

namespace pr_impl {

namespace {

extern const PrSurfaceSession kSessionFunctions;

struct Session : PrSurfaceSession {
    void* state;                        // PrCompositorServicesState, retained
    void* PR_NULLABLE drawable;         // the begun frame's cp_drawable_t, which the surface points to
    float near;
    float far;
};

PrCompositorServicesState* stateOf(const PrSurfaceSession* session) {
    return (__bridge PrCompositorServicesState*)static_cast<const Session*>(session)->state;
}

PrMatrix4 matrixOf(simd_float4x4 m) {
    PrMatrix4 out;
    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 4; ++column)
            out.m[row][column] = m.columns[column][row];
    }
    return out;
}

void destroy(PrSurfaceSession* session) {
    Session* s = static_cast<Session*>(session);
    PrCompositorServicesState* state = (__bridge_transfer PrCompositorServicesState*)s->state;
    if (state->tracking != nil)
        ar_session_stop(state->tracking);
    state = nil;
    prMemoryFree(s);
}

PrSurface surface(const PrSurfaceSession* session) {
    const Session* s = static_cast<const Session*>(session);
    return PrSurface{.kind = PrSurfaceKind_CompositorServices, .handle = const_cast<void**>(&s->drawable)};
}

PrSurfaceSessionState state(const PrSurfaceSession* session) {
    switch (cp_layer_renderer_get_state(stateOf(session)->renderer)) {
        case cp_layer_renderer_state_paused:
            return PrSurfaceSessionState_Paused;
        case cp_layer_renderer_state_running:
            return PrSurfaceSessionState_Running;
        default:
            return PrSurfaceSessionState_Ended;
    }
}

// Returns the time on lib/core's monotonic clock, in milliseconds, of a time of CompositorServices' clock.
double millisecondsOf(cp_time_t time) {
    double ahead = cp_time_to_cf_time_interval(time) - CACurrentMediaTime();
    return prTimeMonotonicMilliseconds() + ahead * 1000;
}

PrStatus frameBegin(PrSurfaceSession* session, PrSurfaceFrame* out) {
    Session* s = static_cast<Session*>(session);
    PrCompositorServicesState* st = stateOf(session);
    cp_drawable_t drawable = nullptr;
    cp_frame_t frame = nullptr;
    while (drawable == nullptr) {
        switch (cp_layer_renderer_get_state(st->renderer)) {
            case cp_layer_renderer_state_paused:
                cp_layer_renderer_wait_until_running(st->renderer);
                continue;
            case cp_layer_renderer_state_running:
                break;
            default:
                return PrStatus_Lost;
        }
        frame = cp_layer_renderer_query_next_frame(st->renderer);
        if (frame == nullptr)
            continue;
        cp_frame_timing_t predicted = cp_frame_predict_timing(frame);
        if (predicted == nullptr)
            continue;
        cp_frame_start_update(frame);
        cp_frame_end_update(frame);
        cp_time_wait_until(cp_frame_timing_get_optimal_input_time(predicted));
        cp_frame_start_submission(frame);
        // The frame draws for the device's own displays. A frame with no drawables is cancelled.
        cp_drawable_array_t drawables = cp_frame_query_drawables(frame);
        for (size_t i = 0; i < cp_drawable_array_get_count(drawables) && drawable == nullptr; ++i) {
            cp_drawable_t d = cp_drawable_array_get_drawable(drawables, i);
            if (cp_drawable_get_target(d) == cp_drawable_target_built_in)
                drawable = d;
        }
        if (drawable == nullptr && cp_drawable_array_get_count(drawables) != 0)
            cp_frame_end_submission(frame);
    }
    cp_time_t presentation = cp_frame_timing_get_presentation_time(cp_drawable_get_frame_timing(drawable));
    // The device's pose when the frame shows, which the compositor reprojects the frame from.
    simd_float4x4 worldFromDevice = matrix_identity_float4x4;
    if (st->world != nil && ar_world_tracking_provider_query_device_anchor_at_timestamp(st->world,
            cp_time_to_cf_time_interval(presentation), st->anchor) == ar_device_anchor_query_status_success) {
        worldFromDevice = ar_anchor_get_origin_from_anchor_transform(st->anchor);
        cp_drawable_set_device_anchor(drawable, st->anchor);
    }
    // CompositorServices takes the far plane first.
    cp_drawable_set_depth_range(drawable, simd_make_float2(s->far, s->near));
    size_t views = cp_drawable_get_view_count(drawable);
    out->time = millisecondsOf(presentation);
    out->viewCount = uint8_t(views < kPrSurfaceMaxViews ? views : kPrSurfaceMaxViews);
    for (uint8_t i = 0; i < out->viewCount; ++i) {
        simd_float4x4 worldFromView = simd_mul(worldFromDevice, cp_view_get_transform(cp_drawable_get_view(drawable, i)));
        out->views[i].view = matrixOf(simd_inverse(worldFromView));
        out->views[i].projection =
            matrixOf(cp_drawable_compute_projection(drawable, cp_axis_direction_convention_right_up_back, i));
    }
    st->frame = frame;
    st->drawable = drawable;
    s->drawable = drawable;
    return PrStatus_Ok;
}

void frameEnd(PrSurfaceSession* session) {
    Session* s = static_cast<Session*>(session);
    PrCompositorServicesState* st = stateOf(session);
    cp_frame_end_submission(st->frame);
    st->frame = nullptr;
    st->drawable = nullptr;
    s->drawable = nullptr;
}

constexpr PrSurfaceSession kSessionFunctions = {
    .destroyFn = destroy,
    .surfaceFn = surface,
    .stateFn = state,
    .frameBeginFn = frameBegin,
    .frameEndFn = frameEnd,
};

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrStatus prSurfaceCompositorServicesCreate(const PrSurfaceSessionDesc* desc, PrSurfaceSession* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    if (desc->native == nullptr)
        return PrStatus_InvalidData;
    PrCompositorServicesState* state = [PrCompositorServicesState new];
    state->renderer = (__bridge cp_layer_renderer_t)desc->native;
    // Without world tracking, the views stay where the device was at the origin.
    if (ar_world_tracking_provider_is_supported()) {
        state->world = ar_world_tracking_provider_create(ar_world_tracking_configuration_create());
        state->anchor = ar_device_anchor_create();
        state->tracking = ar_session_create();
        ar_session_run(state->tracking, ar_data_providers_create_with_data_providers(state->world, nil));
    }
    Session* s = allocZeroed<Session>();
    static_cast<PrSurfaceSession&>(*s) = kSessionFunctions;
    s->state = (__bridge_retained void*)state;
    s->near = desc->near;
    s->far = desc->far;
    *out = s;
    return PrStatus_Ok;
}
