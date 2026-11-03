// Studio Kit's surfaces: the grounds that the application, its panels, bars and cards are drawn on.

#pragma once

#include "pr/kit.h"
#include "pr/kit_box.h"
#include "pr/kit_text.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// The ground that a surface is drawn on, from the application's ground up to a selected row.
enum {
    PrKitGround_None,
    PrKitGround_App,                // bg0
    PrKitGround_Panel,              // bg1
    PrKitGround_Header,             // bg2
    PrKitGround_Raised,             // bg3
    PrKitGround_Select,             // the accent wash
    PrKitGround_Viewport,
    PrKitGround_Hud,                // a chip over the viewport
};
typedef uint8_t PrKitGround;

// The sides on which a surface draws a rule, in its theme's faintest line color.
enum {
    PrKitRuleFlag_Top = 1u << 0,
    PrKitRuleFlag_Right = 1u << 1,
    PrKitRuleFlag_Bottom = 1u << 2,
    PrKitRuleFlag_Left = 1u << 3,
    PrKitRuleFlag_All = 0xf,
};
typedef uint8_t PrKitRuleFlags;

// How a surface looks: a box drawn on a ground, with rules on some of its sides.
PR_NO_PADDING_BEGIN
typedef struct PrKitSurfaceStyle {
    PrKitBoxStyle box;
    PrKitGround ground;
    PrKitRuleFlags rules;
    uint8_t _pad[2];
} PrKitSurfaceStyle;
PR_NO_PADDING_END

// Creates a surface under `parent`, or as the tree's root if `parent` is kPrLayoutNodeNone, as the application's ground
// is.
PrLayoutNode prKitSurface(PrKit* kit, PrLayoutNode parent, PrKitBox box, PrKitSurfaceStyle style);

// A box that the pointer selects, such as a row of a navigation list or a cell of a grid of choices: drawn on the
// select ground while `selected` is true, on the raised ground while the pointer is over it, and on none otherwise.
typedef struct PrKitSelectable {
    PrUiKey key;
    bool selected;
    bool disabled;
} PrKitSelectable;

// Creates a selectable box under `parent`, laid out by `style`. It is activated (prKitActivated) by a click, or by
// Enter or Space while it has focus.
PrLayoutNode prKitSelectable(PrKit* kit, PrLayoutNode parent, PrKitSelectable selectable, PrKitBoxStyle style);

// A panel's header: a bar on the header ground with a rule below it, which shows the title in the caption role, then
// `meta`, if any, faint at its end.
typedef struct PrKitPanelHeader {
    PrUiKey key;
    PrStringView title;
    PrStringView meta;
} PrKitPanelHeader;

// Creates a panel's header, and returns the bar, which can take more children after the meta.
PrLayoutNode prKitPanelHeader(PrKit* kit, PrLayoutNode parent, PrKitPanelHeader header);

// A square filled with `color` and ringed in the theme's rule color, and its side in px.
typedef struct PrKitSwatch {
    PrUiKey key;
    PrColor color;
} PrKitSwatch;
typedef struct PrKitSwatchStyle {
    float size;
} PrKitSwatchStyle;

// Creates a swatch.
PrLayoutNode prKitSwatch(PrKit* kit, PrLayoutNode parent, PrKitSwatch swatch, PrKitSwatchStyle style);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
