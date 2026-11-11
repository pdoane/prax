// Windows: the contract around each backend's functions, and outputs.

#include "pr/app_window.h"

#include "pr/app_backend.h"
#include "pr/assert.h"
#include "pr/count_of.h"
#include "pr/utf8.h"

#include "app/src/app_names.h"
#include "app/src/app_popup.h"

namespace pr_impl {

namespace {

bool limitsHold(PrUVec2 minExtent, PrUVec2 maxExtent) {
    return (maxExtent.x == 0 || minExtent.x <= maxExtent.x) && (maxExtent.y == 0 || minExtent.y <= maxExtent.y);
}

// The names of the enumerations, in the order of their enumerators.
const char* const kWindowStates[] = {"Normal", "Minimized", "Maximized", "Fullscreen"};
static_assert(countOf(kWindowStates) == PrAppWindowState_Fullscreen + 1, "a name for each window state");
} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrRect prAppPopupRect(const PrAppPopupPlacement* placement, PrVec2 extent, PrRect work, PrAppLayoutDirection direction) {
    return appPopupPlace(*placement, placement->anchor, extent, work, direction == PrAppLayoutDirection_RightToLeft);
}

PrStringView prAppWindowStateName(PrAppWindowState state) {
    return appNameOf(kWindowStates, state);
}

PrStatus prAppWindowCreate(PrApp* app, const PrAppWindowDesc* desc, PrAppWindow* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    PR_ASSERT(desc->handler.open != nullptr && desc->handler.frame != nullptr && desc->handler.event != nullptr);
    PR_ASSERT(limitsHold(desc->minExtent, desc->maxExtent));
    PR_ASSERT(desc->role <= PrAppWindowRole_Overlay && desc->decoration <= PrAppWindowDecoration_None);
    PR_ASSERT(desc->backdrop <= PrAppWindowBackdrop_Blur && desc->state <= PrAppWindowState_Fullscreen);
    // A dialog and a popup belong to a parent, and only a dialog is modal.
    PR_ASSERT((desc->parent != nullptr) ==
        (desc->role == PrAppWindowRole_Dialog || desc->role == PrAppWindowRole_Popup));
    PR_ASSERT(!desc->modal || desc->role == PrAppWindowRole_Dialog);
    PR_ASSERT(prUtf8Validate(desc->title) == desc->title.size);
    // Only a Desktop application has windows for its popups, dialogs and overlays.
    if (desc->role != PrAppWindowRole_Normal && prAppWindowModel(app) != PrAppWindowModel_Desktop)
        return PrStatus_NotSupported;
    return app->windowCreateFn(app, desc, out);
}

void prAppWindowDestroy(PrAppWindow* PR_NULLABLE window) {
    if (window != nullptr)
        window->destroyFn(window);
}

PrApp* prAppWindowApp(const PrAppWindow* window) {
    return window->appFn(window);
}

void prAppWindowRequestFrame(PrAppWindow* window) {
    window->requestFrameFn(window);
}

void prAppWindowSetTitle(PrAppWindow* window, PrStringView title) {
    PR_ASSERT(prUtf8Validate(title) == title.size);
    window->setTitleFn(window, title);
}

PrStringView prAppWindowTitle(const PrAppWindow* window) {
    return window->titleFn(window);
}

void prAppWindowSetExtent(PrAppWindow* window, PrUVec2 extent) {
    if (window->setExtentFn != nullptr)
        window->setExtentFn(window, extent);
}

void prAppWindowSetExtentLimits(PrAppWindow* window, PrUVec2 minExtent, PrUVec2 maxExtent) {
    PR_ASSERT(limitsHold(minExtent, maxExtent));
    if (window->setExtentLimitsFn != nullptr)
        window->setExtentLimitsFn(window, minExtent, maxExtent);
}

void prAppWindowSetAspectRatio(PrAppWindow* window, float ratio) {
    PR_ASSERT(ratio >= 0);
    if (window->setAspectRatioFn != nullptr)
        window->setAspectRatioFn(window, ratio);
}

bool prAppWindowPosition(const PrAppWindow* window, PrAppDisplayPosition* out) {
    *out = PrAppDisplayPosition{};
    return window->positionFn != nullptr && window->positionFn(window, out);
}

void prAppWindowSetPosition(PrAppWindow* window, const PrAppDisplayPosition* position) {
    if (window->setPositionFn != nullptr)
        window->setPositionFn(window, position);
}

bool prAppWindowPointToDisplay(const PrAppWindow* window, PrVec2 point, PrAppDisplayPosition* out) {
    *out = PrAppDisplayPosition{};
    return window->pointToDisplayFn != nullptr && window->pointToDisplayFn(window, point, out);
}

bool prAppWindowPointFromDisplay(const PrAppWindow* window, const PrAppDisplayPosition* position, PrVec2* out) {
    *out = PrVec2{};
    return window->pointFromDisplayFn != nullptr && window->pointFromDisplayFn(window, position, out);
}

void prAppWindowSetPopupPlacement(PrAppWindow* window, const PrAppPopupPlacement* placement) {
    PR_ASSERT(placement->side <= PrAppPopupSide_Before && placement->align <= PrAppPopupAlign_End);
    if (window->setPopupPlacementFn != nullptr)
        window->setPopupPlacementFn(window, placement);
}

void prAppWindowSetState(PrAppWindow* window, PrAppWindowState state) {
    PR_ASSERT(state <= PrAppWindowState_Fullscreen);
    if (window->setStateFn != nullptr)
        window->setStateFn(window, state);
}

PrAppWindowState prAppWindowState(const PrAppWindow* window) {
    return window->stateFn != nullptr ? window->stateFn(window) : PrAppWindowState_Normal;
}

void prAppWindowSetVisible(PrAppWindow* window, bool visible) {
    if (window->setVisibleFn != nullptr)
        window->setVisibleFn(window, visible);
}

bool prAppWindowIsVisible(const PrAppWindow* window) {
    return window->isVisibleFn == nullptr || window->isVisibleFn(window);
}

bool prAppWindowIsOccluded(const PrAppWindow* window) {
    return window->isOccludedFn != nullptr && window->isOccludedFn(window);
}

bool prAppWindowIsActive(const PrAppWindow* window) {
    return window->isActiveFn == nullptr || window->isActiveFn(window);
}

void prAppWindowFocus(PrAppWindow* window) {
    if (window->focusFn != nullptr)
        window->focusFn(window);
}

void prAppWindowRequestAttention(PrAppWindow* window) {
    if (window->requestAttentionFn != nullptr)
        window->requestAttentionFn(window);
}

void prAppWindowSetBackdrop(PrAppWindow* window, PrAppWindowBackdrop backdrop) {
    PR_ASSERT(backdrop <= PrAppWindowBackdrop_Blur);
    if (window->setBackdropFn != nullptr)
        window->setBackdropFn(window, backdrop);
}

void prAppWindowSetOpacity(PrAppWindow* window, float opacity) {
    PR_ASSERT(opacity >= 0 && opacity <= 1);
    if (window->setOpacityFn != nullptr)
        window->setOpacityFn(window, opacity);
}

void prAppWindowSetKeepAbove(PrAppWindow* window, bool above) {
    if (window->setKeepAboveFn != nullptr)
        window->setKeepAboveFn(window, above);
}

void prAppWindowSetPointerPassthrough(PrAppWindow* window, bool passthrough) {
    if (window->setPointerPassthroughFn != nullptr)
        window->setPointerPassthroughFn(window, passthrough);
}

void prAppWindowSetPointerMode(PrAppWindow* window, PrAppPointerMode mode) {
    PR_ASSERT(mode <= PrAppPointerMode_LockedRaw);
    if (window->setPointerModeFn != nullptr)
        window->setPointerModeFn(window, mode);
}

PrAppPointerMode prAppWindowPointerMode(const PrAppWindow* window) {
    return window->pointerModeFn != nullptr ? window->pointerModeFn(window) : PrAppPointerMode_Free;
}

void prAppWindowSetIcon(PrAppWindow* window, PrPixelView pixels) {
    if (window->setIconFn != nullptr)
        window->setIconFn(window, pixels);
}

void prAppWindowSetHiddenSystemBars(PrAppWindow* window, PrAppSystemBarFlags hidden) {
    PR_ASSERT((hidden & ~PrAppSystemBarFlag_All) == 0);
    if (window->setHiddenSystemBarsFn != nullptr)
        window->setHiddenSystemBarsFn(window, hidden);
}

void prAppWindowSetProgress(PrAppWindow* window, PrAppProgressState state, float progress) {
    PR_ASSERT(state <= PrAppProgressState_Error && progress >= 0 && progress <= 1);
    if (window->setProgressFn != nullptr)
        window->setProgressFn(window, state, progress);
}

void prAppWindowSetDocument(PrAppWindow* window, PrStringView path, bool edited) {
    PR_ASSERT(prUtf8Validate(path) == path.size);
    if (window->setDocumentFn != nullptr)
        window->setDocumentFn(window, path, edited);
}

PrStatus prAppWindowSurface(const PrAppWindow* window, PrAppSurface* out) {
    return window->surfaceFn(window, out);
}

void prAppWindowDragStart(PrAppWindow* window, const PrAppDragDesc* desc) {
    PR_ASSERT((desc->content.forms & PrAppDataFormFlag_Text) == 0 ||
        prUtf8Validate(desc->content.text) == desc->content.text.size);
    PR_ASSERT(desc->promises != nullptr || desc->promiseCount == 0);
    window->dragStartFn(window, desc);
}

void prAppWindowDragPromiseEnd(PrAppWindow* window, uint32_t index, PrStatus status) {
    if (window->dragPromiseEndFn != nullptr)
        window->dragPromiseEndFn(window, index, status);
}

PrAppOutput prAppOutputPlaced(PrAppOutput output, PrVec2 origin, float scale) {
    // A caret in another window stays where that window has it.
    if (output.textInputWindow != nullptr) {
        output.titleBarRegions = nullptr;
        output.titleBarRegionCount = 0;
        return output;
    }
    output.textInput = PrRect{
        .left = origin.x + output.textInput.left * scale,
        .top = origin.y + output.textInput.top * scale,
        .right = origin.x + output.textInput.right * scale,
        .bottom = origin.y + output.textInput.bottom * scale,
    };
    output.titleBarRegions = nullptr;
    output.titleBarRegionCount = 0;
    return output;
}

PrAppOutput prAppOutputMerge(PrAppOutput first, PrAppOutput second, bool pointerInSecond) {
    const PrAppOutput& input = second.hasTextInput ? second : first;
    double frameTime = first.frameTime != 0 ? first.frameTime : second.frameTime;
    if (second.frameTime != 0 && second.frameTime < frameTime)
        frameTime = second.frameTime;
    return PrAppOutput{
        .cursor = pointerInSecond ? second.cursor : first.cursor,
        .wantsFrame = first.wantsFrame || second.wantsFrame,
        .hasTextInput = input.hasTextInput,
        .textInputKind = input.textInputKind,
        .textInput = input.textInput,
        .textInputWindow = input.textInputWindow,
        .frameTime = frameTime,
        .titleBarRegions = first.titleBarRegionCount != 0 ? first.titleBarRegions : second.titleBarRegions,
        .titleBarRegionCount = first.titleBarRegionCount != 0 ? first.titleBarRegionCount : second.titleBarRegionCount,
        .dropOperation = pointerInSecond ? second.dropOperation : first.dropOperation,
    };
}
