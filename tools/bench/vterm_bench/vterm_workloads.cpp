// The vterm benchmark's workloads (vterm_workloads.h).

#include "tools/bench/vterm_bench/vterm_workloads.h"

#include "pr/random.h"
#include "pr/raw_string.h"
#include "pr/utf8.h"

#include "tools/harness/harness.h"

#include <cstdio>
#include <cstdlib>

using namespace pr;
using namespace harness;

namespace vterm_bench {

namespace {

// The size of each generated workload of the vterm benchmark.
constexpr size_t kWorkloadBytes = 1u << 20;

void appendCodePoint(std::string& out, uint32_t codePoint) {
    char text[kPrUtf8MaxBytes];
    out.append(text, prUtf8Encode(codePoint, text));
}

uint32_t between(PrRandom& random, uint32_t low, uint32_t high) {
    return low + prRandomU32Below(&random, high - low + 1);
}

// A word of `size` letters, digits and punctuation, like the text of a log.
void appendWord(std::string& out, PrRandom& random, uint32_t size) {
    static const char kLetters[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-./:=";
    for (uint32_t i = 0; i < size; ++i)
        out += kLetters[prRandomU32Below(&random, sizeof(kLetters) - 1)];
}

// Words separated by spaces, `size` characters in all.
void appendText(std::string& out, PrRandom& random, uint32_t size) {
    uint32_t written = 0;
    while (written < size) {
        uint32_t word = between(random, 1, 12);
        if (word > size - written)
            word = size - written;
        appendWord(out, random, word);
        written += word;
        if (written < size) {
            out += ' ';
            ++written;
        }
    }
}

// Returns the two decimal digits of `value`, which must be below 100.
void appendTwoDigits(std::string& out, uint32_t value) {
    out += char('0' + value / 10);
    out += char('0' + value % 10);
}

// A line of a service's log: a timestamp in gray, a level in its color, a component in magenta, and a message. The
// message is a request with its status, fields of key=value with the keys in cyan, or text. One line in twenty carries
// a payload that wraps across rows.
void appendLogLine(std::string& out, PrRandom& random, uint64_t& milliseconds) {
    struct Level {
        const char* text;
        uint32_t weight;
    };
    static const Level kLevels[] = {
        {"\x1b[32mINFO \x1b[0m", 60},
        {"\x1b[36mDEBUG\x1b[0m", 25},
        {"\x1b[1;33mWARN \x1b[0m", 11},
        {"\x1b[1;31mERROR\x1b[0m", 4},
    };
    static const char* const kComponents[] = {
        "http.server", "db.pool", "cache", "scheduler", "auth", "worker", "grpc.client", "storage",
    };
    static const char* const kMethods[] = {"GET", "POST", "PUT", "DELETE"};
    static const char* const kKeys[] = {"user", "id", "region", "attempt", "latency", "trace", "shard", "bytes"};

    milliseconds += between(random, 0, 40);
    uint64_t seconds = milliseconds / 1000;
    out += "\x1b[90m2026-11-06 ";
    appendTwoDigits(out, uint32_t(seconds / 3600 % 24));
    out += ':';
    appendTwoDigits(out, uint32_t(seconds / 60 % 60));
    out += ':';
    appendTwoDigits(out, uint32_t(seconds % 60));
    out += '.';
    out += std::to_string(100 + milliseconds % 900);
    out += "\x1b[0m ";
    uint32_t pick = prRandomU32Below(&random, 100);
    for (const Level& level : kLevels) {
        if (pick < level.weight) {
            out += level.text;
            break;
        }
        pick -= level.weight;
    }
    out += " \x1b[35m";
    out += kComponents[prRandomU32Below(&random, 8)];
    out += "\x1b[0m: ";
    switch (prRandomU32Below(&random, 20)) {
        case 0:
            out += "payload ";
            appendText(out, random, between(random, 150, 600));
            break;
        case 1:
        case 2:
        case 3:
        case 4:
        case 5:
        case 6:
        case 7: {
            uint32_t status = prRandomU32Below(&random, 10) == 0 ? 500 + between(random, 0, 4) : 200;
            out += kMethods[prRandomU32Below(&random, 4)];
            out += " /api/v1/";
            appendWord(out, random, between(random, 3, 10));
            out += "/" + std::to_string(between(random, 1, 99999)) + " -> ";
            out += status == 200 ? "\x1b[32m" : "\x1b[31m";
            out += std::to_string(status) + "\x1b[0m in " + std::to_string(between(random, 1, 900)) + "." +
                std::to_string(between(random, 0, 9)) + " ms";
            break;
        }
        case 8:
        case 9:
        case 10:
        case 11:
        case 12:
        case 13: {
            appendText(out, random, between(random, 8, 30));
            uint32_t fields = between(random, 1, 5);
            for (uint32_t i = 0; i < fields; ++i) {
                out += " \x1b[36m";
                out += kKeys[prRandomU32Below(&random, 8)];
                out += "\x1b[0m=";
                appendWord(out, random, between(random, 2, 12));
            }
            break;
        }
        default:
            appendText(out, random, between(random, 10, 80));
            break;
    }
    out += "\r\n";
}

// A line of mostly wide characters, with ASCII words between them. Half of the lines have an odd width, so a line that
// wraps often leaves no room for a wide character at the edge.
void appendCjkLine(std::string& out, PrRandom& random) {
    struct Range {
        uint32_t first;
        uint32_t count;
    };
    static const Range kRanges[] = {{0x4E00, 0x5200}, {0x3041, 0x56}, {0x30A1, 0x5A}, {0xAC00, 0x2BA4}};
    uint32_t width = between(random, 0, kExtent.x + 40);
    uint32_t written = 0;
    while (written < width) {
        if (prRandomU32Below(&random, 8) == 0) {
            uint32_t size = between(random, 1, 6);
            appendWord(out, random, size);
            out += ' ';
            written += size + 1;
            continue;
        }
        const Range& range = kRanges[prRandomU32Below(&random, 4)];
        appendCodePoint(out, range.first + prRandomU32Below(&random, range.count));
        written += 2;
    }
    out += "\r\n";
}

// A line of Latin letters with one to three combining marks, and Devanagari syllables: a consonant, a virama and a
// consonant, or a consonant and a vowel sign.
void appendCombiningLine(std::string& out, PrRandom& random) {
    uint32_t width = between(random, 0, kExtent.x - 1);
    for (uint32_t column = 0; column < width; ++column) {
        uint32_t choice = prRandomU32Below(&random, 8);
        if (choice == 0) {
            out += ' ';
        } else if (choice < 6) {
            out += char('a' + prRandomU32Below(&random, 26));
            uint32_t marks = between(random, 1, 3);
            for (uint32_t i = 0; i < marks; ++i)
                appendCodePoint(out, 0x0300 + prRandomU32Below(&random, 0x70));
        } else {
            appendCodePoint(out, 0x0915 + prRandomU32Below(&random, 0x25));
            if (choice == 6) {
                appendCodePoint(out, 0x094D);
                appendCodePoint(out, 0x0915 + prRandomU32Below(&random, 0x25));
            } else {
                appendCodePoint(out, 0x093E + prRandomU32Below(&random, 0x0F));
            }
        }
    }
    out += "\r\n";
}

// A line of emoji between words: alone, with a variation selector, with a skin tone, in ZWJ sequences of families and
// professions, as flags of regional indicators, and as keycaps.
void appendEmojiLine(std::string& out, PrRandom& random) {
    uint32_t count = between(random, 0, 24);
    for (uint32_t i = 0; i < count; ++i) {
        switch (prRandomU32Below(&random, 7)) {
            case 0:
                appendCodePoint(out, 0x1F600 + prRandomU32Below(&random, 0x50));
                break;
            case 1: {
                static const uint32_t kText[] = {0x2764, 0x263A, 0x2600, 0x270F, 0x2708};
                appendCodePoint(out, kText[prRandomU32Below(&random, 5)]);
                appendCodePoint(out, 0xFE0F);
                break;
            }
            case 2:
                appendCodePoint(out, 0x1F44D + prRandomU32Below(&random, 4));
                appendCodePoint(out, 0x1F3FB + prRandomU32Below(&random, 5));
                break;
            case 3: {
                static const uint32_t kFamily[] = {0x1F468, 0x200D, 0x1F469, 0x200D, 0x1F467, 0x200D, 0x1F466};
                for (uint32_t codePoint : kFamily)
                    appendCodePoint(out, codePoint);
                break;
            }
            case 4: {
                static const uint32_t kProfession[] = {0x1F469, 0x1F3FD, 0x200D, 0x1F4BB};
                for (uint32_t codePoint : kProfession)
                    appendCodePoint(out, codePoint);
                break;
            }
            case 5:
                appendCodePoint(out, 0x1F1E6 + prRandomU32Below(&random, 26));
                appendCodePoint(out, 0x1F1E6 + prRandomU32Below(&random, 26));
                break;
            default:
                out += char('0' + prRandomU32Below(&random, 10));
                appendCodePoint(out, 0xFE0F);
                appendCodePoint(out, 0x20E3);
                break;
        }
        out += ' ';
        appendWord(out, random, between(random, 0, 3));
    }
    out += "\r\n";
}

// Returns the lines that `append` writes from `seed`, until they total at least `bytes`.
std::string linesOf(void (*append)(std::string&, PrRandom&), uint64_t seed, size_t bytes) {
    PrRandom random = {.state = seed};
    std::string out;
    while (out.size() < bytes)
        append(out, random);
    return out;
}

// Directory listings in the colors of ls --color: directories, executables, links, and files colored by extension in
// the 256-color palette, in columns.
std::string sgr256() {
    static const char* const kKinds[] = {"01;34", "01;32", "01;36", "00", "01;35", "40;31;01"};
    PrRandom random = {.state = 2};
    std::string out;
    while (out.size() < kWorkloadBytes) {
        for (uint32_t column = 0; column < 6; ++column) {
            out += "\x1b[0m\x1b[";
            if (prRandomU32Below(&random, 3) == 0) {
                out += "38;5;" + std::to_string(between(random, 16, 231));
            } else {
                out += kKinds[prRandomU32Below(&random, 6)];
            }
            out += 'm';
            uint32_t size = between(random, 3, 16);
            appendWord(out, random, size);
            out += "\x1b[0m";
            out.append(20 - size, ' ');
        }
        out += "\r\n";
    }
    return out;
}

void appendRgb(std::string& out, PrRandom& random, bool background) {
    out += background ? "\x1b[48;2;" : "\x1b[38;2;";
    out += std::to_string(prRandomU32Below(&random, 256)) + ";" + std::to_string(prRandomU32Below(&random, 256)) + ";" +
        std::to_string(prRandomU32Below(&random, 256)) + "m";
}

// Source code highlighted as bat writes it, with a gutter of line numbers, and diffs as delta writes them. Added and
// removed lines have a background that EL extends to the screen's edge.
std::string sgrTruecolor() {
    PrRandom random = {.state = 3};
    std::string out;
    uint32_t number = 1;
    while (out.size() < kWorkloadBytes) {
        bool diff = prRandomU32Below(&random, 3) == 0;
        out += "\x1b[38;2;98;114;164m";
        std::string gutter = std::to_string(number++);
        out.append(6 - gutter.size(), ' ');
        out += gutter + " \xe2\x94\x82 ";
        if (diff)
            appendRgb(out, random, true);
        uint32_t width = 9;
        uint32_t tokens = between(random, 1, 12);
        for (uint32_t i = 0; i < tokens && width < kExtent.x - 16; ++i) {
            appendRgb(out, random, false);
            if (prRandomU32Below(&random, 5) == 0)
                out += "\x1b[1m";
            uint32_t size = between(random, 1, 10);
            appendWord(out, random, size);
            out += ' ';
            width += size + 1;
            if (!diff)
                out += "\x1b[0m";
        }
        out += diff ? "\x1b[0K\x1b[0m\r\n" : "\r\n";
    }
    return out;
}

void appendCursor(std::string& out, uint32_t row, uint32_t column) {
    out += "\x1b[" + std::to_string(row) + ";" + std::to_string(column) + "H";
}

// One frame of htop: the meters at the top, in runs of bold and colored bars, a header in reverse colors, and processes
// in columns of 256-color SGR, one highlighted. Each row is redrawn by CUP and ends in EL.
void appendTuiFrame(std::string& out, PrRandom& random, uint32_t frame) {
    out += "\x1b[?25l";
    for (uint32_t row = 1; row <= kExtent.y; ++row) {
        appendCursor(out, row, 1);
        if (row <= 4) {
            out += "\x1b[1m\x1b[36m" + std::to_string(row - 1) + "\x1b[m\x1b[1m[";
            uint32_t used = between(random, 0, 50);
            out += "\x1b[32m" + std::string(used / 2, '|') + "\x1b[31m" + std::string(used / 2, '|');
            out += std::string(50 - used / 2 * 2, ' ') + "\x1b[m\x1b[1m]\x1b[m";
        } else if (row == 6) {
            out += "\x1b[30;42m    PID USER      PRI  NI  VIRT   RES   SHR S CPU% MEM%   TIME+  Command";
        } else if (row > 6) {
            bool selected = row == 7 + frame % (kExtent.y - 7);
            out += selected ? "\x1b[30;46m" : "\x1b[m";
            out += "\x1b[38;5;" + std::to_string(between(random, 16, 231)) + "m";
            out += std::to_string(between(random, 1000, 99999)) + " ";
            out += "\x1b[38;5;244mroot      \x1b[m\x1b[38;5;" + std::to_string(between(random, 16, 231)) + "m";
            out += std::to_string(between(random, 0, 39)) + "   0 ";
            out += "\x1b[1m" + std::to_string(between(random, 100, 999)) + "M\x1b[m ";
            out += std::to_string(between(random, 0, 99)) + "." + std::to_string(between(random, 0, 9)) + " ";
            out += "\x1b[32m";
            appendWord(out, random, between(random, 8, 40));
        }
        out += "\x1b[K";
    }
    out += "\x1b[m\x1b[?25h";
}

// Frames of htop on the alternate screen, until they are kWorkloadBytes long.
std::string tuiRedraw() {
    PrRandom random = {.state = 7};
    std::string out = tuiEnter();
    for (uint32_t frame = 0; out.size() < kWorkloadBytes; ++frame)
        appendTuiFrame(out, random, frame);
    out += kTuiLeave;
    return out;
}

// The lines that fill the scrollback before a resize. Each is up to three rows wide, or empty.
std::string filledScrollback() {
    PrRandom random = {.state = 9};
    std::string out;
    for (uint32_t line = 0; line < kScrollback + kExtent.y; ++line) {
        uint32_t choice = prRandomU32Below(&random, 8);
        uint32_t size =
            choice == 0 ? 0 : choice < 6 ? between(random, 1, kExtent.x) : between(random, 1, 3 * kExtent.x);
        appendText(out, random, size);
        out += "\r\n";
    }
    return out;
}

// One line of the unicode workload: wide characters, combining marks or emoji, in a ratio of 2:1:1.
void appendUnicodeLine(std::string& out, PrRandom& random) {
    switch (prRandomU32Below(&random, 4)) {
        case 0:
        case 1:
            appendCjkLine(out, random);
            break;
        case 2:
            appendCombiningLine(out, random);
            break;
        default:
            appendEmojiLine(out, random);
            break;
    }
}

// The captures and their extents, as captures/README.md records them.
struct Capture {
    const char* name;
    PrUVec2 extent;
};

constexpr Capture kCaptures[] = {
    {"banner", {60, 12}},
    {"donut", {80, 24}},
    {"doom-fire", {80, 24}},
    {"ocean-sunset", {100, 30}},
};

} // namespace

std::string asciiFlood(size_t bytes) {
    PrRandom random = {.state = 1};
    std::string out;
    while (out.size() < bytes) {
        appendText(out, random, between(random, 0, kExtent.x - 1));
        out += "\r\n";
    }
    return out;
}

std::string logLines(size_t bytes) {
    PrRandom random = {.state = 10};
    uint64_t milliseconds = 9 * 3600 * 1000;
    std::string out;
    while (out.size() < bytes)
        appendLogLine(out, random, milliseconds);
    return out;
}

std::string unicodeLines(size_t bytes) {
    return linesOf(appendUnicodeLine, 11, bytes);
}

std::string longLines(size_t bytes) {
    PrRandom random = {.state = 8};
    std::string out;
    while (out.size() < bytes) {
        appendText(out, random, between(random, 1000, 8000));
        out += "\r\n";
    }
    return out;
}

std::string fullLines(uint32_t count) {
    PrRandom random = {.state = 12};
    std::string out;
    for (uint32_t line = 0; line < count; ++line) {
        appendText(out, random, kExtent.x);
        out += "\r\n";
    }
    return out;
}

std::string tuiEnter() {
    return "\x1b[?1049h\x1b[22;0;0t\x1b[1;" + std::to_string(kExtent.y) + "r\x1b[m\x1b[H\x1b[2J";
}

std::vector<std::string> tuiFrames(uint32_t count) {
    PrRandom random = {.state = 7};
    std::vector<std::string> out(count);
    for (uint32_t frame = 0; frame < count; ++frame)
        appendTuiFrame(out[frame], random, frame);
    return out;
}

std::vector<Workload> workloads() {
    std::vector<Workload> out;
    for (const Capture& capture : kCaptures) {
        PrTextBuffer file{};
        std::string path = std::string(PR_VTERM_CAPTURES_DIR) + "/" + capture.name + ".tty";
        if (!harnessReadFile(&file, path.c_str())) {
            fprintf(stderr, "cannot read %s\n", path.c_str());
            exit(1);
        }
        out.push_back({
            std::string("capture ") + capture.name, capture.extent, WorkloadKind_Write,
            std::string(file.data, file.size),
        });
        prTextBufferReset(&file);
    }
    out.push_back({"ascii flood", kExtent, WorkloadKind_Write, asciiFlood(kWorkloadBytes)});
    out.push_back({"log", kExtent, WorkloadKind_Write, logLines(kWorkloadBytes)});
    out.push_back({"sgr 256", kExtent, WorkloadKind_Write, sgr256()});
    out.push_back({"sgr truecolor", kExtent, WorkloadKind_Write, sgrTruecolor()});
    out.push_back({"cjk", kExtent, WorkloadKind_Write, linesOf(appendCjkLine, 4, kWorkloadBytes)});
    out.push_back({"combining", kExtent, WorkloadKind_Write, linesOf(appendCombiningLine, 5, kWorkloadBytes)});
    out.push_back({"emoji", kExtent, WorkloadKind_Write, linesOf(appendEmojiLine, 6, kWorkloadBytes)});
    out.push_back({"tui redraw", kExtent, WorkloadKind_Write, tuiRedraw()});
    out.push_back({"long lines", kExtent, WorkloadKind_Write, longLines(kWorkloadBytes)});
    out.push_back({"resize rewrap", kExtent, WorkloadKind_Resize, filledScrollback()});
    out.push_back({"scrollback read", kExtent, WorkloadKind_Read, logLines(kWorkloadBytes)});
    return out;
}

} // namespace vterm_bench
