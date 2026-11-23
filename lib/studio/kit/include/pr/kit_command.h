// Studio Kit's entry points for commands: the menu items, buttons, tooltips and command palette that run the commands
// of a registry (pr/command.h).
//
// Each entry point runs a command by its id, through its registry, as a scope of key bindings does (pr/command_ui.h).
// An entry point runs only commands that take no required parameter (prCommandRegistryIsDirect). A disabled command is
// shown faint and does not run. A command runs during the build of the round whose input asked for it.

#pragma once

#include "pr/base.h"
#include "pr/command.h"
#include "pr/command_keymap.h"
#include "pr/kit.h"
#include "pr/kit_button.h"
#include "pr/kit_menu.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Creates a menu item that runs the command `commandId`. The item shows the command's title, the chord of its first
// binding, its check mark, and whether it is disabled. Running the command closes the menu. If the registry has no such
// command, the call creates nothing and returns kPrLayoutNodeNone.
PrLayoutNode prKitRegistryMenuItem(PrKit* kit, PrLayoutNode menu, const PrCommandRegistry* registry,
    const PrCommandKeymap* keymap, PrStringView commandId, PrKitMenuItemStyle style);

// Creates a button that runs the command `commandId`. `button` gives the key and the content, which is an icon, text,
// or both. The command sets the rest. The button is selected while the command is checked. It is disabled while the
// command is disabled or takes a required parameter. Its tooltip shows the command's title and chord
// (prKitRegistryTooltip). If the registry has no such command, the call creates nothing and returns kPrLayoutNodeNone.
PrLayoutNode prKitRegistryButton(PrKit* kit, PrLayoutNode parent, const PrCommandRegistry* registry,
    const PrCommandKeymap* keymap, PrStringView commandId, PrKitButton button, PrKitButtonStyle style);

// Creates the tooltip of `anchor` (prKitTooltip). It shows the title of the command `commandId` and the chord of its
// first binding. Returns kPrLayoutNodeNone until the tooltip shows, or if the registry has no such command.
PrLayoutNode prKitRegistryTooltip(PrKit* kit, PrLayoutNode anchor, const PrCommandRegistry* registry,
    const PrCommandKeymap* keymap, PrStringView commandId);

// Creates the command palette that `trigger` opens (prKitCommandPalette). The palette has an item for each command that
// matches `query`, in the registry's order. It leaves out hidden commands and commands that an entry point cannot run.
// Activating an item runs its command.
//
// The caller reads a new query from prKitTextChanged on the list. Returns the palette's list, or kPrLayoutNodeNone
// while the palette is closed.
PrLayoutNode prKitRegistryPalette(PrKit* kit, PrLayoutNode trigger, const PrCommandRegistry* registry,
    const PrCommandKeymap* keymap, PrStringView query);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
