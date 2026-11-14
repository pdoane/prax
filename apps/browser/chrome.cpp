// The chrome: the browser's own interface around the page, in Studio Kit.
//
// From the top, the chrome is the tab strip, the toolbar with the address bar, and the favorites bar. Below them, the
// active tab's page fills a box that the chrome leaves empty, and the window draws the page there. A tab that shows no
// document shows the start page instead, which the chrome draws: the favorites and the pages visited recently. The
// chrome draws the history page over the empty document at the history's address (kAddressHistory).
//
// The chrome's buttons, its menus and the keyboard run the browser's commands (commands.h) alike.

#include "apps/browser/chrome.h"

#include "pr/array.h"
#include "pr/command_keymap.h"
#include "pr/command_kit.h"
#include "pr/date_time.h"
#include "pr/float.h"
#include "pr/kit_box.h"
#include "pr/kit_button.h"
#include "pr/kit_choice.h"
#include "pr/kit_image.h"
#include "pr/kit_input.h"
#include "pr/kit_menu.h"
#include "pr/kit_panel.h"
#include "pr/kit_text.h"
#include "pr/min_max.h"
#include "pr/storage.h"
#include "pr/string.h"
#include "pr/string_ops.h"
#include "pr/time.h"
#include "pr/utf8.h"

#include "apps/browser/address.h"
#include "apps/browser/commands.h"
#include "apps/browser/icons.h"
#include "apps/browser/tab.h"

namespace browser {

using namespace pr;

namespace {

// ---------------------------------------------------------------------------------------------------------------------
// Constants

// The platform's primary modifier, which opens a link of the chrome in a new tab.
constexpr PrAppModifierFlags kCommand = kPrCommandPrimaryModifier;

// The most suggestions that the address bar lists, the most recent pages that the start page lists, and the most visits
// that the history page lists, past which a search finds the rest.
constexpr size_t kMaxSuggestions = 8;
constexpr size_t kMaxRecent = 8;
constexpr uint32_t kMaxHistoryRows = 300;

// The start page's column, in px, and its favorites' tiles: as many to a row as fill the column, and the gap between
// them in px.
constexpr float kStartWidth = 720;
constexpr uint32_t kTilesPerRow = 4;
constexpr float kTileGap = 8;

// The most characters of a title that a tab or a favorite shows.
constexpr uint32_t kMaxLabel = 28;

// The side of the square that a tab fits its page's icon into, in px.
constexpr float kTabIconSize = 16;

// Returns true if the tab shows the start page: it shows no document and loads none.
bool showsStart(const Tab& tab) {
    return addressIsBlank(tab.info.url) && !tab.info.loading;
}

// Deletes what the history page's dialog chose of the browsing data. The history goes with the tabs that can reopen.
void deleteBrowsingData(Browser& b) {
    const Chrome& chrome = b.chrome;
    if (chrome.deleteHistory) {
        tabsForget(b, PrStringView{});
        b.closed.clear();
    }
    if (chrome.deleteCache)
        prStorageClear(b.cache);
    if (chrome.deleteSiteData)
        prHtmlStorageProviderClear(b.localStorage);
}

// ---------------------------------------------------------------------------------------------------------------------
// Labels

// Returns `text` cut to `max` code points, with an ellipsis in place of what was cut.
String shortened(PrStringView text, uint32_t max) {
    size_t at = 0;
    uint32_t points = 0;
    while (at < text.size && points < max) {
        uint32_t codePoint = 0;
        uint32_t length = prUtf8Decode(text, at, &codePoint);
        at += length != 0 ? length : 1;
        ++points;
    }
    if (at >= text.size)
        return text;
    return format("{}\xe2\x80\xa6", prStringSlice(text, 0, at));
}

// Returns what a tab's label shows: its page's title, or else its host or URL, or "New Tab" for a blank tab.
String tabLabel(const Tab& tab) {
    if (!tab.info.title.isEmpty())
        return shortened(tab.info.title, kMaxLabel);
    PrStringView url = tab.info.url;
    if (addressIsBlank(url))
        return "New Tab";
    PrStringView host = addressHost(url);
    return shortened(host.isEmpty() ? url : host, kMaxLabel);
}

// Returns the icon at the start of the address bar for `url`: a lock for a secure page, and a search for anything else.
PrKitIconKind addressIcon(const String& url) {
    if (prStringStartsWith(url, "https:"))
        return PrKitIconKind_Lock;
    if (prStringStartsWith(url, "http:"))
        return PrKitIconKind_Unlock;
    if (prStringStartsWith(url, "file:"))
        return PrKitIconKind_Folder;
    return PrKitIconKind_Search;
}

// ---------------------------------------------------------------------------------------------------------------------
// Parts of the chrome

// The style of a toolbar's buttons.
PrKitButtonStyle toolStyle() {
    return PrKitButtonStyle{.variant = PrKitButtonVariant_Ghost, .size = PrKitControlSize_Md};
}

// Creates a button that runs the command `id`, with a tooltip that names the command and its shortcut. The button is
// selected while the command is checked, and disabled while the command is.
PrLayoutNode commandButton(Browser& b, PrLayoutNode parent, PrKitButton button, PrKitButtonStyle style, PrStringView id) {
    button.selected = commandIsChecked(b, id);
    button.disabled = !commandIsEnabled(b, id);
    PrLayoutNode node = prKitButton(b.kit, parent, button, style);
    String shortcut = commandShortcut(b, id);
    prKitTooltip(b.kit, node, {.text = commandTitle(b, id), .shortcut = shortcut});
    if (prKitActivated(b.kit, node))
        commandRun(b, id);
    return node;
}

// Creates a toolbar's icon button that runs the command `id`.
PrLayoutNode toolButton(Browser& b, PrLayoutNode toolbar, PrKitIconKind icon, PrStringView id) {
    return commandButton(b, toolbar, {.icon = icon}, toolStyle(), id);
}

// Creates a menu item that runs the command `id`.
void commandItem(Browser& b, PrLayoutNode menu, PrStringView id) {
    prCommandMenuItem(b.kit, menu, b.commands, b.keymap, id, {});
}

// Returns true if a press asks a link in the chrome, such as a favorite, to open in a new tab rather than the active
// tab: a middle click, or a click with the command modifier.
bool opensInNewTab(Browser& b, PrLayoutNode node) {
    return prUiClicked(b.ui, node, 2) || (prKitActivated(b.kit, node) && (prUiPressModifiers(b.ui, 0) & kCommand) != 0);
}

// The tab strip, and the button that opens a tab after the strip's tabs. Each tab has a context menu, a middle click
// closes it, and a drag moves it in the strip.
void buildTabs(Browser& b, PrLayoutNode root) {
    PrKitPanelStyle style{
        .box = {.direction = PrKitDirection_Row, .align = PrKitAlign_End, .gap = 4, .padding = {6, 8, 0, 8}},
        .background = PrKitBackground_Header,
    };
    PrLayoutNode row = prKitPanel(b.kit, root, {}, style);
    PrLayoutNode strip = prKitTabStrip(b.kit, row, {.tabs = b.tabStrip, .reorderable = true}, {});
    prKitPlace(b.kit, strip, {.flex = PrKitFlex_Auto});
    // Tabs close and open once the loop has created every tab, so that the loop walks a list that does not change.
    Tab* closing = nullptr;
    Tab* closingOthers = nullptr;
    Tab* duplicating = nullptr;
    for (Tab* each : b.tabs) {
        Tab& tab = *each;
        // A tab shows its page's icon, if it has one, its label, and its close control.
        PrLayoutNode node = prKitTab(b.kit, strip, {.id = tabId(tab)});
        if (const PrDrawImage* image = iconsGet(b, tab.info.iconUrl)) {
            PrKitImageStyle icon{
                .fit = PrKitImageFit_Contain,
                .place = {.width = prKitPx(kTabIconSize), .height = prKitPx(kTabIconSize), .flex = PrKitFlex_Fixed},
            };
            prKitImage(b.kit, node, {.image = image}, icon);
        }
        prKitTabLabel(b.kit, node, {.text = tabLabel(tab)});
        if (prKitActivated(b.kit, prKitTabClose(b.kit, node)) || prUiClicked(b.ui, node, 2))
            closing = &tab;
        PrStringView tip = tab.info.title;
        if (tip.isEmpty())
            tip = addressShown(tab.info.url);
        if (!tip.isEmpty())
            prKitTooltip(b.kit, node, {.text = tip});
        PrLayoutNode menu = prKitContextMenu(b.kit, node, {}, {});
        if (menu == kPrLayoutNodeNone)
            continue;
        if (prKitActivated(b.kit, prKitMenuItem(b.kit, menu, {.text = "Reload"}, {})))
            tabTraverse(tab, 0);
        if (prKitActivated(b.kit, prKitMenuItem(b.kit, menu, {.text = "Duplicate"}, {})))
            duplicating = &tab;
        prKitMenuSeparator(b.kit, menu, {});
        if (prKitActivated(b.kit, prKitMenuItem(b.kit, menu, {.text = "Close Tab"}, {})))
            closing = &tab;
        PrKitMenuItem others{.text = "Close Other Tabs", .disabled = b.tabs.count() < 2};
        if (prKitActivated(b.kit, prKitMenuItem(b.kit, menu, others, {})))
            closingOthers = &tab;
    }
    // A click or a key in the strip may have chosen another tab.
    tabsShowActive(b);
    PrKitButtonStyle small{.variant = PrKitButtonVariant_Ghost, .size = PrKitControlSize_Sm};
    PrLayoutNode plus = commandButton(b, row, {.icon = PrKitIconKind_Plus}, small, kCommandNewTab);
    prKitPlace(b.kit, plus, {.margin = {0, 0, 4, 0}});

    if (duplicating != nullptr) {
        uint32_t after = prKitTabsIndex(b.tabStrip, tabId(*duplicating)) + 1;
        tabActivate(b, *tabOpen(b, duplicating->info.url, after));
    }
    if (closingOthers != nullptr) {
        for (uint32_t i = b.tabs.count(); i-- > 0;) {
            if (b.tabs[i] != closingOthers)
                tabClose(b, *b.tabs[i]);
        }
        tabActivate(b, *closingOthers);
    } else if (closing != nullptr) {
        tabClose(b, *closing);
    }
}

// Returns the address bar's suggestions for the text that the user typed: favorites first, then the history's pages,
// the most recent first, each URL once.
Array<Favorite> suggestionsFor(const Browser& b, const String& typed) {
    Array<Favorite> found;
    PrStringView query = typed;
    auto add = [&](const String& title, const String& url) {
        if (found.count() >= kMaxSuggestions || url == typed)
            return;
        if (!addressMatches(url, query) && !addressMatches(title, query))
            return;
        for (const Favorite& seen : found) {
            if (seen.url == url)
                return;
        }
        found.push(Favorite{title, url});
    };
    for (const Favorite& favorite : b.favorites)
        add(favorite.title, favorite.url);
    for (uint32_t i = b.history.count(); i-- > 0 && found.count() < kMaxSuggestions;)
        add(b.history[i].title, b.history[i].url);
    return found;
}

// Leaves the address bar: focus leaves the chrome, and the bar shows the active tab's URL again.
void leaveAddress(Browser& b, PrLayoutNode field) {
    prUiSetFocus(b.ui, kPrLayoutNodeNone);
    prKitSetOpen(b.kit, field, false);
    b.chrome.addressTab = nullptr;
}

// The address bar, and the suggestions under it while the user types. Enter loads what the text means (addressResolve)
// in the active tab, and Escape restores the tab's URL.
void buildAddress(Browser& b, PrLayoutNode toolbar, Tab& tab) {
    Chrome& chrome = b.chrome;
    // The bar follows the active tab while the user is not editing it.
    if (!chrome.addressFocused || chrome.addressTab != &tab) {
        chrome.address = addressShown(tab.info.url);
        chrome.typed = chrome.address;
        chrome.addressTab = &tab;
    }
    PrKitTextField field{
        .text = chrome.address,
        .placeholder = "Search or enter an address",
        .icon = addressIcon(tab.info.url),
        .selectOnFocus = true,
    };
    PrLayoutNode node = prKitTextField(b.kit, toolbar, field, {.size = PrKitControlSize_Md});
    PrStringView changed{};
    if (prKitTextChanged(b.kit, node, &changed))
        chrome.address = changed;
    if (chrome.focusAddress) {
        prUiSetFocus(b.ui, node);
        chrome.focusAddress = false;
    }
    chrome.addressFocused = prUiFocus(b.ui) == node;
    PrUiEvents events = prUiEvents(b.ui, node);
    for (uint32_t i = 0; i < events.count; ++i) {
        const PrAppEvent& e = events.events[i];
        if (e.kind != PrAppEventKind_KeyDown)
            continue;
        if (e.keyboard.key == PrAppKey_Enter) {
            String url = addressResolve(chrome.address);
            if (!url.isEmpty())
                tabsLoad(b, url, (e.modifiers & kCommand) != 0 ? TabTarget_New : TabTarget_Active);
            leaveAddress(b, node);
            return;
        }
        if (e.keyboard.key == PrAppKey_Escape) {
            leaveAddress(b, node);
            return;
        }
    }
    // The suggestions open as the user types, and Kit closes them on Escape, a press outside them or a choice.
    Array<Favorite> suggestions;
    if (chrome.addressFocused && !chrome.address.isEmpty())
        suggestions = suggestionsFor(b, chrome.address);
    if (chrome.address != chrome.typed) {
        chrome.typed = chrome.address;
        prKitSetOpen(b.kit, node, chrome.addressFocused && !suggestions.isEmpty());
    }
    if (suggestions.isEmpty())
        return;
    PrRect box = prLayoutRectToPx(prLayoutNodeBounds(b.tree, node, PrLayoutCoordinates_Viewport));
    PrKitPopoverStyle placed{.offset = PrVec2{0, 4}, .minWidth = box.right - box.left};
    PrLayoutNode popover = prKitPopover(b.kit, node, {}, placed);
    if (popover == kPrLayoutNodeNone)
        return;
    PrLayoutNode menu = prKitMenu(b.kit, popover, {}, {.leavesFocus = true});
    for (uint32_t i = 0; i < suggestions.count(); ++i) {
        const Favorite& suggestion = suggestions[i];
        String text = suggestion.url;
        if (!suggestion.title.isEmpty())
            text = format("{}  \xe2\x80\x94  {}", shortened(suggestion.title, kMaxLabel * 2), suggestion.url);
        bool favorite = favoriteFind(b.favorites, suggestion.url) != kFavoriteMissing;
        PrKitMenuItem item{.text = text, .icon = PrKitIconKind(favorite ? PrKitIconKind_Star : PrKitIconKind_Link)};
        if (prKitActivated(b.kit, prKitMenuItem(b.kit, menu, item, {}))) {
            tabsLoad(b, suggestion.url, TabTarget_Active);
            leaveAddress(b, node);
            return;
        }
    }
}

// The menu of the browser's commands, which the button at the end of the toolbar opens.
void buildMenu(Browser& b, PrLayoutNode toolbar) {
    PrKitButton button{.icon = PrKitIconKind_More};
    PrLayoutNode trigger = prKitButton(b.kit, toolbar, button, toolStyle());
    if (prKitActivated(b.kit, trigger))
        prKitToggleOpen(b.kit, trigger);
    PrLayoutNode popover = prKitPopover(b.kit, trigger, {}, {.offset = PrVec2{-200, 4}, .minWidth = 230});
    if (popover == kPrLayoutNodeNone)
        return;
    PrLayoutNode menu = prKitMenu(b.kit, popover, {}, {});
    commandItem(b, menu, kCommandNewTab);
    commandItem(b, menu, "tab.reopen");
    commandItem(b, menu, "tab.close");
    prKitMenuSeparator(b.kit, menu, {});
    commandItem(b, menu, kCommandReload);
    commandItem(b, menu, kCommandToggleFavorite);
    commandItem(b, menu, "favorites.toggleBar");
    prKitMenuSeparator(b.kit, menu, {});
    commandItem(b, menu, kCommandShowHistory);
    commandItem(b, menu, kCommandDeleteBrowsingData);
    prKitMenuSeparator(b.kit, menu, {});
    commandItem(b, menu, "view.zoomIn");
    commandItem(b, menu, "view.zoomOut");
    commandItem(b, menu, kCommandZoomReset);
}

// The toolbar: back, forward and reload, the address bar, the favorite's star, the zoom while it is not 100%, and the
// menu. A bar along its bottom shows while the page loads. The menu comes last, since it can close the tab.
void buildToolbar(Browser& b, PrLayoutNode root, Tab& tab) {
    bool loading = tab.info.loading;
    PrLayoutNode stack = prKitStack(b.kit, root, {}, {});
    PrKitPanelStyle style{
        .box = {.direction = PrKitDirection_Row, .align = PrKitAlign_Center, .gap = 2, .padding = prKitEdgesXY(5, 8)},
        .background = PrKitBackground_Panel,
        .rules = PrKitRuleFlag_Bottom,
    };
    PrLayoutNode toolbar = prKitPanel(b.kit, stack, {}, style);
    toolButton(b, toolbar, PrKitIconKind_ArrowLeft, kCommandBack);
    toolButton(b, toolbar, PrKitIconKind_ArrowRight, kCommandForward);
    toolButton(b, toolbar, PrKitIconKind_Restart, kCommandReload);
    buildAddress(b, toolbar, tab);
    toolButton(b, toolbar, PrKitIconKind_Star, kCommandToggleFavorite);
    if (tab.zoom != 1) {
        String percent = format("{}%", int(prF32Round(tab.zoom * 100)));
        PrKitButtonStyle small{.variant = PrKitButtonVariant_Ghost, .size = PrKitControlSize_Sm};
        commandButton(b, toolbar, {.text = percent}, small, kCommandZoomReset);
    }
    buildMenu(b, toolbar);
    if (!loading)
        return;
    PrKitBoxStyle bottom{.justify = PrKitJustify_End};
    PrLayoutNode layer = prKitLayer(b.kit, stack, {}, bottom);
    PrLayoutNode progress = prKitProgress(b.kit, layer, {.indeterminate = true});
    prKitPlace(b.kit, progress, {.height = prKitPx(2)});
}

// The favorites bar: a button for each favorite, which loads it. Each has a context menu, and a middle click or a click
// with the command modifier opens it in a new tab.
void buildFavorites(Browser& b, PrLayoutNode root) {
    PrKitPanelStyle style{
        .box = {
            .direction = PrKitDirection_Row,
            .align = PrKitAlign_Center,
            .overflow = PrKitOverflow_Clip,
            .gap = 2,
            .padding = prKitEdgesXY(3, 8),
        },
        .background = PrKitBackground_Panel,
        .rules = PrKitRuleFlag_Bottom,
    };
    PrLayoutNode bar = prKitPanel(b.kit, root, {}, style);
    if (b.favorites.isEmpty()) {
        PrKitTextStyle faint{.role = PrKitTextRole_BodySmall, .tone = PrKitTone_Faint};
        prKitLabel(b.kit, bar, {.text = "Pages that you add to your favorites show here."}, faint);
        return;
    }
    uint32_t removing = kFavoriteMissing;
    for (uint32_t i = 0; i < b.favorites.count(); ++i) {
        const Favorite& favorite = b.favorites[i];
        String label = shortened(favoriteLabel(favorite), kMaxLabel);
        PrKitButton button{.text = label};
        PrLayoutNode node = prKitButton(b.kit, bar, button, {
            .variant = PrKitButtonVariant_Ghost,
            .size = PrKitControlSize_Sm,
        });
        prKitTooltip(b.kit, node, {.text = favorite.url});
        if (opensInNewTab(b, node))
            tabsLoad(b, favorite.url, TabTarget_New);
        else if (prKitActivated(b.kit, node))
            tabsLoad(b, favorite.url, TabTarget_Active);
        PrLayoutNode menu = prKitContextMenu(b.kit, node, {}, {});
        if (menu == kPrLayoutNodeNone)
            continue;
        if (prKitActivated(b.kit, prKitMenuItem(b.kit, menu, {.text = "Open"}, {})))
            tabsLoad(b, favorite.url, TabTarget_Active);
        if (prKitActivated(b.kit, prKitMenuItem(b.kit, menu, {.text = "Open in New Tab"}, {})))
            tabsLoad(b, favorite.url, TabTarget_New);
        prKitMenuSeparator(b.kit, menu, {});
        PrKitMenuItem remove{.text = "Remove from Favorites", .icon = PrKitIconKind_Trash};
        if (prKitActivated(b.kit, prKitMenuItem(b.kit, menu, remove, {.variant = PrKitMenuItemVariant_Danger})))
            removing = i;
    }
    if (removing != kFavoriteMissing) {
        b.favorites.erase(removing);
        if (!b.favoritesPath.isEmpty())
            favoritesSave(b.favoritesPath, b.favorites);
    }
}

// Creates a tile or a row of the start page that loads `url`, labeled by `title` above the URL's host or the URL.
void startLink(Browser& b, PrLayoutNode parent, PrStringView title, PrStringView url, PrKitBoxStyle style) {
    PrLayoutNode link = prKitSelectable(b.kit, parent, {}, style);
    prUiSetCursor(b.ui, link, PrAppCursor_Hand);
    PrStringView host = addressHost(url);
    PrKitTextStyle strong{.role = PrKitTextRole_Strong, .wrap = PrKitWrap_Clip};
    PrKitTextStyle meta{.role = PrKitTextRole_Meta, .tone = PrKitTone_Muted, .wrap = PrKitWrap_Clip};
    PrStringView site = host.isEmpty() ? url : host;
    prKitLabel(b.kit, link, {.text = title.isEmpty() ? site : title}, strong);
    prKitLabel(b.kit, link, {.text = title.isEmpty() ? url : site}, meta);
    if (opensInNewTab(b, link))
        tabsLoad(b, url, TabTarget_New);
    else if (prKitActivated(b.kit, link))
        tabsLoad(b, url, TabTarget_Active);
}

// The start page of a tab that shows no document: the favorites as tiles, and the pages visited most recently.
void buildStart(Browser& b, PrLayoutNode root) {
    PrKitPanelStyle style{
        .box = {
            .align = PrKitAlign_Center,
            .overflow = PrKitOverflow_ScrollY,
            .gap = 18,
            .padding = prKitEdgesXY(56, 24),
            .place = {.flex = PrKitFlex_Fill},
        },
        .background = PrKitBackground_App,
    };
    PrLayoutNode page = prKitPanel(b.kit, root, {}, style);
    prKitLabel(b.kit, page, {.text = "Prax"}, {.role = PrKitTextRole_Display});
    PrKitTextStyle caption{.role = PrKitTextRole_Caption, .tone = PrKitTone_Muted};
    PrKitPlace column{.width = prKitPx(kStartWidth), .maxWidth = prKitPercent(100)};
    if (!b.favorites.isEmpty()) {
        PrLayoutNode section = prKitBox(b.kit, page, {}, {.gap = 8, .place = column});
        prKitLabel(b.kit, section, {.text = "FAVORITES"}, caption);
        PrKitBoxStyle rows{.direction = PrKitDirection_Row, .wrap = true, .gap = kTileGap};
        PrLayoutNode grid = prKitBox(b.kit, section, {}, rows);
        float width = (kStartWidth - kTileGap * float(kTilesPerRow - 1)) / float(kTilesPerRow);
        PrKitBoxStyle tile{.gap = 2, .padding = prKitEdgesAll(10), .place = {.width = prKitPx(width)}};
        for (uint32_t i = 0; i < b.favorites.count(); ++i)
            startLink(b, grid, b.favorites[i].title, b.favorites[i].url, tile);
    }
    if (!b.history.isEmpty()) {
        PrLayoutNode section = prKitBox(b.kit, page, {}, {.gap = 4, .place = column});
        prKitLabel(b.kit, section, {.text = "RECENTLY VISITED"}, caption);
        PrKitBoxStyle row{.gap = 1, .padding = prKitEdgesXY(6, 10)};
        uint32_t shown = min(b.history.count(), uint32_t(kMaxRecent));
        for (uint32_t i = 0; i < shown; ++i) {
            const Visit& visit = b.history[b.history.count() - 1 - i];
            startLink(b, section, visit.title, visit.url, row);
        }
        PrKitButton all{.text = "Show Full History"};
        PrLayoutNode more = prKitButton(b.kit, section, all, {.variant = PrKitButtonVariant_Ghost});
        prKitPlace(b.kit, more, {.align = PrKitAlign_Start});
        if (prKitActivated(b.kit, more))
            commandRun(b, kCommandShowHistory);
    }
}

// Creates a row of the history page: the visit's time, and its title and site, which load the page, and a control that
// removes the visit. A middle click or a click with the command modifier opens the page in a new tab. `offset` is the
// local time zone's offset at the visit. Returns true if the user asked to remove the visit.
bool historyRow(Browser& b, PrLayoutNode list, const Visit& visit, int32_t offset) {
    PrLayoutNode row = prKitBox(b.kit, list, {.key = prUiKey(b.kit->ui, list, visit.url)}, {
        .direction = PrKitDirection_Row,
        .align = PrKitAlign_Center,
        .gap = 4,
    });
    PrKitBoxStyle style{
        .direction = PrKitDirection_Row,
        .align = PrKitAlign_Center,
        .gap = 12,
        .padding = prKitEdgesXY(6, 10),
        .place = {.flex = PrKitFlex_Fill},
    };
    PrLayoutNode link = prKitSelectable(b.kit, row, {}, style);
    prUiSetCursor(b.ui, link, PrAppCursor_Hand);
    prKitTooltip(b.kit, link, {.text = visit.url});
    PrKitTextStyle meta{.role = PrKitTextRole_Meta, .tone = PrKitTone_Muted, .wrap = PrKitWrap_Clip};
    PrKitTextStyle strong{.role = PrKitTextRole_Strong, .wrap = PrKitWrap_Clip};
    PrStringView url = visit.url;
    PrStringView title = visit.title;
    PrStringView host = addressHost(url);
    PrStringView site = host.isEmpty() ? url : host;
    prKitLabel(b.kit, link, {.text = historyTimeLabel(visit.time, offset)}, meta);
    PrLayoutNode heading = prKitLabel(b.kit, link, {.text = title.isEmpty() ? site : title}, strong);
    prKitPlace(b.kit, heading, {.flex = PrKitFlex_Auto});
    prKitLabel(b.kit, link, {.text = title.isEmpty() ? url : site}, meta);
    if (opensInNewTab(b, link))
        tabsLoad(b, visit.url, TabTarget_New);
    else if (prKitActivated(b.kit, link))
        tabsLoad(b, visit.url, TabTarget_Active);
    bool removing = false;
    if (PrLayoutNode menu = prKitContextMenu(b.kit, link, {}, {}); menu != kPrLayoutNodeNone) {
        if (prKitActivated(b.kit, prKitMenuItem(b.kit, menu, {.text = "Open"}, {})))
            tabsLoad(b, visit.url, TabTarget_Active);
        if (prKitActivated(b.kit, prKitMenuItem(b.kit, menu, {.text = "Open in New Tab"}, {})))
            tabsLoad(b, visit.url, TabTarget_New);
        prKitMenuSeparator(b.kit, menu, {});
        PrKitMenuItem remove{.text = "Remove from History", .icon = PrKitIconKind_Trash};
        removing = prKitActivated(b.kit, prKitMenuItem(b.kit, menu, remove, {.variant = PrKitMenuItemVariant_Danger}));
    }
    PrKitButton close{.icon = PrKitIconKind_Close};
    PrLayoutNode remove =
        prKitButton(b.kit, row, close, {.variant = PrKitButtonVariant_Ghost, .size = PrKitControlSize_Sm});
    prKitTooltip(b.kit, remove, {.text = "Remove from History"});
    return removing || prKitActivated(b.kit, remove);
}

// The dialog that deletes the browsing data that the user chooses, which `trigger` opens.
void buildDeleteDialog(Browser& b, PrLayoutNode trigger) {
    Chrome& chrome = b.chrome;
    if (chrome.openDelete) {
        prKitSetOpen(b.kit, trigger, true);
        chrome.openDelete = false;
    }
    PrLayoutNode body = prKitDialog(b.kit, trigger, {.title = "Delete Browsing Data"}, {});
    if (body == kPrLayoutNodeNone)
        return;
    PrKitTextStyle muted{.role = PrKitTextRole_Body, .tone = PrKitTone_Muted};
    prKitLabel(b.kit, body, {.text = "The browser deletes what you choose, from every tab."}, muted);
    prKitCheckbox(b.kit, body, {.value = &chrome.deleteHistory, .label = "History and recently closed tabs"}, {});
    prKitCheckbox(b.kit, body, {.value = &chrome.deleteCache, .label = "Cached images and files"}, {});
    prKitCheckbox(b.kit, body, {.value = &chrome.deleteSiteData, .label = "Data that sites keep in local storage"}, {});
    PrLayoutNode footer = prKitDialogFooter(b.kit, body);
    if (prKitActivated(b.kit, prKitButton(b.kit, footer, {.text = "Cancel"}, {})))
        prKitSetOpen(b.kit, trigger, false);
    bool chosen = chrome.deleteHistory || chrome.deleteCache || chrome.deleteSiteData;
    PrKitButton remove{.text = "Delete", .disabled = !chosen};
    if (prKitActivated(b.kit, prKitButton(b.kit, footer, remove, {.variant = PrKitButtonVariant_Danger}))) {
        deleteBrowsingData(b);
        prKitSetOpen(b.kit, trigger, false);
    }
}

// The history page: the pages visited, the most recent first, under the day of each visit, which a search narrows. A
// button opens the dialog that deletes the browsing data.
void buildHistory(Browser& b, PrLayoutNode root) {
    Chrome& chrome = b.chrome;
    PrKitPanelStyle style{
        .box = {
            .align = PrKitAlign_Center,
            .overflow = PrKitOverflow_ScrollY,
            .gap = 12,
            .padding = prKitEdgesXY(32, 24),
            .place = {.flex = PrKitFlex_Fill},
        },
        .background = PrKitBackground_App,
    };
    PrLayoutNode page = prKitPanel(b.kit, root, {}, style);
    PrKitPlace column{.width = prKitPx(kStartWidth), .maxWidth = prKitPercent(100)};
    PrKitBoxStyle heading{.direction = PrKitDirection_Row, .align = PrKitAlign_Center, .gap = 8, .place = column};
    PrLayoutNode header = prKitBox(b.kit, page, {}, heading);
    prKitLabel(b.kit, header, {.text = "History"}, {.role = PrKitTextRole_Title});
    prKitSpacer(b.kit, header, {});
    PrKitButton deleting{.text = commandTitle(b, kCommandDeleteBrowsingData), .icon = PrKitIconKind_Trash};
    PrLayoutNode trigger = prKitButton(b.kit, header, deleting, {.size = PrKitControlSize_Md});
    String shortcut = commandShortcut(b, kCommandDeleteBrowsingData);
    prKitTooltip(b.kit, trigger, {.text = commandTitle(b, kCommandDeleteBrowsingData), .shortcut = shortcut});
    if (prKitActivated(b.kit, trigger))
        prKitToggleOpen(b.kit, trigger);
    buildDeleteDialog(b, trigger);

    PrKitTextField search{
        .text = chrome.historyQuery,
        .placeholder = "Search history",
        .icon = PrKitIconKind_Search,
        .clearable = true,
    };
    PrLayoutNode field = prKitTextField(b.kit, page, search, {.size = PrKitControlSize_Md});
    PrStringView text{};
    if (prKitTextChanged(b.kit, field, &text))
        chrome.historyQuery = text;
    prKitPlace(b.kit, field, column);
    PrLayoutNode list = prKitBox(b.kit, page, {}, {.gap = 1, .place = column});
    PrKitTextStyle faint{.role = PrKitTextRole_Body, .tone = PrKitTone_Faint};
    if (b.history.isEmpty()) {
        prKitLabel(b.kit, list, {.text = "Pages that you visit show here."}, faint);
        return;
    }
    Array<uint32_t> found = historySearch(b.history, chrome.historyQuery);
    if (found.isEmpty()) {
        prKitLabel(b.kit, list, {.text = "No page in the history matches the search."}, faint);
        return;
    }
    PrTimeSystem now = prTimeSystemNow();
    int64_t today = prDateTimeToDays(prDateTimeFromSystem(now, prTimeLocalOffsetSeconds(now)));
    int64_t shownDay = INT64_MIN;
    String removing;
    PrKitTextStyle day{.role = PrKitTextRole_Heading};
    PrKitPlace dayPlace{.margin = {12, 0, 4, 0}};
    for (uint32_t i = 0; i < found.count() && i < kMaxHistoryRows; ++i) {
        const Visit& visit = b.history[found[i]];
        int32_t offset = prTimeLocalOffsetSeconds(visit.time);
        int64_t visitDay = prDateTimeToDays(prDateTimeFromSystem(visit.time, offset));
        if (visitDay != shownDay) {
            shownDay = visitDay;
            PrLayoutNode label = prKitLabel(b.kit, list, {.text = historyDayLabel(visitDay, today)}, day);
            prKitPlace(b.kit, label, dayPlace);
        }
        if (historyRow(b, list, visit, offset))
            removing = visit.url;
    }
    if (found.count() > kMaxHistoryRows)
        prKitLabel(b.kit, list, {.text = "A search finds the pages visited before these."}, faint);
    if (!removing.isEmpty())
        tabsForget(b, removing);
}

} // namespace

void chromeBuild(Browser& b) {
    Chrome& chrome = b.chrome;
    if (chrome.releaseFocus) {
        prUiSetFocus(b.ui, kPrLayoutNodeNone);
        chrome.releaseFocus = false;
    }
    PrKitPanelStyle style{
        .box = {.overflow = PrKitOverflow_Clip, .place = {.width = prKitPercent(100), .height = prKitPercent(100)}},
    };
    PrLayoutNode root = prKitPanel(b.kit, kPrLayoutNodeNone, {}, style);
    prCommandScope(b.ui, root, b.commands, b.keymap, "");
    buildTabs(b, root);
    Tab& tab = *browserActiveTab(b);
    buildToolbar(b, root, tab);
    if (chrome.favoritesBar)
        buildFavorites(b, root);
    // A tab at the history's address shows the history page over its empty document, and a tab whose page shows no
    // document and loads none shows the start page. Otherwise the page fills an empty box, which the window draws the
    // page in.
    Tab& shown = *browserActiveTab(b);
    if (shown.info.url == kAddressHistory) {
        buildHistory(b, root);
        chrome.slot = kPrLayoutNodeNone;
    } else if (showsStart(shown)) {
        buildStart(b, root);
        chrome.slot = kPrLayoutNodeNone;
    } else {
        chrome.slot = prKitBox(b.kit, root, {}, {.place = {.flex = PrKitFlex_Fill}});
    }
}

} // namespace browser
