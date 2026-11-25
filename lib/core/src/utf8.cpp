#include "pr/utf8.h"

#include "pr/mem.h"

namespace pr_impl {

namespace {

// The two encodings, which differ only in whether a surrogate's three bytes are a well-formed sequence.
enum {
    Form_Utf8,
    Form_Wtf8,
};
typedef uint8_t Form;

// The sequence that a lead byte starts. The second byte's range excludes the overlong encodings and the values above
// U+10FFFF, and in UTF-8 the encodings of the surrogates. Every later byte is a continuation byte.
struct Lead {
    uint8_t length;     // zero for a byte that opens no sequence
    uint8_t low;        // the lowest second byte
    uint8_t high;       // the highest second byte
};

struct LeadTable {
    Lead of[256];
};

constexpr LeadTable makeLeads(Form form) {
    LeadTable table = {};
    for (uint32_t b = 0; b < 0x80; ++b)
        table.of[b] = Lead{1, 0, 0};
    for (uint32_t b = 0xc2; b <= 0xdf; ++b)
        table.of[b] = Lead{2, 0x80, 0xbf};
    for (uint32_t b = 0xe0; b <= 0xef; ++b)
        table.of[b] = Lead{3, 0x80, 0xbf};
    for (uint32_t b = 0xf0; b <= 0xf4; ++b)
        table.of[b] = Lead{4, 0x80, 0xbf};
    table.of[0xe0].low = 0xa0;
    table.of[0xed].high = form == Form_Wtf8 ? 0xbf : 0x9f;
    table.of[0xf0].low = 0x90;
    table.of[0xf4].high = 0x8f;
    return table;
}

constexpr bool isContinuation(uint32_t c) {
    return (c & 0xc0) == 0x80;
}

constexpr LeadTable kLeads[] = {makeLeads(Form_Utf8), makeLeads(Form_Wtf8)};

// Returns the length of the well-formed sequence at `offset`, or zero if the sequence is ill formed.
uint32_t sequenceAt(Form form, const uint8_t* bytes, size_t size, size_t offset) {
    Lead lead = kLeads[form].of[bytes[offset]];
    if (lead.length == 0 || lead.length > size - offset)
        return 0;
    if (lead.length == 1)
        return 1;
    uint8_t second = bytes[offset + 1];
    if (second < lead.low || second > lead.high)
        return 0;
    if (lead.length >= 3 && !isContinuation(bytes[offset + 2]))
        return 0;
    if (lead.length == 4 && !isContinuation(bytes[offset + 3]))
        return 0;
    return lead.length;
}

// Returns the length of the maximal subpart at `offset`, for a sequence that is ill formed. One U+FFFD replaces it.
uint32_t subpartAt(Form form, const uint8_t* bytes, size_t size, size_t offset) {
    Lead lead = kLeads[form].of[bytes[offset]];
    if (lead.length == 0 || offset + 1 >= size)
        return 1;
    uint8_t second = bytes[offset + 1];
    if (second < lead.low || second > lead.high)
        return 1;
    if (lead.length == 2 || offset + 2 >= size || !isContinuation(bytes[offset + 2]))
        return 2;
    return 3;
}

// Returns true if the three bytes at `offset` are a high surrogate and the three after them a low surrogate: a
// surrogate pair written as its two surrogates, which WTF-8 does not allow.
bool splitPairAt(const uint8_t* bytes, size_t size, size_t offset) {
    return size - offset >= 6 && bytes[offset] == 0xed && bytes[offset + 1] >= 0xa0 && bytes[offset + 1] <= 0xaf &&
        isContinuation(bytes[offset + 2]) && bytes[offset + 3] == 0xed && bytes[offset + 4] >= 0xb0 &&
        bytes[offset + 4] <= 0xbf && isContinuation(bytes[offset + 5]);
}

// Returns the code point of the well-formed sequence of `length` bytes at `p`.
uint32_t codePointOf(const uint8_t* p, uint32_t length) {
    // The lead byte's payload is the bits below its length marker.
    switch (length) {
        case 1:
            return p[0];
        case 2:
            return (uint32_t(p[0] & 0x1f) << 6) | (p[1] & 0x3fu);
        case 3:
            return (uint32_t(p[0] & 0x0f) << 12) | (uint32_t(p[1] & 0x3f) << 6) | (p[2] & 0x3fu);
        default:
            return (uint32_t(p[0] & 0x07) << 18) | (uint32_t(p[1] & 0x3f) << 12) | (uint32_t(p[2] & 0x3f) << 6) | (p[3] & 0x3fu);
    }
}

// Decodes the sequence at `offset` in `form`. An ill-formed sequence yields kPrUtf8Replacement for its maximal subpart,
// and a split surrogate pair in WTF-8 its code point.
template <Form form>
uint32_t decode(PrStringView text, size_t offset, uint32_t* outCodePoint) {
    if (offset >= text.size)
        return 0;

    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(text.data);
    if (bytes[offset] < 0x80) {
        *outCodePoint = bytes[offset];
        return 1;
    }
    uint32_t length = sequenceAt(form, bytes, text.size, offset);
    if (length == 0) {
        *outCodePoint = kPrUtf8Replacement;
        return subpartAt(form, bytes, text.size, offset);
    }
    // Only a high surrogate, which 0xed leads, can begin a split pair.
    if (form == Form_Wtf8 && bytes[offset] == 0xed && splitPairAt(bytes, text.size, offset)) {
        uint32_t high = codePointOf(bytes + offset, 3);
        uint32_t low = codePointOf(bytes + offset + 3, 3);
        *outCodePoint = 0x10000 + ((high - 0xd800) << 10) + (low - 0xdc00);
        return 6;
    }
    *outCodePoint = codePointOf(bytes + offset, length);
    return length;
}

// UTF-8's validating automaton, whose state is a shift amount. The row for a byte holds six bits for each state, giving
// the next state after that byte. It accepts the sequences that kLeads does for UTF-8.
enum : uint64_t {
    DfaState_Accept = 0 * 6,   // between sequences
    DfaState_One = 1 * 6,      // one continuation byte left
    DfaState_Two = 2 * 6,      // two left
    DfaState_Three = 3 * 6,    // three left
    DfaState_E0 = 4 * 6,       // after 0xe0, whose second byte is 0xa0 to 0xbf
    DfaState_Ed = 5 * 6,       // after 0xed, whose second byte is 0x80 to 0x9f
    DfaState_F0 = 6 * 6,       // after 0xf0, whose second byte is 0x90 to 0xbf
    DfaState_F4 = 7 * 6,       // after 0xf4, whose second byte is 0x80 to 0x8f
    DfaState_Error = 8 * 6,    // an ill-formed sequence has been read
    DfaState_Count = 9,
};
typedef uint64_t DfaState;

constexpr DfaState dfaNext(DfaState state, uint32_t byte) {
    bool continuation = isContinuation(byte);
    switch (state) {
        case DfaState_Accept:
            if (byte < 0x80)
                return DfaState_Accept;
            if (byte >= 0xc2 && byte <= 0xdf)
                return DfaState_One;
            if (byte == 0xe0)
                return DfaState_E0;
            if (byte == 0xed)
                return DfaState_Ed;
            if (byte >= 0xe1 && byte <= 0xef)
                return DfaState_Two;
            if (byte == 0xf0)
                return DfaState_F0;
            if (byte >= 0xf1 && byte <= 0xf3)
                return DfaState_Three;
            if (byte == 0xf4)
                return DfaState_F4;
            return DfaState_Error;
        case DfaState_One:   return continuation ? DfaState_Accept : DfaState_Error;
        case DfaState_Two:   return continuation ? DfaState_One : DfaState_Error;
        case DfaState_Three: return continuation ? DfaState_Two : DfaState_Error;
        case DfaState_E0:    return byte >= 0xa0 && byte <= 0xbf ? DfaState_One : DfaState_Error;
        case DfaState_Ed:    return byte >= 0x80 && byte <= 0x9f ? DfaState_One : DfaState_Error;
        case DfaState_F0:    return byte >= 0x90 && byte <= 0xbf ? DfaState_Two : DfaState_Error;
        case DfaState_F4:    return byte >= 0x80 && byte <= 0x8f ? DfaState_Two : DfaState_Error;
        default:             return DfaState_Error;
    }
}

struct DfaTable {
    uint64_t row[256];
};

constexpr DfaTable makeDfa() {
    DfaTable table = {};
    for (uint32_t b = 0; b < 256; ++b) {
        for (DfaState s = 0; s < DfaState_Count; ++s)
            table.row[b] |= dfaNext(s * 6, b) << (s * 6);
    }
    return table;
}

constexpr DfaTable kDfa = makeDfa();

// Returns the high bits of sixteen bytes, which are all zero if the bytes are all ASCII.
uint64_t highBits(const uint8_t* bytes) {
    uint64_t low;
    uint64_t high;
    prMemCopy(&low, bytes, 8);
    prMemCopy(&high, bytes + 8, 8);
    return (low | high) & 0x8080808080808080ull;
}

// Returns the length of the longest prefix of `text` that is well-formed UTF-8. A run of ASCII costs a test of its high
// bits for each sixteen bytes.
size_t validateUtf8(PrStringView text) {
    const DfaTable& dfa = kDfa;
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(text.data);
    size_t size = text.size;
    DfaState state = DfaState_Accept;
    size_t boundary = 0;
    size_t at = 0;
    for (; size - at >= 64; at += 64) {
        uint64_t high[4] = {
            highBits(bytes + at), highBits(bytes + at + 16),
            highBits(bytes + at + 32), highBits(bytes + at + 48),
        };
        if (state == DfaState_Accept && (high[0] | high[1] | high[2] | high[3]) == 0)
            continue;
        for (uint32_t part = 0; part < 4; ++part) {
            if (state == DfaState_Accept) {
                boundary = at + part * 16;
                if (high[part] == 0)
                    continue;
            }
            const uint8_t* block = bytes + at + part * 16;
            for (uint32_t k = 0; k < 16; ++k)
                state = (dfa.row[block[k]] >> state) & 63;
        }
        if (state == DfaState_Error)
            break;
    }
    if (state == DfaState_Accept)
        boundary = at;
    for (; at < size && state != DfaState_Error; ++at)
        state = (dfa.row[bytes[at]] >> state) & 63;
    if (state == DfaState_Accept)
        return size;

    // No ill-formed sequence begins before `boundary`. Walking the sequences from there finds the first.
    while (boundary < size) {
        uint32_t length = sequenceAt(Form_Utf8, bytes, size, boundary);
        if (length == 0)
            break;
        boundary += length;
    }
    return boundary;
}

// WTF-8's validating automaton, whose state indexes a row of the next state by byte. It accepts what kLeads does for
// WTF-8, and also tracks whether the last sequence was a high surrogate, after which a low surrogate's sequence is ill
// formed: the two are a pair, which WTF-8 writes as its code point's four bytes.
enum : uint8_t {
    WtfState_Accept,        // between sequences
    WtfState_One,           // one continuation byte left
    WtfState_Two,           // two left
    WtfState_Three,         // three left
    WtfState_E0,            // after 0xe0, whose second byte is 0xa0 to 0xbf
    WtfState_Ed,            // after 0xed: 0x80 to 0x9f, a high surrogate's 0xa0 to 0xaf, or a low one's 0xb0 to 0xbf
    WtfState_F0,            // after 0xf0, whose second byte is 0x90 to 0xbf
    WtfState_F4,            // after 0xf4, whose second byte is 0x80 to 0x8f
    WtfState_HighOne,       // one continuation byte of a high surrogate left
    WtfState_AfterHigh,     // between sequences, after a high surrogate
    WtfState_EdAfterHigh,   // after 0xed after a high surrogate, which no low surrogate may follow
    WtfState_Error,         // an ill-formed sequence has been read
    WtfState_Count,
};
typedef uint8_t WtfState;

constexpr WtfState wtfNext(WtfState state, uint32_t byte) {
    bool continuation = isContinuation(byte);
    switch (state) {
        case WtfState_Accept:
        case WtfState_AfterHigh:
            if (byte < 0x80)
                return WtfState_Accept;
            if (byte >= 0xc2 && byte <= 0xdf)
                return WtfState_One;
            if (byte == 0xe0)
                return WtfState_E0;
            if (byte == 0xed)
                return state == WtfState_AfterHigh ? WtfState_EdAfterHigh : WtfState_Ed;
            if (byte >= 0xe1 && byte <= 0xef)
                return WtfState_Two;
            if (byte == 0xf0)
                return WtfState_F0;
            if (byte >= 0xf1 && byte <= 0xf3)
                return WtfState_Three;
            if (byte == 0xf4)
                return WtfState_F4;
            return WtfState_Error;
        case WtfState_One:     return continuation ? WtfState_Accept : WtfState_Error;
        case WtfState_Two:     return continuation ? WtfState_One : WtfState_Error;
        case WtfState_Three:   return continuation ? WtfState_Two : WtfState_Error;
        case WtfState_E0:      return byte >= 0xa0 && byte <= 0xbf ? WtfState_One : WtfState_Error;
        case WtfState_F0:      return byte >= 0x90 && byte <= 0xbf ? WtfState_Two : WtfState_Error;
        case WtfState_F4:      return byte >= 0x80 && byte <= 0x8f ? WtfState_Two : WtfState_Error;
        case WtfState_HighOne: return continuation ? WtfState_AfterHigh : WtfState_Error;
        case WtfState_Ed:
        case WtfState_EdAfterHigh:
            if (byte >= 0x80 && byte <= 0x9f)
                return WtfState_One;
            if (byte >= 0xa0 && byte <= 0xaf)
                return WtfState_HighOne;
            if (byte >= 0xb0 && byte <= 0xbf && state == WtfState_Ed)
                return WtfState_One;
            return WtfState_Error;
        default:
            return WtfState_Error;
    }
}

struct WtfTable {
    WtfState next[WtfState_Count][256];
};

constexpr WtfTable makeWtf() {
    WtfTable table = {};
    for (uint32_t s = 0; s < WtfState_Count; ++s) {
        for (uint32_t b = 0; b < 256; ++b)
            table.next[s][b] = wtfNext(WtfState(s), b);
    }
    return table;
}

constexpr WtfTable kWtf = makeWtf();

// Returns the length of the longest prefix of `text` that is well-formed WTF-8, as validateUtf8 does.
size_t validateWtf8(PrStringView text) {
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(text.data);
    size_t size = text.size;
    WtfState state = WtfState_Accept;
    size_t boundary = 0;
    size_t at = 0;
    for (; size - at >= 64; at += 64) {
        uint64_t high[4] = {
            highBits(bytes + at), highBits(bytes + at + 16),
            highBits(bytes + at + 32), highBits(bytes + at + 48),
        };
        if (state == WtfState_Accept && (high[0] | high[1] | high[2] | high[3]) == 0)
            continue;
        for (uint32_t part = 0; part < 4; ++part) {
            if (state == WtfState_Accept) {
                boundary = at + part * 16;
                if (high[part] == 0)
                    continue;
            }
            const uint8_t* block = bytes + at + part * 16;
            for (uint32_t k = 0; k < 16; ++k)
                state = kWtf.next[state][block[k]];
        }
        if (state == WtfState_Error)
            break;
    }
    if (state == WtfState_Accept)
        boundary = at;
    for (; at < size && state != WtfState_Error; ++at)
        state = kWtf.next[state][bytes[at]];
    if (state == WtfState_Accept || state == WtfState_AfterHigh)
        return size;

    // No ill-formed sequence begins before `boundary`. Walking the sequences from there finds the first, which may be a
    // split pair.
    while (boundary < size && !splitPairAt(bytes, size, boundary)) {
        uint32_t length = sequenceAt(Form_Wtf8, bytes, size, boundary);
        if (length == 0)
            break;
        boundary += length;
    }
    return boundary;
}

// Encodes `codePoint`, any code point or kPrUtf8Replacement for a value above kPrUtf8MaxCodePoint, into `out`, and
// returns its length in bytes. Both encoders inline it.
inline uint32_t encode(uint32_t codePoint, char* out) {
    if (codePoint > kPrUtf8MaxCodePoint)
        codePoint = kPrUtf8Replacement;
    if (codePoint < 0x80) {
        out[0] = char(codePoint);
        return 1;
    }
    if (codePoint < 0x800) {
        out[0] = char(0xc0 | (codePoint >> 6));
        out[1] = char(0x80 | (codePoint & 0x3f));
        return 2;
    }
    if (codePoint < 0x10000) {
        out[0] = char(0xe0 | (codePoint >> 12));
        out[1] = char(0x80 | ((codePoint >> 6) & 0x3f));
        out[2] = char(0x80 | (codePoint & 0x3f));
        return 3;
    }
    out[0] = char(0xf0 | (codePoint >> 18));
    out[1] = char(0x80 | ((codePoint >> 12) & 0x3f));
    out[2] = char(0x80 | ((codePoint >> 6) & 0x3f));
    out[3] = char(0x80 | (codePoint & 0x3f));
    return 4;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

uint32_t prUtf8Decode(PrStringView text, size_t offset, uint32_t* outCodePoint) {
    return decode<Form_Utf8>(text, offset, outCodePoint);
}

uint32_t prWtf8Decode(PrStringView text, size_t offset, uint32_t* outCodePoint) {
    return decode<Form_Wtf8>(text, offset, outCodePoint);
}

uint32_t prWtf8DecodeScalar(PrStringView text, size_t offset, uint32_t* outCodePoint) {
    uint32_t width = decode<Form_Wtf8>(text, offset, outCodePoint);
    if (width != 0 && *outCodePoint >= 0xd800 && *outCodePoint <= 0xdfff)
        *outCodePoint = kPrUtf8Replacement;
    return width;
}

size_t prUtf8Validate(PrStringView text) {
    return validateUtf8(text);
}

size_t prWtf8Validate(PrStringView text) {
    return validateWtf8(text);
}

uint32_t prUtf8Encode(uint32_t codePoint, char out[PR_NONNULL kPrUtf8MaxBytes]) {
    return encode(codePoint >= 0xd800 && codePoint <= 0xdfff ? kPrUtf8Replacement : codePoint, out);
}

uint32_t prWtf8Encode(uint32_t codePoint, char out[PR_NONNULL kPrUtf8MaxBytes]) {
    return encode(codePoint, out);
}

uint32_t prWtf8FromUtf16(const uint16_t* PR_NULLABLE units, uint32_t count, char* PR_NULLABLE out, uint32_t capacity) {
    // Each unit, or each surrogate pair, as its code point.
    auto each = [&](auto visit) {
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t unit = units[i];
            if (unit >= 0xd800 && unit <= 0xdbff && i + 1 < count && units[i + 1] >= 0xdc00 && units[i + 1] <= 0xdfff) {
                visit(0x10000 + ((unit - 0xd800) << 10) + (units[i + 1] - 0xdc00u));
                ++i;
            } else {
                visit(unit);
            }
        }
    };
    uint64_t size = 0;
    each([&](uint32_t codePoint) {
        size += codePoint < 0x80 ? 1 : codePoint < 0x800 ? 2 : codePoint < 0x10000 ? 3 : 4;
    });
    if (size >= kPrWtf8Invalid)
        return kPrWtf8Invalid;
    if (out != nullptr && capacity >= size) {
        uint32_t written = 0;
        each([&](uint32_t codePoint) { written += encode(codePoint, out + written); });
    }
    return uint32_t(size);
}

uint32_t prWtf8ToUtf16(PrStringView text, uint16_t* PR_NULLABLE out, uint32_t capacity) {
    if (prWtf8Validate(text) != text.size)
        return kPrWtf8Invalid;
    uint64_t count = 0;
    for (size_t at = 0; at < text.size;) {
        uint32_t codePoint = 0;
        at += prWtf8Decode(text, at, &codePoint);
        count += codePoint > 0xffff ? 2 : 1;
    }
    if (count >= kPrWtf8Invalid)
        return kPrWtf8Invalid;
    if (out != nullptr && capacity >= count) {
        uint32_t written = 0;
        for (size_t at = 0; at < text.size;) {
            uint32_t codePoint = 0;
            at += prWtf8Decode(text, at, &codePoint);
            if (codePoint > 0xffff) {
                out[written++] = uint16_t(0xd800 + ((codePoint - 0x10000) >> 10));
                out[written++] = uint16_t(0xdc00 + ((codePoint - 0x10000) & 0x3ff));
            } else {
                out[written++] = uint16_t(codePoint);
            }
        }
    }
    return uint32_t(count);
}
