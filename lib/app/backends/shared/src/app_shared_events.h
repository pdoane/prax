// The events and the answers that wait for the callback that runs, which each backend that runs callbacks of its own
// embeds, so that no callback runs during another.

#pragma once

#include "pr/app.h"
#include "pr/base.h"
#include "pr/raw_array.h"

#include "app/backends/shared/src/app_shared_data.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// ---------------------------------------------------------------------------------------------------------------------
// Answers

// What answers a request: content from a store, a file's bytes, a write's status, or a dialog's source.
enum {
    AppAnswerKind_Store,
    AppAnswerKind_Bytes,
    AppAnswerKind_Status,
    AppAnswerKind_Source,
};
typedef uint8_t AppAnswerKind;

// The answer to the request `id`, which owns what it answers with. A Store answer answers from `external`, a source's
// store that the request's reference keeps, in place of its own store if it is set. A Source answer holds a reference
// to its source until its callback returns.
struct AppAnswer {
    AppAnswerKind kind;
    uint32_t id;
    PrStatus status;
    AppDataStore store;
    AppDataStore* PR_NULLABLE external;
    RawArray<uint8_t> bytes;
    PrAppDataSource source;
};

// Makes an answer of `kind` to the request `id`, or frees one, releasing the source that it holds to `sources` if it is
// not null, as when the application is gone.
AppAnswer* appAnswerCreate(AppAnswerKind kind, uint32_t id, PrStatus status);
void appAnswerDestroy(AppAnswer* PR_NULLABLE answer, AppDataSources* PR_NULLABLE sources);

// Calls the callback of the answer's request. A Source answer's source loses the answer's reference once the callback
// returns, so that the program keeps it only by retaining it.
void appAnswerCall(AppAnswer& answer, AppDataRequests& requests, AppDataSources& sources);

// ---------------------------------------------------------------------------------------------------------------------
// Held events

// An event or an answer that waits. It owns copies of the event's strings and arrays, because the sender's do not
// outlive the call.
struct AppHeldEvent {
    PrAppWindow* PR_NULLABLE window;        // the target window, or null for the application
    PrAppEvent event;
    RawArray<char> text;
    AppStrings arguments;                   // the arguments of an Open event
    RawArray<PrAppTrackpadTouch> touches;   // the touches of a TrackpadTouches event
    bool dropped;                           // true if the target window closed before delivery
    AppAnswer* PR_NULLABLE answer;          // an answer in place of an event
};

// The events and answers that wait, in order.
struct AppEventQueue {
    RawArray<AppHeldEvent> held;
};

// Holds a copy of `event` for `window`, or for the application if it is null, at the end of the queue, or at `at` if it
// is less than the count, ahead of the events from there on. The queue takes the reference to the source that the event
// hands over.
void appEventQueueHold(AppEventQueue& queue, PrAppWindow* PR_NULLABLE window, const PrAppEvent& event,
    uint32_t at = UINT32_MAX);

// Holds `answer`, which the queue takes, at the end of the queue.
void appEventQueueHoldAnswer(AppEventQueue& queue, AppAnswer* answer);

// Drops the events that wait for `window`, which is closing. The events stay in place, since a delivery that is under
// way counts through the queue.
void appEventQueueForget(AppEventQueue& queue, PrAppWindow* window);

// Takes the next event or answer to deliver, from `*at` on, past the events of a window that closed, and advances `*at`
// past it. Writes an answer to `answer`, or else null, the event to `event`, with its strings and arrays in the queue's
// copies, and the event's target to `window`. Returns false once nothing is left.
//
// A backend delivers what waits by taking each in turn while it runs, which takes the events that arrive during the
// delivery too, and then empties the queue (appEventQueueClear). The queue may grow during a delivery, which moves its
// events but not what they own.
bool appEventQueueNext(AppEventQueue& queue, uint32_t* at, PrAppWindow* PR_NULLABLE* PR_NONNULL window,
    PrAppEvent* event, AppAnswer* PR_NULLABLE* PR_NONNULL answer);

// Frees the events and answers from `at` on, as for those that arrive once the run stops, or all of them, emptying the
// queue and keeping its storage, or frees the queue. Each releases the sources that what it frees hands over to
// `sources`, if it is not null, as it is not when the application is gone.
void appEventQueueDiscard(AppEventQueue& queue, uint32_t at, AppDataSources* PR_NULLABLE sources);
void appEventQueueClear(AppEventQueue& queue, AppDataSources* PR_NULLABLE sources);
void appEventQueueReset(AppEventQueue& queue, AppDataSources* PR_NULLABLE sources);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
