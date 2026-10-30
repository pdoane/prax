// The Lottie corpora that dev.sh fetches, as one collection each (suite_lottie.h). Each holds every animation in its
// corpus, drawn as its suite renders it (suites/<suite>/render.txt), beside the players: first the reference that its
// suite compares the engine with (tools/suite/reftest.py), then the other players. lottie-web, Skottie and dotLottie's
// player play in the browser (tools/suite/lottie), rlottie in process, and lottie-ios's renders are the snapshots of
// its tests.

#include "tools/viewer/suite_lottie.h"

#include "pr/json.h"

#include "tools/viewer/source_lottie.h"

#include <algorithm>
#include <map>

namespace viewer {

namespace {

namespace fs = std::filesystem;

constexpr const char* kThirdParty = PR_THIRD_PARTY_DIR;
constexpr const char* kSuites = PR_SUITES_DIR;

// The players that a collection offers beside the engine.
enum {
    Player_LottieWeb,
    Player_Skottie,
    Player_DotLottie,
    Player_Rlottie,
    Player_LottieIos,           // lottie-ios's snapshots, of its main thread engine and of its Core Animation engine
};
typedef uint8_t Player;

// The animation's timing, as the snapshots' frames need it: the in point, the length in frames, and the frame rate.
struct Timing {
    double inPoint = 0;
    double length = 0;
    double rate = 30;
};

Timing timingOf(const fs::path& path) {
    Timing timing;
    PrJson* json = prJsonCreate();
    std::string text = readFile(path);
    if (prJsonParse(json, text, nullptr)) {
        timing.inPoint = prJsonNumber(json, prJsonMember(json, 0, "ip"), 0);
        timing.length = prJsonNumber(json, prJsonMember(json, 0, "op"), 0) - timing.inPoint;
        timing.rate = prJsonNumber(json, prJsonMember(json, 0, "fr"), 30);
    }
    prJsonDestroy(json);
    return timing;
}

// A snapshot that lottie-ios's tests took: where in the animation it is, as a progress in percent or a frame, and its
// file.
struct Snapshot {
    int at;
    bool frame;
    fs::path path;
};

// Returns the snapshots that lottie-ios's tests took with `engine`, by the name that the tests give a sample: its path
// within the samples, without its extension and with dashes for slashes.
std::map<std::string, std::vector<Snapshot>> snapshotsOf(const std::string& engine) {
    std::map<std::string, std::vector<Snapshot>> snapshots;
    fs::path directory = fs::path(kThirdParty) / "lottie" / "lottie-ios" / "Tests" / "__Snapshots__" / "SnapshotTests";
    std::error_code error;
    for (fs::directory_iterator it(directory, error), end; it != end; it.increment(error)) {
        if (error)
            break;
        std::string file = it->path().filename().string();
        if (!file.starts_with(engine + ".") || !file.ends_with(".png"))
            continue;
        std::string name = file.substr(engine.size() + 1, file.size() - engine.size() - 5);
        size_t dash = name.rfind('-');
        if (dash == std::string::npos)
            continue;
        Snapshot snapshot{std::atoi(name.c_str() + dash + 1), false, it->path()};
        name.resize(dash);
        if (name.ends_with("-Frame")) {
            name.resize(name.size() - 6);
            snapshot.frame = true;
        }
        snapshots[name].push_back(snapshot);
    }
    return snapshots;
}

// Returns a reference of lottie-ios's snapshots of the animation, at the times they show, or none if there are none. A
// progress of 100% shows the animation's end.
std::vector<Reference> lottieIosReferences(const fs::path& path, const fs::path& root) {
    static const std::pair<const char*, std::map<std::string, std::vector<Snapshot>>> engines[] = {
        {"lottie-ios", snapshotsOf("testMainThreadRenderingEngine")},
        {"lottie-ios (Core Animation)", snapshotsOf("testCoreAnimationRenderingEngine")},
    };
    std::string name = fs::relative(path, root).replace_extension().string();
    std::replace(name.begin(), name.end(), '/', '-');
    Timing timing = timingOf(path);
    std::vector<Reference> references;
    for (const auto& [title, snapshots] : engines) {
        std::map<std::string, std::vector<Snapshot>>::const_iterator found = snapshots.find(name);
        if (found == snapshots.end() || !(timing.rate > 0))
            continue;
        Source source{SourceType_Snapshots};
        source.length = timing.length / timing.rate;
        for (const Snapshot& snapshot : found->second) {
            double frame = snapshot.frame ? snapshot.at - timing.inPoint : snapshot.at / 100.0 * timing.length;
            source.snapshots.emplace_back(std::min(frame / timing.rate, source.length), snapshot.path);
        }
        std::sort(source.snapshots.begin(), source.snapshots.end());
        references.push_back(Reference{title, source});
    }
    return references;
}

// Returns the player's references of the animation, or none if the player is not fetched.
std::vector<Reference> referencesOf(Player player, const fs::path& path, const fs::path& root) {
    fs::path thirdParty = kThirdParty;
    switch (player) {
        case Player_LottieWeb:
            if (fs::exists(thirdParty / "lottie-web" / "lottie.min.js"))
                return {Reference{"lottie-web", {}, lottiePlayerUrl("player.html", path)}};
            break;
        case Player_Skottie:
            if (fs::exists(thirdParty / "canvaskit" / "canvaskit.wasm"))
                return {Reference{"Skottie", {}, lottiePlayerUrl("skottie.html", path)}};
            break;
        case Player_DotLottie:
            if (fs::exists(thirdParty / "dotlottie-web" / "index.js"))
                return {Reference{"dotLottie", {}, lottiePlayerUrl("dotlottie.html", path)}};
            break;
        case Player_Rlottie:
            return {Reference{"rlottie", Source{SourceType_Rlottie, path}}};
        case Player_LottieIos:
            return lottieIosReferences(path, root);
    }
    return {};
}

// The players that every collection offers after its own.
constexpr Player kPlayers[] = {Player_LottieWeb, Player_Skottie, Player_DotLottie, Player_Rlottie};

// A corpus: the suite that runs it, its directory, and its own player, whose references come first.
struct Corpus {
    const char* name;
    const char* suite;
    fs::path root;
    const char* description;
    Player player;
};

// Returns a collection of every animation under the corpus's root, in path order.
Collection collectionOf(const Corpus& c) {
    return Collection{
        c.name,
        512,
        512,
        0,
        [c] {
            std::vector<fs::path> files;
            std::error_code error;
            for (fs::recursive_directory_iterator it(c.root, error), end; it != end; it.increment(error)) {
                if (error)
                    break;
                if (it->is_regular_file() && it->path().extension() == ".json")
                    files.push_back(it->path());
            }
            std::sort(files.begin(), files.end());
            fs::path settings = fs::path(kSuites) / c.suite / "render.txt";
            std::vector<Document> documents;
            for (const fs::path& path : files) {
                Document d{fs::relative(path, c.root).string(), c.description, fileSource(path, settings)};
                d.references = referencesOf(c.player, path, c.root);
                for (Player other : kPlayers) {
                    if (other == c.player)
                        continue;
                    for (Reference& reference : referencesOf(other, path, c.root))
                        d.references.push_back(std::move(reference));
                }
                documents.push_back(std::move(d));
            }
            return documents;
        },
    };
}

} // namespace

std::vector<Collection> lottieCollections() {
    fs::path lottie = fs::path(kThirdParty) / "lottie";
    const Corpus corpora[] = {
        {
            "Lottie: lottie-ios", "lottie-ios", lottie / "lottie-ios" / "Tests" / "Samples", "lottie-ios's samples",
            Player_LottieIos,
        },
        {"Lottie: Skottie", "skottie", lottie / "skia" / "resources" / "skottie", "Skottie's tests", Player_Skottie},
        {
            "Lottie: rlottie", "rlottie", lottie / "rlottie" / "example" / "resource", "rlottie's examples",
            Player_Rlottie,
        },
        {"Lottie: lottie-web", "lottie-web", lottie / "lottie-web", "lottie-web's animations", Player_LottieWeb},
    };
    std::vector<Collection> collections;
    for (const Corpus& c : corpora)
        if (fs::exists(c.root))
            collections.push_back(collectionOf(c));
    return collections;
}

} // namespace viewer
