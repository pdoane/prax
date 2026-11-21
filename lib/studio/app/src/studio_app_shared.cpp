// What Studio's applications and windows share: the device that the windows render on, and the windows' closing.

#include "studio/app/src/studio_app_shared.h"

#include "pr/alloc.h"
#include "pr/assert.h"
#include "pr/draw_select.h"
#include "pr/gpu_select.h"
#include "pr/svg_glyph.h"
#include "pr/texture_cache.h"

namespace pr_impl {

namespace {

// Frees what the windows made on the application's device, and the renderer and the display shader.
void closeRenderer(StudioAppState* app) {
    for (uint32_t i = 0; i < app->windows.count; ++i)
        studioWindowCloseDevice(app->windows.data[i]);
    prDrawDisplayDestroy(app->display);
    prDrawRendererDestroy(app->renderer);
    app->display = nullptr;
    app->renderer = nullptr;
}

} // namespace

void studioAppOpen(StudioAppState* app) {
    PrTextureCacheDesc desc = prTextureCacheDescDefault(nullptr);
    desc.tasks = app->desc.pool;
    // An image that arrives after a frame drew without it wakes the application, which asks every window for a frame.
    desc.arrived = [](void* PR_NULLABLE context) { prAppWake(static_cast<StudioAppState*>(context)->app); };
    desc.arrivedContext = app;
    app->textures = prTextureCacheCreate(&desc);
    app->glyphs = app->images != nullptr ? prSvgGlyphDecoderCreate(app->images, app->compress, app->textures) : nullptr;
}

bool studioAppMakeDevice(StudioAppState* app) {
    if (app->device != nullptr && prGpuDeviceIsLost(app->device)) {
        app->deviceStatus = prGpuDeviceLostStatus(app->device);
        studioAppCloseDevice(app);
    }
    if (app->device == nullptr) {
        PrGpuDeviceOptions options = prGpuDeviceOptionsDefault();
        options.pool = app->desc.pool;
        // A window that has no device draws nothing and asks for no frame. The end of the creation wakes the
        // application, which asks every window for a frame.
        options.ready = [](void* PR_NULLABLE context, PrGpuDevice*) {
            prAppWake(static_cast<StudioAppState*>(context)->app);
        };
        options.readyContext = app;
        app->device = prGpuDeviceCreate(PrGpuBackend_Default, &options);
        if (app->device == nullptr) {
            app->deviceStatus = PrStatus_NotSupported;
            return false;
        }
    }
    if (!prGpuDeviceIsReady(app->device))
        return false;
    if (prGpuDeviceIsLost(app->device)) {
        app->deviceStatus = prGpuDeviceLostStatus(app->device);
        return false;
    }
    if (prTextureCacheDevice(app->textures) == nullptr)
        prTextureCacheSetDevice(app->textures, app->device);
    if (app->renderer == nullptr) {
        PrDrawRendererOptions options = prDrawRendererOptionsDefault();
        options.glyphDecoder = app->glyphs;
        PrStatus status = prDrawRendererCreate(PrDrawRendererBackend_Default, app->device, &options, &app->renderer);
        if (status != PrStatus_Ok) {
            app->deviceStatus = status;
            return false;
        }
        ++app->generation;
    }
    app->deviceStatus = PrStatus_Ok;
    return true;
}

bool studioAppCreatingDevice(const StudioAppState* app) {
    return app->device != nullptr && !prGpuDeviceIsReady(app->device);
}

PrDrawDisplay* PR_NULLABLE studioAppDisplay(StudioAppState* app) {
    if (app->display == nullptr && app->device != nullptr)
        prDrawDisplayCreate(app->device, &app->display);
    return app->display;
}

void studioAppCloseDevice(StudioAppState* app) {
    closeRenderer(app);
    // The cache frees its textures while the device still exists. The cache then has no device until the next device is
    // ready.
    prTextureCacheSetDevice(app->textures, nullptr);
    prGpuDeviceDestroy(app->device);
    app->device = nullptr;
}

void studioAppClose(StudioAppState* app) {
    PR_ASSERT(app->windows.count == 0);
    closeRenderer(app);
    // The renderer used the glyph decoder, and the decoder and the cache use the device. They are therefore destroyed
    // in the order renderer, decoder, cache, device.
    prDrawGlyphDecoderDestroy(app->glyphs);
    prTextureCacheDestroy(app->textures);
    prGpuDeviceDestroy(app->device);
    app->glyphs = nullptr;
    app->textures = nullptr;
    app->device = nullptr;
}

void studioAppBeginWindowFrame(StudioAppState* app, StudioWindowState* window, double time) {
    // The images that change over time, such as animated ones, play on the clock of the frames.
    prTextureCacheSetTime(app->textures, time);
    if (app->textures != nullptr && window->framed) {
        uint64_t cancelled = prTextureCacheInfo(app->textures).cancelled;
        prTextureCacheTrim(app->textures);
        bool anyCancelled = prTextureCacheInfo(app->textures).cancelled != cancelled;
        for (uint32_t i = 0; i < app->windows.count; ++i) {
            StudioWindowState* other = app->windows.data[i];
            if (anyCancelled && !other->framed && !other->closing)
                prAppWindowRequestFrame(other->appWindow);
            other->framed = false;
        }
    }
    window->framed = true;
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
        prGpuTextureRelease(app->device, target->rendered);
    prDrawFrameDestroy(target->frame);
    prGpuSwapChainDestroy(target->swapChain);
    target->rendered = PrGpuTexture{};
    target->frame = nullptr;
    target->swapChain = nullptr;
}

void studioWindowCloseDevice(StudioWindowState* window) {
    studioWindowTargetClose(window->studio, &window->target);
    // A portal's window draws again on the next device, whether or not its root's drawing changed.
    for (uint32_t i = 0; i < window->portals.count; ++i) {
        studioWindowTargetClose(window->studio, &window->portals.data[i]->target);
        window->portals.data[i]->drawn = 0;
    }
}

} // namespace pr_impl
