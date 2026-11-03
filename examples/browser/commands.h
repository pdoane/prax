// The browser's commands and key bindings (pr/command.h), which the chrome's buttons and menus, and the keyboard, run
// alike.

#pragma once

#include "pr/app.h"
#include "pr/base.h"
#include "pr/raw_string.h"
#include "pr/string_view.h"

#include "examples/browser/browser.h"

#include <string>

PR_ASSUME_NONNULL_BEGIN

namespace browser {

// The ids of the commands that the chrome's own controls run.
constexpr const char* kCommandNewTab = "tab.new";
constexpr const char* kCommandBack = "page.back";
constexpr const char* kCommandForward = "page.forward";
constexpr const char* kCommandReload = "page.reload";
constexpr const char* kCommandToggleFavorite = "favorites.toggle";
constexpr const char* kCommandShowHistory = "history.show";
constexpr const char* kCommandDeleteBrowsingData = "history.deleteBrowsingData";
constexpr const char* kCommandZoomReset = "view.zoomReset";

// Creates the browser's registry of commands and its keymap, or frees them.
void commandsCreate(Browser& b);
void commandsDestroy(Browser& b);

// Runs the command `id`, which takes no arguments, unless it is disabled. Asks the window for a frame to show what it
// changed.
void commandRun(Browser& b, PrStringView id);

// Returns true if the command `id` is enabled, or checked.
bool commandIsEnabled(const Browser& b, PrStringView id);
bool commandIsChecked(const Browser& b, PrStringView id);

// Returns the title of the command `id`.
PrStringView commandTitle(const Browser& b, PrStringView id);

// Returns the chord that runs the command `id`, as the platform shows a shortcut, or empty if no chord is bound to it.
std::string commandShortcut(const Browser& b, PrStringView id);

// Runs the command that the chord of `event`, a KeyDown, is bound to, for keys that go to the page rather than through
// the chrome's scope (prCommandScope). Returns true if the chord is bound, whether or not its command was enabled.
bool commandsRunChord(Browser& b, const PrAppEvent& event);

} // namespace browser

PR_ASSUME_NONNULL_END
