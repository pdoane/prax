// Key bindings: chords bound to the ids of commands, each in a context.

#include "pr/command_keymap.h"

#include "pr/alloc.h"
#include "pr/ascii.h"
#include "pr/assert.h"
#include "pr/raw_array.h"
#include "pr/string_ops.h"
#include "pr/text_buffer.h"

namespace pr_impl {

namespace {

// A binding, with copies of its views. Each binding is allocated alone, so that the views of its strings, which may
// hold their bytes inline, stay valid as the keymap grows.
struct Binding {
    PrCommandChord chord;
    PrRawString commandId;
    PrRawString context;
    PrCommandBindingKind kind;
};

struct CommandKeymap {
    RawArray<Binding*> bindings;   // in the order that they were made
};

// A word of a chord's text that names a modifier.
struct ModifierWord {
    PrStringView word;
    PrAppModifierFlags modifier;
};

constexpr ModifierWord kModifierWords[] = {
    {"cmd", kPrCommandPrimaryModifier},  {"ctrl", PrAppModifierFlag_Control}, {"alt", PrAppModifierFlag_Alt},
    {"opt", PrAppModifierFlag_Alt},      {"shift", PrAppModifierFlag_Shift},  {"super", PrAppModifierFlag_Super},
};

// A key that a chord's text names by a word other than its key value, and how each platform shows it.
struct KeyWord {
    PrAppKey key;
    PrStringView word;
    PrStringView apple;
    PrStringView other;
};

constexpr KeyWord kKeyWords[] = {
    {PrAppKey_Left, "left", "\xE2\x86\x90", "Left"},
    {PrAppKey_Right, "right", "\xE2\x86\x92", "Right"},
    {PrAppKey_Up, "up", "\xE2\x86\x91", "Up"},
    {PrAppKey_Down, "down", "\xE2\x86\x93", "Down"},
    {' ', "space", "Space", "Space"},
    {PrAppKey_Enter, "enter", "\xE2\x86\xA9", "Enter"},
    {PrAppKey_Escape, "escape", "\xE2\x8E\x8B", "Esc"},
    {PrAppKey_Backspace, "backspace", "\xE2\x8C\xAB", "Backspace"},
    {PrAppKey_Delete, "delete", "\xE2\x8C\xA6", "Del"},
    {PrAppKey_Tab, "tab", "\xE2\x87\xA5", "Tab"},
    {PrAppKey_PageUp, "pageup", "\xE2\x87\x9E", "PgUp"},
    {PrAppKey_PageDown, "pagedown", "\xE2\x87\x9F", "PgDn"},
    {PrAppKey_Home, "home", "\xE2\x86\x96", "Home"},
    {PrAppKey_End, "end", "\xE2\x86\x98", "End"},
};

// Returns the key that `word` names, or PrAppKey_Unknown.
PrAppKey keyOf(PrStringView word) {
    if (word.size == 1 && word.data[0] > ' ' && word.data[0] <= '~')
        return PrAppKey(prAsciiUpper(word.data[0]));
    for (const KeyWord& named : kKeyWords) {
        if (prAsciiEqualsFold(named.word, word))
            return named.key;
    }
    for (PrAppKey key = PrAppKey_Left; key <= PrAppKey_F20; ++key) {
        if (prAsciiEqualsFold(prAppKeyName(key), word))
            return key;
    }
    return PrAppKey_Unknown;
}

void appendKey(PrTextBuffer* out, PrAppKey key) {
    for (const KeyWord& named : kKeyWords) {
        if (named.key == key) {
#if defined(__APPLE__)
            prTextBufferAppend(out, named.apple);
#else
            prTextBufferAppend(out, named.other);
#endif
            return;
        }
    }
    if (key < 0x80)
        prTextBufferAppendChar(out, char(key));
    else
        prTextBufferAppend(out, prAppKeyName(key));
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

// C opaque types
struct PrCommandKeymap : CommandKeymap {};

bool prCommandChordParse(PrStringView text, PrCommandChord* out) {
    PrCommandChord chord{};
    size_t at = 0;
    bool keyed = false;
    while (at < text.size) {
        while (at < text.size && text.data[at] == ' ')
            ++at;
        size_t end = at;
        while (end < text.size && text.data[end] != ' ')
            ++end;
        if (end == at)
            break;
        PrStringView word = prStringSlice(text, at, end - at);
        at = end;
        // A modifier comes before the key, and the key comes last.
        if (keyed)
            return false;
        bool modifier = false;
        for (const ModifierWord& named : kModifierWords) {
            if (word.size > 1 && prAsciiEqualsFold(named.word, word)) {
                chord.modifiers |= named.modifier;
                modifier = true;
            }
        }
        if (modifier)
            continue;
        chord.key = keyOf(word);
        if (chord.key == PrAppKey_Unknown)
            return false;
        keyed = true;
    }
    if (!keyed)
        return false;
    *out = chord;
    return true;
}

PrRawString prCommandChordText(PrCommandChord chord) {
    PrTextBuffer text{};
#if defined(__APPLE__)
    // Apple's menus show the modifiers as symbols, in the order Control, Option, Shift, Command.
    if ((chord.modifiers & PrAppModifierFlag_Control) != 0)
        prTextBufferAppend(&text, "\xE2\x8C\x83");
    if ((chord.modifiers & PrAppModifierFlag_Alt) != 0)
        prTextBufferAppend(&text, "\xE2\x8C\xA5");
    if ((chord.modifiers & PrAppModifierFlag_Shift) != 0)
        prTextBufferAppend(&text, "\xE2\x87\xA7");
    if ((chord.modifiers & PrAppModifierFlag_Super) != 0)
        prTextBufferAppend(&text, "\xE2\x8C\x98");
#else
    static const ModifierWord kNames[] = {
        {"Ctrl+", PrAppModifierFlag_Control},
        {"Alt+", PrAppModifierFlag_Alt},
        {"Shift+", PrAppModifierFlag_Shift},
        {"Win+", PrAppModifierFlag_Super},
    };
    for (const ModifierWord& named : kNames) {
        if ((chord.modifiers & named.modifier) != 0)
            prTextBufferAppend(&text, named.word);
    }
#endif
    appendKey(&text, chord.key);
    PrRawString string = prTextBufferCopy(&text);
    prTextBufferReset(&text);
    return string;
}

PrCommandKeymap* prCommandKeymapCreate(void) {
    return allocZeroed<PrCommandKeymap>();
}

void prCommandKeymapDestroy(PrCommandKeymap* PR_NULLABLE keymap) {
    if (keymap == nullptr)
        return;
    for (uint32_t i = 0; i < keymap->bindings.count; ++i) {
        Binding* binding = keymap->bindings.data[i];
        prRawStringReset(&binding->commandId);
        prRawStringReset(&binding->context);
        prMemoryFree(binding);
    }
    keymap->bindings.reset();
    prMemoryFree(keymap);
}

void prCommandKeymapBind(PrCommandKeymap* keymap, PrCommandChord chord, PrStringView commandId, PrStringView context,
        PrCommandBindingKind kind) {
    PR_ASSERT(chord.key != PrAppKey_Unknown && commandId.size != 0);
    for (uint32_t i = 0; i < keymap->bindings.count; ++i) {
        Binding* binding = keymap->bindings.data[i];
        if (binding->chord.key == chord.key && binding->chord.modifiers == chord.modifiers &&
                prRawStringView(&binding->context) == context) {
            prRawStringReplace(&binding->commandId, commandId);
            binding->kind = kind;
            return;
        }
    }
    Binding* binding = allocZeroed<Binding>();
    binding->chord = chord;
    binding->commandId = prRawStringCopy(commandId);
    binding->context = prRawStringCopy(context);
    binding->kind = kind;
    keymap->bindings.push(binding);
}

uint32_t prCommandKeymapCount(const PrCommandKeymap* keymap) {
    return keymap->bindings.count;
}

PrCommandBinding prCommandKeymapBinding(const PrCommandKeymap* keymap, uint32_t index) {
    PR_ASSERT(index < keymap->bindings.count);
    const Binding& binding = *keymap->bindings.data[index];
    return PrCommandBinding{
        binding.chord, prRawStringView(&binding.commandId), prRawStringView(&binding.context),
        binding.kind,
    };
}

bool prCommandKeymapChord(const PrCommandKeymap* keymap, PrStringView commandId, PrCommandChord* out) {
    for (uint32_t i = 0; i < keymap->bindings.count; ++i) {
        const Binding& binding = *keymap->bindings.data[i];
        if (prRawStringView(&binding.commandId) == commandId) {
            *out = binding.chord;
            return true;
        }
    }
    return false;
}

PrStringView prCommandKeymapCommand(const PrCommandKeymap* keymap, PrCommandChord chord, PrStringView context) {
    for (uint32_t i = 0; i < keymap->bindings.count; ++i) {
        const Binding& binding = *keymap->bindings.data[i];
        if (binding.chord.key == chord.key && binding.chord.modifiers == chord.modifiers &&
                prRawStringView(&binding.context) == context)
            return prRawStringView(&binding.commandId);
    }
    return PrStringView{};
}
