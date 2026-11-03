// History: the pages that the user visited, the most recent last, and the file that keeps them between runs.

#include "examples/browser/history.h"

#include "pr/float.h"
#include "pr/json.h"
#include "pr/time_date.h"

#include "examples/browser/address.h"
#include "examples/browser/profile.h"

namespace browser {

using namespace pr;

namespace {

constexpr double kDayMilliseconds = 86400000;

// Returns the string member `name` of the object at `item`, or empty if it has none of that type.
std::string stringMember(const PrJson* json, PrJsonValueIndex item, const char* name) {
    PrJsonValueIndex member = prJsonMember(json, item, name);
    if (member == kPrJsonMissing || prJsonKind(json, member) != PrJsonKind_String)
        return std::string();
    PrStringView view = prJsonString(json, member);
    return std::string(view.data, view.size);
}

} // namespace

void historyRecord(std::vector<Visit>* history, PrStringView url, PrStringView title, double time) {
    if (addressIsBlank(url))
        return;
    historyRemove(history, url);
    history->push_back(Visit{std::string(url.data, url.size), std::string(title.data, title.size), time});
    if (history->size() > kHistoryMaxVisits)
        history->erase(history->begin());
}

bool historySetTitle(std::vector<Visit>* history, PrStringView url, PrStringView title) {
    if (title.size == 0)
        return false;
    for (Visit& visit : *history) {
        if (visit.url != url)
            continue;
        if (visit.title == title)
            return false;
        visit.title.assign(title.data, title.size);
        return true;
    }
    return false;
}

bool historyRemove(std::vector<Visit>* history, PrStringView url) {
    for (size_t i = 0; i < history->size(); ++i) {
        if ((*history)[i].url == url) {
            history->erase(history->begin() + std::ptrdiff_t(i));
            return true;
        }
    }
    return false;
}

std::vector<uint32_t> historySearch(const std::vector<Visit>& history, PrStringView query) {
    std::vector<uint32_t> found;
    for (size_t i = history.size(); i-- > 0;) {
        const Visit& visit = history[i];
        if (addressMatches(visit.url, query) || addressMatches(visit.title, query))
            found.push_back(uint32_t(i));
    }
    return found;
}

std::string historyDayLabel(int64_t day, int64_t today) {
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
    return std::string(kWeekdays[prTimeWeekdayFromDays(day)]) + ", " + kMonths[date.month - 1] + " " +
        std::to_string(date.day) + ", " + std::to_string(date.year);
}

std::string historyTimeLabel(double time, double offset) {
    double midnight = double(prTimeDaysFromMilliseconds(time, offset)) * kDayMilliseconds;
    uint32_t minutes = uint32_t(prF64Floor((time + offset - midnight) / 60000));
    uint32_t hours = minutes / 60;
    return {char('0' + hours / 10), char('0' + hours % 10), ':', char('0' + minutes % 60 / 10), char('0' + minutes % 10)};
}

bool historyParse(PrStringView text, std::vector<Visit>* out) {
    PrJson* json = prJsonCreate();
    bool parsed = prJsonParse(json, text, nullptr) && prJsonValueCount(json) != 0 &&
        prJsonKind(json, 0) == PrJsonKind_Array;
    std::vector<Visit> history;
    for (PrJsonValueIndex item = parsed ? prJsonFirst(json, 0) : kPrJsonMissing; item != kPrJsonMissing;
            item = prJsonAfter(json, 0, item)) {
        if (prJsonKind(json, item) != PrJsonKind_Object)
            continue;
        std::string url = stringMember(json, item, "url");
        PrJsonValueIndex time = prJsonMember(json, item, "time");
        bool timed = time != kPrJsonMissing && prJsonKind(json, time) == PrJsonKind_Number;
        historyRecord(&history, url, stringMember(json, item, "title"), timed ? prJsonNumber(json, time, 0) : 0);
    }
    prJsonDestroy(json);
    if (parsed)
        *out = history;
    return parsed;
}

std::string historyText(const std::vector<Visit>& history) {
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
    std::string text(written.data, written.size);
    prJsonWriterDestroy(writer);
    return text;
}

bool historyLoad(PrStringView path, std::vector<Visit>* out) {
    std::string text;
    return profileRead(path, &text) && historyParse(text, out);
}

bool historySave(PrStringView path, const std::vector<Visit>& history) {
    return profileWrite(path, historyText(history));
}

} // namespace browser
