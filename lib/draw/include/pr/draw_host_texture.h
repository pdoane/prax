// A host texture: a picture that a host puts on the device itself and draws through draw lists, such as a frame of a
// video or of an animation.
//
// A texture source is the right form for a picture that is drawn again and again, because the texture cache realizes it
// once, at the size at which it shows, and evicts it later. A host texture is the form for a picture that shows for a
// moment and is never asked for again. The host imports each new picture, draws it, and lets it go.
//
// A host texture is shared by reference. The host holds it while it shows, and each draw list that draws it holds it
// until the list is emptied or freed. Its device must outlive it: the host releases its host textures, and empties the
// lists that draw them, before it destroys the device.

#pragma once

#include "pr/base.h"
#include "pr/draw.h"
#include "pr/draw_list.h"
#include "pr/gpu.h"
#include "pr/status.h"
#include "pr/texture.h"
#include "pr/vec.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// A picture on a device, shared by reference.
typedef struct PrDrawHostTexture PrDrawHostTexture;

// Puts `image` on `device` as a host texture whose texels have the alpha `alpha`, and writes the texture to `*texture`
// with a reference for the caller. The device imports the image in place if it can, and otherwise copies its texels
// (prGpuTextureImport).
//
// `*texture` is null, or the host texture of an earlier import on the same device, whose reference the call takes. If
// nothing else holds that texture, the call imports into it. Otherwise the call releases it and makes another, so the
// lists that still draw the earlier picture keep it.
//
// On failure, `*texture` is left as it was, and the call returns the status of the import.
PrStatus prDrawHostTextureImport(PrGpuDevice* device, const PrGpuImport* image, PrTextureAlpha alpha,
    PrDrawHostTexture* PR_NULLABLE* PR_NONNULL texture);

// Adds or drops a reference. The last release frees the texture on its device.
void prDrawHostTextureRetain(PrDrawHostTexture* texture);
void prDrawHostTextureRelease(PrDrawHostTexture* PR_NULLABLE texture);

// Returns the extent of the texture in texels, and the device that it is on.
PrUVec2 prDrawHostTextureExtent(const PrDrawHostTexture* texture);
PrGpuDevice* prDrawHostTextureDevice(const PrDrawHostTexture* texture);

// Returns a source of the whole texture for `list` to draw, and makes the list hold the texture.
PrDrawSource prDrawSourceHostTexture(PrDrawList* list, PrDrawHostTexture* texture);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
