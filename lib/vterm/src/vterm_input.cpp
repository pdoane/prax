// A terminal's input.

#include "pr/vterm_input.h"

#include "pr/float.h"
#include "pr/mem.h"
#include "pr/unicode.h"
#include "pr/utf8.h"

#include "vterm/src/vterm_screen.h"

namespace pr_impl {

namespace {

constexpr uint8_t kEscape = 0x1b;

// The codes of mouse reports: the buttons, the wheel directions, and the bits that modifiers and motion add.
constexpr uint32_t kMouseLeft = 0;
constexpr uint32_t kMouseMiddle = 1;
constexpr uint32_t kMouseRight = 2;
constexpr uint32_t kMouseNone = 3;
constexpr uint32_t kMouseWheelUp = 64;
constexpr uint32_t kMouseWheelDown = 65;
constexpr uint32_t kMouseWheelLeft = 66;
constexpr uint32_t kMouseWheelRight = 67;
constexpr uint32_t kMouseShift = 4;
constexpr uint32_t kMouseAlt = 8;
constexpr uint32_t kMouseControl = 16;
constexpr uint32_t kMouseMotion = 32;

// The flags of kitty's keyboard protocol in effect on the screen that shows.
uint8_t keyboardFlags(Vterm* vterm) {
    VtermScreen& screen = vtermScreen(vterm);
    return screen.keyboardCount != 0 ? screen.keyboard[screen.keyboardCount - 1] : 0;
}

// The modifier bits of the keyboard protocol. Shift, Alt, Control and Super have the same bits in xterm's modifier
// parameters. Only the protocol has the bits of Caps Lock and Num Lock.
constexpr uint32_t kModifierShift = 1;
constexpr uint32_t kModifierAlt = 2;
constexpr uint32_t kModifierControl = 4;
constexpr uint32_t kModifierSuper = 8;
constexpr uint32_t kModifierCapsLock = 64;
constexpr uint32_t kModifierLocks = 64 | 128;

// The modifiers as bits of the keyboard protocol. PrAppModifierFlags has no Num Lock, so that bit is never set.
uint32_t modifierBits(PrAppModifierFlags modifiers) {
    uint32_t bits = 0;
    if ((modifiers & PrAppModifierFlag_Shift) != 0)
        bits |= kModifierShift;
    if ((modifiers & PrAppModifierFlag_Alt) != 0)
        bits |= kModifierAlt;
    if ((modifiers & PrAppModifierFlag_Control) != 0)
        bits |= kModifierControl;
    if ((modifiers & PrAppModifierFlag_Super) != 0)
        bits |= kModifierSuper;
    if ((modifiers & PrAppModifierFlag_CapsLock) != 0)
        bits |= kModifierCapsLock;
    return bits;
}

// The modifiers as a parameter of a legacy key sequence: one more than their bits. The legacy encodings have no bits
// for the locks, so they are left out.
uint32_t modifierParam(PrAppModifierFlags modifiers) {
    return 1 + (modifierBits(modifiers) & ~kModifierLocks);
}

// Sends a cursor key or another key that ends in a letter. Without modifiers it is SS3 letter in application mode, or
// CSI letter. With modifiers it is CSI 1;m letter.
void putLetterKey(Vterm* vterm, char letter, uint32_t modifiers, bool application) {
    if (modifiers == 1) {
        vtermPut(vterm, application ? "\x1bO" : "\x1b[");
    } else {
        vtermPut(vterm, "\x1b[1;");
        vtermPutNumber(vterm, modifiers);
    }
    vtermPutByte(vterm, uint8_t(letter));
}

// Sends a key of the form CSI n ~, or CSI n;m ~ with modifiers.
void putTildeKey(Vterm* vterm, uint32_t number, uint32_t modifiers) {
    vtermPut(vterm, "\x1b[");
    vtermPutNumber(vterm, number);
    if (modifiers != 1) {
        vtermPutByte(vterm, ';');
        vtermPutNumber(vterm, modifiers);
    }
    vtermPutByte(vterm, '~');
}

// The numbers of F5 to F12 in their sequences CSI n ~.
constexpr uint8_t kFunctionTildeNumbers[8] = {15, 17, 18, 19, 20, 21, 23, 24};

// The control character that Control makes of a printable key, or -1 if the key has none.
int32_t controlCharacter(uint32_t key) {
    if (key >= 'A' && key <= 'Z')
        return int32_t(key - 'A' + 1);
    switch (key) {
        case ' ':
        case '@':
        case '2':
            return 0;
        case '[':
        case '3':
            return 0x1b;
        case '\\':
        case '4':
            return 0x1c;
        case ']':
        case '5':
            return 0x1d;
        case '^':
        case '6':
            return 0x1e;
        case '_':
        case '-':
        case '/':
        case '7':
            return 0x1f;
        case '?':
        case '8':
            return 0x7f;
        default:
            return -1;
    }
}

// Sends a named key in the legacy encoding. Returns false if the key sends nothing.
bool namedKey(Vterm* vterm, PrAppKey key, PrAppModifierFlags modifiers) {
    uint32_t param = modifierParam(modifiers);
    bool alt = (modifiers & PrAppModifierFlag_Alt) != 0;
    bool application = vterm->modes.cursorKeys;
    switch (key) {
        case PrAppKey_Enter:
            if (alt)
                vtermPutByte(vterm, kEscape);
            vtermPut(vterm, vterm->modes.newline ? "\r\n" : "\r");
            return true;
        case PrAppKey_Tab:
            if (alt)
                vtermPutByte(vterm, kEscape);
            vtermPut(vterm, (modifiers & PrAppModifierFlag_Shift) != 0 ? "\x1b[Z" : "\t");
            return true;
        case PrAppKey_Backspace:
            if (alt)
                vtermPutByte(vterm, kEscape);
            vtermPutByte(vterm, (modifiers & PrAppModifierFlag_Control) != 0 ? 0x08 : 0x7f);
            return true;
        case PrAppKey_Escape:
            if (alt)
                vtermPutByte(vterm, kEscape);
            vtermPutByte(vterm, kEscape);
            return true;
        case PrAppKey_Up:
            putLetterKey(vterm, 'A', param, application);
            return true;
        case PrAppKey_Down:
            putLetterKey(vterm, 'B', param, application);
            return true;
        case PrAppKey_Right:
            putLetterKey(vterm, 'C', param, application);
            return true;
        case PrAppKey_Left:
            putLetterKey(vterm, 'D', param, application);
            return true;
        case PrAppKey_Home:
            putLetterKey(vterm, 'H', param, application);
            return true;
        case PrAppKey_End:
            putLetterKey(vterm, 'F', param, application);
            return true;
        case PrAppKey_Insert:
            putTildeKey(vterm, 2, param);
            return true;
        case PrAppKey_Delete:
            putTildeKey(vterm, 3, param);
            return true;
        case PrAppKey_PageUp:
            putTildeKey(vterm, 5, param);
            return true;
        case PrAppKey_PageDown:
            putTildeKey(vterm, 6, param);
            return true;
        case PrAppKey_ContextMenu:
            // xterm's F16, which terminfo names kf16.
            putTildeKey(vterm, 29, param);
            return true;
        case PrAppKey_F1:
        case PrAppKey_F2:
        case PrAppKey_F3:
        case PrAppKey_F4:
            putLetterKey(vterm, char('P' + (key - PrAppKey_F1)), param, true);
            return true;
        default:
            break;
    }
    if (key >= PrAppKey_F5 && key <= PrAppKey_F12) {
        putTildeKey(vterm, kFunctionTildeNumbers[key - PrAppKey_F5], param);
        return true;
    }
    return false;
}

// Sends a printable key in the legacy encoding. Returns false if the key's Text event should send its text instead.
bool printableKey(Vterm* vterm, const PrAppEvent* event, const PrVtermInputDesc* desc) {
    uint32_t key = event->keyboard.key;
    PrAppModifierFlags modifiers = event->modifiers;
    bool control = (modifiers & PrAppModifierFlag_Control) != 0;
    bool meta = (modifiers & PrAppModifierFlag_Alt) != 0 && desc->altSendsEscape;
    if ((modifiers & PrAppModifierFlag_Super) != 0 || (!control && !meta))
        return false;
    uint32_t character = 0;
    if (control) {
        int32_t code = controlCharacter(key);
        if (code < 0)
            return false;
        character = uint32_t(code);
    } else if ((modifiers & PrAppModifierFlag_Shift) == 0) {
        character = key >= 'A' && key <= 'Z' ? key - 'A' + 'a' : key;
    } else if (key >= 'A' && key <= 'Z') {
        character = key;
    } else {
        // A shifted key other than a letter has the character that the layout gives it. The text holds that character
        // unless Alt changed it too.
        uint32_t codePoint = 0;
        if (event->keyboard.text.size == 0 ||
                prUtf8Decode(event->keyboard.text, 0, &codePoint) != event->keyboard.text.size || codePoint >= 0x80)
            return false;
        character = codePoint;
    }
    if (meta)
        vtermPutByte(vterm, kEscape);
    vtermPutByte(vterm, uint8_t(character));
    return true;
}

// The kinds of key event, numbered as the keyboard protocol numbers its event types.
enum {
    KeyAction_Press = 1,
    KeyAction_Repeat = 2,
    KeyAction_Release = 3,
};
typedef uint8_t KeyAction;

// A key event as an escape code of the keyboard protocol: CSI number:shifted:base;modifiers:action;text, then the final
// byte. A field that holds its default is left out, as is a parameter whose fields all hold their defaults.
struct KittyKey {
    uint32_t number;
    char final;
    uint32_t shifted;           // the character that the key types with Shift, or zero
    uint32_t base;              // the character at the key's position on the PC-101 layout, or zero
    uint32_t modifiers;         // the protocol's bits
    KeyAction action;
    PrStringView text;          // the UTF-8 text that the key typed, which the code carries without control characters
};

// Returns true if a code point is a C0 or C1 control character, or DEL. The protocol's associated text excludes them.
bool isControl(uint32_t codePoint) {
    return codePoint < 0x20 || (codePoint >= 0x7f && codePoint <= 0x9f);
}

// Returns true if `text` holds a code point other than a control character.
bool hasPrintable(PrStringView text) {
    size_t at = 0;
    while (at < text.size) {
        uint32_t codePoint = 0;
        at += prUtf8Decode(text, at, &codePoint);
        if (!isControl(codePoint))
            return true;
    }
    return false;
}

void putKittyKey(Vterm* vterm, const KittyKey& key) {
    bool hasText = hasPrintable(key.text);
    bool hasAlternates = key.shifted != 0 || key.base != 0;
    bool hasModifiers = key.modifiers != 0 || key.action != KeyAction_Press;
    vtermPut(vterm, "\x1b[");
    if (key.number != 1 || hasAlternates || hasModifiers || hasText)
        vtermPutNumber(vterm, key.number);
    if (hasAlternates) {
        vtermPutByte(vterm, ':');
        if (key.shifted != 0)
            vtermPutNumber(vterm, key.shifted);
        if (key.base != 0) {
            vtermPutByte(vterm, ':');
            vtermPutNumber(vterm, key.base);
        }
    }
    if (hasModifiers || hasText)
        vtermPutByte(vterm, ';');
    if (hasModifiers) {
        vtermPutNumber(vterm, 1 + key.modifiers);
        if (key.action != KeyAction_Press) {
            vtermPutByte(vterm, ':');
            vtermPutNumber(vterm, key.action);
        }
    }
    if (hasText) {
        uint8_t separator = ';';
        size_t at = 0;
        while (at < key.text.size) {
            uint32_t codePoint = 0;
            at += prUtf8Decode(key.text, at, &codePoint);
            if (isControl(codePoint))
                continue;
            vtermPutByte(vterm, separator);
            vtermPutNumber(vterm, codePoint);
            separator = ':';
        }
    }
    vtermPutByte(vterm, uint8_t(key.final));
}

// The protocol's number for a modifier or lock key, or zero for another key. Writes a modifier key's own bit to `*bit`,
// or zero for a lock key.
uint32_t modifierKeyNumber(PrAppKey key, PrAppKeyCode code, uint32_t* bit) {
    *bit = 0;
    switch (key) {
        case PrAppKey_Shift:
            *bit = kModifierShift;
            return code == PrAppKeyCode_ShiftRight ? 57447 : 57441;
        case PrAppKey_Control:
            *bit = kModifierControl;
            return code == PrAppKeyCode_ControlRight ? 57448 : 57442;
        case PrAppKey_Alt:
            *bit = kModifierAlt;
            return code == PrAppKeyCode_AltRight ? 57449 : 57443;
        case PrAppKey_Meta:
            *bit = kModifierSuper;
            return code == PrAppKeyCode_MetaRight ? 57450 : 57444;
        case PrAppKey_CapsLock:
            return 57358;
        case PrAppKey_NumLock:
            return 57360;
        case PrAppKey_Unknown:
            return code == PrAppKeyCode_ScrollLock ? 57359 : 0;
        default:
            return 0;
    }
}

// Fills the number and final byte of a keypad key, from its position and its meaning. A key that acts as a cursor key
// without Num Lock reports as the keypad's cursor key. Returns false if the key is not on the keypad.
bool keypadKey(PrAppKey key, PrAppKeyCode code, KittyKey* out) {
    struct Keypad {
        PrAppKeyCode code;
        uint16_t number;
    };
    static const Keypad kKeypad[] = {
        {PrAppKeyCode_NumpadDecimal, 57409}, {PrAppKeyCode_NumpadDivide, 57410},
        {PrAppKeyCode_NumpadMultiply, 57411}, {PrAppKeyCode_NumpadSubtract, 57412}, {PrAppKeyCode_NumpadAdd, 57413},
        {PrAppKeyCode_NumpadEnter, 57414}, {PrAppKeyCode_NumpadEqual, 57415}, {PrAppKeyCode_NumpadComma, 57416},
    };
    struct Cursor {
        PrAppKey key;
        uint16_t number;
    };
    static const Cursor kCursor[] = {
        {PrAppKey_Left, 57417}, {PrAppKey_Right, 57418}, {PrAppKey_Up, 57419}, {PrAppKey_Down, 57420},
        {PrAppKey_PageUp, 57421}, {PrAppKey_PageDown, 57422}, {PrAppKey_Home, 57423}, {PrAppKey_End, 57424},
        {PrAppKey_Insert, 57425}, {PrAppKey_Delete, 57426},
    };
    bool digit = code >= PrAppKeyCode_Numpad0 && code <= PrAppKeyCode_Numpad9;
    out->final = 'u';
    if (digit || code == PrAppKeyCode_NumpadDecimal) {
        for (const Cursor& cursor : kCursor) {
            if (cursor.key == key) {
                out->number = cursor.number;
                return true;
            }
        }
        if (key == PrAppKey_Clear) {
            // Begin, which the 5 key sends without Num Lock.
            out->number = 1;
            out->final = 'E';
            return true;
        }
    }
    if (digit) {
        out->number = 57399 + uint32_t(code - PrAppKeyCode_Numpad0);
        return true;
    }
    for (const Keypad& keypad : kKeypad) {
        if (keypad.code == code) {
            out->number = keypad.number;
            return true;
        }
    }
    return false;
}

// Fills the number and final byte of a key in the protocol's table of functional keys. The keypad, modifier and lock
// keys are handled elsewhere. Returns false if the key is not in the table.
bool functionalKey(PrAppKey key, PrAppKeyCode code, KittyKey* out) {
    struct Functional {
        PrAppKey key;
        uint16_t number;
        char final;
    };
    static const Functional kFunctional[] = {
        {PrAppKey_Escape, 27, 'u'}, {PrAppKey_Enter, 13, 'u'}, {PrAppKey_Tab, 9, 'u'},
        {PrAppKey_Backspace, 127, 'u'}, {PrAppKey_Insert, 2, '~'}, {PrAppKey_Delete, 3, '~'},
        {PrAppKey_Left, 1, 'D'}, {PrAppKey_Right, 1, 'C'}, {PrAppKey_Up, 1, 'A'}, {PrAppKey_Down, 1, 'B'},
        {PrAppKey_PageUp, 5, '~'}, {PrAppKey_PageDown, 6, '~'}, {PrAppKey_Home, 1, 'H'}, {PrAppKey_End, 1, 'F'},
        {PrAppKey_F1, 1, 'P'}, {PrAppKey_F2, 1, 'Q'}, {PrAppKey_F3, 13, '~'}, {PrAppKey_F4, 1, 'S'},
        {PrAppKey_ContextMenu, 57363, 'u'},
    };
    for (const Functional& functional : kFunctional) {
        if (functional.key == key) {
            out->number = functional.number;
            out->final = functional.final;
            return true;
        }
    }
    out->final = 'u';
    if (key >= PrAppKey_F5 && key <= PrAppKey_F12) {
        out->number = kFunctionTildeNumbers[key - PrAppKey_F5];
        out->final = '~';
    } else if (key >= PrAppKey_F13 && key <= PrAppKey_F20) {
        out->number = 57376 + uint32_t(key - PrAppKey_F13);
    } else if (key == PrAppKey_Unknown && code == PrAppKeyCode_PrintScreen) {
        out->number = 57361;
    } else if (key == PrAppKey_Unknown && code == PrAppKeyCode_Pause) {
        out->number = 57362;
    } else {
        return false;
    }
    return true;
}

// The lowercase character at a key's position on the PC-101 layout, or zero if that position types no character.
uint32_t baseLayoutKey(PrAppKeyCode code) {
    if (code >= PrAppKeyCode_KeyA && code <= PrAppKeyCode_KeyZ)
        return 'a' + uint32_t(code - PrAppKeyCode_KeyA);
    if (code >= PrAppKeyCode_Digit0 && code <= PrAppKeyCode_Digit9)
        return '0' + uint32_t(code - PrAppKeyCode_Digit0);
    switch (code) {
        case PrAppKeyCode_Backquote:
            return '`';
        case PrAppKeyCode_Backslash:
            return '\\';
        case PrAppKeyCode_BracketLeft:
            return '[';
        case PrAppKeyCode_BracketRight:
            return ']';
        case PrAppKeyCode_Comma:
            return ',';
        case PrAppKeyCode_Equal:
            return '=';
        case PrAppKeyCode_Minus:
            return '-';
        case PrAppKeyCode_Period:
            return '.';
        case PrAppKeyCode_Quote:
            return '\'';
        case PrAppKeyCode_Semicolon:
            return ';';
        case PrAppKeyCode_Slash:
            return '/';
        case PrAppKeyCode_Space:
            return ' ';
        default:
            return 0;
    }
}

// The character that Shift types on the key of `base` in the PC-101 layout.
uint32_t baseLayoutShifted(uint32_t base) {
    if (base >= 'a' && base <= 'z')
        return base - 'a' + 'A';
    static const char kUnshifted[] = "`1234567890-=[]\\;',./";
    static const char kShifted[] = "~!@#$%^&*()_+{}|:\"<>?";
    for (uint32_t i = 0; kUnshifted[i] != 0; ++i) {
        if (uint32_t(kUnshifted[i]) == base)
            return uint32_t(kShifted[i]);
    }
    return base;
}

// The lowercase or uppercase form of a code point, or the code point itself if its full mapping is not one code point.
uint32_t singleCase(uint32_t codePoint, PrUnicodeCase target) {
    uint32_t mapped[kPrUnicodeCaseMaxCodePoints];
    uint32_t count = 0;
    if (target == PrUnicodeCase_Upper)
        count = prUnicodeUppercase(codePoint, mapped);
    else
        count = prUnicodeLowercase(codePoint, mapped);
    return count == 1 ? mapped[0] : codePoint;
}

// Fills the number of a key that types a character, and its alternate keys. Returns false if the key types nothing.
//
// The number is the character without Shift, in lowercase. PrAppEvent gives the key with Shift and the layout's third
// level applied, so the terminal unshifts a symbol by its position on the PC-101 layout. That is exact if the layout
// agrees with PC-101. Otherwise the symbol reports in its shifted form. The number of a non-ASCII key is its text in
// lowercase, and is zero if the text is more than one character. If Shift is held, the shifted key is the number in
// uppercase, or else the text.
bool characterKey(const PrAppEvent* event, KittyKey* out) {
    uint32_t typed = 0;
    if (event->keyboard.text.size == 0 || prUtf8Decode(event->keyboard.text, 0, &typed) != event->keyboard.text.size ||
            isControl(typed))
        typed = 0;
    uint32_t base = baseLayoutKey(event->keyboard.code);
    bool shift = (event->modifiers & PrAppModifierFlag_Shift) != 0;
    uint32_t number = 0;
    if (event->keyboard.key >= 'A' && event->keyboard.key <= 'Z') {
        number = event->keyboard.key - 'A' + 'a';
    } else if (event->keyboard.key != PrAppKey_Unknown && event->keyboard.key < 0x80) {
        number = event->keyboard.key;
        if (shift && base != 0 && baseLayoutShifted(base) == number)
            number = base;
    } else if (typed != 0) {
        number = singleCase(typed, PrUnicodeCase_Lower);
    } else if (!hasPrintable(event->keyboard.text)) {
        return false;
    }
    out->number = number;
    out->final = 'u';
    if (shift) {
        uint32_t upper = singleCase(number, PrUnicodeCase_Upper);
        if (upper != number)
            out->shifted = upper;
        else if (typed != number)
            out->shifted = typed;
    }
    out->base = base != number ? base : 0;
    return true;
}

// Sends a key's press, repeat or release. Returns false if the event sends nothing, for example if the key's Text event
// should send its text.
bool keyEvent(Vterm* vterm, const PrAppEvent* event, const PrVtermInputDesc* desc) {
    uint8_t flags = keyboardFlags(vterm);
    bool disambiguate = (flags & kVtermKeyboardDisambiguate) != 0;
    bool allKeys = (flags & kVtermKeyboardAllKeys) != 0;
    KeyAction action = KeyAction_Press;
    if (event->kind == PrAppEventKind_KeyUp) {
        if ((flags & kVtermKeyboardEvents) == 0)
            return false;
        action = KeyAction_Release;
    } else if (event->keyboard.repeat && (flags & kVtermKeyboardEvents) != 0) {
        action = KeyAction_Repeat;
    }

    // Without the protocol's escape codes, a key uses its legacy encoding, in which a keypad key sends the key that it
    // means. A release is reported only with event types, so it never uses this encoding.
    if ((flags & (kVtermKeyboardDisambiguate | kVtermKeyboardEvents | kVtermKeyboardAllKeys)) == 0) {
        if (event->keyboard.key >= PrAppKey_Left)
            return namedKey(vterm, event->keyboard.key, event->modifiers);
        return event->keyboard.key != PrAppKey_Unknown && printableKey(vterm, event, desc);
    }

    uint32_t modifiers = modifierBits(event->modifiers);
    KittyKey key{.action = action};
    uint32_t bit = 0;
    uint32_t modifierNumber = modifierKeyNumber(event->keyboard.key, event->keyboard.code, &bit);
    if (modifierNumber != 0) {
        if (!allKeys)
            return false;
        // A press includes the key's own modifier. A platform may report that modifier only from the next event.
        key.number = modifierNumber;
        key.final = 'u';
        key.modifiers = action == KeyAction_Release ? modifiers : modifiers | bit;
        putKittyKey(vterm, key);
        return true;
    }
    bool keypad = (disambiguate || allKeys) && keypadKey(event->keyboard.key, event->keyboard.code, &key);
    bool character = false;
    if (!keypad && !functionalKey(event->keyboard.key, event->keyboard.code, &key)) {
        if (event->keyboard.key >= PrAppKey_Left || !characterKey(event, &key))
            return false;
        character = true;
    }

    // If Alt selects the layout's third level, it belongs to the layout and is not a modifier of the text.
    bool typesText = hasPrintable(event->keyboard.text);
    if (typesText && !desc->altSendsEscape && (modifiers & (kModifierControl | kModifierSuper)) == 0)
        modifiers &= ~kModifierAlt;
    uint32_t held = modifiers & ~kModifierLocks;
    bool textual = typesText && (held & ~kModifierShift) == 0;
    key.modifiers = modifiers;

    if (!allKeys) {
        // A key that types text sends the text, and reports only its release as an escape code.
        if (textual && action != KeyAction_Release)
            return false;
        // Enter, Tab and Backspace keep their legacy bytes and report no release. A shell then stays usable if a
        // program exits and leaves the mode on.
        bool editing = !keypad && key.final == 'u' && (key.number == 13 || key.number == 9 || key.number == 127);
        if (editing && held == 0) {
            if (action == KeyAction_Release)
                return false;
            if (key.number == 13)
                vtermPut(vterm, vterm->modes.newline ? "\r\n" : "\r");
            else
                vtermPutByte(vterm, key.number == 9 ? '\t' : 0x7f);
            return true;
        }
        // With event types but without disambiguation, a press of Escape or of a character chord keeps its legacy
        // bytes.
        if (!disambiguate && action != KeyAction_Release) {
            if (!keypad && key.number == 27 && key.final == 'u' && held == 0) {
                vtermPutByte(vterm, kEscape);
                return true;
            }
            if (character && action == KeyAction_Press)
                return event->keyboard.key != PrAppKey_Unknown && printableKey(vterm, event, desc);
        }
    }
    if ((flags & kVtermKeyboardAlternates) == 0 || !character) {
        key.shifted = 0;
        key.base = 0;
    }
    if ((flags & kVtermKeyboardText) != 0 && textual && action != KeyAction_Release)
        key.text = event->keyboard.text;
    putKittyKey(vterm, key);
    return true;
}

// The cell under a position in the window, clamped to the screen.
PrUVec2 cellAt(const Vterm* vterm, PrVec2 position, const PrVtermInputDesc* desc) {
    float x = (position.x - desc->origin.x) / desc->cellExtent.x;
    float y = (position.y - desc->origin.y) / desc->cellExtent.y;
    uint32_t column = x > 0 ? uint32_t(x) : 0;
    uint32_t row = y > 0 ? uint32_t(y) : 0;
    return PrUVec2{column < vterm->columns ? column : vterm->columns - 1, row < vterm->rows ? row : vterm->rows - 1};
}

// A position in the window as an offset from the screen's origin, in logical pixels, clamped at zero.
PrUVec2 pixelAt(PrVec2 position, const PrVtermInputDesc* desc) {
    float x = position.x - desc->origin.x;
    float y = position.y - desc->origin.y;
    return PrUVec2{x > 0 ? uint32_t(x) : 0, y > 0 ? uint32_t(y) : 0};
}

// The report code of a PrAppEvent button, or kMouseNone. PrAppEvent numbers the buttons primary, secondary, middle.
uint32_t buttonCode(uint8_t button) {
    switch (button) {
        case 0:
            return kMouseLeft;
        case 1:
            return kMouseRight;
        case 2:
            return kMouseMiddle;
        default:
            return kMouseNone;
    }
}

// Writes one coordinate or code in the default encoding or the UTF-8 encoding. Returns false if the encoding cannot
// hold the value.
bool putMouseByte(Vterm* vterm, uint32_t value, bool utf8) {
    value += 32;
    if (!utf8) {
        if (value > 255)
            return false;
        vtermPutByte(vterm, uint8_t(value));
        return true;
    }
    if (value > 2047)
        return false;
    char text[kPrUtf8MaxBytes];
    vtermPut(vterm, PrStringView{text, prUtf8Encode(value, text)});
    return true;
}

// Sends a mouse report of `code`, with the bits of the modifiers, at `cell` or `pixel`.
void putMouse(Vterm* vterm, uint32_t code, bool release, PrUVec2 cell, PrUVec2 pixel, PrAppModifierFlags modifiers) {
    if (vterm->mouseTracking != VtermMouseTracking_Press) {
        if ((modifiers & PrAppModifierFlag_Shift) != 0)
            code |= kMouseShift;
        if ((modifiers & PrAppModifierFlag_Alt) != 0)
            code |= kMouseAlt;
        if ((modifiers & PrAppModifierFlag_Control) != 0)
            code |= kMouseControl;
    }
    switch (vterm->mouseEncoding) {
        case VtermMouseEncoding_Sgr:
        case VtermMouseEncoding_SgrPixels: {
            PrUVec2 at = vterm->mouseEncoding == VtermMouseEncoding_Sgr ? cell : pixel;
            vtermPut(vterm, "\x1b[<");
            vtermPutNumber(vterm, code);
            vtermPutByte(vterm, ';');
            vtermPutNumber(vterm, at.x + 1);
            vtermPutByte(vterm, ';');
            vtermPutNumber(vterm, at.y + 1);
            vtermPutByte(vterm, release ? 'm' : 'M');
            return;
        }
        case VtermMouseEncoding_Urxvt:
            if (release)
                code = (code & ~3u) | kMouseNone;
            vtermPut(vterm, "\x1b[");
            vtermPutNumber(vterm, code + 32);
            vtermPutByte(vterm, ';');
            vtermPutNumber(vterm, cell.x + 1);
            vtermPutByte(vterm, ';');
            vtermPutNumber(vterm, cell.y + 1);
            vtermPutByte(vterm, 'M');
            return;
        case VtermMouseEncoding_Default:
        case VtermMouseEncoding_Utf8: {
            if (release)
                code = (code & ~3u) | kMouseNone;
            bool utf8 = vterm->mouseEncoding == VtermMouseEncoding_Utf8;
            uint32_t start = vterm->output.count;
            vtermPut(vterm, "\x1b[M");
            if (!putMouseByte(vterm, code, false) || !putMouseByte(vterm, cell.x + 1, utf8) ||
                    !putMouseByte(vterm, cell.y + 1, utf8)) {
                // A position that the encoding cannot hold is not reported.
                vterm->output.count = start;
            }
            return;
        }
    }
}

bool pointerButton(Vterm* vterm, const PrAppEvent* event, const PrVtermInputDesc* desc) {
    bool press = event->kind == PrAppEventKind_PointerDown;
    if (event->pointer.button < 8) {
        uint8_t bit = uint8_t(1u << event->pointer.button);
        vterm->pointerButtons = press ? uint8_t(vterm->pointerButtons | bit) : uint8_t(vterm->pointerButtons & ~bit);
    }
    if (vterm->mouseTracking == VtermMouseTracking_None)
        return false;
    uint32_t code = buttonCode(event->pointer.button);
    if (code == kMouseNone || (!press && vterm->mouseTracking == VtermMouseTracking_Press))
        return true;
    PrUVec2 cell = cellAt(vterm, event->pointer.position, desc);
    putMouse(vterm, code, !press, cell, pixelAt(event->pointer.position, desc), event->modifiers);
    vterm->mouseCell = cell;
    vterm->mouseMoved = true;
    return true;
}

bool pointerMove(Vterm* vterm, const PrAppEvent* event, const PrVtermInputDesc* desc) {
    VtermMouseTracking tracking = vterm->mouseTracking;
    uint32_t code = kMouseNone;
    for (uint8_t button = 0; button < 3; ++button) {
        if ((vterm->pointerButtons & (1u << button)) != 0) {
            code = buttonCode(button);
            break;
        }
    }
    if (tracking < VtermMouseTracking_Drag || (tracking == VtermMouseTracking_Drag && code == kMouseNone))
        return false;
    PrUVec2 cell = cellAt(vterm, event->pointer.position, desc);
    bool pixels = vterm->mouseEncoding == VtermMouseEncoding_SgrPixels;
    if (!pixels && vterm->mouseMoved && cell.x == vterm->mouseCell.x && cell.y == vterm->mouseCell.y)
        return true;
    putMouse(vterm, code | kMouseMotion, false, cell, pixelAt(event->pointer.position, desc), event->modifiers);
    vterm->mouseCell = cell;
    vterm->mouseMoved = true;
    return true;
}

// Adds `delta` to `*remainder`, and returns the whole lines, rounded toward zero. Leaves the fraction in `*remainder`.
int32_t wholeLines(float* remainder, float delta) {
    *remainder += delta;
    int32_t lines = int32_t(*remainder);
    *remainder -= float(lines);
    return lines;
}

bool scroll(Vterm* vterm, const PrAppEvent* event, const PrVtermInputDesc* desc) {
    bool tracked = vterm->mouseTracking != VtermMouseTracking_None;
    bool keys = !tracked && vterm->active == VtermScreenKind_Alternate && vterm->modes.alternateScroll;
    if (!tracked && !keys) {
        vterm->scrollRemainder = PrVec2{0, 0};
        return false;
    }
    PrVec2 delta = event->scroll.delta;
    if (event->scroll.unit == PrAppScrollUnit_Pixel)
        delta = PrVec2{delta.x / desc->cellExtent.x, delta.y / desc->cellExtent.y};
    int32_t down = wholeLines(&vterm->scrollRemainder.y, delta.y);
    int32_t right = wholeLines(&vterm->scrollRemainder.x, delta.x);
    PrUVec2 cell = cellAt(vterm, event->scroll.position, desc);
    PrUVec2 pixel = pixelAt(event->scroll.position, desc);
    for (int32_t i = 0; i < (down < 0 ? -down : down); ++i) {
        if (tracked)
            putMouse(vterm, down < 0 ? kMouseWheelUp : kMouseWheelDown, false, cell, pixel, event->modifiers);
        else
            putLetterKey(vterm, down < 0 ? 'A' : 'B', 1, vterm->modes.cursorKeys);
    }
    if (tracked) {
        for (int32_t i = 0; i < (right < 0 ? -right : right); ++i)
            putMouse(vterm, right < 0 ? kMouseWheelLeft : kMouseWheelRight, false, cell, pixel, event->modifiers);
    }
    return true;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

bool prVtermInputEvent(PrVterm* vterm, const PrAppEvent* event, const PrVtermInputDesc* desc) {
    PR_ASSERT(desc->cellExtent.x > 0 && desc->cellExtent.y > 0);
    // A key's Text event follows its KeyDown, and only a release may come between them.
    uint8_t dropTextSize = vterm->dropTextSize;
    if (event->kind != PrAppEventKind_KeyUp)
        vterm->dropTextSize = 0;
    switch (event->kind) {
        case PrAppEventKind_KeyDown: {
            bool sent = keyEvent(vterm, event, desc);
            if (sent && event->keyboard.text.size <= kVtermDropTextMax) {
                prMemCopy(vterm->dropText, event->keyboard.text.data, event->keyboard.text.size);
                vterm->dropTextSize = uint8_t(event->keyboard.text.size);
            }
            return sent;
        }
        case PrAppEventKind_KeyUp:
            return keyEvent(vterm, event, desc);
        case PrAppEventKind_Text: {
            if (dropTextSize != 0 && PrStringView{vterm->dropText, dropTextSize} == event->input.text)
                return true;
            // Text that no key reported is sent as key zero, if the program asked for text in the escape codes.
            uint8_t flags = keyboardFlags(vterm);
            uint8_t textKeys = kVtermKeyboardAllKeys | kVtermKeyboardText;
            if ((flags & textKeys) == textKeys && hasPrintable(event->input.text)) {
                putKittyKey(vterm, KittyKey{.final = 'u', .action = KeyAction_Press, .text = event->input.text});
                return true;
            }
            vtermPut(vterm, event->input.text);
            return true;
        }
        case PrAppEventKind_Paste:
            prVtermInputPaste(vterm, event->input.text);
            return true;
        case PrAppEventKind_PointerDown:
        case PrAppEventKind_PointerUp:
            return pointerButton(vterm, event, desc);
        case PrAppEventKind_PointerMove:
            return pointerMove(vterm, event, desc);
        case PrAppEventKind_Scroll:
            return scroll(vterm, event, desc);
        default:
            return false;
    }
}

void prVtermInputPaste(PrVterm* vterm, PrStringView text) {
    bool bracketed = vterm->modes.bracketedPaste;
    if (bracketed)
        vtermPut(vterm, "\x1b[200~");
    for (size_t i = 0; i < text.size; ++i) {
        char c = text.data[i];
        if (c == '\r' && i + 1 < text.size && text.data[i + 1] == '\n')
            continue;
        if (c == '\n')
            c = '\r';
        if (bracketed && c == char(kEscape))
            continue;
        vtermPutByte(vterm, uint8_t(c));
    }
    if (bracketed)
        vtermPut(vterm, "\x1b[201~");
}

void prVtermInputFocus(PrVterm* vterm, bool focused) {
    if (vterm->modes.focusReports)
        vtermPut(vterm, focused ? "\x1b[I" : "\x1b[O");
}
