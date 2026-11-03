// The browser's commands and key bindings (pr/command.h), which the chrome's buttons and menus, and the keyboard, run
// alike.

#include "examples/browser/commands.h"

#include "pr/assert.h"
#include "pr/command.h"
#include "pr/command_keymap.h"
#include "pr/count_of.h"

#include "examples/browser/address.h"
#include "examples/browser/tab.h"

#include <cstdio>
#include <stdint.h>

namespace browser {

using namespace pr;

// A command of the table: what it does, its state if it has one, and the chords that bind it in the application's
// context, up to three, the first of which menus show.
struct CommandSpec {
    const char* id;
    const char* title;
    const char* category;
    void (*run)(Browser& b);
    PrCommandStateFlags (*PR_NULLABLE state)(const Browser& b) = nullptr;
    const char* PR_NULLABLE chords[3] = {};
    PrCommandFlags flags = 0;
};

namespace {

// The zoom factors that zooming in and out steps through.
constexpr float kZooms[] = {0.25f, 0.33f, 0.5f, 0.67f, 0.75f, 0.8f, 0.9f, 1, 1.1f, 1.25f, 1.5f, 1.75f, 2, 2.5f, 3, 4};

// Returns the zoom after `zoom` in the direction of `step`, or `zoom` if it is at the end of the range.
float zoomStep(float zoom, int step) {
    if (step > 0) {
        for (float z : kZooms) {
            if (z > zoom + 0.001f)
                return z;
        }
    } else {
        for (size_t i = countOf(kZooms); i-- > 0;) {
            if (kZooms[i] < zoom - 0.001f)
                return kZooms[i];
        }
    }
    return zoom;
}

PrCommandStateFlags disabledUnless(bool enabled) {
    return enabled ? 0 : PrCommandStateFlag_Disabled;
}

// Returns true if the active tab shows a document of the web, rather than the start page or one of the browser's own.
bool showsWebPage(const Tab* PR_NULLABLE tab) {
    return tab != nullptr && !addressIsBlank(tab->info.url) && !addressIsBrowserPage(tab->info.url);
}

// Adds the active tab's page to the favorites, or removes it if it is one, and saves the favorites.
void toggleFavorite(Browser& b) {
    Tab* tab = browserActiveTab(b);
    uint32_t found = favoriteFind(b.favorites, tab->info.url);
    if (found != kFavoriteMissing)
        b.favorites.erase(b.favorites.begin() + found);
    else
        b.favorites.push_back(Favorite{tab->info.title, tab->info.url});
    if (!b.favoritesPath.empty() && !favoritesSave(b.favoritesPath, b.favorites))
        fprintf(stderr, "browser: cannot save the favorites to %s\n", b.favoritesPath.c_str());
}

// The index that selectTab takes for the last tab.
constexpr uint32_t kLastTab = UINT32_MAX;

// Makes the tab at `index` active, or the last tab for kLastTab, if there is such a tab.
void selectTab(Browser& b, uint32_t index) {
    uint32_t count = uint32_t(b.tabs.size());
    uint32_t chosen = index == kLastTab ? count - 1 : index;
    if (chosen < count)
        tabActivate(b, chosen);
}

PrCommandStateFlags whenOtherTabs(const Browser& b) {
    return disabledUnless(b.tabs.size() > 1);
}

PrCommandStateFlags whenZoomsIn(const Browser& b) {
    const Tab* tab = browserActiveTab(b);
    return disabledUnless(tab != nullptr && zoomStep(tab->zoom, 1) != tab->zoom);
}

PrCommandStateFlags whenZoomsOut(const Browser& b) {
    const Tab* tab = browserActiveTab(b);
    return disabledUnless(tab != nullptr && zoomStep(tab->zoom, -1) != tab->zoom);
}

// The chord that the platform's browsers bind to the history: Command-Y on Apple's platforms, where it is not redo, and
// Control-H elsewhere.
#if defined(__APPLE__)
constexpr const char* kHistoryChord = "Cmd Y";
#else
constexpr const char* kHistoryChord = "Ctrl H";
#endif

constexpr CommandSpec kSpecs[] = {
    // Tabs.
    {
        .id = kCommandNewTab,
        .title = "New Tab",
        .category = "Tabs",
        .run = [](Browser& b) {
            tabOpenNext(b, PrStringView{});
            b.chrome.focusAddress = true;
        },
        .chords = {"Cmd T"},
    },
    {
        .id = "tab.reopen",
        .title = "Reopen Closed Tab",
        .category = "Tabs",
        .run = [](Browser& b) {
            std::string url = b.closed.back();
            b.closed.pop_back();
            tabOpenNext(b, url);
        },
        .state = [](const Browser& b) { return disabledUnless(!b.closed.empty()); },
        .chords = {"Cmd Shift T"},
    },
    {
        .id = "tab.close",
        .title = "Close Tab",
        .category = "Tabs",
        .run = [](Browser& b) { tabClose(b, b.active); },
        .chords = {"Cmd W"},
    },
    {
        .id = "tab.next",
        .title = "Next Tab",
        .category = "Tabs",
        .run = [](Browser& b) { tabActivate(b, (b.active + 1) % uint32_t(b.tabs.size())); },
        .state = whenOtherTabs,
        .chords = {"Ctrl Tab"},
    },
    {
        .id = "tab.previous",
        .title = "Previous Tab",
        .category = "Tabs",
        .run = [](Browser& b) {
            uint32_t count = uint32_t(b.tabs.size());
            tabActivate(b, (b.active + count - 1) % count);
        },
        .state = whenOtherTabs,
        .chords = {"Ctrl Shift Tab"},
    },
    // The command modifier with a digit chooses a tab: 1 to 8 by position, and 9 the last tab.
    {
        .id = "tab.select1",
        .title = "Select Tab 1",
        .category = "Tabs",
        .run = [](Browser& b) { selectTab(b, 0); },
        .chords = {"Cmd 1"},
        .flags = PrCommandFlag_Hidden,
    },
    {
        .id = "tab.select2",
        .title = "Select Tab 2",
        .category = "Tabs",
        .run = [](Browser& b) { selectTab(b, 1); },
        .chords = {"Cmd 2"},
        .flags = PrCommandFlag_Hidden,
    },
    {
        .id = "tab.select3",
        .title = "Select Tab 3",
        .category = "Tabs",
        .run = [](Browser& b) { selectTab(b, 2); },
        .chords = {"Cmd 3"},
        .flags = PrCommandFlag_Hidden,
    },
    {
        .id = "tab.select4",
        .title = "Select Tab 4",
        .category = "Tabs",
        .run = [](Browser& b) { selectTab(b, 3); },
        .chords = {"Cmd 4"},
        .flags = PrCommandFlag_Hidden,
    },
    {
        .id = "tab.select5",
        .title = "Select Tab 5",
        .category = "Tabs",
        .run = [](Browser& b) { selectTab(b, 4); },
        .chords = {"Cmd 5"},
        .flags = PrCommandFlag_Hidden,
    },
    {
        .id = "tab.select6",
        .title = "Select Tab 6",
        .category = "Tabs",
        .run = [](Browser& b) { selectTab(b, 5); },
        .chords = {"Cmd 6"},
        .flags = PrCommandFlag_Hidden,
    },
    {
        .id = "tab.select7",
        .title = "Select Tab 7",
        .category = "Tabs",
        .run = [](Browser& b) { selectTab(b, 6); },
        .chords = {"Cmd 7"},
        .flags = PrCommandFlag_Hidden,
    },
    {
        .id = "tab.select8",
        .title = "Select Tab 8",
        .category = "Tabs",
        .run = [](Browser& b) { selectTab(b, 7); },
        .chords = {"Cmd 8"},
        .flags = PrCommandFlag_Hidden,
    },
    {
        .id = "tab.selectLast",
        .title = "Select Last Tab",
        .category = "Tabs",
        .run = [](Browser& b) { selectTab(b, kLastTab); },
        .chords = {"Cmd 9"},
        .flags = PrCommandFlag_Hidden,
    },

    // The page. Back and forward also take Alt with an arrow, and Super with an arrow, which a text field of the chrome
    // keeps for its caret.
    {
        .id = "address.focus",
        .title = "Go to Address Bar",
        .category = "Page",
        .run = [](Browser& b) { b.chrome.focusAddress = true; },
        .chords = {"Cmd L"},
    },
    {
        .id = kCommandBack,
        .title = "Back",
        .category = "Page",
        .run = [](Browser& b) { tabTraverse(*browserActiveTab(b), -1); },
        .state = [](const Browser& b) {
            const Tab* tab = browserActiveTab(b);
            return disabledUnless(tab != nullptr && tab->info.canGoBack);
        },
        .chords = {"Cmd [", "Alt Left", "Super Left"},
    },
    {
        .id = kCommandForward,
        .title = "Forward",
        .category = "Page",
        .run = [](Browser& b) { tabTraverse(*browserActiveTab(b), 1); },
        .state = [](const Browser& b) {
            const Tab* tab = browserActiveTab(b);
            return disabledUnless(tab != nullptr && tab->info.canGoForward);
        },
        .chords = {"Cmd ]", "Alt Right", "Super Right"},
    },
    {
        .id = kCommandReload,
        .title = "Reload",
        .category = "Page",
        .run = [](Browser& b) { tabTraverse(*browserActiveTab(b), 0); },
        .state = [](const Browser& b) {
            const Tab* tab = browserActiveTab(b);
            return disabledUnless(tab != nullptr && !addressIsBlank(tab->info.url));
        },
        .chords = {"Cmd R", "F5"},
    },

    // Favorites.
    {
        .id = kCommandToggleFavorite,
        .title = "Favorite This Page",
        .category = "Favorites",
        .run = toggleFavorite,
        .state = [](const Browser& b) -> PrCommandStateFlags {
            const Tab* tab = browserActiveTab(b);
            if (!showsWebPage(tab))
                return PrCommandStateFlag_Disabled;
            bool favorite = favoriteFind(b.favorites, tab->info.url) != kFavoriteMissing;
            return favorite ? PrCommandStateFlag_Checked : 0;
        },
        .chords = {"Cmd D"},
    },
    {
        .id = "favorites.toggleBar",
        .title = "Show Favorites Bar",
        .category = "Favorites",
        .run = [](Browser& b) { b.chrome.favoritesBar = !b.chrome.favoritesBar; },
        .state = [](const Browser& b) {
            return PrCommandStateFlags(b.chrome.favoritesBar ? PrCommandStateFlag_Checked : 0);
        },
        .chords = {"Cmd Shift B"},
    },

    // History.
    {
        .id = kCommandShowHistory,
        .title = "History",
        .category = "History",
        .run = tabsShowHistory,
        .chords = {kHistoryChord},
    },
    {
        .id = kCommandDeleteBrowsingData,
        .title = "Delete Browsing Data\xe2\x80\xa6",
        .category = "History",
        .run = [](Browser& b) {
            tabsShowHistory(b);
            b.chrome.openDelete = true;
        },
        .chords = {"Cmd Shift Backspace", "Cmd Shift Delete"},
    },

    // The view.
    {
        .id = "view.zoomIn",
        .title = "Zoom In",
        .category = "View",
        .run = [](Browser& b) { browserActiveTab(b)->zoom = zoomStep(browserActiveTab(b)->zoom, 1); },
        .state = whenZoomsIn,
        .chords = {"Cmd =", "Cmd Shift =", "Cmd Shift +"},
    },
    {
        .id = "view.zoomOut",
        .title = "Zoom Out",
        .category = "View",
        .run = [](Browser& b) { browserActiveTab(b)->zoom = zoomStep(browserActiveTab(b)->zoom, -1); },
        .state = whenZoomsOut,
        .chords = {"Cmd -"},
    },
    {
        .id = kCommandZoomReset,
        .title = "Actual Size",
        .category = "View",
        .run = [](Browser& b) { browserActiveTab(b)->zoom = 1; },
        .state = [](const Browser& b) {
            const Tab* tab = browserActiveTab(b);
            return disabledUnless(tab != nullptr && tab->zoom != 1);
        },
        .chords = {"Cmd 0"},
    },
};

PrCommandIndex indexOf(const Browser& b, PrStringView id) {
    PrCommandIndex index = prCommandRegistryFind(b.commands, id);
    PR_ASSERT(index != kPrCommandMissing);
    return index;
}

} // namespace

void commandsCreate(Browser& b) {
    b.commands = prCommandRegistryCreate();
    b.keymap = prCommandKeymapCreate();
    b.commandContexts.resize(countOf(kSpecs));
    for (size_t i = 0; i < countOf(kSpecs); ++i) {
        const CommandSpec& spec = kSpecs[i];
        b.commandContexts[i] = CommandContext{&b, &spec};
        PrCommandDesc desc{
            .id = spec.id,
            .title = spec.title,
            .category = spec.category,
            .flags = spec.flags,
            .run = [](void* PR_NULLABLE context, const PrCommandArgs*) {
                CommandContext& c = *static_cast<CommandContext*>(context);
                c.spec->run(*c.browser);
                browserWantFrame(*c.browser);
            },
            .context = &b.commandContexts[i],
        };
        if (spec.state != nullptr) {
            desc.state = [](void* PR_NULLABLE context) {
                const CommandContext& c = *static_cast<const CommandContext*>(context);
                return c.spec->state(*c.browser);
            };
        }
        prCommandRegistryAdd(b.commands, &desc);
        for (const char* text : spec.chords) {
            PrCommandChord chord{};
            if (text == nullptr)
                break;
            if (!prCommandChordParse(text, &chord))
                PR_FATAL("a chord of the browser's commands does not parse");
            prCommandKeymapBind(b.keymap, chord, spec.id, "", PrCommandBindingKind_Normal);
        }
    }
}

void commandsDestroy(Browser& b) {
    prCommandKeymapDestroy(b.keymap);
    prCommandRegistryDestroy(b.commands);
    b.keymap = nullptr;
    b.commands = nullptr;
    b.commandContexts.clear();
}

void commandRun(Browser& b, PrStringView id) {
    prCommandRegistryRun(b.commands, id, nullptr, nullptr);
}

bool commandIsEnabled(const Browser& b, PrStringView id) {
    return (prCommandRegistryState(b.commands, indexOf(b, id)) & PrCommandStateFlag_Disabled) == 0;
}

bool commandIsChecked(const Browser& b, PrStringView id) {
    return (prCommandRegistryState(b.commands, indexOf(b, id)) & PrCommandStateFlag_Checked) != 0;
}

PrStringView commandTitle(const Browser& b, PrStringView id) {
    return prCommandRegistryCommand(b.commands, indexOf(b, id)).title;
}

std::string commandShortcut(const Browser& b, PrStringView id) {
    PrCommandChord chord{};
    if (!prCommandKeymapChord(b.keymap, id, &chord))
        return std::string();
    PrRawString text = prCommandChordText(chord);
    PrStringView view = prRawStringView(&text);
    std::string copy(view.data, view.size);
    prRawStringReset(&text);
    return copy;
}

bool commandsRunChord(Browser& b, const PrAppEvent& event) {
    PrAppModifierFlags modifiers = PrAppModifierFlags(event.modifiers & ~PrAppModifierFlag_CapsLock);
    PrStringView id = prCommandKeymapCommand(b.keymap, PrCommandChord{event.key, modifiers}, "");
    if (id.data == nullptr)
        return false;
    commandRun(b, id);
    return true;
}

} // namespace browser
