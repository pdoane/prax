// Tabs: each tab's page on the page's thread, and what the window's thread asks of it.

#pragma once

#include "pr/base.h"
#include "pr/string_view.h"

#include "apps/browser/browser.h"

#include <stdint.h>

PR_ASSUME_NONNULL_BEGIN

namespace browser {

using namespace pr;

// Opens a tab at `index` in the tab strip, or last if `index` is past the last tab, which loads `url`, or shows the
// start page if `url` is empty. The page opens on the page's thread. Returns the tab.
Tab* tabOpen(Browser& b, PrStringView url, uint32_t index);

// Returns the tab's id in the tab strip, which is its address, and the tab of an id, or null for 0. A tab keeps its
// address while the strip holds it.
uint64_t tabId(const Tab& tab);
Tab* PR_NULLABLE tabFromId(uint64_t id);

// Returns the tab at `index` in the tab strip, or null past the last tab.
Tab* PR_NULLABLE tabAt(const Browser& b, uint32_t index);

// Opens a tab after the active one, which loads `url`, or shows the start page if `url` is empty, and makes it the
// active tab.
void tabOpenNext(Browser& b, PrStringView url);

// Where tabsLoad loads a URL.
enum {
    TabTarget_Active,               // the active tab
    TabTarget_New,                  // a new tab after the active one, which becomes the active tab
};
typedef uint8_t TabTarget;

// Loads `url` in the tab that `target` names.
void tabsLoad(Browser& b, PrStringView url, TabTarget target);

// Shows the history page: activates the tab that shows it, or else loads it in the active tab if that tab shows the
// start page, or else in a new tab.
void tabsShowHistory(Browser& b);

// Closes the tab, and remembers its URL so that it can reopen. If it was active, the tab after it becomes active, or
// the last tab. Closing the last tab opens the start page. The page closes on the page's thread.
void tabClose(Browser& b, Tab& tab);

// Makes the tab the active tab, which shows and takes input, and hides the others.
void tabActivate(Browser& b, Tab& tab);

// Shows the active tab, which the strip selects, and hides the others, unless it shows already.
void tabsShowActive(Browser& b);

// Navigates the tab's page to `url`, as a user does by entering an address.
void tabNavigate(Browser& b, Tab& tab, PrStringView url);

// Traverses the tab's session history by `delta` entries, or reloads it for zero.
void tabTraverse(Tab& tab, int32_t delta);

// Takes what the page's thread published of each tab since the last call, and records in the history each document that
// loaded. Returns true if anything changed.
bool tabsRefresh(Browser& b);

// Removes the visit to `url` from the history, or every visit if `url` is empty, and saves the history. The links to
// the URLs removed become unvisited in every tab's page.
void tabsForget(Browser& b, PrStringView url);

// Runs the page's thread: the agent's event loop, until the agent stops.
void tabsRun(Browser& b);

// Closes every tab's page, and the script engine, on the page's thread, and stops the agent's event loop. The window's
// thread forgets its tabs.
void tabsShutdown(Browser& b);

} // namespace browser

PR_ASSUME_NONNULL_END
