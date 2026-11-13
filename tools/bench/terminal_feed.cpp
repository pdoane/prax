// The program that the terminals benchmark (tools/bench/terminals.py) runs in each terminal application. It writes the
// benchmark's workloads to its tty, one run at a time as the runner asks, and times how long the terminal takes to read
// and parse each.
//
// The feed runs for the whole of a terminal's session. It talks with the runner over two FIFOs in DIRECTORY, which the
// runner makes: the feed reports on `report`, a line at a time, and reads commands from `commands`, a line at a time.
//
//   1. The feed puts its tty in raw mode, so that the workload's bytes pass unchanged and the terminal's answers are
//      neither echoed nor buffered by line. It clears the screen and waits for the answer to a cursor position report
//      (DSR 6). The answer shows that the terminal has started and answers DSR before anything is measured. The feed
//      then reports `ready PID`.
//   2. It then answers each command:
//        load NAME              generates the workload NAME, and reports `loaded BYTES`
//        clear                  clears the screen and the scrollback, waits for DSR's answer, and reports `cleared`
//        run SHARE TIMEOUT      writes SHARE of the loaded workload, and then DSR 6, and waits for the answer. It
//                               reports `done MILLISECONDS COLUMNS ROWS BYTES COMPLETE`: the time from the start to the
//                               answer, the tty's extent, the bytes written, and 0 if the feed stopped writing after
//                               TIMEOUT milliseconds, or 1 otherwise
//      Once `commands` closes, it restores the tty and exits, which closes the terminal's window.
//
// A terminal answers DSR when it parses it, after every byte before it. The answer therefore marks the end of the
// terminal's parsing, while the end of the writes marks only that the pty buffered the bytes. Answers to anything else
// are read and dropped. A throughput workload is written as fast as the terminal reads it. A paced workload is a TUI's
// frames, each written at its time at kFrameRate, so its run lasts its duration if the terminal keeps up.
//
// A failure is reported as `error MESSAGE`, and the feed exits with status 1. A feed that runs past kLifetimeSeconds is
// stopped by SIGALRM, so that a terminal whose runner went away closes.
//
// Usage: terminal_feed --list
//        terminal_feed DIRECTORY
//
// --list prints the workloads, one to a line, as three fields separated by tabs: the kind (`fill`, `throughput` or
// `paced`), the amount (in lines, bytes or seconds), and the name.

#include "pr/time.h"

#include "tools/bench/vterm_bench/vterm_workloads.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <string>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#include <vector>

using namespace vterm_bench;

namespace {

constexpr size_t kWriteBytes = 64u << 10;

// The time that the feed waits for an answer to DSR after its last write, in milliseconds.
constexpr double kAnswerTimeout = 30000;

// The longest that the feed runs, in seconds.
constexpr unsigned kLifetimeSeconds = 900;

// The frames per second of a paced workload.
constexpr uint32_t kFrameRate = 60;

// How a workload is written: once, to fill the scrollback; as fast as the terminal reads it; or as frames at their
// times.
enum {
    FeedKind_Fill,
    FeedKind_Throughput,
    FeedKind_Paced,
};
typedef uint8_t FeedKind;

// A workload of the feed: its name, its kind, its amount, and what generates its bytes from the amount. The amount is
// lines for a fill, bytes for a throughput workload, and seconds for a paced one, whose frames are htop's (tuiFrames).
struct FeedWorkload {
    const char* name;
    FeedKind kind;
    uint64_t amount;
    std::string (*PR_NULLABLE generate)(uint64_t amount);
};

// The fill writes as many lines as the scrollback keeps, plus a screen's worth more.
constexpr FeedWorkload kWorkloads[] = {
    {"scrollback fill", FeedKind_Fill, 100000, [](uint64_t lines) { return fullLines(uint32_t(lines) + kExtent.y); }},
    {"log flood", FeedKind_Throughput, 64u << 20, [](uint64_t bytes) { return logLines(bytes); }},
    {"unicode", FeedKind_Throughput, 32u << 20, [](uint64_t bytes) { return unicodeLines(bytes); }},
    {"long lines", FeedKind_Throughput, 64u << 20, [](uint64_t bytes) { return longLines(bytes); }},
    {"tui 60fps", FeedKind_Paced, 5, nullptr},
};

const char* const kKindNames[] = {"fill", "throughput", "paced"};

// A run of bytes that the feed writes once `release` milliseconds have passed since the run's start.
struct Chunk {
    const char* data;
    size_t size;
    double release;
};

// The loaded workload: its bytes, or for a paced workload, the bytes that enter the TUI, its frames, and the bytes that
// leave it.
struct Loaded {
    const FeedWorkload* workload = nullptr;
    std::string bytes;
    std::string enter;
    std::vector<std::string> frames;
};

int reportFd = -1;

void report(const std::string& line) {
    std::string text = line + "\n";
    if (reportFd >= 0 && write(reportFd, text.data(), text.size()) < 0)
        perror("terminal_feed: report");
}

[[noreturn]] void fail(const std::string& message) {
    report("error " + message);
    fprintf(stderr, "terminal_feed: %s\n", message.c_str());
    exit(1);
}

// Reads what the terminal sent and returns true once it holds an answer to DSR 6, `CSI row ; column R`. Everything
// before the answer is dropped from `input`.
bool answered(std::string& input) {
    size_t at = 0;
    while ((at = input.find("\x1b[", at)) != std::string::npos) {
        size_t end = at + 2;
        while (end < input.size() && ((input[end] >= '0' && input[end] <= '9') || input[end] == ';'))
            ++end;
        if (end < input.size() && input[end] == 'R') {
            input.erase(0, end + 1);
            return true;
        }
        at += 2;
    }
    return false;
}

// Reads what the terminal sent into `input`. Fails if the tty closed.
void readInput(int tty, short revents, std::string& input) {
    if ((revents & POLLIN) != 0) {
        char buffer[4096];
        ssize_t got = read(tty, buffer, sizeof(buffer));
        if (got > 0)
            input.append(buffer, size_t(got));
    }
    if ((revents & (POLLHUP | POLLERR)) != 0)
        fail("the tty closed");
}

// Writes `chunks` to the tty, which is non-blocking, each once its release time has passed. Meanwhile it reads and
// drops what the terminal sends, so that a terminal that answers a query never waits on the feed. Stops writing after
// `timeout` milliseconds. Then waits until `end` milliseconds have passed, writes DSR 6 and waits for its answer. Adds
// the bytes written to `written`, and returns true if every chunk was written.
bool play(int tty, const std::vector<Chunk>& chunks, double end, double timeout, size_t* written) {
    std::string input;
    PrTimePerf begin = prTimePerfNow();
    size_t index = 0;
    size_t offset = 0;
    bool complete = true;
    for (;;) {
        double now = prTimePerfMilliseconds(begin, prTimePerfNow());
        if (now > timeout) {
            complete = index == chunks.size();
            break;
        }
        if (index == chunks.size() && now >= end)
            break;
        bool pending = index < chunks.size() && chunks[index].release <= now;
        double next = index < chunks.size() ? chunks[index].release : end;
        int wait = pending ? 1000 : int(next - now) + 1;
        pollfd fds{.fd = tty, .events = short(POLLIN | (pending ? POLLOUT : 0))};
        int ready = poll(&fds, 1, wait < 1000 ? wait : 1000);
        if (ready < 0 && errno != EINTR)
            fail(std::string("poll: ") + strerror(errno));
        if (ready <= 0)
            continue;
        readInput(tty, fds.revents, input);
        if (!pending || (fds.revents & POLLOUT) == 0)
            continue;
        const Chunk& chunk = chunks[index];
        size_t size = chunk.size - offset < kWriteBytes ? chunk.size - offset : kWriteBytes;
        ssize_t put = write(tty, chunk.data + offset, size);
        if (put < 0 && errno != EAGAIN && errno != EINTR)
            fail(std::string("write: ") + strerror(errno));
        if (put > 0) {
            offset += size_t(put);
            *written += size_t(put);
        }
        if (offset == chunk.size) {
            ++index;
            offset = 0;
        }
    }
    // The only cursor position report to expect is the answer to the DSR below, because no workload asks for one.
    input.clear();
    const char query[] = "\x1b[6n";
    size_t sent = 0;
    double deadline = prTimeMonotonicMilliseconds() + kAnswerTimeout;
    for (;;) {
        if (prTimeMonotonicMilliseconds() > deadline)
            fail("the terminal did not answer DSR 6");
        pollfd fds{.fd = tty, .events = short(POLLIN | (sent < sizeof(query) - 1 ? POLLOUT : 0))};
        int ready = poll(&fds, 1, 1000);
        if (ready < 0 && errno != EINTR)
            fail(std::string("poll: ") + strerror(errno));
        if (ready <= 0)
            continue;
        readInput(tty, fds.revents, input);
        if (sent == sizeof(query) - 1 && answered(input))
            return complete;
        if ((fds.revents & POLLOUT) != 0 && sent < sizeof(query) - 1) {
            ssize_t put = write(tty, query + sent, sizeof(query) - 1 - sent);
            if (put > 0)
                sent += size_t(put);
        }
    }
}

// Writes `bytes` and waits for DSR's answer. The only timeout is the answer's (kAnswerTimeout).
void writeAndWait(int tty, const std::string& bytes) {
    size_t written = 0;
    play(tty, {{bytes.data(), bytes.size(), 0}}, 0, 1e12, &written);
}

void load(Loaded& loaded, const std::string& name) {
    const FeedWorkload* workload = nullptr;
    for (const FeedWorkload& candidate : kWorkloads) {
        if (name == candidate.name)
            workload = &candidate;
    }
    if (workload == nullptr)
        fail("no workload named " + name);
    loaded = Loaded{.workload = workload};
    size_t size = 0;
    if (workload->generate != nullptr) {
        loaded.bytes = workload->generate(workload->amount);
        size = loaded.bytes.size();
    } else {
        loaded.enter = tuiEnter();
        loaded.frames = tuiFrames(uint32_t(workload->amount * kFrameRate));
        size = loaded.enter.size() + strlen(kTuiLeave);
        for (const std::string& frame : loaded.frames)
            size += frame.size();
    }
    report("loaded " + std::to_string(size));
}

// Writes `share` of the loaded workload: a prefix of its bytes that ends at a line's end, or its first frames. Each
// frame of a paced workload is bracketed by synchronized output (mode 2026), as a TUI brackets it, so that a terminal
// that supports the mode draws whole frames.
void run(int tty, const Loaded& loaded, double share, double timeout) {
    if (loaded.workload == nullptr)
        fail("run before load");
    static const char kBegin[] = "\x1b[?2026h";
    static const char kEnd[] = "\x1b[?2026l";
    std::vector<Chunk> chunks;
    double end = 0;
    if (loaded.workload->kind == FeedKind_Paced) {
        size_t count = size_t(double(loaded.frames.size()) * share);
        double interval = 1000.0 / kFrameRate;
        chunks.push_back({loaded.enter.data(), loaded.enter.size(), 0});
        for (size_t i = 0; i < count; ++i) {
            chunks.push_back({kBegin, sizeof(kBegin) - 1, double(i) * interval});
            chunks.push_back({loaded.frames[i].data(), loaded.frames[i].size(), double(i) * interval});
            chunks.push_back({kEnd, sizeof(kEnd) - 1, double(i) * interval});
        }
        end = double(count) * interval;
        chunks.push_back({kTuiLeave, strlen(kTuiLeave), end});
    } else {
        size_t size = loaded.bytes.size();
        if (share < 1) {
            size = loaded.bytes.find('\n', size_t(double(size) * share));
            size = size == std::string::npos ? loaded.bytes.size() : size + 1;
        }
        chunks.push_back({loaded.bytes.data(), size, 0});
    }
    winsize extent{};
    ioctl(tty, TIOCGWINSZ, &extent);
    size_t written = 0;
    PrTimePerf begin = prTimePerfNow();
    bool complete = play(tty, chunks, end, timeout, &written);
    double elapsed = prTimePerfMilliseconds(begin, prTimePerfNow());
    char line[160];
    snprintf(line, sizeof(line), "done %.3f %u %u %zu %d", elapsed, unsigned(extent.ws_col), unsigned(extent.ws_row),
        written, complete ? 1 : 0);
    report(line);
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && strcmp(argv[1], "--list") == 0) {
        for (const FeedWorkload& workload : kWorkloads) {
            printf("%s\t%llu\t%s\n", kKindNames[workload.kind], (unsigned long long)workload.amount, workload.name);
        }
        return 0;
    }
    if (argc != 2) {
        fprintf(stderr, "usage: terminal_feed --list | terminal_feed DIRECTORY\n");
        return 2;
    }
    alarm(kLifetimeSeconds);
    std::string directory = argv[1];
    reportFd = open((directory + "/report").c_str(), O_WRONLY);
    if (reportFd < 0) {
        perror("terminal_feed: report");
        return 1;
    }

    int tty = STDIN_FILENO;
    termios saved{};
    if (!isatty(tty) || tcgetattr(tty, &saved) != 0)
        fail("the standard input is not a tty");
    termios raw = saved;
    cfmakeraw(&raw);
    if (tcsetattr(tty, TCSAFLUSH, &raw) != 0)
        fail(std::string("tcsetattr: ") + strerror(errno));
    int flags = fcntl(tty, F_GETFL);
    fcntl(tty, F_SETFL, flags | O_NONBLOCK);
    writeAndWait(tty, "\x1b[m\x1b[H\x1b[2J");
    report("ready " + std::to_string(getpid()));

    int commands = open((directory + "/commands").c_str(), O_RDONLY);
    if (commands < 0)
        fail("no commands");
    Loaded loaded;
    std::string pending;
    for (;;) {
        size_t newline = pending.find('\n');
        if (newline == std::string::npos) {
            char buffer[256];
            ssize_t got = read(commands, buffer, sizeof(buffer));
            if (got < 0 && errno == EINTR)
                continue;
            if (got <= 0)
                break;
            pending.append(buffer, size_t(got));
            continue;
        }
        std::string command = pending.substr(0, newline);
        pending.erase(0, newline + 1);
        if (command.compare(0, 5, "load ") == 0) {
            load(loaded, command.substr(5));
        } else if (command == "clear") {
            writeAndWait(tty, "\x1b[m\x1b[H\x1b[2J\x1b[3J");
            report("cleared");
        } else if (command.compare(0, 4, "run ") == 0) {
            double share = 0;
            double timeout = 0;
            if (sscanf(command.c_str() + 4, "%lf %lf", &share, &timeout) != 2 || share <= 0 || share > 1)
                fail("run takes SHARE TIMEOUT: " + command);
            run(tty, loaded, share, timeout);
        } else {
            fail("no command " + command);
        }
    }
    fcntl(tty, F_SETFL, flags);
    tcsetattr(tty, TCSAFLUSH, &saved);
    return 0;
}
