// Harnesses.
//
// Code shared by the harness and benchmark programs.

#pragma once

#include "pr/string_view.h"
#include "pr/text_buffer.h"
#include "pr/time.h"

PR_ASSUME_NONNULL_BEGIN

namespace harness {

// The maximum number of fields, and of bytes, in the header line of a case.
constexpr uint32_t kHarnessMaxFields = 4;
constexpr uint32_t kHarnessMaxHeaderBytes = 256;

// One case read from standard input: a header line with a byte count and the fields of the case, followed by that many
// bytes.
struct HarnessCase {
    PrTextBuffer input;                      // the bytes the header counts
    char header[kHarnessMaxHeaderBytes];     // the header line, each field NUL terminated in place
    PrStringView fields[kHarnessMaxFields];  // the words after the byte count, in `header`
    uint32_t fieldCount;
};

// Reads the whole file at `path` into `out`, replacing its contents. Returns false if the file cannot be read.
bool harnessReadFile(PrTextBuffer* out, const char* path);

// Returns the name of the file at `path`, without its directory.
PrStringView harnessFileName(const char* path);

// Reads the next case from standard input into `test`. Returns false at the end of the input or on a malformed header.
bool harnessReadCase(HarnessCase* test);

// Frees the input of `test`.
void harnessCaseReset(HarnessCase* test);

// Writes `text` and a newline to standard output.
void harnessWriteLine(PrStringView text);

// Appends `text` to `out` as a JSON string. A surrogate in its UTF-8 form is written as its \u escape.
void harnessWriteString(PrTextBuffer* out, PrStringView text);

// Appends the decimal digits of `value` to `out`.
void harnessWriteNumber(PrTextBuffer* out, uint64_t value);

// Appends what printf writes for `format` and its arguments to `out`.
void harnessAppendFormat(PrTextBuffer* out, const char* format, ...) PR_PRINTF_FORMAT(2, 3);

// Reads every byte of `text` once, 64 at a time, and returns the bytes ORed into one.
uint8_t harnessReadBound(PrStringView text);

// Counters for what the process has spent so far. They hold the instructions retired and cycles spent by the process's
// threads, including work that the kernel did on their behalf, the part of each that ran on performance cores, the
// energy that the CPUs used, and the time on the monotonic clock.
//
// Measurements are compared by instructions, because instruction counts do not change with the clock speed, with the
// kind of core that ran the work, or with other work on the machine. Cycles and time change with all three. If the
// system does not count them, every count is zero.
struct HarnessCounters {
    double instructions;
    double cycles;
    double performanceInstructions; // the part of `instructions` retired on performance cores
    double performanceCycles;
    double energy;                  // millijoules
    double millis;                  // on the monotonic clock
    double gpu;                     // milliseconds that a device spent executing, if the caller counts them
    double cpu;                     // milliseconds that the process's threads ran on cores, summed over the threads
};

// Returns the current counters.
HarnessCounters harnessCounters();

// Return what was spent between two readings, the sum of two spends, and one spend divided among `count` calls.
HarnessCounters harnessSpent(const HarnessCounters& from, const HarnessCounters& to);
HarnessCounters harnessSum(const HarnessCounters& a, const HarnessCounters& b);
HarnessCounters harnessEach(const HarnessCounters& spent, double count);

// Returns the cost of one call of `run`.
//
// A first call fills the caches, so that later calls find them full. Batches of calls then grow until one batch retires
// enough instructions that the cost of reading the counters is lost in it, or runs long enough that the cost must be
// lost in it. Calls that wait for a device reach the second limit. If the system counts no instructions, the batches
// grow until one runs long past the clock's resolution. The cost of the last batch is divided by its size.
template <typename Run>
HarnessCounters harnessMeasure(Run run) {
    constexpr double kBatchInstructions = 5e6;
    constexpr double kCountedMillis = 20.0;
    constexpr double kBatchMillis = 20.0;
    constexpr uint32_t kMaxBatch = 1u << 20;
    run();
    uint32_t batch = 1;
    for (;;) {
        HarnessCounters from = harnessCounters();
        for (uint32_t i = 0; i < batch; ++i)
            run();
        HarnessCounters spent = harnessSpent(from, harnessCounters());
        bool counted = spent.instructions > 0;
        double reached = counted ? spent.instructions / kBatchInstructions : spent.millis / kBatchMillis;
        if (counted && spent.millis / kCountedMillis > reached)
            reached = spent.millis / kCountedMillis;
        if (reached >= 1 || batch >= kMaxBatch)
            return harnessEach(spent, batch);
        // The next batch aims a little past the mark, based on what this batch reached.
        double grow = reached > 0 ? 1.25 / reached : 16;
        double next = double(batch) * (grow < 2 ? 2 : grow > 1024 ? 1024 : grow);
        batch = next < double(kMaxBatch) ? uint32_t(next) : kMaxBatch;
    }
}

// The longest, in seconds, that a harness repeats one case for a profiler to sample (harnessRepeat). It is longer than
// any recording that ./dev.sh profile makes, and it ends a run that is left behind.
constexpr double kHarnessRepeatSeconds = 300;

// Measures `run` over and over, as harnessMeasure does, for kHarnessRepeatSeconds, so that a profiler can sample one
// case of a harness. Then returns.
template <typename Run>
void harnessRepeat(Run run) {
    double end = harnessCounters().millis + kHarnessRepeatSeconds * 1000;
    while (harnessCounters().millis < end)
        harnessMeasure(run);
}

// The counts of measured costs, keyed by the name of what each measures. A benchmark's row carries them as its "counts"
// member: {"name": {"instructions": i, "cycles": c, "performanceInstructions": pi, "performanceCycles": pc, "energy":
// mJ, "time": ms, "gpu": ms}, ...}. The "gpu" member is present only if a device worked. The time is also a member of
// the row itself, under the name, and a browser's row has that member too.
struct HarnessCounts {
    PrTextBuffer text;
};

// Adds `cost` to the counts under `name`.
void harnessCountsAdd(HarnessCounts* counts, PrStringView name, const HarnessCounters& cost);

// Appends the counts to `out` as the row's "counts" member, preceded by a comma, and frees them.
void harnessCountsWrite(HarnessCounts* counts, PrTextBuffer* out);

// Prints the row of a case in a JSON array of rows, preceded by a comma unless it is the first row. The row holds the
// case's name, its time, and the counts of its cost under "time".
void harnessWriteCase(bool first, const char* name, const HarnessCounters& cost);

} // namespace harness

PR_ASSUME_NONNULL_END
