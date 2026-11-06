#include "pr/atomic.h"
#include "pr/mutex.h"
#include "pr/process.h"
#include "pr/thread.h"
#include "pr/time.h"

#include "tools/harness/test.h"

#include <string>

#if !defined(_WIN32)
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

// Starts `program` with `arguments` on an 80 by 24 terminal. It runs in `directory`, or in the launcher's directory if
// `directory` is the null view.
PrProcess* startOnTerminal(Launcher& l, PrStringView program, std::initializer_list<PrStringView> arguments,
        PrStringView directory = {}) {
    PrProcessDesc desc{
        .program = program,
        .arguments = arguments.begin(),
        .argumentCount = uint32_t(arguments.size()),
        .directory = directory,
        .streams = PrProcessStreams_Terminal,
        .terminalExtent = {80, 24},
    };
    PrProcess* process = nullptr;
    REQUIRE(prProcessLaunch(l.launcher, &desc, &process) == PrStatus_Ok);
    return process;
}

// Reads the output until it contains `text`, and returns everything it read.
std::string readUntil(PrProcess* process, const std::string& text) {
    std::string read;
    double end = prTimeMonotonicMilliseconds() + kPatience;
    while (read.find(text) == std::string::npos) {
        uint8_t chunk[4096];
        size_t got = prProcessRead(process, PrProcessStream_Output, chunk, sizeof(chunk));
        read.append(reinterpret_cast<const char*>(chunk), got);
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
    std::string text;
    uint32_t calls = 0;
    bool answered = false;

    Received() { prMutexInit(&mutex); }
    ~Received() { prMutexReset(&mutex); }

    Received(const Received&) = delete;
    Received& operator=(const Received&) = delete;

    std::string taken() {
        prMutexLock(&mutex);
        std::string copy = text;
        prMutexUnlock(&mutex);
        return copy;
    }
};

void receive(void* PR_NULLABLE context, PrProcess* process, PrProcessStream stream, PrByteView bytes) {
    Received* received = static_cast<Received*>(context);
    prMutexLock(&received->mutex);
    received->text.append(reinterpret_cast<const char*>(bytes.data), bytes.size);
    received->calls += stream == PrProcessStream_Output ? 1 : 1000000;
    bool answer = !received->answered && received->text.find("ready") != std::string::npos;
    received->answered = received->answered || answer;
    prMutexUnlock(&received->mutex);
    if (answer)
        prProcessWrite(process, PrByteView{reinterpret_cast<const uint8_t*>("hello\n"), 6});
}

// Starts `script` under sh with a receive function, on pipes or a terminal. Waits for the process to exit and its
// output to end.
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
        REQUIRE(prTimeMonotonicMilliseconds() < end);
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
    CHECK(received.taken().size() == 3000000);
    CHECK(received.calls > 1);
    CHECK(received.calls < 1000000);
    prProcessDestroy(process);
}

TEST_CASE("process: what a receive function writes during a flood reaches a program on a terminal") {
    // A background job floods the terminal while the program waits for the line that the receiver writes.
    Launcher l;
    Received received;
    Atomic<uint32_t> wakes{};
    PrProcess* process = nullptr;
    receiveFrom(l, "stty -echo; yes | head -c 4000000 & printf ready; read line; printf \"[got $line]\"; wait",
        PrProcessStreams_Terminal, &received, &wakes, &process);
    CHECK(received.taken().find("[got hello]") != std::string::npos);
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
    PrProcess* process = startOnTerminal(l, "sh", {
        "-c", "test -t 0 && test -t 2 && stty size && "
            "test \"$(ps -o tpgid= -p $$ | tr -d ' ')\" = \"$$\" && echo controlled",
    });
    // The terminal ends each line with CR LF.
    CHECK(drain(process) == "24 80\r\ncontrolled\r\n");
    CHECK(prProcessInfo(process).code == 0);
    prProcessDestroy(process);
}

TEST_CASE("process: a terminal's pixel extent reaches its program, at the start and after a resize") {
    Launcher l;
    // stty does not print the pixel size, so Python reads it. If Python is missing, the program prints "none" and the
    // test checks nothing more.
    std::string script = "import fcntl, struct, sys, termios\n"
        "rows, columns, width, height = struct.unpack('HHHH', fcntl.ioctl(0, termios.TIOCGWINSZ, b'\\0' * 8))\n"
        "print(columns, rows, width, height)\n";
    std::string command = "if command -v python3 >/dev/null; then python3 -c \"$0\"; read line; python3 -c \"$0\"; "
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
    std::string first = readUntil(process, "\r\n");
    if (first != "none\r\n") {
        CHECK(first == "80 24 640 480\r\n");
        prProcessTerminalResize(process, PrUVec2{100, 30}, PrUVec2{1000, 600});
        prProcessWrite(process, PrByteView{reinterpret_cast<const uint8_t*>("\n"), 1});
        CHECK(readUntil(process, "100 30 1000 600\r\n").size() > 0);
    }
    prProcessDestroy(process);
}

TEST_CASE("process: a job of an interactive shell on a terminal is signaled when the terminal resizes") {
    Launcher l;
    PrProcess* process = startOnTerminal(l, "sh", {"-i"});
    // The shell puts the job in its own process group and makes that the terminal's foreground group. The job computes
    // the marker that it prints on SIGWINCH, so the echoed command line never contains it.
    std::string job = "sh -c 'trap \"echo W\\$((1+1))\" WINCH; echo ready; while :; do sleep 0.05; done'\n";
    prProcessWrite(process, PrByteView{reinterpret_cast<const uint8_t*>(job.data()), job.size()});
    readUntil(process, "ready\r\n");
    prProcessTerminalResize(process, PrUVec2{100, 30}, PrUVec2{800, 480});
    CHECK(readUntil(process, "W2\r\n").size() > 0);
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
    // Inheritable descriptors, numbered above the child's standard streams.
    int ends[2] = {-1, -1};
    REQUIRE(pipe(ends) == 0);
    REQUIRE(dup2(ends[0], 100) == 100);
    REQUIRE(dup2(ends[1], 1000) == 1000);
    PrProcess* process = startOnTerminal(l, "sh", {
        "-c", "for n in 100 1000; do [ -e /dev/fd/$n ] && echo held $n; done; echo checked",
    });
    CHECK(drain(process) == "checked\r\n");
    prProcessDestroy(process);
    for (int descriptor : {ends[0], ends[1], 100, 1000})
        close(descriptor);
}

TEST_CASE("process: a program's exit is seen while a job that it left holds its output open") {
    Launcher l;
    PrProcess* process = l.start("sh", {"-c", "sleep 30 & exit 4"});
    PrProcessInfo info = waitForExit(process);
    CHECK(info.code == 4);
    CHECK_FALSE(info.outputEnded);
    prProcessDestroy(process);
}

TEST_CASE("process: a program on a terminal exits while a job that it left holds the terminal, and its output ends") {
    Launcher l;
    // The job ignores the SIGHUP that the program's exit sends it, and holds the terminal open longer than the test
    // waits.
    PrProcess* process = startOnTerminal(l, "sh", {"-c", "trap '' HUP; sleep 30 & echo started; exit 4"});
    CHECK(drain(process) == "started\r\n");
    CHECK(prProcessInfo(process).code == 4);
    prProcessDestroy(process);
}

TEST_CASE("process: resizing a terminal signals the process, which reads the new extent") {
    Launcher l;
    PrProcess* process = startOnTerminal(l, "sh", {
        "-c", "trap 'stty size; exit' WINCH; echo ready; while :; do sleep 0.01; done",
    });
    readUntil(process, "ready\r\n");
    prProcessTerminalResize(process, PrUVec2{100, 30}, PrUVec2{800, 480});
    CHECK(readUntil(process, "30 100\r\n").size() > 0);
    CHECK(waitForExit(process).code == 0);
    prProcessDestroy(process);
}

TEST_CASE("process: destroying a process whose terminal holds all the output it can ends the process") {
    Launcher l;
    // The terminal fills, and the process waits for a reader. Some systems hold an exiting process until its terminal's
    // output drains, and nothing reads that output once destruction begins.
    PrProcess* process = startOnTerminal(l, "sh", {"-c", "head -c 8000000 /dev/zero; sleep 10"});
    prThreadSleep(200);
    prProcessDestroy(process);
}

TEST_CASE("process: a terminal's foreground directory is its foreground job's, and the shell's once the job ends") {
    Launcher l;
    PrProcess* process = startOnTerminal(l, "sh", {"-i"}, "/");
    // The job prints its directory after a marker that it computes, so the echoed command line never contains it.
    std::string job = "sh -c 'cd /tmp && echo \"P$((1+1)):$(pwd -P):\" && sleep 5'\n";
    prProcessWrite(process, PrByteView{reinterpret_cast<const uint8_t*>(job.data()), job.size()});
    std::string read = readUntil(process, "P2:");
    size_t start = read.find("P2:") + 3;
    if (read.find(":\r\n", start) == std::string::npos)
        read += readUntil(process, ":\r\n");
    std::string expected = read.substr(start, read.find(":\r\n", start) - start);
    REQUIRE(!expected.empty());

    // Returns the foreground directory once it equals `directory`, or its last value if the wait times out.
    auto waitFor = [&](const std::string& directory) {
        double end = prTimeMonotonicMilliseconds() + kPatience;
        for (;;) {
            PrRawString found = prProcessTerminalForegroundDirectory(process);
            std::string text(prRawStringView(&found).data, prRawStringView(&found).size);
            prRawStringReset(&found);
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
    PrProcess* piped = l.start("sleep", {"5"});
    PrRawString none = prProcessTerminalForegroundDirectory(piped);
    CHECK(prRawStringIsNull(&none));
    prProcessDestroy(piped);
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

#else

// The programs are cmd commands. On a terminal, a pseudoconsole wraps their output in control sequences, so those tests
// search the output.

TEST_CASE("process: output arrives, and the exit status follows") {
    Launcher l;
    PrProcess* process = l.start("cmd", {"/c", "echo hello& exit 3"});
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
    PrProcess* process = l.start("cmd", {"/c", "echo out& echo err 1>&2"}, PrProcessErrors_Pipe);
    CHECK(drain(process) == "out\r\n");
    CHECK(drain(process, PrProcessStream_Error) == "err \r\n");
    prProcessDestroy(process);
}

TEST_CASE("process: a killed process exits with 1") {
    Launcher l;
    PrProcess* process = l.start("cmd", {"/c", "set /p line="});
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
    PrProcess* process = startOnTerminal(l, "cmd", {"/c", "echo hello& exit 3"});
    CHECK(drain(process).find("hello\r\n") != std::string::npos);
    CHECK(prProcessInfo(process).code == 3);
    prProcessDestroy(process);
}

TEST_CASE("process: input reaches a program on a terminal") {
    Launcher l;
    PrProcess* process = startOnTerminal(l, "cmd", {"/v:on", "/c", "set /p line=ready& echo [got !line!]"});
    readUntil(process, "ready");
    // A terminal sends a carriage return for the Enter key.
    prProcessWrite(process, PrByteView{reinterpret_cast<const uint8_t*>("hello\r"), 6});
    CHECK(readUntil(process, "[got hello]").size() > 0);
    CHECK(waitForExit(process).code == 0);
    prProcessDestroy(process);
}

TEST_CASE("process: a program on a terminal goes on running once its input is closed") {
    Launcher l;
    PrProcess* process = startOnTerminal(l, "cmd", {"/c", "ping -n 2 127.0.0.1 >nul& echo alive& exit 5"});
    prProcessCloseInput(process);
    prProcessWrite(process, PrByteView{reinterpret_cast<const uint8_t*>("dropped\r"), 8});
    CHECK(drain(process).find("alive") != std::string::npos);
    CHECK(prProcessInfo(process).code == 5);
    prProcessDestroy(process);
}

TEST_CASE("process: resizing a terminal changes the extent that its program reads") {
    Launcher l;
    PrProcess* process = startOnTerminal(l, "cmd", {"/c", "mode con& set /p line=& mode con"});
    readUntil(process, "Columns:        80");
    prProcessTerminalResize(process, PrUVec2{100, 30}, PrUVec2{800, 480});
    prProcessWrite(process, PrByteView{reinterpret_cast<const uint8_t*>("\r"), 1});
    CHECK(readUntil(process, "Columns:        100").size() > 0);
    CHECK(waitForExit(process).code == 0);
    prProcessDestroy(process);
}

TEST_CASE("process: destroying a process whose terminal holds all the output it can ends the process") {
    Launcher l;
    // Nothing reads the output, so the pseudoconsole fills and waits for a reader.
    PrProcess* process = startOnTerminal(l, "cmd", {"/c", "for /l %i in (1,1,1000000) do @echo %i"});
    prThreadSleep(200);
    double before = prTimeMonotonicMilliseconds();
    prProcessDestroy(process);
    CHECK(prTimeMonotonicMilliseconds() - before < 5000);
}

TEST_CASE("process: a terminal's foreground directory is the null string, which a pseudoconsole does not report") {
    Launcher l;
    PrProcess* process = startOnTerminal(l, "cmd", {"/c", "set /p line="});
    PrRawString none = prProcessTerminalForegroundDirectory(process);
    CHECK(prRawStringIsNull(&none));
    prProcessDestroy(process);
}

#endif
