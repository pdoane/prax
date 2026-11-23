// The interface that a texture format implements (pr/texture.h). Only the library of a file format includes this
// header. lib/ktx, lib/dds and lib/image do.

#pragma once

#include "pr/base.h"
#include "pr/blob.h"
#include "pr/status.h"
#include "pr/string_view.h"
#include "pr/texture.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// A texture format: the functions that make sources from the bytes of one file format.
//
// - `destroyFn` frees the format. A format with no state of its own leaves it null.
// - `supportsTypeFn` returns true if the format reads the MIME type `type`, which is lowercase and has no parameters.
// - `createFn` creates a source for `bytes`. The source retains `bytes` and reads them again for each read. `blob`
//   holds the same bytes, as the format set just read them. The function reads the texture's description from `blob`
//   and does not keep it. It returns NotSupported if the bytes are not in the format after all.
//
//   If `partial` is set, `blob` holds the first bytes of a resource that is still arriving (prBlobSourceProgress). A
//   format that shows pictures as the bytes arrive makes a source whose picture changes with them, once the bytes
//   describe the texture. Any other failure, such as Truncated from a format that needs every byte, makes the set ask
//   again when more bytes have arrived, and with every byte at the end.
struct PrTextureFormat {
    void (*PR_NULLABLE destroyFn)(PrTextureFormat* format);
    bool (*supportsTypeFn)(const PrTextureFormat* format, PrStringView type);
    PrStatus (*createFn)(const PrTextureFormat* format, const PrBlobSource* bytes, const PrBlob* blob,
        const PrTextureLimits* limits, bool partial, const PrTextureSource* PR_NULLABLE* PR_NONNULL out);
};

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
