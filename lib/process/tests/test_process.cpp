#include "pr/atomic.h"
#include "pr/integer.h"
#include "pr/mem.h"
#include "pr/mutex.h"
#include "pr/process.h"
#include "pr/span.h"
#include "pr/string.h"
#include "pr/string_ops.h"
#include "pr/thread.h"
#include "pr/time.h"

#include "tools/harness/test.h"

#if !defined(_WIN32)
#include <sys/resource.h>
#include <unistd.h>
#endif

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
    PrProcess* start(PrStringView program, Span<const PrStringView> arguments,
            PrProcessErrors errors = PrProcessErrors_Discard, PrStringView directory = {}) {
        PrProcessDesc desc{
            .program = program,
            .arguments = arguments.data(),
            .argumentCount = arguments.count(),
            .directory = directory,
            .errors = errors,
        };
        PrProcess* process = nullptr;
        REQUIRE(prProcessLaunch(launcher, &desc, &process) == PrStatus_Ok);
        return process;
    }
};

// Reads `stream` until the process exits and its output ends, and returns what it read.
String drain(PrProcess* process, PrProcessStream stream = PrProcessStream_Output) {
    String text;
    double end = prTimeMonotonicMilliseconds() + kPatience;
    for (;;) {
        uint8_t chunk[4096];
        size_t got = prProcessRead(process, stream, chunk, sizeof(chunk));
        text.append(PrStringView{reinterpret_cast<const char*>(chunk), got});
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
            text.append(PrStringView{reinterpret_cast<const char*>(chunk), got});
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

// Starts `program` with `arguments` on an 80 by 24 terminal. It runs in `directory`, or in the launcher's directory if
// `directory` is the null view.
PrProcess* startOnTerminal(Launcher& l, PrStringView program, Span<const PrStringView> arguments, PrStringView directory = {}) {
    PrProcessDesc desc{
        .program = program,
        .arguments = arguments.data(),
        .argumentCount = arguments.count(),
        .directory = directory,
        .streams = PrProcessStreams_Terminal,
        .terminalExtent = {80, 24},
    };
    PrProcess* process = nullptr;
    REQUIRE(prProcessLaunch(l.launcher, &desc, &process) == PrStatus_Ok);
    return process;
}

// Reads the output until it contains `text`, and returns everything it read.
String readUntil(PrProcess* process, PrStringView text) {
    String read;
    double end = prTimeMonotonicMilliseconds() + kPatience;
    while (!prStringContains(read, text)) {
        uint8_t chunk[4096];
        size_t got = prProcessRead(process, PrProcessStream_Output, chunk, sizeof(chunk));
        read.append(PrStringView{reinterpret_cast<const char*>(chunk), got});
        if (got == 0)
            prThreadSleep(1);
        REQUIRE(prTimeMonotonicMilliseconds() < end);
    }
    return read;
}

// The output that the receive function has taken. Once the output contains "ready", the function writes a line to the
// process, once.
struct Received {
    PrMutex mutex;
    String text;
    uint32_t calls = 0;
    bool answered = false;
    size_t answerAfter = 0;         // the bytes past "ready" that arrive before the answer

    Received() { prMutexInit(&mutex); }
    ~Received() { prMutexReset(&mutex); }

    Received(const Received&) = delete;
    Received& operator=(const Received&) = delete;

    String taken() {
        prMutexLock(&mutex);
        String copy = text;
        prMutexUnlock(&mutex);
        return copy;
    }
};

void receive(void* PR_NULLABLE context, PrProcess* process, PrProcessStream stream, PrByteView bytes) {
    Received* received = static_cast<Received*>(context);
    prMutexLock(&received->mutex);
    received->text.append(prStringFromBytes(bytes));
    received->calls += stream == PrProcessStream_Output ? 1 : 1000000;
    PrStringView text = received->text;
    size_t ready = prStringFind(text, "ready");
    bool answer = !received->answered && ready != text.size && text.size >= ready + 5 + received->answerAfter;
    received->answered = received->answered || answer;
    prMutexUnlock(&received->mutex);
    if (answer)
        prProcessWrite(process, PrByteView{reinterpret_cast<const uint8_t*>("hello\n"), 6});
}

// Starts `script` under sh with a receive function, on pipes or a terminal. Waits for the process to exit and its
// output to end. A process that takes longer than kPatience is destroyed before the test fails, so that its reads end
// before `received` does.
PrProcessInfo receiveFrom(Launcher& l, PrStringView script, PrProcessStreams streams, Received* received,
        Atomic<uint32_t>* wakes, PrProcess** out) {
    PrStringView arguments[] = {"-c", script};
    PrProcessDesc desc{
        .program = "sh",
        .arguments = arguments,
        .argumentCount = 2,
        .streams = streams,
        .errors = PrProcessErrors_Discard,
        .terminalExtent = {80, 24},
        .wake = countWake,
        .wakeContext = wakes,
        .receive = receive,
        .receiveContext = received,
    };
    REQUIRE(prProcessLaunch(l.launcher, &desc, out) == PrStatus_Ok);
    double end = prTimeMonotonicMilliseconds() + kPatience;
    PrProcessInfo info = prProcessInfo(*out);
    while (!info.exited || !info.outputEnded) {
        prThreadSleep(1);
        if (prTimeMonotonicMilliseconds() >= end) {
            prProcessDestroy(*out);
            *out = nullptr;
            FAIL("the process did not end in time");
        }
        info = prProcessInfo(*out);
    }
    return info;
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
    const PrStringView arguments[] = {"-c", "printf hello; exit 3"};
    PrProcess* process = l.start("sh", arguments);
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
    const PrStringView arguments[] = {"-c", "printf out; printf err >&2"};
    PrProcess* process = l.start("sh", arguments, PrProcessErrors_Pipe);
    CHECK(drain(process) == "out");
    CHECK(drain(process, PrProcessStream_Error) == "err");
    prProcessDestroy(process);
}

TEST_CASE("process: output past what a process holds waits for its reader, and none is lost") {
    Launcher l;
    const PrStringView arguments[] = {"-c", "3000000", "/dev/zero"};
    PrProcess* process = l.start("head", arguments);
    // Nothing is read until the process has had time to fill what it may hold.
    prThreadSleep(50);
    CHECK(drain(process).count() == 3000000);
    prProcessDestroy(process);
}

TEST_CASE("process: input larger than a pipe holds arrives whole") {
    Launcher l;
    const PrStringView arguments[] = {"-c"};
    PrProcess* process = l.start("wc", arguments);
    Array<uint8_t> input(1000000);
    prMemFill(input.data(), 'x', input.count());
    prProcessWrite(process, input);
    prProcessCloseInput(process);
    String counted = drain(process);
    PrStringView digits = prStringTrim(counted);
    uint64_t count = 0;
    CHECK(prU64ParsePrefix(digits.data, digits.size, &count, 10) != 0);
    CHECK(count == 1000000);
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
    const PrStringView arguments[] = {"30"};
    PrProcess* process = l.start("sleep", arguments);
    prProcessKill(process);
    PrProcessInfo info = waitForExit(process);
    CHECK(info.signaled);
    CHECK(info.code == 9);
    prProcessDestroy(process);
}

TEST_CASE("process: destroying a running process ends it") {
    Launcher l;
    const PrStringView arguments[] = {"30"};
    PrProcess* process = l.start("sleep", arguments);
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

TEST_CASE("process: a receive function takes the output as it arrives, in order, and may answer it") {
    Launcher l;
    Received received;
    Atomic<uint32_t> wakes{};
    PrProcess* process = nullptr;
    PrProcessInfo info = receiveFrom(l, "printf ready; read line; printf \"got $line\"; exit 4",
        PrProcessStreams_Pipes, &received, &wakes, &process);
    CHECK(received.taken() == "readygot hello");
    CHECK(info.code == 4);
    // Nothing is held for prProcessRead. Only the end of the output and the exit wake the caller.
    uint8_t chunk[16];
    CHECK(prProcessRead(process, PrProcessStream_Output, chunk, sizeof(chunk)) == 0);
    CHECK(wakes.load() <= 2);
    prProcessDestroy(process);
}

TEST_CASE("process: a receive function takes output past what a process holds for a read, and none is lost") {
    Launcher l;
    Received received;
    Atomic<uint32_t> wakes{};
    PrProcess* process = nullptr;
    receiveFrom(l, "head -c 3000000 /dev/zero", PrProcessStreams_Pipes, &received, &wakes, &process);
    CHECK(received.taken().count() == 3000000);
    CHECK(received.calls > 1);
    CHECK(received.calls < 1000000);
    prProcessDestroy(process);
}

TEST_CASE("process: what a receive function writes during a flood reaches a program on a terminal") {
    // A background job floods the terminal until the program reads the line that the receiver writes. The receiver
    // answers once the flood has passed what the terminal and the reads buffer, so the line is sent while the flood
    // runs. The program reports the line in its exit code, which no output of the flood can interleave with.
    Launcher l;
    Received received;
    received.answerAfter = 256 * 1024;
    Atomic<uint32_t> wakes{};
    PrProcess* process = nullptr;
    PrProcessInfo info = receiveFrom(l, "stty -echo; printf ready; yes & read line; kill $!; wait; test \"$line\" = hello",
        PrProcessStreams_Terminal, &received, &wakes, &process);
    CHECK(info.code == 0);
    CHECK(received.taken().count() > received.answerAfter);
    prProcessDestroy(process);
}

TEST_CASE("process: the environment sets and replaces variables, and keeps the launcher's others") {
    PrStringView arguments[] = {"-c", "printf '%s|%s|' \"$PRAX_PROCESS_TEST\" \"$HOME\"; test -n \"$PATH\" && printf path"};
    PrStringView environment[] = {"PRAX_PROCESS_TEST=one two", "HOME=/elsewhere"};
    PrProcessDesc desc{
        .program = "sh",
        .arguments = arguments,
        .argumentCount = 2,
        .environment = environment,
        .environmentCount = 2,
        .errors = PrProcessErrors_Discard,
    };
    PrProcess* process = nullptr;
    Launcher l;
    REQUIRE(prProcessLaunch(l.launcher, &desc, &process) == PrStatus_Ok);
    CHECK(drain(process) == "one two|/elsewhere|path");
    prProcessDestroy(process);
}

TEST_CASE("process: a process on a terminal has it as its controlling terminal, at its extent") {
    Launcher l;
    // Only a controlling terminal has a foreground process group, and the shell leads it.
    const PrStringView arguments[] = {
        "-c", "test -t 0 && test -t 2 && stty size && "
            "test \"$(ps -o tpgid= -p $$ | tr -d ' ')\" = \"$$\" && echo controlled",
    };
    PrProcess* process = startOnTerminal(l, "sh", arguments);
    // The terminal ends each line with CR LF.
    CHECK(drain(process) == "24 80\r\ncontrolled\r\n");
    CHECK(prProcessInfo(process).code == 0);
    prProcessDestroy(process);
}

TEST_CASE("process: a terminal's pixel extent reaches its program, at the start and after a resize") {
    Launcher l;
    // stty does not print the pixel size, so Python reads it. If Python is missing, the program prints "none" and the
    // test checks nothing more.
    PrStringView script = "import fcntl, struct, sys, termios\n"
        "rows, columns, width, height = struct.unpack('HHHH', fcntl.ioctl(0, termios.TIOCGWINSZ, b'\\0' * 8))\n"
        "print(columns, rows, width, height)\n";
    PrStringView command = "if command -v python3 >/dev/null; then python3 -c \"$0\"; read line; python3 -c \"$0\"; "
        "else echo none; fi";
    PrStringView arguments[] = {"-c", command, script};
    PrProcessDesc desc{
        .program = "sh",
        .arguments = arguments,
        .argumentCount = 3,
        .streams = PrProcessStreams_Terminal,
        .terminalExtent = {80, 24},
        .terminalPixelExtent = {640, 480},
    };
    PrProcess* process = nullptr;
    REQUIRE(prProcessLaunch(l.launcher, &desc, &process) == PrStatus_Ok);
    String first = readUntil(process, "\r\n");
    if (first != "none\r\n") {
        CHECK(first == "80 24 640 480\r\n");
        prProcessTerminalResize(process, PrUVec2{100, 30}, PrUVec2{1000, 600});
        prProcessWrite(process, PrByteView{reinterpret_cast<const uint8_t*>("\n"), 1});
        CHECK(readUntil(process, "100 30 1000 600\r\n").count() > 0);
    }
    prProcessDestroy(process);
}

TEST_CASE("process: a job of an interactive shell on a terminal is signaled when the terminal resizes") {
    Launcher l;
    const PrStringView arguments[] = {"-i"};
    PrProcess* process = startOnTerminal(l, "sh", arguments);
    // The shell puts the job in its own process group and makes that the terminal's foreground group. The job computes
    // the marker that it prints on SIGWINCH, so the echoed command line never contains it.
    PrStringView job = "sh -c 'trap \"echo W\\$((1+1))\" WINCH; echo ready; while :; do sleep 0.05; done'\n";
    prProcessWrite(process, prStringBytes(job));
    readUntil(process, "ready\r\n");
    prProcessTerminalResize(process, PrUVec2{100, 30}, PrUVec2{800, 480});
    CHECK(readUntil(process, "W2\r\n").count() > 0);
    prProcessDestroy(process);
}

TEST_CASE("process: a terminal echoes its input, and its end-of-file character ends what the process reads") {
    Launcher l;
    PrProcess* process = startOnTerminal(l, "cat", {});
    prProcessWrite(process, PrByteView{reinterpret_cast<const uint8_t*>("hi\n"), 3});
    // The terminal echoes the line, and then cat writes it.
    CHECK(readUntil(process, "hi\r\nhi\r\n") == "hi\r\nhi\r\n");
    prProcessWrite(process, PrByteView{reinterpret_cast<const uint8_t*>("\x04"), 1});
    CHECK(waitForExit(process).code == 0);
    prProcessDestroy(process);
}

TEST_CASE("process: a program on a terminal holds no descriptor of the launcher's, even one that passes to children") {
    Launcher l;
    // Inheritable descriptors, numbered above the child's standard streams: 100, and the highest that the process may
    // open up to 1000. A shell's limit can be lower, as macOS's 256 is.
    struct rlimit limit{};
    REQUIRE(getrlimit(RLIMIT_NOFILE, &limit) == 0);
    int high = limit.rlim_cur != RLIM_INFINITY && limit.rlim_cur <= 1000 ? int(limit.rlim_cur) - 1 : 1000;
    REQUIRE(high > 100);
    int ends[2] = {-1, -1};
    REQUIRE(pipe(ends) == 0);
    REQUIRE(dup2(ends[0], 100) == 100);
    REQUIRE(dup2(ends[1], high) == high);
    String script = format("for n in 100 {}; do [ -e /dev/fd/$n ] && echo held $n; done; echo checked", high);
    const PrStringView arguments[] = {"-c", script};
    PrProcess* process = startOnTerminal(l, "sh", arguments);
    CHECK(drain(process) == "checked\r\n");
    prProcessDestroy(process);
    const int descriptors[] = {ends[0], ends[1], 100, high};
    for (int descriptor : descriptors)
        close(descriptor);
}

TEST_CASE("process: a program's exit is seen while a job that it left holds its output open") {
    Launcher l;
    const PrStringView arguments[] = {"-c", "sleep 30 & exit 4"};
    PrProcess* process = l.start("sh", arguments);
    PrProcessInfo info = waitForExit(process);
    CHECK(info.code == 4);
    CHECK_FALSE(info.outputEnded);
    prProcessDestroy(process);
}

TEST_CASE("process: a program on a terminal exits while a job that it left holds the terminal, and its output ends") {
    Launcher l;
    // The job ignores the SIGHUP that the program's exit sends it, and holds the terminal open longer than the test
    // waits.
    const PrStringView arguments[] = {"-c", "trap '' HUP; sleep 30 & echo started; exit 4"};
    PrProcess* process = startOnTerminal(l, "sh", arguments);
    CHECK(drain(process) == "started\r\n");
    CHECK(prProcessInfo(process).code == 4);
    prProcessDestroy(process);
}

TEST_CASE("process: resizing a terminal signals the process, which reads the new extent") {
    Launcher l;
    const PrStringView arguments[] = {"-c", "trap 'stty size; exit' WINCH; echo ready; while :; do sleep 0.01; done"};
    PrProcess* process = startOnTerminal(l, "sh", arguments);
    readUntil(process, "ready\r\n");
    prProcessTerminalResize(process, PrUVec2{100, 30}, PrUVec2{800, 480});
    CHECK(readUntil(process, "30 100\r\n").count() > 0);
    CHECK(waitForExit(process).code == 0);
    prProcessDestroy(process);
}

TEST_CASE("process: destroying a process whose terminal holds all the output it can ends the process") {
    Launcher l;
    // The terminal fills, and the process waits for a reader. Some systems hold an exiting process until its terminal's
    // output drains, and nothing reads that output once destruction begins.
    const PrStringView arguments[] = {"-c", "head -c 8000000 /dev/zero; sleep 10"};
    PrProcess* process = startOnTerminal(l, "sh", arguments);
    prThreadSleep(200);
    prProcessDestroy(process);
}

TEST_CASE("process: a terminal's foreground directory is its foreground job's, and the shell's once the job ends") {
    Launcher l;
    const PrStringView arguments[] = {"-i"};
    PrProcess* process = startOnTerminal(l, "sh", arguments, "/");
    // The job prints its directory after a marker that it computes, so the echoed command line never contains it.
    PrStringView job = "sh -c 'cd /tmp && echo \"P$((1+1)):$(pwd -P):\" && sleep 5'\n";
    prProcessWrite(process, prStringBytes(job));
    String read = readUntil(process, "P2:");
    size_t start = prStringFind(read, "P2:") + 3;
    auto rest = [&] { return prStringSlice(read, start, read.count() - start); };
    if (!prStringContains(rest(), ":\r\n"))
        read.append(readUntil(process, ":\r\n"));
    String expected = prStringSlice(read, start, prStringFind(rest(), ":\r\n"));
    REQUIRE(!expected.isEmpty());

    // Returns the foreground directory once it equals `directory`, or its last value if the wait times out.
    auto waitFor = [&](PrStringView directory) {
        double end = prTimeMonotonicMilliseconds() + kPatience;
        for (;;) {
            PrRawString found = prProcessTerminalForegroundDirectory(process);
            String text = String::adopt(found);
            if (text == directory || prTimeMonotonicMilliseconds() >= end)
                return text;
            prThreadSleep(2);
        }
    };
    CHECK(waitFor(expected) == expected);
    // Control-C ends the job, and the shell's process group is in the foreground again.
    prProcessWrite(process, PrByteView{reinterpret_cast<const uint8_t*>("\x03"), 1});
    CHECK(waitFor("/") == "/");
    prProcessDestroy(process);

    // A process over pipes has no terminal.
    const PrStringView pipedArguments[] = {"5"};
    PrProcess* piped = l.start("sleep", pipedArguments);
    PrRawString none = prProcessTerminalForegroundDirectory(piped);
    CHECK(prRawStringIsNull(&none));
    prProcessDestroy(piped);
}

TEST_CASE("process: writing to a process that stopped reading drops the input") {
    Launcher l;
    const PrStringView arguments[] = {"-c", "exec 0<&-; sleep 0.05"};
    PrProcess* process = l.start("sh", arguments);
    Array<uint8_t> input(200000);
    prMemFill(input.data(), 'x', input.count());
    for (int i = 0; i < 10; ++i)
        prProcessWrite(process, input);
    waitForExit(process);
    CHECK(prProcessInfo(process).code == 0);
    prProcessDestroy(process);
}

#else

// The programs are cmd commands. On a terminal, a pseudoconsole wraps their output in control sequences, so those tests
// search the output.

TEST_CASE("process: output arrives, and the exit status follows") {
    Launcher l;
    const PrStringView arguments[] = {"/c", "echo hello& exit 3"};
    PrProcess* process = l.start("cmd", arguments);
    CHECK(drain(process) == "hello\r\n");
    PrProcessInfo info = prProcessInfo(process);
    CHECK(info.exited);
    CHECK(!info.signaled);
    CHECK(info.code == 3);
    prProcessDestroy(process);
}

TEST_CASE("process: input reaches the process, and closing it ends what the process reads") {
    Launcher l;
    PrProcess* process = l.start("sort", {});
    prProcessWrite(process, PrByteView{reinterpret_cast<const uint8_t*>("two\r\n"), 5});
    prProcessWrite(process, PrByteView{reinterpret_cast<const uint8_t*>("one\r\n"), 5});
    prProcessCloseInput(process);
    prProcessWrite(process, PrByteView{reinterpret_cast<const uint8_t*>("dropped"), 7});
    CHECK(drain(process) == "one\r\ntwo\r\n");
    CHECK(prProcessInfo(process).code == 0);
    prProcessDestroy(process);
}

TEST_CASE("process: standard error is a stream of its own when it is a pipe") {
    Launcher l;
    const PrStringView arguments[] = {"/c", "echo out& echo err 1>&2"};
    PrProcess* process = l.start("cmd", arguments, PrProcessErrors_Pipe);
    CHECK(drain(process) == "out\r\n");
    CHECK(drain(process, PrProcessStream_Error) == "err \r\n");
    prProcessDestroy(process);
}

TEST_CASE("process: a killed process exits with 1") {
    Launcher l;
    const PrStringView arguments[] = {"/c", "set /p line="};
    PrProcess* process = l.start("cmd", arguments);
    prProcessKill(process);
    PrProcessInfo info = waitForExit(process);
    CHECK(!info.signaled);
    CHECK(info.code == 1);
    prProcessDestroy(process);
}

TEST_CASE("process: the environment sets and replaces variables, and keeps the launcher's others") {
    // Windows matches variable names without regard to case.
    PrStringView arguments[] = {"/c", "echo %PRAX_PROCESS_TEST%-%USERNAME%& if defined PATH echo path"};
    PrStringView environment[] = {"PRAX_PROCESS_TEST=one two", "username=elsewhere"};
    PrProcessDesc desc{
        .program = "cmd",
        .arguments = arguments,
        .argumentCount = 2,
        .environment = environment,
        .environmentCount = 2,
        .errors = PrProcessErrors_Discard,
    };
    PrProcess* process = nullptr;
    Launcher l;
    REQUIRE(prProcessLaunch(l.launcher, &desc, &process) == PrStatus_Ok);
    CHECK(drain(process) == "one two-elsewhere\r\npath\r\n");
    prProcessDestroy(process);
}

TEST_CASE("process: a receive function takes the output as it arrives, in order, and may answer it") {
    Launcher l;
    Received received;
    Atomic<uint32_t> wakes{};
    PrStringView arguments[] = {"/v:on", "/c", "set /p line=ready& echo got !line!& exit 4"};
    PrProcessDesc desc{
        .program = "cmd",
        .arguments = arguments,
        .argumentCount = 3,
        .errors = PrProcessErrors_Discard,
        .wake = countWake,
        .wakeContext = &wakes,
        .receive = receive,
        .receiveContext = &received,
    };
    PrProcess* process = nullptr;
    REQUIRE(prProcessLaunch(l.launcher, &desc, &process) == PrStatus_Ok);
    double end = prTimeMonotonicMilliseconds() + kPatience;
    PrProcessInfo info = prProcessInfo(process);
    while (!info.exited || !info.outputEnded) {
        prThreadSleep(1);
        REQUIRE(prTimeMonotonicMilliseconds() < end);
        info = prProcessInfo(process);
    }
    CHECK(received.taken() == "readygot hello\r\n");
    CHECK(info.code == 4);
    uint8_t chunk[16];
    CHECK(prProcessRead(process, PrProcessStream_Output, chunk, sizeof(chunk)) == 0);
    CHECK(wakes.load() <= 2);
    prProcessDestroy(process);
}

TEST_CASE("process: a program on a terminal writes to it, and its output ends once it exits") {
    Launcher l;
    const PrStringView arguments[] = {"/c", "echo hello& exit 3"};
    PrProcess* process = startOnTerminal(l, "cmd", arguments);
    CHECK(prStringContains(drain(process), "hello\r\n"));
    CHECK(prProcessInfo(process).code == 3);
    prProcessDestroy(process);
}

TEST_CASE("process: input reaches a program on a terminal") {
    Launcher l;
    const PrStringView arguments[] = {"/v:on", "/c", "set /p line=ready& echo [got !line!]"};
    PrProcess* process = startOnTerminal(l, "cmd", arguments);
    readUntil(process, "ready");
    // A terminal sends a carriage return for the Enter key.
    prProcessWrite(process, PrByteView{reinterpret_cast<const uint8_t*>("hello\r"), 6});
    CHECK(readUntil(process, "[got hello]").count() > 0);
    CHECK(waitForExit(process).code == 0);
    prProcessDestroy(process);
}

TEST_CASE("process: a program on a terminal goes on running once its input is closed") {
    Launcher l;
    const PrStringView arguments[] = {"/c", "ping -n 2 127.0.0.1 >nul& echo alive& exit 5"};
    PrProcess* process = startOnTerminal(l, "cmd", arguments);
    prProcessCloseInput(process);
    prProcessWrite(process, PrByteView{reinterpret_cast<const uint8_t*>("dropped\r"), 8});
    CHECK(prStringContains(drain(process), "alive"));
    CHECK(prProcessInfo(process).code == 5);
    prProcessDestroy(process);
}

TEST_CASE("process: resizing a terminal changes the extent that its program reads") {
    Launcher l;
    const PrStringView arguments[] = {"/c", "mode con& set /p line=& mode con"};
    PrProcess* process = startOnTerminal(l, "cmd", arguments);
    readUntil(process, "Columns:        80");
    prProcessTerminalResize(process, PrUVec2{100, 30}, PrUVec2{800, 480});
    prProcessWrite(process, PrByteView{reinterpret_cast<const uint8_t*>("\r"), 1});
    CHECK(readUntil(process, "Columns:        100").count() > 0);
    CHECK(waitForExit(process).code == 0);
    prProcessDestroy(process);
}

TEST_CASE("process: destroying a process whose terminal holds all the output it can ends the process") {
    Launcher l;
    // Nothing reads the output, so the pseudoconsole fills and waits for a reader.
    const PrStringView arguments[] = {"/c", "for /l %i in (1,1,1000000) do @echo %i"};
    PrProcess* process = startOnTerminal(l, "cmd", arguments);
    prThreadSleep(200);
    double before = prTimeMonotonicMilliseconds();
    prProcessDestroy(process);
    CHECK(prTimeMonotonicMilliseconds() - before < 5000);
}

TEST_CASE("process: a terminal's foreground directory is the null string, which a pseudoconsole does not report") {
    Launcher l;
    const PrStringView arguments[] = {"/c", "set /p line="};
    PrProcess* process = startOnTerminal(l, "cmd", arguments);
    PrRawString none = prProcessTerminalForegroundDirectory(process);
    CHECK(prRawStringIsNull(&none));
    prProcessDestroy(process);
}

#endif
