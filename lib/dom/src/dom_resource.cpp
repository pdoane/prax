// Resource documents: the other documents that a document's references name.

#include "dom/src/dom_resource.h"

#include "pr/alloc.h"
#include "pr/array.h"
#include "pr/dom_xml.h"
#include "pr/memory.h"
#include "pr/string_ops.h"
#include "pr/url.h"

#include "dom/src/dom_hooks.h"
#include "dom/src/dom_use.h"
#include "dom/src/dom_xml.h"

namespace pr_impl {

// The state of a resource document's load.
enum {
    DomResourceState_Loading,
    DomResourceState_Loaded,
    DomResourceState_Failed,
};
typedef uint8_t DomResourceState;

// A resource document, by the URL that named it.
struct DomResource {
    PrString url;                       // absolute, without a fragment
    DomResourceState state;
    PrDomDocument* PR_NULLABLE document; // Loaded: the parsed document, which the resources hold a reference to
};

struct DomResources {
    Array<DomResource> documents;
    DomResourceLoadFn PR_NULLABLE load;
    void* PR_NULLABLE loadContext;
};

namespace {

// A fetch of a resource document. It holds a reference to the document that the reference is in.
struct ResourceFetch {
    PrDomDocument* document;
    uint32_t index;                     // into DomResources::documents
};

DomResources* resourcesOf(PrDomDocument* document) {
    if (document->resources == nullptr)
        document->resources = allocZeroed<DomResources>();
    return document->resources;
}

// Tells the readers of the document's references that a resource document has loaded. The document's uses resolve their
// hrefs again, and its presentation hears that what IDs name may have changed.
void loaded(PrDomDocument* document) {
    domUsesResolveAll(document);
    domNotifyResourceLoaded(document);
}

void dropFetch(void* PR_NULLABLE data) {
    ResourceFetch* fetch = static_cast<ResourceFetch*>(data);
    domNodeRelease(fetch->document);
    prMemoryFree(fetch);
}

// Parses a fetched resource document, which has no browsing context (SVG2/linking.html#processingURL-parsing).
void fetched(void* PR_NULLABLE data, const DomFetchResult* result) {
    ResourceFetch* fetch = static_cast<ResourceFetch*>(data);
    DomResource& resource = fetch->document->resources->documents.data[fetch->index];
    resource.state = DomResourceState_Failed;
    if (domFetchIsOk(result) && result->body.size != 0) {
        PrString decoded{};
        PrCharset charset = PrCharset_None;
        PrStringView text = domXmlDecode(result->body, PrCharset_None, &decoded, &charset);
        PrDomDocument* parsed = domDocumentCreate(DomDocumentKind_Xml);
        parsed->contentType = prStringExternal("image/svg+xml");
        domNotifyDocumentCreated(fetch->document, parsed);
        domXmlParse(parsed, text, {}, nullptr, nullptr);
        prStringReset(&decoded);
        parsed->charset = charset;
        prUrlCreate(prStringView(&resource.url), nullptr, PrCharset_Utf8, &parsed->url);
        resource.document = parsed;
        resource.state = DomResourceState_Loaded;
    }
    loaded(fetch->document);
}

void fetchedAndDropped(void* PR_NULLABLE data, const DomFetchResult* result) {
    fetched(data, result);
    dropFetch(data);
}

// Starts the load of the resource at `index`.
void load(PrDomDocument* document, uint32_t index) {
    DomResources* resources = document->resources;
    DomResource& resource = resources->documents.data[index];
    const DomDocumentHost* host = document->host;
    if (host == nullptr) {
        if (resources->load != nullptr)
            resource.document = resources->load(resources->loadContext, document, prStringView(&resource.url));
        else
            resource.document = nullptr;
        resource.state = resource.document != nullptr ? DomResourceState_Loaded : DomResourceState_Failed;
        if (resource.document != nullptr)
            prDomDocumentRetain(resource.document);
        return;
    }
    PrUrl* url = nullptr;
    bool allowed = document->url != nullptr &&
        prUrlCreate(prStringView(&resource.url), nullptr, PrCharset_Utf8, &url) == PrStatus_Ok &&
        prUrlHasSameOrigin(document->url, url);
    prUrlDestroy(url);
    if (!allowed) {
        resource.state = DomResourceState_Failed;
        return;
    }
    ResourceFetch* fetch = allocZeroed<ResourceFetch>();
    domNodeRetain(document);
    fetch->document = document;
    fetch->index = index;
    // The URL is copied, since a result that arrives during the call may grow the list that holds it.
    PrString request = prStringClone(&resource.url);
    DomFetchRequest asked{
        prStringView(&request), DomFetchDestination_Image, DomCorsSettings_NoCors, DomFetchInitiator_Css,
    };
    host->fetch(host->context, &asked, fetchedAndDropped, fetch, dropFetch);
    prStringReset(&request);
}

} // namespace

bool domReferenceSplit(const PrDomDocument* document, PrStringView reference, DomReference* out) {
    *out = DomReference{};
    // Leading and trailing white space is stripped (SVG2/linking.html#processingURL).
    reference = stringTrim(reference);
    if (reference.size == 0)
        return false;
    size_t hash = 0;
    while (hash < reference.size && reference.data[hash] != '#')
        ++hash;
    if (hash < reference.size)
        out->fragment = PrStringView{reference.data + hash + 1, reference.size - hash - 1};
    if (hash == 0)
        return true;
    PrUrl* base = domDocumentBaseUrl(document);
    PrUrl* url = nullptr;
    if (base != nullptr && prUrlCreate(reference, base, document->charset, &url) == PrStatus_Ok) {
        out->url = prUrlSerialize(url, PrUrlSerialization_ExcludeFragment);
        PrString own = prUrlSerialize(base, PrUrlSerialization_ExcludeFragment);
        out->external = prStringView(&own) != prStringView(&out->url);
        prStringReset(&own);
        if (!out->external)
            prStringReset(&out->url);
    } else {
        // A relative reference in a document whose URL cannot serve as a base, such as the about:blank of a document
        // that a harness parses from a file, names another document by the URL as written. A loader may resolve it.
        out->external = true;
        out->url = prStringCopy(PrStringView{reference.data, hash});
    }
    prUrlDestroy(url);
    prUrlDestroy(base);
    return true;
}

PrDomDocument* PR_NULLABLE domResourceDocument(PrDomDocument* document, PrStringView url) {
    DomResources* resources = resourcesOf(document);
    for (uint32_t i = 0; i < resources->documents.count; ++i) {
        const DomResource& resource = resources->documents.data[i];
        if (prStringView(&resource.url) == url)
            return resource.state == DomResourceState_Loaded ? resource.document : nullptr;
    }
    uint32_t index = resources->documents.count;
    resources->documents.push(DomResource{.url = prStringCopy(url), .state = DomResourceState_Loading});
    load(document, index);
    const DomResource& resource = resources->documents.data[index];
    return resource.state == DomResourceState_Loaded ? resource.document : nullptr;
}

const PrDomElement* PR_NULLABLE domReferenceTarget(PrDomDocument* document, const DomReference& reference) {
    PrDomDocument* in = document;
    if (reference.external) {
        in = domResourceDocument(document, prStringView(&reference.url));
        if (in == nullptr)
            return nullptr;
        if (reference.fragment.data == nullptr)
            return prDomDocumentElement(in);
    }
    if (reference.fragment.size == 0)
        return nullptr;
    return domElementById(in, reference.fragment);
}

void domResourcesSetLoader(PrDomDocument* document, DomResourceLoadFn PR_NULLABLE load, void* PR_NULLABLE context) {
    if (load == nullptr && document->resources == nullptr)
        return;
    DomResources* resources = resourcesOf(document);
    resources->load = load;
    resources->loadContext = context;
}

void domResourcesDestroy(DomResources* PR_NULLABLE resources) {
    if (resources == nullptr)
        return;
    for (uint32_t i = 0; i < resources->documents.count; ++i) {
        prStringReset(&resources->documents.data[i].url);
        prDomDocumentRelease(resources->documents.data[i].document);
    }
    resources->documents.reset();
    prMemoryFree(resources);
}

} // namespace pr_impl
