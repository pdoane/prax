// MIME types, as fetched resources declare them.

#include "dom/src/dom_mime.h"

#include "pr/ascii.h"
#include "pr/charset.h"
#include "pr/string_ops.h"
#include "pr/text_buffer.h"

namespace pr_impl {

namespace {

// HTTP whitespace (fetch/#http-whitespace).
constexpr bool isHttpSpace(char c) {
    return c == '\t' || c == '\n' || c == '\r' || c == ' ';
}

bool isTokens(PrStringView text) {
    if (text.size == 0)
        return false;
    for (size_t i = 0; i < text.size; ++i) {
        if (!asciiIsHttpToken(text.data[i]))
            return false;
    }
    return true;
}

// Appends the HTTP quoted string at `*at`, which starts with its opening quote, to `out` without its quotes and
// escapes, and moves `*at` past it (fetch/#collect-an-http-quoted-string).
void collectQuoted(PrStringView text, size_t* at, TextBuffer* out) {
    ++*at;
    while (*at < text.size) {
        char c = text.data[(*at)++];
        if (c == '"')
            return;
        if (c == '\\') {
            if (*at == text.size) {
                textAppendChar(out, '\\');
                return;
            }
            c = text.data[(*at)++];
        }
        textAppendChar(out, c);
    }
}

} // namespace

PrStringView domMimeTypeEssence(PrStringView type) {
    return stringTrim(stringSlice(type, 0, stringFind(type, ';')));
}

PrCharset domMimeTypeCharset(PrStringView type) {
    PrStringView text = stringTrim(type, isHttpSpace);
    size_t slash = stringFind(text, '/');
    size_t semicolon = stringFind(text, ';');
    if (slash >= semicolon || !isTokens(stringSlice(text, 0, slash)))
        return PrCharset_None;
    PrStringView subtype = stringTrim(stringSlice(text, slash + 1, semicolon - slash - 1), isHttpSpace);
    if (!isTokens(subtype))
        return PrCharset_None;
    size_t at = semicolon;
    TextBuffer value;
    PrCharset charset = PrCharset_None;
    bool found = false;
    while (at < text.size && !found) {
        ++at;
        while (at < text.size && isHttpSpace(text.data[at]))
            ++at;
        size_t nameStart = at;
        while (at < text.size && text.data[at] != ';' && text.data[at] != '=')
            ++at;
        PrStringView name = stringSlice(text, nameStart, at - nameStart);
        if (at == text.size)
            break;
        if (text.data[at] == ';')
            continue;
        ++at;
        if (at == text.size)
            break;
        textClear(&value);
        if (text.data[at] == '"') {
            collectQuoted(text, &at, &value);
            while (at < text.size && text.data[at] != ';')
                ++at;
        } else {
            size_t valueStart = at;
            while (at < text.size && text.data[at] != ';')
                ++at;
            textAppend(&value, stringTrim(stringSlice(text, valueStart, at - valueStart), isHttpSpace));
            if (value.text.count == 0)
                continue;
        }
        // The first charset parameter is the type's, and a later one is ignored.
        if (isTokens(name) && asciiEqualsFold(name, "charset")) {
            charset = prCharsetFromLabel(textView(&value));
            found = true;
        }
    }
    textReset(&value);
    return charset;
}

} // namespace pr_impl
