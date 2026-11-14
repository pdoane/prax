// Studio Kit's title bar: the band at the top of a window with a custom title bar (PrAppWindowDecoration_Custom).

#pragma once

#include "pr/app.h"
#include "pr/kit.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// What a title bar shows: the window's title bar as its frame reports it (PrAppFrame::titleBar), and whether the window
// is maximized, which turns the maximize button into a restore button.
typedef struct PrKitTitleBar {
    PrUiKey key;
    PrAppTitleBar bar;
    bool maximized;
} PrKitTitleBar;

// Creates a title bar in `parent` and returns the row within it that holds the program's content, such as a title,
// menus or tabs. The bar is as tall as the window's title bar, or taller if its content needs more. The row fills the
// band between the insets of the system's buttons and, where the program draws the window's buttons, the minimize,
// maximize and close buttons at its end, as the platform places them. The band is the title bar's drag region, and the
// controls in the row take the user's presses (prUiTitleBarRegion). A window without a custom title bar, as one that is
// fullscreen, gets a plain row as tall as its content, which takes no drag.
PrLayoutNode prKitTitleBar(PrKit* kit, PrLayoutNode parent, PrKitTitleBar titleBar);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
