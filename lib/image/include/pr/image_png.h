// PNG text: the keyword and text pairs that a PNG file carries in its tEXt, zTXt and iTXt chunks, read from and written
// into the file's bytes without decoding its pixels (png-3/#11textinfo).

#pragma once

#include "pr/base.h"
#include "pr/byte_view.h"
#include "pr/compress.h"
#include "pr/status.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// The most bytes that a compressed text decompresses to before it is skipped.
enum { kPrImagePngTextMaxBytes = 16 * 1024 * 1024 };

// The longest keyword, in bytes (png-3/#11keywords).
enum { kPrImagePngKeywordMaxBytes = 79 };

// One text of a PNG file, in UTF-8. A tEXt or zTXt chunk stores Latin-1, which is converted.
typedef struct PrImagePngText {
    PrStringView keyword;
    PrStringView text;

    // The language tag and the keyword translated into that language, which only an iTXt chunk records. Each is empty
    // if the chunk records none.
    PrStringView language;
    PrStringView translatedKeyword;
} PrImagePngText;

// Called with each text of a PNG file. The text is valid only during the call.
typedef void (*PrImagePngTextFn)(void* PR_NULLABLE context, const PrImagePngText* text);

// Calls `fn` with each text of the PNG file `png`, in the order of its chunks. `compress` decompresses zTXt chunks and
// compressed iTXt chunks, and a null `compress` skips them. A text chunk that is malformed, or whose CRC does not
// match, is skipped. Returns InvalidData, before calling `fn`, if `png` does not start with the PNG signature and a
// header chunk, and Truncated if the chunks end before the image's end chunk.
PrStatus prImagePngEachText(PrByteView png, PrCompressCodec* PR_NULLABLE compress, PrImagePngTextFn fn,
                            void* PR_NULLABLE context);

// Writes the PNG file `png` with `text` under `keyword` as an uncompressed iTXt chunk with no language, in place of
// every text chunk of that keyword that the file has. The new chunk is placed where the first of those was, or before
// the image data if there was none. The other chunks are copied unchanged. `keyword` must be 1 through
// kPrImagePngKeywordMaxBytes bytes of printable ASCII, without a leading, trailing or repeated space. `text` is UTF-8.
//
// Writes up to `capacity` bytes to `out`, and sets `*size` to the size of the whole file. `out` may be null if
// `capacity` is zero. Returns InvalidData, and sets `*size` to zero, if `png` is not a PNG file whose chunks are whole
// up to its image data.
PrStatus prImagePngSetText(PrByteView png, PrStringView keyword, PrStringView text, uint8_t* PR_NULLABLE out,
                           size_t capacity, size_t* size);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
