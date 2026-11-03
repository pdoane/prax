// Commands in an interface: scopes of key bindings, and Studio Kit's menu items and command palette.

#include "pr/command_kit.h"

#include "pr/ascii.h"
#include "pr/string_ops.h"

namespace pr_impl {

namespace {

// Runs a command that a surface reached, if it is enabled.
void runFromSurface(const PrCommandRegistry* registry, PrCommandIndex index) {
    if ((prCommandRegistryState(registry, index) & PrCommandStateFlag_Disabled) != 0)
        return;
    prCommandRegistryRun(registry, prCommandRegistryCommand(registry, index).id, nullptr, nullptr);
}

// Returns true if a surface may offer the command: it exists and takes no required parameter.
bool isOffered(const PrCommandRegistry* registry, PrCommandIndex index) {
    return index != kPrCommandMissing && prCommandRegistryIsDirect(registry, index);
}

// Returns the text that a surface shows for the chord of `commandId`, or the null string if nothing binds it.
PrString shortcutOf(const PrCommandKeymap* keymap, PrStringView commandId) {
    PrCommandChord chord{};
    return prCommandKeymapChord(keymap, commandId, &chord) ? prCommandChordText(chord) : prStringNull();
}

// Returns true if the palette lists the command at `index` for `query`.
bool isListed(const PrCommandRegistry* registry, PrCommandIndex index, PrStringView query) {
    PrCommandDesc command = prCommandRegistryCommand(registry, index);
    return (command.flags & PrCommandFlag_Hidden) == 0 && prCommandRegistryIsDirect(registry, index) &&
        prCommandMatches(query, command.title, command.category);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

void prCommandScope(PrUi* ui, PrLayoutNode node, const PrCommandRegistry* registry, const PrCommandKeymap* keymap,
        PrStringView context) {
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
        if (pressed)
            runFromSurface(registry, index);
    }
}

PrLayoutNode prCommandMenuItem(PrKit* kit, PrLayoutNode menu, const PrCommandRegistry* registry,
        const PrCommandKeymap* keymap, PrStringView commandId, PrKitMenuItemStyle style) {
    PrCommandIndex index = prCommandRegistryFind(registry, commandId);
    if (index == kPrCommandMissing)
        return kPrLayoutNodeNone;
    PrCommandDesc command = prCommandRegistryCommand(registry, index);
    PrCommandStateFlags state = prCommandRegistryState(registry, index);
    PrString shortcut = shortcutOf(keymap, commandId);
    PrKitMenuItem item{
        .text = command.title,
        .shortcut = prStringView(&shortcut),
        .checked = (state & PrCommandStateFlag_Checked) != 0,
        .disabled = (state & PrCommandStateFlag_Disabled) != 0 || !isOffered(registry, index),
    };
    PrLayoutNode node = prKitMenuItem(kit, menu, item, style);
    prStringReset(&shortcut);
    if (prKitActivated(kit, node))
        runFromSurface(registry, index);
    return node;
}

PrLayoutNode prCommandTooltip(PrKit* kit, PrLayoutNode anchor, const PrCommandRegistry* registry,
        const PrCommandKeymap* keymap, PrStringView commandId) {
    PrCommandIndex index = prCommandRegistryFind(registry, commandId);
    if (index == kPrCommandMissing)
        return kPrLayoutNodeNone;
    PrString shortcut = shortcutOf(keymap, commandId);
    PrKitTooltip tooltip{.text = prCommandRegistryCommand(registry, index).title, .shortcut = prStringView(&shortcut)};
    PrLayoutNode node = prKitTooltip(kit, anchor, tooltip);
    prStringReset(&shortcut);
    return node;
}

bool prCommandMatches(PrStringView query, PrStringView title, PrStringView category) {
    size_t at = 0;
    while (at < query.size) {
        while (at < query.size && query.data[at] == ' ')
            ++at;
        size_t end = at;
        while (end < query.size && query.data[end] != ' ')
            ++end;
        PrStringView word = stringSlice(query, at, end - at);
        if (word.size != 0 && !asciiContainsFold(title, word) && !asciiContainsFold(category, word))
            return false;
        at = end;
    }
    return true;
}

PrLayoutNode prCommandPalette(PrKit* kit, PrLayoutNode trigger, const PrCommandRegistry* registry,
        const PrCommandKeymap* keymap, PrStringView query) {
    uint32_t count = 0;
    for (PrCommandIndex i = 0; i < prCommandRegistryCount(registry); ++i)
        count += isListed(registry, i, query) ? 1 : 0;
    PrKitCommandPalette palette{.prompt = "Type a command", .count = count, .query = query};
    PrLayoutNode list = prKitCommandPalette(kit, trigger, palette);
    if (list == kPrLayoutNodeNone)
        return list;
    // The command that the palette runs, which runs once the list is built, so that it changes nothing that the list
    // reads.
    PrCommandIndex chosen = kPrCommandMissing;
    for (PrCommandIndex i = 0; i < prCommandRegistryCount(registry); ++i) {
        if (!isListed(registry, i, query))
            continue;
        PrCommandDesc command = prCommandRegistryCommand(registry, i);
        PrString shortcut = shortcutOf(keymap, command.id);
        PrKitCommandItem item{
            .key = prUiKey(kit->ui, list, command.id),
            .text = command.title,
            .category = command.category,
            .shortcut = prStringView(&shortcut),
            .disabled = (prCommandRegistryState(registry, i) & PrCommandStateFlag_Disabled) != 0,
        };
        if (prKitActivated(kit, prKitCommandItem(kit, list, item)))
            chosen = i;
        prStringReset(&shortcut);
    }
    prKitCommandPaletteFooter(kit, list);
    if (chosen != kPrCommandMissing)
        runFromSurface(registry, chosen);
    return list;
}
