// Commands in an interface: the scopes that put key bindings on the nodes of a tree, and the menu items and the command
// palette of Studio Kit that run commands.
//
// Each entry point runs a command by its id, through its registry, so a verb behaves the same whichever entry point
// reached it. An entry point runs only the commands that take no required parameter (prCommandRegistryIsDirect), and a
// disabled command shows faint and does not run. A command runs during the build of the round whose input asked for it.

#pragma once

#include "pr/base.h"
#include "pr/command.h"
#include "pr/command_keymap.h"
#include "pr/kit.h"
#include "pr/kit_menu.h"
#include "pr/string_view.h"
#include "pr/ui.h"
#include "pr/ui_text_edit.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Puts the bindings of `context` on `node` for the current round, and runs the command of a chord that was routed to
// the node. The bindings apply while focus is within the node, or on the root while nothing has focus (pr/ui.h). A
// Normal binding claims its chord, and an Override binding captures it. A chord whose command is missing or takes a
// required parameter is left unbound, and one whose command is disabled is taken and does nothing.
void prCommandScope(PrUi* ui, PrLayoutNode node, const PrCommandRegistry* registry, const PrCommandKeymap* keymap,
    PrStringView context);

// Creates a menu item that runs the command `commandId`, showing its title, the chord of its first binding, its check
// and whether it is disabled. Running it closes the menu. Returns kPrLayoutNodeNone, and creates nothing, if the
// registry has no such command.
PrLayoutNode prCommandMenuItem(PrKit* kit, PrLayoutNode menu, const PrCommandRegistry* registry,
    const PrCommandKeymap* keymap, PrStringView commandId, PrKitMenuItemStyle style);

// Creates the tooltip of `anchor` (prKitTooltip), which shows the title of the command `commandId` and the chord of its
// first binding. Returns none until it shows, or if the registry has no such command.
PrLayoutNode prCommandTooltip(PrKit* kit, PrLayoutNode anchor, const PrCommandRegistry* registry,
    const PrCommandKeymap* keymap, PrStringView commandId);

// Returns true if `title` and `category` match `query`: each word of the query, in any case, is in one of them. An
// empty query matches every command.
bool prCommandMatches(PrStringView query, PrStringView title, PrStringView category);

// Creates the command palette that `trigger` opens (prKitCommandPalette), with an item for each command that matches
// `query`, in the registry's order. The caller takes a new query from prKitTextChanged on the list. The palette leaves
// out hidden commands and those that an entry point cannot run. Activating an item runs its command. Returns the
// palette's list, or none while it is closed.
PrLayoutNode prCommandPalette(PrKit* kit, PrLayoutNode trigger, const PrCommandRegistry* registry,
    const PrCommandKeymap* keymap, PrStringView query);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
