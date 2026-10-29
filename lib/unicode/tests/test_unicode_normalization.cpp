#include "pr/unicode_normalization.h"
#include "pr/utf8.h"

#include "doctest/doctest.h"

#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace pr_impl {

namespace {

std::string utf8(const std::vector<uint32_t>& codePoints) {
    std::string out;
    for (uint32_t codePoint : codePoints) {
        char encoded[kPrUtf8MaxBytes];
        out.append(encoded, prUtf8Encode(codePoint, encoded));
    }
    return out;
}

std::string normalize(const std::string& text, PrUnicodeNormalForm form) {
    PrString normalized = prUnicodeNormalize(text, form);
    PrStringView view = prStringView(&normalized);
    std::string out(view.data != nullptr ? view.data : "", view.size);
    prStringReset(&normalized);
    return out;
}

std::vector<uint32_t> codePointsOf(const std::string& field) {
    std::istringstream in(field);
    std::vector<uint32_t> out;
    std::string token;
    while (in >> token)
        out.push_back(uint32_t(std::stoul(token, nullptr, 16)));
    return out;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("normalization is as every case of NormalizationTest.txt says") {
    std::string path = std::string(PR_UNICODE_DIR) + "/NormalizationTest.txt";
    std::ifstream file(path);
    REQUIRE(file.good());
    uint32_t cases = 0;
    uint32_t failures = 0;
    std::set<uint32_t> listed;
    bool partOne = false;
    std::string line;
    while (std::getline(file, line)) {
        if (line.rfind("@Part", 0) == 0) {
            partOne = line.rfind("@Part1", 0) == 0;
            continue;
        }
        std::string data = line.substr(0, line.find('#'));
        if (data.find_first_not_of(" \t\r\n") == std::string::npos)
            continue;
        // The columns c1 to c5: a source, its NFC, its NFD, its NFKC and its NFKD.
        std::vector<std::string> columns;
        std::istringstream in(data);
        std::string column;
        while (std::getline(in, column, ';'))
            columns.push_back(utf8(codePointsOf(column)));
        REQUIRE(columns.size() >= 5);
        if (partOne)
            listed.insert(codePointsOf(data.substr(0, data.find(';')))[0]);
        ++cases;
        bool agrees = true;
        for (size_t i = 0; i < 3; ++i) {
            agrees &= normalize(columns[i], PrUnicodeNormalForm_Nfc) == columns[1];
            agrees &= normalize(columns[i], PrUnicodeNormalForm_Nfd) == columns[2];
        }
        for (size_t i = 3; i < 5; ++i) {
            agrees &= normalize(columns[i], PrUnicodeNormalForm_Nfc) == columns[3];
            agrees &= normalize(columns[i], PrUnicodeNormalForm_Nfd) == columns[4];
        }
        agrees &= prUnicodeIsNormalized(columns[1], PrUnicodeNormalForm_Nfc);
        agrees &= prUnicodeIsNormalized(columns[2], PrUnicodeNormalForm_Nfd);
        if (!agrees && ++failures <= 10)
            MESSAGE("disagrees: ", data);
    }
    // Every code point that Part 1 does not list is in every form.
    for (uint32_t codePoint = 0; codePoint <= kPrUtf8MaxCodePoint; ++codePoint) {
        if ((codePoint >= 0xd800 && codePoint <= 0xdfff) || listed.count(codePoint) != 0)
            continue;
        std::string text = utf8({codePoint});
        ++cases;
        if ((normalize(text, PrUnicodeNormalForm_Nfc) != text || normalize(text, PrUnicodeNormalForm_Nfd) != text)
            && ++failures <= 10)
            MESSAGE("changes: U+", codePoint);
    }
    CHECK(cases > 1000000);
    CHECK(failures == 0);
}

TEST_CASE("ill-formed UTF-8 normalizes to U+FFFD and is in no form") {
    CHECK(normalize("a\xff", PrUnicodeNormalForm_Nfc) == "a\xef\xbf\xbd");
    CHECK(!prUnicodeIsNormalized("a\xff", PrUnicodeNormalForm_Nfc));
    CHECK(prUnicodeIsNormalized("", PrUnicodeNormalForm_Nfd));
}
