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
    if (!asciiStartsWithFold(url, "data:"))
        return PrStatus_NotFound;
    PrStringView input = stringSlice(url, 5, url.size - 5);
    size_t hash = stringFind(input, '#');
    if (hash < input.size)
        input.size = hash;
    size_t comma = stringFind(input, ',');
    if (comma >= input.size)
        return PrStatus_NotFound;
    PrStringView type = stringTrim(PrStringView{input.data, comma});
    PrStringView encoded = stringSlice(input, comma + 1, input.size - comma - 1);
    bool base64 = false;
    size_t semicolon = stringFindLast(type, ';');
    if (semicolon < type.size)
        base64 = asciiEqualsFold(stringTrim(stringSlice(type, semicolon + 1, type.size - semicolon - 1)), "base64");
    PrStringView mime = base64 ? stringTrim(PrStringView{type.data, semicolon}) : type;
    // Both decodings shrink the bytes, so each runs in place in one buffer.
    TextBuffer decoded;
    decoded.text.reserve(uint32_t(encoded.size));
    size_t size = prUrlPercentDecode(encoded, decoded.text.data, encoded.size);
    if (base64 && !base64Decode(PrStringView{decoded.text.data, size}, decoded.text.data, size, &size)) {
        textReset(&decoded);
        return PrStatus_NotFound;
    }
    *mimeType = prStringCopy(mime.size != 0 ? mime : "text/plain;charset=US-ASCII");
    *body = prStringCopy(PrStringView{decoded.text.data != nullptr ? decoded.text.data : "", size});
    textReset(&decoded);
    return PrStatus_Ok;
}
