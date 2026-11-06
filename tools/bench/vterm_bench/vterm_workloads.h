// The vterm benchmark's workloads: what programs write to a terminal. They are the captures under
// lib/vterm/tests/captures, played at the extent that each was recorded at, and generated output, at 120x40 with 10,000
// lines of scrollback:
//
//   ascii flood       lines of plain text, as cat or a build's log writes them, which scroll and fill the scrollback
//   log               a service's log: timestamps, levels and components in color, requests and fields
//   sgr 256           ls --color: names in the colors of 256-color and bold SGR, each run reset after it
//   sgr truecolor     bat and delta: tokens in 24-bit colors, with 24-bit backgrounds that EL extends to the edge
//   cjk               lines of wide Han, kana and Hangul characters with ASCII, some wrapping at the edge
//   combining         Latin letters with stacked combining marks, and Devanagari's vowel signs and viramas
//   emoji             emoji alone and with variation selectors, skin tones, ZWJ sequences, flags and keycaps
//   tui redraw        htop: frames on the alternate screen, each row redrawn by CUP, SGR for each run, and EL
//   long lines        lines of 1,000 to 8,000 characters, which wrap across many rows
//   resize rewrap     a full scrollback of lines, some wrapped, rewrapped by a resize between 120 and 97 columns
//   scrollback read   the cells of every line of a full scrollback of the log, read as a view scrolled back draws them
//
// The vterm benchmark writes them to engines in-process. The terminals benchmark's feed writes larger output of the
// same generators to a terminal application's tty. Each generated workload comes from a fixed seed, so every run writes
// the same bytes.

#pragma once

#include "pr/base.h"
#include "pr/vec.h"

#include <cstdint>
#include <string>
#include <vector>

PR_ASSUME_NONNULL_BEGIN

namespace vterm_bench {

// The extent and the scrollback of the generated workloads' terminal.
constexpr PrUVec2 kExtent = {120, 40};
constexpr uint32_t kScrollback = 10000;

// The width, in columns, that the resize workload alternates with kExtent.x.
constexpr uint32_t kNarrowColumns = 97;

// What a workload does to a terminal.
enum {
    WorkloadKind_Write,     // writes `bytes`
    WorkloadKind_Resize,    // alternates the terminal's width, once `bytes` has filled it
    WorkloadKind_Read,      // reads the cells of each line of the scrollback, once `bytes` has filled it
};
typedef uint8_t WorkloadKind;

// A workload: its name, the extent of the terminal that it is written to, its kind, and its bytes.
struct Workload {
    std::string name;
    PrUVec2 extent;
    WorkloadKind kind;
    std::string bytes;
};

// Return lines generated from a fixed seed, until they total at least `bytes`: plain text; a service's log in color;
// wide characters, combining marks and emoji, line by line; and lines of 1,000 to 8,000 characters.
std::string asciiFlood(size_t bytes);
std::string logLines(size_t bytes);
std::string unicodeLines(size_t bytes);
std::string longLines(size_t bytes);

// Returns `count` lines of text, each kExtent.x columns wide.
std::string fullLines(uint32_t count);

// The bytes that a TUI writes to enter and to leave the alternate screen, and `count` frames of htop to write between
// them. Each frame redraws every row.
std::string tuiEnter();
constexpr char kTuiLeave[] = "\x1b[?1049l";
std::vector<std::string> tuiFrames(uint32_t count);

// Returns every workload: the captures, then the generated output. Exits if a capture cannot be read.
std::vector<Workload> workloads();

} // namespace vterm_bench

PR_ASSUME_NONNULL_END
