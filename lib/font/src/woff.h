// WOFF and WOFF2: the font file that a web font file holds.
//
// woff1 - https://www.w3.org/TR/WOFF/
// woff2 - https://www.w3.org/TR/WOFF2/

#pragma once

#include "pr/array.h"
#include "pr/base.h"
#include "pr/byte_view.h"
#include "pr/compress.h"
#include "pr/status.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Decodes the WOFF or WOFF2 file in `bytes` into the font file it holds, a single font or a collection, and writes it
// to `out`, which must be empty. `codec` decompresses the tables of the file: zlib for WOFF, Brotli for WOFF2.
//
// Returns InvalidData for bytes that are neither format, or that the rules of the format reject. Returns NotSupported
// if `codec` does not decode the compression of the file. On failure, `out` holds nothing that is meaningful.
PrStatus woffDecode(PrByteView bytes, PrCompressCodec* codec, Array<uint8_t>* out);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
