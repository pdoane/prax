// Deflate encoding, bare or framed as zlib or gzip, of a whole input at once.
//
// rfc1950 - https://www.rfc-editor.org/rfc/rfc1950
// rfc1951 - https://www.rfc-editor.org/rfc/rfc1951
// rfc1952 - https://www.rfc-editor.org/rfc/rfc1952

#pragma once

#include "pr/base.h"
#include "pr/byte_view.h"
#include "pr/compress.h"

#include "compress/backends/builtin/src/inflate.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Returns the largest number of bytes that deflateEncode writes for `size` bytes of input in `format`.
size_t deflateBound(InflateFormat format, size_t size);

// Encodes `input` as one stream in `format` into `output`, and returns the number of bytes written. `output` must have
// room for the number of bytes that deflateBound returns. Each block is stored, coded with the fixed codes, or coded
// with its own dynamic codes, whichever is smallest.
size_t deflateEncode(InflateFormat format, PrCompressLevel level, PrByteView input, uint8_t* output);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
