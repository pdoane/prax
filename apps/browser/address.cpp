// Addresses: the URL that the text of the address bar means, and what the chrome shows of a URL.

#include "apps/browser/address.h"

#include "pr/ascii.h"
#include "pr/file.h"
#include "pr/format.h"
#include "pr/string_ops.h"

namespace browser {

namespace {

// The schemes whose URLs have no authority, which text names without `//`. The prax scheme names the browser's own
// pages (addressIsBrowserPage).
constexpr const char* kOpaqueSchemes[] = {"about", "data", "javascript", "mailto", "blob", "prax"};

// Returns the length of the scheme that `text` begins with (url/#scheme-state), or 0 if it begins with none. The colon
// after the scheme is not counted.
size_t schemeLength(PrStringView text) {
    if (text.isEmpty() || !prAsciiIsLetter(text.data[0]))
        return 0;
    size_t at = 1;
    while (at < text.size) {
        char c = text.data[at];
        if (!prAsciiIsLetter(c) && !prAsciiIsDigit(c) && c != '+' && c != '-' && c != '.')
            break;
        ++at;
    }
    return at < text.size && text.data[at] == ':' ? at : 0;
}

// Returns true if `text` has "://" after the scheme of `length` bytes that it begins with.
bool hasAuthority(PrStringView text, size_t length) {
    return prStringStartsWith(prStringSlice(text, length, text.size - length), "://");
}

// Returns true if `text` begins with a scheme that makes it a URL as written.
bool hasScheme(PrStringView text) {
    size_t length = schemeLength(text);
    if (length == 0)
        return false;
    if (hasAuthority(text, length))
        return true;
    PrStringView scheme = prStringSlice(text, 0, length);
    for (const char* opaque : kOpaqueSchemes) {
        if (prAsciiEqualsFold(scheme, opaque))
            return true;
    }
    return false;
}

// Returns the host and port at the start of `text`, up to its path, query or fragment.
PrStringView authorityOf(PrStringView text) {
    size_t end = 0;
    while (end < text.size && text.data[end] != '/' && text.data[end] != '?' && text.data[end] != '#')
        ++end;
    return prStringSlice(text, 0, end);
}

// Returns the host of `authority`, without its port. A bracketed IPv6 host keeps its brackets.
PrStringView hostOf(PrStringView authority) {
    if (prStringStartsWith(authority, "[")) {
        size_t close = prStringFindByte(authority, ']');
        return prStringSlice(authority, 0, close < authority.size ? close + 1 : authority.size);
    }
    return prStringSlice(authority, 0, prStringFindLastByte(authority, ':'));
}

// Returns true if `authority` names the local host, which serves plain HTTP.
bool isLocal(PrStringView authority) {
    PrStringView host = hostOf(authority);
    return prAsciiEqualsFold(host, "localhost") || host == "127.0.0.1" || host == "[::1]";
}

// Returns true if `authority` reads as a host: a name with a dot that is neither its first nor its last character, the
// local host, or a host and a port.
bool isHost(PrStringView authority) {
    if (authority.isEmpty())
        return false;
    if (isLocal(authority))
        return true;
    size_t colon = prStringFindLastByte(authority, ':');
    if (colon != authority.size && colon != 0 && colon + 1 < authority.size) {
        bool port = true;
        for (size_t i = colon + 1; i < authority.size; ++i)
            port = port && prAsciiIsDigit(authority.data[i]);
        if (port)
            return true;
    }
    size_t dot = prStringFindByte(authority, '.');
    return dot != authority.size && dot != 0 && authority.data[authority.size - 1] != '.';
}

// Returns the file: URL of the regular file at `path`, or the null string if there is none.
String fileUrl(PrStringView path) {
    PrFileInfo info = {};
    if (!prFileInfo(path, &info) || info.kind != PrFileKind_Regular)
        return String();
    String absolute = String::adopt(prFilePathAbsolute(path));
    if (absolute.isEmpty())
        return String();
    return format("file://{}", absolute);
}

// Returns `text` percent-encoded as a query's value (url/#application-x-www-form-urlencoded-percent-encode-set), with
// each space as a plus sign.
String formEncoded(PrStringView text) {
    static const char kHex[] = "0123456789ABCDEF";
    String encoded;
    for (char c : text) {
        if (prAsciiIsLetter(c) || prAsciiIsDigit(c) || c == '*' || c == '-' || c == '.' || c == '_') {
            encoded.append(c);
        } else if (c == ' ') {
            encoded.append('+');
        } else {
            encoded.append('%');
            encoded.append(kHex[uint8_t(c) >> 4]);
            encoded.append(kHex[uint8_t(c) & 15]);
        }
    }
    return encoded;
}

} // namespace

String addressResolve(PrStringView text) {
    PrStringView typed = prStringTrim(text);
    if (typed.isEmpty())
        return String();
    if (hasScheme(typed))
        return typed;
    String file = fileUrl(typed);
    if (!file.isEmpty())
        return file;
    bool spaced = false;
    for (char c : typed)
        spaced = spaced || prAsciiIsSpace(c);
    PrStringView authority = authorityOf(typed);
    if (!spaced && isHost(authority))
        return format("{}{}", isLocal(authority) ? "http://" : "https://", typed);
    return format("{}{}", kAddressSearch, formEncoded(typed));
}

bool addressIsBlank(PrStringView url) {
    return url.isEmpty() || url == kAddressBlank;
}

bool addressIsBrowserPage(PrStringView url) {
    return prAsciiStartsWithFold(url, "prax:");
}

PrStringView addressShown(PrStringView url) {
    if (addressIsBlank(url))
        return "";
    return url;
}

PrStringView addressHost(PrStringView url) {
    size_t length = schemeLength(url);
    if (length == 0 || !hasAuthority(url, length))
        return "";
    PrStringView authority = authorityOf(prStringSlice(url, length + 3, url.size - length - 3));
    size_t at = prStringFindLastByte(authority, '@');
    if (at != authority.size)
        authority = prStringSlice(authority, at + 1, authority.size - at - 1);
    PrStringView host = hostOf(authority);
    if (prStringStartsWith(host, "www."))
        host = prStringSlice(host, 4, host.size - 4);
    return host;
}

bool addressMatches(PrStringView text, PrStringView query) {
    return prAsciiContainsFold(text, query);
}

} // namespace browser
