// Studio's terminal: a program that runs on a pseudoterminal, shown in a view of a window.
//
// A terminal runs its program on a pseudoterminal of its own process (pr/process.h) and parses the program's output
// (pr/vterm.h). It draws the screen as cells of a monospaced font, aligned to target pixels. An application gives the
// terminal the kit to draw with, and declares it in each round as one part of its window.
//
// While the view has focus, the terminal sends the user's keys, text, pastes, pointer and scrolls to the program, in
// the forms that the program asked for. If the program does not read the pointer, the view handles it instead:
//
// - A drag selects text. With Shift, a drag selects even while the program reads the pointer.
// - A click with the link modifier opens the link under the pointer. The modifier is Command on a Mac and Control
//   elsewhere. A link is a hyperlink that the program wrote, an http or https URL in the text, or a file reference.
// - The wheel scrolls back through the lines that left the screen.
//
// The clipboard keys are Command with C and V on a Mac. Elsewhere, they are Control with Shift and C or V, and Control
// or Shift with Insert. On Windows, Control with V also pastes, and Control with C copies if text is selected.
//
// A paste that holds line breaks asks the user to confirm it first, because each line would run as a command. It does
// not ask if the program enabled bracketed paste.
//
// Shell integration. If the shell marks its prompts and commands (pr/vterm.h), the view shows its commands. The
// terminal's shell integration makes the user's shell write these marks. A gutter left of the screen marks each
// finished command in the theme's success or danger color. A click in the gutter selects the output of the command on
// that line. Command with Up or Down on a Mac, or Control with Shift and Up or Down elsewhere, scrolls to the previous
// or the next prompt.
//
// Threading. One thread uses a terminal. The program's output is parsed on a thread of its process as it arrives. The
// parser writes into the screen (pr/vterm.h), which a lock guards. The parser holds the lock for a bounded slice of
// output at a time, and yields it at the end of a slice if the terminal's thread waits for it. A frame copies what it
// draws while it holds the lock, and then draws from the copy.
//
// The parser wakes the application (prAppWake) if the screen changes while the terminal shows. It also wakes it for an
// event such as a bell, whether or not the terminal shows. It wakes it at most once until the next frame or update. A
// synchronized update holds back changes until it ends, or for at most one second. The terminal acts on events in its
// next frame, or in its next update if it does not show.
//
// Alerts. The bell flashes the screen briefly if the terminal shows. The flash fades unless the user asks for reduced
// motion. If the window is not active, the bell also requests the user's attention (prAppWindowRequestAttention), once
// until the window is active again.
//
// The program's notifications (pr/vterm.h) post as desktop notifications (prAppNotificationPost), under the condition
// that the program names. A notification with no condition posts unless the terminal has focus in the active window. A
// notification without a title takes the terminal's title. If the system cannot post it while the window is inactive,
// the terminal requests attention instead. A bell or notification that the user did not see is an alert
// (prTerminalAlerts) until the user sees the terminal.

#pragma once

#include "pr/app.h"
#include "pr/base.h"
#include "pr/color.h"
#include "pr/kit.h"
#include "pr/layout_tree.h"
#include "pr/process.h"
#include "pr/raw_string.h"
#include "pr/status.h"
#include "pr/string_view.h"
#include "pr/text_font.h"
#include "pr/vec.h"
#include "pr/vterm.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// A program on a pseudoterminal, and the view that shows it.
typedef struct PrTerminal PrTerminal;

// The default number of scrollback lines.
enum { kPrTerminalScrollbackDefault = 10000 };

// A link to a file that the user opened. The line and column are the ones that the text named after the path. They
// count from 1, and are 0 if the text names none.
typedef struct PrTerminalFileLink {
    PrStringView path;  // absolute
    uint32_t line;
    uint32_t column;
} PrTerminalFileLink;

// Called when the user opens a link to a file. The link is valid only during the call.
typedef void (*PrTerminalOpenFileFn)(void* PR_NULLABLE context, const PrTerminalFileLink* link);

// Whether the terminal starts the user's shell with its shell integration. The integration is a script that the shell
// loads after the user's own startup files. It marks the prompts, the commands and their exit codes, and reports the
// working directory (OSC 7). The terminal has integrations for zsh, bash and fish. On is zero, so a zeroed descriptor
// enables it.
enum {
    PrTerminalShellIntegration_On,
    PrTerminalShellIntegration_Off,
};
typedef uint8_t PrTerminalShellIntegration;

// What a terminal runs and draws with.
typedef struct PrTerminalDesc {
    PrKit* kit;

    // The monospaced font of the screen. Its size sets the size of a cell. The terminal derives the bold, italic and
    // bold italic fonts from the same families.
    PrTextFontDesc font;

    // The window that shows the terminal, or null for none. Its application takes the clipboard's writes, opens URLs,
    // posts notifications and wakes for the program's output.
    PrAppWindow* PR_NULLABLE window;

    // The launcher, which must outlive the terminal, and the program it runs (PrProcessDesc). The program is a path or
    // a name to search for in PATH. A null directory is the launcher's. The terminal adds the variables that name its
    // type (TERM and others) unless `environment` sets them.
    //
    // An empty program runs the user's shell, and ignores `arguments`. On POSIX, the shell is the one that SHELL names
    // in the program's environment, or /bin/sh, and it runs as a login shell. On Windows, it is the one that COMSPEC
    // names, or cmd.exe.
    PrProcessLauncher* launcher;
    PrStringView program;
    const PrStringView* PR_NULLABLE arguments;
    uint32_t argumentCount;
    PrStringView directory;
    const PrStringView* PR_NULLABLE environment;
    uint32_t environmentCount;

    uint32_t scrollback;                         // the most scrollback lines to keep, up to kPrVtermMaxScrollback
    PrTerminalShellIntegration shellIntegration; // applies only if `program` is empty
    uint64_t notificationTag;                    // the tag of the desktop notifications that the terminal posts

    // Opens a link to a file, with its context. If it is null, the terminal opens the file's URL through the
    // application.
    PrTerminalOpenFileFn PR_NULLABLE openFile;
    void* PR_NULLABLE openFileContext;
} PrTerminalDesc;

// Creates a terminal, starts its program, and writes the terminal to `*out`. If the program fails to start, returns the
// status of prProcessLaunch and sets `*out` to null.
PrStatus prTerminalCreate(const PrTerminalDesc* desc, PrTerminal* PR_NULLABLE* PR_NONNULL out);

// Ends the program if it still runs, and frees the terminal.
void prTerminalDestroy(PrTerminal* PR_NULLABLE terminal);

// The box that prTerminalFrame paints with the terminal's background (prTerminalBackground).
enum {
    PrTerminalBackground_View,      // the view's own box
    PrTerminalBackground_Parent,    // the box of `parent`, which colors the window's canvas if it is the root
};
typedef uint8_t PrTerminalBackground;

// Declares the terminal's view under `parent` for this round, with an extent of `extent` px. Draws what the program
// wrote, sends the program the user's input, and acts on the screen's events. The screen takes as many whole columns
// and rows as fit right of the gutter, and the program's pseudoterminal is resized to match.
//
// The box that `background` names takes the terminal's background color, which shows under the whole view. Over it, the
// view draws the gutter's marks, the cell backgrounds that differ from it, the text, the images, the selection and the
// bell's flash. A strip narrower than a cell past the last column and row shows only the background.
void prTerminalFrame(PrTerminal* terminal, PrLayoutNode parent, PrVec2 extent, PrTerminalBackground background);

// The terminal's background color, with straight alpha. It is the screen's default background, or its default
// foreground in reverse video. The kit's theme sets it, and the program may change it (OSC 11). Returns the color that
// the last frame showed, or the screen's current color before the first frame.
PrColor prTerminalBackground(const PrTerminal* terminal);

// Services a terminal that the round does not declare. An application calls it each round for each terminal that does
// not show. It acts on the screen's events, such as a bell or a new title, and shows the program's exit. The program's
// output is parsed whether or not the terminal shows, so the program never blocks on its output. The terminal loses
// focus, and the screen keeps its columns and rows until a frame declares it again.
void prTerminalUpdate(PrTerminal* terminal);

// The element that shows the screen in the last frame, or kPrLayoutNodeNone. This element takes focus.
PrLayoutNode prTerminalNode(const PrTerminal* terminal);

// Gives the screen focus in the next frame.
void prTerminalFocus(PrTerminal* terminal);

// Returns true if the screen had focus in the last frame.
bool prTerminalHasFocus(const PrTerminal* terminal);

// The title that the program set, or the program's file name if it set none. The view is valid until the next frame or
// update.
PrStringView prTerminalTitle(const PrTerminal* terminal);

// The program's working directory, as a path on this machine. It is the file URL that the program last reported (OSC 7)
// if that URL names this machine. Otherwise, it is the directory of the foreground process of the pseudoterminal, if
// the platform can tell. Otherwise, it is the directory that the program started in, which is the null string if the
// program started in the launcher's directory. The caller releases the string with prRawStringReset.
PrRawString prTerminalWorkingDirectory(const PrTerminal* terminal);

// Whether the program exited, and how.
PrProcessInfo prTerminalProcessInfo(const PrTerminal* terminal);

// The kinds of attention requests from the program that the user has not seen.
enum {
    PrTerminalAlertFlag_Bell = 1u << 0,
    PrTerminalAlertFlag_Notification = 1u << 1,
};
typedef uint8_t PrTerminalAlertFlags;

// The alerts since the user last saw the terminal, which is the last time a frame declared it in the active window. An
// application can show them on a terminal that it does not show, for example as a badge on a tab.
PrTerminalAlertFlags prTerminalAlerts(const PrTerminal* terminal);

// The tag of the terminal's desktop notifications (PrTerminalDesc). A click on one of them reports this tag in a
// Notification event.
uint64_t prTerminalNotificationTag(const PrTerminal* terminal);

// Locks the screen and returns it. The caller may read and write the screen until it unlocks it, as a test does. The
// program's output is not parsed while the caller holds the lock, and the caller must not call the terminal.
PrVterm* prTerminalVtermLock(PrTerminal* terminal);
void prTerminalVtermUnlock(PrTerminal* terminal);

// Sets the color scheme that the program can query (prVtermSetColorScheme). If the program asked for changes, it is
// told of a change.
void prTerminalSetColorScheme(PrTerminal* terminal, PrAppColorScheme scheme);

// The number of buckets in PrTerminalLockInfo. Bucket 0 counts waits shorter than 1 microsecond. Bucket `i` counts
// waits from 2^(i-1) up to 2^i microseconds. The last bucket counts every longer wait.
enum { kPrTerminalLockBuckets = 16 };

// The most output, in bytes, that the parser takes while the UI thread waits for the screen's lock: one slice. The
// parser yields the lock at the end of the slice, so the UI waits for one slice of parsing at most, however busy the
// program keeps the parser.
enum { kPrTerminalParseSliceBytes = 16 * 1024 };

// The UI thread's waits for the screen's lock since the terminal was created. A wait's duration also counts the time
// that the scheduler kept the parser off the processor while it held the lock, which load alone can make long. The
// output that the parser took during a wait does not depend on load.
typedef struct PrTerminalLockInfo {
    uint64_t count;
    double totalMilliseconds;
    double worstMilliseconds;
    uint64_t buckets[kPrTerminalLockBuckets];   // wait counts by duration (kPrTerminalLockBuckets)
    uint64_t worstBytes;                        // the most output that the parser took during one wait
} PrTerminalLockInfo;

// Returns the terminal's waits for the screen's lock.
PrTerminalLockInfo prTerminalLockInfo(const PrTerminal* terminal);

// The extent of one cell of the screen in px, for the current font and the last frame's target pixel ratio.
PrVec2 prTerminalCellExtent(const PrTerminal* terminal);

// Sets the font of the screen, as PrTerminalDesc's `font` does, and recomputes the cell extent. The next frame fills
// its extent with the new cells, and resizes the program's pseudoterminal to the new columns and rows.
void prTerminalSetFont(PrTerminal* terminal, const PrTextFontDesc* font);

// The selected text, or the empty string if nothing is selected. A line that wrapped joins the next. Every other line
// loses its trailing spaces and ends with a line feed, except the last. The caller releases the string with
// prRawStringReset.
//
// A drag selects cells. A double click selects words, and a triple click selects lines, and a drag after either extends
// by that unit. A drag with Alt selects a rectangular block of cells, and each of its rows becomes a line.
PrRawString prTerminalSelection(const PrTerminal* terminal);

// prTerminalCopy copies the selected text to the clipboard, and does nothing without a window. prTerminalPaste reads
// the clipboard's text through the kit's context (prUiPaste), and pastes it to the program when it arrives, as if the
// user pasted it.
void prTerminalCopy(PrTerminal* terminal);
void prTerminalPaste(PrTerminal* terminal);

// The position of the screen's first cell in the view, in px. It is right of the gutter.
PrVec2 prTerminalScreenOrigin(const PrTerminal* terminal);

// What the shell is doing, and the result of the last command that finished (prVtermShellInfo).
PrVtermShellInfo prTerminalShellInfo(const PrTerminal* terminal);

// A direction through the prompts of the scrollback.
enum {
    PrTerminalDirection_Previous,
    PrTerminalDirection_Next,
};
typedef uint8_t PrTerminalDirection;

// Scrolls the view to the prompt before or after a starting prompt. The start is the prompt that the view last scrolled
// to. If the view has not scrolled to a prompt since it last showed the live screen, the start is the current command's
// prompt.
//
// The view puts the prompt on its top line, as far as the scrollback allows. The gutter highlights the prompt until the
// user types or the view returns to the live screen. Moving past the last prompt returns the view to the live screen.
// Returns false if the view did not move, which includes every call while the alternate screen shows.
bool prTerminalScrollToPrompt(PrTerminal* terminal, PrTerminalDirection direction);

// The output of the last command that finished, as text in the form of prTerminalSelection, or the empty string if no
// finished command's output is on the screen or in the scrollback. The caller releases the string with
// prRawStringReset.
PrRawString prTerminalCommandOutput(const PrTerminal* terminal);

// Copies prTerminalCommandOutput to the clipboard if it is not empty. Does nothing without an application.
void prTerminalCopyCommandOutput(PrTerminal* terminal);

// Selects the output lines of the command whose prompt the view last scrolled to. If the view has not scrolled to a
// prompt, selects the output of the last command that finished. Returns false, and leaves the selection as it was, if
// the command has no output.
bool prTerminalSelectCommandOutput(PrTerminal* terminal);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
