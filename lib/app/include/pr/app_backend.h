// The interface that application backends implement. Programs call the functions in lib/app's other headers, which
// check their arguments and forward to these. Only backends include this header.

#pragma once

#include "pr/app.h"
#include "pr/base.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// The data that a read requests: the forms that a source holds, its content in one form, or bytes of a file. The
// trailing comment of each kind names the type of the read's `done` callback.
enum {
    PrAppDataReadKind_Forms,        // PrAppDataFormsFn
    PrAppDataReadKind_Text,         // PrAppDataTextFn
    PrAppDataReadKind_Html,         // PrAppDataTextFn
    PrAppDataReadKind_Urls,         // PrAppDataUrlsFn
    PrAppDataReadKind_Files,        // PrAppDataFilesFn
    PrAppDataReadKind_Image,        // PrAppDataImageFn
    PrAppDataReadKind_Custom,       // PrAppDataBytesFn, for the content of `type`
    PrAppDataReadKind_File,         // PrAppDataBytesFn, for `size` bytes at `offset` in the file at `index`
};
typedef uint8_t PrAppDataReadKind;

// A read request (pr/app_data.h). `done` has the callback type that `kind` names. `type`, `index`, `offset` and `size`
// apply only to the kinds that mention them.
typedef struct PrAppDataRead {
    PrAppDataSource source;
    PrAppDataReadKind kind;
    PrStringView type;
    uint32_t index;
    uint64_t offset;
    uint64_t size;
    void (*done)(void);
    void* PR_NULLABLE context;
} PrAppDataRead;

// The function table of an application backend, placed at the start of the backend's state. Each function implements
// the public function of the same name.
//
// The retain, release, read and write functions and `cancelFn` may be called on any thread. A null function means that
// the backend does not implement it. The public call then does nothing, or returns NotSupported, false, zero or the
// default value. The read and dialog functions are never null, so that every callback receives an answer.
struct PrApp {
    void (*destroyFn)(PrApp* app);
    void (*runFn)(PrApp* app, PrAppHandler handler);
    void (*stopFn)(PrApp* app);
    void (*wakeFn)(PrApp* app);
    void (*cancelFn)(PrApp* app, void* PR_NULLABLE context);
    void (*PR_NULLABLE setPresenceFn)(PrApp* app, PrAppPresence presence);
    PrAppWindowModel (*PR_NULLABLE windowModelFn)(const PrApp* app);
    void (*PR_NULLABLE setHiddenFn)(PrApp* app, bool hidden);
    bool (*PR_NULLABLE isHiddenFn)(const PrApp* app);
    PrAppLifecycle (*PR_NULLABLE lifecycleFn)(const PrApp* app);
    void (*PR_NULLABLE quitCancelFn)(PrApp* app);
    void (*PR_NULLABLE timerSetFn)(PrApp* app, uint64_t tag, double delay, double interval);
    void (*PR_NULLABLE timerCancelFn)(PrApp* app, uint64_t tag);

    // Displays
    uint32_t (*displaysFn)(const PrApp* app, PrAppDisplayInfo* PR_NULLABLE out, uint32_t capacity);
    bool (*displayInfoFn)(const PrApp* app, PrAppDisplayId id, PrAppDisplayInfo* out);
    bool (*PR_NULLABLE pointerPositionFn)(const PrApp* app, PrAppDisplayPosition* out);
    void (*PR_NULLABLE setOrientationsFn)(PrApp* app, PrAppOrientationFlags orientations);

    // Input
    PrStringView (*PR_NULLABLE keyCodeLabelFn)(const PrApp* app, PrAppKeyCode code);
    PrStatus (*PR_NULLABLE hotkeyRegisterFn)(PrApp* app, PrAppHotkey hotkey, uint64_t tag);
    void (*PR_NULLABLE hotkeyUnregisterFn)(PrApp* app, uint64_t tag);

    // Data
    void (*dataRetainFn)(PrApp* app, PrAppDataSource source);
    void (*dataReleaseFn)(PrApp* app, PrAppDataSource source);
    void (*dataReadFn)(PrApp* app, const PrAppDataRead* read);
    PrStatus (*dataWriteFn)(PrApp* app, PrAppDataSource source, const PrAppDataContent* content);
    void (*dataWriteFileFn)(PrApp* app, PrAppDataSource source, uint32_t index, uint64_t offset, PrByteView bytes,
        PrAppDataStatusFn done, void* PR_NULLABLE context);
    void (*fileDialogFn)(PrApp* app, const PrAppFileDialogDesc* desc, PrAppFileDialogFn done, void* PR_NULLABLE context);

    // Menus
    void (*PR_NULLABLE setMenuBarFn)(PrApp* app, const PrAppMenu* PR_NULLABLE menu);
    PrStatus (*PR_NULLABLE statusItemCreateFn)(PrApp* app, const PrAppStatusItemDesc* desc,
        PrAppStatusItem* PR_NULLABLE* PR_NONNULL out);

    // Shell
    PrStatus (*PR_NULLABLE notificationPostFn)(PrApp* app, const PrAppNotificationDesc* desc);
    PrStatus (*openUrlFn)(PrApp* app, PrStringView url);
    PrStatus (*PR_NULLABLE revealPathFn)(PrApp* app, PrStringView path);
    PrStatus (*PR_NULLABLE trashPathFn)(PrApp* app, PrStringView path);
    void (*PR_NULLABLE recentDocumentAddFn)(PrApp* app, PrStringView path);
    void (*PR_NULLABLE recentDocumentsClearFn)(PrApp* app);
    void (*PR_NULLABLE setBadgeFn)(PrApp* app, PrStringView badge);

    // Device
    PrAppPower (*PR_NULLABLE powerFn)(const PrApp* app);
    void (*PR_NULLABLE setAwakeFn)(PrApp* app, PrAppAwakeFlags flags);
    PrStatus (*PR_NULLABLE sensorEnableFn)(PrApp* app, PrAppSensor sensor, bool enable, double interval);
    void (*PR_NULLABLE hapticPlayFn)(PrApp* app, PrAppHaptic haptic);

    PrStatus (*windowCreateFn)(PrApp* app, const PrAppWindowDesc* desc, PrAppWindow* PR_NULLABLE* PR_NONNULL out);
};

// The function table of a window backend, placed at the start of the window's state. Each function implements the
// public function of the same name.
//
// If one of the window's callbacks is running, `destroyFn` frees the window when that callback returns. Some public
// functions promise that a change requests a frame, such as prAppWindowIsActive and prAppWindowState. A backend that
// implements them requests the frame.
//
// A null function means that the backend does not implement it. The public call then does nothing or returns the
// default: Normal for the state, visible, and Free for the pointer mode. `dragStartFn` and `shareFn` are never null, so
// that every drag ends and every share answers. A backend that cannot start a drag ends it with None, and one without a
// share sheet answers NotSupported.
struct PrAppWindow {
    void (*destroyFn)(PrAppWindow* window);
    PrApp* PR_NONNULL (*PR_NONNULL appFn)(const PrAppWindow* window);
    void (*requestFrameFn)(PrAppWindow* window);
    void (*setTitleFn)(PrAppWindow* window, PrStringView title);
    PrStringView (*titleFn)(const PrAppWindow* window);
    void (*PR_NULLABLE setExtentFn)(PrAppWindow* window, PrUVec2 extent);
    void (*PR_NULLABLE setExtentLimitsFn)(PrAppWindow* window, PrUVec2 minExtent, PrUVec2 maxExtent);
    void (*PR_NULLABLE setAspectRatioFn)(PrAppWindow* window, float ratio);
    bool (*PR_NULLABLE positionFn)(const PrAppWindow* window, PrAppDisplayPosition* out);
    void (*PR_NULLABLE setPositionFn)(PrAppWindow* window, const PrAppDisplayPosition* position);
    bool (*PR_NULLABLE pointToDisplayFn)(const PrAppWindow* window, PrVec2 point, PrAppDisplayPosition* out);
    bool (*PR_NULLABLE pointFromDisplayFn)(const PrAppWindow* window, const PrAppDisplayPosition* position, PrVec2* out);
    void (*PR_NULLABLE setPopupPlacementFn)(PrAppWindow* window, const PrAppPopupPlacement* placement);
    void (*PR_NULLABLE setStateFn)(PrAppWindow* window, PrAppWindowState state);
    PrAppWindowState (*PR_NULLABLE stateFn)(const PrAppWindow* window);
    void (*PR_NULLABLE setVisibleFn)(PrAppWindow* window, bool visible);
    bool (*PR_NULLABLE isVisibleFn)(const PrAppWindow* window);
    bool (*PR_NULLABLE isOccludedFn)(const PrAppWindow* window);
    bool (*PR_NULLABLE isActiveFn)(const PrAppWindow* window);
    void (*PR_NULLABLE focusFn)(PrAppWindow* window);
    void (*PR_NULLABLE requestAttentionFn)(PrAppWindow* window);
    void (*PR_NULLABLE setBackdropFn)(PrAppWindow* window, PrAppWindowBackdrop backdrop);
    void (*PR_NULLABLE setOpacityFn)(PrAppWindow* window, float opacity);
    void (*PR_NULLABLE setKeepAboveFn)(PrAppWindow* window, bool above);
    void (*PR_NULLABLE setPointerPassthroughFn)(PrAppWindow* window, bool passthrough);
    void (*PR_NULLABLE setPointerModeFn)(PrAppWindow* window, PrAppPointerMode mode);
    PrAppPointerMode (*PR_NULLABLE pointerModeFn)(const PrAppWindow* window);
    void (*PR_NULLABLE setIconFn)(PrAppWindow* window, PrPixelView pixels);
    void (*PR_NULLABLE setHiddenSystemBarsFn)(PrAppWindow* window, PrAppSystemBarFlags hidden);
    void (*PR_NULLABLE setProgressFn)(PrAppWindow* window, PrAppProgressState state, float progress);
    void (*PR_NULLABLE setDocumentFn)(PrAppWindow* window, PrStringView path, bool edited);
    void (*PR_NULLABLE setMenuBarFn)(PrAppWindow* window, const PrAppMenu* PR_NULLABLE menu);
    void (*PR_NULLABLE showMenuFn)(PrAppWindow* window, const PrAppMenu* menu, PrVec2 position);
    void (*shareFn)(PrAppWindow* window, const PrAppDataContent* content, PrRect anchor, PrAppShareFn done,
        void* PR_NULLABLE context);
    void (*dragStartFn)(PrAppWindow* window, const PrAppDragDesc* desc);
    void (*PR_NULLABLE dragPromiseEndFn)(PrAppWindow* window, uint32_t index, PrStatus status);
    PrStatus (*surfaceFn)(const PrAppWindow* window, PrAppSurface* out);
};

// The function table of a status item backend, placed at the start of the item's state.
struct PrAppStatusItem {
    void (*destroyFn)(PrAppStatusItem* item);
    void (*setFn)(PrAppStatusItem* item, const PrAppStatusItemDesc* desc);
};

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
