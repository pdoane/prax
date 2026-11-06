// The desktop notifications that a program posts: the operating system commands that post them, and the queue from
// which the caller takes them.

#pragma once

#include "pr/base.h"
#include "pr/raw_array.h"
#include "pr/raw_string.h"
#include "pr/string_view.h"
#include "pr/vterm.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

struct Vterm;

// A notification that waits for the caller.
struct VtermNotificationRecord {
    PrRawString text;           // the title followed by the body
    uint32_t titleSize;         // the title's bytes at the start of `text`
    PrVtermNotificationCondition condition;
};

// A notification of OSC 99 whose chunks are still arriving, as the bytes that the chunks decoded to.
struct VtermNotificationDraft {
    bool open;
    RawArray<char> id;
    RawArray<char> title;
    RawArray<char> body;
    PrVtermNotificationCondition condition;
};

// A terminal's notifications.
struct VtermNotifications {
    RawArray<VtermNotificationRecord> queue;    // oldest first
    VtermNotificationDraft draft;

    // The notifications that the last take wrote. The caller's views point into their text.
    RawArray<VtermNotificationRecord> taken;
};

// Frees the notifications' storage.
void vtermNotificationsReset(VtermNotifications* notifications);

// Drops every notification and the draft, and keeps the storage.
void vtermNotificationsClear(VtermNotifications* notifications);

// Drops the draft, as a program's reset does.
void vtermNotificationsDropDraft(VtermNotifications* notifications);

// Runs OSC 9, 99 or 777 (`command`) with `rest`, the text after its number. A query's answer ends with `terminator`.
void vtermNotificationCommand(Vterm* vterm, uint32_t command, PrStringView rest, uint8_t terminator);

// Takes the queued notifications (prVtermTakeNotifications).
uint32_t vtermNotificationsTake(VtermNotifications* notifications, PrVtermNotification* PR_NULLABLE out, uint32_t capacity);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
