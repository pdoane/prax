// Commands in an interface of lib/ui: scopes of key bindings, and the match of a palette's query.

#include "pr/command_ui.h"

#include "pr/ascii.h"
#include "pr/string_ops.h"

namespace pr_impl {

namespace {

// Runs a command that an entry point reached, if it is enabled.
void runFromEntryPoint(const PrCommandRegistry* registry, PrCommandIndex index) {
    if ((prCommandRegistryState(registry, index) & PrCommandStateFlag_Disabled) != 0)
        return;
    prCommandRegistryRun(registry, prCommandRegistryCommand(registry, index).id, nullptr, nullptr);
}

// Returns true if an entry point may offer the command. The command must exist and take no required parameter.
bool isOffered(const PrCommandRegistry* registry, PrCommandIndex index) {
    return index != kPrCommandMissing && prCommandRegistryIsDirect(registry, index);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

void prCommandScope(PrUi* ui, PrLayoutNode node, const PrCommandRegistry* registry, const PrCommandKeymap* keymap,
        PrStringView context) {
    // A press within the scope focuses the scope's node, unless the press lands on something focusable inside it. The
    // scope's bindings then take the keys while the user works in that region.
    prUiAddFlags(ui, node, PrUiFlag_PressFocus);
    for (uint32_t i = 0; i < prCommandKeymapCount(keymap); ++i) {
        PrCommandBinding binding = prCommandKeymapBinding(keymap, i);
        if (binding.context != context)
            continue;
        PrCommandIndex index = prCommandRegistryFind(registry, binding.commandId);
        if (!isOffered(registry, index))
            continue;
        bool pressed;
        if (binding.kind == PrCommandBindingKind_Override)
            pressed = prUiKeyCaptured(ui, node, binding.chord.key, binding.chord.modifiers);
        else
            pressed = prUiKeyPressed(ui, node, binding.chord.key, binding.chord.modifiers);
        // A command that runs once for each press takes the repeats of its held chord, and does nothing with them.
        bool once = (prCommandRegistryCommand(registry, index).flags & PrCommandFlag_Once) != 0;
        if (pressed && !(once && prUiKeyRepeated(ui)))
            runFromEntryPoint(registry, index);
    }
}

bool prCommandMatches(PrStringView query, PrStringView title, PrStringView category) {
    size_t at = 0;
    while (at < query.size) {
        while (at < query.size && query.data[at] == ' ')
            ++at;
        size_t end = at;
        while (end < query.size && query.data[end] != ' ')
            ++end;
        PrStringView word = prStringSlice(query, at, end - at);
        if (word.size != 0 && !prAsciiContainsFold(title, word) && !prAsciiContainsFold(category, word))
            return false;
        at = end;
    }
    return true;
}
