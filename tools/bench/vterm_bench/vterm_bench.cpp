// The vterm benchmark, which ./dev.sh bench vterm runs. It writes the same program output, the workloads of
// vterm_workloads.h, to a terminal of each engine (vterm_engine.h): lib/vterm and libvterm. Both terminals have the
// same extent and scrollback.
//
// A measure writes a workload's bytes in chunks of 64 KiB, as the Studio terminal reads a program's output. The first
// measured call warms up the terminal. The terminal's answers to the program are dropped. The resize workload measures
// one resize, alternating between the two widths.
//
// A row is an engine's measure of one workload: {"engine", "name", "bytes", "time", "counts"}. It also holds what one
// step of the workload leaves in a new terminal. {"lineBytes"} is the bytes that the terminal's lines hold, per line of
// its screen and scrollback. For the comparison, {"rows", "sameRows"} counts the screen's rows and how many of them
// show the same text as the PrVterm's. The rows are written as a JSON array.
//
// Usage: vterm_bench [--repeat WORKLOAD [--engine NAME]] [WORKLOAD]...
//
// Measures the workloads whose names start with one of the WORKLOAD arguments, or every workload. --repeat writes one
// workload to the named engine, prax by default, over and over for five minutes (kHarnessRepeatSeconds), so that a
// profiler can sample it. It prints nothing.

#include "pr/assert.h"
#include "pr/raw_string.h"

#include "tools/bench/vterm_bench/libvterm_engine.h"
#include "tools/bench/vterm_bench/prax_engine.h"
#include "tools/bench/vterm_bench/vterm_workloads.h"
#include "tools/harness/harness.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace pr;
using namespace harness;
using namespace vterm_bench;

namespace {

// The size of the chunks in which a workload is written, as one read of a program's output.
constexpr size_t kReadBytes = 64u << 10;

void writeAll(Engine& engine, const std::string& bytes) {
    const uint8_t* data = reinterpret_cast<const uint8_t*>(bytes.data());
    for (size_t at = 0; at < bytes.size(); at += kReadBytes) {
        size_t size = bytes.size() - at < kReadBytes ? bytes.size() - at : kReadBytes;
        engine.write(PrByteView{data + at, size});
    }
}

// The sum of the code points that the scrollback reads return. Keeping it stops the compiler from dropping the reads.
uint64_t readSum = 0;

// A workload in an engine's terminal. `narrow` is true while a resize workload's terminal is kNarrowColumns wide.
struct Run {
    Engine* engine;
    const Workload* workload;
    bool narrow;
};

Run start(Engine& engine, const Workload& workload) {
    engine.create(workload.extent, kScrollback);
    if (workload.kind != WorkloadKind_Write)
        writeAll(engine, workload.bytes);
    return Run{.engine = &engine, .workload = &workload, .narrow = false};
}

void step(Run& run) {
    if (run.workload->kind == WorkloadKind_Write) {
        writeAll(*run.engine, run.workload->bytes);
        return;
    }
    if (run.workload->kind == WorkloadKind_Read) {
        uint64_t sum = 0;
        for (uint32_t i = 0; i < run.engine->scrollbackCount(); ++i)
            sum += run.engine->scrollbackLineRead(i);
        readSum += sum;
        return;
    }
    run.narrow = !run.narrow;
    PrUVec2 extent = run.workload->extent;
    if (run.narrow)
        extent.x = kNarrowColumns;
    run.engine->resize(extent);
}

// Returns the text of a terminal's line, without trailing spaces.
std::string lineText(const Engine& engine, uint32_t index, uint32_t columns) {
    PrRawString text = prRawStringEmpty();
    engine.lineAppendText(index, columns, &text);
    PrStringView view = prRawStringView(&text);
    std::string out(view.data, view.size);
    prRawStringReset(&text);
    while (!out.empty() && out.back() == ' ')
        out.pop_back();
    return out;
}

// A terminal's state after one step of a workload: the text of its screen's rows, and the bytes that its lines hold per
// line of its screen and scrollback.
struct Screen {
    std::vector<std::string> rows;
    double lineBytes;
};

Screen screenAfter(Engine& engine, const Workload& workload) {
    Run run = start(engine, workload);
    step(run);
    PrUVec2 extent = workload.extent;
    if (run.narrow)
        extent.x = kNarrowColumns;
    uint32_t scrollback = engine.scrollbackCount();
    Screen screen{.lineBytes = double(engine.linesMemorySize()) / double(scrollback + extent.y)};
    for (uint32_t row = 0; row < extent.y; ++row)
        screen.rows.push_back(lineText(engine, scrollback + row, extent.x));
    engine.destroy();
    return screen;
}

bool firstRow = true;

// Writes the row for `engine` on `workload`, whose terminal ended as `screen`. If `reference` is not null, it is the
// PrVterm's screen, and the row compares `screen` with it.
void writeRow(const Engine& engine, const Workload& workload, const HarnessCounters& cost, const Screen& screen,
        const Screen* PR_NULLABLE reference) {
    PrTextBuffer row{};
    prTextBufferAppend(&row, firstRow ? "\n  {\"engine\": " : ",\n  {\"engine\": ");
    firstRow = false;
    harnessWriteString(&row, engine.name());
    prTextBufferAppend(&row, ", \"name\": ");
    harnessWriteString(&row, workload.name);
    uint64_t bytes = workload.kind == WorkloadKind_Write ? workload.bytes.size() : 0;
    harnessAppendFormat(&row, ", \"bytes\": %llu, \"time\": %.6f", static_cast<unsigned long long>(bytes), cost.millis);
    harnessAppendFormat(&row, ", \"lineBytes\": %.1f", screen.lineBytes);
    if (reference != nullptr) {
        uint32_t same = 0;
        for (size_t i = 0; i < screen.rows.size() && i < reference->rows.size(); ++i)
            same += screen.rows[i] == reference->rows[i] ? 1 : 0;
        harnessAppendFormat(&row, ", \"rows\": %zu, \"sameRows\": %u", reference->rows.size(), same);
    }
    HarnessCounts counts{};
    harnessCountsAdd(&counts, "time", cost);
    harnessCountsWrite(&counts, &row);
    prTextBufferAppend(&row, "}");
    fwrite(row.data, 1, row.size, stdout);
    prTextBufferReset(&row);
}

Engine* engineFromName(const std::vector<std::unique_ptr<Engine>>& engines, const char* name) {
    for (const std::unique_ptr<Engine>& engine : engines) {
        if (strcmp(engine->name(), name) == 0)
            return engine.get();
    }
    fprintf(stderr, "no engine named %s\n", name);
    exit(2);
}

bool chosen(const Workload& workload, const std::vector<const char*>& names) {
    if (names.empty())
        return true;
    for (const char* name : names) {
        if (workload.name.compare(0, strlen(name), name) == 0)
            return true;
    }
    return false;
}

} // namespace

int main(int argc, char** argv) {
    prCrashHandler();
    // The first engine is lib/vterm, whose screens the other engines' screens are compared with.
    std::vector<std::unique_ptr<Engine>> engines;
    engines.push_back(makePrax());
    engines.push_back(makeLibvterm());
    Engine& prax = *engines[0];

    std::vector<const char*> names;
    const char* repeated = nullptr;
    Engine* repeatedEngine = &prax;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--repeat") == 0 && i + 1 < argc)
            repeated = argv[++i];
        else if (strcmp(argv[i], "--engine") == 0 && i + 1 < argc)
            repeatedEngine = engineFromName(engines, argv[++i]);
        else
            names.push_back(argv[i]);
    }
    std::vector<Workload> all = workloads();
    if (repeated != nullptr) {
        for (const Workload& workload : all) {
            if (workload.name.compare(0, strlen(repeated), repeated) != 0)
                continue;
            Run run = start(*repeatedEngine, workload);
            harnessRepeat([&] { step(run); });
            repeatedEngine->destroy();
            return 0;
        }
        fprintf(stderr, "no workload named %s\n", repeated);
        return 2;
    }

    printf("[");
    for (const Workload& workload : all) {
        if (!chosen(workload, names))
            continue;
        Screen reference = screenAfter(prax, workload);
        for (const std::unique_ptr<Engine>& engine : engines) {
            Run run = start(*engine, workload);
            HarnessCounters cost = harnessMeasure([&] { step(run); });
            engine->destroy();
            if (engine.get() == &prax) {
                writeRow(*engine, workload, cost, reference, nullptr);
                continue;
            }
            Screen screen = screenAfter(*engine, workload);
            writeRow(*engine, workload, cost, screen, &reference);
        }
    }
    printf("\n]\n");
    return 0;
}
