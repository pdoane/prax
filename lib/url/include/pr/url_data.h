// data: URLs.
//
// fetch - https://fetch.spec.whatwg.org/#data-urls

#pragma once

#include "pr/base.h"
#include "pr/status.h"
#include "pr/string.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Decodes the data: URL `url`, without its fragment (fetch/#data-url-processor): percent-decodes the bytes after the
// comma, and base64-decodes them as well if the MIME type ends in ";base64". Writes the bytes to `body` and the MIME
// type to `mimeType`, which is the part before the comma, or text/plain if that is empty. The caller releases both with
// prStringReset. Returns NotFound, which a fetch reports as a network error, if `url` is not a data: URL, has no comma,
// or holds base64 that does not decode.
PrStatus prUrlProcessData(PrStringView url, PrString* body, PrString* mimeType);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
