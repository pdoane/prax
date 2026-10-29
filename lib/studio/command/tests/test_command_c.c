#include "pr/command.h"
#include "pr/command_keymap.h"
#include "pr/command_kit.h"

int prTestCommandCApi(void);

// The public headers compiled as C. A chord reads back from its text, and destroying nothing does nothing.
int prTestCommandCApi(void) {
    PrCommandChord chord = {0};
    int result = !prCommandChordParse(PR_SV("Shift F2"), &chord);
    result |= chord.key != PrAppKey_F2 || chord.modifiers != PrAppModifierFlag_Shift;
    result |= !prCommandMatches(PR_SV("open"), PR_SV("Open File"), PR_SV(""));
    prCommandRegistryDestroy(NULL);
    prCommandKeymapDestroy(NULL);
    return result;
}
