// The image cache: the texture sources of the gallery's files. A texture catalog names the sources by their paths.
//
// A request returns the source of a file once the catalog has opened it. A widget draws the source, and the window's
// texture cache reads the texels that the widget shows. A thumbnail reads a preview of about its own size, so a grid of
// hundreds of images stays small. The viewport reads the full image (pr/texture_cache.h). The cache keeps no pixels. A
// source holds its file's path.
//
// A cache is used from the thread that builds the frames.

#pragma once

#include "pr/base.h"
#include "pr/function.h"
#include "pr/image.h"
#include "pr/string.h"
#include "pr/task.h"
#include "pr/texture.h"

#include <stdint.h>

PR_ASSUME_NONNULL_BEGIN

namespace alchemy {

using namespace pr;

// The sources of image files, by path.
struct ImageCache;

// Creates a cache that opens files on `pool` with `codec`. Both must outlive the cache. The cache calls `wake` from a
// thread of the pool when a file opens. Destroying the cache blocks until the opens in flight complete.
ImageCache* imageCacheCreate(PrTaskPool* pool, const PrImageCodecSet* codec, Function<void()> wake);
void imageCacheDestroy(ImageCache* PR_NULLABLE cache);

// Returns the image of the file at `path`, or null while the file opens or if it does not open. The first request
// starts the open. The image stays valid until the next pump.
const PrTextureSource* PR_NULLABLE imageCacheGet(ImageCache* cache, PrStringView path);

// Returns true if the file at `path` did not open as an image.
bool imageCacheFailed(ImageCache* cache, PrStringView path);

// Forgets the file at `path`, for example after the file changes. A later request opens it again.
void imageCacheForget(ImageCache* cache, PrStringView path);

// Releases the images that the last frame's requests returned. Called once per frame, before the build.
void imageCachePump(ImageCache* cache);

// Returns an image decoded from `bytes`, or null if the codec cannot read them. The image keeps a copy of the bytes.
// The caller releases it.
const PrTextureSource* PR_NULLABLE imageDecode(const PrImageCodecSet* codec, const void* bytes, size_t size);

// Reads the first level of the image as Rgba8 into `out`. `out` must have the image's extent. The pixels have `alpha`,
// or are opaque if the image is opaque. Returns false if the image cannot be read.
bool imageReadRgba8(const PrTextureSource* image, PrTextureAlpha alpha, PrPixels out);

} // namespace alchemy

PR_ASSUME_NONNULL_END
