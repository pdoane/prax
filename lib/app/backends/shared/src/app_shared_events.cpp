// The events and the answers that wait for the callback that runs.

#include "app/backends/shared/src/app_shared_events.h"

#include "pr/alloc.h"

namespace pr_impl {

namespace {

// Frees what a held event owns, and releases the source that it hands over, to `sources` if it is not null.
void heldReset(AppHeldEvent& held, AppDataSources* PR_NULLABLE sources) {
    held.text.reset();
    appStringsReset(held.arguments);
    held.touches.reset();
    bool answer = held.answer != nullptr;
    appAnswerDestroy(held.answer, sources);
    held.answer = nullptr;
    if (!answer && sources != nullptr)
        appDataSourceRelease(*sources, prAppEventSource(&held.event));
}

} // namespace

AppAnswer* appAnswerCreate(AppAnswerKind kind, uint32_t id, PrStatus status) {
    AppAnswer* answer = allocZeroed<AppAnswer>();
    answer->kind = kind;
    answer->id = id;
    answer->status = status;
    return answer;
}

void appAnswerDestroy(AppAnswer* PR_NULLABLE answer, AppDataSources* PR_NULLABLE sources) {
    if (answer == nullptr)
        return;
    if (sources != nullptr)
        appDataSourceRelease(*sources, answer->source);
    appDataStoreReset(answer->store);
    answer->bytes.reset();
    prMemoryFree(answer);
}

void appAnswerCall(AppAnswer& answer, AppDataRequests& requests, AppDataSources& sources) {
    switch (answer.kind) {
        case AppAnswerKind_Store:
            appDataRequestsAnswerFromStore(requests, answer.id, answer.status,
                answer.external != nullptr ? answer.external : &answer.store);
            break;
        case AppAnswerKind_Bytes:
            appDataRequestsAnswerBytes(requests, answer.id, answer.status,
                PrByteView{answer.bytes.data, answer.bytes.count});
            break;
        case AppAnswerKind_Status:
            appDataRequestsAnswerStatus(requests, answer.id, answer.status);
            break;
        default:
            appDataRequestsAnswerSource(requests, answer.id, answer.status, answer.source);
            appDataSourceRelease(sources, answer.source);
            answer.source = kPrAppDataSourceNone;
            break;
    }
}

void appEventQueueHold(AppEventQueue& queue, PrAppWindow* PR_NULLABLE window, const PrAppEvent& event, uint32_t at) {
    AppHeldEvent held{.window = window, .event = event};
    PrStringView text = prAppEventText(&event);
    if (text.size != 0)
        held.text.append(text.data, uint32_t(text.size));
    if (event.kind == PrAppEventKind_Open) {
        for (uint32_t i = 0; i < event.open.argumentCount; ++i)
            appStringsAdd(held.arguments, event.open.arguments[i]);
    }
    if (event.kind == PrAppEventKind_TrackpadTouches && event.trackpad.count != 0)
        held.touches.append(event.trackpad.touches, event.trackpad.count);
    if (at < queue.held.count)
        queue.held.insert(at, held);
    else
        queue.held.push(held);
}

void appEventQueueHoldAnswer(AppEventQueue& queue, AppAnswer* answer) {
    queue.held.push(AppHeldEvent{.answer = answer});
}

void appEventQueueForget(AppEventQueue& queue, PrAppWindow* window) {
    for (uint32_t i = 0; i < queue.held.count; ++i) {
        if (queue.held.data[i].window == window)
            queue.held.data[i].dropped = true;
    }
}

bool appEventQueueNext(AppEventQueue& queue, uint32_t* at, PrAppWindow* PR_NULLABLE* PR_NONNULL window,
        PrAppEvent* event, AppAnswer* PR_NULLABLE* PR_NONNULL answer) {
    while (*at < queue.held.count && queue.held.data[*at].dropped)
        ++*at;
    if (*at == queue.held.count)
        return false;
    AppHeldEvent& held = queue.held.data[*at];
    ++*at;
    *window = held.window;
    *answer = held.answer;
    *event = held.event;
    if (held.answer != nullptr)
        return true;
    prAppEventSetText(event, PrStringView{held.text.data != nullptr ? held.text.data : "", held.text.count});
    if (event->kind == PrAppEventKind_Open)
        event->open.arguments = appStringsViews(held.arguments);
    if (event->kind == PrAppEventKind_TrackpadTouches)
        event->trackpad.touches = held.touches.data;
    return true;
}

void appEventQueueDiscard(AppEventQueue& queue, uint32_t at, AppDataSources* PR_NULLABLE sources) {
    for (uint32_t i = at; i < queue.held.count; ++i)
        heldReset(queue.held.data[i], sources);
    if (at < queue.held.count)
        queue.held.resize(at);
}

void appEventQueueClear(AppEventQueue& queue, AppDataSources* PR_NULLABLE sources) {
    appEventQueueDiscard(queue, 0, sources);
}

void appEventQueueReset(AppEventQueue& queue, AppDataSources* PR_NULLABLE sources) {
    appEventQueueDiscard(queue, 0, sources);
    queue.held.reset();
}

} // namespace pr_impl
