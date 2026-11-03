#include "css/src/css_owner.h"

#include "pr/ascii.h"
#include "pr/charset.h"
#include "pr/memory.h"
#include "pr/name_table.h"
#include "pr/string_ops.h"
#include "pr/url.h"

#include "css/src/css_parser.h"
#include "css/src/css_root.h"
#include "dom/src/dom_node.h"
#include "dom/src/dom_use.h"

namespace pr_impl {
namespace {

// Returns the style of the tree that `element` is in, which belongs to its shadow root or to its document.
CssRoot* scopeOf(PrDomElement* element) {
    PrDomShadowRoot* shadow = domNodeShadowRoot(domNodeRoot(element));
    if (shadow == nullptr)
        return cssDocumentRoot(element->document);
    if (shadow->style == nullptr)
        shadow->style = allocZeroed<CssRoot>();
    return shadow->style;
}

// Inserts the element's new sheet into the list of its tree, in tree order of the owner nodes.
void insertStyleSheet(PrDomElement* element, PrCssStyleSheet* sheet) {
    CssRoot* scope = scopeOf(element);
    RawArray<PrCssStyleSheet* PR_NONNULL>& sheets = scope->sheets.sheets;
    uint32_t index = sheets.count;
    // Parsed sheets usually append, so earlier owners are compared only when needed.
    while (index != 0 && domNodePrecedes(sheet->ownerNode, sheets.data[index - 1]->ownerNode))
        --index;
    sheets.insert(index, sheet);
    cssDocumentStyleChanged(element->document);
    CssElementStyle* style = cssElementStyle(element);
    style->sheet = sheet;
    style->scope = scope;
}

// cssom-1/#remove-a-css-style-sheet, for the element's sheet, from the list that it is in.
void removeStyleSheet(PrDomElement* element) {
    CssElementStyle* style = element->style;
    if (style == nullptr || style->sheet == nullptr)
        return;
    PrCssStyleSheet* sheet = style->sheet;
    RawArray<PrCssStyleSheet* PR_NONNULL>& sheets = style->scope->sheets.sheets;
    for (uint32_t i = 0; i < sheets.count; ++i) {
        if (sheets.data[i] == sheet) {
            sheets.erase(i);
            break;
        }
    }
    style->sheet = nullptr;
    style->scope = nullptr;
    cssDocumentStyleChanged(element->document);
    cssStyleSheetLeaveOwner(sheet);
}

// Gives the sheet the element as its owner node, and its title and media from the element's attributes
// (cssom-1/#create-a-css-style-sheet).
void takeOwnerAttributes(PrCssStyleSheet* sheet, PrDomElement* element) {
    sheet->ownerNode = element;
    const PrDomAttr* title = domAttribute(element, PrDomNamespace_None, "title");
    sheet->title = title != nullptr ? prRawStringClone(&title->value) : prRawStringEmpty();
    const PrDomAttr* media = domAttribute(element, PrDomNamespace_None, "media");
    if (media != nullptr)
        cssMediaListParse(&sheet->media, prRawStringView(&media->value));
}

// Fires load or error at a style element for one update of its block. Releases the element that the task held.
void fireStyleLoad(void* data) {
    PrDomElement* element = static_cast<PrDomElement*>(data);
    domEventFire(element, "load");
    domNodeUnhold(element);
}

void fireStyleError(void* data) {
    PrDomElement* element = static_cast<PrDomElement*>(data);
    domEventFire(element, "error");
    domNodeUnhold(element);
}

// Queues the event that an update of a style element's block fires once the imports of its sheet are obtained
// (html/#update-a-style-block). The task holds the element.
void queueStyleEvent(const DomDocumentHost* host, PrDomElement* element, bool loaded) {
    domNodeHold(element);
    host->queueDelayingTask(host->context, loaded ? fireStyleLoad : fireStyleError, element,
        [](void* data) { domNodeUnhold(static_cast<PrDomElement*>(data)); });
}

void setBlocking(PrDomElement* element, bool blocking) {
    CssElementStyle* style = cssElementStyle(element);
    if (style->blocking == blocking)
        return;
    style->blocking = blocking;
    PrDomDocument* document = element->document;
    style->blocksRendering = blocking && document->kind == DomDocumentKind_Html && domDocumentBody(document) == nullptr;
    RawArray<PrDomElement* PR_NONNULL>& set = cssDocumentRoot(document)->blocking;
    if (blocking)
        set.push(element);
    else
        set.removeFirst(element);
    if (!blocking && document->host != nullptr)
        document->host->sheetsChanged(document->host->context);
}

// A fetch for the sheet of a style or link element, or for a sheet that one of its sheets imports. It holds the element
// (domNodeHold), and the import rule that it fills.
struct SheetFetch {
    PrDomElement* element;
    uint32_t generation;
    CssRuleData* PR_NULLABLE rule;   // import rule, which the fetch holds, or null for the owner's own sheet
    PrRawString href;                // URL fetched, which the sheet's imports resolve against
    RawArray<PrRawString> importing; // URLs of the sheets that import this one, outermost first
    PrCharset environment;           // the charset that the sheet decodes in unless its bytes or response name another
    bool alternate;                  // the owner's own sheet is an alternate one (cssom-1/#css-style-sheet-set)
};

void freeSheetFetch(SheetFetch* fetch) {
    domNodeUnhold(fetch->element);
    cssRuleDataRelease(fetch->rule);
    prRawStringReset(&fetch->href);
    for (uint32_t i = 0; i < fetch->importing.count; ++i)
        prRawStringReset(&fetch->importing.data[i]);
    fetch->importing.reset();
    prMemoryFree(fetch);
}

void dropSheetFetch(void* PR_NULLABLE data) {
    freeSheetFetch(static_cast<SheetFetch*>(data));
}

void sheetFetched(void* PR_NULLABLE data, const DomFetchResult* result);

// Called when the element's sheets have all arrived or failed. Removes the element from the blocking sheets, and runs
// its owner's steps.
void settleSheets(PrDomElement* element, bool loaded) {
    CssElementStyle* style = cssElementStyle(element);
    loaded = loaded && !style->importFailed;
    setBlocking(element, false);
    if (element->document->host != nullptr && style->settled != nullptr)
        style->settled(element, loaded);
}

// The steps of a style element whose sheets have settled (html/#update-a-style-block).
void styleSettled(PrDomElement* element, bool loaded) {
    queueStyleEvent(element->document->host, element, loaded);
}

// Marks one of the fetches that the element's sheets wait for as done, and settles the sheets if it was the last.
void fetchDone(PrDomElement* element, bool loaded) {
    CssElementStyle* style = cssElementStyle(element);
    PR_ASSERT(style->fetching != 0);
    if (--style->fetching == 0)
        settleSheets(element, loaded);
}

// Fetches the sheet that each @import rule of `sheet` names (css-cascade-5/#at-import), for the element's current
// sheet, resolving against `base`. A rule holds no sheet if its URL does not parse, or if it would import a sheet into
// itself or into a sheet that imports it. `importing` holds the URLs of those importing sheets. A URL that does not
// parse or load makes the element's event an error.
void fetchImports(PrDomElement* element, CssSheetData* sheet, const PrUrl* base, const RawArray<PrRawString>& importing,
        PrCharset environment) {
    const DomDocumentHost* host = element->document->host;
    CssElementStyle* style = cssElementStyle(element);
    // Each fetch below may arrive immediately. This count holds the element's sheets open until all have started.
    ++style->fetching;
    for (uint32_t i = 0; host != nullptr && i < sheet->rules.count; ++i) {
        CssRuleData* rule = sheet->rules.data[i];
        if (rule->kind != CssRuleDataKind_At || rule->atRule != CssAtRule_Import || rule->imported != nullptr)
            continue;
        CssValueRun prelude = cssValueListRun(&rule->prelude);
        const CssValueNode* value = prelude.values;
        const CssValueNode* end = prelude.values + prelude.count;
        while (value < end && !cssValueHasText(value->kind))
            ++value;
        PrUrl* url = nullptr;
        if (value == end || prUrlCreate(cssValueText(prelude, value), base, PrCharset_Utf8, &url) != PrStatus_Ok) {
            prUrlDestroy(url);
            style->importFailed = true;
            continue;
        }
        PrRawString href = prUrlSerialize(url, PrUrlSerialization_Whole);
        bool cycle = false;
        for (uint32_t k = 0; k < importing.count && !cycle; ++k)
            cycle = prRawStringView(&importing.data[k]) == prRawStringView(&href);
        if (cycle) {
            prRawStringReset(&href);
            prUrlDestroy(url);
            continue;
        }
        SheetFetch* fetch = allocZeroed<SheetFetch>();
        domNodeHold(element);
        fetch->element = element;
        fetch->generation = style->generation;
        cssRuleDataRetain(rule);
        fetch->rule = rule;
        fetch->href = href;
        for (uint32_t k = 0; k < importing.count; ++k)
            fetch->importing.push(prRawStringClone(&importing.data[k]));
        fetch->environment = environment;
        PrRawString request = prUrlSerialize(url, PrUrlSerialization_ExcludeFragment);
        prUrlDestroy(url);
        ++style->fetching;
        DomFetchRequest asked{
            prRawStringView(&request), DomFetchDestination_Style, DomCorsSettings_NoCors, DomFetchInitiator_Css,
        };
        host->fetch(host->context, &asked, sheetFetched, fetch, dropSheetFetch);
        prRawStringReset(&request);
    }
    fetchDone(element, true);
}

// Receives the result of a fetch for an element's sheet, or for a sheet that it imports. The result of a fetch for an
// earlier sheet of the element is used only as the sheet that an import rule holds.
void sheetFetched(void* PR_NULLABLE data, const DomFetchResult* result) {
    SheetFetch* fetch = static_cast<SheetFetch*>(data);
    PrDomElement* element = fetch->element;
    CssElementStyle* style = element->style;
    bool current = style != nullptr && style->generation == fetch->generation;
    bool loaded = result->status == PrStatus_Ok;
    // The sheet decodes in its fetch's charset unless its bytes or response name another
    // (css-syntax-3/#determine-the-fallback-encoding). An imported sheet decodes in that charset too, in place of its
    // parent sheet's.
    PrRawString decoded = {};
    PrStringView text;
    if (loaded)
        text = cssDecodeBytes(result->body, prCharsetFromMimeType(result->contentType), fetch->environment, &decoded);
    else
        text = PrStringView{};
    CssSheetData* sheet = nullptr;
    if (loaded && fetch->rule != nullptr && fetch->rule->imported == nullptr) {
        sheet = cssSheetDataCreate();
        sheet->href = prRawStringClone(&fetch->href);
        cssSheetDataParse(sheet, text, nullptr, CssOrigin_Author);
        fetch->rule->imported = sheet;
        cssDocumentStyleChanged(element->document);
    } else if (loaded && fetch->rule == nullptr && current) {
        // The owner's own sheet. An alternate sheet with a title starts disabled (cssom-1/#css-style-sheet-set).
        PrCssStyleSheet* own = cssStyleSheetCreate(cssSheetDataCreate());
        own->href = prRawStringClone(&fetch->href);
        own->data->href = prRawStringClone(&fetch->href);
        takeOwnerAttributes(own, element);
        own->disabled = fetch->alternate && !prRawStringIsEmpty(&own->title);
        cssStyleSheetReplace(own, text);
        insertStyleSheet(element, own);
        sheet = own->data;
    }
    prRawStringReset(&decoded);
    if (current) {
        if (sheet != nullptr) {
            PrUrl* base = nullptr;
            fetch->importing.push(prRawStringClone(&fetch->href));
            if (prUrlCreate(prRawStringView(&fetch->href), nullptr, PrCharset_Utf8, &base) == PrStatus_Ok)
                fetchImports(element, sheet, base, fetch->importing, fetch->environment);
            prUrlDestroy(base);
        } else if (!loaded && fetch->rule != nullptr) {
            style->importFailed = true;
        }
        fetchDone(element, loaded || fetch->rule != nullptr);
    }
    freeSheetFetch(fetch);
}

// A fetch for the sheet of an xml-stylesheet processing instruction. It holds a reference to the instruction.
struct InstructionFetch {
    PrDomNode* instruction;
    PrRawString href;                  // the URL fetched
    PrRawString title;                 // the title pseudo-attribute, or the null string
    PrRawString media;                 // the media pseudo-attribute, or the null string
};

void dropInstructionFetch(void* PR_NULLABLE data) {
    InstructionFetch* fetch = static_cast<InstructionFetch*>(data);
    domNodeRelease(fetch->instruction);
    prRawStringReset(&fetch->href);
    prRawStringReset(&fetch->title);
    prRawStringReset(&fetch->media);
    prMemoryFree(fetch);
}

// Gives the document the sheet of an xml-stylesheet processing instruction once it arrives, if the instruction is still
// in the document.
void instructionSheetFetched(void* PR_NULLABLE data, const DomFetchResult* result) {
    InstructionFetch* fetch = static_cast<InstructionFetch*>(data);
    PrDomNode* instruction = fetch->instruction;
    PrDomDocument* document = instruction->document;
    if (result->status != PrStatus_Ok || instruction->parent == nullptr)
        return;
    PrRawString decoded = {};
    PrStringView text = cssDecodeBytes(result->body, prCharsetFromMimeType(result->contentType), document->charset, &decoded);
    PrCssStyleSheet* sheet = cssStyleSheetCreate(cssSheetDataCreate());
    sheet->href = prRawStringClone(&fetch->href);
    sheet->data->href = prRawStringClone(&fetch->href);
    sheet->ownerNode = instruction;
    sheet->title = prRawStringIsNull(&fetch->title) ? prRawStringEmpty() : prRawStringClone(&fetch->title);
    if (!prRawStringIsNull(&fetch->media))
        cssMediaListParse(&sheet->media, prRawStringView(&fetch->media));
    cssStyleSheetReplace(sheet, text);
    prRawStringReset(&decoded);
    CssRoot* scope = cssDocumentRoot(document);
    RawArray<PrCssStyleSheet* PR_NONNULL>& sheets = scope->sheets.sheets;
    uint32_t index = sheets.count;
    while (index != 0 && domNodePrecedes(instruction, sheets.data[index - 1]->ownerNode))
        --index;
    sheets.insert(index, sheet);
    scope->instructionSheets.push(sheet);
    cssDocumentStyleChanged(document);
}

void instructionSheetFetchedAndDropped(void* PR_NULLABLE data, const DomFetchResult* result) {
    instructionSheetFetched(data, result);
    dropInstructionFetch(data);
}

// Writes the value of the pseudo-attribute `name` of an xml-stylesheet processing instruction's data to `out`
// (cssom-1/#requirements-on-user-agents-implementing-the-xml-stylesheet-processing-instruction): a name, an equals sign
// and a quoted value, separated by white space. Returns false if the data does not have it, or does not parse as
// pseudo-attributes.
bool pseudoAttribute(PrStringView data, PrStringView name, PrStringView* out) {
    size_t at = 0;
    for (;;) {
        while (at < data.size && prAsciiIsSpace(data.data[at]))
            ++at;
        if (at == data.size)
            return false;
        size_t start = at;
        while (at < data.size && !prAsciiIsSpace(data.data[at]) && data.data[at] != '=')
            ++at;
        PrStringView found{data.data + start, at - start};
        while (at < data.size && prAsciiIsSpace(data.data[at]))
            ++at;
        if (at == data.size || data.data[at] != '=')
            return false;
        ++at;
        while (at < data.size && prAsciiIsSpace(data.data[at]))
            ++at;
        if (at == data.size || (data.data[at] != '"' && data.data[at] != '\''))
            return false;
        char quote = data.data[at++];
        size_t value = at;
        while (at < data.size && data.data[at] != quote)
            ++at;
        if (at == data.size)
            return false;
        if (found == name) {
            *out = PrStringView{data.data + value, at - value};
            return true;
        }
        ++at;
    }
}

// The seed for hashing sheet text in the cache.
constexpr uint64_t kSheetCacheSeed = 0x243f6a8885a308d3ull;

// Returns the data shared by style elements of the document with `text`, with a reference that the caller holds.
// Returns null if the document keeps no such data. The entry found becomes the most recently used.
CssSheetData* PR_NULLABLE cachedSheet(CssRoot* style, PrStringView text, uint64_t hash) {
    RawArray<CssSheetCacheEntry>& cache = style->cache;
    for (uint32_t i = cache.count; i-- != 0;) {
        CssSheetCacheEntry entry = cache.data[i];
        if (entry.hash != hash || prRawStringView(&entry.text) != text)
            continue;
        cache.erase(i);
        cache.push(entry);
        cssSheetDataRetain(entry.data);
        return entry.data;
    }
    return nullptr;
}

// Keeps `data` for another style element of the document with `text`. Evicts the least recently used entry if the cache
// is full.
void cacheSheet(CssRoot* style, PrStringView text, uint64_t hash, CssSheetData* data) {
    RawArray<CssSheetCacheEntry>& cache = style->cache;
    if (cache.count == kCssSheetCacheSize) {
        prRawStringReset(&cache.data[0].text);
        cssSheetDataRelease(cache.data[0].data);
        cache.erase(0);
    }
    cssSheetDataRetain(data);
    cache.push(CssSheetCacheEntry{hash, prRawStringCopy(text), data});
}

} // namespace

PrCssStyleSheet* PR_NULLABLE cssElementSheet(const PrDomElement* element) {
    return element->style != nullptr ? element->style->sheet : nullptr;
}

void cssStyleBlockUpdate(PrDomElement* element, DomInsertedBy by) {
    if ((element->space != PrDomNamespace_Html && element->space != PrDomNamespace_Svg) ||
            element->tag != PrDomTag_Style)
        return;
    PrDomDocument* document = element->document;
    cssOwnerReset(element);
    if (!domIsConnected(element))
        return;
    if (!cssOwnerTypeIsCss(element))
        return;
    // The sheets of the original of a use's instance are the document's, which the instances match already
    // (SVG2/struct.html#UseStyleInheritance).
    if (domInUseShadowTree(element))
        return;

    // A sheet with the same text as another style element of the document shares its data.
    CssRoot* cache = cssDocumentRoot(document);
    PrRawString text = domChildTextContent(element);
    PrStringView view = prRawStringView(&text);
    uint64_t hash = nameHash(view.data, view.size, kSheetCacheSeed);
    CssSheetData* data = cachedSheet(cache, view, hash);
    PrCssStyleSheet* sheet = cssStyleSheetCreate(data != nullptr ? data : cssSheetDataCreate());
    if (data == nullptr) {
        cssStyleSheetReplace(sheet, view);
        if (!cssSheetDataImports(sheet->data))
            cacheSheet(cache, view, hash, sheet->data);
    }
    prRawStringReset(&text);
    takeOwnerAttributes(sheet, element);
    insertStyleSheet(element, sheet);
    // Once the imports are obtained, the element fires load (html/#update-a-style-block). A sheet that the parser
    // inserted blocks scripts and rendering until then.
    if (document->host == nullptr)
        return;
    if (by == DomInsertedBy_Parser && cssSheetDataImports(sheet->data))
        setBlocking(element, true);
    cssElementStyle(element)->settled = styleSettled;
    PrUrl* base = domDocumentBaseUrl(document);
    RawArray<PrRawString> importing;
    fetchImports(element, sheet->data, base, importing, document->charset);
    importing.reset();
    prUrlDestroy(base);
}

void cssOwnerReset(PrDomElement* element) {
    removeStyleSheet(element);
    if (element->style != nullptr) {
        ++element->style->generation;
        element->style->fetching = 0;
        element->style->importFailed = false;
        element->style->settled = nullptr;
        setBlocking(element, false);
    }
}

void cssOwnerSetBlocking(PrDomElement* element, bool blocking) {
    setBlocking(element, blocking);
}

void cssOwnerFetch(PrDomElement* element, const PrUrl* url, DomCorsSettings cors, PrCharset environment,
        bool alternate, CssOwnerSettledFn* settled) {
    const DomDocumentHost* host = element->document->host;
    PR_ASSERT(host != nullptr);
    CssElementStyle* style = cssElementStyle(element);
    style->settled = settled;
    SheetFetch* fetch = allocZeroed<SheetFetch>();
    domNodeHold(element);
    fetch->element = element;
    fetch->generation = style->generation;
    fetch->href = prUrlSerialize(url, PrUrlSerialization_Whole);
    fetch->environment = environment;
    fetch->alternate = alternate;
    PrRawString request = prUrlSerialize(url, PrUrlSerialization_ExcludeFragment);
    style->fetching = 1;
    DomFetchRequest asked{
        prRawStringView(&request), DomFetchDestination_Style, cors, DomFetchInitiator_Link,
        domAttributeValue(element, "referrerpolicy"),
    };
    host->fetch(host->context, &asked, sheetFetched, fetch, dropSheetFetch);
    prRawStringReset(&request);
}

bool cssOwnerTypeIsCss(const PrDomElement* element) {
    const PrDomAttr* type = domAttribute(element, PrDomNamespace_None, "type");
    return type == nullptr || prRawStringIsEmpty(&type->value) || prAsciiEqualsFold(prRawStringView(&type->value), "text/css");
}

void cssOwnerMediaChanged(PrDomElement* element, PrStringView value) {
    // The sheet's media is the media attribute of its owner (cssom-1/#concept-css-style-sheet-media), and changes in
    // place.
    PrCssStyleSheet* sheet = cssElementSheet(element);
    if (sheet == nullptr)
        return;
    cssMediaListParse(&sheet->media, value.data != nullptr ? value : PrStringView{""});
    cssDocumentStyleChanged(element->document);
}

void cssXmlStylesheetInserted(PrDomNode* node) {
    if (node->type != PrDomNodeType_ProcessingInstruction || node->parent != node->document)
        return;
    const DomProcessingInstruction* instruction = static_cast<const DomProcessingInstruction*>(node);
    PrDomDocument* document = node->document;
    const DomDocumentHost* host = document->host;
    if (host == nullptr || prRawStringView(&instruction->target) != "xml-stylesheet")
        return;
    // Only an instruction in the prolog, before the document element, gives a sheet.
    for (const PrDomNode* at = document->first; at != node; at = at->next) {
        if (at->type == PrDomNodeType_Element)
            return;
    }
    PrStringView data = prRawStringView(&instruction->data);
    PrStringView href{};
    PrStringView type{};
    PrStringView alternate{};
    if (!pseudoAttribute(data, "href", &href) || href.size == 0)
        return;
    if (pseudoAttribute(data, "type", &type) && !prAsciiEqualsFold(prStringTrim(type), "text/css"))
        return;
    if (pseudoAttribute(data, "alternate", &alternate) && alternate == "yes")
        return;
    PrUrl* url = nullptr;
    if (domDocumentParseUrl(document, href, &url) != PrStatus_Ok) {
        prUrlDestroy(url);
        return;
    }
    InstructionFetch* fetch = allocZeroed<InstructionFetch>();
    domNodeRetain(node);
    fetch->instruction = node;
    fetch->href = prUrlSerialize(url, PrUrlSerialization_Whole);
    PrStringView title{};
    PrStringView media{};
    if (pseudoAttribute(data, "title", &title))
        fetch->title = prRawStringCopy(title);
    if (pseudoAttribute(data, "media", &media))
        fetch->media = prRawStringCopy(media);
    PrRawString request = prUrlSerialize(url, PrUrlSerialization_ExcludeFragment);
    prUrlDestroy(url);
    DomFetchRequest asked{
        prRawStringView(&request), DomFetchDestination_Style, DomCorsSettings_NoCors, DomFetchInitiator_Other,
    };
    host->fetch(host->context, &asked, instructionSheetFetchedAndDropped, fetch, dropInstructionFetch);
    prRawStringReset(&request);
}

bool cssDocumentHasBlockingSheet(const PrDomDocument* document) {
    return document->style != nullptr && document->style->blocking.count != 0;
}

bool cssDocumentBlocksRendering(const PrDomDocument* document) {
    const CssRoot* style = document->style;
    for (uint32_t i = 0; style != nullptr && i < style->blocking.count; ++i) {
        if (style->blocking.data[i]->style->blocksRendering)
            return true;
    }
    return false;
}

void cssInstructionRemoving(PrDomNode* node) {
    CssRoot* style = node->document->style;
    if (node->type != PrDomNodeType_ProcessingInstruction || style == nullptr)
        return;
    for (uint32_t i = 0; i < style->instructionSheets.count; ++i) {
        PrCssStyleSheet* sheet = style->instructionSheets.data[i];
        if (sheet->ownerNode != node)
            continue;
        style->instructionSheets.erase(i);
        style->sheets.sheets.removeFirst(sheet);
        cssStyleSheetLeaveOwner(sheet);
        cssDocumentStyleChanged(node->document);
        return;
    }
}

} // namespace pr_impl
