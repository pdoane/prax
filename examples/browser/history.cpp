// History: the pages that the user visited, the most recent last, and the file that keeps them between runs.

#include "examples/browser/history.h"

#include "pr/array.h"
#include "pr/float.h"
#include "pr/json.h"
#include "pr/string.h"
#include "pr/time_date.h"

#include "examples/browser/address.h"
#include "examples/browser/profile.h"

namespace browser {

using namespace pr;

namespace {

constexpr double kDayMilliseconds = 86400000;

// Returns the string member `name` of the object at `item`, or empty if it has none of that type.
String stringMember(const PrJson* json, PrJsonValueIndex item, const char* name) {
    PrJsonValueIndex member = prJsonMember(json, item, name);
    if (member == kPrJsonMissing || prJsonKind(json, member) != PrJsonKind_String)
        return String();
    PrStringView view = prJsonString(json, member);
    return view;
}

} // namespace

void historyRecord(Array<Visit>* history, PrStringView url, PrStringView title, double time) {
    if (addressIsBlank(url))
        return;
    historyRemove(history, url);
    history->push(Visit{url, title, time});
    if (history->count() > kHistoryMaxVisits)
        history->erase(0);
}

bool historySetTitle(Array<Visit>* history, PrStringView url, PrStringView title) {
    if (title.isEmpty())
        return false;
    for (Visit& visit : *history) {
        if (visit.url != url)
            continue;
        if (visit.title == title)
            return false;
        visit.title = title;
        return true;
    }
    return false;
}

bool historyRemove(Array<Visit>* history, PrStringView url) {
    for (uint32_t i = 0; i < history->count(); ++i) {
        if ((*history)[i].url == url) {
            history->erase(i);
            return true;
        }
    }
    return false;
}

Array<uint32_t> historySearch(const Array<Visit>& history, PrStringView query) {
    Array<uint32_t> found;
    for (uint32_t i = history.count(); i-- > 0;) {
        const Visit& visit = history[i];
        if (addressMatches(visit.url, query) || addressMatches(visit.title, query))
            found.push(uint32_t(i));
    }
    return found;
}

String historyDayLabel(int64_t day, int64_t today) {
    if (day == today)
        return "Today";
    if (day == today - 1)
        return "Yesterday";
    static const char* const kWeekdays[] = {
        "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday",
    };
    static const char* const kMonths[] = {
        "January", "February", "March", "April", "May", "June",
        "July", "August", "September", "October", "November", "December",
    };
    PrTimeDate date = prTimeDateFromDays(day);
    return format("{}, {} {}, {}", kWeekdays[prTimeWeekdayFromDays(day)], kMonths[date.month - 1], date.day, date.year);
}

String historyTimeLabel(double time, double offset) {
    double midnight = double(prTimeDaysFromMilliseconds(time, offset)) * kDayMilliseconds;
    uint32_t minutes = uint32_t(prF64Floor((time + offset - midnight) / 60000));
    uint32_t hours = minutes / 60;
    return format("{:02}:{:02}", hours, minutes % 60);
}

bool historyParse(PrStringView text, Array<Visit>* out) {
    PrJson* json = prJsonCreate();
    bool parsed = prJsonParse(json, text, nullptr) && prJsonValueCount(json) != 0 &&
        prJsonKind(json, 0) == PrJsonKind_Array;
    Array<Visit> history;
    for (PrJsonValueIndex item = parsed ? prJsonFirst(json, 0) : kPrJsonMissing; item != kPrJsonMissing;
            item = prJsonAfter(json, 0, item)) {
        if (prJsonKind(json, item) != PrJsonKind_Object)
            continue;
        String url = stringMember(json, item, "url");
        PrJsonValueIndex time = prJsonMember(json, item, "time");
        bool timed = time != kPrJsonMissing && prJsonKind(json, time) == PrJsonKind_Number;
        historyRecord(&history, url, stringMember(json, item, "title"), timed ? prJsonNumber(json, time, 0) : 0);
    }
    prJsonDestroy(json);
    if (parsed)
        *out = history;
    return parsed;
}

String historyText(const Array<Visit>& history) {
    PrJsonWriter* writer = prJsonWriterCreate();
    prJsonWriterBeginArray(writer);
    for (const Visit& visit : history) {
        prJsonWriterBeginObject(writer);
        prJsonWriterKey(writer, "url");
        prJsonWriterString(writer, visit.url);
        prJsonWriterKey(writer, "title");
        prJsonWriterString(writer, visit.title);
        prJsonWriterKey(writer, "time");
        prJsonWriterNumber(writer, visit.time);
        prJsonWriterEndObject(writer);
    }
    prJsonWriterEndArray(writer);
    PrStringView written = prJsonWriterText(writer);
    String text(written);
    prJsonWriterDestroy(writer);
    return text;
}

bool historyLoad(PrStringView path, Array<Visit>* out) {
    String text;
    return profileRead(path, &text) && historyParse(text, out);
}

bool historySave(PrStringView path, const Array<Visit>& history) {
    return profileWrite(path, historyText(history));
}

} // namespace browser
