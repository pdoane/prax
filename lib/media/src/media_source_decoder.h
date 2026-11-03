// The decoder of a media source: it reads the coded frames of track buffers as they arrive, and decodes them with a
// codec's sample decoders.

#pragma once

#include "pr/media.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// prMediaDecoderCreateFromSource, after `info` is zeroed.
PrStatus mediaSourceDecoderCreate(PrMediaCodec* codec, PrMediaSourceBuffer* PR_NONNULL const* PR_NONNULL buffers,
    uint32_t count, PrMediaDecoder* PR_NULLABLE* PR_NONNULL out, PrMediaInfo* info);

// prMediaDecoderSetSourceEnded.
void mediaSourceDecoderSetEnded(PrMediaDecoder* decoder, bool ended);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
