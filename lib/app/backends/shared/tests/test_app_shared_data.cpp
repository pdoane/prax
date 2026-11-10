// Data transfer: stores copy content, sources live as long as their references, requests answer once, and a window's
// answer to a drag respects the source's operations.

#include "pr/alloc.h"
#include "pr/app.h"

#include "app/backends/shared/src/app_shared_data.h"
#include "app/backends/shared/src/app_shared_drag.h"

#include "tools/harness/test.h"

#include <string>
#include <vector>

namespace pr_impl {

namespace {

// Returns the name of a status as a std::string.
std::string statusText(PrStatus status) {
    PrStringView name = prStatusName(status);
    return std::string(name.data, name.size);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a store copies each form of the content and gives it back") {
    char text[] = "hello";
    char first[] = "/tmp/a.txt";
    char second[] = "/tmp/b";
    PrStringView paths[] = {first, second};
    PrStringView urls[] = {"https://example.com/"};
    const uint8_t bytes[] = {1, 2, 3};
    PrAppDataCustom custom[] = {{.type = "org.prax.test", .bytes = PrByteView{bytes, 3}}};
    // A 2x1 image in rows of 12 bytes, of which the store keeps the first 8.
    uint8_t pixels[12] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 9, 9, 9};
    PrAppDataContent content{
        .forms = PrAppDataFormFlag_Text | PrAppDataFormFlag_Html | PrAppDataFormFlag_Urls | PrAppDataFormFlag_Files |
            PrAppDataFormFlag_Image | PrAppDataFormFlag_Custom | PrAppDataFormFlag_Payload,
        .text = text,
        .html = "<b>hello</b>",
        .urls = urls,
        .urlCount = 1,
        .paths = paths,
        .pathCount = 2,
        .image = {pixels, 12, {2, 1}},
        .custom = custom,
        .customCount = 1,
        .payload = {.kind = 3, .value = 4},
    };
    AppDataStore store{};
    appDataStoreSet(store, content);
    // The copy outlives the original.
    text[0] = 'j';
    first[1] = 'x';
    pixels[0] = 0;
    PrAppDataContent copy = appDataStoreContent(store);
    CHECK(copy.forms == content.forms);
    CHECK(copy.text == "hello");
    CHECK(copy.html == "<b>hello</b>");
    REQUIRE(copy.urlCount == 1);
    CHECK(copy.urls[0] == "https://example.com/");
    REQUIRE(copy.pathCount == 2);
    CHECK(copy.paths[0] == "/tmp/a.txt");
    CHECK(copy.paths[1] == "/tmp/b");
    CHECK(copy.image.extent.x == 2);
    CHECK(copy.image.stride == 8);
    CHECK(copy.image.data[0] == 1);
    CHECK(copy.image.data[7] == 8);
    CHECK(appDataStoreCustom(store, "org.prax.test").size == 3);
    CHECK(appDataStoreCustom(store, "org.prax.other").size == 0);
    CHECK(copy.payload.kind == 3);
    CHECK(copy.payload.value == 4);

    // A path's file is named for its last component.
    uint32_t count = 0;
    const PrAppDataFile* files = appDataStoreFiles(store, &count);
    REQUIRE(count == 2);
    CHECK(files[0].name == "a.txt");
    CHECK(files[0].path == "/tmp/a.txt");
    CHECK(files[1].name == "b");

    // Only the forms that the content names are copied.
    appDataStoreSet(store, PrAppDataContent{.forms = PrAppDataFormFlag_Text, .text = "only", .pathCount = 2});
    copy = appDataStoreContent(store);
    CHECK(copy.forms == PrAppDataFormFlag_Text);
    CHECK(copy.text == "only");
    CHECK(copy.pathCount == 0);
    CHECK(copy.image.data == nullptr);
    appDataStoreReset(store);
}

TEST_CASE("a pending drag copies its description, promises and preview included") {
    uint8_t preview[4] = {9, 8, 7, 6};
    PrAppDragPromise promises[] = {{.name = "drawing.png"}};
    PrAppDragDesc desc{
        .content = {.forms = PrAppDataFormFlag_Text, .text = "dragged"},
        .promises = promises,
        .promiseCount = 1,
        .operations = PrAppDragOperationFlag_Copy,
        .preview = {preview, 4, {1, 1}},
        .previewOffset = {2, 3},
    };
    AppDragPending pending{};
    CHECK(!pending.pending);
    appDragPendingSet(pending, desc);
    CHECK(pending.pending);
    CHECK(appDataStoreContent(pending.content).text == "dragged");
    CHECK(appStringsViews(pending.promises)[0] == "drawing.png");
    CHECK(pending.operations == PrAppDragOperationFlag_Copy);
    CHECK(pending.previewExtent.x == 1);
    CHECK(pending.preview.data[0] == 9);
    CHECK(pending.previewOffset.y == 3);
    appDragPendingReset(pending);
    CHECK(!pending.pending);
}

TEST_CASE("a source lives until its last reference goes, which a request holds until it answers") {
    AppDataSources sources{};
    appDataSourcesInit(sources);
    static int freed = 0;
    freed = 0;
    sources.releaseNative = [](void*) { ++freed; };
    AppDataRequests requests{};
    appDataRequestsInit(requests, sources);

    AppDataStore* store = allocZeroed<AppDataStore>();
    appDataStoreSetText(*store, "dropped");
    appDataStoreAddFile(*store, PrAppDataFile{.name = "a", .path = "/tmp/a", .size = 5});
    static int native = 0;
    PrAppDataSource source = appDataSourceCreate(sources, store, &native);
    CHECK(source != kPrAppDataSourceNone);
    CHECK(source != kPrAppDataSourceClipboard);
    CHECK(source != kPrAppDataSourceSelection);

    std::vector<std::string> seen;
    auto text = [](void* context, PrStatus status, PrStringView value) {
        static_cast<std::vector<std::string>*>(context)->push_back(statusText(status) + ":" +
            std::string(value.data, value.size));
    };
    auto files = [](void* context, PrStatus, const PrAppDataFile* entries, uint32_t count) {
        for (uint32_t i = 0; i < count; ++i)
            static_cast<std::vector<std::string>*>(context)->push_back(std::string(entries[i].name.data,
                entries[i].name.size) + " " + std::to_string(entries[i].size));
    };
    uint32_t read = appDataRequestsAdd(requests, source, PrAppDataReadKind_Text, (void (*)(void))+text, &seen);
    uint32_t listed = appDataRequestsAdd(requests, source, PrAppDataReadKind_Files, (void (*)(void))+files, &seen);
    uint32_t cancelled = appDataRequestsAdd(requests, source, PrAppDataReadKind_Text, (void (*)(void))+text, &freed);
    PrAppDataSource found = kPrAppDataSourceNone;
    AppDataAnswer answer = PrAppDataReadKind_Text;
    REQUIRE(appDataRequestsFind(requests, listed, &found, &answer));
    CHECK(found == source);
    CHECK(answer == PrAppDataReadKind_Files);

    // The event that handed the source over is done with it, and the requests still hold it.
    appDataSourceRelease(sources, source);
    CHECK(appDataSourceStore(sources, source) == store);
    appDataRequestsCancel(requests, &freed);
    appDataRequestsAnswerFromStore(requests, read, PrStatus_Ok, appDataSourceStore(sources, source));
    CHECK(freed == 0);
    appDataRequestsAnswerFromStore(requests, listed, PrStatus_Ok, appDataSourceStore(sources, source));
    CHECK(seen == std::vector<std::string>{":dropped", "a 5"});
    // The last answer released the last reference, and an answered or cancelled request is gone.
    CHECK(freed == 1);
    CHECK(appDataSourceStore(sources, source) == nullptr);
    CHECK_FALSE(appDataRequestsFind(requests, cancelled, &found, &answer));
    appDataRequestsAnswerFromStore(requests, read, PrStatus_Ok, nullptr);
    CHECK(seen.size() == 2);
    appDataRequestsReset(requests);
    appDataSourcesReset(sources);
}

TEST_CASE("a window's answer is its operation if the source allows it, and None if not") {
    PrAppDragOperationFlags copyOrMove = PrAppDragOperationFlag_Copy | PrAppDragOperationFlag_Move;
    CHECK(appDragAnswer(PrAppDragOperation_Move, copyOrMove) == PrAppDragOperation_Move);
    CHECK(appDragAnswer(PrAppDragOperation_Link, copyOrMove) == PrAppDragOperation_None);
    CHECK(appDragAnswer(PrAppDragOperation_None, PrAppDragOperationFlag_All) == PrAppDragOperation_None);
}
