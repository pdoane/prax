#include "examples/browser/favorites.h"

#include "doctest/doctest.h"

#include <string>
#include <vector>

using namespace browser;

TEST_CASE("favorites survive being written and read back") {
    std::vector<Favorite> favorites{
        {"Example", "https://example.com/"},
        {"", "https://untitled.example/"},
        {"Quotes \" and \\ and \xe2\x9c\x93", "https://example.com/?q=\"x\""},
    };
    std::string text = favoritesText(favorites);
    std::vector<Favorite> read;
    REQUIRE(favoritesParse(text, &read));
    REQUIRE(read.size() == favorites.size());
    for (size_t i = 0; i < favorites.size(); ++i) {
        CHECK(read[i].title == favorites[i].title);
        CHECK(read[i].url == favorites[i].url);
    }
}

TEST_CASE("an entry without a URL is skipped, and text that is not an array is refused") {
    std::vector<Favorite> read{{"kept", "https://kept.example/"}};
    CHECK(favoritesParse(R"([{"title":"no url"},{"url":7},{"url":""},3,{"url":"https://a.example/","title":5}])", &read));
    REQUIRE(read.size() == 1);
    CHECK(read[0].url == "https://a.example/");
    CHECK(read[0].title == "");

    std::vector<Favorite> unchanged{{"kept", "https://kept.example/"}};
    CHECK_FALSE(favoritesParse(R"({"url":"https://a.example/"})", &unchanged));
    CHECK_FALSE(favoritesParse("[", &unchanged));
    CHECK_FALSE(favoritesParse("", &unchanged));
    REQUIRE(unchanged.size() == 1);
    CHECK(unchanged[0].title == "kept");
}

TEST_CASE("a favorite is found by its URL, and labeled by its title or else its host") {
    std::vector<Favorite> favorites{{"One", "https://one.example/"}, {"", "https://www.two.example/x"}};
    CHECK(favoriteFind(favorites, "https://one.example/") == 0);
    CHECK(favoriteFind(favorites, "https://www.two.example/x") == 1);
    CHECK(favoriteFind(favorites, "https://three.example/") == kFavoriteMissing);
    CHECK(favoriteLabel(favorites[0]) == "One");
    CHECK(favoriteLabel(favorites[1]) == "two.example");
    CHECK(favoriteLabel(Favorite{"", "about:blank"}) == "about:blank");
}
