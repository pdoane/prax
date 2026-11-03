// Alchemy's interface: a menu bar, a dock of the parameters, the viewport and the browser, and a status bar. Each round
// declares it from the state, and every verb that it offers runs a command.

#pragma once

#include "pr/base.h"
#include "pr/kit.h"
#include "pr/kit_dock.h"

#include "examples/alchemy/commands.h"

#include <string>

PR_ASSUME_NONNULL_BEGIN

namespace alchemy {

// What the interface keeps between rounds besides the state: the dock's arrangement and the palette's query.
struct View {
    PrKitDock* dock = nullptr;
    std::string paletteQuery;

    // Whether each dialog was told of the state's request, and whether the import's has taken focus.
    bool deleteShown = false;
    bool serverShown = false;
    bool importShown = false;
    bool importFocused = false;
};

// Creates the dock's arrangement, or frees it.
void viewInit(View& view);
void viewReset(View& view);

// Declares the interface under `root`, a surface that fills the window.
void buildView(PrKit* kit, PrLayoutNode root, View& view, Commands& commands);

} // namespace alchemy

PR_ASSUME_NONNULL_END
