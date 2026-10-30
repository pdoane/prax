#include "html/src/html_image.h"

#include "pr/ascii.h"
#include "pr/integer.h"
#include "pr/string_ops.h"

namespace pr_impl {
namespace {

uint32_t bigEndian16(const uint8_t* at) {
    return uint32_t(at[0]) << 8 | at[1];
}

uint32_t bigEndian32(const uint8_t* at) {
    return uint32_t(at[0]) << 24 | uint32_t(at[1]) << 16 | uint32_t(at[2]) << 8 | at[3];
}

uint32_t littleEndian16(const uint8_t* at) {
    return uint32_t(at[1]) << 8 | at[0];
}

uint32_t littleEndian24(const uint8_t* at) {
    return uint32_t(at[2]) << 16 | uint32_t(at[1]) << 8 | at[0];
}

uint32_t littleEndian32(const uint8_t* at) {
    return uint32_t(at[3]) << 24 | uint32_t(at[2]) << 16 | uint32_t(at[1]) << 8 | at[0];
}

bool startsWith(const uint8_t* data, size_t size, PrStringView signature, size_t at = 0) {
    return size >= at + signature.size && PrStringView{reinterpret_cast<const char*>(data) + at, signature.size} == signature;
}

// Reads the size of a JPEG image from the frame header of the first start-of-frame marker, skipping the segments before
// it.
bool jpegSize(const uint8_t* data, size_t size, uint32_t* width, uint32_t* height) {
    size_t at = 2;
    while (at + 4 <= size) {
        if (data[at] != 0xFF)
            return false;
        uint8_t marker = data[at + 1];
        if (marker == 0xFF) {
            ++at;
            continue;
        }
        uint32_t length = bigEndian16(data + at + 2);
        bool frame = marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC;
        if (frame) {
            if (at + 9 > size)
                return false;
            *height = bigEndian16(data + at + 5);
            *width = bigEndian16(data + at + 7);
            return true;
        }
        at += 2 + length;
    }
    return false;
}

// Reads the size of a WebP image from the header of its first chunk, which is lossy, lossless or extended.
bool webpSize(const uint8_t* data, size_t size, uint32_t* width, uint32_t* height) {
    if (startsWith(data, size, "VP8 ", 12) && size >= 30) {
        *width = littleEndian16(data + 26) & 0x3FFF;
        *height = littleEndian16(data + 28) & 0x3FFF;
        return true;
    }
    if (startsWith(data, size, "VP8L", 12) && size >= 25) {
        uint32_t bits = littleEndian32(data + 21);
        *width = (bits & 0x3FFF) + 1;
        *height = (bits >> 14 & 0x3FFF) + 1;
        return true;
    }
    if (startsWith(data, size, "VP8X", 12) && size >= 30) {
        *width = littleEndian24(data + 24) + 1;
        *height = littleEndian24(data + 27) + 1;
        return true;
    }
    return false;
}

// Returns a length of an SVG image's root in pixels, if the length is a number, bare or in px. Otherwise returns zero,
// since a percentage or a relative length resolves against the image's context.
uint32_t svgLength(PrStringView value) {
    value = stringTrim(value);
    uint64_t whole = 0;
    size_t at = parseU64Prefix(value.data, value.size, &whole);
    if (at == 0)
        return 0;
    if (at < value.size && value.data[at] == '.') {
        ++at;
        while (at < value.size && asciiIsDigit(value.data[at]))
            ++at;
    }
    PrStringView unit = stringSlice(value, at, value.size - at);
    return unit.size == 0 || unit == "px" ? uint32_t(whole < UINT32_MAX ? whole : UINT32_MAX) : 0;
}

// Reads the size of an SVG image from the width and height attributes of its root element.
bool svgSize(PrStringView text, uint32_t* width, uint32_t* height) {
    size_t start = 0;
    while (start < text.size && asciiIsSpace(text.data[start]))
        ++start;
    if (start == text.size || text.data[start] != '<')
        return false;
    size_t root = start;
    for (;;) {
        root += stringFind(stringSlice(text, root, text.size - root), '<');
        if (root >= text.size)
            return false;
        PrStringView rest = stringSlice(text, root, text.size - root);
        if (!stringStartsWith(rest, "<?") && !stringStartsWith(rest, "<!"))
            break;
        ++root;
    }
    PrStringView rest = stringSlice(text, root, text.size - root);
    if (!stringStartsWith(rest, "<svg") || rest.size == 4 || !(asciiIsSpace(rest.data[4]) || rest.data[4] == '>'))
        return false;
    *width = 0;
    *height = 0;
    size_t at = 4;
    while (at < rest.size && rest.data[at] != '>') {
        if (asciiIsSpace(rest.data[at]) || rest.data[at] == '/') {
            ++at;
            continue;
        }
        size_t name = at;
        while (at < rest.size && rest.data[at] != '=' && rest.data[at] != '>' && !asciiIsSpace(rest.data[at]))
            ++at;
        PrStringView attribute = stringSlice(rest, name, at - name);
        while (at < rest.size && asciiIsSpace(rest.data[at]))
            ++at;
        if (at == rest.size || rest.data[at] != '=')
            continue;
        ++at;
        while (at < rest.size && asciiIsSpace(rest.data[at]))
            ++at;
        if (at == rest.size || (rest.data[at] != '"' && rest.data[at] != '\''))
            return true;
        char quote = rest.data[at++];
        size_t value = at;
        while (at < rest.size && rest.data[at] != quote)
            ++at;
        PrStringView content = stringSlice(rest, value, at - value);
        ++at;
        if (attribute == "width")
            *width = svgLength(content);
        else if (attribute == "height")
            *height = svgLength(content);
    }
    return true;
}

} // namespace

bool htmlImageSize(PrStringView bytes, uint32_t* width, uint32_t* height) {
    const uint8_t* data = reinterpret_cast<const uint8_t*>(bytes.data);
    size_t size = bytes.size;
    *width = 0;
    *height = 0;
    if (startsWith(data, size, "\x89PNG\r\n\x1A\n")) {
        if (size < 24)
            return false;
        *width = bigEndian32(data + 16);
        *height = bigEndian32(data + 20);
        return true;
    }
    if (startsWith(data, size, "GIF87a") || startsWith(data, size, "GIF89a")) {
        if (size < 10)
            return false;
        *width = littleEndian16(data + 6);
        *height = littleEndian16(data + 8);
        return true;
    }
    if (startsWith(data, size, "\xFF\xD8\xFF"))
        return jpegSize(data, size, width, height);
    if (startsWith(data, size, "RIFF") && startsWith(data, size, "WEBPVP", 8))
        return webpSize(data, size, width, height);
    if (startsWith(data, size, "BM")) {
        if (size < 26)
            return false;
        int32_t signedHeight = int32_t(littleEndian32(data + 22));
        *width = littleEndian32(data + 18);
        *height = signedHeight < 0 ? uint32_t(-int64_t(signedHeight)) : uint32_t(signedHeight);
        return true;
    }
    // An icon or cursor file. Its first image gives its size, and a 0 byte means 256.
    if (size >= 4 && data[0] == 0 && data[1] == 0 && (data[2] == 1 || data[2] == 2) && data[3] == 0) {
        if (size < 8)
            return false;
        *width = data[6] != 0 ? data[6] : 256;
        *height = data[7] != 0 ? data[7] : 256;
        return true;
    }
    return svgSize(bytes, width, height);
}

bool htmlImageIsSvg(PrStringView bytes) {
    uint32_t width = 0;
    uint32_t height = 0;
    return svgSize(bytes, &width, &height);
}

} // namespace pr_impl
