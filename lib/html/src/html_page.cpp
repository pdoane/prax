// Native hosting of HTML documents.
//
// html - https://html.spec.whatwg.org/multipage/document-sequences.html

#include "pr/html_page.h"
#include "html/src/html_page.h"

#include "pr/alloc.h"
#include "pr/ascii.h"
#include "pr/assert.h"
#include "pr/charset.h"
#include "pr/css.h"
#include "pr/css_presentation.h"
#include "pr/dom_xml.h"
#include "pr/float.h"
#include "pr/mime.h"
#include "pr/net.h"
#include "pr/string_ops.h"
#include "pr/svg.h"
#include "pr/svg_render.h"
#include "pr/text_buffer.h"
#include "pr/text_font.h"
#include "pr/url.h"
#include "pr/url_data.h"

#include "css/src/css_font_face.h"
#include "css/src/css_hints.h"
#include "css/src/css_presentation.h"
#include "css/src/css_presentation_hooks.h"
#include "css/src/css_root.h"
#include "css/src/css_serialize.h"
#include "dom/src/dom_attribute.h"
#include "dom/src/dom_custom.h"
#include "dom/src/dom_element_state.h"
#include "dom/src/dom_microsyntax.h"
#include "dom/src/dom_resource.h"
#include "html/src/html_canvas.h"
#include "html/src/html_document.h"
#include "html/src/html_image.h"
#include "html/src/html_image_source.h"
#include "html/src/html_markdown.h"
#include "html/src/html_media.h"
#include "html/src/html_parser.h"
#include "html/src/html_window.h"
#include "html/src/html_xml.h"
#include "svg/src/svg_render.h"

namespace pr_impl {

// A script prepared to run (html/#prepare-the-script-element). It holds its element (domNodeHold). An external script
// whose fetch failed has the null string as its source.
struct PreparedScript {
    PrDomElement* element;
    PrString source;
    PrString url;
    PrString baseUrl;
    PrHtmlScriptKind kind;
    bool external;
    bool ready;                 // html/#ready-to-be-parser-executed: its source is available, or its fetch failed
    bool fetching;              // true while a fetch of its source refers to it
    bool released;              // true once nothing waits for it to run; its fetch frees it when answered
    HtmlModuleIndex module;     // a module script's module, the root of its graph
    uint32_t line;
    uint32_t column;
    PrCharset charset;          // the charset that decodes an external script, unless its response names another
};

namespace {

// Returns true if the document's content type is Markdown's (RFC 7763). A Markdown document is an HTML document that
// the context builds from the Markdown, and it runs no script.
bool isMarkdownDocument(const PrDomDocument* document) {
    return prStringView(&document->contentType) == "text/markdown";
}

// Returns the options that parse Markdown of the content type `type`. Its variant parameter (RFC 7763) chooses
// CommonMark alone for `CommonMark` and GFM's extensions for `GFM`, compared without regard to case. Any other variant,
// or none, is parsed with the extensions that GitHub renders.
PrMarkdownOptions markdownOptionsOf(PrStringView type) {
    PrMarkdownOptions options = prMarkdownOptionsDefault();
    PrString variant = prStringNull();
    if (prMimeTypeParameter(type, "variant", &variant)) {
        if (asciiEqualsFold(prStringView(&variant), "CommonMark"))
            options.extensions = 0;
        else if (asciiEqualsFold(prStringView(&variant), "GFM"))
            options.extensions = PrMarkdownExtensionFlag_Gfm;
    }
    prStringReset(&variant);
    return options;
}

// Frees the builder of a Markdown document whose load streams, if there is one.
void dropMarkdown(PrHtmlPage* self) {
    if (self->markdown == nullptr)
        return;
    htmlMarkdownBuilderReset(self->markdown);
    prMemoryFree(self->markdown);
    self->markdown = nullptr;
}

// Writes the script's type, as html/#prepare-the-script-element determines it, to `out`. Returns false for a type that
// does not run, which makes the element a data block.
bool scriptKind(const PrDomElement* element, PrHtmlScriptKind* out) {
    PrStringView type = domAttributeValue(element, "type");
    PrStringView language = domAttributeValue(element, "language");
    *out = PrHtmlScriptKind_Classic;
    if (type.data != nullptr ? type.size == 0 : language.size == 0)
        return true;
    if (type.data == nullptr) {
        // The language attribute names the subtype of a text/ type.
        TextBuffer named;
        textAppend(&named, "text/");
        textAppend(&named, language);
        bool javaScript = prMimeTypeIsJavaScript(textView(&named));
        textReset(&named);
        return javaScript;
    }
    type = stringTrim(type);
    if (prMimeTypeIsJavaScript(type))
        return true;
    *out = PrHtmlScriptKind_Module;
    return asciiEqualsFold(type, "module");
}

// Returns true if the document's load event waits for a fetch for `destination` (html/#delay-the-load-event). A font
// that a style or document.fonts asks for while the document loads holds its load event, so that a script that runs at
// load sees the fonts that the document started loading.
bool delaysLoad(DomFetchDestination destination) {
    return destination != DomFetchDestination_Media && destination != DomFetchDestination_Empty;
}

void advance(PrHtmlPage* self);
void queueAdvance(PrHtmlPage* self);

// Takes the fetch out of its context's list.
void unlinkFetch(HtmlFetch* fetch) {
    PrHtmlPage* self = fetch->page;
    if (self == nullptr)
        return;
    for (HtmlFetch** at = &self->fetches; *at != nullptr; at = &(*at)->next) {
        if (*at == fetch) {
            *at = fetch->next;
            break;
        }
    }
    if (fetch->delaysLoad)
        --self->delayingFetches;
    fetch->page = nullptr;
}

void resetHeaders(Array<HtmlFetchHeader>* headers) {
    for (uint32_t i = 0; i < headers->count; ++i) {
        prStringReset(&headers->data[i].name);
        prStringReset(&headers->data[i].value);
    }
    headers->reset();
}

// Releases a reference to the fetch, and frees it with the last one.
void releaseFetch(HtmlFetch* fetch) {
    if (fetch->references.fetchSub(1) != 1)
        return;
    prStringReset(&fetch->url);
    prStringReset(&fetch->origin);
    prStringReset(&fetch->referrer);
    prStringReset(&fetch->method);
    resetHeaders(&fetch->requestHeaders);
    fetch->requestBody.reset();
    resetHeaders(&fetch->responseHeaders);
    fetch->body.reset();
    prStringReset(&fetch->contentType);
    prStringReset(&fetch->contentLanguage);
    prStringReset(&fetch->responseUrl);
    prMutexReset(&fetch->lock);
    prMemoryFree(fetch);
}

// Passes a script's fetch's response to its callback, filtered as its type says (fetch/#concept-filtered-response).
void completeScriptFetch(HtmlFetch* fetch) {
    PrFetchClientResponse response{.status = fetch->status, .type = fetch->responseType};
    Array<PrFetchHeader> headers;
    TextBuffer exposed;
    bool opaque = fetch->responseType == PrFetchResponseType_Opaque ||
        fetch->responseType == PrFetchResponseType_OpaqueRedirect;
    if (fetch->status == PrStatus_Ok && !opaque) {
        // A CORS response exposes the headers that its Access-Control-Expose-Headers lines list, together.
        for (uint32_t i = 0; i < fetch->responseHeaders.count; ++i) {
            const HtmlFetchHeader& header = fetch->responseHeaders.data[i];
            if (asciiEqualsFold(prStringView(&header.name), "access-control-expose-headers")) {
                if (textView(&exposed).size != 0)
                    textAppend(&exposed, ", ");
                textAppend(&exposed, prStringView(&header.value));
            }
        }
        for (uint32_t i = 0; i < fetch->responseHeaders.count; ++i) {
            PrStringView name = prStringView(&fetch->responseHeaders.data[i].name);
            bool shown;
            if (fetch->responseType == PrFetchResponseType_Cors)
                shown = prFetchIsCorsExposedHeader(name, textView(&exposed), fetch->credentials);
            else
                shown = !prFetchIsForbiddenResponseHeaderName(name);
            if (shown)
                headers.push(PrFetchHeader{name, prStringView(&fetch->responseHeaders.data[i].value)});
        }
        response.responseStatus = fetch->responseStatus;
        response.url = prStringView(&fetch->responseUrl);
        response.headers = headers.data;
        response.headerCount = headers.count;
        response.body = PrByteView{
            fetch->body.data != nullptr ? fetch->body.data : reinterpret_cast<const uint8_t*>(""),
            fetch->body.count,
        };
    }
    fetch->scriptDone(fetch->data, &response);
    headers.reset();
    textReset(&exposed);
}

// Passes the response to the fetch's callback, and releases the agent's reference.
void completeFetch(HtmlFetch* fetch) {
    unlinkFetch(fetch);
    if (fetch->scriptDone != nullptr) {
        if (!fetch->cancelled)
            completeScriptFetch(fetch);
        releaseFetch(fetch);
        return;
    }
    PrStringView body{reinterpret_cast<const char*>(fetch->body.data), fetch->body.count};
    if (fetch->status == PrStatus_Ok && body.data == nullptr)
        body = "";
    DomFetchResult result{
        fetch->status, fetch->responseStatus, body, prStringView(&fetch->contentType),
        prStringView(&fetch->contentLanguage), prStringView(&fetch->responseUrl),
        fetch->responseType == PrFetchResponseType_Opaque,
    };
    fetch->done(fetch->data, &result);
    releaseFetch(fetch);
}

// The task that passes the response to the fetch's callback, then runs whatever the response made ready.
void completeFetchTask(void* data) {
    HtmlFetch* fetch = static_cast<HtmlFetch*>(data);
    PrHtmlPage* self = fetch->page;
    completeFetch(fetch);
    if (self != nullptr)
        advance(self);
}

// Ends a fetch on the loop, once. Stops it if it still runs.
void endOnLoop(HtmlFetch* fetch) {
    fetch->loopDone = true;
    prFetchDestroy(fetch->fetch);
    fetch->fetch = nullptr;
}

// Cancels a dropped fetch on the loop, with the agent's reference, which the browsing context handed to this task.
void cancelOnLoop(void* PR_NULLABLE data) {
    HtmlFetch* fetch = static_cast<HtmlFetch*>(data);
    if (!fetch->loopDone) {
        endOnLoop(fetch);
        releaseFetch(fetch);
    }
    releaseFetch(fetch);
}

// Drops a fetch whose response will not be used, and tells its callback's data so. A fetch whose response arrived has a
// task of the window, which this is the drop of. Otherwise the agent's reference goes to the task that the loop posted,
// or to a task that cancels the fetch on the loop.
void dropFetch(void* data) {
    HtmlFetch* fetch = static_cast<HtmlFetch*>(data);
    unlinkFetch(fetch);
    if (fetch->drop != nullptr)
        fetch->drop(fetch->data);
    if (fetch->scriptDone != nullptr && !fetch->cancelled) {
        PrFetchClientResponse lost{.status = PrStatus_Lost};
        fetch->scriptDone(fetch->data, &lost);
    }
    if (fetch->answered) {
        releaseFetch(fetch);
        return;
    }
    prMutexLock(&fetch->lock);
    fetch->dropped = true;
    bool posted = fetch->posted;
    prMutexUnlock(&fetch->lock);
    if (!posted)
        prNetPost(fetch->fetcher->net, cancelOnLoop, fetch);
}

// Drops every fetch that the context waits for and whose response has not arrived. A fetch whose response arrived
// belongs to its task, which drops it when the window's tasks are cleared.
void dropFetches(PrHtmlPage* self) {
    HtmlFetch* fetch = self->fetches;
    while (fetch != nullptr) {
        HtmlFetch* next = fetch->next;
        if (!fetch->answered)
            dropFetch(fetch);
        fetch = next;
    }
}

// Receives on the agent's thread the fetch that the loop posted, and queues the task of the window that uses its
// response. A fetch that the browsing context dropped is released.
void fetchArrived(void* PR_NULLABLE data) {
    HtmlFetch* fetch = static_cast<HtmlFetch*>(data);
    if (fetch->page == nullptr) {
        releaseFetch(fetch);
        return;
    }
    fetch->answered = true;
    htmlWindowQueueTask(fetch->page->window, completeFetchTask, fetch, dropFetch);
}

// Ends the fetch on the loop with `status`, and posts it to the agent unless the browsing context dropped it. Then
// releases the loop's reference.
void finishOnLoop(HtmlFetch* fetch, PrStatus status) {
    endOnLoop(fetch);
    fetch->status = status;
    prMutexLock(&fetch->lock);
    if (!fetch->dropped) {
        fetch->posted = true;
        prDomAgentPost(fetch->agent, fetchArrived, fetch);
    }
    prMutexUnlock(&fetch->lock);
    releaseFetch(fetch);
}

// Returns true if a response to a fetch for `destination` is a network error unless its status is ok
// (fetch/#ok-status): a script's (html/#fetch-a-classic-script), a style sheet's (html/#link-type-stylesheet) or a
// font's (css-fonts-4/#font-fetching-requirements).
bool needsOkStatus(DomFetchDestination destination) {
    return destination == DomFetchDestination_Script || destination == DomFetchDestination_Style ||
        destination == DomFetchDestination_Font;
}

void readBody(HtmlFetch* fetch, PrFetch* handle) {
    uint8_t chunk[16 * 1024];
    for (size_t got = prFetchRead(handle, chunk, sizeof(chunk)); got != 0; got =
            prFetchRead(handle, chunk, sizeof(chunk)))
        fetch->body.append(chunk, uint32_t(got));
}

// Receives the fetch's events on the loop.
void fetchEvent(void* PR_NULLABLE context, PrFetch* handle, PrFetchEvent event) {
    HtmlFetch* fetch = static_cast<HtmlFetch*>(context);
    switch (event) {
        case PrFetchEvent_Response: {
            PrStringView type = prFetchHeaderValue(handle, "content-type");
            PrStringView language = prFetchHeaderValue(handle, "content-language");
            if (type.data != nullptr)
                fetch->contentType = prStringCopy(type);
            if (language.data != nullptr)
                fetch->contentLanguage = prStringCopy(language);
            fetch->responseUrl = prStringCopy(prFetchUrl(handle));
            fetch->responseType = prFetchResponseType(handle);
            if (fetch->scriptDone != nullptr) {
                uint32_t count = prFetchHeaderCount(handle);
                for (uint32_t i = 0; i < count; ++i) {
                    PrFetchHeader header = prFetchHeader(handle, i);
                    fetch->responseHeaders.push(HtmlFetchHeader{prStringCopy(header.name), prStringCopy(header.value)});
                }
            }
            break;
        }
        case PrFetchEvent_Readable:
            readBody(fetch, handle);
            break;
        case PrFetchEvent_End: {
            readBody(fetch, handle);
            fetch->responseStatus = prFetchStatus(handle);
            bool ok = fetch->responseStatus >= 200 && fetch->responseStatus < 300;
            finishOnLoop(fetch, ok || !needsOkStatus(fetch->destination) ? PrStatus_Ok : PrStatus_NotFound);
            break;
        }
        case PrFetchEvent_Fail:
            finishOnLoop(fetch, PrStatus_Io);
            break;
    }
}

PrFetchDestination fetchDestinationOf(DomFetchDestination destination) {
    switch (destination) {
        case DomFetchDestination_Document:
            return PrFetchDestination_Document;
        case DomFetchDestination_Script:
            return PrFetchDestination_Script;
        case DomFetchDestination_Style:
            return PrFetchDestination_Style;
        case DomFetchDestination_Image:
            return PrFetchDestination_Image;
        case DomFetchDestination_Font:
            return PrFetchDestination_Font;
        case DomFetchDestination_Media:
            return PrFetchDestination_Media;
        case DomFetchDestination_Empty:
            return PrFetchDestination_Empty;
        case DomFetchDestination_Object:
            break;
    }
    return PrFetchDestination_Object;
}

// Starts the fetch on the loop. A fetch that the browsing context dropped first is left to its cancel task.
void beginOnLoop(void* PR_NULLABLE data) {
    HtmlFetch* fetch = static_cast<HtmlFetch*>(data);
    prMutexLock(&fetch->lock);
    bool dropped = fetch->dropped;
    prMutexUnlock(&fetch->lock);
    if (dropped)
        return;
    Array<PrFetchHeader> headers;
    for (uint32_t i = 0; i < fetch->requestHeaders.count; ++i) {
        const HtmlFetchHeader& header = fetch->requestHeaders.data[i];
        headers.push(PrFetchHeader{prStringView(&header.name), prStringView(&header.value)});
    }
    PrFetchRequestDesc desc{
        .url = prStringView(&fetch->url),
        .method = prStringView(&fetch->method),
        .headers = headers.data,
        .headerCount = headers.count,
        .body = PrByteView{fetch->requestBody.data, fetch->requestBody.count},
        .destination = fetchDestinationOf(fetch->destination),
        .mode = fetch->mode,
        .credentials = fetch->credentials,
        .redirect = fetch->redirect,
        .origin = prStringView(&fetch->origin),
        .referrer = prStringView(&fetch->referrer),
        .event = fetchEvent,
        .context = fetch,
    };
    PrStatus status = prFetchCreate(fetch->fetcher, &desc, &fetch->fetch);
    headers.reset();
    if (status != PrStatus_Ok)
        finishOnLoop(fetch, PrStatus_InvalidData);
}

// How a browsing context fetches a resource: what the request sends, and to whom
// (html/#create-a-potential-cors-request), and whether its document's load event waits for the response.
struct FetchSettings {
    PrFetchMode mode;
    PrFetchCredentials credentials;
    bool delaysLoad;
};

// Returns the settings of a fetch for `destination` whose CORS settings are `cors`. A frame's or an object's document
// is fetched as a navigation. Any other resource is fetched without CORS for No CORS, and with CORS otherwise, with
// credentials for the request's own origin for Anonymous and for any origin for Use Credentials
// (html/#cors-settings-attribute-credentials-mode).
FetchSettings fetchSettingsOf(DomFetchDestination destination, DomCorsSettings cors) {
    FetchSettings settings{PrFetchMode_NoCors, PrFetchCredentials_Include, delaysLoad(destination)};
    if (destination == DomFetchDestination_Document || destination == DomFetchDestination_Object) {
        settings.mode = PrFetchMode_Navigate;
        return settings;
    }
    switch (cors) {
        case DomCorsSettings_NoCors:
            break;
        case DomCorsSettings_Anonymous:
            settings.mode = PrFetchMode_Cors;
            settings.credentials = PrFetchCredentials_SameOrigin;
            break;
        case DomCorsSettings_UseCredentials:
            settings.mode = PrFetchMode_Cors;
            break;
    }
    return settings;
}

// Returns the CORS settings that the element's crossorigin attribute names.
DomCorsSettings corsOf(const PrDomElement* element) {
    return domCorsSettings(domAttributeValue(element, "crossorigin"));
}

// Returns the settings of the fetch of a script of `kind` for `element`. A module script is fetched with CORS whatever
// its element's CORS settings (html/#fetch-a-single-module-script), with No CORS acting as Anonymous.
FetchSettings scriptSettingsOf(const PrDomElement* element, PrHtmlScriptKind kind) {
    DomCorsSettings cors = corsOf(element);
    if (kind == PrHtmlScriptKind_Module && cors == DomCorsSettings_NoCors)
        cors = DomCorsSettings_Anonymous;
    return fetchSettingsOf(DomFetchDestination_Script, cors);
}

// Returns a fetch of `url` for `destination` with `settings`, which the context waits for, from the window's document.
// It goes to the host's fetcher once the caller has set its callback and posts it.
HtmlFetch* createFetch(PrHtmlPage* self, PrStringView url, DomFetchDestination destination, FetchSettings settings) {
    HtmlFetch* fetch = allocZeroed<HtmlFetch>();
    fetch->page = self;
    fetch->next = self->fetches;
    self->fetches = fetch;
    if (settings.delaysLoad)
        ++self->delayingFetches;
    fetch->destination = destination;
    fetch->delaysLoad = settings.delaysLoad;
    fetch->agent = self->window->agent;
    fetch->fetcher = self->host.fetcher;
    fetch->url = prStringCopy(url);
    fetch->origin = htmlWindowOrigin(self->window);
    fetch->referrer = prUrlSerialize(self->window->document->url, PrUrlSerialization_Whole);
    fetch->mode = settings.mode;
    fetch->credentials = settings.credentials;
    prMutexInit(&fetch->lock);
    return fetch;
}

// Starts the fetch on the fetcher's loop, with a reference for each side.
void postFetch(HtmlFetch* fetch) {
    fetch->references.store(2);
    prNetPost(fetch->fetcher->net, beginOnLoop, fetch);
}

// Answers the fetch without its fetcher, with `status`, in a task as the loop's answer would be. The agent holds the
// only reference.
void answerFetch(HtmlFetch* fetch, PrStatus status) {
    fetch->status = status;
    fetch->references.store(1);
    fetch->answered = true;
    htmlWindowQueueTask(fetch->page->window, completeFetchTask, fetch, dropFetch);
}

// Fetches `url`, an absolute URL without a fragment, for `destination` with `settings`, and calls `done` with `data`
// and the result. The result of a data: URL, or a network error for a context without a fetcher, is passed during this
// call. Any other result is passed in a task once the response arrives. If the context drops the fetch first, it calls
// `drop` with `data` instead.
void startFetch(PrHtmlPage* self, PrStringView url, DomFetchDestination destination,
        FetchSettings settings, DomFetchFn done, void* PR_NULLABLE data, void (*PR_NULLABLE drop)(void* PR_NULLABLE data)) {
    if (asciiStartsWithFold(url, "data:") || self->host.fetcher == nullptr) {
        PrString body = {};
        PrString contentType = {};
        PrStatus status = PrStatus_NotSupported;
        if (asciiStartsWithFold(url, "data:"))
            status = prUrlProcessData(url, &body, &contentType);
        DomFetchResult result{
            status, uint16_t(status == PrStatus_Ok ? 200 : 0), prStringView(&body),
            prStringView(&contentType), PrStringView{nullptr, 0},
            status == PrStatus_Ok ? url : PrStringView{nullptr, 0}, false,
        };
        done(data, &result);
        prStringReset(&body);
        prStringReset(&contentType);
        return;
    }
    HtmlFetch* fetch = createFetch(self, url, destination, settings);
    fetch->done = done;
    fetch->data = data;
    fetch->drop = drop;
    postFetch(fetch);
}

// The document host's fetch, which goes through its browsing context.
void fetchHost(void* PR_NULLABLE context, const DomFetchRequest* request, DomFetchFn done, void* PR_NULLABLE data,
        void (*PR_NULLABLE drop)(void* PR_NULLABLE data)) {
    PrHtmlPage* self = static_cast<PrHtmlPage*>(context);
    startFetch(self, request->url, request->destination, fetchSettingsOf(request->destination, request->cors), done,
        data, drop);
}

// Frees the script, or leaves it to the fetch that refers to it, which frees it when the fetch is answered.
void releaseScript(PreparedScript* script) {
    script->released = true;
    if (script->fetching)
        return;
    domNodeUnhold(script->element);
    prStringReset(&script->source);
    prStringReset(&script->url);
    prStringReset(&script->baseUrl);
    prMemoryFree(script);
}

// Returns the module of the window's module map at `url`, or kArrayMissing.
HtmlModuleIndex findModule(const PrHtmlPage* self, PrStringView url) {
    for (HtmlModuleIndex i = 0; i < self->modules.count; ++i) {
        const PrString& key = self->modules.data[i].url;
        if (!prStringIsNull(&key) && prStringView(&key) == url)
            return i;
    }
    return kArrayMissing;
}

// Returns Failed if the fetch of a module that the graph at `root` reaches failed
// (html/#fetch-the-descendants-of-and-link-a-module-script), or else Fetching while a module of it is being fetched, or
// else Parsed.
HtmlModuleState moduleGraphState(const PrHtmlPage* self, HtmlModuleIndex root) {
    Array<bool> reached;
    reached.resize(self->modules.count);
    for (uint32_t i = 0; i < reached.count; ++i)
        reached.data[i] = false;
    Array<HtmlModuleIndex> pending;
    pending.push(root);
    reached.data[root] = true;
    HtmlModuleState state = HtmlModuleState_Parsed;
    while (pending.count != 0 && state != HtmlModuleState_Failed) {
        const HtmlModule& module = self->modules.data[pending.last()];
        pending.pop();
        if (module.state != HtmlModuleState_Parsed) {
            state = module.state;
            continue;
        }
        for (uint32_t i = 0; i < module.requestCount; ++i) {
            HtmlModuleIndex request = self->moduleRequests.data[module.firstRequest + i];
            if (!reached.data[request]) {
                reached.data[request] = true;
                pending.push(request);
            }
        }
    }
    reached.reset();
    pending.reset();
    return state;
}

void settleModulesTask(void* data);

// Queues the task that settles the waits for module graphs.
void queueModules(PrHtmlPage* self) {
    if (self->modulesQueued)
        return;
    self->modulesQueued = true;
    htmlWindowQueueTask(self->window, settleModulesTask, self,
        [](void* data) { static_cast<PrHtmlPage*>(data)->modulesQueued = false; });
}

// The fetch of a module of the module map.
struct ModuleFetch {
    PrHtmlPage* page;
    HtmlModuleIndex module;
};

void parseModule(PrHtmlPage* self, HtmlModuleIndex index, const PrHtmlScript& script);

// Receives a module's response, and parses the module if the response is ok and JavaScript
// (html/#fetch-a-single-module-script). A module script is UTF-8. The map keys the module by the URL that it was
// requested at, and its imports resolve against the URL of its response, after redirects.
void moduleFetched(void* PR_NULLABLE data, const DomFetchResult* result) {
    ModuleFetch* fetch = static_cast<ModuleFetch*>(data);
    PrHtmlPage* self = fetch->page;
    HtmlModuleIndex index = fetch->module;
    prMemoryFree(fetch);
    if (!domFetchIsOk(result) || !prMimeTypeIsJavaScript(prMimeTypeEssence(result->contentType))) {
        self->modules.data[index].state = HtmlModuleState_Failed;
    } else {
        PrString decoded = {};
        PrCharsetDecoder decoder = prCharsetDecoderMake(PrCharset_Utf8, 0);
        PrStringView text = prCharsetDecodeView(&decoder, result->body, PrCharsetInput_Final, &decoded);
        PrString url = prStringClone(&self->modules.data[index].url);
        PrString base = result->url.data != nullptr ? prStringCopy(result->url) : prStringClone(&url);
        PrHtmlScript script{
            .source = text.data != nullptr ? text : "",
            .url = prStringView(&url),
            .baseUrl = prStringView(&base),
            .kind = PrHtmlScriptKind_Module,
            .external = true,
            .line = 1,
            .column = 1,
        };
        parseModule(self, index, script);
        prStringReset(&url);
        prStringReset(&base);
        prStringReset(&decoded);
    }
    queueModules(self);
}

void dropModuleFetch(void* PR_NULLABLE data) {
    prMemoryFree(data);
}

// Fetches the module of the map at `index` with CORS (html/#fetch-a-single-module-script). The fetch does not delay the
// load event: a module script does, while it waits to run, and an import() does not.
void fetchModule(PrHtmlPage* self, HtmlModuleIndex index) {
    const HtmlModule& module = self->modules.data[index];
    PrStringView url = prStringView(&module.url);
    PrString request = prStringCopy(stringSlice(url, 0, stringFind(url, '#')));
    FetchSettings mode{PrFetchMode_Cors, module.credentials, false};
    ModuleFetch* fetch = allocZeroed<ModuleFetch>();
    *fetch = ModuleFetch{self, index};
    startFetch(self, prStringView(&request), DomFetchDestination_Script, mode, moduleFetched, fetch, dropModuleFetch);
    prStringReset(&request);
}

// Returns the module of the map at `url`, which is added if the map lacks it. A module that is added appends its index
// to `added`, and its fetch is the caller's to start.
HtmlModuleIndex addModule(PrHtmlPage* self, PrStringView url, PrFetchCredentials credentials, Array<HtmlModuleIndex>* added) {
    HtmlModuleIndex index = findModule(self, url);
    if (index != kArrayMissing)
        return index;
    index = self->modules.count;
    self->modules.push(HtmlModule{.url = prStringCopy(url), .credentials = credentials});
    added->push(index);
    return index;
}

void collectRequest(void* PR_NULLABLE context, PrStringView url) {
    static_cast<Array<PrString>*>(context)->push(prStringCopy(url));
}

// Parses `script` into the module at `index` with the host, records the modules that it imports, and fetches those that
// the map lacks (html/#fetch-the-descendants-of-and-link-a-module-script). The fetches start once the module's imports
// are recorded, since the fetch of a data: URL is answered during the call that starts it.
void parseModule(PrHtmlPage* self, HtmlModuleIndex index, const PrHtmlScript& script) {
    Array<PrString> requests;
    uint32_t record = self->host.parseModule(self->host.context, self, &script, collectRequest, &requests);
    PrFetchCredentials credentials = self->modules.data[index].credentials;
    Array<HtmlModuleIndex> added;
    uint32_t first = self->moduleRequests.count;
    for (uint32_t i = 0; i < requests.count; ++i) {
        self->moduleRequests.push(addModule(self, prStringView(&requests.data[i]), credentials, &added));
        prStringReset(&requests.data[i]);
    }
    requests.reset();
    HtmlModule& module = self->modules.data[index];
    module.state = HtmlModuleState_Parsed;
    module.record = record;
    module.firstRequest = first;
    module.requestCount = self->moduleRequests.count - first;
    for (uint32_t i = 0; i < added.count; ++i)
        fetchModule(self, added.data[i]);
    added.reset();
}

// Settles the waits whose module graphs are fetched or failed, among those that wait when it starts. A module script
// becomes ready. An import()'s callback is called, and the waits that it adds are left to the next settling.
void settleModules(PrHtmlPage* self) {
    uint32_t end = self->moduleWaits.count;
    for (uint32_t i = 0; i < end;) {
        HtmlModuleWait wait = self->moduleWaits.data[i];
        HtmlModuleState state = moduleGraphState(self, wait.root);
        if (state == HtmlModuleState_Fetching) {
            ++i;
            continue;
        }
        self->moduleWaits.erase(i);
        --end;
        if (wait.script != nullptr)
            wait.script->ready = true;
        else
            wait.done(wait.context, state == HtmlModuleState_Parsed ? PrStatus_Ok : PrStatus_NotFound);
    }
}

void settleModulesTask(void* data) {
    PrHtmlPage* self = static_cast<PrHtmlPage*>(data);
    self->modulesQueued = false;
    settleModules(self);
    advance(self);
}

// Makes `script` wait for the graph at `root`. The script is ready at once if the graph is fetched.
void waitForModules(PrHtmlPage* self, PreparedScript* script, HtmlModuleIndex root) {
    script->module = root;
    script->ready = moduleGraphState(self, root) != HtmlModuleState_Fetching;
    if (!script->ready)
        self->moduleWaits.push(HtmlModuleWait{.root = root, .script = script});
}

// Empties the module map, once the fetches of its modules are dropped, when the window's document is replaced or the
// context destroyed. An import() that waits is called with Lost.
void dropModules(PrHtmlPage* self) {
    Array<HtmlModuleWait> waits = self->moduleWaits;
    self->moduleWaits = {};
    for (uint32_t i = 0; i < waits.count; ++i) {
        if (waits.data[i].script == nullptr)
            waits.data[i].done(waits.data[i].context, PrStatus_Lost);
    }
    waits.reset();
    for (uint32_t i = 0; i < self->modules.count; ++i)
        prStringReset(&self->modules.data[i].url);
    self->modules.reset();
    self->moduleRequests.reset();
}

// html/#execute-the-script-element
PrStatus executeScript(PrHtmlPage* self, const PreparedScript* prepared) {
    bool module = prepared->kind == PrHtmlScriptKind_Module;
    if (module ? moduleGraphState(self, prepared->module) == HtmlModuleState_Failed : prStringIsNull(&prepared->source)) {
        domEventFire(prepared->element, "error");
        return PrStatus_Ok;
    }
    PrHtmlScript script{
        prStringView(&prepared->source), prStringView(&prepared->url),
        prStringView(&prepared->baseUrl), prepared->kind, prepared->external,
        prepared->line, prepared->column,
        module ? self->modules.data[prepared->module].record : 0,
    };
    bool running = self->runningScript;
    self->runningScript = running || (self->parsing && prepared->kind == PrHtmlScriptKind_Classic);
    PrDomElement* previous = self->window->currentScript;
    bool shadow = domNodeShadowRoot(domNodeRoot(prepared->element)) != nullptr;
    self->window->currentScript = prepared->kind == PrHtmlScriptKind_Classic && !shadow ? prepared->element : nullptr;
    // An external or module script cannot open the document by writing to it.
    bool destructive = prepared->external || prepared->kind == PrHtmlScriptKind_Module;
    self->ignoreDestructiveWrites += destructive;
    PrStatus status = self->host.execute(self->host.context, self, &script);
    self->ignoreDestructiveWrites -= destructive;
    self->window->currentScript = previous;
    self->runningScript = running;
    if (status == PrStatus_Ok && prepared->external)
        domEventFire(prepared->element, "load");
    return status;
}

// Receives the result of fetching a script's source. The script becomes ready to run, with its source, or without one
// for a network error.
void scriptFetched(void* PR_NULLABLE data, const DomFetchResult* result) {
    PreparedScript* script = static_cast<PreparedScript*>(data);
    script->fetching = false;
    script->ready = true;
    if (result->status == PrStatus_Ok) {
        // A classic script is decoded in the charset of its byte order mark, or of its response, or its own
        // (html/#fetch-a-classic-script). A module script is UTF-8 (html/#fetch-a-single-module-script).
        PrStringView body = result->body.data != nullptr ? result->body : "";
        PrCharset charset = script->charset;
        if (script->kind == PrHtmlScriptKind_Classic) {
            PrCharset response = prCharsetFromMimeType(result->contentType);
            charset = prCharsetSniffBom(body);
            if (charset == PrCharset_None)
                charset = response != PrCharset_None ? response : script->charset;
        }
        PrCharsetDecoder decoder = prCharsetDecoderMake(charset, 0);
        PrStringView text = prCharsetDecodeView(&decoder, body, PrCharsetInput_Final, &script->source);
        // Text that decoded in place is still the response's, which the fetch frees.
        if (prStringIsNull(&script->source))
            script->source = prStringCopy(text.data != nullptr ? text : "");
        // A classic script's imports resolve against its response's URL, after redirects, or against about:blank if its
        // errors are muted, as a cross-origin script fetched without CORS has them (html/#creating-a-classic-script).
        if (script->kind == PrHtmlScriptKind_Classic && (result->url.data != nullptr || result->opaque))
            prStringReplace(&script->baseUrl, result->opaque ? PrStringView{"about:blank"} : result->url);
    }
    if (script->released)
        releaseScript(script);
}

void dropScriptFetch(void* PR_NULLABLE data) {
    PreparedScript* script = static_cast<PreparedScript*>(data);
    script->fetching = false;
    script->ready = true;
    if (script->released)
        releaseScript(script);
}

// Fetches the script that `src` names. The script is ready at once, with no source, if there is nothing to fetch.
// Otherwise it is ready once the fetch is answered.
void fetchScript(PrHtmlPage* self, PreparedScript* script, PrStringView src, const PrUrl* base) {
    PrUrl* resource = nullptr;
    if (src.size == 0 || prUrlCreate(src, base, self->window->document->charset, &resource) != PrStatus_Ok) {
        prUrlDestroy(resource);
        script->ready = true;
        return;
    }
    script->url = prUrlSerialize(resource, PrUrlSerialization_Whole);
    script->baseUrl = prStringClone(&script->url);
    if (script->kind == PrHtmlScriptKind_Module) {
        prUrlDestroy(resource);
        Array<HtmlModuleIndex> added;
        FetchSettings mode = scriptSettingsOf(script->element, script->kind);
        HtmlModuleIndex root = addModule(self, prStringView(&script->url), mode.credentials, &added);
        if (added.count != 0)
            fetchModule(self, root);
        added.reset();
        waitForModules(self, script, root);
        return;
    }
    // html/#prepare-the-script-element: the charset that the element names, or else its document's output charset
    // (encoding/#get-an-output-encoding). A script is ASCII-compatible text, which a UTF-16 document's charset says
    // nothing about, so it falls back to UTF-8.
    PrCharset named = prCharsetFromLabel(domAttributeValue(script->element, "charset"));
    if (script->kind == PrHtmlScriptKind_Module)
        script->charset = PrCharset_Utf8;
    else if (named != PrCharset_None)
        script->charset = named;
    else
        script->charset = prCharsetOutput(self->window->document->charset);
    PrString request = prUrlSerialize(resource, PrUrlSerialization_ExcludeFragment);
    prUrlDestroy(resource);
    script->fetching = true;
    FetchSettings mode = scriptSettingsOf(script->element, script->kind);
    startFetch(self, prStringView(&request), DomFetchDestination_Script, mode, scriptFetched, script, dropScriptFetch);
    prStringReset(&request);
}

// Returns the attribute that names an external script's source: an HTML script's src, or an SVG script's href, or its
// XLink href if it has no href (SVG2/#ScriptElement).
PrStringView scriptSource(const PrDomElement* element) {
    if (element->space != PrDomNamespace_Svg)
        return domAttributeValue(element, "src");
    PrStringView href = domAttributeValue(element, "href");
    if (href.data != nullptr)
        return href;
    const PrDomAttr* xlink = domAttribute(element, PrDomNamespace_XLink, "href");
    return xlink != nullptr ? prStringView(&xlink->value) : PrStringView{};
}

// Pauses the parse of the window's document, which its HTML or XML parser runs.
void pauseParse(PrHtmlPage* self) {
    if (self->window->document->kind == DomDocumentKind_Xml)
        prXmlParserPause(self->xml.parser);
    else
        self->parser.paused = true;
}

// Returns true if a script paused the parse of the window's document.
bool parsePaused(const PrHtmlPage* self) {
    if (self->window->document->kind == DomDocumentKind_Xml)
        return self->xml.parser != nullptr && prXmlParserIsPaused(self->xml.parser);
    return self->parser.paused;
}

// html/#prepare-the-script-element. An inline script's text starts at `line` and `column` of its document's source, or
// at 1 and 1 for a script that no parser inserted. A parser-inserted classic script that is not deferred or async
// blocks the parser until the script is fetched and no style sheet blocks scripts (html/#parsing-xhtml-documents for
// the XML parser). An external or module script that no parser inserted runs later, in a task (advance): as soon as it
// is ready if it is async, which such a script is unless it is told otherwise, and otherwise in the order of insertion.
PrStatus prepareScriptAt(PrHtmlPage* self, PrDomElement* element, uint32_t line, uint32_t column) {
    DomScriptFlags flags = domScriptFlags(element);
    if ((flags & DomScriptFlag_Started) != 0)
        return PrStatus_Ok;
    // A parser-inserted script that does not start here is left for script to start, as a script that no parser
    // inserted, which is async unless it says otherwise.
    bool parserInserted = (flags & DomScriptFlag_ParserInserted) != 0;
    bool async = domAttributeValue(element, "async").data != nullptr;
    DomScriptFlags left = flags & ~DomScriptFlag_ParserInserted;
    if (parserInserted && !async)
        left &= ~DomScriptFlag_NotForceAsync;
    domScriptSetFlags(element, left);
    PrStringView src = scriptSource(element);
    PrString text = src.data == nullptr ? domNodeTextContent(element) : PrString{};
    PrHtmlScriptKind kind = PrHtmlScriptKind_Classic;
    if (src.data == nullptr && prStringIsEmpty(&text)) {
        prStringReset(&text);
        domScriptSetFlags(element, flags);
        return PrStatus_Ok;
    }
    if (!domIsConnected(element) || !scriptKind(element, &kind)) {
        prStringReset(&text);
        return PrStatus_Ok;
    }
    domScriptSetFlags(element, flags | DomScriptFlag_Started);
    bool forceAsync = (flags & DomScriptFlag_NotForceAsync) == 0;
    if ((kind == PrHtmlScriptKind_Classic && domAttributeValue(element, "nomodule").data != nullptr) ||
            (kind == PrHtmlScriptKind_Module && self->host.parseModule == nullptr)) {
        prStringReset(&text);
        return PrStatus_Ok;
    }
    domNodeHold(element);
    PreparedScript* script = allocZeroed<PreparedScript>();
    *script = PreparedScript{
        .element = element,
        .source = text,
        .kind = kind,
        .external = src.data != nullptr,
        .line = 1,
        .column = 1,
    };
    PrUrl* base = domDocumentBaseUrl(self->window->document);
    if (script->external) {
        fetchScript(self, script, src, base);
    } else {
        script->url = prUrlSerialize(self->window->document->url, PrUrlSerialization_Whole);
        script->baseUrl = prUrlSerialize(base, PrUrlSerialization_Whole);
        script->line = line;
        script->column = column;
        script->ready = true;
        if (script->kind == PrHtmlScriptKind_Module) {
            HtmlModuleIndex root = self->modules.count;
            FetchSettings mode = scriptSettingsOf(element, script->kind);
            self->modules.push(HtmlModule{.credentials = mode.credentials});
            PrHtmlScript parsed{
                .source = prStringView(&script->source),
                .url = prStringView(&script->url),
                .baseUrl = prStringView(&script->baseUrl),
                .kind = PrHtmlScriptKind_Module,
                .line = line,
                .column = column,
            };
            parseModule(self, root, parsed);
            waitForModules(self, script, root);
        }
    }
    prUrlDestroy(base);
    bool defer = domAttributeValue(element, "defer").data != nullptr;
    bool blocking = parserInserted;
    if (script->external || script->kind == PrHtmlScriptKind_Module) {
        bool soon = async || forceAsync;
        if (soon && parserInserted && script->ready) {
            // A parser-inserted async script that is ready already, such as a module that imports nothing, runs at once
            // rather than when the parse next yields.
            blocking = false;
        } else if (soon || !parserInserted) {
            (soon ? self->soon : self->inOrder).push(script);
            queueAdvance(self);
            return PrStatus_Ok;
        } else if (defer || script->kind == PrHtmlScriptKind_Module) {
            self->deferred.push(script);
            return PrStatus_Ok;
        }
    }
    // A parser-blocking script runs at once if it is ready and no sheet blocks it, as does an inline script that no
    // parser inserted (html/#script-processing-inline), even while another script runs.
    if (blocking && (!script->ready || cssDocumentHasBlockingSheet(self->window->document))) {
        self->blocking = script;
        pauseParse(self);
        return PrStatus_Ok;
    }
    PrStatus status = executeScript(self, script);
    releaseScript(script);
    return status;
}

// The HTML parser's scripting hook. The script text starts at the parser's script start.
PrStatus prepareScript(void* context, PrDomElement* element) {
    PrHtmlPage* self = static_cast<PrHtmlPage*>(context);
    htmlInputPosition(htmlInputText(&self->parser.tokenizer), &self->locationAt, self->parser.scriptStart, &self->line,
        &self->column);
    return prepareScriptAt(self, element, self->line, self->column);
}

// The document host's preparation of a script that no parser inserted, once it is connected or changes while connected.
// A document that is not the window's, or a window that runs no scripts, runs none.
void prepareConnectedScript(void* PR_NULLABLE context, PrDomElement* element) {
    PrHtmlPage* self = static_cast<PrHtmlPage*>(context);
    if (self->host.execute != nullptr && element->document == self->window->document)
        prepareScriptAt(self, element, 1, 1);
}

// The XML parser's scripting hook.
PrStatus prepareXmlScript(void* context, PrDomElement* element, uint32_t line, uint32_t column) {
    return prepareScriptAt(static_cast<PrHtmlPage*>(context), element, line, column);
}

void queueTask(void* PR_NULLABLE context, void (*run)(void* data), void* data, void (*PR_NULLABLE drop)(void* data)) {
    htmlWindowQueueTask(static_cast<PrHtmlPage*>(context)->window, run, data, drop);
}

void queueDelayingTask(void* PR_NULLABLE context, void (*run)(void* data), void* data, void (*PR_NULLABLE drop)(void* data)) {
    htmlWindowQueueTask(static_cast<PrHtmlPage*>(context)->window, run, data, drop, true);
}

bool queueMicrotask(void* PR_NULLABLE context, void (*run)(void* data), void* data) {
    PrHtmlPage* self = static_cast<PrHtmlPage*>(context);
    if (self->host.microtask == nullptr)
        return false;
    self->host.microtask(self->host.context, self, run, data);
    return true;
}

bool compileHandler(void* PR_NULLABLE context, PrDomEventTarget* target, PrDomElement* element,
        PrDomEventHandler handler, PrStringView body, PrDomEventHandlerCallback* out) {
    PrHtmlPage* self = static_cast<PrHtmlPage*>(context);
    if (self->host.compile == nullptr || isMarkdownDocument(self->window->document))
        return false;
    return self->host.compile(self->host.context, self, target, element, handler, body, out);
}

PrDomHostState elementState(void* PR_NULLABLE context, const PrDomElement* element, PrDomHostState wanted) {
    PrHtmlPage* self = static_cast<PrHtmlPage*>(context);
    if (self->host.elementState == nullptr)
        return 0;
    return self->host.elementState(self->host.context, self, element, wanted);
}

// Starts the decoder of the load's bytes in `charset`, which becomes the document's.
void startDecoder(PrHtmlPage* self, PrCharset charset, HtmlConfidence confidence) {
    self->decoder = prCharsetDecoderMake(charset, 0);
    self->confidence = confidence;
    self->window->document->charset = charset;
}

// Decides the charset of an HTML load from the bytes that have arrived (html/#encoding-sniffing-algorithm), and starts
// the decoder in it. Returns false if the bytes are not `complete` and the load waits for more of them: those that
// decide whether the input starts with a byte order mark, and the first kilobyte, which the prescan reads. A meta
// element past it changes the encoding as the parser reaches it.
bool sniffCharset(PrHtmlPage* self, PrStringView bytes, bool complete) {
    if (!complete && !prCharsetIsBomDecided(bytes))
        return false;
    PrCharset charset = prCharsetSniffBom(bytes);
    if (charset == PrCharset_None)
        charset = self->transportCharset;
    if (charset != PrCharset_None) {
        startDecoder(self, charset, HtmlConfidence_Certain);
        return true;
    }
    if (!htmlPrescan(bytes, complete, &charset))
        return false;
    // A frame takes the charset of its parent's document of the same origin.
    const PrHtmlWindow* parent = self->window->parent;
    if (charset == PrCharset_None && parent != nullptr && parent->document != nullptr) {
        PrCharset inherited = parent->document->charset;
        if (inherited != PrCharset_Utf16Be && inherited != PrCharset_Utf16Le &&
                prUrlHasSameOrigin(parent->document->url, self->window->document->url))
            charset = inherited;
    }
    startDecoder(self, charset != PrCharset_None ? charset : self->defaultCharset, HtmlConfidence_Tentative);
    return true;
}

// Changes the charset of the load to `charset`, which a meta element declares, if the parse is not certain of its
// charset (html/#change-the-encoding). If the bytes that have been decoded decode to the same text in the new charset,
// the decoder changes for the bytes that follow. Otherwise the load parses its bytes again in the new charset, and
// returns true.
bool changeEncoding(void* data, PrCharset charset) {
    PrHtmlPage* self = static_cast<PrHtmlPage*>(data);
    if (self->confidence != HtmlConfidence_Tentative)
        return false;
    PrCharset current = self->decoder.charset;
    charset = htmlDeclaredCharset(charset);
    if (current == PrCharset_Utf16Be || current == PrCharset_Utf16Le || charset == current) {
        self->confidence = HtmlConfidence_Certain;
        return false;
    }
    bool whole = self->loadSource.data != nullptr;
    PrStringView decoded = whole ? self->loadSource : stringSlice(textView(&self->streamInput), 0, self->streamRead);
    PrCharsetInput input = whole || self->streamFlushed ? PrCharsetInput_Final : PrCharsetInput_Partial;
    PrCharsetDecoder before = prCharsetDecoderMake(current, 0);
    PrCharsetDecoder after = prCharsetDecoderMake(charset, 0);
    PrString first = {};
    PrString second = {};
    bool same = prCharsetDecodeView(&before, decoded, input, &first) == prCharsetDecodeView(&after, decoded, input, &second);
    prStringReset(&first);
    prStringReset(&second);
    if (!same) {
        self->reparse = charset;
        return true;
    }
    self->decoder = after;
    self->confidence = HtmlConfidence_Certain;
    self->window->document->charset = charset;
    return false;
}

// Discards the window's document, which a script may still hold. The document loses the objects that the window gave
// it, which the document refers to without holding them, and its listeners, as its closed window loses its own: the
// window keeps the document until the window is freed, and a listener's callback could otherwise keep the window.
void discardDocument(PrHtmlPage* self) {
    PrDomDocument* document = self->window->document;
    if (document == nullptr)
        return;
    domListenersDestroy(document->listeners);
    document->listeners = nullptr;
    prCssPresentationDestroy(self->presentation);
    self->presentation = nullptr;
    svgPresentationDestroy(self->svg);
    self->svg = nullptr;
    document->media = nullptr;
    document->global = nullptr;
    document->host = nullptr;
    document->customElements = nullptr;
}

// Gives the window a new document. The document's media queries are evaluated against the window's environment, and the
// document fetches and queues tasks through the browsing context.
void newDocument(PrHtmlPage* self) {
    PrHtmlWindow* window = self->window;
    window->document = domDocumentCreate(DomDocumentKind_Html);
    prCssEnable(window->document);
    prSvgEnable(window->document);
    htmlEnable(window->document);
    window->document->media = &window->media;
    window->document->global = window;
    window->document->host = &self->documentHost;
    window->document->customElements = window->customElements;
    if (self->host.font != nullptr && self->host.shaper != nullptr) {
        PrCssPresentationEmbedding embedding =
            window->parent != nullptr ? PrCssPresentationEmbedding_Frame : PrCssPresentationEmbedding_TopLevel;
        uint32_t sheetCount = 0;
        const PrCssUserAgentSheet* sheets = prHtmlUserAgentSheets(&sheetCount);
        PrCssPresentationDesc desc{
            .document = window->document,
            .font = self->host.font,
            .shaper = self->host.shaper,
            .agent = window->agent,
            .userAgentSheets = sheets,
            .userAgentSheetCount = sheetCount,
            .embedding = embedding,
        };
        self->presentation = prCssPresentationCreate(&desc);
        self->svg = svgPresentationCreate(self->presentation);
        // The window's rendering updates fire the presentation's animation events.
        self->presentation->animations.recordsEvents = true;
    }
}

// Fires the queued events of embedded content that are still current. The window's load event waits for them
// (html/#delay-the-load-event).
void fireElementEvents(PrHtmlPage* self) {
    while (self->elementEvents.count != 0) {
        HtmlElementEvent event = self->elementEvents.data[0];
        self->elementEvents.erase(0);
        if (event.generation == event.element->state->generation)
            domEventFire(event.element, event.error ? "error" : "load");
        domNodeUnhold(event.element);
    }
}

// Releases the queued events of embedded content without firing them.
void dropElementEvents(PrHtmlPage* self) {
    for (uint32_t i = 0; i < self->elementEvents.count; ++i)
        domNodeUnhold(self->elementEvents.data[i].element);
    self->elementEvents.clear();
}

void fireElementEventsTask(void* context) {
    fireElementEvents(static_cast<PrHtmlPage*>(context));
}

// Queues the load or error event for the element's current content. An feImage fires neither, since filter-effects-1
// gives it no such event.
void queueElementEvent(PrHtmlPage* self, PrDomElement* element, bool error) {
    if (element->space == PrDomNamespace_Svg && element->tag == PrDomTag_FeImage)
        return;
    domNodeHold(element);
    self->elementEvents.push(HtmlElementEvent{element, domElementState(element)->generation, error});
    htmlWindowQueueTask(self->window, fireElementEventsTask, self);
}

// A fetch for an element's embedded content (html/#the-iframe-element, html/#the-img-element,
// html/#the-object-element). It holds the element (domNodeHold), and is dropped if a later update of the content
// supersedes it.
struct EmbedFetch {
    PrHtmlPage* page;
    PrDomElement* element;
    uint32_t generation;
    PrString url;               // the URL that it fetches, serialized with its fragment
};

EmbedFetch* newEmbedFetch(PrHtmlPage* self, PrDomElement* element, PrString url) {
    EmbedFetch* fetch = allocZeroed<EmbedFetch>();
    domNodeHold(element);
    *fetch = EmbedFetch{self, element, domElementState(element)->generation, url};
    return fetch;
}

void dropEmbedFetch(void* PR_NULLABLE data) {
    EmbedFetch* fetch = static_cast<EmbedFetch*>(data);
    domNodeUnhold(fetch->element);
    prStringReset(&fetch->url);
    prMemoryFree(fetch);
}

// Returns true if the content that a fetch is for is still the element's content.
bool isCurrent(const EmbedFetch* fetch) {
    return fetch->element->state != nullptr && fetch->element->state->generation == fetch->generation;
}

// Sets the child's viewport to the iframe's width and height attributes, which are dimension values, or else to an
// iframe's default size (html/#the-iframe-element). A percentage is relative to a container that the engine does not
// lay out, so it means the default size.
void frameViewport(PrHtmlPage* child, const PrDomElement* element) {
    struct ViewportSide {
        PrStringView attribute;
        double fallback;
    };
    constexpr ViewportSide kSides[] = {{"width", 300}, {"height", 150}};
    double sizes[2];
    for (uint32_t i = 0; i < 2; ++i) {
        bool percentage = false;
        if (!domParseDimension(domAttributeValue(element, kSides[i].attribute), &sizes[i], &percentage) || percentage)
            sizes[i] = kSides[i].fallback;
    }
    prHtmlPageSetViewport(child, PrVec2{float(sizes[0]), float(sizes[1])});
}

// Releases the frame that the element holds, if any. The browsing context that it held is discarded: its tasks no
// longer run, and it lives on until this context is destroyed.
void dropFrame(PrHtmlPage* self, PrDomElement* element) {
    PrHtmlWindow* window = self->window;
    for (uint32_t i = 0; i < window->frames.count; ++i) {
        if (window->frames.data[i].container != element)
            continue;
        for (uint32_t k = 0; k < self->children.count; ++k) {
            PrHtmlPage* child = self->children.data[k];
            if (child->window == window->frames.data[i].window)
                domAgentRemoveGlobal(child->window->agent, child);
        }
        window->frames.erase(i);
        // A load that waited for the frame's document no longer does.
        queueAdvance(self);
        // The element no longer shows any browsing context's content.
        domElementState(element)->embed = 0;
        if (domIsConnected(element))
            cssPresentationElementContentChanged(element);
        return;
    }
}

// Whether a frame's document is the initial about:blank of an iframe whose src is being fetched.
enum {
    FrameDocument_Navigated,
    FrameDocument_InitialBlank,
};
typedef uint8_t FrameDocument;

// Gives the element a frame that holds a new browsing context, navigated to `source` at `url` as a response with
// `headers`.
void attachFrame(PrHtmlPage* self, PrDomElement* element, PrStringView source, PrStringView url,
        const PrHtmlResponseHeaders& headers, FrameDocument kind = FrameDocument_Navigated) {
    PrHtmlWindow* window = self->window;
    PrHtmlHost childHost = prHtmlHostDefault();
    childHost.navigatorCompatibilityMode = self->host.navigatorCompatibilityMode;
    if (self->host.frame != nullptr)
        self->host.frame(self->host.context, self, &childHost);
    if (childHost.agent == nullptr)
        childHost.agent = window->agent;
    if (childHost.font == nullptr)
        childHost.font = self->host.font;
    if (childHost.shaper == nullptr)
        childHost.shaper = self->host.shaper;
    if (childHost.images == nullptr)
        childHost.images = self->host.images;
    if (childHost.media == nullptr)
        childHost.media = self->host.media;
    if (childHost.compress == nullptr)
        childHost.compress = self->host.compress;
    if (childHost.fetcher == nullptr)
        childHost.fetcher = self->host.fetcher;
    if (childHost.localStorage == nullptr)
        childHost.localStorage = self->host.localStorage;
    if (childHost.device == nullptr)
        childHost.device = self->host.device;
    if (childHost.sessionStorage == nullptr)
        childHost.sessionStorage = self->host.sessionStorage;
    PrHtmlPage* child = prHtmlPageCreate(&childHost);
    child->window->parent = window;
    child->initialBlank = kind == FrameDocument_InitialBlank;
    child->defaultCharset = self->defaultCharset;
    prHtmlPageSetDisplay(child, &self->display);
    prHtmlPageSetUserPreferences(child, &self->preferences);
    child->visibility = self->visibility;
    // The context made its first document's presentation before it had a parent.
    if (child->presentation != nullptr)
        child->presentation->embedding = PrCssPresentationEmbedding_Frame;
    self->children.push(child);
    window->frames.push(HtmlFrame{element, child->window});
    frameViewport(child, element);
    domElementState(element)->embed = htmlPageEmbed(child);
    if (domIsConnected(element))
        cssPresentationElementContentChanged(element);
    prHtmlPageLoad(child, source, url, &headers);
}

// Returns true if the element is connected in the window's document, which is where its content loads.
bool inWindow(const PrHtmlPage* self, PrDomElement* element) {
    return domIsConnected(element) && element->document == self->window->document;
}

// Receives the result of fetching an iframe's src. The iframe's browsing context navigates to the resource, or to an
// empty document at its URL if the resource does not load. If the host gives no MIME type, the resource is HTML.
void frameFetched(void* PR_NULLABLE data, const DomFetchResult* result) {
    EmbedFetch* fetch = static_cast<EmbedFetch*>(data);
    if (isCurrent(fetch) && inWindow(fetch->page, fetch->element)) {
        bool loaded = result->status == PrStatus_Ok;
        PrHtmlResponseHeaders headers{};
        if (loaded)
            headers = PrHtmlResponseHeaders{result->contentType, result->contentLanguage};
        // The document replaces the initial about:blank, as a navigation of the iframe's browsing context would.
        dropFrame(fetch->page, fetch->element);
        attachFrame(fetch->page, fetch->element, loaded ? result->body : "", prStringView(&fetch->url), headers);
    }
    dropEmbedFetch(fetch);
}

// Handles a change to an iframe's frame (html/#the-iframe-element). An iframe connected in the window's document
// contains a new browsing context, navigated to what its attributes name. This is its srcdoc, or the resource that its
// src names, fetched through its document's browsing context, or else about:blank. Any other iframe contains no
// browsing context. Its load event follows html/#iframe-load-event-steps.
void frameChanged(PrHtmlPage* self, PrDomElement* element) {
    ++domElementState(element)->generation;
    dropFrame(self, element);
    if (!inWindow(self, element))
        return;
    PrStringView srcdoc = domAttributeValue(element, "srcdoc");
    PrStringView src = domAttributeValue(element, "src");
    PrUrl* resolved = nullptr;
    if (srcdoc.data != nullptr) {
        attachFrame(self, element, srcdoc, "about:srcdoc", PrHtmlResponseHeaders{});
    } else if (src.size != 0 && domDocumentParseUrl(self->window->document, src, &resolved) == PrStatus_Ok) {
        attachFrame(self, element, "", "about:blank", PrHtmlResponseHeaders{}, FrameDocument_InitialBlank);
        self->children.data[self->children.count - 1]->awaitingSource = true;
        EmbedFetch* fetch = newEmbedFetch(self, element, prUrlSerialize(resolved, PrUrlSerialization_Whole));
        PrString request = prUrlSerialize(resolved, PrUrlSerialization_ExcludeFragment);
        startFetch(self, prStringView(&request), DomFetchDestination_Document,
            fetchSettingsOf(DomFetchDestination_Document, DomCorsSettings_NoCors), frameFetched, fetch, dropEmbedFetch);
        prStringReset(&request);
    } else {
        attachFrame(self, element, "", "about:blank", PrHtmlResponseHeaders{});
    }
    prUrlDestroy(resolved);
}

const PrSvgImage* PR_NULLABLE svgImageOf(PrStringView bytes, PrStringView fragment, PrImageCodec* PR_NULLABLE codec);

// Loads an image that an SVG image names (PrSvgLoadImageFn), with the codec that `context` points to. An SVG image is
// processed in secure static mode (svg-integration/#secure-static-mode), which loads no external resource, so only a
// data: URL, which holds its own bytes, loads.
bool loadSecureImage(void* PR_NULLABLE context, PrDomDocument* from, PrStringView url, PrSvgLoadedImage* out) {
    (void)from;
    PrImageCodec* codec = static_cast<PrImageCodec*>(context);
    PrString body{};
    PrString type{};
    if (prUrlProcessData(url, &body, &type) == PrStatus_Ok) {
        PrStringView bytes = prStringView(&body);
        if (htmlImageIsSvg(bytes)) {
            out->svg = svgImageOf(bytes, domUrlFragment(url), codec);
        } else if (codec != nullptr) {
            PrByteView view{reinterpret_cast<const uint8_t*>(bytes.data), bytes.size};
            prDrawImageCreateFromBytes(codec, view, &out->raster, nullptr);
        }
    }
    prStringReset(&body);
    prStringReset(&type);
    return out->raster != nullptr || out->svg != nullptr;
}

// Returns the SVG image `bytes`, named by a URL whose fragment is `fragment`, with a reference for the caller, or null
// if the bytes do not parse as an SVG document. The image is its own document, which loads only the images of data:
// URLs, with `codec`.
const PrSvgImage* PR_NULLABLE svgImageOf(PrStringView bytes, PrStringView fragment, PrImageCodec* PR_NULLABLE codec) {
    PrString decoded = {};
    PrCharset charset = PrCharset_None;
    PrDomDocument* document = prDomParseXml(domXmlDecode(bytes, PrCharset_None, &decoded, &charset), "image/svg+xml", nullptr);
    prCssEnable(document);
    prSvgEnable(document);
    prStringReset(&decoded);
    document->charset = charset;
    PrDomElement* root = prDomDocumentElement(document);
    PrSvgElement* svg = root != nullptr ? prSvgElementQuery(prDomElementAsEventTarget(root)) : nullptr;
    PrSvgDrawOptions options = prSvgDrawOptionsDefault();
    options.loadImage = loadSecureImage;
    options.loadContext = codec;
    const PrSvgImage* image = svg != nullptr ? prSvgImageCreate(svg, fragment, &options) : nullptr;
    prDomDocumentRelease(document);
    return image;
}

// Returns a painter of the SVG image `bytes`, which draws at whatever size the image is shown, with a reference for the
// caller, and writes its natural dimensions. `fragment` is the fragment of the URL that named the image. Returns a
// painter with no paint function if the bytes do not parse as an SVG document.
PrLayoutPainter svgPainter(PrHtmlPage* self, PrStringView bytes, PrStringView fragment, PrLayoutNaturalSize* natural) {
    const PrSvgImage* image = svgImageOf(bytes, fragment, self->host.images);
    if (image == nullptr)
        return PrLayoutPainter{};
    PrSvgNaturalSize size = prSvgImageNaturalSize(image);
    *natural = prLayoutNaturalSizeNone();
    if (size.width >= 0)
        natural->width = prLayoutUnitFromPx(size.width);
    if (size.height >= 0)
        natural->height = prLayoutUnitFromPx(size.height);
    natural->ratio = size.ratio;
    // The painter holds the image's only reference.
    return prSvgImagePainter(image);
}

// Decodes an image that a style or an SVG image element names (DomDocumentHost::decodeImage). An SVG image is drawn
// here, and the host decodes any other image.
bool decodeImage(void* PR_NULLABLE context, PrByteView bytes, PrStringView fragment, DomDecodedImage* out) {
    PrHtmlPage* self = static_cast<PrHtmlPage*>(context);
    PrStringView text{reinterpret_cast<const char*>(bytes.data), bytes.size};
    *out = DomDecodedImage{};
    uint32_t width = 0;
    uint32_t height = 0;
    if (!htmlImageSize(text, &width, &height))
        return false;
    if (htmlImageIsSvg(text)) {
        out->vector = svgPainter(self, text, fragment, &out->natural);
    } else if (self->host.images != nullptr &&
            prDrawImageCreateFromBytes(self->host.images, bytes, &out->image, &out->orientation) == PrStatus_Ok) {
        // The decoded image is in its orientation, which turns a header's width and height a quarter turn for some
        // orientations (css-images-3/#the-image-orientation).
        PrUVec2 extent = prDrawImageExtent(out->image);
        out->natural = prLayoutNaturalSizeNone();
        out->natural.width = prLayoutUnitFromPx(float(extent.x));
        out->natural.height = prLayoutUnitFromPx(float(extent.y));
    }
    return out->image != nullptr || out->vector.paint != nullptr;
}

// Sets the image that the element shows, according to its current request's state, and tells its presentation. An SVG
// image is drawn here, and the host decodes any other image. A decoded image's natural size is that of its oriented
// pixels. Divides the natural size of the current request by its pixel density
// (html/#density-corrected-intrinsic-width-and-height).
void correctForDensity(DomElementState* state) {
    if (state->density <= 0 || state->density == 1)
        return;
    state->naturalWidth = uint32_t(roundF64(double(state->naturalWidth) / state->density));
    state->naturalHeight = uint32_t(roundF64(double(state->naturalHeight) / state->density));
}

void showImage(PrHtmlPage* self, PrDomElement* element, PrStringView bytes) {
    DomElementState* state = element->state;
    prDrawImageRelease(state->picture);
    state->picture = nullptr;
    prDrawImageRelease(state->storedPicture);
    state->storedPicture = nullptr;
    state->orientation = PrImageOrientation_Identity;
    prLayoutPainterRelease(state->vector);
    state->vector = PrLayoutPainter{};
    if (state->image == PrDomImageState_CompletelyAvailable && htmlImageIsSvg(bytes)) {
        state->vector = svgPainter(self, bytes, domUrlFragment(prStringView(&state->imageUrl)), &state->vectorNatural);
    } else if (state->image == PrDomImageState_CompletelyAvailable && self->host.images != nullptr) {
        PrByteView view{reinterpret_cast<const uint8_t*>(bytes.data), bytes.size};
        PrImageOrientation orientation = PrImageOrientation_Identity;
        if (prDrawImageCreateFromBytes(self->host.images, view, &state->picture, &orientation) == PrStatus_Ok) {
            PrUVec2 extent = prDrawImageExtent(state->picture);
            state->naturalWidth = extent.x;
            state->naturalHeight = extent.y;
            correctForDensity(state);
            state->orientation = orientation;
        }
    }
    if (!domIsConnected(element))
        return;
    // Only the content of svg elements draws an SVG element's image.
    if (element->space == PrDomNamespace_Svg)
        svgContentImageChanged(element);
    else
        cssPresentationElementContentChanged(element);
}

// Receives the result of fetching an image's source: sets its natural size and image if it decodes, and queues its load
// or error event. Returns the image that the context's document fetched at `url`, without its fragment, or null if it
// fetched none.
const HtmlAvailableImage* PR_NULLABLE availableImage(const PrHtmlPage* self, PrStringView url) {
    for (uint32_t i = 0; i < self->availableImages.count; ++i) {
        if (prStringView(&self->availableImages.data[i].url) == url)
            return &self->availableImages.data[i];
    }
    return nullptr;
}

void dropAvailableImages(PrHtmlPage* self) {
    for (uint32_t i = 0; i < self->availableImages.count; ++i) {
        prStringReset(&self->availableImages.data[i].url);
        prStringReset(&self->availableImages.data[i].bytes);
    }
    self->availableImages.reset();
}

void imageFetched(void* PR_NULLABLE data, const DomFetchResult* result) {
    EmbedFetch* fetch = static_cast<EmbedFetch*>(data);
    if (isCurrent(fetch)) {
        DomElementState* state = fetch->element->state;
        bool loaded = result->status == PrStatus_Ok &&
            htmlImageSize(result->body, &state->naturalWidth, &state->naturalHeight);
        correctForDensity(state);
        PrStringView url = prStringView(&fetch->url);
        PrStringView request = stringSlice(url, 0, stringFind(url, '#'));
        if (loaded && availableImage(fetch->page, request) == nullptr)
            fetch->page->availableImages.push(HtmlAvailableImage{prStringCopy(request), prStringCopy(result->body)});
        if (loaded)
            state->image = PrDomImageState_CompletelyAvailable;
        showImage(fetch->page, fetch->element, loaded ? result->body : PrStringView{});
        queueElementEvent(fetch->page, fetch->element, !loaded);
    }
    dropEmbedFetch(fetch);
}

// html/#update-the-image-data for an img or an image button whose source is `source`, which is its src attribute, or
// the null view if it has none. An image that the document has already fetched is shown at once. Any other image is
// fetched at once. Its load or error event waits for a task. Starts the request of the image `source` with the pixel
// density `density`, which replaces the current request (html/#update-the-image-data).
void updateImage(PrHtmlPage* self, PrDomElement* element, PrStringView source, double density = 1) {
    DomElementState* state = domElementState(element);
    ++state->generation;
    state->density = density;
    prStringReset(&state->imageUrl);
    state->image = PrDomImageState_Broken;
    state->naturalWidth = 0;
    state->naturalHeight = 0;
    showImage(self, element, PrStringView{});
    if (source.size == 0) {
        if (source.data != nullptr)
            queueElementEvent(self, element, true);
        return;
    }
    PrUrl* url = nullptr;
    if (domDocumentParseUrl(element->document, source, &url) == PrStatus_Ok) {
        state->imageUrl = prUrlSerialize(url, PrUrlSerialization_Whole);
        PrString request = prUrlSerialize(url, PrUrlSerialization_ExcludeFragment);
        if (const HtmlAvailableImage* available = availableImage(self, prStringView(&request))) {
            htmlImageSize(prStringView(&available->bytes), &state->naturalWidth, &state->naturalHeight);
            correctForDensity(state);
            state->image = PrDomImageState_CompletelyAvailable;
            showImage(self, element, prStringView(&available->bytes));
            queueElementEvent(self, element, false);
        } else {
            EmbedFetch* fetch = newEmbedFetch(self, element, prUrlSerialize(url, PrUrlSerialization_Whole));
            startFetch(self, prStringView(&request), DomFetchDestination_Image,
                fetchSettingsOf(DomFetchDestination_Image, corsOf(element)), imageFetched, fetch, dropEmbedFetch);
        }
        prStringReset(&request);
    } else {
        queueElementEvent(self, element, true);
    }
    prUrlDestroy(url);
}

// Updates an img element's image to the source that it selects for the window's display (html_image_source.h).
void updateImageSource(PrHtmlPage* self, PrDomElement* element) {
    const CssMediaEnvironment* media = &self->window->media;
    HtmlImageEnvironment environment{
        .media = media,
        .pixelRatio = double(cssMediaEnvironmentPixelRatio(media)),
        .viewportWidth = media->features[PrCssDescriptor_MediaWidth - kCssDescriptorMediaFirst].number,
        .fontSize = media->fontSize,
        .images = self->host.images,
    };
    HtmlImageSource source = htmlSelectImageSource(element, environment);
    updateImage(self, element, source.url, source.density);
}

// Updates the image of each img element from `next` on among the children of `parent`, if `parent` is a picture: the
// img elements whose source sets a change to a source element before them can change. An img that its browsing context
// has not updated yet selects its source when it is first connected.
void sourceChanged(PrHtmlPage* self, PrDomNode* parent, PrDomNode* PR_NULLABLE next) {
    PrDomElement* picture = domElementFromNode(parent);
    if (picture == nullptr || !domIsHtml(picture, PrDomTag_Picture))
        return;
    for (PrDomNode* at = next; at != nullptr; at = at->next) {
        PrDomElement* img = domElementFromNode(at);
        if (img != nullptr && domIsHtml(img, PrDomTag_Img) && img->state != nullptr && img->state->generation != 0)
            updateImageSource(self, img);
    }
}

// Loads the image that an SVG image or feImage element shows, while it is connected (SVG2/embedded.html#ImageElement).
// The element loads once for each URL that it names in turn, so that moving it within its document loads nothing again.
void updateSvgImage(PrHtmlPage* self, PrDomElement* element) {
    if (!domIsConnected(element))
        return;
    PrStringView source = svgImageSource(element);
    PrUrl* url = nullptr;
    if (source.size != 0 && domDocumentParseUrl(element->document, source, &url) == PrStatus_Ok) {
        PrString whole = prUrlSerialize(url, PrUrlSerialization_Whole);
        bool same = element->state != nullptr && prStringView(&element->state->imageUrl) == prStringView(&whole);
        prStringReset(&whole);
        prUrlDestroy(url);
        if (same)
            return;
    }
    updateImage(self, element, source);
}

// Receives the result of fetching an object's data. The type attribute, or else the resource's own type, decides how
// the result is shown. HTML and an XML MIME type, which includes an SVG image's, are shown as a document in the nested
// browsing context. Another image type is shown as an image. Any other type shows the element's fallback content, which
// a plugin would otherwise replace. A resource that fails to fetch fires error.
void objectFetched(void* PR_NULLABLE data, const DomFetchResult* result) {
    EmbedFetch* fetch = static_cast<EmbedFetch*>(data);
    PrDomElement* element = fetch->element;
    if (isCurrent(fetch) && inWindow(fetch->page, element)) {
        DomElementState* state = element->state;
        PrStringView type = domAttributeValue(element, "type");
        PrStringView essence = prMimeTypeEssence(type.size != 0 ? type : result->contentType);
        if (!domFetchIsOk(result)) {
            queueElementEvent(fetch->page, element, true);
        } else if (asciiStartsWithFold(essence, "image/") && !prMimeTypeIsXml(essence)) {
            bool decoded = htmlImageSize(result->body, &state->naturalWidth, &state->naturalHeight);
            if (decoded)
                state->image = PrDomImageState_CompletelyAvailable;
            showImage(fetch->page, element, decoded ? result->body : PrStringView{});
            queueElementEvent(fetch->page, element, !decoded);
        } else if (asciiEqualsFold(essence, "text/html") || prMimeTypeIsXml(essence)) {
            attachFrame(fetch->page, element, result->body, prStringView(&fetch->url),
                PrHtmlResponseHeaders{essence, result->contentLanguage});
        }
    }
    dropEmbedFetch(fetch);
}

// Handles a change to an object element's content (html/#the-object-element) by fetching the resource that its data
// attribute names.
void objectChanged(PrHtmlPage* self, PrDomElement* element) {
    DomElementState* state = domElementState(element);
    ++state->generation;
    state->image = PrDomImageState_Broken;
    state->naturalWidth = 0;
    state->naturalHeight = 0;
    showImage(self, element, PrStringView{});
    dropFrame(self, element);
    PrStringView data = domAttributeValue(element, "data");
    if (!inWindow(self, element) || data.size == 0)
        return;
    PrUrl* url = nullptr;
    if (domDocumentParseUrl(element->document, data, &url) == PrStatus_Ok) {
        EmbedFetch* fetch = newEmbedFetch(self, element, prUrlSerialize(url, PrUrlSerialization_Whole));
        PrString request = prUrlSerialize(url, PrUrlSerialization_ExcludeFragment);
        startFetch(self, prStringView(&request), DomFetchDestination_Object,
            fetchSettingsOf(DomFetchDestination_Object, DomCorsSettings_NoCors), objectFetched, fetch, dropEmbedFetch);
        prStringReset(&request);
    } else {
        queueElementEvent(self, element, true);
    }
    prUrlDestroy(url);
}

// Returns true if the element has activation behavior that follows its hyperlink: an a or area element
// (html/#the-a-element, html/#the-area-element), or an SVG a element that is a hyperlink (SVG2/linking.html#AElement).
bool activates(void* PR_NULLABLE, const PrDomElement* element) {
    if (element->space == PrDomNamespace_Svg)
        return element->tag == PrDomTag_A && domElementLink(element);
    return domIsHtml(element, PrDomTag_A) || domIsHtml(element, PrDomTag_Area);
}

void activate(void* PR_NULLABLE context, PrDomElement* element, PrDomEvent*) {
    htmlWindowFollowHyperlink(static_cast<PrHtmlPage*>(context)->window, element);
}

// Handles a change to embedded content that the browsing context of its document loads. An image updates when its
// source changes, and once when it is first connected, if the parser created it. An SVG image or feImage updates when
// its href changes and when it is connected.
void embedChanged(void* PR_NULLABLE context, PrDomElement* element, PrStringView attribute) {
    PrHtmlPage* self = static_cast<PrHtmlPage*>(context);
    bool connection = attribute.data == nullptr;
    if (element->space == PrDomNamespace_Svg) {
        if (connection || attribute == "href")
            updateSvgImage(self, element);
        return;
    }
    bool fresh = element->state == nullptr || element->state->generation == 0;
    switch (element->tag) {
        case PrDomTag_Iframe:
            if (connection || attribute == "src" || attribute == "srcdoc")
                frameChanged(self, element);
            return;
        case PrDomTag_Object:
            if (connection || attribute == "data" || attribute == "type")
                objectChanged(self, element);
            return;
        case PrDomTag_Audio:
        case PrDomTag_Video:
            htmlMediaChanged(element, attribute);
            return;
        case PrDomTag_Canvas:
            htmlCanvasChanged(element, attribute);
            return;
        case PrDomTag_Img: {
            // A change of the CORS settings changes the request, and so does an insertion into a picture, whose sources
            // the img selects from (html/#reacting-to-dom-mutations).
            PrDomElement* parent = domElementFromNode(element->parent);
            bool pictured = parent != nullptr && domIsHtml(parent, PrDomTag_Picture);
            bool requested = attribute == "src" || attribute == "srcset" || attribute == "sizes" || attribute == "crossorigin";
            if (connection ? fresh || pictured : requested)
                updateImageSource(self, element);
            return;
        }
        case PrDomTag_Source:
            // A source of a picture offers the images of the img elements after it (html/#reacting-to-dom-mutations).
            if (element->parent != nullptr &&
                    (connection || attribute == "srcset" || attribute == "sizes" || attribute == "media" || attribute == "type"))
                sourceChanged(self, element->parent, element->next);
            return;
        case PrDomTag_Input:
            if (connection ? fresh && domElementIsImageButton(element) : attribute == "src" || attribute == "type") {
                if (domElementIsImageButton(element))
                    updateImage(self, element, domAttributeValue(element, "src"));
                else if (!fresh)
                    ++element->state->generation;
            }
            return;
        default:
            return;
    }
}

// Handles the removal of embedded content from `oldParent`. A source removed from a picture no longer offers the images
// of the img elements that followed it (html/#reacting-to-dom-mutations).
void embedRemoved(void* PR_NULLABLE context, PrDomElement* element, PrDomNode* oldParent, PrDomNode* PR_NULLABLE oldNext) {
    if (domIsHtml(element, PrDomTag_Source))
        sourceChanged(static_cast<PrHtmlPage*>(context), oldParent, oldNext);
}

// Brings a frame's viewport up to date as its iframe's content box, after the parent's layout. A frame whose iframe has
// no box keeps its last viewport.
void updateFrameViewport(PrHtmlPage* self) {
    PrHtmlWindow* parentWindow = self->window->parent;
    PrHtmlPage* parent = parentWindow != nullptr ? htmlDocumentPage(parentWindow->document) : nullptr;
    if (parent == nullptr || parent->presentation == nullptr)
        return;
    PrDomElement* container = nullptr;
    for (uint32_t i = 0; i < parent->window->frames.count && container == nullptr; ++i) {
        if (parent->window->frames.data[i].window == self->window)
            container = parent->window->frames.data[i].container;
    }
    if (container == nullptr)
        return;
    cssPresentationFlush(parent->presentation);
    PrLayoutNode box = cssPresentationBox(parent->presentation, prDomElementAsNode(container));
    if (box == kPrLayoutNodeNone)
        return;
    PrLayoutRect content = prLayoutNodeReplacedContent(cssPresentationOwnTree(parent->presentation), box);
    PrVec2 extent = prLayoutVecToPx(prLayoutRectExtent(content));
    if (extent.x > 0 && extent.y > 0)
        prHtmlPageSetViewport(self, extent);
}

// The document host's viewport (DomDocumentHost::viewport): the page's, after a frame's is brought up to date.
bool viewportNow(void* PR_NULLABLE context, PrVec2* out) {
    PrHtmlPage* self = static_cast<PrHtmlPage*>(context);
    updateFrameViewport(self);
    *out = self->viewport;
    return true;
}

// Brings the presentation of the window's document up to date, as a synchronous query needs
// (DomDocumentHost::updatePresentation), in the viewport that it has now. The content of an svg element whose box the
// layout resized is built for the box's new extent by a second update.
void updatePresentationNow(void* PR_NULLABLE context) {
    PrHtmlPage* self = static_cast<PrHtmlPage*>(context);
    PrCssPresentation* presentation = self->presentation;
    if (presentation == nullptr)
        return;
    PrVec2 viewport{};
    viewportNow(self, &viewport);
    PrLayoutVec extent = prLayoutVecFromPx(viewport);
    prHtmlPresentationUpdate(presentation, extent);
    if (cssPresentationSvgsResized(presentation))
        prHtmlPresentationUpdate(presentation, extent);
}

// Erases the listeners and event handlers of `root` and its shadow-including descendants.
void eraseListeners(PrDomNode* root) {
    for (PrDomNode* node = root; node != nullptr; node = domNodeNext(root, node)) {
        domListenersErase(node);
        if (PrDomElement* element = domElementFromNode(node)) {
            if (PrDomShadowRoot* shadow = domShadowRootOfHost(element))
                eraseListeners(shadow);
        }
    }
}

// html/#update-the-current-document-readiness
void setReadiness(PrDomDocument* document, DomReadiness readiness) {
    if (document->readiness == readiness)
        return;
    document->readiness = readiness;
    cssFontsReadinessChanged(document);
    domEventFire(document, "readystatechange");
}

// Prepares the context's parser for `document`. The parser runs the scripts that it inserts if the host runs scripts.
void startParser(PrHtmlPage* self, PrDomDocument* document) {
    bool scripting = self->host.execute != nullptr;
    HtmlParseFlags flags = HtmlParseFlag_DeclarativeShadowRoots;
    if (scripting)
        flags |= HtmlParseFlag_Scripting;
    htmlParserInit(&self->parser, document, flags);
    self->parser.host = HtmlParserHost{self, scripting ? prepareScript : nullptr, changeEncoding};
}

// Prepares the context's XML parser for the window's document. The parser runs the scripts that it closes if the host
// runs scripts.
void startXmlParser(PrHtmlPage* self) {
    DomXmlScripting scripting = {self, self->host.execute != nullptr ? prepareXmlScript : nullptr};
    domXmlBuilderInit(&self->xml, self->window->document, scripting, htmlXhtmlEntities());
}

// Returns true if script may open, write to or close the document. This holds for an HTML document whose markup
// insertion does not throw.
bool takesDynamicMarkup(const PrDomDocument* document) {
    return document->kind == DomDocumentKind_Html && document->throwOnDynamicMarkupInsertion == 0;
}

// Releases the scripts that the context waits to run, and their waits for module graphs, when its load is aborted or
// its document is replaced.
void releaseWaiting(PrHtmlPage* self) {
    if (self->blocking != nullptr)
        releaseScript(self->blocking);
    self->blocking = nullptr;
    for (uint32_t i = 0; i < self->deferred.count; ++i)
        releaseScript(self->deferred.data[i]);
    self->deferred.clear();
    for (uint32_t i = 0; i < self->soon.count; ++i)
        releaseScript(self->soon.data[i]);
    self->soon.clear();
    for (uint32_t i = 0; i < self->inOrder.count; ++i)
        releaseScript(self->inOrder.data[i]);
    self->inOrder.clear();
    for (uint32_t i = 0; i < self->moduleWaits.count;) {
        if (self->moduleWaits.data[i].script != nullptr)
            self->moduleWaits.erase(i);
        else
            ++i;
    }
    self->ending = false;
}

// Runs html/#the-end as far as it can go. The scripts that the parse deferred run in order, each once it is fetched and
// no style sheet blocks scripts. Then the document is notified that its content is loaded. Once nothing delays its load
// event, the document is complete. If `settleAtEnd` is set, the host first finishes the work that scripts left pending.
// Returns when it must wait for something, and continues when advance calls it again. Returns true if a frame of the
// context's window has a document whose load has not ended, which delays the window's load event
// (html/#delay-the-load-event).
bool framesLoading(const PrHtmlPage* self) {
    for (uint32_t i = 0; i < self->window->frames.count; ++i) {
        const PrHtmlPage* frame = htmlDocumentPage(self->window->frames.data[i].window->document);
        if (frame != nullptr && !frame->loadEnded)
            return true;
    }
    return false;
}

void continueEnd(PrHtmlPage* self);

// Ends the load of a frame's document for the element that holds the frame: the element's load event follows
// (html/#iframe-load-event-steps), and the end of the holder's own load goes on, which the frame delayed.
void frameLoadEnded(PrHtmlPage* self) {
    self->loadEnded = true;
    if (self->host.loaded != nullptr)
        self->host.loaded(self->host.context, self,
            self->endStatus != PrStatus_Ok ? self->endStatus : self->navigationFailure);
    PrHtmlWindow* parent = self->window->parent;
    PrHtmlPage* holder = parent != nullptr ? htmlDocumentPage(parent->document) : nullptr;
    if (holder == nullptr || self->initialBlank)
        return;
    for (uint32_t i = 0; i < parent->frames.count; ++i) {
        if (parent->frames.data[i].window == self->window)
            queueElementEvent(holder, parent->frames.data[i].container, false);
    }
    continueEnd(holder);
}

void continueEnd(PrHtmlPage* self) {
    if (!self->ending || self->continuing)
        return;
    self->continuing = true;
    PrDomDocument* document = self->window->document;
    while (self->deferred.count != 0 && self->endStatus == PrStatus_Ok) {
        PreparedScript* script = self->deferred.data[0];
        if (!script->ready || cssDocumentHasBlockingSheet(document)) {
            self->continuing = false;
            return;
        }
        self->deferred.erase(0);
        self->endStatus = executeScript(self, script);
        releaseScript(script);
    }
    if (!self->contentLoaded) {
        self->contentLoaded = true;
        if (self->endStatus == PrStatus_Ok)
            domEventFire(document, "DOMContentLoaded", {.bubbles = true});
        if (self->endStatus == PrStatus_Ok && self->settleAtEnd && self->host.settle != nullptr) {
            self->settling = true;
            self->endStatus = self->host.settle(self->host.context, self);
            self->settling = false;
        }
    }
    if (self->endStatus == PrStatus_Ok)
        htmlWindowRunLoadDelaying(self->window);
    // The load event waits for the fetches that delay it, for the scripts that run as soon as possible, and for the
    // loads of its frames' documents.
    bool delayed =
        self->delayingFetches != 0 || self->soon.count != 0 || self->inOrder.count != 0 || framesLoading(self);
    if (self->endStatus == PrStatus_Ok && delayed) {
        self->continuing = false;
        return;
    }
    // It waits for the tasks that delay it too, which cannot run during a task. A task queued after them goes on.
    if (self->endStatus == PrStatus_Ok && self->window->delayingTasks != 0) {
        self->continuing = false;
        queueAdvance(self);
        return;
    }
    if (self->endStatus != PrStatus_Ok)
        releaseWaiting(self);
    self->ending = false;
    self->continuing = false;
    if (self->endStatus == PrStatus_Ok) {
        fireElementEvents(self);
        // The load event is fired at the window with the legacy target override flag, so its target is the document.
        setReadiness(document, DomReadiness_Complete);
        domEventFire(self->window, "load", {}, document);
    }
    frameLoadEnded(self);
}

// Runs html/#the-end once the parse of the window's document stops. The document becomes interactive, and the end goes
// as far as it can (continueEnd). Returns the failure of a script that it ran, or `status`, the status of the parse. A
// parse that a script started does not settle, since that script is still running.
PrStatus finishParse(PrHtmlPage* page, PrStatus status, bool settle) {
    setReadiness(page->window->document, DomReadiness_Interactive);
    page->ending = true;
    page->contentLoaded = false;
    page->settleAtEnd = settle;
    page->endStatus = status;
    continueEnd(page);
    return page->endStatus;
}

// Finishes a script-created parser whose input is closed and which has parsed to its end. The parser releases its
// document, and the parse ends (html/#the-end). An iframe that holds the window runs its load event steps
// (html/#iframe-load-event-steps).
PrStatus finishScriptCreated(PrHtmlPage* self) {
    PrStatus status = self->parser.status;
    htmlParserReset(&self->parser);
    self->scriptCreated = false;
    return finishParse(self, status, false);
}

// Parses the contents of a script-created parser's input, and finishes the parser if its input is closed and parsed.
PrStatus runScriptCreated(PrHtmlPage* self, void (*step)(HtmlParser* parser, PrStringView text), PrStringView text) {
    self->parsing = true;
    step(&self->parser, text);
    self->parsing = false;
    if (self->parser.status != PrStatus_Ok || (!self->parser.tokenizer.open && self->parser.stopped))
        return finishScriptCreated(self);
    return PrStatus_Ok;
}

void closeStep(HtmlParser* parser, PrStringView) {
    htmlParserClose(parser);
}

// Returns the first language tag of a Content-Language header's value, or the null view if it names none.
PrStringView firstLanguage(PrStringView header) {
    PrStringView first = stringTrim(stringSlice(header, 0, stringFind(header, ',')), asciiIsSpace);
    return first.size != 0 ? first : PrStringView{nullptr, 0};
}

void rendered(void* context);

// Gives `window` what every window of the page shares: the host's clock and services, and the page's rendering
// callback. Starts the window's clock.
void shareWithWindow(PrHtmlPage* self, PrHtmlWindow* window) {
    window->clock = self->host.now;
    window->clockContext = self->host.context;
    window->storage[PrHtmlStorageKind_Local].provider = self->host.localStorage;
    window->storage[PrHtmlStorageKind_Session].provider = self->host.sessionStorage;
    window->device = self->host.device;
    window->images = self->host.images;
    window->mediaCodec = self->host.media;
    if (self->host.fetcher != nullptr)
        window->userAgent = prFetchNetworkUserAgent(self->host.fetcher);
    window->navigatorCompatibilityMode = self->host.navigatorCompatibilityMode;
    window->animationFrames = self->host.animationFrames;
    if (self->host.rendered != nullptr) {
        window->rendered = rendered;
        window->renderedContext = self;
    }
    htmlWindowStartClock(window);
}

// Closes the page's window. Its host learns of it first, and then the window's tasks, timers, media, listeners and
// storage objects go. The window lives on while references to it remain.
void closeWindow(PrHtmlPage* self) {
    if (self->host.windowClosed != nullptr)
        self->host.windowClosed(self->host.context, self);
    PrHtmlWindow* window = self->window;
    htmlWindowClearTasks(window);
    htmlMediaReset(window);
    htmlCanvasReset(window);
    domListenersDestroy(window->listeners);
    window->listeners = nullptr;
    for (PrHtmlPreferenceObject& preference : window->preferences) {
        domListenersDestroy(preference.listeners);
        preference.listeners = nullptr;
    }
    domListenersDestroy(window->performance.listeners);
    window->performance.listeners = nullptr;
    domListenersDestroy(window->orientation.listeners);
    window->orientation.listeners = nullptr;
    for (PrHtmlStorage& storage : window->storage)
        htmlStorageReset(&storage);
}

// Replaces the page's window, which closeWindow closed, with the window of the document that a navigation commits
// (html/#initialise-the-document-object). The new window has the page's agent, session history and parent, and the
// media environment of the window before it, without the preferences that its scripts overrode. The parent's record of
// the iframe names the new window.
void replaceWindow(PrHtmlPage* self) {
    PrHtmlWindow* closed = self->window;
    PrHtmlWindow* window = htmlWindowCreate();
    shareWithWindow(self, window);
    window->agent = closed->agent;
    window->history = closed->history;
    window->parent = closed->parent;
    window->media = closed->media;
    for (CssMediaValue& value : window->media.overrides)
        value.keyword = PrCssKeyword_Unknown;
    // A load whose input still arrives, such as one that a meta element restarts in another charset, goes on in the new
    // window's document.
    window->streaming = closed->streaming;
    if (window->parent != nullptr) {
        for (uint32_t i = 0; i < window->parent->frames.count; ++i) {
            if (window->parent->frames.data[i].window == closed)
                window->parent->frames.data[i].window = window;
        }
    }
    self->window = window;
    domEventTargetRelease(closed);
}

// Starts a new document at `url` for a load of a response with `headers`. The window's last document is discarded, and
// the new document is loading. A response without a Content-Type is HTML.
PrStatus beginDocument(PrHtmlPage* page, PrStringView url, const PrHtmlResponseHeaders* PR_NULLABLE headers) {
    PrHtmlResponseHeaders given = headers != nullptr ? *headers : PrHtmlResponseHeaders{};
    PrStringView contentType = given.contentType.data != nullptr ? given.contentType : "text/html";
    PrUrl* documentUrl = nullptr;
    PrStatus status = prUrlCreate(url, nullptr, PrCharset_Utf8, &documentUrl);
    if (status != PrStatus_Ok)
        return status;
    if (page->scriptCreated) {
        htmlParserReset(&page->parser);
        page->scriptCreated = false;
    }
    if (page->loaded) {
        dropMarkdown(page);
        dropFetches(page);
        domXmlBuilderReset(&page->xml);
        dropAvailableImages(page);
        releaseWaiting(page);
        dropModules(page);
        dropElementEvents(page);
        closeWindow(page);
        discardDocument(page);
        replaceWindow(page);
        newDocument(page);
        if (page->host.windowOpened != nullptr)
            page->host.windowOpened(page->host.context, page);
    }
    PrDomDocument* document = page->window->document;
    prUrlDestroy(document->url);
    document->url = documentUrl;
    // The document takes its entry in the session history, and the history state is the entry's.
    PrString committed = prUrlSerialize(documentUrl, PrUrlSerialization_Whole);
    page->window->historyDocument = htmlHistoryCommit(page->window->history, prStringView(&committed), false);
    ++page->window->historyGeneration;
    prStringReset(&committed);
    htmlWindowStartClock(page->window);
    page->window->revealing = true;
    document->kind = prMimeTypeIsXml(contentType) ? DomDocumentKind_Xml : DomDocumentKind_Html;
    TextBuffer essence;
    textAppend(&essence, prMimeTypeEssence(contentType));
    textAsciiLower(&essence);
    prStringReset(&document->contentType);
    document->contentType = textTake(&essence);
    page->markdownOptions = markdownOptionsOf(contentType);
    prStringReset(&document->languagePragma);
    prStringReset(&document->languageHeader);
    PrStringView language = firstLanguage(given.contentLanguage);
    if (language.data != nullptr)
        document->languageHeader = prStringCopy(language);
    page->locationAt = 0;
    page->line = 1;
    page->column = 1;
    page->loaded = true;
    page->loadEnded = false;
    document->readiness = DomReadiness_Loading;
    return PrStatus_Ok;
}

// Returns the element that shows a resource of MIME type `essence` in a media document (html/#read-media), or
// PrDomTag_Unknown if the resource is not an image, a video or an audio resource. An XML MIME type, such as an SVG
// image's, is never media, since the XML parser loads it as a document.
PrDomTag mediaHostOf(PrStringView essence) {
    if (prMimeTypeIsXml(essence))
        return PrDomTag_Unknown;
    if (asciiStartsWithFold(essence, "image/"))
        return PrDomTag_Img;
    if (asciiStartsWithFold(essence, "video/"))
        return PrDomTag_Video;
    if (asciiStartsWithFold(essence, "audio/"))
        return PrDomTag_Audio;
    return PrDomTag_Unknown;
}

// Loads the window's new document as a media document (html/#read-media), whose body holds one element of `tag` with
// the document's URL as its source, and stops its parse. The element fetches the resource itself, so the response's
// bytes are not read. Returns the status of the end of the load (finishParse).
PrStatus loadMediaDocument(PrHtmlPage* page, PrDomTag tag) {
    PrDomDocument* document = page->window->document;
    page->loading = true;
    document->quirks = DomQuirks_No;
    PrDomElement* html = domElementCreate(document, PrDomNamespace_Html, PrDomTag_Html, PrStringView{});
    domInsert(document, html, nullptr);
    domInsert(html, domElementCreate(document, PrDomNamespace_Html, PrDomTag_Head, PrStringView{}), nullptr);
    PrDomElement* body = domElementCreate(document, PrDomNamespace_Html, PrDomTag_Body, PrStringView{});
    domInsert(html, body, nullptr);
    PrDomElement* host = domElementCreate(document, PrDomNamespace_Html, tag, PrStringView{});
    PrString source = prUrlSerialize(document->url, PrUrlSerialization_Whole);
    domSetAttribute(host, PrDomNamespace_None, "src", prStringView(&source));
    prStringReset(&source);
    domInsert(body, host, nullptr);
    PrStatus status = finishParse(page, PrStatus_Ok, true);
    page->loading = false;
    return status;
}

// Runs the XML parse of the window's document as far as its input has arrived, and returns its status. A document that
// is not well-formed shows its error once its parse ends, and its load goes on as for any other document, with Ok.
PrStatus runXmlParser(PrHtmlPage* self) {
    self->parsing = true;
    PrStatus status = prXmlParserRun(self->xml.parser);
    self->parsing = false;
    if (status != PrStatus_Syntax)
        return status;
    htmlShowXmlError(self->window->document, prXmlParserError(self->xml.parser));
    return PrStatus_Ok;
}

// Continues a load of a whole source in tasks, in the same way as a streamed load whose bytes have all arrived and been
// decoded: a parse that waits for a script.
void continueInTasks(PrHtmlPage* page, PrStringView source) {
    page->window->streaming = true;
    page->streamInput.text.clear();
    textAppend(&page->streamInput, source);
    page->streamRead = page->streamInput.text.count;
    page->streamEnded = true;
    page->streamQueued = false;
    page->streamSniffed = true;
    page->streamFlushed = true;
    page->loading = false;
}

// Returns the charset that the Content-Type of a load's response names, or PrCharset_None.
PrCharset transportCharsetOf(const PrHtmlResponseHeaders* PR_NULLABLE headers) {
    if (headers == nullptr || headers->contentType.data == nullptr)
        return PrCharset_None;
    return prCharsetFromMimeType(headers->contentType);
}

// Starts the load's document again, to parse its bytes in the charset that a meta element named
// (html/#change-the-encoding). The new document replaces the old, as a navigation to it that replaces the old entry
// would. The parse is certain of the new charset.
PrStatus restartDocument(PrHtmlPage* self) {
    PrCharset charset = self->reparse;
    self->reparse = PrCharset_None;
    PrDomDocument* old = self->window->document;
    PrString url = prUrlSerialize(old->url, PrUrlSerialization_Whole);
    PrString contentType = prStringClone(&old->contentType);
    PrString language = prStringClone(&old->languageHeader);
    PrHtmlResponseHeaders headers = {prStringView(&contentType), prStringView(&language)};
    htmlParserReset(&self->parser);
    PrStatus status = beginDocument(self, prStringView(&url), &headers);
    prStringReset(&url);
    prStringReset(&contentType);
    prStringReset(&language);
    if (status == PrStatus_Ok)
        startDecoder(self, charset, HtmlConfidence_Certain);
    return status;
}

// Parses the whole source of a load as HTML, decoded in the load's charset.
void parseHtmlSource(PrHtmlPage* page, PrStringView source) {
    startParser(page, page->window->document);
    PrString decoded = {};
    page->loadSource = source;
    page->parsing = true;
    htmlParse(&page->parser, prCharsetDecodeView(&page->decoder, source, PrCharsetInput_Final, &decoded));
    page->parsing = false;
    page->loadSource = PrStringView{nullptr, 0};
    prStringReset(&decoded);
}

// The maximum number of bytes of a streamed document that one task parses, so that rendering opportunities come between
// tasks.
constexpr uint32_t kStreamSlice = 16 * 1024;

void parseStream(void* data);

// Queues a task to parse what has arrived of a streamed load, unless such a task is already queued.
void queueStream(PrHtmlPage* page) {
    if (page->streamQueued)
        return;
    page->streamQueued = true;
    htmlWindowQueueTask(page->window, parseStream, page, nullptr, true);
}

// Ends a load whose parse ran in tasks, by stopping the parse (html/#the-end).
void endStream(PrHtmlPage* page, PrStatus status) {
    page->window->streaming = false;
    page->streamInput.text.reset();
    page->streamRead = 0;
    if (page->window->document->kind == DomDocumentKind_Html)
        htmlParserReset(&page->parser);
    domXmlBuilderReset(&page->xml);
    finishParse(page, status, true);
}

// Parses a streamed load's bytes again from the start, in the charset that a meta element named.
void restartStream(PrHtmlPage* page) {
    PrStatus status = restartDocument(page);
    if (status != PrStatus_Ok) {
        endStream(page, status);
        return;
    }
    startParser(page, page->window->document);
    htmlParserOpen(&page->parser);
    page->streamRead = 0;
    page->streamFlushed = false;
    page->streamQueued = false;
    queueStream(page);
}

// Returns the charset that a Markdown load's bytes decode in: the one that a byte order mark gives, or else the one
// that the response's Content-Type names, which RFC 7763 makes the charset parameter of text/markdown, or else UTF-8.
// The HTML prescan does not apply, since Markdown has no meta element to declare a charset.
PrCharset markdownCharsetOf(const PrHtmlPage* page, PrStringView bytes) {
    PrCharset charset = prCharsetSniffBom(bytes);
    if (charset == PrCharset_None)
        charset = page->transportCharset;
    return charset != PrCharset_None ? charset : PrCharset_Utf8;
}

// Decodes the next slice of what has arrived of a streamed Markdown document and builds it, which runs no script that
// could pause it, and queues the task again while more remains. Once the input has ended and has been built, the load
// ends.
void parseMarkdownStream(PrHtmlPage* page, PrStringView input) {
    if (!page->streamSniffed) {
        if (!page->streamEnded && !prCharsetIsBomDecided(input))
            return;
        startDecoder(page, markdownCharsetOf(page, input), HtmlConfidence_Certain);
        page->streamSniffed = true;
    }
    uint32_t end;
    if (page->streamRead + kStreamSlice < input.size)
        end = page->streamRead + kStreamSlice;
    else
        end = uint32_t(input.size);
    bool last = page->streamEnded && end == input.size;
    if (end > page->streamRead || (last && !page->streamFlushed)) {
        PrString decoded = {};
        PrStringView text = prCharsetDecodeView(&page->decoder, stringSlice(input, page->streamRead,
            end - page->streamRead), last ? PrCharsetInput_Final : PrCharsetInput_Partial, &decoded);
        page->streamRead = end;
        page->streamFlushed = last;
        page->parsing = true;
        htmlMarkdownBuilderAppend(page->markdown, text);
        if (last)
            htmlMarkdownBuilderEnd(page->markdown);
        page->parsing = false;
        prStringReset(&decoded);
    }
    if (last) {
        dropMarkdown(page);
        endStream(page, PrStatus_Ok);
    } else if (page->streamRead < input.size) {
        queueStream(page);
    }
}

// Runs the XML parse as far as its input has arrived. The load ends once the parse ends. Otherwise the parse continues
// in a task while more of the load's bytes are to be decoded, or its end is yet to be given to the parser, unless a
// script paused it. The load's input may have ended while the parse was paused.
void continueXml(PrHtmlPage* page) {
    PrStatus status = runXmlParser(page);
    if (prXmlParserIsEnded(page->xml.parser))
        endStream(page, status);
    else if (!prXmlParserIsPaused(page->xml.parser) && (page->streamRead < page->streamInput.text.count ||
            (page->streamEnded && !page->streamFlushed)))
        queueStream(page);
}

// Decodes the next slice of what has arrived of a streamed XML document and parses it. The charset is decided before
// any of the document is decoded, from its byte order mark, its Content-Type or its XML declaration (domXmlCharset).
void parseXmlStream(PrHtmlPage* page, PrStringView input) {
    if (!page->streamSniffed) {
        PrCharset charset = PrCharset_None;
        if (!domXmlCharset(input, page->transportCharset, page->streamEnded, &charset))
            return;
        startDecoder(page, charset, HtmlConfidence_Certain);
        page->streamSniffed = true;
    }
    uint32_t end;
    if (page->streamRead + kStreamSlice < input.size)
        end = page->streamRead + kStreamSlice;
    else
        end = uint32_t(input.size);
    bool last = page->streamEnded && end == input.size;
    if (end > page->streamRead || (last && !page->streamFlushed)) {
        PrString decoded = {};
        PrStringView text = prCharsetDecodeView(&page->decoder, stringSlice(input, page->streamRead,
            end - page->streamRead), last ? PrCharsetInput_Final : PrCharsetInput_Partial, &decoded);
        page->streamRead = end;
        page->streamFlushed = last;
        prXmlParserAppend(page->xml.parser, text);
        if (last)
            prXmlParserEnd(page->xml.parser);
        prStringReset(&decoded);
    }
    continueXml(page);
}

// A task of a streamed load. Decodes and parses the next slice of what has arrived, and queues itself again while more
// remains. Once the input has ended and has been parsed, the parse stops.
void parseStream(void* data) {
    PrHtmlPage* page = static_cast<PrHtmlPage*>(data);
    page->streamQueued = false;
    // A parse that a script paused continues once the script runs (resumeParse).
    if (!page->window->streaming || parsePaused(page))
        return;
    PrStringView input = textView(&page->streamInput);
    if (page->window->document->kind == DomDocumentKind_Xml) {
        parseXmlStream(page, input);
        return;
    }
    if (page->reparse != PrCharset_None) {
        restartStream(page);
        return;
    }
    if (page->markdown != nullptr) {
        parseMarkdownStream(page, input);
        return;
    }
    if (!page->streamSniffed) {
        if (!sniffCharset(page, input, page->streamEnded))
            return;
        page->streamSniffed = true;
    }
    uint32_t end;
    if (page->streamRead + kStreamSlice < input.size)
        end = page->streamRead + kStreamSlice;
    else
        end = uint32_t(input.size);
    bool last = page->streamEnded && end == input.size;
    if (end > page->streamRead || (last && !page->streamFlushed)) {
        PrString decoded = {};
        PrStringView text = prCharsetDecodeView(&page->decoder, stringSlice(input, page->streamRead,
            end - page->streamRead), last ? PrCharsetInput_Final : PrCharsetInput_Partial, &decoded);
        page->streamRead = end;
        page->streamFlushed = last;
        page->parsing = true;
        htmlParserAppend(&page->parser, text);
        page->parsing = false;
        prStringReset(&decoded);
        if (page->reparse != PrCharset_None) {
            restartStream(page);
            return;
        }
        if (page->parser.status != PrStatus_Ok) {
            endStream(page, page->parser.status);
            return;
        }
        if (page->parser.paused)
            return;
    }
    if (page->streamRead < page->streamInput.text.count) {
        queueStream(page);
        return;
    }
    if (page->streamEnded) {
        page->parsing = true;
        htmlParserClose(&page->parser);
        page->parsing = false;
        if (page->parser.status != PrStatus_Ok || page->parser.stopped)
            endStream(page, page->parser.status);
    }
}

// Continues a parse once the script that paused it has run. The parse goes as far as its input reaches, then continues
// in tasks as more input arrives, and runs to its end if its input has ended.
void resumeParse(PrHtmlPage* self) {
    if (self->window->document->kind == DomDocumentKind_Xml) {
        continueXml(self);
        return;
    }
    self->parsing = true;
    htmlParserResume(&self->parser);
    self->parsing = false;
    HtmlParser* parser = &self->parser;
    if (self->scriptCreated) {
        if (parser->status != PrStatus_Ok || (!parser->tokenizer.open && parser->stopped))
            finishScriptCreated(self);
        return;
    }
    // A parse that a change of encoding abandoned starts again in a task of its own.
    if (self->reparse != PrCharset_None)
        queueStream(self);
    else if (parser->status != PrStatus_Ok || parser->stopped)
        endStream(self, parser->status);
    else if (!parser->paused)
        queueStream(self);
}

// Runs what has become ready. This is the pending parsing-blocking script, once it is fetched and no style sheet blocks
// scripts, after which the parse continues. It is also the scripts to run as soon as possible that have been fetched,
// and the rest of the end of a load.
void advance(PrHtmlPage* self) {
    if (self->parsing || self->runningScript)
        return;
    PreparedScript* script = self->blocking;
    if (script != nullptr && script->ready && !cssDocumentHasBlockingSheet(self->window->document)) {
        self->blocking = nullptr;
        self->parsing = true;
        PrStatus status = executeScript(self, script);
        self->parsing = false;
        releaseScript(script);
        // A script's failure stops the parse. The XML parser has no status of its own to carry it.
        if (status != PrStatus_Ok && self->window->document->kind == DomDocumentKind_Xml) {
            endStream(self, status);
        } else {
            if (status != PrStatus_Ok)
                self->parser.status = status;
            resumeParse(self);
        }
    }
    for (uint32_t i = 0; i < self->soon.count;) {
        PreparedScript* ready = self->soon.data[i];
        if (!ready->ready) {
            ++i;
            continue;
        }
        self->soon.erase(i);
        executeScript(self, ready);
        releaseScript(ready);
    }
    while (self->inOrder.count != 0 && self->inOrder.data[0]->ready) {
        PreparedScript* ready = self->inOrder.data[0];
        self->inOrder.erase(0);
        executeScript(self, ready);
        releaseScript(ready);
    }
    continueEnd(self);
}

void advanceTask(void* data) {
    PrHtmlPage* self = static_cast<PrHtmlPage*>(data);
    self->advanceQueued = false;
    advance(self);
}

// Queues a task that runs what has become ready, if a blocking script, a script to run as soon as possible or in order,
// or the end of the load waits.
void queueAdvance(PrHtmlPage* self) {
    bool waiting = self->blocking != nullptr || self->ending || self->soon.count != 0 || self->inOrder.count != 0;
    if (self->advanceQueued || !waiting)
        return;
    self->advanceQueued = true;
    htmlWindowQueueTask(self->window, advanceTask, self,
        [](void* data) { static_cast<PrHtmlPage*>(data)->advanceQueued = false; });
}

// The document host's notice that the style sheets that block scripts changed. A script that waits for them may now
// run.
void sheetsChanged(void* PR_NULLABLE context) {
    queueAdvance(static_cast<PrHtmlPage*>(context));
}

// Returns true if the agent's event loop runs the browsing context's tasks, which are its window's tasks. No tasks run
// while a load is on the stack, except from its settle hook.
bool runsTasks(const PrHtmlPage* page) {
    return !page->loading || page->settling;
}

double windowTaskDelay(void* global) {
    PrHtmlPage* page = static_cast<PrHtmlPage*>(global);
    return runsTasks(page) ? htmlWindowNextTaskDelay(page->window) : -1;
}

// The settle hook spins the event loop, from the task that ends a streamed load too.
bool windowRunTask(void* global) {
    PrHtmlPage* page = static_cast<PrHtmlPage*>(global);
    if (page->settling)
        return htmlWindowSpinTask(page->window);
    return runsTasks(page) && htmlWindowRunTask(page->window);
}

// A window is not idle while it waits for a fetch, whose response arrives as a task.
bool windowIsIdle(void* global) {
    PrHtmlPage* page = static_cast<PrHtmlPage*>(global);
    return page->fetches == nullptr && htmlWindowIsIdle(page->window);
}

constexpr DomGlobalTasks kWindowTasks = {windowTaskDelay, windowRunTask, windowIsIdle};

// Tells the host that a rendering update of the browsing context's window finished (PrHtmlHost::rendered).
void rendered(void* context) {
    PrHtmlPage* self = static_cast<PrHtmlPage*>(context);
    self->host.rendered(self->host.context, self);
}

// A navigation that a task hands to the host.
struct Navigation {
    PrHtmlPage* page;
    uint32_t id;
    PrString url;
    HtmlHistoryCommit commit;   // how the document that the navigation loads takes its entry
    uint32_t target;
};

void dropNavigation(void* data) {
    Navigation* navigation = static_cast<Navigation*>(data);
    prStringReset(&navigation->url);
    prMemoryFree(navigation);
}

void abortDocument(PrHtmlPage* self);

// Navigates the page to another document (html/#navigate), from the task that htmlPageNavigate queued. A document that
// is still loading stops (html/#abort-a-document).
void runNavigation(void* data) {
    Navigation* navigation = static_cast<Navigation*>(data);
    PrHtmlPage* page = navigation->page;
    if (page->pendingNavigation == navigation->id) {
        page->pendingNavigation = 0;
        abortDocument(page);
        htmlHistoryExpect(page->window->history, navigation->commit, navigation->target);
        prHtmlPageNavigate(page, prStringView(&navigation->url));
    }
    dropNavigation(navigation);
}

// The kinds of piece that the loop posts of a document's load.
enum {
    LoadPiece_Head,         // the response's URL and headers
    LoadPiece_Body,         // a part of its body
    LoadPiece_End,          // the end of its body
    LoadPiece_Fail,         // a network error, or a URL that the fetcher does not take
};
typedef uint8_t LoadPiece;

// A piece of a document's load, which a task carries from the loop to the agent.
struct DocumentLoadPiece {
    HtmlDocumentLoad* load;
    LoadPiece kind;
    PrString url;               // Head: the response's URL, with the fragment that the navigation asked for
    PrString contentType;       // Head: the headers' values, or the null string for a header that is absent
    PrString contentLanguage;
    TextBuffer bytes;           // Body
    PrNetError error;           // Fail: PrNetError_None for a URL that the fetcher does not take
};

void releaseDocumentLoad(HtmlDocumentLoad* load) {
    if (load->references.fetchSub(1) != 1)
        return;
    prStringReset(&load->url);
    prMutexReset(&load->lock);
    prMemoryFree(load);
}

void freePiece(DocumentLoadPiece* piece) {
    prStringReset(&piece->url);
    prStringReset(&piece->contentType);
    prStringReset(&piece->contentLanguage);
    textReset(&piece->bytes);
    prMemoryFree(piece);
}

// Returns why a navigation's fetch failed, as a sentence for the page that says so.
PrStringView failureReason(PrNetError error) {
    switch (error) {
        case PrNetError_None: return "The address is not one that can be fetched.";
        case PrNetError_NameNotResolved: return "The server's name was not found.";
        case PrNetError_Refused: return "The server refused the connection.";
        case PrNetError_Unreachable: return "The server cannot be reached.";
        case PrNetError_TimedOut: return "The connection timed out.";
        case PrNetError_Reset: return "The connection was reset.";
        case PrNetError_Protocol: return "The server's response was not accepted: its certificate, its TLS or its HTTP.";
        case PrNetError_Blocked: return "A policy blocked the request.";
        default: return "The network failed.";
    }
}

// Appends `text` to `out` with the characters that HTML reads as markup escaped.
void appendEscaped(TextBuffer* out, PrStringView text) {
    for (size_t i = 0; i < text.size; ++i) {
        char c = text.data[i];
        if (c == '<')
            textAppend(out, "&lt;");
        else if (c == '>')
            textAppend(out, "&gt;");
        else if (c == '&')
            textAppend(out, "&amp;");
        else
            textAppendChar(out, c);
    }
}

// Loads a document at `url` that says why the navigation to it failed, as a browser's error page does, and reports the
// failure at the end of its load.
void loadFailure(PrHtmlPage* self, PrStringView url, PrNetError error) {
    TextBuffer page{};
    textAppend(&page, "<!doctype html><title>Not loaded</title><body style=\"font: 16px system-ui; margin: 40px\">"
        "<h1>The page did not load</h1><p>");
    appendEscaped(&page, url);
    textAppend(&page, "</p><p>");
    appendEscaped(&page, failureReason(error));
    textAppend(&page, "</p>");
    PrHtmlResponseHeaders headers{.contentType = "text/html;charset=utf-8"};
    self->navigationFailure = PrStatus_Io;
    prHtmlPageLoad(self, textView(&page), url, &headers);
    self->navigationFailure = PrStatus_Ok;
    textReset(&page);
}

// Stops waiting for the page's document load. Its pieces in flight find no page, and its fetch is cancelled on the loop
// if it still runs.
void dropDocumentLoad(PrHtmlPage* self);

// Ends the load's part on the agent, once its fetch has ended or failed.
void endDocumentLoad(PrHtmlPage* self) {
    HtmlDocumentLoad* load = self->documentLoad;
    self->documentLoad = nullptr;
    load->page = nullptr;
    releaseDocumentLoad(load);
}

// Loads a piece that the loop posted, in a task of the agent, unless the page dropped the load.
void receivePiece(void* PR_NULLABLE data) {
    DocumentLoadPiece* piece = static_cast<DocumentLoadPiece*>(data);
    HtmlDocumentLoad* load = piece->load;
    PrHtmlPage* self = load->page;
    if (self != nullptr) {
        switch (piece->kind) {
            case LoadPiece_Head: {
                load->responded = true;
                PrHtmlResponseHeaders headers{
                    .contentType = prStringView(&piece->contentType),
                    .contentLanguage = prStringView(&piece->contentLanguage),
                };
                if (htmlPageBeginStreamedLoad(self, prStringView(&piece->url), &headers) != PrStatus_Ok) {
                    PrString url = prStringCopy(prStringView(&load->url));
                    dropDocumentLoad(self);
                    loadFailure(self, prStringView(&url), PrNetError_None);
                    prStringReset(&url);
                }
                break;
            }
            case LoadPiece_Body:
                prHtmlPageAppendLoad(self, textView(&piece->bytes));
                break;
            case LoadPiece_End:
                endDocumentLoad(self);
                prHtmlPageEndLoad(self);
                break;
            case LoadPiece_Fail: {
                PrString url = prStringCopy(prStringView(&load->url));
                endDocumentLoad(self);
                loadFailure(self, prStringView(&url), piece->error);
                prStringReset(&url);
                break;
            }
        }
    }
    releaseDocumentLoad(load);
    freePiece(piece);
}

// Posts a piece to the agent, on the loop, unless the page dropped the load.
void postPiece(HtmlDocumentLoad* load, DocumentLoadPiece* piece) {
    piece->load = load;
    prMutexLock(&load->lock);
    bool dropped = load->dropped;
    if (!dropped) {
        load->references.fetchAdd(1);
        prDomAgentPost(load->agent, receivePiece, piece);
    }
    prMutexUnlock(&load->lock);
    if (dropped)
        freePiece(piece);
}

// Ends the fetch on the loop, once, and releases the loop's reference.
void endLoadOnLoop(HtmlDocumentLoad* load) {
    load->loopDone = true;
    prFetchDestroy(load->fetch);
    load->fetch = nullptr;
    releaseDocumentLoad(load);
}

DocumentLoadPiece* newPiece(LoadPiece kind) {
    DocumentLoadPiece* piece = allocZeroed<DocumentLoadPiece>();
    piece->kind = kind;
    return piece;
}

// Posts what the fetch's body has, on the loop.
void postBody(HtmlDocumentLoad* load, PrFetch* fetch) {
    DocumentLoadPiece* piece = newPiece(LoadPiece_Body);
    char chunk[16 * 1024];
    for (size_t got = prFetchRead(fetch, reinterpret_cast<uint8_t*>(chunk), sizeof(chunk)); got != 0;
            got = prFetchRead(fetch, reinterpret_cast<uint8_t*>(chunk), sizeof(chunk)))
        textAppend(&piece->bytes, chunk, uint32_t(got));
    if (piece->bytes.text.count != 0)
        postPiece(load, piece);
    else
        freePiece(piece);
}

// Receives the document fetch's events on the loop.
void documentLoadEvent(void* PR_NULLABLE context, PrFetch* fetch, PrFetchEvent event) {
    HtmlDocumentLoad* load = static_cast<HtmlDocumentLoad*>(context);
    switch (event) {
        case PrFetchEvent_Response: {
            DocumentLoadPiece* piece = newPiece(LoadPiece_Head);
            // The document's URL keeps the fragment that the navigation asked for, which the response's URL has not.
            TextBuffer url{};
            textAppend(&url, prFetchUrl(fetch));
            PrStringView asked = prStringView(&load->url);
            for (size_t i = 0; i < asked.size; ++i) {
                if (asked.data[i] == '#') {
                    textAppend(&url, PrStringView{asked.data + i, asked.size - i});
                    break;
                }
            }
            piece->url = prStringCopy(textView(&url));
            textReset(&url);
            PrStringView type = prFetchHeaderValue(fetch, "content-type");
            PrStringView language = prFetchHeaderValue(fetch, "content-language");
            if (type.data != nullptr)
                piece->contentType = prStringCopy(type);
            if (language.data != nullptr)
                piece->contentLanguage = prStringCopy(language);
            postPiece(load, piece);
            break;
        }
        case PrFetchEvent_Readable:
            postBody(load, fetch);
            break;
        case PrFetchEvent_End:
            postBody(load, fetch);
            postPiece(load, newPiece(LoadPiece_End));
            endLoadOnLoop(load);
            break;
        case PrFetchEvent_Fail: {
            DocumentLoadPiece* piece = newPiece(LoadPiece_Fail);
            piece->error = prFetchError(fetch);
            postPiece(load, piece);
            endLoadOnLoop(load);
            break;
        }
    }
}

// Starts the document's fetch on the loop, as a navigation that the user starts: with no client, so from an opaque
// origin and with no referrer. A load that the page dropped first is left to its cancel task.
void beginLoadOnLoop(void* PR_NULLABLE data) {
    HtmlDocumentLoad* load = static_cast<HtmlDocumentLoad*>(data);
    prMutexLock(&load->lock);
    bool dropped = load->dropped;
    prMutexUnlock(&load->lock);
    if (dropped)
        return;
    PrFetchRequestDesc desc{
        .url = prStringView(&load->url),
        .destination = PrFetchDestination_Document,
        .mode = PrFetchMode_Navigate,
        .credentials = PrFetchCredentials_Include,
        .event = documentLoadEvent,
        .context = load,
    };
    if (prFetchCreate(load->fetcher, &desc, &load->fetch) != PrStatus_Ok) {
        postPiece(load, newPiece(LoadPiece_Fail));
        endLoadOnLoop(load);
    }
}

// Cancels a dropped load on the loop if its fetch still runs, and releases the reference that the cancel holds.
void cancelLoadOnLoop(void* PR_NULLABLE data) {
    HtmlDocumentLoad* load = static_cast<HtmlDocumentLoad*>(data);
    if (!load->loopDone)
        endLoadOnLoop(load);
    releaseDocumentLoad(load);
}

void dropDocumentLoad(PrHtmlPage* self) {
    HtmlDocumentLoad* load = self->documentLoad;
    if (load == nullptr)
        return;
    self->documentLoad = nullptr;
    load->page = nullptr;
    prMutexLock(&load->lock);
    load->dropped = true;
    prMutexUnlock(&load->lock);
    // The cancel takes the page's reference.
    prNetPost(load->fetcher->net, cancelLoadOnLoop, load);
}

// html/#nav-stop: the navigable's ongoing navigation is cancelled, and its document stops loading what it waits for
// (html/#abort-a-document). An iframe's source that its container's document fetches no longer replaces it.
void stopLoading(PrHtmlPage* self) {
    self->pendingNavigation = 0;
    if (self->awaitingSource) {
        self->awaitingSource = false;
        PrHtmlWindow* parent = self->window->parent;
        for (uint32_t i = 0; parent != nullptr && i < parent->frames.count; ++i) {
            if (parent->frames.data[i].window == self->window)
                ++domElementState(parent->frames.data[i].container)->generation;
        }
    }
    dropDocumentLoad(self);
    dropFetches(self);
}

// html/#abort-a-document: a load whose input is still arriving, or whose parse waits for a script, stops.
void abortDocument(PrHtmlPage* self) {
    if (!self->window->streaming)
        return;
    htmlParserReset(&self->parser);
    self->window->streaming = false;
    self->streamInput.text.reset();
    self->streamRead = 0;
    releaseWaiting(self);
}

// Fires visibilitychange at the window's document in a task of the window (html/#update-the-visibility-state). The
// event bubbles, and the task holds the document.
void queueVisibilityChange(PrHtmlWindow* window) {
    PrDomDocument* document = window->document;
    if (document == nullptr)
        return;
    auto fire = [](void* data) {
        PrDomDocument* held = static_cast<PrDomDocument*>(data);
        PrDomEventInit init = prDomEventInitDefault();
        init.bubbles = true;
        PrDomEvent* event = prDomEventCreate("visibilitychange", &init);
        event->trusted = true;
        domEventDispatch(held, event);
        domEventRelease(event);
        domNodeUnhold(held);
    };
    domNodeHold(document);
    htmlWindowQueueTask(window, fire, document, [](void* data) { domNodeUnhold(static_cast<PrDomDocument*>(data)); });
}

} // namespace

PrStatus htmlDocumentOpen(PrDomDocument* document) {
    if (!takesDynamicMarkup(document))
        return PrStatus_InvalidState;
    PrHtmlPage* self = htmlDocumentPage(document);
    // html/#document-open-steps: does nothing while the parser runs a script or parses a load.
    if (self == nullptr || self->runningScript || (self->parsing && !self->scriptCreated))
        return PrStatus_Ok;
    if (self->scriptCreated)
        htmlParserReset(&self->parser);
    // A page that is navigating stops loading.
    if (self->awaitingSource || self->pendingNavigation != 0)
        stopLoading(self);
    abortDocument(self);
    // The listeners of the document and the window are erased, and the document's children are removed. A
    // script-created parser takes over the document, with its insertion point at the end of an empty input.
    eraseListeners(document);
    domListenersErase(self->window);
    domReplaceAll(document, nullptr);
    startParser(self, document);
    htmlParserOpen(&self->parser);
    self->scriptCreated = true;
    // A script-created parser reads text, which no charset decodes.
    self->confidence = HtmlConfidence_Irrelevant;
    return PrStatus_Ok;
}

PrStatus htmlDocumentWrite(PrDomDocument* document, PrStringView text) {
    if (!takesDynamicMarkup(document))
        return PrStatus_InvalidState;
    PrHtmlPage* self = htmlDocumentPage(document);
    if (self == nullptr)
        return PrStatus_Ok;
    // html/#document-write-steps: a script that the parser runs writes at the parser's read position.
    if (self->runningScript) {
        htmlParserWrite(&self->parser, text);
        return PrStatus_Ok;
    }
    // If the insertion point is undefined, the document is opened anew, unless an external or module script is writing.
    if (!self->scriptCreated) {
        if (self->ignoreDestructiveWrites != 0)
            return PrStatus_Ok;
        htmlDocumentOpen(document);
        if (!self->scriptCreated)
            return PrStatus_Ok;
    }
    return runScriptCreated(self, htmlParserAppend, text);
}

PrStatus htmlDocumentClose(PrDomDocument* document) {
    if (!takesDynamicMarkup(document))
        return PrStatus_InvalidState;
    PrHtmlPage* self = htmlDocumentPage(document);
    if (self == nullptr || !self->scriptCreated)
        return PrStatus_Ok;
    // A script that the parser runs closes the input, and the parse ends when control returns to the parser.
    if (self->runningScript) {
        self->parser.tokenizer.open = false;
        return PrStatus_Ok;
    }
    return runScriptCreated(self, closeStep, PrStringView{});
}

void htmlPageFrames(PrHtmlPage* page, Array<PrHtmlPage*>* out) {
    PrHtmlPage* self = page;
    for (uint32_t i = 0; i < self->window->frames.count; ++i) {
        for (uint32_t k = 0; k < self->children.count; ++k) {
            if (self->children.data[k]->window == self->window->frames.data[i].window)
                out->push(self->children.data[k]);
        }
    }
}

PrHtmlPage* PR_NULLABLE htmlDocumentPage(const PrDomDocument* document) {
    const DomDocumentHost* host = document->host;
    if (host == nullptr || host->fetch != fetchHost)
        return nullptr;
    PrHtmlPage* self = static_cast<PrHtmlPage*>(host->context);
    return self->window->document == document ? self : nullptr;
}

uint64_t htmlPageEmbed(const PrHtmlPage* page) {
    return uint64_t(uintptr_t(page));
}

bool htmlPageNavigate(PrHtmlWindow* window, PrStringView url, HtmlHistoryCommit commit, uint32_t target) {
    PrHtmlPage* self = htmlDocumentPage(window->document);
    if (self == nullptr || self->window != window || self->host.fetcher == nullptr)
        return false;
    Navigation* navigation = allocZeroed<Navigation>();
    navigation->page = self;
    navigation->id = self->pendingNavigation = ++self->navigations;
    navigation->url = prStringCopy(url);
    navigation->commit = commit;
    navigation->target = target;
    htmlWindowQueueTask(window, runNavigation, navigation, dropNavigation);
    return true;
}

void htmlPageFollowFrameScheme(PrHtmlPage* parent, PrHtmlPage* page) {
    if (parent->presentation == nullptr)
        return;
    for (uint32_t i = 0; i < parent->window->frames.count; ++i) {
        const HtmlFrame& frame = parent->window->frames.data[i];
        if (frame.window != page->window)
            continue;
        PrAppUserPreferences preferences = page->preferences;
        bool dark = cssPresentationUsesDarkScheme(parent->presentation, frame.container);
        preferences.colorScheme = dark ? PrAppColorScheme_Dark : PrAppColorScheme_Light;
        if (preferences.colorScheme != page->preferences.colorScheme)
            prHtmlPageSetUserPreferences(page, &preferences);
        return;
    }
}

void htmlPresentationPrepareJob(PrCssPresentation* presentation, PrLayoutVec extent) {
    cssPresentationPrepareJob(presentation, extent);
    if (!cssPresentationSvgIdle(presentation) && svgPresentationUpdate(presentation)) {
        cssPresentationPrepareJob(presentation, extent);
        svgPresentationUpdate(presentation);
    }
}

PrStatus htmlPageBeginStreamedLoad(PrHtmlPage* self, PrStringView url, const PrHtmlResponseHeaders* PR_NULLABLE headers) {
    PrHtmlPage* page = self;
    if (page->loading || page->runningScript || page->window->streaming)
        return PrStatus_InvalidState;
    PrStatus status = beginDocument(page, url, headers);
    if (status != PrStatus_Ok)
        return status;
    PrDomTag media = mediaHostOf(prStringView(&page->window->document->contentType));
    if (media != PrDomTag_Unknown)
        return loadMediaDocument(page, media);
    page->window->streaming = true;
    page->streamInput.text.clear();
    page->streamRead = 0;
    page->streamEnded = false;
    page->streamQueued = false;
    page->streamSniffed = false;
    page->streamFlushed = false;
    page->transportCharset = transportCharsetOf(headers);
    page->reparse = PrCharset_None;
    if (isMarkdownDocument(page->window->document)) {
        page->markdown = allocZeroed<HtmlMarkdownBuilder>();
        htmlMarkdownBuilderInitDocument(page->markdown, page->window->document,
            HtmlParseFlag_DeclarativeShadowRoots, &page->markdownOptions);
    } else if (page->window->document->kind == DomDocumentKind_Html) {
        startParser(page, page->window->document);
        htmlParserOpen(&page->parser);
    } else {
        startXmlParser(page);
    }
    return PrStatus_Ok;
}

} // namespace pr_impl

using namespace pr_impl;

PrHtmlPage* prHtmlPageCreate(const PrHtmlHost* PR_NULLABLE host) {
    PrHtmlPage* self = allocZeroed<PrHtmlPage>();
    self->window = htmlWindowCreate();
    self->host = host != nullptr ? *host : prHtmlHostDefault();
    shareWithWindow(self, self->window);
    // A browsing context with no history of its host's keeps its own, whose first entry is its initial about:blank's.
    self->ownsHistory = self->host.sessionHistory == nullptr;
    self->window->history = self->ownsHistory ? prHtmlSessionHistoryCreate() : self->host.sessionHistory;
    if (self->ownsHistory)
        self->window->historyDocument = htmlHistoryCommit(self->window->history, "about:blank", true);
    self->ownsAgent = self->host.agent == nullptr;
    self->window->agent = self->ownsAgent ? prDomAgentCreate(nullptr) : self->host.agent;
    domAgentAddGlobal(self->window->agent, self, &kWindowTasks);
    cssMediaEnvironmentInit(&self->window->media, self->host.execute != nullptr);
    self->defaultCharset = PrCharset_Windows1252;
    self->display = prAppDisplayDefault();
    self->visibility = PrHtmlVisibility_Visible;
    cssMediaEnvironmentSetDisplay(&self->window->media, self->display);
    prHtmlPageSetViewport(self, PrVec2{800, 600});
    self->documentHost = DomDocumentHost{
        .context = self,
        .fetch = fetchHost,
        .compress = self->host.compress,
        .decodeImage = decodeImage,
        .updatePresentation = updatePresentationNow,
        .viewport = viewportNow,
        .sheetsChanged = sheetsChanged,
        .queueTask = queueTask,
        .queueDelayingTask = queueDelayingTask,
        .queueMicrotask = queueMicrotask,
        .compile = compileHandler,
        .embedChanged = embedChanged,
        .embedRemoved = embedRemoved,
        .prepareScript = prepareConnectedScript,
        .elementState = elementState,
        .activates = activates,
        .activate = activate,
    };
    newDocument(self);
    if (self->host.windowOpened != nullptr)
        self->host.windowOpened(self->host.context, self);
    return self;
}

void prHtmlPageDestroy(PrHtmlPage* PR_NULLABLE self) {
    if (self == nullptr)
        return;
    PrHtmlPage* page = self;
    domAgentRemoveGlobal(page->window->agent, page);
    if (page->scriptCreated)
        htmlParserReset(&page->parser);
    for (uint32_t i = 0; i < page->children.count; ++i)
        prHtmlPageDestroy(page->children.data[i]);
    page->children.reset();
    page->window->frames.reset();
    page->streamInput.text.reset();
    dropDocumentLoad(page);
    dropFetches(page);
    domXmlBuilderReset(&page->xml);
    dropAvailableImages(page);
    releaseWaiting(page);
    dropModules(page);
    page->deferred.reset();
    page->soon.reset();
    page->inOrder.reset();
    dropElementEvents(page);
    page->elementEvents.reset();
    closeWindow(page);
    if (page->host.closed != nullptr)
        page->host.closed(page->host.context, self);
    dropMarkdown(page);
    if (page->ownsHistory)
        prHtmlSessionHistoryDestroy(static_cast<PrHtmlSessionHistory*>(page->window->history));
    discardDocument(page);
    if (page->ownsAgent)
        domAgentDestroy(page->window->agent);
    domEventTargetRelease(page->window);
    prMemoryFree(page);
}

void prHtmlPageTraverse(PrHtmlPage* self, int32_t delta) {
    htmlHistoryTraverse(self->window, delta);
}

PrHtmlWindow* prHtmlPageWindow(PrHtmlPage* self) {
    return self->window;
}

PrHtmlPage* PR_NULLABLE prHtmlWindowPage(PrHtmlWindow* window) {
    PrHtmlPage* page = htmlDocumentPage(window->document);
    return page != nullptr && page->window == window ? page : nullptr;
}

PrDomAgent* prHtmlPageAgent(PrHtmlPage* self) {
    return self->window->agent;
}

PrStatus prHtmlPageLoad(PrHtmlPage* self, PrStringView source, PrStringView url,
        const PrHtmlResponseHeaders* PR_NULLABLE headers) {
    PrHtmlPage* page = self;
    if (page->loading || page->runningScript || page->window->streaming)
        return PrStatus_InvalidState;
    dropDocumentLoad(page);
    PrStatus status = beginDocument(page, url, headers);
    if (status != PrStatus_Ok)
        return status;
    PrDomDocument* document = page->window->document;
    PrDomTag media = mediaHostOf(prStringView(&document->contentType));
    if (media != PrDomTag_Unknown)
        return loadMediaDocument(page, media);
    page->loading = true;
    page->transportCharset = transportCharsetOf(headers);
    page->reparse = PrCharset_None;
    if (document->kind == DomDocumentKind_Xml) {
        startXmlParser(page);
        PrString decoded = {};
        prXmlParserAppend(page->xml.parser, domXmlDecode(source, page->transportCharset, &decoded, &document->charset));
        prXmlParserEnd(page->xml.parser);
        prStringReset(&decoded);
        status = runXmlParser(page);
        if (!prXmlParserIsEnded(page->xml.parser)) {
            // The parse waits for a script, and continues in tasks once the script runs.
            continueInTasks(page, source);
            return PrStatus_Ok;
        }
        domXmlBuilderReset(&page->xml);
    } else if (isMarkdownDocument(document)) {
        startDecoder(page, markdownCharsetOf(page, source), HtmlConfidence_Certain);
        PrString decoded = {};
        HtmlMarkdownBuilder builder;
        htmlMarkdownBuilderInitDocument(&builder, document, HtmlParseFlag_DeclarativeShadowRoots,
            &page->markdownOptions);
        page->parsing = true;
        htmlMarkdownBuilderAppend(&builder,
            prCharsetDecodeView(&page->decoder, source, PrCharsetInput_Final, &decoded));
        htmlMarkdownBuilderEnd(&builder);
        page->parsing = false;
        htmlMarkdownBuilderReset(&builder);
        prStringReset(&decoded);
    } else {
        sniffCharset(page, source, true);
        parseHtmlSource(page, source);
        while (page->reparse != PrCharset_None) {
            status = restartDocument(page);
            if (status != PrStatus_Ok) {
                page->loading = false;
                return status;
            }
            parseHtmlSource(page, source);
        }
        status = page->parser.status;
        if (status == PrStatus_Ok && page->parser.paused) {
            // The parse waits for a script, and continues in tasks once the script runs. The load keeps its bytes,
            // which a change of encoding parses again.
            continueInTasks(page, source);
            return PrStatus_Ok;
        }
        htmlParserReset(&page->parser);
    }
    status = finishParse(page, status, true);
    page->loading = false;
    return status;
}

PrStatus prHtmlPageBeginLoad(PrHtmlPage* self, PrStringView url, const PrHtmlResponseHeaders* PR_NULLABLE headers) {
    if (self->loading || self->runningScript || self->window->streaming)
        return PrStatus_InvalidState;
    dropDocumentLoad(self);
    return htmlPageBeginStreamedLoad(self, url, headers);
}

void prHtmlPageAppendLoad(PrHtmlPage* self, PrStringView bytes) {
    PrHtmlPage* page = self;
    if (!page->window->streaming || page->streamEnded)
        return;
    textAppend(&page->streamInput, bytes);
    queueStream(page);
}

void prHtmlPageEndLoad(PrHtmlPage* self) {
    if (!self->window->streaming || self->streamEnded)
        return;
    self->streamEnded = true;
    queueStream(self);
}

PrStatus prHtmlPageNavigate(PrHtmlPage* self, PrStringView url) {
    if (self->host.fetcher == nullptr || self->loading || self->runningScript || self->window->streaming)
        return PrStatus_InvalidState;
    PrUrl* parsed = nullptr;
    PrStatus status = prUrlCreate(url, nullptr, PrCharset_Utf8, &parsed);
    prUrlDestroy(parsed);
    if (status != PrStatus_Ok)
        return status;
    dropDocumentLoad(self);
    HtmlDocumentLoad* load = allocZeroed<HtmlDocumentLoad>();
    load->page = self;
    load->agent = self->window->agent;
    load->fetcher = self->host.fetcher;
    load->url = prStringCopy(url);
    prMutexInit(&load->lock);
    load->references.store(2);
    self->documentLoad = load;
    prNetPost(load->fetcher->net, beginLoadOnLoop, load);
    return PrStatus_Ok;
}

void prHtmlPageImportModule(PrHtmlPage* self, PrStringView url, PrHtmlImportFn done, void* PR_NULLABLE context) {
    PR_ASSERT(self->host.parseModule != nullptr);
    // An import() fetches with the credentials of the default script fetch options
    // (html/#default-script-fetch-options).
    Array<HtmlModuleIndex> added;
    HtmlModuleIndex root = addModule(self, url, PrFetchCredentials_SameOrigin, &added);
    if (added.count != 0)
        fetchModule(self, root);
    added.reset();
    self->moduleWaits.push(HtmlModuleWait{.root = root, .done = done, .context = context});
    queueModules(self);
}

PrStatus prHtmlPageFetch(PrHtmlPage* self, const PrFetchClientRequest* request, PrFetchClientFn done, void* PR_NULLABLE context) {
    PR_ASSERT(request->mode != PrFetchMode_Navigate);
    PR_ASSERT(request->headers != nullptr || request->headerCount == 0);
    PrUrl* base = domDocumentBaseUrl(self->window->document);
    PrUrl* parsed = nullptr;
    PrStatus status = prUrlCreate(request->url, base, PrCharset_Utf8, &parsed);
    prUrlDestroy(base);
    if (status != PrStatus_Ok)
        return PrStatus_InvalidData;
    if (prUrlUsername(parsed).size != 0 || prUrlPassword(parsed).size != 0) {
        prUrlDestroy(parsed);
        return PrStatus_InvalidData;
    }
    PrString url = prUrlSerialize(parsed, PrUrlSerialization_ExcludeFragment);
    prUrlDestroy(parsed);
    FetchSettings settings{request->mode, request->credentials, false};
    HtmlFetch* fetch = createFetch(self, prStringView(&url), DomFetchDestination_Empty, settings);
    prStringReset(&url);
    fetch->scriptDone = done;
    fetch->data = context;
    fetch->redirect = request->redirect;
    if (request->method.data != nullptr)
        fetch->method = prStringCopy(request->method);
    for (uint32_t i = 0; i < request->headerCount; ++i) {
        fetch->requestHeaders.push(HtmlFetchHeader{
            prStringCopy(request->headers[i].name),
            prStringCopy(request->headers[i].value),
        });
    }
    if (request->body.size != 0)
        fetch->requestBody.append(request->body.data, uint32_t(request->body.size));
    // A data: URL is answered whatever the method (fetch/#scheme-fetch), as a document's is, but in a task.
    if (asciiStartsWithFold(prStringView(&fetch->url), "data:")) {
        PrString body = {};
        status = prUrlProcessData(prStringView(&fetch->url), &body, &fetch->contentType);
        if (status == PrStatus_Ok) {
            fetch->responseStatus = 200;
            fetch->responseUrl = prStringCopy(prStringView(&fetch->url));
            fetch->body.append(reinterpret_cast<const uint8_t*>(prStringView(&body).data),
                uint32_t(prStringView(&body).size));
            fetch->responseHeaders.push(HtmlFetchHeader{
                prStringCopy("Content-Type"),
                prStringCopy(prStringView(&fetch->contentType)),
            });
        }
        prStringReset(&body);
        answerFetch(fetch, status == PrStatus_Ok ? PrStatus_Ok : PrStatus_InvalidData);
    } else if (fetch->fetcher == nullptr) {
        answerFetch(fetch, PrStatus_NotSupported);
    } else {
        postFetch(fetch);
    }
    return PrStatus_Ok;
}

void prHtmlPageCancelFetch(PrHtmlPage* self, void* PR_NULLABLE context) {
    for (HtmlFetch* fetch = self->fetches; fetch != nullptr; fetch = fetch->next) {
        if (fetch->scriptDone == nullptr || fetch->data != context || fetch->cancelled)
            continue;
        fetch->cancelled = true;
        // A fetch whose response arrived waits for its task, which then passes nothing on. Any other stops.
        if (!fetch->answered)
            dropFetch(fetch);
        return;
    }
}

bool prHtmlPageIsNavigating(const PrHtmlPage* self) {
    return self->documentLoad != nullptr && !self->documentLoad->responded;
}

bool prHtmlPageIsFetching(const PrHtmlPage* self) {
    if (self->documentLoad != nullptr)
        return true;
    for (const HtmlFetch* fetch = self->fetches; fetch != nullptr; fetch = fetch->next) {
        if (!fetch->answered)
            return true;
    }
    return false;
}

bool prHtmlPageIsRenderBlocked(PrHtmlPage* self) {
    return htmlWindowRenderBlocked(self->window);
}

bool prHtmlPageSetMediaFeature(PrHtmlPage* self, PrCssDescriptor feature, PrStringView value) {
    PrHtmlPage* page = self;
    cssPresentationDocumentMediaChanged(page->window->document);
    return cssMediaEnvironmentSet(&page->window->media, feature, value);
}

void prHtmlPageSetMediaType(PrHtmlPage* self, PrCssMediaType type) {
    PrHtmlPage* page = self;
    cssPresentationDocumentMediaChanged(page->window->document);
    cssMediaEnvironmentSetType(&page->window->media, type);
}

void prHtmlPageSetDefaultCharset(PrHtmlPage* self, PrCharset charset) {
    PR_ASSERT(charset != PrCharset_None && charset < PrCharset_Count);
    self->defaultCharset = charset;
}

void prHtmlPageSetViewport(PrHtmlPage* self, PrVec2 extent) {
    if (self->viewport.x == extent.x && self->viewport.y == extent.y)
        return;
    self->viewport = extent;
    const PrCssDescriptor features[] = {PrCssDescriptor_MediaWidth, PrCssDescriptor_MediaHeight};
    const float sizes[] = {extent.x, extent.y};
    for (uint32_t i = 0; i < 2; ++i) {
        TextBuffer length;
        cssSerializeNumber(&length, double(sizes[i]));
        textAppend(&length, "px");
        cssMediaEnvironmentSet(&self->window->media, features[i], textView(&length));
        textReset(&length);
    }
    cssPresentationDocumentMediaChanged(self->window->document);
}

void prHtmlPageSetDisplay(PrHtmlPage* self, const PrAppDisplay* display) {
    self->display = *display;
    if (cssMediaEnvironmentSetDisplay(&self->window->media, *display))
        cssPresentationDocumentMediaChanged(self->window->document);
    for (uint32_t i = 0; i < self->children.count; ++i)
        prHtmlPageSetDisplay(self->children.data[i], display);
}

void prHtmlPageSetUserPreferences(PrHtmlPage* self, const PrAppUserPreferences* preferences) {
    self->preferences = *preferences;
    if (cssMediaEnvironmentSetUserPreferences(&self->window->media, *preferences))
        cssPresentationDocumentMediaChanged(self->window->document);
    // Each frame keeps the color scheme that its iframe element gives it.
    for (uint32_t i = 0; i < self->children.count; ++i) {
        PrHtmlPage* child = self->children.data[i];
        PrAppUserPreferences taken = *preferences;
        taken.colorScheme = child->preferences.colorScheme;
        prHtmlPageSetUserPreferences(child, &taken);
    }
}

void prHtmlPageSetVisibility(PrHtmlPage* self, PrHtmlVisibility visibility) {
    if (self->visibility == visibility)
        return;
    self->visibility = visibility;
    queueVisibilityChange(self->window);
    for (uint32_t i = 0; i < self->children.count; ++i)
        prHtmlPageSetVisibility(self->children.data[i], visibility);
}

PrCssPresentation* PR_NULLABLE prHtmlPagePresentation(PrHtmlPage* self) {
    return self->presentation;
}

bool prHtmlPresentationUpdate(PrCssPresentation* presentation, PrLayoutVec extent) {
    cssPresentationUpdateBoxes(presentation, extent);
    // The boxes that name a reference whose source changed take it again before the layout.
    if (!cssPresentationSvgIdle(presentation) && svgPresentationUpdate(presentation)) {
        cssPresentationUpdateBoxes(presentation, extent);
        svgPresentationUpdate(presentation);
    }
    return cssPresentationUpdateLayout(presentation, extent);
}

void prHtmlPresentationStart(PrCssPresentation* presentation, PrLayoutVec extent) {
    htmlPresentationPrepareJob(presentation, extent);
    cssPresentationPostJob(presentation, extent);
}

double prHtmlPageTimeOrigin(PrHtmlPage* self) {
    return self->window->timeOrigin;
}

