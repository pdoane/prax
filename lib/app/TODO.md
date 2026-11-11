# lib/app: remaining work

A temporary checklist for the worktree-app branch. Delete it when the branch lands. A null slot in a backend's
function table (pr/app_backend.h) reads the same whether the feature is unfinished or impossible on the platform, so
this file is the record of which is which. Delete each item when it is done.

Most items are checks by hand through examples/app_features, whose pages cover lib/app. The headless tests cannot
drive menus, gestures, sheets, drags or the system's notices.

## macOS

Every slot is implemented, and each page of app_features has been checked by hand.

## Windows (Win32)

The backend has never been compiled.

- [ ] Compile the backend and run its tests
- [ ] A modal dialog with decoration as a sheet with no title bar, and the parent of every modal dialog dimmed while
      it shows. It is an ordinary owned dialog now
- [ ] Check by hand the window model, which refuses a second window of a Single application, and the state that a
      window opens in
- [ ] Check by hand what was written blind: timers, the lifecycle, ClipboardChanged, power and its events, the
      session lock, LowMemory, the session's end (WM_QUERYENDSESSION and its block reason), and keep awake
- [ ] The message window is now a hidden top-level window: check that the notification icon, the clipboard, hotkeys
      and the dialogs still work through it
- [ ] Single instance through the mutex and WM_COPYDATA
- [ ] Key labels and KeyboardLayoutChanged
- [ ] The taskbar's progress and badge overlay, recent documents, and the trash
- [ ] Menus: the menu bar on each Normal window, the window's own, context menus, their shortcuts, and MenuOpening
      updating a menu in place
- [ ] Status items, with and without a menu
- [ ] Popups, dialogs and their modal blocking, Decoration None, and Accessory presence keeping windows off the
      taskbar
- [ ] Display names, refresh rates and orientations
- [ ] The password input setting the input method aside, the accent color and the text scale
- [ ] LockedRaw through Raw Input
- [ ] `setOpacityFn`: a layered window does not present a flip-model swap chain, so this needs DirectComposition
- [ ] `setBackdropFn`: Acrylic for Blur, and Transparent, both through DirectComposition
- [ ] `shareFn` (DataTransferManager), which answers NotSupported
- [ ] File promises: `PrAppDragDesc::promises`, DragPromise events and `dragPromiseEndFn` (CFSTR_FILEDESCRIPTOR)
- [ ] Gesture events (WM_GESTURE, or Direct Manipulation for a precision touchpad) and TrackpadTouches events
      (Precision Touchpad raw input)
- [ ] The editing items of a menu send MenuItem to the window that has the keyboard, but a system text field, as in
      a file dialog, does not see them

## Not applicable

These slots stay null, and need no work:

- The web has no native menus, status items, global hotkeys, trash, file manager, recent documents, presence, quit
  request or window progress. It cannot confine the pointer, keep the system awake, or tell when the clipboard
  changes.
- Orientation, system bars, motion sensors, `PrAppKey_GoBack`, and the safe area and keyboard insets belong to a
  mobile backend. The primary selection reads as empty on macOS and Windows.
- On macOS, `setIconFn`, because the system shows only the application's icon, and `textScale`, which stays 1 because
  macOS has no system-wide text size.
- On Windows, `setDocumentFn`, because the system shows no document in a title bar.
