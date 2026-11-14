// Studio's applications: the run of the platform's application, and what the windows share.

#include "pr/studio_app.h"

#include "pr/alloc.h"

#include "studio/app/src/studio_app_shared.h"

namespace pr_impl {

namespace {

// Makes the device and opens the description's window, and then calls the application's start. A run without a device
// fails.
bool start(void* context, PrApp*) {
    PrStudioApp* s = static_cast<PrStudioApp*>(context);
    if (!studioAppMakeDevice(s)) {
        s->status = PrStatus_NotSupported;
        return false;
    }
    if (s->desc.window != nullptr) {
        PrStudioWindow* window = nullptr;
        PrStatus status = prStudioWindowCreate(s, s->desc.window, &window);
        if (status != PrStatus_Ok) {
            s->status = status;
            return false;
        }
        window->initial = true;
    }
    if (s->desc.start != nullptr && !s->desc.start(s->desc.context, s)) {
        s->status = PrStatus_InvalidState;
        return false;
    }
    return true;
}

// A wake asks every window for a frame, which takes up what woke the application.
void event(void* context, PrApp*, const PrAppEvent* e) {
    PrStudioApp* s = static_cast<PrStudioApp*>(context);
    if (e->kind == PrAppEventKind_Wake) {
        for (uint32_t i = 0; i < s->windows.count; ++i) {
            if (!s->windows.data[i]->closing)
                prAppWindowRequestFrame(s->windows.data[i]->appWindow);
        }
    }
    if (s->desc.event != nullptr)
        s->desc.event(s->desc.context, s, e);
}

// Calls the application's shutdown, and then closes the windows that are still open, newest first, and the device.
void shutdown(void* context, PrApp*) {
    PrStudioApp* s = static_cast<PrStudioApp*>(context);
    s->stopping = true;
    if (s->desc.shutdown != nullptr)
        s->desc.shutdown(s->desc.context, s);
    while (s->windows.count != 0)
        prStudioWindowDestroy(s->windows.data[s->windows.count - 1]);
    studioAppCloseDevice(s);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrStatus prStudioAppRun(const PrStudioAppDesc* desc) {
    PrApp* platform = prAppCreate(PrAppBackend_Default, desc->options);
    if (platform == nullptr)
        return PrStatus_NotSupported;
    PrStudioApp* s = allocZeroed<PrStudioApp>();
    s->desc = *desc;
    s->app = platform;
    s->status = PrStatus_Ok;
    s->shaper = prTextShaperCreate(PrTextShaperBackend_Default);
    s->compress = prCompressCodecCreate(PrCompressCodecBackend_Default);
    s->images = s->compress != nullptr ? prImageCodecCreate(PrImageCodecBackend_Default, s->compress) : nullptr;
    s->glyphs = s->images != nullptr ? prDrawGlyphBitmapCreate(s->images) : nullptr;
    prAppRun(platform, {.context = s, .start = start, .event = event, .shutdown = shutdown});
    PrStatus status = s->status;
    prAppDestroy(platform);
    s->windows.reset();
    prDrawGlyphDecoderDestroy(s->glyphs);
    prImageCodecDestroy(s->images);
    prCompressCodecDestroy(s->compress);
    prTextShaperDestroy(s->shaper);
    prMemoryFree(s);
    return status;
}

void prStudioAppStop(PrStudioApp* app) {
    prAppStop(app->app);
}

PrApp* prStudioAppApp(const PrStudioApp* app) {
    return app->app;
}

PrFontSet* prStudioAppFonts(const PrStudioApp* app) {
    return app->desc.fonts;
}

PrTextShaper* prStudioAppShaper(const PrStudioApp* app) {
    return app->shaper;
}

PrGpuDevice* PR_NULLABLE prStudioAppDevice(const PrStudioApp* app) {
    return app->device;
}

PrDrawRenderer* PR_NULLABLE prStudioAppRenderer(const PrStudioApp* app) {
    return app->renderer;
}
