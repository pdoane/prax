// Captures the Lottie references that a player in the system's browser draws, which tools/suite/reftest.py's Lottie
// suites compare the engine's renders with. The browser, headless, loads a player page of tools/suite/lottie, which
// plays one animation: lottie-web's (player.html) or Skottie's (skottie.html). A frame is captured at each requested
// frame.
//
// A case's input is lines: the player page's URL, with the animation that it plays; the size to capture, in
// `<width>x<height>` CSS pixels; the frame, counted from the animation's in point; and the PNG to write. The page shows
// the frame when told `window.prSetFrame(frame)`. The answer is the status.

#include "pr/app.h"
#include "pr/app_cocoa.h"
#include "pr/assert.h"
#include "pr/vec.h"

#include "tools/harness/harness.h"
#include "tools/harness/harness_png.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace pr;
using namespace harness;

namespace {

struct Case {
    std::string url;
    uint32_t width = 0;
    uint32_t height = 0;
    double frame = 0;
    std::string png;
};

struct Capturing {
    PrApp* app = nullptr;
    PrAppBrowser* browser = nullptr;
    std::vector<Case> cases;
    size_t next = 0;
    std::string loaded;             // the page that the browser has loaded
};

void answer(PrStatus status) {
    std::string line = "{\"status\":\"" + std::string(prStatusName(status).data, prStatusName(status).size) + "\"}";
    harnessWriteLine(line);
    std::fflush(stdout);
}

bool writePng(const std::string& path, PrPixelView pixels) {
    std::vector<uint8_t> rgba(size_t(pixels.extent.x) * pixels.extent.y * 4);
    harnessStraightRgba(pixels, rgba.data());
    return harnessWritePng(path.c_str(), PrPixelView{rgba.data(), size_t(pixels.extent.x) * 4, pixels.extent});
}

void runNext(Capturing& c);

void captured(void* context, PrStatus status, PrPixelView pixels) {
    Capturing& c = *static_cast<Capturing*>(context);
    const Case& now = c.cases[c.next];
    if (status == PrStatus_Ok && !writePng(now.png, pixels))
        status = PrStatus_Io;
    answer(status);
    ++c.next;
    runNext(c);
}

void timed(void* context, PrStatus status, PrStringView message) {
    Capturing& c = *static_cast<Capturing*>(context);
    if (status != PrStatus_Ok) {
        std::fprintf(stderr, "lottie_reference: %s: %.*s\n", c.cases[c.next].url.c_str(), int(message.size),
                     message.data);
        captured(context, status, PrPixelView{});
        return;
    }
    const Case& now = c.cases[c.next];
    prAppBrowserCapture(c.browser, PrRect{0, 0, float(now.width), float(now.height)}, PrUVec2{now.width, now.height},
                        captured, context);
}

void seek(Capturing& c) {
    char script[128];
    std::snprintf(script, sizeof(script), "window.prSetFrame(%.6f)", c.cases[c.next].frame);
    prAppBrowserEvaluate(c.browser, script, timed, &c);
}

void loaded(void* context, PrStatus status) {
    Capturing& c = *static_cast<Capturing*>(context);
    if (status != PrStatus_Ok) {
        c.loaded.clear();
        captured(context, status, PrPixelView{});
        return;
    }
    seek(c);
}

void runNext(Capturing& c) {
    if (c.next >= c.cases.size()) {
        prAppStop(c.app);
        return;
    }
    const Case& now = c.cases[c.next];
    prAppBrowserResize(c.browser, PrUVec2{now.width, now.height});
    if (c.loaded == now.url) {
        seek(c);
        return;
    }
    c.loaded = now.url;
    prAppBrowserLoad(c.browser, now.url, loaded, &c);
}

} // namespace

int main() {
    prCrashHandler();
    Capturing c;
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
        Case one;
        if (lines.size() != 4 || std::sscanf(lines[1].c_str(), "%ux%u", &one.width, &one.height) != 2)
            return 1;
        one.url = lines[0];
        one.frame = std::strtod(lines[2].c_str(), nullptr);
        one.png = lines[3];
        c.cases.push_back(one);
    }
    harnessCaseReset(&test);
    if (c.cases.empty())
        return 0;
    PrAppDesc appDesc{.title = "Lottie references", .extent = {64, 64}, .headless = true};
    c.app = prAppCocoaCreate(&appDesc);
    PrAppHandler handler{
        &c,
        [](void* context, PrApp* app) {
            Capturing& c = *static_cast<Capturing*>(context);
            PrAppBrowserDesc desc{.viewport = PrUVec2{c.cases[0].width, c.cases[0].height}};
            if (prAppBrowserCreate(app, &desc, &c.browser) != PrStatus_Ok) {
                for (; c.next < c.cases.size(); ++c.next)
                    answer(PrStatus_NotSupported);
                prAppStop(app);
                return;
            }
            runNext(c);
        },
        [](void*, PrApp*, const PrAppFrame*) {},
        [](void*, PrApp*, const PrAppEvent*) {},
        [](void* context, PrApp*) {
            Capturing& c = *static_cast<Capturing*>(context);
            prAppBrowserDestroy(c.browser);
            c.browser = nullptr;
        },
    };
    prAppRun(c.app, &handler);
    prAppDestroy(c.app);
    return 0;
}
