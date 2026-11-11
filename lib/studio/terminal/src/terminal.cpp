// Studio's terminal: the program's process and screen, the user's input, and drawing the screen.

#include "pr/terminal.h"

#include "pr/alloc.h"
#include "pr/assert.h"
#include "pr/atomic.h"
#include "pr/compress.h"
#include "pr/draw_image.h"
#include "pr/file.h"
#include "pr/float.h"
#include "pr/image.h"
#include "pr/kit_button.h"
#include "pr/kit_menu.h"
#include "pr/kit_text.h"
#include "pr/kit_theme.h"
#include "pr/layout_frame.h"
#include "pr/layout_style.h"
#include "pr/mem.h"
#include "pr/memory.h"
#include "pr/raw_array.h"
#include "pr/raw_string.h"
#include "pr/string_ops.h"
#include "pr/thread.h"
#include "pr/time.h"
#include "pr/ui.h"
#include "pr/url_file.h"
#include "pr/vterm_graphics.h"
#include "pr/vterm_input.h"

#include "studio/terminal/src/terminal_draw.h"
#include "studio/terminal/src/terminal_link.h"
#include "studio/terminal/src/terminal_shell.h"

namespace pr_impl {

namespace {

// If one delivery of output drops at least this many scrollback lines, as a clear does, the parser returns their memory
// to the system immediately instead of waiting for the allocator's next purge.
constexpr uint32_t kTrimLines = 4096;

// The duration of each phase of the blink of the cursor and of blinking text, in seconds.
constexpr double kBlinkSeconds = 0.5;

// The number of lines of a paste that its confirmation shows, and the most bytes it shows of each.
constexpr uint32_t kPreviewLines = 4;
constexpr size_t kPreviewLineBytes = 72;

// The longest that a synchronized update holds the screen back, in seconds, in case the program never ends it.
constexpr double kSynchronizedSeconds = 1.0;

// The duration of the bell's flash in seconds, and its starting opacity. The flash is the foreground color over the
// screen.
constexpr double kBellSeconds = 0.15;
constexpr float kBellOpacity = 0.25f;

// The gutter left of the screen holds the command marks, so that they cover no cell. These are the gutter's width, the
// width of a mark, and the mark's inset from the gutter's left edge, all in px.
constexpr float kGutterWidth = 6;
constexpr float kGutterMarkWidth = 2;
constexpr float kGutterMarkInset = 2;

// The environment variables that identify the terminal. The caller's values take precedence. TERM_PROGRAM_VERSION
// replaces the version of the terminal that launched this one.
constexpr PrStringView kEnvironment[] = {
    "TERM=xterm-256color", "COLORTERM=truecolor", "TERM_PROGRAM=prax",
    "TERM_PROGRAM_VERSION=" PR_TERMINAL_VERSION
};

// The 16 named colors, chosen to suit both dark and light backgrounds.
constexpr PrColor8 kNamedColors[16] = {
    {0x00, 0x00, 0x00, 0xff}, {0xcd, 0x31, 0x31, 0xff}, {0x0d, 0xbc, 0x79, 0xff}, {0xe5, 0xe5, 0x10, 0xff},
    {0x24, 0x72, 0xc8, 0xff}, {0xbc, 0x3f, 0xbc, 0xff}, {0x11, 0xa8, 0xcd, 0xff}, {0xe5, 0xe5, 0xe5, 0xff},
    {0x66, 0x66, 0x66, 0xff}, {0xf1, 0x4c, 0x4c, 0xff}, {0x23, 0xd1, 0x8b, 0xff}, {0xf5, 0xf5, 0x43, 0xff},
    {0x3b, 0x8e, 0xea, 0xff}, {0xd6, 0x70, 0xd6, 0xff}, {0x29, 0xb8, 0xdb, 0xff}, {0xff, 0xff, 0xff, 0xff},
};

// The unit by which a selection grows from its press. A single click selects cells, a double click words, and a triple
// click lines.
enum {
    TerminalUnit_Cell,
    TerminalUnit_Word,
    TerminalUnit_Line,
};
typedef uint8_t TerminalUnit;

// The cells of a line from `first` to `last`, inclusive. `line` is a line number, which stays fixed as lines leave the
// scrollback (prVtermScrollbackDropped).
struct TerminalSpan {
    uint64_t line;
    uint32_t first;
    uint32_t last;
};

bool isSameSpan(TerminalSpan a, TerminalSpan b) {
    return a.line == b.line && a.first == b.first && a.last == b.last;
}

// What a link opens.
enum {
    TerminalLinkKind_Url,       // a URL, through the application
    TerminalLinkKind_File,      // a file that exists, through the terminal's handler
};
typedef uint8_t TerminalLinkKind;

// A link under a cell, and what it opens. A hyperlink that the program wrote (OSC 8) covers every cell that holds
// `hyperlink`. A link found in the text covers the cells of `span`. The target is a URL or an absolute file path. The
// line and column count from 1, and are 0 if the text names none.
struct TerminalLink {
    TerminalSpan span;
    PrVtermLinkIndex hyperlink;
    TerminalLinkKind kind;
    PrRawString target;
    uint32_t line;
    uint32_t column;
};

// The cached drawing of a row of the view. The backgrounds and the glyphs with decorations are separate lists, so that
// images can draw between them. The cache is valid while the line's revision and its selected columns are unchanged,
// and, if the row has blinking text, while the blink phase is unchanged.
struct TerminalRow {
    PrDrawList* PR_NULLABLE backgrounds;
    PrDrawList* PR_NULLABLE list;
    uint64_t revision;
    uint32_t selectionStart;
    uint32_t selectionEnd;
    bool blinks;
    bool blinkHidden;
};

// A drawing image made from the pixels of a screen image. `key` is the screen image's key (PrVtermPlacement), and
// `drawing` is the number of the last drawing that showed it (Terminal::drawings).
struct TerminalImage {
    uint64_t key;
    const PrDrawImage* image;
    uint32_t drawing;
};

// What the last drawing of the screen showed. A frame that would show the same keeps the drawing.
struct TerminalShown {
    uint64_t revision;
    uint64_t top;
    bool hasSelection;
    bool columnSelection;
    TerminalSpan anchor;
    TerminalSpan head;
    bool hasLink;
    TerminalSpan link;
    PrVtermLinkIndex hyperlink;
    PrVec2 extent;
    bool focused;
    bool cursorOn;
    uint32_t generation;
    float flash;                // the opacity of the bell's flash
    bool hasJump;
    uint64_t jumpLine;

    // Whether the drawing is in the blink phase that hides blinking text, and whether any of its rows has blinking
    // text. The phase matters only if a row has blinking text.
    bool blinkHidden;
    bool blinks;
};

bool isSame(const TerminalShown& a, const TerminalShown& b) {
    bool selection = a.hasSelection == b.hasSelection && a.columnSelection == b.columnSelection &&
        isSameSpan(a.anchor, b.anchor) && isSameSpan(a.head, b.head);
    bool link = a.hasLink == b.hasLink && isSameSpan(a.link, b.link) && a.hyperlink == b.hyperlink;
    bool jump = a.hasJump == b.hasJump && a.jumpLine == b.jumpLine;
    return a.revision == b.revision && a.top == b.top && selection && link && a.extent.x == b.extent.x &&
        a.extent.y == b.extent.y && a.focused == b.focused && a.cursorOn == b.cursorOn &&
        a.generation == b.generation && a.flash == b.flash && jump;
}

// A line of the view copied from the screen. `line` points into `cells` and `codePoints`.
struct TerminalViewLine {
    PrVtermLine line;
    RawArray<PrVtermCell> cells;
    RawArray<uint32_t> codePoints;
};

// What a frame draws, copied from the screen while the frame holds the screen's lock. The drawing reads only this copy,
// so the parser can change the screen meanwhile. A line is copied again only if its revision changed.
struct TerminalSnapshot {
    bool valid;
    bool held;                          // a synchronized update holds the screen back, so the copy is stale
    uint64_t revision;
    uint64_t colorRevision;
    PrUVec2 extent;
    uint64_t firstLine;                 // the line number of the first scrollback line
    uint32_t topIndex;                  // the index of the view's top line (prVtermLine)
    PrVtermCursor cursor;
    PrVtermModeFlags modes;
    TerminalColors colors;
    RawArray<TerminalViewLine> lines;   // by row of the view, as many as it shows
};

struct Terminal {
    PrKit* kit;
    PrUi* ui;
    PrApp* PR_NULLABLE app;
    PrProcess* PR_NULLABLE process;
    PrVterm* vterm;
    TerminalDrawer drawer;

    // Images that the program shows (pr/vterm.h): the codecs that decode them, the drawing images made from them, the
    // placements that the drawing shows, and a count of drawings. A drawing frees the drawing images that it does not
    // show.
    PrCompressCodec* PR_NULLABLE compressCodec;
    PrImageCodec* PR_NULLABLE imageCodec;
    RawArray<TerminalImage> images;
    RawArray<PrVtermPlacement> placements;
    uint32_t drawings;

    // The program: the extent of its pseudoterminal in cells and in target pixels as last sent to the process, its file
    // name, the directory it started in, and whether the screen has shown its exit.
    PrUVec2 processExtent;
    PrUVec2 processPixels;
    PrRawString name;
    PrRawString directory;              // null for the launcher's directory
    bool exitShown;

    PrLayoutNode node;
    bool focused;
    bool focusAsked;
    double blinkStart;
    double synchronizedSince;           // when the program's synchronized update began, or negative for none

    // The target pixel ratio and font revision of the cell metrics. `generation` changes with either, and every drawing
    // depends on it.
    float metricsRatio;
    uint64_t fontRevision;
    uint32_t generation;

    // The theme palette that the screen's color roles were last set from.
    PrKitPalette palette;
    bool hasPalette;

    // The scroll position. While the view is scrolled back, `viewTop` is the line number of its top line.
    // `scrollRemainder` is the fraction of a line scrolled that has not yet moved the view.
    bool scrolledBack;
    uint64_t viewTop;
    float scrollRemainder;

    // The line number of the prompt that the view last scrolled to. It is cleared when the view returns to the screen.
    bool hasJump;
    uint64_t jumpLine;

    // The selection. `anchor` is the unit at the press and `head` is the unit under the pointer. With Alt, they are the
    // corners of a block. A press that does not move selects nothing, unless it selects a word or a line.
    bool selecting;
    bool hasSelection;
    bool columnSelection;
    bool moved;
    TerminalUnit unit;
    TerminalSpan press;
    PrAppModifierFlags pressModifiers;
    TerminalSpan anchor;
    TerminalSpan head;

    // The text of a copy or paste key that the view handled. The Text event that follows the key is dropped if it
    // matches. Any event other than the key's release clears it.
    char dropText[16];
    uint8_t dropTextSize;

    // A bit set, by PrAppKeyCode, of the keys whose presses went to the program, so that their releases go too. A key
    // with an unidentified code has no bit, and its release always goes to the program.
    uint32_t programKeys[(PrAppKeyCode_Count + 31) / 32];

    // The pointer over the screen if the program does not read it, and the modifiers of the latest event. While the
    // link modifier is held, the view underlines the link under the pointer, and a click opens it.
    bool hasPointer;
    PrVec2 pointer;
    PrAppModifierFlags modifiers;
    bool hasLink;
    TerminalLink link;

    // Opens a file that a link names. A relative path resolves against the program's working directory.
    PrTerminalOpenFileFn PR_NULLABLE openFile;
    void* PR_NULLABLE openFileContext;

    // A multi-line paste that waits for the user's confirmation, or the null string. The confirmation dialog is open
    // while it waits.
    PrRawString paste;

    RawArray<TerminalRow> rows;         // by row of the view
    uint64_t rowsColorRevision;         // the screen's color revision when the rows were drawn
    TerminalShown shown;
    bool hasShown;

    // The bell and notifications: the notifications' tag, the alerts that the user has not seen, and whether the
    // terminal requested attention since the window was last active.
    uint64_t notificationTag;
    PrTerminalAlertFlags alerts;
    bool attentionAsked;

    // The bell's flash: whether the next drawing starts one, and when the current one began, or negative for none.
    bool bellPending;
    double bellTime;

    // Whether a frame is declaring the terminal now, and whether a frame declared it since the last update. The
    // terminal counts as shown if either is set (isVisible).
    bool inFrame;
    bool declared;

    // A link that a click opened. It opens after the frame releases the screen's lock, because its handler may call the
    // terminal.
    bool hasOpened;
    TerminalLink opened;

    RawArray<char> title;               // the program's title as of the last frame or update, or empty
    TerminalSnapshot snapshot;
    PrTerminalLockInfo waits;           // the UI thread's waits for the screen's lock
    Atomic<uint64_t> parsedBytes;       // the output that the parser has taken, which the UI's waits compare

    // Threading. The parser runs on the process's thread (receiveOutput) and writes into `vterm`. `mutex` guards
    // `vterm` and the fields after it. The parser holds the mutex for one slice of output at a time. If `uiWaiting` is
    // nonzero at the end of a slice, the parser waits on `turn` until the UI thread is done. The UI therefore never
    // waits for more than one slice.
    PrMutex mutex;
    PrCondition turn;
    Atomic<uint32_t> uiWaiting;
    bool parserWaiting;

    // State that the parser shares with the UI: the events it took from the screen, and whether it woke the UI without
    // a service since. The parser wakes the UI once for new events. While the terminal shows, it also wakes the UI if
    // the screen changed since the last copy (`copiedRevision`), unless a synchronized update holds the screen back.
    // The start of a synchronized update also wakes the UI, so that the UI can time it out.
    PrVtermEventFlags parsedEvents;
    bool wakePending;
    bool showing;
    bool parsedSynchronized;
    uint64_t copiedRevision;
};

PrColor8 color8Of(PrColor color) {
    auto channel = [](float value) { return uint8_t(prF32Round(prF32Clamp(value, 0, 1) * 255)); };
    return PrColor8{channel(color.r), channel(color.g), channel(color.b), 0xff};
}

void wakeApp(void* PR_NULLABLE context) {
    prAppWake(static_cast<PrApp*>(context));
}

// Records a wait of `milliseconds` for the screen's lock, during which the parser took `bytes` of output.
void lockWaitAdd(PrTerminalLockInfo* info, double milliseconds, uint64_t bytes) {
    ++info->count;
    info->worstBytes = bytes > info->worstBytes ? bytes : info->worstBytes;
    info->totalMilliseconds += milliseconds;
    info->worstMilliseconds = milliseconds > info->worstMilliseconds ? milliseconds : info->worstMilliseconds;
    uint32_t bucket = 0;
    for (double bound = 0.001; bucket + 1 < kPrTerminalLockBuckets && milliseconds >= bound; bound *= 2)
        ++bucket;
    ++info->buckets[bucket];
}

// Locks the screen on the UI thread, and records how long the wait took. While the UI waits, the parser yields the lock
// at the end of its current slice.
void lockScreen(const Terminal* constTerminal) {
    Terminal* terminal = const_cast<Terminal*>(constTerminal);
    terminal->uiWaiting.fetchAdd(1);
    double start = prTimeMonotonicMilliseconds();
    uint64_t parsed = terminal->parsedBytes.load();
    prMutexLock(&terminal->mutex);
    lockWaitAdd(&terminal->waits, prTimeMonotonicMilliseconds() - start, terminal->parsedBytes.load() - parsed);
    terminal->uiWaiting.fetchSub(1);
}

void unlockScreen(const Terminal* constTerminal) {
    Terminal* terminal = const_cast<Terminal*>(constTerminal);
    if (terminal->parserWaiting)
        prConditionSignal(&terminal->turn);
    prMutexUnlock(&terminal->mutex);
}

// Sends the screen's pending output to the program: its replies to queries and the user's input. The caller holds the
// screen's lock.
void flushOutput(Terminal* terminal, PrProcess* process) {
    PrByteView output = prVtermOutput(terminal->vterm);
    if (output.size == 0)
        return;
    prProcessWrite(process, output);
    prVtermOutputConsume(terminal->vterm, output.size);
}

// Runs on the parser's thread with the screen's lock held. Sends the screen's replies, takes its events for the UI, and
// wakes the UI if there is something new to show.
void publish(Terminal* terminal, PrProcess* process) {
    PrVterm* vterm = terminal->vterm;
    flushOutput(terminal, process);
    terminal->parsedEvents |= prVtermTakeEvents(vterm);
    bool synchronized = (prVtermModes(vterm) & PrVtermModeFlag_Synchronized) != 0;
    bool begins = synchronized && !terminal->parsedSynchronized;
    terminal->parsedSynchronized = synchronized;
    bool changed = prVtermRevision(vterm) != terminal->copiedRevision;
    bool wake = terminal->parsedEvents != 0 || (terminal->showing && changed && (!synchronized || begins));
    if (!wake || terminal->wakePending || terminal->app == nullptr)
        return;
    terminal->wakePending = true;
    prAppWake(terminal->app);
}

// Releases the screen's lock to the UI thread while it waits, and returns once the UI is done. The parser holds the
// lock.
void yieldScreen(Terminal* terminal) {
    while (terminal->uiWaiting.load() != 0) {
        terminal->parserWaiting = true;
        prConditionWait(&terminal->turn, &terminal->mutex);
    }
    terminal->parserWaiting = false;
}

// Parses the program's output one slice at a time. Runs on the process's thread.
void receiveOutput(void* PR_NULLABLE context, PrProcess* process, PrProcessStream stream, PrByteView bytes) {
    PR_ASSERT(stream == PrProcessStream_Output);
    Terminal* terminal = static_cast<Terminal*>(context);
    prMutexLock(&terminal->mutex);
    uint32_t scrollback = prVtermScrollbackCount(terminal->vterm);
    for (size_t at = 0; at < bytes.size;) {
        if (terminal->uiWaiting.load() != 0) {
            publish(terminal, process);
            yieldScreen(terminal);
        }
        size_t size = bytes.size - at < kPrTerminalParseSliceBytes ? bytes.size - at : kPrTerminalParseSliceBytes;
        prVtermWrite(terminal->vterm, PrByteView{bytes.data + at, size});
        terminal->parsedBytes.fetchAdd(size);
        at += size;
    }
    publish(terminal, process);
    bool dropped = prVtermScrollbackCount(terminal->vterm) + kTrimLines <= scrollback;
    prMutexUnlock(&terminal->mutex);
    // The trim runs outside the lock so that the UI never waits for it.
    if (dropped)
        prMemoryTrim();
}

// Returns true if `entries` sets the variable named in `variable`, which is NAME=value.
bool setsName(const PrStringView* PR_NULLABLE entries, uint32_t count, PrStringView variable) {
    size_t length = prStringFindByte(variable, '=') + 1;
    for (uint32_t i = 0; i < count; ++i) {
        if (entries[i].size >= length && prMemCompare(entries[i].data, variable.data, length) == 0)
            return true;
    }
    return false;
}

void rowsReset(Terminal* terminal) {
    for (TerminalRow& row : terminal->rows.span()) {
        prDrawListRelease(row.backgrounds);
        prDrawListRelease(row.list);
    }
    terminal->rows.clear();
}

// The extent of a cell in target pixels, at least 1 on each axis. The screen uses it to place images.
PrUVec2 cellPixels(const Terminal* terminal) {
    PrVec2 cell = terminal->drawer.metrics.cell;
    float ratio = terminal->drawer.pixelRatio;
    return PrUVec2{uint32_t(prF32Max(1, prF32Round(cell.x * ratio))), uint32_t(prF32Max(1, prF32Round(cell.y * ratio)))};
}

// Resizes the program's pseudoterminal to `extent` columns and rows, and to the target pixels they fill, if either
// changed.
void resizeProcess(Terminal* terminal, PrUVec2 extent) {
    PrUVec2 cell = cellPixels(terminal);
    PrUVec2 pixels{extent.x * cell.x, extent.y * cell.y};
    bool same = extent.x == terminal->processExtent.x && extent.y == terminal->processExtent.y &&
        pixels.x == terminal->processPixels.x && pixels.y == terminal->processPixels.y;
    if (same || terminal->process == nullptr || prProcessInfo(terminal->process).exited)
        return;
    terminal->processExtent = extent;
    terminal->processPixels = pixels;
    prProcessTerminalResize(terminal->process, extent, pixels);
}

void imagesReset(Terminal* terminal) {
    for (TerminalImage& image : terminal->images.span())
        prDrawImageRelease(image.image);
    terminal->images.clear();
}

// The line number of the first scrollback line.
uint64_t firstLine(const Terminal* terminal) {
    return prVtermScrollbackDropped(terminal->vterm);
}

// The index (prVtermLine) of the view's top line. It is the screen's first row unless the view is scrolled back. If the
// view's top line has reached the screen, the view stops being scrolled back.
uint32_t viewTopIndex(Terminal* terminal) {
    uint32_t scrollback = prVtermScrollbackCount(terminal->vterm);
    if (!terminal->scrolledBack)
        return scrollback;
    uint64_t first = firstLine(terminal);
    uint64_t top = terminal->viewTop > first ? terminal->viewTop - first : 0;
    if (top >= scrollback) {
        terminal->scrolledBack = false;
        return scrollback;
    }
    return uint32_t(top);
}

void scrollToBottom(Terminal* terminal) {
    terminal->scrolledBack = false;
    terminal->scrollRemainder = 0;
    terminal->hasJump = false;
}

// Scrolls the view back by `lines`, or forward if `lines` is negative.
void scrollBy(Terminal* terminal, int32_t lines) {
    uint32_t scrollback = prVtermScrollbackCount(terminal->vterm);
    int64_t top = int64_t(viewTopIndex(terminal)) - lines;
    if (top < 0)
        top = 0;
    if (top >= int64_t(scrollback)) {
        scrollToBottom(terminal);
        return;
    }
    terminal->scrolledBack = true;
    terminal->viewTop = firstLine(terminal) + uint64_t(top);
}

// The title that the program set as of the last frame or update, or the program's name (prTerminalTitle).
PrStringView terminalTitle(const Terminal* terminal) {
    if (terminal->title.count != 0)
        return PrStringView{terminal->title.data, terminal->title.count};
    return prRawStringView(&terminal->name);
}

// Returns true if the window is active. Without an application, the window counts as active.
bool isWindowActive(const Terminal* terminal) {
    return terminal->app == nullptr || prAppIsActive(terminal->app);
}

// Returns true if the terminal shows: a frame is declaring it, or one declared it since the last update.
bool isVisible(const Terminal* terminal) {
    return terminal->inFrame || terminal->declared;
}

// Requests the user's attention, at most once until the window is active again.
void askAttention(Terminal* terminal) {
    if (terminal->app == nullptr || terminal->attentionAsked)
        return;
    terminal->attentionAsked = true;
    prAppRequestAttention(terminal->app);
}

// Answers the bell. Flashes the screen if the terminal shows, records an alert if the user cannot see it, and requests
// attention if the window is inactive.
void ring(Terminal* terminal) {
    bool visible = isVisible(terminal);
    bool active = isWindowActive(terminal);
    terminal->bellPending = visible;
    if (!visible || !active)
        terminal->alerts |= PrTerminalAlertFlag_Bell;
    if (!active)
        askAttention(terminal);
}

// Posts each of the program's notifications as a desktop notification, unless its condition says that the user already
// sees the terminal. Records an alert if the user cannot see the terminal.
void notify(Terminal* terminal) {
    PrVtermNotification notifications[kPrVtermMaxNotifications];
    uint32_t count = prVtermTakeNotifications(terminal->vterm, notifications, kPrVtermMaxNotifications);
    bool visible = isVisible(terminal);
    bool active = isWindowActive(terminal);
    bool focused = visible && terminal->focused;
    for (uint32_t i = 0; i < count; ++i) {
        const PrVtermNotification& notification = notifications[i];
        if (!visible || !active)
            terminal->alerts |= PrTerminalAlertFlag_Notification;
        bool seen = active && focused;
        if (notification.condition == PrVtermNotificationCondition_Always)
            seen = false;
        else if (notification.condition == PrVtermNotificationCondition_Invisible)
            seen = active && visible;
        if (seen || terminal->app == nullptr)
            continue;
        PrAppNotificationDesc desc{
            .title = notification.title.size != 0 ? notification.title : terminalTitle(terminal),
            .body = notification.body,
            .tag = terminal->notificationTag,
        };
        if (prAppNotificationPost(terminal->app, &desc) != PrStatus_Ok && !active)
            askAttention(terminal);
    }
}

// Services the terminal on the UI thread. Shows the program's exit once its output ends, sends the screen's output to
// the program, and acts on the screen's events, including those that the parser took. The caller holds the screen's
// lock.
void service(Terminal* terminal) {
    PrVterm* vterm = terminal->vterm;
    terminal->wakePending = false;
    if (terminal->process != nullptr) {
        PrProcessInfo info = prProcessInfo(terminal->process);
        if (info.exited && info.outputEnded && !terminal->exitShown) {
            terminal->exitShown = true;
            PrRawString message = prRawStringNull();
            prRawStringAppend(&message, "\r\n\x1b[0;2m[");
            if (info.signaled) {
                prRawStringAppend(&message, "ended by signal ");
                prRawStringAppendU64(&message, uint64_t(info.code), 10);
            } else {
                prRawStringAppend(&message, "exited with code ");
                prRawStringAppendU64(&message, uint64_t(uint32_t(info.code)), 10);
            }
            prRawStringAppend(&message, "]\x1b[0m\r\n");
            PrStringView text = prRawStringView(&message);
            prVtermWrite(vterm, PrByteView{reinterpret_cast<const uint8_t*>(text.data), text.size});
            prRawStringReset(&message);
        }
        if (!terminal->exitShown)
            flushOutput(terminal, terminal->process);
    }
    prVtermOutputConsume(vterm, prVtermOutput(vterm).size);
    PrStringView title = prVtermTitle(vterm);
    if (title != PrStringView{terminal->title.data, terminal->title.count}) {
        terminal->title.clear();
        terminal->title.append(title.data, uint32_t(title.size));
    }
    PrVtermEventFlags events = terminal->parsedEvents | prVtermTakeEvents(vterm);
    terminal->parsedEvents = 0;
    if ((events & PrVtermEventFlag_Clipboard) != 0 && terminal->app != nullptr)
        prAppSetClipboardText(terminal->app, prVtermClipboard(vterm));
    if ((events & PrVtermEventFlag_Bell) != 0)
        ring(terminal);
    if ((events & PrVtermEventFlag_Notification) != 0)
        notify(terminal);
    if (!terminal->inFrame)
        terminal->declared = false;
}

// Creates the regular, bold, italic and bold italic fonts from the families and size of `font`.
void setFonts(Terminal* terminal, const PrTextFontDesc& font) {
    for (uint32_t i = 0; i < kTerminalFontCount; ++i) {
        PrTextFontDesc fontDesc = font;
        if ((i & 1) != 0)
            fontDesc.style.weight = kPrFontWeightBold;
        if ((i & 2) != 0)
            fontDesc.style.slope = PrFontSlope_Italic;
        PrTextFont* made = prTextFontCreate(&fontDesc);
        prTextFontRelease(terminal->drawer.fonts[i]);
        terminal->drawer.fonts[i] = made;
    }
    // A new font may have the same revision as the old one, so this forces the metrics to be recomputed.
    terminal->metricsRatio = 0;
}

// Recomputes the cell metrics if the tree's target pixel ratio or the font revision changed.
void updateMetrics(Terminal* terminal) {
    float ratio = prLayoutTreeTargetPixelRatio(terminal->kit->tree);
    if (ratio <= 0)
        ratio = 1;
    uint64_t revision = prTextFontRevision(terminal->drawer.fonts[0]);
    if (ratio == terminal->metricsRatio && revision == terminal->fontRevision)
        return;
    terminal->metricsRatio = ratio;
    terminal->fontRevision = revision;
    terminal->drawer.pixelRatio = ratio;
    terminal->drawer.metrics = prTextGridMetrics(terminal->drawer.fonts[0], ratio, 1);
    ++terminal->generation;
    rowsReset(terminal);
}

// Sets the screen's color roles and the selection color from the kit's theme if its palette changed. A color that the
// program set lasts until the palette changes.
void updateTheme(Terminal* terminal) {
    const PrKitTheme& theme = terminal->kit->theme;
    const PrKitPalette& palette = theme.palette;
    if (terminal->hasPalette && prMemCompare(&palette, &terminal->palette, sizeof(palette)) == 0)
        return;
    terminal->palette = palette;
    terminal->hasPalette = true;
    terminal->drawer.colors.selection = palette.textSelection;
    prVtermRoleSetColor(terminal->vterm, PrVtermColorRole_Foreground, color8Of(palette.text1));
    prVtermRoleSetColor(terminal->vterm, PrVtermColorRole_Background, color8Of(palette.bg1));
    prVtermRoleSetColor(terminal->vterm, PrVtermColorRole_Cursor, color8Of(palette.accent));
    rowsReset(terminal);
}

// The cell under `local`, a position relative to the screen's first cell, as a span of one cell. Clamps to the screen.
TerminalSpan cellAt(Terminal* terminal, PrVec2 local) {
    const PrVec2 cell = terminal->drawer.metrics.cell;
    PrUVec2 extent = prVtermExtent(terminal->vterm);
    float row = prF32Clamp(prF32Floor(local.y / cell.y), 0, float(extent.y - 1));
    float column = prF32Clamp(prF32Floor(local.x / cell.x), 0, float(extent.x - 1));
    uint64_t line = firstLine(terminal) + viewTopIndex(terminal) + uint32_t(row);
    return TerminalSpan{line, uint32_t(column), uint32_t(column)};
}

// Writes the index (prVtermLine) of line number `line` to `*index`. Returns false if the line is not in the scrollback
// or on the screen.
bool lineIndexOf(const Terminal* terminal, uint64_t line, uint32_t* index) {
    uint64_t first = firstLine(terminal);
    uint32_t count = prVtermScrollbackCount(terminal->vterm) + prVtermExtent(terminal->vterm).y;
    if (line < first || line - first >= count)
        return false;
    *index = uint32_t(line - first);
    return true;
}

// Returns true if the cell at `column` separates words, because it is empty or holds a space.
bool isSeparator(const PrVtermLine& line, uint32_t column) {
    const PrVtermCell& cell = line.cells[column];
    if ((cell.flags & PrVtermCellFlag_WideTail) != 0)
        return false;
    return cell.codePoint == 0 || cell.codePoint == ' ';
}

// Widens `*span` from one cell to the run of non-separator cells around it. Returns false if the cell is a separator.
bool wordAt(const Terminal* terminal, TerminalSpan* span) {
    uint32_t index = 0;
    if (!lineIndexOf(terminal, span->line, &index))
        return false;
    PrVtermLine line = prVtermLine(terminal->vterm, index);
    if (span->first >= line.cellCount || isSeparator(line, span->first))
        return false;
    while (span->first > 0 && !isSeparator(line, span->first - 1))
        --span->first;
    while (span->last + 1 < line.cellCount && !isSeparator(line, span->last + 1))
        ++span->last;
    return true;
}

// The span of the selection's unit (cell, word or line) that contains `cell`.
TerminalSpan unitAt(const Terminal* terminal, TerminalSpan cell) {
    TerminalSpan span = cell;
    if (terminal->unit == TerminalUnit_Line) {
        span.first = 0;
        span.last = prVtermExtent(terminal->vterm).x - 1;
    } else if (terminal->unit == TerminalUnit_Word && !wordAt(terminal, &span)) {
        span = cell;
    }
    return span;
}

// The program's working directory (prTerminalWorkingDirectory).
PrRawString workingDirectory(const Terminal* terminal) {
    PrRawString path = prRawStringNull();
    if (prUrlToFilePath(prVtermWorkingDirectory(terminal->vterm), &path))
        return path;
    if (terminal->process != nullptr && !prProcessInfo(terminal->process).exited) {
        path = prProcessTerminalForegroundDirectory(terminal->process);
        if (!prRawStringIsNull(&path))
            return path;
    }
    return prRawStringClone(&terminal->directory);
}

// Returns true if a regular file exists at the absolute `path`.
bool isFile(PrStringView path) {
    PrFileInfo info{};
    return prFileInfo(path, &info) && info.kind == PrFileKind_Regular;
}

// Writes the absolute path of the file that `path` names to `*out`. A relative path resolves against the program's
// working directory, and a path that begins with ~/ against the home directory. Returns false, and leaves `*out`
// unchanged, if no regular file is there.
bool resolveFile(const Terminal* terminal, PrStringView path, PrRawString* out) {
    // A null working directory means the launcher's, which is this process's. An empty base names it too.
    PrRawString base = prRawStringNull();
    if (prStringStartsWith(path, "~/")) {
        base = prFileUserDirectory(PrFileUserDirectory_Home);
        path = prStringSlice(path, 2, path.size - 2);
    } else {
        base = workingDirectory(terminal);
    }
    PrRawString absolute = prFilePathAbsolute(path, prRawStringView(&base));
    prRawStringReset(&base);
    if (prRawStringIsEmpty(&absolute) || !isFile(prRawStringView(&absolute))) {
        prRawStringReset(&absolute);
        return false;
    }
    *out = absolute;
    return true;
}

// Finds the link under `cell`. It is the program's hyperlink if the cell has one. Otherwise, it is a URL, or a
// reference to an existing file, in the cell's line. A hyperlink to an existing file opens through the terminal's
// handler, if it has one. Returns false if no link is under the cell. The caller releases the link's target with
// prRawStringReset.
bool linkAt(const Terminal* terminal, TerminalSpan cell, TerminalLink* link) {
    uint32_t index = 0;
    if (!lineIndexOf(terminal, cell.line, &index))
        return false;
    PrVtermLine line = prVtermLine(terminal->vterm, index);
    if (cell.first < line.cellCount && line.cells[cell.first].link != kPrVtermLinkNone) {
        PrVtermLinkIndex hyperlink = line.cells[cell.first].link;
        PrStringView uri = prVtermLink(terminal->vterm, hyperlink).uri;
        *link = TerminalLink{.span = cell, .hyperlink = hyperlink, .kind = TerminalLinkKind_Url};
        PrRawString path = prRawStringNull();
        if (terminal->openFile != nullptr && prUrlToFilePath(uri, &path) && isFile(prRawStringView(&path))) {
            link->kind = TerminalLinkKind_File;
            link->target = path;
        } else {
            prRawStringReset(&path);
            link->target = prRawStringCopy(uri);
        }
        return true;
    }

    TerminalLineText text{};
    terminalLineText(terminal->vterm, index, &text);
    RawArray<TerminalTextLink> found;
    terminalTextLinks(PrStringView{text.text.data, text.text.count}, &found);
    bool linked = false;
    const uint32_t* columns = text.columns.data;
    for (const TerminalTextLink& candidate : found.span()) {
        if (cell.first < columns[candidate.start] || cell.first > columns[candidate.end - 1])
            continue;
        PrStringView target{text.text.data + candidate.targetStart, candidate.targetEnd - candidate.targetStart};
        *link = TerminalLink{
            .span = TerminalSpan{cell.line, columns[candidate.start], columns[candidate.end - 1]},
            .hyperlink = kPrVtermLinkNone,
            .line = candidate.line,
            .column = candidate.column,
        };
        if (candidate.kind == TerminalTextLinkKind_Url) {
            link->kind = TerminalLinkKind_Url;
            link->target = prRawStringCopy(target);
            linked = true;
        } else {
            link->kind = TerminalLinkKind_File;
            linked = resolveFile(terminal, target, &link->target);
        }
        break;
    }
    found.reset();
    terminalLineTextReset(&text);
    return linked;
}

// Opens `link`. A file opens with the terminal's handler, or as a file URL through the application if there is no
// handler. A URL opens through the application.
void openLink(Terminal* terminal, const TerminalLink& link) {
    PrStringView target = prRawStringView(&link.target);
    if (link.kind == TerminalLinkKind_File && terminal->openFile != nullptr) {
        PrTerminalFileLink file{.path = target, .line = link.line, .column = link.column};
        terminal->openFile(terminal->openFileContext, &file);
        return;
    }
    if (terminal->app == nullptr)
        return;
    if (link.kind == TerminalLinkKind_File) {
        PrRawString url = prUrlFromFilePath(target);
        prAppOpenUrl(terminal->app, prRawStringView(&url));
        prRawStringReset(&url);
        return;
    }
    prAppOpenUrl(terminal->app, target);
}

// Writes the line numbers of the selection's first and last lines.
void selectionLines(const Terminal* terminal, uint64_t* first, uint64_t* last) {
    bool forward = terminal->anchor.line <= terminal->head.line;
    *first = forward ? terminal->anchor.line : terminal->head.line;
    *last = forward ? terminal->head.line : terminal->anchor.line;
}

// Writes the columns of line number `line` that the selection covers, from `*start` to `*end`, exclusive. Both are 0 if
// the selection does not cover the line.
void selectedColumns(const Terminal* terminal, uint64_t line, uint32_t columns, uint32_t* start, uint32_t* end) {
    *start = 0;
    *end = 0;
    if (!terminal->hasSelection)
        return;
    uint64_t first = 0;
    uint64_t last = 0;
    selectionLines(terminal, &first, &last);
    if (line < first || line > last)
        return;
    TerminalSpan anchor = terminal->anchor;
    TerminalSpan head = terminal->head;
    if (terminal->columnSelection) {
        *start = anchor.first < head.first ? anchor.first : head.first;
        *end = (anchor.last > head.last ? anchor.last : head.last) + 1;
        return;
    }
    bool forward = anchor.line < head.line || (anchor.line == head.line && anchor.first <= head.first);
    TerminalSpan from = forward ? anchor : head;
    TerminalSpan to = forward ? head : anchor;
    *start = line == from.line ? from.first : 0;
    *end = line == to.line ? to.last + 1 : columns;
}

PrVtermInputDesc inputDesc(const Terminal* terminal) {
    PrVtermInputDesc desc{.cellExtent = terminal->drawer.metrics.cell};
#if !defined(__APPLE__)
    // On a Mac, Option types the characters of the keyboard layout's third level, as in other Mac text fields.
    desc.altSendsEscape = true;
#endif
    return desc;
}

// The modifiers with which C copies and V pastes. Control alone belongs to the program, except on Windows
// (clipboardKey).
PrAppModifierFlags clipboardModifiers() {
#if defined(__APPLE__)
    return PrAppModifierFlag_Super;
#else
    return PrAppModifierFlag_Control | PrAppModifierFlag_Shift;
#endif
}

// What a key does with the clipboard.
enum {
    ClipboardKey_None,
    ClipboardKey_Copy,
    ClipboardKey_Paste,
};
typedef uint8_t ClipboardKey;

// Returns what `key` with `modifiers` does with the clipboard (pr/terminal.h).
ClipboardKey clipboardKey(const Terminal* terminal, PrAppKey key, PrAppModifierFlags modifiers) {
    if (modifiers == clipboardModifiers() && (key == 'C' || key == 'V'))
        return key == 'C' ? ClipboardKey_Copy : ClipboardKey_Paste;
#if !defined(__APPLE__)
    if (key == PrAppKey_Insert && modifiers == PrAppModifierFlag_Control)
        return ClipboardKey_Copy;
    if (key == PrAppKey_Insert && modifiers == PrAppModifierFlag_Shift)
        return ClipboardKey_Paste;
#endif
#if defined(_WIN32)
    if (key == 'V' && modifiers == PrAppModifierFlag_Control)
        return ClipboardKey_Paste;
    if (key == 'C' && modifiers == PrAppModifierFlag_Control && terminal->hasSelection)
        return ClipboardKey_Copy;
#else
    (void)terminal;
#endif
    return ClipboardKey_None;
}

// The modifiers of the arrow keys that scroll to the previous or the next prompt. On a Mac it is Command, as in other
// Mac terminals. Elsewhere it is Control with Shift, because shells' line editors use Control with an arrow to move by
// a word.
PrAppModifierFlags promptModifiers() {
#if defined(__APPLE__)
    return PrAppModifierFlag_Super;
#else
    return PrAppModifierFlag_Control | PrAppModifierFlag_Shift;
#endif
}

// The modifier for a click that opens a link: Command on a Mac, and Control elsewhere. A plain click on a link only
// selects, so that a stray click cannot open something that the program printed.
PrAppModifierFlags linkModifier() {
#if defined(__APPLE__)
    return PrAppModifierFlag_Super;
#else
    return PrAppModifierFlag_Control;
#endif
}

// Finds the link under the pointer while the link modifier is held, so that the view can underline it.
void updateLink(Terminal* terminal) {
    prRawStringReset(&terminal->link.target);
    terminal->link = TerminalLink{};
    bool held = (terminal->modifiers & linkModifier()) != 0;
    terminal->hasLink = terminal->hasPointer && !terminal->selecting && held &&
        linkAt(terminal, cellAt(terminal, terminal->pointer), &terminal->link);
}

// Appends the text of line `index`, from column `from` to `to`, exclusive, to `text`. If `joinWrapped` is true and the
// line wrapped, the text runs straight into the next line. Otherwise, it loses its trailing spaces and ends with a line
// feed, unless `last` is true.
void lineAppendText(const Terminal* terminal, uint32_t index, uint32_t from, uint32_t to, bool joinWrapped, bool last,
        PrRawString* text) {
    PrVtermLine line = prVtermLine(terminal->vterm, index);
    size_t before = prRawStringView(text).size;
    prVtermLineAppendText(terminal->vterm, index, from, to, text);
    if (joinWrapped && (line.flags & PrVtermLineFlag_Wrapped) != 0 && to == prVtermExtent(terminal->vterm).x)
        return;
    PrStringView all = prRawStringView(text);
    size_t size = all.size;
    while (size > before && all.data[size - 1] == ' ')
        --size;
    prRawStringTruncate(text, size);
    if (!last)
        prRawStringAppend(text, "\n");
}

// The selected text (prTerminalSelection).
PrRawString selectionText(const Terminal* terminal) {
    PrRawString text = prRawStringEmpty();
    if (!terminal->hasSelection)
        return text;
    uint64_t first = 0;
    uint64_t last = 0;
    selectionLines(terminal, &first, &last);
    for (uint64_t number = first; number <= last; ++number) {
        uint32_t index = 0;
        if (!lineIndexOf(terminal, number, &index))
            continue;
        uint32_t from = 0;
        uint32_t to = 0;
        selectedColumns(terminal, number, prVtermExtent(terminal->vterm).x, &from, &to);
        lineAppendText(terminal, index, from, to, !terminal->columnSelection, number == last, &text);
    }
    return text;
}

// Copies the selected text to the clipboard.
void terminalCopy(Terminal* terminal) {
    if (terminal->app == nullptr || !terminal->hasSelection)
        return;
    PrRawString text = selectionText(terminal);
    prAppSetClipboardText(terminal->app, prRawStringView(&text));
    prRawStringReset(&text);
}

// Requests the clipboard's text. It arrives later as a Clipboard event, which the view pastes.
void terminalPaste(Terminal* terminal) {
    if (terminal->app != nullptr)
        prAppRequestClipboardText(terminal->app);
}

// Returns true if a paste of `text` must ask the user first. That is the case if the text holds a line break, which
// would run the text before it as a command, and the program has not enabled bracketed paste, which would let it tell a
// paste from typing.
bool asksBeforePaste(const Terminal* terminal, PrStringView text) {
    if ((prVtermModes(terminal->vterm) & PrVtermModeFlag_BracketedPaste) != 0)
        return false;
    return prStringFindByte(text, '\n') < text.size || prStringFindByte(text, '\r') < text.size;
}

// Drops the paste that waits for confirmation, and gives focus back to the screen. Called when the confirmation closes.
void dropPaste(Terminal* terminal) {
    prRawStringReset(&terminal->paste);
    terminal->focusAsked = true;
    prUiRequestFrame(terminal->ui);
}

// Holds a paste of `text` until the user confirms it, and opens the confirmation. It replaces any paste that waits.
void askBeforePaste(Terminal* terminal, PrStringView text) {
    prRawStringReset(&terminal->paste);
    terminal->paste = prRawStringCopy(text);
    prKitSetOpen(terminal->kit, terminal->node, true);
}

// Appends the first kPreviewLines lines of `text` to `preview`, each cut at a character boundary after at most
// kPreviewLineBytes bytes. Returns the number of lines in `text`.
uint32_t appendPastePreview(PrRawString* preview, PrStringView text) {
    uint32_t lines = 0;
    size_t start = 0;
    while (start < text.size) {
        size_t end = start;
        while (end < text.size && text.data[end] != '\n' && text.data[end] != '\r')
            ++end;
        if (lines < kPreviewLines) {
            size_t cut = end - start > kPreviewLineBytes ? start + kPreviewLineBytes : end;
            while (cut > start && cut < end && (uint8_t(text.data[cut]) & 0xc0) == 0x80)
                --cut;
            if (lines != 0)
                prRawStringAppend(preview, "\n");
            prRawStringAppend(preview, PrStringView{text.data + start, cut - start});
            if (cut < end)
                prRawStringAppend(preview, "\xe2\x80\xa6");
        }
        ++lines;
        // A carriage return followed by a line feed is one line break.
        if (end + 1 < text.size && text.data[end] == '\r' && text.data[end + 1] == '\n')
            ++end;
        start = end + 1;
    }
    return lines;
}

// Declares the confirmation dialog for the paste that waits, if there is one. The dialog shows the first lines of the
// paste and its line count. The Paste button has focus, so Enter presses it, and it sends the paste to the program.
// Cancel, or dismissing the dialog with Escape or otherwise, drops the paste.
void buildPasteConfirmation(Terminal* terminal) {
    PrStringView text = prRawStringView(&terminal->paste);
    if (text.data == nullptr)
        return;
    PrKit* kit = terminal->kit;
    PrRawString preview = prRawStringNull();
    uint32_t lines = appendPastePreview(&preview, text);
    PrRawString title = prRawStringNull();
    prRawStringAppend(&title, "Paste ");
    prRawStringAppendU64(&title, lines, 10);
    prRawStringAppend(&title, lines == 1 ? " line?" : " lines?");
    PrLayoutNode body = prKitDialog(kit, terminal->node, {.title = prRawStringView(&title)}, {});
    prRawStringReset(&title);
    if (body == kPrLayoutNodeNone) {
        prRawStringReset(&preview);
        dropPaste(terminal);
        return;
    }
    PrKitTextStyle note{.role = PrKitTextRole_Body, .tone = PrKitTone_Secondary, .wrap = PrKitWrap_Wrap};
    prKitLabel(kit, body, {.text = "Each line runs as a command as it arrives."}, note);
    PrKitTextStyle mono{.role = PrKitTextRole_Mono, .wrap = PrKitWrap_Clip, .preserve = true};
    prKitLabel(kit, body, {.text = prRawStringView(&preview)}, mono);
    prRawStringReset(&preview);
    if (lines > kPreviewLines) {
        PrRawString more = prRawStringNull();
        prRawStringAppend(&more, "and ");
        prRawStringAppendU64(&more, lines - kPreviewLines, 10);
        prRawStringAppend(&more, lines - kPreviewLines == 1 ? " more line" : " more lines");
        PrKitTextStyle small{.role = PrKitTextRole_BodySmall, .tone = PrKitTone_Muted};
        prKitLabel(kit, body, {.text = prRawStringView(&more)}, small);
        prRawStringReset(&more);
    }
    PrLayoutNode footer = prKitDialogFooter(kit, body);
    PrLayoutNode cancel = prKitButton(kit, footer, {.text = "Cancel"}, {});
    PrLayoutNode paste = prKitButton(kit, footer, {.text = "Paste"}, {.variant = PrKitButtonVariant_Primary});
    prKitModalAutofocus(kit, paste);
    bool pasted = prKitActivated(kit, paste);
    if (!pasted && !prKitActivated(kit, cancel))
        return;
    if (pasted) {
        prVtermInputPaste(terminal->vterm, text);
        scrollToBottom(terminal);
        terminal->hasSelection = false;
    }
    prKitSetOpen(kit, terminal->node, false);
    dropPaste(terminal);
}

// Commands, found from the shell integration's marks on lines. A command spans from the line of its prompt to the line
// of the next prompt. Its output starts on the line marked as the start of output.

// The number of lines in the scrollback and on the screen (prVtermLine).
uint32_t lineCount(const Terminal* terminal) {
    return prVtermScrollbackCount(terminal->vterm) + prVtermExtent(terminal->vterm).y;
}

bool isPrompt(const Terminal* terminal, uint32_t index) {
    return (prVtermLine(terminal->vterm, index).marks & PrVtermLineMarkFlag_Prompt) != 0;
}

// Returns true if line `index` holds only empty cells and spaces.
bool isBlank(const Terminal* terminal, uint32_t index) {
    PrVtermLine line = prVtermLine(terminal->vterm, index);
    for (uint32_t i = 0; i < line.cellCount; ++i) {
        if (line.cells[i].codePoint != 0 && line.cells[i].codePoint != ' ')
            return false;
    }
    return true;
}

// Writes the index of the nearest prompt line before `index` to `*prompt`. If `inclusive` is true, `index` itself
// counts. Returns false if there is none.
bool promptBefore(const Terminal* terminal, uint32_t index, bool inclusive, uint32_t* prompt) {
    uint32_t at = inclusive ? index + 1 : index;
    while (at > 0) {
        --at;
        if (isPrompt(terminal, at)) {
            *prompt = at;
            return true;
        }
    }
    return false;
}

// Writes the index of the nearest prompt line after `index` to `*prompt`. Returns false if there is none.
bool promptAfter(const Terminal* terminal, uint32_t index, uint32_t* prompt) {
    uint32_t count = lineCount(terminal);
    for (uint32_t at = index + 1; at < count; ++at) {
        if (isPrompt(terminal, at)) {
            *prompt = at;
            return true;
        }
    }
    return false;
}

// Writes the output lines of the command whose prompt is on line `prompt` to `*first` and `*end`, exclusive. Trailing
// blank lines are left out. Returns false if the command has no output.
bool outputLines(const Terminal* terminal, uint32_t prompt, uint32_t* first, uint32_t* end) {
    uint32_t count = lineCount(terminal);
    uint32_t start = prompt;
    while (start < count) {
        PrVtermLine line = prVtermLine(terminal->vterm, start);
        if (start > prompt && (line.marks & PrVtermLineMarkFlag_Prompt) != 0)
            return false;
        if ((line.marks & PrVtermLineMarkFlag_Output) != 0)
            break;
        ++start;
    }
    if (start == count)
        return false;
    uint32_t stop = count;
    promptAfter(terminal, start, &stop);
    while (stop > start && isBlank(terminal, stop - 1))
        --stop;
    *first = start;
    *end = stop;
    return stop > start;
}

// Writes the index of the prompt line of the last finished command to `*prompt`. Returns false if no finished command's
// prompt is on the screen or in the scrollback.
bool lastFinished(const Terminal* terminal, uint32_t* prompt) {
    uint32_t at = lineCount(terminal);
    while (promptBefore(terminal, at, false, &at)) {
        switch (prVtermLine(terminal->vterm, at).commandStatus) {
            case PrVtermCommandStatus_Succeeded:
            case PrVtermCommandStatus_Failed:
            case PrVtermCommandStatus_Finished:
                *prompt = at;
                return true;
            default:
                break;
        }
    }
    return false;
}

// Selects the whole lines from `first` to `end`, exclusive.
void selectLines(Terminal* terminal, uint32_t first, uint32_t end) {
    uint64_t base = firstLine(terminal);
    uint32_t last = prVtermExtent(terminal->vterm).x - 1;
    terminal->selecting = false;
    terminal->hasSelection = true;
    terminal->columnSelection = false;
    terminal->unit = TerminalUnit_Line;
    terminal->anchor = TerminalSpan{base + first, 0, last};
    terminal->head = TerminalSpan{base + end - 1, 0, last};
}

// Selects the output of the command that contains line `index`, whose prompt is on that line or the nearest before it.
// Returns false if there is no such command, or if it has no output.
bool selectOutputAt(Terminal* terminal, uint32_t index) {
    uint32_t prompt = 0;
    uint32_t first = 0;
    uint32_t end = 0;
    if (!promptBefore(terminal, index, true, &prompt) || !outputLines(terminal, prompt, &first, &end))
        return false;
    selectLines(terminal, first, end);
    return true;
}

// The index of the line that a jump to a prompt starts from. It is the prompt that the view last jumped to, if that
// line is still kept. Otherwise, it is the view's top line if the view is scrolled back, or the nearest prompt at or
// before the cursor's line.
uint32_t jumpStart(Terminal* terminal) {
    uint32_t index = 0;
    if (terminal->hasJump && lineIndexOf(terminal, terminal->jumpLine, &index))
        return index;
    if (terminal->scrolledBack)
        return viewTopIndex(terminal);
    uint32_t cursor = prVtermScrollbackCount(terminal->vterm) + prVtermCursor(terminal->vterm).position.y;
    uint32_t prompt = cursor;
    promptBefore(terminal, cursor, true, &prompt);
    return prompt;
}

// Scrolls the view to the prompt before or after the line that a jump starts from (prTerminalScrollToPrompt).
bool scrollToPrompt(Terminal* terminal, PrTerminalDirection direction) {
    if ((prVtermModes(terminal->vterm) & PrVtermModeFlag_AlternateScreen) != 0)
        return false;
    uint32_t start = jumpStart(terminal);
    uint32_t target = 0;
    bool found = false;
    if (direction == PrTerminalDirection_Previous)
        found = promptBefore(terminal, start, false, &target);
    else
        found = promptAfter(terminal, start, &target);
    if (!found) {
        if (direction == PrTerminalDirection_Previous || (!terminal->scrolledBack && !terminal->hasJump))
            return false;
        scrollToBottom(terminal);
        return true;
    }
    if (target < prVtermScrollbackCount(terminal->vterm)) {
        terminal->scrolledBack = true;
        terminal->viewTop = firstLine(terminal) + target;
        terminal->scrollRemainder = 0;
    } else {
        scrollToBottom(terminal);
    }
    terminal->hasJump = true;
    terminal->jumpLine = firstLine(terminal) + target;
    return true;
}

// The output text of the last finished command (prTerminalCommandOutput).
PrRawString commandOutputText(const Terminal* terminal) {
    PrRawString text = prRawStringEmpty();
    uint32_t prompt = 0;
    uint32_t first = 0;
    uint32_t end = 0;
    if (!lastFinished(terminal, &prompt) || !outputLines(terminal, prompt, &first, &end))
        return text;
    uint32_t columns = prVtermExtent(terminal->vterm).x;
    for (uint32_t index = first; index < end; ++index)
        lineAppendText(terminal, index, 0, columns, true, index + 1 == end, &text);
    return text;
}

// Translates a platform text-editing key into the key that shells' line editors expect. Returns false if the key needs
// no translation.
//
// On a Mac, Option with Left or Right moves by a word, so it becomes Alt with B or F. Command with Left or Right moves
// to the start or end of the line, and Command with Backspace erases to the start, so they become Control with A, E and
// U. Elsewhere, the text-editing keys are Control with an arrow, Backspace or Delete, and Home and End. Those need no
// translation, since the terminal sends them in xterm's encoding, which the line editors read. On Windows, the
// pseudoconsole turns these sequences back into the keys that console programs read.
bool translateEditingKey(PrAppEvent* event, PrAppModifierFlags modifiers) {
#if defined(__APPLE__)
    struct Translation {
        PrAppModifierFlags modifiers;
        PrAppKey key;
        PrAppModifierFlags sentModifiers;
        PrAppKey sentKey;
    };
    static const Translation kTranslations[] = {
        {PrAppModifierFlag_Alt, PrAppKey_Left, PrAppModifierFlag_Alt, 'B'},
        {PrAppModifierFlag_Alt, PrAppKey_Right, PrAppModifierFlag_Alt, 'F'},
        {PrAppModifierFlag_Super, PrAppKey_Left, PrAppModifierFlag_Control, 'A'},
        {PrAppModifierFlag_Super, PrAppKey_Right, PrAppModifierFlag_Control, 'E'},
        {PrAppModifierFlag_Super, PrAppKey_Backspace, PrAppModifierFlag_Control, 'U'},
    };
    for (const Translation& translation : kTranslations) {
        if (translation.modifiers == modifiers && translation.key == event->key) {
            event->key = translation.sentKey;
            event->modifiers = translation.sentModifiers;
            event->text = PrStringView{};
            return true;
        }
    }
#else
    (void)event;
    (void)modifiers;
#endif
    return false;
}

// Claims the chords that the program reads, so that the view receives them ahead of the key bindings around it. These
// are every key with any combination of Shift, Control and Alt, including the keys that would otherwise move focus or
// activate. On a Mac, Command chords stay with the application, except copy, paste, the text-editing chords, and the
// chords that scroll to a prompt. A binding that captures its chord still takes precedence.
void claimKeys(Terminal* terminal) {
    PrUi* ui = terminal->ui;
    PrLayoutNode node = terminal->node;
    static const PrAppModifierFlags kModifiers[] = {
        0,
        PrAppModifierFlag_Shift,
        PrAppModifierFlag_Control,
        PrAppModifierFlag_Control | PrAppModifierFlag_Shift,
        PrAppModifierFlag_Alt,
        PrAppModifierFlag_Alt | PrAppModifierFlag_Shift,
        PrAppModifierFlag_Control | PrAppModifierFlag_Alt,
    };
    for (PrAppModifierFlags modifiers : kModifiers) {
        for (uint32_t key = ' '; key < 0x7f; ++key) {
            // A letter key's code is its uppercase letter.
            if (key >= 'a' && key <= 'z')
                continue;
            prUiKeyPressed(ui, node, PrAppKey(key), modifiers);
        }
        for (uint32_t key = PrAppKey_Left; key <= PrAppKey_Insert; ++key)
            prUiKeyPressed(ui, node, PrAppKey(key), modifiers);
        for (uint32_t key = PrAppKey_F1; key <= PrAppKey_F12; ++key)
            prUiKeyPressed(ui, node, PrAppKey(key), modifiers);
    }
    PrAppModifierFlags clipboard = clipboardModifiers();
    prUiKeyPressed(ui, node, 'C', clipboard);
    prUiKeyPressed(ui, node, 'V', clipboard);
#if defined(__APPLE__)
    // The Command chords of text editing (translateEditingKey).
    prUiKeyPressed(ui, node, PrAppKey_Left, PrAppModifierFlag_Super);
    prUiKeyPressed(ui, node, PrAppKey_Right, PrAppModifierFlag_Super);
    prUiKeyPressed(ui, node, PrAppKey_Backspace, PrAppModifierFlag_Super);
    // The Command chords that scroll to a prompt (promptKey).
    prUiKeyPressed(ui, node, PrAppKey_Up, PrAppModifierFlag_Super);
    prUiKeyPressed(ui, node, PrAppKey_Down, PrAppModifierFlag_Super);
#endif
}

// Scrolls the view for Shift with Page Up, Page Down, Home or End: by a page, or to the top or the bottom. Returns
// false for any other key, or while the alternate screen shows, since it has no scrollback.
bool scrollbackKey(Terminal* terminal, PrAppKey key) {
    if ((prVtermModes(terminal->vterm) & PrVtermModeFlag_AlternateScreen) != 0)
        return false;
    int32_t page = int32_t(prVtermExtent(terminal->vterm).y);
    page = page > 1 ? page - 1 : 1;
    switch (key) {
        case PrAppKey_PageUp:
            scrollBy(terminal, page);
            return true;
        case PrAppKey_PageDown:
            scrollBy(terminal, -page);
            return true;
        case PrAppKey_Home:
            scrollBy(terminal, int32_t(prVtermScrollbackCount(terminal->vterm)));
            return true;
        case PrAppKey_End:
            scrollToBottom(terminal);
            return true;
        default:
            return false;
    }
}

// Records whether the press of the key with `code` went to the program.
void setProgramKey(Terminal* terminal, PrAppKeyCode code, bool program) {
    uint32_t bit = 1u << (code % 32);
    uint32_t& word = terminal->programKeys[code / 32];
    word = program ? word | bit : word & ~bit;
}

// Returns true if the release of the key with `code` goes to the program, and clears the record of its press.
bool takeProgramKey(Terminal* terminal, PrAppKeyCode code) {
    if (code == PrAppKeyCode_Unidentified)
        return true;
    bool program = (terminal->programKeys[code / 32] & (1u << (code % 32))) != 0;
    setProgramKey(terminal, code, false);
    return program;
}

// Scrolls the view to the previous or the next prompt for Up or Down with the prompt modifiers. Returns false for any
// other key, or if the view did not move. The program then receives the key, which matters if no prompts are marked, as
// on the alternate screen.
bool promptKey(Terminal* terminal, PrAppKey key) {
    if (key == PrAppKey_Up)
        return scrollToPrompt(terminal, PrTerminalDirection_Previous);
    if (key == PrAppKey_Down)
        return scrollToPrompt(terminal, PrTerminalDirection_Next);
    return false;
}

// Handles the events that the view received in this round. The caller holds the screen's lock.
void handleEvents(Terminal* terminal) {
    PrUi* ui = terminal->ui;
    PrLayoutNode node = terminal->node;
    PrVterm* vterm = terminal->vterm;
    PrVtermInputDesc desc = inputDesc(terminal);
    PrUiEvents events = prUiEvents(ui, node);
    for (uint32_t i = 0; i < events.count; ++i) {
        PrAppEvent event = events.events[i];
        PrAppModifierFlags modifiers = event.modifiers & PrAppModifierFlags(~PrAppModifierFlag_CapsLock);
        bool pointer = event.kind == PrAppEventKind_PointerDown || event.kind == PrAppEventKind_PointerUp ||
            event.kind == PrAppEventKind_PointerMove || event.kind == PrAppEventKind_Scroll;
        // Pointer positions are relative to the screen's first cell. Over the gutter, x is negative.
        if (pointer) {
            PrVec2 local{};
            if (!prUiLocalPoint(ui, node, event.position, &local))
                continue;
            event.position = PrVec2{local.x - kGutterWidth, local.y};
        }
        terminal->modifiers = modifiers;
        uint8_t dropTextSize = terminal->dropTextSize;
        if (event.kind != PrAppEventKind_KeyUp)
            terminal->dropTextSize = 0;
        switch (event.kind) {
            case PrAppEventKind_KeyDown:
                setProgramKey(terminal, event.code, false);
                if (ClipboardKey action = clipboardKey(terminal, event.key, modifiers); action != ClipboardKey_None) {
                    if (action == ClipboardKey_Paste) {
                        terminalPaste(terminal);
                    } else {
                        terminalCopy(terminal);
                        // The next Control with C goes to the program.
                        if (modifiers == PrAppModifierFlag_Control && event.key == 'C')
                            terminal->hasSelection = false;
                    }
                    if (event.text.size <= sizeof(terminal->dropText)) {
                        prMemCopy(terminal->dropText, event.text.data, event.text.size);
                        terminal->dropTextSize = uint8_t(event.text.size);
                    }
                    continue;
                }
                if (modifiers == PrAppModifierFlag_Shift && scrollbackKey(terminal, event.key))
                    continue;
                if (modifiers == promptModifiers() && promptKey(terminal, event.key))
                    continue;
                setProgramKey(terminal, event.code, true);
                if (translateEditingKey(&event, modifiers)) {
                    // Alt in a translated key is the meta key, whatever the user's setting for Option.
                    PrVtermInputDesc editing = desc;
                    editing.altSendsEscape = true;
                    prVtermInputEvent(vterm, &event, &editing);
                    scrollToBottom(terminal);
                    terminal->hasSelection = false;
                    continue;
                }
                if (prVtermInputEvent(vterm, &event, &desc)) {
                    scrollToBottom(terminal);
                    terminal->hasSelection = false;
                    terminal->blinkStart = prUiCurrentRound(ui)->time;
                }
                continue;
            case PrAppEventKind_KeyUp: {
                // A release goes to the program only if the press did, and is translated as the press was. The program
                // reports releases only if it asked for event types.
                if (!takeProgramKey(terminal, event.code))
                    continue;
                PrVtermInputDesc editing = desc;
                if (translateEditingKey(&event, modifiers))
                    editing.altSendsEscape = true;
                prVtermInputEvent(vterm, &event, &editing);
                continue;
            }
            case PrAppEventKind_Text:
                if (dropTextSize != 0 && PrStringView{terminal->dropText, dropTextSize} == event.text)
                    continue;
                prVtermInputEvent(vterm, &event, &desc);
                if (event.text.size != 0) {
                    scrollToBottom(terminal);
                    terminal->hasSelection = false;
                    terminal->blinkStart = prUiCurrentRound(ui)->time;
                }
                continue;
            case PrAppEventKind_Clipboard:
                if (asksBeforePaste(terminal, event.text)) {
                    askBeforePaste(terminal, event.text);
                    continue;
                }
                prVtermInputEvent(vterm, &event, &desc);
                scrollToBottom(terminal);
                continue;
            case PrAppEventKind_PointerDown:
                prUiSetFocus(ui, node);
                // The view handles clicks in the gutter. A click there selects the output of the command on that line.
                if (event.position.x < 0) {
                    uint64_t line = cellAt(terminal, event.position).line;
                    if (event.button == 0 && !selectOutputAt(terminal, uint32_t(line - firstLine(terminal))))
                        terminal->hasSelection = false;
                    continue;
                }
                // Shift selects even while the program reads the pointer, as in other terminals.
                if ((modifiers & PrAppModifierFlag_Shift) == 0 && prVtermInputEvent(vterm, &event, &desc))
                    continue;
                if (event.button == 0) {
                    terminal->unit = TerminalUnit_Cell;
                    if (event.clicks >= 3)
                        terminal->unit = TerminalUnit_Line;
                    else if (event.clicks == 2)
                        terminal->unit = TerminalUnit_Word;
                    terminal->columnSelection = terminal->unit == TerminalUnit_Cell &&
                        (modifiers & PrAppModifierFlag_Alt) != 0;
                    terminal->selecting = true;
                    terminal->moved = false;
                    terminal->press = cellAt(terminal, event.position);
                    terminal->pressModifiers = modifiers;
                    terminal->anchor = unitAt(terminal, terminal->press);
                    terminal->head = terminal->anchor;
                    terminal->hasSelection = terminal->unit != TerminalUnit_Cell;
                }
                continue;
            case PrAppEventKind_PointerMove: {
                TerminalSpan cell = cellAt(terminal, event.position);
                terminal->pointer = event.position;
                if (terminal->selecting) {
                    terminal->moved = terminal->moved || !isSameSpan(cell, terminal->press);
                    terminal->head = terminal->columnSelection ? cell : unitAt(terminal, cell);
                    terminal->hasSelection = terminal->moved || terminal->unit != TerminalUnit_Cell;
                    continue;
                }
                terminal->hasPointer = !prVtermInputEvent(vterm, &event, &desc);
                continue;
            }
            case PrAppEventKind_PointerUp:
                if (terminal->selecting && event.button == 0) {
                    terminal->selecting = false;
                    // A click that did not move and did not select a word or line selects nothing. With the link
                    // modifier, it opens the link under it.
                    TerminalLink link{};
                    if (!terminal->moved && terminal->unit == TerminalUnit_Cell) {
                        terminal->hasSelection = false;
                        if ((terminal->pressModifiers & linkModifier()) != 0 && linkAt(terminal, terminal->press, &link)) {
                            prRawStringReset(&terminal->opened.target);
                            terminal->opened = link;
                            terminal->hasOpened = true;
                            continue;
                        }
                    }
                    prRawStringReset(&link.target);
                    continue;
                }
                if (event.position.x < 0)
                    continue;
                prVtermInputEvent(vterm, &event, &desc);
                continue;
            case PrAppEventKind_Scroll: {
                if (prVtermInputEvent(vterm, &event, &desc))
                    continue;
                float lines = event.delta.y;
                if (event.scrollUnit == PrAppScrollUnit_Pixel)
                    lines /= terminal->drawer.metrics.cell.y;
                terminal->scrollRemainder += lines;
                int32_t whole = int32_t(terminal->scrollRemainder);
                terminal->scrollRemainder -= float(whole);
                // Scrolling down moves the view toward the newest lines.
                if (whole != 0)
                    scrollBy(terminal, -whole);
                continue;
            }
            default:
                continue;
        }
    }
    // The link can change with the pointer, the modifiers, or the lines that scroll under the pointer.
    updateLink(terminal);
}

// Records an underline in the accent color for the link under the pointer. It underlines every cell of the view that
// holds the link's hyperlink, or else the link's span.
void drawLink(const Terminal* terminal, PrDrawList* list) {
    const TerminalLink& link = terminal->link;
    const TerminalSnapshot& snapshot = terminal->snapshot;
    const PrTextGridMetrics& m = terminal->drawer.metrics;
    PrDrawPaint accent{};
    accent.color = prColor16FromColor(terminal->kit->theme.palette.accent);
    uint64_t top = snapshot.firstLine + snapshot.topIndex;
    for (uint32_t row = 0; row < snapshot.lines.count; ++row) {
        const PrVtermLine& line = snapshot.lines.data[row].line;
        float y = float(row) * m.cell.y + m.underlineTop;
        for (uint32_t column = 0; column < line.cellCount;) {
            auto linked = [&](uint32_t at) {
                if (link.hyperlink != kPrVtermLinkNone)
                    return line.cells[at].link == link.hyperlink;
                return top + row == link.span.line && at >= link.span.first && at <= link.span.last;
            };
            if (!linked(column)) {
                ++column;
                continue;
            }
            uint32_t end = column + 1;
            while (end < line.cellCount && linked(end))
                ++end;
            PrRect rect{float(column) * m.cell.x, y, float(end) * m.cell.x, y + m.underlineThickness};
            prDrawListRect(list, rect, accent);
            column = end;
        }
    }
}

// Records the cursor over the screen. It blinks off if `on` is false, and is a hollow block without focus.
void drawCursor(Terminal* terminal, PrDrawList* list, bool on) {
    const TerminalSnapshot& snapshot = terminal->snapshot;
    PrVtermCursor cursor = snapshot.cursor;
    if (!cursor.visible || terminal->scrolledBack || cursor.position.y >= snapshot.lines.count)
        return;
    const PrTextGridMetrics& m = terminal->drawer.metrics;
    const PrVtermLine& line = snapshot.lines.data[cursor.position.y].line;
    uint32_t column = cursor.position.x;
    uint32_t columns = 1;
    if (column < line.cellCount && (line.cells[column].flags & PrVtermCellFlag_Wide) != 0)
        columns = 2;
    float left = float(column) * m.cell.x;
    float top = float(cursor.position.y) * m.cell.y;
    PrRect rect{left, top, left + float(columns) * m.cell.x, top + m.cell.y};
    PrColor color = prColorFromColor8(snapshot.colors.roles[PrVtermColorRole_Cursor]);
    float thickness = prF32Round(m.underlineThickness * terminal->drawer.pixelRatio);
    thickness = (thickness < 1 ? 1 : thickness) / terminal->drawer.pixelRatio;
    PrDrawPaint paint{};
    paint.color = prColor16FromColor(color);
    if (!terminal->focused) {
        // A hollow block shows where the cursor is without suggesting that the view takes input.
        prDrawListRect(list, PrRect{rect.left, rect.top, rect.right, rect.top + thickness}, paint);
        prDrawListRect(list, PrRect{rect.left, rect.bottom - thickness, rect.right, rect.bottom}, paint);
        prDrawListRect(list, PrRect{rect.left, rect.top, rect.left + thickness, rect.bottom}, paint);
        prDrawListRect(list, PrRect{rect.right - thickness, rect.top, rect.right, rect.bottom}, paint);
        return;
    }
    if (!on)
        return;
    switch (cursor.shape) {
        case PrVtermCursorShape_Underline:
            prDrawListRect(list, PrRect{rect.left, rect.bottom - 2 * thickness, rect.right, rect.bottom}, paint);
            return;
        case PrVtermCursorShape_Bar:
            prDrawListRect(list, PrRect{rect.left, rect.top, rect.left + 2 * thickness, rect.bottom}, paint);
            return;
        default: {
            prDrawListRect(list, rect, paint);
            TerminalCellColors colors{};
            colors.foreground = terminalBackground(snapshot.colors);
            terminalDrawCell(&terminal->drawer, list, line, column, PrVec2{rect.left, rect.top}, colors);
            return;
        }
    }
}

// Records the gutter's marks beside the rows of the view. Each finished command's prompt gets a bar in the theme's
// success or danger color, or a muted color if the shell reported no exit code. The prompt that the view last scrolled
// to gets a full-width bar in the accent color.
void drawGutter(const Terminal* terminal, PrDrawList* list) {
    const PrKitPalette& palette = terminal->kit->theme.palette;
    const TerminalSnapshot& snapshot = terminal->snapshot;
    const PrTextGridMetrics& m = terminal->drawer.metrics;
    float ratio = terminal->drawer.pixelRatio;
    auto snap = [ratio](float px) { return prF32Round(px * ratio) / ratio; };
    for (uint32_t row = 0; row < snapshot.lines.count; ++row) {
        uint32_t index = snapshot.topIndex + row;
        const PrVtermLine& line = snapshot.lines.data[row].line;
        float top = float(row) * m.cell.y;
        PrRect rect{
            snap(kGutterMarkInset), snap(top + m.cell.y * 0.15f), snap(kGutterMarkInset + kGutterMarkWidth),
            snap(top + m.cell.y * 0.85f),
        };
        PrColor color{};
        bool jumped = terminal->hasJump && terminal->jumpLine == snapshot.firstLine + index;
        if (jumped) {
            color = palette.accent;
            rect = PrRect{0, top, kGutterWidth, top + m.cell.y};
        } else if ((line.marks & PrVtermLineMarkFlag_Prompt) == 0) {
            continue;
        } else if (line.commandStatus == PrVtermCommandStatus_Succeeded) {
            color = palette.success;
        } else if (line.commandStatus == PrVtermCommandStatus_Failed) {
            color = palette.danger;
        } else if (line.commandStatus == PrVtermCommandStatus_Finished) {
            color = palette.text3;
        } else {
            continue;
        }
        PrDrawPaint paint{};
        paint.color = prColor16FromColor(color);
        prDrawListRect(list, rect, paint);
    }
}

// The drawing image for the screen image `key`, which the snapshot made, or null if there is none. Marks it as shown by
// the current drawing, so that imagesTrim keeps it.
const PrDrawImage* PR_NULLABLE drawImageOf(Terminal* terminal, uint64_t key) {
    for (TerminalImage& image : terminal->images.span()) {
        if (image.key == key) {
            image.drawing = terminal->drawings;
            return image.image;
        }
    }
    return nullptr;
}

// Makes the drawing image for the screen image `key`, if it does not exist yet. It lasts until a drawing does not show
// it. The caller holds the screen's lock.
void imageMake(Terminal* terminal, uint64_t key) {
    for (const TerminalImage& image : terminal->images.span()) {
        if (image.key == key)
            return;
    }
    PrVtermImage pixels{};
    if (!prVtermImage(terminal->vterm, key, &pixels))
        return;
    PrDrawImageDesc desc{
        .pixels = PrPixelView{pixels.pixels, size_t(pixels.extent.x) * 4, pixels.extent},
        .format = PrPixelFormat_Rgba8,
        .alpha = PrDrawAlpha_Straight,
        .space = PrColorSpace_Srgb,
    };
    const PrDrawImage* image = nullptr;
    if (prDrawImageCreate(&desc, &image) == PrStatus_Ok)
        terminal->images.push(TerminalImage{.key = key, .image = image, .drawing = terminal->drawings});
}

// Frees the drawing images that the drawing just recorded did not show.
void imagesTrim(Terminal* terminal) {
    for (uint32_t i = terminal->images.count; i-- > 0;) {
        const TerminalImage& image = terminal->images.data[i];
        if (image.drawing != terminal->drawings) {
            prDrawImageRelease(image.image);
            terminal->images.erase(i);
        }
    }
}

// Records the image placements in the view whose z-index is from `low` to `high`, exclusive. Each is clipped to its
// cells and to the screen.
void drawImages(Terminal* terminal, PrDrawList* list, int64_t low, int64_t high) {
    const TerminalSnapshot& snapshot = terminal->snapshot;
    const PrVec2 cell = terminal->drawer.metrics.cell;
    uint32_t topIndex = snapshot.topIndex;
    PrRect screen{0, 0, float(snapshot.extent.x) * cell.x, float(snapshot.lines.count) * cell.y};
    for (const PrVtermPlacement& placement : terminal->placements.span()) {
        if (placement.z < low || placement.z >= high)
            continue;
        float left = float(placement.column) * cell.x;
        float top = float(placement.line - int64_t(topIndex)) * cell.y;
        PrRect clip{
            prF32Max(screen.left, left), prF32Max(screen.top, top + float(placement.firstRow) * cell.y),
            prF32Min(screen.right, left + float(placement.columns) * cell.x),
            prF32Min(screen.bottom, top + float(placement.endRow) * cell.y),
        };
        if (clip.left >= clip.right || clip.top >= clip.bottom)
            continue;
        const PrDrawImage* image = drawImageOf(terminal, placement.image);
        if (image == nullptr)
            continue;
        PrRect destination{
            left + placement.offset.x * cell.x, top + placement.offset.y * cell.y,
            left + (placement.offset.x + placement.extent.x) * cell.x,
            top + (placement.offset.y + placement.extent.y) * cell.y,
        };
        PrRect source{
            float(placement.sourceOrigin.x), float(placement.sourceOrigin.y),
            float(placement.sourceOrigin.x + placement.sourceExtent.x),
            float(placement.sourceOrigin.y + placement.sourceExtent.y),
        };
        PrDrawBrush brush = prDrawBrushImage(prDrawSourceImage(image, source), destination, PrDrawSampling_Linear);
        prDrawListPushClipRect(list, clip, PrDrawEdges_Antialiased);
        prDrawListRect(list, destination, PrDrawPaint{.brush = &brush});
        prDrawListPopClip(list);
    }
}

// Copies what the frame draws from the screen into the snapshot. That is the view's lines whose revisions changed, the
// cursor, the modes, the colors, and the image placements, with their drawing images. If a synchronized update holds
// the screen back, the snapshot keeps the last copy. The caller holds the screen's lock.
void snapshotTake(Terminal* terminal, double time) {
    PrVterm* vterm = terminal->vterm;
    TerminalSnapshot& snapshot = terminal->snapshot;
    PrVtermModeFlags modes = prVtermModes(vterm);
    bool synchronized = (modes & PrVtermModeFlag_Synchronized) != 0;
    if (!synchronized) {
        terminal->synchronizedSince = -1;
    } else if (terminal->synchronizedSince < 0) {
        terminal->synchronizedSince = time;
    }
    if (synchronized)
        prUiRequestFrameAt(terminal->ui, terminal->synchronizedSince + kSynchronizedSeconds);
    snapshot.held = synchronized && time - terminal->synchronizedSince < kSynchronizedSeconds;
    if (snapshot.held && snapshot.valid)
        return;
    snapshot.valid = true;
    snapshot.revision = prVtermRevision(vterm);
    terminal->copiedRevision = snapshot.revision;
    snapshot.extent = prVtermExtent(vterm);
    snapshot.firstLine = firstLine(terminal);
    snapshot.topIndex = viewTopIndex(terminal);
    snapshot.cursor = prVtermCursor(vterm);
    snapshot.modes = modes;
    uint64_t colorRevision = prVtermColorRevision(vterm);
    if (snapshot.colorRevision != colorRevision || snapshot.lines.count == 0) {
        snapshot.colorRevision = colorRevision;
        snapshot.colors = terminalColors(vterm);
    }

    uint32_t count = lineCount(terminal) - snapshot.topIndex;
    count = count < snapshot.extent.y ? count : snapshot.extent.y;
    for (uint32_t row = count; row < snapshot.lines.count; ++row) {
        snapshot.lines.data[row].cells.reset();
        snapshot.lines.data[row].codePoints.reset();
    }
    uint32_t had = snapshot.lines.count < count ? snapshot.lines.count : count;
    snapshot.lines.resize(count);
    prMemZero(snapshot.lines.data + had, sizeof(TerminalViewLine) * (count - had));
    for (uint32_t row = 0; row < count; ++row) {
        TerminalViewLine& copy = snapshot.lines.data[row];
        PrVtermLine line = prVtermLine(vterm, snapshot.topIndex + row);
        if (copy.line.cells == nullptr || copy.line.revision != line.revision) {
            copy.cells.clear();
            copy.cells.append(line.cells, line.cellCount);
            uint32_t codePoints = 0;
            for (uint32_t i = 0; i < line.cellCount; ++i) {
                uint32_t end = line.cells[i].clusterStart + line.cells[i].clusterCount;
                if (line.cells[i].clusterCount != 0 && end > codePoints)
                    codePoints = end;
            }
            copy.codePoints.clear();
            if (codePoints != 0)
                copy.codePoints.append(line.codePoints, codePoints);
        }
        copy.line = line;
        copy.line.cells = copy.cells.data;
        copy.line.codePoints = copy.codePoints.data;
    }

    uint32_t placements = prVtermPlacements(vterm, snapshot.topIndex, count, nullptr, 0);
    terminal->placements.resize(placements);
    prVtermPlacements(vterm, snapshot.topIndex, count, terminal->placements.data, placements);
    for (const PrVtermPlacement& placement : terminal->placements.span())
        imageMake(terminal, placement.image);
}

void snapshotReset(TerminalSnapshot* snapshot) {
    for (TerminalViewLine& line : snapshot->lines.span()) {
        line.cells.reset();
        line.codePoints.reset();
    }
    snapshot->lines.reset();
}

// Rebuilds the view's drawing from the snapshot if what it shows changed. Runs without the screen's lock.
void draw(Terminal* terminal, PrLayoutNode screen, PrVec2 extent, double time) {
    const TerminalSnapshot& snapshot = terminal->snapshot;
    PrVtermCursor cursor = snapshot.cursor;
    bool held = snapshot.held;

    // With reduced motion, the cursor and blinking text do not blink.
    bool steady = prUiUserPreferences(terminal->ui).reducedMotion;
    bool cursorOn = true;
    if (terminal->focused && cursor.blinking && cursor.visible && !steady) {
        double phase = (time - terminal->blinkStart) / kBlinkSeconds;
        cursorOn = (uint64_t(prF64Floor(phase)) & 1) == 0;
        prUiRequestFrameAt(terminal->ui, terminal->blinkStart + (prF64Floor(phase) + 1) * kBlinkSeconds);
    }

    // The bell's flash fades out from kBellOpacity. With reduced motion, it holds that opacity until it ends.
    if (terminal->bellPending) {
        terminal->bellPending = false;
        terminal->bellTime = time;
    }
    float flash = 0;
    if (terminal->bellTime >= 0) {
        double elapsed = time - terminal->bellTime;
        if (elapsed >= kBellSeconds) {
            terminal->bellTime = -1;
        } else if (prUiUserPreferences(terminal->ui).reducedMotion) {
            flash = kBellOpacity;
            prUiRequestMotionAt(terminal->ui, terminal->node, terminal->bellTime + kBellSeconds);
        } else {
            flash = kBellOpacity * float(1 - elapsed / kBellSeconds);
            prUiRequestMotion(terminal->ui, terminal->node);
        }
    }

    uint32_t topIndex = snapshot.topIndex;
    // Blinking text hides in every other phase. Phases count from time zero, so that all blinking text blinks together.
    // The phase matters only if the drawing has blinking text, and then the drawing requests a frame for the next
    // phase.
    double phase = prF64Floor(time / kBlinkSeconds);
    bool blinkHidden = !steady && (uint64_t(phase) & 1) != 0;
    TerminalShown shown{
        .revision = snapshot.revision,
        .top = snapshot.firstLine + topIndex,
        .extent = extent,
        .focused = terminal->focused,
        .cursorOn = cursorOn,
        .generation = terminal->generation,
        .flash = flash,
        .blinkHidden = blinkHidden,
    };
    shown.hasSelection = terminal->hasSelection;
    if (terminal->hasSelection) {
        shown.columnSelection = terminal->columnSelection;
        shown.anchor = terminal->anchor;
        shown.head = terminal->head;
    }
    shown.hasLink = terminal->hasLink;
    if (terminal->hasLink) {
        shown.link = terminal->link.span;
        shown.hyperlink = terminal->link.hyperlink;
    }
    shown.hasJump = terminal->hasJump;
    if (terminal->hasJump)
        shown.jumpLine = terminal->jumpLine;
    PrLayoutTree* tree = terminal->kit->tree;
    bool drawn = prLayoutNodeContent(tree, screen).kind == PrLayoutContentKind_DrawList;
    bool same = terminal->hasShown && isSame(shown, terminal->shown) &&
        (!terminal->shown.blinks || terminal->shown.blinkHidden == blinkHidden);
    if (drawn && (same || held)) {
        if (!steady && terminal->shown.blinks)
            prUiRequestMotionAt(terminal->ui, terminal->node, (phase + 1) * kBlinkSeconds);
        return;
    }
    terminal->shown = shown;
    terminal->hasShown = true;

    const PrTextGridMetrics& m = terminal->drawer.metrics;
    PrUVec2 size = snapshot.extent;
    // The view's drawing holds the gutter, and then the screen offset to its first cell. A box beneath the view paints
    // the screen's background (prTerminalFrame), so the drawing holds only the cell backgrounds that differ from it.
    PrDrawList* view = prDrawListCreate();
    drawGutter(terminal, view);
    PrDrawList* list = prDrawListCreate();
    // The rows' drawings hold resolved colors, so a change of the screen's colors invalidates them.
    uint64_t colorRevision = snapshot.colorRevision;
    if (terminal->rows.count != size.y || terminal->rowsColorRevision != colorRevision) {
        rowsReset(terminal);
        terminal->rows.resize(size.y);
        prMemZero(terminal->rows.data, sizeof(TerminalRow) * size.y);
        terminal->rowsColorRevision = colorRevision;
    }
    uint32_t shownRows = snapshot.lines.count;
    for (uint32_t row = 0; row < shownRows; ++row) {
        uint32_t index = topIndex + row;
        const PrVtermLine& line = snapshot.lines.data[row].line;
        uint32_t selectionStart = 0;
        uint32_t selectionEnd = 0;
        selectedColumns(terminal, snapshot.firstLine + index, snapshot.extent.x, &selectionStart, &selectionEnd);
        TerminalRow& cached = terminal->rows.data[row];
        bool fresh = cached.list != nullptr && cached.revision == line.revision &&
            cached.selectionStart == selectionStart && cached.selectionEnd == selectionEnd &&
            (!cached.blinks || cached.blinkHidden == blinkHidden);
        if (!fresh) {
            prDrawListRelease(cached.backgrounds);
            prDrawListRelease(cached.list);
            cached.backgrounds = prDrawListCreate();
            cached.list = prDrawListCreate();
            cached.blinks = terminalDrawRow(&terminal->drawer, cached.backgrounds, cached.list, snapshot.colors, line,
                selectionStart, selectionEnd, blinkHidden);
            cached.revision = line.revision;
            cached.selectionStart = selectionStart;
            cached.selectionEnd = selectionEnd;
            cached.blinkHidden = blinkHidden;
        }
        terminal->shown.blinks = terminal->shown.blinks || cached.blinks;
    }
    if (!steady && terminal->shown.blinks)
        prUiRequestMotionAt(terminal->ui, terminal->node, (phase + 1) * kBlinkSeconds);

    // Images draw in three layers by z-index: under the cell backgrounds, between the backgrounds and the text, and
    // over the text. They are recorded in the view's list, not in the rows' drawings, so that a row's cached drawing
    // stays valid while the images over it change.
    ++terminal->drawings;
    drawImages(terminal, list, INT64_MIN, kPrVtermImageUnderBackgrounds);
    for (uint32_t row = 0; row < shownRows; ++row)
        prDrawListEmbed(list, terminal->rows.data[row].backgrounds, PrDVec2{0, double(row) * double(m.cell.y)});
    drawImages(terminal, list, kPrVtermImageUnderBackgrounds, 0);
    for (uint32_t row = 0; row < shownRows; ++row)
        prDrawListEmbed(list, terminal->rows.data[row].list, PrDVec2{0, double(row) * double(m.cell.y)});
    drawImages(terminal, list, 0, INT64_MAX);
    imagesTrim(terminal);
    drawCursor(terminal, list, cursorOn);

    if (terminal->hasLink)
        drawLink(terminal, list);

    if (flash > 0) {
        PrColor color = prColorFromColor8(snapshot.colors.roles[PrVtermColorRole_Foreground]);
        color.a = flash;
        PrDrawPaint paint{};
        paint.color = prColor16FromColor(color);
        prDrawListRect(list, PrRect{0, 0, extent.x, extent.y}, paint);
    }
    prDrawListEmbed(view, list, PrDVec2{double(kGutterWidth), 0});
    prDrawListRelease(list);

    PrLayoutContent content{};
    content.kind = PrLayoutContentKind_DrawList;
    content.list = view;
    prLayoutTreeFrameSetNaturalSize(tree, screen, prLayoutNaturalSizeNone());
    prLayoutTreeFrameSetContent(tree, screen, &content);
    prDrawListRelease(view);
}

// The style of a block box of `extent` px that clips its contents.
PrLayoutStyleIndex boxStyle(PrLayoutTree* tree, PrVec2 extent) {
    PrLayoutBoxStyle box = prLayoutBoxStyleDefault();
    box.display = PrLayoutDisplay_Block;
    box.width = prLayoutLengthPx(extent.x);
    box.height = prLayoutLengthPx(extent.y);
    box.overflowX = PrLayoutOverflow_Hidden;
    box.overflowY = PrLayoutOverflow_Hidden;
    PrLayoutStyle style = prLayoutStyleDefault();
    style.groups[PrLayoutGroup_Box] = prLayoutBoxStyleIntern(tree, &box);
    return prLayoutStyleIntern(tree, &style);
}

// Sets the background of `node` to `color` for this round, and keeps the rest of its style.
void setBackground(PrLayoutTree* tree, PrLayoutNode node, PrColor color) {
    PrLayoutStyle style = prLayoutStyleRecord(tree, prLayoutTreeFrameStyle(tree, node));
    PrLayoutDecorationStyle decoration = prLayoutDecorationStyleRecord(tree, style.groups[PrLayoutGroup_Decoration]);
    decoration.background = prColor16FromColor(color);
    style.groups[PrLayoutGroup_Decoration] = prLayoutDecorationStyleIntern(tree, &decoration);
    prLayoutTreeFrameSetStyle(tree, node, prLayoutStyleIntern(tree, &style));
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

// C opaque types
struct PrTerminal : Terminal {};

PrStatus prTerminalCreate(const PrTerminalDesc* desc, PrTerminal* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    PrTerminal* terminal = allocZeroed<PrTerminal>();
    terminal->kit = desc->kit;
    terminal->ui = desc->kit->ui;
    terminal->app = desc->app;
    terminal->synchronizedSince = -1;
    terminal->openFile = desc->openFile;
    terminal->openFileContext = desc->openFileContext;
    terminal->notificationTag = desc->notificationTag;
    terminal->bellTime = -1;
    prMutexInit(&terminal->mutex);
    prConditionInit(&terminal->turn);

    // The program, or the user's shell if the program is empty.
    PrStringView program = desc->program;
    const PrStringView* PR_NULLABLE arguments = desc->arguments;
    uint32_t argumentCount = desc->argumentCount;
    PrRawString shell = prRawStringNull();
    TerminalShellLaunch launch{};
    RawArray<PrStringView> launchArguments;
    if (program.size == 0) {
#if defined(_WIN32)
        shell = terminalShellVariable("COMSPEC", desc->environment, desc->environmentCount);
        program = prRawStringView(&shell).size != 0 ? prRawStringView(&shell) : PrStringView("cmd.exe");
        arguments = nullptr;
        argumentCount = 0;
#else
        static const PrStringView kLogin[] = {"-l"};
        shell = terminalShellVariable("SHELL", desc->environment, desc->environmentCount);
        program = prRawStringView(&shell).size != 0 ? prRawStringView(&shell) : PrStringView("/bin/sh");
        arguments = kLogin;
        argumentCount = 1;
#endif
        // If the shell has no integration, or the scripts cannot be written, the shell runs without integration.
        PrRawString directory = prRawStringNull();
        if (desc->shellIntegration == PrTerminalShellIntegration_On)
            directory = terminalShellDirectory();
        bool integrated = !prRawStringIsNull(&directory) && terminalShellIntegrate(program,
            prRawStringView(&directory), desc->environment, desc->environmentCount, &launch);
        prRawStringReset(&directory);
        if (integrated) {
            for (const PrRawString& argument : launch.arguments.span())
                launchArguments.push(prRawStringView(&argument));
            arguments = launchArguments.data;
            argumentCount = launchArguments.count;
        }
    }
    size_t base = program.size;
    while (base > 0 && program.data[base - 1] != '/' && program.data[base - 1] != '\\')
        --base;
    terminal->name = prRawStringCopy(PrStringView{program.data + base, program.size - base});
    terminal->directory = prRawStringCopy(desc->directory);

    setFonts(terminal, desc->font);
    terminal->drawer.shaper = prTextShaperCreate(PrTextShaperBackend_Default);
    terminal->drawer.grid = prTextGridCreate();
    updateMetrics(terminal);

    uint32_t scrollback = desc->scrollback;
    // The program runs on this machine, so it may send images by file name.
    terminal->compressCodec = prCompressCodecCreate(PrCompressCodecBackend_Default);
    if (terminal->compressCodec != nullptr)
        terminal->imageCodec = prImageCodecCreate(PrImageCodecBackend_Default, terminal->compressCodec);
    PrVtermDesc vtermDesc{
        .extent = {80, 24},
        .scrollback = scrollback,
        .imageBytes = kPrVtermImageBytesDefault,
        .imageFlags = PrVtermImageFlag_Files,
        .compressCodec = terminal->compressCodec,
        .imageCodec = terminal->imageCodec,
    };
    terminal->vterm = prVtermCreate(&vtermDesc);
    prVtermSetCellPixelExtent(terminal->vterm, cellPixels(terminal));
    for (uint32_t i = 0; i < 16; ++i)
        prVtermPaletteSetColor(terminal->vterm, i, kNamedColors[i]);
    updateTheme(terminal);

    // The environment: the variables that identify the terminal, then the caller's, then the shell integration's. The
    // integration's variables replace the caller's of the same name.
    RawArray<PrStringView> integration;
    for (const PrRawString& entry : launch.environment.span())
        integration.push(prRawStringView(&entry));
    RawArray<PrStringView> environment;
    for (PrStringView variable : kEnvironment) {
        if (!setsName(desc->environment, desc->environmentCount, variable))
            environment.push(variable);
    }
    for (uint32_t i = 0; i < desc->environmentCount; ++i) {
        if (!setsName(integration.data, integration.count, desc->environment[i]))
            environment.push(desc->environment[i]);
    }
    environment.append(integration.data, integration.count);
    PrProcessDesc processDesc{
        .program = program,
        .arguments = arguments,
        .argumentCount = argumentCount,
        .directory = desc->directory,
        .environment = environment.data,
        .environmentCount = environment.count,
        .streams = PrProcessStreams_Terminal,
        .terminalExtent = prVtermExtent(terminal->vterm),
        .terminalPixelExtent = {80 * cellPixels(terminal).x, 24 * cellPixels(terminal).y},
        .wake = desc->app != nullptr ? wakeApp : nullptr,
        .wakeContext = desc->app,
        .receive = receiveOutput,
        .receiveContext = terminal,
    };
    terminal->processExtent = processDesc.terminalExtent;
    terminal->processPixels = processDesc.terminalPixelExtent;
    PrStatus status = prProcessLaunch(desc->launcher, &processDesc, &terminal->process);
    environment.reset();
    integration.reset();
    launchArguments.reset();
    terminalShellLaunchReset(&launch);
    prRawStringReset(&shell);
    if (status != PrStatus_Ok) {
        prTerminalDestroy(terminal);
        return status;
    }
    *out = terminal;
    return PrStatus_Ok;
}

void prTerminalDestroy(PrTerminal* PR_NULLABLE terminal) {
    if (terminal == nullptr)
        return;
    // The process's thread parses into the screen until the process is destroyed, so the process goes first.
    prProcessDestroy(terminal->process);
    prConditionReset(&terminal->turn);
    prMutexReset(&terminal->mutex);
    snapshotReset(&terminal->snapshot);
    terminal->title.reset();
    rowsReset(terminal);
    terminal->rows.reset();
    imagesReset(terminal);
    terminal->images.reset();
    terminal->placements.reset();
    prVtermDestroy(terminal->vterm);
    prImageCodecDestroy(terminal->imageCodec);
    prCompressCodecDestroy(terminal->compressCodec);
    for (PrTextFont* font : terminal->drawer.fonts)
        prTextFontRelease(font);
    prTextGridDestroy(terminal->drawer.grid);
    prTextShaperDestroy(terminal->drawer.shaper);
    terminal->drawer.text.reset();
    terminal->drawer.cells.reset();
    terminal->drawer.groups.reset();
    prRawStringReset(&terminal->name);
    prRawStringReset(&terminal->directory);
    prRawStringReset(&terminal->link.target);
    prRawStringReset(&terminal->opened.target);
    prRawStringReset(&terminal->paste);
    prMemoryFree(terminal);
}

void prTerminalFrame(PrTerminal* terminal, PrLayoutNode parent, PrVec2 extent, PrTerminalBackground background) {
    PR_ASSERT(background <= PrTerminalBackground_Parent);
    PrUi* ui = terminal->ui;
    PrLayoutTree* tree = terminal->kit->tree;
    double time = prUiCurrentRound(ui)->time;
    terminal->inFrame = true;
    terminal->declared = true;
    updateMetrics(terminal);

    // The screen fills the extent right of the gutter with whole cells, at least one column and one row.
    const PrVec2 cell = terminal->drawer.metrics.cell;
    uint32_t columns = uint32_t(prF32Max(1, prF32Floor((extent.x - kGutterWidth) / cell.x)));
    uint32_t rows = uint32_t(prF32Max(1, prF32Floor(extent.y / cell.y)));

    PrLayoutNode node = prUiElement(ui, parent, 0, boxStyle(tree, extent),
        PrUiFlag_Pressable | PrUiFlag_Focusable | PrUiFlag_Wheel);
    terminal->node = node;
    // A focus request repeats each frame until the view has focus. Focus shows only in the frame after the request.
    if (terminal->focusAsked) {
        if (prUiIsFocusWithin(ui, node)) {
            terminal->focusAsked = false;
        } else {
            prUiSetFocus(ui, node);
            prUiRequestFrame(ui);
        }
    }
    claimKeys(terminal);

    // Everything that reads or writes the screen holds its lock. The drawing reads the snapshot after the lock is
    // released.
    lockScreen(terminal);
    PrVterm* vterm = terminal->vterm;
    terminal->showing = true;
    // The theme updates first, so that a color scheme report goes to the program with this frame's output.
    updateTheme(terminal);
    PrUVec2 pixels = cellPixels(terminal);
    PrUVec2 known = prVtermCellPixelExtent(vterm);
    if (known.x != pixels.x || known.y != pixels.y)
        prVtermSetCellPixelExtent(vterm, pixels);
    PrUVec2 size = prVtermExtent(vterm);
    if (size.x != columns || size.y != rows) {
        prVtermResize(vterm, PrUVec2{columns, rows});
        terminal->hasSelection = false;
    }
    handleEvents(terminal);
    buildPasteConfirmation(terminal);
    bool focused = prUiIsFocusWithin(ui, node);
    if (focused != terminal->focused) {
        terminal->focused = focused;
        terminal->blinkStart = time;
        prVtermInputFocus(vterm, focused);
    }
    service(terminal);
    snapshotTake(terminal, time);
    unlockScreen(terminal);
    PrLayoutNode backgroundNode = background == PrTerminalBackground_Parent ? parent : node;
    setBackground(tree, backgroundNode, terminalBackground(terminal->snapshot.colors));

    // The pseudoterminal resizes after the screen, so that the screen already has the new size when the program redraws
    // for it.
    resizeProcess(terminal, PrUVec2{columns, rows});
    if (terminal->hasOpened) {
        terminal->hasOpened = false;
        openLink(terminal, terminal->opened);
        prRawStringReset(&terminal->opened.target);
    }
    bool mouse = (terminal->snapshot.modes & PrVtermModeFlag_MouseReports) != 0;
    PrAppCursor pointer = mouse ? PrAppCursor_Arrow : PrAppCursor_Text;
    if (terminal->hasLink)
        pointer = PrAppCursor_Hand;
    prUiSetCursor(ui, node, pointer);

    // The drawing passes the pointer through to the view, which receives its moves as well as its presses.
    PrLayoutNode screen = prLayoutTreeFrameReplaced(tree, node, 0, boxStyle(tree, extent));
    prUiSetFlags(ui, screen, PrUiFlag_Transparent);
    draw(terminal, screen, extent, time);

    // The user sees a terminal that shows in the active window, so its alerts clear.
    if (isWindowActive(terminal)) {
        terminal->alerts = 0;
        terminal->attentionAsked = false;
    }
    terminal->inFrame = false;
}

PrColor prTerminalBackground(const PrTerminal* terminal) {
    if (terminal->snapshot.valid)
        return terminalBackground(terminal->snapshot.colors);
    lockScreen(terminal);
    PrColor color = terminalBackground(terminalColors(terminal->vterm));
    unlockScreen(terminal);
    return color;
}

void prTerminalUpdate(PrTerminal* terminal) {
    // A terminal that does not show has no element, no focus, and no pointer or drag over it.
    terminal->node = kPrLayoutNodeNone;
    terminal->selecting = false;
    terminal->hasPointer = false;
    terminal->hasLink = false;
    lockScreen(terminal);
    terminal->showing = false;
    if (terminal->focused) {
        terminal->focused = false;
        prVtermInputFocus(terminal->vterm, false);
    }
    service(terminal);
    unlockScreen(terminal);
}

PrLayoutNode prTerminalNode(const PrTerminal* terminal) {
    return terminal->node;
}

void prTerminalFocus(PrTerminal* terminal) {
    terminal->focusAsked = true;
    prUiRequestFrame(terminal->ui);
}

bool prTerminalHasFocus(const PrTerminal* terminal) {
    return terminal->focused;
}

PrStringView prTerminalTitle(const PrTerminal* terminal) {
    return terminalTitle(terminal);
}

PrRawString prTerminalWorkingDirectory(const PrTerminal* terminal) {
    lockScreen(terminal);
    PrRawString directory = workingDirectory(terminal);
    unlockScreen(terminal);
    return directory;
}

PrProcessInfo prTerminalProcessInfo(const PrTerminal* terminal) {
    return prProcessInfo(terminal->process);
}

PrTerminalAlertFlags prTerminalAlerts(const PrTerminal* terminal) {
    return terminal->alerts;
}

uint64_t prTerminalNotificationTag(const PrTerminal* terminal) {
    return terminal->notificationTag;
}

PrVterm* prTerminalVtermLock(PrTerminal* terminal) {
    lockScreen(terminal);
    return terminal->vterm;
}

void prTerminalVtermUnlock(PrTerminal* terminal) {
    unlockScreen(terminal);
}

void prTerminalSetColorScheme(PrTerminal* terminal, PrAppColorScheme scheme) {
    lockScreen(terminal);
    if (prVtermColorScheme(terminal->vterm) != scheme) {
        prVtermSetColorScheme(terminal->vterm, scheme);
        if (terminal->process != nullptr && !terminal->exitShown)
            flushOutput(terminal, terminal->process);
    }
    unlockScreen(terminal);
}

PrTerminalLockInfo prTerminalLockInfo(const PrTerminal* terminal) {
    return terminal->waits;
}

PrVec2 prTerminalCellExtent(const PrTerminal* terminal) {
    return terminal->drawer.metrics.cell;
}

void prTerminalSetFont(PrTerminal* terminal, const PrTextFontDesc* font) {
    setFonts(terminal, *font);
    updateMetrics(terminal);
    prUiRequestFrame(terminal->ui);
}

PrRawString prTerminalSelection(const PrTerminal* terminal) {
    lockScreen(terminal);
    PrRawString text = selectionText(terminal);
    unlockScreen(terminal);
    return text;
}

void prTerminalCopy(PrTerminal* terminal) {
    lockScreen(terminal);
    terminalCopy(terminal);
    unlockScreen(terminal);
}

void prTerminalPaste(PrTerminal* terminal) {
    terminalPaste(terminal);
}

PrVec2 prTerminalScreenOrigin(const PrTerminal* terminal) {
    (void)terminal;
    return PrVec2{kGutterWidth, 0};
}

PrVtermShellInfo prTerminalShellInfo(const PrTerminal* terminal) {
    lockScreen(terminal);
    PrVtermShellInfo info = prVtermShellInfo(terminal->vterm);
    unlockScreen(terminal);
    return info;
}

bool prTerminalScrollToPrompt(PrTerminal* terminal, PrTerminalDirection direction) {
    lockScreen(terminal);
    bool moved = scrollToPrompt(terminal, direction);
    unlockScreen(terminal);
    if (moved)
        prUiRequestFrame(terminal->ui);
    return moved;
}

PrRawString prTerminalCommandOutput(const PrTerminal* terminal) {
    lockScreen(terminal);
    PrRawString text = commandOutputText(terminal);
    unlockScreen(terminal);
    return text;
}

void prTerminalCopyCommandOutput(PrTerminal* terminal) {
    if (terminal->app == nullptr)
        return;
    PrRawString text = prTerminalCommandOutput(terminal);
    if (!prRawStringIsEmpty(&text))
        prAppSetClipboardText(terminal->app, prRawStringView(&text));
    prRawStringReset(&text);
}

bool prTerminalSelectCommandOutput(PrTerminal* terminal) {
    lockScreen(terminal);
    uint32_t prompt = 0;
    uint32_t first = 0;
    uint32_t end = 0;
    bool found = terminal->hasJump && lineIndexOf(terminal, terminal->jumpLine, &prompt);
    found = (found || lastFinished(terminal, &prompt)) && outputLines(terminal, prompt, &first, &end);
    if (found)
        selectLines(terminal, first, end);
    unlockScreen(terminal);
    if (found)
        prUiRequestFrame(terminal->ui);
    return found;
}
