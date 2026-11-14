// The Cocoa backend's input: keys, modifiers, cursors, global hotkeys and key labels.

#include "app/backends/cocoa/src/app_cocoa_input.h"

namespace pr_impl {

namespace {

constexpr CocoaMacKey kMacKeys[] = {
    {0, PrAppKeyCode_KeyA}, {1, PrAppKeyCode_KeyS}, {2, PrAppKeyCode_KeyD}, {3, PrAppKeyCode_KeyF},
    {4, PrAppKeyCode_KeyH}, {5, PrAppKeyCode_KeyG}, {6, PrAppKeyCode_KeyZ}, {7, PrAppKeyCode_KeyX},
    {8, PrAppKeyCode_KeyC}, {9, PrAppKeyCode_KeyV}, {10, PrAppKeyCode_IntlBackslash}, {11, PrAppKeyCode_KeyB},
    {12, PrAppKeyCode_KeyQ}, {13, PrAppKeyCode_KeyW}, {14, PrAppKeyCode_KeyE}, {15, PrAppKeyCode_KeyR},
    {16, PrAppKeyCode_KeyY}, {17, PrAppKeyCode_KeyT}, {18, PrAppKeyCode_Digit1}, {19, PrAppKeyCode_Digit2},
    {20, PrAppKeyCode_Digit3}, {21, PrAppKeyCode_Digit4}, {22, PrAppKeyCode_Digit6}, {23, PrAppKeyCode_Digit5},
    {24, PrAppKeyCode_Equal}, {25, PrAppKeyCode_Digit9}, {26, PrAppKeyCode_Digit7}, {27, PrAppKeyCode_Minus},
    {28, PrAppKeyCode_Digit8}, {29, PrAppKeyCode_Digit0}, {30, PrAppKeyCode_BracketRight}, {31, PrAppKeyCode_KeyO},
    {32, PrAppKeyCode_KeyU}, {33, PrAppKeyCode_BracketLeft}, {34, PrAppKeyCode_KeyI}, {35, PrAppKeyCode_KeyP},
    {36, PrAppKeyCode_Enter, PrAppKey_Enter}, {37, PrAppKeyCode_KeyL}, {38, PrAppKeyCode_KeyJ},
    {39, PrAppKeyCode_Quote}, {40, PrAppKeyCode_KeyK}, {41, PrAppKeyCode_Semicolon}, {42, PrAppKeyCode_Backslash},
    {43, PrAppKeyCode_Comma}, {44, PrAppKeyCode_Slash}, {45, PrAppKeyCode_KeyN}, {46, PrAppKeyCode_KeyM},
    {47, PrAppKeyCode_Period}, {48, PrAppKeyCode_Tab, PrAppKey_Tab}, {49, PrAppKeyCode_Space},
    {50, PrAppKeyCode_Backquote}, {51, PrAppKeyCode_Backspace, PrAppKey_Backspace},
    {53, PrAppKeyCode_Escape, PrAppKey_Escape}, {54, PrAppKeyCode_MetaRight, PrAppKey_Meta},
    {55, PrAppKeyCode_MetaLeft, PrAppKey_Meta}, {56, PrAppKeyCode_ShiftLeft, PrAppKey_Shift},
    {57, PrAppKeyCode_CapsLock, PrAppKey_CapsLock}, {58, PrAppKeyCode_AltLeft, PrAppKey_Alt},
    {59, PrAppKeyCode_ControlLeft, PrAppKey_Control}, {60, PrAppKeyCode_ShiftRight, PrAppKey_Shift},
    {61, PrAppKeyCode_AltRight, PrAppKey_Alt}, {62, PrAppKeyCode_ControlRight, PrAppKey_Control},
    {63, PrAppKeyCode_Fn, PrAppKey_Fn}, {64, PrAppKeyCode_F17, PrAppKey_F17}, {65, PrAppKeyCode_NumpadDecimal},
    {67, PrAppKeyCode_NumpadMultiply}, {69, PrAppKeyCode_NumpadAdd}, {71, PrAppKeyCode_NumLock, PrAppKey_Clear},
    {75, PrAppKeyCode_NumpadDivide}, {76, PrAppKeyCode_NumpadEnter, PrAppKey_Enter}, {78, PrAppKeyCode_NumpadSubtract},
    {79, PrAppKeyCode_F18, PrAppKey_F18}, {80, PrAppKeyCode_F19, PrAppKey_F19}, {81, PrAppKeyCode_NumpadEqual},
    {82, PrAppKeyCode_Numpad0}, {83, PrAppKeyCode_Numpad1}, {84, PrAppKeyCode_Numpad2}, {85, PrAppKeyCode_Numpad3},
    {86, PrAppKeyCode_Numpad4}, {87, PrAppKeyCode_Numpad5}, {88, PrAppKeyCode_Numpad6}, {89, PrAppKeyCode_Numpad7},
    {90, PrAppKeyCode_F20, PrAppKey_F20}, {91, PrAppKeyCode_Numpad8}, {92, PrAppKeyCode_Numpad9},
    {93, PrAppKeyCode_IntlYen}, {94, PrAppKeyCode_IntlRo}, {95, PrAppKeyCode_NumpadComma},
    {96, PrAppKeyCode_F5, PrAppKey_F5}, {97, PrAppKeyCode_F6, PrAppKey_F6}, {98, PrAppKeyCode_F7, PrAppKey_F7},
    {99, PrAppKeyCode_F3, PrAppKey_F3}, {100, PrAppKeyCode_F8, PrAppKey_F8}, {101, PrAppKeyCode_F9, PrAppKey_F9},
    {102, PrAppKeyCode_Lang2}, {103, PrAppKeyCode_F11, PrAppKey_F11}, {104, PrAppKeyCode_Lang1},
    {105, PrAppKeyCode_F13, PrAppKey_F13}, {106, PrAppKeyCode_F16, PrAppKey_F16}, {107, PrAppKeyCode_F14, PrAppKey_F14},
    {109, PrAppKeyCode_F10, PrAppKey_F10}, {110, PrAppKeyCode_ContextMenu, PrAppKey_ContextMenu},
    {111, PrAppKeyCode_F12, PrAppKey_F12}, {113, PrAppKeyCode_F15, PrAppKey_F15},
    {114, PrAppKeyCode_Insert, PrAppKey_Insert}, {115, PrAppKeyCode_Home, PrAppKey_Home},
    {116, PrAppKeyCode_PageUp, PrAppKey_PageUp}, {117, PrAppKeyCode_Delete, PrAppKey_Delete},
    {118, PrAppKeyCode_F4, PrAppKey_F4}, {119, PrAppKeyCode_End, PrAppKey_End}, {120, PrAppKeyCode_F2, PrAppKey_F2},
    {121, PrAppKeyCode_PageDown, PrAppKey_PageDown}, {122, PrAppKeyCode_F1, PrAppKey_F1},
    {123, PrAppKeyCode_ArrowLeft, PrAppKey_Left}, {124, PrAppKeyCode_ArrowRight, PrAppKey_Right},
    {125, PrAppKeyCode_ArrowDown, PrAppKey_Down}, {126, PrAppKeyCode_ArrowUp, PrAppKey_Up},
};

// The device-dependent flags (NX_DEVICE*KEYMASK in IOKit) that tell a modifier's left key from its right.
constexpr NSEventModifierFlags kLeftControl = 0x0001;
constexpr NSEventModifierFlags kLeftShift = 0x0002;
constexpr NSEventModifierFlags kRightShift = 0x0004;
constexpr NSEventModifierFlags kLeftCommand = 0x0008;
constexpr NSEventModifierFlags kRightCommand = 0x0010;
constexpr NSEventModifierFlags kLeftOption = 0x0020;
constexpr NSEventModifierFlags kRightOption = 0x0040;
constexpr NSEventModifierFlags kRightControl = 0x2000;

// Returns a cursor that shows nothing, for PrAppCursor_None. A hidden cursor would need [NSCursor hide], whose calls
// must balance, so a transparent image stands in for it.
NSCursor* emptyCursor() {
    static NSCursor* empty = [[NSCursor alloc] initWithImage:[[NSImage alloc] initWithSize:NSMakeSize(1, 1)]
        hotSpot:NSZeroPoint];
    return empty;
}

// Returns the system's cursor that resizes a frame from `position`, outward only or in both directions.
NSCursor* frameResize(NSCursorFrameResizePosition position, NSCursorFrameResizeDirections directions) {
    return [NSCursor frameResizeCursorFromPosition:position inDirections:directions];
}

// Returns the cursor of AppKit's undeclared class method `name`, or the arrow if AppKit has none. Only the help and
// busy cursors come from here: some undeclared methods, such as `_moveCursor`, answer the selector and then throw.
NSCursor* undeclaredCursor(NSString* name) {
    SEL selector = NSSelectorFromString(name);
    if (![NSCursor respondsToSelector:selector])
        return NSCursor.arrowCursor;
    using CursorFn = NSCursor* (*)(id, SEL);
    NSCursor* cursor = reinterpret_cast<CursorFn>([NSCursor methodForSelector:selector])(NSCursor.class, selector);
    return cursor != nil ? cursor : NSCursor.arrowCursor;
}

// Returns a cursor of arrows in four directions, for Move and AllScroll, which AppKit lacks. It is drawn in black with
// a white outline, as the system's cursors are.
NSCursor* moveCursor() {
    static NSCursor* move = [] {
        constexpr CGFloat kSize = 24;
        NSImage* image = [NSImage imageWithSize:NSMakeSize(kSize, kSize) flipped:NO drawingHandler:^BOOL(NSRect bounds) {
            (void)bounds;
            // One arrow, pointing up from the center, turned to each direction.
            NSBezierPath* arrows = [NSBezierPath bezierPath];
            for (int turn = 0; turn < 4; ++turn) {
                NSAffineTransform* transform = [NSAffineTransform transform];
                [transform translateXBy:kSize / 2 yBy:kSize / 2];
                [transform rotateByDegrees:CGFloat(turn * 90)];
                NSBezierPath* arrow = [NSBezierPath bezierPath];
                [arrow moveToPoint:NSMakePoint(-1.5, 0)];
                [arrow lineToPoint:NSMakePoint(-1.5, 6)];
                [arrow lineToPoint:NSMakePoint(-4.5, 6)];
                [arrow lineToPoint:NSMakePoint(0, 10.5)];
                [arrow lineToPoint:NSMakePoint(4.5, 6)];
                [arrow lineToPoint:NSMakePoint(1.5, 6)];
                [arrow lineToPoint:NSMakePoint(1.5, 0)];
                [arrow closePath];
                [arrow transformUsingAffineTransform:transform];
                [arrows appendBezierPath:arrow];
            }
            arrows.lineJoinStyle = NSLineJoinStyleRound;
            arrows.lineWidth = 2;
            [NSColor.whiteColor setStroke];
            [arrows stroke];
            [NSColor.blackColor setFill];
            [arrows fill];
            return YES;
        }];
        return [[NSCursor alloc] initWithImage:image hotSpot:NSMakePoint(kSize / 2, kSize / 2)];
    }();
    return move;
}

// The signature of the application's hot keys, which tells them from those of other code in the process.
constexpr OSType kHotkeySignature = 'PrAx';

OSStatus hotkeyPressed(EventHandlerCallRef, EventRef event, void* context) {
    EventHotKeyID id{};
    OSStatus status = GetEventParameter(event, kEventParamDirectObject, typeEventHotKeyID, nullptr, sizeof(id), nullptr, &id);
    if (status != noErr || id.signature != kHotkeySignature)
        return eventNotHandledErr;
    [(__bridge PrCocoaHost*)context deliverHotkey:id.id];
    return noErr;
}

UInt32 carbonModifiersOf(PrAppModifierFlags modifiers) {
    UInt32 carbon = 0;
    if ((modifiers & PrAppModifierFlag_Shift) != 0)
        carbon |= shiftKey;
    if ((modifiers & PrAppModifierFlag_Control) != 0)
        carbon |= controlKey;
    if ((modifiers & PrAppModifierFlag_Alt) != 0)
        carbon |= optionKey;
    if ((modifiers & PrAppModifierFlag_Super) != 0)
        carbon |= cmdKey;
    return carbon;
}

// Writes the virtual key code of the key at `code` to `out`, and returns false for a key that a Mac's keyboard lacks.
bool virtualKeyOf(PrAppKeyCode code, uint16_t* out) {
    for (const CocoaMacKey& key : kMacKeys) {
        if (key.code == code) {
            *out = key.virtualKey;
            return true;
        }
    }
    return false;
}

// Returns the character that the key `virtualKey` types without modifiers in the current layout, uppercased, or nil if
// it types none that a label can show.
NSString* PR_NULLABLE layoutCharacterOf(uint16_t virtualKey) {
    TISInputSourceRef source = TISCopyCurrentKeyboardLayoutInputSource();
    if (source == nullptr)
        return nil;
    CFDataRef data = static_cast<CFDataRef>(TISGetInputSourceProperty(source, kTISPropertyUnicodeKeyLayoutData));
    NSString* result = nil;
    if (data != nullptr) {
        const UCKeyboardLayout* layout = reinterpret_cast<const UCKeyboardLayout*>(CFDataGetBytePtr(data));
        UInt32 dead = 0;
        UniChar characters[4];
        UniCharCount length = 0;
        OSStatus status = UCKeyTranslate(layout, virtualKey, kUCKeyActionDisplay, 0, LMGetKbdType(),
            kUCKeyTranslateNoDeadKeysBit, &dead, countOf(characters), &length, characters);
        // A control character or a space is a key that a label names rather than shows.
        if (status == noErr && length != 0 && characters[0] > ' ' && characters[0] != 0x7f)
            result = [NSString stringWithCharacters:characters length:length].uppercaseString;
    }
    CFRelease(source);
    return result;
}

} // namespace

bool cocoaModifierDown(NSEvent* event, PrAppKeyCode code) {
    NSEventModifierFlags flags = event.modifierFlags;
    switch (code) {
        case PrAppKeyCode_ShiftLeft: return (flags & kLeftShift) != 0;
        case PrAppKeyCode_ShiftRight: return (flags & kRightShift) != 0;
        case PrAppKeyCode_ControlLeft: return (flags & kLeftControl) != 0;
        case PrAppKeyCode_ControlRight: return (flags & kRightControl) != 0;
        case PrAppKeyCode_AltLeft: return (flags & kLeftOption) != 0;
        case PrAppKeyCode_AltRight: return (flags & kRightOption) != 0;
        case PrAppKeyCode_MetaLeft: return (flags & kLeftCommand) != 0;
        case PrAppKeyCode_MetaRight: return (flags & kRightCommand) != 0;
        case PrAppKeyCode_CapsLock: return (flags & NSEventModifierFlagCapsLock) != 0;
        case PrAppKeyCode_Fn: return (flags & NSEventModifierFlagFunction) != 0;
        default: return false;
    }
}

void cocoaDescribePen(NSEvent* source, bool eraser, PrAppEvent* event) {
    event->pointer.type = PrAppPointerType_Pen;
    event->pointer.id = kPenPointerBase + uint32_t(source.deviceID);
    event->pointer.pressure = float(source.pressure);
    NSPoint tilt = source.tilt;
    event->pointer.tilt = PrVec2{float(tilt.x) * 90.0f, float(-tilt.y) * 90.0f};
    float twist = 360.0f - float(fmod(double(source.rotation), 360.0));
    event->pointer.twist = twist >= 360.0f ? twist - 360.0f : twist;
    if (eraser && event->kind != PrAppEventKind_PointerMove)
        event->pointer.button = kPrAppPointerButtonEraser;
}

PrAppModifierFlags cocoaModifiersOf(NSEventModifierFlags flags) {
    PrAppModifierFlags modifiers = 0;
    if ((flags & NSEventModifierFlagCapsLock) != 0)
        modifiers |= PrAppModifierFlag_CapsLock;
    if ((flags & NSEventModifierFlagShift) != 0)
        modifiers |= PrAppModifierFlag_Shift;
    if ((flags & NSEventModifierFlagControl) != 0)
        modifiers |= PrAppModifierFlag_Control;
    if ((flags & NSEventModifierFlagOption) != 0)
        modifiers |= PrAppModifierFlag_Alt;
    if ((flags & NSEventModifierFlagCommand) != 0)
        modifiers |= PrAppModifierFlag_Super;
    return modifiers;
}

CocoaMacKey cocoaMacKeyOf(NSEvent* event) {
    for (const CocoaMacKey& key : kMacKeys) {
        if (key.virtualKey == event.keyCode)
            return key;
    }
    return CocoaMacKey{event.keyCode, PrAppKeyCode_Unidentified, PrAppKey_Unknown};
}

NSString* PR_NULLABLE cocoaCharacterOf(NSEvent* event) {
    NSEventModifierFlags commands = NSEventModifierFlagCommand | NSEventModifierFlagControl;
    // charactersIgnoringModifiers keeps Shift and drops Option, Control and Command.
    NSString* characters = (event.modifierFlags & commands) != 0 ? event.charactersIgnoringModifiers : event.characters;
    unichar first = characters.length != 0 ? [characters characterAtIndex:0] : 0;
    // Excludes control characters, delete, and the private-use range that AppKit assigns to function keys.
    bool printable = first >= 32 && first != 127 && (first < 0xF700 || first > 0xF8FF);
    return printable ? characters : nil;
}

bool cocoaIsDead(NSEvent* event) {
    if (event.characters.length != 0)
        return false;
    NSString* plain = event.charactersIgnoringModifiers;
    return plain.length != 0 && [plain characterAtIndex:0] >= 32;
}

uint32_t cocoaUtf8Length(NSString* string, NSUInteger units) {
    uint32_t size = prAppleNSStringToUtf8([string substringToIndex:MIN(units, string.length)], nullptr, 0);
    return size != kPrAppleStringInvalid ? size : 0;
}

NSString* cocoaStringOf(PrStringView text) {
    NSString* string = prAppleNSString(text);
    return string != nil ? string : @"";
}

NSCursor* cocoaCursorFor(PrAppCursor cursor) {
    constexpr NSCursorFrameResizeDirections kOutward = NSCursorFrameResizeDirectionsOutward;
    constexpr NSCursorFrameResizeDirections kBoth = NSCursorFrameResizeDirectionsAll;
    switch (cursor) {
        case PrAppCursor_None: return emptyCursor();
        case PrAppCursor_ContextMenu: return NSCursor.contextualMenuCursor;
        case PrAppCursor_Help: return undeclaredCursor(@"_helpCursor");
        case PrAppCursor_Hand: return NSCursor.pointingHandCursor;
        case PrAppCursor_Progress: return undeclaredCursor(@"busyButClickableCursor");
        case PrAppCursor_Wait: return undeclaredCursor(@"busyButClickableCursor");
        case PrAppCursor_Cell: return NSCursor.crosshairCursor;
        case PrAppCursor_Crosshair: return NSCursor.crosshairCursor;
        case PrAppCursor_Text: return NSCursor.IBeamCursor;
        case PrAppCursor_VerticalText: return NSCursor.IBeamCursorForVerticalLayout;
        case PrAppCursor_Alias: return NSCursor.dragLinkCursor;
        case PrAppCursor_Copy: return NSCursor.dragCopyCursor;
        case PrAppCursor_Move: return moveCursor();
        case PrAppCursor_NoDrop: return NSCursor.operationNotAllowedCursor;
        case PrAppCursor_NotAllowed: return NSCursor.operationNotAllowedCursor;
        case PrAppCursor_Grab: return NSCursor.openHandCursor;
        case PrAppCursor_Grabbing: return NSCursor.closedHandCursor;
        case PrAppCursor_ResizeN: return frameResize(NSCursorFrameResizePositionTop, kOutward);
        case PrAppCursor_ResizeE: return frameResize(NSCursorFrameResizePositionRight, kOutward);
        case PrAppCursor_ResizeS: return frameResize(NSCursorFrameResizePositionBottom, kOutward);
        case PrAppCursor_ResizeW: return frameResize(NSCursorFrameResizePositionLeft, kOutward);
        case PrAppCursor_ResizeNE: return frameResize(NSCursorFrameResizePositionTopRight, kOutward);
        case PrAppCursor_ResizeNW: return frameResize(NSCursorFrameResizePositionTopLeft, kOutward);
        case PrAppCursor_ResizeSE: return frameResize(NSCursorFrameResizePositionBottomRight, kOutward);
        case PrAppCursor_ResizeSW: return frameResize(NSCursorFrameResizePositionBottomLeft, kOutward);
        case PrAppCursor_ResizeX: return frameResize(NSCursorFrameResizePositionRight, kBoth);
        case PrAppCursor_ResizeY: return frameResize(NSCursorFrameResizePositionBottom, kBoth);
        case PrAppCursor_ResizeNESW: return frameResize(NSCursorFrameResizePositionTopRight, kBoth);
        case PrAppCursor_ResizeNWSE: return frameResize(NSCursorFrameResizePositionTopLeft, kBoth);
        case PrAppCursor_ResizeColumn: return NSCursor.columnResizeCursor;
        case PrAppCursor_ResizeRow: return NSCursor.rowResizeCursor;
        case PrAppCursor_AllScroll: return moveCursor();
        case PrAppCursor_ZoomIn: return NSCursor.zoomInCursor;
        case PrAppCursor_ZoomOut: return NSCursor.zoomOutCursor;
        default: return NSCursor.arrowCursor;
    }
}

void cocoaHotkeyUnregister(PrApp* app, uint64_t tag) {
    RawArray<CocoaHotkey>& hotkeys = static_cast<CocoaApp*>(app)->hotkeys;
    for (uint32_t i = 0; i < hotkeys.count; ++i) {
        if (hotkeys.data[i].tag == tag) {
            UnregisterEventHotKey(hotkeys.data[i].ref);
            hotkeys.erase(i);
            return;
        }
    }
}

PrStatus cocoaHotkeyRegister(PrApp* app, PrAppHotkey hotkey, uint64_t tag) {
    CocoaApp* cocoa = static_cast<CocoaApp*>(app);
    PrCocoaHost* host = cocoaHostOf(app);
    if (!host.running)
        return PrStatus_InvalidState;
    uint16_t key = 0;
    if (host.headless || !virtualKeyOf(hotkey.code, &key))
        return PrStatus_NotSupported;
    if (cocoa->hotkeyHandler == nullptr) {
        EventTypeSpec pressed{kEventClassKeyboard, kEventHotKeyPressed};
        EventHandlerRef handler = nullptr;
        if (InstallEventHandler(GetApplicationEventTarget(), hotkeyPressed, 1, &pressed, (__bridge void*)host, &handler) != noErr)
            return PrStatus_NotSupported;
        cocoa->hotkeyHandler = handler;
    }
    cocoaHotkeyUnregister(app, tag);
    uint32_t id = ++cocoa->lastHotkeyId;
    EventHotKeyRef ref = nullptr;
    if (RegisterEventHotKey(key, carbonModifiersOf(hotkey.modifiers), EventHotKeyID{kHotkeySignature, id},
            GetApplicationEventTarget(), 0, &ref) != noErr)
        return PrStatus_NotAllowed;
    cocoa->hotkeys.push(CocoaHotkey{.tag = tag, .id = id, .ref = ref});
    return PrStatus_Ok;
}

void cocoaHotkeysReset(CocoaApp* app) {
    for (uint32_t i = 0; i < app->hotkeys.count; ++i)
        UnregisterEventHotKey(app->hotkeys.data[i].ref);
    app->hotkeys.reset();
    if (app->hotkeyHandler != nullptr)
        RemoveEventHandler(app->hotkeyHandler);
    app->hotkeyHandler = nullptr;
}

PrStringView cocoaKeyCodeLabel(const PrApp* app, PrAppKeyCode code) {
    const CocoaApp* cocoa = static_cast<const CocoaApp*>(app);
    if (code >= PrAppKeyCode_Count)
        return prAppKeyCodeName(PrAppKeyCode_Unidentified);
    if (!cocoa->keyLabeled[code]) {
        cocoa->keyLabeled[code] = true;
        uint16_t virtualKey = 0;
        NSString* character = virtualKeyOf(code, &virtualKey) ? layoutCharacterOf(virtualKey) : nil;
        prRawStringReset(&cocoa->keyLabels[code]);
        if (character != nil)
            cocoa->keyLabels[code] = prAppleNSStringToRawString(character);
    }
    if (!prRawStringIsEmpty(&cocoa->keyLabels[code]))
        return prRawStringView(&cocoa->keyLabels[code]);
    return prAppKeyCodeName(code);
}

void cocoaKeyLabelsReset(CocoaApp* app) {
    for (uint32_t i = 0; i < PrAppKeyCode_Count; ++i) {
        prRawStringReset(&app->keyLabels[i]);
        app->keyLabeled[i] = false;
    }
}

} // namespace pr_impl
