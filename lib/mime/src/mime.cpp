// MIME types (mimesniff): the essence of a type, its parameters, and the groups that its essence places it in.

#include "pr/mime.h"

#include "pr/ascii.h"
#include "pr/string_ops.h"
#include "pr/string_trim.h"
#include "pr/text_buffer.h"

namespace pr_impl {

namespace {

// mimesniff/#javascript-mime-type
constexpr PrStringView kJavaScriptMimeTypes[] = {
    "application/ecmascript", "application/javascript",
    "application/x-ecmascript", "application/x-javascript",
    "text/ecmascript", "text/javascript", "text/javascript1.0",
    "text/javascript1.1", "text/javascript1.2", "text/javascript1.3",
    "text/javascript1.4", "text/javascript1.5", "text/jscript",
    "text/livescript", "text/x-ecmascript", "text/x-javascript",
};

bool isTokens(PrStringView text) {
    if (text.size == 0)
        return false;
    for (size_t i = 0; i < text.size; ++i) {
        if (!prAsciiIsHttpToken(text.data[i]))
            return false;
    }
    return true;
}

// Appends the HTTP quoted string at `*at`, which starts with its opening quote, to `out` without its quotes and
// escapes, and moves `*at` past it (fetch/#collect-an-http-quoted-string).
void collectQuoted(PrStringView text, size_t* at, PrTextBuffer* out) {
    ++*at;
    while (*at < text.size) {
        char c = text.data[(*at)++];
        if (c == '"')
            return;
        if (c == '\\') {
            if (*at == text.size) {
                prTextBufferAppendChar(out, '\\');
                return;
            }
            c = text.data[(*at)++];
        }
        prTextBufferAppendChar(out, c);
    }
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrStringView prMimeTypeEssence(PrStringView type) {
    return stringTrimBy(prStringSlice(type, 0, prStringFindByte(type, ';')), prAsciiIsHttpSpace);
}

bool prMimeTypeParameter(PrStringView type, PrStringView name, PrString* out) {
    *out = prStringNull();
    PrStringView text = stringTrimBy(type, prAsciiIsHttpSpace);
    size_t slash = prStringFindByte(text, '/');
    size_t semicolon = prStringFindByte(text, ';');
    if (slash >= semicolon || !isTokens(prStringSlice(text, 0, slash)))
        return false;
    if (!isTokens(stringTrimBy(prStringSlice(text, slash + 1, semicolon - slash - 1), prAsciiIsHttpSpace)))
        return false;
    size_t at = semicolon;
    PrTextBuffer value = {};
    while (at < text.size) {
        ++at;
        while (at < text.size && prAsciiIsHttpSpace(text.data[at]))
            ++at;
        size_t nameStart = at;
        while (at < text.size && text.data[at] != ';' && text.data[at] != '=')
            ++at;
        PrStringView parameter = prStringSlice(text, nameStart, at - nameStart);
        if (at == text.size)
            break;
        if (text.data[at] == ';')
            continue;
        ++at;
        if (at == text.size)
            break;
        prTextBufferClear(&value);
        if (text.data[at] == '"') {
            collectQuoted(text, &at, &value);
            while (at < text.size && text.data[at] != ';')
                ++at;
        } else {
            size_t valueStart = at;
            while (at < text.size && text.data[at] != ';')
                ++at;
            PrStringView written = prStringSlice(text, valueStart, at - valueStart);
            prTextBufferAppend(&value, stringTrimBy(written, prAsciiIsHttpSpace));
            if (value.size == 0)
                continue;
        }
        // The first parameter of a name is the type's, and a later one is ignored.
        if (isTokens(parameter) && prAsciiEqualsFold(parameter, name)) {
            *out = prTextBufferTake(&value);
            return true;
        }
    }
    prTextBufferReset(&value);
    return false;
}

bool prMimeTypeIsJavaScript(PrStringView essence) {
    for (PrStringView type : kJavaScriptMimeTypes) {
        if (prAsciiEqualsFold(type, essence))
            return true;
    }
    return false;
}

bool prMimeTypeIsXml(PrStringView type) {
    PrStringView essence = prMimeTypeEssence(type);
    return prAsciiEqualsFold(essence, "text/xml") || prAsciiEqualsFold(essence, "application/xml") ||
        prAsciiEndsWithFold(essence, "+xml");
}

PrStringView prMimeTypeMinimize(PrStringView essence) {
    if (prMimeTypeIsJavaScript(essence))
        return "text/javascript";
    if (essence == "application/json" || essence == "text/json" || prAsciiEndsWithFold(essence, "+json"))
        return "application/json";
    if (essence == "image/svg+xml")
        return essence;
    if (prMimeTypeIsXml(essence))
        return "application/xml";
    return essence;
}
