#include "pr/atomic.h"
#include "pr/process.h"
#include "pr/thread.h"
#include "pr/time.h"

#include "tools/harness/test.h"

#include <string>

extern "C" int prTestProcessCApi(void);

namespace pr_impl {

namespace {

// How long a test waits for a process before it fails, in milliseconds.
constexpr double kPatience = 10000;

// A launcher of the system's processes, for the length of a test.
struct Launcher {
    PrProcessLauncher* launcher = prProcessLauncherCreate(PrProcessBackend_Default);

    ~Launcher() {
        prProcessLauncherDestroy(launcher);
    }

    // Starts `program` with `arguments`.
    PrProcess* start(PrStringView program, std::initializer_list<PrStringView> arguments,
            PrProcessErrors errors = PrProcessErrors_Discard, PrStringView directory = {}) {
        PrProcessDesc desc{
            .program = program,
            .arguments = arguments.begin(),
            .argumentCount = uint32_t(arguments.size()),
            .directory = directory,
            .errors = errors,
        };
        PrProcess* process = nullptr;
        REQUIRE(prProcessLaunch(launcher, &desc, &process) == PrStatus_Ok);
        return process;
    }
};

// Reads `stream` until the process exits and its output ends, and returns what it read.
std::string drain(PrProcess* process, PrProcessStream stream = PrProcessStream_Output) {
    std::string text;
    double end = prTimeMonotonicMilliseconds() + kPatience;
    for (;;) {
        uint8_t chunk[4096];
        size_t got = prProcessRead(process, stream, chunk, sizeof(chunk));
        text.append(reinterpret_cast<const char*>(chunk), got);
        PrProcessInfo info = prProcessInfo(process);
        if (got == 0 && info.exited && info.outputEnded)
            break;
        if (got == 0)
            prThreadSleep(1);
        REQUIRE(prTimeMonotonicMilliseconds() < end);
    }
    // Standard error may still hold bytes once the output ends.
    if (stream == PrProcessStream_Error) {
        uint8_t chunk[4096];
        while (size_t got = prProcessRead(process, stream, chunk, sizeof(chunk)))
            text.append(reinterpret_cast<const char*>(chunk), got);
    }
    return text;
}

PrProcessInfo waitForExit(PrProcess* process) {
    double end = prTimeMonotonicMilliseconds() + kPatience;
    while (!prProcessInfo(process).exited) {
        prThreadSleep(1);
        REQUIRE(prTimeMonotonicMilliseconds() < end);
    }
    return prProcessInfo(process);
}

void countWake(void* PR_NULLABLE context) {
    static_cast<Atomic<uint32_t>*>(context)->fetchAdd(1);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("process: the public headers compile as C") {
    CHECK(prTestProcessCApi() == 0);
}

TEST_CASE("process: backends are named, and Default creates the first that the build has") {
#if defined(_WIN32)
    PrProcessBackend system = PrProcessBackend_Windows;
#else
    PrProcessBackend system = PrProcessBackend_Posix;
#endif
    CHECK(prProcessBackendFromName("Windows") == PrProcessBackend_Windows);
    CHECK(prProcessBackendFromName("nothing") == PrProcessBackend_Count);
    CHECK(prProcessBackendName(PrProcessBackend_Posix) == "posix");
    CHECK(prProcessBackendIsBuilt(system));
    CHECK(prProcessBackendIsBuilt(PrProcessBackend_Default));
    PrProcessLauncher* launcher = prProcessLauncherCreate(PrProcessBackend_Default);
    REQUIRE(launcher != nullptr);
    prProcessLauncherDestroy(launcher);
}

#if !defined(_WIN32)

TEST_CASE("process: output arrives, and the exit status follows") {
    Launcher l;
    PrProcess* process = l.start("sh", {"-c", "printf hello; exit 3"});
    CHECK(drain(process) == "hello");
    PrProcessInfo info = prProcessInfo(process);
    CHECK(info.exited);
    CHECK(!info.signaled);
    CHECK(info.code == 3);
    prProcessDestroy(process);
}

TEST_CASE("process: input reaches the process, and closing it ends what the process reads") {
    Launcher l;
    PrProcess* process = l.start("cat", {});
    prProcessWrite(process, PrByteView{reinterpret_cast<const uint8_t*>("one\n"), 4});
    prProcessWrite(process, PrByteView{reinterpret_cast<const uint8_t*>("two\n"), 4});
    prProcessCloseInput(process);
    prProcessWrite(process, PrByteView{reinterpret_cast<const uint8_t*>("dropped"), 7});
    CHECK(drain(process) == "one\ntwo\n");
    CHECK(prProcessInfo(process).code == 0);
    prProcessDestroy(process);
}

TEST_CASE("process: standard error is a stream of its own when it is a pipe") {
    Launcher l;
    PrProcess* process = l.start("sh", {"-c", "printf out; printf err >&2"}, PrProcessErrors_Pipe);
    CHECK(drain(process) == "out");
    CHECK(drain(process, PrProcessStream_Error) == "err");
    prProcessDestroy(process);
}

TEST_CASE("process: output past what a process holds waits for its reader, and none is lost") {
    Launcher l;
    PrProcess* process = l.start("head", {"-c", "3000000", "/dev/zero"});
    // Nothing is read until the process has had time to fill what it may hold.
    prThreadSleep(50);
    CHECK(drain(process).size() == 3000000);
    prProcessDestroy(process);
}

TEST_CASE("process: input larger than a pipe holds arrives whole") {
    Launcher l;
    PrProcess* process = l.start("wc", {"-c"});
    std::string input(1000000, 'x');
    prProcessWrite(process, PrByteView{reinterpret_cast<const uint8_t*>(input.data()), input.size()});
    prProcessCloseInput(process);
    std::string counted = drain(process);
    CHECK(std::stoul(counted) == 1000000);
    prProcessDestroy(process);
}

TEST_CASE("process: a process starts in the directory that it is given") {
    Launcher l;
    PrProcess* process = l.start("pwd", {}, PrProcessErrors_Discard, "/");
    CHECK(drain(process) == "/\n");
    prProcessDestroy(process);
}

TEST_CASE("process: a program that cannot be found is NotFound") {
    PrStringView program = "prax-no-such-program";
    PrProcessDesc desc{.program = program};
    PrProcess* process = reinterpret_cast<PrProcess*>(1);
    Launcher l;
    CHECK(prProcessLaunch(l.launcher, &desc, &process) == PrStatus_NotFound);
    CHECK(process == nullptr);
}

TEST_CASE("process: a killed process ends by its signal") {
    Launcher l;
    PrProcess* process = l.start("sleep", {"30"});
    prProcessKill(process);
    PrProcessInfo info = waitForExit(process);
    CHECK(info.signaled);
    CHECK(info.code == 9);
    prProcessDestroy(process);
}

TEST_CASE("process: destroying a running process ends it") {
    Launcher l;
    PrProcess* process = l.start("sleep", {"30"});
    double before = prTimeMonotonicMilliseconds();
    prProcessDestroy(process);
    CHECK(prTimeMonotonicMilliseconds() - before < 5000);
}

TEST_CASE("process: the wake function runs as output arrives and as the process exits") {
    Atomic<uint32_t> wakes{};
    PrStringView arguments[] = {"-c", "printf a"};
    PrProcessDesc desc{
        .program = "sh",
        .arguments = arguments,
        .argumentCount = 2,
        .errors = PrProcessErrors_Discard,
        .wake = countWake,
        .wakeContext = &wakes,
    };
    PrProcess* process = nullptr;
    Launcher l;
    REQUIRE(prProcessLaunch(l.launcher, &desc, &process) == PrStatus_Ok);
    CHECK(drain(process) == "a");
    // Output, its end, and the exit.
    CHECK(wakes.load() >= 3);
    prProcessDestroy(process);
}

TEST_CASE("process: writing to a process that stopped reading drops the input") {
    Launcher l;
    PrProcess* process = l.start("sh", {"-c", "exec 0<&-; sleep 0.05"});
    std::string input(200000, 'x');
    for (int i = 0; i < 10; ++i)
        prProcessWrite(process, PrByteView{reinterpret_cast<const uint8_t*>(input.data()), input.size()});
    waitForExit(process);
    CHECK(prProcessInfo(process).code == 0);
    prProcessDestroy(process);
}

#endif
