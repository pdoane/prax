// dotLottie: animations and their images in a ZIP archive.
//
// dotlottie - https://dotlottie.io/spec/
//
// A .lottie file is a ZIP archive of a manifest, one or more animations and the images that they name. An archive opens
// bytes, and decompresses them with a backend. The caller must keep the backend alive for the archive's lifetime, and
// the bytes alive while they are open.

#pragma once

#include "pr/base.h"
#include "pr/byte_view.h"
#include "pr/compress.h"
#include "pr/lottie.h"
#include "pr/status.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// A reader of .lottie archives. It holds at most one archive's bytes open at a time.
typedef struct PrLottieArchive PrLottieArchive;

// Creates an archive with nothing open. The archive decompresses with `codec`, and keeps its storage from one open to
// the next.
PrLottieArchive* prLottieArchiveCreate(PrCompressCodec* codec);

// Closes the open bytes, if any, and destroys the archive.
void prLottieArchiveDestroy(PrLottieArchive* PR_NULLABLE archive);

// Opens `bytes`, after closing any bytes already open. The archive's animations are the ones its manifest lists, or
// every JSON file in its animations folder if the manifest lists none. Returns InvalidData if the bytes are not a ZIP
// archive, and leaves nothing open.
PrStatus prLottieArchiveOpen(PrLottieArchive* archive, PrByteView bytes);

// Closes the open bytes, leaving no animations.
void prLottieArchiveClose(PrLottieArchive* archive);

// Returns the number of animations in the open bytes.
uint32_t prLottieArchiveAnimationCount(const PrLottieArchive* archive);

// Returns the id of the animation at `index`, which must be below the count. The id is the one in the manifest, or the
// file name without its extension.
PrStringView prLottieArchiveAnimationId(const PrLottieArchive* archive, uint32_t index);

// Reads the archive's animation at `index` as prLottieAnimationCreate does. The files it names come from the archive,
// regardless of the options' loader. Returns InvalidData if its entry cannot be read, and otherwise the status of
// reading the animation.
PrStatus prLottieAnimationCreateFromArchive(const PrLottieArchive* archive, uint32_t index,
                                            const PrLottieOptions* PR_NULLABLE options,
                                            PrLottieAnimation* PR_NULLABLE* PR_NONNULL out);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
