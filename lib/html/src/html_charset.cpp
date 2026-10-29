// Character encodings of HTML documents (html/#determining-the-character-encoding).

#include "html/src/html_charset.h"

#include "pr/ascii.h"
#include "pr/string_ops.h"
#include "pr/text_buffer.h"

namespace pr_impl {

namespace {

// The bytes that the prescan reads, and its position in them. A read past the end aborts the prescan.
struct Prescan {
    PrStringView bytes;
    size_t at;
};

bool atEnd(const Prescan* p) {
    return p->at >= p->bytes.size;
}

char byteAt(const Prescan* p) {
    return p->bytes.data[p->at];
}

bool startsWith(const Prescan* p, PrStringView prefix) {
    return stringStartsWith(stringSlice(p->bytes, p->at, p->bytes.size - p->at), prefix);
}

// An attribute that the prescan read, its name and value in ASCII lowercase.
struct PrescanAttribute {
    TextBuffer name;
    TextBuffer value;
};

enum {
    GetAttribute_Found,
    GetAttribute_None,      // the tag ends without another attribute
    GetAttribute_OutOfBytes,
};
typedef uint8_t GetAttribute;

// Reads the next attribute of a tag (html/#concept-get-attributes-when-sniffing).
GetAttribute getAttribute(Prescan* p, PrescanAttribute* attribute) {
    textClear(&attribute->name);
    textClear(&attribute->value);
    while (!atEnd(p) && (asciiIsSpace(byteAt(p)) || byteAt(p) == '/'))
        ++p->at;
    if (atEnd(p))
        return GetAttribute_OutOfBytes;
    if (byteAt(p) == '>')
        return GetAttribute_None;
    for (;;) {
        if (atEnd(p))
            return GetAttribute_OutOfBytes;
        char byte = byteAt(p);
        if (byte == '=' && attribute->name.text.count != 0) {
            ++p->at;
            break;
        }
        if (asciiIsSpace(byte)) {
            while (!atEnd(p) && asciiIsSpace(byteAt(p)))
                ++p->at;
            if (atEnd(p))
                return GetAttribute_OutOfBytes;
            if (byteAt(p) != '=')
                return GetAttribute_Found;
            ++p->at;
            break;
        }
        if (byte == '/' || byte == '>')
            return GetAttribute_Found;
        textAppendChar(&attribute->name, asciiLower(byte));
        ++p->at;
    }
    while (!atEnd(p) && asciiIsSpace(byteAt(p)))
        ++p->at;
    if (atEnd(p))
        return GetAttribute_OutOfBytes;
    char quote = byteAt(p);
    if (quote == '"' || quote == '\'') {
        for (++p->at;; ++p->at) {
            if (atEnd(p))
                return GetAttribute_OutOfBytes;
            if (byteAt(p) == quote) {
                ++p->at;
                return GetAttribute_Found;
            }
            textAppendChar(&attribute->value, asciiLower(byteAt(p)));
        }
    }
    if (quote == '>')
        return GetAttribute_Found;
    for (;; ++p->at) {
        if (atEnd(p))
            return GetAttribute_OutOfBytes;
        char byte = byteAt(p);
        if (asciiIsSpace(byte) || byte == '>')
            return GetAttribute_Found;
        textAppendChar(&attribute->value, asciiLower(byte));
    }
}

// Returns the charset that an XML declaration at the start of the bytes names, or PrCharset_None
// (html/#concept-get-xml-encoding-when-sniffing).
PrCharset xmlEncoding(PrStringView bytes) {
    if (!stringStartsWith(bytes, "<?xml"))
        return PrCharset_None;
    size_t end = stringFind(bytes, '>');
    if (end == bytes.size)
        return PrCharset_None;
    PrStringView declaration = stringSlice(bytes, 0, end);
    size_t at = stringFind(declaration, "encoding");
    if (at == declaration.size)
        return PrCharset_None;
    at += 8;
    while (at < bytes.size && uint8_t(bytes.data[at]) <= 0x20)
        ++at;
    if (at >= bytes.size || bytes.data[at] != '=')
        return PrCharset_None;
    ++at;
    while (at < bytes.size && uint8_t(bytes.data[at]) <= 0x20)
        ++at;
    if (at >= bytes.size || (bytes.data[at] != '"' && bytes.data[at] != '\''))
        return PrCharset_None;
    char quote = bytes.data[at++];
    size_t close = at;
    while (close < bytes.size && bytes.data[close] != quote)
        ++close;
    if (close == bytes.size)
        return PrCharset_None;
    PrStringView potential = stringSlice(bytes, at, close - at);
    for (size_t i = 0; i < potential.size; ++i) {
        if (uint8_t(potential.data[i]) <= 0x20)
            return PrCharset_None;
    }
    PrCharset charset = prCharsetFromLabel(potential);
    return charset == PrCharset_Utf16Be || charset == PrCharset_Utf16Le ? PrCharset_Utf8 : charset;
}

// Reads a meta element's attributes, from just past its name, and returns the charset that they declare, or
// PrCharset_None. Sets `*outOfBytes` if the bytes end within the element.
PrCharset metaCharset(Prescan* p, bool* outOfBytes) {
    // The attributes already read, which are http-equiv, content and charset.
    bool seenHttpEquiv = false;
    bool seenContent = false;
    bool seenCharset = false;
    bool gotPragma = false;
    // Whether the declaration needs a pragma, once an attribute has decided it.
    bool decided = false;
    bool needPragma = false;
    PrCharset charset = PrCharset_None;
    bool charsetSet = false;
    PrescanAttribute attribute;
    for (;;) {
        GetAttribute got = getAttribute(p, &attribute);
        if (got == GetAttribute_OutOfBytes) {
            *outOfBytes = true;
            textReset(&attribute.name);
            textReset(&attribute.value);
            return PrCharset_None;
        }
        if (got == GetAttribute_None)
            break;
        PrStringView name = textView(&attribute.name);
        PrStringView value = textView(&attribute.value);
        if (name == "http-equiv") {
            if (seenHttpEquiv)
                continue;
            seenHttpEquiv = true;
            if (value == "content-type")
                gotPragma = true;
        } else if (name == "content") {
            if (seenContent)
                continue;
            seenContent = true;
            PrCharset extracted = htmlCharsetFromMetaContent(value);
            if (extracted != PrCharset_None && !charsetSet) {
                charset = extracted;
                charsetSet = true;
                decided = true;
                needPragma = true;
            }
        } else if (name == "charset") {
            if (seenCharset)
                continue;
            seenCharset = true;
            charset = prCharsetFromLabel(value);
            charsetSet = true;
            decided = true;
            needPragma = false;
        }
    }
    textReset(&attribute.name);
    textReset(&attribute.value);
    if (!decided || (needPragma && !gotPragma) || charset == PrCharset_None)
        return PrCharset_None;
    return htmlDeclaredCharset(charset);
}

// The elements whose content the tokenizer reads as text, which holds no tags (html/#parsing-html-fragments). The
// prescan assumes that scripting is enabled, as a document's parse usually has it, so noscript is among them.
constexpr PrStringView kTextElements[] = {"title", "textarea", "style", "xmp", "iframe", "noembed", "noframes",
                                          "noscript", "script"};

// Skips the text of the element that the start tag `name` opens, if its content is text, to its end tag, and leaves the
// position at the `<` of the end tag. A plaintext element's text runs to the end. Returns false if the bytes end first.
bool skipText(Prescan* p, PrStringView name) {
    if (asciiEqualsFold(name, "plaintext")) {
        p->at = p->bytes.size;
        return false;
    }
    bool text = false;
    for (PrStringView element : kTextElements)
        text = text || asciiEqualsFold(name, element);
    if (!text)
        return true;
    for (size_t at = p->at + 1; at + 2 + name.size < p->bytes.size; ++at) {
        if (p->bytes.data[at] != '<' || p->bytes.data[at + 1] != '/'
            || !asciiEqualsFold(stringSlice(p->bytes, at + 2, name.size), name))
            continue;
        char after = p->bytes.data[at + 2 + name.size];
        if (asciiIsSpace(after) || after == '/' || after == '>') {
            p->at = at - 1;
            return true;
        }
    }
    p->at = p->bytes.size;
    return false;
}

// Advances past the first `>` from the position.
void skipPast(Prescan* p, char byte) {
    while (!atEnd(p) && byteAt(p) != byte)
        ++p->at;
}

} // namespace

PrCharset htmlDeclaredCharset(PrCharset charset) {
    if (charset == PrCharset_Utf16Be || charset == PrCharset_Utf16Le)
        return PrCharset_Utf8;
    if (charset == PrCharset_XUserDefined)
        return PrCharset_Windows1252;
    return charset;
}

PrCharset htmlCharsetFromMetaContent(PrStringView content) {
    size_t position = 0;
    for (;;) {
        size_t found = position;
        while (found + 7 <= content.size && !asciiEqualsFold(stringSlice(content, found, 7), "charset"))
            ++found;
        if (found + 7 > content.size)
            return PrCharset_None;
        size_t at = found + 7;
        while (at < content.size && asciiIsSpace(content.data[at]))
            ++at;
        if (at >= content.size || content.data[at] != '=') {
            position = at;
            continue;
        }
        ++at;
        while (at < content.size && asciiIsSpace(content.data[at]))
            ++at;
        if (at >= content.size)
            return PrCharset_None;
        char quote = content.data[at];
        if (quote == '"' || quote == '\'') {
            size_t close = at + 1;
            while (close < content.size && content.data[close] != quote)
                ++close;
            if (close >= content.size)
                return PrCharset_None;
            return prCharsetFromLabel(stringSlice(content, at + 1, close - at - 1));
        }
        size_t end = at;
        while (end < content.size && !asciiIsSpace(content.data[end]) && content.data[end] != ';')
            ++end;
        return prCharsetFromLabel(stringSlice(content, at, end - at));
    }
}

bool htmlPrescan(PrStringView bytes, bool complete, PrCharset* out) {
    *out = PrCharset_None;
    PrStringView scanned = stringSlice(bytes, 0, bytes.size < kHtmlPrescanBytes ? bytes.size : kHtmlPrescanBytes);
    Prescan p = {scanned, 0};
    // A UTF-16 XML declaration.
    if (startsWith(&p, PrStringView{"<\0?\0x\0", 6})) {
        *out = PrCharset_Utf16Le;
        return true;
    }
    if (startsWith(&p, PrStringView{"\0<\0?\0x", 6})) {
        *out = PrCharset_Utf16Be;
        return true;
    }
    PrescanAttribute attribute;
    PrCharset result = PrCharset_None;
    bool outOfBytes = false;
    while (!atEnd(&p) && !outOfBytes) {
        if (startsWith(&p, "<!--")) {
            // The comment ends at the first `-->` whose dashes may be those of `<!--`.
            p.at += 2;
            while (!atEnd(&p) && !startsWith(&p, "-->"))
                ++p.at;
            if (atEnd(&p))
                break;
            p.at += 2;
        } else if (scanned.size - p.at >= 6 && asciiEqualsFold(stringSlice(scanned, p.at, 5), "<meta")
                   && (asciiIsSpace(scanned.data[p.at + 5]) || scanned.data[p.at + 5] == '/')) {
            p.at += 5;
            PrCharset charset = metaCharset(&p, &outOfBytes);
            if (charset != PrCharset_None) {
                result = charset;
                break;
            }
        } else if (byteAt(&p) == '<' && p.at + 1 < scanned.size
                   && (asciiIsLetter(scanned.data[p.at + 1])
                       || (scanned.data[p.at + 1] == '/' && p.at + 2 < scanned.size
                           && asciiIsLetter(scanned.data[p.at + 2])))) {
            bool start = scanned.data[p.at + 1] != '/';
            size_t nameStart = p.at + (start ? 1 : 2);
            while (!atEnd(&p) && !asciiIsSpace(byteAt(&p)) && byteAt(&p) != '>')
                ++p.at;
            size_t nameEnd = nameStart;
            while (nameEnd < p.at && scanned.data[nameEnd] != '/')
                ++nameEnd;
            for (;;) {
                GetAttribute got = getAttribute(&p, &attribute);
                if (got == GetAttribute_OutOfBytes)
                    outOfBytes = true;
                if (got != GetAttribute_Found)
                    break;
            }
            if (outOfBytes)
                break;
            if (start && !skipText(&p, stringSlice(scanned, nameStart, nameEnd - nameStart))) {
                outOfBytes = true;
                break;
            }
        } else if (startsWith(&p, "<!") || startsWith(&p, "</") || startsWith(&p, "<?")) {
            skipPast(&p, '>');
            if (atEnd(&p))
                break;
        }
        ++p.at;
    }
    textReset(&attribute.name);
    textReset(&attribute.value);
    if (result != PrCharset_None) {
        *out = result;
        return true;
    }
    // The prescan ran out of bytes without finding a declaration. More of the first kilobyte may hold one.
    if (!complete && scanned.size < kHtmlPrescanBytes)
        return false;
    *out = xmlEncoding(scanned);
    return true;
}

} // namespace pr_impl
