// Web Storage: the areas that a window's Storage objects reach, by origin, in a provider in memory or on disk.
//
// html - https://html.spec.whatwg.org/multipage/webstorage.html

#include "pr/html_page.h"
#include "pr/html_storage.h"
#include "pr/task_builtin.h"

#include "doctest/doctest.h"

#include <string>

namespace pr_impl {

namespace {

std::string textOf(PrString value) {
    PrStringView view = prStringView(&value);
    std::string text = prStringIsNull(&value) ? "<null>" : std::string(view.data, view.size);
    prStringReset(&value);
    return text;
}

// A browsing context whose window's document is at `url`, with the providers given.
PrHtmlPage* openAt(PrHtmlStorageProvider* local, PrHtmlStorageProvider* PR_NULLABLE session, const char* url) {
    PrHtmlHost host = prHtmlHostDefault();
    host.localStorage = local;
    host.sessionStorage = session;
    PrHtmlPage* page = prHtmlPageCreate(&host);
    REQUIRE(prHtmlPageLoad(page, "<p>", url, nullptr) == PrStatus_Ok);
    return page;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a window's storage is its document's origin's area, which a provider on disk keeps") {
    const std::string directory = std::string(PR_HTML_TEST_DIR) + "/storage";
    PrTaskPool* pool = prTaskBuiltinCreate(1);
    PrHtmlStorageProviderDesc disk{.directory = directory, .quotaBytes = 16, .pool = pool};
    PrHtmlStorageProvider* local = prHtmlStorageProviderCreate(&disk);
    PrHtmlStorageProviderDesc memory{.quotaBytes = 16};
    PrHtmlStorageProvider* session = prHtmlStorageProviderCreate(&memory);

    PrHtmlPage* page = openAt(local, session, "https://a.test/page");
    PrHtmlWindow* window = prHtmlPageWindow(page);
    PrHtmlStorage* storage = prHtmlWindowStorage(window, PrHtmlStorageKind_Local);
    REQUIRE(storage != nullptr);
    prHtmlStorageClear(storage);
    CHECK(prHtmlStorageSetItem(storage, "key", "value") == PrStatus_Ok);
    CHECK(prHtmlStorageLength(storage) == 1);
    CHECK(textOf(prHtmlStorageKey(storage, 0)) == "key");
    CHECK(textOf(prHtmlStorageKey(storage, 1)) == "<null>");
    CHECK(textOf(prHtmlStorageGetItem(storage, "key")) == "value");

    // Past the quota, which counts keys, the item keeps its value.
    CHECK(prHtmlStorageSetItem(storage, "key", "a value too long") == PrStatus_Limit);
    CHECK(textOf(prHtmlStorageGetItem(storage, "key")) == "value");

    // Session storage is another area.
    PrHtmlStorage* sessionStorage = prHtmlWindowStorage(window, PrHtmlStorageKind_Session);
    REQUIRE(sessionStorage != nullptr);
    CHECK(textOf(prHtmlStorageGetItem(sessionStorage, "key")) == "<null>");

    // A document of another origin has a window whose storage is that origin's area, and an opaque origin has none.
    REQUIRE(prHtmlPageLoad(page, "<p>", "https://b.test/", nullptr) == PrStatus_Ok);
    storage = prHtmlWindowStorage(prHtmlPageWindow(page), PrHtmlStorageKind_Local);
    REQUIRE(storage != nullptr);
    CHECK(prHtmlStorageLength(storage) == 0);
    REQUIRE(prHtmlPageLoad(page, "<p>", "data:text/html,", nullptr) == PrStatus_Ok);
    CHECK(prHtmlWindowStorage(prHtmlPageWindow(page), PrHtmlStorageKind_Local) == nullptr);
    prHtmlPageDestroy(page);

    // Without a provider of a kind, there is no storage of that kind.
    page = openAt(local, nullptr, "https://a.test/");
    CHECK(prHtmlWindowStorage(prHtmlPageWindow(page), PrHtmlStorageKind_Session) == nullptr);
    prHtmlPageDestroy(page);
    prHtmlStorageProviderDestroy(local);
    prHtmlStorageProviderDestroy(session);

    // A provider opened again on the same directory has the items.
    local = prHtmlStorageProviderCreate(&disk);
    page = openAt(local, nullptr, "https://a.test/other");
    storage = prHtmlWindowStorage(prHtmlPageWindow(page), PrHtmlStorageKind_Local);
    REQUIRE(storage != nullptr);
    CHECK(textOf(prHtmlStorageGetItem(storage, "key")) == "value");
    prHtmlStorageClear(storage);
    prHtmlPageDestroy(page);
    prHtmlStorageProviderDestroy(local);
    prTaskPoolDestroy(pool);
}

TEST_CASE("clearing a provider empties every origin's area, including those that an earlier provider left on disk") {
    const std::string directory = std::string(PR_HTML_TEST_DIR) + "/storage_clear";
    PrTaskPool* pool = prTaskBuiltinCreate(1);
    PrHtmlStorageProviderDesc disk{.directory = directory, .quotaBytes = 16, .pool = pool};
    auto setAt = [](PrHtmlStorageProvider* provider, const char* url, const char* value) {
        PrHtmlPage* page = openAt(provider, nullptr, url);
        PrHtmlStorage* storage = prHtmlWindowStorage(prHtmlPageWindow(page), PrHtmlStorageKind_Local);
        REQUIRE(storage != nullptr);
        CHECK(prHtmlStorageSetItem(storage, "key", value) == PrStatus_Ok);
        prHtmlPageDestroy(page);
    };
    auto valueAt = [](PrHtmlStorageProvider* provider, const char* url) {
        PrHtmlPage* page = openAt(provider, nullptr, url);
        PrHtmlStorage* storage = prHtmlWindowStorage(prHtmlPageWindow(page), PrHtmlStorageKind_Local);
        REQUIRE(storage != nullptr);
        std::string value = textOf(prHtmlStorageGetItem(storage, "key"));
        prHtmlPageDestroy(page);
        return value;
    };
    PrHtmlStorageProvider* earlier = prHtmlStorageProviderCreate(&disk);
    setAt(earlier, "https://a.test/", "a");
    setAt(earlier, "https://b.test/", "b");
    prHtmlStorageProviderDestroy(earlier);

    // In a later run, b.test opens its area before the clear, and a.test's area is only on disk.
    PrHtmlStorageProvider* local = prHtmlStorageProviderCreate(&disk);
    CHECK(valueAt(local, "https://b.test/") == "b");
    prHtmlStorageProviderClear(local);
    CHECK(valueAt(local, "https://a.test/") == "<null>");
    CHECK(valueAt(local, "https://b.test/") == "<null>");
    prHtmlStorageProviderDestroy(local);

    // A provider in memory clears its areas in the same way.
    PrHtmlStorageProviderDesc memory{.quotaBytes = 16};
    PrHtmlStorageProvider* session = prHtmlStorageProviderCreate(&memory);
    setAt(session, "https://a.test/", "a");
    prHtmlStorageProviderClear(session);
    CHECK(valueAt(session, "https://a.test/") == "<null>");
    prHtmlStorageProviderDestroy(session);
    prTaskPoolDestroy(pool);
}
