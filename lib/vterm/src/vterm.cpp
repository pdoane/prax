// Virtual terminals: the public functions, and the dispatch of the control functions that the parser reads.

#include "pr/vterm.h"

#include "pr/alloc.h"
#include "pr/assert.h"
#include "pr/base64.h"
#include "pr/integer.h"
#include "pr/mem.h"
#include "pr/utf8.h"

#include "vterm/src/vterm_parser.h"
#include "vterm/src/vterm_report.h"
#include "vterm/src/vterm_screen.h"

namespace pr_impl {

// Cells are most of a terminal's memory, and a write of text moves them.
static_assert(sizeof(PrVtermCell) == 24);

// The number of code points in a cluster fits its bit-field.
static_assert(kPrVtermClusterMaxCodePoints < 32);

namespace {

// The 16 named colors of the default palette, with xterm's values.
constexpr PrColor8 kNamedColors[16] = {
    {0x00, 0x00, 0x00, 0xff}, {0xcd, 0x00, 0x00, 0xff}, {0x00, 0xcd, 0x00, 0xff}, {0xcd, 0xcd, 0x00, 0xff},
    {0x00, 0x00, 0xee, 0xff}, {0xcd, 0x00, 0xcd, 0xff}, {0x00, 0xcd, 0xcd, 0xff}, {0xe5, 0xe5, 0xe5, 0xff},
    {0x7f, 0x7f, 0x7f, 0xff}, {0xff, 0x00, 0x00, 0xff}, {0x00, 0xff, 0x00, 0xff}, {0xff, 0xff, 0x00, 0xff},
    {0x5c, 0x5c, 0xff, 0xff}, {0xff, 0x00, 0xff, 0xff}, {0x00, 0xff, 0xff, 0xff}, {0xff, 0xff, 0xff, 0xff},
};

// The default colors of the roles, by PrVtermColorRole.
constexpr PrColor8 kRoleColors[3] = {{0xe5, 0xe5, 0xe5, 0xff}, {0x00, 0x00, 0x00, 0xff}, {0xe5, 0xe5, 0xe5, 0xff}};

// The terminal's answer to a request for its version (XTVERSION).
constexpr PrStringView kVersion = "prax-vterm";

// The default palette: the named colors, a cube of six levels of each channel, and a ramp of grays.
void paletteDefault(PrColor8 palette[kPrVtermPaletteCount]) {
    for (uint32_t i = 0; i < 16; ++i)
        palette[i] = kNamedColors[i];
    constexpr uint8_t kLevels[6] = {0x00, 0x5f, 0x87, 0xaf, 0xd7, 0xff};
    for (uint32_t i = 0; i < 216; ++i)
        palette[16 + i] = PrColor8{kLevels[i / 36], kLevels[i / 6 % 6], kLevels[i % 6], 0xff};
    for (uint32_t i = 0; i < 24; ++i) {
        uint8_t level = uint8_t(8 + 10 * i);
        palette[232 + i] = PrColor8{level, level, level, 0xff};
    }
}

void putHex4(Vterm* vterm, uint8_t channel) {
    constexpr char kHex[] = "0123456789abcdef";
    for (uint32_t i = 0; i < 2; ++i) {
        vtermPutByte(vterm, uint8_t(kHex[channel >> 4]));
        vtermPutByte(vterm, uint8_t(kHex[channel & 15]));
    }
}

// Writes a color in X11's form rgb:rrrr/gggg/bbbb.
void putColor(Vterm* vterm, PrColor8 color) {
    vtermPut(vterm, "rgb:");
    putHex4(vterm, color.r);
    vtermPutByte(vterm, '/');
    putHex4(vterm, color.g);
    vtermPutByte(vterm, '/');
    putHex4(vterm, color.b);
}

// Ends a string that the terminal sends with the same terminator as the program's request.
void putTerminator(Vterm* vterm, uint8_t terminator) {
    if (terminator == 0x07)
        vtermPutByte(vterm, 0x07);
    else
        vtermPut(vterm, "\x1b\\");
}

// Records that the colors that cells resolve to changed (prVtermColorRevision).
void colorsChanged(Vterm* vterm) {
    vterm->colorRevision = vtermRevise(vterm);
}

// State

VtermCursorState cursorDefault() {
    return VtermCursorState{};
}

VtermModes modesDefault() {
    return VtermModes{.autowrap = true, .cursorVisible = true, .alternateScroll = true};
}

// Returns the terminal's screens and modes to their state at creation, and keeps the palette's defaults. The lines that
// it empties count as dropped from the scrollback, so that no later line reuses their numbers.
void stateReset(Vterm* vterm) {
    vterm->scrollbackDropped += vterm->scrollback.count + vterm->screens[VtermScreenKind_Primary].rows.slots.count;
    vtermScreensReset(vterm);
    vtermLinksReset(&vterm->links);
    vterm->link = kPrVtermLinkNone;
    for (VtermScreen& screen : vterm->screens) {
        screen.saved = cursorDefault();
        screen.hasSaved = false;
        screen.keyboardCount = 0;
    }
    vterm->active = VtermScreenKind_Primary;
    vterm->cursor = cursorDefault();
    vterm->modes = modesDefault();
    vterm->cursorShape = PrVtermCursorShape_Block;
    vterm->checksum = vterm->defaultChecksum;
    vterm->mouseTracking = VtermMouseTracking_None;
    vterm->mouseEncoding = VtermMouseEncoding_Default;
    vterm->mouseMoved = false;
    vterm->hasLastPrinted = false;
    vterm->lastCodePoint = 0;
    prMemCopy(vterm->palette, vterm->defaultPalette, sizeof(vterm->palette));
    prMemCopy(vterm->roles, vterm->defaultRoles, sizeof(vterm->roles));
    prRawStringClear(&vterm->title);
    for (uint32_t i = 0; i < vterm->titleCount; ++i)
        prRawStringReset(&vterm->titles[i]);
    vterm->titleCount = 0;
    prRawStringClear(&vterm->workingDirectory);
    vtermNotificationsDropDraft(&vterm->notifications);
    vtermGraphicsClear(vterm);
    vterm->shell = PrVtermShellInfo{};
    vterm->hasPrompt = false;
    vtermScreensInit(vterm);
    colorsChanged(vterm);
}

// The cursor's row as the program counts it. In origin mode it counts from the top of the scroll region.
uint32_t cursorRowRelative(const Vterm* vterm) {
    return vterm->cursor.originMode ? vterm->cursor.row - vterm->top : vterm->cursor.row;
}

// The cursor's column as the program counts it. In origin mode it counts from the left margin.
uint32_t cursorColumnRelative(const Vterm* vterm) {
    return vterm->cursor.originMode ? vterm->cursor.column - vterm->left : vterm->cursor.column;
}

// Moves the cursor to `column` and `row` as the program counts them. In origin mode the cursor stays in the scroll
// region.
void cursorToRelative(Vterm* vterm, uint32_t column, uint32_t row) {
    if (vterm->cursor.originMode) {
        row += vterm->top;
        if (row >= vterm->bottom)
            row = vterm->bottom - 1;
        column += vterm->left;
        if (column >= vterm->right)
            column = vterm->right - 1;
    }
    vtermCursorTo(vterm, column, row);
}

// Moves the cursor left or right by `count` columns. If the cursor starts between the margins, it stops at a margin.
// Otherwise it stops at the screen's edge.
void cursorBack(Vterm* vterm, uint32_t count) {
    VtermCursorState& cursor = vterm->cursor;
    uint32_t limit = cursor.column >= vterm->left ? vterm->left : 0;
    vtermCursorTo(vterm, cursor.column > limit + count ? cursor.column - count : limit, cursor.row);
}

void cursorForward(Vterm* vterm, uint32_t count) {
    VtermCursorState& cursor = vterm->cursor;
    uint32_t limit = (cursor.column < vterm->right ? vterm->right : vterm->columns) - 1;
    vtermCursorTo(vterm, cursor.column + count < limit ? cursor.column + count : limit, cursor.row);
}

// Sets the left and right margins (DECSLRM) and homes the cursor. Margins of fewer than two columns are ignored.
void marginsSet(Vterm* vterm, const VtermAction* action) {
    uint32_t left = vtermParamCount(action, 0, 1) - 1;
    uint32_t right = vtermParamCount(action, 1, vterm->columns);
    if (right > vterm->columns)
        right = vterm->columns;
    if (left + 1 < right) {
        vterm->left = left;
        vterm->right = right;
        cursorToRelative(vterm, 0, 0);
    }
}

void cursorSave(Vterm* vterm) {
    VtermScreen& screen = vtermScreen(vterm);
    screen.saved = vterm->cursor;
    screen.hasSaved = true;
}

void cursorRestore(Vterm* vterm) {
    VtermScreen& screen = vtermScreen(vterm);
    vterm->cursor = screen.hasSaved ? screen.saved : cursorDefault();
    vtermCursorTo(vterm, vterm->cursor.column, vterm->cursor.row);
    vterm->cursor.wrapPending = screen.hasSaved && screen.saved.wrapPending;
}

// Moves the cursor to the next tab stop. It stops at the right margin if it starts between the margins, and otherwise
// at the last column.
void tabForward(Vterm* vterm, uint32_t count) {
    VtermCursorState& cursor = vterm->cursor;
    uint32_t last = (cursor.column < vterm->right ? vterm->right : vterm->columns) - 1;
    while (count-- > 0 && cursor.column < last) {
        ++cursor.column;
        while (cursor.column < last && vterm->tabStops.data[cursor.column] == 0)
            ++cursor.column;
    }
    cursor.wrapPending = false;
}

// Moves the cursor to the previous tab stop. It stops at the first column, or at the left margin in origin mode.
void tabBackward(Vterm* vterm, uint32_t count) {
    VtermCursorState& cursor = vterm->cursor;
    uint32_t first = cursor.originMode ? vterm->left : 0;
    while (count-- > 0 && cursor.column > first) {
        --cursor.column;
        while (cursor.column > first && vterm->tabStops.data[cursor.column] == 0)
            --cursor.column;
    }
    cursor.wrapPending = false;
}

void softReset(Vterm* vterm) {
    vterm->modes.insert = false;
    vterm->modes.cursorKeys = false;
    vterm->modes.autowrap = true;
    vterm->modes.cursorVisible = true;
    vterm->modes.leftRightMargins = false;
    vterm->cursor.originMode = false;
    vterm->cursor.pen = VtermPen{};
    vterm->link = kPrVtermLinkNone;
    vterm->cursor.shift = 0;
    for (VtermCharset& charset : vterm->cursor.charsets)
        charset = VtermCharset_Ascii;
    vterm->top = 0;
    vterm->bottom = vterm->rows;
    vtermMarginsReset(vterm);
    for (VtermScreen& screen : vterm->screens) {
        screen.saved = cursorDefault();
        screen.hasSaved = false;
    }
}

// Control characters

void execute(Vterm* vterm, uint8_t byte) {
    VtermCursorState& cursor = vterm->cursor;
    switch (byte) {
        case 0x07:
            vterm->events |= PrVtermEventFlag_Bell;
            break;
        case 0x08:
            if (cursor.wrapPending)
                cursor.wrapPending = false;
            else
                cursorBack(vterm, 1);
            break;
        case 0x09:
            tabForward(vterm, 1);
            break;
        case 0x0a:
        case 0x0b:
        case 0x0c:
            vtermIndex(vterm);
            if (vterm->modes.newline)
                vtermCarriageReturn(vterm);
            break;
        case 0x0d:
            vtermCarriageReturn(vterm);
            break;
        case 0x0e:
            cursor.shift = 1;
            break;
        case 0x0f:
            cursor.shift = 0;
            break;
        default:
            break;
    }
}

// Escape sequences

void escape(Vterm* vterm, const VtermAction* action) {
    VtermCursorState& cursor = vterm->cursor;
    if (action->intermediateCount == 1) {
        char intermediate = action->intermediates[0];
        if (intermediate >= '(' && intermediate <= '+') {
            VtermCharset charset = VtermCharset_Ascii;
            if (action->final == '0')
                charset = VtermCharset_DecSpecial;
            else if (action->final == 'A')
                charset = VtermCharset_British;
            cursor.charsets[intermediate - '('] = charset;
            return;
        }
        if (intermediate == '#' && action->final == '8') {
            // DECALN fills the screen with E, which was used to align a CRT.
            vterm->top = 0;
            vterm->bottom = vterm->rows;
            vtermMarginsReset(vterm);
            for (uint32_t row = 0; row < vterm->rows; ++row) {
                VtermStoredLine& line = vtermRow(vterm, row);
                vtermLineClear(vterm, &line, vterm->columns);
                for (uint32_t column = 0; column < vterm->columns; ++column)
                    line.cells.data[column] = PrVtermCell{.codePoint = 'E'};
                vtermLineWrote(&line, vterm->columns);
            }
            vtermCursorTo(vterm, 0, 0);
        }
        return;
    }
    if (action->intermediateCount != 0)
        return;
    switch (action->final) {
        case '6':
            // DECBI moves the cursor left. At the left margin, it scrolls the columns between the margins right.
            if (cursor.column == vterm->left)
                vtermInsertColumns(vterm, cursor.column, 1);
            else
                cursorBack(vterm, 1);
            break;
        case '7':
            cursorSave(vterm);
            break;
        case '8':
            cursorRestore(vterm);
            break;
        case '9':
            // DECFI moves the cursor right. At the right margin, it scrolls the columns between the margins left.
            if (cursor.column + 1 == vterm->right)
                vtermDeleteColumns(vterm, vterm->left, 1);
            else
                cursorForward(vterm, 1);
            break;
        case 'D':
            vtermIndex(vterm);
            break;
        case 'E':
            vtermIndex(vterm);
            vtermCarriageReturn(vterm);
            break;
        case 'H':
            vterm->tabStops.data[cursor.column] = 1;
            break;
        case 'M':
            vtermReverseIndex(vterm);
            break;
        case 'c':
            stateReset(vterm);
            break;
        case 'n':
            cursor.shift = 2;
            break;
        case 'o':
            cursor.shift = 3;
            break;
        default:
            break;
    }
}

// Control sequences

// The channel of an RGB color of SGR at parameter `index`, clamped to 255.
uint8_t rgbChannel(const VtermAction* action, uint32_t index) {
    uint32_t value = vtermParam(action, index, 0);
    return uint8_t(value < 255 ? value : 255);
}

// Reads an extended color of SGR. Its kind is at parameter `kind`, and its values follow up to parameter `last`,
// included. Returns false and leaves `*color` unchanged if the values are too few or the palette has no such entry.
bool extendedColorValues(const VtermAction* action, uint32_t kind, uint32_t last, PrVtermColor* color) {
    switch (vtermParam(action, kind, 0)) {
        case 5: {
            uint32_t entry = vtermParam(action, kind + 1, 0);
            if (last < kind + 1 || entry >= kPrVtermPaletteCount)
                return false;
            *color = prVtermColorPalette(uint8_t(entry));
            return true;
        }
        case 2: {
            // In the colon form, a color space identifier may precede the three channels.
            uint32_t first = last >= kind + 4 ? kind + 2 : kind + 1;
            if (last < first + 2)
                return false;
            *color = prVtermColorRgb(rgbChannel(action, first), rgbChannel(action, first + 1), rgbChannel(action, first + 2));
            return true;
        }
        default:
            return false;
    }
}

// Reads an extended color of SGR, in the colon form or the semicolon form, from the parameters after `*index`. Advances
// `*index` to the color's last parameter. A color that is incomplete, or that a cell cannot hold, leaves `*color`
// unchanged.
void extendedColor(const VtermAction* action, uint32_t* index, PrVtermColor* color) {
    uint32_t i = *index;
    if (vtermParamIsSub(action, i + 1)) {
        uint32_t end = i + 1;
        while (vtermParamIsSub(action, end + 1))
            ++end;
        *index = end;
        extendedColorValues(action, i + 1, end, color);
        return;
    }
    // The semicolon form takes as many parameters as its kind needs, up to the last parameter.
    uint32_t kind = vtermParam(action, i + 1, 0);
    uint32_t needed = kind == 5 ? 2 : kind == 2 ? 4 : 1;
    uint32_t last = i + needed < action->paramCount ? i + needed : action->paramCount - 1;
    *index = last;
    if (kind == 5 || kind == 2)
        extendedColorValues(action, i + 1, last, color);
}

PrVtermColor paletteColor(uint32_t index) {
    return prVtermColorPalette(uint8_t(index));
}

void selectGraphicRendition(Vterm* vterm, const VtermAction* action) {
    VtermPen& pen = vterm->cursor.pen;
    if (action->paramCount == 0) {
        pen = VtermPen{};
        return;
    }
    for (uint32_t i = 0; i < action->paramCount; ++i) {
        uint32_t value = vtermParam(action, i, 0);
        switch (value) {
            case 0:
                pen = VtermPen{};
                break;
            case 1:
                pen.flags |= PrVtermCellFlag_Bold;
                break;
            case 2:
                pen.flags |= PrVtermCellFlag_Faint;
                break;
            case 3:
                pen.flags |= PrVtermCellFlag_Italic;
                break;
            case 4:
                if (vtermParamIsSub(action, i + 1)) {
                    uint32_t style = vtermParam(action, ++i, 1);
                    pen.underline =
                        style <= PrVtermUnderline_Dashed ? PrVtermUnderline(style) : PrVtermUnderline_Single;
                } else {
                    pen.underline = PrVtermUnderline_Single;
                }
                break;
            case 5:
            case 6:
                pen.flags |= PrVtermCellFlag_Blink;
                break;
            case 7:
                pen.flags |= PrVtermCellFlag_Inverse;
                break;
            case 8:
                pen.flags |= PrVtermCellFlag_Invisible;
                break;
            case 9:
                pen.flags |= PrVtermCellFlag_Strikethrough;
                break;
            case 21:
                pen.underline = PrVtermUnderline_Double;
                break;
            case 22:
                pen.flags &= PrVtermCellFlags(~(PrVtermCellFlag_Bold | PrVtermCellFlag_Faint));
                break;
            case 23:
                pen.flags &= PrVtermCellFlags(~PrVtermCellFlag_Italic);
                break;
            case 24:
                pen.underline = PrVtermUnderline_None;
                break;
            case 25:
                pen.flags &= PrVtermCellFlags(~PrVtermCellFlag_Blink);
                break;
            case 27:
                pen.flags &= PrVtermCellFlags(~PrVtermCellFlag_Inverse);
                break;
            case 28:
                pen.flags &= PrVtermCellFlags(~PrVtermCellFlag_Invisible);
                break;
            case 29:
                pen.flags &= PrVtermCellFlags(~PrVtermCellFlag_Strikethrough);
                break;
            case 38:
                extendedColor(action, &i, &pen.foreground);
                break;
            case 39:
                pen.foreground = PrVtermColor{};
                break;
            case 48:
                extendedColor(action, &i, &pen.background);
                break;
            case 49:
                pen.background = PrVtermColor{};
                break;
            case 53:
                pen.flags |= PrVtermCellFlag_Overline;
                break;
            case 55:
                pen.flags &= PrVtermCellFlags(~PrVtermCellFlag_Overline);
                break;
            case 58:
                extendedColor(action, &i, &pen.underlineColor);
                break;
            case 59:
                pen.underlineColor = PrVtermColor{};
                break;
            default:
                if (value >= 30 && value <= 37)
                    pen.foreground = paletteColor(value - 30);
                else if (value >= 40 && value <= 47)
                    pen.background = paletteColor(value - 40);
                else if (value >= 90 && value <= 97)
                    pen.foreground = paletteColor(value - 90 + 8);
                else if (value >= 100 && value <= 107)
                    pen.background = paletteColor(value - 100 + 8);
                break;
        }
        // Skip any subparameters that the parameter did not read.
        while (vtermParamIsSub(action, i + 1))
            ++i;
    }
}

// The state of a mode, as DECRQM reports it.
enum {
    ModeSetting_Unknown = 0,
    ModeSetting_Set = 1,
    ModeSetting_Reset = 2,
    ModeSetting_AlwaysSet = 3,
};
typedef uint8_t ModeSetting;

ModeSetting setting(bool value) {
    return value ? ModeSetting_Set : ModeSetting_Reset;
}

// Sets or resets an ANSI mode, or reads it if `query` is set.
ModeSetting ansiMode(Vterm* vterm, uint32_t mode, bool set, bool query) {
    bool* flag = nullptr;
    switch (mode) {
        case 4:
            flag = &vterm->modes.insert;
            break;
        case 20:
            flag = &vterm->modes.newline;
            break;
        default:
            return ModeSetting_Unknown;
    }
    if (!query)
        *flag = set;
    return setting(*flag);
}

void mouseTrackingSet(Vterm* vterm, VtermMouseTracking tracking, bool set) {
    if (set)
        vterm->mouseTracking = tracking;
    else if (vterm->mouseTracking == tracking)
        vterm->mouseTracking = VtermMouseTracking_None;
    vterm->mouseMoved = false;
}

void mouseEncodingSet(Vterm* vterm, VtermMouseEncoding encoding, bool set) {
    if (set)
        vterm->mouseEncoding = encoding;
    else if (vterm->mouseEncoding == encoding)
        vterm->mouseEncoding = VtermMouseEncoding_Default;
}

// Sets or resets a DEC private mode, or reads it if `query` is set.
ModeSetting decMode(Vterm* vterm, uint32_t mode, bool set, bool query) {
    VtermModes& modes = vterm->modes;
    bool* flag = nullptr;
    switch (mode) {
        case 1:
            flag = &modes.cursorKeys;
            break;
        case 5:
            if (!query && set != modes.reverseVideo) {
                modes.reverseVideo = set;
                colorsChanged(vterm);
            }
            return setting(modes.reverseVideo);
        case 6:
            if (!query) {
                vterm->cursor.originMode = set;
                cursorToRelative(vterm, 0, 0);
            }
            return setting(vterm->cursor.originMode);
        case 7:
            flag = &modes.autowrap;
            break;
        case 12:
            flag = &modes.cursorBlinking;
            break;
        case 25:
            flag = &modes.cursorVisible;
            break;
        case 9:
        case 1000:
        case 1002:
        case 1003: {
            VtermMouseTracking tracking = VtermMouseTracking_Any;
            if (mode == 9)
                tracking = VtermMouseTracking_Press;
            else if (mode == 1000)
                tracking = VtermMouseTracking_Click;
            else if (mode == 1002)
                tracking = VtermMouseTracking_Drag;
            if (!query)
                mouseTrackingSet(vterm, tracking, set);
            return setting(vterm->mouseTracking == tracking);
        }
        case 1005:
        case 1006:
        case 1015:
        case 1016: {
            VtermMouseEncoding encoding = VtermMouseEncoding_SgrPixels;
            if (mode == 1005)
                encoding = VtermMouseEncoding_Utf8;
            else if (mode == 1006)
                encoding = VtermMouseEncoding_Sgr;
            else if (mode == 1015)
                encoding = VtermMouseEncoding_Urxvt;
            if (!query)
                mouseEncodingSet(vterm, encoding, set);
            return setting(vterm->mouseEncoding == encoding);
        }
        case 1004:
            flag = &modes.focusReports;
            break;
        case 1007:
            flag = &modes.alternateScroll;
            break;
        case 47:
        case 1047:
        case 1049: {
            bool alternate = vterm->active == VtermScreenKind_Alternate;
            if (query)
                return setting(alternate);
            if (set && !alternate) {
                if (mode == 1049)
                    cursorSave(vterm);
                vtermScreenShow(vterm, VtermScreenKind_Alternate, mode != 47);
            } else if (!set && alternate) {
                vtermScreenShow(vterm, VtermScreenKind_Primary, false);
                if (mode == 1049)
                    cursorRestore(vterm);
            }
            return setting(set);
        }
        case 1048:
            if (!query) {
                if (set)
                    cursorSave(vterm);
                else
                    cursorRestore(vterm);
            }
            return ModeSetting_Reset;
        case 2004:
            flag = &modes.bracketedPaste;
            break;
        case 2026:
            flag = &modes.synchronized;
            break;
        case 2027:
            // Grapheme cluster mode is always set: a cluster always takes as many cells as it is wide.
            return ModeSetting_AlwaysSet;
        case 2031:
            flag = &modes.colorSchemeReports;
            break;
        case 69:
            if (!query) {
                modes.leftRightMargins = set;
                if (!set)
                    vtermMarginsReset(vterm);
            }
            return setting(modes.leftRightMargins);
        default:
            return ModeSetting_Unknown;
    }
    if (!query)
        *flag = set;
    return setting(*flag);
}

void modesSet(Vterm* vterm, const VtermAction* action, bool set) {
    for (uint32_t i = 0; i < action->paramCount; ++i) {
        uint32_t mode = vtermParam(action, i, 0);
        if (action->marker == '?')
            decMode(vterm, mode, set, false);
        else
            ansiMode(vterm, mode, set, false);
    }
}

void eraseInDisplay(Vterm* vterm, uint32_t mode) {
    VtermCursorState& cursor = vterm->cursor;
    switch (mode) {
        case 0:
            vtermErase(vterm, cursor.row, cursor.column, vterm->columns);
            for (uint32_t row = cursor.row + 1; row < vterm->rows; ++row)
                vtermErase(vterm, row, 0, vterm->columns);
            break;
        case 1:
            for (uint32_t row = 0; row < cursor.row; ++row)
                vtermErase(vterm, row, 0, vterm->columns);
            vtermErase(vterm, cursor.row, 0, cursor.column + 1);
            break;
        case 2:
            for (uint32_t row = 0; row < vterm->rows; ++row) {
                vtermErase(vterm, row, 0, vterm->columns);
                vtermRow(vterm, row).flags = 0;
            }
            vtermGraphicsEraseScreen(vterm);
            break;
        case 3:
            vtermGraphicsEraseScrollback(vterm);
            vtermScrollbackClear(vterm);
            break;
        default:
            return;
    }
    cursor.wrapPending = false;
}

void eraseInLine(Vterm* vterm, uint32_t mode) {
    VtermCursorState& cursor = vterm->cursor;
    switch (mode) {
        case 0:
            vtermErase(vterm, cursor.row, cursor.column, vterm->columns);
            break;
        case 1:
            vtermErase(vterm, cursor.row, 0, cursor.column + 1);
            break;
        case 2:
            vtermErase(vterm, cursor.row, 0, vterm->columns);
            break;
        default:
            return;
    }
    cursor.wrapPending = false;
}

// Answers a request for the state of a mode (DECRQM).
void modeReport(Vterm* vterm, const VtermAction* action) {
    uint32_t mode = vtermParam(action, 0, 0);
    bool dec = action->marker == '?';
    ModeSetting value = dec ? decMode(vterm, mode, false, true) : ansiMode(vterm, mode, false, true);
    vtermPut(vterm, dec ? "\x1b[?" : "\x1b[");
    vtermPutNumber(vterm, mode);
    vtermPutByte(vterm, ';');
    vtermPutNumber(vterm, value);
    vtermPut(vterm, "$y");
}

// The edge of a rectangle that parameter `index` of DECRQCRA gives, or `fallback` if it is absent or zero. The
// parameter counts from one at `minimum`, and the edge is clamped to `maximum`.
uint32_t rectEdge(const VtermAction* action, uint32_t index, uint32_t minimum, uint32_t maximum, uint32_t fallback) {
    uint32_t value = vtermParam(action, index, 0);
    if (value == 0)
        return fallback;
    return value - 1 < maximum - minimum ? minimum + value - 1 : maximum;
}

// Answers a request for the checksum of a rectangle (DECRQCRA). The rectangle's edges are inclusive and count from one.
// In origin mode, rows and columns count from the top and left margins of the scroll region, and stay in the region.
// The terminal has one page, so it ignores the request's page.
void checksumReport(Vterm* vterm, const VtermAction* action) {
    uint32_t minRow = vterm->cursor.originMode ? vterm->top : 0;
    uint32_t maxRow = vterm->cursor.originMode ? vterm->bottom - 1 : vterm->rows - 1;
    uint32_t top = rectEdge(action, 2, minRow, maxRow, minRow);
    uint32_t minColumn = vterm->cursor.originMode ? vterm->left : 0;
    uint32_t maxColumn = vterm->cursor.originMode ? vterm->right - 1 : vterm->columns - 1;
    uint32_t left = rectEdge(action, 3, minColumn, maxColumn, minColumn);
    uint32_t bottom = rectEdge(action, 4, minRow, maxRow, maxRow);
    uint32_t right = rectEdge(action, 5, minColumn, maxColumn, maxColumn);
    uint16_t checksum = 0;
    if (top <= bottom && left <= right)
        checksum = vtermChecksum(vterm, top, bottom + 1, left, right + 1);
    vtermPut(vterm, "\x1bP");
    vtermPutNumber(vterm, vtermParam(action, 0, 0));
    vtermPut(vterm, "!~");
    constexpr char kHex[] = "0123456789ABCDEF";
    for (uint32_t shift = 16; shift != 0; shift -= 4)
        vtermPutByte(vterm, uint8_t(kHex[(checksum >> (shift - 4)) & 15]));
    vtermPut(vterm, "\x1b\\");
}

// Answers an XTWINOPS request for an extent: the text area in pixels (14), a cell in pixels (16), or the screen in
// cells (18). The answer gives the height first.
void windowReport(Vterm* vterm, uint32_t operation) {
    PrUVec2 cell = vterm->graphics.cellPixels;
    PrUVec2 extent{vterm->columns, vterm->rows};
    if (operation == 14)
        extent = PrUVec2{cell.x * vterm->columns, cell.y * vterm->rows};
    else if (operation == 16)
        extent = cell;
    vtermPut(vterm, "\x1b[");
    vtermPutNumber(vterm, operation - 10);
    vtermPutByte(vterm, ';');
    vtermPutNumber(vterm, extent.y);
    vtermPutByte(vterm, ';');
    vtermPutNumber(vterm, extent.x);
    vtermPutByte(vterm, 't');
}

void titlePush(Vterm* vterm) {
    if (vterm->titleCount == kVtermTitleStackMax) {
        prRawStringReset(&vterm->titles[0]);
        prMemMove(vterm->titles, vterm->titles + 1, sizeof(PrRawString) * (kVtermTitleStackMax - 1));
        --vterm->titleCount;
    }
    vterm->titles[vterm->titleCount++] = prRawStringCopy(prRawStringView(&vterm->title));
}

void titlePop(Vterm* vterm) {
    if (vterm->titleCount == 0)
        return;
    PrRawString& title = vterm->titles[--vterm->titleCount];
    prRawStringReplace(&vterm->title, prRawStringView(&title));
    prRawStringReset(&title);
    vterm->events |= PrVtermEventFlag_Title;
}

void keyboardMode(Vterm* vterm, const VtermAction* action) {
    VtermScreen& screen = vtermScreen(vterm);
    switch (action->marker) {
        case '?': {
            uint8_t flags = screen.keyboardCount != 0 ? screen.keyboard[screen.keyboardCount - 1] : 0;
            vtermPut(vterm, "\x1b[?");
            vtermPutNumber(vterm, flags);
            vtermPutByte(vterm, 'u');
            break;
        }
        case '>': {
            if (screen.keyboardCount == kVtermKeyboardStackMax) {
                prMemMove(screen.keyboard, screen.keyboard + 1, kVtermKeyboardStackMax - 1);
                --screen.keyboardCount;
            }
            screen.keyboard[screen.keyboardCount++] = uint8_t(vtermParam(action, 0, 0) & kVtermKeyboardSupported);
            break;
        }
        case '<': {
            uint32_t count = vtermParamCount(action, 0, 1);
            screen.keyboardCount = count < screen.keyboardCount ? screen.keyboardCount - count : 0;
            break;
        }
        case '=': {
            uint8_t flags = uint8_t(vtermParam(action, 0, 0) & kVtermKeyboardSupported);
            if (screen.keyboardCount == 0)
                screen.keyboard[screen.keyboardCount++] = 0;
            uint8_t& top = screen.keyboard[screen.keyboardCount - 1];
            switch (vtermParam(action, 1, 1)) {
                case 1:
                    top = flags;
                    break;
                case 2:
                    top |= flags;
                    break;
                case 3:
                    top &= uint8_t(~flags);
                    break;
                default:
                    break;
            }
            break;
        }
        default:
            break;
    }
}

void controlSequence(Vterm* vterm, const VtermAction* action) {
    VtermCursorState& cursor = vterm->cursor;
    char final = action->final;
    char intermediate = action->intermediateCount == 1 ? action->intermediates[0] : 0;
    if (action->intermediateCount > 1)
        return;
    if (intermediate != 0) {
        if (intermediate == ' ' && final == 'q' && action->marker == 0) {
            // DECSCUSR: odd styles blink and even styles are steady. Zero is the default, a blinking block.
            uint32_t style = vtermParam(action, 0, 0);
            if (style <= 2)
                vterm->cursorShape = PrVtermCursorShape_Block;
            else if (style <= 4)
                vterm->cursorShape = PrVtermCursorShape_Underline;
            else
                vterm->cursorShape = PrVtermCursorShape_Bar;
            vterm->modes.cursorBlinking = style == 0 || style % 2 == 1;
        } else if (intermediate == '!' && final == 'p') {
            softReset(vterm);
        } else if (intermediate == '$' && final == 'p') {
            modeReport(vterm, action);
        } else if (intermediate == ' ' && (final == '@' || final == 'A') && action->marker == 0) {
            // SL and SR scroll the columns between the margins left or right.
            uint32_t count = vtermParamCount(action, 0, 1);
            if (final == '@')
                vtermDeleteColumns(vterm, vterm->left, count);
            else
                vtermInsertColumns(vterm, vterm->left, count);
        } else if (intermediate == '\'' && (final == '}' || final == '~') && action->marker == 0) {
            uint32_t count = vtermParamCount(action, 0, 1);
            if (final == '}')
                vtermInsertColumns(vterm, cursor.column, count);
            else
                vtermDeleteColumns(vterm, cursor.column, count);
        } else if (intermediate == '*' && final == 'y' && action->marker == 0) {
            checksumReport(vterm, action);
        } else if (intermediate == '#' && final == 'y' && action->marker == 0) {
            // XTCHECKSUM selects the flags of later checksums (PrVtermChecksumFlags).
            vterm->checksum = PrVtermChecksumFlags(vtermParam(action, 0, 0) & PrVtermChecksumFlag_All);
        }
        return;
    }
    if (action->marker == '?' || action->marker == '>' || action->marker == '<' || action->marker == '=') {
        switch (final) {
            case 'h':
            case 'l':
                if (action->marker == '?')
                    modesSet(vterm, action, final == 'h');
                return;
            case 'J':
                if (action->marker == '?')
                    eraseInDisplay(vterm, vtermParam(action, 0, 0));
                return;
            case 'K':
                if (action->marker == '?')
                    eraseInLine(vterm, vtermParam(action, 0, 0));
                return;
            case 'c':
                if (action->marker == '>' && vtermParam(action, 0, 0) == 0)
                    vtermPut(vterm, "\x1b[>1;10;0c");
                return;
            case 'n':
                if (action->marker == '?' && vtermParam(action, 0, 0) == 6) {
                    vtermPut(vterm, "\x1b[?");
                    vtermPutNumber(vterm, cursorRowRelative(vterm) + 1);
                    vtermPutByte(vterm, ';');
                    vtermPutNumber(vterm, cursorColumnRelative(vterm) + 1);
                    vtermPut(vterm, ";1R");
                } else if (action->marker == '?' && vtermParam(action, 0, 0) == 996) {
                    vtermColorSchemeReport(vterm);
                }
                return;
            case 'q':
                if (action->marker == '>' && vtermParam(action, 0, 0) == 0) {
                    vtermPut(vterm, "\x1bP>|");
                    vtermPut(vterm, kVersion);
                    vtermPut(vterm, "\x1b\\");
                }
                return;
            case 'u':
                keyboardMode(vterm, action);
                return;
            default:
                return;
        }
    }

    switch (final) {
        case '@':
            vtermInsertCells(vterm, vtermParamCount(action, 0, 1));
            break;
        case 'A': {
            uint32_t count = vtermParamCount(action, 0, 1);
            uint32_t limit = cursor.row >= vterm->top ? vterm->top : 0;
            vtermCursorTo(vterm, cursor.column, cursor.row > limit + count ? cursor.row - count : limit);
            break;
        }
        case 'B':
        case 'e': {
            uint32_t count = vtermParamCount(action, 0, 1);
            uint32_t limit = cursor.row < vterm->bottom ? vterm->bottom - 1 : vterm->rows - 1;
            vtermCursorTo(vterm, cursor.column, cursor.row + count < limit ? cursor.row + count : limit);
            break;
        }
        case 'C':
        case 'a':
            cursorForward(vterm, vtermParamCount(action, 0, 1));
            break;
        case 'D':
            cursorBack(vterm, vtermParamCount(action, 0, 1));
            break;
        case 'E':
        case 'F': {
            uint32_t count = vtermParamCount(action, 0, 1);
            uint32_t row = cursor.row;
            if (final == 'E') {
                uint32_t limit = row < vterm->bottom ? vterm->bottom - 1 : vterm->rows - 1;
                row = row + count < limit ? row + count : limit;
            } else {
                uint32_t limit = row >= vterm->top ? vterm->top : 0;
                row = row > limit + count ? row - count : limit;
            }
            vtermCursorTo(vterm, cursor.column, row);
            vtermCarriageReturn(vterm);
            break;
        }
        case 'G':
        case '`':
            cursorToRelative(vterm, vtermParamCount(action, 0, 1) - 1, cursorRowRelative(vterm));
            break;
        case 'H':
        case 'f':
            cursorToRelative(vterm, vtermParamCount(action, 1, 1) - 1, vtermParamCount(action, 0, 1) - 1);
            break;
        case 'I':
            tabForward(vterm, vtermParamCount(action, 0, 1));
            break;
        case 'J':
            eraseInDisplay(vterm, vtermParam(action, 0, 0));
            break;
        case 'K':
            eraseInLine(vterm, vtermParam(action, 0, 0));
            break;
        case 'L':
            vtermInsertLines(vterm, vtermParamCount(action, 0, 1));
            break;
        case 'M':
            vtermDeleteLines(vterm, vtermParamCount(action, 0, 1));
            break;
        case 'P':
            vtermDeleteCells(vterm, vtermParamCount(action, 0, 1));
            break;
        case 'S':
            vtermScrollUp(vterm, vtermParamCount(action, 0, 1));
            break;
        case 'T':
            if (action->paramCount <= 1)
                vtermScrollDown(vterm, vtermParamCount(action, 0, 1));
            break;
        case 'X': {
            uint32_t count = vtermParamCount(action, 0, 1);
            vtermErase(vterm, cursor.row, cursor.column, cursor.column + count);
            cursor.wrapPending = false;
            break;
        }
        case 'Z':
            tabBackward(vterm, vtermParamCount(action, 0, 1));
            break;
        case 'b': {
            if (vterm->lastCodePoint == 0)
                break;
            uint32_t count = vtermParamCount(action, 0, 1);
            uint32_t limit = vterm->columns * vterm->rows;
            for (uint32_t i = 0; i < count && i < limit; ++i)
                vtermPrint(vterm, vterm->lastCodePoint);
            break;
        }
        case 'c':
            // A VT220 (62) with horizontal scrolling (21), which is DECSLRM, and ANSI color (22).
            if (vtermParam(action, 0, 0) == 0)
                vtermPut(vterm, "\x1b[?62;21;22c");
            break;
        case 'd':
            cursorToRelative(vterm, cursorColumnRelative(vterm), vtermParamCount(action, 0, 1) - 1);
            break;
        case 'g': {
            uint32_t mode = vtermParam(action, 0, 0);
            if (mode == 0)
                vterm->tabStops.data[cursor.column] = 0;
            else if (mode == 3)
                prMemZero(vterm->tabStops.data, vterm->tabStops.count);
            break;
        }
        case 'h':
        case 'l':
            modesSet(vterm, action, final == 'h');
            break;
        case 'm':
            selectGraphicRendition(vterm, action);
            break;
        case 'n':
            if (vtermParam(action, 0, 0) == 5) {
                vtermPut(vterm, "\x1b[0n");
            } else if (vtermParam(action, 0, 0) == 6) {
                vtermPut(vterm, "\x1b[");
                vtermPutNumber(vterm, cursorRowRelative(vterm) + 1);
                vtermPutByte(vterm, ';');
                vtermPutNumber(vterm, cursorColumnRelative(vterm) + 1);
                vtermPutByte(vterm, 'R');
            }
            break;
        case 'r': {
            uint32_t top = vtermParamCount(action, 0, 1) - 1;
            uint32_t bottom = vtermParamCount(action, 1, vterm->rows);
            if (bottom > vterm->rows)
                bottom = vterm->rows;
            if (top + 1 < bottom) {
                vterm->top = top;
                vterm->bottom = bottom;
                cursorToRelative(vterm, 0, 0);
            }
            break;
        }
        case 's':
            // If DECLRMM is set, the sequence is DECSLRM. Otherwise it saves the cursor.
            if (vterm->modes.leftRightMargins)
                marginsSet(vterm, action);
            else if (action->paramCount == 0)
                cursorSave(vterm);
            break;
        case 'u':
            if (action->paramCount == 0)
                cursorRestore(vterm);
            break;
        case 't':
            switch (vtermParam(action, 0, 0)) {
                case 14:
                case 16:
                case 18:
                    windowReport(vterm, vtermParam(action, 0, 0));
                    break;
                case 22:
                    titlePush(vterm);
                    break;
                case 23:
                    titlePop(vterm);
                    break;
                default:
                    break;
            }
            break;
        default:
            break;
    }
}

// Operating system commands

// Reads a hexadecimal channel of one to four digits, and scales it to eight bits.
bool hexChannel(PrStringView text, uint8_t* channel) {
    uint64_t value = 0;
    if (text.size > 4 || !prU64Parse(text.data, text.size, &value, 16))
        return false;
    uint64_t maximum = (uint64_t(1) << (4 * text.size)) - 1;
    *channel = uint8_t((value * 255 + maximum / 2) / maximum);
    return true;
}

// Reads a color in one of X11's forms: rgb:r/g/b, or # followed by the three channels. Each channel has one to four
// hexadecimal digits. After #, the channels have the same number of digits.
bool parseColor(PrStringView text, PrColor8* color) {
    uint8_t channels[3];
    if (text.size > 4 && prMemCompare(text.data, "rgb:", 4) == 0) {
        size_t at = 4;
        for (uint32_t i = 0; i < 3; ++i) {
            size_t end = at;
            while (end < text.size && text.data[end] != '/')
                ++end;
            if (!hexChannel(PrStringView{text.data + at, end - at}, &channels[i]))
                return false;
            if (i < 2 && end == text.size)
                return false;
            at = end + 1;
        }
        if (at <= text.size)
            return false;
    } else if (text.size > 1 && text.data[0] == '#' && (text.size - 1) % 3 == 0 && text.size - 1 <= 12) {
        size_t digits = (text.size - 1) / 3;
        for (uint32_t i = 0; i < 3; ++i) {
            if (!hexChannel(PrStringView{text.data + 1 + i * digits, digits}, &channels[i]))
                return false;
        }
    } else {
        return false;
    }
    *color = PrColor8{channels[0], channels[1], channels[2], 0xff};
    return true;
}

// Splits `*text` at its first semicolon: returns what comes before it, and leaves what follows in `*text`.
PrStringView splitField(PrStringView* text) {
    size_t end = 0;
    while (end < text->size && text->data[end] != ';')
        ++end;
    PrStringView field{text->data, end};
    size_t next = end < text->size ? end + 1 : end;
    *text = PrStringView{text->data + next, text->size - next};
    return field;
}

// Reads a decimal number that spans all of `text`, up to UINT32_MAX.
bool parseNumber(PrStringView text, uint32_t* value) {
    uint64_t result = 0;
    if (!prU64Parse(text.data, text.size, &result, 10) || result > UINT32_MAX)
        return false;
    *value = uint32_t(result);
    return true;
}

bool isValidText(PrStringView text) {
    return prUtf8Validate(text) == text.size;
}

void paletteCommand(Vterm* vterm, PrStringView rest, uint8_t terminator) {
    while (rest.size != 0) {
        uint32_t index = 0;
        bool valid = parseNumber(splitField(&rest), &index) && index < kPrVtermPaletteCount;
        PrStringView spec = splitField(&rest);
        if (!valid)
            continue;
        if (spec == "?") {
            vtermPut(vterm, "\x1b]4;");
            vtermPutNumber(vterm, index);
            vtermPutByte(vterm, ';');
            putColor(vterm, vterm->palette[index]);
            putTerminator(vterm, terminator);
        } else if (parseColor(spec, &vterm->palette[index])) {
            colorsChanged(vterm);
        }
    }
}

void roleCommand(Vterm* vterm, uint32_t command, PrStringView rest, uint8_t terminator) {
    // Each field sets or reads one role, starting with the command's own role and continuing with the next.
    for (uint32_t role = command - 10; role < 3 && rest.size != 0; ++role) {
        PrStringView spec = splitField(&rest);
        if (spec == "?") {
            vtermPut(vterm, "\x1b]");
            vtermPutNumber(vterm, 10 + role);
            vtermPutByte(vterm, ';');
            putColor(vterm, vterm->roles[role]);
            putTerminator(vterm, terminator);
        } else if (parseColor(spec, &vterm->roles[role])) {
            colorsChanged(vterm);
        }
    }
}

void clipboardCommand(Vterm* vterm, PrStringView rest) {
    splitField(&rest);
    if (rest == "?" || rest.size == 0)
        return;
    size_t capacity = prBase64DecodedCapacity(rest.size);
    RawArray<char> decoded;
    decoded.resize(uint32_t(capacity));
    size_t size = 0;
    if (prBase64Decode(rest, decoded.data, capacity, &size) && isValidText(PrStringView{decoded.data, size})) {
        prRawStringReplace(&vterm->clipboard, PrStringView{decoded.data, size});
        vterm->events |= PrVtermEventFlag_Clipboard;
    }
    decoded.reset();
}

// Shell integration

// Returns the value of option `name` in `options`, whose fields have the form name=value. Returns the null view if no
// field names the option.
PrStringView markOption(PrStringView options, PrStringView name) {
    while (options.size != 0) {
        PrStringView field = splitField(&options);
        if (field.size > name.size && field.data[name.size] == '=' && prMemCompare(field.data, name.data, name.size) == 0)
            return PrStringView{field.data + name.size + 1, field.size - name.size - 1};
    }
    return PrStringView{};
}

// Reads an exit code that spans all of `text`: a decimal number with an optional minus sign, in the range of int32_t.
bool parseExitCode(PrStringView text, int32_t* code) {
    bool negative = text.size > 1 && text.data[0] == '-';
    PrStringView digits = negative ? PrStringView{text.data + 1, text.size - 1} : text;
    uint32_t magnitude = 0;
    if (!parseNumber(digits, &magnitude) || magnitude > (negative ? 0x80000000u : 0x7fffffffu))
        return false;
    *code = negative ? int32_t(-int64_t(magnitude)) : int32_t(magnitude);
    return true;
}

// Ends the current command. If the command was running, it gets `status` and `exitCode`, and so does the line of its
// prompt.
void commandEnd(Vterm* vterm, PrVtermCommandStatus status, int32_t exitCode) {
    if (vterm->shell.state == PrVtermShellState_Running) {
        vterm->shell.lastStatus = status;
        vterm->shell.lastExitCode = exitCode;
        vtermPromptSetStatus(vterm, status, exitCode);
    }
    vterm->shell.state = PrVtermShellState_None;
}

// Applies a semantic prompt mark of FinalTerm (OSC 133) to the cursor's line of the primary screen. The marks follow
// the shell integration documentation of iTerm2 and WezTerm:
//
// - A, N and P start a prompt. P does not start a fresh line.
// - B starts the input, and C starts the output.
// - D ends the command, with its exit code.
//
// The option k tells a secondary prompt (s or c) and a right prompt (r) from a prompt that starts a command. A program
// on the alternate screen is not the shell, so the alternate screen takes no marks.
void promptMark(Vterm* vterm, PrStringView rest) {
    if (vterm->active != VtermScreenKind_Primary)
        return;
    PrStringView kind = splitField(&rest);
    if (kind.size != 1)
        return;
    VtermCursorState& cursor = vterm->cursor;
    switch (kind.data[0]) {
        case 'A':
        case 'N':
        case 'P': {
            PrStringView role = markOption(rest, "k");
            if (role == "r")
                return;
            if (kind.data[0] != 'P' && (cursor.column != 0 || cursor.wrapPending)) {
                // A prompt starts on a line of its own, even if the output before it did not end its line.
                cursor.column = 0;
                vtermIndex(vterm);
                vterm->hasLastPrinted = false;
            }
            VtermStoredLine& line = vtermRow(vterm, cursor.row);
            if (role == "s" || role == "c") {
                line.marks |= PrVtermLineMarkFlag_Continuation;
                vtermTouch(vterm, &line);
                return;
            }
            // A command that never reported its end ends at the next prompt. If the shell writes a prompt again on the
            // same line, as a redraw does, the line keeps its command's status.
            if (vterm->shell.state == PrVtermShellState_Running)
                commandEnd(vterm, PrVtermCommandStatus_Finished, 0);
            uint64_t number = vterm->scrollbackDropped + vterm->scrollback.count + cursor.row;
            bool same = vterm->hasPrompt && vterm->promptLine == number && (line.marks & PrVtermLineMarkFlag_Prompt) != 0;
            if (!same) {
                line.commandStatus = PrVtermCommandStatus_None;
                line.exitCode = 0;
            }
            line.marks |= PrVtermLineMarkFlag_Prompt;
            vterm->hasPrompt = true;
            vterm->promptLine = number;
            vterm->shell.state = PrVtermShellState_Prompt;
            vtermTouch(vterm, &line);
            return;
        }
        case 'B': {
            VtermStoredLine& line = vtermRow(vterm, cursor.row);
            line.marks |= PrVtermLineMarkFlag_Input;
            line.inputColumn = cursor.column;
            vterm->shell.state = PrVtermShellState_Input;
            vtermTouch(vterm, &line);
            return;
        }
        case 'C': {
            VtermStoredLine& line = vtermRow(vterm, cursor.row);
            line.marks |= PrVtermLineMarkFlag_Output;
            vtermTouch(vterm, &line);
            vterm->shell.state = PrVtermShellState_Running;
            vtermPromptSetStatus(vterm, PrVtermCommandStatus_Running, 0);
            return;
        }
        case 'D': {
            VtermStoredLine& line = vtermRow(vterm, cursor.row);
            line.marks |= PrVtermLineMarkFlag_End;
            vtermTouch(vterm, &line);
            int32_t exitCode = 0;
            PrVtermCommandStatus status = PrVtermCommandStatus_Finished;
            if (parseExitCode(splitField(&rest), &exitCode))
                status = exitCode == 0 ? PrVtermCommandStatus_Succeeded : PrVtermCommandStatus_Failed;
            commandEnd(vterm, status, exitCode);
            return;
        }
        default:
            return;
    }
}

void operatingSystemCommand(Vterm* vterm, const VtermAction* action) {
    PrStringView rest = action->text;
    uint32_t command = 0;
    if (!parseNumber(splitField(&rest), &command))
        return;
    switch (command) {
        case 0:
        case 2:
            if (isValidText(rest)) {
                prRawStringReplace(&vterm->title, rest);
                vterm->events |= PrVtermEventFlag_Title;
            }
            break;
        case 4:
            paletteCommand(vterm, rest, action->terminator);
            break;
        case 7:
            if (isValidText(rest)) {
                prRawStringReplace(&vterm->workingDirectory, rest);
                vterm->events |= PrVtermEventFlag_WorkingDirectory;
            }
            break;
        case 10:
        case 11:
        case 12:
            roleCommand(vterm, command, rest, action->terminator);
            break;
        case 8:
            vtermLinkCommand(vterm, rest);
            break;
        case 9:
        case 99:
        case 777:
            vtermNotificationCommand(vterm, command, rest, action->terminator);
            break;
        case 52:
            clipboardCommand(vterm, rest);
            break;
        case 133:
            promptMark(vterm, rest);
            break;
        case 104:
            colorsChanged(vterm);
            if (rest.size == 0) {
                prMemCopy(vterm->palette, vterm->defaultPalette, sizeof(vterm->palette));
                break;
            }
            while (rest.size != 0) {
                uint32_t index = 0;
                if (parseNumber(splitField(&rest), &index) && index < kPrVtermPaletteCount)
                    vterm->palette[index] = vterm->defaultPalette[index];
            }
            break;
        case 110:
        case 111:
        case 112:
            vterm->roles[command - 110] = vterm->defaultRoles[command - 110];
            colorsChanged(vterm);
            break;
        default:
            break;
    }
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrVterm* prVtermCreate(const PrVtermDesc* desc) {
    PR_ASSERT(desc->extent.x > 0 && desc->extent.y > 0);
    PR_ASSERT(desc->scrollback <= kPrVtermMaxScrollback);
    PR_ASSERT((desc->checksum & ~PrVtermChecksumFlag_All) == 0);
    PrVterm* vterm = allocZeroed<PrVterm>();
    vterm->columns = desc->extent.x;
    vterm->rows = desc->extent.y;
    vterm->scrollback.limit = desc->scrollback;
    vterm->defaultChecksum = desc->checksum;
    vtermGraphicsInit(&vterm->graphics, desc);
    paletteDefault(vterm->defaultPalette);
    prMemCopy(vterm->defaultRoles, kRoleColors, sizeof(kRoleColors));
    vterm->colorScheme = PrAppColorScheme_Dark;
    stateReset(vterm);
    return vterm;
}

void prVtermDestroy(PrVterm* PR_NULLABLE vterm) {
    if (vterm == nullptr)
        return;
    vtermScreensReset(vterm);
    prRawStringReset(&vterm->title);
    for (uint32_t i = 0; i < vterm->titleCount; ++i)
        prRawStringReset(&vterm->titles[i]);
    prRawStringReset(&vterm->workingDirectory);
    prRawStringReset(&vterm->clipboard);
    vtermNotificationsReset(&vterm->notifications);
    vtermLinksReset(&vterm->links);
    vtermGraphicsReset(&vterm->graphics);
    vterm->output.reset();
    vtermParserReset(&vterm->parser);
    prMemoryFree(vterm);
}

void prVtermWrite(PrVterm* vterm, PrByteView bytes) {
    if (bytes.size == 0)
        return;
    vtermRevise(vterm);
    PrStringView text{reinterpret_cast<const char*>(bytes.data), bytes.size};
    size_t at = 0;
    for (;;) {
        VtermAction* action = vtermParserNext(&vterm->parser, text, &at);
        switch (action->kind) {
            case VtermActionKind_None:
                return;
            case VtermActionKind_Ascii:
                vtermPrintAscii(vterm, action->text);
                continue;
            case VtermActionKind_Print:
                for (uint32_t i = 0; i < action->codePointCount; ++i)
                    vtermPrint(vterm, action->codePoints[i]);
                continue;
            case VtermActionKind_Execute:
                vterm->hasLastPrinted = false;
                execute(vterm, uint8_t(action->final));
                continue;
            case VtermActionKind_Escape:
                vterm->hasLastPrinted = false;
                escape(vterm, action);
                continue;
            case VtermActionKind_Csi:
                // REP repeats the last printed character, so it must not forget that character.
                if (action->final != 'b')
                    vterm->hasLastPrinted = false;
                controlSequence(vterm, action);
                continue;
            case VtermActionKind_Osc:
                operatingSystemCommand(vterm, action);
                continue;
            case VtermActionKind_Dcs:
                vtermDeviceControl(vterm, action);
                continue;
            case VtermActionKind_Apc:
                if (action->text.size != 0 && action->text.data[0] == 'G') {
                    vterm->hasLastPrinted = false;
                    vtermGraphicsCommand(vterm, PrStringView{action->text.data + 1, action->text.size - 1});
                }
                continue;
        }
    }
}

void prVtermResize(PrVterm* vterm, PrUVec2 extent) {
    PR_ASSERT(extent.x > 0 && extent.y > 0);
    vtermScreensResize(vterm, extent.x, extent.y);
}

PrUVec2 prVtermExtent(const PrVterm* vterm) {
    return PrUVec2{vterm->columns, vterm->rows};
}

void prVtermReset(PrVterm* vterm) {
    vtermParserClear(&vterm->parser);
    stateReset(vterm);
    vterm->output.clear();
    vterm->events = 0;
    prRawStringClear(&vterm->clipboard);
    vtermNotificationsClear(&vterm->notifications);
    vterm->pointerButtons = 0;
    vterm->mouseCell = PrUVec2{};
    vterm->scrollRemainder = PrVec2{};
    vterm->dropTextSize = 0;
}

uint64_t prVtermRevision(const PrVterm* vterm) {
    return vterm->revision;
}

uint64_t prVtermColorRevision(const PrVterm* vterm) {
    return vterm->colorRevision;
}

uint32_t prVtermScrollbackCount(const PrVterm* vterm) {
    return vterm->scrollback.count;
}

uint64_t prVtermScrollbackDropped(const PrVterm* vterm) {
    return vterm->scrollbackDropped;
}

size_t prVtermLinesMemorySize(const PrVterm* vterm) {
    size_t size = vtermScrollbackMemorySize(&vterm->scrollback);
    for (const VtermScreen& screen : vterm->screens) {
        const RawArray<VtermStoredLine>& slots = screen.rows.slots;
        size += sizeof(VtermStoredLine) * slots.capacity;
        for (uint32_t i = 0; i < slots.count; ++i) {
            const VtermStoredLine& line = slots.data[i];
            size += sizeof(PrVtermCell) * line.cells.capacity + sizeof(uint32_t) * line.codePoints.capacity +
                sizeof(PrVtermLinkIndex) * line.links.capacity;
        }
    }
    const RawArray<VtermLineCacheEntry>& entries = vterm->lineCache.entries;
    size += sizeof(VtermLineCacheEntry) * entries.capacity;
    for (const VtermLineCacheEntry& entry : entries.span())
        size += sizeof(PrVtermCell) * entry.cells.capacity + sizeof(uint32_t) * entry.codePoints.capacity;
    return size;
}

PrVtermLine prVtermLine(const PrVterm* vterm, uint32_t index) {
    return vtermLineRead(vterm, index);
}

void prVtermLineAppendText(const PrVterm* vterm, uint32_t index, uint32_t start, uint32_t end, PrRawString* out) {
    if (end > vterm->columns)
        end = vterm->columns;
    const VtermScrollback* scrollback = &vterm->scrollback;
    if (index < scrollback->count) {
        VtermPackedView view = vtermPackedView(scrollback, vtermScrollbackAt(scrollback, index));
        vtermPackedAppendText(view, start, end, out);
        for (uint32_t column = view.cellCount > start ? view.cellCount : start; column < end; ++column)
            prRawStringAppend(out, " ");
        return;
    }
    PrVtermLine line = prVtermLine(vterm, index);
    uint32_t stored = end < line.cellCount ? end : line.cellCount;
    for (uint32_t column = start; column < stored; ++column) {
        const PrVtermCell& cell = line.cells[column];
        if ((cell.flags & (PrVtermCellFlag_WideTail | PrVtermCellFlag_WideSpacer)) != 0)
            continue;
        char text[kPrUtf8MaxBytes];
        if (cell.codePoint == 0) {
            prRawStringAppend(out, " ");
            continue;
        }
        prRawStringAppend(out, PrStringView{text, prUtf8Encode(cell.codePoint, text)});
        for (uint32_t i = 0; i < cell.clusterCount; ++i) {
            uint32_t size = prUtf8Encode(line.codePoints[cell.clusterStart + i], text);
            prRawStringAppend(out, PrStringView{text, size});
        }
    }
    for (uint32_t column = stored > start ? stored : start; column < end; ++column)
        prRawStringAppend(out, " ");
}

PrVtermCursor prVtermCursor(const PrVterm* vterm) {
    return PrVtermCursor{
        .position = PrUVec2{vterm->cursor.column, vterm->cursor.row},
        .shape = vterm->cursorShape,
        .visible = vterm->modes.cursorVisible,
        .blinking = vterm->modes.cursorBlinking,
    };
}

PrVtermModeFlags prVtermModes(const PrVterm* vterm) {
    const VtermModes& modes = vterm->modes;
    PrVtermModeFlags flags = 0;
    if (vterm->active == VtermScreenKind_Alternate)
        flags |= PrVtermModeFlag_AlternateScreen;
    if (modes.reverseVideo)
        flags |= PrVtermModeFlag_ReverseVideo;
    if (modes.bracketedPaste)
        flags |= PrVtermModeFlag_BracketedPaste;
    if (modes.focusReports)
        flags |= PrVtermModeFlag_FocusReports;
    if (vterm->mouseTracking != VtermMouseTracking_None)
        flags |= PrVtermModeFlag_MouseReports;
    if (modes.synchronized)
        flags |= PrVtermModeFlag_Synchronized;
    return flags;
}

PrVtermEventFlags prVtermTakeEvents(PrVterm* vterm) {
    PrVtermEventFlags events = vterm->events;
    vterm->events = 0;
    return events;
}

PrStringView prVtermTitle(const PrVterm* vterm) {
    return prRawStringView(&vterm->title);
}

PrStringView prVtermWorkingDirectory(const PrVterm* vterm) {
    return prRawStringView(&vterm->workingDirectory);
}

PrStringView prVtermClipboard(const PrVterm* vterm) {
    return prRawStringView(&vterm->clipboard);
}

PrVtermLink prVtermLink(const PrVterm* vterm, PrVtermLinkIndex link) {
    return vtermLinksAt(&vterm->links, link);
}

uint32_t prVtermTakeNotifications(PrVterm* vterm, PrVtermNotification* PR_NULLABLE out, uint32_t capacity) {
    return vtermNotificationsTake(&vterm->notifications, out, capacity);
}

PrVtermShellInfo prVtermShellInfo(const PrVterm* vterm) {
    return vterm->shell;
}

PrColor8 prVtermPaletteColor(const PrVterm* vterm, uint32_t index) {
    PR_ASSERT(index < kPrVtermPaletteCount);
    return vterm->palette[index];
}

PrColor8 prVtermRoleColor(const PrVterm* vterm, PrVtermColorRole role) {
    PR_ASSERT(role <= PrVtermColorRole_Cursor);
    return vterm->roles[role];
}

void prVtermPaletteSetColor(PrVterm* vterm, uint32_t index, PrColor8 color) {
    PR_ASSERT(index < kPrVtermPaletteCount);
    vterm->palette[index] = color;
    vterm->defaultPalette[index] = color;
    colorsChanged(vterm);
}

void prVtermRoleSetColor(PrVterm* vterm, PrVtermColorRole role, PrColor8 color) {
    PR_ASSERT(role <= PrVtermColorRole_Cursor);
    vterm->roles[role] = color;
    vterm->defaultRoles[role] = color;
    colorsChanged(vterm);
}

PrColor8 prVtermColorResolve(const PrVterm* vterm, PrVtermColor color, PrVtermColorRole role) {
    switch (color.kind) {
        case PrVtermColorKind_Palette:
            return vterm->palette[prVtermColorIndex(color)];
        case PrVtermColorKind_Rgb:
            return PrColor8{color.red, color.green, color.blue, 0xff};
        default:
            return prVtermRoleColor(vterm, role);
    }
}

PrAppColorScheme prVtermColorScheme(const PrVterm* vterm) {
    return vterm->colorScheme;
}

void prVtermSetColorScheme(PrVterm* vterm, PrAppColorScheme scheme) {
    PR_ASSERT(scheme <= PrAppColorScheme_Dark);
    if (scheme == vterm->colorScheme)
        return;
    vterm->colorScheme = scheme;
    if (vterm->modes.colorSchemeReports)
        vtermColorSchemeReport(vterm);
}

PrByteView prVtermOutput(const PrVterm* vterm) {
    return PrByteView{vterm->output.data, vterm->output.count};
}

void prVtermOutputConsume(PrVterm* vterm, size_t size) {
    PR_ASSERT(size <= vterm->output.count);
    vterm->output.erase(0, uint32_t(size));
}
