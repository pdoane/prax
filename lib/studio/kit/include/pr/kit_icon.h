// Studio Kit's icons: Lucide's glyphs, drawn as paths at any size.

#pragma once

#include "pr/kit.h"
#include "pr/kit_text.h"
#include "pr/path.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Returns the icon's name, as Lucide names it.
PrStringView prKitIconName(PrKitIconKind icon);

// Returns true if the icon is filled rather than stroked.
bool prKitIconFilled(PrKitIconKind icon);

// How an icon is drawn: a square of `size` px, or 16 px for zero, in `tone`.
PR_NO_PADDING_BEGIN
typedef struct PrKitIconStyle {
    float size;
    PrKitTone tone;
    uint8_t _pad[3];
} PrKitIconStyle;
PR_NO_PADDING_END

// An icon of the set.
typedef struct PrKitIcon {
    PrUiKey key;
    PrKitIconKind icon;
} PrKitIcon;

// Creates an icon under `parent`.
PrLayoutNode prKitIcon(PrKit* kit, PrLayoutNode parent, PrKitIcon icon, PrKitIconStyle style);

// An icon of the caller's: `path`, on the set's grid of 24 units, in `color`, stroked at the set's stroke width if
// `stroked` is true and filled otherwise. `path` must outlive every round that draws it.
typedef struct PrKitPathIcon {
    PrUiKey key;
    const PrPath* path;
    PrColor color;
    bool stroked;
} PrKitPathIcon;

// Creates a path icon under `parent`. Its style's tone is not used.
PrLayoutNode prKitPathIcon(PrKit* kit, PrLayoutNode parent, PrKitPathIcon icon, PrKitIconStyle style);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
