// Favorites: the pages that the user keeps, in the order that the user arranged them, and the file that keeps them
// between runs.

#include "examples/browser/favorites.h"

#include "pr/json.h"

#include "examples/browser/address.h"
#include "examples/browser/profile.h"

namespace browser {

uint32_t favoriteFind(const std::vector<Favorite>& favorites, PrStringView url) {
    for (size_t i = 0; i < favorites.size(); ++i) {
        if (favorites[i].url == url)
            return uint32_t(i);
    }
    return kFavoriteMissing;
}

std::string favoriteLabel(const Favorite& favorite) {
    if (!favorite.title.empty())
        return favorite.title;
    std::string host = addressHost(favorite.url);
    return host.empty() ? favorite.url : host;
}

std::vector<Favorite> favoritesDefault() {
    return {
        {"WHATWG HTML", "https://html.spec.whatwg.org/multipage/"},
        {"Wikipedia", "https://en.wikipedia.org/wiki/Main_Page"},
        {"Hacker News", "https://news.ycombinator.com/"},
        {"MDN Web Docs", "https://developer.mozilla.org/en-US/"},
    };
}

bool favoritesParse(PrStringView text, std::vector<Favorite>* out) {
    PrJson* json = prJsonCreate();
    bool parsed = prJsonParse(json, text, nullptr) && prJsonValueCount(json) != 0 &&
        prJsonKind(json, 0) == PrJsonKind_Array;
    std::vector<Favorite> favorites;
    for (PrJsonValueIndex item = parsed ? prJsonFirst(json, 0) : kPrJsonMissing; item != kPrJsonMissing;
            item = prJsonAfter(json, 0, item)) {
        if (prJsonKind(json, item) != PrJsonKind_Object)
            continue;
        PrJsonValueIndex url = prJsonMember(json, item, "url");
        if (url == kPrJsonMissing || prJsonKind(json, url) != PrJsonKind_String)
            continue;
        Favorite favorite;
        PrStringView view = prJsonString(json, url);
        favorite.url.assign(view.data, view.size);
        PrJsonValueIndex title = prJsonMember(json, item, "title");
        if (title != kPrJsonMissing && prJsonKind(json, title) == PrJsonKind_String) {
            view = prJsonString(json, title);
            favorite.title.assign(view.data, view.size);
        }
        if (!favorite.url.empty())
            favorites.push_back(favorite);
    }
    prJsonDestroy(json);
    if (parsed)
        *out = favorites;
    return parsed;
}

std::string favoritesText(const std::vector<Favorite>& favorites) {
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
    std::string text(written.data, written.size);
    prJsonWriterDestroy(writer);
    return text;
}

bool favoritesLoad(PrStringView path, std::vector<Favorite>* out) {
    std::string text;
    return profileRead(path, &text) && favoritesParse(text, out);
}

bool favoritesSave(PrStringView path, const std::vector<Favorite>& favorites) {
    return profileWrite(path, favoritesText(favorites));
}

} // namespace browser
