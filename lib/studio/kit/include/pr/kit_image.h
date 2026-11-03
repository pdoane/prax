// Studio Kit's images: pictures fitted into the boxes that they are placed in.

#pragma once

#include "pr/draw_image.h"
#include "pr/kit.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// How an image fills its box. The fit never changes the box's size.
enum {
    PrKitImageFit_Fill,             // stretched over the box; the image's ratio is not kept
    PrKitImageFit_Contain,          // the whole image, as large as fits, with the ratio kept
    PrKitImageFit_Cover,            // the whole box, cropping the image, with the ratio kept
    PrKitImageFit_None,             // the image's own size, cropped by the box
    PrKitImageFit_ScaleDown,        // as Contain, but never larger than the image's own size
};
typedef uint8_t PrKitImageFit;

// The content of an image widget: the image that it shows.
typedef struct PrKitImage {
    PrUiKey key;
    const PrDrawImage* image;
} PrKitImage;

// The style of an image widget: the image fitted into its box as `fit` says, at `alignX` and `alignY` in the room that
// the fit leaves or crops (Start, Center or End; Auto centers), in a box that `place` places in its parent. A box with
// no size of its own takes the image's size, a px for each pixel.
typedef struct PrKitImageStyle {
    PrKitImageFit fit;
    PrKitAlign alignX;
    PrKitAlign alignY;
    PrKitPlace place;
} PrKitImageStyle;

// Creates an image widget under `parent`.
PrLayoutNode prKitImage(PrKit* kit, PrLayoutNode parent, PrKitImage image, PrKitImageStyle style);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
