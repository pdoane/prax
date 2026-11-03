// data: URLs.
//
// fetch - https://fetch.spec.whatwg.org/#data-urls

#include "pr/url_data.h"

#include "pr/ascii.h"
#include "pr/base64.h"
#include "pr/string_ops.h"
#include "pr/text_buffer.h"
#include "pr/url_percent.h"

using namespace pr_impl;

PrStatus prUrlProcessData(PrStringView url, PrString* body, PrString* mimeType) {
    *body = PrString{};
    *mimeType = PrString{};
    if (!prAsciiStartsWithFold(url, "data:"))
        return PrStatus_NotFound;
    PrStringView input = prStringSlice(url, 5, url.size - 5);
    size_t hash = prStringFindByte(input, '#');
    if (hash < input.size)
        input.size = hash;
    size_t comma = prStringFindByte(input, ',');
    if (comma >= input.size)
        return PrStatus_NotFound;
    PrStringView type = prStringTrim(PrStringView{input.data, comma});
    PrStringView encoded = prStringSlice(input, comma + 1, input.size - comma - 1);
    bool base64 = false;
    size_t semicolon = prStringFindLastByte(type, ';');
    if (semicolon < type.size)
        base64 = prAsciiEqualsFold(prStringTrim(prStringSlice(type, semicolon + 1, type.size - semicolon - 1)), "base64");
    PrStringView mime = base64 ? prStringTrim(PrStringView{type.data, semicolon}) : type;
    // Both decodings shrink the bytes, so each runs in place in one buffer.
    PrTextBuffer decoded = {};
    prTextBufferReserve(&decoded, uint32_t(encoded.size));
    size_t size = prUrlPercentDecode(encoded, decoded.data, encoded.size);
    if (base64 && !prBase64Decode(PrStringView{decoded.data, size}, decoded.data, size, &size)) {
        prTextBufferReset(&decoded);
        return PrStatus_NotFound;
    }
    *mimeType = prStringCopy(mime.size != 0 ? mime : "text/plain;charset=US-ASCII");
    *body = prStringCopy(PrStringView{decoded.data != nullptr ? decoded.data : "", size});
    prTextBufferReset(&decoded);
    return PrStatus_Ok;
}
