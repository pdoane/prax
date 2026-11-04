// Favorites: the pages that the user keeps, in the order that the user arranged them, and the file that keeps them
// between runs.

#include "examples/browser/favorites.h"

#include "pr/array.h"
#include "pr/json.h"
#include "pr/string.h"

#include "examples/browser/address.h"
#include "examples/browser/profile.h"

namespace browser {

namespace {

// A favorite that a new profile starts with.
struct DefaultFavorite {
    const char* title;
    const char* url;
};

constexpr DefaultFavorite kDefaultFavorites[] = {
    {"WHATWG HTML", "https://html.spec.whatwg.org/multipage/"},
    {"Wikipedia", "https://en.wikipedia.org/wiki/Main_Page"},
    {"Hacker News", "https://news.ycombinator.com/"},
    {"MDN Web Docs", "https://developer.mozilla.org/en-US/"},
};

} // namespace

uint32_t favoriteFind(const Array<Favorite>& favorites, PrStringView url) {
    for (uint32_t i = 0; i < favorites.count(); ++i) {
        if (favorites[i].url == url)
            return uint32_t(i);
    }
    return kFavoriteMissing;
}

PrStringView favoriteLabel(const Favorite& favorite) {
    if (!favorite.title.isEmpty())
        return favorite.title;
    PrStringView host = addressHost(favorite.url);
    if (host.isEmpty())
        return favorite.url;
    return host;
}

Array<Favorite> favoritesDefault() {
    Array<Favorite> favorites;
    for (const DefaultFavorite& seed : kDefaultFavorites)
        favorites.push(Favorite{.title = seed.title, .url = seed.url});
    return favorites;
}

bool favoritesParse(PrStringView text, Array<Favorite>* out) {
    PrJson* json = prJsonCreate();
    bool parsed = prJsonParse(json, text, nullptr) && prJsonValueCount(json) != 0 &&
        prJsonKind(json, 0) == PrJsonKind_Array;
    Array<Favorite> favorites;
    for (PrJsonValueIndex item = parsed ? prJsonFirst(json, 0) : kPrJsonMissing; item != kPrJsonMissing;
            item = prJsonAfter(json, 0, item)) {
        if (prJsonKind(json, item) != PrJsonKind_Object)
            continue;
        PrJsonValueIndex url = prJsonMember(json, item, "url");
        if (url == kPrJsonMissing || prJsonKind(json, url) != PrJsonKind_String)
            continue;
        Favorite favorite;
        PrStringView view = prJsonString(json, url);
        favorite.url = view;
        PrJsonValueIndex title = prJsonMember(json, item, "title");
        if (title != kPrJsonMissing && prJsonKind(json, title) == PrJsonKind_String) {
            view = prJsonString(json, title);
            favorite.title = view;
        }
        if (!favorite.url.isEmpty())
            favorites.push(favorite);
    }
    prJsonDestroy(json);
    if (parsed)
        *out = favorites;
    return parsed;
}

String favoritesText(const Array<Favorite>& favorites) {
    PrJsonWriter* writer = prJsonWriterCreate();
    prJsonWriterBeginArray(writer);
    for (const Favorite& favorite : favorites) {
        prJsonWriterBeginObject(writer);
        prJsonWriterKey(writer, "title");
        prJsonWriterString(writer, favorite.title);
        prJsonWriterKey(writer, "url");
        prJsonWriterString(writer, favorite.url);
        prJsonWriterEndObject(writer);
    }
    prJsonWriterEndArray(writer);
    PrStringView written = prJsonWriterText(writer);
    String text(written);
    prJsonWriterDestroy(writer);
    return text;
}

bool favoritesLoad(PrStringView path, Array<Favorite>* out) {
    String text;
    return profileRead(path, &text) && favoritesParse(text, out);
}

bool favoritesSave(PrStringView path, const Array<Favorite>& favorites) {
    return profileWrite(path, favoritesText(favorites));
}

} // namespace browser
