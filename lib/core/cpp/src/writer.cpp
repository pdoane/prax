// Writers: sinks of text, which formatting writes into, with a buffer in front of where the text goes.

#include "pr/writer.h"

#include "pr/mem.h"

namespace pr {
namespace {

// Returns the size of the longest prefix of the first `count` bytes of `text` that does not end inside a UTF-8
// sequence. A sequence that the prefix would cut is left out whole.
uint32_t wholeSequences(const char* text, uint32_t count) {
    uint32_t start = count;
    while (start > 0 && count - start < 3 && (uint8_t(text[start - 1]) & 0xc0) == 0x80)
        --start;
    if (start == 0)
        return count;
    uint8_t lead = uint8_t(text[start - 1]);
    uint32_t length = lead >= 0xf0 ? 4 : lead >= 0xe0 ? 3 : lead >= 0xc0 ? 2 : 1;
    uint32_t present = count - start + 1;
    return length > present ? start - 1 : count;
}

} // namespace

void Writer::write(PrStringView text) {
    if (text.isEmpty())
        return;
    if (text.size <= capacity - size) {
        prMemCopy(buffer + size, text.data, text.size);
        size += uint32_t(text.size);
        return;
    }
    if (drain == nullptr) {
        uint32_t room = wholeSequences(text.data, capacity - size);
        if (room != 0)
            prMemCopy(buffer + size, text.data, room);
        size += room;
        truncated = true;
        return;
    }
    // The buffered text goes first, and then the text itself, through the buffer if it fits there.
    if (!flush())
        return;
    if (text.size <= capacity) {
        prMemCopy(buffer, text.data, text.size);
        size = uint32_t(text.size);
        return;
    }
    if (!drain(context, text))
        truncated = true;
}

void Writer::writeChar(char c) {
    write(PrStringView{&c, 1});
}

void Writer::writeRepeated(char c, uint32_t count) {
    char run[64];
    prMemFill(run, uint8_t(c), sizeof(run));
    while (count != 0) {
        uint32_t step = count < sizeof(run) ? count : uint32_t(sizeof(run));
        write(PrStringView{run, step});
        count -= step;
    }
}

void Writer::writePadded(PrStringView text, uint32_t width, char fill, char align) {
    uint32_t padding = text.size < width ? width - uint32_t(text.size) : 0;
    uint32_t before = padding;
    if (align == '<')
        before = 0;
    else if (align == '^')
        before = padding / 2;
    writeRepeated(fill, before);
    write(text);
    writeRepeated(fill, padding - before);
}

bool Writer::flush() {
    if (drain != nullptr && size != 0) {
        if (!drain(context, PrStringView{buffer, size}))
            truncated = true;
        size = 0;
    }
    return !truncated;
}

} // namespace pr
