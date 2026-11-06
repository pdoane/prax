// The engine interface of the vterm benchmark. Each adapter holds one terminal of its engine. The adapter writes
// program output to the terminal, resizes it, and reads its text, so that the benchmark and the check can compare the
// engines' screens.

#pragma once

#include "pr/base.h"
#include "pr/byte_view.h"
#include "pr/raw_string.h"
#include "pr/vec.h"

#include <cstdint>
#include <memory>

PR_ASSUME_NONNULL_BEGIN

namespace vterm_bench {

// An engine's adapter: a terminal with a screen, an alternate screen and a scrollback, which parses UTF-8.
class Engine {
public:
    virtual ~Engine() = default;

    virtual const char* name() const = 0;

    // Creates a terminal of `extent`, with an empty screen and up to `scrollback` lines of scrollback, and replaces any
    // terminal that the adapter holds. Destroy frees the terminal.
    virtual void create(PrUVec2 extent, uint32_t scrollback) = 0;
    virtual void destroy() = 0;

    // Parses `bytes` that the program wrote, and applies them to the screen. The terminal's answers to the program are
    // dropped.
    virtual void write(PrByteView bytes) = 0;

    // Resizes the screen to `extent`, whose columns and rows are each at least one.
    virtual void resize(PrUVec2 extent) = 0;

    // The number of lines of scrollback that the terminal holds. They come before the screen's rows in line indices.
    virtual uint32_t scrollbackCount() const = 0;

    // The bytes of memory that the terminal holds for the lines of its screens and its scrollback.
    virtual size_t linesMemorySize() const = 0;

    // Appends to `out`, in UTF-8, the text of the first `columns` cells of line `index`. Lines are counted as
    // prVtermLine counts them. An empty cell appends a space, and the second cell of a wide character appends nothing.
    virtual void lineAppendText(uint32_t index, uint32_t columns, PrRawString* out) const = 0;

    // Reads every cell of scrollback line `index`, as a renderer of a view scrolled back would. Returns the sum of the
    // cells' first code points, so that the compiler cannot drop the reads.
    virtual uint64_t scrollbackLineRead(uint32_t index) const = 0;
};

} // namespace vterm_bench

PR_ASSUME_NONNULL_END
