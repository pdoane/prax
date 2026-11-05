// The viewer's panes.

#include "tools/viewer/panes.h"

#include "pr/draw_commands.h"
#include "pr/float16.h"
#include "pr/vec.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace viewer {

double millisSince(std::chrono::steady_clock::time_point begin) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
}

void releaseSlot(Slot& slot) {
    prDrawFrameDestroy(slot.drawn);
    slot.drawn = nullptr;
    prDrawListRelease(slot.prepared);
    slot.prepared = nullptr;
    if (slot.device != nullptr)
        prGpuTextureDestroy(slot.device, slot.texture);
    slot.texture = PrGpuTexture{};
    slot.shown.clear();
}

namespace {

// Returns true if the texture exists with the extent and format.
bool fits(PrGpuDevice* device, PrGpuTexture texture, uint32_t width, uint32_t height, PrPixelFormat format) {
    if (texture.id == 0)
        return false;
    PrGpuTextureDesc desc = prGpuTextureDesc(device, texture);
    return desc.extent.width == width && desc.extent.height == height &&
        desc.format == prGpuFormatFromPixelFormat(format);
}

bool sourceEquals(const PaneSource& a, const PaneSource& b) {
    return a.stem == b.stem && a.url == b.url && (a.compare == nullptr) == (b.compare == nullptr);
}

// Waits for commands the device runs, and measures their time in seconds.
struct DeviceWait {
    std::mutex mutex;
    std::condition_variable doneChanged;
    bool done = false;
    double seconds = 0;
};

void deviceDone(void* PR_NULLABLE context, double seconds) {
    DeviceWait* wait = static_cast<DeviceWait*>(context);
    std::lock_guard<std::mutex> lock(wait->mutex);
    wait->done = true;
    wait->seconds = seconds;
    wait->doneChanged.notify_one();
}

// Passes a render's texels to be compared, as BGRA. HDR texels are clamped, so that renders compare as they would in 8
// bits.
void compareTexels(const std::function<void(const std::string&, PrPixelView)>& compare, const std::string& shown,
        PrPixelView texels, PrPixelFormat format) {
    if (format != PrPixelFormat_RgbaF16) {
        compare(shown, texels);
        return;
    }
    std::vector<uint8_t> bgra = narrowed(texels);
    compare(shown, PrPixelView{bgra.data(), size_t(texels.extent.x) * 4, texels.extent});
}

// The pixels that a device render reads back to be compared.
struct CompareRead {
    std::function<void(const std::string&, PrPixelView)> compare;
    std::string shown;
    PrPixelFormat format;
};

void compareRead(void* PR_NULLABLE context, PrPixelView texels) {
    CompareRead* read = static_cast<CompareRead*>(context);
    compareTexels(read->compare, read->shown, texels, read->format);
    delete read;
}

} // namespace

PrStatus sizeTexture(PrGpuDevice* device, PrGpuTexture& texture, uint32_t width, uint32_t height, PrPixelFormat format) {
    if (fits(device, texture, width, height, format))
        return PrStatus_Ok;
    prGpuTextureDestroy(device, texture);
    texture = PrGpuTexture{};
    PrGpuTextureDesc desc = prGpuTextureDesc2d(prGpuFormatFromPixelFormat(format), PrUVec2{width, height},
        PrGpuTextureUsageFlag_Sampled | PrGpuTextureUsageFlag_RenderTarget);
    return prGpuTextureCreate(device, &desc, &texture);
}

PrStatus sizeSlot(PrGpuDevice* device, Slot& slot, uint32_t width, uint32_t height, PrPixelFormat format) {
    if (slot.device == device && fits(device, slot.texture, width, height, format))
        return PrStatus_Ok;
    uint64_t frame = slot.frame;
    releaseSlot(slot);
    slot.frame = frame;
    slot.device = device;
    return sizeTexture(device, slot.texture, width, height, format);
}

std::vector<uint8_t> pixelsOf(PrColor color, size_t count, PrPixelFormat format) {
    float a = std::clamp(color.a, 0.0f, 1.0f);
    if (format == PrPixelFormat_RgbaF16) {
        PrFloat16 pixel[4] = {
            prFloat16FromFloat(color.r * a), prFloat16FromFloat(color.g * a),
            prFloat16FromFloat(color.b * a), prFloat16FromFloat(a),
        };
        std::vector<uint8_t> pixels(count * 8);
        for (size_t i = 0; i < count; ++i)
            std::memcpy(pixels.data() + i * 8, pixel, 8);
        return pixels;
    }
    auto byte = [](float v) { return uint8_t(std::lround(std::clamp(v, 0.0f, 1.0f) * 255)); };
    uint8_t pixel[4] = {byte(color.b * a), byte(color.g * a), byte(color.r * a), byte(a)};
    std::vector<uint8_t> pixels(count * 4);
    for (size_t i = 0; i < count; ++i)
        std::memcpy(pixels.data() + i * 4, pixel, 4);
    return pixels;
}

std::vector<uint8_t> narrowed(PrPixelView texels) {
    std::vector<uint8_t> bgra(size_t(texels.extent.x) * texels.extent.y * 4);
    for (uint32_t y = 0; y < texels.extent.y; ++y) {
        for (uint32_t x = 0; x < texels.extent.x; ++x) {
            PrFloat16 wide[4];
            std::memcpy(wide, texels.data + y * texels.stride + x * 8, sizeof(wide));
            float a = std::clamp(prFloat16ToFloat(wide[3]), 0.0f, 1.0f);
            uint8_t* out = bgra.data() + (size_t(y) * texels.extent.x + x) * 4;
            for (uint32_t k = 0; k < 3; ++k)
                out[2 - k] = uint8_t(std::lround(std::clamp(prFloat16ToFloat(wide[k]), 0.0f, a) * 255));
            out[3] = uint8_t(std::lround(a * 255));
        }
    }
    return bgra;
}

Rendered renderSlot(Slot& slot, const std::vector<RendererConfig>& configs, uint32_t config, const PrDrawList* list,
        PrColor background, PrGpuCommands* commands, PaneOutput output) {
    Rendered r{};
    r.commands = prDrawListCommandCount(list);
    r.width = prGpuTextureExtent(slot.device, slot.texture).x;
    r.height = prGpuTextureExtent(slot.device, slot.texture).y;
    std::chrono::steady_clock::time_point begin = std::chrono::steady_clock::now();
    if (prDrawRendererDevice(configs[config].renderer) != slot.device) {
        std::vector<uint8_t> pixels = pixelsOf(background, size_t(r.width) * r.height, output.format);
        PrPixels into{pixels.data(), size_t(r.width) * prPixelFormatSize(output.format), {r.width, r.height}};
        r.status = prDrawRendererRenderPixels(configs[config].renderer, list, into, output.format);
        prGpuTextureWrite(slot.device, slot.texture, PrIVec2{0, 0}, prPixelsView(into));
        r.ms = millisSince(begin);
        r.rendererMs = r.ms;
        return r;
    }
    // Backdrops read the background beneath the list, in the same way that they read the pixels that the host fills in
    // memory.
    PrDrawBeneath beneath{};
    beneath.color = background;
    PrDrawTarget prepared{
        .extent = {r.width, r.height},
        .format = output.format,
        .commands = commands,
        .beneath = &beneath,
    };
    r.status = prDrawFramePrepare(frameFor(slot.drawn, configs[config].renderer), list, &prepared);
    // The pass clears to the background and draws the render over it, or clears to white where the render failed, since
    // what the texture held is not a render.
    float a = std::clamp(background.a, 0.0f, 1.0f);
    PrColor clear;
    if (r.status == PrStatus_Ok)
        clear = PrColor{background.r * a, background.g * a, background.b * a, a};
    else
        clear = PrColor{1, 1, 1, 1};
    PrGpuSubresources all{};
    prGpuCommandsTextureBarrier(commands, slot.texture, all, PrGpuAccessFlag_ShaderRead,
        PrGpuAccessFlag_ColorAttachment);
    PrGpuPassDesc desc = prGpuPassDescColor(slot.texture, PrGpuLoad_Clear, clear);
    PrGpuPass* pass = prGpuPassBegin(commands, &desc);
    if (r.status == PrStatus_Ok)
        prDrawFrameRender(slot.drawn, pass, PrIVec2{0, 0});
    prGpuPassEnd(pass);
    prGpuCommandsTextureBarrier(commands, slot.texture, all, PrGpuAccessFlag_ColorAttachment,
        PrGpuAccessFlag_ShaderRead);
    r.ms = millisSince(begin);
    r.rendererMs = r.ms;
    r.onGpu = true;
    return r;
}

void SharedClock::set(double seconds, bool running) {
    std::lock_guard<std::mutex> lock(mutex_);
    base_ = seconds;
    at_ = std::chrono::steady_clock::now();
    running_ = running;
}

double SharedClock::now() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_)
        return base_;
    return base_ + std::chrono::duration<double>(std::chrono::steady_clock::now() - at_).count();
}

bool SharedClock::running() {
    std::lock_guard<std::mutex> lock(mutex_);
    return running_;
}

double FpsMeter::done() {
    std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
    times_.push_back(now);
    size_t old = 0;
    while (old < times_.size() && now - times_[old] > std::chrono::seconds(1))
        ++old;
    times_.erase(times_.begin(), times_.begin() + std::ptrdiff_t(old));
    if (times_.size() < 2)
        return 0;
    return double(times_.size() - 1) / std::chrono::duration<double>(times_.back() - times_.front()).count();
}

std::string shownAt(const std::string& stem, double time) {
    char at[32];
    std::snprintf(at, sizeof(at), " %.6f", time);
    return stem + at;
}

Pane::Pane(const PaneContext& context, bool threaded) : context_(context) {
    if (threaded)
        thread_ = std::thread([this] { run(); });
}

Pane::~Pane() {
    stop();
    for (Slot& slot : slots_)
        releaseSlot(slot);
}

void Pane::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    changed_.notify_one();
    {
        std::lock_guard<std::mutex> lock(*context_.framesMutex);
        context_.framesDoneChanged->notify_all();
    }
    if (thread_.joinable())
        thread_.join();
}

void Pane::show(PaneSource source) {
    bool changed = !sourceEquals(source, source_);
    source_ = source;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (changed)
            ++version_;
        wanted_ = std::move(source);
    }
    changed_.notify_one();
}

void Pane::touch() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++version_;
    }
    changed_.notify_one();
}

const Slot& Pane::take(uint64_t frame, PrPixelFormat format) {
    Slot* taken = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const Slot& latest = slots_[latest_];
        if (!wanted_.url.empty() || latest.texture.id == 0 ||
                prGpuTextureDesc(latest.device, latest.texture).format == prGpuFormatFromPixelFormat(format))
            shown_ = latest_;
        slots_[shown_].frame = frame;
        taken = &slots_[shown_];
    }
    changed_.notify_one();
    return *taken;
}

bool Pane::settled() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (shown_ != latest_)
        return false;
    if (wanted_.stem.empty())
        return !rendering_;
    // A pane that renders continuously still holds what it shows while it renders the same source again.
    double time = context_.clock->now();
    return latestVersion_ == version_ && slots_[latest_].shown == shownAt(wanted_.stem, time);
}

bool Pane::current() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (wanted_.stem.empty() || !wanted_.url.empty())
        return true;
    return shown_ == latest_ && latestVersion_ == version_ && !slots_[latest_].shown.empty();
}

bool Pane::holds(const std::string& shown) {
    std::lock_guard<std::mutex> lock(mutex_);
    return latestVersion_ == version_ && slots_[latest_].shown == shown;
}

bool Pane::rendersIn(PrPixelFormat format) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (wanted_.stem.empty() || !wanted_.url.empty())
        return true;
    const Slot& latest = slots_[latest_];
    return latestVersion_ == version_ && (latest.texture.id == 0 ||
        prGpuTextureDesc(latest.device, latest.texture).format == prGpuFormatFromPixelFormat(format));
}

uint64_t Pane::version() {
    std::lock_guard<std::mutex> lock(mutex_);
    return version_;
}

uint32_t Pane::begin() {
    uint32_t index = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        while (index == shown_ || index == latest_)
            ++index;
        rendering_ = true;
    }
    // A slot that a frame drew is written once the device has finished that frame.
    uint64_t frame = slots_[index].frame;
    std::unique_lock<std::mutex> lock(*context_.framesMutex);
    context_.framesDoneChanged->wait(lock, [&] {
        std::lock_guard<std::mutex> own(mutex_);
        return context_.framesDone->load() >= frame || stopping_;
    });
    return index;
}

void Pane::publish(uint32_t index, uint64_t version) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        latest_ = index;
        latestVersion_ = version;
        rendering_ = false;
    }
    prAppWake(context_.app);
}

void Pane::run() {
    for (;;) {
        PaneSource source;
        double time = 0;
        uint64_t version = 0;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            for (;;) {
                if (stopping_)
                    return;
                // The browser fills a pane that shows a page. The thread renders the rest: a changed source at once,
                // and otherwise once the viewer has taken the latest render, or at once if it renders back to back.
                if (!wanted_.stem.empty() && wanted_.url.empty() &&
                        (latestVersion_ != version_ || shown_ == latest_ || wanted_.backToBack)) {
                    time = context_.clock->now();
                    break;
                }
                changed_.wait(lock);
            }
            source = wanted_;
            version = version_;
        }
        uint32_t index = begin();
        render(source, time, slots_[index]);
        publish(index, version);
    }
}

// Renders the source at `time` into the slot, measuring the whole render on this thread. That is recording, rendering
// in memory and writing the texture, or rendering on the device and waiting for it.
void Pane::render(const PaneSource& source, double time, Slot& slot) {
    const PaneContext& c = context_;
    std::chrono::steady_clock::time_point begin = std::chrono::steady_clock::now();
    Rendered r{};
    r.time = time;
    r.backToBack = source.backToBack;
    r.frame = source.loaded != nullptr ? update(*source.loaded, time) : -1;
    r.width = source.width;
    r.height = source.height;
    std::string shown = shownAt(source.stem, time);
    const RendererConfig& config = (*c.configs)[source.config];
    std::vector<uint8_t> bgra;
    PrDrawList* list = nullptr;
    const PaneOutput& output = source.output;
    const uint32_t bytes = prPixelFormatSize(output.format);
    // A view's region draws through a list. A whole document may come as its source's own pixels, which are BGRA.
    bool inMemory = source.loaded != nullptr && !source.viewed && pixels(*source.loaded, source.width, source.height, bgra);
    if (inMemory && output.format == PrPixelFormat_RgbaF16) {
        std::vector<uint8_t> wide(size_t(source.width) * source.height * 8);
        for (size_t i = 0; i < size_t(source.width) * source.height; ++i) {
            const uint8_t* p = bgra.data() + i * 4;
            PrFloat16 texel[4] = {
                prFloat16FromFloat(float(p[2]) / 255), prFloat16FromFloat(float(p[1]) / 255),
                prFloat16FromFloat(float(p[0]) / 255), prFloat16FromFloat(float(p[3]) / 255),
            };
            std::memcpy(wide.data() + i * 8, texel, 8);
        }
        bgra = std::move(wide);
    }
    if (!inMemory && source.loaded != nullptr) {
        std::chrono::steady_clock::time_point recording = std::chrono::steady_clock::now();
        list = prDrawListCreate();
        prDrawListPushTransformAffine2(
            list, PrAffine2{source.pixelRatio, 0, 0, source.pixelRatio, source.origin.x, source.origin.y});
        // The page renders at the pane's own pixel ratio.
        PrAppDisplay display = output.display;
        display.pixelRatio = source.pixelRatio;
        draw(*source.loaded, list, source.docWidth, source.docHeight, display, output.preferences);
        prDrawListPopTransform(list);
        r.recordMs = millisSince(recording);
        r.commands = prDrawListCommandCount(list);
        if (prDrawRendererDevice(config.renderer) != c.device) {
            bgra = pixelsOf(source.background, size_t(source.width) * source.height, output.format);
            PrPixels into{bgra.data(), size_t(source.width) * bytes, {source.width, source.height}};
            std::lock_guard<std::mutex> lock(*(*c.configLocks)[source.config]);
            std::chrono::steady_clock::time_point rendering = std::chrono::steady_clock::now();
            r.status = prDrawRendererRenderPixels(config.renderer, list, into, output.format);
            r.rendererMs = millisSince(rendering);
            prDrawListRelease(list);
            list = nullptr;
            inMemory = true;
        }
    } else if (!inMemory) {
        r.status = PrStatus_NotSupported;
    }
    if (inMemory) {
        if (sizeSlot(c.device, slot, source.width, source.height, output.format) == PrStatus_Ok) {
            const PrPixelView texels{bgra.data(), size_t(source.width) * bytes, {source.width, source.height}};
            prGpuTextureWrite(slot.device, slot.texture, PrIVec2{0, 0}, texels);
            if (source.compare != nullptr && r.status == PrStatus_Ok)
                compareTexels(source.compare, shown, texels, output.format);
        }
    } else if (list != nullptr) {
        DeviceWait wait;
        {
            std::lock_guard<std::mutex> own(*(*c.configLocks)[source.config]);
            PrGpuCommands* commands = nullptr;
            if (sizeSlot(c.device, slot, source.width, source.height, output.format) == PrStatus_Ok &&
                    prGpuCommandsBegin(c.device, nullptr, &commands)) {
                Rendered onDevice = renderSlot(slot, *c.configs, source.config, list, source.background, commands, output);
                r.status = onDevice.status;
                r.rendererMs = onDevice.rendererMs;
                r.onGpu = onDevice.onGpu;
                if (source.compare != nullptr && r.status == PrStatus_Ok)
                    prGpuCommandsReadTexture(commands, slot.texture, PrGpuAccessFlag_ShaderRead, compareRead,
                        new CompareRead{source.compare, shown, output.format});
                prGpuCommandsOnDone(commands, deviceDone, &wait);
                prGpuCommandsSubmit(commands);
            } else {
                wait.done = true;
            }
        }
        std::unique_lock<std::mutex> lock(wait.mutex);
        wait.doneChanged.wait(lock, [&] { return wait.done; });
        r.gpuMs = wait.seconds * 1000;
        prDrawListRelease(slot.prepared);
        slot.prepared = list;
    }
    r.ms = millisSince(begin);
    r.fps = fps_.done();
    slot.rendered = r;
    slot.shown = shown;
    slot.pixelRatio = source.pixelRatio;
    slot.viewed = source.viewed;
    slot.at = source.at;
}

} // namespace viewer
