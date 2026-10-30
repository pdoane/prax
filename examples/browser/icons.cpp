// Icons: the images of the icons of the tabs' pages, by URL.
//
// A miss fetches the icon on the network's loop, which posts the response to the cache. The cache's pump on the
// window's thread decodes it and adopts the image.

#include "examples/browser/icons.h"

#include "pr/fetch_load.h"

namespace browser {

namespace {

// The largest icon that the browser decodes, in bytes.
constexpr uint64_t kIconMaxBytes = uint64_t(1) << 20;

// The frames that may pass without a tab showing an icon before the cache frees it.
constexpr uint32_t kIconIdleFrames = 600;

// One icon's load in the cache, and the fetch of its bytes.
struct IconLoad {
    Browser* browser;
    PrCacheLoad* load;
    PrFetchLoad* PR_NULLABLE fetch = nullptr;
};

void postToCache(void* PR_NULLABLE cache, PrFetchTaskFn fn, void* PR_NULLABLE context) {
    prCachePost(static_cast<PrCache*>(cache), fn, context);
}

// Decodes the icon that arrived, and finishes its load with the image, or with none.
void iconFetched(void* PR_NULLABLE context, const PrFetchResponse* response) {
    IconLoad* icon = static_cast<IconLoad*>(context);
    const PrDrawImage* image = nullptr;
    if (response->status == PrStatus_Ok && prFetchIsOkStatus(response->responseStatus))
        prDrawImageCreateFromBytes(icon->browser->images, response->body, &image, nullptr);
    prCacheLoadFinish(icon->load, image);
    prFetchLoadDestroy(icon->fetch);
    delete icon;
}

void* loadIcon(void* PR_NULLABLE context, PrStringView url, PrCacheLoad* load) {
    Browser& b = *static_cast<Browser*>(context);
    IconLoad* icon = new IconLoad{.browser = &b, .load = load};
    PrFetchRequestDesc request{.url = url, .destination = PrFetchDestination_Image, .mode = PrFetchMode_NoCors};
    PrFetchLoadDesc desc{
        .post = postToCache,
        .queue = b.icons,
        .done = iconFetched,
        .context = icon,
        .maxBodySize = kIconMaxBytes,
    };
    icon->fetch = prFetchLoadCreate(b.fetcher, &request, &desc);
    return icon;
}

void cancelIcon(void* PR_NULLABLE, void* PR_NULLABLE state) {
    IconLoad* icon = static_cast<IconLoad*>(state);
    prFetchLoadDestroy(icon->fetch);
    prCacheLoadFinish(icon->load, nullptr);
    delete icon;
}

void releaseIcon(void* PR_NULLABLE, const void* image) {
    prDrawImageRelease(static_cast<const PrDrawImage*>(image));
}

void wakeWindow(void* PR_NULLABLE context) {
    browserWantFrame(*static_cast<Browser*>(context));
}

} // namespace

PrCache* iconsCreate(Browser& b) {
    PrCacheDesc desc{
        .load = loadIcon,
        .cancel = cancelIcon,
        .release = releaseIcon,
        .wake = wakeWindow,
        .context = &b,
        .idlePumps = kIconIdleFrames,
    };
    return prCacheCreate(&desc);
}

const PrDrawImage* PR_NULLABLE iconsGet(Browser& b, const std::string& url) {
    if (url.empty())
        return nullptr;
    return static_cast<const PrDrawImage*>(prCacheValue(b.icons, url));
}

} // namespace browser
