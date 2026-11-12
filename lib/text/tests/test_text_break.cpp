// Where CSS divides text: word-break, overflow-wrap and the first-letter text.

#include "pr/array.h"
#include "pr/string.h"
#include "pr/text_break.h"
#include "pr/utf8.h"

#include "tools/harness/test.h"

namespace pr_impl {

using namespace pr;

namespace {

// Returns the first-letter text of `text`, or "none".
String firstLetter(const char* text) {
    PrStringView view(text);
    size_t start = 0;
    size_t end = 0;
    if (!prTextFirstLetter(view, &start, &end))
        return "none";
    return String(PrStringView(text + start, end - start));
}

// The opportunities of `text` in `style`, as one character per code point, which marks the opportunity after it: '|' if
// a line may break, '!' if it must, '~' for an emergency break that counts when the narrowest width is measured, '-'
// for one that does not, and '.' for none.
String breaksOf(const char* text, PrTextBreakStyle style) {
    PrStringView view(text);
    Array<uint32_t> codePoints;
    Array<uint32_t> ends;
    for (size_t at = 0; at < view.size;) {
        uint32_t codePoint = 0;
        at += prUtf8Decode(view, at, &codePoint);
        codePoints.push(codePoint);
        ends.push(uint32_t(at));
    }
    uint32_t count = codePoints.count();
    Array<PrUnicodeLineBreak> breaks(count);
    Array<PrTextEmergency> emergency(count);
    Array<PrTextBreakStyle> styles(count);
    for (PrTextBreakStyle& each : styles)
        each = style;
    prUnicodeLineBreaks(codePoints.data(), count, breaks.data());
    PrTextBreakDesc desc{view, codePoints.data(), ends.data(), styles.data(), count};
    prTextBreaksTailor(&desc, breaks.data(), emergency.data());
    String out;
    for (uint32_t i = 0; i < count; ++i) {
        if (breaks[i] == PrUnicodeLineBreak_Mandatory)
            out.append('!');
        else if (breaks[i] == PrUnicodeLineBreak_Allowed)
            out.append('|');
        else if (emergency[i] == PrTextEmergency_Anywhere)
            out.append('~');
        else if (emergency[i] == PrTextEmergency_BreakWord)
            out.append('-');
        else
            out.append('.');
    }
    return out;
}

constexpr PrTextBreakStyle kNormal{PrTextWordBreak_Normal, PrTextOverflowWrap_Normal};

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("the first-letter text is the first letter, number or symbol with the punctuation around it") {
    CHECK(firstLetter("first") == "f");
    CHECK(firstLetter("67 million") == "6");
    CHECK(firstLetter("$5") == "$");
    CHECK(firstLetter("  leading spaces") == "l");
    CHECK(firstLetter("\n\t collapsed white space") == "c");
    // Punctuation before the letter, with the spaces after it, and punctuation after it that does not open.
    CHECK(firstLetter("\"(A) quote") == "\"(A)");
    CHECK(firstLetter("\xe2\x80\x9cThe\xe2\x80\x9d") == "\xe2\x80\x9cT");
    CHECK(firstLetter("A. B") == "A.");
    CHECK(firstLetter("A-B") == "A");
    // A word separator ends it, and a thin space does not.
    CHECK(firstLetter("A .") == "A");
    CHECK(firstLetter("A\xe2\x80\x89!") == "A\xe2\x80\x89!");
    // A combining mark is part of the letter's cluster.
    CHECK(firstLetter("e\xcc\x81t\xc3\xa9") == "e\xcc\x81");
    CHECK(firstLetter("") == "none");
    CHECK(firstLetter("!!") == "none");
    CHECK(firstLetter("\xe3\x80\x80" "A") == "none");
}

TEST_CASE("word-break allows a break between any two letters, or none between East Asian ones") {
    CHECK(breaksOf("ab cd", kNormal) == "..|.!");
    CHECK(breaksOf("ab cd", PrTextBreakStyle{PrTextWordBreak_BreakAll, PrTextOverflowWrap_Normal}) == "|.||!");
    // keep-all keeps the ideographs together, and the space still breaks.
    CHECK(breaksOf("\xe6\x97\xa5\xe6\x9c\xac \xe8\xaa\x9e", kNormal) == "|.|!");
    CHECK(breaksOf("\xe6\x97\xa5\xe6\x9c\xac \xe8\xaa\x9e",
        PrTextBreakStyle{PrTextWordBreak_KeepAll, PrTextOverflowWrap_Normal}) == "..|!");
    // A combining mark stays with its letter.
    CHECK(breaksOf("e\xcc\x81" "e", PrTextBreakStyle{PrTextWordBreak_BreakAll, PrTextOverflowWrap_Normal}) == ".|!");
}

TEST_CASE("overflow-wrap adds an opportunity between the grapheme clusters of a word, and before the space after it") {
    CHECK(breaksOf("abc d", PrTextBreakStyle{PrTextWordBreak_Normal, PrTextOverflowWrap_Anywhere}) == "~~~|!");
    CHECK(breaksOf("abc d", PrTextBreakStyle{PrTextWordBreak_Normal, PrTextOverflowWrap_BreakWord}) == "---|!");
    // word-break: break-word is overflow-wrap: anywhere.
    CHECK(breaksOf("abc", PrTextBreakStyle{PrTextWordBreak_BreakWord, PrTextOverflowWrap_Normal}) == "~~!");
    CHECK(breaksOf("e\xcc\x81" "e", PrTextBreakStyle{PrTextWordBreak_Normal, PrTextOverflowWrap_Anywhere}) == ".~!");
}
