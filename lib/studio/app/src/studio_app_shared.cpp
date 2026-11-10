// What Studio's applications and windows share: the device that the windows render on, and the windows' closing.

#include "studio/app/src/studio_app_shared.h"

#include "pr/alloc.h"

namespace pr_impl {

bool studioAppMakeDevice(StudioAppState* app) {
    studioAppCloseDevice(app);
    PrDrawRendererOptions options = prDrawRendererOptionsDefault();
    options.glyphDecoder = app->glyphs;
    if (prGpuDeviceCreate(PrGpuBackend_Default, nullptr, &app->device) != PrStatus_Ok ||
            prDrawRendererCreate(PrDrawRendererBackend_Default, app->device, &options, &app->renderer) != PrStatus_Ok) {
        studioAppCloseDevice(app);
        return false;
    }
    ++app->generation;
    return true;
}

PrDrawDisplay* PR_NULLABLE studioAppDisplay(StudioAppState* app) {
    if (app->display == nullptr && app->device != nullptr)
        prDrawDisplayCreate(app->device, &app->display);
    return app->display;
}

void studioAppCloseDevice(StudioAppState* app) {
    for (uint32_t i = 0; i < app->windows.count; ++i)
        studioWindowCloseDevice(app->windows.data[i]);
    prDrawDisplayDestroy(app->display);
    prDrawRendererDestroy(app->renderer);
    prGpuDeviceDestroy(app->device);
    app->display = nullptr;
    app->renderer = nullptr;
    app->device = nullptr;
}

void studioAppWindowClosed(StudioAppState* app, StudioWindowState* window) {
    for (uint32_t i = 0; i < app->windows.count; ++i) {
        if (app->windows.data[i] == window) {
            app->windows.erase(i);
            break;
        }
    }
    if (window->initial && window->status != PrStatus_Ok)
        app->status = window->status;
    if (app->windows.count == 0 && !app->stopping && app->desc.lastWindow == PrStudioAppLastWindow_Stop)
        prAppStop(app->app);
}

void studioWindowCloseDevice(StudioWindowState* window) {
    PrGpuDevice* device = window->studio->device;
    if (device != nullptr)
        prGpuTextureDestroy(device, window->rendered);
    prDrawFrameDestroy(window->frame);
    prGpuSwapChainDestroy(window->swapChain);
    window->rendered = PrGpuTexture{};
    window->frame = nullptr;
    window->swapChain = nullptr;
}

} // namespace pr_impl
