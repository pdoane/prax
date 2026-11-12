// number_bench: what converting numbers between binary and text costs in lib/core, beside the C++ standard library's
// <charconv> on the same inputs. ./dev.sh bench numbers runs it.
//
// A case converts each of a set of inputs that a fixed seed makes, so that every run and both engines convert the same
// values. Each input set stands for a kind of number that the engine meets: random bit patterns cover the whole
// exponent range, unit values have all 17 digits, short values have the few digits of hand-written text such as CSS and
// JSON, and integers are whole. A row is one engine's cost of one conversion: {"engine", "name", "time", "counts"},
// written as a JSON array.
//
// Before a case is measured, each of prax's results is checked against std's. A parse must give the same bits. A
// shortest form must read back as the value with as many significant digits as std's. A correctly rounded form must be
// the same text. A form that rounds the shortest digits again, which prF64FormatSignificant and prF64FormatFixed do, is
// not checked. A mismatch is reported, and fails the run.
//
// Usage: number_bench [--repeat CASE [--engine NAME]] [CASE]...
//
// Measures the cases whose names start with one of the CASE arguments, or every case. --repeat runs one case on the
// named engine, prax by default, over and over for kHarnessRepeatSeconds, so that a profiler can sample it. It prints
// nothing.

#include "pr/assert.h"
#include "pr/float.h"
#include "pr/float_convert.h"
#include "pr/format.h"
#include "pr/integer.h"
#include "pr/text_buffer.h"

#include "tools/harness/harness.h"

#include <charconv>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

using namespace pr;
using namespace harness;

namespace {

// The number of inputs in each set. The set's values and text stay in the first levels of the cache, so that a case
// measures conversion, not memory.
constexpr uint32_t kCount = 4096;

// Room for any text that a case writes.
constexpr size_t kTextBytes = 64;

// The results of every conversion are summed here, so that no conversion is optimized away.
volatile uint64_t gSink;

// SplitMix64, for inputs that are the same on every platform.
struct SplitMix {
    uint64_t state;

    uint64_t next() {
        uint64_t z = (state += 0x9e3779b97f4a7c15ull);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
        return z ^ (z >> 31);
    }
};

uint64_t bitsOf(double value) {
    uint64_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

uint32_t bitsOf(float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

// ---------------------------------------------------------------------------------------------------------------------
// Inputs

enum {
    Kind_Random,
    Kind_Unit,
    Kind_Short,
    Kind_Integer,
};
typedef uint8_t Kind;

double makeDouble(SplitMix& random, Kind kind) {
    switch (kind) {
        case Kind_Random:
            for (;;) {
                uint64_t bits = random.next();
                if ((bits >> 52 & 0x7ff) != 0x7ff) {
                    double value;
                    memcpy(&value, &bits, sizeof(value));
                    return value;
                }
            }
        case Kind_Unit:
            return double(random.next() >> 11) * 0x1p-53;
        case Kind_Short: {
            // Up to six digits with up to four after the point, such as 1234.56 or 0.25.
            static const double kScale[] = {1, 10, 100, 1000, 10000};
            double units = double(random.next() % 1000000);
            return units / kScale[random.next() % 5];
        }
        case Kind_Integer:
            return double(random.next() >> 32);
    }
    return 0;
}

std::vector<double> doubles(Kind kind, uint64_t seed) {
    SplitMix random{seed};
    std::vector<double> out;
    for (uint32_t i = 0; i < kCount; ++i)
        out.push_back(makeDouble(random, kind));
    return out;
}

std::vector<float> floats(Kind kind, uint64_t seed) {
    SplitMix random{seed};
    std::vector<float> out;
    while (out.size() < kCount) {
        if (kind == Kind_Random) {
            uint32_t bits = uint32_t(random.next());
            if ((bits >> 23 & 0xff) == 0xff)
                continue;
            float value;
            memcpy(&value, &bits, sizeof(value));
            out.push_back(value);
        } else {
            // The float nearest the decimal, as a parse of its text gives.
            char text[kTextBytes];
            std::to_chars_result end = std::to_chars(text, text + sizeof(text), makeDouble(random, kind));
            float value = 0;
            std::from_chars(text, end.ptr, value);
            out.push_back(value);
        }
    }
    return out;
}

std::vector<uint64_t> integers(int digits, uint64_t seed) {
    SplitMix random{seed};
    std::vector<uint64_t> out;
    for (uint32_t i = 0; i < kCount; ++i) {
        uint64_t value = random.next();
        // Small values have up to three digits, and mixed values a count of digits spread evenly over the range.
        if (digits == 3)
            value %= 1000;
        else if (digits < 0)
            value >>= random.next() % 64;
        out.push_back(value);
    }
    return out;
}

// Texts laid end to end, with the offset of each.
struct Texts {
    std::string bytes;
    std::vector<uint32_t> starts;

    void add(const char* begin, const char* end) {
        starts.push_back(uint32_t(bytes.size()));
        bytes.append(begin, end);
    }
    uint32_t count() const { return uint32_t(starts.size()); }
    const char* data(uint32_t i) const { return bytes.data() + starts[i]; }
    size_t size(uint32_t i) const {
        return (i + 1 < starts.size() ? starts[i + 1] : uint32_t(bytes.size())) - starts[i];
    }
};

// The shortest text of each value, or with `precision`, the text of its first `precision` + 1 significant digits.
Texts doubleTexts(const std::vector<double>& values, int precision = -1) {
    Texts out;
    for (double value : values) {
        char text[kTextBytes];
        std::to_chars_result end = std::to_chars(text, text + sizeof(text), value);
        if (precision >= 0)
            end = std::to_chars(text, text + sizeof(text), value, std::chars_format::scientific, precision);
        out.add(text, end.ptr);
    }
    return out;
}

Texts integerTexts(const std::vector<uint64_t>& values) {
    Texts out;
    for (uint64_t value : values) {
        char text[kTextBytes];
        std::to_chars_result end = std::to_chars(text, text + sizeof(text), value);
        out.add(text, end.ptr);
    }
    return out;
}

// ---------------------------------------------------------------------------------------------------------------------
// Checks

// Returns the significant digits of a number's text: those of its mantissa, without its sign, its point, and the zeros
// that lead and trail them.
std::string significant(const char* text, size_t size) {
    std::string digits;
    for (size_t i = 0; i < size && text[i] != 'e' && text[i] != 'E'; ++i) {
        if (text[i] >= '0' && text[i] <= '9')
            digits += text[i];
    }
    size_t first = digits.find_first_not_of('0');
    if (first == std::string::npos)
        return "";
    return digits.substr(first, digits.find_last_not_of('0') + 1 - first);
}

bool failed = false;

void mismatch(const char* name, const std::string& input, const std::string& prax, const std::string& theirs) {
    fprintf(stderr, "%s: %s gives %s in prax and %s in std\n", name, input.c_str(), prax.c_str(), theirs.c_str());
    failed = true;
}

// Checks that `prax` reads back as `value`, with as many significant digits as std's shortest scientific form. std's
// plain shortest form can be positional, which writes every digit of a large integer.
template <typename T>
void checkShortest(const char* name, T value, const std::string& prax) {
    char text[kTextBytes];
    std::string theirs(text, std::to_chars(text, text + sizeof(text), value, std::chars_format::scientific).ptr);
    T back = 0;
    std::from_chars_result read = std::from_chars(prax.data(), prax.data() + prax.size(), back);
    bool same = read.ptr == prax.data() + prax.size() && bitsOf(back) == bitsOf(value);
    if (!same || significant(prax.data(), prax.size()).size() != significant(theirs.data(), theirs.size()).size())
        mismatch(name, theirs, prax, theirs);
}

// ---------------------------------------------------------------------------------------------------------------------
// Cases

// A case: one pass of each engine over its inputs, and a check of prax's results against std's.
struct Case {
    std::string name;
    std::function<void()> prax;
    std::function<void()> theirs;
    std::function<void(const char* name)> check;
};

std::string writeFloat(double value, FormatSpec spec) {
    char text[kTextBytes];
    Writer writer = bufferWriter(Span<char>(text, uint32_t(sizeof(text))));
    formatFloat(writer, value, spec);
    return std::string(text, writer.size);
}

std::string writeFloat(float value) {
    char text[kTextBytes];
    Writer writer = bufferWriter(Span<char>(text, uint32_t(sizeof(text))));
    formatFloat(writer, value, FormatSpec{});
    return std::string(text, writer.size);
}

// Formatting a uint64_t in decimal.
Case formatU64(const char* name, std::vector<uint64_t> values) {
    std::shared_ptr<std::vector<uint64_t>> shared = std::make_shared<std::vector<uint64_t>>(std::move(values));
    return Case{
        .name = name,
        .prax = [shared] {
            char text[kPrU64TextMaxBytes];
            uint64_t sum = 0;
            for (uint64_t value : *shared)
                sum += prU64Format(value, text, 10) + uint8_t(text[0]);
            gSink = gSink + sum;
        },
        .theirs = [shared] {
            char text[kTextBytes];
            uint64_t sum = 0;
            for (uint64_t value : *shared)
                sum += size_t(std::to_chars(text, text + sizeof(text), value).ptr - text) + uint8_t(text[0]);
            gSink = gSink + sum;
        },
        .check = [shared](const char* name) {
            for (uint64_t value : *shared) {
                char prax[kPrU64TextMaxBytes];
                char theirs[kTextBytes];
                std::string a(prax, prU64Format(value, prax, 10));
                std::string b(theirs, std::to_chars(theirs, theirs + sizeof(theirs), value).ptr);
                if (a != b)
                    mismatch(name, b, a, b);
            }
        },
    };
}

// Formatting a double as its shortest text, by prF64Format.
Case formatShortest(const char* name, std::vector<double> values) {
    std::shared_ptr<std::vector<double>> shared = std::make_shared<std::vector<double>>(std::move(values));
    return Case{
        .name = name,
        .prax = [shared] {
            char text[kPrF64TextMaxBytes];
            uint64_t sum = 0;
            for (double value : *shared)
                sum += prF64Format(value, text) + uint8_t(text[0]);
            gSink = gSink + sum;
        },
        .theirs = [shared] {
            char text[kTextBytes];
            uint64_t sum = 0;
            for (double value : *shared)
                sum += size_t(std::to_chars(text, text + sizeof(text), value).ptr - text) + uint8_t(text[0]);
            gSink = gSink + sum;
        },
        .check = [shared](const char* name) {
            for (double value : *shared) {
                char prax[kPrF64TextMaxBytes];
                checkShortest(name, value, std::string(prax, prF64Format(value, prax)));
            }
        },
    };
}

// Formatting a float as its shortest text, by prF32Format.
Case formatShortestF32(const char* name, std::vector<float> values) {
    std::shared_ptr<std::vector<float>> shared = std::make_shared<std::vector<float>>(std::move(values));
    return Case{
        .name = name,
        .prax = [shared] {
            char text[kPrF64TextMaxBytes];
            uint64_t sum = 0;
            for (float value : *shared)
                sum += prF32Format(value, text) + uint8_t(text[0]);
            gSink = gSink + sum;
        },
        .theirs = [shared] {
            char text[kTextBytes];
            uint64_t sum = 0;
            for (float value : *shared)
                sum += size_t(std::to_chars(text, text + sizeof(text), value).ptr - text) + uint8_t(text[0]);
            gSink = gSink + sum;
        },
        .check = [shared](const char* name) {
            for (float value : *shared) {
                char prax[kPrF64TextMaxBytes];
                checkShortest(name, value, std::string(prax, prF32Format(value, prax)));
                // pr::format writes the same digits in a notation of its own.
                checkShortest(name, value, writeFloat(value));
            }
        },
    };
}

// Formatting a double correctly rounded by pr::formatFloat, under `spec`, whose type is f or e, as std's `format` does
// with the same precision. The two write the same text.
Case formatRounded(const char* name, std::vector<double> values, FormatSpec spec) {
    std::shared_ptr<std::vector<double>> shared = std::make_shared<std::vector<double>>(std::move(values));
    std::chars_format format = spec.type == 'f' ? std::chars_format::fixed : std::chars_format::scientific;
    return Case{
        .name = name,
        .prax = [shared, spec] {
            char text[kTextBytes];
            uint64_t sum = 0;
            for (double value : *shared) {
                Writer writer = bufferWriter(Span<char>(text, uint32_t(sizeof(text))));
                formatFloat(writer, value, spec);
                sum += writer.size + uint8_t(text[0]);
            }
            gSink = gSink + sum;
        },
        .theirs = [shared, spec, format] {
            char text[kTextBytes];
            uint64_t sum = 0;
            for (double value : *shared) {
                char* end = std::to_chars(text, text + sizeof(text), value, format, spec.precision).ptr;
                sum += size_t(end - text) + uint8_t(text[0]);
            }
            gSink = gSink + sum;
        },
        .check = [shared, spec, format](const char* name) {
            for (double value : *shared) {
                char theirs[kTextBytes];
                std::string b(theirs,
                    std::to_chars(theirs, theirs + sizeof(theirs), value, format, spec.precision).ptr);
                std::string a = writeFloat(value, spec);
                if (a != b)
                    mismatch(name, b, a, b);
            }
        },
    };
}

// Formatting a double by `prax`, one of the C API's forms that round the shortest digits, beside std's `format` with
// `precision`. Not checked.
Case formatReRounded(const char* name, std::vector<double> values, size_t (*prax)(double, uint32_t, char*),
        uint32_t digits, std::chars_format format) {
    std::shared_ptr<std::vector<double>> shared = std::make_shared<std::vector<double>>(std::move(values));
    return Case{
        .name = name,
        .prax = [shared, prax, digits] {
            char text[kPrF64FixedMaxBytes > kPrF64TextMaxBytes ? kPrF64FixedMaxBytes : kPrF64TextMaxBytes];
            uint64_t sum = 0;
            for (double value : *shared)
                sum += prax(value, digits, text) + uint8_t(text[0]);
            gSink = gSink + sum;
        },
        .theirs = [shared, digits, format] {
            char text[kTextBytes];
            uint64_t sum = 0;
            for (double value : *shared) {
                char* end = std::to_chars(text, text + sizeof(text), value, format, int(digits)).ptr;
                sum += size_t(end - text) + uint8_t(text[0]);
            }
            gSink = gSink + sum;
        },
        .check = [](const char*) {},
    };
}

// Parsing a uint64_t's decimal text.
Case parseU64(const char* name, std::vector<uint64_t> values) {
    std::shared_ptr<Texts> shared = std::make_shared<Texts>(integerTexts(values));
    return Case{
        .name = name,
        .prax = [shared] {
            uint64_t sum = 0;
            for (uint32_t i = 0; i < shared->count(); ++i) {
                uint64_t value = 0;
                prU64Parse(shared->data(i), shared->size(i), &value, 10);
                sum += value;
            }
            gSink = gSink + sum;
        },
        .theirs = [shared] {
            uint64_t sum = 0;
            for (uint32_t i = 0; i < shared->count(); ++i) {
                uint64_t value = 0;
                std::from_chars(shared->data(i), shared->data(i) + shared->size(i), value);
                sum += value;
            }
            gSink = gSink + sum;
        },
        .check = [shared](const char* name) {
            for (uint32_t i = 0; i < shared->count(); ++i) {
                uint64_t a = 0;
                uint64_t b = 0;
                prU64Parse(shared->data(i), shared->size(i), &a, 10);
                std::from_chars(shared->data(i), shared->data(i) + shared->size(i), b);
                if (a != b)
                    mismatch(name, std::string(shared->data(i), shared->size(i)), std::to_string(a), std::to_string(b));
            }
        },
    };
}

// Parsing a double's text by prF64ParsePrefix.
Case parseF64(const char* name, Texts texts) {
    std::shared_ptr<Texts> shared = std::make_shared<Texts>(std::move(texts));
    return Case{
        .name = name,
        .prax = [shared] {
            uint64_t sum = 0;
            for (uint32_t i = 0; i < shared->count(); ++i) {
                double value = 0;
                prF64ParsePrefix(PrStringView{shared->data(i), shared->size(i)}, &value);
                sum += bitsOf(value);
            }
            gSink = gSink + sum;
        },
        .theirs = [shared] {
            uint64_t sum = 0;
            for (uint32_t i = 0; i < shared->count(); ++i) {
                double value = 0;
                std::from_chars(shared->data(i), shared->data(i) + shared->size(i), value);
                sum += bitsOf(value);
            }
            gSink = gSink + sum;
        },
        .check = [shared](const char* name) {
            for (uint32_t i = 0; i < shared->count(); ++i) {
                double a = 0;
                double b = 0;
                size_t read = prF64ParsePrefix(PrStringView{shared->data(i), shared->size(i)}, &a);
                std::from_chars(shared->data(i), shared->data(i) + shared->size(i), b);
                if (read != shared->size(i) || bitsOf(a) != bitsOf(b)) {
                    char texts[2][kTextBytes];
                    std::string x(texts[0], std::to_chars(texts[0], texts[0] + kTextBytes, a).ptr);
                    std::string y(texts[1], std::to_chars(texts[1], texts[1] + kTextBytes, b).ptr);
                    mismatch(name, std::string(shared->data(i), shared->size(i)), x, y);
                }
            }
        },
    };
}

std::vector<Case> cases() {
    // Each input set has a seed of its own, so that adding a set leaves the others as they are.
    std::vector<double> random = doubles(Kind_Random, 1);
    std::vector<double> unit = doubles(Kind_Unit, 2);
    std::vector<double> shortValues = doubles(Kind_Short, 3);
    std::vector<double> integral = doubles(Kind_Integer, 4);
    std::vector<uint64_t> small = integers(3, 5);
    std::vector<uint64_t> wide = integers(20, 6);
    std::vector<uint64_t> mixed = integers(-1, 7);

    std::vector<Case> out;
    out.push_back(formatU64("format u64 small", small));
    out.push_back(formatU64("format u64 random", wide));
    out.push_back(formatU64("format u64 mixed", mixed));
    out.push_back(formatShortest("format f64 random", random));
    out.push_back(formatShortest("format f64 unit", unit));
    out.push_back(formatShortest("format f64 short", shortValues));
    out.push_back(formatShortest("format f64 integer", integral));
    out.push_back(formatShortestF32("format f32 random", floats(Kind_Random, 8)));
    out.push_back(formatShortestF32("format f32 short", floats(Kind_Short, 9)));
    out.push_back(formatRounded("format f64 .2f short", shortValues, FormatSpec{.precision = 2, .type = 'f'}));
    out.push_back(formatRounded("format f64 .6f unit", unit, FormatSpec{.precision = 6, .type = 'f'}));
    out.push_back(formatRounded("format f64 .16e unit", unit, FormatSpec{.precision = 16, .type = 'e'}));
    out.push_back(formatRounded("format f64 .16e random", random, FormatSpec{.precision = 16, .type = 'e'}));
    out.push_back(formatReRounded("format f64 fixed 2 short", shortValues, prF64FormatFixed, 2,
        std::chars_format::fixed));
    out.push_back(formatReRounded("format f64 significant 6 unit", unit, prF64FormatSignificant, 6,
        std::chars_format::general));
    out.push_back(parseU64("parse u64 small", small));
    out.push_back(parseU64("parse u64 random", wide));
    out.push_back(parseF64("parse f64 random", doubleTexts(random)));
    out.push_back(parseF64("parse f64 unit", doubleTexts(unit)));
    out.push_back(parseF64("parse f64 short", doubleTexts(shortValues)));
    out.push_back(parseF64("parse f64 integer", doubleTexts(integral)));
    // 25 significant digits, past the 19 that fit in a uint64_t.
    out.push_back(parseF64("parse f64 long", doubleTexts(unit, 24)));
    return out;
}

bool selected(const std::string& name, const std::vector<const char*>& prefixes) {
    if (prefixes.empty())
        return true;
    for (const char* prefix : prefixes) {
        if (name.compare(0, strlen(prefix), prefix) == 0)
            return true;
    }
    return false;
}

void writeRow(bool first, const char* engine, const std::string& name, const HarnessCounters& cost) {
    PrTextBuffer row{};
    prTextBufferAppend(&row, first ? "\n  {\"engine\": " : ",\n  {\"engine\": ");
    harnessWriteString(&row, engine);
    prTextBufferAppend(&row, ", \"name\": ");
    harnessWriteString(&row, name);
    harnessAppendFormat(&row, ", \"time\": %.9g", cost.millis);
    HarnessCounts counts{};
    harnessCountsAdd(&counts, "time", cost);
    harnessCountsWrite(&counts, &row);
    prTextBufferAppend(&row, "}");
    printf("%.*s", int(row.size), row.data);
    prTextBufferReset(&row);
}

} // namespace

int main(int argc, char** argv) {
    prCrashHandler();
    const char* repeated = nullptr;
    const char* repeatedEngine = "prax";
    std::vector<const char*> prefixes;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--repeat") == 0 && i + 1 < argc)
            repeated = argv[++i];
        else if (strcmp(argv[i], "--engine") == 0 && i + 1 < argc)
            repeatedEngine = argv[++i];
        else
            prefixes.push_back(argv[i]);
    }

    std::vector<Case> all = cases();
    if (repeated != nullptr) {
        for (Case& test : all) {
            if (test.name == repeated) {
                harnessRepeat(strcmp(repeatedEngine, "std") == 0 ? test.theirs : test.prax);
                return 0;
            }
        }
        fprintf(stderr, "no case named %s\n", repeated);
        return 1;
    }

    printf("[");
    bool first = true;
    for (Case& test : all) {
        if (!selected(test.name, prefixes))
            continue;
        test.check(test.name.c_str());
        writeRow(first, "prax", test.name, harnessEach(harnessMeasure(test.prax), kCount));
        writeRow(false, "std", test.name, harnessEach(harnessMeasure(test.theirs), kCount));
        first = false;
    }
    printf("\n]\n");
    return failed ? 1 : 0;
}
