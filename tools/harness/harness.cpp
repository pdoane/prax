#include "tools/harness/harness.h"

#include "pr/integer.h"
#include "pr/memory.h"
#include "pr/string_ops.h"

#include "tools/harness/cpu_counters.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif

using namespace pr;

namespace harness {
namespace {

// The size of one read from a file, in bytes.
constexpr size_t kReadChunk = 64 * 1024;

// Returns true if the bytes at `at` encode a surrogate in UTF-8 form: ED, then A0 through BF.
bool isSurrogate(PrStringView text, size_t at) {
    return at + 2 < text.size && uint8_t(text.data[at]) == 0xed && (uint8_t(text.data[at + 1]) & 0xe0) == 0xa0 &&
        (uint8_t(text.data[at + 2]) & 0xc0) == 0x80;
}

uint32_t surrogateAt(PrStringView text, size_t at) {
    return 0xd000u | uint32_t(uint8_t(text.data[at + 1]) & 0x3f) << 6 | uint32_t(uint8_t(text.data[at + 2]) & 0x3f);
}

void writeEscape(PrTextBuffer* out, uint32_t unit) {
    char escape[8];
    int width = snprintf(escape, sizeof(escape), "\\u%04x", unit);
    prTextBufferAppend(out, PrStringView{escape, uint32_t(width)});
}

} // namespace

bool harnessReadFile(PrTextBuffer* out, const char* path) {
    FILE* file = fopen(path, "rb");
    if (file == nullptr)
        return false;

    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);

    prTextBufferClear(out);
    if (size > 0)
        prTextBufferReserve(out, uint32_t(size));
    char chunk[kReadChunk];
    size_t got;
    while ((got = fread(chunk, 1, sizeof(chunk), file)) != 0)
        prTextBufferAppend(out, PrStringView{chunk, uint32_t(got)});
    bool read = size >= 0 && ferror(file) == 0;
    fclose(file);
    return read;
}

PrStringView harnessFileName(const char* path) {
    const char* name = path;
    // Windows separates the parts of a path with either slash.
#if defined(_WIN32)
    const bool backslashes = true;
#else
    const bool backslashes = false;
#endif
    for (const char* at = path; *at != '\0'; ++at) {
        if (*at == '/' || (backslashes && *at == '\\'))
            name = at + 1;
    }
    return name;
}

bool harnessReadCase(HarnessCase* test) {
    if (fgets(test->header, sizeof(test->header), stdin) == nullptr)
        return false;

    char* at = test->header;
    long size = strtol(at, &at, 10);
    if (size < 0)
        return false;

    test->fieldCount = 0;
    for (;;) {
        while (*at == ' ')
            ++at;
        if (*at == '\n' || *at == '\0')
            break;
        if (test->fieldCount == kHarnessMaxFields)
            return false;

        char* start = at;
        while (*at != ' ' && *at != '\n' && *at != '\0')
            ++at;
        test->fields[test->fieldCount++] = PrStringView{start, size_t(at - start)};
        if (*at != '\0')
            *at++ = '\0';
    }

    prTextBufferClear(&test->input);
    prTextBufferReserve(&test->input, uint32_t(size));
    char chunk[kReadChunk];
    for (size_t left = size_t(size); left != 0;) {
        size_t want = left < sizeof(chunk) ? left : sizeof(chunk);
        size_t got = fread(chunk, 1, want, stdin);
        if (got == 0)
            return false;
        prTextBufferAppend(&test->input, PrStringView{chunk, uint32_t(got)});
        left -= got;
    }
    return true;
}

void harnessCaseReset(HarnessCase* test) {
    prTextBufferReset(&test->input);
}

void harnessWriteLine(PrStringView text) {
    fwrite(text.data, 1, text.size, stdout);
    fputc('\n', stdout);
}

void harnessWriteString(PrTextBuffer* out, PrStringView text) {
    prTextBufferAppendChar(out, '"');
    for (size_t i = 0; i < text.size; ++i) {
        if (isSurrogate(text, i)) {
            writeEscape(out, surrogateAt(text, i));
            i += 2;
            continue;
        }

        char c = text.data[i];
        switch (c) {
            case '"':  prTextBufferAppend(out, "\\\""); break;
            case '\\': prTextBufferAppend(out, "\\\\"); break;
            case '\n': prTextBufferAppend(out, "\\n"); break;
            case '\r': prTextBufferAppend(out, "\\r"); break;
            case '\t': prTextBufferAppend(out, "\\t"); break;
            default:
                if (uint8_t(c) < 0x20)
                    writeEscape(out, uint8_t(c));
                else
                    prTextBufferAppendChar(out, c);
        }
    }
    prTextBufferAppendChar(out, '"');
}

void harnessWriteNumber(PrTextBuffer* out, uint64_t value) {
    char digits[kPrU64TextMaxBytes];
    prTextBufferAppend(out, PrStringView{digits, uint32_t(prU64Format(value, digits, 10))});
}

void harnessAppendFormat(PrTextBuffer* out, const char* format, ...) {
    va_list args;
    va_start(args, format);
    char small[512];
    int width = vsnprintf(small, sizeof(small), format, args);
    va_end(args);
    if (width < 0)
        return;
    if (size_t(width) < sizeof(small)) {
        prTextBufferAppend(out, PrStringView{small, uint32_t(width)});
        return;
    }
    char* large = static_cast<char*>(prMemoryAlloc(size_t(width) + 1));
    va_start(args, format);
    vsnprintf(large, size_t(width) + 1, format, args);
    va_end(args);
    prTextBufferAppend(out, PrStringView{large, uint32_t(width)});
    prMemoryFree(large);
}

uint8_t harnessReadBound(PrStringView text) {
    const uint8_t* bytes = prStringBytes(text);
    size_t i = 0;

#if defined(__ARM_NEON)
    uint8x16_t f0 = vdupq_n_u8(0);
    uint8x16_t f1 = f0;
    uint8x16_t f2 = f0;
    uint8x16_t f3 = f0;
    for (; i + 64 <= text.size; i += 64) {
        f0 = vorrq_u8(f0, vld1q_u8(bytes + i));
        f1 = vorrq_u8(f1, vld1q_u8(bytes + i + 16));
        f2 = vorrq_u8(f2, vld1q_u8(bytes + i + 32));
        f3 = vorrq_u8(f3, vld1q_u8(bytes + i + 48));
    }
    uint8_t fold = vmaxvq_u8(vorrq_u8(vorrq_u8(f0, f1), vorrq_u8(f2, f3)));
#else
    uint8_t fold = 0;
#endif

    for (; i < text.size; ++i)
        fold |= bytes[i];
    return fold;
}

HarnessCounters harnessCounters() {
    CpuCounters cpu = cpuCounters();
    return HarnessCounters{
        cpu.instructions, cpu.cycles, cpu.performanceInstructions, cpu.performanceCycles,
        cpu.energy, prTimeMonotonicMilliseconds(), 0, cpu.cpuMillis,
    };
}

HarnessCounters harnessSpent(const HarnessCounters& from, const HarnessCounters& to) {
    return HarnessCounters{
        to.instructions - from.instructions, to.cycles - from.cycles,
        to.performanceInstructions - from.performanceInstructions,
        to.performanceCycles - from.performanceCycles, to.energy - from.energy,
        to.millis - from.millis, to.gpu - from.gpu, to.cpu - from.cpu,
    };
}

HarnessCounters harnessSum(const HarnessCounters& a, const HarnessCounters& b) {
    return HarnessCounters{
        a.instructions + b.instructions, a.cycles + b.cycles,
        a.performanceInstructions + b.performanceInstructions,
        a.performanceCycles + b.performanceCycles, a.energy + b.energy, a.millis + b.millis,
        a.gpu + b.gpu, a.cpu + b.cpu,
    };
}

HarnessCounters harnessEach(const HarnessCounters& spent, double count) {
    return HarnessCounters{
        spent.instructions / count, spent.cycles / count, spent.performanceInstructions / count,
        spent.performanceCycles / count, spent.energy / count, spent.millis / count,
        spent.gpu / count, spent.cpu / count,
    };
}

void harnessCountsAdd(HarnessCounts* counts, PrStringView name, const HarnessCounters& cost) {
    PrTextBuffer* out = &counts->text;
    if (out->size != 0)
        prTextBufferAppend(out, ", ");
    harnessWriteString(out, name);
    harnessAppendFormat(out, ": {\"instructions\": %.0f, \"cycles\": %.0f, \"performanceInstructions\": %.0f, "
        "\"performanceCycles\": %.0f, \"energy\": %.4f, \"time\": %.6f",
        cost.instructions, cost.cycles, cost.performanceInstructions, cost.performanceCycles, cost.energy, cost.millis);
    if (cost.gpu > 0)
        harnessAppendFormat(out, ", \"gpu\": %.6f", cost.gpu);
    prTextBufferAppend(out, "}");
}

void harnessCountsWrite(HarnessCounts* counts, PrTextBuffer* out) {
    prTextBufferAppend(out, ", \"counts\": {");
    prTextBufferAppend(out, prTextBufferView(&counts->text));
    prTextBufferAppend(out, "}");
    prTextBufferReset(&counts->text);
}

void harnessWriteCase(bool first, const char* name, const HarnessCounters& cost) {
    PrTextBuffer row{};
    prTextBufferAppend(&row, first ? "\n  {\"name\": " : ",\n  {\"name\": ");
    harnessWriteString(&row, name);
    harnessAppendFormat(&row, ", \"time\": %.6f", cost.millis);
    HarnessCounts counts{};
    harnessCountsAdd(&counts, "time", cost);
    harnessCountsWrite(&counts, &row);
    prTextBufferAppend(&row, "}");
    fwrite(row.data, 1, row.size, stdout);
    prTextBufferReset(&row);
}

} // namespace harness
