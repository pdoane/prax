// Addresses: the URL that the text of the address bar means, and what the chrome shows of a URL.

#include "examples/browser/address.h"

#include "pr/ascii.h"
#include "pr/string_ops.h"

#include <filesystem>
#include <system_error>

namespace browser {

using namespace pr;

namespace {

// The schemes whose URLs have no authority, which text names without `//`. The prax scheme names the browser's own
// pages (addressIsBrowserPage).
constexpr const char* kOpaqueSchemes[] = {"about", "data", "javascript", "mailto", "blob", "prax"};

// Returns the length of the scheme that `text` begins with (url/#scheme-state), or 0 if it begins with none. The colon
// after the scheme is not counted.
size_t schemeLength(const std::string& text) {
    if (text.empty() || !asciiIsLetter(text[0]))
        return 0;
    size_t at = 1;
    while (at < text.size() && (asciiIsLetter(text[at]) || asciiIsDigit(text[at]) || text[at] == '+' || text[at] == '-' ||
            text[at] == '.'))
        ++at;
    return at < text.size() && text[at] == ':' ? at : 0;
}

// Returns true if `text` begins with a scheme that makes it a URL as written.
bool hasScheme(const std::string& text) {
    size_t length = schemeLength(text);
    if (length == 0)
        return false;
    if (text.compare(length, 3, "://") == 0)
        return true;
    PrStringView scheme{text.data(), length};
    for (const char* opaque : kOpaqueSchemes) {
        if (asciiEqualsFold(scheme, opaque))
            return true;
    }
    return false;
}

// Returns the host and port at the start of `text`, up to its path, query or fragment.
std::string authorityOf(const std::string& text) {
    size_t end = text.find_first_of("/?#");
    return text.substr(0, end);
}

// Returns true if `authority` names the local host, which serves plain HTTP.
bool isLocal(const std::string& authority) {
    std::string host = authority.substr(0, authority.rfind(':'));
    if (authority.compare(0, 1, "[") == 0)
        host = authority.substr(0, authority.find(']') + 1);
    return asciiEqualsFold(host, "localhost") || host == "127.0.0.1" || host == "[::1]";
}

// Returns true if `authority` reads as a host: a name with a dot that is neither its first nor its last character, the
// local host, or a host and a port.
bool isHost(const std::string& authority) {
    if (authority.empty())
        return false;
    if (isLocal(authority))
        return true;
    size_t colon = authority.rfind(':');
    if (colon != std::string::npos && colon != 0 && colon + 1 < authority.size()) {
        bool port = true;
        for (size_t i = colon + 1; i < authority.size(); ++i)
            port = port && asciiIsDigit(authority[i]);
        if (port)
            return true;
    }
    size_t dot = authority.find('.');
    return dot != std::string::npos && dot != 0 && authority.back() != '.';
}

// Returns the file: URL of the regular file at `path`, or the empty string if there is none.
std::string fileUrl(const std::string& path) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error))
        return std::string();
    std::filesystem::path absolute = std::filesystem::absolute(path, error);
    if (error)
        return std::string();
    return "file://" + absolute.lexically_normal().generic_string();
}

// Returns `text` percent-encoded as a query's value (url/#application-x-www-form-urlencoded-percent-encode-set), with
// each space as a plus sign.
std::string formEncoded(const std::string& text) {
    static const char kHex[] = "0123456789ABCDEF";
    std::string encoded;
    for (char c : text) {
        if (asciiIsLetter(c) || asciiIsDigit(c) || c == '*' || c == '-' || c == '.' || c == '_') {
            encoded += c;
        } else if (c == ' ') {
            encoded += '+';
        } else {
            encoded += '%';
            encoded += kHex[uint8_t(c) >> 4];
            encoded += kHex[uint8_t(c) & 15];
        }
    }
    return encoded;
}

} // namespace

std::string addressResolve(PrStringView text) {
    PrStringView trimmed = stringTrim(text);
    std::string typed(trimmed.data, trimmed.size);
    if (typed.empty())
        return typed;
    if (hasScheme(typed))
        return typed;
    std::string file = fileUrl(typed);
    if (!file.empty())
        return file;
    bool spaced = false;
    for (char c : typed)
        spaced = spaced || asciiIsSpace(c);
    std::string authority = authorityOf(typed);
    if (!spaced && isHost(authority))
        return (isLocal(authority) ? "http://" : "https://") + typed;
    return kAddressSearch + formEncoded(typed);
}

bool addressIsBlank(PrStringView url) {
    return url.size == 0 || url == kAddressBlank;
}

bool addressIsBrowserPage(PrStringView url) {
    return asciiStartsWithFold(url, "prax:");
}

std::string addressShown(PrStringView url) {
    return addressIsBlank(url) ? std::string() : std::string(url.data, url.size);
}

std::string addressHost(PrStringView url) {
    std::string text(url.data, url.size);
    size_t length = schemeLength(text);
    if (length == 0 || text.compare(length, 3, "://") != 0)
        return std::string();
    std::string authority = authorityOf(text.substr(length + 3));
    size_t at = authority.rfind('@');
    if (at != std::string::npos)
        authority = authority.substr(at + 1);
    if (authority.compare(0, 1, "[") == 0)
        authority = authority.substr(0, authority.find(']') + 1);
    else
        authority = authority.substr(0, authority.find(':'));
    if (authority.compare(0, 4, "www.") == 0)
        authority = authority.substr(4);
    return authority;
}

bool addressMatches(PrStringView text, PrStringView query) {
    return asciiContainsFold(text, query);
}

} // namespace browser
