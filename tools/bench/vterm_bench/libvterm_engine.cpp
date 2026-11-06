// The vterm benchmark's adapter for libvterm. It drives libvterm's screen layer, and keeps in its own scrollback the
// lines that scroll off the top of the screen.

#include "tools/bench/vterm_bench/libvterm_engine.h"

#include "pr/assert.h"
#include "pr/mem.h"
#include "pr/memory.h"
#include "pr/raw_array.h"
#include "pr/utf8.h"

#include "vterm.h"

namespace vterm_bench {
namespace {

using namespace pr;

// A line of the scrollback, at the width of the screen that it scrolled off.
struct ScrollbackLine {
    RawArray<VTermScreenCell> cells;    // by column
};

// libvterm expects its allocations to be zeroed. They come from the engine's allocator, as a PrVterm's allocations do.
// The context points to the count of bytes that the terminal holds. Each allocation adds its usable size to the count,
// and each release subtracts it.
void* allocate(size_t size, void* context) {
    void* memory = prMemoryAllocZeroed(size);
    *static_cast<size_t*>(context) += prMemoryUsableSize(memory);
    return memory;
}

void release(void* memory, void* context) {
    if (memory == nullptr)
        return;
    *static_cast<size_t*>(context) -= prMemoryUsableSize(memory);
    prMemoryFree(memory);
}

constexpr VTermAllocatorFunctions kAllocator = {.malloc = allocate, .free = release};

void dropOutput(const char*, size_t, void*) {}

// libvterm marks the second cell of a wide character with this code point.
constexpr uint32_t kWideTail = 0xFFFFFFFFu;

void appendCell(const VTermScreenCell& cell, PrRawString* out) {
    if (cell.chars[0] == kWideTail)
        return;
    if (cell.chars[0] == 0) {
        prRawStringAppend(out, " ");
        return;
    }
    for (uint32_t i = 0; i < VTERM_MAX_CHARS_PER_CELL && cell.chars[i] != 0; ++i) {
        char text[kPrUtf8MaxBytes];
        prRawStringAppend(out, PrStringView{text, prUtf8Encode(cell.chars[i], text)});
    }
}

// A libvterm terminal and its scrollback. The scrollback is a ring of lines that grows to `limit_`. After that, each
// line that it adds replaces the oldest line and reuses its storage.
class Libvterm final : public Engine {
public:
    Libvterm() = default;
    Libvterm(const Libvterm&) = delete;
    Libvterm& operator=(const Libvterm&) = delete;
    ~Libvterm() override {
        destroy();
        for (uint32_t i = 0; i < lines_.count; ++i)
            lines_.data[i].cells.reset();
        lines_.reset();
    }

    const char* name() const override { return "libvterm"; }

    void create(PrUVec2 extent, uint32_t scrollback) override {
        PR_ASSERT(extent.x > 0 && extent.y > 0);
        destroy();
        limit_ = scrollback;
        terminal_ = vterm_new_with_allocator(int(extent.y), int(extent.x),
            const_cast<VTermAllocatorFunctions*>(&kAllocator), &allocated_);
        vterm_set_utf8(terminal_, 1);
        vterm_output_set_callback(terminal_, dropOutput, nullptr);
        screen_ = vterm_obtain_screen(terminal_);
        vterm_screen_set_callbacks(screen_, &kCallbacks, this);
        vterm_screen_enable_altscreen(screen_, 1);
        vterm_screen_enable_reflow(screen_, true);
        vterm_screen_reset(screen_, 1);
    }

    void destroy() override {
        if (terminal_ != nullptr)
            vterm_free(terminal_);
        terminal_ = nullptr;
        screen_ = nullptr;
        start_ = 0;
        count_ = 0;
    }

    void write(PrByteView bytes) override {
        if (bytes.size == 0)
            return;
        vterm_input_write(terminal_, reinterpret_cast<const char*>(bytes.data), bytes.size);
    }

    void resize(PrUVec2 extent) override {
        PR_ASSERT(extent.x > 0 && extent.y > 0);
        vterm_set_size(terminal_, int(extent.y), int(extent.x));
    }

    uint32_t scrollbackCount() const override { return count_; }

    // What libvterm allocated, mostly the cells of its screens, plus the scrollback lines that the adapter holds. The
    // ring keeps the storage of lines from an earlier terminal, which does not count.
    size_t linesMemorySize() const override {
        size_t size = allocated_ + sizeof(ScrollbackLine) * count_;
        for (uint32_t i = 0; i < count_; ++i)
            size += sizeof(VTermScreenCell) * lineAt(i).cells.capacity;
        return size;
    }

    void lineAppendText(uint32_t index, uint32_t columns, PrRawString* out) const override {
        if (index < count_) {
            const ScrollbackLine& line = lineAt(index);
            for (uint32_t column = 0; column < columns && column < line.cells.count; ++column)
                appendCell(line.cells.data[column], out);
            return;
        }
        int rows = 0;
        int width = 0;
        vterm_get_size(terminal_, &rows, &width);
        PR_ASSERT(index - count_ < uint32_t(rows));
        VTermPos position = {.row = int(index - count_)};
        for (uint32_t column = 0; column < columns && column < uint32_t(width); ++column) {
            position.col = int(column);
            VTermScreenCell cell{};
            vterm_screen_get_cell(screen_, position, &cell);
            appendCell(cell, out);
        }
    }

    uint64_t scrollbackLineRead(uint32_t index) const override {
        const ScrollbackLine& line = lineAt(index);
        uint64_t sum = 0;
        for (uint32_t i = 0; i < line.cells.count; ++i)
            sum += line.cells.data[i].chars[0];
        return sum;
    }

private:
    ScrollbackLine& lineAt(uint32_t index) { return lines_.data[(start_ + index) % limit_]; }
    const ScrollbackLine& lineAt(uint32_t index) const { return lines_.data[(start_ + index) % limit_]; }

    static int pushLine(int columns, const VTermScreenCell* cells, void* context) {
        Libvterm* vterm = static_cast<Libvterm*>(context);
        if (vterm->limit_ == 0)
            return 1;
        ScrollbackLine* line;
        if (vterm->count_ < vterm->limit_) {
            // The ring has not wrapped yet, so the next slot follows the newest line. The slot is created if the ring
            // has never held this many lines.
            if (vterm->lines_.count == vterm->count_)
                vterm->lines_.push(ScrollbackLine{});
            line = &vterm->lineAt(vterm->count_);
            ++vterm->count_;
        } else {
            line = &vterm->lines_.data[vterm->start_];
            vterm->start_ = (vterm->start_ + 1) % vterm->limit_;
        }
        line->cells.clear();
        line->cells.append(cells, uint32_t(columns));
        return 1;
    }

    static int popLine(int columns, VTermScreenCell* cells, void* context) {
        Libvterm* vterm = static_cast<Libvterm*>(context);
        if (vterm->count_ == 0)
            return 0;
        --vterm->count_;
        const ScrollbackLine& line = vterm->lineAt(vterm->count_);
        uint32_t kept = line.cells.count < uint32_t(columns) ? line.cells.count : uint32_t(columns);
        prMemCopy(cells, line.cells.data, size_t(kept) * sizeof(VTermScreenCell));
        // Pad a line that is narrower than the screen with blank cells. Each blank is one cell wide, because libvterm
        // steps through a line by the cells' widths.
        VTermScreenCell blank{};
        blank.width = 1;
        vterm_state_get_default_colors(vterm_obtain_state(vterm->terminal_), &blank.fg, &blank.bg);
        for (uint32_t column = kept; column < uint32_t(columns); ++column)
            cells[column] = blank;
        return 1;
    }

    static int clearScrollback(void* context) {
        Libvterm* vterm = static_cast<Libvterm*>(context);
        vterm->start_ = 0;
        vterm->count_ = 0;
        return 1;
    }

    static constexpr VTermScreenCallbacks kCallbacks =
        {.sb_pushline = pushLine, .sb_popline = popLine, .sb_clear = clearScrollback};

    VTerm* terminal_ = nullptr;
    VTermScreen* screen_ = nullptr;
    size_t allocated_ = 0;              // the bytes that libvterm holds
    RawArray<ScrollbackLine> lines_{};
    uint32_t start_ = 0;                // the slot of the oldest line
    uint32_t count_ = 0;
    uint32_t limit_ = 0;
};

} // namespace

std::unique_ptr<Engine> makeLibvterm() {
    return std::make_unique<Libvterm>();
}

} // namespace vterm_bench
