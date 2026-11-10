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

void studioWindowTargetClose(StudioAppState* app, StudioWindowTarget* target) {
    if (app->device != nullptr)
        prGpuTextureDestroy(app->device, target->rendered);
    prDrawFrameDestroy(target->frame);
    prGpuSwapChainDestroy(target->swapChain);
    target->rendered = PrGpuTexture{};
    target->frame = nullptr;
    target->swapChain = nullptr;
}

void studioWindowCloseDevice(StudioWindowState* window) {
    studioWindowTargetClose(window->studio, &window->target);
    // A surface's window draws again on the next device, whether or not its root's drawing changed.
    for (uint32_t i = 0; i < window->surfaces.count; ++i) {
        studioWindowTargetClose(window->studio, &window->surfaces.data[i]->target);
        window->surfaces.data[i]->drawn = 0;
    }
}

} // namespace pr_impl
