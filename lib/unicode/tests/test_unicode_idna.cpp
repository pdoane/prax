#include "pr/unicode_idna.h"
#include "pr/utf8.h"

#include "doctest/doctest.h"

#include <fstream>
#include <string>
#include <vector>

namespace pr_impl {

namespace {

std::string trimmed(const std::string& text) {
    size_t first = text.find_first_not_of(" \t");
    size_t last = text.find_last_not_of(" \t");
    return first == std::string::npos ? "" : text.substr(first, last - first + 1);
}

// Replaces the \uXXXX and \x{XXXX} escapes of the test file with the code points they name.
std::string unescaped(const std::string& text) {
    std::string out;
    for (size_t i = 0; i < text.size(); ++i) {
        size_t digits = 0;
        size_t end = 0;
        if (text.compare(i, 2, "\\u") == 0) {
            digits = i + 2;
            end = i + 6;
        } else if (text.compare(i, 3, "\\x{") == 0) {
            digits = i + 3;
            end = text.find('}', i);
        }
        if (digits == 0) {
            out += text[i];
            continue;
        }
        char encoded[kPrUtf8MaxBytes];
        uint32_t codePoint = uint32_t(std::stoul(text.substr(digits, end - digits), nullptr, 16));
        out.append(encoded, prUtf8Encode(codePoint, encoded));
        i = text[end] == '}' ? end : end - 1;
    }
    return out;
}

// A column's value: the empty string for "", or `fallback` for a blank column.
std::string value(const std::string& column, const std::string& fallback) {
    std::string text = trimmed(column);
    if (text.empty())
        return fallback;
    return text == "\"\"" ? "" : unescaped(text);
}

// Returns true if a status column records an error, other than the codes in `ignored`. A blank column is `fallback`.
bool hasError(const std::string& column, bool fallback, const std::vector<std::string>& ignored) {
    std::string text = trimmed(column);
    if (text.empty())
        return fallback;
    std::string codes = text.substr(1, text.size() - 2);
    size_t at = 0;
    while (at < codes.size()) {
        size_t end = codes.find(',', at);
        std::string code = trimmed(codes.substr(at, end == std::string::npos ? std::string::npos : end - at));
        bool skip = code.empty();
        for (const std::string& name : ignored)
            skip |= code == name;
        if (!skip)
            return true;
        at = end == std::string::npos ? codes.size() : end + 1;
    }
    return false;
}

std::string taken(PrRawString* text) {
    PrStringView view = prRawStringView(text);
    std::string out(view.data != nullptr ? view.data : "", view.size);
    prRawStringReset(text);
    return out;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("conversions are as every case of IdnaTestV2.txt says") {
    std::string path = std::string(PR_UNICODE_DIR) + "/IdnaTestV2.txt";
    std::ifstream file(path);
    REQUIRE(file.good());
    uint32_t cases = 0;
    uint32_t failures = 0;
    std::string line;
    while (std::getline(file, line)) {
        std::string data = line.substr(0, line.find('#'));
        if (data.find_first_not_of(" \t\r\n") == std::string::npos)
            continue;
        std::vector<std::string> columns;
        size_t at = 0;
        for (size_t end; (end = data.find(';', at)) != std::string::npos; at = end + 1)
            columns.push_back(data.substr(at, end - at));
        columns.push_back(data.substr(at));
        REQUIRE(columns.size() >= 7);
        std::string source = value(columns[0], "");
        std::string toUnicode = value(columns[1], source);
        // X4_2 marks an empty label, which only VerifyDnsLength rejects.
        bool unicodeError = hasError(columns[2], false, {"X4_2"});
        std::string toAsciiN = value(columns[3], toUnicode);
        bool asciiNError = hasError(columns[4], hasError(columns[2], false, {}), {});
        std::string toAsciiT = value(columns[5], toAsciiN);
        bool asciiTError = hasError(columns[6], asciiNError, {});

        PrRawString out{};
        bool unicodeValid = prUnicodeIdnaToUnicode(source, PrUnicodeIdnaFlag_Strict, &out);
        std::string unicode = taken(&out);
        bool asciiNValid = prUnicodeIdnaToAscii(source, PrUnicodeIdnaFlag_Strict, &out);
        std::string asciiN = taken(&out);
        bool asciiTValid = prUnicodeIdnaToAscii(source, PrUnicodeIdnaFlag_Strict | PrUnicodeIdnaFlag_Transitional, &out);
        std::string asciiT = taken(&out);

        // An implementation need only record an error: the text of a conversion that fails is not compared.
        bool agrees = unicodeValid == !unicodeError && (unicodeError || unicode == toUnicode) &&
            asciiNValid == !asciiNError && (asciiNError || asciiN == toAsciiN) &&
            asciiTValid == !asciiTError && (asciiTError || asciiT == toAsciiT);
        ++cases;
        if (!agrees && ++failures <= 20)
            MESSAGE("disagrees: ", line, "\n    toUnicode ", unicodeValid, " ", unicode, "\n    toAsciiN ",
                asciiNValid, " ", asciiN, "\n    toAsciiT ", asciiTValid, " ", asciiT);
    }
    CHECK(cases > 6000);
    CHECK(failures == 0);
}

TEST_CASE("the URL Standard's flags accept what its host parser does") {
    PrRawString out{};
    CHECK(prUnicodeIdnaToAscii("Bu\xcc\x88" "cher.de", PrUnicodeIdnaFlag_CheckBidi | PrUnicodeIdnaFlag_CheckJoiners, &out));
    CHECK(taken(&out) == "xn--bcher-kva.de");
    CHECK(prUnicodeIdnaToAscii("a_b.\xe2\x98\x95", PrUnicodeIdnaFlag_CheckBidi | PrUnicodeIdnaFlag_CheckJoiners, &out));
    CHECK(taken(&out) == "a_b.xn--53h");
    CHECK(!prUnicodeIdnaToAscii("xn--a.\xc3\x9f", PrUnicodeIdnaFlag_CheckBidi | PrUnicodeIdnaFlag_CheckJoiners, &out));
    CHECK(prRawStringIsNull(&out));
}
