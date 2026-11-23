// Commands in a lib/ui interface: scopes, which put key bindings on the nodes of a tree, and the query match that a
// command palette filters commands by.
//
// A scope runs a command by its id, through its registry, so a command behaves the same from every entry point. A scope
// runs only commands that take no required parameter (prCommandRegistryIsDirect). A disabled command does not run. A
// command runs during the build of the round whose input asked for it. Studio Kit's menu items, buttons and command
// palette run commands the same way (pr/kit_command.h).

#pragma once

#include "pr/base.h"
#include "pr/command.h"
#include "pr/command_keymap.h"
#include "pr/string_view.h"
#include "pr/ui.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Puts the bindings of `context` on `node` for the current round, and runs the command of a chord that was routed to
// the node.
//
// The bindings apply while focus is within the node. On the root, they also apply while nothing has focus (pr/ui.h). A
// Normal binding claims its chord, and an Override binding captures it. A chord is left unbound if its command is
// missing or takes a required parameter. A chord whose command is disabled is taken and does nothing.
//
// A held chord runs its command again each time the key repeats, unless the command runs once for each press
// (PrCommandFlag_Once).
//
// A press inside the node that lands on nothing focusable focuses the node (PrUiFlag_PressFocus). The node's bindings
// then take the keys while the user works in that region.
void prCommandScope(PrUi* ui, PrLayoutNode node, const PrCommandRegistry* registry, const PrCommandKeymap* keymap,
    PrStringView context);

// Returns true if `title` and `category` match `query`. They match if each word of the query appears in one of them, in
// any case. An empty query matches every command.
bool prCommandMatches(PrStringView query, PrStringView title, PrStringView category);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
