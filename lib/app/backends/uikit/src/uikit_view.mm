// The UIKit backend's view: a window's touches, pointer, gestures, keys, text, composition and drops.

#include "pr/alloc.h"
#include "pr/count_of.h"
#include "pr/vec.h"

#include "app/backends/shared/src/app_shared_drag.h"
#include "app/backends/uikit/src/app_uikit.h"

#import <QuartzCore/QuartzCore.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

// A position in the text that an input method composes: the UTF-16 offset from its start.
@interface PrUiKitTextPosition : UITextPosition
@property(nonatomic) NSUInteger offset;
+ (instancetype)at:(NSUInteger)offset;
@end

// A range of the text that an input method composes.
@interface PrUiKitTextRange : UITextRange
@property(nonatomic) NSRange range;
+ (instancetype)of:(NSRange)range;
@end

namespace pr_impl {

namespace {

// The pointer id of the first touch. Touches count up from here, so that they never equal kPrAppPointerMouse.
constexpr uint32_t kTouchPointerBase = 2;

// ---------------------------------------------------------------------------------------------------------------------
// Keys

// A key's position, by its HID usage, and what it means when it types no character.
struct HidKey {
    UIKeyboardHIDUsage usage;
    PrAppKeyCode code;
    PrAppKey key;
};

constexpr HidKey kHidKeys[] = {
    {UIKeyboardHIDUsageKeyboardReturnOrEnter, PrAppKeyCode_Enter, PrAppKey_Enter},
    {UIKeyboardHIDUsageKeyboardEscape, PrAppKeyCode_Escape, PrAppKey_Escape},
    {UIKeyboardHIDUsageKeyboardDeleteOrBackspace, PrAppKeyCode_Backspace, PrAppKey_Backspace},
    {UIKeyboardHIDUsageKeyboardTab, PrAppKeyCode_Tab, PrAppKey_Tab},
    {UIKeyboardHIDUsageKeyboardSpacebar, PrAppKeyCode_Space, ' '},
    {UIKeyboardHIDUsageKeyboardHyphen, PrAppKeyCode_Minus, PrAppKey_Unknown},
    {UIKeyboardHIDUsageKeyboardEqualSign, PrAppKeyCode_Equal, PrAppKey_Unknown},
    {UIKeyboardHIDUsageKeyboardOpenBracket, PrAppKeyCode_BracketLeft, PrAppKey_Unknown},
    {UIKeyboardHIDUsageKeyboardCloseBracket, PrAppKeyCode_BracketRight, PrAppKey_Unknown},
    {UIKeyboardHIDUsageKeyboardBackslash, PrAppKeyCode_Backslash, PrAppKey_Unknown},
    {UIKeyboardHIDUsageKeyboardNonUSPound, PrAppKeyCode_Backslash, PrAppKey_Unknown},
    {UIKeyboardHIDUsageKeyboardSemicolon, PrAppKeyCode_Semicolon, PrAppKey_Unknown},
    {UIKeyboardHIDUsageKeyboardQuote, PrAppKeyCode_Quote, PrAppKey_Unknown},
    {UIKeyboardHIDUsageKeyboardGraveAccentAndTilde, PrAppKeyCode_Backquote, PrAppKey_Unknown},
    {UIKeyboardHIDUsageKeyboardComma, PrAppKeyCode_Comma, PrAppKey_Unknown},
    {UIKeyboardHIDUsageKeyboardPeriod, PrAppKeyCode_Period, PrAppKey_Unknown},
    {UIKeyboardHIDUsageKeyboardSlash, PrAppKeyCode_Slash, PrAppKey_Unknown},
    {UIKeyboardHIDUsageKeyboardCapsLock, PrAppKeyCode_CapsLock, PrAppKey_CapsLock},
    {UIKeyboardHIDUsageKeyboardPrintScreen, PrAppKeyCode_PrintScreen, PrAppKey_Unknown},
    {UIKeyboardHIDUsageKeyboardScrollLock, PrAppKeyCode_ScrollLock, PrAppKey_Unknown},
    {UIKeyboardHIDUsageKeyboardPause, PrAppKeyCode_Pause, PrAppKey_Unknown},
    {UIKeyboardHIDUsageKeyboardInsert, PrAppKeyCode_Insert, PrAppKey_Insert},
    {UIKeyboardHIDUsageKeyboardHome, PrAppKeyCode_Home, PrAppKey_Home},
    {UIKeyboardHIDUsageKeyboardPageUp, PrAppKeyCode_PageUp, PrAppKey_PageUp},
    {UIKeyboardHIDUsageKeyboardDeleteForward, PrAppKeyCode_Delete, PrAppKey_Delete},
    {UIKeyboardHIDUsageKeyboardEnd, PrAppKeyCode_End, PrAppKey_End},
    {UIKeyboardHIDUsageKeyboardPageDown, PrAppKeyCode_PageDown, PrAppKey_PageDown},
    {UIKeyboardHIDUsageKeyboardRightArrow, PrAppKeyCode_ArrowRight, PrAppKey_Right},
    {UIKeyboardHIDUsageKeyboardLeftArrow, PrAppKeyCode_ArrowLeft, PrAppKey_Left},
    {UIKeyboardHIDUsageKeyboardDownArrow, PrAppKeyCode_ArrowDown, PrAppKey_Down},
    {UIKeyboardHIDUsageKeyboardUpArrow, PrAppKeyCode_ArrowUp, PrAppKey_Up},
    {UIKeyboardHIDUsageKeypadNumLock, PrAppKeyCode_NumLock, PrAppKey_NumLock},
    {UIKeyboardHIDUsageKeypadSlash, PrAppKeyCode_NumpadDivide, PrAppKey_Unknown},
    {UIKeyboardHIDUsageKeypadAsterisk, PrAppKeyCode_NumpadMultiply, PrAppKey_Unknown},
    {UIKeyboardHIDUsageKeypadHyphen, PrAppKeyCode_NumpadSubtract, PrAppKey_Unknown},
    {UIKeyboardHIDUsageKeypadPlus, PrAppKeyCode_NumpadAdd, PrAppKey_Unknown},
    {UIKeyboardHIDUsageKeypadEnter, PrAppKeyCode_NumpadEnter, PrAppKey_Enter},
    {UIKeyboardHIDUsageKeypadPeriod, PrAppKeyCode_NumpadDecimal, PrAppKey_Unknown},
    {UIKeyboardHIDUsageKeypadEqualSign, PrAppKeyCode_NumpadEqual, PrAppKey_Unknown},
    {UIKeyboardHIDUsageKeypadComma, PrAppKeyCode_NumpadComma, PrAppKey_Unknown},
    {UIKeyboardHIDUsageKeyboardNonUSBackslash, PrAppKeyCode_IntlBackslash, PrAppKey_Unknown},
    {UIKeyboardHIDUsageKeyboardApplication, PrAppKeyCode_ContextMenu, PrAppKey_ContextMenu},
    {UIKeyboardHIDUsageKeyboardHelp, PrAppKeyCode_Help, PrAppKey_Help},
    {UIKeyboardHIDUsageKeyboardInternational1, PrAppKeyCode_IntlRo, PrAppKey_Unknown},
    {UIKeyboardHIDUsageKeyboardInternational3, PrAppKeyCode_IntlYen, PrAppKey_Unknown},
    {UIKeyboardHIDUsageKeyboardLANG1, PrAppKeyCode_Lang1, PrAppKey_Unknown},
    {UIKeyboardHIDUsageKeyboardLANG2, PrAppKeyCode_Lang2, PrAppKey_Unknown},
    {UIKeyboardHIDUsageKeyboardLeftControl, PrAppKeyCode_ControlLeft, PrAppKey_Control},
    {UIKeyboardHIDUsageKeyboardLeftShift, PrAppKeyCode_ShiftLeft, PrAppKey_Shift},
    {UIKeyboardHIDUsageKeyboardLeftAlt, PrAppKeyCode_AltLeft, PrAppKey_Alt},
    {UIKeyboardHIDUsageKeyboardLeftGUI, PrAppKeyCode_MetaLeft, PrAppKey_Meta},
    {UIKeyboardHIDUsageKeyboardRightControl, PrAppKeyCode_ControlRight, PrAppKey_Control},
    {UIKeyboardHIDUsageKeyboardRightShift, PrAppKeyCode_ShiftRight, PrAppKey_Shift},
    {UIKeyboardHIDUsageKeyboardRightAlt, PrAppKeyCode_AltRight, PrAppKey_Alt},
    {UIKeyboardHIDUsageKeyboardRightGUI, PrAppKeyCode_MetaRight, PrAppKey_Meta},
};

// Returns the key at a HID usage: its position, and what it means if it types no character. The letters, digits,
// function keys and keypad digits are runs of usages.
HidKey hidKeyOf(UIKeyboardHIDUsage usage) {
    if (usage >= UIKeyboardHIDUsageKeyboardA && usage <= UIKeyboardHIDUsageKeyboardZ)
        return {usage, PrAppKeyCode(PrAppKeyCode_KeyA + (usage - UIKeyboardHIDUsageKeyboardA)), PrAppKey_Unknown};
    if (usage >= UIKeyboardHIDUsageKeyboard1 && usage <= UIKeyboardHIDUsageKeyboard9)
        return {usage, PrAppKeyCode(PrAppKeyCode_Digit1 + (usage - UIKeyboardHIDUsageKeyboard1)), PrAppKey_Unknown};
    if (usage == UIKeyboardHIDUsageKeyboard0)
        return {usage, PrAppKeyCode_Digit0, PrAppKey_Unknown};
    if (usage >= UIKeyboardHIDUsageKeyboardF1 && usage <= UIKeyboardHIDUsageKeyboardF12)
        return {
            usage, PrAppKeyCode(PrAppKeyCode_F1 + (usage - UIKeyboardHIDUsageKeyboardF1)),
            PrAppKey(PrAppKey_F1 + (usage - UIKeyboardHIDUsageKeyboardF1)),
        };
    if (usage >= UIKeyboardHIDUsageKeyboardF13 && usage <= UIKeyboardHIDUsageKeyboardF20)
        return {
            usage, PrAppKeyCode(PrAppKeyCode_F13 + (usage - UIKeyboardHIDUsageKeyboardF13)),
            PrAppKey(PrAppKey_F13 + (usage - UIKeyboardHIDUsageKeyboardF13)),
        };
    if (usage >= UIKeyboardHIDUsageKeypad1 && usage <= UIKeyboardHIDUsageKeypad9)
        return {usage, PrAppKeyCode(PrAppKeyCode_Numpad1 + (usage - UIKeyboardHIDUsageKeypad1)), PrAppKey_Unknown};
    if (usage == UIKeyboardHIDUsageKeypad0)
        return {usage, PrAppKeyCode_Numpad0, PrAppKey_Unknown};
    for (const HidKey& key : kHidKeys) {
        if (key.usage == usage)
            return key;
    }
    return {usage, PrAppKeyCode_Unidentified, PrAppKey_Unknown};
}

// Returns true if `text` holds a character that a key types, rather than a control character.
bool isPrintable(NSString* PR_NULLABLE text) {
    if (text.length == 0)
        return false;
    unichar first = [text characterAtIndex:0];
    return first >= 0x20 && first != 0x7f && !(first >= 0xf700 && first <= 0xf8ff);
}

// Returns the character that `key` types with Shift and Option applied and Control and Command ignored, or nil.
NSString* PR_NULLABLE characterOf(UIKey* key) {
    bool ignored = (key.modifierFlags & (UIKeyModifierControl | UIKeyModifierCommand)) != 0;
    NSString* text = ignored ? key.charactersIgnoringModifiers : key.characters;
    return isPrintable(text) ? text : nil;
}

// Returns what a key means: its named key, or else the ASCII character that it types without modifiers, uppercase.
PrAppKey keyOf(UIKey* key, const HidKey& hid) {
    if (hid.key != PrAppKey_Unknown)
        return hid.key;
    NSString* plain = key.charactersIgnoringModifiers;
    if (!isPrintable(plain))
        return PrAppKey_Unknown;
    unichar first = [plain characterAtIndex:0];
    if (first >= 'a' && first <= 'z')
        return PrAppKey(first - 'a' + 'A');
    return first < 128 ? PrAppKey(first) : PrAppKey_Unknown;
}

// ---------------------------------------------------------------------------------------------------------------------
// Gestures and drops

PrAppGesturePhase phaseOf(UIGestureRecognizerState state) {
    switch (state) {
        case UIGestureRecognizerStateBegan: return PrAppGesturePhase_Begin;
        case UIGestureRecognizerStateChanged: return PrAppGesturePhase_Change;
        case UIGestureRecognizerStateEnded: return PrAppGesturePhase_End;
        default: return PrAppGesturePhase_Cancel;
    }
}

// Returns the forms of content that the items of a drop carry: text, URLs, and files, which any other content is.
PrAppDataFormFlags formsOf(id<UIDropSession> session) {
    PrAppDataFormFlags forms = 0;
    for (UIDragItem* item in session.items) {
        NSItemProvider* provider = item.itemProvider;
        if ([provider hasItemConformingToTypeIdentifier:UTTypePlainText.identifier])
            forms |= PrAppDataFormFlag_Text;
        else if ([provider canLoadObjectOfClass:NSURL.class])
            forms |= PrAppDataFormFlag_Urls;
        else if (provider.registeredTypeIdentifiers.count != 0)
            forms |= PrAppDataFormFlag_Files;
    }
    return forms;
}

// The operations that a drop's source allows. A drop always copies, and moves if its source allows it.
PrAppDragOperationFlags operationsOf(id<UIDropSession> session) {
    return PrAppDragOperationFlag_Copy | (session.allowsMoveOperation ? PrAppDragOperationFlag_Move : 0);
}

UIDropOperation dropOperationOf(PrAppDragOperation operation) {
    switch (operation) {
        case PrAppDragOperation_Copy: return UIDropOperationCopy;
        case PrAppDragOperation_Move: return UIDropOperationMove;
        default: return UIDropOperationForbidden;
    }
}

// Returns a path in a new directory of its own under the temporary directory for a dropped file named `name`.
NSString* PR_NULLABLE dropPathFor(NSString* name) {
    NSString* folder = [NSTemporaryDirectory() stringByAppendingPathComponent:NSUUID.UUID.UUIDString];
    if (![NSFileManager.defaultManager createDirectoryAtPath:folder withIntermediateDirectories:YES attributes:nil
            error:nil])
        return nil;
    return [folder stringByAppendingPathComponent:name.lastPathComponent.length != 0 ? name.lastPathComponent : @"file"];
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

@implementation PrUiKitTextPosition
+ (instancetype)at:(NSUInteger)offset {
    PrUiKitTextPosition* position = [[PrUiKitTextPosition alloc] init];
    position.offset = offset;
    return position;
}
@end

@implementation PrUiKitTextRange
+ (instancetype)of:(NSRange)range {
    PrUiKitTextRange* made = [[PrUiKitTextRange alloc] init];
    made.range = range;
    return made;
}
- (UITextPosition*)start {
    return [PrUiKitTextPosition at:self.range.location];
}
- (UITextPosition*)end {
    return [PrUiKitTextPosition at:NSMaxRange(self.range)];
}
- (BOOL)isEmpty {
    return self.range.length == 0;
}
@end

@implementation PrUiKitView {
    // The touches that are down, by the UITouch, each with its pointer id, and the id of the primary touch.
    NSMapTable<UITouch*, NSNumber*>* _touches;
    uint32_t _nextTouch;
    uint32_t _primary;
    BOOL _hovering;                     // the mouse pointer is over the view
    CGPoint _scrolled;                  // the translation of the scroll that is under way, as last reported
    CGFloat _scale;                     // the scale of the pinch that is under way, as last reported
    CGFloat _rotation;                  // the rotation of the rotation that is under way, as last reported, in radians
    UIView* _noKeyboard;                // the empty input view that keeps the keyboard down
    UIPointerInteraction* _pointer;

    // The text that an input method composes, or nil, and the selection within it.
    NSString* _marked;
    NSRange _markedSelection;
    UITextInputStringTokenizer* _tokenizer;

    // The keys that are down, by their position, so that the view releases them when it stops being the responder, and
    // so that the text that their press inserts does not repeat them.
    PrAppKey _keys[PrAppKeyCode_Count];
    bool _down[PrAppKeyCode_Count];
}
@synthesize inputDelegate = _inputDelegate;
@synthesize markedTextStyle = _markedTextStyle;
+ (Class)layerClass {
    return CAMetalLayer.class;
}
- (instancetype)initWithFrame:(CGRect)frame {
    self = [super initWithFrame:frame];
    if (self != nil) {
        _touches = [NSMapTable weakToStrongObjectsMapTable];
        _noKeyboard = [[UIView alloc] initWithFrame:CGRectZero];
        self.multipleTouchEnabled = YES;
        self.backgroundColor = UIColor.blackColor;
        self.autocorrectionType = UITextAutocorrectionTypeNo;
        self.autocapitalizationType = UITextAutocapitalizationTypeNone;
        self.spellCheckingType = UITextSpellCheckingTypeNo;
        self.caret = CGRectNull;
        UIHoverGestureRecognizer* hover =
            [[UIHoverGestureRecognizer alloc] initWithTarget:self action:@selector(hovered:)];
        [self addGestureRecognizer:hover];
        // The scrolls of a trackpad or a mouse's wheel, which no touch makes.
        UIPanGestureRecognizer* scroll =
            [[UIPanGestureRecognizer alloc] initWithTarget:self action:@selector(scrolled:)];
        scroll.allowedScrollTypesMask = UIScrollTypeMaskAll;
        scroll.allowedTouchTypes = @[];
        [self addGestureRecognizer:scroll];
        // The pinch and rotation of two fingers, or of a trackpad, which leave the touches to the pointer events.
        UIPinchGestureRecognizer* pinch =
            [[UIPinchGestureRecognizer alloc] initWithTarget:self action:@selector(pinched:)];
        UIRotationGestureRecognizer* rotation =
            [[UIRotationGestureRecognizer alloc] initWithTarget:self action:@selector(rotated:)];
        for (UIGestureRecognizer* recognizer in @[ pinch, rotation ]) {
            recognizer.delegate = self;
            recognizer.cancelsTouchesInView = NO;
            recognizer.delaysTouchesEnded = NO;
            [self addGestureRecognizer:recognizer];
        }
        _pointer = [[UIPointerInteraction alloc] initWithDelegate:self];
        [self addInteraction:_pointer];
        [self addInteraction:[[UIDropInteraction alloc] initWithDelegate:self]];
        __weak PrUiKitView* weak = self;
        NSArray<UITrait>* traits = @[
            UITraitUserInterfaceStyle.class, UITraitDisplayScale.class, UITraitDisplayGamut.class,
            UITraitAccessibilityContrast.class, UITraitPreferredContentSizeCategory.class,];
        [self registerForTraitChanges:traits withHandler:^(id<UITraitEnvironment>, UITraitCollection*) {
            PrUiKitView* view = weak;
            [view takeScale];
            [view.owner requestFrame];
        }];
    }
    return self;
}
// Takes the scale of the display that shows the view, which the layer's drawables are sized in.
- (void)takeScale {
    CGFloat scale = self.traitCollection.displayScale;
    if (self.window != nil && scale > 0 && self.contentScaleFactor != scale)
        self.contentScaleFactor = scale;
}
- (void)didMoveToWindow {
    [super didMoveToWindow];
    [self takeScale];
    if (self.window != nil && self.owner.shown)
        [self becomeFirstResponder];
}
- (void)layoutSubviews {
    [super layoutSubviews];
    [self.owner layoutChanged];
}
- (void)safeAreaInsetsDidChange {
    [super safeAreaInsetsDidChange];
    [self.owner requestFrame];
}
- (BOOL)canBecomeFirstResponder {
    return YES;
}
- (UIView*)inputView {
    return self.editing ? nil : _noKeyboard;
}
- (void)setEditing:(BOOL)editing kind:(PrAppTextInputKind)kind {
    if (editing == self.editing && kind == self.inputKind)
        return;
    // A composition ends with the text that it edits, as the user canceled it.
    if (!editing && _marked != nil)
        [self endComposition:@""];
    self.editing = editing;
    self.inputKind = kind;
    switch (kind) {
        case PrAppTextInputKind_Number: self.keyboardType = UIKeyboardTypeNumberPad; break;
        case PrAppTextInputKind_Decimal: self.keyboardType = UIKeyboardTypeDecimalPad; break;
        case PrAppTextInputKind_Phone: self.keyboardType = UIKeyboardTypePhonePad; break;
        case PrAppTextInputKind_Email: self.keyboardType = UIKeyboardTypeEmailAddress; break;
        case PrAppTextInputKind_Url: self.keyboardType = UIKeyboardTypeURL; break;
        case PrAppTextInputKind_Search: self.keyboardType = UIKeyboardTypeWebSearch; break;
        default: self.keyboardType = UIKeyboardTypeDefault; break;
    }
    self.returnKeyType = kind == PrAppTextInputKind_Search ? UIReturnKeySearch : UIReturnKeyDefault;
    self.secureTextEntry = kind == PrAppTextInputKind_Password;
    if (self.isFirstResponder)
        [self reloadInputViews];
    else if (self.window != nil)
        [self becomeFirstResponder];
}
- (void)deliver:(const PrAppEvent&)event {
    [self.owner deliver:event];
}

// --- Pointers ---

// Returns the pointer event of `kind` for `touch`, with the modifiers of `source`.
- (PrAppEvent)pointer:(UITouch*)touch kind:(PrAppEventKind)kind event:(UIEvent* PR_NULLABLE)source {
    CGPoint at = [touch locationInView:self];
    PrAppEvent event{};
    event.kind = kind;
    event.time = touch.timestamp;
    event.modifiers = uiKitModifiersOf(source != nil ? source.modifierFlags : UIKeyModifierFlags(0));
    event.pointer.position = PrVec2{float(at.x), float(at.y)};
    bool down = kind == PrAppEventKind_PointerDown || kind == PrAppEventKind_PointerMove;
    if (touch.type == UITouchTypeIndirectPointer) {
        event.pointer.type = PrAppPointerType_Mouse;
        event.pointer.id = kPrAppPointerMouse;
        event.pointer.primary = true;
        event.pointer.button = source != nil && (source.buttonMask & UIEventButtonMaskSecondary) != 0 ? 1 : 0;
        event.pointer.pressure = down ? 0.5f : 0.0f;
    } else {
        NSNumber* number = [_touches objectForKey:touch];
        event.pointer.id = number != nil ? number.unsignedIntValue : 0;
        event.pointer.primary = event.pointer.id == _primary;
        event.pointer.type = touch.type == UITouchTypePencil ? PrAppPointerType_Pen : PrAppPointerType_Touch;
        if (touch.maximumPossibleForce > 0 && touch.force > 0)
            event.pointer.pressure = float(touch.force / touch.maximumPossibleForce);
        else
            event.pointer.pressure = down ? 0.5f : 0.0f;
        if (kind == PrAppEventKind_PointerUp || kind == PrAppEventKind_PointerCancel)
            event.pointer.pressure = 0;
        float diameter = float(touch.majorRadius * 2);
        event.pointer.contactSize = PrVec2{diameter, diameter};
        // A pen's tilt from its altitude above the surface and its azimuth (pointerevents3/#converting-between-tiltx-
        // tilty-and-altitudeangle-azimuthangle).
        if (touch.type == UITouchTypePencil && touch.altitudeAngle < M_PI_2) {
            double azimuth = [touch azimuthAngleInView:self];
            double tangent = tan(touch.altitudeAngle);
            event.pointer.tilt = PrVec2{
                float(atan(cos(azimuth) / tangent) * 180 / M_PI),
                float(atan(sin(azimuth) / tangent) * 180 / M_PI),
            };
        }
    }
    if (kind == PrAppEventKind_PointerDown)
        event.pointer.clicks = uint32_t(touch.tapCount);
    if (kind == PrAppEventKind_PointerMove) {
        CGPoint before = [touch previousLocationInView:self];
        event.pointer.movement = PrVec2{float(at.x - before.x), float(at.y - before.y)};
    }
    return event;
}
// Delivers a pointer event, and the PointerEnter or PointerLeave of a touch, which is over the view only while down.
- (void)deliver:(UITouch*)touch kind:(PrAppEventKind)kind event:(UIEvent*)source {
    PrAppEvent event = [self pointer:touch kind:kind event:source];
    bool mouse = touch.type == UITouchTypeIndirectPointer;
    if (kind == PrAppEventKind_PointerDown && (!mouse || !_hovering)) {
        PrAppEvent enter = event;
        enter.kind = PrAppEventKind_PointerEnter;
        enter.pointer.clicks = 0;
        [self deliver:enter];
        _hovering = _hovering || mouse;
    }
    [self deliver:event];
    if (!mouse && (kind == PrAppEventKind_PointerUp || kind == PrAppEventKind_PointerCancel)) {
        PrAppEvent leave = event;
        leave.kind = PrAppEventKind_PointerLeave;
        [self deliver:leave];
    }
}
- (void)touchesBegan:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    for (UITouch* touch in touches) {
        if (touch.type != UITouchTypeIndirectPointer) {
            uint32_t pointer = kTouchPointerBase + _nextTouch++;
            if (_touches.count == 0)
                _primary = pointer;
            [_touches setObject:@(pointer) forKey:touch];
        }
        [self deliver:touch kind:PrAppEventKind_PointerDown event:event];
    }
}
- (void)touchesMoved:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    for (UITouch* touch in touches)
        [self deliver:touch kind:PrAppEventKind_PointerMove event:event];
}
- (void)endTouches:(NSSet<UITouch*>*)touches kind:(PrAppEventKind)kind event:(UIEvent*)event {
    for (UITouch* touch in touches) {
        [self deliver:touch kind:kind event:event];
        [_touches removeObjectForKey:touch];
    }
    if (_touches.count == 0)
        _primary = 0;
}
- (void)touchesEnded:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    [self endTouches:touches kind:PrAppEventKind_PointerUp event:event];
}
- (void)touchesCancelled:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    [self endTouches:touches kind:PrAppEventKind_PointerCancel event:event];
}
// The mouse pointer's moves while no button is down: its entry, its moves and its exit.
- (void)hovered:(UIHoverGestureRecognizer*)recognizer {
    CGPoint at = [recognizer locationInView:self];
    PrAppEvent event{};
    event.time = uiKitUptime();
    event.modifiers = uiKitModifiersOf(recognizer.modifierFlags);
    event.pointer.position = PrVec2{float(at.x), float(at.y)};
    event.pointer.type = PrAppPointerType_Mouse;
    event.pointer.id = kPrAppPointerMouse;
    event.pointer.primary = true;
    UIGestureRecognizerState state = recognizer.state;
    if (state == UIGestureRecognizerStateBegan && !_hovering) {
        _hovering = YES;
        event.kind = PrAppEventKind_PointerEnter;
        [self deliver:event];
    }
    if (state == UIGestureRecognizerStateBegan || state == UIGestureRecognizerStateChanged) {
        event.kind = PrAppEventKind_PointerMove;
        [self deliver:event];
    } else if (_hovering) {
        _hovering = NO;
        event.kind = PrAppEventKind_PointerLeave;
        [self deliver:event];
    }
}
// A scroll moves the content with the fingers, and its delta is the distance that the view scrolls, the reverse.
- (void)scrolled:(UIPanGestureRecognizer*)recognizer {
    if (recognizer.state == UIGestureRecognizerStateBegan)
        _scrolled = CGPointZero;
    CGPoint translation = [recognizer translationInView:self];
    CGPoint at = [recognizer locationInView:self];
    PrAppEvent event{};
    event.kind = PrAppEventKind_Scroll;
    event.time = uiKitUptime();
    event.modifiers = uiKitModifiersOf(recognizer.modifierFlags);
    event.scroll.position = PrVec2{float(at.x), float(at.y)};
    event.scroll.delta = PrVec2{float(_scrolled.x - translation.x), float(_scrolled.y - translation.y)};
    event.scroll.unit = PrAppScrollUnit_Pixel;
    _scrolled = translation;
    if (event.scroll.delta.x != 0 || event.scroll.delta.y != 0)
        [self deliver:event];
}

// --- Gestures ---

// The gestures run beside the touches' pointer events and beside each other.
- (BOOL)gestureRecognizer:(UIGestureRecognizer*)recognizer
        shouldRecognizeSimultaneouslyWithGestureRecognizer:(UIGestureRecognizer*)other {
    (void)recognizer;
    (void)other;
    return YES;
}
- (PrAppEvent)gesture:(UIGestureRecognizer*)recognizer kind:(PrAppGestureKind)kind {
    CGPoint at = [recognizer locationInView:self];
    PrAppEvent event{};
    event.kind = PrAppEventKind_Gesture;
    event.time = uiKitUptime();
    event.modifiers = uiKitModifiersOf(recognizer.modifierFlags);
    event.gesture.kind = kind;
    event.gesture.phase = phaseOf(recognizer.state);
    event.gesture.position = PrVec2{float(at.x), float(at.y)};
    return event;
}
- (void)pinched:(UIPinchGestureRecognizer*)recognizer {
    if (recognizer.state == UIGestureRecognizerStateBegan)
        _scale = 1;
    PrAppEvent event = [self gesture:recognizer kind:PrAppGestureKind_Magnify];
    if (_scale > 0)
        event.gesture.magnification = float(recognizer.scale / _scale - 1);
    _scale = recognizer.scale;
    [self deliver:event];
}
// UIKit's rotation is clockwise on the screen, as the event's is.
- (void)rotated:(UIRotationGestureRecognizer*)recognizer {
    if (recognizer.state == UIGestureRecognizerStateBegan)
        _rotation = 0;
    PrAppEvent event = [self gesture:recognizer kind:PrAppGestureKind_Rotate];
    event.gesture.rotation = float((recognizer.rotation - _rotation) * 180 / M_PI);
    _rotation = recognizer.rotation;
    [self deliver:event];
}

// --- Cursors ---

- (void)setCursor:(PrAppCursor)cursor {
    if (cursor == _cursor)
        return;
    _cursor = cursor;
    [_pointer invalidate];
}
- (UIPointerRegion*)pointerInteraction:(UIPointerInteraction*)interaction
        regionForRequest:(UIPointerRegionRequest*)request defaultRegion:(UIPointerRegion*)region {
    (void)interaction;
    (void)request;
    return region;
}
// The system's pointer has no shapes for most cursors. The text cursors show as a beam the height of the caret, and
// None hides the pointer.
- (UIPointerStyle*)pointerInteraction:(UIPointerInteraction*)interaction styleForRegion:(UIPointerRegion*)region {
    (void)interaction;
    (void)region;
    CGFloat length = !CGRectIsNull(self.caret) && self.caret.size.height > 0 ? self.caret.size.height : 20;
    switch (self.cursor) {
        case PrAppCursor_None: return [UIPointerStyle hiddenPointerStyle];
        case PrAppCursor_Text:
            return [UIPointerStyle styleWithShape:[UIPointerShape beamWithPreferredLength:length axis:UIAxisVertical]
                constrainedAxes:UIAxis(0)];
        case PrAppCursor_VerticalText:
            return [UIPointerStyle styleWithShape:[UIPointerShape beamWithPreferredLength:length axis:UIAxisHorizontal]
                constrainedAxes:UIAxis(0)];
        default: return nil;
    }
}

// --- Drops ---

- (PrAppEvent)drop:(id<UIDropSession>)session kind:(PrAppEventKind)kind {
    CGPoint at = [session locationInView:self];
    PrAppEvent event{};
    event.kind = kind;
    event.time = uiKitUptime();
    event.drag.position = PrVec2{float(at.x), float(at.y)};
    event.drag.forms = formsOf(session);
    event.drag.operations = operationsOf(session);
    return event;
}
- (BOOL)dropInteraction:(UIDropInteraction*)interaction canHandleSession:(id<UIDropSession>)session {
    (void)interaction;
    (void)session;
    return YES;
}
- (void)dropInteraction:(UIDropInteraction*)interaction sessionDidEnter:(id<UIDropSession>)session {
    (void)interaction;
    [self deliver:[self drop:session kind:PrAppEventKind_DragEnter]];
}
// Answers with the operation of the window's latest output, as it stands when the drag moves.
- (UIDropProposal*)dropInteraction:(UIDropInteraction*)interaction sessionDidUpdate:(id<UIDropSession>)session {
    (void)interaction;
    [self deliver:[self drop:session kind:PrAppEventKind_DragOver]];
    PrAppDragOperation operation = appDragAnswer(self.owner.dropOperation, operationsOf(session));
    return [[UIDropProposal alloc] initWithDropOperation:dropOperationOf(operation)];
}
- (void)dropInteraction:(UIDropInteraction*)interaction sessionDidExit:(id<UIDropSession>)session {
    (void)interaction;
    (void)session;
    PrAppEvent event{};
    event.kind = PrAppEventKind_DragLeave;
    event.time = uiKitUptime();
    [self deliver:event];
}
// Loads every item of the drop, and then delivers the Drop with a source of what loaded: the text, the URLs, and the
// paths of copies of the files, which UIKit removes once its loading ends.
- (void)dropInteraction:(UIDropInteraction*)interaction performDrop:(id<UIDropSession>)session {
    (void)interaction;
    PrAppEvent event = [self drop:session kind:PrAppEventKind_Drop];
    event.drag.operation = appDragAnswer(self.owner.dropOperation, event.drag.operations);
    NSMutableArray<NSString*>* texts = [NSMutableArray array];
    NSMutableArray<NSString*>* urls = [NSMutableArray array];
    NSMutableArray<NSString*>* paths = [NSMutableArray array];
    dispatch_group_t group = dispatch_group_create();
    for (UIDragItem* item in session.items) {
        NSItemProvider* provider = item.itemProvider;
        dispatch_group_enter(group);
        if ([provider hasItemConformingToTypeIdentifier:UTTypePlainText.identifier]) {
            [provider loadObjectOfClass:NSString.class completionHandler:^(id<NSItemProviderReading> text, NSError*) {
                dispatch_async(dispatch_get_main_queue(), ^{
                    if ([(id)text isKindOfClass:NSString.class])
                        [texts addObject:(NSString*)text];
                    dispatch_group_leave(group);
                });
            }];
        } else if ([provider canLoadObjectOfClass:NSURL.class]) {
            [provider loadObjectOfClass:NSURL.class completionHandler:^(id<NSItemProviderReading> url, NSError*) {
                dispatch_async(dispatch_get_main_queue(), ^{
                    NSURL* address = [(id)url isKindOfClass:NSURL.class] ? (NSURL*)url : nil;
                    if (address.isFileURL)
                        [paths addObject:address.path];
                    else if (address != nil)
                        [urls addObject:address.absoluteString];
                    dispatch_group_leave(group);
                });
            }];
        } else if (NSString* type = provider.registeredTypeIdentifiers.firstObject) {
            NSString* name = provider.suggestedName;
            [provider loadFileRepresentationForTypeIdentifier:type completionHandler:^(NSURL* url, NSError*) {
                NSString* path = url != nil ? dropPathFor(name != nil ? name : url.lastPathComponent) : nil;
                if (path != nil && ![NSFileManager.defaultManager copyItemAtURL:url
                        toURL:[NSURL fileURLWithPath:path] error:nil])
                    path = nil;
                dispatch_async(dispatch_get_main_queue(), ^{
                    if (path != nil)
                        [paths addObject:path];
                    dispatch_group_leave(group);
                });
            }];
        } else {
            dispatch_group_leave(group);
        }
    }
    __weak PrUiKitView* weak = self;
    dispatch_group_notify(group, dispatch_get_main_queue(), ^{
        PrUiKitView* view = weak;
        PrUiKitHost* host = view.owner.host;
        if (host.app == nullptr)
            return;
        AppDataStore* store = allocZeroed<AppDataStore>();
        if (texts.count != 0) {
            PrRawString text = uiKitTextOf([texts componentsJoinedByString:@"\n"]);
            appDataStoreSetText(*store, prRawStringView(&text));
            prRawStringReset(&text);
        }
        for (NSString* url in urls) {
            PrRawString text = uiKitTextOf(url);
            appDataStoreAddUrl(*store, prRawStringView(&text));
            prRawStringReset(&text);
        }
        for (NSString* path in paths) {
            PrRawString text = uiKitTextOf(path);
            appDataStoreAddPath(*store, prRawStringView(&text));
            prRawStringReset(&text);
        }
        PrAppEvent dropped = event;
        dropped.drag.forms = store->forms;
        dropped.drag.drop = appDataSourceCreate(static_cast<UiKitApp*>(host.app)->sources, store, nullptr);
        [view deliver:dropped];
    });
}

// --- Keys ---

- (void)pressesBegan:(NSSet<UIPress*>*)presses withEvent:(UIPressesEvent*)event {
    for (UIPress* press in presses) {
        UIKey* key = press.key;
        if (key == nil)
            continue;
        HidKey hid = hidKeyOf(key.keyCode);
        NSString* character = characterOf(key);
        PrAppEvent down{};
        down.kind = PrAppEventKind_KeyDown;
        down.time = press.timestamp;
        down.modifiers = uiKitModifiersOf(key.modifierFlags);
        down.keyboard.key = keyOf(key, hid);
        down.keyboard.code = hid.code;
        PrRawString text = uiKitTextOf(character);
        down.keyboard.text = prRawStringView(&text);
        // A key that comes during a composition goes to the input method.
        down.keyboard.composing = _marked != nil;
        if (hid.code != PrAppKeyCode_Unidentified) {
            _keys[hid.code] = down.keyboard.key;
            _down[hid.code] = true;
        }
        [self deliver:down];
        prRawStringReset(&text);
    }
    // The text system takes the presses that type, and inserts their text (insertText:).
    [super pressesBegan:presses withEvent:event];
}
- (void)endPresses:(NSSet<UIPress*>*)presses {
    for (UIPress* press in presses) {
        UIKey* key = press.key;
        if (key == nil)
            continue;
        HidKey hid = hidKeyOf(key.keyCode);
        if (hid.code != PrAppKeyCode_Unidentified)
            _down[hid.code] = false;
        PrAppEvent up{};
        up.kind = PrAppEventKind_KeyUp;
        up.time = press.timestamp;
        up.modifiers = uiKitModifiersOf(key.modifierFlags);
        up.keyboard.key = keyOf(key, hid);
        up.keyboard.code = hid.code;
        up.keyboard.text = "";
        [self deliver:up];
    }
}
- (void)pressesEnded:(NSSet<UIPress*>*)presses withEvent:(UIPressesEvent*)event {
    [self endPresses:presses];
    [super pressesEnded:presses withEvent:event];
}
- (void)pressesCancelled:(NSSet<UIPress*>*)presses withEvent:(UIPressesEvent*)event {
    [self endPresses:presses];
    [super pressesCancelled:presses withEvent:event];
}
// Releases the keys that are down, for a view that stopped being the first responder.
- (BOOL)resignFirstResponder {
    BOOL resigned = [super resignFirstResponder];
    if (resigned) {
        for (uint32_t code = 0; code < countOf(_down); ++code) {
            if (!_down[code])
                continue;
            _down[code] = false;
            PrAppEvent up{};
            up.kind = PrAppEventKind_KeyUp;
            up.time = uiKitUptime();
            up.keyboard.key = _keys[code];
            up.keyboard.code = PrAppKeyCode(code);
            up.keyboard.text = "";
            [self deliver:up];
        }
    }
    return resigned;
}
// Delivers the press and release of a key that the on-screen keyboard typed, unless a hardware key that is down already
// reported it.
- (void)typed:(PrAppKey)key code:(PrAppKeyCode)code {
    if (_down[code])
        return;
    PrAppEvent event{};
    event.kind = PrAppEventKind_KeyDown;
    event.time = uiKitUptime();
    event.keyboard.key = key;
    event.keyboard.code = code;
    event.keyboard.text = "";
    [self deliver:event];
    event.kind = PrAppEventKind_KeyUp;
    [self deliver:event];
}

// --- Text ---

- (void)deliverText:(NSString*)text kind:(PrAppEventKind)kind {
    PrAppEvent event{};
    event.kind = kind;
    event.time = uiKitUptime();
    PrRawString utf8 = uiKitTextOf(text);
    event.input.text = prRawStringView(&utf8);
    [self deliver:event];
    prRawStringReset(&utf8);
}
// Ends the composition with `committed`, which is empty if the user canceled it, and types what it committed.
- (void)endComposition:(NSString*)committed {
    [self.inputDelegate textWillChange:self];
    _marked = nil;
    _markedSelection = NSMakeRange(0, 0);
    [self.inputDelegate textDidChange:self];
    [self deliverText:committed kind:PrAppEventKind_CompositionEnd];
    if (committed.length != 0)
        [self deliverText:committed kind:PrAppEventKind_Text];
}
- (BOOL)hasText {
    return YES;
}
- (void)insertText:(NSString*)text {
    if (_marked != nil) {
        [self endComposition:text];
        return;
    }
    if ([text isEqualToString:@"\n"]) {
        [self typed:PrAppKey_Enter code:PrAppKeyCode_Enter];
        return;
    }
    if ([text isEqualToString:@"\t"]) {
        [self typed:PrAppKey_Tab code:PrAppKeyCode_Tab];
        return;
    }
    if (isPrintable(text))
        [self deliverText:text kind:PrAppEventKind_Text];
}
- (void)deleteBackward {
    [self typed:PrAppKey_Backspace code:PrAppKeyCode_Backspace];
}

// --- Composition (UITextInput) ---
//
// The document is the composed text alone, so that positions are offsets into it. Without a composition it is empty,
// with the caret at its start.

- (NSString*)document {
    return _marked != nil ? _marked : @"";
}
- (NSRange)clamped:(NSRange)range {
    NSUInteger length = [self document].length;
    NSUInteger start = MIN(range.location, length);
    return NSMakeRange(start, MIN(range.length, length - start));
}
- (void)setMarkedText:(NSString*)text selectedRange:(NSRange)selection {
    if (text.length == 0) {
        if (_marked != nil)
            [self endComposition:@""];
        return;
    }
    _marked = [text copy];
    _markedSelection = [self clamped:selection];
    PrAppEvent event{};
    event.kind = PrAppEventKind_CompositionUpdate;
    event.time = uiKitUptime();
    PrRawString utf8 = uiKitTextOf(_marked);
    event.input.text = prRawStringView(&utf8);
    event.input.selection = PrAppTextRange{
        uiKitUtf8Length(_marked, _markedSelection.location),
        uiKitUtf8Length(_marked, NSMaxRange(_markedSelection)),
    };
    [self deliver:event];
    prRawStringReset(&utf8);
}
- (void)unmarkText {
    if (_marked != nil)
        [self endComposition:_marked];
}
- (UITextRange*)markedTextRange {
    return _marked != nil ? [PrUiKitTextRange of:NSMakeRange(0, _marked.length)] : nil;
}
- (UITextRange*)selectedTextRange {
    return [PrUiKitTextRange of:_marked != nil ? _markedSelection : NSMakeRange(0, 0)];
}
- (void)setSelectedTextRange:(UITextRange*)range {
    if (_marked != nil && [range isKindOfClass:PrUiKitTextRange.class])
        _markedSelection = [self clamped:((PrUiKitTextRange*)range).range];
}
- (NSString*)textInRange:(UITextRange*)range {
    if (![range isKindOfClass:PrUiKitTextRange.class])
        return @"";
    return [[self document] substringWithRange:[self clamped:((PrUiKitTextRange*)range).range]];
}
- (void)replaceRange:(UITextRange*)range withText:(NSString*)text {
    (void)range;
    [self insertText:text];
}
- (UITextPosition*)beginningOfDocument {
    return [PrUiKitTextPosition at:0];
}
- (UITextPosition*)endOfDocument {
    return [PrUiKitTextPosition at:[self document].length];
}
- (UITextRange*)textRangeFromPosition:(UITextPosition*)from toPosition:(UITextPosition*)to {
    NSUInteger a = ((PrUiKitTextPosition*)from).offset;
    NSUInteger b = ((PrUiKitTextPosition*)to).offset;
    return [PrUiKitTextRange of:NSMakeRange(MIN(a, b), a > b ? a - b : b - a)];
}
- (UITextPosition*)positionFromPosition:(UITextPosition*)position offset:(NSInteger)offset {
    NSInteger moved = NSInteger(((PrUiKitTextPosition*)position).offset) + offset;
    if (moved < 0 || NSUInteger(moved) > [self document].length)
        return nil;
    return [PrUiKitTextPosition at:NSUInteger(moved)];
}
- (UITextPosition*)positionFromPosition:(UITextPosition*)position inDirection:(UITextLayoutDirection)direction
        offset:(NSInteger)offset {
    bool back = direction == UITextLayoutDirectionLeft || direction == UITextLayoutDirectionUp;
    return [self positionFromPosition:position offset:back ? -offset : offset];
}
- (NSComparisonResult)comparePosition:(UITextPosition*)position toPosition:(UITextPosition*)other {
    NSUInteger a = ((PrUiKitTextPosition*)position).offset;
    NSUInteger b = ((PrUiKitTextPosition*)other).offset;
    return a < b ? NSOrderedAscending : a > b ? NSOrderedDescending : NSOrderedSame;
}
- (NSInteger)offsetFromPosition:(UITextPosition*)from toPosition:(UITextPosition*)to {
    return NSInteger(((PrUiKitTextPosition*)to).offset) - NSInteger(((PrUiKitTextPosition*)from).offset);
}
- (id<UITextInputTokenizer>)tokenizer {
    if (_tokenizer == nil)
        _tokenizer = [[UITextInputStringTokenizer alloc] initWithTextInput:self];
    return _tokenizer;
}
- (UITextPosition*)positionWithinRange:(UITextRange*)range farthestInDirection:(UITextLayoutDirection)direction {
    bool back = direction == UITextLayoutDirectionLeft || direction == UITextLayoutDirectionUp;
    return back ? range.start : range.end;
}
- (UITextRange*)characterRangeByExtendingPosition:(UITextPosition*)position inDirection:(UITextLayoutDirection)direction {
    UITextPosition* other = [self positionFromPosition:position inDirection:direction offset:1];
    return other != nil ? [self textRangeFromPosition:position toPosition:other] : nil;
}
- (NSWritingDirection)baseWritingDirectionForPosition:(UITextPosition*)position
        inDirection:(UITextStorageDirection)direction {
    (void)position;
    (void)direction;
    return NSWritingDirectionNatural;
}
- (void)setBaseWritingDirection:(NSWritingDirection)writingDirection forRange:(UITextRange*)range {
    (void)writingDirection;
    (void)range;
}
// The input method places its windows at the caret of the window's latest output.
- (CGRect)caretRect {
    return CGRectIsNull(self.caret) ? CGRectMake(0, 0, 1, 20) : self.caret;
}
- (CGRect)firstRectForRange:(UITextRange*)range {
    (void)range;
    return [self caretRect];
}
- (CGRect)caretRectForPosition:(UITextPosition*)position {
    (void)position;
    return [self caretRect];
}
- (NSArray<UITextSelectionRect*>*)selectionRectsForRange:(UITextRange*)range {
    (void)range;
    return @[];
}
- (UITextPosition*)closestPositionToPoint:(CGPoint)point {
    (void)point;
    return [self endOfDocument];
}
- (UITextPosition*)closestPositionToPoint:(CGPoint)point withinRange:(UITextRange*)range {
    (void)point;
    return range.end;
}
- (UITextRange*)characterRangeAtPoint:(CGPoint)point {
    (void)point;
    return nil;
}
@end
