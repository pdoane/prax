// Captures the references of the rlottie suite, which tools/suite/reftest.py compares the engine's renders with:
// rlottie, at the commit of its corpus, draws each animation on transparent pixels, fitted to the size as xMidYMid
// meet.
//
// A case's input is lines: the animation, a JSON file; the size to draw, `<width>x<height>` pixels; the frame, counted
// from the animation's in point, which rlottie takes whole; and the PNG to write. The answer is the status.

#include "pr/assert.h"
#include "pr/status.h"

#include "tools/harness/harness.h"
#include "tools/harness/harness_png.h"

#include "rlottie.h"

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

using namespace pr;
using namespace harness;

namespace {

void answer(PrStatus status) {
    std::string line = "{\"status\":\"" + std::string(prStatusName(status).data, prStatusName(status).size) + "\"}";
    harnessWriteLine(line);
    std::fflush(stdout);
}

// Draws the animation's frame and writes it to the PNG.
PrStatus capture(const std::string& path, uint32_t width, uint32_t height, size_t frame, const std::string& png) {
    std::unique_ptr<rlottie::Animation> animation = rlottie::Animation::loadFromFile(path, false);
    if (animation == nullptr)
        return PrStatus_InvalidData;
    // rlottie draws premultiplied ARGB words, which are Bgra8 in memory on the platforms that build it.
    std::vector<uint8_t> bgra(size_t(width) * height * 4, 0);
    rlottie::Surface surface(reinterpret_cast<uint32_t*>(bgra.data()), width, height, size_t(width) * 4);
    animation->renderSync(frame, surface);
    std::vector<uint8_t> rgba(bgra.size());
    harnessStraightRgba(PrPixelView{bgra.data(), size_t(width) * 4, {width, height}}, rgba.data());
    if (!harnessWritePng(png.c_str(), PrPixelView{rgba.data(), size_t(width) * 4, {width, height}}))
        return PrStatus_Io;
    return PrStatus_Ok;
}

} // namespace

int main() {
    prCrashHandler();
    HarnessCase test = {};
    while (harnessReadCase(&test)) {
        std::vector<std::string> lines;
        PrStringView input = textView(&test.input);
        size_t start = 0;
        for (size_t i = 0; i <= input.size; ++i) {
            if (i == input.size || input.data[i] == '\n') {
                lines.emplace_back(input.data + start, i - start);
                start = i + 1;
            }
        }
        unsigned width = 0;
        unsigned height = 0;
        if (lines.size() != 4 || std::sscanf(lines[1].c_str(), "%ux%u", &width, &height) != 2 || width == 0
            || height == 0)
            return 1;
        double frame = std::strtod(lines[2].c_str(), nullptr);
        answer(capture(lines[0], width, height, frame > 0 ? size_t(frame) : 0, lines[3]));
    }
    harnessCaseReset(&test);
    return 0;
}
