// MessagePort: one end of a channel between scripts of an agent (html/#message-ports).

#pragma once

#include "pr/base.h"
#include "pr/dom.h"
#include "pr/html.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// A port is reference counted through its event target. An entangled port names its peer without a counted reference,
// so a host that wants the pair to live together keeps the pair's objects reachable from each other.
typedef struct PrHtmlMessagePort PrHtmlMessagePort;

// A MessagePort as the interface it inherits from.
PR_HANDLE_UPCAST(HtmlMessagePort, DomEventTarget, EventTarget)

// Called with true when a message is queued for delivery at `port`, and with false once none is queued, so that the
// host can keep the port's object alive for that time.
typedef void (*PrHtmlMessagePortPendingFn)(void* PR_NULLABLE context, PrHtmlMessagePort* port, bool pending);

// Creates two entangled ports whose messages arrive in tasks of `window` (html/#dom-messagechannel). The caller holds a
// reference to each.
void prHtmlMessagePortCreatePair(PrHtmlWindow* window, PrHtmlMessagePortPendingFn PR_NULLABLE pending,
    void* PR_NULLABLE context, PrHtmlMessagePort* PR_NULLABLE* PR_NONNULL first,
    PrHtmlMessagePort* PR_NULLABLE* PR_NONNULL second);

// Returns `object` as a MessagePort, or null if it is not one.
PrHtmlMessagePort* PR_NULLABLE prHtmlMessagePortQuery(PrDomEventTarget* PR_NULLABLE object);

// Returns the port that is entangled with this one, or null if it has none.
PrHtmlMessagePort* PR_NULLABLE prHtmlMessagePortPeer(PrHtmlMessagePort* port);

// Returns true if the port was transferred away, which leaves it with no peer and no messages.
bool prHtmlMessagePortIsDetached(const PrHtmlMessagePort* port);

// Returns true if the port belongs to a window. A transferred port has none until prHtmlMessagePortAdopt.
bool prHtmlMessagePortHasWindow(const PrHtmlMessagePort* port);

// Queues `serialized` for the peer, with the ports that the message transfers, in the format that the peer's host reads
// (html/#message-port-post-message-steps). The call takes a reference to each of `ports` and copies the bytes. The
// message is dropped if the port has no peer. Messages arrive in order, once the peer has started.
void prHtmlMessagePortPostMessage(PrHtmlMessagePort* port, PrStringView serialized,
    PrHtmlMessagePort* PR_NONNULL const* PR_NULLABLE ports, uint32_t portCount);

// Starts delivering the messages queued for the port (html/#dom-messageport-start).
void prHtmlMessagePortStart(PrHtmlMessagePort* port);

// Disentangles the port from its peer (html/#dom-messageport-close).
void prHtmlMessagePortClose(PrHtmlMessagePort* port);

// Transfers the port (html/#message-ports:transfer-steps). Returns a new port, with a reference that the caller holds,
// which takes over the peer and the queued messages and has no window. The original port is detached.
PrHtmlMessagePort* prHtmlMessagePortTransfer(PrHtmlMessagePort* port);

// Gives a transferred port the window whose tasks deliver its messages (html/#message-ports:transfer-receiving-steps).
// The port has not started.
void prHtmlMessagePortAdopt(PrHtmlMessagePort* port, PrHtmlWindow* window, PrHtmlMessagePortPendingFn PR_NULLABLE pending,
    void* PR_NULLABLE context);

// Returns the number of ports that a message event's message transferred, and one of them, which the event holds.
uint32_t prHtmlMessageEventPortCount(const PrHtmlMessageEvent* event);
PrHtmlMessagePort* prHtmlMessageEventPort(PrHtmlMessageEvent* event, uint32_t index);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
