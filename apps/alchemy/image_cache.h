// The image cache: the gallery's files decoded into images that a frame draws, at full size for the viewport and as
// thumbnails for the browser.
//
// A request starts a decode on the task pool and returns null until it lands. The cache's pump adopts the decodes that
// landed, on the thread that builds the frames, and frees the images that no frame has asked for in a while. A
// thumbnail is the image halved until it fits kThumbnailMaxEdge, so a grid of hundreds of images stays small.

#pragma once

#include "pr/base.h"
#include "pr/draw_image.h"
#include "pr/function.h"
#include "pr/image.h"
#include "pr/string.h"
#include "pr/task.h"

#include <stdint.h>

PR_ASSUME_NONNULL_BEGIN

namespace alchemy {

using namespace pr;

// The longest edge of a thumbnail, in pixels.
constexpr uint32_t kThumbnailMaxEdge = 256;

// The sizes that the cache keeps an image at.
enum {
    ImageScale_Full,
    ImageScale_Thumbnail,
};
typedef uint8_t ImageScale;

// The decoded images of files, by path and scale.
struct ImageCache;

// Creates a cache that decodes on `pool` with `codec`, which must outlive it, and calls `wake` from the pool's threads
// when a decode lands. Freeing waits for the decodes in flight.
ImageCache* imageCacheCreate(PrTaskPool* pool, PrImageCodec* codec, Function<void()> wake);
void imageCacheDestroy(ImageCache* PR_NULLABLE cache);

// Returns the image of the file at `path` at `scale`, or null while it decodes or if it cannot be decoded. The first
// request starts the decode. The image stays valid until the next pump.
const PrDrawImage* PR_NULLABLE imageCacheGet(ImageCache* cache, PrStringView path, ImageScale scale);

// Returns true if the file at `path` could not be decoded.
bool imageCacheFailed(ImageCache* cache, PrStringView path, ImageScale scale);

// Forgets the file at `path`, so that a later request decodes it again, as after the file changes.
void imageCacheForget(ImageCache* cache, PrStringView path);

// Adopts the decodes that landed, and frees the images that the frames before have not asked for. Returns true if a
// decode landed, which the next frame shows. Called once per frame, before the build.
bool imageCachePump(ImageCache* cache);

// Returns an image decoded from `bytes`, which the caller releases, or null if the codec cannot read them.
const PrDrawImage* PR_NULLABLE imageDecode(PrImageCodec* codec, const void* bytes, size_t size);

// Returns a copy of `image` halved until it fits `maxEdge`, which the caller releases, or a new reference to `image` if
// it already fits or is not in 8 bits.
const PrDrawImage* imageShrink(const PrDrawImage* image, uint32_t maxEdge);

} // namespace alchemy

PR_ASSUME_NONNULL_END
