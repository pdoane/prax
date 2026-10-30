#include "pr/utf8.h"

#include "pr/mem.h"

namespace pr_impl {

namespace {

// The sequence that a lead byte starts. The second byte's range excludes the overlong encodings and the values above
// U+10FFFF. If surrogates are not code points, it also excludes the encodings of U+D800 to U+DFFF. Every later byte is
// a continuation byte.
struct Lead {
    uint8_t length;     // zero for a byte that opens no sequence
    uint8_t low;        // the lowest second byte
    uint8_t high;       // the highest second byte
};

struct LeadTable {
    Lead of[256];
};

constexpr LeadTable makeLeads(bool surrogates) {
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
    table.of[0xed].high = surrogates ? 0xbf : 0x9f;
    table.of[0xf0].low = 0x90;
    table.of[0xf4].high = 0x8f;
    return table;
}

constexpr bool isContinuation(uint32_t c) {
    return (c & 0xc0) == 0x80;
}

constexpr LeadTable kLeads = makeLeads(true);
constexpr LeadTable kScalarLeads = makeLeads(false);

// Returns the length of the well-formed sequence at `offset`, or zero if the sequence is ill formed.
uint32_t sequenceAt(const LeadTable& leads, const uint8_t* bytes, size_t size, size_t offset) {
    Lead lead = leads.of[bytes[offset]];
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

// Returns the length of the maximal subpart at `offset`, for a sequence that is ill formed. The maximal subpart is the
// longest run of bytes that begins a well-formed sequence, or one byte if that byte starts none. One U+FFFD replaces
// it.
uint32_t subpartAt(const LeadTable& leads, const uint8_t* bytes, size_t size, size_t offset) {
    Lead lead = leads.of[bytes[offset]];
    if (lead.length == 0 || offset + 1 >= size)
        return 1;
    uint8_t second = bytes[offset + 1];
    if (second < lead.low || second > lead.high)
        return 1;
    if (lead.length == 2 || offset + 2 >= size || !isContinuation(bytes[offset + 2]))
        return 2;
    return 3;
}

// `surrogates` says whether the three bytes that encode a surrogate decode as that code point or are ill formed. That
// is the only difference between the two entry points.
template <bool surrogates>
uint32_t decode(PrStringView text, size_t offset, uint32_t* outCodePoint) {
    if (offset >= text.size)
        return 0;

    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(text.data);
    if (bytes[offset] < 0x80) {
        *outCodePoint = bytes[offset];
        return 1;
    }
    const LeadTable& leads = surrogates ? kLeads : kScalarLeads;
    uint32_t length = sequenceAt(leads, bytes, text.size, offset);
    if (length == 0) {
        *outCodePoint = kPrUtf8Replacement;
        return subpartAt(leads, bytes, text.size, offset);
    }

    // The lead byte's payload is the bits below its length marker.
    const uint8_t* p = bytes + offset;
    switch (length) {
        case 2:
            *outCodePoint = (uint32_t(p[0] & 0x1f) << 6) | (p[1] & 0x3fu);
            break;
        case 3:
            *outCodePoint = (uint32_t(p[0] & 0x0f) << 12) | (uint32_t(p[1] & 0x3f) << 6) | (p[2] & 0x3fu);
            break;
        default:
            *outCodePoint = (uint32_t(p[0] & 0x07) << 18) | (uint32_t(p[1] & 0x3f) << 12) |
                (uint32_t(p[2] & 0x3f) << 6) | (p[3] & 0x3fu);
            break;
    }
    return length;
}

// A validating automaton whose state is a shift amount. The row for a byte holds six bits for each state, giving the
// next state after that byte. A surrogate is a code point, which matches kLeads.
enum : uint64_t {
    DfaState_Accept = 0 * 6,   // between sequences
    DfaState_One = 1 * 6,      // one continuation byte left
    DfaState_Two = 2 * 6,      // two left
    DfaState_Three = 3 * 6,    // three left
    DfaState_E0 = 4 * 6,       // after 0xe0, whose second byte is 0xa0 to 0xbf
    DfaState_F0 = 5 * 6,       // after 0xf0, whose second byte is 0x90 to 0xbf
    DfaState_F4 = 6 * 6,       // after 0xf4, whose second byte is 0x80 to 0x8f
    DfaState_Error = 7 * 6,    // an ill-formed sequence has been read
    DfaState_Count = 8,
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
        case DfaState_F0:    return byte >= 0x90 && byte <= 0xbf ? DfaState_Two : DfaState_Error;
        case DfaState_F4:    return byte >= 0x80 && byte <= 0x8f ? DfaState_Two : DfaState_Error;
        default:        return DfaState_Error;
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

DfaState dfaStep(DfaState state, uint8_t byte) {
    return (kDfa.row[byte] >> state) & 63;
}

// Returns the high bits of sixteen bytes, which are all zero if the bytes are all ASCII.
uint64_t highBits(const uint8_t* bytes) {
    uint64_t low;
    uint64_t high;
    memCopy(&low, bytes, 8);
    memCopy(&high, bytes + 8, 8);
    return (low | high) & 0x8080808080808080ull;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

uint32_t prUtf8Decode(PrStringView text, size_t offset, uint32_t* outCodePoint) {
    return decode<true>(text, offset, outCodePoint);
}

uint32_t prUtf8DecodeScalar(PrStringView text, size_t offset, uint32_t* outCodePoint) {
    uint32_t width = decode<false>(text, offset, outCodePoint);
    if (width != 0 && *outCodePoint == 0)
        *outCodePoint = kPrUtf8Replacement;
    return width;
}

size_t prUtf8Validate(PrStringView text) {
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
                state = dfaStep(state, block[k]);
        }
        if (state == DfaState_Error)
            break;
    }
    if (state == DfaState_Accept)
        boundary = at;
    for (; at < size && state != DfaState_Error; ++at)
        state = dfaStep(state, bytes[at]);
    if (state == DfaState_Accept)
        return size;

    // No ill-formed sequence begins before `boundary`. Walking the sequences from there finds the first.
    while (boundary < size) {
        uint32_t length = sequenceAt(kLeads, bytes, size, boundary);
        if (length == 0)
            break;
        boundary += length;
    }
    return boundary;
}

uint32_t prUtf8Encode(uint32_t codePoint, char out[PR_NONNULL kPrUtf8MaxBytes]) {
    if ((codePoint >= 0xd800 && codePoint <= 0xdfff) || codePoint > kPrUtf8MaxCodePoint)
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
