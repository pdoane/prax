// The shell: the contract around each backend's functions.

#include "pr/app_shell.h"

#include "pr/app_backend.h"
#include "pr/assert.h"
#include "pr/utf8.h"

namespace pr_impl {

namespace {

bool isUtf8(PrStringView text) {
    return prUtf8Validate(text) == text.size;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrStatus prAppNotificationPost(PrApp* app, const PrAppNotificationDesc* desc) {
    PR_ASSERT(isUtf8(desc->title) && isUtf8(desc->body));
    if (app->notificationPostFn == nullptr)
        return PrStatus_NotSupported;
    return app->notificationPostFn(app, desc);
}

PrStatus prAppOpenUrl(PrApp* app, PrStringView url) {
    return app->openUrlFn(app, url);
}

PrStatus prAppRevealPath(PrApp* app, PrStringView path) {
    PR_ASSERT(isUtf8(path));
    return app->revealPathFn != nullptr ? app->revealPathFn(app, path) : PrStatus_NotSupported;
}

PrStatus prAppTrashPath(PrApp* app, PrStringView path) {
    PR_ASSERT(isUtf8(path));
    return app->trashPathFn != nullptr ? app->trashPathFn(app, path) : PrStatus_NotSupported;
}

void prAppWindowShare(PrAppWindow* window, const PrAppDataContent* content, PrRect anchor, PrAppShareFn done,
        void* PR_NULLABLE context) {
    window->shareFn(window, content, anchor, done, context);
}

void prAppRecentDocumentAdd(PrApp* app, PrStringView path) {
    PR_ASSERT(isUtf8(path));
    if (app->recentDocumentAddFn != nullptr)
        app->recentDocumentAddFn(app, path);
}

void prAppRecentDocumentsClear(PrApp* app) {
    if (app->recentDocumentsClearFn != nullptr)
        app->recentDocumentsClearFn(app);
}

void prAppSetBadge(PrApp* app, PrStringView badge) {
    PR_ASSERT(isUtf8(badge));
    if (app->setBadgeFn != nullptr)
        app->setBadgeFn(app, badge);
}
