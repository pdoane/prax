// The Cocoa backend's menus: menu bars, context menus and status items.

#include "app/backends/cocoa/src/app_cocoa_menu.h"

#include "app/backends/cocoa/src/app_cocoa_data.h"
#include "app/backends/cocoa/src/app_cocoa_input.h"

namespace pr_impl {

namespace {

// Returns AppKit's key equivalent for `key`, or the empty string for none.
NSString* keyEquivalentOf(PrAppKey key) {
    if (key > ' ' && key < 127) {
        unichar character = unichar(key >= 'A' && key <= 'Z' ? key - 'A' + 'a' : key);
        return [NSString stringWithCharacters:&character length:1];
    }
    unichar character = 0;
    switch (key) {
        case PrAppKey_Enter:
            character = '\r';
            break;
        case PrAppKey_Escape:
            character = 0x1b;
            break;
        case PrAppKey_Backspace:
            character = NSBackspaceCharacter;
            break;
        case PrAppKey_Tab:
            character = '\t';
            break;
        case PrAppKey_Delete:
            character = NSDeleteFunctionKey;
            break;
        case PrAppKey_Left:
            character = NSLeftArrowFunctionKey;
            break;
        case PrAppKey_Right:
            character = NSRightArrowFunctionKey;
            break;
        case PrAppKey_Up:
            character = NSUpArrowFunctionKey;
            break;
        case PrAppKey_Down:
            character = NSDownArrowFunctionKey;
            break;
        case PrAppKey_Home:
            character = NSHomeFunctionKey;
            break;
        case PrAppKey_End:
            character = NSEndFunctionKey;
            break;
        case PrAppKey_PageUp:
            character = NSPageUpFunctionKey;
            break;
        case PrAppKey_PageDown:
            character = NSPageDownFunctionKey;
            break;
        default:
            if (key >= PrAppKey_F1 && key <= PrAppKey_F20)
                character = unichar(NSF1FunctionKey + (key - PrAppKey_F1));
            break;
    }
    return character != 0 ? [NSString stringWithCharacters:&character length:1] : @"";
}

NSEventModifierFlags keyModifiersOf(PrAppModifierFlags modifiers) {
    NSEventModifierFlags flags = 0;
    if ((modifiers & PrAppModifierFlag_Shift) != 0)
        flags |= NSEventModifierFlagShift;
    if ((modifiers & PrAppModifierFlag_Control) != 0)
        flags |= NSEventModifierFlagControl;
    if ((modifiers & PrAppModifierFlag_Alt) != 0)
        flags |= NSEventModifierFlagOption;
    if ((modifiers & PrAppModifierFlag_Super) != 0)
        flags |= NSEventModifierFlagCommand;
    return flags;
}

// What AppKit does for an item of a role: the action that the responder chain takes, and the title and the key
// equivalent of an item that the program leaves without them. A title of nil is the application's name after `title`.
struct CocoaMenuRole {
    PrAppMenuRole role;
    SEL action;
    NSString* title;
    PrAppKey key;
    PrAppModifierFlags modifiers;
};

const CocoaMenuRole* PR_NULLABLE menuRoleOf(PrAppMenuRole role) {
    static const CocoaMenuRole kRoles[] = {
        {PrAppMenuRole_About, @selector(orderFrontStandardAboutPanel:), @"About ", PrAppKey_Unknown, 0},
        {PrAppMenuRole_Services, nullptr, @"Services", PrAppKey_Unknown, 0},
        {PrAppMenuRole_Hide, @selector(hide:), @"Hide ", PrAppKey('H'), PrAppModifierFlag_Super},
        {
            PrAppMenuRole_HideOthers, @selector(hideOtherApplications:), @"Hide Others", PrAppKey('H'),
            PrAppModifierFlag_Super | PrAppModifierFlag_Alt,
        },
        {PrAppMenuRole_ShowAll, @selector(unhideAllApplications:), @"Show All", PrAppKey_Unknown, 0},
        {PrAppMenuRole_Quit, @selector(terminate:), @"Quit ", PrAppKey('Q'), PrAppModifierFlag_Super},
        {PrAppMenuRole_Undo, @selector(undo:), @"Undo", PrAppKey('Z'), PrAppModifierFlag_Super},
        {
            PrAppMenuRole_Redo, @selector(redo:), @"Redo", PrAppKey('Z'),
            PrAppModifierFlag_Super | PrAppModifierFlag_Shift,
        },
        {PrAppMenuRole_Cut, @selector(cut:), @"Cut", PrAppKey('X'), PrAppModifierFlag_Super},
        {PrAppMenuRole_Copy, @selector(copy:), @"Copy", PrAppKey('C'), PrAppModifierFlag_Super},
        {PrAppMenuRole_Paste, @selector(paste:), @"Paste", PrAppKey('V'), PrAppModifierFlag_Super},
        {PrAppMenuRole_SelectAll, @selector(selectAll:), @"Select All", PrAppKey('A'), PrAppModifierFlag_Super},
        {PrAppMenuRole_Minimize, @selector(performMiniaturize:), @"Minimize", PrAppKey('M'), PrAppModifierFlag_Super},
        {PrAppMenuRole_Zoom, @selector(performZoom:), @"Zoom", PrAppKey_Unknown, 0},
        {
            PrAppMenuRole_Fullscreen, @selector(toggleFullScreen:), @"Enter Full Screen", PrAppKey('F'),
            PrAppModifierFlag_Super | PrAppModifierFlag_Control,
        },
        {PrAppMenuRole_BringAllToFront, @selector(arrangeInFront:), @"Bring All to Front", PrAppKey_Unknown, 0},
        {PrAppMenuRole_WindowList, nullptr, @"Window", PrAppKey_Unknown, 0},
        {PrAppMenuRole_Help, nullptr, @"Help", PrAppKey_Unknown, 0},
    };
    for (const CocoaMenuRole& entry : kRoles) {
        if (entry.role == role)
            return &entry;
    }
    return nullptr;
}

// Returns the title of a role's item: the role's title, which ends in a space where the application's name follows.
NSString* roleTitleOf(const CocoaMenuRole& role) {
    if (![role.title hasSuffix:@" "])
        return role.title;
    return [role.title stringByAppendingString:NSProcessInfo.processInfo.processName];
}

NSMenu* menuOf(const PrAppMenu& description, PrCocoaMenuTree* tree, NSMutableArray<PrCocoaMenu*>* previous,
    PrCocoaHost* host, PrCocoaWindow* PR_NULLABLE owner);

// Returns the item that `description` describes in a menu of `tree`.
NSMenuItem* menuItemOf(const PrAppMenuItem& description, PrCocoaMenu* parent, PrCocoaMenuTree* tree,
        NSMutableArray<PrCocoaMenu*>* previous) {
    if (description.role == PrAppMenuRole_Separator)
        return NSMenuItem.separatorItem;
    const CocoaMenuRole* role = menuRoleOf(description.role);
    NSString* title = description.title.size != 0 ? cocoaStringOf(description.title) : @"";
    if (description.title.size == 0 && role != nullptr)
        title = roleTitleOf(*role);
    if (description.title.size == 0 && description.submenu != nullptr)
        title = cocoaStringOf(description.submenu->title);
    PrAppKey key = description.key;
    PrAppModifierFlags modifiers = description.modifiers;
    if (key == PrAppKey_Unknown && role != nullptr) {
        key = role->key;
        modifiers = role->modifiers;
    }
    // A role's action goes through the responder chain, to a text field, a window or the application.
    SEL action = role != nullptr ? role->action : @selector(chosen:);
    NSMenuItem* item = [[NSMenuItem alloc] initWithTitle:title action:action keyEquivalent:keyEquivalentOf(key)];
    item.keyEquivalentModifierMask = keyModifiersOf(modifiers);
    item.target = role != nullptr ? nil : parent;
    item.tag = NSInteger(description.tag);
    item.enabled = (description.flags & PrAppMenuItemFlag_Disabled) == 0;
    item.state = (description.flags & PrAppMenuItemFlag_Checked) != 0 ? NSControlStateValueOn : NSControlStateValueOff;
    if (description.submenu != nullptr) {
        item.action = nil;
        item.submenu = menuOf(*description.submenu, tree, previous, parent.host, parent.owner);
        item.submenu.title = title;
        if (description.role == PrAppMenuRole_Services)
            NSApp.servicesMenu = item.submenu;
        else if (description.role == PrAppMenuRole_WindowList)
            NSApp.windowsMenu = item.submenu;
        else if (description.role == PrAppMenuRole_Help)
            NSApp.helpMenu = item.submenu;
    }
    return item;
}

// Returns the menu that `description` describes, made a menu of `tree`. A menu of `previous` with the same tag is
// filled again in place, so that a menu that is open shows the new items.
NSMenu* menuOf(const PrAppMenu& description, PrCocoaMenuTree* tree, NSMutableArray<PrCocoaMenu*>* previous,
        PrCocoaHost* host, PrCocoaWindow* PR_NULLABLE owner) {
    PrCocoaMenu* node = nil;
    for (PrCocoaMenu* candidate in previous) {
        if (candidate.tag == description.tag) {
            node = candidate;
            break;
        }
    }
    if (node != nil) {
        [previous removeObject:node];
        [node.menu removeAllItems];
    } else {
        node = [[PrCocoaMenu alloc] init];
        node.menu = [[NSMenu alloc] init];
        node.tag = description.tag;
    }
    node.host = host;
    node.owner = owner;
    node.ofWindow = owner != nil;
    node.menu.title = cocoaStringOf(description.title);
    node.menu.delegate = node;
    node.menu.autoenablesItems = NO;
    [tree.menus addObject:node];
    for (uint32_t i = 0; i < description.count; ++i)
        [node.menu addItem:menuItemOf(description.items[i], node, tree, previous)];
    return node.menu;
}

// Returns the application's own menu, which macOS shows first in the menu bar.
NSMenuItem* applicationMenuItem() {
    PrAppMenuItem items[] = {
        {.role = PrAppMenuRole_About},
        {.role = PrAppMenuRole_Separator},
        {.role = PrAppMenuRole_Hide},
        {.role = PrAppMenuRole_HideOthers},
        {.role = PrAppMenuRole_ShowAll},
        {.role = PrAppMenuRole_Separator},
        {.role = PrAppMenuRole_Quit},
    };
    NSMenu* menu = [[NSMenu alloc] init];
    for (const PrAppMenuItem& description : items) {
        if (description.role == PrAppMenuRole_Separator) {
            [menu addItem:NSMenuItem.separatorItem];
            continue;
        }
        const CocoaMenuRole* role = menuRoleOf(description.role);
        NSMenuItem* item = [[NSMenuItem alloc] initWithTitle:roleTitleOf(*role) action:role->action
            keyEquivalent:keyEquivalentOf(role->key)];
        item.keyEquivalentModifierMask = keyModifiersOf(role->modifiers);
        [menu addItem:item];
    }
    NSMenuItem* services = [[NSMenuItem alloc] initWithTitle:@"Services" action:nil keyEquivalent:@""];
    services.submenu = [[NSMenu alloc] initWithTitle:@"Services"];
    NSApp.servicesMenu = services.submenu;
    [menu insertItem:services atIndex:2];
    [menu insertItem:NSMenuItem.separatorItem atIndex:3];
    NSMenuItem* item = [[NSMenuItem alloc] init];
    item.submenu = menu;
    return item;
}

// Shows a context menu from the main queue once no callback runs, so that its MenuOpening event reaches the program
// before the menu opens.
void showMenuSoon(PrCocoaWindow* object, PrVec2 position) {
    __weak PrCocoaWindow* weak = object;
    dispatch_async(dispatch_get_main_queue(), ^{
        PrCocoaWindow* window = weak;
        if (window == nil || window.record == nullptr || window.contextMenu == nil)
            return;
        if (window.host.inCallback) {
            showMenuSoon(window, position);
            return;
        }
        NSMenu* menu = window.contextMenu.root;
        [menu popUpMenuPositioningItem:nil atLocation:NSMakePoint(CGFloat(position.x), CGFloat(position.y))
            inView:window.view];
        window.contextMenu = nil;
    });
}

struct CocoaStatusItem : PrAppStatusItem {
    void* object;               // the PrCocoaStatusItem, retained
};

// Returns the icon of a status item at the menu bar's height, from straight-alpha pixels, or nil for none.
NSImage* PR_NULLABLE statusIconOf(PrPixelView pixels, bool templated) {
    NSBitmapImageRep* rep = cocoaImageRepOf(pixels);
    if (rep == nil)
        return nil;
    CGFloat height = NSStatusBar.systemStatusBar.thickness - 4;
    CGFloat width = height * CGFloat(pixels.extent.x) / CGFloat(MAX(pixels.extent.y, 1u));
    NSImage* image = [[NSImage alloc] initWithSize:NSMakeSize(width, height)];
    [image addRepresentation:rep];
    [image setTemplate:templated];
    return image;
}

void statusItemApply(PrCocoaStatusItem* object, const PrAppStatusItemDesc& desc) {
    NSStatusBarButton* button = object.item.button;
    object.tag = desc.tag;
    button.image = statusIconOf(desc.icon, desc.templateIcon);
    button.title = cocoaStringOf(desc.title);
    button.imagePosition = desc.title.size != 0 ? NSImageLeft : NSImageOnly;
    button.toolTip = desc.tooltip.size != 0 ? cocoaStringOf(desc.tooltip) : nil;
    if (desc.menu != nullptr) {
        NSMutableArray<PrCocoaMenu*>* previous = object.tree.menus != nil ? object.tree.menus : [NSMutableArray array];
        if (object.tree == nil)
            object.tree = [[PrCocoaMenuTree alloc] init];
        object.tree.menus = [NSMutableArray array];
        object.tree.root = menuOf(*desc.menu, object.tree, previous, object.host, nil);
        object.item.menu = object.tree.root;
    } else {
        object.tree = nil;
        object.item.menu = nil;
        button.target = object;
        button.action = @selector(clicked:);
    }
}

void statusItemDestroy(PrAppStatusItem* item) {
    CocoaStatusItem* cocoa = static_cast<CocoaStatusItem*>(item);
    PrCocoaStatusItem* object = (__bridge_transfer PrCocoaStatusItem*)cocoa->object;
    [NSStatusBar.systemStatusBar removeStatusItem:object.item];
    prMemoryFree(cocoa);
}

void statusItemSet(PrAppStatusItem* item, const PrAppStatusItemDesc* desc) {
    statusItemApply((__bridge PrCocoaStatusItem*)static_cast<CocoaStatusItem*>(item)->object, *desc);
}

} // namespace

void cocoaMenuBarOf(const PrAppMenu* PR_NULLABLE description, PrCocoaMenuTree* tree, PrCocoaHost* host,
        PrCocoaWindow* PR_NULLABLE owner) {
    NSMutableArray<PrCocoaMenu*>* previous = tree.menus != nil ? tree.menus : [NSMutableArray array];
    tree.menus = [NSMutableArray array];
    if (tree.root == nil)
        tree.root = [[NSMenu alloc] init];
    [tree.root removeAllItems];
    [tree.root addItem:applicationMenuItem()];
    for (uint32_t i = 0; description != nullptr && i < description->count; ++i) {
        const PrAppMenuItem& top = description->items[i];
        if (top.submenu == nullptr)
            continue;
        NSMenuItem* item = [[NSMenuItem alloc] init];
        item.submenu = menuOf(*top.submenu, tree, previous, host, owner);
        item.title = top.title.size != 0 ? cocoaStringOf(top.title) : cocoaStringOf(top.submenu->title);
        item.submenu.title = item.title;
        if (top.role == PrAppMenuRole_WindowList)
            NSApp.windowsMenu = item.submenu;
        else if (top.role == PrAppMenuRole_Help)
            NSApp.helpMenu = item.submenu;
        [tree.root addItem:item];
    }
}

void cocoaSetMenuBar(PrApp* app, const PrAppMenu* PR_NULLABLE menu) {
    PrCocoaHost* host = cocoaHostOf(app);
    if (host.menuBar == nil)
        host.menuBar = [[PrCocoaMenuTree alloc] init];
    cocoaMenuBarOf(menu, host.menuBar, host, nil);
    [host showMenuBar];
}

void cocoaWindowSetMenuBar(PrAppWindow* window, const PrAppMenu* PR_NULLABLE menu) {
    PrCocoaWindow* object = cocoaObjectOf(window);
    if (menu == nullptr) {
        object.menuBar = nil;
    } else {
        if (object.menuBar == nil)
            object.menuBar = [[PrCocoaMenuTree alloc] init];
        cocoaMenuBarOf(menu, object.menuBar, object.host, object);
    }
    [object.host showMenuBar];
}

void cocoaShowMenu(PrAppWindow* window, const PrAppMenu* menu, PrVec2 position) {
    PrCocoaWindow* object = cocoaObjectOf(window);
    if (object.host.headless)
        return;
    PrCocoaMenuTree* tree = [[PrCocoaMenuTree alloc] init];
    tree.menus = [NSMutableArray array];
    tree.root = menuOf(*menu, tree, [NSMutableArray array], object.host, object);
    object.contextMenu = tree;
    showMenuSoon(object, position);
}

PrStatus cocoaStatusItemCreate(PrApp* app, const PrAppStatusItemDesc* desc, PrAppStatusItem* PR_NULLABLE* PR_NONNULL out) {
    PrCocoaHost* host = cocoaHostOf(app);
    if (!host.running)
        return PrStatus_InvalidState;
    if (host.headless)
        return PrStatus_NotSupported;
    PrCocoaStatusItem* object = [[PrCocoaStatusItem alloc] init];
    object.host = host;
    object.item = [NSStatusBar.systemStatusBar statusItemWithLength:NSVariableStatusItemLength];
    statusItemApply(object, *desc);
    CocoaStatusItem* item = allocZeroed<CocoaStatusItem>();
    item->destroyFn = statusItemDestroy;
    item->setFn = statusItemSet;
    item->object = (__bridge_retained void*)object;
    *out = item;
    return PrStatus_Ok;
}

} // namespace pr_impl

using namespace pr_impl;

@implementation PrCocoaMenu
// Delivers the item's tag to the menu's window or to the application.
- (void)deliverKind:(PrAppEventKind)kind tag:(uint64_t)tag {
    PrCocoaHost* host = self.host;
    if (host == nil || !host.running)
        return;
    PrAppWindow* target = nullptr;
    if (self.ofWindow) {
        target = self.owner.record;
        if (target == nullptr)
            return;
    }
    PrAppEvent event{};
    event.kind = kind;
    event.time = cocoaUptime();
    event.tag = tag;
    event.modifiers = cocoaModifiersOf(NSEvent.modifierFlags);
    [host deliver:event to:target];
}
- (void)chosen:(NSMenuItem*)item {
    [self deliverKind:PrAppEventKind_MenuItem tag:uint64_t(item.tag)];
}
// AppKit asks before the menu shows, which lets the program set the menu again with its current state.
- (void)menuNeedsUpdate:(NSMenu*)menu {
    (void)menu;
    [self deliverKind:PrAppEventKind_MenuOpening tag:self.tag];
}
@end

@implementation PrCocoaMenuTree
@end

@implementation PrCocoaStatusItem
- (void)clicked:(id)sender {
    (void)sender;
    PrCocoaHost* host = self.host;
    if (host == nil || !host.running)
        return;
    PrAppEvent event{};
    event.kind = PrAppEventKind_StatusItem;
    event.time = cocoaUptime();
    event.tag = self.tag;
    [host deliver:event to:nullptr];
}
@end
