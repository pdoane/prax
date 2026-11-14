// The events and the answers that wait for the callback that runs.

#include "pr/alloc.h"
#include "pr/array.h"
#include "pr/string.h"

#include "app/backends/shared/src/app_shared_events.h"

#include "tools/harness/test.h"

namespace pr_impl {

namespace {

// What a delivery saw: each event's kind and text, or "answer", and the window that it went to. A delivery takes what
// waits as a backend does, and empties the queue.
struct Delivered {
    Array<String> seen;
    Array<PrAppWindow*> windows;
    bool running = true;
    AppEventQueue* queue = nullptr;
    PrAppWindow* holdDuring = nullptr;      // a window that a delivery holds another event for, once
    PrAppWindow* stopAfter = nullptr;       // a window whose event stops the run
};

void deliverTo(void* PR_NULLABLE context, PrAppWindow* PR_NULLABLE window, const PrAppEvent* PR_NULLABLE event,
        AppAnswer* PR_NULLABLE answer) {
    Delivered& d = *static_cast<Delivered*>(context);
    d.windows.push(window);
    if (answer != nullptr) {
        d.seen.push("answer");
        return;
    }
    String text(prAppEventText(event));
    if (event->kind == PrAppEventKind_Open) {
        for (uint32_t i = 0; i < event->open.argumentCount; ++i)
            text.append(event->open.arguments[i]);
    }
    if (event->kind == PrAppEventKind_TrackpadTouches)
        text = String(format("{}", event->trackpad.touches[1].id));
    d.seen.push(String(format("{}:{}", prAppEventKindName(event->kind), text)));
    if (d.holdDuring != nullptr && window == d.holdDuring) {
        d.holdDuring = nullptr;
        appEventQueueHold(*d.queue, nullptr, PrAppEvent{.kind = PrAppEventKind_Wake});
    }
    if (window != nullptr && window == d.stopAfter)
        d.running = false;
}

void deliverAll(AppEventQueue& queue, AppDataSources& sources, Delivered& d) {
    uint32_t at = 0;
    PrAppWindow* window = nullptr;
    PrAppEvent event{};
    AppAnswer* answer = nullptr;
    while (d.running && appEventQueueNext(queue, &at, &window, &event, &answer))
        deliverTo(&d, window, answer != nullptr ? nullptr : &event, answer);
    appEventQueueClear(queue, &sources);
}

// A store with text, for a source.
AppDataStore* storeOf(PrStringView text) {
    AppDataStore* store = allocZeroed<AppDataStore>();
    appDataStoreSetText(*store, text);
    return store;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("held events keep copies of their strings, and arrive in order with what their delivery holds") {
    AppDataSources sources{};
    appDataSourcesInit(sources);
    AppEventQueue queue{};
    PrAppWindow* first = reinterpret_cast<PrAppWindow*>(uintptr_t(0x10));
    PrAppWindow* second = reinterpret_cast<PrAppWindow*>(uintptr_t(0x20));
    {
        String typed("typed");
        const PrStringView arguments[] = {"a", "b"};
        const PrAppTrackpadTouch touches[] = {{.id = 3}, {.id = 4}};
        appEventQueueHold(queue, first, PrAppEvent{.kind = PrAppEventKind_Text, .input = {.text = typed}});
        appEventQueueHold(queue, nullptr,
            PrAppEvent{.kind = PrAppEventKind_Open, .open = {.arguments = arguments, .argumentCount = 2}});
        appEventQueueHold(queue, second,
            PrAppEvent{.kind = PrAppEventKind_TrackpadTouches, .trackpad = {.touches = touches, .count = 2}});
        // A key that its delivery held ahead of the events from the mark.
        appEventQueueHold(queue, first, PrAppEvent{.kind = PrAppEventKind_KeyDown, .keyboard = {.text = "k"}}, 1);
        typed.assign("overwritten");
    }
    appEventQueueHoldAnswer(queue, appAnswerCreate(AppAnswerKind_Status, 1, PrStatus_Ok));
    Delivered d;
    d.queue = &queue;
    d.holdDuring = second;
    deliverAll(queue, sources, d);
    CHECK(d.seen == Array<String>({"Text:typed", "KeyDown:k", "Open:ab", "TrackpadTouches:4", "answer", "Wake:"}));
    CHECK(d.windows == Array<PrAppWindow*>({first, first, nullptr, second, nullptr, nullptr}));
    CHECK(queue.held.count == 0);
    appEventQueueReset(queue, &sources);
    appDataSourcesReset(sources);
}

TEST_CASE("a closed window's events are dropped, and a stopped run drops the rest, releasing what they hand over") {
    AppDataSources sources{};
    appDataSourcesInit(sources);
    AppEventQueue queue{};
    PrAppWindow* closing = reinterpret_cast<PrAppWindow*>(uintptr_t(0x10));
    PrAppWindow* open = reinterpret_cast<PrAppWindow*>(uintptr_t(0x20));
    PrAppDataSource dropped = appDataSourceCreate(sources, storeOf("dropped"), nullptr);
    PrAppDataSource late = appDataSourceCreate(sources, storeOf("late"), nullptr);
    appEventQueueHold(queue, closing, PrAppEvent{.kind = PrAppEventKind_Drop, .drag = {.drop = dropped}});
    appEventQueueHold(queue, open, PrAppEvent{.kind = PrAppEventKind_KeyDown});
    appEventQueueHold(queue, open, PrAppEvent{.kind = PrAppEventKind_Drop, .drag = {.drop = late}});
    appEventQueueForget(queue, closing);
    Delivered d;
    d.stopAfter = open;
    deliverAll(queue, sources, d);
    CHECK(d.seen == Array<String>({"KeyDown:"}));
    // The queue held each drop's reference, which it released whether or not the drop arrived.
    CHECK(appDataSourceStore(sources, dropped) == nullptr);
    CHECK(appDataSourceStore(sources, late) == nullptr);
    appEventQueueReset(queue, &sources);
    appDataSourcesReset(sources);
}

TEST_CASE("a dialog's answer holds its source until its callback returns, and a reset releases what waits") {
    AppDataSources sources{};
    appDataSourcesInit(sources);
    AppDataRequests requests{};
    appDataRequestsInit(requests, sources);
    static PrAppDataSource answered = kPrAppDataSourceNone;
    PrAppFileDialogFn done = [](void*, PrStatus, PrAppDataSource source) { answered = source; };
    uint32_t id = appDataRequestsAdd(requests, kPrAppDataSourceNone, AppDataAnswer_Source, (void (*)(void))done, nullptr);
    AppAnswer* answer = appAnswerCreate(AppAnswerKind_Source, id, PrStatus_Ok);
    answer->source = appDataSourceCreate(sources, storeOf("chosen"), nullptr);
    PrAppDataSource chosen = answer->source;
    appAnswerCall(*answer, requests, sources);
    CHECK(answered == chosen);
    // The program did not retain the source, so it is gone once the callback returns.
    CHECK(appDataSourceStore(sources, chosen) == nullptr);
    appAnswerDestroy(answer, &sources);

    AppEventQueue queue{};
    PrAppDataSource waiting = appDataSourceCreate(sources, storeOf("waiting"), nullptr);
    appEventQueueHold(queue, nullptr, PrAppEvent{.kind = PrAppEventKind_Open, .open = {.source = waiting}});
    AppAnswer* held = appAnswerCreate(AppAnswerKind_Source, 9, PrStatus_Ok);
    held->source = appDataSourceCreate(sources, storeOf("held"), nullptr);
    PrAppDataSource heldSource = held->source;
    appEventQueueHoldAnswer(queue, held);
    appEventQueueReset(queue, &sources);
    CHECK(appDataSourceStore(sources, waiting) == nullptr);
    CHECK(appDataSourceStore(sources, heldSource) == nullptr);
    appDataRequestsReset(requests);
    appDataSourcesReset(sources);
}
