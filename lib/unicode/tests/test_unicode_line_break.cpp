#include "pr/unicode_line_break.h"

#include "doctest/doctest.h"

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

TEST_CASE("line breaks are as every case of LineBreakTest.txt says") {
    std::string path = std::string(PR_UNICODE_DIR) + "/LineBreakTest.txt";
    std::ifstream file(path);
    REQUIRE(file.good());
    uint32_t cases = 0;
    uint32_t failures = 0;
    std::string line;
    while (std::getline(file, line)) {
        std::string data = line.substr(0, line.find('#'));
        if (data.find_first_not_of(" \t\r\n") == std::string::npos)
            continue;
        // "× 0023 ÷ 0020 ÷": the code points, and whether each is followed by a break.
        std::istringstream in(data);
        std::string token;
        std::vector<uint32_t> codePoints;
        std::vector<PrUnicodeLineBreak> expected;
        while (in >> token) {
            if (token == "\xc3\xb7") {
                if (!codePoints.empty())
                    expected.push_back(PrUnicodeLineBreak_Allowed);
            } else if (token == "\xc3\x97") {
                if (!codePoints.empty())
                    expected.push_back(PrUnicodeLineBreak_Prohibited);
            } else {
                codePoints.push_back(uint32_t(std::stoul(token, nullptr, 16)));
            }
        }
        std::vector<PrUnicodeLineBreak> actual(codePoints.size());
        prUnicodeLineBreaks(codePoints.data(), uint32_t(codePoints.size()), actual.data());
        // The test marks a mandatory break as a break.
        for (PrUnicodeLineBreak& b : actual)
            if (b == PrUnicodeLineBreak_Mandatory)
                b = PrUnicodeLineBreak_Allowed;
        ++cases;
        if (actual != expected && ++failures <= 20) {
            INFO(line);
            CHECK(actual == expected);
        }
    }
    CHECK(failures == 0);
    CHECK(cases > 10000);
}

TEST_CASE("a hard line break is mandatory, and a combining mark never breaks from its base") {
    const uint32_t text[] = {'a', ' ', 'b', '\n', 'c', 0x0301, ' ', 0x0301};
    PrUnicodeLineBreak breaks[8];
    prUnicodeLineBreaks(text, 8, breaks);
    CHECK(breaks[0] == PrUnicodeLineBreak_Prohibited);
    CHECK(breaks[1] == PrUnicodeLineBreak_Allowed);
    CHECK(breaks[3] == PrUnicodeLineBreak_Mandatory);
    CHECK(breaks[4] == PrUnicodeLineBreak_Prohibited);
    // A mark after a space has no base, and is treated as an alphabetic character on its own.
    CHECK(breaks[6] == PrUnicodeLineBreak_Allowed);
    CHECK(breaks[7] == PrUnicodeLineBreak_Mandatory);
}
