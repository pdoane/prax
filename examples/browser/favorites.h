// Favorites: the pages that the user keeps, in the order that the user arranged them, and the file that keeps them
// between runs.

#pragma once

#include "pr/base.h"
#include "pr/string_view.h"

#include <stdint.h>
#include <string>
#include <vector>

PR_ASSUME_NONNULL_BEGIN

namespace browser {

// A page that the user keeps.
struct Favorite {
    std::string title;              // the page's title when it was added, or empty if it had none
    std::string url;
};

// The value that favoriteFind returns if no favorite has the URL.
constexpr uint32_t kFavoriteMissing = 0xffffffffu;

// Returns the index of the first favorite whose URL is `url`, or kFavoriteMissing.
uint32_t favoriteFind(const std::vector<Favorite>& favorites, PrStringView url);

// Returns the label that the chrome shows for a favorite: its title, or else the host of its URL, or else the URL.
std::string favoriteLabel(const Favorite& favorite);

// Returns the favorites that the browser starts with if the user has none yet.
std::vector<Favorite> favoritesDefault();

// Reads favorites from JSON `text`: an array of objects whose string members `title` and `url` give a favorite. An
// object without a URL is skipped, as is a member of another type. Returns false if the text is not such an array, and
// leaves `out` unchanged in that case.
bool favoritesParse(PrStringView text, std::vector<Favorite>* out);

// Returns the favorites as the JSON text that favoritesParse reads.
std::string favoritesText(const std::vector<Favorite>& favorites);

// Reads the favorites from the file at `path`. Returns false if the file cannot be read or does not parse, and leaves
// `out` unchanged in that case.
bool favoritesLoad(PrStringView path, std::vector<Favorite>* out);

// Writes the favorites to the file at `path`, creating its directory if it is missing. Returns false if the file cannot
// be written.
bool favoritesSave(PrStringView path, const std::vector<Favorite>& favorites);

} // namespace browser

PR_ASSUME_NONNULL_END
