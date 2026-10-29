// Studio Kit's colors: color fields, and the picker that edits a color in HSV, in OKLCH, or from a tonal ramp.

#pragma once

#include "pr/color.h"
#include "pr/kit.h"
#include "pr/kit_button.h"
#include "pr/string.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// ---------------------------------------------------------------------------------------------------------------------
// Fields

// The content of a color field: the color that it shows and edits.
typedef struct PrKitColorField {
    uint64_t key;
    PrColor* color;
    bool disabled;
} PrKitColorField;

// The style of a color field. Zero is medium.
typedef struct PrKitColorFieldStyle {
    PrKitControlSize size;
} PrKitColorFieldStyle;

// Creates a color field that shows its color as a swatch and its hex value, and edits it with a picker. A click, or
// Enter or Space while the field has focus, opens the picker in a popover below the field, or closes it.
PrLayoutNode prKitColorField(PrKit* kit, PrLayoutNode parent, PrKitColorField field, PrKitColorFieldStyle style);

// ---------------------------------------------------------------------------------------------------------------------
// Pickers

// The plane that a color picker edits a color on.
enum {
    PrKitColorMode_Hsv,             // saturation across and value up, at the hue that a strip of hues chooses
    PrKitColorMode_Oklch,           // lightness across and chroma up, at the perceptual hue that a strip chooses
    PrKitColorMode_Ramp,            // perceptual hue across and chroma up, which make a ramp of tones to choose from
};
typedef uint8_t PrKitColorMode;

// What a color picker edits: `color`, and its alpha unless `opaque` is true. `mode` is the plane that the picker opens
// on; its tabs choose another, which the picker keeps. `gamut` is the RGB space that its perceptual planes work in,
// which a picker for the display's colors takes from the display (prUiDisplay).
typedef struct PrKitColorPicker {
    uint64_t key;
    PrColor* color;
    PrKitColorMode mode;
    PrColorSpace gamut;
    bool opaque;
} PrKitColorPicker;

// Creates a color picker: tabs of its modes; the mode's plane, with a marker at the color, beside a strip of hues and a
// strip of alpha unless the picker is opaque; fields of the mode's own coordinates; the color's swatch and its hex
// value; and a field for each channel, from 0 to 255.
//
// The HSV and OKLCH planes and the strips set the color at the pointer when pressed, and follow a drag. The perceptual
// planes work in the picker's gamut, by default the display's that the frame is shown on: they show a color past it as
// CSS Color 4 maps it into the gamut, pick colors within it, which may lie past sRGB, and mark where the gamut ends
// with a line, and where sRGB ends inside a wider gamut with a dashed one. The ramp's plane sets the hue and the chroma
// of a ramp of eleven tones, light to dark, and a click on a tone sets the color to it, keeping its alpha. Each drag,
// and each edit of a field, is one gesture. The picker keeps a mode's coordinates while the color alone leaves them
// undefined, as a gray leaves its hue.
PrLayoutNode prKitColorPicker(PrKit* kit, PrLayoutNode parent, PrKitColorPicker picker);

// ---------------------------------------------------------------------------------------------------------------------
// Hex values

// Writes `color` to `out`, which holds 9 bytes, as #RRGGBB if the color is opaque and #RRGGBBAA otherwise. Returns a
// view of the bytes that it wrote to `out`.
PrStringView prKitColorHex(PrColor color, char* out);

// Parses #RRGGBB or #RRGGBBAA, with the # optional, into `out`. Returns false, and leaves `out` unchanged, if `text` is
// neither.
bool prKitColorParseHex(PrStringView text, PrColor* out);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
