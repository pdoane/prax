// The state of a terminal, and the screen operations that the control functions perform.

#pragma once

#include "pr/base.h"
#include "pr/raw_array.h"
#include "pr/raw_string.h"
#include "pr/unicode_grapheme.h"
#include "pr/vterm.h"
#include "pr/vterm_input.h"

#include "vterm/src/vterm_graphics.h"
#include "vterm/src/vterm_link.h"
#include "vterm/src/vterm_notification.h"
#include "vterm/src/vterm_parser.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// A row of a screen: a line of cells, one for each column. Lines of the scrollback are packed instead
// (VtermPackedLine).
struct VtermStoredLine {
    RawArray<PrVtermCell> cells;        // by column
    RawArray<uint32_t> codePoints;      // each cell's cluster after its first code point
    uint64_t revision;
    PrVtermLineFlags flags;

    // The marks of shell integration, as PrVtermLine has them.
    PrVtermLineMarkFlags marks;
    PrVtermCommandStatus commandStatus;
    uint32_t inputColumn;
    int32_t exitCode;

    // A bound on the written cells: every cell from this column on is all zeros. A write raises the bound past every
    // cell that it leaves nonzero. A clear needs to zero only the cells before the bound, and a search for the last
    // cell that shows something starts from it.
    uint32_t written;

    // The links that the cells held when the last sweep of the links read them, at revision `linksRevision`. They are
    // in cell order, with repeats.
    RawArray<PrVtermLinkIndex> links;
    uint64_t linksRevision;
};

// The rows of a screen, in a ring with one slot for each row. A scroll of the whole screen rotates the ring.
struct VtermRows {
    RawArray<VtermStoredLine> slots;
    uint32_t start;             // the slot of the top row
};

// ---------------------------------------------------------------------------------------------------------------------
// The scrollback
//
// A line that leaves the top of the primary screen for the scrollback is packed. Its cells are written once, in order,
// to the end of an arena of blocks, and the scrollback's ring keeps a 24-byte record of the line (VtermPackedLine). The
// arena frees its blocks oldest first, as the ring drops its oldest lines past the limit. Storing a line is therefore
// one sequential append, and dropping a line frees nothing until a whole block is dead.
//
// A packed line holds its cells up to the last cell that is not all zeros. A blank cell in the default colors is all
// zeros. The cells past the end read as such blanks, so packing also trims the line. A packed line holds these parts,
// in order:
//
// - The shell integration marks, if the record has VtermPackedFlag_Marks: the marks and the command's status, one
//   byte each, then the input column and the exit code, 4 bytes each in host byte order. They come first and have a
//   fixed size, so that the end of a command can update the line of its prompt in place.
// - The counts, each an unsigned LEB128: the number of cells, the bytes of text, the bytes of style runs, and, if the
//   record has VtermPackedFlag_Links, the bytes of link runs.
// - The text. Each cell that is not the tail of a wide cluster or a spacer contributes its code points in UTF-8, or a
//   zero byte if it holds nothing. A reader of text maps the zero byte to a space. Unless the line is shaped, a reader
//   of text needs no other part of the line.
// - The shapes, if the record has VtermPackedFlag_Shaped: one byte for each cell. The low 5 bits count the code points
//   of the cell's cluster after its first. Bits 5, 6 and 7 are its Wide, WideTail and WideSpacer flags. A line without
//   shapes has one code point in each cell and none of these flags. A tail or a spacer holds nothing.
// - The style runs. Each run of cells with one pen stores its cell count and its pen, each as a LEB128. The pen is an
//   index into the pens of the line's block, counted from 1, and 0 is the pen of all zeros. The runs cover every cell,
//   or there are none if every cell has the pen of all zeros.
// - The link runs. Each run of cells with one link stores the number of cells since the previous run, its cell count
//   and its link, each as a LEB128. Cells outside the runs hold no link.
//
// The record holds the line's revision, its line flags and its packing flags. Once a line is packed, only the command's
// status and exit code can change, and a change gives the line a new revision. A line keeps its line number however it
// is stored, and images anchor to that number. The number is prVtermScrollbackDropped plus the line's index.
//
// Packing reads a row's cells once, in order. An ASCII cell in the same style as the cell before it costs packing only
// its byte. A reader of cells reads a packed line through the terminal's cache of unpacked lines (VtermLineCache). A
// reader of text reads the packed text directly. A rewrap slices a logical line of packed ASCII with no links, marks or
// shapes at the new width, from its text and its runs. It unpacks any other line into cells and packs it again. A
// resize unpacks the lines that it brings back to the screen into rows.

// The attributes and colors of a run of cells in a packed line: a cell's style, without its link or its place in a wide
// cluster. The bytes of a color that its kind does not use are zeros, so that pens compare and hash as bytes.
PR_NO_PADDING_BEGIN
struct VtermPackedPen {
    PrVtermCellFlags flags;     // without PrVtermCellFlag_WidePlacement
    PrVtermUnderline underline;
    uint8_t _pad;
    PrVtermColor foreground;
    PrVtermColor background;
    PrVtermColor underlineColor;
};
PR_NO_PADDING_END

// A block of an arena: the bytes of packed lines, appended in order, and the pens that their runs index.
struct VtermArenaBlock {
    uint8_t* data;
    uint32_t size;
    uint32_t used;
    RawArray<VtermPackedPen> pens;      // by pen, from 1
};

// The size of a standard block of an arena, in bytes. A packed line that needs more gets a block of its own.
constexpr uint32_t kVtermArenaBlockBytes = 64u << 10;

// Where a packed line starts: the number that the arena gave its block, and its offset in the block. A line in a
// standard block starts within kVtermArenaBlockBytes, so the offset fits in 16 bits. A larger line starts its own
// block.
struct VtermArenaPosition {
    uint32_t block;
    uint16_t offset;
};

// The number of sets in the arena's table of recent pens, and the number of pens (ways) in each set.
constexpr uint32_t kVtermArenaPenSets = 256;
constexpr uint32_t kVtermArenaPenWays = 2;

// The storage of packed lines: a ring of blocks, oldest first. Lines are appended to the newest block, and blocks are
// freed oldest first. The arena numbers its blocks in the order that it makes them, so a position names the same block
// for as long as the block lives. Pens are added to the newest block, so a block's pens are freed with it, and their
// count is bounded by its runs. A table of recently added pens finds a pen that the block already has. A block
// therefore holds each pen of its lines once, unless more pens than the table holds alternate.
struct VtermArena {
    RawArray<VtermArenaBlock> slots;
    uint32_t start;             // the slot of the oldest block
    uint32_t count;
    uint32_t first;             // the number of the oldest block
    uint8_t* PR_NULLABLE spare; // the storage of a freed standard block, for the next block to reuse

    // The table of recent pens, by a set from the pen's hash and then by way, newest first. `lastPen` is the pen that
    // the arena found or added last, which lets a repeated pen skip the hash. Both hold indices among the newest
    // block's pens, counted from 1. An index past the block's pens, or one whose pen differs, holds no pen.
    uint32_t recentPens[kVtermArenaPenSets][kVtermArenaPenWays];
    uint32_t lastPen;
};

// The flags of a line's packing.
enum {
    VtermPackedFlag_Marks = 1u << 0,    // the line has marks of shell integration
    VtermPackedFlag_Links = 1u << 1,    // a cell holds a link
    VtermPackedFlag_Shaped = 1u << 2,   // a cell holds a cluster or is part of a wide cluster
};
typedef uint8_t VtermPackedFlags;

// The record of a scrollback line. The line's packed bytes are in the scrollback's arena.
struct VtermPackedLine {
    uint64_t revision;
    VtermArenaPosition position;
    PrVtermLineFlags flags;
    VtermPackedFlags packed;
};

// A packed line's counts and where its parts start.
struct VtermPackedView {
    uint8_t* PR_NULLABLE marks;
    uint32_t cellCount;
    uint32_t textBytes;
    uint32_t runBytes;
    uint32_t linkBytes;
    const uint8_t* text;
    const uint8_t* PR_NULLABLE shapes;
    const uint8_t* runs;
    const uint8_t* links;
    const VtermPackedPen* PR_NULLABLE pens;  // the pens of the line's block, by pen - 1
};

// A run of cells with one pen or one link, as packing collects it before writing it.
struct VtermPackedRun {
    uint32_t start;             // the run's first column
    uint32_t count;
    uint32_t index;             // the pen's index among the pens of the line's block, once interned
    VtermPackedPen pen;
    PrVtermLinkIndex link;
};

// The scrollback of the primary screen: a ring of packed lines, and the arena that holds their bytes. The ring grows to
// `limit` lines, and then drops its oldest line for each line that it adds.
struct VtermScrollback {
    RawArray<VtermPackedLine> slots;
    uint32_t start;             // the slot of the oldest line
    uint32_t count;
    uint32_t limit;
    VtermArena arena;

    // Scratch storage for packing a line, reused from line to line.
    RawArray<uint8_t> text;
    RawArray<uint8_t> shapes;
    RawArray<VtermPackedRun> runs;
    RawArray<VtermPackedRun> linkRuns;
};

// A line of the scrollback, unpacked into cells for a reader that reads cells.
struct VtermLineCacheEntry {
    uint64_t revision;          // the revision of the line that the entry holds, or zero for none
    RawArray<PrVtermCell> cells;
    RawArray<uint32_t> codePoints;
};

// Unpacked scrollback lines for readers that read cells. A line's entry is its line number modulo the screen's rows, so
// a view as tall as the screen fits in the cache at once. An entry identifies its line by revision, and no two lines
// share a revision. An entry therefore stays valid while its line keeps that revision, and needs no invalidation.
struct VtermLineCache {
    RawArray<VtermLineCacheEntry> entries;
};

// The character sets that a terminal maps ASCII through.
enum {
    VtermCharset_Ascii,
    VtermCharset_DecSpecial,    // the DEC Special Graphics set, of line drawing
    VtermCharset_British,       // ASCII with a pound sign for `#`
};
typedef uint8_t VtermCharset;

// The attributes and colors that new text is written with.
struct VtermPen {
    PrVtermCellFlags flags;
    PrVtermUnderline underline;
    PrVtermColor foreground;
    PrVtermColor background;
    PrVtermColor underlineColor;
};

// The cursor and what DECSC saves with it.
struct VtermCursorState {
    uint32_t column;
    uint32_t row;
    bool wrapPending;           // the last column was written, so the next character wraps first
    bool originMode;            // positions count from the scroll region's top left corner, and stay in the region
    uint8_t shift;              // the set of `charsets` that GL invokes
    VtermCharset charsets[4];   // G0 to G3
    VtermPen pen;
};

// The mouse events that the program asked to be reported.
enum {
    VtermMouseTracking_None,
    VtermMouseTracking_Press,   // X10: presses alone
    VtermMouseTracking_Click,   // presses and releases
    VtermMouseTracking_Drag,    // and moves while a button is held
    VtermMouseTracking_Any,     // and every move
};
typedef uint8_t VtermMouseTracking;

// How mouse events are reported.
enum {
    VtermMouseEncoding_Default,     // bytes offset by 32, which reach column 223 at most
    VtermMouseEncoding_Utf8,        // those values as UTF-8
    VtermMouseEncoding_Sgr,         // decimal parameters of a control sequence
    VtermMouseEncoding_Urxvt,       // decimal parameters, offset as the default's are
    VtermMouseEncoding_SgrPixels,   // the SGR form, in pixels
};
typedef uint8_t VtermMouseEncoding;

// The most entries of the stack of keyboard modes, and of saved titles.
constexpr uint32_t kVtermKeyboardStackMax = 8;
constexpr uint32_t kVtermTitleStackMax = 10;

// The longest text of a key, in bytes, whose Text event the terminal waits to drop.
constexpr uint32_t kVtermDropTextMax = 16;

// The flags of kitty's progressive keyboard enhancement protocol, all of which the terminal supports.
constexpr uint8_t kVtermKeyboardDisambiguate = 1;
constexpr uint8_t kVtermKeyboardEvents = 2;
constexpr uint8_t kVtermKeyboardAlternates = 4;
constexpr uint8_t kVtermKeyboardAllKeys = 8;
constexpr uint8_t kVtermKeyboardText = 16;
constexpr uint8_t kVtermKeyboardSupported = kVtermKeyboardDisambiguate | kVtermKeyboardEvents |
    kVtermKeyboardAlternates | kVtermKeyboardAllKeys | kVtermKeyboardText;

// A screen: its rows, the cursor state that DECSC saved, and its stack of keyboard modes. The primary screen's
// scrollback belongs to the terminal.
struct VtermScreen {
    VtermRows rows;
    VtermCursorState saved;
    bool hasSaved;

    // The stack of keyboard modes, whose top is the mode in effect.
    uint8_t keyboard[kVtermKeyboardStackMax];
    uint32_t keyboardCount;
};

// The screens of a terminal.
enum {
    VtermScreenKind_Primary,
    VtermScreenKind_Alternate,
};
typedef uint8_t VtermScreenKind;

// The modes of a terminal that a program sets.
struct VtermModes {
    bool insert;                // IRM: text moves the cells after it right
    bool newline;               // LNM: a line feed also returns, and Enter sends CR LF
    bool autowrap;              // DECAWM
    bool cursorKeys;            // DECCKM: the cursor keys send their application sequences
    bool reverseVideo;          // DECSCNM
    bool cursorVisible;         // DECTCEM
    bool cursorBlinking;
    bool bracketedPaste;
    bool focusReports;
    bool synchronized;
    bool alternateScroll;       // the wheel sends cursor keys while the alternate screen shows
    bool colorSchemeReports;    // a change of the color scheme is reported
    bool leftRightMargins;      // DECLRMM: DECSLRM sets the left and right margins
};

// A terminal.
struct Vterm {
    uint32_t columns;
    uint32_t rows;

    VtermScreen screens[2];         // by VtermScreenKind
    VtermScrollback scrollback;     // the lines that left the primary screen, up to the terminal's limit
    VtermScreenKind active;
    VtermCursorState cursor;
    VtermModes modes;
    PrVtermCursorShape cursorShape;

    // How a checksum of a rectangle sums its cells, and the flags that a reset restores.
    PrVtermChecksumFlags checksum;
    PrVtermChecksumFlags defaultChecksum;

    // The scroll region: the rows from `top` to `bottom`, excluded, and the columns from `left` to `right`, excluded.
    // The columns span the screen unless DECLRMM is set.
    uint32_t top;
    uint32_t bottom;
    uint32_t left;
    uint32_t right;

    RawArray<uint8_t> tabStops;     // by column: nonzero where a tab stops

    VtermMouseTracking mouseTracking;
    VtermMouseEncoding mouseEncoding;
    bool mouseMoved;                // a move was reported, at `mouseCell`
    PrUVec2 mouseCell;
    uint8_t pointerButtons;         // the buttons held, by bit of PrAppEvent's `button`
    PrVec2 scrollRemainder;         // the part of a line of scroll that no report sent yet, in lines

    // The text of the last key, if the key sent its own sequence. The Text event that follows the key is dropped if it
    // carries this text. Any other event ends the wait.
    char dropText[kVtermDropTextMax];
    uint8_t dropTextSize;

    // The cell that the last character was printed to, and that character. A combining character joins the cell, and
    // REP repeats the character. They are cleared when the cursor moves by any other means.
    bool hasLastPrinted;
    uint32_t lastPrintedRow;
    uint32_t lastPrintedColumn;
    uint32_t lastCodePoint;
    PrUnicodeGraphemeState lastCluster;     // grapheme segmentation at the end of the last printed cluster

    PrColor8 palette[kPrVtermPaletteCount];
    PrColor8 defaultPalette[kPrVtermPaletteCount];
    PrColor8 roles[3];              // by PrVtermColorRole
    PrColor8 defaultRoles[3];
    PrAppColorScheme colorScheme;   // the dark or light scheme that the caller set, which a program cannot change

    PrRawString title;
    PrRawString titles[kVtermTitleStackMax];
    uint32_t titleCount;
    PrRawString workingDirectory;
    PrRawString clipboard;
    VtermNotifications notifications;
    PrVtermEventFlags events;

    // The hyperlinks that cells hold, and the open link, which newly printed cells take. The open link is not part of
    // the pen, so SGR, DECSC and DECRC leave it unchanged.
    VtermLinks links;
    PrVtermLinkIndex link;

    // The state of shell integration. If `hasPrompt`, `promptLine` is the line number of the primary screen's line
    // where the current command's prompt starts. Line numbers include the dropped lines (prVtermScrollbackDropped).
    PrVtermShellInfo shell;
    bool hasPrompt;
    uint64_t promptLine;

    RawArray<uint8_t> output;

    uint64_t revision;              // the last revision given to the terminal or a line
    uint64_t colorRevision;         // the revision of the last change of the palette, the roles or reverse video
    uint64_t scrollbackDropped;
    VtermParser parser;

    // The images of the graphics protocol. They follow the state that every write reads.
    VtermGraphics graphics;

    // The cache of scrollback lines that readers read as cells. Reading a line fills the cache, so the cache changes
    // under a const terminal. That is safe because one thread uses a terminal at a time.
    mutable VtermLineCache lineCache;
};

// The rows of a screen

// Frees the rows' storage.
void vtermRowsReset(VtermRows* rows);

// The screen's row `row`, from the top.
VtermStoredLine& vtermRowsAt(VtermRows* rows, uint32_t row);
const VtermStoredLine& vtermRowsAt(const VtermRows* rows, uint32_t row);

// The arena of packed lines

// Frees the arena's blocks and leaves it empty.
void vtermArenaReset(VtermArena* arena);

// Returns room for `size` bytes at the end of the newest block, and writes the position of the room to `*position`.
// Starts a new block if the newest has no room. The bytes belong to a line once vtermArenaCommit marks them used.
uint8_t* vtermArenaReserve(VtermArena* arena, uint32_t size, VtermArenaPosition* position);

// Marks `size` bytes of the room that the last vtermArenaReserve returned as used. `size` is at most the size reserved.
void vtermArenaCommit(VtermArena* arena, uint32_t size);

// The bytes of the line that starts at `position`, and the block with the number `block`.
uint8_t* vtermArenaAt(const VtermArena* arena, VtermArenaPosition position);
const VtermArenaBlock& vtermArenaBlock(const VtermArena* arena, uint32_t block);

// Returns the index of `pen` among the newest block's pens, counted from 1, or 0 for the pen of all zeros. Adds the pen
// to the block unless the table of recent pens finds it.
uint32_t vtermArenaPen(VtermArena* arena, const VtermPackedPen& pen);

// Frees the blocks older than block `block`. Keeps the storage of one standard block for the next block to reuse.
void vtermArenaDropBefore(VtermArena* arena, uint32_t block);

// Drops the bytes from `position` on, and the blocks after its block.
void vtermArenaTruncate(VtermArena* arena, VtermArenaPosition position);

// The memory that the arena holds, in bytes, counting the capacity of its storage.
size_t vtermArenaMemorySize(const VtermArena* arena);

// The scrollback

// Frees the scrollback's lines and leaves it empty. Keeps its limit.
void vtermScrollbackReset(VtermScrollback* scrollback);

// The record of line `index`, counted from the oldest.
VtermPackedLine& vtermScrollbackAt(VtermScrollback* scrollback, uint32_t index);
const VtermPackedLine& vtermScrollbackAt(const VtermScrollback* scrollback, uint32_t index);

// Packs `line` after the newest line, with its revision, its flags and its marks. If the scrollback is at its limit,
// its oldest line is dropped first. `*dropped` is set to whether a line was dropped. The line's cells past its
// `written` bound must be all zeros, and a tail or a spacer of a wide cluster must hold nothing.
void vtermScrollbackPush(VtermScrollback* scrollback, const VtermStoredLine& line, bool* dropped);

// Removes the newest line, or the `count` oldest lines.
void vtermScrollbackPop(VtermScrollback* scrollback);
void vtermScrollbackDrop(VtermScrollback* scrollback, uint32_t count);

// The memory that the scrollback holds, in bytes, counting the capacity of its storage.
size_t vtermScrollbackMemorySize(const VtermScrollback* scrollback);

// The parts of the packed line `line`.
VtermPackedView vtermPackedView(const VtermScrollback* scrollback, const VtermPackedLine& line);

// Reads an unsigned LEB128 at `*at`, and moves `*at` past it.
uint32_t vtermPackedReadCount(const uint8_t* PR_NONNULL* PR_NONNULL at);

// Sets `*cells` to the cells of a packed line, padded with cells of all zeros to `count` cells, and `*codePoints` to
// the code points of their clusters. `count` is at least the line's cell count.
void vtermPackedCells(const VtermPackedView& view, uint32_t count, RawArray<PrVtermCell>* cells,
    RawArray<uint32_t>* codePoints);

// Appends to `out` the UTF-8 text of a packed line's cells from column `start` to `end`, excluded, as
// prVtermLineAppendText would read them. The range is clamped to the line's cells. Reads only the line's text, and its
// shapes if it has them.
void vtermPackedAppendText(const VtermPackedView& view, uint32_t start, uint32_t end, PrRawString* out);

// Unpacks line `index` of the scrollback into `line`, a row of `columns` cells: its cells, revision, flags and marks.
// The row's `written` bound is set to the end of the packed cells.
void vtermScrollbackUnpack(const VtermScrollback* scrollback, uint32_t index, uint32_t columns, VtermStoredLine* line);

// Output

// Queues bytes for the program: text, a byte, or a number in decimal.
void vtermPut(Vterm* vterm, PrStringView text);
void vtermPutByte(Vterm* vterm, uint8_t byte);
void vtermPutNumber(Vterm* vterm, uint32_t value);

// Lines as readers read them

// The number of lines of the scrollback and the screen that shows.
uint32_t vtermLineCount(const Vterm* vterm);

// Reads line `index` of the scrollback and the screen that shows (prVtermLine). A scrollback line is unpacked into the
// terminal's line cache. Its cells are valid until the terminal changes, or until a read of another line whose number
// differs by a multiple of the screen's rows.
PrVtermLine vtermLineRead(const Vterm* vterm, uint32_t index);

// Empties the line cache, and gives it one entry for each of the screen's rows.
void vtermLineCacheReset(Vterm* vterm);

// The screens

// A new revision, for the terminal or a line.
uint64_t vtermRevise(Vterm* vterm);

// The screen that shows, and its row `row`.
VtermScreen& vtermScreen(Vterm* vterm);
VtermStoredLine& vtermRow(Vterm* vterm, uint32_t row);

// Marks `line` changed.
void vtermTouch(Vterm* vterm, VtermStoredLine* line);

// Records that the cells of `line` before column `end` may not be all zeros (VtermStoredLine's `written`).
void vtermLineWrote(VtermStoredLine* line, uint32_t end);

// A cell that holds nothing, in the pen's background color. Erasing leaves such cells.
PrVtermCell vtermBlankCell(const Vterm* vterm);

// Sets the line's cells to `columns` blank cells of the pen's background, and clears its flags and marks.
void vtermLineClear(Vterm* vterm, VtermStoredLine* line, uint32_t columns);

// Clears the line's marks of shell integration, and the status of its command.
void vtermLineClearMarks(VtermStoredLine* line);

// Sets the status and exit code of the current command on its prompt's line, and gives that line a new revision. The
// line may be a row or a scrollback line. Does nothing if the line was dropped or lost its prompt mark.
void vtermPromptSetStatus(Vterm* vterm, PrVtermCommandStatus status, int32_t exitCode);

// Creates the screens of a terminal of `columns` and `rows`, with empty lines and the cursor at the top left.
void vtermScreensInit(Vterm* vterm);

// Frees the screens' storage.
void vtermScreensReset(Vterm* vterm);

// Returns true if `column` is in the scroll region's columns.
bool vtermColumnInMargins(const Vterm* vterm, uint32_t column);

// Prints `codePoint` at the cursor, after mapping it through the charset that GL invokes, and advances the cursor. A
// code point that continues the grapheme cluster of the last printed cell joins that cell. Text wraps to the left
// margin at the right margin, or at the screen's last column if the cursor is right of the margin. Only a wrap from the
// last column to the first marks the line wrapped, because only then does its text continue at the start of the next
// line.
void vtermPrint(Vterm* vterm, uint32_t codePoint);

// Prints the printable ASCII bytes of `text`, with the same result as calling vtermPrint for each. Writes each row's
// part of the run at once.
void vtermPrintAscii(Vterm* vterm, PrStringView text);

// Moves the cursor down a line, scrolling the region up at its bottom margin if the cursor is in the region's columns
// (IND).
void vtermIndex(Vterm* vterm);

// Moves the cursor up a line, scrolling the region down at its top margin if the cursor is in the region's columns
// (RI).
void vtermReverseIndex(Vterm* vterm);

// Moves the cursor to the left margin (CR). Outside origin mode, a cursor left of the margin moves to the first column
// instead.
void vtermCarriageReturn(Vterm* vterm);

// Scrolls the scroll region up or down by `count` lines. If the region is on the primary screen, starts at its top and
// spans its width, the lines that scroll off the top enter the scrollback.
void vtermScrollUp(Vterm* vterm, uint32_t count);
void vtermScrollDown(Vterm* vterm, uint32_t count);

// Sets the cells of row `row` from column `start` to `end`, excluded, to blank cells, splitting the wide clusters that
// the range cuts. An erase of the whole row clears its marks.
void vtermErase(Vterm* vterm, uint32_t row, uint32_t start, uint32_t end);

// Inserts or deletes `count` blank cells at the cursor, moving the cells after it in its row up to the right margin
// (ICH, DCH). The cursor must be in the region's columns, or nothing changes.
void vtermInsertCells(Vterm* vterm, uint32_t count);
void vtermDeleteCells(Vterm* vterm, uint32_t count);

// Inserts or deletes `count` lines at the cursor's row, within the scroll region, and moves the cursor to the left
// margin (IL, DL). The cursor must be in the region, or nothing changes.
void vtermInsertLines(Vterm* vterm, uint32_t count);
void vtermDeleteLines(Vterm* vterm, uint32_t count);

// Inserts or deletes `count` blank columns at `column`, in the scroll region's rows, moving the columns after it up to
// the right margin (DECIC, DECDC, and the scrolls at the margins). The cursor and `column` must be in the region, or
// nothing changes.
void vtermInsertColumns(Vterm* vterm, uint32_t column, uint32_t count);
void vtermDeleteColumns(Vterm* vterm, uint32_t column, uint32_t count);

// Sets the scroll region's columns to the screen's width.
void vtermMarginsReset(Vterm* vterm);

// Moves the cursor to `column` and `row` of the screen, clamped to it, and clears the pending wrap.
void vtermCursorTo(Vterm* vterm, uint32_t column, uint32_t row);

// Shows the alternate or the primary screen. Showing the alternate screen with `clear` empties it first.
void vtermScreenShow(Vterm* vterm, VtermScreenKind kind, bool clear);

// Resizes the screens to `columns` and `rows`, rewrapping the primary screen if the width changes.
void vtermScreensResize(Vterm* vterm, uint32_t columns, uint32_t rows);

// Empties the primary screen's scrollback.
void vtermScrollbackClear(Vterm* vterm);

// Returns the checksum of the screen's cells from row `top` to `bottom`, excluded, and from column `left` to `right`,
// excluded (DECRQCRA). The terminal's checksum flags say how cells are summed. The result is the low 16 bits of the
// sum.
uint16_t vtermChecksum(const Vterm* vterm, uint32_t top, uint32_t bottom, uint32_t left, uint32_t right);

} // namespace pr_impl

// C opaque types
struct PrVterm : pr_impl::Vterm {};

PR_ASSUME_NONNULL_END
