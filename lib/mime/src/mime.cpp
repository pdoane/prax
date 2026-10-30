// MIME types (mimesniff): the essence of a type, its parameters, and the groups that its essence places it in.

#include "pr/mime.h"

#include "pr/ascii.h"
#include "pr/string_ops.h"
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

} // namespace pr_impl

using namespace pr_impl;

PrStringView prMimeTypeEssence(PrStringView type) {
    return stringTrim(stringSlice(type, 0, stringFind(type, ';')), asciiIsHttpSpace);
}

bool prMimeTypeParameter(PrStringView type, PrStringView name, PrString* out) {
    *out = prStringNull();
    PrStringView text = stringTrim(type, asciiIsHttpSpace);
    size_t slash = stringFind(text, '/');
    size_t semicolon = stringFind(text, ';');
    if (slash >= semicolon || !isTokens(stringSlice(text, 0, slash)))
        return false;
    if (!isTokens(stringTrim(stringSlice(text, slash + 1, semicolon - slash - 1), asciiIsHttpSpace)))
        return false;
    size_t at = semicolon;
    TextBuffer value;
    while (at < text.size) {
        ++at;
        while (at < text.size && asciiIsHttpSpace(text.data[at]))
            ++at;
        size_t nameStart = at;
        while (at < text.size && text.data[at] != ';' && text.data[at] != '=')
            ++at;
        PrStringView parameter = stringSlice(text, nameStart, at - nameStart);
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
            textAppend(&value, stringTrim(stringSlice(text, valueStart, at - valueStart), asciiIsHttpSpace));
            if (value.text.count == 0)
                continue;
        }
        // The first parameter of a name is the type's, and a later one is ignored.
        if (isTokens(parameter) && asciiEqualsFold(parameter, name)) {
            *out = textTake(&value);
            return true;
        }
    }
    textReset(&value);
    return false;
}

bool prMimeTypeIsJavaScript(PrStringView essence) {
    for (PrStringView type : kJavaScriptMimeTypes) {
        if (asciiEqualsFold(type, essence))
            return true;
    }
    return false;
}

bool prMimeTypeIsXml(PrStringView type) {
    PrStringView essence = prMimeTypeEssence(type);
    return asciiEqualsFold(essence, "text/xml") || asciiEqualsFold(essence, "application/xml") ||
        asciiEndsWithFold(essence, "+xml");
}
