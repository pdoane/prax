// Resource documents: the other documents that references name, fetched once and kept.

#include "pr/dom.h"
#include "pr/dom_xml.h"
#include "pr/string_ops.h"
#include "pr/url.h"

#include "dom/src/dom_node.h"
#include "dom/src/dom_node_leaves.h"
#include "dom/src/dom_resource.h"
#include "dom/src/dom_use.h"

#include "tools/harness/test.h"

#include <string>
#include <vector>

namespace pr_impl {

namespace {

// A document at an https URL whose host holds each fetch until the test answers it.
struct HostedDocument {
    struct HeldFetch {
        std::string url;
        DomFetchFn done;
        void* data;
        void (*drop)(void*);
    };

    std::vector<HeldFetch> fetches;
    DomDocumentHost host{};
    PrDomDocument* document = nullptr;

    explicit HostedDocument(const char* source) {
        document = prDomParseXml(source, "image/svg+xml", nullptr);
        REQUIRE(document != nullptr);
        prUrlDestroy(document->url);
        document->url = nullptr;
        REQUIRE(prUrlCreate("https://example.test/dir/page.svg", nullptr, PrCharset_Utf8, &document->url) ==
            PrStatus_Ok);
        host.context = this;
        host.fetch = [](void* context, const DomFetchRequest* request, DomFetchFn done, void* data, void (*drop)(void*)) {
            static_cast<HostedDocument*>(context)->fetches.push_back(
                HeldFetch{std::string(request->url.data, request->url.size), done, data, drop});
        };
        document->host = &host;
    }

    ~HostedDocument() {
        for (HeldFetch& fetch : fetches) {
            if (fetch.drop != nullptr)
                fetch.drop(fetch.data);
        }
        document->host = nullptr;
        prDomDocumentRelease(document);
    }

    // Answers the first fetch waiting with `body`. A fetch that is answered is not dropped.
    void answer(const char* body) {
        REQUIRE(!fetches.empty());
        HeldFetch fetch = fetches.front();
        fetches.erase(fetches.begin());
        DomFetchResult result{.status = PrStatus_Ok, .responseStatus = 200, .body = body};
        fetch.done(fetch.data, &result);
    }

    PrDomElement* byId(const char* id) {
        PrDomElement* element = prDomNodeGetElementById(prDomDocumentAsNode(document), id);
        REQUIRE(element != nullptr);
        return element;
    }

    // Returns the instance root of `use`, brought up to date.
    PrDomElement* instanceOf(PrDomElement* use) {
        domUsesSync(document);
        const PrDomShadowRoot* shadow = domShadowRootOfHost(use);
        return shadow != nullptr ? domElementFromNode(shadow->first) : nullptr;
    }
};

std::string urlOf(const DomReference& reference) {
    PrStringView url = prRawStringView(&reference.url);
    return url.data != nullptr ? std::string(url.data, url.size) : std::string();
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a reference without its surrounding white space names the document itself by a fragment alone or by its "
        "own URL, and another document otherwise") {
    HostedDocument d("<svg xmlns='http://www.w3.org/2000/svg'/>");
    DomReference reference{};
    REQUIRE(domReferenceSplit(d.document, " #a\n", &reference));
    CHECK_FALSE(reference.external);
    CHECK(reference.fragment == "a");
    REQUIRE(domReferenceSplit(d.document, "page.svg#b", &reference));
    CHECK_FALSE(reference.external);
    CHECK(reference.fragment == "b");
    REQUIRE(domReferenceSplit(d.document, "defs.svg#c", &reference));
    CHECK(reference.external);
    CHECK(urlOf(reference) == "https://example.test/dir/defs.svg");
    CHECK(reference.fragment == "c");
    prRawStringReset(&reference.url);
    REQUIRE(domReferenceSplit(d.document, "../other.svg", &reference));
    CHECK(urlOf(reference) == "https://example.test/other.svg");
    CHECK(reference.fragment.data == nullptr);
    prRawStringReset(&reference.url);
    CHECK_FALSE(domReferenceSplit(d.document, "", &reference));
}

TEST_CASE("a use instances an element of another document once the document loads") {
    HostedDocument d("<svg xmlns='http://www.w3.org/2000/svg'><use id='use' href='defs.svg#square'/>"
        "<use id='again' href='defs.svg'/></svg>");
    PrDomElement* use = d.byId("use");
    CHECK(d.instanceOf(use) == nullptr);
    // Both uses name one document, which is fetched once.
    REQUIRE(d.fetches.size() == 1);
    CHECK(d.fetches[0].url == "https://example.test/dir/defs.svg");
    d.answer("<svg xmlns='http://www.w3.org/2000/svg'><rect id='square' width='10'/></svg>");
    PrDomElement* instance = d.instanceOf(use);
    REQUIRE(instance != nullptr);
    CHECK(domElementName(instance) == "rect");
    CHECK(domAttributeValue(instance, "width") == "10");
    CHECK(instance->document == d.document);
    PrDomElement* root = d.instanceOf(d.byId("again"));
    REQUIRE(root != nullptr);
    CHECK(domElementName(root) == "svg");
}

TEST_CASE("a reference to another origin, or a document that fails to load, names nothing") {
    HostedDocument d("<svg xmlns='http://www.w3.org/2000/svg'><use id='other' href='https://elsewhere.test/a.svg#a'/>"
        "<use id='broken' href='broken.svg#a'/></svg>");
    CHECK(d.instanceOf(d.byId("other")) == nullptr);
    REQUIRE(d.fetches.size() == 1);
    CHECK(d.fetches[0].url == "https://example.test/dir/broken.svg");
    d.answer("");
    CHECK(d.instanceOf(d.byId("broken")) == nullptr);
    CHECK(d.fetches.empty());
}

TEST_CASE("a document without a browsing context takes resource documents from its loader") {
    PrDomDocument* document = prDomParseXml("<svg xmlns='http://www.w3.org/2000/svg'><use id='use' href='lib.svg#a'/>"
        "</svg>", "image/svg+xml", nullptr);
    PrDomDocument* library =
        prDomParseXml("<svg xmlns='http://www.w3.org/2000/svg'><circle id='a' r='3'/></svg>", "image/svg+xml", nullptr);
    struct Loader {
        PrDomDocument* library;
        std::vector<std::string> urls;
    } loader{library, {}};
    domResourcesSetLoader(document, [](void* context, PrDomDocument*, PrStringView url) -> PrDomDocument* {
        Loader* l = static_cast<Loader*>(context);
        l->urls.emplace_back(url.data, url.size);
        return l->library;
    }, &loader);
    domUsesSync(document);
    PrDomElement* use = prDomNodeGetElementById(prDomDocumentAsNode(document), "use");
    const PrDomShadowRoot* shadow = domShadowRootOfHost(use);
    REQUIRE(shadow != nullptr);
    REQUIRE(domElementFromNode(shadow->first) != nullptr);
    CHECK(domElementName(domElementFromNode(shadow->first)) == "circle");
    REQUIRE(loader.urls.size() == 1);
    CHECK(loader.urls[0] == "lib.svg");
    domResourcesSetLoader(document, nullptr, nullptr);
    prDomDocumentRelease(library);
    prDomDocumentRelease(document);
}
