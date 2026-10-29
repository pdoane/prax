// Text transforms: text set in another case, width or form for styling, with the content itself unchanged.

#include "pr/text_transform.h"

#include "pr/assert.h"
#include "pr/utf8.h"

namespace pr_impl {

namespace {

// Returns true if `codePoint` is a letter.
bool isLetter(uint32_t codePoint) {
    PrUnicodeGeneralCategory category = prUnicodeGeneralCategory(codePoint);
    return category >= PrUnicodeGeneralCategory_UppercaseLetter && category <= PrUnicodeGeneralCategory_OtherLetter;
}

// Returns true if `codePoint` continues a word that the code point before it is in: a letter, a number, or connector
// punctuation such as a low line.
bool isWordPart(uint32_t codePoint) {
    PrUnicodeGeneralCategory category = prUnicodeGeneralCategory(codePoint);
    return isLetter(codePoint)
           || (category >= PrUnicodeGeneralCategory_DecimalNumber && category <= PrUnicodeGeneralCategory_OtherNumber)
           || category == PrUnicodeGeneralCategory_ConnectorPunctuation;
}

// Returns true if `codePoint` is passed over in finding the code point before another: a mark or a format character,
// which belongs to the character before it.
bool isPassedOver(uint32_t codePoint) {
    return (prUnicodeFlags(codePoint) & PrUnicodeFlag_Mark) != 0
           || prUnicodeGeneralCategory(codePoint) == PrUnicodeGeneralCategory_Format;
}

// Returns true if `codePoint` is punctuation that stays inside a word between two letters, as an apostrophe, a period,
// a colon or a middle dot does (UAX #29's MidLetter, MidNumLet and Single_Quote, which are case-ignorable).
bool isMidWord(uint32_t codePoint) {
    PrUnicodeGeneralCategory category = prUnicodeGeneralCategory(codePoint);
    return (prUnicodeFlags(codePoint) & PrUnicodeFlag_CaseIgnorable) != 0
           && category >= PrUnicodeGeneralCategory_ConnectorPunctuation
           && category <= PrUnicodeGeneralCategory_OtherPunctuation;
}

// Finds the code point before byte `*end` of `text`, past marks and format characters, writes it to `*codePoint`, and
// moves `*end` to its start. Returns false if there is none.
bool previous(PrStringView text, size_t* end, uint32_t* codePoint) {
    while (*end > 0) {
        size_t start = *end - 1;
        while (start > 0 && *end - start < kPrUtf8MaxBytes && (uint8_t(text.data[start]) & 0xc0) == 0x80)
            --start;
        prUtf8Decode(text, start, codePoint);
        *end = start;
        if (!isPassedOver(*codePoint))
            return true;
    }
    return false;
}

// Returns true if the letter at byte `at` of `text` begins a word.
bool beginsWord(PrStringView text, size_t at) {
    uint32_t before = 0;
    if (!previous(text, &at, &before))
        return true;
    if (isWordPart(before))
        return false;
    uint32_t letter = 0;
    return !isMidWord(before) || !previous(text, &at, &letter) || !isLetter(letter);
}

// Returns true if the run's text, besides white space, is the one code point at byte `at`.
bool isSingle(const PrTextTransformRun* run, size_t at) {
    for (size_t i = run->start; i < run->end;) {
        uint32_t codePoint = 0;
        uint32_t length = prUtf8Decode(run->text, i, &codePoint);
        if (i != at && codePoint != ' ' && codePoint != '\t' && codePoint != '\n')
            return false;
        i += length;
    }
    return true;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

uint32_t prTextTransformAt(const PrTextTransformRun* run, size_t at, uint32_t out[kPrTextTransformMaxCodePoints]) {
    PR_ASSERT(at >= run->start && at < run->end);
    uint32_t codePoint = 0;
    prUtf8Decode(run->text, at, &codePoint);
    out[0] = codePoint;
    uint32_t count = 1;
    PrLanguage language = prLocaleLanguage(run->locale);
    switch (run->transform & kPrTextTransformValueMask) {
        case PrTextTransform_Capitalize:
            if (isLetter(codePoint) && (prUnicodeFlags(codePoint) & PrUnicodeFlag_Lowercase) != 0
                && beginsWord(run->text, at))
                count = prUnicodeMapCase(run->text, at, PrUnicodeCase_Title, language, out);
            break;
        case PrTextTransform_Uppercase:
            count = prUnicodeMapCase(run->text, at, PrUnicodeCase_Upper, language, out);
            break;
        case PrTextTransform_Lowercase:
            count = prUnicodeMapCase(run->text, at, PrUnicodeCase_Lower, language, out);
            break;
        case PrTextTransform_MathAuto:
            if (isSingle(run, at))
                out[0] = prUnicodeMathItalic(codePoint);
            break;
        default: break;
    }
    for (uint32_t i = 0; i < count; ++i) {
        if ((run->transform & PrTextTransformFlag_FullWidth) != 0)
            out[i] = prUnicodeFullWidth(out[i]);
        if ((run->transform & PrTextTransformFlag_FullSizeKana) != 0)
            out[i] = prUnicodeFullSizeKana(out[i]);
    }
    return count;
}
