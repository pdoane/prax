// The viewport's heads-up display: a component library of the editor's own, over lib/ui and lib/layout, which the
// editor composes with Studio Kit in one tree.
//
// The HUD shares nothing with the kit but the context. Its keys, its overlay, its edit gestures and its records are
// lib/ui's, so a kit widget may sit in a HUD panel, a HUD widget in a kit panel, and a kit menu opened from a HUD panel
// floats over both. Its look is its own: phosphor text on a smoked glass.

#pragma once

#include "pr/font_set.h"
#include "pr/layout_tree.h"
#include "pr/string_view.h"
#include "pr/ui.h"

PR_ASSUME_NONNULL_BEGIN

namespace editor {

// The HUD's widgets for one context: its font, and the owner of its records.
struct Hud;

// Creates a HUD for `ui`, with its text in the monospace family of `fonts`. `ui` and `fonts` must outlive the HUD.
Hud* hudCreate(PrUi* ui, PrFontSet* fonts);

// Destroys the HUD, and releases the records that it made. Call between rounds.
void hudDestroy(Hud* PR_NULLABLE hud);

// A layer over its parent, which holds panels in its corners and lets the pointer through to what it covers.
struct HudLayer {
    uint64_t key;
};

// Creates a layer under `parent`, which must be a containing block for positioned boxes.
PrLayoutNode hudLayer(Hud* hud, PrLayoutNode parent, HudLayer layer);

// The corner of its layer that a panel sits in.
enum {
    HudCorner_TopLeft,
    HudCorner_TopRight,
    HudCorner_BottomLeft,
    HudCorner_BottomRight,
};
typedef uint8_t HudCorner;

// A panel, which lays out what it holds in a column, or in a row if `row` is true.
struct HudPanel {
    uint64_t key;
    HudCorner corner;
    bool row;
};

// Creates a panel in a corner of `layer`, which a hudLayer call made in this round. Returns the panel, which takes any
// library's widgets as children.
PrLayoutNode hudPanel(Hud* hud, PrLayoutNode layer, HudPanel panel);

// A reading: a dim label and a bright value.
struct HudReadout {
    uint64_t key;
    PrStringView label;
    PrStringView value;
};

// Creates a reading under `parent`.
PrLayoutNode hudReadout(Hud* hud, PrLayoutNode parent, HudReadout readout);

// A switch bound to `value`, which shows `label` lit while the value is true.
struct HudSwitch {
    uint64_t key;
    PrStringView label;
    bool* value;
};

// Creates a switch under `parent`. A click, or Enter or Space while it has focus, flips the value as one edit gesture
// (prUiEdit). Returns the switch.
PrLayoutNode hudSwitch(Hud* hud, PrLayoutNode parent, HudSwitch control);

// Shows `text` in the overlay below `anchor`, a node that the round created, once the pointer has rested on the anchor
// for a moment.
void hudHint(Hud* hud, PrLayoutNode anchor, PrStringView text);

} // namespace editor

PR_ASSUME_NONNULL_END
