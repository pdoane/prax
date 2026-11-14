// Studio Kit's themes: the colors, the type scale and the measures its widgets are drawn in.
//
// A theme is data. Its text roles name faces of a font set that the host owns, and the kit sets its text in those
// faces.

#pragma once

#include "pr/base.h"
#include "pr/color.h"
#include "pr/font_set.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// ---------------------------------------------------------------------------------------------------------------------
// Palettes

// A theme's colors, in straight alpha. Accent and semantic colors keep their roles across variants. The panel, rule and
// text ramps give each variant its contrast.
typedef struct PrKitPalette {
    PrColor accent;
    PrColor accentWeak;             // a selected item's wash
    PrColor textSelection;          // the background of selected text, which the text stays readable over
    PrColor accentText;             // text over an accent fill
    PrColor accentHover;
    PrColor accentPress;
    PrColor success;
    PrColor warning;
    PrColor danger;

    // The panels, from the application's background up to a raised control, and the viewport's.
    PrColor bg0;
    PrColor bg1;
    PrColor bg2;
    PrColor bg3;
    PrColor bg4;
    PrColor bgViewport;

    // Rules, from the faintest separator to a control's border.
    PrColor line1;
    PrColor line2;
    PrColor line3;

    // Text colors, from the primary down to the faintest.
    PrColor text1;
    PrColor text2;
    PrColor text3;
    PrColor text4;
    PrColor textInverse;

    PrColor hudFill;                // the fill of a chip over the viewport
    PrColor hudBorder;
    PrColor axisX;
    PrColor axisY;
    PrColor axisZ;
    PrColor axisW;
} PrKitPalette;

// The palettes Studio ships.
enum {
    PrKitThemeVariant_Dark,
    PrKitThemeVariant_Light,
};
typedef uint8_t PrKitThemeVariant;

// ---------------------------------------------------------------------------------------------------------------------
// Type

// The color that text or an icon is set in. Auto is the default of where the tone is used: the tone of a text's role
// (PrKitType), and the primary tone elsewhere.
enum {
    PrKitTone_Auto,
    PrKitTone_Primary,
    PrKitTone_Secondary,
    PrKitTone_Muted,
    PrKitTone_Faint,
    PrKitTone_Accent,
    PrKitTone_Success,
    PrKitTone_Warning,
    PrKitTone_Danger,
    PrKitTone_Inverse,
    PrKitTone_AxisX,
    PrKitTone_AxisY,
    PrKitTone_AxisZ,
    PrKitTone_AxisW,
    PrKitTone_Count,
};
typedef uint8_t PrKitTone;

// What text does at the end of its box. Auto is the wrap of the text's role (PrKitType).
enum {
    PrKitWrap_Auto,
    PrKitWrap_Wrap,                 // breaks onto additional lines
    PrKitWrap_Nowrap,               // stays on one line, extending past the box
    PrKitWrap_Clip,                 // stays on one line, cut at the box
};
typedef uint8_t PrKitWrap;

// The part that text plays in a widget. Each role is a face and a step of the type scale.
enum {
    PrKitTextRole_Body,             // sans 12, regular
    PrKitTextRole_BodySmall,        // sans 11, regular
    PrKitTextRole_Strong,           // sans 12, medium
    PrKitTextRole_BodyItalic,       // sans 12, italic
    PrKitTextRole_Heading,          // sans 16, medium
    PrKitTextRole_Title,            // sans 22, medium
    PrKitTextRole_Display,          // sans 40, semibold
    PrKitTextRole_Caption,          // mono 10, medium, tracked: panel headers and strips
    PrKitTextRole_Meta,             // mono 10, regular: counts and tags
    PrKitTextRole_Mono,             // mono 11, regular: identifiers, paths and values
    PrKitTextRole_MonoStrong,       // mono 11, medium
    PrKitTextRole_MonoNum,          // mono 22, medium: numeric readouts
    PrKitTextRole_Count,
};
typedef uint8_t PrKitTextRole;

// The face and size of a text role, and the tone and wrap that text of the role takes when its style leaves them Auto.
// Letter spacing comes from the design, and the kit applies it once layout spaces letters.
typedef struct PrKitType {
    bool mono;                      // the theme's monospaced family rather than its ui family
    bool italic;
    PrKitTone tone;                 // Auto is the primary tone
    PrKitWrap wrap;                 // Auto wraps
    float size;                     // px
    float weight;                   // CSS weight
    float lineHeight;               // px
    float letterSpacing;            // px
} PrKitType;

// ---------------------------------------------------------------------------------------------------------------------
// Looks
//
// A look is the colors of a control in each of its states. The kit makes its looks from the theme's palette when the
// theme is set, so the palette's colors are named in one place, and a widget names the look it is drawn in.

// The states of a control that its look gives a fill for. A control blends between them as its hover and press ease.
enum {
    PrKitState_Rest,
    PrKitState_Hover,
    PrKitState_Press,
    PrKitState_Count,
};
typedef uint8_t PrKitState;

// The looks of the kit's controls.
enum {
    PrKitLookId_Button,
    PrKitLookId_ButtonPrimary,
    PrKitLookId_ButtonGhost,
    PrKitLookId_ButtonDanger,
    PrKitLookId_ButtonSelected,         // a button toggled on, as a toolbar's tool
    PrKitLookId_ButtonDisabled,
    PrKitLookId_ButtonGhostDisabled,
    PrKitLookId_Selectable,             // a box that the pointer selects, at rest
    PrKitLookId_SelectableSelected,
    PrKitLookId_Plain,                  // a part drawn only while hovered or pressed, as a chip's remove control
    PrKitLookId_Quiet,                  // the same, a step lighter, as a tab's close control

    // A tab, which draws its rule below it in the accent while chosen, and a segment of a segmented control.
    PrKitLookId_Tab,
    PrKitLookId_TabChosen,
    PrKitLookId_TabDisabled,
    PrKitLookId_TabChosenDisabled,
    PrKitLookId_Segment,
    PrKitLookId_SegmentChosen,
    PrKitLookId_SegmentDisabled,
    PrKitLookId_SegmentChosenDisabled,

    // The glyphs of a checkbox, a toggle's track and a radio, off and on, enabled and disabled.
    PrKitLookId_Check,
    PrKitLookId_CheckOn,
    PrKitLookId_CheckDisabled,
    PrKitLookId_CheckOnDisabled,
    PrKitLookId_Rail,
    PrKitLookId_RailDisabled,
    PrKitLookId_Radio,
    PrKitLookId_RadioOn,
    PrKitLookId_RadioOnDisabled,
    PrKitLookId_WindowButton,           // a window's minimize or maximize button in a custom title bar
    PrKitLookId_WindowClose,            // a window's close button, which turns to the danger color when hovered
    PrKitLookId_Count,
};
typedef uint16_t PrKitLookId;

// The fills of a look in each state, its border, and the colors of its text and its icons. A look that eases fades its
// fill between states; one that does not takes each state's fill at once, as does a look whose states share one fill.
typedef struct PrKitLook {
    PrColor fill[PrKitState_Count];
    PrColor border;
    PrColor text;
    PrColor icon;
    bool eases;
} PrKitLook;

// ---------------------------------------------------------------------------------------------------------------------
// Themes

// What a kit draws with. `fonts` must outlive every kit that the theme is set on. Text is set in the family `ui`, or in
// `mono` if its role's type is monospaced (PrKitType::mono).
typedef struct PrKitTheme {
    PrKitPalette palette;
    PrKitType types[PrKitTextRole_Count];   // by PrKitTextRole
    PrFontSet* PR_NULLABLE fonts;
    PrFontFamily ui;
    PrFontFamily mono;
} PrKitTheme;

// Returns Studio's theme of `variant`, which uses the generic families of `fonts`: system-ui for `ui`, and monospace
// for `mono`.
PrKitTheme prKitThemeStudio(PrKitThemeVariant variant, PrFontSet* PR_NULLABLE fonts);

// ---------------------------------------------------------------------------------------------------------------------
// Measures

// The spacing scale in px, on a base of two.
static const float kPrKitSpace1 = 2.0f;
static const float kPrKitSpace2 = 4.0f;
static const float kPrKitSpace3 = 6.0f;
static const float kPrKitSpace4 = 8.0f;
static const float kPrKitSpace5 = 10.0f;
static const float kPrKitSpace6 = 12.0f;
static const float kPrKitSpace8 = 16.0f;
static const float kPrKitSpace10 = 20.0f;
static const float kPrKitSpace12 = 24.0f;
static const float kPrKitSpace16 = 32.0f;

// The heights of controls in px. The smallest height is for compact rows that take no input.
static const float kPrKitHeightXxs = 14.0f;
static const float kPrKitHeightXs = 18.0f;
static const float kPrKitHeightSm = 22.0f;
static const float kPrKitHeightMd = 26.0f;
static const float kPrKitHeightLg = 30.0f;
static const float kPrKitHeightXl = 36.0f;

// The radius of a control's corners in px, which the kit draws once layout rounds corners.
static const float kPrKitRadius = 3.0f;

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
