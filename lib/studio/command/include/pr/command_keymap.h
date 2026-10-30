// Key bindings: chords bound to the ids of commands, each in a context that names where it applies.
//
// A keymap is a table and nothing more. It knows nothing of focus or of the commands that its ids name. A context is a
// name that the application gives a region of its interface, such as "parameters" for a panel; the empty context is the
// application's own, which applies everywhere. A scope (pr/command_kit.h) puts a context's bindings on a node, so that
// a chord applies while focus is within that node, and the innermost scope that binds a chord takes it.
//
// A keymap is used from one thread at a time.

#pragma once

#include "pr/app.h"
#include "pr/base.h"
#include "pr/string.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// A key with exactly the modifiers held. A letter key is its uppercase ASCII letter, as a key event reports it.
typedef struct PrCommandChord {
    PrAppKey key;
    PrAppModifierFlags modifiers;
} PrCommandChord;

// The modifier that an application's shortcuts use on the platform that the build targets: Command on Apple's
// platforms, and Control elsewhere.
#if defined(__APPLE__)
enum { kPrCommandPrimaryModifier = PrAppModifierFlag_Super };
#else
enum { kPrCommandPrimaryModifier = PrAppModifierFlag_Control };
#endif

// Reads a chord from `text`: words separated by spaces, the modifiers and then the key, such as "Cmd Shift P". Cmd is
// the platform's primary modifier (kPrCommandPrimaryModifier). The other modifiers are Ctrl, Alt (or Opt), Shift and
// Super. A key is a single printable ASCII character, a letter in either case, or the name of a named key, as
// prAppKeyName spells it, such as Enter or F5. Left, Right, Up and Down name the arrow keys, and Space names the space
// bar. Words match without regard to case. Returns false if the text is not a chord.
bool prCommandChordParse(PrStringView text, PrCommandChord* out);

// Returns the chord as the platform shows a shortcut: symbols in the order that Apple's menus use, such as ⇧⌘P, on
// Apple's platforms, and words joined by plus signs, such as Ctrl+Shift+P, elsewhere. The caller releases the string
// with prStringReset.
PrString prCommandChordText(PrCommandChord chord);

// Where a binding takes its chord from. A Normal binding takes only what the focused widget leaves: a text field keeps
// the arrows and the edit chords that it claims. An Override binding takes its chord ahead of every widget, so that it
// works inside a text field. An override of a key that a widget needs breaks the widget, so overrides are rare.
enum {
    PrCommandBindingKind_Normal,
    PrCommandBindingKind_Override,
};
typedef uint8_t PrCommandBindingKind;

// A binding of a keymap. The views are valid while the keymap lives.
typedef struct PrCommandBinding {
    PrCommandChord chord;
    PrStringView commandId;
    PrStringView context;
    PrCommandBindingKind kind;
} PrCommandBinding;

// Chords bound to commands, by context.
typedef struct PrCommandKeymap PrCommandKeymap;

// Creates an empty keymap, or frees one.
PrCommandKeymap* prCommandKeymapCreate(void);
void prCommandKeymapDestroy(PrCommandKeymap* PR_NULLABLE keymap);

// Binds `chord` to the command `commandId` in `context`. A later binding of the same chord in the same context replaces
// the earlier one, so that a user's bindings, made after the application's, win. The keymap copies the views.
void prCommandKeymapBind(PrCommandKeymap* keymap, PrCommandChord chord, PrStringView commandId, PrStringView context,
    PrCommandBindingKind kind);

// Return the number of bindings, and the binding at `index`, in the order that they were made.
uint32_t prCommandKeymapCount(const PrCommandKeymap* keymap);
PrCommandBinding prCommandKeymapBinding(const PrCommandKeymap* keymap, uint32_t index);

// Writes to `out` the chord that a menu or the palette shows for `commandId`: the chord of its first binding in any
// context. Returns false if nothing binds the command.
bool prCommandKeymapChord(const PrCommandKeymap* keymap, PrStringView commandId, PrCommandChord* out);

// Returns the id of the command that `chord` is bound to in `context`, or the null view if the chord is bound to none
// there. The view is valid until the keymap changes. A host that routes keys past the scopes of its interface, such as
// to content that keeps the keyboard, looks its chords up here.
PrStringView prCommandKeymapCommand(const PrCommandKeymap* keymap, PrCommandChord chord, PrStringView context);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
