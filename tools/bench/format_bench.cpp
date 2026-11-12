// format_bench: what formatting with lib/core's format strings (pr/format.h) costs, beside the C++ standard library's
// <format> with the same format strings and arguments. ./dev.sh bench format runs it.
//
// pr/format.h parses its format string at run time and formats each argument through one function per type, where
// <format> checks the string at compile time and instantiates its formatting for each call's types. A case formats each
// of a set of inputs that a fixed seed makes, so that both engines format the same values: integers, floats, text with
// padding, and lines that mix them as a log does. The buffer cases write into a buffer on the stack, and the string
// cases make a String or a std::string, so that they pay for the allocation too. A row is one engine's cost of one
// call: {"engine", "name", "time", "counts"}, written as a JSON array.
//
// Before a case is measured, each of prax's results is checked against std's where their specs mean the same text. A
// mismatch is reported, and fails the run.
//
// Usage: format_bench [--repeat CASE [--engine NAME]] [CASE]...
//
// Measures the cases whose names start with one of the CASE arguments, or every case. --repeat runs one case on the
// named engine, prax by default, over and over for kHarnessRepeatSeconds, so that a profiler can sample it. It prints
// nothing.

#include "pr/assert.h"
#include "pr/format.h"
#include "pr/string.h"
#include "pr/text_buffer.h"

#include "tools/harness/harness.h"

#include <cstdio>
#include <cstring>
#include <format>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

using namespace pr;
using namespace harness;

namespace {

// The number of inputs in each set, few enough that they stay in the first levels of the cache.
constexpr uint32_t kCount = 4096;

// Room for any text that a case writes.
constexpr size_t kTextBytes = 128;

// The results of every call are summed here, so that no call is optimized away.
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

// The arguments of one call. Each case reads the members that its format string names.
struct Input {
    int64_t count;
    uint32_t total;
    uint32_t bits;
    double fraction;    // in [0, 1), with all 17 digits
    double amount;      // up to six digits, four after the point
    std::string word;
};

std::vector<Input> inputs() {
    static const char* const kWords[] = {"layout", "style", "paint", "a", "composite", "script", "fetch", "idle"};
    static const double kScale[] = {1, 10, 100, 1000, 10000};
    SplitMix random{1};
    std::vector<Input> out;
    for (uint32_t i = 0; i < kCount; ++i) {
        Input input;
        // Counts of a spread of digits, a tenth of them negative.
        uint64_t magnitude = random.next() >> (random.next() % 64);
        input.count = random.next() % 10 == 0 ? -int64_t(magnitude >> 1) : int64_t(magnitude >> 1);
        input.total = uint32_t(random.next() % 100000);
        input.bits = uint32_t(random.next());
        input.fraction = double(random.next() >> 11) * 0x1p-53;
        input.amount = double(random.next() % 1000000) / kScale[random.next() % 5];
        input.word = kWords[random.next() % 8];
        out.push_back(input);
    }
    return out;
}

// A case: one pass of each engine over the inputs, and a check of prax's text against std's.
struct Case {
    std::string name;
    std::function<void()> prax;
    std::function<void()> theirs;
    bool checked;
    std::function<std::string(const Input&)> praxText;
    std::function<std::string(const Input&)> theirText;
};

bool failed = false;

// A case that formats into a buffer on the stack. `prax` and `theirs` write one input's text into the buffer, and
// return its size.
template <typename Prax, typename Theirs>
Case buffered(const char* name, std::shared_ptr<std::vector<Input>> values, bool checked, Prax prax, Theirs theirs) {
    return Case{
        .name = name,
        .prax = [values, prax] {
            char text[kTextBytes];
            uint64_t sum = 0;
            for (const Input& input : *values)
                sum += prax(input, text) + uint8_t(text[0]);
            gSink = gSink + sum;
        },
        .theirs = [values, theirs] {
            char text[kTextBytes];
            uint64_t sum = 0;
            for (const Input& input : *values)
                sum += theirs(input, text) + uint8_t(text[0]);
            gSink = gSink + sum;
        },
        .checked = checked,
        .praxText = [prax](const Input& input) {
            char text[kTextBytes];
            return std::string(text, prax(input, text));
        },
        .theirText = [theirs](const Input& input) {
            char text[kTextBytes];
            return std::string(text, theirs(input, text));
        },
    };
}

// pr::formatToBuffer's text, as its size.
template <typename... Arguments>
size_t praxBuffer(char* text, PrStringView format, const Arguments&... arguments) {
    return formatToBuffer(Span<char>(text, uint32_t(kTextBytes)), format, arguments...).size;
}

// std::format_to_n's text, as its size.
template <typename... Arguments>
size_t stdBuffer(char* text, std::format_string<Arguments...> format, Arguments&&... arguments) {
    return size_t(std::format_to_n(text, kTextBytes, format, std::forward<Arguments>(arguments)...).out - text);
}

std::vector<Case> cases() {
    std::shared_ptr<std::vector<Input>> values = std::make_shared<std::vector<Input>>(inputs());
    std::vector<Case> out;

    out.push_back(buffered("buffer literal", values, true,
        [](const Input&, char* text) { return praxBuffer(text, "frame complete"); },
        [](const Input&, char* text) { return stdBuffer(text, "frame complete"); }));
    out.push_back(buffered("buffer integer", values, true,
        [](const Input& in, char* text) { return praxBuffer(text, "{}", in.count); },
        [](const Input& in, char* text) { return stdBuffer(text, "{}", in.count); }));
    out.push_back(buffered("buffer integer padded", values, true,
        [](const Input& in, char* text) { return praxBuffer(text, "{:>8}|{:<6}", in.total, in.total); },
        [](const Input& in, char* text) { return stdBuffer(text, "{:>8}|{:<6}", in.total, in.total); }));
    out.push_back(buffered("buffer hex", values, true,
        [](const Input& in, char* text) { return praxBuffer(text, "{:#010x}", in.bits); },
        [](const Input& in, char* text) { return stdBuffer(text, "{:#010x}", in.bits); }));
    // The shortest forms agree on their digits, not on their notation, so they are not checked.
    out.push_back(buffered("buffer float shortest", values, false,
        [](const Input& in, char* text) { return praxBuffer(text, "{}", in.fraction); },
        [](const Input& in, char* text) { return stdBuffer(text, "{}", in.fraction); }));
    out.push_back(buffered("buffer float .2f", values, true,
        [](const Input& in, char* text) { return praxBuffer(text, "{:.2f}", in.amount); },
        [](const Input& in, char* text) { return stdBuffer(text, "{:.2f}", in.amount); }));
    out.push_back(buffered("buffer float .3e", values, true,
        [](const Input& in, char* text) { return praxBuffer(text, "{:.3e}", in.fraction); },
        [](const Input& in, char* text) { return stdBuffer(text, "{:.3e}", in.fraction); }));
    out.push_back(buffered("buffer float .6g", values, true,
        [](const Input& in, char* text) { return praxBuffer(text, "{:.6g}", in.amount); },
        [](const Input& in, char* text) { return stdBuffer(text, "{:.6g}", in.amount); }));
    out.push_back(buffered("buffer text padded", values, true, [](const Input& in, char* text) {
        PrStringView word{in.word.data(), in.word.size()};
        return praxBuffer(text, "[{:<10}][{:^10}][{:>10}]", word, word, word);
    }, [](const Input& in, char* text) {
        std::string_view word = in.word;
        return stdBuffer(text, "[{:<10}][{:^10}][{:>10}]", word, word, word);
    }));
    out.push_back(buffered("buffer log line", values, true, [](const Input& in, char* text) {
        PrStringView word{in.word.data(), in.word.size()};
        return praxBuffer(text, "{}: {} of {} ({:.1f}%) in {:.3f}ms", word, in.count, in.total,
            in.fraction * 100, in.amount);
    }, [](const Input& in, char* text) {
        std::string_view word = in.word;
        return stdBuffer(text, "{}: {} of {} ({:.1f}%) in {:.3f}ms", word, in.count, in.total,
            in.fraction * 100, in.amount);
    }));

    // Strings, which each engine allocates.
    out.push_back(Case{
        .name = "string log line",
        .prax = [values] {
            uint64_t sum = 0;
            for (const Input& in : *values) {
                PrStringView word{in.word.data(), in.word.size()};
                String text = format("{}: {} of {} ({:.1f}%)", word, in.count, in.total, in.fraction * 100);
                sum += text.view().size;
            }
            gSink = gSink + sum;
        },
        .theirs = [values] {
            uint64_t sum = 0;
            for (const Input& in : *values) {
                std::string text = std::format("{}: {} of {} ({:.1f}%)", in.word, in.count, in.total, in.fraction * 100);
                sum += text.size();
            }
            gSink = gSink + sum;
        },
        .checked = true,
        .praxText = [](const Input& in) {
            PrStringView word{in.word.data(), in.word.size()};
            String text = format("{}: {} of {} ({:.1f}%)", word, in.count, in.total, in.fraction * 100);
            return std::string(text.view().data, text.view().size);
        },
        .theirText = [](const Input& in) {
            return std::format("{}: {} of {} ({:.1f}%)", in.word, in.count, in.total, in.fraction * 100);
        },
    });
    return out;
}

void check(const Case& test, const std::vector<Input>& values) {
    if (!test.checked)
        return;
    uint32_t reported = 0;
    for (const Input& input : values) {
        std::string prax = test.praxText(input);
        std::string theirs = test.theirText(input);
        if (prax != theirs) {
            if (reported++ < 10)
                fprintf(stderr, "%s: prax wrote \"%s\" and std \"%s\"\n", test.name.c_str(), prax.c_str(),
                    theirs.c_str());
            failed = true;
        }
    }
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

    std::vector<Input> values = inputs();
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
        check(test, values);
        writeRow(first, "prax", test.name, harnessEach(harnessMeasure(test.prax), kCount));
        writeRow(false, "std", test.name, harnessEach(harnessMeasure(test.theirs), kCount));
        first = false;
    }
    printf("\n]\n");
    return failed ? 1 : 0;
}
