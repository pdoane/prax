// Applications: the backends, and the contract around each backend's functions.

#include "pr/app_application.h"

#include "pr/app_backend.h"
#include "pr/ascii.h"
#include "pr/assert.h"
#include "pr/count_of.h"

#include "app/src/app_names.h"

#if defined(PR_APP_HAS_COCOA)
#include "pr/app_cocoa.h"
#endif
#if defined(PR_APP_HAS_WIN32)
#include "pr/app_win32.h"
#endif
#if defined(PR_APP_HAS_WEB)
#include "pr/app_web.h"
#endif

namespace pr_impl {

namespace {

constexpr PrStringView kBackendNames[PrAppBackend_Count] = {"default", "cocoa", "win32", "web"};

// The backends that the build has, which Default creates the first of, and then Default, so that a build without
// backends creates none.
constexpr PrAppBackend kBuilt[] = {
#if defined(PR_APP_HAS_COCOA)
    PrAppBackend_Cocoa,
#endif
#if defined(PR_APP_HAS_WIN32)
    PrAppBackend_Win32,
#endif
#if defined(PR_APP_HAS_WEB)
    PrAppBackend_Web,
#endif
    PrAppBackend_Default,
};

// The names of the enumerations, in the order of their enumerators.
const char* const kLifecycles[] = {"Active", "Inactive", "Background"};
static_assert(countOf(kLifecycles) == PrAppLifecycle_Background + 1, "a name for each lifecycle state");
} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrAppBackend prAppBackendFromName(PrStringView name) {
    for (PrAppBackend backend = 0; backend < PrAppBackend_Count; ++backend) {
        if (prAsciiEqualsFold(kBackendNames[backend], name))
            return backend;
    }
    return PrAppBackend_Count;
}

PrStringView prAppBackendName(PrAppBackend backend) {
    return backend < PrAppBackend_Count ? kBackendNames[backend] : PrStringView{};
}

bool prAppBackendIsBuilt(PrAppBackend backend) {
    for (PrAppBackend built : kBuilt) {
        if (built != PrAppBackend_Default && (built == backend || backend == PrAppBackend_Default))
            return true;
    }
    return false;
}

PrAppOptions prAppOptionsDefault(void) {
    return PrAppOptions{.presence = PrAppPresence_Regular};
}

PrApp* PR_NULLABLE prAppCreate(PrAppBackend backend, const PrAppOptions* PR_NULLABLE options) {
    PrAppOptions chosen = options != nullptr ? *options : prAppOptionsDefault();
    // A build without a backend has no use for the options.
    (void)chosen;
    PrApp* app = nullptr;
    switch (backend == PrAppBackend_Default ? kBuilt[0] : backend) {
#if defined(PR_APP_HAS_COCOA)
        case PrAppBackend_Cocoa: app = prAppCocoaCreate(&chosen); break;
#endif
#if defined(PR_APP_HAS_WIN32)
        case PrAppBackend_Win32: app = prAppWin32Create(&chosen); break;
#endif
#if defined(PR_APP_HAS_WEB)
        case PrAppBackend_Web: app = prAppWebCreate(&chosen, nullptr); break;
#endif
        default: break;
    }
    return app;
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

void prAppTimerSet(PrApp* app, uint64_t tag, double delay, double interval) {
    PR_ASSERT(delay >= 0 && interval >= 0);
    if (app->timerSetFn != nullptr)
        app->timerSetFn(app, tag, delay, interval);
}

void prAppTimerCancel(PrApp* app, uint64_t tag) {
    if (app->timerCancelFn != nullptr)
        app->timerCancelFn(app, tag);
}
