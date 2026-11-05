// History: the pages that the user visited, the most recent last, and the file that keeps them between runs.

#pragma once

#include "pr/array.h"
#include "pr/base.h"
#include "pr/string.h"
#include "pr/string_view.h"

#include <stdint.h>

PR_ASSUME_NONNULL_BEGIN

namespace browser {

using namespace pr;

// A page that the user visited. The history keeps one visit for each URL, its most recent.
struct Visit {
    String url;
    String title;              // the page's title at the visit, or empty if it had none
    double time = 0;           // when the visit was, in milliseconds since the Unix epoch
};

// The most visits that the history keeps. Recording a visit past it forgets the oldest.
constexpr size_t kHistoryMaxVisits = 5000;

// Records a visit to `url` with `title` at `time`, which becomes the most recent visit. A blank URL is not recorded.
void historyRecord(Array<Visit>* history, PrStringView url, PrStringView title, double time);

// Sets the title of the visit to `url`, which a page names once its document has loaded. Does nothing if the history
// has no visit to `url`, or if `title` is empty. Returns true if the visit's title changed.
bool historySetTitle(Array<Visit>* history, PrStringView url, PrStringView title);

// Removes the visit to `url`. Returns true if the history had one.
bool historyRemove(Array<Visit>* history, PrStringView url);

// Returns the indices of the visits whose URL or title holds `query` (addressMatches), the most recent first.
Array<uint32_t> historySearch(const Array<Visit>& history, PrStringView query);

// Returns the heading of a day (pr/time_date.h): "Today" or "Yesterday" relative to `today`, or else the weekday and
// the date, as in "Thursday, October 29, 2026".
String historyDayLabel(int64_t day, int64_t today);

// Returns the time of day of `time` on a 24-hour clock, as in "09:05", as a clock `offset` milliseconds ahead of UTC
// shows it (prTimeDaysFromMilliseconds).
String historyTimeLabel(double time, double offset);

// Reads visits from JSON `text`: an array of objects whose members `url` and `title` are strings and whose member
// `time` is a number. An object without a URL is skipped, as is a member of another type. The visits are kept in the
// order of the array, oldest first, and a later visit to a URL replaces an earlier one. Returns false if the text is
// not such an array, and leaves `out` unchanged in that case.
bool historyParse(PrStringView text, Array<Visit>* out);

// Returns the visits as the JSON text that historyParse reads.
String historyText(const Array<Visit>& history);

// Reads the history from the file at `path`. Returns false if the file cannot be read or does not parse, and leaves
// `out` unchanged in that case.
bool historyLoad(PrStringView path, Array<Visit>* out);

// Writes the history to the file at `path`, creating its directory if it is missing. Returns false if the file cannot
// be written.
bool historySave(PrStringView path, const Array<Visit>& history);

} // namespace browser

PR_ASSUME_NONNULL_END
