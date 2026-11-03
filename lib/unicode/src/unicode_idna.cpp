// Unicode IDNA Compatibility Processing.
//
// The steps and criteria are those of UTS #46 section 4. Punycode is RFC 3492, the joiner rules are RFC 5892 Appendix
// A, and the bidi rules are RFC 5893 section 2.

#include "pr/unicode_idna.h"
#include "unicode/src/unicode_idna.h"

#include "pr/raw_array.h"
#include "pr/text_buffer.h"
#include "pr/unicode.h"
#include "pr/unicode_bidi.h"
#include "pr/utf8.h"

#include "unicode/src/unicode_normalization.h"

namespace pr_impl {
namespace {

// The parameters of Punycode (RFC 3492 section 5).
constexpr uint32_t kPunycodeBase = 36;
constexpr uint32_t kPunycodeTMin = 1;
constexpr uint32_t kPunycodeTMax = 26;
constexpr uint32_t kPunycodeSkew = 38;
constexpr uint32_t kPunycodeDamp = 700;
constexpr uint32_t kPunycodeInitialBias = 72;
constexpr uint32_t kPunycodeInitialN = 0x80;

constexpr uint32_t kZwnj = 0x200c;
constexpr uint32_t kZwj = 0x200d;

// The Canonical_Combining_Class of a virama.
constexpr uint8_t kViramaClass = 9;

// The DNS limits on a name, without its root label, and on a label, in bytes (RFC 1034 section 3.1).
constexpr uint32_t kDnsMaxNameBytes = 253;
constexpr uint32_t kDnsMaxLabelBytes = 63;

// A label of a domain name: a range of its code points.
struct IdnaLabel {
    uint32_t start;
    uint32_t count;
};

// Returns the labels of `domain`, separated by U+002E FULL STOP. A domain always has at least one label.
RawArray<IdnaLabel> labelsOf(const RawArray<uint32_t>& domain) {
    RawArray<IdnaLabel> labels{};
    uint32_t start = 0;
    for (uint32_t i = 0; i <= domain.count; ++i) {
        if (i == domain.count || domain.data[i] == '.') {
            labels.push({start, i - start});
            start = i + 1;
        }
    }
    return labels;
}

bool isAscii(const uint32_t* PR_NULLABLE codePoints, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) {
        if (codePoints[i] >= 0x80)
            return false;
    }
    return true;
}

bool startsWithAcePrefix(const uint32_t* PR_NULLABLE codePoints, uint32_t count) {
    return count >= 4 && codePoints[0] == 'x' && codePoints[1] == 'n' && codePoints[2] == '-' && codePoints[3] == '-';
}

// RFC 3492 section 6.1.
uint32_t adapt(uint32_t delta, uint32_t points, bool first) {
    delta = first ? delta / kPunycodeDamp : delta / 2;
    delta += delta / points;
    uint32_t k = 0;
    while (delta > (kPunycodeBase - kPunycodeTMin) * kPunycodeTMax / 2) {
        delta /= kPunycodeBase - kPunycodeTMin;
        k += kPunycodeBase;
    }
    return k + (kPunycodeBase - kPunycodeTMin + 1) * delta / (delta + kPunycodeSkew);
}

uint32_t threshold(uint32_t k, uint32_t bias) {
    return k <= bias ? kPunycodeTMin : k >= bias + kPunycodeTMax ? kPunycodeTMax : k - bias;
}

// Returns the value of a Punycode digit, or kPunycodeBase for a code point that is not one.
uint32_t digitValue(uint32_t codePoint) {
    if (codePoint >= 'a' && codePoint <= 'z')
        return codePoint - 'a';
    if (codePoint >= 'A' && codePoint <= 'Z')
        return codePoint - 'A';
    if (codePoint >= '0' && codePoint <= '9')
        return codePoint - '0' + 26;
    return kPunycodeBase;
}

char digitOf(uint32_t value) {
    return char(value < 26 ? 'a' + value : '0' + value - 26);
}

// Decodes the ASCII Punycode at `input` (RFC 3492 section 6.2) and appends the code points to `out`. Returns false if
// the input is not valid Punycode, or decodes to a value that is not a scalar value.
bool punycodeDecode(const uint32_t* PR_NULLABLE input, uint32_t count, RawArray<uint32_t>* out) {
    uint32_t delimiter = count;
    while (delimiter != 0 && input[delimiter - 1] != '-')
        --delimiter;
    uint32_t start = out->count;
    uint32_t at = 0;
    if (delimiter != 0) {
        out->append(input, delimiter - 1);
        at = delimiter;
    }
    uint32_t n = kPunycodeInitialN;
    uint32_t i = 0;
    uint32_t bias = kPunycodeInitialBias;
    while (at < count) {
        uint32_t old = i;
        uint32_t weight = 1;
        for (uint32_t k = kPunycodeBase;; k += kPunycodeBase) {
            if (at == count)
                return false;
            uint32_t digit = digitValue(input[at++]);
            if (digit == kPunycodeBase || digit > (UINT32_MAX - i) / weight)
                return false;
            i += digit * weight;
            uint32_t t = threshold(k, bias);
            if (digit < t)
                break;
            if (weight > UINT32_MAX / (kPunycodeBase - t))
                return false;
            weight *= kPunycodeBase - t;
        }
        uint32_t length = out->count - start + 1;
        bias = adapt(i - old, length, old == 0);
        if (i / length > UINT32_MAX - n)
            return false;
        n += i / length;
        i %= length;
        if (n > kPrUtf8MaxCodePoint || (n >= 0xd800 && n <= 0xdfff))
            return false;
        out->insert(start + i, n);
        ++i;
    }
    return true;
}

// Appends the Punycode of the code points at `input` to `out` (RFC 3492 section 6.3). Returns false on overflow.
bool punycodeEncode(const uint32_t* PR_NULLABLE input, uint32_t count, PrTextBuffer* out) {
    uint32_t basic = 0;
    for (uint32_t j = 0; j < count; ++j) {
        if (input[j] < 0x80) {
            prTextBufferAppendChar(out, char(input[j]));
            ++basic;
        }
    }
    if (basic != 0)
        prTextBufferAppendChar(out, '-');
    uint32_t n = kPunycodeInitialN;
    uint32_t delta = 0;
    uint32_t bias = kPunycodeInitialBias;
    for (uint32_t handled = basic; handled < count;) {
        uint32_t next = UINT32_MAX;
        for (uint32_t j = 0; j < count; ++j) {
            if (input[j] >= n && input[j] < next)
                next = input[j];
        }
        if (next - n > (UINT32_MAX - delta) / (handled + 1))
            return false;
        delta += (next - n) * (handled + 1);
        n = next;
        for (uint32_t j = 0; j < count; ++j) {
            if (input[j] < n && ++delta == 0)
                return false;
            if (input[j] != n)
                continue;
            uint32_t q = delta;
            for (uint32_t k = kPunycodeBase;; k += kPunycodeBase) {
                uint32_t t = threshold(k, bias);
                if (q < t)
                    break;
                prTextBufferAppendChar(out, digitOf(t + (q - t) % (kPunycodeBase - t)));
                q = (q - t) / (kPunycodeBase - t);
            }
            prTextBufferAppendChar(out, digitOf(q));
            bias = adapt(delta, handled + 1, handled == basic);
            delta = 0;
            ++handled;
        }
        ++delta;
        ++n;
    }
    return true;
}

// Returns true if the joiner at `at` satisfies its ContextJ rule (RFC 5892 Appendix A.1 and A.2).
bool joinerAllowed(const uint32_t* label, uint32_t count, uint32_t at) {
    if (at != 0 && prUnicodeCombiningClass(label[at - 1]) == kViramaClass)
        return true;
    if (label[at] == kZwj)
        return false;
    // A ZWNJ also stands between a left- or dual-joining character and a right- or dual-joining one, with only
    // transparent characters between it and each of them.
    uint32_t before = at;
    while (before != 0 && prUnicodeJoiningType(label[before - 1]) == PrUnicodeJoiningType_Transparent)
        --before;
    if (before == 0)
        return false;
    PrUnicodeJoiningType left = prUnicodeJoiningType(label[before - 1]);
    if (left != PrUnicodeJoiningType_Left && left != PrUnicodeJoiningType_Dual)
        return false;
    uint32_t after = at + 1;
    while (after < count && prUnicodeJoiningType(label[after]) == PrUnicodeJoiningType_Transparent)
        ++after;
    if (after == count)
        return false;
    PrUnicodeJoiningType right = prUnicodeJoiningType(label[after]);
    return right == PrUnicodeJoiningType_Right || right == PrUnicodeJoiningType_Dual;
}

// Returns true if the label meets the validity criteria of UTS #46 section 4.1, other than the bidi rules, which depend
// on the whole name.
bool isValidLabel(const uint32_t* PR_NULLABLE label, uint32_t count, PrUnicodeIdnaFlags flags) {
    if (count == 0)
        return true;
    if (!unicodeIsNormalized(label, count, PrUnicodeNormalForm_Nfc))
        return false;
    if (flags & PrUnicodeIdnaFlag_CheckHyphens) {
        if ((count >= 4 && label[2] == '-' && label[3] == '-') || label[0] == '-' || label[count - 1] == '-')
            return false;
    } else if (startsWithAcePrefix(label, count)) {
        return false;
    }
    if (prUnicodeFlags(label[0]) & PrUnicodeFlag_Mark)
        return false;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t codePoint = label[i];
        if (codePoint == '.')
            return false;
        IdnaStatus status = unicodeIdnaMapping(codePoint).status;
        if (status != IdnaStatus_Valid && (status != IdnaStatus_Deviation || (flags & PrUnicodeIdnaFlag_Transitional)))
            return false;
        if ((flags & PrUnicodeIdnaFlag_UseStd3AsciiRules) && codePoint < 0x80 && codePoint != '-' &&
                !(codePoint >= 'a' && codePoint <= 'z') && !(codePoint >= '0' && codePoint <= '9'))
            return false;
        if ((flags & PrUnicodeIdnaFlag_CheckJoiners) && (codePoint == kZwnj || codePoint == kZwj) &&
                !joinerAllowed(label, count, i))
            return false;
    }
    return true;
}

constexpr uint32_t bidiBit(PrUnicodeBidiClass c) {
    return 1u << c;
}

// The classes that a right-to-left label may hold, and that a left-to-right one may (RFC 5893 rules 2 and 5).
constexpr uint32_t kBidiNeutralClasses = bidiBit(PrUnicodeBidiClass_EN) | bidiBit(PrUnicodeBidiClass_ES) |
    bidiBit(PrUnicodeBidiClass_CS) | bidiBit(PrUnicodeBidiClass_ET) |
    bidiBit(PrUnicodeBidiClass_ON) | bidiBit(PrUnicodeBidiClass_BN) | bidiBit(PrUnicodeBidiClass_NSM);
constexpr uint32_t kBidiRtlClasses =
    kBidiNeutralClasses | bidiBit(PrUnicodeBidiClass_R) | bidiBit(PrUnicodeBidiClass_AL) |
    bidiBit(PrUnicodeBidiClass_AN);
constexpr uint32_t kBidiLtrClasses = kBidiNeutralClasses | bidiBit(PrUnicodeBidiClass_L);

// The classes that make a domain name a bidi domain name (RFC 5893 section 1.4).
constexpr uint32_t kBidiDomainClasses = bidiBit(PrUnicodeBidiClass_R) | bidiBit(PrUnicodeBidiClass_AL) |
    bidiBit(PrUnicodeBidiClass_AN);

// The classes that a right-to-left label may end with, and that a left-to-right one may, before any NSM (RFC 5893 rules
// 3 and 6).
constexpr uint32_t kBidiRtlEndClasses =
    bidiBit(PrUnicodeBidiClass_R) | bidiBit(PrUnicodeBidiClass_AL) | bidiBit(PrUnicodeBidiClass_EN) |
    bidiBit(PrUnicodeBidiClass_AN);
constexpr uint32_t kBidiLtrEndClasses = bidiBit(PrUnicodeBidiClass_L) | bidiBit(PrUnicodeBidiClass_EN);

// Returns true if the label satisfies the six rules of RFC 5893 section 2.
bool meetsBidiRules(const uint32_t* label, uint32_t count) {
    PrUnicodeBidiClass first = prUnicodeBidiClass(label[0]);
    bool rtl = first == PrUnicodeBidiClass_R || first == PrUnicodeBidiClass_AL;
    if (!rtl && first != PrUnicodeBidiClass_L)
        return false;                                               // 1
    uint32_t allowed = rtl ? kBidiRtlClasses : kBidiLtrClasses;
    uint32_t seen = 0;
    PrUnicodeBidiClass last = first;
    for (uint32_t i = 0; i < count; ++i) {
        PrUnicodeBidiClass c = prUnicodeBidiClass(label[i]);
        if ((allowed & bidiBit(c)) == 0)
            return false;                                           // 2, 5
        seen |= bidiBit(c);
        if (c != PrUnicodeBidiClass_NSM)
            last = c;
    }
    if (!rtl)
        return (kBidiLtrEndClasses & bidiBit(last)) != 0;           // 6
    bool mixedNumbers = (seen & bidiBit(PrUnicodeBidiClass_EN)) != 0 && (seen & bidiBit(PrUnicodeBidiClass_AN)) != 0;
    return (kBidiRtlEndClasses & bidiBit(last)) != 0 && !mixedNumbers;  // 3, 4
}

// Applies the processing steps of UTS #46 section 4 to `domain`, and writes the result to `*out`. Returns false if
// processing records an error.
bool process(PrStringView domain, PrUnicodeIdnaFlags flags, RawArray<uint32_t>* out) {
    bool valid = true;
    RawArray<uint32_t> mapped{};
    for (size_t at = 0; at < domain.size;) {
        uint32_t codePoint;
        at += prUtf8Decode(domain, at, &codePoint);
        UnicodeIdnaMapping mapping = unicodeIdnaMapping(codePoint);
        bool transitional = (flags & PrUnicodeIdnaFlag_Transitional) != 0;
        switch (mapping.status) {
            case IdnaStatus_Ignored:
                break;
            case IdnaStatus_Mapped:
                if (transitional && codePoint == 0x1e9e) {
                    mapped.push('s');
                    mapped.push('s');
                } else {
                    mapped.append(mapping.codePoints, mapping.count);
                }
                break;
            case IdnaStatus_Deviation:
                if (transitional)
                    mapped.append(mapping.codePoints, mapping.count);
                else
                    mapped.push(codePoint);
                break;
            default:
                mapped.push(codePoint);
                break;
        }
    }
    unicodeNormalize(&mapped, PrUnicodeNormalForm_Nfc);

    RawArray<IdnaLabel> labels = labelsOf(mapped);
    for (uint32_t l = 0; l < labels.count; ++l) {
        const uint32_t* label = mapped.data + labels.data[l].start;
        uint32_t count = labels.data[l].count;
        if (l != 0)
            out->push('.');
        if (!startsWithAcePrefix(label, count)) {
            valid &= isValidLabel(label, count, flags);
            out->append(label, count);
            continue;
        }
        uint32_t start = out->count;
        bool ascii = isAscii(label, count);
        if (!ascii || !punycodeDecode(label + 4, count - 4, out)) {
            out->resize(start);
            out->append(label, count);
            if (!ascii || (flags & PrUnicodeIdnaFlag_IgnoreInvalidPunycode) == 0)
                valid = false;
            continue;
        }
        const uint32_t* decoded = out->data + start;
        uint32_t decodedCount = out->count - start;
        if (decodedCount == 0 || isAscii(decoded, decodedCount))
            valid = false;
        valid &= isValidLabel(decoded, decodedCount, PrUnicodeIdnaFlags(flags & ~PrUnicodeIdnaFlag_Transitional));
    }
    labels.reset();
    mapped.reset();

    if ((flags & PrUnicodeIdnaFlag_CheckBidi) == 0)
        return valid;
    bool bidiDomain = false;
    for (uint32_t i = 0; i < out->count && !bidiDomain; ++i)
        bidiDomain = (kBidiDomainClasses & bidiBit(prUnicodeBidiClass(out->data[i]))) != 0;
    if (!bidiDomain)
        return valid;
    RawArray<IdnaLabel> processed = labelsOf(*out);
    for (uint32_t l = 0; l < processed.count; ++l) {
        if (processed.data[l].count != 0)
            valid &= meetsBidiRules(out->data + processed.data[l].start, processed.data[l].count);
    }
    processed.reset();
    return valid;
}

} // namespace
} // namespace pr_impl

using namespace pr_impl;

bool prUnicodeIdnaToAscii(PrStringView domain, PrUnicodeIdnaFlags flags, PrRawString* out) {
    *out = PrRawString{};
    RawArray<uint32_t> processed{};
    bool valid = process(domain, flags, &processed);
    RawArray<IdnaLabel> labels = labelsOf(processed);
    PrTextBuffer text = {};
    for (uint32_t l = 0; l < labels.count && valid; ++l) {
        const uint32_t* label = processed.data + labels.data[l].start;
        uint32_t count = labels.data[l].count;
        if (l != 0)
            prTextBufferAppendChar(&text, '.');
        uint32_t start = text.size;
        if (isAscii(label, count)) {
            for (uint32_t i = 0; i < count; ++i)
                prTextBufferAppendChar(&text, char(label[i]));
        } else {
            prTextBufferAppend(&text, "xn--");
            valid = punycodeEncode(label, count, &text);
        }
        uint32_t bytes = text.size - start;
        if ((flags & PrUnicodeIdnaFlag_VerifyDnsLength) && (bytes == 0 || bytes > kDnsMaxLabelBytes))
            valid = false;
    }
    if ((flags & PrUnicodeIdnaFlag_VerifyDnsLength) && text.size > kDnsMaxNameBytes)
        valid = false;
    labels.reset();
    processed.reset();
    if (valid)
        *out = prTextBufferTake(&text);
    else
        prTextBufferReset(&text);
    return valid;
}

bool prUnicodeIdnaToUnicode(PrStringView domain, PrUnicodeIdnaFlags flags, PrRawString* out) {
    RawArray<uint32_t> processed{};
    bool valid = process(domain, PrUnicodeIdnaFlags(flags & ~PrUnicodeIdnaFlag_VerifyDnsLength), &processed);
    PrTextBuffer text = {};
    for (uint32_t i = 0; i < processed.count; ++i)
        prTextBufferAppendCodePoint(&text, processed.data[i]);
    processed.reset();
    *out = prTextBufferTake(&text);
    return valid;
}
