// Studio Kit's entry points of commands: menu items, buttons, tooltips and the command palette.

#include "pr/kit_command.h"

#include "pr/command_ui.h"
#include "pr/hash.h"

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

// Returns the text that an entry point shows for the chord of `commandId`, or the null string if nothing binds it.
PrRawString shortcutOf(const PrCommandKeymap* keymap, PrStringView commandId) {
    PrCommandChord chord{};
    return prCommandKeymapChord(keymap, commandId, &chord) ? prCommandChordText(chord) : prRawStringNull();
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

PrLayoutNode prKitRegistryMenuItem(PrKit* kit, PrLayoutNode menu, const PrCommandRegistry* registry,
        const PrCommandKeymap* keymap, PrStringView commandId, PrKitMenuItemStyle style) {
    PrCommandIndex index = prCommandRegistryFind(registry, commandId);
    if (index == kPrCommandMissing)
        return kPrLayoutNodeNone;
    PrCommandDesc command = prCommandRegistryCommand(registry, index);
    PrCommandStateFlags state = prCommandRegistryState(registry, index);
    PrRawString shortcut = shortcutOf(keymap, commandId);
    PrKitMenuItem item{
        .text = command.title,
        .shortcut = prRawStringView(&shortcut),
        .checked = (state & PrCommandStateFlag_Checked) != 0,
        .disabled = (state & PrCommandStateFlag_Disabled) != 0 || !isOffered(registry, index),
    };
    PrLayoutNode node = prKitMenuItem(kit, menu, item, style);
    prRawStringReset(&shortcut);
    if (prKitActivated(kit, node))
        runFromEntryPoint(registry, index);
    return node;
}

PrLayoutNode prKitRegistryButton(PrKit* kit, PrLayoutNode parent, const PrCommandRegistry* registry,
        const PrCommandKeymap* keymap, PrStringView commandId, PrKitButton button, PrKitButtonStyle style) {
    PrCommandIndex index = prCommandRegistryFind(registry, commandId);
    if (index == kPrCommandMissing)
        return kPrLayoutNodeNone;
    PrCommandStateFlags state = prCommandRegistryState(registry, index);
    button.selected = (state & PrCommandStateFlag_Checked) != 0;
    button.disabled = (state & PrCommandStateFlag_Disabled) != 0 || !isOffered(registry, index);
    PrLayoutNode node = prKitButton(kit, parent, button, style);
    prKitRegistryTooltip(kit, node, registry, keymap, commandId);
    if (prKitActivated(kit, node))
        runFromEntryPoint(registry, index);
    return node;
}

PrLayoutNode prKitRegistryTooltip(PrKit* kit, PrLayoutNode anchor, const PrCommandRegistry* registry,
        const PrCommandKeymap* keymap, PrStringView commandId) {
    PrCommandIndex index = prCommandRegistryFind(registry, commandId);
    if (index == kPrCommandMissing)
        return kPrLayoutNodeNone;
    PrRawString shortcut = shortcutOf(keymap, commandId);
    PrKitTooltip tooltip{
        .text = prCommandRegistryCommand(registry, index).title,
        .shortcut = prRawStringView(&shortcut),
    };
    PrLayoutNode node = prKitTooltip(kit, anchor, tooltip);
    prRawStringReset(&shortcut);
    return node;
}

PrLayoutNode prKitRegistryPalette(PrKit* kit, PrLayoutNode trigger, const PrCommandRegistry* registry,
        const PrCommandKeymap* keymap, PrStringView query) {
    uint32_t count = 0;
    for (PrCommandIndex i = 0; i < prCommandRegistryCount(registry); ++i)
        count += isListed(registry, i, query) ? 1 : 0;
    PrKitCommandPalette palette{.prompt = "Type a command", .count = count, .query = query, .searchable = true};
    PrLayoutNode list = prKitCommandPalette(kit, trigger, palette);
    if (list == kPrLayoutNodeNone)
        return list;
    // The command that the palette runs. It runs after the list is built, so that it changes nothing that the list
    // reads.
    PrCommandIndex chosen = kPrCommandMissing;
    for (PrCommandIndex i = 0; i < prCommandRegistryCount(registry); ++i) {
        if (!isListed(registry, i, query))
            continue;
        PrCommandDesc command = prCommandRegistryCommand(registry, i);
        PrRawString shortcut = shortcutOf(keymap, command.id);
        PrKitCommandItem item{
            .key = prHashString(command.id, 0),
            .text = command.title,
            .category = command.category,
            .shortcut = prRawStringView(&shortcut),
            .disabled = (prCommandRegistryState(registry, i) & PrCommandStateFlag_Disabled) != 0,
        };
        if (prKitActivated(kit, prKitCommandItem(kit, list, item)))
            chosen = i;
        prRawStringReset(&shortcut);
    }
    prKitCommandPaletteFooter(kit, list);
    if (chosen != kPrCommandMissing)
        runFromEntryPoint(registry, chosen);
    return list;
}
