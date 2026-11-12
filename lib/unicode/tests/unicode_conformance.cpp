// The conformance files of the Unicode Character Database and of UTS #46, which tools/suite/unicode.py drives.
//
// `unicode_conformance <part> <path>` runs every case of the file at `path`, which is the part that `part` names, such
// as BidiTest. It writes a line of JSON for each case that disagrees, {"case": its index, "text": what it tests}, and
// then {"cases": the number of cases}. A case is a line of the file, or in BidiTest a line in one of the paragraph
// directions that it lists, and in NormalizationTest also a code point that Part 1 does not list.

#include "pr/count_of.h"
#include "pr/unicode_bidi.h"
#include "pr/unicode_grapheme.h"
#include "pr/unicode_idna.h"
#include "pr/unicode_line_break.h"
#include "pr/unicode_normalization.h"
#include "pr/utf8.h"

#include "tools/harness/harness.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace harness;
using namespace pr;

namespace {

// Counts the cases, and writes each that disagrees.
struct Report {
    uint64_t cases = 0;

    void outcome(bool agrees, const std::string& text) {
        if (!agrees) {
            PrTextBuffer out = {};
            harnessAppendFormat(&out, "{\"case\":%llu,\"text\":", static_cast<unsigned long long>(cases));
            harnessWriteString(&out, text);
            prTextBufferAppendChar(&out, '}');
            harnessWriteLine(prTextBufferView(&out));
            prTextBufferReset(&out);
        }
        ++cases;
    }
};

[[noreturn]] void fail(const std::string& message) {
    std::fprintf(stderr, "unicode_conformance: %s\n", message.c_str());
    std::exit(2);
}

std::string trimmed(const std::string& text) {
    size_t start = text.find_first_not_of(" \t\r\n");
    size_t end = text.find_last_not_of(" \t\r\n");
    return start == std::string::npos ? std::string() : text.substr(start, end - start + 1);
}

// The data of a line: the line without its comment, trimmed.
std::string dataOf(const std::string& line) {
    return trimmed(line.substr(0, line.find('#')));
}

std::vector<std::string> fields(const std::string& text, char separator) {
    std::vector<std::string> out;
    std::string field;
    std::istringstream in(text);
    while (std::getline(in, field, separator))
        out.push_back(field);
    return out;
}

void appendUtf8(std::string* out, uint32_t codePoint) {
    char encoded[kPrUtf8MaxBytes];
    out->append(encoded, prUtf8Encode(codePoint, encoded));
}

std::vector<uint32_t> codePointsOf(const std::string& field) {
    std::istringstream in(field);
    std::vector<uint32_t> out;
    std::string token;
    while (in >> token)
        out.push_back(uint32_t(std::stoul(token, nullptr, 16)));
    return out;
}

// ---------------------------------------------------------------------------------------------------------------------
// BidiTest and BidiCharacterTest

const char* const kClassNames[] = {
    "L",   "R",   "AL",  "EN",  "ES",  "ET",  "AN",  "CS",
    "NSM", "BN",  "B",   "S",   "WS",  "ON",  "LRE", "LRO",
    "RLE", "RLO", "PDF", "LRI", "RLI", "FSI", "PDI",
};

PrUnicodeBidiClass classNamed(const std::string& name) {
    for (uint32_t i = 0; i < countOf(kClassNames); ++i)
        if (name == kClassNames[i])
            return PrUnicodeBidiClass(i);
    fail("unknown bidi class " + name);
}

// The levels of a line, and its visual order without the characters that X9 removes, in the format of the conformance
// files. The level of a removed character is `x`.
struct BidiLine {
    std::string levels;
    std::string order;
};

// Resolves the text as one line of a paragraph in the given direction.
BidiLine resolveLine(const std::vector<PrUnicodeBidiClass>& classes, const std::vector<uint32_t>* codePoints,
        PrUnicodeBidiDirection direction, uint8_t* paragraphLevel) {
    uint32_t count = uint32_t(classes.size());
    std::vector<uint8_t> levels(count);
    *paragraphLevel = prUnicodeBidiResolve(classes.data(), codePoints ? codePoints->data() : nullptr, count, direction,
        levels.data());
    prUnicodeBidiLineLevels(classes.data(), count, *paragraphLevel, levels.data());
    std::vector<uint32_t> order(count);
    prUnicodeBidiReorder(levels.data(), count, order.data());
    BidiLine line;
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

void bidiTest(std::ifstream& file, Report* report) {
    const PrUnicodeBidiDirection kBits[3] = {
        PrUnicodeBidiDirection_Auto, PrUnicodeBidiDirection_LeftToRight, PrUnicodeBidiDirection_RightToLeft,
    };
    std::string levels;
    std::string order;
    std::string line;
    while (std::getline(file, line)) {
        std::string data = dataOf(line);
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
        for (int bit = 0; bit < 3; ++bit) {
            if ((directions & (1 << bit)) == 0)
                continue;
            uint8_t paragraph = 0;
            BidiLine resolved = resolveLine(classes, nullptr, kBits[bit], &paragraph);
            report->outcome(resolved.levels == levels && resolved.order == order,
                data + " (direction " + std::to_string(bit) + "): levels " + resolved.levels + "; order " +
                    resolved.order);
        }
    }
}

void bidiCharacterTest(std::ifstream& file, Report* report) {
    const PrUnicodeBidiDirection kDirections[3] = {
        PrUnicodeBidiDirection_LeftToRight, PrUnicodeBidiDirection_RightToLeft, PrUnicodeBidiDirection_Auto,
    };
    std::string line;
    while (std::getline(file, line)) {
        std::string data = dataOf(line);
        if (data.empty())
            continue;
        // Code points; direction, 0 LTR, 1 RTL or 2 auto; paragraph level; levels; order.
        std::vector<std::string> parts = fields(data, ';');
        std::vector<uint32_t> codePoints = codePointsOf(parts[0]);
        std::vector<PrUnicodeBidiClass> classes;
        for (uint32_t codePoint : codePoints)
            classes.push_back(prUnicodeBidiClass(codePoint));
        uint8_t paragraph = 0;
        BidiLine resolved = resolveLine(classes, &codePoints, kDirections[std::stoi(parts[1])], &paragraph);
        std::string order = trimmed(parts.size() > 4 ? parts[4] : std::string());
        report->outcome(paragraph == std::stoi(parts[2]) && resolved.levels == trimmed(parts[3]) && resolved.order == order,
            data + ": paragraph " + std::to_string(paragraph) + "; levels " + resolved.levels + "; order " +
                resolved.order);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// NormalizationTest

std::string utf8(const std::vector<uint32_t>& codePoints) {
    std::string out;
    for (uint32_t codePoint : codePoints)
        appendUtf8(&out, codePoint);
    return out;
}

std::string normalize(const std::string& text, PrUnicodeNormalForm form) {
    PrRawString normalized = prUnicodeNormalize(text, form);
    PrStringView view = prRawStringView(&normalized);
    std::string out(view.data != nullptr ? view.data : "", view.size);
    prRawStringReset(&normalized);
    return out;
}

bool isNormalized(const std::string& text, PrUnicodeNormalForm form) {
    return prUnicodeIsNormalized(text, form);
}

void normalizationTest(std::ifstream& file, Report* report) {
    std::set<uint32_t> listed;
    bool partOne = false;
    std::string line;
    while (std::getline(file, line)) {
        if (line.rfind("@Part", 0) == 0) {
            partOne = line.rfind("@Part1", 0) == 0;
            continue;
        }
        std::string data = dataOf(line);
        if (data.empty())
            continue;
        // The columns c1 to c5: a source, its NFC, its NFD, its NFKC and its NFKD.
        std::vector<std::string> columns;
        for (const std::string& column : fields(data, ';'))
            columns.push_back(utf8(codePointsOf(column)));
        if (columns.size() < 5)
            fail("a line of NormalizationTest has fewer than five columns: " + line);
        if (partOne)
            listed.insert(codePointsOf(data.substr(0, data.find(';')))[0]);
        bool agrees = true;
        for (size_t i = 0; i < 3; ++i) {
            agrees &= normalize(columns[i], PrUnicodeNormalForm_Nfc) == columns[1];
            agrees &= normalize(columns[i], PrUnicodeNormalForm_Nfd) == columns[2];
        }
        for (size_t i = 3; i < 5; ++i) {
            agrees &= normalize(columns[i], PrUnicodeNormalForm_Nfc) == columns[3];
            agrees &= normalize(columns[i], PrUnicodeNormalForm_Nfd) == columns[4];
        }
        agrees &= isNormalized(columns[1], PrUnicodeNormalForm_Nfc);
        agrees &= isNormalized(columns[2], PrUnicodeNormalForm_Nfd);
        report->outcome(agrees, data);
    }
    // Every code point that Part 1 does not list is in every form.
    for (uint32_t codePoint = 0; codePoint <= kPrUtf8MaxCodePoint; ++codePoint) {
        if ((codePoint >= 0xd800 && codePoint <= 0xdfff) || listed.count(codePoint) != 0)
            continue;
        std::string text = utf8({codePoint});
        char name[16];
        std::snprintf(name, sizeof(name), "U+%04X", codePoint);
        report->outcome(normalize(text, PrUnicodeNormalForm_Nfc) == text && normalize(text, PrUnicodeNormalForm_Nfd) == text,
            std::string(name) + " is not in every form");
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// LineBreakTest, GraphemeBreakTest and WordBreakTest

// A line of a break test, "÷ 0020 × 0308 ÷": its code points, and after each whether a break follows.
struct BreakLine {
    std::vector<uint32_t> codePoints;
    std::vector<bool> breaks;
};

BreakLine breakLineOf(const std::string& data) {
    BreakLine out;
    std::istringstream in(data);
    std::string token;
    while (in >> token) {
        if (token == "\xc3\xb7" || token == "\xc3\x97") {
            if (!out.codePoints.empty())
                out.breaks.push_back(token == "\xc3\xb7");
        } else {
            out.codePoints.push_back(uint32_t(std::stoul(token, nullptr, 16)));
        }
    }
    return out;
}

void lineBreakTest(std::ifstream& file, Report* report) {
    std::string line;
    while (std::getline(file, line)) {
        std::string data = dataOf(line);
        if (data.empty())
            continue;
        BreakLine expected = breakLineOf(data);
        std::vector<PrUnicodeLineBreak> actual(expected.codePoints.size());
        prUnicodeLineBreaks(expected.codePoints.data(), uint32_t(expected.codePoints.size()), actual.data());
        // The test marks a mandatory break as a break.
        std::vector<bool> breaks;
        for (PrUnicodeLineBreak b : actual)
            breaks.push_back(b != PrUnicodeLineBreak_Prohibited);
        report->outcome(breaks == expected.breaks, data);
    }
}

// The byte offsets of the breaks of a break test's line, after each code point that a break follows.
std::vector<size_t> breakOffsets(const BreakLine& line, std::string* text) {
    std::vector<size_t> out;
    for (size_t i = 0; i < line.codePoints.size(); ++i) {
        appendUtf8(text, line.codePoints[i]);
        if (line.breaks[i])
            out.push_back(text->size());
    }
    return out;
}

void graphemeBreakTest(std::ifstream& file, Report* report) {
    std::string line;
    while (std::getline(file, line)) {
        std::string data = dataOf(line);
        if (data.empty())
            continue;
        std::string text;
        std::vector<size_t> expected = breakOffsets(breakLineOf(data), &text);
        PrStringView view{text.data(), text.size()};
        std::vector<size_t> actual;
        for (size_t at = 0; at < text.size();) {
            at = prUnicodeGraphemeNext(view, at);
            actual.push_back(at);
        }
        // Segmenting one code point at a time with the state breaks at the same offsets.
        std::vector<size_t> pushed;
        PrUnicodeGraphemeState state{};
        for (size_t at = 0; at < text.size();) {
            uint32_t codePoint = 0;
            uint32_t length = prUtf8Decode(view, at, &codePoint);
            if (at == 0)
                state = prUnicodeGraphemeStateBegin(codePoint);
            else if (!prUnicodeGraphemeStatePush(&state, codePoint))
                pushed.push_back(at);
            at += length;
        }
        pushed.push_back(text.size());
        report->outcome(actual == expected && pushed == expected, data);
    }
}

void wordBreakTest(std::ifstream& file, Report* report) {
    std::string line;
    while (std::getline(file, line)) {
        std::string data = dataOf(line);
        if (data.empty())
            continue;
        std::string text;
        std::vector<size_t> expected = breakOffsets(breakLineOf(data), &text);
        PrStringView view{text.data(), text.size()};
        std::vector<size_t> actual;
        for (size_t at = 0; at < text.size();) {
            at = prUnicodeWordNext(view, at);
            actual.push_back(at);
        }
        report->outcome(actual == expected, data);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// IdnaTestV2

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
        appendUtf8(&out, uint32_t(std::stoul(text.substr(digits, end - digits), nullptr, 16)));
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

void idnaTest(std::ifstream& file, Report* report) {
    std::string line;
    while (std::getline(file, line)) {
        std::string data = line.substr(0, line.find('#'));
        if (trimmed(data).empty())
            continue;
        std::vector<std::string> columns;
        size_t at = 0;
        for (size_t end; (end = data.find(';', at)) != std::string::npos; at = end + 1)
            columns.push_back(data.substr(at, end - at));
        columns.push_back(data.substr(at));
        if (columns.size() < 7)
            fail("a line of IdnaTestV2 has fewer than seven columns: " + line);
        std::string source = value(columns[0], "");
        std::string toUnicode = value(columns[1], source);
        // X4_2 marks an empty label, which only VerifyDnsLength rejects.
        bool unicodeError = hasError(columns[2], false, {"X4_2"});
        std::string toAsciiN = value(columns[3], toUnicode);
        bool asciiNError = hasError(columns[4], hasError(columns[2], false, {}), {});
        std::string toAsciiT = value(columns[5], toAsciiN);
        bool asciiTError = hasError(columns[6], asciiNError, {});

        PrStringView input(source);
        PrRawString out{};
        bool unicodeValid = prUnicodeIdnaToUnicode(input, PrUnicodeIdnaFlag_Strict, &out);
        std::string unicode = taken(&out);
        bool asciiNValid = prUnicodeIdnaToAscii(input, PrUnicodeIdnaFlag_Strict, &out);
        std::string asciiN = taken(&out);
        bool asciiTValid = prUnicodeIdnaToAscii(input, PrUnicodeIdnaFlag_Strict | PrUnicodeIdnaFlag_Transitional, &out);
        std::string asciiT = taken(&out);

        // An implementation need only record an error: the text of a conversion that fails is not compared.
        bool agrees = unicodeValid == !unicodeError && (unicodeError || unicode == toUnicode) &&
            asciiNValid == !asciiNError && (asciiNError || asciiN == toAsciiN) &&
            asciiTValid == !asciiTError && (asciiTError || asciiT == toAsciiT);
        report->outcome(agrees, trimmed(data) + ": toUnicode " + (unicodeValid ? unicode : "error") + "; toAsciiN " +
            (asciiNValid ? asciiN : "error") + "; toAsciiT " + (asciiTValid ? asciiT : "error"));
    }
}

struct Part {
    const char* name;
    void (*run)(std::ifstream& file, Report* report);
};

const Part kParts[] = {
    {"BidiTest", bidiTest},
    {"BidiCharacterTest", bidiCharacterTest},
    {"NormalizationTest", normalizationTest},
    {"LineBreakTest", lineBreakTest},
    {"GraphemeBreakTest", graphemeBreakTest},
    {"WordBreakTest", wordBreakTest},
    {"IdnaTestV2", idnaTest},
};

} // namespace

int main(int argc, char** argv) {
    if (argc != 3)
        fail("usage: unicode_conformance <part> <path>");
    for (const Part& part : kParts) {
        if (std::strcmp(argv[1], part.name) != 0)
            continue;
        std::ifstream file(argv[2], std::ios::binary);
        if (!file.good())
            fail(std::string("cannot read ") + argv[2]);
        Report report;
        part.run(file, &report);
        PrTextBuffer out = {};
        harnessAppendFormat(&out, "{\"cases\":%llu}", static_cast<unsigned long long>(report.cases));
        harnessWriteLine(prTextBufferView(&out));
        prTextBufferReset(&out);
        return 0;
    }
    fail(std::string("no part named ") + argv[1]);
}
