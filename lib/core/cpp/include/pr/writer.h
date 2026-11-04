// Writers: sinks of text, which formatting writes into, with a buffer in front of where the text goes.

#pragma once

#include "pr/base.h"
#include "pr/span.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr {

// Takes `text` from a writer's buffer, or from a write too large for it, to where the writer's text goes. Returns false
// if the destination takes no more, as a closed stream does.
typedef bool (*WriterDrainFn)(void* PR_NULLABLE context, PrStringView text);

// A sink of text. Writes collect in `buffer` and go to `drain`, with `context`, when it fills and when the writer is
// flushed. A writer with no drain is its buffer: text past its capacity is dropped, short of any partial UTF-8
// sequence, and `truncated` is set.
struct Writer {
    WriterDrainFn PR_NULLABLE drain;
    void* PR_NULLABLE context;
    char* PR_NULLABLE buffer;
    uint32_t capacity;
    uint32_t size;               // bytes in the buffer that have not been drained
    bool truncated;              // true once text was dropped, by a full buffer or a drain that failed

    // Write `text`, the byte `c`, or `count` copies of `c`.
    void write(PrStringView text);
    void writeChar(char c);
    void writeRepeated(char c, uint32_t count);

    // Writes `text` within `width` bytes, padded by `fill`: `<` puts the padding after the text, `^` around it, and
    // anything else before it.
    void writePadded(PrStringView text, uint32_t width, char fill, char align);

    // Drains the buffer. Returns false if any text was dropped.
    bool flush();

    // The text in the buffer, which for a writer with no drain is all of its text.
    PrStringView view() const { return PrStringView{buffer, size}; }
};

// Returns a writer whose text is `buffer`, with no drain, so that it truncates what does not fit.
inline Writer bufferWriter(Span<char> buffer) {
    return Writer{.buffer = buffer.data(), .capacity = buffer.count()};
}

} // namespace pr

PR_ASSUME_NONNULL_END
