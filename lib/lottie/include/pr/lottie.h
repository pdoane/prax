// Lottie animations, drawn into draw lists.
//
// lottie-spec - https://lottie.github.io/lottie-spec/
//
// An animation is read once and drawn at any frame. An animation is immutable, and several threads may draw one at
// once.

#pragma once

#include "pr/base.h"
#include "pr/byte_view.h"
#include "pr/draw_list.h"
#include "pr/font_set.h"
#include "pr/image.h"
#include "pr/status.h"
#include "pr/string_view.h"
#include "pr/text.h"
#include "pr/texture_cache.h"
#include "pr/vec.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// An animation read from its JSON, with its images decoded and its text laid out.
typedef struct PrLottieAnimation PrLottieAnimation;

// The animation's own properties. Times are in frames. The animation plays from `inPoint` to `outPoint`, at `frameRate`
// frames per second.
typedef struct PrLottieInfo {
    PrVec2 extent;              // the root composition's width and height
    double frameRate;
    double inPoint;
    double outPoint;
} PrLottieInfo;

// Loads the bytes of a file that the animation names by `path`, exactly as the animation gives it. The host resolves
// the path against the animation's origin. Returns false if the file cannot be read. The bytes stay valid until
// prLottieAnimationCreate returns.
typedef bool (*PrLottieLoadFn)(void* PR_NULLABLE context, PrStringView path, PrByteView* out);

// The host services that reading uses.
typedef struct PrLottieOptions {
    // The codec that decodes image assets, whenever the cache reads them, so it must outlive the animation. Without a
    // codec, no images draw.
    const PrImageCodecSet* PR_NULLABLE codec;

    // The texture cache that image assets are drawn from. It must be on the device of the renderer that draws the
    // frames, and it must outlive the animation. Without a cache, images draw nothing.
    //
    // The animation asks the cache for each asset when it loads. A frame leaves out an image whose texture has not
    // arrived, or draws a smaller version of it, and the list that receives the frame awaits it (prDrawListAwaited), so
    // that a caller that keeps the list draws the frame again when the cache reports an arrival. The list holds the
    // images that it draws. A caller that keeps the list also draws the frame again after the cache moves to another
    // device (prTextureCacheGeneration).
    PrTextureCache* PR_NULLABLE textures;
    PrLottieLoadFn PR_NULLABLE load;        // loads the files that assets name; without it, nothing loads
    void* PR_NULLABLE loadContext;

    // The families that text is set in, with the set's aliases and default family. Unused for text whose glyphs the
    // animation carries.
    PrFontSet* PR_NULLABLE fonts;

    // Shapes text in `fonts`. Without it, only the animation's own glyphs draw.
    PrTextShaper* PR_NULLABLE shaper;
} PrLottieOptions;

// Returns options with no services.
PrLottieOptions prLottieOptionsDefault(void);

// Reads a Lottie animation from its JSON, decoding the images it contains or names and laying out its text. The options
// are used only during the call, and null options mean the defaults. Returns Syntax for text that is not JSON, and
// InvalidData for JSON that is not an animation. On failure, `*out` is null.
//
// In a build with PR_JS, an animation whose properties have expressions is drawn by one thread at a time.
PrStatus prLottieAnimationCreate(PrStringView json, const PrLottieOptions* PR_NULLABLE options,
    PrLottieAnimation* PR_NULLABLE* PR_NONNULL out);

// Frees the animation.
void prLottieAnimationDestroy(PrLottieAnimation* PR_NULLABLE animation);

// Returns the animation's size, frame rate, and in and out points.
PrLottieInfo prLottieAnimationInfo(const PrLottieAnimation* animation);

// Returns the frame shown `seconds` into the animation. The animation loops from its in point. Returns the in point if
// the animation has no length.
double prLottieAnimationFrameAt(const PrLottieAnimation* animation, double seconds);

// Draws the animation at `frame`, which may be fractional, into the list. It draws the root composition with its top
// left at the origin of the list's current transform, clipped to its size. The list's state is unchanged when the call
// returns.
void prLottieAnimationDraw(const PrLottieAnimation* animation, double frame, PrDrawList* list);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
