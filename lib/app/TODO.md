# lib/app: remaining work

A temporary checklist for the worktree-app branch. Delete it when the branch lands. A null slot in a backend's
function table (pr/app_backend.h) reads the same whether the feature is unfinished or impossible on the platform, so
this file is the record of which is which. Delete each item when it is done.

Most items are checks by hand through examples/app_features, whose pages cover lib/app. The headless tests cannot
drive menus, gestures, sheets, drags or the system's notices.

## macOS

Every slot is implemented, and each page of app_features has been checked by hand.

## UIKit: iOS, tvOS and visionOS

The backend runs in the simulators (`./dev.sh example ios-sim draw_gallery`, and the same with tvos-sim and
visionos-sim, and app_features on an iPad simulator for several windows). It has never run on a device. tvOS renders,
and its remote, its focus and game controllers wait for lib/app's gamepads. visionOS shows windows, and its immersive
rendering is to come. Everything below the first item is written and built, and has been
seen only as far as a screenshot shows it.

- [ ] Check by hand on a device: touches, the Pencil, the hover and scroll of a trackpad, hardware keys, the on-screen
      keyboard, that a hardware key's Text follows its KeyDown, and that Enter and Backspace arrive once
- [ ] Check by hand composition through an input method (UITextInput), whose document is the composed text alone, and
      that its candidates show at the caret
- [ ] Check by hand pinch and rotation Gesture events, which arrive beside the touches' pointer events
- [ ] Check by hand the pointer's text beam and hidden cursor, the only cursors that the system's pointer has
- [ ] Check by hand drops (UIDropInteraction) of text, URLs and files, and clipboard images
- [ ] Check by hand the document picker for files and directories, notifications and their permission, the badge, and
      sensors, which the simulator lacks
- [ ] Check by hand several windows on an iPad: a second window's scene, closing a scene, a scene that the user opens,
      which closes, and positions, which iPadOS reports for a window the user can move
- [ ] Rotation: DisplaysChanged after the transition, and the frame at the new extent
- [ ] Drags from the application (UIDragInteraction), which end with None
- [ ] Save dialogs, which answer NotSupported: the picker exports a file that exists, and the program writes its file
      after the dialog answers
- [ ] A window that keeps running after its scene closed has no scene, and does not get one back
- [ ] A scene that the user opens could ask the program for a window, where now it closes

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
- On UIKit's platforms, the window's extent, position, state, opacity, backdrop, keep-above, passthrough, pointer
  mode, icon, progress and document, which the system decides or has no place for, the menu bar, status items, global
  hotkeys, presence, hiding, the quit request, the trash, the file manager, recent documents and key labels, which the
  system does not have, and `PrAppKey_GoBack`, which no iOS key sends. tvOS has no clipboard, document picker, share
  sheet, drops, pointer, gestures, keyboard frame, orientations, haptics, motion sensors, battery, extended range or
  notifications but the badge. visionOS has no displays, so no display list, positions or extended range, and no
  haptics, magnetometer or keyboard frame.
