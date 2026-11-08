#include "pr/vterm_input.h"

#include "tools/harness/test.h"

#include <string>

namespace pr_impl {

namespace {

// A terminal of 80 columns by 24 rows. Its cells are 10 by 20 logical pixels, measured from the window's origin.
struct Term {
    PrVterm* vterm;
    PrVtermInputDesc desc{.origin = {0, 0}, .cellExtent = {10, 20}, .altSendsEscape = true};

    Term() {
        PrVtermDesc create{.extent = {80, 24}, .scrollback = 0};
        vterm = prVtermCreate(&create);
    }
    ~Term() { prVtermDestroy(vterm); }
    Term(const Term&) = delete;
    Term& operator=(const Term&) = delete;

    void write(const std::string& text) {
        prVtermWrite(vterm, PrByteView{reinterpret_cast<const uint8_t*>(text.data()), text.size()});
    }

    std::string output() {
        PrByteView bytes = prVtermOutput(vterm);
        std::string out(reinterpret_cast<const char*>(bytes.data), bytes.size);
        prVtermOutputConsume(vterm, bytes.size);
        return out;
    }

    // Delivers a key press and then the text that it types, in the order that an application delivers them. Returns the
    // bytes that the terminal sent.
    std::string key(PrAppKey key, PrAppModifierFlags modifiers = 0, const char* text = "",
            PrAppKeyCode code = PrAppKeyCode_Unidentified) {
        return press(PrAppEvent{
            .kind = PrAppEventKind_KeyDown,
            .modifiers = modifiers,
            .keyboard = {.key = key, .code = code, .text = text},
        });
    }

    // Delivers an auto-repeat of a held key, in the same way as `key`.
    std::string repeat(PrAppKey key, PrAppModifierFlags modifiers = 0, const char* text = "",
            PrAppKeyCode code = PrAppKeyCode_Unidentified) {
        return press(PrAppEvent{
            .kind = PrAppEventKind_KeyDown,
            .modifiers = modifiers,
            .keyboard = {.key = key, .code = code, .repeat = true, .text = text},
        });
    }

    // Releases `key`, and returns what the release sent.
    std::string release(PrAppKey key, PrAppModifierFlags modifiers = 0, PrAppKeyCode code = PrAppKeyCode_Unidentified) {
        PrAppEvent up{.kind = PrAppEventKind_KeyUp, .modifiers = modifiers, .keyboard = {.key = key, .code = code}};
        bool taken = prVtermInputEvent(vterm, &up, &desc);
        std::string sent = output();
        CHECK(taken == !sent.empty());
        return sent;
    }

    std::string press(const PrAppEvent& down) {
        prVtermInputEvent(vterm, &down, &desc);
        if (down.keyboard.text.size != 0) {
            PrAppEvent typed{.kind = PrAppEventKind_Text, .input = {.text = down.keyboard.text}};
            prVtermInputEvent(vterm, &typed, &desc);
        }
        return output();
    }

    std::string text(const char* typed) {
        PrAppEvent event{.kind = PrAppEventKind_Text, .input = {.text = typed}};
        prVtermInputEvent(vterm, &event, &desc);
        return output();
    }

    bool pointer(PrAppEventKind kind, PrVec2 position, uint8_t button = 0, PrAppModifierFlags modifiers = 0) {
        PrAppEvent event{.kind = kind, .modifiers = modifiers, .pointer = {.position = position, .button = button}};
        return prVtermInputEvent(vterm, &event, &desc);
    }

    bool scroll(float lines, PrVec2 position = {5, 5}) {
        PrAppEvent event{
            .kind = PrAppEventKind_Scroll,
            .scroll = {.position = position, .delta = {0, lines}, .unit = PrAppScrollUnit_Line},
        };
        return prVtermInputEvent(vterm, &event, &desc);
    }
};

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("vterm input: typed text is sent, and the text of a key that sent its own sequence is dropped") {
    Term term;
    CHECK(term.key('A', PrAppModifierFlag_Shift, "A") == "A");
    CHECK(term.key('C', PrAppModifierFlag_Control, "c") == "\x03");
    CHECK(term.key('X', PrAppModifierFlag_Alt, "x") == "\x1bx");
    CHECK(term.key('X', PrAppModifierFlag_Alt | PrAppModifierFlag_Shift, "X") == "\x1bX");
    CHECK(term.key('[', PrAppModifierFlag_Control, "[") == "\x1b");
    CHECK(term.key(' ', PrAppModifierFlag_Control, " ") == std::string(1, '\0'));

    // If Alt does not send an escape, Alt types the layout's text.
    term.desc.altSendsEscape = false;
    CHECK(term.key('A', PrAppModifierFlag_Alt, "\xc3\xa5") == "\xc3\xa5");

    // Super is left to the application, so only the key's text is sent.
    CHECK(term.key('C', PrAppModifierFlag_Super, "c") == "c");
}

TEST_CASE("vterm input: only the text that a key typed is dropped, and text from elsewhere is sent") {
    Term term;
    // A key that types nothing, followed by text that an input method or a picker types without a key.
    CHECK(term.key(PrAppKey_Up) == "\x1b[A");
    CHECK(term.text("\xf0\x9f\x98\x80") == "\xf0\x9f\x98\x80");
    // A key whose text the platform does not deliver, then other text.
    PrAppEvent down{
        .kind = PrAppEventKind_KeyDown,
        .modifiers = PrAppModifierFlag_Control,
        .keyboard = {.key = 'A', .text = "a"},
    };
    prVtermInputEvent(term.vterm, &down, &term.desc);
    CHECK(term.output() == "\x01");
    CHECK(term.text("\xc3\xa9") == "\xc3\xa9");
    // A pointer event between a key and its text ends the wait for the text.
    prVtermInputEvent(term.vterm, &down, &term.desc);
    CHECK(term.output() == "\x01");
    term.pointer(PrAppEventKind_PointerMove, {5, 5});
    CHECK(term.text("a") == "a");
}

TEST_CASE("vterm input: named keys send their sequences, with modifiers and the cursor keys' mode") {
    Term term;
    CHECK(term.key(PrAppKey_Enter) == "\r");
    CHECK(term.key(PrAppKey_Backspace) == "\x7f");
    CHECK(term.key(PrAppKey_Backspace, PrAppModifierFlag_Control) == "\x08");
    CHECK(term.key(PrAppKey_Tab, PrAppModifierFlag_Shift) == "\x1b[Z");
    CHECK(term.key(PrAppKey_Up) == "\x1b[A");
    CHECK(term.key(PrAppKey_Left, PrAppModifierFlag_Control | PrAppModifierFlag_Shift) == "\x1b[1;6D");
    CHECK(term.key(PrAppKey_Delete) == "\x1b[3~");
    CHECK(term.key(PrAppKey_PageUp, PrAppModifierFlag_Alt) == "\x1b[5;3~");
    CHECK(term.key(PrAppKey_F1) == "\x1bOP");
    CHECK(term.key(PrAppKey_F5) == "\x1b[15~");
    CHECK(term.key(PrAppKey_F12, PrAppModifierFlag_Shift) == "\x1b[24;2~");
    term.write("\x1b[?1h");
    CHECK(term.key(PrAppKey_Up) == "\x1bOA");
    CHECK(term.key(PrAppKey_Home) == "\x1bOH");
    term.write("\x1b[20h");
    CHECK(term.key(PrAppKey_Enter) == "\r\n");
    CHECK(term.key(PrAppKey_Shift, PrAppModifierFlag_Shift).empty());
    CHECK(term.key(PrAppKey_ContextMenu) == "\x1b[29~");
}

TEST_CASE("vterm input: the keyboard protocol disambiguates keys, and can send every key as an escape code") {
    Term term;
    term.write("\x1b[>1u");
    CHECK(term.key(PrAppKey_Escape) == "\x1b[27u");
    CHECK(term.key('C', PrAppModifierFlag_Control, "c") == "\x1b[99;5u");
    CHECK(term.key(PrAppKey_Enter) == "\r");
    CHECK(term.key(PrAppKey_Enter, PrAppModifierFlag_Shift) == "\x1b[13;2u");
    CHECK(term.key('A', 0, "a") == "a");
    term.write("\x1b[=8;2u");
    CHECK(term.key('A', 0, "a") == "\x1b[97u");
    CHECK(term.key(PrAppKey_Enter) == "\x1b[13u");
    term.write("\x1b[<u");
    CHECK(term.key(PrAppKey_Escape) == "\x1b");
}

TEST_CASE("vterm input: disambiguated keys take the protocol's forms, and text keys without chords type text") {
    Term term;
    term.write("\x1b[>1u");
    PrAppModifierFlags shift = PrAppModifierFlag_Shift;
    PrAppModifierFlags control = PrAppModifierFlag_Control;
    PrAppModifierFlags alt = PrAppModifierFlag_Alt;
    CHECK(term.key('A', shift, "A") == "A");
    CHECK(term.key('A', control, "a") == "\x1b[97;5u");
    CHECK(term.key('A', alt, "a") == "\x1b[97;3u");
    CHECK(term.key('A', PrAppModifierFlags(control | shift), "A") == "\x1b[97;6u");
    CHECK(term.key('A', PrAppModifierFlags(alt | shift), "A") == "\x1b[97;4u");
    CHECK(term.key(' ', control, " ") == "\x1b[32;5u");
    CHECK(term.key('C', PrAppModifierFlag_Super, "c") == "\x1b[99;9u");
    CHECK(term.key(PrAppKey_Tab) == "\t");
    CHECK(term.key(PrAppKey_Backspace) == "\x7f");
    CHECK(term.key(PrAppKey_Tab, shift) == "\x1b[9;2u");

    // The functional keys use the protocol's forms in either cursor key mode, never SS3.
    term.write("\x1b[?1h");
    CHECK(term.key(PrAppKey_Up) == "\x1b[A");
    CHECK(term.key(PrAppKey_Up, control) == "\x1b[1;5A");
    CHECK(term.key(PrAppKey_Home) == "\x1b[H");
    CHECK(term.key(PrAppKey_F1) == "\x1b[P");
    CHECK(term.key(PrAppKey_F2, shift) == "\x1b[1;2Q");
    CHECK(term.key(PrAppKey_F3) == "\x1b[13~");
    CHECK(term.key(PrAppKey_F4) == "\x1b[S");
    CHECK(term.key(PrAppKey_F5) == "\x1b[15~");
    CHECK(term.key(PrAppKey_F13) == "\x1b[57376u");
    CHECK(term.key(PrAppKey_F20, alt) == "\x1b[57383;3u");
    CHECK(term.key(PrAppKey_ContextMenu) == "\x1b[57363u");
    CHECK(term.key(PrAppKey_Unknown, 0, "", PrAppKeyCode_PrintScreen) == "\x1b[57361u");
    CHECK(term.key(PrAppKey_Unknown, 0, "", PrAppKeyCode_Pause) == "\x1b[57362u");

    // Keypad keys that type no text report as keypad keys. Keypad keys that type text send the text, unless a chord
    // makes them report as keys.
    CHECK(term.key(PrAppKey_PageUp, 0, "", PrAppKeyCode_Numpad9) == "\x1b[57421u");
    CHECK(term.key(PrAppKey_Left, 0, "", PrAppKeyCode_Numpad4) == "\x1b[57417u");
    CHECK(term.key(PrAppKey_Clear, 0, "", PrAppKeyCode_Numpad5) == "\x1b[E");
    CHECK(term.key(PrAppKey_Enter, 0, "", PrAppKeyCode_NumpadEnter) == "\x1b[57414u");
    CHECK(term.key('0', 0, "0", PrAppKeyCode_Numpad0) == "0");
    CHECK(term.key('0', control, "0", PrAppKeyCode_Numpad0) == "\x1b[57399;5u");
    CHECK(term.key('+', control, "+", PrAppKeyCode_NumpadAdd) == "\x1b[57413;5u");

    // Modifier and lock keys are reported only if the program asks for every key as an escape code.
    CHECK(term.key(PrAppKey_Shift, shift, "", PrAppKeyCode_ShiftLeft).empty());
    CHECK(term.key(PrAppKey_CapsLock, PrAppModifierFlag_CapsLock, "", PrAppKeyCode_CapsLock).empty());
}

TEST_CASE("vterm input: caps lock is reported with the keys that the protocol sends, and keeps text keys typing") {
    Term term;
    PrAppModifierFlags caps = PrAppModifierFlag_CapsLock;
    // The legacy encodings have no bit for caps lock.
    CHECK(term.key('C', PrAppModifierFlag_Control | caps, "C") == "\x03");
    CHECK(term.key(PrAppKey_Up, caps) == "\x1b[A");
    term.write("\x1b[>1u");
    CHECK(term.key('A', caps, "A") == "A");
    CHECK(term.key('A', PrAppModifierFlag_Control | caps, "A") == "\x1b[97;69u");
    CHECK(term.key(PrAppKey_Enter, caps) == "\r");
    CHECK(term.key(PrAppKey_Escape, caps) == "\x1b[27;65u");
    CHECK(term.key(PrAppKey_Up, caps) == "\x1b[1;65A");
    term.write("\x1b[=8u");
    CHECK(term.key('A', caps, "A") == "\x1b[97;65u");
}

TEST_CASE("vterm input: event types report repeats and releases, but Enter, Tab and Backspace release only with every key") {
    Term term;
    // Without event types, a repeat is sent as a press, and a release is left to the caller.
    term.write("\x1b[>1u");
    CHECK(term.repeat(PrAppKey_Up) == "\x1b[A");
    CHECK(term.release(PrAppKey_Up).empty());

    term.write("\x1b[=2u");
    CHECK(term.key('A', 0, "a", PrAppKeyCode_KeyA) == "a");
    CHECK(term.repeat('A', 0, "a", PrAppKeyCode_KeyA) == "a");
    CHECK(term.release('A', 0, PrAppKeyCode_KeyA) == "\x1b[97;1:3u");
    CHECK(term.release('A', PrAppModifierFlag_Shift, PrAppKeyCode_KeyA) == "\x1b[97;2:3u");
    // The press of a chord sends its legacy bytes, and its repeat and release are reported.
    CHECK(term.key('C', PrAppModifierFlag_Control, "c") == "\x03");
    CHECK(term.repeat('C', PrAppModifierFlag_Control, "c") == "\x1b[99;5:2u");
    CHECK(term.release('C', PrAppModifierFlag_Control) == "\x1b[99;5:3u");
    CHECK(term.key(PrAppKey_Escape) == "\x1b");
    CHECK(term.release(PrAppKey_Escape) == "\x1b[27;1:3u");

    term.write("\x1b[=3u");
    CHECK(term.key(PrAppKey_Up) == "\x1b[A");
    CHECK(term.repeat(PrAppKey_Up) == "\x1b[1;1:2A");
    CHECK(term.release(PrAppKey_Up) == "\x1b[1;1:3A");
    CHECK(term.release(PrAppKey_F5, PrAppModifierFlag_Alt) == "\x1b[15;3:3~");
    CHECK(term.repeat('C', PrAppModifierFlag_Control, "c") == "\x1b[99;5:2u");
    CHECK(term.release(PrAppKey_Escape) == "\x1b[27;1:3u");
    CHECK(term.key(PrAppKey_Backspace) == "\x7f");
    CHECK(term.repeat(PrAppKey_Backspace) == "\x7f");
    CHECK(term.release(PrAppKey_Backspace).empty());
    PrAppModifierFlags caps = PrAppModifierFlag_CapsLock;
    CHECK(term.key(PrAppKey_Enter, caps) == "\r");
    CHECK(term.release(PrAppKey_Enter, caps).empty());
    CHECK(term.release(PrAppKey_Tab).empty());
    CHECK(term.release(PrAppKey_Enter, PrAppModifierFlag_Shift) == "\x1b[13;2:3u");

    term.write("\x1b[=10u");
    CHECK(term.repeat('A', 0, "a") == "\x1b[97;1:2u");
    CHECK(term.release(PrAppKey_Enter) == "\x1b[13;1:3u");
    CHECK(term.release(PrAppKey_Tab) == "\x1b[9;1:3u");
    CHECK(term.release(PrAppKey_Backspace) == "\x1b[127;1:3u");
    CHECK(term.release(PrAppKey_Shift, 0, PrAppKeyCode_ShiftLeft) == "\x1b[57441;1:3u");
}

TEST_CASE("vterm input: alternate keys report the shifted key and the key of the base layout") {
    Term term;
    PrAppModifierFlags control = PrAppModifierFlag_Control;
    PrAppModifierFlags shift = PrAppModifierFlag_Shift;
    PrAppModifierFlags controlShift = control | shift;
    // Alternate keys apply only to keys that are already sent as escape codes.
    term.write("\x1b[>4u");
    CHECK(term.key('A', 0, "a", PrAppKeyCode_KeyA) == "a");
    CHECK(term.key('A', control, "a", PrAppKeyCode_KeyA) == "\x01");

    term.write("\x1b[=5u");
    CHECK(term.key('A', shift, "A", PrAppKeyCode_KeyA) == "A");
    CHECK(term.key('A', control, "a", PrAppKeyCode_KeyA) == "\x1b[97;5u");
    CHECK(term.key('A', controlShift, "A", PrAppKeyCode_KeyA) == "\x1b[97:65;6u");
    // Control+Shift+= reports plus as the shifted key.
    CHECK(term.key('+', controlShift, "+", PrAppKeyCode_Equal) == "\x1b[61:43;6u");
    CHECK(term.key('!', controlShift, "!", PrAppKeyCode_Digit1) == "\x1b[49:33;6u");
    // A key on a Cyrillic layout reports its own character and the base layout's key at the same position.
    CHECK(term.key(PrAppKey_Unknown, control, "\xd1\x81", PrAppKeyCode_KeyC) == "\x1b[1089::99;5u");
    CHECK(term.key(PrAppKey_Unknown, controlShift, "\xd0\xa1", PrAppKeyCode_KeyC) == "\x1b[1089:1057:99;6u");
    // The functional keys have no alternates.
    CHECK(term.key(PrAppKey_Up, shift, "", PrAppKeyCode_ArrowUp) == "\x1b[1;2A");

    term.write("\x1b[=12u");
    CHECK(term.key('A', 0, "a", PrAppKeyCode_KeyA) == "\x1b[97u");
    CHECK(term.key('A', shift, "A", PrAppKeyCode_KeyA) == "\x1b[97:65;2u");
    CHECK(term.key('/', 0, "/", PrAppKeyCode_Slash) == "\x1b[47u");
    CHECK(term.key('?', shift, "?", PrAppKeyCode_Slash) == "\x1b[47:63;2u");
}

TEST_CASE("vterm input: associated text travels in the key's escape code, and the Text event is dropped") {
    Term term;
    term.write("\x1b[>24u");
    CHECK(term.key('A', 0, "a", PrAppKeyCode_KeyA) == "\x1b[97;;97u");
    CHECK(term.key('A', PrAppModifierFlag_Shift, "A", PrAppKeyCode_KeyA) == "\x1b[97;2;65u");
    CHECK(term.key('A', PrAppModifierFlag_Shift, "AB", PrAppKeyCode_KeyA) == "\x1b[97;2;65:66u");
    // A chord, or a key that types no text, carries no associated text.
    CHECK(term.key('A', PrAppModifierFlag_Control, "a", PrAppKeyCode_KeyA) == "\x1b[97;5u");
    CHECK(term.key(PrAppKey_Enter) == "\x1b[13u");
    CHECK(term.key('5', 0, "5", PrAppKeyCode_Numpad5) == "\x1b[57404;;53u");
    // Text without a key is sent as key zero.
    CHECK(term.text("\xc3\xa9") == "\x1b[0;;233u");

    // If Option types a character from the layout's third level, Option is not reported as a modifier.
    term.desc.altSendsEscape = false;
    CHECK(term.key(PrAppKey_Unknown, PrAppModifierFlag_Alt, "\xc3\xa5", PrAppKeyCode_KeyA) == "\x1b[229;;229u");

    // Every enhancement at once.
    term.write("\x1b[=31u");
    CHECK(term.key(PrAppKey_Unknown, PrAppModifierFlag_Alt, "\xc3\xa5", PrAppKeyCode_KeyA) == "\x1b[229::97;;229u");
    CHECK(term.repeat('A', PrAppModifierFlag_Shift, "A", PrAppKeyCode_KeyA) == "\x1b[97:65;2:2;65u");
    CHECK(term.release('A', PrAppModifierFlag_Shift, PrAppKeyCode_KeyA) == "\x1b[97:65;2:3u");
}

TEST_CASE("vterm input: with every key, the modifier and lock keys report as keys of their own") {
    Term term;
    term.write("\x1b[>8u");
    CHECK(term.key(PrAppKey_Shift, PrAppModifierFlag_Shift, "", PrAppKeyCode_ShiftLeft) == "\x1b[57441;2u");
    // A press sets the modifier bit of the key itself, because a platform may report that bit only after the event.
    CHECK(term.key(PrAppKey_Control, 0, "", PrAppKeyCode_ControlRight) == "\x1b[57448;5u");
    CHECK(term.key(PrAppKey_Alt, PrAppModifierFlag_Alt, "", PrAppKeyCode_AltLeft) == "\x1b[57443;3u");
    CHECK(term.key(PrAppKey_Meta, PrAppModifierFlag_Super, "", PrAppKeyCode_MetaRight) == "\x1b[57450;9u");
    CHECK(term.key(PrAppKey_CapsLock, PrAppModifierFlag_CapsLock, "", PrAppKeyCode_CapsLock) == "\x1b[57358;65u");
    CHECK(term.key(PrAppKey_NumLock, 0, "", PrAppKeyCode_NumLock) == "\x1b[57360u");
    CHECK(term.key(PrAppKey_Unknown, 0, "", PrAppKeyCode_ScrollLock) == "\x1b[57359u");
    CHECK(term.key(PrAppKey_Fn, 0, "", PrAppKeyCode_Fn).empty());
}

TEST_CASE("vterm input: a paste is bracketed if the program asked, and loses the escapes that would end it") {
    Term term;
    prVtermInputPaste(term.vterm, "a\nb\r\nc");
    CHECK(term.output() == "a\rb\rc");
    term.write("\x1b[?2004h");
    prVtermInputPaste(term.vterm, "x\x1b[201~y");
    CHECK(term.output() == "\x1b[200~x[201~y\x1b[201~");
    PrAppEvent clipboard{.kind = PrAppEventKind_Clipboard, .input = {.text = "z"}};
    CHECK(prVtermInputEvent(term.vterm, &clipboard, &term.desc));
    CHECK(term.output() == "\x1b[200~z\x1b[201~");
}

TEST_CASE("vterm input: focus is reported if the program asked") {
    Term term;
    prVtermInputFocus(term.vterm, true);
    CHECK(term.output().empty());
    term.write("\x1b[?1004h");
    prVtermInputFocus(term.vterm, true);
    prVtermInputFocus(term.vterm, false);
    CHECK(term.output() == "\x1b[I\x1b[O");
}

TEST_CASE("vterm input: the pointer is the caller's until the program asks for reports") {
    Term term;
    CHECK(!term.pointer(PrAppEventKind_PointerDown, PrVec2{15, 25}));
    CHECK(!term.scroll(1));
    CHECK(term.output().empty());

    // Clicks in the default encoding: a press, then a release reported as button 3. Cells are one-based and offset by
    // 32.
    term.write("\x1b[?1000h");
    CHECK(term.pointer(PrAppEventKind_PointerDown, PrVec2{15, 25}));
    CHECK(term.pointer(PrAppEventKind_PointerUp, PrVec2{15, 25}));
    CHECK(term.output() == "\x1b[M\x20\x22\x22\x1b[M\x23\x22\x22");

    // Mode 1000 does not report moves, so a move is left to the caller.
    CHECK(!term.pointer(PrAppEventKind_PointerMove, PrVec2{45, 25}));
}

TEST_CASE("vterm input: SGR reports carry the button, the modifiers, moves and the wheel") {
    Term term;
    term.write("\x1b[?1002h\x1b[?1006h");
    CHECK(term.pointer(PrAppEventKind_PointerDown, PrVec2{5, 5}, 1, PrAppModifierFlag_Control));
    CHECK(term.output() == "\x1b[<18;1;1M");
    CHECK(term.pointer(PrAppEventKind_PointerMove, PrVec2{8, 8}));
    CHECK(term.output().empty());
    CHECK(term.pointer(PrAppEventKind_PointerMove, PrVec2{25, 45}));
    CHECK(term.output() == "\x1b[<34;3;3M");
    CHECK(term.pointer(PrAppEventKind_PointerUp, PrVec2{25, 45}, 1));
    CHECK(term.output() == "\x1b[<2;3;3m");
    CHECK(!term.pointer(PrAppEventKind_PointerMove, PrVec2{55, 45}));

    CHECK(term.scroll(-2));
    CHECK(term.output() == "\x1b[<64;1;1M\x1b[<64;1;1M");
    CHECK(term.scroll(0.5f));
    CHECK(term.output().empty());
    CHECK(term.scroll(0.5f));
    CHECK(term.output() == "\x1b[<65;1;1M");
}

TEST_CASE("vterm input: a scroll sends the cursor keys while the alternate screen shows") {
    Term term;
    term.write("\x1b[?1049h");
    CHECK(term.scroll(2));
    CHECK(term.output() == "\x1b[B\x1b[B");
    term.write("\x1b[?1007l");
    CHECK(!term.scroll(1));
}
