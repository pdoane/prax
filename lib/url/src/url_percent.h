// Percent encoding and decoding of bytes.
//
// url - https://url.spec.whatwg.org/

#pragma once

#include "pr/charset.h"
#include "pr/string_view.h"
#include "pr/text_buffer.h"
#include "pr/url_percent.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Appends `text` to `out`, with each byte in `set` percent-encoded.
inline void urlAppendPercentEncoded(TextBuffer* out, PrStringView text, PrUrlPercentEncodeSet set) {
    size_t size = prUrlPercentEncode(text, set, nullptr, 0);
    out->text.reserve(out->text.count + uint32_t(size));
    prUrlPercentEncode(text, set, out->text.data + out->text.count, size);
    out->text.count += uint32_t(size);
}

// Appends `text` to `out` encoded in `charset`, with each byte of the encoding in `set` percent-encoded
// (url/#string-percent-encode-after-encoding). A code point that the charset cannot encode is written as a
// percent-encoded numeric character reference. `charset` is its own output charset.
void urlAppendPercentEncodedIn(TextBuffer* out, PrStringView text, PrCharset charset, PrUrlPercentEncodeSet set);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
