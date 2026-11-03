// Unicode normalization.
//
// Decomposition, canonical ordering and canonical composition are those of The Unicode Standard, section 3.11, and the
// Hangul syllables decompose and compose by the algorithm of section 3.12.

#include "pr/unicode_normalization.h"
#include "unicode/src/unicode_normalization.h"

#include "pr/text_buffer.h"
#include "pr/unicode.h"
#include "pr/utf8.h"

namespace pr_impl {
namespace {

// The Hangul syllables, each a leading consonant, a vowel and an optional trailing consonant.
constexpr uint32_t kSyllableFirst = 0xac00;
constexpr uint32_t kLeadFirst = 0x1100;
constexpr uint32_t kVowelFirst = 0x1161;
constexpr uint32_t kTrailBase = 0x11a7;     // one before the first trailing consonant
constexpr uint32_t kLeadCount = 19;
constexpr uint32_t kVowelCount = 21;
constexpr uint32_t kTrailCount = 28;        // the trailing consonants, and their absence
constexpr uint32_t kSyllablesPerLead = kVowelCount * kTrailCount;
constexpr uint32_t kSyllableCount = kLeadCount * kSyllablesPerLead;

// Code points below this one neither decompose nor follow anything that they compose with, so text made only of them is
// in every form.
constexpr uint32_t kFirstChanging = 0xc0;

// The absent index of a starter.
constexpr uint32_t kStarterNone = UINT32_MAX;

bool isUnchanging(const uint32_t* PR_NULLABLE codePoints, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) {
        if (codePoints[i] >= kFirstChanging)
            return false;
    }
    return true;
}

void appendDecomposition(Array<uint32_t>* out, uint32_t codePoint) {
    if (codePoint - kSyllableFirst < kSyllableCount) {
        uint32_t index = codePoint - kSyllableFirst;
        out->push(kLeadFirst + index / kSyllablesPerLead);
        out->push(kVowelFirst + index % kSyllablesPerLead / kTrailCount);
        if (index % kTrailCount != 0)
            out->push(kTrailBase + index % kTrailCount);
        return;
    }
    const uint32_t* parts = nullptr;
    uint32_t count = unicodeDecomposition(codePoint, &parts);
    if (count == 0)
        out->push(codePoint);
    else
        out->append(parts, count);
}

// Sorts each run of non-starters by combining class, keeping the order of code points of the same class.
void reorder(Array<uint32_t>* text) {
    for (uint32_t i = 1; i < text->count; ++i) {
        uint32_t codePoint = text->data[i];
        uint8_t combining = prUnicodeCombiningClass(codePoint);
        if (combining == 0)
            continue;
        uint32_t at = i;
        while (at != 0 && prUnicodeCombiningClass(text->data[at - 1]) > combining) {
            text->data[at] = text->data[at - 1];
            --at;
        }
        text->data[at] = codePoint;
    }
}

bool composes(uint32_t first, uint32_t second, uint32_t* composite) {
    if (first - kLeadFirst < kLeadCount && second - kVowelFirst < kVowelCount) {
        *composite = kSyllableFirst + ((first - kLeadFirst) * kVowelCount + second - kVowelFirst) * kTrailCount;
        return true;
    }
    if (first - kSyllableFirst < kSyllableCount && (first - kSyllableFirst) % kTrailCount == 0 &&
            second - kTrailBase - 1 < kTrailCount - 1) {
        *composite = first + second - kTrailBase;
        return true;
    }
    return unicodeComposition(first, second, composite);
}

// Composes each code point that is not blocked from the last starter with it, in canonically ordered text. A code point
// is blocked if a code point between them has a combining class of zero or of at least its own. In ordered text, the
// last code point kept carries the highest class between them.
void compose(Array<uint32_t>* text) {
    uint32_t starter = kStarterNone;
    uint8_t lastCombining = 0;
    uint32_t kept = 0;
    for (uint32_t i = 0; i < text->count; ++i) {
        uint32_t codePoint = text->data[i];
        uint8_t combining = prUnicodeCombiningClass(codePoint);
        uint32_t composite;
        if (starter != kStarterNone && (lastCombining == 0 || lastCombining < combining) &&
                composes(text->data[starter], codePoint, &composite)) {
            text->data[starter] = composite;
            continue;
        }
        if (combining == 0)
            starter = kept;
        lastCombining = combining;
        text->data[kept++] = codePoint;
    }
    text->count = kept;
}

void appendDecoded(Array<uint32_t>* out, PrStringView text) {
    for (size_t at = 0; at < text.size;) {
        uint32_t codePoint;
        at += prUtf8Decode(text, at, &codePoint);
        out->push(codePoint);
    }
}

} // namespace

void unicodeNormalize(Array<uint32_t>* codePoints, PrUnicodeNormalForm form) {
    if (isUnchanging(codePoints->data, codePoints->count))
        return;
    Array<uint32_t> normalized{};
    for (uint32_t i = 0; i < codePoints->count; ++i)
        appendDecomposition(&normalized, codePoints->data[i]);
    reorder(&normalized);
    if (form == PrUnicodeNormalForm_Nfc)
        compose(&normalized);
    codePoints->reset();
    *codePoints = normalized;
}

bool unicodeIsNormalized(const uint32_t* PR_NULLABLE codePoints, uint32_t count, PrUnicodeNormalForm form) {
    if (isUnchanging(codePoints, count))
        return true;
    Array<uint32_t> normalized{};
    normalized.append(codePoints, count);
    unicodeNormalize(&normalized, form);
    bool same = normalized.count == count;
    for (uint32_t i = 0; same && i < count; ++i)
        same = normalized.data[i] == codePoints[i];
    normalized.reset();
    return same;
}

} // namespace pr_impl

using namespace pr_impl;

PrString prUnicodeNormalize(PrStringView text, PrUnicodeNormalForm form) {
    Array<uint32_t> codePoints{};
    appendDecoded(&codePoints, text);
    unicodeNormalize(&codePoints, form);
    PrTextBuffer out = {};
    for (uint32_t i = 0; i < codePoints.count; ++i)
        prTextBufferAppendCodePoint(&out, codePoints.data[i]);
    codePoints.reset();
    return prTextBufferTake(&out);
}

bool prUnicodeIsNormalized(PrStringView text, PrUnicodeNormalForm form) {
    if (prUtf8Validate(text) != text.size)
        return false;
    Array<uint32_t> codePoints{};
    appendDecoded(&codePoints, text);
    bool normalized = unicodeIsNormalized(codePoints.data, codePoints.count, form);
    codePoints.reset();
    return normalized;
}
