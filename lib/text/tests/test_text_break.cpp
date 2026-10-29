// Where CSS divides text: word-break, overflow-wrap and the first-letter text.

#include "pr/text_break.h"
#include "pr/utf8.h"

#include "doctest/doctest.h"

#include <cstring>
#include <string>
#include <vector>

namespace pr_impl {

namespace {

// Returns the first-letter text of `text`, or "none".
std::string firstLetter(const char* text) {
    PrStringView view{text, std::strlen(text)};
    size_t start = 0;
    size_t end = 0;
    if (!prTextFirstLetter(view, &start, &end))
        return "none";
    return std::string(text + start, end - start);
}

// The opportunities of `text` in `style`, as one character per code point, which marks the opportunity after it: '|' if
// a line may break, '!' if it must, '~' for an emergency break that counts when the narrowest width is measured, '-'
// for one that does not, and '.' for none.
std::string breaksOf(const char* text, PrTextBreakStyle style) {
    PrStringView view{text, std::strlen(text)};
    std::vector<uint32_t> codePoints;
    std::vector<uint32_t> ends;
    for (size_t at = 0; at < view.size;) {
        uint32_t codePoint = 0;
        at += prUtf8Decode(view, at, &codePoint);
        codePoints.push_back(codePoint);
        ends.push_back(uint32_t(at));
    }
    uint32_t count = uint32_t(codePoints.size());
    std::vector<PrUnicodeLineBreak> breaks(count);
    std::vector<PrTextEmergency> emergency(count);
    std::vector<PrTextBreakStyle> styles(count, style);
    prUnicodeLineBreaks(codePoints.data(), count, breaks.data());
    PrTextBreakDesc desc{view, codePoints.data(), ends.data(), styles.data(), count};
    prTextBreaksTailor(&desc, breaks.data(), emergency.data());
    std::string out;
    for (uint32_t i = 0; i < count; ++i) {
        if (breaks[i] == PrUnicodeLineBreak_Mandatory)
            out += '!';
        else if (breaks[i] == PrUnicodeLineBreak_Allowed)
            out += '|';
        else if (emergency[i] == PrTextEmergency_Anywhere)
            out += '~';
        else if (emergency[i] == PrTextEmergency_BreakWord)
            out += '-';
        else
            out += '.';
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
                   PrTextBreakStyle{PrTextWordBreak_KeepAll, PrTextOverflowWrap_Normal})
          == "..|!");
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
