// MessagePort
//
// html - https://html.spec.whatwg.org/multipage/web-messaging.html
//
// The state of a message port, and the event that messages arrive in.

#pragma once

#include "pr/array.h"
#include "pr/html_message.h"

#include "dom/src/dom_event.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// A message on its way: its serialized bytes, and the ports that it transfers, each with a reference.
struct HtmlMessage {
    PrString data;
    Array<PrDomEventTarget* PR_NONNULL> ports;
};

// One end of a channel.
struct HtmlMessagePort : DomCountedTarget {
    PrHtmlWindow* PR_NULLABLE window;       // the window that delivers the messages and is held, or null in transit
    PrHtmlMessagePort* PR_NULLABLE peer;    // the entangled port, uncounted, which clears this when it goes
    PrHtmlMessagePortPendingFn PR_NULLABLE pending;
    void* PR_NULLABLE context;

    // The port message queue (html/#port-message-queue), in the order the messages were posted. A started port queues
    // one task for each message, which takes the first message that remains.
    Array<HtmlMessage> queue;
    uint32_t queueHead;

    uint32_t scheduled;                     // the tasks that will deliver a message
    bool started;
    bool detached;                          // true once the port was transferred away
};

// Releases what a message holds that no event took.
void htmlMessageReset(HtmlMessage* message);

// Fires a trusted message event at `target` (html/#messageevent). The event takes the message's bytes and ports, and
// `origin`.
void htmlFireMessage(PrDomEventTarget* target, HtmlMessage* message, PrString origin, PrHtmlWindow* PR_NULLABLE source);

} // namespace pr_impl

// C opaque types
struct PrHtmlMessagePort : pr_impl::HtmlMessagePort {};

PR_ASSUME_NONNULL_END
