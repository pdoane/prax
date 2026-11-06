// The vterm benchmark's adapter for lib/vterm. It holds a PrVterm, and consumes the terminal's answers to the program
// after each write.

#include "tools/bench/vterm_bench/prax_engine.h"

#include "pr/vterm.h"

namespace vterm_bench {
namespace {

class Prax final : public Engine {
public:
    Prax() = default;
    Prax(const Prax&) = delete;
    Prax& operator=(const Prax&) = delete;
    ~Prax() override { destroy(); }

    const char* name() const override { return "prax"; }

    void create(PrUVec2 extent, uint32_t scrollback) override {
        destroy();
        PrVtermDesc desc{.extent = extent, .scrollback = scrollback};
        vterm_ = prVtermCreate(&desc);
    }

    void destroy() override {
        prVtermDestroy(vterm_);
        vterm_ = nullptr;
    }

    void write(PrByteView bytes) override {
        prVtermWrite(vterm_, bytes);
        prVtermOutputConsume(vterm_, prVtermOutput(vterm_).size);
    }

    void resize(PrUVec2 extent) override { prVtermResize(vterm_, extent); }

    uint32_t scrollbackCount() const override { return prVtermScrollbackCount(vterm_); }

    size_t linesMemorySize() const override { return prVtermLinesMemorySize(vterm_); }

    void lineAppendText(uint32_t index, uint32_t columns, PrRawString* out) const override {
        prVtermLineAppendText(vterm_, index, 0, columns, out);
    }

    uint64_t scrollbackLineRead(uint32_t index) const override {
        PrVtermLine line = prVtermLine(vterm_, index);
        uint64_t sum = 0;
        for (uint32_t i = 0; i < line.cellCount; ++i)
            sum += line.cells[i].codePoint;
        return sum;
    }

private:
    PrVterm* vterm_ = nullptr;
};

} // namespace

std::unique_ptr<Engine> makePrax() {
    return std::make_unique<Prax>();
}

} // namespace vterm_bench
