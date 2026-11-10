// URL records, parsing and serialization.
//
// url - https://url.spec.whatwg.org/

#include "url/src/url_record.h"

#include "pr/alloc.h"
#include "pr/ascii.h"
#include "pr/integer.h"
#include "pr/string_ops.h"
#include "pr/string_trim.h"
#include "pr/text_buffer.h"
#include "pr/unicode_idna.h"
#include "pr/utf8.h"

#include "url/src/url_percent.h"
#include "url/src/url_scheme.h"

namespace pr_impl {
namespace {

constexpr int kEof = -1;

// An IPv4 number of 2^32. Every range that the IPv4 parser checks rejects it.
constexpr uint64_t kIpv4Overflow = UINT64_C(1) << 32;

struct Parser {
    PrStringView input;
    size_t pointer;
    const Url* PR_NULLABLE base;
    Url* url;
    PrTextBuffer path;            // url->path while the parse runs
    PrTextBuffer buffer;
    UrlState state;
    UrlScheme scheme;           // the special scheme of url->scheme, or UrlScheme_None
    bool hasOverride;
    UrlState override;
    bool atSignSeen;
    bool insideBrackets;
    bool passwordTokenSeen;
    PrCharset charset;          // the encoding of the query (url/#concept-basic-url-parser)
};

bool isDigit(int c) {
    return c != kEof && prAsciiIsDigit(char(c));
}

bool isHexDigit(int c) {
    return c != kEof && prAsciiIsHexDigit(char(c));
}

void assign(PrRawString* target, const PrRawString* value) {
    prRawStringReplace(target, prRawStringView(value));
}

void appendEncoded(PrRawString* target, PrStringView text, PrUrlPercentEncodeSet set) {
    PrTextBuffer encoded = {};
    urlAppendPercentEncoded(&encoded, text, set);
    prRawStringAppend(target, prTextBufferView(&encoded));
    prTextBufferReset(&encoded);
}

bool includesCredentials(const Url* url) {
    return !prRawStringIsEmpty(&url->username) || !prRawStringIsEmpty(&url->password);
}

bool isEmptyHost(const PrRawString* host) {
    return !prRawStringIsNull(host) && prRawStringIsEmpty(host);
}

// url/#windows-drive-letter
bool isDriveLetter(PrStringView text) {
    return text.size == 2 && prAsciiIsLetter(text.data[0]) && (text.data[1] == ':' || text.data[1] == '|');
}

bool isNormalizedDriveLetter(PrStringView text) {
    return isDriveLetter(text) && text.data[1] == ':';
}

// url/#start-with-a-windows-drive-letter
bool startsWithDriveLetter(PrStringView text) {
    if (text.size < 2 || !isDriveLetter(prStringSlice(text, 0, 2)))
        return false;
    if (text.size == 2)
        return true;
    char c = text.data[2];
    return c == '/' || c == '\\' || c == '?' || c == '#';
}

// url/#single-dot-path-segment
bool isSingleDot(PrStringView segment) {
    return segment == "." || prAsciiEqualsFold(segment, "%2e");
}

// url/#double-dot-path-segment
bool isDoubleDot(PrStringView segment) {
    return segment == ".." || prAsciiEqualsFold(segment, ".%2e") ||
        prAsciiEqualsFold(segment, "%2e.") || prAsciiEqualsFold(segment, "%2e%2e");
}

// url/#forbidden-host-code-point
bool isForbiddenHost(char c) {
    return c == '\0' || c == '\t' || c == '\n' || c == '\r' || c == ' ' || c == '#' || c == '/' ||
        c == ':' || c == '<' || c == '>' || c == '?' || c == '@' || c == '[' || c == '\\' || c == ']' || c == '^' || c == '|';
}

// url/#forbidden-domain-code-point
bool isForbiddenDomain(char c) {
    return isForbiddenHost(c) || uint8_t(c) <= 0x1f || c == '%' || c == 0x7f;
}

// url/#ipv4-number-parser, saturating at kIpv4Overflow.
bool parseIpv4Number(PrStringView text, uint64_t* out) {
    if (text.size == 0)
        return false;
    uint32_t radix = 10;
    if (text.size >= 2 && text.data[0] == '0' && prAsciiLower(text.data[1]) == 'x') {
        radix = 16;
        text = prStringFrom(text, 2);
    } else if (text.size >= 2 && text.data[0] == '0') {
        radix = 8;
        text = prStringFrom(text, 1);
    }
    // "0x" alone is zero.
    uint64_t value = 0;
    if (text.size != 0 && !prU64Parse(text.data, text.size, &value, radix))
        return false;
    *out = value < kIpv4Overflow ? value : kIpv4Overflow;
    return true;
}

// url/#ends-in-a-number-checker
bool endsInNumber(PrStringView text) {
    if (text.size != 0 && text.data[text.size - 1] == '.')
        --text.size;
    size_t first = text.size;
    while (first != 0 && text.data[first - 1] != '.')
        --first;
    PrStringView last = prStringFrom(text, first);
    bool digits = last.size != 0;
    for (size_t i = 0; i < last.size; ++i)
        digits = digits && prAsciiIsDigit(last.data[i]);
    uint64_t number;
    return digits || parseIpv4Number(last, &number);
}

// url/#concept-ipv4-parser
PrStatus parseIpv4(PrStringView text, uint32_t* out) {
    if (text.size != 0 && text.data[text.size - 1] == '.')
        --text.size;
    uint64_t numbers[4];
    uint32_t count = 0;
    for (size_t first = 0;;) {
        size_t end = first + prStringFindByte(prStringFrom(text, first), '.');
        if (count == 4 || !parseIpv4Number(prStringSlice(text, first, end - first), &numbers[count]))
            return PrStatus_Syntax;
        ++count;
        if (end == text.size)
            break;
        first = end + 1;
    }
    uint64_t address = numbers[count - 1];
    if (address >= UINT64_C(1) << (8 * (5 - count)))
        return PrStatus_Syntax;
    for (uint32_t i = 0; i + 1 < count; ++i) {
        if (numbers[i] > 255)
            return PrStatus_Syntax;
        address += numbers[i] << (8 * (3 - i));
    }
    *out = uint32_t(address);
    return PrStatus_Ok;
}

// url/#concept-ipv4-serializer
void serializeIpv4(PrTextBuffer* out, uint32_t address) {
    for (uint32_t i = 0; i < 4; ++i) {
        if (i != 0)
            prTextBufferAppendChar(out, '.');
        prTextBufferAppendU64(out, (address >> (24 - 8 * i)) & 255, 10);
    }
}

// url/#concept-ipv6-parser
PrStatus parseIpv6(PrStringView input, uint16_t address[8]) {
    auto at = [&](size_t i) { return i < input.size ? int(uint8_t(input.data[i])) : kEof; };
    for (uint32_t i = 0; i < 8; ++i)
        address[i] = 0;
    uint32_t pieceIndex = 0;
    int32_t compress = -1;
    size_t pointer = 0;
    if (at(pointer) == ':') {
        if (at(pointer + 1) != ':')
            return PrStatus_Syntax;
        pointer += 2;
        compress = int32_t(++pieceIndex);
    }
    while (at(pointer) != kEof) {
        if (pieceIndex == 8)
            return PrStatus_Syntax;
        if (at(pointer) == ':') {
            if (compress != -1)
                return PrStatus_Syntax;
            ++pointer;
            compress = int32_t(++pieceIndex);
            continue;
        }
        uint32_t value = 0;
        uint32_t length = 0;
        while (length < 4 && isHexDigit(at(pointer))) {
            value = value * 16 + prAsciiHexValue(char(at(pointer)));
            ++pointer;
            ++length;
        }
        if (at(pointer) == '.') {
            if (length == 0 || pieceIndex > 6)
                return PrStatus_Syntax;
            pointer -= length;
            uint32_t numbersSeen = 0;
            while (at(pointer) != kEof) {
                if (numbersSeen > 0) {
                    if (at(pointer) != '.' || numbersSeen == 4)
                        return PrStatus_Syntax;
                    ++pointer;
                }
                if (!isDigit(at(pointer)))
                    return PrStatus_Syntax;
                int32_t piece = -1;
                while (isDigit(at(pointer))) {
                    int32_t number = at(pointer) - '0';
                    if (piece == 0)
                        return PrStatus_Syntax;
                    piece = piece == -1 ? number : piece * 10 + number;
                    if (piece > 255)
                        return PrStatus_Syntax;
                    ++pointer;
                }
                address[pieceIndex] = uint16_t(address[pieceIndex] * 0x100 + piece);
                ++numbersSeen;
                if (numbersSeen == 2 || numbersSeen == 4)
                    ++pieceIndex;
            }
            if (numbersSeen != 4)
                return PrStatus_Syntax;
            break;
        }
        if (at(pointer) == ':') {
            ++pointer;
            if (at(pointer) == kEof)
                return PrStatus_Syntax;
        } else if (at(pointer) != kEof) {
            return PrStatus_Syntax;
        }
        address[pieceIndex++] = uint16_t(value);
    }
    if (compress != -1) {
        uint32_t swaps = pieceIndex - uint32_t(compress);
        for (pieceIndex = 7; pieceIndex != 0 && swaps > 0; --pieceIndex, --swaps) {
            uint16_t piece = address[pieceIndex];
            address[pieceIndex] = address[uint32_t(compress) + swaps - 1];
            address[uint32_t(compress) + swaps - 1] = piece;
        }
    } else if (pieceIndex != 8) {
        return PrStatus_Syntax;
    }
    return PrStatus_Ok;
}

// url/#concept-ipv6-serializer. The address is enclosed in brackets, as the host serializer writes it.
void serializeIpv6(PrTextBuffer* out, const uint16_t address[8]) {
    uint32_t compress = 8;
    uint32_t longest = 1;
    for (uint32_t i = 0; i < 8;) {
        uint32_t start = i;
        while (i < 8 && address[i] == 0)
            ++i;
        if (i - start > longest) {
            compress = start;
            longest = i - start;
        }
        i += i == start;
    }
    prTextBufferAppendChar(out, '[');
    for (uint32_t i = 0; i < 8; ++i) {
        if (i == compress) {
            prTextBufferAppend(out, i == 0 ? "::" : ":");
            i += longest - 1;
            continue;
        }
        prTextBufferAppendU64(out, address[i], 16);
        if (i != 7)
            prTextBufferAppendChar(out, ':');
    }
    prTextBufferAppendChar(out, ']');
}

// url/#concept-domain-to-ascii with beStrict false. An ASCII domain is lowercased, whatever Unicode ToASCII makes of
// it. Any other domain is converted by ToASCII, which rejects an ill-formed sequence as it rejects the U+FFFD that
// decoding would replace it with.
PrStatus domainToAscii(PrTextBuffer* domain) {
    PrStringView text = prTextBufferView(domain);
    bool ascii = true;
    for (size_t i = 0; i < text.size && ascii; ++i)
        ascii = uint8_t(text.data[i]) < 0x80;
    if (ascii) {
        prTextBufferAsciiLower(domain);
    } else {
        PrRawString converted{};
        if (!prUnicodeIdnaToAscii(text, PrUnicodeIdnaFlag_CheckBidi | PrUnicodeIdnaFlag_CheckJoiners, &converted))
            return PrStatus_Syntax;
        prTextBufferClear(domain);
        prTextBufferAppend(domain, prRawStringView(&converted));
        prRawStringReset(&converted);
    }
    text = prTextBufferView(domain);
    if (text.size == 0)
        return PrStatus_Syntax;
    for (size_t i = 0; i < text.size; ++i) {
        if (isForbiddenDomain(text.data[i]))
            return PrStatus_Syntax;
    }
    return PrStatus_Ok;
}

// url/#concept-host-parser. On success, writes the serialized host to `out`.
PrStatus parseHost(PrStringView input, bool opaque, PrRawString* out) {
    PrTextBuffer host = {};
    PrStatus status = PrStatus_Ok;
    if (input.size != 0 && input.data[0] == '[') {
        uint16_t address[8];
        if (input.data[input.size - 1] != ']')
            status = PrStatus_Syntax;
        else
            status = parseIpv6(prStringSlice(input, 1, input.size - 2), address);
        if (status == PrStatus_Ok)
            serializeIpv6(&host, address);
    } else if (opaque) {
        // url/#concept-opaque-host-parser
        for (size_t i = 0; i < input.size && status == PrStatus_Ok; ++i) {
            if (isForbiddenHost(input.data[i]))
                status = PrStatus_Syntax;
        }
        urlAppendPercentEncoded(&host, input, PrUrlPercentEncodeSet_C0Control);
    } else {
        PR_ASSERT(input.size != 0);
        prTextBufferAppend(&host, input);
        host.size = uint32_t(prUrlPercentDecode(input, host.data, host.size));
        status = domainToAscii(&host);
        if (status == PrStatus_Ok && endsInNumber(prTextBufferView(&host))) {
            uint32_t address;
            status = parseIpv4(prTextBufferView(&host), &address);
            prTextBufferClear(&host);
            if (status == PrStatus_Ok)
                serializeIpv4(&host, address);
        }
    }
    if (status == PrStatus_Ok) {
        prRawStringReset(out);
        *out = prTextBufferTake(&host);
    }
    prTextBufferReset(&host);
    return status;
}

int codePoint(const Parser* p) {
    return p->pointer < p->input.size ? int(uint8_t(p->input.data[p->pointer])) : kEof;
}

PrStringView remaining(const Parser* p) {
    size_t next = p->pointer + 1;
    return next < p->input.size ? prStringFrom(p->input, next) : "";
}

bool remainingStartsWith(const Parser* p, char c) {
    PrStringView rest = remaining(p);
    return rest.size != 0 && rest.data[0] == c;
}

// Returns the input from the pointer onward.
PrStringView fromPointer(const Parser* p) {
    return prStringFrom(p->input, p->pointer);
}

bool isSpecial(const Parser* p) {
    return p->scheme != UrlScheme_None;
}

void setScheme(Parser* p, PrStringView scheme) {
    prRawStringReplace(&p->url->scheme, scheme);
    p->scheme = urlScheme(scheme);
}

// Copies the username, password, host and port from the base.
void takeAuthority(Parser* p) {
    assign(&p->url->username, &p->base->username);
    assign(&p->url->password, &p->base->password);
    assign(&p->url->host, &p->base->host);
    p->url->port = p->base->port;
    p->url->hasPort = p->base->hasPort;
}

void takePath(Parser* p) {
    prTextBufferClear(&p->path);
    prTextBufferAppend(&p->path, prRawStringView(&p->base->path));
}

void appendSegment(Parser* p, PrStringView segment) {
    prTextBufferAppendChar(&p->path, '/');
    prTextBufferAppend(&p->path, segment);
}

PrStringView firstSegment(PrStringView path) {
    if (path.size == 0)
        return path;
    PrStringView rest = prStringFrom(path, 1);
    return prStringSlice(rest, 0, prStringFindByte(rest, '/'));
}

// url/#shorten-a-urls-path
void shorten(Parser* p) {
    PrStringView path = prTextBufferView(&p->path);
    if (p->scheme == UrlScheme_File && path.size == 3 && isNormalizedDriveLetter(prStringSlice(path, 1, 2)))
        return;
    uint32_t end = p->path.size;
    while (end != 0 && p->path.data[end - 1] != '/')
        --end;
    p->path.size = end != 0 ? end - 1 : 0;
}

void startQuery(Parser* p) {
    prRawStringReplace(&p->url->query, "");
    p->state = UrlState_Query;
}

void startFragment(Parser* p) {
    prRawStringReplace(&p->url->fragment, "");
    p->state = UrlState_Fragment;
}

// url/#concept-basic-url-parser, from its state machine onward. Decrementing the pointer from zero wraps, and the
// increment that follows brings it back.
PrStatus run(Parser* p) {
    Url* url = p->url;
    for (;; ++p->pointer) {
        int c = codePoint(p);
        bool special = isSpecial(p);
        bool slash = c == '/' || (special && c == '\\');
        switch (p->state) {
            case UrlState_SchemeStart:
                if (c != kEof && prAsciiIsLetter(char(c))) {
                    prTextBufferAppendChar(&p->buffer, prAsciiLower(char(c)));
                    p->state = UrlState_Scheme;
                } else if (!p->hasOverride) {
                    p->state = UrlState_NoScheme;
                    --p->pointer;
                } else {
                    return PrStatus_Syntax;
                }
                break;

            case UrlState_Scheme:
                if (c != kEof && (prAsciiIsLetter(char(c)) || prAsciiIsDigit(char(c)) || c == '+' || c == '-' || c == '.')) {
                    prTextBufferAppendChar(&p->buffer, prAsciiLower(char(c)));
                } else if (c == ':') {
                    PrStringView scheme = prTextBufferView(&p->buffer);
                    if (p->hasOverride) {
                        UrlScheme next = urlScheme(scheme);
                        if ((next != UrlScheme_None) != special)
                            return PrStatus_Ok;
                        if ((includesCredentials(url) || url->hasPort) && next == UrlScheme_File)
                            return PrStatus_Ok;
                        if (p->scheme == UrlScheme_File && isEmptyHost(&url->host))
                            return PrStatus_Ok;
                    }
                    setScheme(p, scheme);
                    if (p->hasOverride) {
                        if (url->hasPort && int32_t(url->port) == kUrlSchemePort[p->scheme])
                            url->hasPort = false;
                        return PrStatus_Ok;
                    }
                    prTextBufferClear(&p->buffer);
                    if (p->scheme == UrlScheme_File) {
                        p->state = UrlState_File;
                    } else if (isSpecial(p) && p->base != nullptr &&
                            prRawStringView(&p->base->scheme) == prRawStringView(&url->scheme)) {
                        p->state = UrlState_SpecialRelativeOrAuthority;
                    } else if (isSpecial(p)) {
                        p->state = UrlState_SpecialAuthoritySlashes;
                    } else if (remainingStartsWith(p, '/')) {
                        p->state = UrlState_PathOrAuthority;
                        ++p->pointer;
                    } else {
                        prTextBufferClear(&p->path);
                        url->opaquePath = true;
                        p->state = UrlState_OpaquePath;
                    }
                } else if (!p->hasOverride) {
                    prTextBufferClear(&p->buffer);
                    p->state = UrlState_NoScheme;
                    p->pointer = size_t(-1);
                } else {
                    return PrStatus_Syntax;
                }
                break;

            case UrlState_NoScheme:
                if (p->base == nullptr || (p->base->opaquePath && c != '#'))
                    return PrStatus_Syntax;
                if (p->base->opaquePath) {
                    setScheme(p, prRawStringView(&p->base->scheme));
                    takePath(p);
                    url->opaquePath = true;
                    assign(&url->query, &p->base->query);
                    startFragment(p);
                } else {
                    bool file = urlScheme(prRawStringView(&p->base->scheme)) == UrlScheme_File;
                    p->state = file ? UrlState_File : UrlState_Relative;
                    --p->pointer;
                }
                break;

            case UrlState_SpecialRelativeOrAuthority:
                if (c == '/' && remainingStartsWith(p, '/')) {
                    p->state = UrlState_SpecialAuthorityIgnoreSlashes;
                    ++p->pointer;
                } else {
                    p->state = UrlState_Relative;
                    --p->pointer;
                }
                break;

            case UrlState_PathOrAuthority:
                if (c == '/') {
                    p->state = UrlState_Authority;
                } else {
                    p->state = UrlState_Path;
                    --p->pointer;
                }
                break;

            case UrlState_Relative:
                setScheme(p, prRawStringView(&p->base->scheme));
                if (c == '/' || (isSpecial(p) && c == '\\')) {
                    p->state = UrlState_RelativeSlash;
                    break;
                }
                takeAuthority(p);
                takePath(p);
                assign(&url->query, &p->base->query);
                if (c == '?') {
                    startQuery(p);
                } else if (c == '#') {
                    startFragment(p);
                } else if (c != kEof) {
                    prRawStringReset(&url->query);
                    shorten(p);
                    p->state = UrlState_Path;
                    --p->pointer;
                }
                break;

            case UrlState_RelativeSlash:
                if (special && (c == '/' || c == '\\')) {
                    p->state = UrlState_SpecialAuthorityIgnoreSlashes;
                } else if (c == '/') {
                    p->state = UrlState_Authority;
                } else {
                    takeAuthority(p);
                    p->state = UrlState_Path;
                    --p->pointer;
                }
                break;

            case UrlState_SpecialAuthoritySlashes:
                p->state = UrlState_SpecialAuthorityIgnoreSlashes;
                if (c == '/' && remainingStartsWith(p, '/'))
                    ++p->pointer;
                else
                    --p->pointer;
                break;

            case UrlState_SpecialAuthorityIgnoreSlashes:
                if (c != '/' && c != '\\') {
                    p->state = UrlState_Authority;
                    --p->pointer;
                }
                break;

            case UrlState_Authority:
                if (c == '@') {
                    if (p->atSignSeen)
                        appendEncoded(p->passwordTokenSeen ? &url->password : &url->username,
                            "%40", PrUrlPercentEncodeSet_Userinfo);
                    p->atSignSeen = true;
                    PrStringView credentials = prTextBufferView(&p->buffer);
                    if (!p->passwordTokenSeen) {
                        size_t colon = prStringFindByte(credentials, ':');
                        appendEncoded(&url->username, prStringSlice(credentials, 0, colon),
                            PrUrlPercentEncodeSet_Userinfo);
                        p->passwordTokenSeen = colon != credentials.size;
                        credentials =
                            p->passwordTokenSeen ? prStringSlice(credentials, colon + 1, credentials.size - colon - 1) : "";
                    }
                    if (p->passwordTokenSeen)
                        appendEncoded(&url->password, credentials, PrUrlPercentEncodeSet_Userinfo);
                    prTextBufferClear(&p->buffer);
                } else if (c == kEof || slash || c == '?' || c == '#') {
                    if (p->atSignSeen && p->buffer.size == 0)
                        return PrStatus_Syntax;
                    p->pointer -= p->buffer.size + 1;
                    prTextBufferClear(&p->buffer);
                    p->state = UrlState_Host;
                } else {
                    prTextBufferAppendChar(&p->buffer, char(c));
                }
                break;

            case UrlState_Host:
            case UrlState_Hostname:
                if (p->hasOverride && p->scheme == UrlScheme_File) {
                    --p->pointer;
                    p->state = UrlState_FileHost;
                } else if (c == ':' && !p->insideBrackets) {
                    if (p->buffer.size == 0)
                        return PrStatus_Syntax;
                    if (p->hasOverride && p->override == UrlState_Hostname)
                        return PrStatus_Syntax;
                    PrStatus status = parseHost(prTextBufferView(&p->buffer), !special, &url->host);
                    if (status != PrStatus_Ok)
                        return status;
                    prTextBufferClear(&p->buffer);
                    p->state = UrlState_Port;
                } else if (c == kEof || slash || c == '?' || c == '#') {
                    --p->pointer;
                    if (special && p->buffer.size == 0)
                        return PrStatus_Syntax;
                    if (p->hasOverride && p->buffer.size == 0 && (includesCredentials(url) || url->hasPort))
                        return PrStatus_Syntax;
                    PrStatus status = parseHost(prTextBufferView(&p->buffer), !special, &url->host);
                    if (status != PrStatus_Ok)
                        return status;
                    prTextBufferClear(&p->buffer);
                    p->state = UrlState_PathStart;
                    if (p->hasOverride)
                        return PrStatus_Ok;
                } else {
                    if (c == '[')
                        p->insideBrackets = true;
                    if (c == ']')
                        p->insideBrackets = false;
                    prTextBufferAppendChar(&p->buffer, char(c));
                }
                break;

            case UrlState_Port:
                if (isDigit(c)) {
                    prTextBufferAppendChar(&p->buffer, char(c));
                } else if (c == kEof || slash || c == '?' || c == '#' || p->hasOverride) {
                    if (p->buffer.size != 0) {
                        uint64_t port = 0;
                        prU64Parse(p->buffer.data, p->buffer.size, &port, 10);
                        if (port > 0xffff)
                            return PrStatus_Syntax;
                        url->hasPort = int32_t(port) != kUrlSchemePort[p->scheme];
                        url->port = url->hasPort ? uint16_t(port) : 0;
                        prTextBufferClear(&p->buffer);
                        if (p->hasOverride)
                            return PrStatus_Ok;
                    }
                    if (p->hasOverride)
                        return PrStatus_Syntax;
                    p->state = UrlState_PathStart;
                    --p->pointer;
                } else {
                    return PrStatus_Syntax;
                }
                break;

            case UrlState_File:
                setScheme(p, "file");
                prRawStringReplace(&url->host, "");
                if (c == '/' || c == '\\') {
                    p->state = UrlState_FileSlash;
                } else if (p->base != nullptr && urlScheme(prRawStringView(&p->base->scheme)) == UrlScheme_File) {
                    assign(&url->host, &p->base->host);
                    takePath(p);
                    assign(&url->query, &p->base->query);
                    if (c == '?') {
                        startQuery(p);
                    } else if (c == '#') {
                        startFragment(p);
                    } else if (c != kEof) {
                        prRawStringReset(&url->query);
                        if (!startsWithDriveLetter(fromPointer(p)))
                            shorten(p);
                        else
                            prTextBufferClear(&p->path);
                        p->state = UrlState_Path;
                        --p->pointer;
                    }
                } else {
                    p->state = UrlState_Path;
                    --p->pointer;
                }
                break;

            case UrlState_FileSlash:
                if (c == '/' || c == '\\') {
                    p->state = UrlState_FileHost;
                    break;
                }
                if (p->base != nullptr && urlScheme(prRawStringView(&p->base->scheme)) == UrlScheme_File) {
                    assign(&url->host, &p->base->host);
                    PrStringView drive = firstSegment(prRawStringView(&p->base->path));
                    if (!startsWithDriveLetter(fromPointer(p)) && isNormalizedDriveLetter(drive))
                        appendSegment(p, drive);
                }
                p->state = UrlState_Path;
                --p->pointer;
                break;

            case UrlState_FileHost:
                if (c == kEof || c == '/' || c == '\\' || c == '?' || c == '#') {
                    --p->pointer;
                    if (!p->hasOverride && isDriveLetter(prTextBufferView(&p->buffer))) {
                        // The buffer carries over into the path state.
                        p->state = UrlState_Path;
                    } else if (p->buffer.size == 0) {
                        prRawStringReplace(&url->host, "");
                        if (p->hasOverride)
                            return PrStatus_Ok;
                        p->state = UrlState_PathStart;
                    } else {
                        PrStatus status = parseHost(prTextBufferView(&p->buffer), !special, &url->host);
                        if (status != PrStatus_Ok)
                            return status;
                        if (prRawStringView(&url->host) == "localhost")
                            prRawStringReplace(&url->host, "");
                        if (p->hasOverride)
                            return PrStatus_Ok;
                        prTextBufferClear(&p->buffer);
                        p->state = UrlState_PathStart;
                    }
                } else {
                    prTextBufferAppendChar(&p->buffer, char(c));
                }
                break;

            case UrlState_PathStart:
                if (special) {
                    p->state = UrlState_Path;
                    if (!slash)
                        --p->pointer;
                } else if (!p->hasOverride && c == '?') {
                    startQuery(p);
                } else if (!p->hasOverride && c == '#') {
                    startFragment(p);
                } else if (c != kEof) {
                    p->state = UrlState_Path;
                    if (c != '/')
                        --p->pointer;
                } else if (p->hasOverride && prRawStringIsNull(&url->host)) {
                    appendSegment(p, "");
                }
                break;

            case UrlState_Path:
                if (c == kEof || slash || (!p->hasOverride && (c == '?' || c == '#'))) {
                    PrStringView segment = prTextBufferView(&p->buffer);
                    if (isDoubleDot(segment)) {
                        shorten(p);
                        if (!slash)
                            appendSegment(p, "");
                    } else if (isSingleDot(segment)) {
                        if (!slash)
                            appendSegment(p, "");
                    } else {
                        if (p->scheme == UrlScheme_File && p->path.size == 0 && isDriveLetter(segment))
                            p->buffer.data[1] = ':';
                        appendSegment(p, segment);
                    }
                    prTextBufferClear(&p->buffer);
                    if (c == '?')
                        startQuery(p);
                    if (c == '#')
                        startFragment(p);
                } else {
                    urlAppendPercentEncoded(&p->buffer, prStringSlice(p->input, p->pointer, 1),
                        PrUrlPercentEncodeSet_Path);
                }
                break;

            case UrlState_OpaquePath:
                if (c == '?') {
                    startQuery(p);
                } else if (c == '#') {
                    startFragment(p);
                } else if (c == ' ') {
                    bool delimited = remainingStartsWith(p, '?') || remainingStartsWith(p, '#');
                    prTextBufferAppend(&p->path, delimited ? "%20" : " ");
                } else if (c != kEof) {
                    urlAppendPercentEncoded(&p->path, prStringSlice(p->input, p->pointer, 1),
                        PrUrlPercentEncodeSet_C0Control);
                }
                break;

            case UrlState_Query:
                if ((!p->hasOverride && c == '#') || c == kEof) {
                    // The query of a URL that is not special, or of a WebSocket URL, is UTF-8 in any charset.
                    bool utf8 = !special || p->scheme == UrlScheme_Ws || p->scheme == UrlScheme_Wss;
                    PrTextBuffer encoded = {};
                    PrCharset queryCharset = utf8 ? PrCharset_Utf8 : p->charset;
                    PrUrlPercentEncodeSet querySet = PrUrlPercentEncodeSet_Query;
                    if (special)
                        querySet = PrUrlPercentEncodeSet_SpecialQuery;
                    urlAppendPercentEncodedIn(&encoded, prTextBufferView(&p->buffer), queryCharset, querySet);
                    prRawStringAppend(&url->query, prTextBufferView(&encoded));
                    prTextBufferReset(&encoded);
                    prTextBufferClear(&p->buffer);
                    if (c == '#')
                        startFragment(p);
                } else {
                    prTextBufferAppendChar(&p->buffer, char(c));
                }
                break;

            case UrlState_Fragment:
                // The buffer holds the encoded text so far, which becomes the fragment at the end.
                if (c != kEof)
                    urlAppendPercentEncoded(&p->buffer, prStringSlice(p->input, p->pointer, 1),
                        PrUrlPercentEncodeSet_Fragment);
                else
                    prRawStringAppend(&url->fragment, prTextBufferView(&p->buffer));
                break;
        }
        if (p->pointer == p->input.size)
            return PrStatus_Ok;
    }
}

bool isTabOrNewline(char c) {
    return c == '\t' || c == '\n' || c == '\r';
}

PrStatus parse(Url* url, PrStringView input, const Url* PR_NULLABLE base, UrlState state, bool hasOverride, PrCharset charset) {
    size_t first = 0;
    while (first < input.size && !isTabOrNewline(input.data[first]))
        ++first;
    PrTextBuffer cleaned = {};
    if (first != input.size) {
        for (size_t i = 0; i < input.size; ++i) {
            if (!isTabOrNewline(input.data[i]))
                prTextBufferAppendChar(&cleaned, input.data[i]);
        }
        input = prTextBufferView(&cleaned);
    }
    Parser p = {};
    p.input = input;
    p.base = base;
    p.url = url;
    p.state = state;
    p.scheme = urlScheme(prRawStringView(&url->scheme));
    p.hasOverride = hasOverride;
    p.override = state;
    p.charset = charset;
    prTextBufferAppend(&p.path, prRawStringView(&url->path));
    PrStatus status = run(&p);
    prRawStringReset(&url->path);
    url->path = prTextBufferTake(&p.path);
    prTextBufferReset(&p.buffer);
    prTextBufferReset(&cleaned);
    return status;
}

bool isC0ControlOrSpace(char c) {
    return uint8_t(c) <= 0x20;
}

// Decodes a name or value for url/#concept-urlencoded-parser, using `bytes` as scratch.
PrRawString formDecode(PrTextBuffer* bytes, PrStringView text) {
    prTextBufferClear(bytes);
    prTextBufferAppend(bytes, text);
    for (uint32_t i = 0; i < bytes->size; ++i) {
        if (bytes->data[i] == '+')
            bytes->data[i] = ' ';
    }
    bytes->size = uint32_t(prUrlPercentDecode(prTextBufferView(bytes), bytes->data, bytes->size));
    PrTextBuffer decoded = {};
    prTextBufferAppendUtf8(&decoded, prTextBufferView(bytes));
    return prTextBufferTake(&decoded);
}

// Encodes a name or value for url/#concept-urlencoded-serializer.
void formEncode(PrTextBuffer* out, PrStringView text) {
    for (size_t first = 0; first <= text.size;) {
        PrStringView rest = prStringFrom(text, first);
        size_t space = prStringFindByte(rest, ' ');
        urlAppendPercentEncoded(out, prStringSlice(rest, 0, space), PrUrlPercentEncodeSet_Form);
        if (space != rest.size)
            prTextBufferAppendChar(out, '+');
        first += space + 1;
    }
}

} // namespace

PrStatus urlHostParse(PrStringView input, PrRawString* out) {
    *out = prRawStringNull();
    return parseHost(input, false, out);
}

PrStatus urlParse(PrStringView input, const Url* PR_NULLABLE base, PrCharset charset, PrUrl* PR_NULLABLE* PR_NONNULL out) {
    PR_ASSERT(prCharsetOutput(charset) == charset);
    *out = nullptr;
    PrUrl* url = allocZeroed<PrUrl>();
    PrStatus status = parse(url, stringTrimBy(input, isC0ControlOrSpace), base, UrlState_SchemeStart, false, charset);
    if (status == PrStatus_Ok)
        *out = url;
    else
        urlDestroy(url);
    return status;
}

PrStatus urlParseFrom(Url* url, PrStringView input, UrlState state) {
    return parse(url, input, nullptr, state, true, PrCharset_Utf8);
}

PrUrl* urlClone(const Url* url) {
    PrUrl* result = alloc<PrUrl>();
    *result = PrUrl{*url};
    result->scheme = prRawStringClone(&url->scheme);
    result->username = prRawStringClone(&url->username);
    result->password = prRawStringClone(&url->password);
    result->host = prRawStringClone(&url->host);
    result->path = prRawStringClone(&url->path);
    result->query = prRawStringClone(&url->query);
    result->fragment = prRawStringClone(&url->fragment);
    result->searchParams = nullptr;
    return result;
}

void urlDestroy(Url* PR_NULLABLE url) {
    if (url == nullptr)
        return;
    urlSearchParamsDestroy(url->searchParams);
    prRawStringReset(&url->scheme);
    prRawStringReset(&url->username);
    prRawStringReset(&url->password);
    prRawStringReset(&url->host);
    prRawStringReset(&url->path);
    prRawStringReset(&url->query);
    prRawStringReset(&url->fragment);
    prMemoryFree(url);
}

bool urlIsSpecial(const Url* url) {
    return urlScheme(prRawStringView(&url->scheme)) != UrlScheme_None;
}

bool urlCannotHaveCredentials(const Url* url) {
    return prRawStringIsEmpty(&url->host) || urlScheme(prRawStringView(&url->scheme)) == UrlScheme_File;
}

PrRawString urlSerialize(const Url* url, PrUrlSerialization serialization) {
    PrTextBuffer out = {};
    prTextBufferAppend(&out, prRawStringView(&url->scheme));
    prTextBufferAppendChar(&out, ':');
    PrStringView path = prRawStringView(&url->path);
    if (!prRawStringIsNull(&url->host)) {
        prTextBufferAppend(&out, "//");
        if (includesCredentials(url)) {
            prTextBufferAppend(&out, prRawStringView(&url->username));
            if (!prRawStringIsEmpty(&url->password)) {
                prTextBufferAppendChar(&out, ':');
                prTextBufferAppend(&out, prRawStringView(&url->password));
            }
            prTextBufferAppendChar(&out, '@');
        }
        prTextBufferAppend(&out, prRawStringView(&url->host));
        if (url->hasPort) {
            prTextBufferAppendChar(&out, ':');
            prTextBufferAppendU64(&out, url->port, 10);
        }
    } else if (!url->opaquePath && prStringStartsWith(path, "//")) {
        prTextBufferAppend(&out, "/.");
    }
    prTextBufferAppend(&out, path);
    if (!prRawStringIsNull(&url->query)) {
        prTextBufferAppendChar(&out, '?');
        prTextBufferAppend(&out, prRawStringView(&url->query));
    }
    if (serialization != PrUrlSerialization_ExcludeFragment && !prRawStringIsNull(&url->fragment)) {
        prTextBufferAppendChar(&out, '#');
        prTextBufferAppend(&out, prRawStringView(&url->fragment));
    }
    return prTextBufferTake(&out);
}

PrRawString urlOrigin(const Url* url) {
    if (prRawStringView(&url->scheme) == "blob") {
        PrUrl* pathUrl = nullptr;
        urlParse(prRawStringView(&url->path), nullptr, PrCharset_Utf8, &pathUrl);
        UrlScheme inner = pathUrl != nullptr ? urlScheme(prRawStringView(&pathUrl->scheme)) : UrlScheme_None;
        PrRawString origin;
        if (inner == UrlScheme_Http || inner == UrlScheme_Https)
            origin = urlOrigin(pathUrl);
        else
            origin = prRawStringCopy("null");
        urlDestroy(pathUrl);
        return origin;
    }
    UrlScheme scheme = urlScheme(prRawStringView(&url->scheme));
    if (scheme == UrlScheme_None || scheme == UrlScheme_File)
        return prRawStringCopy("null");
    PrTextBuffer out = {};
    prTextBufferAppend(&out, prRawStringView(&url->scheme));
    prTextBufferAppend(&out, "://");
    prTextBufferAppend(&out, prRawStringView(&url->host));
    if (url->hasPort) {
        prTextBufferAppendChar(&out, ':');
        prTextBufferAppendU64(&out, url->port, 10);
    }
    return prTextBufferTake(&out);
}

void urlSearchParamsParse(UrlSearchParams* params, PrStringView input) {
    urlSearchParamsClear(params);
    PrTextBuffer bytes = {};
    for (size_t first = 0; first < input.size;) {
        PrStringView rest = prStringFrom(input, first);
        PrStringView sequence = prStringSlice(rest, 0, prStringFindByte(rest, '&'));
        first += sequence.size + 1;
        if (sequence.size == 0)
            continue;
        size_t equals = prStringFindByte(sequence, '=');
        PrStringView value = equals != sequence.size ? prStringSlice(sequence, equals + 1, sequence.size - equals - 1) : "";
        PrRawString name = formDecode(&bytes, prStringSlice(sequence, 0, equals));
        params->pairs.push(UrlPair{name, formDecode(&bytes, value)});
    }
    prTextBufferReset(&bytes);
}

PrRawString urlSearchParamsSerialize(const UrlSearchParams* params) {
    PrTextBuffer out = {};
    for (uint32_t i = 0; i < params->pairs.count; ++i) {
        if (i != 0)
            prTextBufferAppendChar(&out, '&');
        formEncode(&out, prRawStringView(&params->pairs.data[i].name));
        prTextBufferAppendChar(&out, '=');
        formEncode(&out, prRawStringView(&params->pairs.data[i].value));
    }
    return prTextBufferTake(&out);
}

void urlSearchParamsUpdate(UrlSearchParams* params) {
    if (params->url == nullptr)
        return;
    PrRawString query = urlSearchParamsSerialize(params);
    prRawStringReset(&params->url->query);
    if (!prRawStringIsEmpty(&query))
        params->url->query = query;
    else
        prRawStringReset(&query);
}

void urlSearchParamsClear(UrlSearchParams* params) {
    for (uint32_t i = 0; i < params->pairs.count; ++i) {
        prRawStringReset(&params->pairs.data[i].name);
        prRawStringReset(&params->pairs.data[i].value);
    }
    params->pairs.count = 0;
}

void urlSearchParamsDestroy(UrlSearchParams* PR_NULLABLE params) {
    if (params == nullptr)
        return;
    urlSearchParamsClear(params);
    params->pairs.reset();
    prMemoryFree(params);
}

} // namespace pr_impl
