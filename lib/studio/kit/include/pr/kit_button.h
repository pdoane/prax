// Studio Kit's buttons: text and icon buttons, toolbar buttons, and the badges and key caps beside them.

#pragma once

#include "pr/kit.h"
#include "pr/kit_icon.h"
#include "pr/kit_text.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// The height a control is drawn at. Zero is medium.
enum {
    PrKitControlSize_Md,            // 26 px
    PrKitControlSize_Sm,            // 22 px
    PrKitControlSize_Xs,            // 18 px
    PrKitControlSize_Lg,            // 30 px
};
typedef uint8_t PrKitControlSize;

// What a button's fill says. Zero is the default, a raised button.
enum {
    PrKitButtonVariant_Default,
    PrKitButtonVariant_Primary,     // the accent fill
    PrKitButtonVariant_Ghost,       // no fill until hovered
    PrKitButtonVariant_Danger,
};
typedef uint8_t PrKitButtonVariant;

// How a button looks. Zero is a medium raised button.
typedef struct PrKitButtonStyle {
    PrKitButtonVariant variant;
    PrKitControlSize size;
} PrKitButtonStyle;

// What a button shows: an icon, text, or both, as wide as it is tall if it shows an icon alone. `selected` toggles the
// button on, as for a toolbar's tool.
typedef struct PrKitButton {
    uint64_t key;
    PrStringView text;
    PrKitIconKind icon;
    bool selected;
    bool disabled;
} PrKitButton;

// Creates a button. It is activated (prKitActivated) by a click, or by Enter or Space while it has focus. A button made
// in a button group shares the group's border.
PrLayoutNode prKitButton(PrKit* kit, PrLayoutNode parent, PrKitButton button, PrKitButtonStyle style);

// What a badge says.
enum {
    PrKitBadgeVariant_Neutral,
    PrKitBadgeVariant_Accent,
    PrKitBadgeVariant_Success,
    PrKitBadgeVariant_Warning,
    PrKitBadgeVariant_Danger,
};
typedef uint8_t PrKitBadgeVariant;

// A small tag of text, and how it looks.
typedef struct PrKitBadge {
    uint64_t key;
    PrStringView text;
} PrKitBadge;
typedef struct PrKitBadgeStyle {
    PrKitBadgeVariant variant;
} PrKitBadgeStyle;

// Creates a badge.
PrLayoutNode prKitBadge(PrKit* kit, PrLayoutNode parent, PrKitBadge badge, PrKitBadgeStyle style);

// A key cap, in the style of a shortcut in a menu.
typedef struct PrKitKbd {
    uint64_t key;
    PrStringView text;
} PrKitKbd;

// Creates a key cap.
PrLayoutNode prKitKbd(PrKit* kit, PrLayoutNode parent, PrKitKbd kbd);

// A status: one or two characters in a pill washed in its tone, such as the M, A and D of a file's state in version
// control.
typedef struct PrKitStatus {
    uint64_t key;
    PrStringView text;
} PrKitStatus;
typedef struct PrKitStatusStyle {
    PrKitTone tone;
} PrKitStatusStyle;

// Creates a status.
PrLayoutNode prKitStatus(PrKit* kit, PrLayoutNode parent, PrKitStatus status, PrKitStatusStyle style);

// ---------------------------------------------------------------------------------------------------------------------
// Chips

// What a chip shows: a label, and a control that removes it if `removable` is true.
typedef struct PrKitChip {
    uint64_t key;
    PrStringView label;
    bool removable;
    bool disabled;
} PrKitChip;

// Creates a chip.
PrLayoutNode prKitChip(PrKit* kit, PrLayoutNode parent, PrKitChip chip);

// ---------------------------------------------------------------------------------------------------------------------
// Button groups

// A row of buttons that share one border, divided by a rule between each two of them.
typedef struct PrKitButtonGroup {
    uint64_t key;
    bool disabled;
} PrKitButtonGroup;

// Creates a button group. The buttons made directly under it draw no border of their own.
PrLayoutNode prKitButtonGroup(PrKit* kit, PrLayoutNode parent, PrKitButtonGroup group);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
