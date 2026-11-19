// Applications: the contract around each backend's functions.

#include "pr/app_application.h"

#include "pr/app_backend.h"
#include "pr/assert.h"
#include "pr/count_of.h"

#include "app/src/app_names.h"

namespace pr_impl {

namespace {

// The names of the enumerations, in the order of their enumerators.
const char* const kLifecycles[] = {"Active", "Inactive", "Background"};
static_assert(countOf(kLifecycles) == PrAppLifecycle_Background + 1, "a name for each lifecycle state");
const char* const kWindowModels[] = {"Single", "Multiple", "Desktop"};
static_assert(countOf(kWindowModels) == PrAppWindowModel_Desktop + 1, "a name for each window model");
} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrStringView prAppWindowModelName(PrAppWindowModel model) {
    return appNameOf(kWindowModels, model);
}

PrAppOptions prAppOptionsDefault(void) {
    return PrAppOptions{.presence = PrAppPresence_Regular};
}

void prAppDestroy(PrApp* PR_NULLABLE app) {
    if (app != nullptr)
        app->destroyFn(app);
}

void prAppRun(PrApp* app, PrAppHandler handler) {
    PR_ASSERT(handler.start != nullptr && handler.event != nullptr && handler.shutdown != nullptr);
    app->runFn(app, handler);
}

void prAppStop(PrApp* app) {
    app->stopFn(app);
}

void prAppSetPresence(PrApp* app, PrAppPresence presence) {
    PR_ASSERT(presence <= PrAppPresence_Accessory);
    if (app->setPresenceFn != nullptr)
        app->setPresenceFn(app, presence);
}

PrAppWindowModel prAppWindowModel(const PrApp* app) {
    return app->windowModelFn != nullptr ? app->windowModelFn(app) : PrAppWindowModel(PrAppWindowModel_Single);
}

void prAppSetHidden(PrApp* app, bool hidden) {
    if (app->setHiddenFn != nullptr)
        app->setHiddenFn(app, hidden);
}

bool prAppIsHidden(const PrApp* app) {
    return app->isHiddenFn != nullptr && app->isHiddenFn(app);
}

void prAppWake(PrApp* app) {
    app->wakeFn(app);
}

void prAppCancel(PrApp* app, void* PR_NULLABLE context) {
    app->cancelFn(app, context);
}

PrStringView prAppLifecycleName(PrAppLifecycle lifecycle) {
    return appNameOf(kLifecycles, lifecycle);
}

PrAppLifecycle prAppLifecycle(const PrApp* app) {
    return app->lifecycleFn != nullptr ? app->lifecycleFn(app) : PrAppLifecycle_Active;
}

void prAppQuitCancel(PrApp* app) {
    if (app->quitCancelFn != nullptr)
        app->quitCancelFn(app);
}

PrStatus prAppImmersiveOpen(PrApp* app) {
    return app->immersiveOpenFn != nullptr ? app->immersiveOpenFn(app) : PrStatus_NotSupported;
}

void prAppImmersiveClose(PrApp* app) {
    if (app->immersiveCloseFn != nullptr)
        app->immersiveCloseFn(app);
}

void prAppTimerSet(PrApp* app, uint64_t tag, double delay, double interval) {
    PR_ASSERT(delay >= 0 && interval >= 0);
    if (app->timerSetFn != nullptr)
        app->timerSetFn(app, tag, delay, interval);
}

void prAppTimerCancel(PrApp* app, uint64_t tag) {
    if (app->timerCancelFn != nullptr)
        app->timerCancelFn(app, tag);
}
