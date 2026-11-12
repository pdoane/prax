// The conformance files of the Unicode Character Database and of UTS #46, which tools/suite/unicode.py drives.
//
// `unicode_conformance <part> <path>` runs every case of the file at `path`, which is the part that `part` names, such
// as BidiTest. It writes a line of JSON for each case that disagrees, {"case": its index, "text": what it tests}, and
// then {"cases": the number of cases}. A case is a line of the file, or in BidiTest a line in one of the paragraph
// directions that it lists, and in NormalizationTest also a code point that Part 1 does not list.

#include "pr/array.h"
#include "pr/count_of.h"
#include "pr/integer.h"
#include "pr/print.h"
#include "pr/set.h"
#include "pr/span.h"
#include "pr/string.h"
#include "pr/string_ops.h"
#include "pr/text_buffer.h"
#include "pr/unicode_bidi.h"
#include "pr/unicode_grapheme.h"
#include "pr/unicode_idna.h"
#include "pr/unicode_line_break.h"
#include "pr/unicode_normalization.h"
#include "pr/utf8.h"

#include "tools/harness/harness.h"

using namespace harness;
using namespace pr;

namespace {

// Counts the cases, and writes each that disagrees. A file that cannot be read as its part fails the report, which ends
// the run.
struct Report {
    uint64_t cases = 0;
    bool failed = false;

    void outcome(bool agrees, PrStringView text) {
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

    void fail(PrStringView message) {
        printError("unicode_conformance: {}\n", message);
        failed = true;
    }
};

// Reads the lines of a file, each without its line feed. A line feed at the end of the file ends the last line rather
// than starting another.
struct Lines {
    PrStringView rest;

    bool next(PrStringView* line) {
        if (rest.isEmpty())
            return false;
        size_t end = prStringFindByte(rest, '\n');
        *line = prStringHead(rest, end);
        rest = prStringFrom(rest, end + 1);
        return true;
    }
};

// Reads the next word of `rest`, the bytes up to the next ASCII whitespace, and returns false if no word is left.
bool nextWord(PrStringView* rest, PrStringView* word) {
    size_t start = 0;
    while (start < rest->size && prAsciiIsSpace(rest->data[start]))
        ++start;
    if (start == rest->size)
        return false;
    size_t end = start;
    while (end < rest->size && !prAsciiIsSpace(rest->data[end]))
        ++end;
    *word = prStringSlice(*rest, start, end - start);
    *rest = prStringFrom(*rest, end);
    return true;
}

// Returns the value of the digits in `radix` at the start of `text`, after any whitespace, or zero if there are none.
uint32_t numberOf(PrStringView text, uint32_t radix) {
    PrStringView digits = prStringTrim(text);
    uint64_t value = 0;
    prU64ParsePrefix(digits.data, digits.size, &value, radix);
    return uint32_t(value);
}

// The data of a line: the line without its comment, trimmed.
PrStringView dataOf(PrStringView line) {
    return prStringTrim(prStringHead(line, prStringFindByte(line, '#')));
}

// The fields of `text` between each `separator`. A separator at the end of the text ends the last field rather than
// starting another.
Array<PrStringView> fields(PrStringView text, char separator) {
    Array<PrStringView> out;
    Lines parts{text};
    while (!parts.rest.isEmpty()) {
        size_t end = prStringFindByte(parts.rest, separator);
        out.push(prStringHead(parts.rest, end));
        parts.rest = prStringFrom(parts.rest, end + 1);
    }
    return out;
}

void appendUtf8(String* out, uint32_t codePoint) {
    char encoded[kPrUtf8MaxBytes];
    out->append(PrStringView(encoded, prUtf8Encode(codePoint, encoded)));
}

Array<uint32_t> codePointsOf(PrStringView field) {
    Array<uint32_t> out;
    PrStringView token;
    while (nextWord(&field, &token))
        out.push(numberOf(token, 16));
    return out;
}

// ---------------------------------------------------------------------------------------------------------------------
// BidiTest and BidiCharacterTest

const char* const kClassNames[] = {
    "L",   "R",   "AL",  "EN",  "ES",  "ET",  "AN",  "CS",
    "NSM", "BN",  "B",   "S",   "WS",  "ON",  "LRE", "LRO",
    "RLE", "RLO", "PDF", "LRI", "RLI", "FSI", "PDI",
};

// Returns the class named `name`, or fails the report.
PrUnicodeBidiClass classNamed(PrStringView name, Report* report) {
    for (uint32_t i = 0; i < countOf(kClassNames); ++i)
        if (name == kClassNames[i])
            return PrUnicodeBidiClass(i);
    report->fail(format("unknown bidi class {}", name));
    return 0;
}

// The levels of a line, and its visual order without the characters that X9 removes, in the format of the conformance
// files. The level of a removed character is `x`.
struct BidiLine {
    String levels;
    String order;
};

// Resolves the text as one line of a paragraph in the given direction.
BidiLine resolveLine(const Array<PrUnicodeBidiClass>& classes, const Array<uint32_t>* PR_NULLABLE codePoints,
        PrUnicodeBidiDirection direction, uint8_t* paragraphLevel) {
    uint32_t count = classes.count();
    Array<uint8_t> levels(count);
    *paragraphLevel = prUnicodeBidiResolve(classes.data(), codePoints ? codePoints->data() : nullptr, count, direction,
        levels.data());
    prUnicodeBidiLineLevels(classes.data(), count, *paragraphLevel, levels.data());
    Array<uint32_t> order(count);
    prUnicodeBidiReorder(levels.data(), count, order.data());
    BidiLine line;
    for (uint32_t i = 0; i < count; ++i) {
        if (!line.levels.isEmpty())
            line.levels.append(' ');
        if (levels[i] == kPrUnicodeBidiRemoved)
            line.levels.append("x");
        else
            formatTo(line.levels, "{}", uint32_t(levels[i]));
    }
    for (uint32_t i : order) {
        if (levels[i] == kPrUnicodeBidiRemoved)
            continue;
        if (!line.order.isEmpty())
            line.order.append(' ');
        formatTo(line.order, "{}", i);
    }
    return line;
}

void bidiTest(PrStringView file, Report* report) {
    const PrUnicodeBidiDirection kBits[3] = {
        PrUnicodeBidiDirection_Auto, PrUnicodeBidiDirection_LeftToRight, PrUnicodeBidiDirection_RightToLeft,
    };
    PrStringView levels;
    PrStringView order;
    Lines lines{file};
    PrStringView line;
    while (lines.next(&line)) {
        PrStringView data = dataOf(line);
        if (data.isEmpty())
            continue;
        if (prStringStartsWith(data, "@Levels:")) {
            levels = prStringTrim(prStringFrom(data, 8));
            continue;
        }
        if (prStringStartsWith(data, "@Reorder:")) {
            order = prStringTrim(prStringFrom(data, 9));
            continue;
        }
        Array<PrStringView> parts = fields(data, ';');
        Array<PrUnicodeBidiClass> classes;
        PrStringView names = parts[0];
        PrStringView name;
        while (!report->failed && nextWord(&names, &name))
            classes.push(classNamed(name, report));
        if (report->failed)
            return;
        uint32_t directions = numberOf(parts[1], 10);
        for (uint32_t bit = 0; bit < 3; ++bit) {
            if ((directions & (1u << bit)) == 0)
                continue;
            uint8_t paragraph = 0;
            BidiLine resolved = resolveLine(classes, nullptr, kBits[bit], &paragraph);
            report->outcome(resolved.levels == levels && resolved.order == order,
                format("{} (direction {}): levels {}; order {}", data, bit, resolved.levels, resolved.order));
        }
    }
}

void bidiCharacterTest(PrStringView file, Report* report) {
    const PrUnicodeBidiDirection kDirections[3] = {
        PrUnicodeBidiDirection_LeftToRight, PrUnicodeBidiDirection_RightToLeft, PrUnicodeBidiDirection_Auto,
    };
    Lines lines{file};
    PrStringView line;
    while (lines.next(&line)) {
        PrStringView data = dataOf(line);
        if (data.isEmpty())
            continue;
        // Code points; direction, 0 LTR, 1 RTL or 2 auto; paragraph level; levels; order.
        Array<PrStringView> parts = fields(data, ';');
        Array<uint32_t> codePoints = codePointsOf(parts[0]);
        Array<PrUnicodeBidiClass> classes;
        for (uint32_t codePoint : codePoints)
            classes.push(prUnicodeBidiClass(codePoint));
        uint8_t paragraph = 0;
        BidiLine resolved = resolveLine(classes, &codePoints, kDirections[numberOf(parts[1], 10)], &paragraph);
        PrStringView order = prStringTrim(parts.count() > 4 ? parts[4] : PrStringView(""));
        report->outcome(paragraph == numberOf(parts[2], 10) && resolved.levels == prStringTrim(parts[3]) &&
            resolved.order == order,
            format("{}: paragraph {}; levels {}; order {}", data, uint32_t(paragraph), resolved.levels,
                resolved.order));
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// NormalizationTest

String utf8(Span<const uint32_t> codePoints) {
    String out("");
    for (uint32_t codePoint : codePoints)
        appendUtf8(&out, codePoint);
    return out;
}

String normalize(PrStringView text, PrUnicodeNormalForm form) {
    return String::adopt(prUnicodeNormalize(text, form));
}

void normalizationTest(PrStringView file, Report* report) {
    Set<uint32_t> listed;
    bool partOne = false;
    Lines lines{file};
    PrStringView line;
    while (lines.next(&line)) {
        if (prStringStartsWith(line, "@Part")) {
            partOne = prStringStartsWith(line, "@Part1");
            continue;
        }
        PrStringView data = dataOf(line);
        if (data.isEmpty())
            continue;
        // The columns c1 to c5: a source, its NFC, its NFD, its NFKC and its NFKD.
        Array<String> columns;
        for (PrStringView column : fields(data, ';'))
            columns.push(utf8(codePointsOf(column).span().asConst()));
        if (columns.count() < 5) {
            report->fail(format("a line of NormalizationTest has fewer than five columns: {}", line));
            return;
        }
        if (partOne)
            listed.add(codePointsOf(prStringHead(data, prStringFindByte(data, ';')))[0]);
        bool agrees = true;
        for (uint32_t i = 0; i < 3; ++i) {
            agrees &= normalize(columns[i], PrUnicodeNormalForm_Nfc) == columns[1];
            agrees &= normalize(columns[i], PrUnicodeNormalForm_Nfd) == columns[2];
        }
        for (uint32_t i = 3; i < 5; ++i) {
            agrees &= normalize(columns[i], PrUnicodeNormalForm_Nfc) == columns[3];
            agrees &= normalize(columns[i], PrUnicodeNormalForm_Nfd) == columns[4];
        }
        agrees &= prUnicodeIsNormalized(columns[1], PrUnicodeNormalForm_Nfc);
        agrees &= prUnicodeIsNormalized(columns[2], PrUnicodeNormalForm_Nfd);
        report->outcome(agrees, data);
    }
    // Every code point that Part 1 does not list is in every form.
    for (uint32_t codePoint = 0; codePoint <= kPrUtf8MaxCodePoint; ++codePoint) {
        if ((codePoint >= 0xd800 && codePoint <= 0xdfff) || listed.contains(codePoint))
            continue;
        String text = utf8(Span<const uint32_t>(&codePoint, 1));
        report->outcome(normalize(text, PrUnicodeNormalForm_Nfc) == text && normalize(text, PrUnicodeNormalForm_Nfd) == text,
            format("U+{:04X} is not in every form", codePoint));
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// LineBreakTest, GraphemeBreakTest and WordBreakTest

// A line of a break test, "÷ 0020 × 0308 ÷": its code points, and after each whether a break follows.
struct BreakLine {
    Array<uint32_t> codePoints;
    Array<bool> breaks;
};

BreakLine breakLineOf(PrStringView data) {
    BreakLine out;
    PrStringView token;
    while (nextWord(&data, &token)) {
        if (token == "\xc3\xb7" || token == "\xc3\x97") {
            if (!out.codePoints.isEmpty())
                out.breaks.push(token == "\xc3\xb7");
        } else {
            out.codePoints.push(numberOf(token, 16));
        }
    }
    return out;
}

void lineBreakTest(PrStringView file, Report* report) {
    Lines lines{file};
    PrStringView line;
    while (lines.next(&line)) {
        PrStringView data = dataOf(line);
        if (data.isEmpty())
            continue;
        BreakLine expected = breakLineOf(data);
        Array<PrUnicodeLineBreak> actual(expected.codePoints.count());
        prUnicodeLineBreaks(expected.codePoints.data(), expected.codePoints.count(), actual.data());
        // The test marks a mandatory break as a break.
        Array<bool> breaks;
        for (PrUnicodeLineBreak b : actual)
            breaks.push(b != PrUnicodeLineBreak_Prohibited);
        report->outcome(breaks == expected.breaks, data);
    }
}

// The byte offsets of the breaks of a break test's line, after each code point that a break follows.
Array<size_t> breakOffsets(const BreakLine& line, String* text) {
    Array<size_t> out;
    for (uint32_t i = 0; i < line.codePoints.count(); ++i) {
        appendUtf8(text, line.codePoints[i]);
        if (line.breaks[i])
            out.push(text->count());
    }
    return out;
}

void graphemeBreakTest(PrStringView file, Report* report) {
    Lines lines{file};
    PrStringView line;
    while (lines.next(&line)) {
        PrStringView data = dataOf(line);
        if (data.isEmpty())
            continue;
        String text("");
        Array<size_t> expected = breakOffsets(breakLineOf(data), &text);
        PrStringView view = text;
        Array<size_t> actual;
        for (size_t at = 0; at < view.size;) {
            at = prUnicodeGraphemeNext(view, at);
            actual.push(at);
        }
        // Segmenting one code point at a time with the state breaks at the same offsets.
        Array<size_t> pushed;
        PrUnicodeGraphemeState state{};
        for (size_t at = 0; at < view.size;) {
            uint32_t codePoint = 0;
            uint32_t length = prUtf8Decode(view, at, &codePoint);
            if (at == 0)
                state = prUnicodeGraphemeStateBegin(codePoint);
            else if (!prUnicodeGraphemeStatePush(&state, codePoint))
                pushed.push(at);
            at += length;
        }
        pushed.push(view.size);
        report->outcome(actual == expected && pushed == expected, data);
    }
}

void wordBreakTest(PrStringView file, Report* report) {
    Lines lines{file};
    PrStringView line;
    while (lines.next(&line)) {
        PrStringView data = dataOf(line);
        if (data.isEmpty())
            continue;
        String text("");
        Array<size_t> expected = breakOffsets(breakLineOf(data), &text);
        PrStringView view = text;
        Array<size_t> actual;
        for (size_t at = 0; at < view.size;) {
            at = prUnicodeWordNext(view, at);
            actual.push(at);
        }
        report->outcome(actual == expected, data);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// IdnaTestV2

// Replaces the \uXXXX and \x{XXXX} escapes of the test file with the code points they name.
String unescaped(PrStringView text) {
    String out("");
    for (size_t i = 0; i < text.size; ++i) {
        size_t digits = 0;
        size_t end = 0;
        if (prStringStartsWith(prStringFrom(text, i), "\\u")) {
            digits = i + 2;
            end = i + 6 < text.size ? i + 6 : text.size;
        } else if (prStringStartsWith(prStringFrom(text, i), "\\x{")) {
            digits = i + 3;
            end = prStringFindFrom(text, "}", i);
        }
        if (digits == 0) {
            out.append(text.data[i]);
            continue;
        }
        appendUtf8(&out, numberOf(prStringSlice(text, digits, end - digits), 16));
        i = end < text.size && text.data[end] == '}' ? end : end - 1;
    }
    return out;
}

// A column's value: the empty string for "", or `fallback` for a blank column.
String value(PrStringView column, PrStringView fallback) {
    PrStringView text = prStringTrim(column);
    if (text.isEmpty())
        return fallback;
    return text == "\"\"" ? String("") : unescaped(text);
}

// Returns true if a status column records an error, other than the code `ignored` if it is given. A blank column is
// `fallback`.
bool hasError(PrStringView column, bool fallback, PrStringView ignored = PrStringView{}) {
    PrStringView text = prStringTrim(column);
    if (text.isEmpty())
        return fallback;
    PrStringView codes = text.size >= 2 ? prStringSlice(text, 1, text.size - 2) : PrStringView("");
    size_t at = 0;
    while (at < codes.size) {
        size_t end = prStringFindFrom(codes, ",", at);
        PrStringView code = prStringTrim(prStringSlice(codes, at, end - at));
        bool skip = code.isEmpty() || (!ignored.isEmpty() && code == ignored);
        if (!skip)
            return true;
        at = end == codes.size ? codes.size : end + 1;
    }
    return false;
}

// Returns the text of `text`, which it takes, and leaves `text` null.
String taken(PrRawString* text) {
    String out = String::adopt(*text);
    *text = PrRawString{};
    return out;
}

void idnaTest(PrStringView file, Report* report) {
    Lines lines{file};
    PrStringView line;
    while (lines.next(&line)) {
        PrStringView data = prStringHead(line, prStringFindByte(line, '#'));
        if (prStringTrim(data).isEmpty())
            continue;
        Array<PrStringView> columns;
        size_t at = 0;
        for (size_t end; (end = prStringFindFrom(data, ";", at)) != data.size; at = end + 1)
            columns.push(prStringSlice(data, at, end - at));
        columns.push(prStringFrom(data, at));
        if (columns.count() < 7) {
            report->fail(format("a line of IdnaTestV2 has fewer than seven columns: {}", line));
            return;
        }
        String source = value(columns[0], "");
        String toUnicode = value(columns[1], source);
        // X4_2 marks an empty label, which only VerifyDnsLength rejects.
        bool unicodeError = hasError(columns[2], false, "X4_2");
        String toAsciiN = value(columns[3], toUnicode);
        bool asciiNError = hasError(columns[4], hasError(columns[2], false));
        String toAsciiT = value(columns[5], toAsciiN);
        bool asciiTError = hasError(columns[6], asciiNError);

        PrStringView input = source;
        PrRawString out{};
        bool unicodeValid = prUnicodeIdnaToUnicode(input, PrUnicodeIdnaFlag_Strict, &out);
        String unicode = taken(&out);
        bool asciiNValid = prUnicodeIdnaToAscii(input, PrUnicodeIdnaFlag_Strict, &out);
        String asciiN = taken(&out);
        bool asciiTValid = prUnicodeIdnaToAscii(input, PrUnicodeIdnaFlag_Strict | PrUnicodeIdnaFlag_Transitional, &out);
        String asciiT = taken(&out);

        // An implementation need only record an error: the text of a conversion that fails is not compared.
        bool agrees = unicodeValid == !unicodeError && (unicodeError || unicode == toUnicode) &&
            asciiNValid == !asciiNError && (asciiNError || asciiN == toAsciiN) &&
            asciiTValid == !asciiTError && (asciiTError || asciiT == toAsciiT);
        report->outcome(agrees, format("{}: toUnicode {}; toAsciiN {}; toAsciiT {}", prStringTrim(data),
            unicodeValid ? unicode.view() : PrStringView("error"), asciiNValid ? asciiN.view() : PrStringView("error"),
            asciiTValid ? asciiT.view() : PrStringView("error")));
    }
}

struct Part {
    const char* name;
    void (*run)(PrStringView file, Report* report);
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
    Report report;
    if (argc != 3) {
        report.fail("usage: unicode_conformance <part> <path>");
        return 2;
    }
    for (const Part& part : kParts) {
        if (PrStringView(argv[1]) != part.name)
            continue;
        PrTextBuffer file = {};
        if (!harnessReadFile(&file, argv[2])) {
            report.fail(format("cannot read {}", argv[2]));
            return 2;
        }
        part.run(prTextBufferView(&file), &report);
        prTextBufferReset(&file);
        if (report.failed)
            return 2;
        PrTextBuffer out = {};
        harnessAppendFormat(&out, "{\"cases\":%llu}", static_cast<unsigned long long>(report.cases));
        harnessWriteLine(prTextBufferView(&out));
        prTextBufferReset(&out);
        return 0;
    }
    report.fail(format("no part named {}", argv[1]));
    return 2;
}
