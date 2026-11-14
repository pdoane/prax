// Alchemy's interface: a menu bar, a dock of the parameters, the viewport and the browser, and a status bar. Each round
// declares it from the state, and every verb that it offers runs a command.

#pragma once

#include "pr/base.h"
#include "pr/kit.h"
#include "pr/kit_dock.h"
#include "pr/string.h"

#include "apps/alchemy/commands.h"

PR_ASSUME_NONNULL_BEGIN

namespace alchemy {

using namespace pr;

// What the interface keeps between rounds besides the state: the dock's arrangement and the palette's query.
struct View {
    PrKitDock* dock = nullptr;
    String paletteQuery;
    String browserTitle;                            // the browser's tab, which counts the gallery's images

    // The title bar's server button, which the server's dialog floats from, as the current round made it.
    PrLayoutNode server = kPrLayoutNodeNone;

    // Whether each dialog was told of the state's request.
    bool deleteShown = false;
    bool serverShown = false;
    bool importShown = false;
};

// Creates the dock's arrangement, or frees it.
void viewInit(View& view);
void viewReset(View& view);

// Declares the title bar's content in `bar`, before each round's build of the view.
void buildTitleBarRow(PrKit* kit, PrLayoutNode bar, View& view, Commands& commands);

// Declares the interface under `root`, a surface that fills the window below its title bar.
void buildView(PrKit* kit, PrLayoutNode root, View& view, Commands& commands);

} // namespace alchemy

PR_ASSUME_NONNULL_END
