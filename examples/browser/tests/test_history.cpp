#include "examples/browser/history.h"

#include "doctest/doctest.h"

#include <string>
#include <vector>

using namespace browser;

TEST_CASE("the history keeps a URL's most recent visit, the most recent last, and no blank tab") {
    std::vector<Visit> history;
    historyRecord(&history, "https://a.example/", "A", 1000);
    historyRecord(&history, "https://b.example/", "", 2000);
    historyRecord(&history, "about:blank", "", 2500);
    historyRecord(&history, "", "", 2600);
    historyRecord(&history, "https://a.example/", "A again", 3000);
    REQUIRE(history.size() == 2);
    CHECK(history[0].url == "https://b.example/");
    CHECK(history[1].url == "https://a.example/");
    CHECK(history[1].title == "A again");
    CHECK(history[1].time == 3000);

    // A title that comes once the page loads names the visit, and an empty one leaves it.
    CHECK(historySetTitle(&history, "https://b.example/", "B"));
    CHECK_FALSE(historySetTitle(&history, "https://b.example/", "B"));
    CHECK_FALSE(historySetTitle(&history, "https://b.example/", ""));
    CHECK_FALSE(historySetTitle(&history, "https://c.example/", "C"));
    CHECK(history[0].title == "B");
    CHECK(history[0].time == 2000);

    CHECK(historyRemove(&history, "https://b.example/"));
    CHECK_FALSE(historyRemove(&history, "https://b.example/"));
    REQUIRE(history.size() == 1);
    CHECK(history[0].url == "https://a.example/");
}

TEST_CASE("the history forgets its oldest visit past its limit") {
    std::vector<Visit> history;
    for (size_t i = 0; i <= kHistoryMaxVisits; ++i)
        historyRecord(&history, "https://example.com/" + std::to_string(i), "", double(i));
    REQUIRE(history.size() == kHistoryMaxVisits);
    CHECK(history.front().url == "https://example.com/1");
}

TEST_CASE("a search finds visits by URL or title, ignoring case, the most recent first") {
    std::vector<Visit> history{
        {"https://news.example/", "Morning News", 1},
        {"https://shop.example/", "Shop", 2},
        {"https://example.com/news", "", 3},
    };
    CHECK(historySearch(history, "NEWS") == std::vector<uint32_t>{2, 0});
    CHECK(historySearch(history, "") == std::vector<uint32_t>{2, 1, 0});
    CHECK(historySearch(history, "missing").empty());
}

TEST_CASE("a day is headed by its name near today, and by its weekday and date further back") {
    CHECK(historyDayLabel(20468, 20468) == "Today");
    CHECK(historyDayLabel(20467, 20468) == "Yesterday");
    CHECK(historyDayLabel(20468, 20470) == "Thursday, January 15, 2026");
    CHECK(historyDayLabel(0, 20468) == "Thursday, January 1, 1970");
    CHECK(historyDayLabel(-1, 20468) == "Wednesday, December 31, 1969");
    // 2024-02-29, a leap day, and the day after it.
    CHECK(historyDayLabel(19782, 20468) == "Thursday, February 29, 2024");
    CHECK(historyDayLabel(19783, 20468) == "Friday, March 1, 2024");
}

TEST_CASE("a visit's time of day is local, on a 24-hour clock") {
    double time = 1768519800000.0;  // 2026-01-15 at 23:30 UTC
    CHECK(historyTimeLabel(time, 0) == "23:30");
    CHECK(historyTimeLabel(time, 2 * 3600000.0) == "01:30");
    CHECK(historyTimeLabel(time, -5.5 * 3600000.0) == "18:00");
}

TEST_CASE("the history survives being written and read back, and text that is not an array is refused") {
    std::vector<Visit> history{
        {"https://example.com/", "Example", 1768478400000.5},
        {"https://untitled.example/", "", 1768478500000},
        {"https://example.com/?q=\"x\"", "Quotes \" and \\ and \xe2\x9c\x93", 1768478600000},
    };
    std::vector<Visit> read;
    REQUIRE(historyParse(historyText(history), &read));
    REQUIRE(read.size() == history.size());
    for (size_t i = 0; i < history.size(); ++i) {
        CHECK(read[i].url == history[i].url);
        CHECK(read[i].title == history[i].title);
        CHECK(read[i].time == history[i].time);
    }

    // An entry without a URL is skipped, and a member of another type reads as missing.
    CHECK(historyParse(R"([{"title":"no url"},{"url":7},3,{"url":"https://a.example/","title":5,"time":"x"}])", &read));
    REQUIRE(read.size() == 1);
    CHECK(read[0].url == "https://a.example/");
    CHECK(read[0].title == "");
    CHECK(read[0].time == 0);

    std::vector<Visit> unchanged{{"https://kept.example/", "kept", 1}};
    CHECK_FALSE(historyParse(R"({"url":"https://a.example/"})", &unchanged));
    CHECK_FALSE(historyParse("[", &unchanged));
    REQUIRE(unchanged.size() == 1);
    CHECK(unchanged[0].title == "kept");
}
