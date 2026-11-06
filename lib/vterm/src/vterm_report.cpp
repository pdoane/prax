// The terminal's answers to a program's requests for its settings and its capabilities, and its reports of its color
// scheme.

#include "vterm/src/vterm_report.h"

#include "pr/ascii.h"

namespace pr_impl {

namespace {

// Settings (DECRQSS)

// Writes the SGR parameters that select `color` for one role. `extended` is the role's parameter for an extended color.
// `named` and `bright` are its parameters for the first eight named colors and for the other eight, or zero if the role
// has no parameters for named colors.
void putPenColor(Vterm* vterm, PrVtermColor color, uint32_t extended, uint32_t named, uint32_t bright) {
    switch (color.kind) {
        case PrVtermColorKind_Palette: {
            uint8_t index = prVtermColorIndex(color);
            vtermPutByte(vterm, ';');
            if (named != 0 && index < 8) {
                vtermPutNumber(vterm, named + index);
            } else if (named != 0 && index < 16) {
                vtermPutNumber(vterm, bright + index - 8);
            } else {
                vtermPutNumber(vterm, extended);
                vtermPut(vterm, ":5:");
                vtermPutNumber(vterm, index);
            }
            break;
        }
        case PrVtermColorKind_Rgb:
            vtermPutByte(vterm, ';');
            vtermPutNumber(vterm, extended);
            vtermPut(vterm, ":2::");
            vtermPutNumber(vterm, color.red);
            vtermPutByte(vterm, ':');
            vtermPutNumber(vterm, color.green);
            vtermPutByte(vterm, ':');
            vtermPutNumber(vterm, color.blue);
            break;
        default:
            break;
    }
}

// A cell attribute and the SGR parameter that sets it.
struct PenFlag {
    PrVtermCellFlags flag;
    uint8_t parameter;
};

constexpr PenFlag kPenFlags[] = {
    {PrVtermCellFlag_Bold, 1},
    {PrVtermCellFlag_Faint, 2},
    {PrVtermCellFlag_Italic, 3},
    {PrVtermCellFlag_Blink, 5},
    {PrVtermCellFlag_Inverse, 7},
    {PrVtermCellFlag_Invisible, 8},
    {PrVtermCellFlag_Strikethrough, 9},
    {PrVtermCellFlag_Overline, 53},
};

// Writes the SGR parameters that build the pen after a reset. Extended colors and underline styles use the colon form,
// so that a program reads each back as one parameter.
void putPen(Vterm* vterm) {
    const VtermPen& pen = vterm->cursor.pen;
    vtermPutByte(vterm, '0');
    for (const PenFlag& entry : kPenFlags) {
        if ((pen.flags & entry.flag) == 0)
            continue;
        vtermPutByte(vterm, ';');
        vtermPutNumber(vterm, entry.parameter);
    }
    if (pen.underline == PrVtermUnderline_Single) {
        vtermPut(vterm, ";4");
    } else if (pen.underline != PrVtermUnderline_None) {
        vtermPut(vterm, ";4:");
        vtermPutNumber(vterm, pen.underline);
    }
    putPenColor(vterm, pen.foreground, 38, 30, 90);
    putPenColor(vterm, pen.background, 48, 40, 100);
    putPenColor(vterm, pen.underlineColor, 58, 0, 0);
}

// Writes the top and bottom margins, counted from one.
void putScrollRegion(Vterm* vterm) {
    vtermPutNumber(vterm, vterm->top + 1);
    vtermPutByte(vterm, ';');
    vtermPutNumber(vterm, vterm->bottom);
}

// Writes the left and right margins, counted from one. They are the screen's edges unless DECLRMM lets DECSLRM set
// them.
void putMargins(Vterm* vterm) {
    vtermPutNumber(vterm, vterm->left + 1);
    vtermPutByte(vterm, ';');
    vtermPutNumber(vterm, vterm->right);
}

// Writes the DECSCUSR style of the cursor's shape and blinking. Odd styles blink and even styles are steady.
void putCursorStyle(Vterm* vterm) {
    uint32_t style = 1;
    if (vterm->cursorShape == PrVtermCursorShape_Underline)
        style = 3;
    else if (vterm->cursorShape == PrVtermCursorShape_Bar)
        style = 5;
    vtermPutNumber(vterm, vterm->modes.cursorBlinking ? style : style + 1);
}

// Writes the conformance level of a VT220, as the primary device attributes report, with 7-bit controls.
void putConformance(Vterm* vterm) {
    vtermPut(vterm, "62;1");
}

// A setting that DECRQSS reports. `name` is the intermediate and final bytes of the control function that sets it, and
// `put` writes the function's parameters.
struct Setting {
    PrStringView name;
    void (*put)(Vterm* vterm);
};

constexpr Setting kSettings[] = {
    {"m", putPen},              // SGR
    {"r", putScrollRegion},     // DECSTBM
    {"s", putMargins},          // DECSLRM
    {" q", putCursorStyle},     // DECSCUSR
    {"\"p", putConformance},    // DECSCL
};

// Answers a request for the setting of the control function `name` with the sequence that restores it. Answers that the
// request is invalid if the terminal has no such setting.
void settingReport(Vterm* vterm, PrStringView name) {
    for (const Setting& setting : kSettings) {
        if (setting.name == name) {
            vtermPut(vterm, "\x1bP1$r");
            setting.put(vterm);
            vtermPut(vterm, name);
            vtermPut(vterm, "\x1b\\");
            return;
        }
    }
    vtermPut(vterm, "\x1bP0$r\x1b\\");
}

// Capabilities (XTGETTCAP)

// A terminfo capability, by its terminfo or termcap name. Its value is what the terminal's terminfo entry would give,
// with escapes already turned into bytes. A boolean capability's value is the null view.
struct Capability {
    PrStringView name;
    PrStringView value;
};

// The terminal's name is the terminfo entry whose capabilities it has, which a host sets in TERM. The other
// capabilities are extensions to that entry that the terminal implements, under the names that programs look for.
constexpr Capability kCapabilities[] = {
    {"TN", "xterm-256color"},
    {"name", "xterm-256color"},
    {"Co", "256"},
    {"colors", "256"},
    {"RGB", {}},
    {"Tc", {}},
    {"setrgbf", "\x1b[38;2;%p1%d;%p2%d;%p3%dm"},
    {"setrgbb", "\x1b[48;2;%p1%d;%p2%d;%p3%dm"},
    {"Su", {}},
    {"Smulx", "\x1b[4:%p1%dm"},
    {"Setulc", "\x1b[58:2::%p1%{65536}%/%d:%p1%{256}%/%{255}%&%d:%p1%{255}%&%d%;m"},
    {"smxx", "\x1b[9m"},
    {"rmxx", "\x1b[29m"},
    {"Smol", "\x1b[53m"},
    {"Rmol", "\x1b[55m"},
    {"Ss", "\x1b[%p1%d q"},
    {"Se", "\x1b[2 q"},
    {"Cs", "\x1b]12;%p1%s\x07"},
    {"Cr", "\x1b]112\x07"},
    {"Ms", "\x1b]52;%p1%s;%p2%s\x07"},
    {"Sync", "\x1b[?2026%?%p1%{1}%-%tl%eh%;"},
    {"BE", "\x1b[?2004h"},
    {"BD", "\x1b[?2004l"},
    {"PS", "\x1b[200~"},
    {"PE", "\x1b[201~"},
    {"fe", "\x1b[?1004h"},
    {"fd", "\x1b[?1004l"},
    {"kxIN", "\x1b[I"},
    {"kxOUT", "\x1b[O"},
    {"XT", {}},
};

// The most bytes in a capability's name. No capability has a longer name.
constexpr uint32_t kCapabilityNameMax = 16;

// Decodes the hexadecimal `hex` into `out`, which holds kCapabilityNameMax bytes, and writes the decoded size to
// `*size`. Returns false if `hex` is not whole bytes of hexadecimal digits, or is longer than a name.
bool hexDecode(PrStringView hex, char* out, uint32_t* size) {
    if (hex.size % 2 != 0 || hex.size / 2 > kCapabilityNameMax)
        return false;
    for (size_t i = 0; i < hex.size; ++i) {
        if (!prAsciiIsHexDigit(hex.data[i]))
            return false;
    }
    for (size_t i = 0; i < hex.size; i += 2)
        out[i / 2] = char(prAsciiHexValue(hex.data[i]) << 4 | prAsciiHexValue(hex.data[i + 1]));
    *size = uint32_t(hex.size / 2);
    return true;
}

void putHex(Vterm* vterm, PrStringView text) {
    constexpr char kDigits[] = "0123456789ABCDEF";
    for (size_t i = 0; i < text.size; ++i) {
        uint8_t byte = uint8_t(text.data[i]);
        vtermPutByte(vterm, uint8_t(kDigits[byte >> 4]));
        vtermPutByte(vterm, uint8_t(kDigits[byte & 15]));
    }
}

// Answers a request for a capability with its value. `hex` is the capability's name in hexadecimal. Answers that the
// request is invalid if the terminal does not have the capability. The reply names the capability with the request's
// own hexadecimal.
void capabilityReport(Vterm* vterm, PrStringView hex) {
    char name[kCapabilityNameMax];
    uint32_t size = 0;
    if (hexDecode(hex, name, &size)) {
        for (const Capability& capability : kCapabilities) {
            if (capability.name != PrStringView{name, size})
                continue;
            vtermPut(vterm, "\x1bP1+r");
            vtermPut(vterm, hex);
            if (capability.value.data != nullptr) {
                vtermPutByte(vterm, '=');
                putHex(vterm, capability.value);
            }
            vtermPut(vterm, "\x1b\\");
            return;
        }
    }
    vtermPut(vterm, "\x1bP0+r\x1b\\");
}

} // namespace

void vtermDeviceControl(Vterm* vterm, const VtermAction* action) {
    if (action->marker != 0 || action->intermediateCount != 1 || action->final != 'q')
        return;
    switch (action->intermediates[0]) {
        case '$':
            settingReport(vterm, action->text);
            break;
        case '+': {
            // Each name gets a reply of its own, in the order of the request. A program can then match an invalid
            // reply, which names no capability, to its request.
            PrStringView rest = action->text;
            while (rest.size != 0) {
                size_t end = 0;
                while (end < rest.size && rest.data[end] != ';')
                    ++end;
                capabilityReport(vterm, PrStringView{rest.data, end});
                size_t next = end < rest.size ? end + 1 : end;
                rest = PrStringView{rest.data + next, rest.size - next};
            }
            break;
        }
        default:
            break;
    }
}

void vtermColorSchemeReport(Vterm* vterm) {
    vtermPut(vterm, vterm->colorScheme == PrAppColorScheme_Dark ? "\x1b[?997;1n" : "\x1b[?997;2n");
}

} // namespace pr_impl
