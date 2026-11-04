// The state's verbs that work over the gallery's files: the record's groups, and the import.

#include "pr/array.h"
#include "pr/compress.h"
#include "pr/file.h"
#include "pr/image.h"
#include "pr/random.h"
#include "pr/string.h"

#include "examples/alchemy/state.h"

#include "tools/harness/test.h"

using namespace alchemy;

namespace {

// Returns the strings of `items`.
template <uint32_t N>
Array<String> strings(const char* const (&items)[N]) {
    Array<String> out;
    for (const char* item : items)
        out.push(item);
    return out;
}

// A state over a gallery in a directory of its own, with the codecs that the verbs use.
struct Fixture {
    AppState state;
    String dir;

    Fixture() {
        PrRawString cache = prFileCacheDirectory();
        dir = format("{}/prax-tests/alchemy_state_{}", prRawStringView(&cache), prRandomSeed());
        prRawStringReset(&cache);
        state.compress = prCompressCodecCreate(PrCompressCodecBackend_Default);
        state.codec = prImageCodecCreate(PrImageCodecBackend_Default, state.compress);
        galleryOpen(state.gallery, dir);
    }

    ~Fixture() {
        for (const GalleryEntry& entry : state.gallery.entries)
            prFileRemove(entry.path);
        closeImport(state);
        // The gallery's directory holds only its images, which are removed above.
        prFileRemove(dir);
        prImageCodecDestroy(state.codec);
        prCompressCodecDestroy(state.compress);
    }

    // Saves a 2 by 1 image that carries `record`, which the gallery selects.
    void addImage(const ProvenanceRecord& record) {
        static const uint8_t kPixels[] = {255, 0, 0, 255, 0, 255, 0, 128};
        stageImport(state, PrPixelView{kPixels, 8, PrUVec2{2, 1}});
        REQUIRE(confirmImport(state));
        const GalleryEntry* entry = gallerySelected(state.gallery);
        REQUIRE(entry != nullptr);
        PrFileMapping mapping{};
        REQUIRE(prFileMap(entry->path, &mapping));
        Array<uint8_t> png = provenanceWrite(PrByteView{mapping.data, mapping.size}, record);
        prFileUnmap(&mapping);
        REQUIRE(prFileWrite(entry->path, png));
        galleryRescan(state.gallery);
    }
};

ProvenanceRecord chain() {
    ProvenanceOp generate;
    generate.mode = "generate";
    generate.prompt = "first line\nsecond line";
    generate.negative = "blurry";
    generate.sampler = "euler";
    generate.scheduler = "karras";
    generate.steps = 31;
    generate.cfg = 6.5;
    generate.seed = 77;
    generate.width = 1024;
    generate.height = 768;
    ProvenanceOp upscale;
    upscale.mode = "upscale";
    upscale.upscaleModel = "4x";
    upscale.factor = 3.0;
    upscale.source = "older.png";
    ProvenanceRecord record;
    record.operations.push(generate);
    record.operations.push(upscale);
    return record;
}

} // namespace

TEST_CASE("a paste stages its image, and only the confirm adds it to the gallery") {
    Fixture f;
    static const uint8_t kPixels[] = {10, 20, 30, 255, 40, 50, 60, 128};
    openImport(f.state);
    stageImport(f.state, PrPixelView{kPixels, 8, PrUVec2{2, 1}});
    CHECK(f.state.import.image != nullptr);
    CHECK(f.state.gallery.entries.isEmpty());
    REQUIRE(confirmImport(f.state));
    CHECK(!f.state.import.open);
    CHECK(f.state.import.image == nullptr);
    REQUIRE(f.state.gallery.entries.count() == 1);

    // The file holds what was staged, which a copy of the selected image reads back.
    Array<uint8_t> pixels;
    PrUVec2 extent = selectedPixels(f.state, &pixels);
    CHECK(extent.x == 2);
    CHECK(extent.y == 1);
    CHECK(pixels == Array<uint8_t>(kPixels));
}

TEST_CASE("a paste of no image says so and keeps what was staged") {
    Fixture f;
    static const uint8_t kPixels[] = {1, 2, 3, 255};
    stageImport(f.state, PrPixelView{kPixels, 4, PrUVec2{1, 1}});
    const PrDrawImage* staged = f.state.import.image;
    stageImport(f.state, PrPixelView{nullptr, 0, PrUVec2{0, 0}});
    CHECK(f.state.import.image == staged);
    CHECK(!f.state.import.message.isEmpty());
    closeImport(f.state);
    CHECK(!f.state.import.open);
    CHECK(f.state.gallery.entries.isEmpty());
}

TEST_CASE("a record's verbs carry one group of the operation that they name") {
    Fixture f;
    f.state.catalog.samplers = strings({"dpm", "euler"});
    f.state.catalog.schedulers = strings({"normal", "karras"});
    f.state.catalog.upscaleModels = strings({"2x", "4x"});
    f.addImage(chain());
    GenerateParams& p = f.state.params;

    SUBCASE("the newest operation opens first") {
        REQUIRE(selectedRecord(f.state) != nullptr);
        CHECK(f.state.recordOpen == Array<bool>({false, true}));
    }
    SUBCASE("a group leaves the rest alone") {
        CHECK(useRecorded(f.state, 0, UseWhat_Sampling));
        CHECK(p.sampler == "euler");
        CHECK(p.scheduler == "karras");
        CHECK(p.steps == 31);
        CHECK(p.cfg == 6.5);
        CHECK(p.prompt.isEmpty());
        CHECK(!p.seedLocked);
    }
    SUBCASE("a line joins the prompt being written") {
        p.prompt = "mine";
        CHECK(useRecorded(f.state, 0, UseWhat_PromptLine, 1));
        CHECK(p.prompt == "mine\nsecond line");
        CHECK(!useRecorded(f.state, 0, UseWhat_PromptLine, 2));
    }
    SUBCASE("a seed locks") {
        CHECK(useRecorded(f.state, 0, UseWhat_Seed));
        CHECK(p.seed == 77);
        CHECK(p.seedLocked);
    }
    SUBCASE("an older operation stays reachable, and one does not state another's groups") {
        CHECK(useRecorded(f.state, 1, UseWhat_Upscale));
        CHECK(f.state.upscale.model == "4x");
        CHECK(f.state.upscale.factor == 3.0);
        CHECK(!useRecorded(f.state, 1, UseWhat_Prompt));
        CHECK(!useRecorded(f.state, 2, UseWhat_Prompt));
    }
}

TEST_CASE("the names of the record's groups parse") {
    CHECK(parseUseWhat("promptLine") == UseWhat_PromptLine);
    CHECK(parseUseWhat("all") == UseWhat_All);
    CHECK(parseUseWhat("nothing") == UseWhat_Count);
}
