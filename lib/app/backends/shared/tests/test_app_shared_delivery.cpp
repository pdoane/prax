// The delivery of a window's callbacks: when the window opens, and when a window that the program destroyed is freed.

#include "pr/app.h"
#include "pr/array.h"
#include "pr/string.h"

#include "app/backends/shared/src/app_shared_delivery.h"

#include "tools/harness/test.h"

namespace pr_impl {

namespace {

// A window that records the calls of its handler and its frees in `seen`.
struct RecordingWindow {
    AppWindowDelivery delivery;
    Array<String> seen;
};

RecordingWindow& recordOf(void* context) {
    return *static_cast<RecordingWindow*>(context);
}

// A frame of 100 logical px at a pixel ratio of 2, which a window that opens on an event finds.
PrAppFrame frameState(PrAppWindow*) {
    return PrAppFrame{.extent = {100, 50}, .display = {.pixelRatio = 2, .headroom = 1, .potentialHeadroom = 1}};
}

// The handle that the deliveries pass to the handler. The tests never dereference it.
PrAppWindow* handleOf(RecordingWindow& window) {
    return reinterpret_cast<PrAppWindow*>(&window);
}

// A handler that records the delta of `open`, the width in target pixels of each frame, and the key of each event.
PrAppWindowHandler recordingHandler(RecordingWindow& window) {
    return PrAppWindowHandler{
        .context = &window,
        .open = [](void* context, PrAppWindow*, const PrAppFrame* frame) {
            recordOf(context).seen.push(format("open {}", int(frame->delta * 10)));
            return true;
        },
        .frame = [](void* context, PrAppWindow*, const PrAppFrame* frame) {
            recordOf(context).seen.push(format("frame {}", int(frame->extent.x * frame->display.pixelRatio)));
            return PrAppOutput{.cursor = PrAppCursor_Hand, .wantsFrame = true};
        },
        .event = [](void* context, PrAppWindow*, const PrAppEvent* event) {
            recordOf(context).seen.push(String("key ").append(char(event->keyboard.key)));
        },
    };
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a window opens before its first frame, with a delta of zero, and the frame's output is returned") {
    RecordingWindow window{};
    window.delivery.handler = recordingHandler(window);
    PrAppFrame frame = frameState(nullptr);
    frame.delta = 0.5;
    PrAppOutput output{};
    CHECK(appWindowDeliverFrame(window.delivery, handleOf(window), &frame, &output));
    CHECK(window.seen == Array<String>({"open 0", "frame 200"}));
    CHECK(output.cursor == PrAppCursor_Hand);
    CHECK(output.wantsFrame);
}

TEST_CASE("a window's first event opens it, as a frame would find it then") {
    RecordingWindow window{};
    window.delivery.handler = recordingHandler(window);
    PrAppEvent event{.kind = PrAppEventKind_KeyDown, .keyboard = {.key = 'B'}};
    CHECK(appWindowDeliverEvent(window.delivery, handleOf(window), &event, frameState));
    CHECK(appWindowDeliverEvent(window.delivery, handleOf(window), &event, frameState));
    CHECK(window.seen == Array<String>({"open 0", "key B", "key B"}));
}

TEST_CASE("a window that fails to open is destroyed as the delivery ends, with no frame") {
    RecordingWindow window{};
    window.delivery.handler = recordingHandler(window);
    window.delivery.handler.open = [](void* context, PrAppWindow*, const PrAppFrame*) {
        recordOf(context).seen.push("open failed");
        return false;
    };
    PrAppFrame frame{};
    PrAppOutput output{};
    CHECK_FALSE(appWindowDeliverFrame(window.delivery, handleOf(window), &frame, &output));
    CHECK(window.seen == Array<String>({"open failed"}));
    CHECK_FALSE(output.wantsFrame);
}

TEST_CASE("a window destroyed during its own callback is reported as destroyed when the callback returns") {
    RecordingWindow window{};
    window.delivery.handler = recordingHandler(window);
    window.delivery.opened = true;
    window.delivery.handler.frame = [](void* context, PrAppWindow*, const PrAppFrame*) {
        RecordingWindow& w = recordOf(context);
        // The backend's destroyFn defers the free, and the window stays whole until the callback returns.
        CHECK(appWindowDeliveryDeferDestroy(w.delivery));
        w.seen.push("frame ran");
        return PrAppOutput{.wantsFrame = true};
    };
    PrAppFrame frame{};
    PrAppOutput output{};
    CHECK_FALSE(appWindowDeliverFrame(window.delivery, handleOf(window), &frame, &output));
    CHECK(window.seen == Array<String>({"frame ran"}));
}

TEST_CASE("a window destroyed outside its callbacks is freed at once") {
    AppWindowDelivery delivery{};
    CHECK_FALSE(appWindowDeliveryDeferDestroy(delivery));
}
