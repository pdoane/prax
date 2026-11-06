// File URLs and the paths of this machine.

#include "pr/url_file.h"

#include "pr/ascii.h"
#include "pr/environment.h"
#include "pr/raw_array.h"
#include "pr/string_ops.h"
#include "pr/url.h"
#include "pr/url_percent.h"
#include "pr/utf8.h"

namespace pr_impl {

namespace {

// Whether a backslash separates the parts of a path, as on Windows.
#if defined(_WIN32)
constexpr bool kBackslashSeparates = true;
#else
constexpr bool kBackslashSeparates = false;
#endif

// Appends `text` to `out`, with the bytes of a URL's path percent-encoded.
void appendEncoded(PrRawString* out, PrStringView text) {
    RawArray<char> bytes;
    bytes.resizeZeroed(uint32_t(prUrlPercentEncode(text, PrUrlPercentEncodeSet_Path, nullptr, 0)));
    prUrlPercentEncode(text, PrUrlPercentEncodeSet_Path, bytes.data, bytes.count);
    prRawStringAppend(out, PrStringView{bytes.data, bytes.count});
    bytes.reset();
}

// Returns true if the host of a file URL names this machine. That is the empty host, the machine's host name, or the
// first label of that name.
bool isThisHost(PrStringView host) {
    if (host.size == 0)
        return true;
    PrRawString name = prEnvironmentHostName();
    PrStringView own = prRawStringView(&name);
    PrStringView label = prStringSlice(own, 0, prStringFindByte(own, '.'));
    bool same = own.size != 0 && (prAsciiEqualsFold(host, own) || prAsciiEqualsFold(host, label));
    prRawStringReset(&name);
    return same;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrRawString prUrlFromFilePath(PrStringView path) {
    PrRawString url = prRawStringCopy("file://");
#if defined(_WIN32)
    if (path.size != 0 && path.data[0] != '/' && path.data[0] != '\\')
        prRawStringAppend(&url, "/");
#endif
    // The path encode set leaves `%` alone for text that is already encoded, so a `%` is escaped here. A backslash that
    // does not separate parts is escaped too, because a file URL's parser reads it as a slash.
    size_t at = 0;
    while (at < path.size) {
        size_t end = at;
        while (end < path.size && path.data[end] != '%' && path.data[end] != '\\')
            ++end;
        appendEncoded(&url, prStringSlice(path, at, end - at));
        if (end < path.size && path.data[end] == '%')
            prRawStringAppend(&url, "%25");
        else if (end < path.size && kBackslashSeparates)
            prRawStringAppend(&url, "/");
        else if (end < path.size)
            prRawStringAppend(&url, "%5C");
        at = end + 1;
    }
    return url;
}

bool prUrlToFilePath(PrStringView url, PrRawString* out) {
    PrUrl* parsed = nullptr;
    if (url.size == 0 || prUrlCreate(url, nullptr, PrCharset_Utf8, &parsed) != PrStatus_Ok)
        return false;
    bool found = false;
    if (prUrlScheme(parsed) == "file" && isThisHost(prUrlHost(parsed))) {
        PrStringView encoded = prUrlPath(parsed);
        RawArray<char> decoded;
        decoded.resizeZeroed(uint32_t(prUrlPercentDecode(encoded, nullptr, 0)));
        prUrlPercentDecode(encoded, decoded.data, decoded.count);
        PrStringView path{decoded.data, decoded.count};
#if defined(_WIN32)
        if (path.size >= 3 && path.data[0] == '/' && path.data[2] == ':')
            path = prStringSlice(path, 1, path.size - 1);
#endif
        found = path.size != 0 && prStringFindByte(path, '\0') == path.size && prUtf8Validate(path) == path.size;
        if (found)
            *out = prRawStringCopy(path);
        decoded.reset();
    }
    prUrlDestroy(parsed);
    return found;
}
