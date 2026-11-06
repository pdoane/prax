// Virtual terminals: the screen of a terminal emulator, which changes as a program writes bytes to its terminal.
//
// A terminal parses the escape sequences of ECMA-48 and the DEC terminals, with the extensions of xterm and later
// terminals. It keeps the screen that they draw: a grid of cells, the cursor, and the lines that scrolled off the top.
// Each cell holds a grapheme cluster with its colors and attributes.
//
// A terminal does no input or output of its own. Its answers to the program, such as a report of the cursor's position,
// and the bytes of the user's input (pr/vterm_input.h) are queued as output, which the caller sends to the program. So
// the same terminal serves a pseudoterminal, a remote connection or a recording. The terminal reads a file only if a
// program names an image by its path and the caller allows it (PrVtermImageFlag_Files).
//
// The terminal's lines are the scrollback, oldest first, followed by the rows of the screen that shows. The alternate
// screen has no scrollback of its own. The scrollback of the primary screen stays readable while the alternate screen
// shows. Lines that the program wrote past the right edge are wrapped, and a resize to another width rewraps them.
//
// A terminal is not thread-safe. Only one thread may use it at a time.

#pragma once

#include "pr/app_user_preferences.h"
#include "pr/base.h"
#include "pr/byte_view.h"
#include "pr/color.h"
#include "pr/compress.h"
#include "pr/image.h"
#include "pr/raw_string.h"
#include "pr/string_view.h"
#include "pr/vec.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// ---------------------------------------------------------------------------------------------------------------------
// Cells

// What a cell's color refers to.
enum {
    PrVtermColorKind_Default,   // the color of the role it is drawn in (PrVtermColorRole)
    PrVtermColorKind_Palette,   // an entry of the terminal's palette
    PrVtermColorKind_Rgb,       // an RGB color of its own
};
typedef uint8_t PrVtermColorKind;

// A color of a cell. An RGB color holds its channels. A palette color holds its entry in `red`, which prVtermColorIndex
// reads. The bytes that a color's kind does not use are zero, so that equal colors have equal bytes: cells and pens are
// compared and hashed as raw memory.
PR_NO_PADDING_BEGIN
typedef struct PrVtermColor {
    PrVtermColorKind kind;
    uint8_t red;
    uint8_t green;
    uint8_t blue;
} PrVtermColor;
PR_NO_PADDING_END

// Makes a color of palette entry `index`, or of channels `red`, `green` and `blue`. The default color is all zeros.
PR_CONSTEXPR PrVtermColor prVtermColorPalette(uint8_t index) {
    PrVtermColor color = {.kind = PrVtermColorKind_Palette, .red = index};
    return color;
}
PR_CONSTEXPR PrVtermColor prVtermColorRgb(uint8_t red, uint8_t green, uint8_t blue) {
    PrVtermColor color = {.kind = PrVtermColorKind_Rgb, .red = red, .green = green, .blue = blue};
    return color;
}

// The palette entry of a palette color.
PR_CONSTEXPR uint8_t prVtermColorIndex(PrVtermColor color) {
    return color.red;
}

// The attributes of a cell, and its part in a cluster that is two cells wide. A wide cluster is held in its first cell
// (Wide). The next cell is the cluster's tail (WideTail) and holds nothing. If a wide cluster does not fit at the end
// of a line, it wraps to the next line and leaves a spacer (WideSpacer) in the last cell. A spacer holds nothing and is
// not text.
enum {
    PrVtermCellFlag_Bold = 1u << 0,
    PrVtermCellFlag_Faint = 1u << 1,
    PrVtermCellFlag_Italic = 1u << 2,
    PrVtermCellFlag_Blink = 1u << 3,
    PrVtermCellFlag_Inverse = 1u << 4,      // swaps the foreground and the background
    PrVtermCellFlag_Invisible = 1u << 5,    // hides the text but not its background
    PrVtermCellFlag_Strikethrough = 1u << 6,
    PrVtermCellFlag_Overline = 1u << 7,
    PrVtermCellFlag_Wide = 1u << 8,
    PrVtermCellFlag_WideTail = 1u << 9,
    PrVtermCellFlag_WideSpacer = 1u << 10,
    PrVtermCellFlag_WidePlacement = PrVtermCellFlag_Wide | PrVtermCellFlag_WideTail | PrVtermCellFlag_WideSpacer,
};
typedef uint16_t PrVtermCellFlags;

// The line under a cell's text.
enum {
    PrVtermUnderline_None,
    PrVtermUnderline_Single,
    PrVtermUnderline_Double,
    PrVtermUnderline_Curly,
    PrVtermUnderline_Dotted,
    PrVtermUnderline_Dashed,
};
typedef uint8_t PrVtermUnderline;

// The most code points that a cluster holds after its first. Further code points are dropped.
enum { kPrVtermClusterMaxCodePoints = 31 };

// The index of a hyperlink (OSC 8) among the terminal's links, or kPrVtermLinkNone.
typedef uint16_t PrVtermLinkIndex;
enum { kPrVtermLinkNone = 0 };

// One cell of a line. The cluster's fields and the underline share a word, which keeps a cell at 24 bytes.
typedef struct PrVtermCell {
    // The first code point of the cell's grapheme cluster, or zero if the cell holds nothing and shows as a space. The
    // cluster's other code points are the `clusterCount` entries of the line's `codePoints` from `clusterStart`.
    uint32_t codePoint : 21;
    uint32_t clusterCount : 5;
    uint32_t underline : 3;         // a PrVtermUnderline
    uint32_t clusterStart;

    PrVtermCellFlags flags;

    // The hyperlink that the cell belongs to. All cells with the same link form one hyperlink, wherever they are. The
    // index stays the same while any cell holds it.
    PrVtermLinkIndex link;

    PrVtermColor foreground;
    PrVtermColor background;
    PrVtermColor underlineColor;    // Default to use the foreground color
} PrVtermCell;

// ---------------------------------------------------------------------------------------------------------------------
// Terminals

// A terminal: its screens, the state of its parser, and its output to the program.
typedef struct PrVterm PrVterm;

// The most lines of scrollback that a terminal keeps.
enum { kPrVtermMaxScrollback = 1000000 };

// How the terminal sums the cells of a rectangle for a checksum request (DECRQCRA), as xterm's checksumExtension
// resource selects. With no flags, the sum is the one that DEC's VT520 computes: the negated sum of the 8-bit code and
// the attributes of each written cell. A program selects other flags with XTCHECKSUM, until a reset restores the
// terminal's own.
enum {
    PrVtermChecksumFlag_Positive = 1u << 0,         // the sum is not negated
    PrVtermChecksumFlag_NoAttributes = 1u << 1,     // the cells' attributes are not summed

    // Every cell counts, and a cell that holds nothing counts as a space. Without CodePoints, the code points of a
    // cluster after its first are summed too.
    PrVtermChecksumFlag_Untrimmed = 1u << 2,

    // A cell that holds nothing reads as a space instead of being skipped. Without Untrimmed, such a space counts only
    // if it is the rectangle's first cell or has attributes.
    PrVtermChecksumFlag_Unwritten = 1u << 3,

    PrVtermChecksumFlag_CodePoints = 1u << 4,       // a cell adds its first code point, not an 8-bit code

    // The characters alone: the positive sum of each cell's first code point, without attributes, with a space for a
    // cell that holds nothing. xterm's patches 334 and 335 summed this, and its checksumExtension of 23 still does.
    PrVtermChecksumFlag_Characters = PrVtermChecksumFlag_Positive | PrVtermChecksumFlag_NoAttributes |
        PrVtermChecksumFlag_Untrimmed | PrVtermChecksumFlag_CodePoints,

    PrVtermChecksumFlag_All = PrVtermChecksumFlag_Characters | PrVtermChecksumFlag_Unwritten,
};
typedef uint8_t PrVtermChecksumFlags;

// A budget for the image memory of a terminal that shows images, in bytes (PrVtermDesc::imageBytes).
PR_CONSTANT uint64_t kPrVtermImageBytesDefault = 320ull << 20;

// The ways a program may send images of the graphics protocol, besides inline in its escape codes.
enum {
    // The program may name a file or a temporary file on this machine, which the terminal reads. Such a name is useful
    // only from a program that shares the caller's machine and file system. A caller that relays a remote connection or
    // a recording leaves this flag out.
    PrVtermImageFlag_Files = 1u << 0,
};
typedef uint8_t PrVtermImageFlags;

// What a terminal starts with.
typedef struct PrVtermDesc {
    PrUVec2 extent;                // the screen's columns and rows, each at least one
    uint32_t scrollback;           // the most lines of scrollback to keep, up to kPrVtermMaxScrollback
    PrVtermChecksumFlags checksum; // the checksum flags, which a reset restores

    // The images of the graphics protocol (pr/vterm_graphics.h). `imageBytes` limits the decoded pixels of the images
    // that both screens hold, in bytes. Zero ignores the protocol's commands, as a terminal without the protocol does.
    // kPrVtermImageBytesDefault is a budget for a terminal that shows images.
    uint64_t imageBytes;
    PrVtermImageFlags imageFlags;

    // The codecs that decompress compressed images and decode PNG images. If a codec is null, the terminal refuses the
    // images that need it. Each codec outlives the terminal.
    PrCompressCodec* PR_NULLABLE compressCodec;
    PrImageCodec* PR_NULLABLE imageCodec;
} PrVtermDesc;

// Creates a terminal with an empty screen and the default palette. prVtermDestroy frees it.
PrVterm* prVtermCreate(const PrVtermDesc* desc);
void prVtermDestroy(PrVterm* PR_NULLABLE vterm);

// Parses `bytes` that the program wrote, and applies them to the screen. If `bytes` ends in the middle of a sequence,
// the sequence continues in the next write. The parser ignores what it does not recognize, as a terminal does.
void prVtermWrite(PrVterm* vterm, PrByteView bytes);

// Resizes the screen to `extent`, whose columns and rows are each at least one. A new width rewraps the lines of the
// primary screen and its scrollback, and the cursor stays on the same text. The screen's top row stays on the line that
// it showed. It moves only as far as the cursor needs to stay on the screen, and the rows above it move to the
// scrollback. If the cursor is on the bottom row, it stays there instead, so that a taller screen brings rows back from
// the scrollback. The alternate screen is cut or filled, not rewrapped.
void prVtermResize(PrVterm* vterm, PrUVec2 extent);

// The screen's columns and rows.
PrUVec2 prVtermExtent(const PrVterm* vterm);

// Returns the terminal to its state at creation, as a program's full reset (RIS) does. Both empty the scrollback. This
// call also drops the output, the events, the notifications and the clipboard text, which a program's reset leaves for
// the caller. The palette and the roles return to the colors that the caller set last.
void prVtermReset(PrVterm* vterm);

// A number that grows whenever the screen, the cursor, a mode or a property of the terminal changes. While it stays the
// same, a caller need not redraw.
uint64_t prVtermRevision(const PrVterm* vterm);

// A number that changes whenever the colors that cells resolve to change while the cells do not: a palette entry, a
// role, or reverse video. A renderer that caches its work for a line by the line's revision also checks this number.
uint64_t prVtermColorRevision(const PrVterm* vterm);

// ---------------------------------------------------------------------------------------------------------------------
// Lines

// How a line continues.
enum {
    PrVtermLineFlag_Wrapped = 1u << 0,      // the text ran past the line's end and continues on the next line
};
typedef uint8_t PrVtermLineFlags;

// The marks that shell integration puts on a line with the semantic prompts of FinalTerm (OSC 133). They tell the
// shell's prompts, command input and command output apart. A line keeps its marks as long as it keeps its text: they
// scroll into the scrollback and rewrap with the line. Erasing the whole line, or a full reset, clears them.
enum {
    PrVtermLineMarkFlag_Prompt = 1u << 0,       // a prompt starts on the line, and with it a command
    PrVtermLineMarkFlag_Continuation = 1u << 1, // a secondary prompt, which continues the previous command
    PrVtermLineMarkFlag_Input = 1u << 2,        // the command's input starts on the line, at `inputColumn`
    PrVtermLineMarkFlag_Output = 1u << 3,       // the command's output starts on the line
    PrVtermLineMarkFlag_End = 1u << 4,          // the command finished on the line
};
typedef uint8_t PrVtermLineMarkFlags;

// What became of a command.
enum {
    PrVtermCommandStatus_None,          // no command ran, or none ran yet
    PrVtermCommandStatus_Running,       // the command's output started, and the command has not finished
    PrVtermCommandStatus_Succeeded,     // the command finished with an exit code of zero
    PrVtermCommandStatus_Failed,        // the command finished with a nonzero exit code
    PrVtermCommandStatus_Finished,      // the command finished, and the shell reported no exit code
};
typedef uint8_t PrVtermCommandStatus;

// A line of the scrollback or the screen.
typedef struct PrVtermLine {
    // The line's cells, by column. A row of the screen has a cell for each column. A line of the scrollback stores its
    // cells only up to the last one that shows something, so it may have fewer cells, or none. Each column past them,
    // up to the screen's width, reads as a cell of all zeros: blank, with the default colors.
    const PrVtermCell* PR_NULLABLE cells;
    uint32_t cellCount;

    const uint32_t* PR_NULLABLE codePoints;   // the code points of each cluster after its first
    PrVtermLineFlags flags;
    PrVtermLineMarkFlags marks;
    uint32_t inputColumn;               // the column where the command's input starts, if `marks` has Input

    // The status of the command whose prompt starts on the line, and its exit code if it succeeded or failed. A line
    // without the Prompt mark has the status None.
    PrVtermCommandStatus commandStatus;
    int32_t exitCode;

    // A number that changes whenever the line's cells or marks change. No other line has the same number, so a renderer
    // caches its work for the line by it. A line keeps its revision when it scrolls.
    uint64_t revision;
} PrVtermLine;

// The number of lines in the scrollback. They come before the screen's rows.
uint32_t prVtermScrollbackCount(const PrVterm* vterm);

// The number of lines that have left the scrollback since the terminal was created. A line leaves if it is the oldest
// past the limit, if a reset empties it, or if a resize to another width rewraps it. Adding this count to an index of
// prVtermLine gives the line a number that stays the same for as long as the line exists, and that no other line ever
// has.
uint64_t prVtermScrollbackDropped(const PrVterm* vterm);

// The memory that the lines of both screens and the scrollback hold, including the cache of prVtermLine, in bytes. It
// counts the capacity of their storage.
size_t prVtermLinesMemorySize(const PrVterm* vterm);

// The line at `index`. Indices run through the scrollback, oldest first, and then the rows of the screen that shows,
// from the top. `index` is less than prVtermScrollbackCount plus the screen's rows.
//
// The cells of a row of the screen are valid until the terminal changes. The scrollback stores its lines packed. A read
// unpacks a line of the scrollback into a cache that has one entry for each row of the screen. Its cells are valid
// until the terminal changes, or until a read of another line of the scrollback reuses their entry. Lines whose indices
// differ by less than the screen's height never share an entry.
//
// A read of the scrollback writes the cache, even through a const terminal. Reads of one terminal on two threads race.
PrVtermLine prVtermLine(const PrVterm* vterm, uint32_t index);

// Appends the text of line `index` to `out` in UTF-8, from column `start` up to column `end`, excluded. `end` is
// clamped to the screen's width. A cell that holds nothing is a space, as is each column past the line's cells. The
// tail of a wide cluster and a spacer add nothing. A line of the scrollback is read from its packed text, without the
// cache of prVtermLine.
void prVtermLineAppendText(const PrVterm* vterm, uint32_t index, uint32_t start, uint32_t end, PrRawString* out);

// ---------------------------------------------------------------------------------------------------------------------
// Cursor and modes

// How the cursor is drawn.
enum {
    PrVtermCursorShape_Block,
    PrVtermCursorShape_Underline,
    PrVtermCursorShape_Bar,
};
typedef uint8_t PrVtermCursorShape;

// The cursor.
typedef struct PrVtermCursor {
    PrUVec2 position;           // the column and the row on the screen
    PrVtermCursorShape shape;
    bool visible;
    bool blinking;
} PrVtermCursor;

// The cursor of the screen that shows.
PrVtermCursor prVtermCursor(const PrVterm* vterm);

// The modes, set by the program, that change how the caller shows the terminal or reads its input.
enum {
    PrVtermModeFlag_AlternateScreen = 1u << 0,  // the alternate screen shows
    PrVtermModeFlag_ReverseVideo = 1u << 1,     // the default foreground and background of the whole screen swap
    PrVtermModeFlag_BracketedPaste = 1u << 2,
    PrVtermModeFlag_FocusReports = 1u << 3,
    PrVtermModeFlag_MouseReports = 1u << 4,     // the program reads the mouse, so a click does not select text

    // The program is drawing a frame. The caller shows the frame once the mode ends, or after a timeout of its own if
    // the mode never ends.
    PrVtermModeFlag_Synchronized = 1u << 5,
};
typedef uint8_t PrVtermModeFlags;

// The modes that are set.
PrVtermModeFlags prVtermModes(const PrVterm* vterm);

// ---------------------------------------------------------------------------------------------------------------------
// Properties and events

// The requests from the program for the caller to act on, since the caller last took them.
enum {
    PrVtermEventFlag_Bell = 1u << 0,
    PrVtermEventFlag_Title = 1u << 1,               // the title changed
    PrVtermEventFlag_WorkingDirectory = 1u << 2,    // the working directory changed
    PrVtermEventFlag_Clipboard = 1u << 3,           // the program set the clipboard to prVtermClipboard
    PrVtermEventFlag_Notification = 1u << 4,        // the program posted a notification (prVtermTakeNotifications)
};
typedef uint8_t PrVtermEventFlags;

// Returns the events since the last call, and clears them.
PrVtermEventFlags prVtermTakeEvents(PrVterm* vterm);

// The title that the program set for its window, or the empty view. It is valid until the terminal changes.
PrStringView prVtermTitle(const PrVterm* vterm);

// The URL of the working directory that the program reported, or the empty view. It is valid until the terminal
// changes.
PrStringView prVtermWorkingDirectory(const PrVterm* vterm);

// The text that the program last asked to put on the clipboard, or the empty view. It is valid until the terminal
// changes. A program cannot read the clipboard.
PrStringView prVtermClipboard(const PrVterm* vterm);

// ---------------------------------------------------------------------------------------------------------------------
// Hyperlinks
//
// A program puts a hyperlink on the text that it prints with OSC 8. `OSC 8 ; params ; URI ST` opens a link to the URI,
// and the cells that the program prints next take that link. An empty URI ends the link. The params are colon-separated
// `key=value` pairs, and the terminal reads only `id`. Cells with the same id and URI form one link, so that a link
// stays whole across the lines that it wraps to and across a program's redraws. A link opened without an id is a link
// of its own.
//
// The open link stays open through SGR and through saving and restoring the cursor. A soft or full reset ends it. A
// cell keeps its link as it scrolls, rewraps and moves. An erase or a write over the cell drops the link. The terminal
// frees a link once no cell holds it.

// The most links that a terminal holds at once, and the most bytes in a link's URI and in its id. A URI or an id that
// is longer, or that has a byte outside printable ASCII, opens no link. An id may not hold a space either. A link past
// the most opens no link.
enum {
    kPrVtermMaxLinks = 65535,
    kPrVtermLinkMaxUriBytes = 4096,
    kPrVtermLinkMaxIdBytes = 256,
};

// A hyperlink: its URI, and the id that the program gave it, or the empty view if the link was opened without an id.
typedef struct PrVtermLink {
    PrStringView uri;
    PrStringView id;
} PrVtermLink;

// The link with index `link`, which a cell of the terminal holds. Its text is valid until the terminal changes.
PrVtermLink prVtermLink(const PrVterm* vterm, PrVtermLinkIndex link);

// ---------------------------------------------------------------------------------------------------------------------
// Notifications
//
// A program posts a desktop notification for the caller to show with one of three operating system commands.
//
// - OSC 9 (iTerm2's) gives the body's text. The terminal ignores the forms of OSC 9 that begin with a number from 1 to
//   12, which are ConEmu's other commands.
// - OSC 777 (rxvt-unicode's) gives `notify`, a title and a body.
// - OSC 99 (kitty's desktop notification protocol) gives metadata keys and a payload. The terminal reads five keys. `i`
//   is an identifier that the chunks of one notification share. `d=0` marks a chunk that more chunks follow. `p` says
//   whether the payload is the title (the default), the body, or `?`, a query of what the terminal supports, which the
//   terminal answers. `e=1` marks a payload in base64, with each chunk encoded on its own. `o` is the condition. The
//   terminal ignores the other keys, and payloads of other kinds. A chunk with another identifier drops the
//   notification whose chunks were arriving.

// When the program wants a notification to show, by what the user can see of the terminal.
enum {
    PrVtermNotificationCondition_None,          // the program named no condition, and the caller chooses
    PrVtermNotificationCondition_Always,
    PrVtermNotificationCondition_Unfocused,     // unless the terminal has keyboard focus in the active window
    PrVtermNotificationCondition_Invisible,     // unless the terminal is visible in the active window
};
typedef uint8_t PrVtermNotificationCondition;

// The most notifications that wait for the caller, and the most bytes in a notification's title and in its body. A
// notification past the most drops the oldest. A longer title or body is cut at a code point boundary.
enum {
    kPrVtermMaxNotifications = 16,
    kPrVtermNotificationMaxTitleBytes = 256,
    kPrVtermNotificationMaxBodyBytes = 2048,
};

// A notification that the program posted. Its title and body are UTF-8 without control characters, except for line
// feeds in the body. Either one may be empty, but not both.
typedef struct PrVtermNotification {
    PrStringView title;
    PrStringView body;
    PrVtermNotificationCondition condition;
} PrVtermNotification;

// Takes the notifications that the program posted since the last take, oldest first. Writes up to `capacity` of them to
// `out`, keeps the rest, and returns how many it wrote. Their text is valid until the next take, a call to
// prVtermReset, or the terminal's destruction.
uint32_t prVtermTakeNotifications(PrVterm* vterm, PrVtermNotification* PR_NULLABLE out, uint32_t capacity);

// ---------------------------------------------------------------------------------------------------------------------
// Shell integration

// What the shell is doing, by the last semantic prompt mark (OSC 133) that it wrote to the primary screen.
enum {
    PrVtermShellState_None,         // no command is under way: the shell wrote no mark, or a command finished
    PrVtermShellState_Prompt,       // the shell is writing its prompt
    PrVtermShellState_Input,        // the user is typing a command
    PrVtermShellState_Running,      // a command is running and writing its output
};
typedef uint8_t PrVtermShellState;

// The state of the shell, and the result of the last command that finished.
typedef struct PrVtermShellInfo {
    PrVtermShellState state;
    PrVtermCommandStatus lastStatus;    // None until a command finishes
    int32_t lastExitCode;               // if `lastStatus` is Succeeded or Failed
} PrVtermShellInfo;

// The state of the shell. A full reset returns it to None.
PrVtermShellInfo prVtermShellInfo(const PrVterm* vterm);

// ---------------------------------------------------------------------------------------------------------------------
// Colors

// The number of entries in the terminal's palette.
enum {
    kPrVtermPaletteCount = 256,
};

// The roles that a Default color stands for.
enum {
    PrVtermColorRole_Foreground,
    PrVtermColorRole_Background,
    PrVtermColorRole_Cursor,
};
typedef uint8_t PrVtermColorRole;

// The color of palette entry `index`, which is below kPrVtermPaletteCount, or of a role. The program may change either.
// A reset restores the colors that the caller set last.
PrColor8 prVtermPaletteColor(const PrVterm* vterm, uint32_t index);
PrColor8 prVtermRoleColor(const PrVterm* vterm, PrVtermColorRole role);

// Sets the color of palette entry `index`, or of a role, both now and as a reset restores it. The palette starts with
// the 16 named colors, followed by a cube of 216 colors and a ramp of 24 grays. A theme rarely changes the cube or the
// ramp.
void prVtermPaletteSetColor(PrVterm* vterm, uint32_t index, PrColor8 color);
void prVtermRoleSetColor(PrVterm* vterm, PrVtermColorRole role, PrColor8 color);

// The color that `color` resolves to in `role`, through the palette and the colors of the roles.
PrColor8 prVtermColorResolve(const PrVterm* vterm, PrVtermColor color, PrVtermColorRole role);

// The color scheme of the caller's theme. The program can query it, and can ask to hear whenever it changes. A terminal
// starts dark, to match its default roles. The program cannot change the scheme, so a reset keeps it.
PrAppColorScheme prVtermColorScheme(const PrVterm* vterm);
void prVtermSetColorScheme(PrVterm* vterm, PrAppColorScheme scheme);

// ---------------------------------------------------------------------------------------------------------------------
// Output

// The bytes that the terminal queued for the program: its answers to the program's queries and the user's input. The
// view is valid until the terminal changes.
PrByteView prVtermOutput(const PrVterm* vterm);

// Removes the first `size` bytes of the output, once the caller has sent them.
void prVtermOutputConsume(PrVterm* vterm, size_t size);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
