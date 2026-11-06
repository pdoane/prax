// A terminal's input: the bytes that a key, text, a paste, the pointer or a change of focus sends to the program. The
// bytes depend on the modes that the program set. Each call queues them as the terminal's output (prVtermOutput).

#pragma once

#include "pr/app.h"
#include "pr/base.h"
#include "pr/string_view.h"
#include "pr/vec.h"
#include "pr/vterm.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Where the screen is in the window, and how keys read.
typedef struct PrVtermInputDesc {
    PrVec2 origin;              // the top left of the screen's first cell, in the window's logical pixels
    PrVec2 cellExtent;          // the extent of a cell, in logical pixels, each greater than zero

    // If set, Alt works as a meta key: it sends an escape followed by the key's character without Alt. Otherwise Alt
    // types the text that the layout gives the key with Alt, as Option does on a Mac.
    bool altSendsEscape;
} PrVtermInputDesc;

// Sends `event` to the program. Returns true if the terminal took the event, in which case the caller does not also act
// on it.
//
// - A key sends an escape sequence if it is a named key, a character with Control or Alt, or any key while the program
//   asks for keys as escape codes. The key's sequence already carries its text, so the terminal takes the Text event
//   that follows with the same text and drops it. A Text event with other text is sent, and so is one that follows
//   any event other than the key's release. Any other key sends nothing, and the Text event that follows sends its
//   text.
// - A key's repeat and release are reported if the program asked for the event types of kitty's keyboard protocol.
//   Otherwise a repeat is sent as a press, and a release is left to the caller. A caller passes a release only for a
//   key whose press it passed. The protocol's alternate keys are approximated from the event. The shifted key comes
//   from the key's meaning and its text, and the base layout key from its position on the PC-101 layout.
// - Clipboard text is sent as a paste, as prVtermInputPaste sends it.
// - A pointer's press, release and move, and a scroll, are reported if the program asked for them. If it did not, the
//   pointer is left to the caller, for example to select text. While the alternate screen shows, a scroll that is not
//   reported sends cursor keys instead, so that a pager scrolls, unless the program turned off alternate scroll mode
//   (1007). Otherwise the scroll is left to the caller, for example to scroll the scrollback.
//
// Every other event is left to the caller.
bool prVtermInputEvent(PrVterm* vterm, const PrAppEvent* event, const PrVtermInputDesc* desc);

// Sends `text` that the user pasted. Its line breaks are sent as carriage returns. If the program asked for bracketed
// paste, the text is bracketed, and escapes are removed from it so that it cannot end the bracket early.
void prVtermInputPaste(PrVterm* vterm, PrStringView text);

// Sends a report that the terminal gained or lost focus, if the program asked for focus reports.
void prVtermInputFocus(PrVterm* vterm, bool focused);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
