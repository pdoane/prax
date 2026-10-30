#include "pr/count_of.h"
#include "pr/unicode_bidi.h"

#include "doctest/doctest.h"

#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

namespace pr_impl {

namespace {

const char* const kClassNames[] = {
    "L",   "R",   "AL",  "EN",  "ES",  "ET",  "AN",  "CS",
    "NSM", "BN",  "B",   "S",   "WS",  "ON",  "LRE", "LRO",
    "RLE", "RLO", "PDF", "LRI", "RLI", "FSI", "PDI",
};

PrUnicodeBidiClass classNamed(const std::string& name) {
    for (uint32_t i = 0; i < countOf(kClassNames); ++i)
        if (name == kClassNames[i])
            return PrUnicodeBidiClass(i);
    FAIL("unknown class " << name);
    return PrUnicodeBidiClass_ON;
}

std::vector<std::string> fields(const std::string& text, char separator) {
    std::vector<std::string> out;
    std::string field;
    std::istringstream in(text);
    while (std::getline(in, field, separator))
        out.push_back(field);
    return out;
}

// The levels of a line, and its visual order without the characters that X9 removes, in the format of the conformance
// files. The level of a removed character is `x`.
struct Line {
    std::string levels;
    std::string order;
};

// Resolves the text as one line of a paragraph in the given direction.
Line resolveLine(const std::vector<PrUnicodeBidiClass>& classes, const std::vector<uint32_t>* codePoints,
        PrUnicodeBidiDirection direction, uint8_t* paragraphLevel) {
    uint32_t count = uint32_t(classes.size());
    std::vector<uint8_t> levels(count);
    *paragraphLevel = prUnicodeBidiResolve(classes.data(), codePoints ? codePoints->data() : nullptr, count, direction,
        levels.data());
    prUnicodeBidiLineLevels(classes.data(), count, *paragraphLevel, levels.data());
    std::vector<uint32_t> order(count);
    prUnicodeBidiReorder(levels.data(), count, order.data());
    Line line;
    for (uint32_t i = 0; i < count; ++i) {
        if (!line.levels.empty())
            line.levels += ' ';
        line.levels += levels[i] == kPrUnicodeBidiRemoved ? "x" : std::to_string(levels[i]);
    }
    for (uint32_t i : order) {
        if (levels[i] == kPrUnicodeBidiRemoved)
            continue;
        if (!line.order.empty())
            line.order += ' ';
        line.order += std::to_string(i);
    }
    return line;
}

std::string trimmed(const std::string& text) {
    size_t start = text.find_first_not_of(" \t\r\n");
    size_t end = text.find_last_not_of(" \t\r\n");
    return start == std::string::npos ? std::string() : text.substr(start, end - start + 1);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("the strong type of a code point is left, right or none") {
    CHECK(prUnicodeBidiStrong('A') == PrUnicodeBidiStrong_Left);
    CHECK(prUnicodeBidiStrong('1') == PrUnicodeBidiStrong_None);
    CHECK(prUnicodeBidiStrong(0x05EA) == PrUnicodeBidiStrong_Right);  // HEBREW LETTER TAV
    CHECK(prUnicodeBidiStrong(0x0627) == PrUnicodeBidiStrong_Right);  // ARABIC LETTER ALEF, which is AL
    CHECK(prUnicodeBidiStrong(0x0660) == PrUnicodeBidiStrong_None);   // ARABIC-INDIC DIGIT ZERO, which is AN
    CHECK(prUnicodeBidiStrong(0x4E00) == PrUnicodeBidiStrong_Left);
    CHECK(prUnicodeBidiStrong(0x05FF) == PrUnicodeBidiStrong_Right);  // unassigned in a right-to-left block
    CHECK(prUnicodeBidiStrong(0x10FFFF) == PrUnicodeBidiStrong_None);
}

TEST_CASE("the first strong code point of a text decides its direction") {
    CHECK(prUnicodeBidiFirstStrong("") == PrUnicodeBidiStrong_None);
    CHECK(prUnicodeBidiFirstStrong("12 -") == PrUnicodeBidiStrong_None);
    CHECK(prUnicodeBidiFirstStrong("12 \xD7\xAA abc") == PrUnicodeBidiStrong_Right);
    CHECK(prUnicodeBidiFirstStrong("\xC3\xA9 \xD7\xAA") == PrUnicodeBidiStrong_Left);
    CHECK(prUnicodeBidiFirstStrong("\xFF\xD7\xAA") == PrUnicodeBidiStrong_Right);
}

TEST_CASE("levels and orders are as every case of BidiTest.txt says") {
    std::string path = std::string(PR_UNICODE_DIR) + "/BidiTest.txt";
    FILE* file = std::fopen(path.c_str(), "r");
    REQUIRE(file != nullptr);
    char buffer[4096];
    std::string levels;
    std::string order;
    uint32_t cases = 0;
    uint32_t failures = 0;
    while (std::fgets(buffer, sizeof(buffer), file) != nullptr) {
        std::string data = trimmed(std::string(buffer).substr(0, std::string(buffer).find('#')));
        if (data.empty())
            continue;
        if (data.rfind("@Levels:", 0) == 0) {
            levels = trimmed(data.substr(8));
            continue;
        }
        if (data.rfind("@Reorder:", 0) == 0) {
            order = trimmed(data.substr(9));
            continue;
        }
        std::vector<std::string> parts = fields(data, ';');
        std::vector<PrUnicodeBidiClass> classes;
        std::istringstream in(parts[0]);
        std::string name;
        while (in >> name)
            classes.push_back(classNamed(name));
        int directions = std::stoi(parts[1]);
        const PrUnicodeBidiDirection kBits[3] = {
            PrUnicodeBidiDirection_Auto, PrUnicodeBidiDirection_LeftToRight,
            PrUnicodeBidiDirection_RightToLeft,
        };
        for (int bit = 0; bit < 3; ++bit) {
            if ((directions & (1 << bit)) == 0)
                continue;
            uint8_t paragraph = 0;
            Line line = resolveLine(classes, nullptr, kBits[bit], &paragraph);
            ++cases;
            if (line.levels != levels || line.order != order) {
                if (++failures <= 20) {
                    INFO(data << " (direction " << bit << ")");
                    CHECK(line.levels == levels);
                    CHECK(line.order == order);
                }
            }
        }
    }
    std::fclose(file);
    CHECK(failures == 0);
    CHECK(cases > 400000);
}

TEST_CASE("levels and orders are as every case of BidiCharacterTest.txt says") {
    std::string path = std::string(PR_UNICODE_DIR) + "/BidiCharacterTest.txt";
    FILE* file = std::fopen(path.c_str(), "r");
    REQUIRE(file != nullptr);
    char buffer[4096];
    uint32_t cases = 0;
    uint32_t failures = 0;
    while (std::fgets(buffer, sizeof(buffer), file) != nullptr) {
        std::string data = trimmed(std::string(buffer).substr(0, std::string(buffer).find('#')));
        if (data.empty())
            continue;
        // Code points; direction, 0 LTR, 1 RTL or 2 auto; paragraph level; levels; order.
        std::vector<std::string> parts = fields(data, ';');
        std::vector<uint32_t> codePoints;
        std::vector<PrUnicodeBidiClass> classes;
        std::istringstream in(parts[0]);
        std::string hex;
        while (in >> hex) {
            codePoints.push_back(uint32_t(std::stoul(hex, nullptr, 16)));
            classes.push_back(prUnicodeBidiClass(codePoints.back()));
        }
        const PrUnicodeBidiDirection kDirections[3] = {
            PrUnicodeBidiDirection_LeftToRight,
            PrUnicodeBidiDirection_RightToLeft, PrUnicodeBidiDirection_Auto,
        };
        uint8_t paragraph = 0;
        Line line = resolveLine(classes, &codePoints, kDirections[std::stoi(parts[1])], &paragraph);
        ++cases;
        bool ok = paragraph == std::stoi(parts[2]) && line.levels == trimmed(parts[3]) &&
            line.order == trimmed(parts.size() > 4 ? parts[4] : std::string());
        if (!ok && ++failures <= 20) {
            INFO(data);
            CHECK(int(paragraph) == std::stoi(parts[2]));
            CHECK(line.levels == trimmed(parts[3]));
            CHECK(line.order == trimmed(parts.size() > 4 ? parts[4] : std::string()));
        }
    }
    std::fclose(file);
    CHECK(failures == 0);
    CHECK(cases > 90000);
}
