#include "pr/array.h"
#include "pr/string.h"

#include "apps/browser/favorites.h"

#include "tools/harness/test.h"

using namespace browser;

namespace {

// Returns the favorite of `title` and `url`.
Favorite favorite(const char* title, const char* url) {
    return Favorite{.title = title, .url = url};
}

} // namespace

TEST_CASE("favorites survive being written and read back") {
    Array<Favorite> favorites;
    favorites.push(favorite("Example", "https://example.com/"));
    favorites.push(favorite("", "https://untitled.example/"));
    favorites.push(favorite("Quotes \" and \\ and \xe2\x9c\x93", "https://example.com/?q=\"x\""));
    String text = favoritesText(favorites);
    Array<Favorite> read;
    REQUIRE(favoritesParse(text, &read));
    REQUIRE(read.count() == favorites.count());
    for (uint32_t i = 0; i < favorites.count(); ++i) {
        CHECK(read[i].title == favorites[i].title);
        CHECK(read[i].url == favorites[i].url);
    }
}

TEST_CASE("an entry without a URL is skipped, and text that is not an array is refused") {
    Array<Favorite> read;
    read.push(favorite("kept", "https://kept.example/"));
    CHECK(favoritesParse(R"([{"title":"no url"},{"url":7},{"url":""},3,{"url":"https://a.example/","title":5}])", &read));
    REQUIRE(read.count() == 1);
    CHECK(read[0].url == "https://a.example/");
    CHECK(read[0].title == "");

    Array<Favorite> unchanged;
    unchanged.push(favorite("kept", "https://kept.example/"));
    CHECK_FALSE(favoritesParse(R"({"url":"https://a.example/"})", &unchanged));
    CHECK_FALSE(favoritesParse("[", &unchanged));
    CHECK_FALSE(favoritesParse("", &unchanged));
    REQUIRE(unchanged.count() == 1);
    CHECK(unchanged[0].title == "kept");
}

TEST_CASE("a favorite is found by its URL, and labeled by its title or else its host") {
    Array<Favorite> favorites;
    favorites.push(favorite("One", "https://one.example/"));
    favorites.push(favorite("", "https://www.two.example/x"));
    CHECK(favoriteFind(favorites, "https://one.example/") == 0);
    CHECK(favoriteFind(favorites, "https://www.two.example/x") == 1);
    CHECK(favoriteFind(favorites, "https://three.example/") == kFavoriteMissing);
    CHECK(favoriteLabel(favorites[0]) == "One");
    CHECK(favoriteLabel(favorites[1]) == "two.example");
    CHECK(favoriteLabel(favorite("", "about:blank")) == "about:blank");
}
