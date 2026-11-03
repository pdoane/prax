// Base64 encoding and decoding of bytes.

#include "pr/base64.h"

#include "pr/ascii.h"

namespace pr_impl {
namespace {

constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// The markers, above 63, that the decoder's table holds for a byte that is not in the alphabet. For a byte that is, the
// table holds the six bits that the byte encodes.
constexpr uint8_t kSpace = 64;
constexpr uint8_t kPad = 65;
constexpr uint8_t kInvalid = 255;

struct Sextets {
    uint8_t of[256];
};

constexpr Sextets sextets() {
    Sextets t{};
    for (uint32_t c = 0; c < 256; ++c)
        t.of[c] = prAsciiIsSpace(char(c)) ? kSpace : c == '=' ? kPad : kInvalid;
    for (uint8_t v = 0; v < 64; ++v)
        t.of[uint8_t(kAlphabet[v])] = v;
    return t;
}

constexpr Sextets kSextets = sextets();

} // namespace

} // namespace pr_impl

using namespace pr_impl;

size_t prBase64EncodedSize(size_t size) {
    return (size + 2) / 3 * 4;
}

size_t prBase64Encode(PrStringView input, char* PR_NULLABLE out, size_t capacity) {
    if (out == nullptr)
        capacity = 0;
    size_t written = 0;
    auto put = [&](char c) {
        if (written < capacity)
            out[written] = c;
        ++written;
    };
    for (size_t i = 0; i < input.size; i += 3) {
        uint32_t group = uint32_t(uint8_t(input.data[i])) << 16;
        size_t left = input.size - i;
        if (left > 1)
            group |= uint32_t(uint8_t(input.data[i + 1])) << 8;
        if (left > 2)
            group |= uint8_t(input.data[i + 2]);
        put(kAlphabet[(group >> 18) & 63]);
        put(kAlphabet[(group >> 12) & 63]);
        put(left > 1 ? kAlphabet[(group >> 6) & 63] : '=');
        put(left > 2 ? kAlphabet[group & 63] : '=');
    }
    return written;
}

bool prBase64Decode(PrStringView input, char* PR_NULLABLE out, size_t capacity, size_t* size) {
    // After removing ASCII whitespace, and one or two trailing '=' if the length is a multiple of four, only alphabet
    // characters may remain. Their number must not leave a remainder of one when divided by four. Each output byte
    // comes from characters already read, so the output can overwrite the input.
    if (out == nullptr)
        capacity = 0;
    const uint8_t* in = reinterpret_cast<const uint8_t*>(input.data);
    size_t n = input.size;
    size_t written = 0;
    size_t count = 0;
    uint32_t equals = 0;
    uint32_t buffer = 0;
    uint32_t bits = 0;
    for (size_t i = 0; i < n;) {
        // Four characters of the alphabet at a byte boundary decode to three bytes.
        if (bits == 0 && equals == 0 && i + 4 <= n) {
            uint32_t a = kSextets.of[in[i]];
            uint32_t b = kSextets.of[in[i + 1]];
            uint32_t c = kSextets.of[in[i + 2]];
            uint32_t d = kSextets.of[in[i + 3]];
            if ((a | b | c | d) < 64) {
                uint32_t group = a << 18 | b << 12 | c << 6 | d;
                if (written + 3 <= capacity) {
                    out[written] = char(group >> 16);
                    out[written + 1] = char(group >> 8);
                    out[written + 2] = char(group);
                } else {
                    for (uint32_t k = 0; k < 3; ++k) {
                        if (written + k < capacity)
                            out[written + k] = char(group >> (16 - 8 * k));
                    }
                }
                written += 3;
                count += 4;
                i += 4;
                continue;
            }
        }
        uint32_t value = kSextets.of[in[i++]];
        if (value == kSpace)
            continue;
        if (value == kPad) {
            ++equals;
            continue;
        }
        if (value == kInvalid || equals != 0)
            return false;
        ++count;
        buffer = (buffer << 6) | value;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (written < capacity)
                out[written] = char((buffer >> bits) & 0xff);
            ++written;
        }
    }
    if (equals > 2 || (equals != 0 && (count + equals) % 4 != 0) || count % 4 == 1)
        return false;
    *size = written;
    return true;
}
